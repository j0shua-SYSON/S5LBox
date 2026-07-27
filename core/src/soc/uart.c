/*
 * S5LBox — S5L8900 UART.
 *
 * This is the first device that makes the emulator *observable*: whatever the
 * guest writes to UTXH is captured, which is how iBoot and the XNU kernel will
 * later tell us how far they got.
 *
 * It is now also the first device that makes the emulator *reachable*: the
 * receive FIFO below is the host->guest direction docs/networking.md §6 needs
 * for Route D, and the only thing that can put a byte in it is a host calling
 * s5l_uart_rx_push() between run slices. Every register rule the receive path
 * implements is quoted from the UART block in soc.h; read that first.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <string.h>

/* UTRSTAT bits: [0] receive data ready, [1] TX buffer empty, [2] transmitter
 * empty. We report the transmitter permanently drained so guest spin-loops of
 * the form "wait until TX empty, then write" always make progress. */
#define UTRSTAT_RX_READY     (1u << 0)
#define UTRSTAT_TX_EMPTY     (1u << 2)
#define UTRSTAT_TX_BUF_EMPTY (1u << 1)

/* UFSTAT: bits[3:0] receive count, bit 8 receive full, bits[7:4] transmit
 * count, bit 9 transmit full. The transmitter drains instantly, so its two
 * fields are permanently zero. */
#define UFSTAT_RX_FULL       (1u << 8)

void s5l_uart_reset(s5l_uart_t *u) {
    /* Still every byte, including rx_irq_suppressed. Preserving the flag here
     * was tried and reverted: reset's one caller is s5l8900_init(), which runs
     * on storage that has not been initialised yet, so reading any field before
     * the memset is reading uninitialised memory — and core/tests/test_uart4.c
     * pins reset as "a statement about every byte", which a preserved field
     * would quietly falsify. The ordering requirement lives in
     * s5l_uart_set_rx_irq()'s contract instead: call it AFTER init. */
    memset(u, 0, sizeof *u);
}

void s5l_uart_set_rx_irq(s5l_uart_t *u, bool enabled) {
    if (u) u->rx_irq_suppressed = !enabled;
}

unsigned s5l_uart_rx_space(const s5l_uart_t *u) {
    if (!u || u->rx_count >= UART_RX_FIFO) return 0u;
    return UART_RX_FIFO - u->rx_count;
}

bool s5l_uart_rx_irq(const s5l_uart_t *u) {
    /* The suppression is applied HERE and nowhere else, so that every other
     * observable — UTRSTAT bit 0, UFSTAT's count, what URXH dequeues — is bit
     * for bit what it is with the interrupt on. That is what makes the two runs
     * a controlled pair: the VIC line is the only difference between them. */
    if (u && u->rx_irq_suppressed) return false;
    return u && u->rx_count != 0u;
}

bool s5l_uart_rx_push(s5l_uart_t *u, uint8_t byte) {
    if (!u) return false;
    if (u->rx_count >= UART_RX_FIFO) {
        /* Refuse and count. Growing the FIFO would model a UART nobody built;
         * blocking would stall the CPU thread, which docs/networking.md §8.3
         * forbids outright. A dropped byte is a lost frame and the peer's
         * restart timer is what recovers it. */
        u->rx_dropped++;
        return false;
    }
    u->rx[(unsigned)(u->rx_head + u->rx_count) % UART_RX_FIFO] = byte;
    u->rx_count++;
    u->rx_pushed++;
    return true;
}

uint32_t s5l_uart_read(s5l_uart_t *u, uint32_t off) {
    switch (off) {
        case UART_ULCON:   return u->ulcon;
        case UART_UCON:    return u->ucon;
        case UART_UFCON:   return u->ufcon;
        case UART_UMCON:   return u->umcon;
        case UART_UBRDIV:  return u->ubrdiv;
        case UART_UTRSTAT:
            return UTRSTAT_TX_EMPTY | UTRSTAT_TX_BUF_EMPTY |
                   (u->rx_count ? UTRSTAT_RX_READY : 0u);
        case UART_UERSTAT: return 0;
        case UART_UFSTAT:
            /* The count field is four bits wide and the depth is sixteen, so a
             * full FIFO reports 0 in the field and 1 in the full bit. That is
             * not a truncation bug: it is why the full bit exists. */
            return ((uint32_t)u->rx_count & 0x0fu) |
                   (u->rx_count >= UART_RX_FIFO ? UFSTAT_RX_FULL : 0u);
        case UART_UMSTAT:  return 0;
        case UART_URXH: {
            if (u->rx_count == 0u) {
                /* No byte has arrived. Zero is the only answer that does not
                 * invent one; the counter is what tells a reader afterwards
                 * that the guest polled an empty port rather than receiving
                 * a stream of NULs. */
                u->rx_underruns++;
                return 0;
            }
            uint8_t b = u->rx[u->rx_head];
            u->rx_head = (uint8_t)((u->rx_head + 1u) % UART_RX_FIFO);
            u->rx_count--;
            u->rx_reads++;
            return b;
        }
        default:           return 0;
    }
}

void s5l_uart_write(s5l_uart_t *u, uint32_t off, uint32_t val) {
    switch (off) {
        case UART_ULCON:  u->ulcon  = val; break;
        case UART_UCON:   u->ucon   = val; break;
        case UART_UFCON:  u->ufcon  = val; break;
        case UART_UMCON:  u->umcon  = val; break;
        case UART_UBRDIV: u->ubrdiv = val; break;
        case UART_UTXH:
            if (u->tx_len < UART_TX_BUFFER - 1) u->tx[u->tx_len++] = (char)(val & 0xff);
            break;
        /* UART_UTRSTAT is write-one-to-clear on the hardware and is dropped
         * here on purpose — every bit this model reports is a level with a live
         * source behind it, and clearing bit 0 while a byte still sat in the
         * FIFO would lose that byte. See the receive-path block in soc.h. */
        default: break;
    }
}
