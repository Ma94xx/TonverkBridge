// Host-Test: do_start darf nur Endpoints ansprechen, die im Config-Deskriptor
// tatsächlich zu IF4/Alt1 gehören.
//
// Hintergrund (Tonverk-OS-Update, Nov 2026): EP 0x84 ist nicht mehr der Feedback-EP
// von IF4, sondern der ISO-IN-Daten-EP von IF5. Ein blindes Submit auf 0x84 lässt
// usbfs IF5 implizit für unser Handle claimen; das spätere explizite Claim in
// capture_start (DISCONNECT_CLAIM, "außer usbfs") scheitert dann mit
// LIBUSB_ERROR_BUSY -> "Recording failed (driver error -4)".

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_audio.c"

static uint8_t g_desc[512];
static size_t g_desc_len = 0;
static unsigned char g_submitted[256];   // Zähler pro EP-Adresse

int libusb_set_option(libusb_context *ctx, int option, ...) { (void)ctx; (void)option; return 0; }
int libusb_init(libusb_context **ctx) { *ctx = (libusb_context *) 1; return 0; }
void libusb_exit(libusb_context *ctx) { (void)ctx; }
int libusb_wrap_sys_device(libusb_context *ctx, intptr_t fd, libusb_device_handle **h)
{ (void)ctx; (void)fd; *h = (libusb_device_handle *) 1; return 0; }
int libusb_set_auto_detach_kernel_driver(libusb_device_handle *h, int e) { (void)h; (void)e; return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_control_transfer(libusb_device_handle *h, uint8_t t, uint8_t r, uint16_t v,
                            uint16_t i, unsigned char *d, uint16_t l, unsigned int to) {
    (void)h; (void)i; (void)to;
    if (t == 0x80 && r == 0x06 && (v >> 8) == 0x02) {       // GET_DESCRIPTOR(CONFIG)
        size_t n = g_desc_len < l ? g_desc_len : l;
        memcpy(d, g_desc, n);
        return (int) n;
    }
    return 0;
}
int libusb_set_interface_alt_setting(libusb_device_handle *h, int i, int a) { (void)h;(void)i;(void)a; return 0; }
struct libusb_transfer *libusb_alloc_transfer(int n) { (void)n; return calloc(1, sizeof(struct libusb_transfer)); }
void libusb_free_transfer(struct libusb_transfer *t) { free(t); }
int libusb_submit_transfer(struct libusb_transfer *t) { g_submitted[t->endpoint]++; return 0; }
int libusb_cancel_transfer(struct libusb_transfer *t) {
    // Event-Loop simulieren: abgebrochene Transfers melden sich ab.
    if (g_active > 0) g_active--;
    (void)t; return 0;
}
int libusb_handle_events_timeout_completed(libusb_context *c, struct timeval *tv, int *x)
{ (void)c;(void)tv;(void)x; usleep(1000); return 0; }
void libusb_close(libusb_device_handle *h) { (void)h; }
const char *libusb_error_name(int code) { (void)code; return "stub"; }

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

#define PUT(...) do { uint8_t b_[] = { __VA_ARGS__ }; memcpy(d + i, b_, sizeof b_); i += sizeof b_; } while (0)

// explicit_fb=1: altes Layout (IF4: OUT 0x03 + Feedback 0x84, IF5: IN 0x83)
// explicit_fb=0: neues Layout (IF4: nur OUT 0x03, IF5: IN 0x84, 4ch, mps 588)
static void build_desc(int explicit_fb) {
    uint8_t *d = g_desc; size_t i = 9;
    PUT(9,0x04, 3,0, 0, 0x01,0x01,0x20, 0);                         // IF3 AudioControl
    PUT(8,0x24, 0x0A, 7, 0x01, 0x01, 0, 0);                         // Clock-Source 7
    PUT(9,0x04, 4,0, 0, 0x01,0x02,0x20, 0);                         // IF4 alt0
    PUT(9,0x04, 4,1, (uint8_t)(explicit_fb ? 2 : 1), 0x01,0x02,0x20, 0);
    PUT(16,0x24, 0x01, 1, 0, 0x01, 0x01,0,0,0, 2, 0,0,0,0, 0);
    PUT(6,0x24, 0x02, 0x01, 3, 24);
    PUT(7,0x05, 0x03, 0x05, 0x26,0x01, 1);                          // OUT 0x03 async
    PUT(8,0x25, 0x01, 0, 0, 0, 0,0);
    if (explicit_fb) PUT(7,0x05, 0x84, 0x11, 0x04,0x00, 1);        // Feedback 0x84
    PUT(9,0x04, 5,0, 0, 0x01,0x02,0x20, 0);                         // IF5 alt0
    PUT(9,0x04, 5,1, 1, 0x01,0x02,0x20, 0);                         // IF5 alt1
    PUT(16,0x24, 0x01, 2, 0, 0x01, 0x01,0,0,0, 4, 0,0,0,0, 0);
    PUT(6,0x24, 0x02, 0x01, 3, 24);
    if (explicit_fb) PUT(7,0x05, 0x83, 0x05, 0x4C,0x02, 1);        // IN 0x83
    else             PUT(7,0x05, 0x84, 0x05, 0x4C,0x02, 1);        // IN 0x84
    PUT(8,0x25, 0x01, 0, 0, 0, 0,0);
    uint8_t cfg[] = { 9,0x02, (uint8_t)(i & 0xff),(uint8_t)(i >> 8), 6, 1, 0, 0x80, 50 };
    memcpy(d, cfg, 9);
    g_desc_len = i;
}

static void run(int explicit_fb) {
    build_desc(explicit_fb);
    memset(g_submitted, 0, sizeof g_submitted);
    int r = do_start(0, 48000);
    CHECK(r == 0, "do_start=%d", r);
    CHECK(g_submitted[0x03] == NUM_TRANSFERS, "OUT 0x03: %d Submits", g_submitted[0x03]);
    if (explicit_fb)
        CHECK(g_submitted[0x84] == NUM_FB, "altes Layout: Feedback 0x84 nicht submitted (%d)", g_submitted[0x84]);
    else
        CHECK(g_submitted[0x84] == 0,
              "neues Layout: %d Submits auf 0x84 (gehört zu IF5 -> implizites Claim -> BUSY)",
              g_submitted[0x84]);
    native_stop();
}

int main(void) {
    run(1);
    run(0);
    if (failures) { printf("FAIL (%d)\n", failures); return 1; }
    printf("PASS\n");
    return 0;
}
