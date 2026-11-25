// Host-Test für den generischen UAC1/UAC2-Treiber.
//
// Simuliert verschiedene Gerätetypen über ein kleines Gerätemodell:
//   - Config-Deskriptor (mit Baukasten erzeugt),
//   - Antworten auf UAC2-Clock-Requests (RANGE/CUR) und UAC1-Endpoint-Rate,
//   - Mitschnitt aller SET_CUR-Requests und ISO-Submits.
// Dann läuft do_start() wie auf dem Handy, und der vom Treiber erzeugte OUT-Strom
// wird so gelesen, wie ihn usbfs senden würde (kumulative Paket-Offsets), im
// Geräteformat dekodiert und auf Frequenz, Kanalzuordnung und Kontinuität geprüft.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_audio.c"

// ======================= Gerätemodell =======================

static uint8_t g_desc[2048];
static size_t g_desc_len = 0;
static int g_ranges[8][2], g_nranges = 0;     // UAC2: unterstützte Raten [min,max]
static int g_cur_rate[256];                   // UAC2: CUR je Clock-ID
static int g_ep_rate[256];                    // UAC1: CUR je Endpoint
static int g_set_widx[32], g_set_val[32], g_nset = 0;   // Mitschnitt SET_CUR
static int g_submits[256];
static int g_claimed[32];

static void model_reset(void) {
    g_desc_len = 0; g_nranges = 0; g_nset = 0;
    memset(g_cur_rate, 0, sizeof g_cur_rate);
    memset(g_ep_rate, 0, sizeof g_ep_rate);
    memset(g_submits, 0, sizeof g_submits);
    memset(g_claimed, 0, sizeof g_claimed);
}

int libusb_set_option(libusb_context *ctx, int option, ...) { (void)ctx; (void)option; return 0; }
int libusb_init(libusb_context **ctx) { *ctx = (libusb_context *) 1; return 0; }
void libusb_exit(libusb_context *ctx) { (void)ctx; }
int libusb_wrap_sys_device(libusb_context *ctx, intptr_t fd, libusb_device_handle **h)
{ (void)ctx; (void)fd; *h = (libusb_device_handle *) 1; return 0; }
int libusb_set_auto_detach_kernel_driver(libusb_device_handle *h, int e) { (void)h; (void)e; return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h; if (i >= 0 && i < 32) g_claimed[i]++; return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h; if (i >= 0 && i < 32) g_claimed[i]--; return 0; }
int libusb_set_interface_alt_setting(libusb_device_handle *h, int i, int a) { (void)h;(void)i;(void)a; return 0; }

static int rd32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24); }

int libusb_control_transfer(libusb_device_handle *h, uint8_t t, uint8_t r, uint16_t v,
                            uint16_t i, unsigned char *d, uint16_t l, unsigned int to) {
    (void)h; (void)to;
    if (t == 0x80 && r == 0x06 && (v >> 8) == 0x02) {           // GET_DESCRIPTOR(CONFIG)
        size_t n = g_desc_len < l ? g_desc_len : l;
        memcpy(d, g_desc, n); return (int) n;
    }
    int clk = i >> 8;
    if (t == 0xA1 && r == 0x02 && v == 0x0100) {                 // UAC2 RANGE
        if (!g_nranges) return LIBUSB_ERROR_PIPE;
        int n = 2 + 12 * g_nranges; if (n > l) return LIBUSB_ERROR_OVERFLOW;
        d[0] = (uint8_t) g_nranges; d[1] = 0;
        for (int k = 0; k < g_nranges; k++) {
            le32(d + 2 + 12 * k, g_ranges[k][0]); le32(d + 6 + 12 * k, g_ranges[k][1]);
            le32(d + 10 + 12 * k, 0);
        }
        return n;
    }
    if (t == 0x21 && r == 0x01 && v == 0x0100) {                 // UAC2 SET_CUR
        int rate = rd32(d), ok = 0;
        for (int k = 0; k < g_nranges; k++) if (rate >= g_ranges[k][0] && rate <= g_ranges[k][1]) ok = 1;
        if (g_nset < 32) { g_set_widx[g_nset] = i; g_set_val[g_nset] = rate; g_nset++; }
        if (!ok) return LIBUSB_ERROR_PIPE;
        g_cur_rate[clk] = rate; return 4;
    }
    if (t == 0xA1 && r == 0x01 && v == 0x0100) {                 // UAC2 GET_CUR
        if (!g_cur_rate[clk]) return LIBUSB_ERROR_PIPE;
        le32(d, g_cur_rate[clk]); return 4;
    }
    if (t == 0x22 && r == 0x01 && v == 0x0100) {                 // UAC1 SET_CUR (Endpoint)
        int rate = d[0] | (d[1] << 8) | (d[2] << 16);
        if (g_nset < 32) { g_set_widx[g_nset] = i; g_set_val[g_nset] = rate; g_nset++; }
        g_ep_rate[i & 0xff] = rate; return 3;
    }
    if (t == 0xA2 && r == 0x81 && v == 0x0100) {                 // UAC1 GET_CUR (Endpoint)
        int rate = g_ep_rate[i & 0xff]; if (!rate) return LIBUSB_ERROR_PIPE;
        d[0] = rate & 0xff; d[1] = (rate >> 8) & 0xff; d[2] = (rate >> 16) & 0xff; return 3;
    }
    return 0;
}
struct libusb_transfer *libusb_alloc_transfer(int n) { (void)n; return calloc(1, sizeof(struct libusb_transfer)); }
void libusb_free_transfer(struct libusb_transfer *t) { free(t); }
int libusb_submit_transfer(struct libusb_transfer *t) { g_submits[t->endpoint]++; return 0; }
int libusb_cancel_transfer(struct libusb_transfer *t) { (void)t; if (g_active > 0) g_active--; return 0; }
int libusb_handle_events_timeout_completed(libusb_context *c, struct timeval *tv, int *x)
{ (void)c;(void)tv;(void)x; usleep(1000); return 0; }
void libusb_close(libusb_device_handle *h) { (void)h; }
const char *libusb_error_name(int code) { (void)code; return "stub"; }

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

