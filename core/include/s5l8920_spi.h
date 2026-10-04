/* Guarded programming and FIFO preparation of the N88 SPI controllers.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_SPI_H
#define S5LBOX_S5L8920_SPI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S5L8920_SPI_BASE UINT32_C(0x82000000)
#define S5L8920_SPI_STRIDE UINT32_C(0x100000)
/* Controllers described by the matching N88 device tree. */
#define S5L8920_SPI_COUNT 3u
#define S5L8920_SPI_EVENT_MASK UINT32_C(0x0040000f)
#define S5L8920_SPI_FIFO_LEVEL_MASK UINT32_C(0x0000ffc0)
/* Fields emitted by the matching SPI driver and bootloader. Their effects
 * are not activated while the controller remains stopped. */
#define S5L8920_SPI_CONFIG_MASK UINT32_C(0x0021e1ff)
#define S5L8920_SPI_FIFO_DEPTH 16u
#define S5L8920_SPI0_IRQ 29u

typedef struct {
    uint32_t words[S5L8920_SPI_FIFO_DEPTH];
    unsigned head, count;
    bool known;
} s5l8920_spi_fifo_t;

typedef struct {
    uint32_t pin, cleared_events;
    /* Raw programming. Timing words have no inferred silicon width,
     * effective rate or readback until serial clock support is modeled. */
    uint32_t config, divider, word_delay;
    bool stopped, pin_programmed;
    bool config_programmed, divider_programmed, word_delay_programmed;
    /* Programmed counts; a configured link decrements them on word completion. */
    uint32_t tx_words, rx_words;
    bool tx_count_programmed, rx_count_programmed;
    bool control_programmed, control_readable;
    s5l8920_spi_fifo_t tx, rx;
    uint32_t pending, shift_word, delay_remaining;
    unsigned bits_remaining, tx_low, rx_high;
    bool link_configured, busy;
} s5l8920_spi_t;

/* Reset invalidates all programming; it supplies no hardware reset image,
 * FIFO contents or interrupt observations. A zero object is also unprepared. */
void s5l8920_spi_reset(s5l8920_spi_t *s);

/* Word offsets. Control0=0 establishes a stopped controller. Pin0c accepts
 * the observed software CS bit1, after known control; writes preserve its latch.
 * Status08 acknowledges event mask40000f, accumulating which unknown
 * causes have been cleared. Echoed read-only FIFO level fields (bits6..15)
 * are ignored, not retained as observations. Other fields remain refused.
 * Without an explicitly configured word link, status reads remain unavailable,
 * even after a complete ACK. With a link, status combines actual FIFO levels,
 * latched RX/TX completion bits0/22 and the configured threshold level bit1.
 * While stopped, config04 retains driver fields with master encodings0/3,
 * mode0/1/2 and word-size0/1/2. Divider30 and word-delay38 retain raw words
 * independently; neither applies a guessed width mask. All three reads remain
 * refused. Staging does not validate timing, enable DMA, or start a transfer.
 * Control bits2/3 empty TX/RX respectively and invalidate their count knowledge.
 * Bit0 records an armed request only for prepared manual master/PIO operation,
 * known FIFOs and a nonzero divider. Nonzero control readback remains refused.
 * Stop preserves queued data and programming. Count4c/34 retain raw TX/RX
 * requests after the corresponding FIFO is known. PIO TX10 enqueues a raw word
 * in a 16-entry FIFO; RX20 consumes a known queued word. Full TX and empty RX
 * accesses refuse without effects. Armed configuration changes are limited
 * to selecting PIO mode and enabling modeled interrupts. IRQ enables require
 * a prepared link; DMA, automatic transmit and unknown bit13 wire ordering
 * remain guarded. Stop pauses an active word. In-flight reset, count, timing,
 * base configuration and CS changes refuse; whole component reset discards it.
 * No MMIO read/write implicitly advances a transfer. Physical pin routing,
 * source-clock conversion and attached devices are owned by the backend.
 * Unsupported operations preserve state and read output. */
bool s5l8920_spi_read(s5l8920_spi_t *s, uint32_t offset, uint32_t *value);
bool s5l8920_spi_write(s5l8920_spi_t *s, uint32_t offset, uint32_t value);
/* Opt into an error-free word backend before programming control. Explicit
 * thresholds: TX level <= tx_low (0..15) with words still requested, or RX level
 * >= rx_high (1..16). This is a functional level contract, not measured N88
 * reset thresholds/event-edge timing. An active level persists through W1C.
 * Repeating identical inputs is harmless; other reconfiguration refuses.
 * Functional reset invalidates the link. No NOR identity or reply is supplied. */
bool s5l8920_spi_configure_link(s5l8920_spi_t *s, unsigned tx_low, unsigned rx_high);
/* Completion enables7/21 and threshold enable8; external VIC input is separate. */
bool s5l8920_spi_irq(const s5l8920_spi_t *s);
/* Supply complete SCK periods after external selection/division. A word uses
 * 8/16/32 periods. The backend owns pin phase and supplies already decoded,
 * error-free received words at completion; config bit13 remains unsupported.
 * These are not CPU cycles or a guessed divider/reference frequency.
 * A completed word consumes one TX count and, while RX count is nonzero, one
 * received[] word. Returned transmitted words are masked to the word width.
 * Missing RX input, RX overflow or insufficient output capacity refuses the
 * entire call, preserving state, output and count. A call can complete at most
 * FIFO_DEPTH+1 words (queued FIFO plus any active shifter). Partial words need
 * no receive input until completion. Active exchange requires software CS0.
 * Idle, stopped and delay-blocked periods are discarded, never banked.
 * Status reads do not clock; absent backend input never becomes zero data. */
bool s5l8920_spi_serial_clock(s5l8920_spi_t *s, uint64_t cycles,
    const uint32_t *received, size_t received_count, uint32_t *transmitted,
    size_t capacity, size_t *count);
/* Independent word-delay reference cycles, supplied only in their real order
 * relative to serial periods. No extra delay follows the final TX word.
 * Stopping pauses this clock; excess idle cycles cannot start a serial word. */
bool s5l8920_spi_delay_clock(s5l8920_spi_t *s, uint64_t cycles);
#endif
