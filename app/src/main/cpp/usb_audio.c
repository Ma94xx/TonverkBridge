// UAC2-Ausgabetreiber für den Elektron Tonverk über libusb (ohne Root).
// Zielparameter aus dem USB-Deskriptor-Dump:
//   AudioControl-IF=3, Clock-ID=7, Streaming-IF=4/Alt1, ISO-OUT=0x03 (maxPkt 294, 1ms)
//   Format 48 kHz, 24 Bit (3-Byte-Subslot), Stereo; Feedback-EP 0x84 (16.16, Samples/ms)
//
// Kernproblem: Tonverk-Clock != Handy-Capture-Clock. Deshalb adaptives Resampling:
//   - Output-Framezahl/Paket folgt dem Geräte-Feedback (Geräterate).
//   - Der Inhalt wird per 16-Tap-Kaiser-Sinc-Interpolation (64 Phasen) aus dem
//     48,0-kHz-Capture erzeugt; ein Proportionalregler hält den Ring stabil.
//   - Pakete werden lückenlos (kumulative Offsets, usbfs-Layout) befüllt; Fade-in
//     nach (Re-)Prime und abklingende Underflows vermeiden harte Sprünge.

#include <jni.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <math.h>
#include <android/log.h>
#include <libusb.h>

#define TAG "usbaudio"
#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

#define IF_CONTROL     3
#define IF_STREAM      4
#define ALT_STREAM     1
#define EP_OUT         0x03
#define EP_FB          0x84
#define NUM_FB         4
#define CLOCK_ID       7
#define CHANNELS       2
#define FRAME_BYTES    6            // 2ch * 24 Bit
#define MAX_PKT        294
#define NUM_TRANSFERS  24          // tiefe ISO-Warteschlange (~192 ms) gegen Nachlade-Aussetzer
#define PKTS_PER_XFER  8
#define FRAMES_PER_PKT 48

static JavaVM *g_vm = NULL;
static jclass  g_cls = NULL;
static jmethodID g_logm = NULL;

static libusb_context *g_ctx = NULL;
static libusb_device_handle *g_handle = NULL;
static struct libusb_transfer *g_xfers[NUM_TRANSFERS];
static struct libusb_transfer *g_fb_xfers[NUM_FB];
static volatile int g_running = 0, g_evstop = 0, g_active = 0;
static pthread_t g_evthread;
static int g_thread_started = 0;

// Ring: interleaved L,R als int32 (24-Bit-Wert), damit Interpolation einfach ist.
static int32_t *g_rb = NULL;
static size_t g_cap = 0;                 // Kapazität in SAMPLES (Frames*2)
static volatile size_t g_head = 0;       // Schreibindex (Samples)
static volatile size_t g_tail = 0;       // Leseindex (Samples)
static double g_rpos = 0.0;              // Bruchteil-Leseposition in Frames ab g_tail
static double g_step = 1.0;              // Input-Frames pro Output-Frame (Resampling)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int  g_sample_rate = 48000;
static volatile int g_primed = 0;
static size_t g_prime_frames = 0;
static volatile int g_tone = 0;
static double g_phase = 0.0;

static volatile double g_fb_spp = 48.0;
static double g_frac = 0.0;
static long g_fb_count = 0, g_pkts = 0, g_errs = 0, g_underflow = 0, g_drops = 0, g_reprime = 0;
static long g_in_frames = 0, g_out_frames = 0;
static unsigned g_fb_raw = 0;

static void jlog(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    ALOG("%s", buf);
    if (!g_vm || !g_cls || !g_logm) return;
    JNIEnv *env = NULL; int attached = 0;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    }
    jstring s = (*env)->NewStringUTF(env, buf);
    (*env)->CallStaticVoidMethod(env, g_cls, g_logm, s);
    (*env)->DeleteLocalRef(env, s);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

static void native_stop(void);   // Vorwärtsdeklaration (idempotenter Start)

static size_t frames_avail(void) {   // Aufruf unter g_lock
    size_t s = (g_head - g_tail + g_cap) % g_cap;
    return s / 2;
}

