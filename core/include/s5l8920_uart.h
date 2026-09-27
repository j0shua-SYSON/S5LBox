/* Partial Apple UART used by N88's early polled console.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_UART_H
#define S5LBOX_S5L8920_UART_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    S5L8920_UART_ULCON = 0x00, S5L8920_UART_UCON = 0x04,
    S5L8920_UART_UFCON = 0x08, S5L8920_UART_UMCON = 0x0c,
    S5L8920_UART_UTRSTAT = 0x10, S5L8920_UART_UFSTAT = 0x18,
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
    bool tx_busy;
} s5l8920_uart_t;

/* Functional empty component reset, not an assertion of hardware reset values
 * or bootloader handoff. Configuration reads fail until the guest writes them;
 * status/data require all five configuration registers to be programmed.
 * No interrupts, DMA, errors, modem input, fractional offsets or auto baud.
 * No host pointers or output callbacks are retained. */
void s5l8920_uart_reset(s5l8920_uart_t *u);

/* Aligned word selectors only. Rejected accesses leave state/output unchanged.
 * Supports 8N1, disabled/polled channels, PCLK/NCLK selection, FIFO commands
 * with zero trigger fields, manual RTS, divider/sample fields (rates8..16).
 * Traffic requires enabled FIFO. Configuration changes during transmission
 * are refused; FIFO resets affect queued bytes, not the active shift register.
 * UTRSTAT RX/TX events are latched until word W1C, independently of IRQ enables;
 * enabling interrupt delivery itself remains unsupported. */
bool s5l8920_uart_read(s5l8920_uart_t *u, uint32_t offset, uint32_t *value);
bool s5l8920_uart_write(s5l8920_uart_t *u, uint32_t offset, uint32_t value);

/* Supply cycles of the named source (false=PCLK,true=NCLK), after any external
 * gating. Unselected source has no effect. An 8N1 frame takes
 * 10 * (UBRDIV.divider+1) * (16-UBRDIV.sample_field) selected input cycles.
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
#endif
