/* Independent implementation of the usbmux wire protocol. MIT licensed.
 * References: libimobiledevice/usbmuxd's documented wire structures, TCP
 * sequence arithmetic. No IP routing: streams end in the running guest. */
#include "usb_mux.h"
#include <string.h>

enum { SEND_VERSION, WAIT_VERSION, SEND_SETUP, WAIT_SETUP, ACTIVE, FAILED };
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static unsigned header_size(const usb_mux_t *u) { return u->version == 2 ? 16u : 8u; }
void usb_mux_init(usb_mux_t *u) { memset(u, 0, sizeof *u); u->next_port = 1; }
bool usb_mux_ready(const usb_mux_t *u) { return u && u->stage == ACTIVE; }
void usb_mux_cancel(usb_mux_t *u, const char *reason) {
    u->stage = FAILED; u->error = reason;
    u->transmit_size = u->transmit_at = u->receive_used = 0;
    u->transmit_zlp = false;
    for (unsigned i = 0; i < USB_MUX_CHANNELS; i++) {
        usb_mux_channel_t *c = &u->channel[i];
        if (c->state != USB_MUX_UNUSED && c->state != USB_MUX_CLOSED) {
            c->state = USB_MUX_ERROR; c->error = reason;
        }
    }
}
int usb_mux_connect(usb_mux_t *u, uint16_t port) {
    if (!usb_mux_ready(u) || !port || !u->next_port) return -1;
    for (unsigned i = 0; i < USB_MUX_CHANNELS; i++) {
        usb_mux_channel_t *c = &u->channel[i];
        if (c->state != USB_MUX_UNUSED && c->state != USB_MUX_CLOSED) continue;
        memset(c, 0, sizeof *c);
        c->local_port = u->next_port++; c->remote_port = port;
        c->state = USB_MUX_CONNECT;
        return (int)i;
    }
    return -1;
}
size_t usb_mux_write(usb_mux_t *u, unsigned channel, const void *data, size_t size) {
    if (!usb_mux_ready(u) || channel >= USB_MUX_CHANNELS || (!data && size)) return 0;
    usb_mux_channel_t *c = &u->channel[channel];
    if (c->state != USB_MUX_OPEN) return 0;
    if (size > sizeof c->send - c->send_used) size = sizeof c->send - c->send_used;
    if (size) memcpy(c->send + c->send_used, data, size);
    c->send_used += (unsigned)size;
    return size;
}
size_t usb_mux_read(usb_mux_t *u, unsigned channel, void *data, size_t capacity) {
    if (channel >= USB_MUX_CHANNELS || (!data && capacity)) return 0;
    usb_mux_channel_t *c = &u->channel[channel];
    if (capacity > c->receive_used) capacity = c->receive_used;
    if (capacity) {
        memcpy(data, c->receive, capacity);
        c->receive_used -= (unsigned)capacity;
        memmove(c->receive, c->receive + capacity, c->receive_used);
        if (c->state == USB_MUX_OPEN) c->ack_pending = true; /* window update */
    }
    return capacity;
}
void usb_mux_close(usb_mux_t *u, unsigned channel) {
    if (channel >= USB_MUX_CHANNELS) return;
    usb_mux_channel_t *c = &u->channel[channel];
    if (c->state == USB_MUX_UNUSED || c->state == USB_MUX_CLOSED) return;
    c->send_used = c->receive_used = 0;
    c->state = usb_mux_ready(u) ? USB_MUX_CLOSING : USB_MUX_CLOSED;
}
static uint8_t *frame(usb_mux_t *u, unsigned protocol, unsigned payload) {
    unsigned h = header_size(u);
    memset(u->transmit, 0, h);
    u->transmit_size = h + payload; u->transmit_at = 0;
    u->transmit_zlp = (u->transmit_size % 512u) == 0;
    put32(u->transmit, protocol); put32(u->transmit + 4, u->transmit_size);
    if (h == 16) {
        put32(u->transmit + 8, 0xfeedfaceu);
        put16(u->transmit + 12, u->tx_sequence++);
        put16(u->transmit + 14, u->rx_sequence);
    }
    return u->transmit + h;
}
static void tcp_frame(usb_mux_t *u, usb_mux_channel_t *c, unsigned flags, unsigned n) {
    uint8_t *p = frame(u, 6, 20u + n);
    memset(p, 0, 20);
    put16(p, c->local_port); put16(p + 2, c->remote_port);
    put32(p + 4, c->send_next); put32(p + 8, c->receive_next);
    p[12] = 5u << 4; p[13] = (uint8_t)flags;
    /* usbmux uses a fixed 8-bit window scale, without TCP options. */
    put16(p + 14, (sizeof c->receive - c->receive_used) >> 8);
    if (n) {
        memcpy(p + 20, c->send, n);
        c->send_used -= n; memmove(c->send, c->send + n, c->send_used);
    }
    c->send_next += n + ((flags & 2u) ? 1u : 0u);
    c->ack_pending = false;
}
const uint8_t *usb_mux_output(usb_mux_t *u, size_t *size) {
    *size = 0;
    if (u->stage == FAILED) return NULL;
    if (!u->transmit_size) {
        if (u->stage == SEND_VERSION) {
            uint8_t *p = frame(u, 0, 12);
            put32(p, 2); put32(p + 4, 0); put32(p + 8, 0);
            u->stage = WAIT_VERSION;
        } else if (u->stage == SEND_SETUP) {
            u->tx_sequence = 0; u->rx_sequence = UINT16_MAX;
            *frame(u, 2, 1) = 7;
            u->stage = WAIT_SETUP;
        } else if (u->stage == ACTIVE) {
            for (unsigned k = 0; k < USB_MUX_CHANNELS; k++) {
                unsigned i = (u->round_robin + k) % USB_MUX_CHANNELS;
                usb_mux_channel_t *c = &u->channel[i];
                if (c->state == USB_MUX_CONNECT) {
                    tcp_frame(u, c, 2, 0); c->state = USB_MUX_SYN_SENT;
                } else if (c->state == USB_MUX_CLOSING) {
                    tcp_frame(u, c, 4, 0); c->state = USB_MUX_CLOSED;
                } else if (c->state == USB_MUX_OPEN) {
                    uint32_t flight = c->send_next - c->send_acked;
                    uint32_t available = flight < c->peer_window ? c->peer_window - flight : 0;
                    unsigned n = c->send_used;
                    unsigned max_payload = sizeof u->transmit - header_size(u) - 20u;
                    if (n > max_payload) n = max_payload;
                    if (n > available) n = available;
                    if (n || c->ack_pending) tcp_frame(u, c, 16, n);
                }
                if (u->transmit_size) { u->round_robin = (i + 1u) % USB_MUX_CHANNELS; break; }
            }
        }
    }
    *size = u->transmit_size;
    return *size ? u->transmit : NULL;
}
void usb_mux_output_complete(usb_mux_t *u) {
    u->transmit_size = u->transmit_at = 0; u->transmit_zlp = false;
    if (u->stage == WAIT_SETUP) u->stage = ACTIVE;
}
static void channel_error(usb_mux_channel_t *c, const char *message) {
    c->error = message; c->state = USB_MUX_ERROR; c->send_used = 0;
}
static void input_frame(usb_mux_t *u, const uint8_t *p, unsigned n) {
    unsigned h = header_size(u), protocol = be32(p);
    if (n < h) { usb_mux_cancel(u, "Truncated usbmux header"); return; }
    if (h == 16) {
        /* The signature is host-to-device only. Real 7E18 replies put zero
         * here (observed SYN-ACK: 00000006 00000024 00000000 00000001).
         * Validate length/protocol, not an invented guest magic requirement. */
        u->rx_sequence = (uint16_t)be16(p + 14);
    }
    p += h; n -= h;
    if (u->stage == WAIT_VERSION) {
        if (protocol || n != 12 || (be32(p) != 1 && be32(p) != 2)) {
            usb_mux_cancel(u, "Unsupported guest usbmux version"); return;
        }
        u->version = be32(p);
        u->stage = u->version == 2 ? SEND_SETUP : ACTIVE;
        return;
    }
    if (u->stage != ACTIVE) { usb_mux_cancel(u, "Unexpected usbmux negotiation packet"); return; }
    if (protocol == 1) {
        /* The setup verbosity byte enables these diagnostic messages. They
         * are not stream payloads; warnings/info do not reset connections. */
        if (n && (p[0] == 5 || p[0] == 7)) return;
        usb_mux_cancel(u, "Guest reported a usbmux control error"); return;
    }
    if (protocol != 6 || n < 20 || (p[12] >> 4) != 5) {
        usb_mux_cancel(u, "Invalid usbmux stream header"); return;
    }
    usb_mux_channel_t *c = NULL;
    for (unsigned i = 0; i < USB_MUX_CHANNELS; i++)
        if (u->channel[i].local_port == be16(p + 2) &&
            u->channel[i].remote_port == be16(p)) { c = &u->channel[i]; break; }
    /* Late replies to a canceled channel cannot contaminate a new stream. */
    if (!c || c->state == USB_MUX_CLOSED || c->state == USB_MUX_CLOSING ||
        c->state == USB_MUX_ERROR) return;
    unsigned flags = p[13];
    uint32_t sequence = be32(p + 4), ack = be32(p + 8);
    if (flags & 4) { channel_error(c, "Guest refused or reset the service connection"); return; }
    if (c->state == USB_MUX_SYN_SENT) {
        if (flags != 18 || n != 20 || ack != c->send_next) {
            channel_error(c, "Invalid service connection handshake"); return;
        }
        c->send_acked = ack; c->receive_next = sequence + 1u;
        c->peer_window = be16(p + 14) << 8;
        c->state = USB_MUX_OPEN; c->ack_pending = true;
        return;
    }
    if (c->state != USB_MUX_OPEN || (flags != 16 && flags != 24)) {
        channel_error(c, "Unexpected service connection flags"); return;
    }
    /* Every outstanding byte is bounded by our queue/window. Unsigned
     * subtraction handles sequence wrap without implementation-defined casts. */
    uint32_t advanced = ack - c->send_acked;
    if (advanced > c->send_next - c->send_acked) {
        channel_error(c, "Guest acknowledged unsent stream bytes"); return;
    }
    c->send_acked = ack; c->peer_window = be16(p + 14) << 8;
    n -= 20; p += 20;
    if (sequence != c->receive_next) {
        uint32_t behind = c->receive_next - sequence;
        if (behind >= n && behind <= sizeof c->receive) { c->ack_pending = true; return; }
        channel_error(c, "Out-of-order usbmux stream bytes"); return;
    }
    if (n > sizeof c->receive - c->receive_used) {
        channel_error(c, "Guest exceeded advertised receive window"); return;
    }
    if (n) {
        memcpy(c->receive + c->receive_used, p, n);
        c->receive_used += n; c->receive_next += n; c->ack_pending = true;
    }
}
void usb_mux_feed(usb_mux_t *u, const void *data, size_t size) {
    if (u->stage == FAILED || (!data && size)) return;
    const uint8_t *p = data;
    while (size && u->stage != FAILED) {
        unsigned wanted = u->receive_used < 8 ? 8 : be32(u->receive + 4);
        if (wanted < 8 || wanted > sizeof u->receive || wanted < u->receive_used) {
            usb_mux_cancel(u, "Invalid usbmux packet length"); return;
        }
        size_t n = wanted - u->receive_used;
        if (n > size) n = size;
        if (n) memcpy(u->receive + u->receive_used, p, n);
        u->receive_used += (unsigned)n; p += n; size -= n;
        if (u->receive_used >= 8 && (be32(u->receive + 4) < header_size(u) ||
            be32(u->receive + 4) > sizeof u->receive)) {
            usb_mux_cancel(u, "Invalid usbmux packet length"); return;
        }
        if (u->receive_used >= 8 && u->receive_used == be32(u->receive + 4)) {
            input_frame(u, u->receive, u->receive_used); u->receive_used = 0;
        }
    }
}
void usb_mux_poll(usb_mux_t *u, s5l8900_t *m, const usb_host_t *host) {
    if (u->stage == FAILED) return;
    if (!usb_host_ready(host) || !m->usbotg.enabled || !m->usbotg.connected) {
        usb_mux_cancel(u, "Virtual USB session disconnected"); return;
    }
    /* Keep guest execution responsive even when both bulk endpoints are busy. */
    for (unsigned i = 0; i < 32; i++) {
        uint8_t packet[512]; size_t actual = 0;
        s5l_usb_result_t r = s5l8900_usb_packet(m, S5L_USB_IN, host->bulk_in,
                                               packet, sizeof packet, &actual);
        if (r == S5L_USB_NAK) break;
        if (r != S5L_USB_ACK) { usb_mux_cancel(u, "Virtual USB receive failed"); return; }
        usb_mux_feed(u, packet, actual);
        if (u->error) return;
        if (actual < sizeof packet) break;
    }
    size_t size;
    if (!usb_mux_output(u, &size)) return;
    for (unsigned i = 0; i < 32; i++) {
        size_t n = size - u->transmit_at, actual = 0;
        if (n > 512) n = 512;
        s5l_usb_result_t r = s5l8900_usb_packet(m, S5L_USB_OUT, host->bulk_out,
                              u->transmit + u->transmit_at, n, &actual);
        if (r == S5L_USB_NAK) return;
        if (r != S5L_USB_ACK || actual != n) {
            usb_mux_cancel(u, "Virtual USB send failed"); return;
        }
        u->transmit_at += (unsigned)n;
        if (!n) u->transmit_zlp = false;
        if (u->transmit_at == size && !u->transmit_zlp) {
            usb_mux_output_complete(u); return;
        }
    }
}