// ======================= Deskriptor-Baukasten =======================

static size_t D;
#define B(...) do { uint8_t b_[] = { __VA_ARGS__ }; memcpy(g_desc + D, b_, sizeof b_); D += sizeof b_; } while (0)
#define LO(x) (uint8_t)((x) & 0xff)
#define HI(x) (uint8_t)(((x) >> 8) & 0xff)
#define B3(x) LO(x), HI(x), (uint8_t)(((x) >> 16) & 0xff)

static void cfg_begin(void) { model_reset(); D = 9; }
static void cfg_end(void) {
    uint8_t cfg[] = { 9,0x02, LO(D),HI(D), 6, 1, 0, 0x80, 50 };
    memcpy(g_desc, cfg, 9); g_desc_len = D;
}
static void iface(int n, int alt, int neps, int sub, int proto) { B(9,0x04, (uint8_t)n,(uint8_t)alt,(uint8_t)neps, 0x01,(uint8_t)sub,(uint8_t)proto, 0); }
static void ep(int addr, int attr, int mps, int interval) { B(7,0x05, (uint8_t)addr,(uint8_t)attr, LO(mps),HI(mps), (uint8_t)interval); }
static void ep1(int addr, int attr, int mps, int interval, int synch) {   // UAC1: 9 Byte
    B(9,0x05, (uint8_t)addr,(uint8_t)attr, LO(mps),HI(mps), (uint8_t)interval, 0, (uint8_t)synch);
}
static void cs_ep(void) { B(8,0x25, 0x01, 0, 0, 0, 0,0); }
// UAC2 AudioControl
static void u2_header(void) { B(9,0x24, 0x01, 0x00,0x02, 0x08, 0x40,0x00, 0x00); }
static void u2_clock(int id) { B(8,0x24, 0x0A, (uint8_t)id, 0x01, 0x07, 0, 0); }
static void u2_sel(int id, int a, int b) { B(9,0x24, 0x0B, (uint8_t)id, 2, (uint8_t)a, (uint8_t)b, 0x03, 0); }
static void u2_it(int id, int type, int clk, int ch) { B(17,0x24, 0x02, (uint8_t)id, LO(type),HI(type), 0, (uint8_t)clk, (uint8_t)ch, 0,0,0,0, 0, 0,0, 0); }
static void u2_ot(int id, int type, int src, int clk) { B(12,0x24, 0x03, (uint8_t)id, LO(type),HI(type), 0, (uint8_t)src, (uint8_t)clk, 0,0, 0); }
// UAC2 AudioStreaming
static void u2_asg(int term, int ch) { B(16,0x24, 0x01, (uint8_t)term, 0, 0x01, 0x01,0,0,0, (uint8_t)ch, 0,0,0,0, 0); }
static void u2_fmt(int ss, int bits) { B(6,0x24, 0x02, 0x01, (uint8_t)ss, (uint8_t)bits); }
// UAC1
static void u1_header(int n_as, int as1, int as2) { B(10,0x24, 0x01, 0x00,0x01, 30,0, (uint8_t)n_as, (uint8_t)as1, (uint8_t)as2); }
static void u1_it(int id, int type, int ch) { B(12,0x24, 0x02, (uint8_t)id, LO(type),HI(type), 0, (uint8_t)ch, 0x03,0x00, 0, 0); }
static void u1_ot(int id, int type, int src) { B(9,0x24, 0x03, (uint8_t)id, LO(type),HI(type), 0, (uint8_t)src, 0); }
static void u1_asg(int term) { B(7,0x24, 0x01, (uint8_t)term, 1, 0x01,0x00); }
static void u1_fmt2(int ch, int ss, int bits, int r1, int r2) {
    B(14,0x24, 0x02, 0x01, (uint8_t)ch, (uint8_t)ss, (uint8_t)bits, 2, B3(r1), B3(r2));
}
static void u1_fmt1(int ch, int ss, int bits, int r1) { B(11,0x24, 0x02, 0x01, (uint8_t)ch, (uint8_t)ss, (uint8_t)bits, 1, B3(r1)); }

// ======================= Audio-Pfad durchspielen =======================

#define MAXS 400000
static double g_dec[8][MAXS];   // dekodierte Kanäle (normiert auf ±1)
static size_t g_ndec = 0;

static double dec_sample(const uint8_t *p, int ss) {
    if (ss == 2) return (double)(int16_t)(p[0] | (p[1] << 8)) / 32768.0;
    if (ss == 3) { int32_t v = p[0] | (p[1] << 8) | (p[2] << 16); if (v & 0x800000) v -= 0x1000000; return v / 8388608.0; }
    return (double)(int32_t)((uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24)) / 2147483648.0;
}

