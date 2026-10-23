// Host-Test für den Capture-Pfad (Tonverk -> Handy):
//   1. find_capture_params: findet im rohen Config-Deskriptor das AudioStreaming-
//      IN-Interface (Daten-EP, NICHT den Feedback-EP des OUT-Pfads) samt
//      Kanalzahl/Subslot/Auflösung.
//   2. Capture-Ring: push/read inkl. Wrap-around und Overflow (älteste Frames weg).
//   3. cap_xfer_cb: erntet ISO-IN-Pakete mit variablen actual_lengths aus dem
//      Transferpuffer (simple-Layout, alle Paketslots gleich groß).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_audio.c"

// ---- libusb-Stubs (nur für den Linker) ----
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

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

// ---- Synthetischer Config-Deskriptor im Tonverk-Layout ----
// IF3=AudioControl, IF4=AS-OUT (EP 0x03 Daten + EP 0x84 Feedback),
// IF5=AS-IN (EP 0x83 Daten, 2ch/24bit/Subslot 3).
static size_t build_desc(uint8_t *d, int with_in_iface) {
    size_t i = 9;   // Platz für den Config-Header, wird am Ende befüllt

    // IF3 alt0: AudioControl
    uint8_t if3[] = { 9,0x04, 3,0, 0, 0x01,0x01,0x20, 0 };
    memcpy(d+i, if3, 9); i += 9;
    // Clock-Source (CS_INTERFACE, wird vom Parser ignoriert)
    uint8_t cs_clk[] = { 8,0x24, 0x0A, 7, 0x01, 0x01, 0, 0 };
    memcpy(d+i, cs_clk, 8); i += 8;

    // IF4 alt0: AS-OUT, keine EPs
    uint8_t if4a0[] = { 9,0x04, 4,0, 0, 0x01,0x02,0x20, 0 };
    memcpy(d+i, if4a0, 9); i += 9;
    // IF4 alt1: AS-OUT mit Daten-EP 0x03 + Feedback-EP 0x84 (IN! darf nicht gewählt werden)
    uint8_t if4a1[] = { 9,0x04, 4,1, 2, 0x01,0x02,0x20, 0 };
    memcpy(d+i, if4a1, 9); i += 9;
    uint8_t asg_out[] = { 16,0x24, 0x01, 1, 0, 0x01, 0x01,0,0,0, 2, 0,0,0,0, 0 };
    memcpy(d+i, asg_out, 16); i += 16;
    uint8_t fmt_out[] = { 6,0x24, 0x02, 0x01, 3, 24 };
    memcpy(d+i, fmt_out, 6); i += 6;
    uint8_t ep_out[] = { 7,0x05, 0x03, 0x05, 0x26,0x01, 1 };          // ISO async Daten, mps 294
    memcpy(d+i, ep_out, 7); i += 7;
    uint8_t csep_out[] = { 8,0x25, 0x01, 0, 0, 0, 0,0 };
    memcpy(d+i, csep_out, 8); i += 8;
    uint8_t ep_fb[] = { 7,0x05, 0x84, 0x11, 0x04,0x00, 1 };           // ISO Feedback (usage=1)
    memcpy(d+i, ep_fb, 7); i += 7;

    if (with_in_iface) {
        // IF5 alt0: AS-IN, keine EPs
        uint8_t if5a0[] = { 9,0x04, 5,0, 0, 0x01,0x02,0x20, 0 };
        memcpy(d+i, if5a0, 9); i += 9;
        // IF5 alt1: AS-IN mit Daten-EP 0x83
        uint8_t if5a1[] = { 9,0x04, 5,1, 1, 0x01,0x02,0x20, 0 };
        memcpy(d+i, if5a1, 9); i += 9;
        uint8_t asg_in[] = { 16,0x24, 0x01, 2, 0, 0x01, 0x01,0,0,0, 2, 0,0,0,0, 0 };
        memcpy(d+i, asg_in, 16); i += 16;
        uint8_t fmt_in[] = { 6,0x24, 0x02, 0x01, 3, 24 };
        memcpy(d+i, fmt_in, 6); i += 6;
        uint8_t ep_in[] = { 7,0x05, 0x83, 0x05, 0x26,0x01, 1 };       // ISO async Daten IN, mps 294
        memcpy(d+i, ep_in, 7); i += 7;
        uint8_t csep_in[] = { 8,0x25, 0x01, 0, 0, 0, 0,0 };
        memcpy(d+i, csep_in, 8); i += 8;
    }

    // Config-Header
    uint8_t cfg[] = { 9,0x02, (uint8_t)(i & 0xff),(uint8_t)(i >> 8), 6, 1, 0, 0x80, 50 };
    memcpy(d, cfg, 9);
    return i;
}

