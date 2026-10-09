/* Deterministic virtual-host tests, no firmware, disk or network. */
#include "soc.h"
#include "snapshot.h"
#include "usb_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL %u: %s\n", __LINE__, #x); } } while (0)
static s5l8900_t m;
static void wr(unsigned off, uint32_t value) {
    m.bus.write32(m.bus.ctx, S5L8900_USB_OTG_BASE + off, value);
}
static uint32_t rd(unsigned off) {
    return m.bus.read32(m.bus.ctx, S5L8900_USB_OTG_BASE + off);
}
static void arm_ep(unsigned d, unsigned n, uint32_t dma, unsigned bytes,
                   unsigned packets) {
    unsigned base = d ? USBOTG_DIEP(n) : USBOTG_DOEP(n);
    wr(base + 0x14u, dma);
    wr(base + 0x10u, bytes | (packets << 19));
    wr(base, USBOTG_EP_ACTIVE | USBOTG_EP_ENABLE | USBOTG_EP_CNAK |
             (n ? (2u << 18) | 512u : 0u));
}
/* A tiny independent USB gadget responds through DMA/interrupts, not through
 * calls into the host enumerator. It is protocol coverage, not firmware proof. */
static void test_enumeration(bool malformed) {
    static const uint8_t device[] = {
        18, 1, 0, 2, 0, 0, 0, 64, 0xac, 5, 0x90, 0x12, 0, 1, 0, 0, 0, 1
    };
    static const uint8_t config[] = {
        9, 2, 32, 0, 1, 1, 0, 0x80, 250,
        9, 4, 0, 0, 2, 0xff, 0xfe, 2, 0,
        7, 5, 3, 2, 0, 2, 0,
        7, 5, 0x82, 2, 0, 2, 0
    };
    CHECK(s5l8900_init(&m, 0x08000000, 1u << 20));
    usb_host_t host;
    usb_host_init(&host, &m);
    wr(USBOTG_GAHBCFG, 0x21);
    wr(USBOTG_DCTL, 0);
    bool status_in = false;
    unsigned requests = 0;
    for (unsigned i = 0; i < 300 && !usb_host_ready(&host) && !host.error; i++) {
        usb_host_poll(&host, &m);
        if (rd(USBOTG_GINTSTS) & USBOTG_INT_RESET)
            wr(USBOTG_GINTSTS, USBOTG_INT_RESET);
        if (rd(USBOTG_GINTSTS) & USBOTG_INT_ENUM) {
            wr(USBOTG_GINTSTS, USBOTG_INT_ENUM);
            arm_ep(0, 0, m.ram_base + 4096, 24, 1);
            wr(USBOTG_DOEP(0) + 16, (3u << 29) | (1u << 19) | 24u);
        }
        if (rd(USBOTG_DOEP(0) + 8) & 8u) {
            requests++;
            const uint8_t *s = m.ram + 4096;
            unsigned length = s[6] | (unsigned)s[7] << 8;
            status_in = !(s[0] & 0x80);
            wr(USBOTG_DOEP(0) + 8, UINT32_MAX);
            if (!status_in) {
                const uint8_t *src = s[3] == 1 ? device : config;
                unsigned size = s[3] == 1 ? sizeof device : sizeof config;
                if (length > size) length = size;
                s5l8900_load(&m, m.ram_base + 8192, src, length);
                if (malformed) m.ram[8192] = 0;
                arm_ep(1, 0, m.ram_base + 8192, length, 1);
                arm_ep(0, 0, m.ram_base + 4096, 64, 1);
            } else {
                CHECK(s[1] == 5 || s[1] == 9);
                arm_ep(1, 0, m.ram_base + 8192, 0, 1);
            }
        }
        bool done = (status_in ? rd(USBOTG_DIEP(0) + 8) :
                                   rd(USBOTG_DOEP(0) + 8)) & 1u;
        wr(USBOTG_DIEP(0) + 8, 1u);
        if (done) {
            wr(USBOTG_DOEP(0) + 8, 1u);
            arm_ep(0, 0, m.ram_base + 4096, 24, 1);
            wr(USBOTG_DOEP(0) + 16, (3u << 29) | (1u << 19) | 24u);
        }
    }
    if (malformed) CHECK(host.error && !usb_host_ready(&host));
    else {
        CHECK(usb_host_ready(&host) && !host.error);
        CHECK(requests == 5);
        CHECK(host.vendor == 0x05ac && host.product == 0x1290);
        CHECK(host.configuration == 1 && host.bulk_in == 2 && host.bulk_out == 3);
    }
    usb_host_disconnect(&host, &m);
    CHECK(!m.usbotg.connected && !(m.pmu.regs[PCF50635_MBCS1] & 3));
    s5l8900_free(&m);
}