// Liest einen Transfer wie usbfs (Paket p beginnt nach der Summe der Längen davor).
static void kernel_read(struct libusb_transfer *x) {
    int nch = g_dev.out.channels, ss = g_dev.out.subslot, fb = nch * ss;
    size_t off = 0;
    for (int p = 0; p < x->num_iso_packets; p++) {
        unsigned len = x->iso_packet_desc[p].length;
        CHECK(len % (unsigned) fb == 0, "Paketlänge %u kein Vielfaches der Framegröße %d", len, fb);
        CHECK(len <= (unsigned) g_dev.out.max_pkt, "Paketlänge %u > wMaxPacketSize %d", len, g_dev.out.max_pkt);
        for (unsigned b = 0; b + (unsigned) fb <= len && g_ndec < MAXS; b += (unsigned) fb, g_ndec++)
            for (int c = 0; c < nch && c < 8; c++) g_dec[c][g_ndec] = dec_sample(x->buffer + off + b + (size_t) c * ss, ss);
        off += len;
    }
}

// Speist Stereo-Sinus (L = f Hz, R = halbe Amplitude) mit 48 kHz ein und lässt den
// OUT-Pfad laufen, bis `seconds` Geräte-Audio erzeugt sind.
static void run_audio(double f, double seconds) {
    g_ndec = 0;
    static double ph = 0.0;
    size_t want = (size_t)(seconds * g_dev_rate);
    double fed = 0.0;   // eingespeiste Input-Frames
    for (int call = 0; g_ndec < want && call < 100000; call++) {
        // Echtzeit-Input wie der AudioRecord-Thread: Vorlauf (Prime + Reserve) plus
        // genau so viel, wie das Gerät bis zum Ende dieses Transfers verbraucht hat.
        double target = (double) g_prime_frames + (double) SINC_HALF +
                        (double)(g_ndec + (size_t)(g_pkts_per_xfer * g_spp_nom)) * g_step_base;
        pthread_mutex_lock(&g_lock);
        while (fed < target) {
            int32_t l = (int32_t) lrint(0.5 * sin(ph) * 8388607.0);
            ph += 2.0 * M_PI * f / 48000.0;
            g_rb[g_head] = l; g_head = (g_head + 1) % g_cap;
            g_rb[g_head] = l / 2; g_head = (g_head + 1) % g_cap;
            fed += 1.0;
        }
        pthread_mutex_unlock(&g_lock);
        struct libusb_transfer *t = g_xfers[call % NUM_TRANSFERS];
        t->status = LIBUSB_TRANSFER_COMPLETED;
        xfer_cb(t);
        kernel_read(t);
    }
    CHECK(g_underflow == 0, "Underflows: %ld", g_underflow);
    CHECK(fabs(g_step / g_step_base - 1.0) < 0.002, "Regler weggelaufen: step %.5f (Basis %.5f)", g_step, g_step_base);
}

// Frequenz über Nulldurchgänge (nach Einschwingen), Kontinuität über max. Sprung.
static void check_audio(const char *name, double f) {
    int nch = g_dev.out.channels;
    size_t s0 = (size_t)(2.0 * g_dev_rate);         // Einschwingen des Füllstandsreglers abwarten
    CHECK(g_ndec > s0 + 1000, "%s: zu wenig Output (%zu)", name, g_ndec);
    if (g_ndec <= s0 + 1000) return;
    const double *L = g_dec[0];
    double ref[MAXS]; static double tmp[MAXS];
    (void) ref;
    for (size_t i = s0; i < g_ndec; i++) tmp[i] = nch == 1 ? L[i] / 0.75 : L[i];
    int zc = 0; size_t first = 0, last = 0;
    for (size_t i = s0 + 1; i < g_ndec; i++)
        if ((tmp[i - 1] < 0) != (tmp[i] < 0)) { if (!zc) first = i; last = i; zc++; }
    double fm = zc > 2 ? (zc - 1) / 2.0 / ((double)(last - first) / g_dev_rate) : 0;
    CHECK(fabs(fm / f - 1.0) < 0.005, "%s: Frequenz %.2f Hz statt %.0f Hz (Rate %d)", name, fm, f, g_dev_rate);
    double maxjump = 0, peak = 0, expjump = 2 * M_PI * f / g_dev_rate * 0.5 * 1.3 + 1e-3;
    for (size_t i = s0 + 1; i < g_ndec; i++) {
        double j = fabs(tmp[i] - tmp[i - 1]); if (j > maxjump) maxjump = j;
        if (fabs(tmp[i]) > peak) peak = fabs(tmp[i]);
    }
    CHECK(maxjump < expjump, "%s: Sprung %.4f > %.4f (Knackser)", name, maxjump, expjump);
    CHECK(fabs(peak - 0.5) < 0.02, "%s: Amplitude %.3f statt 0.5", name, peak);
    if (nch >= 2) {
        double pr = 0; for (size_t i = s0; i < g_ndec; i++) if (fabs(g_dec[1][i]) > pr) pr = fabs(g_dec[1][i]);
        CHECK(fabs(pr - 0.25) < 0.02, "%s: rechter Kanal %.3f statt 0.25", name, pr);
    }
    for (int c = 2; c < nch && c < 8; c++) {
        double pc = 0; for (size_t i = s0; i < g_ndec; i++) if (fabs(g_dec[c][i]) > pc) pc = fabs(g_dec[c][i]);
        CHECK(pc == 0.0, "%s: Kanal %d nicht still (%.4f)", name, c + 1, pc);
    }
    printf("  %-34s %dch %dB @%d Hz: f=%.2f Hz, peak=%.3f, max. Sprung %.4f\n",
           name, nch, g_dev.out.subslot, g_dev_rate, fm, peak, maxjump);
}

