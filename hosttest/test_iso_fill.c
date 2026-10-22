// Host-Test: Prüft, dass der Bytestrom, den der Kernel (usbfs, kumulative
// Paket-Offsets) tatsächlich aus dem Transferpuffer sendet, bei variablen
// ISO-Paketgrößen kontinuierlich bleibt (keine Knackser durch übersprungene
// oder doppelte Frames an Paketgrenzen).
//
// Eingespeist wird eine Rampe (Frame i -> Wert i*100 auf L und R). Nach dem
// Resampling muss jedes Delta benachbarter Output-Samples nahe 100*step
// liegen. Offset-Fehler erzeugen Deltas von ~0 oder >=200 bzw. ±600.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_audio.c"

// ---- libusb-Stubs (werden vom Test nicht wirklich gebraucht, nur Linker) ----
int libusb_set_option(libusb_context *ctx, int option, ...) { (void)ctx; (void)option; return 0; }
int libusb_init(libusb_context **ctx) { (void)ctx; return 0; }
void libusb_exit(libusb_context *ctx) { (void)ctx; }
int libusb_wrap_sys_device(libusb_context *ctx, intptr_t fd, libusb_device_handle **h) { (void)ctx; (void)fd; (void)h; return 0; }
int libusb_set_auto_detach_kernel_driver(libusb_device_handle *h, int e) { (void)h; (void)e; return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_control_transfer(libusb_device_handle *h, uint8_t t, uint8_t r, uint16_t v,
                            uint16_t i, unsigned char *d, uint16_t l, unsigned int to)
{ (void)h;(void)t;(void)r;(void)v;(void)i;(void)d;(void)l;(void)to; return 0; }
int libusb_set_interface_alt_setting(libusb_device_handle *h, int i, int a) { (void)h;(void)i;(void)a; return 0; }
struct libusb_transfer *libusb_alloc_transfer(int n) { (void)n; return calloc(1, sizeof(struct libusb_transfer)); }
void libusb_free_transfer(struct libusb_transfer *t) { free(t); }
int libusb_submit_transfer(struct libusb_transfer *t) { (void)t; return 0; }
int libusb_cancel_transfer(struct libusb_transfer *t) { (void)t; return 0; }
int libusb_handle_events_timeout_completed(libusb_context *c, struct timeval *tv, int *x) { (void)c;(void)tv;(void)x; return 0; }
void libusb_close(libusb_device_handle *h) { (void)h; }
const char *libusb_error_name(int code) { (void)code; return "stub"; }

static int32_t dec24(const uint8_t *p) {
    int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    if (v & 0x800000) v -= 0x1000000;
    return v;
}

#define MAX_OUT 2000000
static int32_t outL[MAX_OUT];
static size_t n_out = 0;

// Liest den Transferpuffer so aus, wie der Kernel ihn senden würde:
// Paket p beginnt bei der Summe der Längen der Pakete davor.
static void kernel_read(struct libusb_transfer *x) {
    size_t off = 0;
    for (int p = 0; p < x->num_iso_packets; p++) {
        unsigned len = x->iso_packet_desc[p].length;
        for (unsigned b = 0; b + FRAME_BYTES <= len; b += FRAME_BYTES) {
            if (n_out < MAX_OUT) outL[n_out++] = dec24(x->buffer + off + b);
        }
        off += len;
    }
}

int main(void) {
    const int IN_FRAMES = 40000;

    sinc_init();
    g_cap = (size_t)(48000 * 2);
    g_rb = malloc(g_cap * sizeof(int32_t));
    memset(g_rb, 0, g_cap * sizeof(int32_t));
    g_head = g_tail = 0; g_rpos = 0.0; g_step = 1.0;
    for (int i = 0; i < IN_FRAMES; i++) {
        g_rb[g_head] = i * 100; g_head = (g_head + 1) % g_cap;
        g_rb[g_head] = i * 100; g_head = (g_head + 1) % g_cap;
    }
    g_prime_frames = 7200;
    g_primed = 0;
    g_running = 1;
    g_tone = 0;
    g_fb_spp = 48.5;        // erzwingt abwechselnd 48/49-Frame-Pakete
    g_frac = 0.0;

    struct libusb_transfer *x[2];
    for (int i = 0; i < 2; i++) {
        x[i] = libusb_alloc_transfer(PKTS_PER_XFER);
        unsigned char *buf = malloc(PKTS_PER_XFER * MAX_PKT);
        memset(buf, 0, PKTS_PER_XFER * MAX_PKT);
        libusb_fill_iso_transfer(x[i], NULL, EP_OUT, buf, PKTS_PER_XFER * MAX_PKT,
                                 PKTS_PER_XFER, xfer_cb, NULL, 1000);
        libusb_set_iso_packet_lengths(x[i], MAX_PKT);
        for (int p = 0; p < PKTS_PER_XFER; p++)
            x[i]->iso_packet_desc[p].length = FRAMES_PER_PKT * FRAME_BYTES;
        x[i]->status = LIBUSB_TRANSFER_COMPLETED;
    }

    for (int call = 0; call < 60; call++) {
        struct libusb_transfer *t = x[call % 2];
        t->status = LIBUSB_TRANSFER_COMPLETED;
        xfer_cb(t);          // befüllt die Pakete
        kernel_read(t);      // liest wie der Kernel senden würde
    }

    if (n_out < 20000) {
        printf("FAIL: zu wenig Output (%zu Samples)\n", n_out);
        return 1;
    }

    // Fade-in/Einschwingen überspringen, dann Kontinuität prüfen.
    int bad = 0;
    for (size_t i = 2000; i + 1 < n_out; i++) {
        int32_t d = outL[i + 1] - outL[i];
        if (d < 40 || d > 170) {
            if (bad < 10)
                printf("Diskontinuität bei Sample %zu: %d -> %d (delta %d)\n",
                       i, outL[i], outL[i + 1], d);
            bad++;
        }
    }
    printf("%zu Output-Samples, %d Diskontinuitäten\n", n_out, bad);
    if (bad) { printf("FAIL\n"); return 1; }

    // ---- Teil 2: Sinc-Interpolator numerisch prüfen (Zentrierung + Tabelle) ----
    // Rekonstruiert einen Sinus an fraktionalen Positionen mit exakt der
    // Tap-Konvention des Render-Loops und vergleicht mit sin() als Referenz.
    double freqs[2] = { 1000.0, 10000.0 };
    double maxerr_allow[2] = { 2e-3, 5e-2 };
    for (int fc = 0; fc < 2; fc++) {
        double w = 2.0 * M_PI * freqs[fc] / 48000.0;
        double maxerr = 0.0;
        for (int fi = 100; fi < 200; fi++) {
            for (int sub = 0; sub < 10; sub++) {
                double frac = sub / 10.0 + 0.05;
                double pf = frac * SINC_PHASES;
                int ph = (int) pf;
                float pr = (float)(pf - ph);
                double acc = 0.0;
                for (int t = 0; t < SINC_TAPS; t++) {
                    float wt = g_sinc[ph][t] + (g_sinc[ph + 1][t] - g_sinc[ph][t]) * pr;
                    acc += sin(w * (double)(fi - (SINC_HALF - 1) + t)) * (double) wt;
                }
                double ref = sin(w * ((double) fi + frac));
                double e = fabs(acc - ref);
                if (e > maxerr) maxerr = e;
            }
        }
        printf("Sinc %5.0f Hz: max. Fehler %.5f (erlaubt %.5f)\n",
               freqs[fc], maxerr, maxerr_allow[fc]);
        if (maxerr > maxerr_allow[fc]) { printf("FAIL\n"); return 1; }
    }

    printf("PASS\n");
    return 0;
}