static void test_parser(void) {
    uint8_t d[512];
    size_t len = build_desc(d, 1);
    cap_params cp;
    int r = find_capture_params(d, len, &cp);
    CHECK(r == 0, "Parser: nichts gefunden (r=%d)", r);
    if (r == 0) {
        CHECK(cp.ifnum == 5,    "ifnum=%d, erwartet 5", cp.ifnum);
        CHECK(cp.alt == 1,      "alt=%d, erwartet 1", cp.alt);
        CHECK(cp.ep == 0x83,    "ep=0x%02X, erwartet 0x83 (nicht der Feedback-EP!)", cp.ep);
        CHECK(cp.max_pkt == 294,"max_pkt=%d, erwartet 294", cp.max_pkt);
        CHECK(cp.channels == 2, "channels=%d, erwartet 2", cp.channels);
        CHECK(cp.subslot == 3,  "subslot=%d, erwartet 3", cp.subslot);
        CHECK(cp.bits == 24,    "bits=%d, erwartet 24", cp.bits);
    }

    // Ohne IN-Interface: muss sauber scheitern, darf nicht EP 0x84 nehmen.
    len = build_desc(d, 0);
    r = find_capture_params(d, len, &cp);
    CHECK(r != 0, "Parser: hat ohne IN-Interface etwas 'gefunden' (ifnum=%d ep=0x%02X)",
          cp.ifnum, cp.ep);
}

static void test_ring(void) {
    // 960 Bytes Kapazität, Framegröße 6 (2ch * 24 Bit)
    CHECK(cap_ring_init(960, 6) == 0, "cap_ring_init fehlgeschlagen");

    // Einfacher Roundtrip
    uint8_t in[100], out[200];
    for (int i = 0; i < 100; i++) in[i] = (uint8_t) i;
    cap_push(in, 96);                       // 16 Frames
    int n = cap_read(out, sizeof out);
    CHECK(n == 96, "read=%d, erwartet 96", n);
    CHECK(memcmp(in, out, 96) == 0, "Roundtrip-Daten kaputt");
    CHECK(cap_read(out, sizeof out) == 0, "Ring sollte leer sein");

    // Wrap-around + Overflow: deutlich mehr pushen als reinpasst.
    // Byte-Muster: fortlaufender Zähler mod 251 (prim, damit kein Alias mit 960).
    long total = 0;
    uint8_t chunk[114];                     // 19 Frames pro Push
    for (int k = 0; k < 40; k++) {
        for (int j = 0; j < 114; j++) chunk[j] = (uint8_t)((total + j) % 251);
        cap_push(chunk, 114);
        total += 114;
    }
    CHECK(g_cap_drops > 0, "Overflow nicht gezählt (drops=%ld)", g_cap_drops);

    // Alles auslesen: muss ein zusammenhängendes Endstück des Streams sein,
    // frame-aligned (Vielfaches von 6) und mit dem letzten gepushten Byte enden.
    uint8_t big[2048];
    int got = 0, r2;
    while ((r2 = cap_read(big + got, 96)) > 0 && got < 1500) got += r2;
    CHECK(got > 0 && got % 6 == 0, "gelesen=%d, nicht frame-aligned", got);
    long start = total - got;
    CHECK(start % 6 == 0, "Startoffset %ld nicht frame-aligned (Drops nicht frameweise?)", start);
    int bad = 0;
    for (int j = 0; j < got; j++)
        if (big[j] != (uint8_t)((start + j) % 251)) bad++;
    CHECK(bad == 0, "%d Bytes falsch nach Overflow/Wrap (Kontinuität verletzt)", bad);

    cap_ring_free();
}

static void test_harvest(void) {
    CHECK(cap_ring_init(4096 * 6, 6) == 0, "cap_ring_init fehlgeschlagen");
    g_cap_running = 1;

    struct libusb_transfer *x = libusb_alloc_transfer(4);
    uint8_t *buf = calloc(1, 4 * 294);
    libusb_fill_iso_transfer(x, NULL, 0x83, buf, 4 * 294, 4, cap_xfer_cb, NULL, 1000);
    libusb_set_iso_packet_lengths(x, 294);

    // Pakete mit variablen actual_lengths an den Slot-Offsets (simple-Layout)
    unsigned actual[4] = { 288, 294, 0, 6 };
    long pos = 0;
    for (int p = 0; p < 4; p++) {
        x->iso_packet_desc[p].actual_length = actual[p];
        x->iso_packet_desc[p].status = LIBUSB_TRANSFER_COMPLETED;
        for (unsigned b = 0; b < actual[p]; b++)
            buf[p * 294 + b] = (uint8_t)((pos + b) % 251);
        pos += actual[p];
    }
    x->status = LIBUSB_TRANSFER_COMPLETED;
    cap_xfer_cb(x);

    uint8_t out[2048];
    int got = 0, r;
    while ((r = cap_read(out + got, 200)) > 0) got += r;
    CHECK(got == 588, "geerntet=%d Bytes, erwartet 588 (288+294+0+6)", got);
    int bad = 0;
    for (int j = 0; j < got; j++)
        if (out[j] != (uint8_t)(j % 251)) bad++;
    CHECK(bad == 0, "%d Bytes falsch – Paket-Offsets (simple-Layout) nicht korrekt gelesen", bad);

    g_cap_running = 0;
    free(buf);
    libusb_free_transfer(x);
    cap_ring_free();
}

