// Minimaler libusb-Stub für Host-Tests von usb_audio.c.
// libusb_get_iso_packet_buffer_simple ist 1:1 die echte Implementierung
// (Offset = desc[0].length * Paketindex), damit der Test das reale Verhalten trifft.
#ifndef HOSTTEST_LIBUSB_H
#define HOSTTEST_LIBUSB_H
#include <stdint.h>
#include <stddef.h>
#include <limits.h>

#define LIBUSB_CALL
#define LIBUSB_OPTION_NO_DEVICE_DISCOVERY 2

typedef struct libusb_context libusb_context;
typedef struct libusb_device_handle libusb_device_handle;

enum libusb_transfer_status {
    LIBUSB_TRANSFER_COMPLETED = 0,
    LIBUSB_TRANSFER_ERROR,
    LIBUSB_TRANSFER_TIMED_OUT,
    LIBUSB_TRANSFER_CANCELLED,
    LIBUSB_TRANSFER_STALL,
    LIBUSB_TRANSFER_NO_DEVICE,
    LIBUSB_TRANSFER_OVERFLOW
};

struct libusb_iso_packet_descriptor {
    unsigned int length;
    unsigned int actual_length;
    enum libusb_transfer_status status;
};

struct libusb_transfer;
typedef void (LIBUSB_CALL *libusb_transfer_cb_fn)(struct libusb_transfer *transfer);

struct libusb_transfer {
    libusb_device_handle *dev_handle;
    uint8_t flags;
    unsigned char endpoint;
    unsigned char type;
    unsigned int timeout;
    enum libusb_transfer_status status;
    int length;
    int actual_length;
    libusb_transfer_cb_fn callback;
    void *user_data;
    unsigned char *buffer;
    int num_iso_packets;
    struct libusb_iso_packet_descriptor iso_packet_desc[32];
};

int libusb_set_option(libusb_context *ctx, int option, ...);
int libusb_init(libusb_context **ctx);
void libusb_exit(libusb_context *ctx);
int libusb_wrap_sys_device(libusb_context *ctx, intptr_t fd, libusb_device_handle **h);
int libusb_set_auto_detach_kernel_driver(libusb_device_handle *h, int enable);
int libusb_claim_interface(libusb_device_handle *h, int ifnum);
int libusb_release_interface(libusb_device_handle *h, int ifnum);
int libusb_control_transfer(libusb_device_handle *h, uint8_t t, uint8_t r, uint16_t v,
                            uint16_t i, unsigned char *d, uint16_t l, unsigned int to);
int libusb_set_interface_alt_setting(libusb_device_handle *h, int ifnum, int alt);
struct libusb_transfer *libusb_alloc_transfer(int iso_packets);
void libusb_free_transfer(struct libusb_transfer *t);
int libusb_submit_transfer(struct libusb_transfer *t);
int libusb_cancel_transfer(struct libusb_transfer *t);
int libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *c);
void libusb_close(libusb_device_handle *h);
const char *libusb_error_name(int code);

static inline void libusb_fill_iso_transfer(struct libusb_transfer *transfer,
    libusb_device_handle *dev_handle, unsigned char endpoint, unsigned char *buffer,
    int length, int num_iso_packets, libusb_transfer_cb_fn callback, void *user_data,
    unsigned int timeout)
{
    transfer->dev_handle = dev_handle;
    transfer->endpoint = endpoint;
    transfer->buffer = buffer;
    transfer->length = length;
    transfer->num_iso_packets = num_iso_packets;
    transfer->callback = callback;
    transfer->user_data = user_data;
    transfer->timeout = timeout;
}

static inline void libusb_set_iso_packet_lengths(struct libusb_transfer *transfer, unsigned int length)
{
    for (int i = 0; i < transfer->num_iso_packets; i++)
        transfer->iso_packet_desc[i].length = length;
}

static inline unsigned char *libusb_get_iso_packet_buffer_simple(
    struct libusb_transfer *transfer, unsigned int packet)
{
    int _packet;
    if (packet > INT_MAX) return (unsigned char *) 0;
    _packet = (int) packet;
    if (_packet >= transfer->num_iso_packets) return (unsigned char *) 0;
    return transfer->buffer + ((int) transfer->iso_packet_desc[0].length * _packet);
}

#endif