// ---- Resampler: 16-Tap-Sinc mit Kaiser-Fenster, 64 Phasen + Phasen-Interpolation ----
// Die Leseposition hält SINC_HALF History-Frames hinter g_tail+g_rpos; dadurch
// stehen für jedes Output-Sample Taps fi-7 .. fi+8 im Ring bereit.
#define SINC_HALF   8
#define SINC_TAPS   (2 * SINC_HALF)
#define SINC_PHASES 64
#define FADE_FRAMES 480            // ~10 ms Einblendung nach (Re-)Prime

static float g_sinc[SINC_PHASES + 1][SINC_TAPS];
static int   g_sinc_ready = 0;
static int   g_fade = 0;           // Frames seit (Re-)Prime, für Fade-in
static int32_t g_lastL = 0, g_lastR = 0;   // letztes Output-Sample (Underflow-Decay)

static double bessel_i0(double x) {
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 40; k++) {
        t *= (x * x) / (4.0 * (double) k * (double) k);
        s += t;
        if (t < 1e-12 * s) break;
    }
    return s;
}

static void sinc_init(void) {
    if (g_sinc_ready) return;
    const double cutoff = 0.92;    // relativ zu Nyquist; Übergangsband für 16 Taps
    const double beta = 8.0;       // Kaiser-Parameter (~80 dB Dämpfung)
    for (int ph = 0; ph <= SINC_PHASES; ph++) {
        double fr = (double) ph / SINC_PHASES;
        double w[SINC_TAPS], sum = 0.0;
        for (int t = 0; t < SINC_TAPS; t++) {
            double x = (double)(t - (SINC_HALF - 1)) - fr;   // Abstand Tap <-> Leseposition
            double s = (x == 0.0) ? cutoff : sin(M_PI * cutoff * x) / (M_PI * x);
            double u = x / (double) SINC_HALF;
            double k = (fabs(u) <= 1.0) ? bessel_i0(beta * sqrt(1.0 - u * u)) / bessel_i0(beta) : 0.0;
            w[t] = s * k;
            sum += w[t];
        }
        for (int t = 0; t < SINC_TAPS; t++)        // DC-Gain pro Phase exakt 1
            g_sinc[ph][t] = (float)(w[t] / sum);
    }
    g_sinc_ready = 1;
}

// Feedback: wie viele Samples das Gerät pro 1-ms-Paket will (16.16).
static void LIBUSB_CALL fb_cb(struct libusb_transfer *x) {
    if (!g_running || x->status == LIBUSB_TRANSFER_CANCELLED) { g_active--; return; }
    if (x->status == LIBUSB_TRANSFER_COMPLETED && x->num_iso_packets > 0) {
        int al = x->iso_packet_desc[0].actual_length;
        unsigned char *d = libusb_get_iso_packet_buffer_simple(x, 0);
        double spp = 0.0;
        if (al >= 4) {
            unsigned v = (unsigned) d[0] | ((unsigned) d[1] << 8) |
                         ((unsigned) d[2] << 16) | ((unsigned) d[3] << 24);
            g_fb_raw = v; spp = (double) v / 65536.0;
        } else if (al == 3) {
            unsigned v = (unsigned) d[0] | ((unsigned) d[1] << 8) | ((unsigned) d[2] << 16);
            g_fb_raw = v; spp = (double) v / 16384.0;
        }
        if (spp > 40.0 && spp < 56.0) {
            if (g_fb_count == 0) g_fb_spp = spp;   // erster Messwert: direkt übernehmen
            else g_fb_spp = g_fb_spp * 0.9 + spp * 0.1;
            g_fb_count++;
        }
    }
    if (g_running) { if (libusb_submit_transfer(x) < 0) g_active--; }
}