static int last_set_for(int widx) { int v = 0; for (int k = 0; k < g_nset; k++) if (g_set_widx[k] == widx) v = g_set_val[k]; return v; }

// ======================= Testfälle =======================

// Tonverk (neues OS): IF3 AC, Clock 7, IF4 OUT 0x03 ohne Feedback, IF5 IN 0x84 4ch.
static void build_tonverk(int explicit_fb) {
    cfg_begin();
    iface(3,0, 0, 0x01, 0x20); u2_header(); u2_clock(7);
    u2_it(1, 0x0101, 7, 2); u2_ot(2, 0x0301, 1, 7);      // USB-Streaming -> Line out
    u2_it(3, 0x0201, 7, 4); u2_ot(4, 0x0101, 3, 7);      // Line in -> USB-Streaming
    iface(4,0, 0, 0x02, 0x20);
    iface(4,1, explicit_fb ? 2 : 1, 0x02, 0x20); u2_asg(1, 2); u2_fmt(3, 24);
    ep(0x03, 0x05, 294, 4); cs_ep();
    if (explicit_fb) ep(0x84, 0x11, 4, 4);
    iface(5,0, 0, 0x02, 0x20);
    iface(5,1, 1, 0x02, 0x20); u2_asg(4, 4); u2_fmt(3, 24);
    ep(explicit_fb ? 0x83 : 0x84, 0x05, 588, 4); cs_ep();
    cfg_end();
    g_nranges = 1; g_ranges[0][0] = g_ranges[0][1] = 48000;
}

static void test_tonverk(void) {
    printf("Tonverk (neues OS, kein Feedback-EP)\n");
    build_tonverk(0);
    g_stub_speed = LIBUSB_SPEED_HIGH;
    CHECK(do_start(0, 48000) == 0, "do_start");
    CHECK(g_dev.ac_if == 3 && g_dev.out.ifnum == 4 && g_dev.out.ep == 0x03, "Layout: AC %d OUT IF%d EP 0x%02X",
          g_dev.ac_if, g_dev.out.ifnum, g_dev.out.ep);
    CHECK(g_dev.out.clock_id == 7, "Clock %d statt 7", g_dev.out.clock_id);
    CHECK(last_set_for(0x0703) == 48000, "SET_CUR 48 kHz auf Clock 7 / IF3 fehlt (wie bisher fest verdrahtet)");
    CHECK(g_submits[0x84] == 0, "Submit auf 0x84 (IN-EP von IF5!)");
    CHECK(g_pkts_per_xfer == 8 && fabs(g_spp_nom - 48.0) < 1e-9, "Timing: %d Pakete, spp %.2f", g_pkts_per_xfer, g_spp_nom);
    run_audio(1000.0, 4.0); check_audio("Tonverk OUT", 1000.0);
    CHECK(capture_start() == 0, "capture_start");
    CHECK(g_cap_rate == 48000 && g_dev.in.channels == 4 && g_dev.in.ep == 0x84, "Capture: %d Hz %dch EP 0x%02X",
          g_cap_rate, g_dev.in.channels, g_dev.in.ep);
    CHECK(g_submits[0x84] == NUM_CAP_XFERS, "Capture-Submits auf 0x84: %d", g_submits[0x84]);
    native_stop();
    CHECK(g_claimed[3] == 0 && g_claimed[4] == 0 && g_claimed[5] == 0, "Interfaces nicht freigegeben (%d/%d/%d)",
          g_claimed[3], g_claimed[4], g_claimed[5]);

    printf("Tonverk (altes OS, Feedback 0x84 pro ms)\n");
    build_tonverk(1);
    CHECK(do_start(0, 48000) == 0, "do_start");
    CHECK(g_dev.out.fb_ep == 0x84 && g_submits[0x84] == NUM_FB, "Feedback-EP 0x%02X, %d Submits", g_dev.out.fb_ep, g_submits[0x84]);
    // Feedback 48,02 Samples/ms im 16.16-Format (so meldet es der Tonverk)
    struct libusb_transfer *x = g_fb_xfers[0];
    unsigned v = (unsigned) lrint(48.02 * 65536.0);
    x->buffer[0] = v & 0xff; x->buffer[1] = (v >> 8) & 0xff; x->buffer[2] = (v >> 16) & 0xff; x->buffer[3] = v >> 24;
    x->iso_packet_desc[0].actual_length = 4; x->status = LIBUSB_TRANSFER_COMPLETED;
    fb_cb(x);
    CHECK(fabs(g_fb_spp - 48.02) < 1e-4 && g_fb_shift == 0, "Feedback %.4f (shift %d) statt 48.02", g_fb_spp, g_fb_shift);
    native_stop();
}

