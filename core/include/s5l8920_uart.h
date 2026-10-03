/* Partial Apple UART used by N88's console and bootloader.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_UART_H
#define S5LBOX_S5L8920_UART_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    S5L8920_UART_ULCON = 0x00, S5L8920_UART_UCON = 0x04,
    S5L8920_UART_UFCON = 0x08, S5L8920_UART_UMCON = 0x0c,
    S5L8920_UART_UTRSTAT = 0x10, S5L8920_UART_UERSTAT = 0x14,
    S5L8920_UART_UFSTAT = 0x18,
    S5L8920_UART_UTXH = 0x20, S5L8920_UART_URXH = 0x24,
    S5L8920_UART_UBRDIV = 0x28
};
#define S5L8920_UART_FIFO_SIZE 16u
typedef struct {
    uint32_t ulcon, ucon, ufcon, umcon, ubrdiv;
    uint32_t programmed, pending, tx_remaining;
    uint8_t tx[S5L8920_UART_FIFO_SIZE], rx[S5L8920_UART_FIFO_SIZE];
    unsigned tx_head, tx_count, rx_head, rx_count;
    uint8_t tx_shift;
    bool tx_busy, no_modem, cts_valid, cts_asserted;
} s5l8920_uart_t;

/* Functional empty component reset, not an assertion of hardware reset values
 * or bootloader handoff. Configuration reads fail until the guest writes them;
 * status/data require line, channel, FIFO and divisor programming, plus modem
 * control unless the containing board sets its fixed no_modem capability.
 * Reset also clears that capability and external CTS observations.
 * Zero-threshold RX/TX events and explicitly supplied receive-timeout events
 * can drive interrupts. Received frames are error-free: UERSTAT reads zero
 * and accepts only zero acknowledgement; erroneous frames, DMA, fractional
 * offsets and auto baud remain unsupported.
 * No host pointers or output callbacks are retained. */
void s5l8920_uart_reset(s5l8920_uart_t *u);

/* Aligned word selectors only. Rejected accesses leave state/output unchanged.
 * Supports 8N1/8N2, disabled/polled channels, PCLK/NCLK selection, FIFO commands
 * with zero trigger fields, manual RTS/automatic CTS, and sample rates8..16.
 * Non-FIFO mode has one receive/transmit holding byte plus the TX shifter.
 * FIFO count status is unavailable in non-FIFO mode.
 * Configuration changes during transmission are refused except interrupt
 * and receive-timeout enables; FIFO resets affect queued
 * bytes, not the active shift register.
 * UTRSTAT RX/TX events are latched until word W1C, independently of IRQ enables;
 * live status bits0..2 in acknowledgements are ignored. Unknown bits refuse. */
bool s5l8920_uart_read(s5l8920_uart_t *u, uint32_t offset, uint32_t *value);
bool s5l8920_uart_write(s5l8920_uart_t *u, uint32_t offset, uint32_t value);
/* RX threshold/TX empty/receive-timeout causes use UCON bits12/13/11. */
bool s5l8920_uart_irq(const s5l8920_uart_t *u);

/* Supply cycles of the named source (false=PCLK,true=NCLK), after any external
 * gating. Unselected source has no effect. An 8N1 frame takes
 * (10 or 11) * (UBRDIV.divider+1) * (16-UBRDIV.sample_field) selected cycles.
 * Automatic flow control starts a frame only with known asserted CTS; a
 * started frame completes even if CTS is withdrawn. These are TX clocks;
 * receiver timeout duration/line sampling is owned by the input backend.
 * No CPU-cycle conversion or physical board frequency is implied.
 * Completed bytes are delivered in order. Insufficient output capacity fails
 * atomically, including count/output/state; retry with the same ticks.
 * Idle cycles are consumed, never banked as credit for a future byte. */
bool s5l8920_uart_clock(s5l8920_uart_t *u, bool nclk, uint64_t ticks,
                       uint8_t *output, size_t capacity, size_t *count);

/* Host supplies an already completed, error-free frame. This is a byte-level
 * backend boundary, not receive-line sampling or physical timing evidence.
 * Disabled/unconfigured/full receive paths refuse without inventing overrun. */
bool s5l8920_uart_receive(s5l8920_uart_t *u, uint8_t byte);
/* Observe CTS between frames. No-modem ports reject this input. */
bool s5l8920_uart_cts(s5l8920_uart_t *u, bool asserted);
/* Supply an observed receiver idle timeout, with enabled RX/timeout detection
 * and a held byte. No timeout threshold or event is inferred from polling or
 * TX clocks. Repeated events coalesce until W1C. */
bool s5l8920_uart_receive_timeout(s5l8920_uart_t *u);
#endif
