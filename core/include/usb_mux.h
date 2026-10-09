/* In-process Apple usbmux peer. MIT licensed, copyright 2026 j0shua-SYSON.
 * Host-only state: never serialize it with guest hardware. All calls belong
 * to the emulator thread. Service clients communicate through bounded streams;
 * this layer has no sockets, filesystem, plist, TLS or installation policy. */
#ifndef S5LBOX_USB_MUX_H
#define S5LBOX_USB_MUX_H
#include "usb_host.h"

#define USB_MUX_CHANNELS 4u
#define USB_MUX_RX_SIZE 65536u
#define USB_MUX_TX_SIZE 16384u
typedef enum {
    USB_MUX_UNUSED, USB_MUX_CONNECT, USB_MUX_SYN_SENT, USB_MUX_OPEN,
    USB_MUX_CLOSING, USB_MUX_CLOSED, USB_MUX_ERROR
} usb_mux_state_t;
typedef struct {
    usb_mux_state_t state;
    uint16_t local_port, remote_port;
    uint32_t send_next, send_acked, receive_next, peer_window;
    unsigned receive_used, send_used;
    bool ack_pending;
    const char *error;
    uint8_t receive[USB_MUX_RX_SIZE], send[USB_MUX_TX_SIZE];
} usb_mux_channel_t;
typedef struct {
    unsigned version, stage, round_robin, receive_used, transmit_size, transmit_at;
    uint16_t tx_sequence, rx_sequence, next_port;
    bool transmit_zlp;
    const char *error;
    uint8_t receive[USB_MUX_RX_SIZE], transmit[USB_MUX_TX_SIZE];
    usb_mux_channel_t channel[USB_MUX_CHANNELS];
} usb_mux_t;

void usb_mux_init(usb_mux_t *u);
bool usb_mux_ready(const usb_mux_t *u);
/* Index, or -1 if not ready / all four channels are occupied. */
int usb_mux_connect(usb_mux_t *u, uint16_t port);
size_t usb_mux_write(usb_mux_t *u, unsigned channel, const void *data, size_t size);
size_t usb_mux_read(usb_mux_t *u, unsigned channel, void *data, size_t capacity);
void usb_mux_close(usb_mux_t *u, unsigned channel);
void usb_mux_cancel(usb_mux_t *u, const char *reason);
/* Wire entry points are also used by deterministic peer tests. */
void usb_mux_feed(usb_mux_t *u, const void *data, size_t size);
const uint8_t *usb_mux_output(usb_mux_t *u, size_t *size);
void usb_mux_output_complete(usb_mux_t *u);
/* One bounded burst, between guest CPU batches. Caller owns timeouts/cancel. */
void usb_mux_poll(usb_mux_t *u, s5l8900_t *m, const usb_host_t *host);
#endif