// UAC1 Full-Speed-Dongle: 16 Bit Stereo out (44,1/48 kHz, adaptive), Mono-Mic in.
static void test_uac1_dongle(void) {
    printf("UAC1 Full-Speed (C-Media-artig)\n");
    cfg_begin();
    iface(0,0, 0, 0x01, 0x00); u1_header(2, 1, 2);
    u1_it(1, 0x0101, 2); u1_ot(2, 0x0301, 1); u1_it(3, 0x0201, 1); u1_ot(4, 0x0101, 3);
    iface(1,0, 0, 0x02, 0); iface(1,1, 1, 0x02, 0); u1_asg(1); u1_fmt2(2, 2, 16, 44100, 48000);
    ep1(0x01, 0x09, 192, 1, 0); B(7,0x25, 0x01, 0x01, 0, 0,0);
    iface(2,0, 0, 0x02, 0); iface(2,1, 1, 0x02, 0); u1_asg(4); u1_fmt1(1, 2, 16, 48000);
    ep1(0x82, 0x05, 96, 1, 0); B(7,0x25, 0x01, 0x01, 0, 0,0);
    cfg_end();
    g_stub_speed = LIBUSB_SPEED_FULL;
    CHECK(do_start(0, 48000) == 0, "do_start");
    CHECK(g_dev.uac == 1 && g_dev.out.ep == 0x01 && g_dev.out.subslot == 2 && g_dev.out.fb_ep == 0, "Parse UAC1 OUT");
    CHECK(last_set_for(0x01) == 48000 && g_dev_rate == 48000, "UAC1 SET_CUR 48 kHz auf EP 0x01");
    CHECK(g_dev.out.sync == UAC_SYNC_ADAPTIVE, "Sync-Art %d", g_dev.out.sync);
    run_audio(1000.0, 4.0); check_audio("UAC1 16 Bit Stereo", 1000.0);
    CHECK(capture_start() == 0 && g_dev.in.channels == 1 && last_set_for(0x82) == 48000, "UAC1 Capture Mono 48 kHz");
    native_stop();
}

// UAC1 async mit Sync-EP (bSynchAddress), nur 44,1 kHz.
static void test_uac1_async_441(void) {
    printf("UAC1 async + Sync-EP, nur 44,1 kHz\n");
    cfg_begin();
    iface(0,0, 0, 0x01, 0x00); u1_header(1, 1, 0);
    u1_it(1, 0x0101, 2); u1_ot(2, 0x0301, 1);
    iface(1,0, 0, 0x02, 0); iface(1,1, 2, 0x02, 0); u1_asg(1); u1_fmt1(2, 3, 24, 44100);
    ep1(0x01, 0x05, 300, 1, 0x81); B(7,0x25, 0x01, 0x01, 0, 0,0);
    ep1(0x81, 0x01, 3, 1, 0);                         // Sync-EP: Usage-Bits 0!
    cfg_end();
    g_stub_speed = LIBUSB_SPEED_FULL;
    CHECK(do_start(0, 48000) == 0, "do_start");
    CHECK(g_dev.out.fb_ep == 0x81 && !g_dev.in.valid, "Sync-EP 0x%02X als Feedback, kein Capture (in.valid=%d)",
          g_dev.out.fb_ep, g_dev.in.valid);
    CHECK(g_dev_rate == 44100 && fabs(g_spp_nom - 44.1) < 1e-9, "Rate %d, spp %.2f", g_dev_rate, g_spp_nom);
    CHECK(g_submits[0x81] == NUM_FB, "Feedback-Submits %d", g_submits[0x81]);
    // 10.14-Feedback (3 Byte): 44,1 Samples/Frame
    struct libusb_transfer *x = g_fb_xfers[0];
    unsigned v = (unsigned) lrint(44.1 * 16384.0);
    x->buffer[0] = v & 0xff; x->buffer[1] = (v >> 8) & 0xff; x->buffer[2] = (v >> 16) & 0xff;
    x->iso_packet_desc[0].actual_length = 3; x->status = LIBUSB_TRANSFER_COMPLETED;
    fb_cb(x);
    CHECK(fabs(g_fb_spp - 44.1) < 1e-3, "10.14-Feedback %.4f statt 44.1", g_fb_spp);
    run_audio(1000.0, 4.0); check_audio("UAC1 24 Bit @44,1 (Resampling)", 1000.0);
    CHECK(capture_start() == -3, "Capture ohne IN-Interface muss -3 liefern");
    native_stop();
}