int main(void) {
    CHECK(s5l8900_init(&m, 0x08000000u, 1u << 20));
    if (!m.ram) return 1;
    s5l_usbotg_enable(&m.usbotg);
    CHECK(!s5l_usbotg_bus_reset(&m.usbotg));
    s5l_usbotg_connect(&m.usbotg, true);
    CHECK(rd(USBOTG_GOTGCTL) & (1u << 19));
    CHECK(!s5l_usbotg_bus_reset(&m.usbotg)); /* soft disconnected */
    wr(USBOTG_DCTL, 0u);
    wr(USBOTG_GAHBCFG, 0x21u);
    wr(USBOTG_GINTMSK, USBOTG_INT_RESET | USBOTG_INT_ENUM |
                       USBOTG_INT_IN | USBOTG_INT_OUT);
    CHECK(s5l_usbotg_bus_reset(&m.usbotg));
    CHECK(s5l_usbotg_irq(&m.usbotg));
    CHECK(!s5l_usbotg_enumerated(&m.usbotg));
    wr(USBOTG_GINTSTS, USBOTG_INT_RESET);
    CHECK(s5l_usbotg_enumerated(&m.usbotg));
    wr(USBOTG_GINTSTS, UINT32_MAX);
    CHECK(!s5l_usbotg_irq(&m.usbotg));
    wr(USBOTG_DIEPMSK, 7u);
    wr(USBOTG_DOEPMSK, 15u);
    wr(USBOTG_DAINTMSK, UINT32_MAX);
    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE,
                   1u << S5L8900_IRQ_USB_OTG);

    uint8_t data[1024], out[1024]; size_t actual = 99;
    memset(data, 0xa7, sizeof data);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_OUT, 1u, data, 512, &actual) == S5L_USB_NAK);
    CHECK(actual == 0);
    arm_ep(0, 1, m.ram_base + 4096, 1024, 2);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_OUT, 1u, data, 512, &actual) == S5L_USB_ACK);
    CHECK(actual == 512 && !memcmp(m.ram + 4096, data, 512));
    CHECK(!(rd(USBOTG_DOEP(1) + 8) & 1u));
    CHECK((rd(USBOTG_DOEP(1) + 16) & 0x7ffffu) == 512);
    /* Short OUT terminates, and only endpoint W1C can clear the level. */
    CHECK(s5l8900_usb_packet(&m, S5L_USB_OUT, 1u, data, 13, &actual) == S5L_USB_ACK);
    CHECK(actual == 13 && (rd(USBOTG_DOEP(1) + 8) & 1u));
    CHECK(m.cpu.irq_line);
    wr(USBOTG_GINTSTS, USBOTG_INT_OUT);
    CHECK(s5l_usbotg_irq(&m.usbotg));
    wr(USBOTG_DOEP(1) + 8, 1u);
    s5l8900_tick(&m, 0);
    CHECK(!m.cpu.irq_line);

    s5l8900_load(&m, m.ram_base + 8192, data, sizeof data);
    arm_ep(1, 2, m.ram_base + 8192, 525, 2);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_IN, 2u, out, 511, &actual) == S5L_USB_INVALID);
    CHECK(rd(USBOTG_DIEP(2) + 0x14) == m.ram_base + 8192);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_IN, 2u, out, sizeof out, &actual) == S5L_USB_ACK);
    CHECK(actual == 512 && !memcmp(data, out, 512));
    CHECK(s5l8900_usb_packet(&m, S5L_USB_IN, 2u, out, sizeof out, &actual) == S5L_USB_ACK);
    CHECK(actual == 13 && (rd(USBOTG_DIEP(2) + 8) & 1u));
    wr(USBOTG_DIEP(2) + 8, 1u);
    arm_ep(1, 2, 0xffffffffu, 0, 1);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_IN, 2u, NULL, 0, &actual) == S5L_USB_ACK);
    CHECK(actual == 0); /* ZLP needs no DMA memory */

    arm_ep(0, 1, m.ram_base + m.ram_size - 2, 512, 1);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_OUT, 1u, data, 8, &actual) == S5L_USB_DMA_ERROR);
    CHECK(actual == 0 && (rd(USBOTG_DOEP(1) + 8) & 4u));
    CHECK(m.ram[m.ram_size - 1] == 0); /* no partial memory write */
    arm_ep(0, 1, S5L8900_USB_OTG_BASE, 512, 1);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_OUT, 1u, data, 8, &actual) == S5L_USB_DMA_ERROR);

    arm_ep(0, 0, m.ram_base + 12288, 24, 1);
    wr(USBOTG_DOEP(0) + 16, (3u << 29) | (1u << 19) | 24u);
    wr(USBOTG_DOEP(0), rd(USBOTG_DOEP(0)) | USBOTG_EP_STALL | USBOTG_EP_SNAK);
    uint8_t setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
    CHECK(s5l8900_usb_packet(&m, S5L_USB_SETUP, 0u, setup, 8, &actual) == S5L_USB_ACK);
    CHECK(actual == 8 && !memcmp(m.ram + 12288, setup, 8));
    CHECK(rd(USBOTG_DOEP(0) + 8) & 8u);
    CHECK(!(rd(USBOTG_DOEP(0)) & USBOTG_EP_STALL));
    wr(USBOTG_DOEP(0) + 8, 8u);
    arm_ep(1, 0, m.ram_base + 16384, 18, 1);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_IN, 0, out, sizeof out, &actual) == S5L_USB_ACK);
    CHECK(actual == 18);
    arm_ep(0, 0, m.ram_base + 12288, 64, 1);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_OUT, 0, NULL, 0, &actual) == S5L_USB_ACK);

    /* A snapshot includes pending IRQs, DMA cursors and packet counts. */
    uint8_t *save = NULL; size_t length = 0;
    arm_ep(0, 3, m.ram_base + 20000, 1024, 2);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_OUT, 3, data, 512, &actual) == S5L_USB_ACK);
    s5l_usbotg_t saved = m.usbotg;
    CHECK(snapshot_save_mem(&m, &save, &length) == SNAP_OK);
    s5l_usbotg_reset(&m.usbotg);
    CHECK(snapshot_load_mem(&m, save, length) == SNAP_OK);
    CHECK(!memcmp(&saved, &m.usbotg, sizeof saved));
    free(save);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_OUT, 3, data, 512, &actual) == S5L_USB_ACK);
    CHECK(!memcmp(m.ram + 20000, data, 1024));
    s5l_usbotg_connect(&m.usbotg, false);
    CHECK(s5l8900_usb_packet(&m, S5L_USB_OUT, 3, data, 1, &actual) == S5L_USB_DISCONNECTED);
    CHECK(!(rd(USBOTG_GOTGCTL) & (1u << 19)));
    s5l8900_free(&m);
    test_enumeration(false);
    test_enumeration(true);
    printf("USB: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
