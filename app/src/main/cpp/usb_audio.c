// Generischer USB-Audio-Treiber (UAC1/UAC2) über libusb (ohne Root).
// Ursprünglich für den Elektron Tonverk gebaut; Interfaces, Endpoints, Format,
// Clock und Feedback werden jetzt zur Laufzeit aus dem Config-Deskriptor gelesen
// (uac_desc.h), damit auch andere class-compliant Geräte funktionieren.
//
// Kernproblem: Geräte-Clock != Handy-Capture-Clock. Deshalb adaptives Resampling:
//   - Output-Framezahl/Paket folgt dem Geräte-Feedback (falls vorhanden).
//   - Der Inhalt wird per 16-Tap-Kaiser-Sinc-Interpolation (64 Phasen) aus dem
//     48-kHz-Capture erzeugt (auch auf abweichende Geräteraten, z. B. 44,1 kHz);
//     ein Proportionalregler hält den Ring stabil.
//   - Pakete werden lückenlos (kumulative Offsets, usbfs-Layout) befüllt; Fade-in
//     nach (Re-)Prime und abklingende Underflows vermeiden harte Sprünge.

#include <jni.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <math.h>
#include <android/log.h>
#include <libusb.h>

#include "uac_desc.h"

#define TAG "usbaudio"
#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

#define NUM_FB         4
#define NUM_TRANSFERS  24          // tiefe ISO-Warteschlange (~192 ms) gegen Nachlade-Aussetzer
#define MAX_ISO_PKTS   64          // Pakete pro Transfer (HS mit 125-µs-Intervall: 64 = 8 ms)
#define XFER_US        8000        // Ziel-Dauer eines Transfers

static JavaVM *g_vm = NULL;
static jclass  g_cls = NULL;
static jmethodID g_logm = NULL;

static libusb_context *g_ctx = NULL;
static libusb_device_handle *g_handle = NULL;
static struct libusb_transfer *g_xfers[NUM_TRANSFERS];
static struct libusb_transfer *g_fb_xfers[NUM_FB];
static uac_dev g_dev = { .ac_if = -1 };  // geparster Deskriptor (do_start)
static int g_high_speed = 0;
static int g_out_frame = 6;              // Bytes pro Output-Frame (Kanäle * Subslot)
static int g_out_max_frames = 49;        // Obergrenze Frames/Paket (wMaxPacketSize)
static int g_pkts_per_xfer = 8;
static double g_spp_nom = 48.0;          // nominale Frames pro Paket
static int g_fb_shift = 0, g_fb_locked = 0;   // Feedback-Skalierung (2^shift), automatisch erkannt
static int g_dev_rate = 48000;           // Geräte-Rate (Playback)
static double g_step_base = 1.0;         // Input-Rate / Geräte-Rate
static double g_step_alpha = 0.0003;     // Reglerglättung pro Paket (zeitbasiert: 0.0003 pro ms)
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
static long g_pkt_errs = 0, g_submit_fail = 0;   // OUT: einzelne ISO-Pakete / Resubmits
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

// Feedback: Samples pro Service-Intervall. Format laut Spec 10.14 (FS, 3 Byte) bzw.
// 16.16 (HS, 4 Byte, pro Microframe) — in der Praxis melden viele Geräte (u. a. der
// Tonverk) pro Paket statt pro (Micro-)Frame. Wie snd-usb-audio erkennen wir die
// Zweierpotenz-Skalierung beim ersten plausiblen Wert und halten sie fest.
static double fb_scale(double raw) {
    if (!g_fb_locked) {
        static const int order[] = { 0, 3, -3, 1, -1, 2, -2, 4, -4 };
        for (unsigned k = 0; k < sizeof order / sizeof order[0]; k++) {
            double v = ldexp(raw, order[k]);
            if (fabs(v / g_spp_nom - 1.0) < 0.1) { g_fb_shift = order[k]; g_fb_locked = 1; break; }
        }
        if (!g_fb_locked) return 0.0;
    }
    return ldexp(raw, g_fb_shift);
}

static void LIBUSB_CALL fb_cb(struct libusb_transfer *x) {
    if (!g_running || x->status == LIBUSB_TRANSFER_CANCELLED) { g_active--; return; }
    if (x->status == LIBUSB_TRANSFER_COMPLETED && x->num_iso_packets > 0) {
        int al = x->iso_packet_desc[0].actual_length;
        unsigned char *d = libusb_get_iso_packet_buffer_simple(x, 0);
        double raw = 0.0;
        if (al >= 4) {
            unsigned v = (unsigned) d[0] | ((unsigned) d[1] << 8) |
                         ((unsigned) d[2] << 16) | ((unsigned) d[3] << 24);
            g_fb_raw = v; raw = (double) v / 65536.0;
        } else if (al == 3) {
            unsigned v = (unsigned) d[0] | ((unsigned) d[1] << 8) | ((unsigned) d[2] << 16);
            g_fb_raw = v; raw = (double) v / 16384.0;
        }
        double spp = raw > 0.0 ? fb_scale(raw) : 0.0;
        if (spp > g_spp_nom * 0.85 && spp < g_spp_nom * 1.15) {
            if (g_fb_count == 0) g_fb_spp = spp;   // erster Messwert: direkt übernehmen
            else g_fb_spp = g_fb_spp * 0.9 + spp * 0.1;
            g_fb_count++;
        }
    }
    if (g_running) { if (libusb_submit_transfer(x) < 0) g_active--; }
}