// UAC2 High-Speed, bInterval 1 (125 µs), 32-Bit-Slots, expl. Feedback pro Microframe.
static void test_uac2_hs_fb(void) {
    printf("UAC2 High-Speed 125 us + Feedback (Scarlett-artig)\n");
    cfg_begin();
    iface(0,0, 0, 0x01, 0x20); u2_header(); u2_clock(0x29);
    u2_it(1, 0x0101, 0x29, 2); u2_ot(2, 0x0301, 1, 0x29);
    u2_it(3, 0x0201, 0x29, 2); u2_ot(4, 0x0101, 3, 0x29);
    iface(1,0, 0, 0x02, 0x20); iface(1,1, 2, 0x02, 0x20); u2_asg(1, 2); u2_fmt(4, 24);
    ep(0x01, 0x05, 72, 1); cs_ep(); ep(0x81, 0x11, 4, 4);
    iface(2,0, 0, 0x02, 0x20); iface(2,1, 1, 0x02, 0x20); u2_asg(4, 2); u2_fmt(4, 24);
    ep(0x82, 0x25, 72, 1); cs_ep();                    // IN, async, Usage 2 (impl. Feedback-Daten)
    cfg_end();
    g_nranges = 2; g_ranges[0][0] = g_ranges[0][1] = 44100; g_ranges[1][0] = g_ranges[1][1] = 48000;
    g_stub_speed = LIBUSB_SPEED_HIGH;
    CHECK(do_start(0, 48000) == 0, "do_start");
    CHECK(g_pkts_per_xfer == 64 && fabs(g_spp_nom - 6.0) < 1e-9, "HS-Timing: %d Pakete, spp %.3f", g_pkts_per_xfer, g_spp_nom);
    CHECK(last_set_for(0x2900) == 48000, "SET_CUR Clock 0x29/IF0");
    struct libusb_transfer *x = g_fb_xfers[0];
    unsigned v = (unsigned) lrint(6.001 * 65536.0);    // 16.16 pro Microframe
    x->buffer[0] = v & 0xff; x->buffer[1] = (v >> 8) & 0xff; x->buffer[2] = (v >> 16) & 0xff; x->buffer[3] = v >> 24;
    x->iso_packet_desc[0].actual_length = 4; x->status = LIBUSB_TRANSFER_COMPLETED;
    fb_cb(x);
    CHECK(fabs(g_fb_spp - 6.001) < 1e-4, "HS-Feedback %.4f statt 6.001", g_fb_spp);
    run_audio(1000.0, 4.0); check_audio("UAC2 HS 32-Bit-Slot", 1000.0);
    CHECK(capture_start() == 0 && g_dev.in.ep == 0x82 && g_cap_pkts_per_xfer == 64, "Capture EP 0x%02X, %d Pakete",
          g_dev.in.ep, g_cap_pkts_per_xfer);
    native_stop();
}

// UAC2 HS, bInterval 4, Feedback spec-konform pro Microframe (6.0) -> Shift 3 nötig.
static void test_fb_shift(void) {
    printf("Feedback-Skalierung (pro Microframe bei 1-ms-Paketen)\n");
    build_tonverk(1);
    CHECK(do_start(0, 48000) == 0, "do_start");
    struct libusb_transfer *x = g_fb_xfers[0];
    unsigned v = (unsigned) lrint(6.0025 * 65536.0);
    x->buffer[0] = v & 0xff; x->buffer[1] = (v >> 8) & 0xff; x->buffer[2] = (v >> 16) & 0xff; x->buffer[3] = v >> 24;
    x->iso_packet_desc[0].actual_length = 4; x->status = LIBUSB_TRANSFER_COMPLETED;
    fb_cb(x);
    CHECK(g_fb_shift == 3 && fabs(g_fb_spp - 48.02) < 1e-4, "shift %d, spp %.4f (erwartet 3 / 48.02)", g_fb_shift, g_fb_spp);
    // Ausreißer darf nicht übernommen werden
    v = (unsigned) lrint(9.0 * 65536.0);
    x->buffer[0] = v & 0xff; x->buffer[1] = (v >> 8) & 0xff; x->buffer[2] = (v >> 16) & 0xff; x->buffer[3] = v >> 24;
    fb_cb(x);
    CHECK(fabs(g_fb_spp - 48.02) < 1e-4, "Ausreißer übernommen: spp %.4f", g_fb_spp);
    native_stop();
}

// UAC2 Mehrkanal mit mehreren Alt-Settings und Clock-Selector.
static void test_uac2_multich(void) {
    printf("UAC2 Mehrkanal, Alt-Auswahl, Clock-Selector\n");
    cfg_begin();
    iface(0,0, 0, 0x01, 0x20); u2_header(); u2_clock(0x29); u2_clock(0x2A); u2_sel(0x28, 0x29, 0x2A);
    u2_it(1, 0x0101, 0x28, 8); u2_ot(2, 0x0301, 1, 0x28);
    u2_it(3, 0x0201, 0x2A, 8); u2_ot(4, 0x0101, 3, 0x2A);
    iface(1,0, 0, 0x02, 0x20);
    iface(1,1, 1, 0x02, 0x20); u2_asg(1, 8); u2_fmt(3, 24); ep(0x01, 0x05, 8*3*7, 1); cs_ep();
    iface(1,2, 1, 0x02, 0x20); u2_asg(1, 2); u2_fmt(2, 16); ep(0x01, 0x05, 2*2*7, 1); cs_ep();
    iface(1,3, 1, 0x02, 0x20); u2_asg(1, 2); u2_fmt(3, 24); ep(0x01, 0x05, 2*3*7, 1); cs_ep();
    iface(2,0, 0, 0x02, 0x20);
    iface(2,1, 1, 0x02, 0x20); u2_asg(4, 2); u2_fmt(3, 24); ep(0x82, 0x05, 2*3*7, 1); cs_ep();
    iface(2,2, 1, 0x02, 0x20); u2_asg(4, 8); u2_fmt(3, 24); ep(0x82, 0x05, 8*3*7, 1); cs_ep();
    cfg_end();
    g_nranges = 1; g_ranges[0][0] = 44100; g_ranges[0][1] = 96000;
    g_stub_speed = LIBUSB_SPEED_HIGH;
    CHECK(do_start(0, 48000) == 0, "do_start");
    CHECK(g_dev.out.alt == 3 && g_dev.out.channels == 2 && g_dev.out.bits == 24, "OUT-Alt %d (%dch %dbit), erwartet 3 (2ch 24bit)",
          g_dev.out.alt, g_dev.out.channels, g_dev.out.bits);
    CHECK(g_dev.out.clock_id == 0x29 && g_dev.in.clock_id == 0x2A, "Clocks %02X/%02X", g_dev.out.clock_id, g_dev.in.clock_id);
    CHECK(g_dev.in.alt == 2 && g_dev.in.channels == 8, "IN-Alt %d (%dch), erwartet 2 (8ch)", g_dev.in.alt, g_dev.in.channels);
    run_audio(440.0, 4.0); check_audio("UAC2 Stereo-Alt", 440.0);
    CHECK(capture_start() == 0 && last_set_for(0x2A00) == 48000 && g_cap_rate == 48000,
          "Capture-Clock 0x2A nicht auf 48 kHz gesetzt");
    native_stop();
}

