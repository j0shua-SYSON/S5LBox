/*
 * S5LBox - DWC2 device-mode internal DMA. MIT licensed.
 * Copyright (c) 2026 j0shua-SYSON.
 *
 * Registers follow Synopsys DWC2 (Linux dwc2/hw.h). Straps are a legal
 * sufficient configuration, NOT a measured S5L8900 dump. Bidirectional
 * GHWCFG1 and GHWCFG2=0x228de550 make findMaxEndpoints derive 5 IN/5 OUT;
 * zero straps formerly caused a stock-driver panic.
 *
 * enable() opts in; otherwise the original config-only aperture is unchanged.
 * No host mode, PIO or isochronous transfers. A virtual host supplies packets
 * between CPU batches. An enabled endpoint alone never completes a transfer.
 */
#include "soc.h"
#include <string.h>

#define EP_COMPLETE 1u
#define EP_DISABLED 2u
#define EP_AHB_ERROR 4u
#define EP_SETUP 8u
#define EP_IN_NAK (1u << 6)
#define SIZE_BYTES 0x7ffffu
#define SIZE_PACKETS (0x3ffu << 19)
#define SOFT_DISCONNECT 2u
#define GLOBAL_IN_NAK 4u
#define GLOBAL_OUT_NAK 8u

void s5l_usbotg_reset(s5l_usbotg_t *u) { memset(u, 0, sizeof *u); }

void s5l_usbotg_enable(s5l_usbotg_t *u) {
    if (!u || u->enabled) return;
    u->enabled = 1u;
    /* This older integration starts connected at the controller level;
     * Apple's 7E18 initialization resets/configures the core without clearing
     * SDIS. A modern STM32-style reset value of 2 leaves it disconnected
     * forever. This matches the older S5L8900 model, not a measured chip dump.
     * Cable presence and host readiness still gate every transfer. */
    u->dctl = 0;
    for (unsigned d = 0; d < 2u; d++)
        for (unsigned n = 0; n < S5L_USB_ENDPOINTS; n++)
            u->ep[d][n].ctl = USBOTG_EP_NAK;
}

static uint32_t endpoint_interrupts(const s5l_usbotg_t *u) {
    uint32_t bits = 0;
    for (unsigned n = 0; n < S5L_USB_ENDPOINTS; n++) {
        if (u->ep[1][n].interrupt & u->diepmsk) bits |= 1u << n;
        if (u->ep[0][n].interrupt & u->doepmsk) bits |= 1u << (16u + n);
    }
    return bits;
}

static uint32_t interrupts(const s5l_usbotg_t *u) {
    uint32_t bits = u->gintsts;
    uint32_t eps = endpoint_interrupts(u) & u->daintmsk;
    if (eps & 0xffffu) bits |= USBOTG_INT_IN;
    if (eps >> 16) bits |= USBOTG_INT_OUT;
    if (u->gotgint) bits |= 4u;
    if (u->dctl & GLOBAL_IN_NAK) bits |= 1u << 6;
    if (u->dctl & GLOBAL_OUT_NAK) bits |= 1u << 7;
    return bits;
}

bool s5l_usbotg_irq(const s5l_usbotg_t *u) {
    return u && u->enabled && (u->gahbcfg & 1u) &&
           (interrupts(u) & u->gintmsk) != 0u;
}

static s5l_usb_ep_t *endpoint(s5l_usbotg_t *u, uint32_t off, uint32_t *reg) {
    unsigned d;
    if (off >= 0x900u && off < 0x900u + S5L_USB_ENDPOINTS * 32u) {
        d = 1u; off -= 0x900u;
    } else if (off >= 0xb00u && off < 0xb00u + S5L_USB_ENDPOINTS * 32u) {
        d = 0u; off -= 0xb00u;
    } else return NULL;
    *reg = off & 31u;
    return &u->ep[d][off / 32u];
}