// Schreibt ein Stereo-Frame (24-Bit-Werte) im Geräteformat: L/R auf Kanal 1/2,
// weitere Kanäle still, Mono = Mittelwert; Samples MSB-bündig im Subslot.
static inline void put_frame(uint8_t *o, int32_t L, int32_t R) {
    int nch = g_dev.out.channels, ss = g_dev.out.subslot;
    for (int c = 0; c < nch; c++, o += ss) {
        int32_t v = nch == 1 ? (int32_t)(((int64_t) L + R) / 2) : c == 0 ? L : c == 1 ? R : 0;
        if (ss == 2) { int32_t w = v >> 8; o[0] = w & 0xff; o[1] = (w >> 8) & 0xff; }
        else if (ss == 3) { o[0] = v & 0xff; o[1] = (v >> 8) & 0xff; o[2] = (v >> 16) & 0xff; }
        else { uint32_t w = (uint32_t) v << 8; o[0] = w & 0xff; o[1] = (w >> 8) & 0xff;
               o[2] = (w >> 16) & 0xff; o[3] = (w >> 24) & 0xff; }
    }
}

static void LIBUSB_CALL xfer_cb(struct libusb_transfer *xfer) {
    if (xfer->status != LIBUSB_TRANSFER_COMPLETED &&
        xfer->status != LIBUSB_TRANSFER_CANCELLED) g_errs++;
    if (!g_running || xfer->status == LIBUSB_TRANSFER_CANCELLED) { g_active--; return; }

    // Diagnose: fehlgeschlagene Einzel-Pakete (z. B. Bandbreiten-/Scheduling-Konflikte)
    for (int p = 0; p < xfer->num_iso_packets; p++)
        if (xfer->iso_packet_desc[p].status != LIBUSB_TRANSFER_COMPLETED) g_pkt_errs++;

    // WICHTIG: Der Kernel (usbfs) sendet die Pakete lückenlos hintereinander
    // (Offset = Summe der tatsächlichen Paketlängen). libusb_get_iso_packet_buffer_simple
    // stimmt nur bei gleich großen Paketen – bei variablen Längen (47/48/49 Frames)
    // entstehen sonst übersprungene/doppelte Frames = Knackser.
    uint8_t *pb = xfer->buffer;
    for (int p = 0; p < xfer->num_iso_packets; p++) {
        // Output-Framezahl folgt dem Geräte-Feedback.
        g_frac += g_fb_spp;
        int ofr = (int) g_frac; g_frac -= ofr;
        if (ofr > g_out_max_frames) ofr = g_out_max_frames;
        if (ofr < 1)  ofr = 1;
        g_out_frames += ofr;

        if (g_tone) {
            for (int f = 0; f < ofr; f++) {
                int v = (int)(2000000.0 * sin(g_phase));
                g_phase += 2.0 * M_PI * 440.0 / g_dev_rate;
                if (g_phase > 2.0 * M_PI) g_phase -= 2.0 * M_PI;
                put_frame(pb + (size_t) f * g_out_frame, v, v);
            }
            xfer->iso_packet_desc[p].length = (size_t) ofr * g_out_frame;
            pb += (size_t) ofr * g_out_frame;
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
            double target = g_step_base * (1.0 + ((double) availF - (double) g_prime_frames) * 3e-5);
            if (target < g_step_base * 0.95) target = g_step_base * 0.95;
            if (target > g_step_base * 1.05) target = g_step_base * 1.05;
            g_step = g_step * (1.0 - g_step_alpha) + target * g_step_alpha;
        }
        for (int f = 0; f < ofr; f++) {
            uint8_t *o = pb + (size_t) f * g_out_frame;
            size_t fi = (size_t) g_rpos;
            if (!g_primed || fi + SINC_HALF >= availF) {
                // Kein frisches Material: letztes Sample abklingen lassen statt Null-Sprung.
                if (g_primed) {
                    g_underflow++;
                    if (availF < (size_t) SINC_TAPS) { g_primed = 0; g_reprime++; }
                }
                g_lastL = (int32_t)((double) g_lastL * 0.995);
                g_lastR = (int32_t)((double) g_lastR * 0.995);
                put_frame(o, g_lastL, g_lastR);
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
            put_frame(o, L, R);
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
        xfer->iso_packet_desc[p].length = (size_t) ofr * g_out_frame;
        pb += (size_t) ofr * g_out_frame;
    }
    g_pkts += xfer->num_iso_packets;
    if (libusb_submit_transfer(xfer) < 0) { g_submit_fail++; g_active--; }
}

// ======================= Samplerate-Steuerung =======================

static void le32(uint8_t *b, int v) { b[0] = v & 0xff; b[1] = (v >> 8) & 0xff; b[2] = (v >> 16) & 0xff; b[3] = (v >> 24) & 0xff; }

// UAC2: Clock-Source auf `want` setzen (bzw. die nächstliegende Rate aus GET RANGE)
// und die tatsächliche Rate zurücklesen. Rückgabe: aktive Rate.
static int uac2_set_clock(int ac_if, int clock_id, int want) {
    if (clock_id <= 0) { jlog("no clock source found, assuming %d Hz", want); return want; }
    uint16_t widx = (uint16_t)((clock_id << 8) | ac_if);
    uint8_t buf[256];
    int rate = want;
    // RANGE: wNumSubRanges, dann je {dMIN, dMAX, dRES}
    int n = libusb_control_transfer(g_handle, 0xA1, 0x02, 0x0100, widx, buf, sizeof buf, 1000);
    if (n >= 14) {
        int nsub = buf[0] | (buf[1] << 8), best = -1, bestd = 1 << 30;
        for (int k = 0; k < nsub && 2 + 12 * k + 12 <= n; k++) {
            const uint8_t *r = buf + 2 + 12 * k;
            int mn = r[0] | (r[1] << 8) | (r[2] << 16) | (r[3] << 24);
            int mx = r[4] | (r[5] << 8) | (r[6] << 16) | (r[7] << 24);
            int c = want < mn ? mn : want > mx ? mx : want;
            int dd = c > want ? c - want : want - c;
            if (dd < bestd) { bestd = dd; best = c; }
        }
        if (best > 0) rate = best;
    }
    le32(buf, rate);
    int r = libusb_control_transfer(g_handle, 0x21, 0x01, 0x0100, widx, buf, 4, 1000);
    if (r < 0) jlog("clock %d: set %d Hz warn %s", clock_id, rate, libusb_error_name(r));
    memset(buf, 0, 4);
    r = libusb_control_transfer(g_handle, 0xA1, 0x01, 0x0100, widx, buf, 4, 1000);
    if (r == 4) {
        int cur = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
        if (cur >= 8000 && cur <= 768000) rate = cur;
    }
    jlog("clock %d: %d Hz", clock_id, rate);
    return rate;
}

// UAC1: Rate pro Endpoint (SET_CUR SAMPLING_FREQ_CONTROL, 3 Byte).
static int uac1_set_rate(const uac_stream *st, int want) {
    int rate = uac1_pick_rate(st, want);
    uint8_t buf[3] = { (uint8_t)(rate & 0xff), (uint8_t)((rate >> 8) & 0xff), (uint8_t)((rate >> 16) & 0xff) };
    int r = libusb_control_transfer(g_handle, 0x22, 0x01, 0x0100, (uint16_t) st->ep, buf, 3, 1000);
    if (r < 0) jlog("EP 0x%02X: set %d Hz warn %s", st->ep, rate, libusb_error_name(r));
    r = libusb_control_transfer(g_handle, 0xA2, 0x81, 0x0100, (uint16_t) st->ep, buf, 3, 1000);
    if (r == 3) {
        int cur = buf[0] | (buf[1] << 8) | (buf[2] << 16);
        if (cur >= 8000 && cur <= 768000) rate = cur;
    }
    jlog("EP 0x%02X: %d Hz", st->ep, rate);
    return rate;
}

static int set_stream_rate(const uac_stream *st, int want) {
    return st->uac == 2 ? uac2_set_clock(st->ac_if, st->clock_id, want) : uac1_set_rate(st, want);
}

static int pkts_per_xfer(int period_us) {
    int n = XFER_US / period_us;
    return n < 1 ? 1 : n > MAX_ISO_PKTS ? MAX_ISO_PKTS : n;
}

// ======================= Capture-Pfad (Gerät -> Handy) =======================
// Umgekehrte Richtung: isochroner IN-Endpoint -> Byte-Ring -> Kotlin (WAV-Datei).
// Kein Resampler nötig: Die Datei folgt 1:1 der Geräte-Clock, es wird geschrieben,
// was ankommt.

#define NUM_CAP_XFERS     16

// ---- Capture-Ring (Bytes, wie vom Gerät gepackt; z. B. 24-Bit-LE interleaved) ----
static uint8_t *g_cr = NULL;
static size_t g_cr_cap = 0, g_cr_head = 0, g_cr_tail = 0, g_cr_count = 0;
static int g_cr_frame = 6;
static pthread_mutex_t g_cap_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_cap_running = 0;
static volatile int g_cap_active = 0;   // vom Event-Loop-Thread dekrementiert
static int g_cap_claimed = 0;
static long g_cap_pkts = 0, g_cap_errs = 0, g_cap_drops = 0, g_cap_bytes = 0;
static volatile int g_cap_peak = 0;   // Betrags-Peak (16-Bit-Auflösung) seit letztem Lesen
static int g_cap_rate = 0, g_cap_pkts_per_xfer = 8;
static struct libusb_transfer *g_cap_xfers[NUM_CAP_XFERS];

static int cap_ring_init(size_t bytes, int frame_bytes) {
    if (frame_bytes <= 0) return -1;
    bytes -= bytes % (size_t) frame_bytes;
    if (bytes == 0) return -1;
    uint8_t *b = (uint8_t *) malloc(bytes);
    if (!b) return -1;
    pthread_mutex_lock(&g_cap_lock);
    g_cr = b; g_cr_cap = bytes; g_cr_frame = frame_bytes;
    g_cr_head = g_cr_tail = g_cr_count = 0;
    g_cap_pkts = g_cap_errs = g_cap_drops = g_cap_bytes = 0;
    g_cap_peak = 0;
    pthread_mutex_unlock(&g_cap_lock);
    return 0;
}

static void cap_ring_free(void) {
    pthread_mutex_lock(&g_cap_lock);
    if (g_cr) { free(g_cr); g_cr = NULL; }
    g_cr_cap = g_cr_head = g_cr_tail = g_cr_count = 0;
    pthread_mutex_unlock(&g_cap_lock);
}

static void cap_push(const uint8_t *p, size_t n) {
    pthread_mutex_lock(&g_cap_lock);
    if (!g_cr || n == 0) { pthread_mutex_unlock(&g_cap_lock); return; }
    if (n > g_cr_cap) { p += n - g_cr_cap; n = g_cr_cap; }   // absurder Einzel-Push
    // Überlauf (Kotlin liest nicht schnell genug): älteste Frames verwerfen.
    while (g_cr_count + n > g_cr_cap && g_cr_count >= (size_t) g_cr_frame) {
        g_cr_tail = (g_cr_tail + (size_t) g_cr_frame) % g_cr_cap;
        g_cr_count -= (size_t) g_cr_frame;
        g_cap_drops++;
    }
    if (g_cr_count + n > g_cr_cap) {     // Rest < 1 Frame (sollte nie passieren): Ring leeren
        g_cr_tail = g_cr_head; g_cr_count = 0; g_cap_drops++;
    }
    size_t first = g_cr_cap - g_cr_head;
    if (first > n) first = n;
    memcpy(g_cr + g_cr_head, p, first);
    if (n > first) memcpy(g_cr, p + first, n - first);
    g_cr_head = (g_cr_head + n) % g_cr_cap;
    g_cr_count += n;
    g_cap_bytes += (long) n;
    // Live-Meter: Betrags-Peak über die 2 MSBs jedes Samples (reicht fürs Anzeigen).
    int ss = g_dev.in.subslot >= 2 ? g_dev.in.subslot : 2;
    int pk = g_cap_peak;
    for (size_t s = 0; s + (size_t) ss <= n; s += (size_t) ss) {
        int16_t v = (int16_t)(p[s + ss - 2] | (p[s + ss - 1] << 8));
        int a = v < 0 ? -(int) v : (int) v;
        if (a > pk) pk = a;
    }
    g_cap_peak = pk;
    pthread_mutex_unlock(&g_cap_lock);
}

// Peak lesen und nullen (Polling durch die UI).
static int cap_take_peak(void) {
    int p = g_cap_peak;
    g_cap_peak = 0;
    return p;
}

// Ring leeren: Monitor-Altbestand darf nicht an den Anfang einer Aufnahme rutschen.
static void cap_flush(void) {
    pthread_mutex_lock(&g_cap_lock);
    g_cr_tail = g_cr_head;
    g_cr_count = 0;
    pthread_mutex_unlock(&g_cap_lock);
}

// Liest höchstens max Bytes, immer ganze Frames (hält den Stream kanal-stabil).
static int cap_read(uint8_t *dst, size_t max) {
    pthread_mutex_lock(&g_cap_lock);
    if (!g_cr) { pthread_mutex_unlock(&g_cap_lock); return -1; }
    size_t n = g_cr_count < max ? g_cr_count : max;
    n -= n % (size_t) g_cr_frame;
    size_t first = g_cr_cap - g_cr_tail;
    if (first > n) first = n;
    memcpy(dst, g_cr + g_cr_tail, first);
    if (n > first) memcpy(dst + first, g_cr, n - first);
    g_cr_tail = (g_cr_tail + n) % g_cr_cap;
    g_cr_count -= n;
    pthread_mutex_unlock(&g_cap_lock);
    return (int) n;
}

static void LIBUSB_CALL cap_xfer_cb(struct libusb_transfer *x) {
    if (x->status != LIBUSB_TRANSFER_COMPLETED &&
        x->status != LIBUSB_TRANSFER_CANCELLED) g_cap_errs++;
    if (!g_cap_running || x->status == LIBUSB_TRANSFER_CANCELLED) {
        __atomic_sub_fetch(&g_cap_active, 1, __ATOMIC_RELAXED);
        return;
    }
    // IN-ISO: jedes Paket liegt in seinem eigenen Slot fester Größe (alle Längen
    // gleich = max_pkt), gültig sind actual_length Bytes ab Slot-Anfang.
    for (int p = 0; p < x->num_iso_packets; p++) {
        struct libusb_iso_packet_descriptor *pd = &x->iso_packet_desc[p];
        if (pd->status != LIBUSB_TRANSFER_COMPLETED) { g_cap_errs++; continue; }
        // Auf ganze Frames stutzen: ein korruptes Kurz-Paket darf die
        // Kanalzuordnung im Ring nicht verschieben.
        size_t len = pd->actual_length - (pd->actual_length % (unsigned) g_cr_frame);
        if (len == 0) continue;
        unsigned char *pb = libusb_get_iso_packet_buffer_simple(x, (unsigned) p);
        if (pb) cap_push(pb, len);
    }
    g_cap_pkts += x->num_iso_packets;
    if (libusb_submit_transfer(x) < 0) {
        g_cap_errs++;
        __atomic_sub_fetch(&g_cap_active, 1, __ATOMIC_RELAXED);
    }
}

// Serialisiert Start/Stop des Capture-Pfads: kann gleichzeitig vom Service-Thread
// (Aufnahme beenden) und von native_stop() (Treiber-Stop der Activity) kommen.
static pthread_mutex_t g_cap_ctl = PTHREAD_MUTEX_INITIALIZER;

// Beendet nur den Capture-Pfad; der OUT-Stream läuft weiter. Mehrfach aufrufbar.
static void capture_stop_locked(void) {
    if (!g_cap_running && !g_cap_claimed) return;
    g_cap_running = 0;
    for (int i = 0; i < NUM_CAP_XFERS; i++)
        if (g_cap_xfers[i]) libusb_cancel_transfer(g_cap_xfers[i]);
    for (int k = 0; k < 100 && g_cap_active > 0; k++) usleep(5000);
    for (int i = 0; i < NUM_CAP_XFERS; i++)
        if (g_cap_xfers[i]) { free(g_cap_xfers[i]->buffer); libusb_free_transfer(g_cap_xfers[i]); g_cap_xfers[i] = NULL; }
    if (g_handle && g_cap_claimed) {
        libusb_set_interface_alt_setting(g_handle, g_dev.in.ifnum, 0);
        if (!g_dev.out.valid || g_dev.in.ifnum != g_dev.out.ifnum)
            libusb_release_interface(g_handle, g_dev.in.ifnum);
    }
    g_cap_claimed = 0;
    cap_ring_free();
    jlog("capture stopped. pkts=%ld bytes=%ld drops=%ld errs=%ld",
         g_cap_pkts, g_cap_bytes, g_cap_drops, g_cap_errs);
}

static void capture_stop(void) {
    pthread_mutex_lock(&g_cap_ctl);
    capture_stop_locked();
    pthread_mutex_unlock(&g_cap_ctl);
}

// Startet den Capture-Pfad; setzt einen laufenden Treiber (do_start) voraus,
// weil Handle + Event-Loop geteilt werden.
static int capture_start_locked(void) {
    if (!g_handle || !g_running) return -1;
    if (g_cap_running) return 0;

    const uac_stream *in = &g_dev.in;
    if (!in->valid) { jlog("capture: device has no audio input (ISO IN) interface"); return -3; }
    jlog("capture: IF%d Alt%d EP 0x%02X mps=%d %dch %dbit subslot=%d",
         in->ifnum, in->alt, in->ep, in->max_pkt, in->channels, in->bits, in->subslot);

    int r = libusb_claim_interface(g_handle, in->ifnum);
    if (r < 0) { jlog("capture: claim IF%d FAIL %s", in->ifnum, libusb_error_name(r)); return -4; }
    g_cap_claimed = 1;

    // Rate: eigene Clock (UAC2) bzw. eigener Endpoint (UAC1) -> passend zur Playback-Rate setzen.
    if (in->uac == 2 && g_dev.out.valid && in->ac_if == g_dev.out.ac_if && in->clock_id == g_dev.out.clock_id) g_cap_rate = g_dev_rate;
    else g_cap_rate = set_stream_rate(in, g_dev.out.valid ? g_dev_rate : 48000);

    r = libusb_set_interface_alt_setting(g_handle, in->ifnum, in->alt);
    if (r < 0) {
        jlog("capture: set_alt FAIL %s", libusb_error_name(r));
        libusb_release_interface(g_handle, in->ifnum); g_cap_claimed = 0;
        return -5;
    }

    int frame = in->channels * in->subslot;
    // ~6 s Ring: genug Reserve, um Schreib-Hänger langsamer Speicher (SD-Karten-GC,
    // SAF/Cloud) während langer Aufnahmen ohne Lücke zu überbrücken.
    if (cap_ring_init((size_t) g_cap_rate * (size_t) frame * 6, frame) != 0) {
        capture_stop_locked(); return -6;
    }
    g_cap_pkts_per_xfer = pkts_per_xfer(uac_period_us(in->interval, g_high_speed));

    g_cap_running = 1; g_cap_active = 0;
    for (int i = 0; i < NUM_CAP_XFERS; i++) {
        int np = g_cap_pkts_per_xfer;
        struct libusb_transfer *x = libusb_alloc_transfer(np);
        unsigned char *buf = (unsigned char *) malloc((size_t) np * (size_t) in->max_pkt);
        libusb_fill_iso_transfer(x, g_handle, (unsigned char) in->ep, buf,
                                 np * in->max_pkt, np, cap_xfer_cb, NULL, 1000);
        libusb_set_iso_packet_lengths(x, (unsigned) in->max_pkt);
        g_cap_xfers[i] = x;
        if (libusb_submit_transfer(x) < 0) jlog("capture: submit %d fail", i);
        else __atomic_add_fetch(&g_cap_active, 1, __ATOMIC_RELAXED);
    }
    if (g_cap_active == 0) { jlog("capture: no transfer submitted"); capture_stop_locked(); return -7; }
    jlog("capture started (%d transfers)", g_cap_active);
    return 0;
}

static int capture_start(void) {
    pthread_mutex_lock(&g_cap_ctl);
    int r = capture_start_locked();
    pthread_mutex_unlock(&g_cap_ctl);
    return r;
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

// sampleRate = Rate der eingespeisten Handy-Samples (48 kHz). Das Gerät läuft
// bevorzugt mit derselben Rate; sonst wird im Resampler umgerechnet.
static int do_start(int fd, int sampleRate) {
    native_stop();            // evtl. laufende/zombie Session zuerst sauber abräumen
    g_sample_rate = sampleRate;
    int r;

    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    r = libusb_init(&g_ctx);
    if (r < 0) { jlog("libusb_init fail %d", r); return -1; }

    r = libusb_wrap_sys_device(g_ctx, (intptr_t) fd, &g_handle);
    if (r < 0) { jlog("wrap_sys_device fail %s", libusb_error_name(r)); libusb_exit(g_ctx); g_ctx = NULL; return -2; }

    static unsigned char cfg[4096];
    int n = libusb_control_transfer(g_handle, 0x80, 0x06, (0x02 << 8) | 0, 0, cfg, sizeof cfg, 1000);
    if (n < 9 || uac_parse(cfg, (size_t) n, &g_dev) != 0) {
        jlog("no USB audio streaming interface found (descriptor %d bytes)", n);
        memset(&g_dev, 0, sizeof g_dev); g_dev.ac_if = -1;
        return -7;
    }
    int spd = libusb_get_device_speed(libusb_get_device(g_handle));
    g_high_speed = spd >= LIBUSB_SPEED_HIGH ? 1 : spd == LIBUSB_SPEED_UNKNOWN ? (g_dev.uac == 2) : 0;
    jlog("UAC%d device, AC IF%d, %s-speed", g_dev.uac, g_dev.ac_if, g_high_speed ? "high" : "full");

    libusb_set_auto_detach_kernel_driver(g_handle, 1);
    if (g_dev.ac_if >= 0) {
        r = libusb_claim_interface(g_handle, g_dev.ac_if);
        if (r < 0) jlog("claim IF%d warn %s", g_dev.ac_if, libusb_error_name(r));
    }

    g_running = 1; g_evstop = 0; g_active = 0;
    g_pkts = g_errs = g_underflow = g_drops = g_reprime = 0;
    g_in_frames = g_out_frames = 0;

    const uac_stream *out = &g_dev.out;
    if (!out->valid) {
        // Reines Aufnahmegerät: kein Playback, nur Event-Loop für den Capture-Pfad.
        jlog("device has no audio output (playback) interface - capture only");
        g_dev_rate = 48000;
    } else {
        r = libusb_claim_interface(g_handle, out->ifnum);
        if (r < 0) { jlog("claim IF%d FAIL %s", out->ifnum, libusb_error_name(r)); g_running = 0; return -3; }

        g_dev_rate = set_stream_rate(out, sampleRate);
        r = libusb_set_interface_alt_setting(g_handle, out->ifnum, out->alt);
        if (r < 0) { jlog("set_alt FAIL %s", libusb_error_name(r)); g_running = 0; return -4; }

        int period = uac_period_us(out->interval, g_high_speed);
        g_out_frame = out->channels * out->subslot;
        g_spp_nom = (double) g_dev_rate * period / 1e6;
        g_out_max_frames = out->max_pkt / g_out_frame;
        int lim = (int) ceil(g_spp_nom * 1.15) + 1;
        if (g_out_max_frames > lim) g_out_max_frames = lim;
        if (g_out_max_frames < 1) { jlog("wMaxPacketSize %d too small", out->max_pkt); g_running = 0; return -4; }
        g_pkts_per_xfer = pkts_per_xfer(period);
        g_step_base = (double) sampleRate / (double) g_dev_rate;
        g_step_alpha = 0.0003 * period / 1000.0;
        jlog("playback: IF%d Alt%d EP 0x%02X %dch %dbit/%dB @%d Hz, %d us/pkt, %s%s",
             out->ifnum, out->alt, out->ep, out->channels, out->bits, out->subslot, g_dev_rate, period,
             out->sync == UAC_SYNC_ASYNC ? "async" : out->sync == UAC_SYNC_ADAPTIVE ? "adaptive" : "sync",
             out->fb_ep ? "" : " (no feedback EP)");
        if (out->fb_ep) jlog("feedback EP 0x%02X", out->fb_ep);

        g_cap = (size_t)(g_sample_rate * 2);      // ~1 s Ring (2ch) -> Transienten-Reserve
        g_rb = (int32_t *) malloc(g_cap * sizeof(int32_t));
        if (!g_rb) { jlog("ring malloc fail"); g_running = 0; return -5; }
        memset(g_rb, 0, g_cap * sizeof(int32_t));
        sinc_init();
        g_head = g_tail = 0; g_rpos = 0.0; g_step = g_step_base;
        g_prime_frames = (size_t)(g_sample_rate * 150 / 1000);  // ~150 ms vorpuffern
        g_primed = 0; g_fade = 0; g_lastL = g_lastR = 0;
        g_fb_spp = g_spp_nom; g_frac = 0.0; g_fb_count = 0; g_fb_raw = 0; g_phase = 0.0;
        g_fb_locked = 0; g_fb_shift = 0;

        int nomlen = (int) g_spp_nom * g_out_frame;
        if (nomlen > out->max_pkt) nomlen = out->max_pkt;
        for (int i = 0; i < NUM_TRANSFERS; i++) {
            int np = g_pkts_per_xfer;
            struct libusb_transfer *x = libusb_alloc_transfer(np);
            unsigned char *buf = (unsigned char *) calloc((size_t) np, (size_t) out->max_pkt);
            libusb_fill_iso_transfer(x, g_handle, (unsigned char) out->ep, buf, np * out->max_pkt,
                                     np, xfer_cb, NULL, 1000);
            libusb_set_iso_packet_lengths(x, (unsigned) out->max_pkt);
            for (int p = 0; p < np; p++) x->iso_packet_desc[p].length = (unsigned) nomlen;
            g_xfers[i] = x;
            if (libusb_submit_transfer(x) < 0) jlog("submit %d fail", i); else g_active++;
        }
        // Nur einen Feedback-EP ansprechen, der laut Deskriptor zu diesem Alt-Setting
        // gehört: ein fremder EP würde dessen Interface implizit claimen (-> BUSY).
        int fbl = out->fb_max_pkt >= 3 ? out->fb_max_pkt : 4;
        for (int i = 0; out->fb_ep && i < NUM_FB; i++) {
            struct libusb_transfer *x = libusb_alloc_transfer(1);
            unsigned char *buf = (unsigned char *) malloc((size_t) fbl);
            libusb_fill_iso_transfer(x, g_handle, (unsigned char) out->fb_ep, buf, fbl, 1, fb_cb, NULL, 1000);
            libusb_set_iso_packet_lengths(x, (unsigned) fbl);
            g_fb_xfers[i] = x;
            if (libusb_submit_transfer(x) < 0) jlog("submit FB %d fail", i); else g_active++;
        }
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
    capture_stop();          // braucht den noch laufenden Event-Loop zum Canceln
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
        if (g_dev.out.valid) {
            libusb_set_interface_alt_setting(g_handle, g_dev.out.ifnum, 0);
            libusb_release_interface(g_handle, g_dev.out.ifnum);   // auto-detach reattached den Kernel-Treiber
        }
        if (g_dev.ac_if >= 0) libusb_release_interface(g_handle, g_dev.ac_if);
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

// ---- JNI: Capture (Tonverk -> Handy) ----

JNIEXPORT jint JNICALL
Java_com_tonverk_bridge_NativeUsb_captureStart(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    return capture_start();
}

JNIEXPORT void JNICALL
Java_com_tonverk_bridge_NativeUsb_captureStop(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    capture_stop();
}

// Holt bis zu max Bytes (ganze Frames) aus dem Capture-Ring.
// 0 = nichts da, -1 = Capture beendet/tot (z. B. Gerät abgezogen).
// Über einen nativen Zwischenpuffer + SetByteArrayRegion: kopiert nur die n gültigen
// Bytes statt (wie GetByteArrayElements) das ganze Array hin und zurück.
#define CAP_READ_MAX 65536
static uint8_t g_cap_tmp[CAP_READ_MAX];   // nur der eine rec-writer-Thread liest

JNIEXPORT jint JNICALL
Java_com_tonverk_bridge_NativeUsb_captureRead(JNIEnv *env, jclass clazz, jbyteArray arr, jint max) {
    (void) clazz;
    if (max < 0) return 0;
    size_t want = (size_t) max;
    if (want > CAP_READ_MAX) want = CAP_READ_MAX;
    int n = cap_read(g_cap_tmp, want);
    if (n > 0) (*env)->SetByteArrayRegion(env, arr, 0, n, (const jbyte *) g_cap_tmp);
    // Alle IN-Transfers gestorben (Unplug/Fehler) und nichts mehr im Ring:
    // dem Leser signalisieren, dass nichts mehr kommt.
    if (n == 0 && g_cap_running && g_cap_active <= 0) return -1;
    return n;
}

// Pegel-Poll fürs Live-Meter: >=0 Peak (16-Bit-Betrag) seit letztem Aufruf,
// -1 = Capture tot (Gerät weg), -2 = kein Capture aktiv.
JNIEXPORT jint JNICALL
Java_com_tonverk_bridge_NativeUsb_capturePeak(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    if (!g_cap_running) return -2;
    if (g_cap_active <= 0) return -1;
    return cap_take_peak();
}

JNIEXPORT void JNICALL
Java_com_tonverk_bridge_NativeUsb_captureFlush(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    cap_flush();
}

JNIEXPORT jint JNICALL
Java_com_tonverk_bridge_NativeUsb_captureChannels(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz; return g_cap_running ? g_dev.in.channels : 0;
}

JNIEXPORT jint JNICALL
Java_com_tonverk_bridge_NativeUsb_captureSubslot(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz; return g_cap_running ? g_dev.in.subslot : 0;
}

JNIEXPORT jint JNICALL
Java_com_tonverk_bridge_NativeUsb_captureRate(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz; return g_cap_running ? g_cap_rate : 0;
}

JNIEXPORT jstring JNICALL
Java_com_tonverk_bridge_NativeUsb_captureStats(JNIEnv *env, jclass clazz) {
    (void) clazz;
    char buf[160];
    pthread_mutex_lock(&g_cap_lock);
    size_t cnt = g_cr_count, cap = g_cr_cap;
    pthread_mutex_unlock(&g_cap_lock);
    snprintf(buf, sizeof buf, "ring=%zu/%zu pkts=%ld bytes=%ld drops=%ld errs=%ld",
             cnt, cap, g_cap_pkts, g_cap_bytes, g_cap_drops, g_cap_errs);
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL
Java_com_tonverk_bridge_NativeUsb_stats(JNIEnv *env, jclass clazz) {
    (void) clazz;
    char buf[320];
    pthread_mutex_lock(&g_lock);
    size_t af = g_rb ? frames_avail() : 0;
    pthread_mutex_unlock(&g_lock);
    snprintf(buf, sizeof buf,
             "availF=%zu/%zu step=%.4f in=%ld out=%ld underflow=%ld reprime=%ld drops=%ld "
             "errs=%ld pktErrs=%ld submitFail=%ld active=%d spp=%.3f",
             af, g_cap / 2, g_step, g_in_frames, g_out_frames, g_underflow, g_reprime, g_drops,
             g_errs, g_pkt_errs, g_submit_fail, g_active, g_fb_spp);
    return (*env)->NewStringUTF(env, buf);
}

// Hat das Gerät einen Playback-Pfad (Handy -> Gerät)? Gültig nach start().
JNIEXPORT jboolean JNICALL
Java_com_tonverk_bridge_NativeUsb_hasPlayback(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz; return g_dev.out.valid ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_tonverk_bridge_NativeUsb_hasCapture(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz; return g_dev.in.valid ? JNI_TRUE : JNI_FALSE;
}