// Mehrkanal-Playback ohne Stereo-Alt: L/R auf 1/2, Rest still. Und Mono-Gerät.
static void test_channel_mapping(void) {
    printf("Kanalzuordnung (4ch / Mono)\n");
    int chs[2] = { 4, 1 };
    for (int k = 0; k < 2; k++) {
        int ch = chs[k];
        cfg_begin();
        iface(0,0, 0, 0x01, 0x20); u2_header(); u2_clock(5); u2_it(1, 0x0101, 5, ch); u2_ot(2, 0x0301, 1, 5);
        iface(1,0, 0, 0x02, 0x20); iface(1,1, 1, 0x02, 0x20); u2_asg(1, ch); u2_fmt(3, 24);
        ep(0x01, 0x09, ch * 3 * 49, 4); cs_ep();
        cfg_end();
        g_nranges = 1; g_ranges[0][0] = g_ranges[0][1] = 48000;
        CHECK(do_start(0, 48000) == 0, "do_start");
        CHECK(!g_dev.in.valid && capture_start() == -3, "Playback-only-Gerät: Capture muss -3 liefern");
        run_audio(1000.0, 4.0); check_audio(ch == 1 ? "Mono (L+R)/2" : "4ch, Kanal 3/4 still", 1000.0);
        native_stop();
    }
}

// Reines Aufnahmegerät (kein OUT): Treiber startet, Capture läuft.
static void test_capture_only(void) {
    printf("Reines Aufnahmegerät\n");
    cfg_begin();
    iface(0,0, 0, 0x01, 0x20); u2_header(); u2_clock(9); u2_it(3, 0x0201, 9, 2); u2_ot(4, 0x0101, 3, 9);
    iface(1,0, 0, 0x02, 0x20); iface(1,1, 1, 0x02, 0x20); u2_asg(4, 2); u2_fmt(3, 24); ep(0x81, 0x05, 294, 4); cs_ep();
    cfg_end();
    g_nranges = 1; g_ranges[0][0] = g_ranges[0][1] = 48000;
    CHECK(do_start(0, 48000) == 0, "do_start");
    CHECK(!g_dev.out.valid && g_rb == NULL, "darf keinen Playback-Ring anlegen");
    CHECK(capture_start() == 0 && g_cap_rate == 48000 && last_set_for(0x0900) == 48000, "Capture-only: Clock 9 auf 48 kHz");
    native_stop();
    CHECK(g_claimed[0] == 0 && g_claimed[1] == 0, "Interfaces nicht freigegeben");
}

// Kein Audio-Gerät (z. B. reines MIDI-Gerät): sauberer Fehler.
static void test_no_audio(void) {
    printf("Kein Audio-Streaming-Interface\n");
    cfg_begin();
    iface(0,0, 0, 0x01, 0x00); u1_header(1, 1, 0);
    B(9,0x04, 1,0, 2, 0x01,0x03,0x00, 0);              // MIDIStreaming
    ep(0x01, 0x02, 64, 0); ep(0x81, 0x02, 64, 0);
    cfg_end();
    CHECK(do_start(0, 48000) == -7, "do_start muss -7 liefern");
    native_stop();
}

