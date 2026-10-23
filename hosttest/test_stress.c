// Langzeit-Stresstest für den Capture-Ring (lange Jams / stundenlange Aufnahmen).
// Prüft über viele tausend Ring-Wraps hinweg:
//   A) Byte-Erhaltung + lückenlose Kontinuität, wenn der Leser Schritt hält,
//   B) korrekte, frame-ausgerichtete Drop-Buchführung bei Leser-Stall (Überlauf),
//   C) Peak-Tracking über lange Läufe (kein Überlauf / sauberes Nullen).
//
// Modell: Tonverk-IN wie im echten Log (4 Kanäle, 24 Bit, Subslot 3) -> frame=12,
// ~48 Frames/ms. Ring = 2 s (wie capture_start). "Stunden" werden skaliert über
// die Paketzahl simuliert; der Ring wrappt alle ~2 s, also deckt der Lauf viele
// tausend Wraps ab.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_audio.c"

// ---- libusb-Stubs (nur Linker) ----
int libusb_set_option(libusb_context *c, int o, ...) { (void)c;(void)o; return 0; }
int libusb_init(libusb_context **c) { (void)c; return 0; }
void libusb_exit(libusb_context *c) { (void)c; }
int libusb_wrap_sys_device(libusb_context *c, intptr_t f, libusb_device_handle **h) { (void)c;(void)f;(void)h; return 0; }
int libusb_set_auto_detach_kernel_driver(libusb_device_handle *h, int e) { (void)h;(void)e; return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h;(void)i; return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h;(void)i; return 0; }
int libusb_control_transfer(libusb_device_handle *h, uint8_t t, uint8_t r, uint16_t v, uint16_t i, unsigned char *d, uint16_t l, unsigned int to){ (void)h;(void)t;(void)r;(void)v;(void)i;(void)d;(void)l;(void)to; return 0; }
int libusb_set_interface_alt_setting(libusb_device_handle *h, int i, int a) { (void)h;(void)i;(void)a; return 0; }
struct libusb_transfer *libusb_alloc_transfer(int n) { (void)n; return calloc(1,sizeof(struct libusb_transfer)); }
void libusb_free_transfer(struct libusb_transfer *t) { free(t); }
int libusb_submit_transfer(struct libusb_transfer *t) { (void)t; return 0; }
int libusb_cancel_transfer(struct libusb_transfer *t) { (void)t; return 0; }
int libusb_handle_events_timeout_completed(libusb_context *c, struct timeval *tv, int *x){ (void)c;(void)tv;(void)x; return 0; }
void libusb_close(libusb_device_handle *h) { (void)h; }
const char *libusb_error_name(int code) { (void)code; return "stub"; }

static int failures = 0;
#define CHECK(cond, ...) do { if(!(cond)){ printf("FAIL %s:%d: ",__FILE__,__LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while(0)

#define FRAME 12
#define FPP   48          // Frames pro ms-Paket
#define PKT   (FPP*FRAME) // 576 Bytes

// A) Leser hält Schritt: lückenlose Kontinuität über viele Wraps.
static void test_longrun_continuity(long minutes) {
    CHECK(cap_ring_init((size_t)48000*FRAME*2, FRAME) == 0, "init");
    g_cp.subslot = 3;
    long packets = minutes * 60 * 1000;      // 1 Paket/ms
    unsigned char pkt[PKT];
    unsigned char sink[PKT*4];
    unsigned long produced = 0, consumed = 0;
    for (long i = 0; i < packets; i++) {
        for (int b = 0; b < PKT; b++) pkt[b] = (unsigned char)((produced + b) % 251);
        cap_push(pkt, PKT);
        produced += PKT;
        // Leser leert alle 4 Pakete (simuliert ~4 ms Lese-Takt), hält locker Schritt.
        if (i % 4 == 3) {
            int n;
            while ((n = cap_read(sink, sizeof sink)) > 0) {
                for (int b = 0; b < n; b++)
                    if (sink[b] != (unsigned char)((consumed + b) % 251)) { failures++; goto done; }
                consumed += n;
            }
        }
    }
    { int n; while ((n = cap_read(sink, sizeof sink)) > 0) {
        for (int b = 0; b < n; b++)
            if (sink[b] != (unsigned char)((consumed + b) % 251)) { failures++; break; }
        consumed += n; } }
done:
    CHECK(g_cap_drops == 0, "unerwartete Drops bei mithaltendem Leser: %ld", g_cap_drops);
    CHECK(consumed == produced, "Byte-Verlust: produced=%lu consumed=%lu", produced, consumed);
    CHECK((long)g_cap_bytes == (long)produced, "g_cap_bytes=%ld != produced=%lu", g_cap_bytes, produced);
    printf("  continuity: %ld min simuliert, %lu MB, drops=%ld -> %s\n",
           minutes, produced/1000000, g_cap_drops, failures? "FAIL":"ok");
    cap_ring_free();
}