uint32_t s5l_usbotg_read(const s5l_usbotg_t *u, uint32_t off) {
    switch (off) {
        case USBOTG_GHWCFG1: return S5L_DWC2_GHWCFG1;
        case USBOTG_GHWCFG2: return S5L_DWC2_GHWCFG2;
        case USBOTG_GHWCFG4: return S5L_DWC2_GHWCFG4;
        case USBOTG_PCGCCTL: return u->pcgcctl;
        default: break;
    }
    if (!u->enabled || (off & 3u)) return 0;
    switch (off) {
        case USBOTG_GOTGCTL: return (1u << 16) | (u->connected ? 1u << 19 : 0u);
        case USBOTG_GOTGINT: return u->gotgint;
        case USBOTG_GAHBCFG: return u->gahbcfg;
        case USBOTG_GUSBCFG: return u->gusbcfg;
        case USBOTG_GRSTCTL: return 1u << 31; /* idle, synchronous reset/flush */
        case USBOTG_GINTSTS: return interrupts(u);
        case USBOTG_GINTMSK: return u->gintmsk;
        case USBOTG_GRXFSIZ: return u->grxfsiz;
        case USBOTG_GNPTXFSIZ: return u->gnptxfsiz;
        case 0x02cu: return 0x00080000u | (u->gnptxfsiz >> 16);
        case 0x040u: return 0x4f542800u; /* modeled DWC2 2.80a, not silicon ID */
        case 0x04cu: return 0x08000035u; /* 2048 FIFO words, 19/10 counters */
        case USBOTG_DCFG: return u->dcfg;
        case USBOTG_DCTL: return u->dctl;
        case USBOTG_DSTS: return 0; /* high speed */
        case USBOTG_DIEPMSK: return u->diepmsk;
        case USBOTG_DOEPMSK: return u->doepmsk;
        case USBOTG_DAINT: return endpoint_interrupts(u);
        case USBOTG_DAINTMSK: return u->daintmsk;
        default: break;
    }
    if (off >= 0x104u && off < 0x140u)
        return u->dptxfsiz[(off - 0x104u) / 4u];
    uint32_t reg = 0;
    /* The common decoder computes a pointer, never mutates state. */
    const s5l_usb_ep_t *e = endpoint((s5l_usbotg_t *)(uintptr_t)u, off, &reg);
    if (!e) return 0;
    switch (reg) {
        case 0: return e->ctl;
        case 8: return e->interrupt;
        case 0x10: return e->size;
        case 0x14: return e->dma;
        default: return 0;
    }
}

void s5l_usbotg_write(s5l_usbotg_t *u, uint32_t off, uint32_t val) {
    if (off == USBOTG_PCGCCTL) { u->pcgcctl = val; return; }
    if (!u->enabled || (off & 3u)) return;
    switch (off) {
        case USBOTG_GOTGINT: u->gotgint &= ~val; return;
        case USBOTG_GAHBCFG: u->gahbcfg = val; return;
        case USBOTG_GUSBCFG: u->gusbcfg = val; return;
        case USBOTG_GINTSTS: u->gintsts &= ~val; return;
        case USBOTG_GINTMSK: u->gintmsk = val; return;
        case USBOTG_GRXFSIZ: u->grxfsiz = val; return;
        case USBOTG_GNPTXFSIZ: u->gnptxfsiz = val; return;
        case USBOTG_DCFG: u->dcfg = val; return;
        case USBOTG_DIEPMSK: u->diepmsk = val; return;
        case USBOTG_DOEPMSK: u->doepmsk = val; return;
        case USBOTG_DAINTMSK: u->daintmsk = val; return;
        case USBOTG_GRSTCTL:
            if (val & 1u) {
                uint32_t connected = u->connected, pcgcctl = u->pcgcctl;
                s5l_usbotg_reset(u);
                s5l_usbotg_enable(u);
                u->connected = connected;
                u->pcgcctl = pcgcctl;
            }
            return;
        case USBOTG_DCTL: {
            uint32_t nak = u->dctl & (GLOBAL_IN_NAK | GLOBAL_OUT_NAK);
            if (val & (1u << 7)) nak |= GLOBAL_IN_NAK;
            if (val & (1u << 8)) nak &= ~GLOBAL_IN_NAK;
            if (val & (1u << 9)) nak |= GLOBAL_OUT_NAK;
            if (val & (1u << 10)) nak &= ~GLOBAL_OUT_NAK;
            u->dctl = (val & ~(0x780u | 12u)) | nak;
            return;
        }
        default: break;
    }
    if (off >= 0x104u && off < 0x140u) {
        u->dptxfsiz[(off - 0x104u) / 4u] = val; return;
    }
    uint32_t reg = 0;
    s5l_usb_ep_t *e = endpoint(u, off, &reg);
    if (!e) return;
    switch (reg) {
        case 0: {
            uint32_t nak = e->ctl & USBOTG_EP_NAK;
            uint32_t pid = e->ctl & (1u << 16);
            if (val & USBOTG_EP_CNAK) nak = 0;
            if (val & USBOTG_EP_SNAK) {
                nak = USBOTG_EP_NAK;
                if (off < 0xb00u) e->interrupt |= EP_IN_NAK;
            }
            if (val & (1u << 28)) pid = 0;
            if (val & (1u << 29)) pid = 1u << 16;
            if ((val & USBOTG_EP_DISABLE) && (e->ctl & USBOTG_EP_ENABLE)) {
                e->interrupt |= EP_DISABLED;
                val &= ~USBOTG_EP_ENABLE;
            }
            e->ctl = (val & ~(0x7c000000u | USBOTG_EP_NAK | (1u << 16))) |
                     nak | pid;
            return;
        }
        case 8: e->interrupt &= ~val; return;
        case 0x10: e->size = val; return;
        case 0x14: e->dma = val; return;
        default: return;
    }
}

void s5l_usbotg_connect(s5l_usbotg_t *u, bool connected) {
    if (!u || !u->enabled || u->connected == (uint32_t)connected) return;
    u->connected = connected;
    if (connected) u->gintsts |= 1u << 30;
    else {
        u->gotgint |= 1u << 2;
        u->gintsts |= 1u << 29;
        for (unsigned d = 0; d < 2u; d++)
            for (unsigned n = 0; n < S5L_USB_ENDPOINTS; n++)
                u->ep[d][n].ctl &= ~USBOTG_EP_ENABLE;
    }
}