// Echter Geräte-Deskriptor (Hex-Datei aus lsusb2bin.py) durch den kompletten Pfad.
static void test_fixture(const char *path) {
    FILE *f = fopen(path, "r");
    CHECK(f != NULL, "%s nicht lesbar", path);
    if (!f) return;
    model_reset();
    // Hex-Bytes; "RAW"-Präfixe und Kommentarzeilen (#) der App-Dumps werden übersprungen.
    char tok[64];
    while (g_desc_len < sizeof g_desc && fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '#') { int ch; while ((ch = fgetc(f)) != EOF && ch != '\n') {} continue; }
        unsigned v; char rest;
        if (strlen(tok) == 2 && sscanf(tok, "%2x%c", &v, &rest) == 1) g_desc[g_desc_len++] = (uint8_t) v;
    }
    fclose(f);
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    printf("Fixture %s\n", name);
    // Gerät bietet 44,1/48/96 kHz (Antwort auf UAC2 RANGE); UAC1 nutzt die Deskriptor-Raten.
    g_nranges = 3;
    g_ranges[0][0] = g_ranges[0][1] = 44100; g_ranges[1][0] = g_ranges[1][1] = 48000;
    g_ranges[2][0] = g_ranges[2][1] = 96000;
    uac_dev pre; uac_parse(g_desc, g_desc_len, &pre);
    g_stub_speed = pre.uac == 2 ? LIBUSB_SPEED_HIGH : LIBUSB_SPEED_FULL;
    if (uac_parse(g_desc, g_desc_len, &pre) != 0) {
        // z. B. Elektron im Overbridge-Modus: nur herstellerspezifische Interfaces
        CHECK(do_start(0, 48000) == -7, "%s: Gerät ohne Audio-Streaming muss -7 liefern", name);
        native_stop();
        return;
    }
    CHECK(do_start(0, 48000) == 0, "%s: do_start", name);
    if (g_dev.out.valid) {
        CHECK(g_dev_rate == 48000, "%s: Rate %d", name, g_dev_rate);
        CHECK(g_submits[g_dev.out.ep] == NUM_TRANSFERS, "%s: OUT-Submits %d", name, g_submits[g_dev.out.ep]);
        for (int e = 0; e < 256; e++)
            if (g_submits[e] && e != g_dev.out.ep && e != g_dev.out.fb_ep)
                CHECK(0, "%s: Submit auf fremden EP 0x%02X", name, e);
        run_audio(1000.0, 3.0);
        check_audio(name, 1000.0);
    }
    if (strstr(name, "tonverk")) {
        CHECK(g_dev.ac_if == 3 && g_dev.out.ifnum == 4 && g_dev.out.ep == 0x03 && g_dev.in.ep == 0x84 &&
              g_dev.in.channels == 4, "%s: Tonverk-Layout falsch erkannt", name);
        CHECK(last_set_for(0x0703) == 48000, "%s: SET_CUR muss an Clock 7 / IF3 gehen", name);
    }
    if (g_dev.in.valid) {
        CHECK(capture_start() == 0, "%s: capture_start", name);
        CHECK(g_cap_rate == 48000, "%s: Capture-Rate %d", name, g_cap_rate);
    }
    native_stop();
    for (int i = 0; i < 32; i++) CHECK(g_claimed[i] == 0, "%s: IF%d nicht freigegeben (%d)", name, i, g_claimed[i]);
}

// Tonverk mit vorangestellter MIDI-Funktion (UAC1-AudioControl + MIDIStreaming), wie bei
// vielen Elektron-Geräten: die UAC2-Audio-Interfaces dürfen nicht nach UAC1-Regeln
// gelesen werden, Clock-Requests müssen an das AC-Interface der Audio-Funktion gehen.
static void test_midi_first(void) {
    printf("MIDI-Funktion vor der UAC2-Audio-Funktion\n");
    cfg_begin();
    iface(0,0, 0, 0x01, 0x00); u1_header(1, 1, 0);                     // AC (UAC1) für MIDI
    B(9,0x04, 1,0, 2, 0x01,0x03,0x00, 0);                              // MIDIStreaming
    B(7,0x24, 0x01, 0x00,0x01, 0x41,0x00);
    ep(0x02, 0x02, 512, 0); ep(0x81, 0x02, 512, 0);
    iface(3,0, 0, 0x01, 0x20); u2_header(); u2_clock(7);
    u2_it(1, 0x0101, 7, 2); u2_ot(2, 0x0301, 1, 7);
    u2_it(3, 0x0201, 7, 4); u2_ot(4, 0x0101, 3, 7);
    iface(4,0, 0, 0x02, 0x20);
    iface(4,1, 1, 0x02, 0x20); u2_asg(1, 2); u2_fmt(3, 24); ep(0x03, 0x05, 294, 4); cs_ep();
    iface(5,0, 0, 0x02, 0x20);
    iface(5,1, 1, 0x02, 0x20); u2_asg(4, 4); u2_fmt(3, 24); ep(0x84, 0x05, 588, 4); cs_ep();
    cfg_end();
    g_nranges = 1; g_ranges[0][0] = g_ranges[0][1] = 48000;
    g_stub_speed = LIBUSB_SPEED_HIGH;
    int r = do_start(0, 48000);
    CHECK(r == 0, "do_start=%d (erwartet 0)", r);
    CHECK(g_dev.uac == 2 && g_dev.ac_if == 3, "UAC%d, AC IF%d (erwartet UAC2, IF3)", g_dev.uac, g_dev.ac_if);
    CHECK(g_dev.out.valid && g_dev.out.ifnum == 4 && g_dev.in.valid && g_dev.in.channels == 4, "Streams nicht erkannt");
    CHECK(last_set_for(0x0703) == 48000, "SET_CUR muss an Clock 7 / IF3 gehen");
    if (r == 0) CHECK(capture_start() == 0, "capture_start");
    native_stop();
}

int main(int argc, char **argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; i++) test_fixture(argv[i]);
        if (failures) { printf("FAIL (%d)\n", failures); return 1; }
        printf("PASS\n");
        return 0;
    }
    test_tonverk();
    test_midi_first();
    test_fb_shift();
    test_uac1_dongle();
    test_uac1_async_441();
    test_uac2_hs_fb();
    test_uac2_multich();
    test_channel_mapping();
    test_capture_only();
    test_no_audio();
    if (failures) { printf("FAIL (%d)\n", failures); return 1; }
    printf("PASS\n");
    return 0;
}