// B) Leser-Stall: Ring läuft über. Drops frame-ausgerichtet, Rest ein sauberer Tail.
static void test_reader_stall(void) {
    CHECK(cap_ring_init((size_t)48000*FRAME*2, FRAME) == 0, "init");
    g_cp.subslot = 3;
    unsigned char pkt[PKT];
    unsigned long produced = 0;
    for (long i = 0; i < 5000; i++) {        // 5 s ohne Lesen in einen 2-s-Ring
        for (int b = 0; b < PKT; b++) pkt[b] = (unsigned char)((produced + b) % 251);
        cap_push(pkt, PKT);
        produced += PKT;
    }
    CHECK(g_cap_drops > 0, "kein Drop trotz Überlauf");
    // Verbleibender Inhalt: frame-ausgerichteter, lückenloser Tail des Streams.
    unsigned char out[4096];
    unsigned long got = 0; int n;
    unsigned char first[4096]; int firstn = 0;
    while ((n = cap_read(out, sizeof out)) > 0) {
        if (firstn == 0) { firstn = n; memcpy(first, out, n); }
        got += n;
    }
    CHECK(got % FRAME == 0, "Tail nicht frame-ausgerichtet: %lu", got);
    CHECK(got <= (size_t)48000*FRAME*2, "mehr gelesen als Ringkapazität");
    // Der Tail muss an einer Frame-Grenze des Originalstroms beginnen.
    unsigned long start = produced - got;
    CHECK(start % FRAME == 0, "Tail-Start nicht frame-ausgerichtet: %lu", start);
    int bad = 0;
    for (int b = 0; b < firstn; b++)
        if (first[b] != (unsigned char)((start + b) % 251)) bad++;
    CHECK(bad == 0, "%d Byte-Fehler am Tail-Anfang (Kanalversatz nach Drops)", bad);
    printf("  reader-stall: %lu MB produziert, drops=%ld, sauberer Tail=%lu B -> %s\n",
           produced/1000000, g_cap_drops, got, bad? "FAIL":"ok");
    cap_ring_free();
}

// C) Peak über langen Lauf: kein int-Überlauf, sauberes Nullen.
static void test_peak_longrun(void) {
    CHECK(cap_ring_init((size_t)48000*FRAME*2, FRAME) == 0, "init");
    g_cp.subslot = 3;
    unsigned char pkt[PKT];
    for (long i = 0; i < 2000000; i++) {     // 2 Mio Pakete
        memset(pkt, 0, PKT);
        // Vollaussteuerung in einem Sample (MSB), Rest leise
        pkt[FRAME-1] = 0x7F; pkt[FRAME-2] = 0xFF;
        cap_push(pkt, PKT);
        if (i % 1000 == 0) {
            int p = cap_take_peak();
            CHECK(p >= 0 && p <= 0x8000, "Peak außerhalb Bereich: %d", p);
        }
        // Ring leeren, damit er nicht ewig überläuft
        unsigned char s[4096]; while (cap_read(s, sizeof s) > 0) {}
    }
    printf("  peak-longrun: 2M Pakete, Peak stabil -> %s\n", failures? "FAIL":"ok");
    cap_ring_free();
}

int main(void) {
    printf("Langzeit-Stresstest (Capture-Ring, 4ch/24bit):\n");
    test_longrun_continuity(30);   // 30 min lückenlos
    test_reader_stall();           // Überlauf-Buchführung
    test_peak_longrun();           // Peak-Stabilität
    if (failures) { printf("FAIL (%d)\n", failures); return 1; }
    printf("PASS\n");
    return 0;
}
