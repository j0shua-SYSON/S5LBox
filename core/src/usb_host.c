/* USB enumeration for Apple's vendor-specific usbmux interface.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * Protocol reference: USB 2.0 ch. 9; libimobiledevice/usbmuxd usb.c.
 * This code does not synthesize descriptors or accept a simulated service. */
#include "usb_host.h"
#include <string.h>

enum { WAIT_DEVICE, WAIT_RESET, WAIT_ENUM, DEVICE, ADDRESS,
       CONFIG_HEAD, CONFIG_BODY, CONFIG_SET, READY, FAILED };
enum { CONTROL_IDLE, CONTROL_SETUP, CONTROL_DATA, CONTROL_STATUS };
static unsigned le16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }

void usb_host_init(usb_host_t *h, s5l8900_t *m) {
    memset(h, 0, sizeof *h);
    s5l_usbotg_enable(&m->usbotg);
    s5l_usbotg_connect(&m->usbotg, true);
    s5l_pcf50635_set_usb(&m->pmu, true);
    m->level_dirty = true;
}
void usb_host_disconnect(usb_host_t *h, s5l8900_t *m) {
    s5l_usbotg_connect(&m->usbotg, false);
    s5l_pcf50635_set_usb(&m->pmu, false);
    m->level_dirty = true;
    memset(h, 0, sizeof *h);
}
bool usb_host_ready(const usb_host_t *h) { return h->stage == READY; }
static void fail(usb_host_t *h, const char *message) {
    h->error = message; h->stage = FAILED; h->control = CONTROL_IDLE;
}
static void request(usb_host_t *h, uint8_t type, uint8_t req,
                     unsigned value, unsigned length) {
    h->setup[0] = type; h->setup[1] = req;
    h->setup[2] = (uint8_t)value; h->setup[3] = (uint8_t)(value >> 8);
    h->setup[4] = h->setup[5] = 0;
    h->setup[6] = (uint8_t)length; h->setup[7] = (uint8_t)(length >> 8);
    h->wanted = length; h->used = 0; h->control = CONTROL_SETUP;
}
static bool control_poll(usb_host_t *h, s5l8900_t *m) {
    uint8_t packet[64]; size_t n = 0;
    s5l_usb_result_t r;
    switch (h->control) {
        case CONTROL_SETUP:
            r = s5l8900_usb_packet(m, S5L_USB_SETUP, 0, h->setup, 8, &n);
            if (r == S5L_USB_ACK) {
                h->control = h->wanted ? CONTROL_DATA : CONTROL_STATUS;
                return false;
            }
            break;
        case CONTROL_DATA:
            r = s5l8900_usb_packet(m, S5L_USB_IN, 0, packet, sizeof packet, &n);
            if (r == S5L_USB_ACK) {
                if (n > h->wanted - h->used || n > sizeof h->descriptor - h->used) {
                    fail(h, "USB descriptor exceeds request"); return false;
                }
                memcpy(h->descriptor + h->used, packet, n);
                h->used += (unsigned)n;
                if (n < 64 || h->used == h->wanted) h->control = CONTROL_STATUS;
                return false;
            }
            break;
        case CONTROL_STATUS:
            r = s5l8900_usb_packet(m, (h->setup[0] & 0x80) ? S5L_USB_OUT : S5L_USB_IN,
                                    0, packet, 0, &n);
            if (r == S5L_USB_ACK) { h->control = CONTROL_IDLE; return true; }
            break;
        default: return false;
    }
    if (r != S5L_USB_NAK) fail(h, r == S5L_USB_STALL ?
                              "USB control request stalled" : "USB control transport failed");
    return false;
}

static bool parse_config(usb_host_t *h) {
    bool mux = false;
    h->bulk_in = h->bulk_out = 0;
    for (unsigned at = 0; at < h->used;) {
        if (h->used - at < 2u) return false;
        const uint8_t *p = h->descriptor + at;
        unsigned len = p[0];
        if (len < 2u || len > h->used - at) return false;
        if (p[1] == 4) {
            if (len < 9u) return false;
            if (mux && h->bulk_in && h->bulk_out) return true;
            mux = p[5] == 0xff && p[6] == 0xfe && p[7] == 2 && p[3] == 0;
            h->bulk_in = h->bulk_out = 0;
        } else if (p[1] == 5 && mux) {
            if (len < 7u) return false;
            if ((p[3] & 3u) == 2u && (p[2] & 15u) &&
                (p[2] & 15u) < S5L_USB_ENDPOINTS && le16(p + 4) == 512u) {
                if (p[2] & 0x80) h->bulk_in = p[2] & 15u;
                else h->bulk_out = p[2] & 15u;
            }
        }
        at += len;
    }
    return mux && h->bulk_in && h->bulk_out;
}

void usb_host_poll(usb_host_t *h, s5l8900_t *m) {
    if (h->stage == FAILED || h->stage == READY) return;
    if (h->control && !control_poll(h, m)) return;
    switch (h->stage) {
        case WAIT_DEVICE:
            if (s5l_usbotg_bus_reset(&m->usbotg)) {
                m->level_dirty = true; h->stage = WAIT_RESET;
            }
            return;
        case WAIT_RESET:
            if (s5l_usbotg_enumerated(&m->usbotg)) {
                m->level_dirty = true; h->stage = WAIT_ENUM;
            }
            return;
        case WAIT_ENUM:
            if (m->usbotg.gintsts & USBOTG_INT_ENUM) return;
            request(h, 0x80, 6, 0x100, 18); h->stage = DEVICE; return;
        case DEVICE:
            if (h->used != 18 || h->descriptor[0] != 18 ||
                h->descriptor[1] != 1 || h->descriptor[7] != 64 ||
                !h->descriptor[17] || h->descriptor[17] > 16) {
                fail(h, "Unsupported USB device descriptor"); return;
            }
            h->vendor = (uint16_t)le16(h->descriptor + 8);
            h->product = (uint16_t)le16(h->descriptor + 10);
            h->config_count = h->descriptor[17];
            request(h, 0, 5, 1, 0); h->stage = ADDRESS; return;
        case ADDRESS:
            request(h, 0x80, 6, 0x200 | h->config_index, 9);
            h->stage = CONFIG_HEAD; return;
        case CONFIG_HEAD: {
            if (h->used < 9 || h->descriptor[1] != 2) {
                fail(h, "Invalid USB configuration header"); return;
            }
            unsigned total = le16(h->descriptor + 2);
            if (total < 9 || total > sizeof h->descriptor) {
                fail(h, "USB configuration too large"); return;
            }
            h->configuration = h->descriptor[5];
            request(h, 0x80, 6, 0x200 | h->config_index, total);
            h->stage = CONFIG_BODY; return;
        }
        case CONFIG_BODY:
            if (h->used != h->wanted) { fail(h, "Truncated USB configuration"); return; }
            if (!parse_config(h)) {
                if (++h->config_index < h->config_count) { h->stage = ADDRESS; return; }
                fail(h, "Guest has no supported usbmux interface"); return;
            }
            request(h, 0, 9, h->configuration, 0); h->stage = CONFIG_SET; return;
        case CONFIG_SET: h->stage = READY; return;
        default: return;
    }
}
