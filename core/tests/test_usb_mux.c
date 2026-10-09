#include "usb_mux.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %u: %s\n", __LINE__, #x); } } while (0)
static usb_mux_t u;
static uint8_t packet[USB_MUX_RX_SIZE];
static void p16(uint8_t *p, unsigned n) { p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n; }
static void p32(uint8_t *p, uint32_t n) { p16(p, n >> 16); p16(p + 2, n); }
static uint32_t g32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void negotiate(unsigned version) {
    usb_mux_init(&u);
    size_t n; const uint8_t *p = usb_mux_output(&u, &n);
    CHECK(p && n == 20 && !g32(p) && g32(p + 4) == 20 && g32(p + 8) == 2);
    usb_mux_output_complete(&u);
    memset(packet, 0, 20); p32(packet + 4, 20); p32(packet + 8, version);
    for (unsigned i = 0; i < 20; i++) usb_mux_feed(&u, packet + i, 1);
    if (version == 2) {
        CHECK(!usb_mux_ready(&u));
        p = usb_mux_output(&u, &n);
        CHECK(p && n == 17 && g32(p) == 2 && g32(p + 8) == 0xfeedfaceu &&
              p[14] == 255 && p[15] == 255 && p[16] == 7);
        usb_mux_output_complete(&u);
    }
    CHECK(usb_mux_ready(&u) && u.version == version);
}
static void tcp(unsigned ch, uint32_t seq, uint32_t ack, unsigned flags,
                unsigned window, const void *data, unsigned size) {
    unsigned h = u.version == 2 ? 16 : 8;
    memset(packet, 0, h + 20);
    p32(packet, 6); p32(packet + 4, h + 20 + size);
    /* Legacy guest leaves the receive-direction reserved signature zero. */
    uint8_t *p = packet + h;
    p16(p, u.channel[ch].remote_port); p16(p + 2, u.channel[ch].local_port);
    p32(p + 4, seq); p32(p + 8, ack); p[12] = 0x50; p[13] = (uint8_t)flags;
    p16(p + 14, window);
    if (size) memcpy(p + 20, data, size);
    /* Arbitrary fragmentation, including a split inside the header. */
    usb_mux_feed(&u, packet, 3);
    usb_mux_feed(&u, packet + 3, h + 17 + size);
}
static int connect_service(uint16_t port) {
    int ch = usb_mux_connect(&u, port);
    CHECK(ch >= 0);
    if (ch < 0) return 0;
    size_t n; const uint8_t *p = usb_mux_output(&u, &n);
    unsigned h = u.version == 2 ? 16 : 8;
    CHECK(p && n == h + 20 && p[h + 13] == 2 && g32(p + h + 4) == 0);
    usb_mux_output_complete(&u);
    tcp((unsigned)ch, 0xfffffffcu, 1, 18, 256, NULL, 0);
    CHECK(u.channel[ch].state == USB_MUX_OPEN && u.channel[ch].receive_next == 0xfffffffdu);
    p = usb_mux_output(&u, &n);
    CHECK(p && n == h + 20 && p[h + 13] == 16 && g32(p + h + 8) == 0xfffffffdu);
    usb_mux_output_complete(&u);
    return ch;
}
static void streams(unsigned version) {
    negotiate(version);
    int ch = connect_service(62078);
    unsigned h = version == 2 ? 16 : 8;
    CHECK(usb_mux_write(&u, (unsigned)ch, "hello", 5) == 5);
    size_t n; const uint8_t *p = usb_mux_output(&u, &n);
    CHECK(p && n == h + 25 && !memcmp(p + h + 20, "hello", 5) && g32(p + h + 4) == 1);
    usb_mux_output_complete(&u);
    tcp((unsigned)ch, 0xfffffffdu, 6, 16, 0, "world", 5);
    CHECK(u.channel[ch].receive_used == 5 && u.channel[ch].receive_next == 2);
    CHECK(usb_mux_write(&u, (unsigned)ch, "blocked", 7) == 7);
    p = usb_mux_output(&u, &n); /* ACK only: zero peer window */
    CHECK(p && n == h + 20 && g32(p + h + 8) == 2);
    usb_mux_output_complete(&u);
    CHECK(!usb_mux_output(&u, &n));
    char out[16] = {0};
    CHECK(usb_mux_read(&u, (unsigned)ch, out, 3) == 3 && !memcmp(out, "wor", 3));
    CHECK(usb_mux_read(&u, (unsigned)ch, out, sizeof out) == 2 && !memcmp(out, "ld", 2));
    tcp((unsigned)ch, 2, 6, 16, 256, NULL, 0);
    p = usb_mux_output(&u, &n);
    CHECK(p && n == h + 27 && !memcmp(p + h + 20, "blocked", 7));
    usb_mux_output_complete(&u);
    tcp((unsigned)ch, 2, 13, 16, 256, "abc", 3);
    tcp((unsigned)ch, 2, 13, 16, 256, "abc", 3); /* duplicate not redelivered */
    CHECK(u.channel[ch].receive_used == 3 && u.channel[ch].receive_next == 5);
    usb_mux_close(&u, (unsigned)ch);
    p = usb_mux_output(&u, &n);
    CHECK(p && p[h + 13] == 4 && u.channel[ch].state == USB_MUX_CLOSED);
    usb_mux_output_complete(&u);
    int next = connect_service(1234);
    CHECK(u.channel[next].local_port == 2);
    tcp((unsigned)next, u.channel[next].receive_next, 2, 16, 256, NULL, 0);
    CHECK(u.channel[next].state == USB_MUX_ERROR && u.channel[next].error);
    CHECK(usb_mux_write(&u, (unsigned)next, "x", 1) == 0);
    usb_mux_cancel(&u, "test disconnect");
    CHECK(!usb_mux_ready(&u) && usb_mux_connect(&u, 1) < 0 && !usb_mux_output(&u, &n));
}
static void bounds(void) {
    negotiate(1);
    size_t n;
    for (unsigned i = 0; i < USB_MUX_CHANNELS; i++) (void)connect_service((uint16_t)(62078 + i));
    CHECK(usb_mux_connect(&u, 1) < 0 && usb_mux_connect(&u, 0) < 0);
    memset(packet, 0, sizeof packet);
    CHECK(usb_mux_write(&u, 0, packet, sizeof packet) == USB_MUX_TX_SIZE);
    CHECK(usb_mux_write(&u, 0, packet, 1) == 0);
    const uint8_t *p = usb_mux_output(&u, &n);
    CHECK(p && n == USB_MUX_TX_SIZE && u.transmit_zlp);
    usb_mux_output_complete(&u);
    p32(packet, 6); p32(packet + 4, USB_MUX_RX_SIZE + 1);
    usb_mux_feed(&u, packet, 8);
    CHECK(u.error && !usb_mux_ready(&u));
    negotiate(2);
    memset(packet, 0, 16); p32(packet, 6); p32(packet + 4, 16);
    usb_mux_feed(&u, packet, 16);
    CHECK(u.error); /* TCP frame is too short even with a valid mux header */
    negotiate(1);
    memset(packet, 0, 8); p32(packet + 4, 7);
    usb_mux_feed(&u, packet, 8);
    CHECK(u.error); /* too short, detected without another input byte */
    negotiate(2);
    memset(packet, 0, 18); p32(packet, 1); p32(packet + 4, 18); packet[16] = 7;
    usb_mux_feed(&u, packet, 18);
    CHECK(usb_mux_ready(&u) && !u.error);
    packet[16] = 5; usb_mux_feed(&u, packet, 18);
    CHECK(usb_mux_ready(&u) && !u.error);
    packet[16] = 3; usb_mux_feed(&u, packet, 18);
    CHECK(!usb_mux_ready(&u) && u.error);
}
static s5l8900_t machine;
static void arm(unsigned in, unsigned ep, unsigned dma, unsigned size) {
    unsigned base = in ? USBOTG_DIEP(ep) : USBOTG_DOEP(ep);
    s5l_usbotg_write(&machine.usbotg, base + 0x14, machine.ram_base + dma);
    s5l_usbotg_write(&machine.usbotg, base + 0x10, size | (((size + 511u) / 512u) << 19));
    s5l_usbotg_write(&machine.usbotg, base,
        USBOTG_EP_ACTIVE | USBOTG_EP_ENABLE | USBOTG_EP_CNAK | (2u << 18) | 512u);
}
static void dma_transport(void) {
    CHECK(s5l8900_init(&machine, 0x08000000, 1u << 20));
    s5l_usbotg_enable(&machine.usbotg);
    s5l_usbotg_connect(&machine.usbotg, true);
    s5l_usbotg_write(&machine.usbotg, USBOTG_GAHBCFG, 0x21);
    /* Enumeration itself is covered by test_usbotg's independent gadget. */
    usb_host_t host = {0}; host.stage = 8; host.bulk_in = 2; host.bulk_out = 1;
    usb_mux_init(&u);
    arm(0, 1, 4096, 512);
    usb_mux_poll(&u, &machine, &host);
    CHECK(g32(machine.ram + 4100) == 20 && g32(machine.ram + 4104) == 2);
    CHECK(machine.usbotg.ep[0][1].interrupt & 1u);
    memset(packet, 0, 20); p32(packet + 4, 20); p32(packet + 8, 1);
    s5l8900_load(&machine, machine.ram_base + 8192, packet, 20);
    arm(1, 2, 8192, 20);
    usb_mux_poll(&u, &machine, &host);
    CHECK(usb_mux_ready(&u) && u.version == 1);
    int ch = connect_service(62078);
    memset(packet, 0xab, sizeof packet);
    CHECK(usb_mux_write(&u, (unsigned)ch, packet, USB_MUX_TX_SIZE - 28) == USB_MUX_TX_SIZE - 28);
    arm(0, 1, 16384, USB_MUX_TX_SIZE + 512);
    usb_mux_poll(&u, &machine, &host);
    CHECK(u.transmit_at == USB_MUX_TX_SIZE && u.transmit_zlp);
    CHECK(machine.usbotg.ep[0][1].ctl & USBOTG_EP_ENABLE);
    usb_mux_poll(&u, &machine, &host); /* required zero-length terminator */
    CHECK(!u.transmit_size && !u.transmit_zlp);
    CHECK(!(machine.usbotg.ep[0][1].ctl & USBOTG_EP_ENABLE));
    CHECK((machine.usbotg.ep[0][1].size & 0x7ffffu) == 512);
    CHECK(!memcmp(machine.ram + 16384 + 28, packet, USB_MUX_TX_SIZE - 28));
    s5l_usbotg_connect(&machine.usbotg, false);
    usb_mux_poll(&u, &machine, &host);
    CHECK(u.error && u.channel[ch].state == USB_MUX_ERROR);
    s5l8900_free(&machine);
}
int main(void) {
    streams(1); streams(2); bounds(); dma_transport();
    printf("usb_mux: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