static void LIBUSB_CALL xfer_cb(struct libusb_transfer *xfer) {
    if (xfer->status != LIBUSB_TRANSFER_COMPLETED &&
        xfer->status != LIBUSB_TRANSFER_CANCELLED) g_errs++;
    if (!g_running || xfer->status == LIBUSB_TRANSFER_CANCELLED) { g_active--; return; }

    // WICHTIG: Der Kernel (usbfs) sendet die Pakete lückenlos hintereinander
    // (Offset = Summe der tatsächlichen Paketlängen). libusb_get_iso_packet_buffer_simple
    // stimmt nur bei gleich großen Paketen – bei variablen Längen (47/48/49 Frames)
    // entstehen sonst übersprungene/doppelte Frames = Knackser.
    uint8_t *pb = xfer->buffer;
    for (int p = 0; p < xfer->num_iso_packets; p++) {
        // Output-Framezahl folgt dem Geräte-Feedback.
        g_frac += g_fb_spp;
        int ofr = (int) g_frac; g_frac -= ofr;
        if (ofr > 49) ofr = 49;
        if (ofr < 1)  ofr = 1;
        g_out_frames += ofr;

        if (g_tone) {
            for (int f = 0; f < ofr; f++) {
                int v = (int)(2000000.0 * sin(g_phase));
                g_phase += 2.0 * M_PI * 440.0 / g_sample_rate;
                if (g_phase > 2.0 * M_PI) g_phase -= 2.0 * M_PI;
                int o = f * FRAME_BYTES;
                pb[o+0]=v&0xff; pb[o+1]=(v>>8)&0xff; pb[o+2]=(v>>16)&0xff;
                pb[o+3]=v&0xff; pb[o+4]=(v>>8)&0xff; pb[o+5]=(v>>16)&0xff;
            }
            xfer->iso_packet_desc[p].length = (size_t) ofr * FRAME_BYTES;
            pb += (size_t) ofr * FRAME_BYTES;
            continue;
        }

        pthread_mutex_lock(&g_lock);
        size_t availF = frames_avail();
        if (!g_primed && availF >= g_prime_frames) {
            g_primed = 1;
            g_rpos = (double) SINC_HALF;   // Leseposition hinter den History-Frames
            g_fade = 0;                    // weich einblenden statt hart einsetzen
        }
        if (g_primed) {
            // Sanft geglättete Schrittweite (die als "sehr gut" empfundene Variante):
            // keine abrupten Sprünge -> kein Knistern, stabile Tonhöhe.
            double target = 1.0 + ((double) availF - (double) g_prime_frames) * 3e-5;
            if (target < 0.95) target = 0.95;
            if (target > 1.05) target = 1.05;
            g_step = g_step * 0.9997 + target * 0.0003;
        }
        for (int f = 0; f < ofr; f++) {
            int o = f * FRAME_BYTES;
            size_t fi = (size_t) g_rpos;
            if (!g_primed || fi + SINC_HALF >= availF) {
                // Kein frisches Material: letztes Sample abklingen lassen statt Null-Sprung.
                if (g_primed) {
                    g_underflow++;
                    if (availF < (size_t) SINC_TAPS) { g_primed = 0; g_reprime++; }
                }
                g_lastL = (int32_t)((double) g_lastL * 0.995);
                g_lastR = (int32_t)((double) g_lastR * 0.995);
                int L = g_lastL, R = g_lastR;
                pb[o+0]=L&0xff; pb[o+1]=(L>>8)&0xff; pb[o+2]=(L>>16)&0xff;
                pb[o+3]=R&0xff; pb[o+4]=(R>>8)&0xff; pb[o+5]=(R>>16)&0xff;
                continue;
            }
            double frac = g_rpos - (double) fi;
            double pf = frac * SINC_PHASES;
            int ph = (int) pf;
            float pr = (float)(pf - (double) ph);
            const float *w0 = g_sinc[ph], *w1 = g_sinc[ph + 1];
            // g_tail ist immer gerade; L liegt auf geraden, R auf ungeraden Indizes.
            size_t base = (g_tail + 2 * (fi - (SINC_HALF - 1))) % g_cap;
            double accL = 0.0, accR = 0.0;
            for (int t = 0; t < SINC_TAPS; t++) {
                float wt = w0[t] + (w1[t] - w0[t]) * pr;
                size_t idx = (base + 2 * (size_t) t) % g_cap;
                accL += (double) g_rb[idx]     * (double) wt;
                accR += (double) g_rb[idx + 1] * (double) wt;
            }
            double gain = 1.0;
            if (g_fade < FADE_FRAMES) { gain = (double) g_fade / FADE_FRAMES; g_fade++; }
            long Ll = lrint(accL * gain), Rl = lrint(accR * gain);
            if (Ll >  8388607) Ll =  8388607;
            if (Ll < -8388608) Ll = -8388608;
            if (Rl >  8388607) Rl =  8388607;
            if (Rl < -8388608) Rl = -8388608;
            int L = (int) Ll, R = (int) Rl;
            g_lastL = L; g_lastR = R;
            pb[o+0]=L&0xff; pb[o+1]=(L>>8)&0xff; pb[o+2]=(L>>16)&0xff;
            pb[o+3]=R&0xff; pb[o+4]=(R>>8)&0xff; pb[o+5]=(R>>16)&0xff;
            g_rpos += g_step;
        }
        if (g_primed) {
            size_t fi_end = (size_t) g_rpos;
            size_t consumed = fi_end > (size_t) SINC_HALF ? fi_end - (size_t) SINC_HALF : 0;
            if (consumed > availF) consumed = availF;
            g_tail = (g_tail + consumed * 2) % g_cap;
            g_rpos -= (double) consumed;
        }
        pthread_mutex_unlock(&g_lock);
        xfer->iso_packet_desc[p].length = (size_t) ofr * FRAME_BYTES;
        pb += (size_t) ofr * FRAME_BYTES;
    }
    g_pkts += xfer->num_iso_packets;
    if (libusb_submit_transfer(xfer) < 0) { g_errs++; g_active--; }
}