bool s5l_usbotg_bus_reset(s5l_usbotg_t *u) {
    if (!u || !u->enabled || !u->connected || (u->dctl & SOFT_DISCONNECT))
        return false;
    u->dcfg &= ~(0x7fu << 4);
    for (unsigned d = 0; d < 2u; d++)
        for (unsigned n = 0; n < S5L_USB_ENDPOINTS; n++) {
            u->ep[d][n].ctl &= ~(USBOTG_EP_ENABLE | USBOTG_EP_STALL);
            u->ep[d][n].ctl |= USBOTG_EP_NAK;
            u->ep[d][n].interrupt = 0;
        }
    u->gintsts |= USBOTG_INT_RESET;
    return true;
}

bool s5l_usbotg_enumerated(s5l_usbotg_t *u) {
    if (!u || !u->enabled || !u->connected ||
        (u->dctl & SOFT_DISCONNECT) || (u->gintsts & USBOTG_INT_RESET))
        return false;
    u->gintsts |= USBOTG_INT_ENUM;
    return true;
}

/* Owner-thread-only. DMA is restricted to guest RAM, never MMIO/host memory.
 * OUT uses the normal host-load barrier to invalidate derived RAM grants. */
s5l_usb_result_t s5l8900_usb_packet(s5l8900_t *m, s5l_usb_token_t token,
                                    unsigned ep, void *data, size_t length,
                                    size_t *actual) {
    if (actual) *actual = 0;
    if (!m || !actual || ep >= S5L_USB_ENDPOINTS ||
        (token != S5L_USB_OUT && token != S5L_USB_IN && token != S5L_USB_SETUP) ||
        (length && !data) || (token == S5L_USB_SETUP && (ep || length != 8u)))
        return S5L_USB_INVALID;
    s5l_usbotg_t *u = &m->usbotg;
    if (!u->enabled || !u->connected || (u->dctl & SOFT_DISCONNECT))
        return S5L_USB_DISCONNECTED;
    unsigned d = token == S5L_USB_IN ? 1u : 0u;
    s5l_usb_ep_t *e = &u->ep[d][ep];
    bool setup = token == S5L_USB_SETUP;
    if (!setup && (e->ctl & USBOTG_EP_STALL)) return S5L_USB_STALL;
    if (!(u->gahbcfg & (1u << 5)) || (u->dcfg & (1u << 23)))
        return S5L_USB_INVALID; /* PIO / descriptor DMA unsupported */
    if (!(e->ctl & USBOTG_EP_ENABLE)) return S5L_USB_NAK;
    if (!setup && ((e->ctl & USBOTG_EP_NAK) ||
        (u->dctl & (d ? GLOBAL_IN_NAK : GLOBAL_OUT_NAK))))
        return S5L_USB_NAK;
    unsigned mps = ep ? e->ctl & 0x7ffu : 64u >> (e->ctl & 3u);
    uint32_t remaining = e->size & (ep ? SIZE_BYTES : 0x7fu);
    unsigned packets = (e->size & SIZE_PACKETS) >> 19;
    if (!mps || mps > 1024u || (!setup && !packets)) return S5L_USB_INVALID;
    size_t count = length;
    if (d) {
        count = remaining < mps ? remaining : mps;
        if (length < count) return S5L_USB_INVALID;
    } else if (!setup && (count > mps || count > remaining)) {
        return S5L_USB_INVALID;
    }
    if (setup && (!(e->size >> 29) || remaining < 8u)) return S5L_USB_NAK;
    if (count && (e->dma < m->ram_base || count > m->ram_size ||
        (uint64_t)(e->dma - m->ram_base) + count > m->ram_size)) {
        e->interrupt |= EP_AHB_ERROR;
        e->ctl &= ~USBOTG_EP_ENABLE;
        m->level_dirty = true;
        s5l8900_tick(m, 0);
        return S5L_USB_DMA_ERROR;
    }
    if (count) {
        if (d) memcpy(data, m->ram + (e->dma - m->ram_base), count);
        else s5l8900_load(m, e->dma, data, count);
    }
    e->dma += (uint32_t)count;
    remaining -= (uint32_t)count;
    if (packets) packets--;
    e->size = (e->size & ~(SIZE_BYTES | SIZE_PACKETS)) |
              remaining | (packets << 19);
    if (setup) {
        e->size -= 1u << 29;
        u->ep[1][0].ctl &= ~(USBOTG_EP_STALL | USBOTG_EP_ENABLE);
        u->ep[1][0].interrupt = 0;
        e->ctl &= ~USBOTG_EP_STALL;
        e->interrupt |= EP_SETUP;
        e->ctl &= ~USBOTG_EP_ENABLE;
    } else if (!remaining || !packets || count < mps) {
        e->interrupt |= EP_COMPLETE;
        e->ctl &= ~USBOTG_EP_ENABLE;
    }
    if (ep) e->ctl ^= 1u << 16;
    *actual = count;
    m->level_dirty = true;
    s5l8900_tick(m, 0);
    return S5L_USB_ACK;
}