// Härtung 1: korrupte/unausgerichtete Paketlängen dürfen die Kanalzuordnung
// nicht verschieben -> cap_xfer_cb stutzt auf ganze Frames.
static void test_harvest_unaligned(void) {
    CHECK(cap_ring_init(4096 * 6, 6) == 0, "cap_ring_init fehlgeschlagen");
    g_cap_running = 1;

    struct libusb_transfer *x = libusb_alloc_transfer(2);
    uint8_t *buf = calloc(1, 2 * 294);
    libusb_fill_iso_transfer(x, NULL, 0x83, buf, 2 * 294, 2, cap_xfer_cb, NULL, 1000);
    libusb_set_iso_packet_lengths(x, 294);
    unsigned actual[2] = { 291, 294 };       // 291 = kein Frame-Vielfaches
    for (int p = 0; p < 2; p++) {
        x->iso_packet_desc[p].actual_length = actual[p];
        x->iso_packet_desc[p].status = LIBUSB_TRANSFER_COMPLETED;
        for (unsigned b = 0; b < actual[p]; b++) buf[p * 294 + b] = (uint8_t)(p + 1);
    }
    x->status = LIBUSB_TRANSFER_COMPLETED;
    cap_xfer_cb(x);

    uint8_t out[1024];
    int got = 0, r;
    while ((r = cap_read(out + got, 200)) > 0) got += r;
    CHECK(got == 288 + 294, "geerntet=%d, erwartet 582 (291 auf 288 gestutzt)", got);
    CHECK(got % 6 == 0, "nicht frame-aligned nach Kurz-Paket");

    g_cap_running = 0;
    free(buf);
    libusb_free_transfer(x);
    cap_ring_free();
}

// Härtung 2: Overflow-Drop darf bei unausgerichtetem Füllstand nicht
// unterlaufen (size_t-Wrap) oder hängen.
static void test_push_overflow_unaligned(void) {
    CHECK(cap_ring_init(60, 6) == 0, "cap_ring_init fehlgeschlagen");
    uint8_t junk[64];
    memset(junk, 0xAA, sizeof junk);
    cap_push(junk, 54);     // fast voll
    cap_push(junk, 58);     // unausgerichtet, erzwingt Drops
    cap_push(junk, 60);     // Restbestand < 1 Frame -> Ring wird geleert
    CHECK(g_cap_drops > 0, "keine Drops gezählt");
    uint8_t out[128];
    int n = cap_read(out, sizeof out);
    CHECK(n >= 0 && n <= 60 && n % 6 == 0, "read=%d nach Overflow kaputt", n);
    cap_ring_free();
}

// Peak-Tracking für das Live-Meter: cap_push verfolgt den Betrags-Peak
// (16-Bit-Auflösung aus den 2 MSBs jedes Samples), cap_take_peak liest und nullt.
static void test_peak(void) {
    CHECK(cap_ring_init(4096 * 6, 6) == 0, "cap_ring_init fehlgeschlagen");
    g_cp.subslot = 3;   // 24-Bit-Subslot, MSB an Offset 2 jedes Samples

    // Zwei Frames: L=+0x123456 R=-0x200000 (abs MSBs 0x1234 bzw. 0x2000), dann leiser.
    uint8_t f[12] = { 0x56,0x34,0x12,  0x00,0x00,0xE0,   // +0x123456, -0x200000
                      0x01,0x00,0x00,  0x02,0x00,0x00 };
    cap_push(f, 12);
    int p = cap_take_peak();
    CHECK(p == 0x2000, "peak=0x%04X, erwartet 0x2000 (|-0x200000| >> 8)", p);
    CHECK(cap_take_peak() == 0, "Peak nicht genullt nach dem Lesen");

    // Flush: Ring leeren (Monitor-Altbestand darf nicht in die Aufnahme rutschen).
    cap_push(f, 12);
    cap_flush();
    uint8_t out[64];
    CHECK(cap_read(out, sizeof out) == 0, "Ring nach cap_flush nicht leer");
    cap_ring_free();
}

int main(void) {
    test_parser();
    test_ring();
    test_harvest();
    test_harvest_unaligned();
    test_push_overflow_unaligned();
    test_peak();
    if (failures) { printf("FAIL (%d)\n", failures); return 1; }
    printf("PASS\n");
    return 0;
}
