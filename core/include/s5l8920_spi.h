/* Guarded programming and FIFO preparation of the N88 SPI controllers.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_SPI_H
#define S5LBOX_S5L8920_SPI_H
#include <stdbool.h>
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
    /* Raw count requests, not completed or remaining serial transfers. */
    uint32_t tx_words, rx_words;
    bool tx_count_programmed, rx_count_programmed;
    bool control_programmed, control_readable;
    s5l8920_spi_fifo_t tx, rx;
} s5l8920_spi_t;

/* Reset invalidates all programming; it supplies no hardware reset image,
 * FIFO contents or interrupt observations. A zero object is also unprepared. */
void s5l8920_spi_reset(s5l8920_spi_t *s);

/* Word offsets. Control0=0 establishes a stopped controller. Pin0c accepts
 * the observed software CS bit1, after known control; writes preserve its latch.
 * Status08 acknowledges event mask40000f, accumulating which unknown
 * causes have been cleared. Echoed read-only FIFO level fields (bits6..15)
 * are ignored, not retained as observations. Other fields remain refused.
 * Neither stopping nor acknowledgement establishes
 * FIFO counts: status reads remain unavailable, even after a complete ACK.
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
 * accesses refuse without effects; there is no serial RX producer yet.
 * Armed configuration changes are limited to selecting PIO mode. IRQ enables,
 * DMA, automatic transmit and timing changes are guarded before effects.
 * Pin signal routing, serial clocks, events, IRQ generation and attached
 * devices remain unimplemented. No write/read implicitly advances a transfer.
 * Unsupported operations preserve state and read output. */
bool s5l8920_spi_read(s5l8920_spi_t *s, uint32_t offset, uint32_t *value);
bool s5l8920_spi_write(s5l8920_spi_t *s, uint32_t offset, uint32_t value);
#endif