static void *evloop(void *arg) {
    (void) arg;
    // Nachlade-Thread auf Audio-Priorität (−19 = THREAD_PRIORITY_URGENT_AUDIO),
    // damit isochrone Transfers prompt wieder eingereiht werden -> weniger Aussetzer.
    setpriority(PRIO_PROCESS, (id_t) syscall(SYS_gettid), -19);
    while (!g_evstop) {
        struct timeval tv = { 0, 100000 };
        libusb_handle_events_timeout_completed(g_ctx, &tv, NULL);
    }
    return NULL;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void) reserved;
    g_vm = vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return -1;
    jclass c = (*env)->FindClass(env, "com/tonverk/bridge/NativeUsb");
    if (c) {
        g_cls = (*env)->NewGlobalRef(env, c);
        g_logm = (*env)->GetStaticMethodID(env, c, "log", "(Ljava/lang/String;)V");
    }
    return JNI_VERSION_1_6;
}

static int do_start(int fd, int sampleRate) {
    native_stop();            // evtl. laufende/zombie Session zuerst sauber abräumen
    g_sample_rate = sampleRate;
    int r;

    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    r = libusb_init(&g_ctx);
    if (r < 0) { jlog("libusb_init fail %d", r); return -1; }

    r = libusb_wrap_sys_device(g_ctx, (intptr_t) fd, &g_handle);
    if (r < 0) { jlog("wrap_sys_device fail %s", libusb_error_name(r)); libusb_exit(g_ctx); g_ctx = NULL; return -2; }

    libusb_set_auto_detach_kernel_driver(g_handle, 1);
    r = libusb_claim_interface(g_handle, IF_CONTROL);
    if (r < 0) jlog("claim IF%d warn %s", IF_CONTROL, libusb_error_name(r));
    r = libusb_claim_interface(g_handle, IF_STREAM);
    if (r < 0) { jlog("claim IF%d FAIL %s", IF_STREAM, libusb_error_name(r)); return -3; }

    unsigned char sr[4] = {
        (unsigned char)(sampleRate & 0xff), (unsigned char)((sampleRate >> 8) & 0xff),
        (unsigned char)((sampleRate >> 16) & 0xff), (unsigned char)((sampleRate >> 24) & 0xff)
    };
    r = libusb_control_transfer(g_handle, 0x21, 0x01, (0x01 << 8) | 0x00,
                                (CLOCK_ID << 8) | IF_CONTROL, sr, 4, 1000);
    if (r < 0) jlog("set sample rate warn %s", libusb_error_name(r));
    else jlog("sample rate set to %d", sampleRate);

    r = libusb_set_interface_alt_setting(g_handle, IF_STREAM, ALT_STREAM);
    if (r < 0) { jlog("set_alt FAIL %s", libusb_error_name(r)); return -4; }
    jlog("IF%d Alt%d active", IF_STREAM, ALT_STREAM);

    g_cap = (size_t)(g_sample_rate * 2);      // ~1 s Ring (2ch) -> Transienten-Reserve
    g_rb = (int32_t *) malloc(g_cap * sizeof(int32_t));
    if (!g_rb) { jlog("ring malloc fail"); return -5; }
    memset(g_rb, 0, g_cap * sizeof(int32_t));
    sinc_init();
    g_head = g_tail = 0; g_rpos = 0.0; g_step = 1.0;
    g_prime_frames = (size_t)(g_sample_rate * 150 / 1000);  // ~150 ms vorpuffern
    g_primed = 0; g_fade = 0; g_lastL = g_lastR = 0;
    g_fb_spp = 48.0; g_frac = 0.0; g_fb_count = 0; g_fb_raw = 0; g_phase = 0.0;

    g_running = 1; g_evstop = 0; g_active = 0;
    g_pkts = g_errs = g_underflow = g_drops = g_reprime = 0;
    g_in_frames = g_out_frames = 0;

    for (int i = 0; i < NUM_TRANSFERS; i++) {
        struct libusb_transfer *x = libusb_alloc_transfer(PKTS_PER_XFER);
        unsigned char *buf = (unsigned char *) malloc(PKTS_PER_XFER * MAX_PKT);
        memset(buf, 0, PKTS_PER_XFER * MAX_PKT);
        libusb_fill_iso_transfer(x, g_handle, EP_OUT, buf, PKTS_PER_XFER * MAX_PKT,
                                 PKTS_PER_XFER, xfer_cb, NULL, 1000);
        libusb_set_iso_packet_lengths(x, MAX_PKT);
        for (int p = 0; p < PKTS_PER_XFER; p++)
            x->iso_packet_desc[p].length = FRAMES_PER_PKT * FRAME_BYTES;
        g_xfers[i] = x;
        if (libusb_submit_transfer(x) < 0) jlog("submit %d fail", i); else g_active++;
    }
    for (int i = 0; i < NUM_FB; i++) {
        struct libusb_transfer *x = libusb_alloc_transfer(1);
        unsigned char *buf = (unsigned char *) malloc(64);
        libusb_fill_iso_transfer(x, g_handle, EP_FB, buf, 4, 1, fb_cb, NULL, 1000);
        libusb_set_iso_packet_lengths(x, 4);
        g_fb_xfers[i] = x;
        if (libusb_submit_transfer(x) < 0) jlog("submit FB %d fail", i); else g_active++;
    }

    pthread_create(&g_evthread, NULL, evloop, NULL);
    g_thread_started = 1;
    jlog("driver started (%d transfers)%s", g_active, g_tone ? " [TEST TONE]" : "");
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_tonverk_bridge_NativeUsb_start(JNIEnv *env, jclass clazz, jint fd, jint sampleRate) {
    (void) env; (void) clazz; g_tone = 0; return do_start(fd, sampleRate);
}

JNIEXPORT jint JNICALL
Java_com_tonverk_bridge_NativeUsb_startTone(JNIEnv *env, jclass clazz, jint fd, jint sampleRate) {
    (void) env; (void) clazz; g_tone = 1; return do_start(fd, sampleRate);
}

JNIEXPORT void JNICALL
Java_com_tonverk_bridge_NativeUsb_write(JNIEnv *env, jclass clazz, jbyteArray arr, jint len) {
    (void) clazz;
    if (!g_running || !g_rb || g_tone) return;
    jbyte *src = (*env)->GetByteArrayElements(env, arr, NULL);
    if (!src) return;
    pthread_mutex_lock(&g_lock);
    if (!g_rb) { pthread_mutex_unlock(&g_lock); (*env)->ReleaseByteArrayElements(env, arr, src, JNI_ABORT); return; }
    int i = 0;
    while (i + 1 < len) {
        int16_t s = (int16_t)((uint8_t) src[i] | ((uint8_t) src[i + 1] << 8));
        g_rb[g_head] = (int32_t) s << 8;           // 16 -> 24 Bit
        g_head = (g_head + 1) % g_cap;
        if (g_head == g_tail) {                     // Überlauf: ältestes Frame verwerfen
            g_tail = (g_tail + 2) % g_cap;
            if (g_rpos >= 1.0) g_rpos -= 1.0;       // Leseposition mitführen
            g_drops++;
        }
        i += 2;
    }
    g_in_frames += len / 4;   // 2ch * 16 Bit = 4 Byte/Frame
    pthread_mutex_unlock(&g_lock);
    (*env)->ReleaseByteArrayElements(env, arr, src, JNI_ABORT);
}

// Float-Capture-Pfad: volle Mixer-Auflösung -> 24 Bit (gerundet, geclampt).
JNIEXPORT void JNICALL
Java_com_tonverk_bridge_NativeUsb_writeFloat(JNIEnv *env, jclass clazz, jfloatArray arr, jint len) {
    (void) clazz;
    if (!g_running || !g_rb || g_tone) return;
    jfloat *src = (*env)->GetFloatArrayElements(env, arr, NULL);
    if (!src) return;
    pthread_mutex_lock(&g_lock);
    if (!g_rb) { pthread_mutex_unlock(&g_lock); (*env)->ReleaseFloatArrayElements(env, arr, src, JNI_ABORT); return; }
    for (int i = 0; i < len; i++) {
        float f = src[i];
        if (f >  1.0f) f =  1.0f;
        if (f < -1.0f) f = -1.0f;
        g_rb[g_head] = (int32_t) lrintf(f * 8388607.0f);
        g_head = (g_head + 1) % g_cap;
        if (g_head == g_tail) {                     // Überlauf: ältestes Frame verwerfen
            g_tail = (g_tail + 2) % g_cap;
            if (g_rpos >= 1.0) g_rpos -= 1.0;       // Leseposition mitführen
            g_drops++;
        }
    }
    g_in_frames += len / 2;   // 2ch -> 2 Samples/Frame
    pthread_mutex_unlock(&g_lock);
    (*env)->ReleaseFloatArrayElements(env, arr, src, JNI_ABORT);
}

// Räumt eine laufende Session vollständig ab. Sicher mehrfach aufrufbar (no-op wenn nichts läuft).
static void native_stop(void) {
    if (!g_running && !g_ctx) return;
    g_running = 0;
    for (int i = 0; i < NUM_TRANSFERS; i++) if (g_xfers[i]) libusb_cancel_transfer(g_xfers[i]);
    for (int i = 0; i < NUM_FB; i++) if (g_fb_xfers[i]) libusb_cancel_transfer(g_fb_xfers[i]);
    for (int k = 0; k < 100 && g_active > 0; k++) usleep(5000);
    g_evstop = 1;
    if (g_thread_started) { pthread_join(g_evthread, NULL); g_thread_started = 0; }
    for (int i = 0; i < NUM_TRANSFERS; i++)
        if (g_xfers[i]) { free(g_xfers[i]->buffer); libusb_free_transfer(g_xfers[i]); g_xfers[i] = NULL; }
    for (int i = 0; i < NUM_FB; i++)
        if (g_fb_xfers[i]) { free(g_fb_xfers[i]->buffer); libusb_free_transfer(g_fb_xfers[i]); g_fb_xfers[i] = NULL; }
    if (g_handle) {
        libusb_set_interface_alt_setting(g_handle, IF_STREAM, 0);
        libusb_release_interface(g_handle, IF_STREAM);   // auto-detach reattached den Kernel-Treiber
        libusb_release_interface(g_handle, IF_CONTROL);
        libusb_close(g_handle); g_handle = NULL;
    }
    if (g_ctx) { libusb_exit(g_ctx); g_ctx = NULL; }
    pthread_mutex_lock(&g_lock);
    if (g_rb) { free(g_rb); g_rb = NULL; }
    pthread_mutex_unlock(&g_lock);
    jlog("driver stopped. pkts=%ld errs=%ld underflow=%ld", g_pkts, g_errs, g_underflow);
}

JNIEXPORT void JNICALL
Java_com_tonverk_bridge_NativeUsb_stop(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    native_stop();
}

JNIEXPORT jstring JNICALL
Java_com_tonverk_bridge_NativeUsb_stats(JNIEnv *env, jclass clazz) {
    (void) clazz;
    char buf[200];
    pthread_mutex_lock(&g_lock);
    size_t af = g_rb ? frames_avail() : 0;
    pthread_mutex_unlock(&g_lock);
    snprintf(buf, sizeof buf,
             "availF=%zu/%zu step=%.4f in=%ld out=%ld underflow=%ld reprime=%ld drops=%ld errs=%ld spp=%.3f",
             af, g_cap / 2, g_step, g_in_frames, g_out_frames, g_underflow, g_reprime, g_drops, g_errs, g_fb_spp);
    return (*env)->NewStringUTF(env, buf);
}
