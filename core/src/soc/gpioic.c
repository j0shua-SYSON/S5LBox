/*
 * S5LBox — Samsung S5L8900 GPIO interrupt controller, and the GPIO pin block.
 *
 * Two blocks, one file, because /arm-io/gpio is one device tree node with two
 * reg ranges and one driver reaches both: the interrupt cascade on the
 * 0x39a00000 page and the pin state on the 0x3e400000 page. See the GPIOIC and
 * GPIO sections of core/include/soc.h for the register map, the disassembly
 * behind every offset, and why the two pages are addressed the way they are.
 *
 * WHAT THIS EXISTS FOR. The guest has already armed the touch interrupt: run59
 * measured this controller's group-4 enable register holding 0x08000000, which
 * is bit 27, which is line 155, which is /arm-io/spi1/multi-touch's
 * `interrupts {0x9b,0}`. Against a storage stub that enable was written into a
 * word nobody read and no line could ever be asserted, so a touch report had
 * nowhere to go. This turns the enable into a route.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <string.h>

/* ============================================================ interrupts === */

/*
 * The cascade, from /arm-io/gpio's own `interrupts` property:
 *   {0x21, 0x20, 0x1f, 0x03, 0x02, 0x01, 0x00}
 * one entry per interrupt group, in group order. It is descending, which looks
 * like a reversal and is not: the driver asks its provider for interrupt index
 * `group`, so index 4 — VIC line 2 — is the multi-touch group's line.
 *
 * Kept as one table behind one accessor because the same seven numbers are
 * needed by the bus routing, by the WFI wake table and by the tests, and three
 * transcriptions of a descending list is three chances to get one wrong.
 */
static const unsigned GPIOIC_CASCADE[S5L_GPIOIC_GROUPS] = {
    33u, 32u, 31u, 3u, 2u, 1u, 0u
};

unsigned s5l_gpioic_cascade(unsigned group) {
    /* Out of range answers with a line no VIC carries, which every caller —
     * s5l_vic_set_line(), wake_line_enabled() — already drops. */
    if (group >= S5L_GPIOIC_GROUPS) return 32u * S5L8900_VIC_COUNT;
    return GPIOIC_CASCADE[group];
}

void s5l_gpioic_reset(s5l_gpioic_t *g) {
    if (!g) return;
    /* Total, as the I2C and SPI controllers' resets are: valid on a poisoned
     * stack object, and it drops any level the board was driving. */
    memset(g, 0, sizeof *g);
}

static void note_unknown(s5l_gpioic_t *g, uint32_t off) {
    for (unsigned i = 0; i < g->unknown_off_count; i++)
        if (g->unknown_off[i] == off) return;
    if (g->unknown_off_count < S5L_GPIOIC_UNKNOWN_OFF)
        g->unknown_off[g->unknown_off_count++] = off;
}

/* Decode `off` into one of the four banks. Returns false for anything else. */
static bool decode(uint32_t off, uint32_t base, unsigned *group) {
    if (off < base) return false;
    uint32_t rel = off - base;
    if (rel & 3u) return false;
    uint32_t idx = rel >> 2;
    if (idx >= S5L_GPIOIC_GROUPS) return false;
    *group = (unsigned)idx;
    return true;
}

/*
 * THE PENDING LATCH IS EDGE-TRIGGERED, AND THAT WAS MEASURED, NOT CHOSEN.
 *
 * An earlier revision of this file made it LEVEL-sensitive: the write-one-to-
 * clear cleared the bit and then re-asserted it from whatever the board was
 * still driving. The reasoning was that a level latch cannot lose the second
 * report of a device that raises one mid-acknowledge, and that a device which
 * does deassert cannot tell the difference. The first half is true. The second
 * half is false, and run71 measured exactly how false:
 *
 *   HOT PAGE 0x39a00000, offset 0x0b0 (INTSTAT group 4):
 *       reads 1,193,122   writes 1,193,123   lastval 0x08000000
 *   @1799986776 R 0x39a000b0 val 0x08000000  pc 0xc05a44dc
 *   @1799986797 W 0x39a000b0 val 0x08000000  pc 0xc05a44e8
 *   @1799987195 R 0x39a000b0 val 0x08000000  pc 0xc05a44dc   <- 419 later
 *   @1799987216 W 0x39a000b0 val 0x08000000  pc 0xc05a44e8
 *
 * One touch report was queued at instruction 1,300,000,000 and the attention
 * line came up. The GPIO interrupt controller's filter read the pending word,
 * wrote it back to acknowledge, and our re-latch undid the acknowledge inside
 * the same store — so it re-entered every ~419 instructions and did so
 * 1,193,122 times, for the whole remaining half-billion instructions of the
 * run. `IOWorkLoop::signalWorkAvailable` was reached every time and the work
 * loop's thread was never scheduled, so AppleMultitouchZ2SPI's handler never
 * ran, never issued the SPI frame read, and the report was still pending at
 * the cap. A livelock, not a lost interrupt.
 *
 * The driver settles it a second way: it writes INTEN for this group exactly
 * TWICE in the whole boot (offset 0x0d0, writes 2), so it does not mask the
 * line while servicing it. A controller whose pending bit survived its own
 * acknowledge would be unusable by this driver, which means the real part's
 * does not.
 *
 * So: the write-one-to-clear CLEARS, and only a RISING edge on the incoming
 * line sets a pending bit again. A device that holds its line up until it is
 * serviced -- which is exactly what the Z2's attention line does -- gets one
 * interrupt per report, which is what it wants.
 *
 * INTLEVEL and INTTYPE are still deliberately NOT consulted. They are stored
 * and read-modify-written exactly as the driver expects (0xc05a5530-
 * 0xc05a5600), but nothing in the shipped device tree asks for a non-default
 * configuration: every `interrupts` property's second cell —
 * /arm-io/spi1/multi-touch's included — is zero, so both registers stay zero
 * for every line this machine can drive, and interpreting them would be
 * inventing a polarity nobody selected. Callers of s5l_gpioic_set_line() state
 * assertion as `true`.
 */

uint32_t s5l_gpioic_read(s5l_gpioic_t *g, uint32_t off) {
    unsigned group;
    if (!g) return 0u;
    if (decode(off, GPIOIC_INTLEVEL, &group)) return g->level[group];
    if (decode(off, GPIOIC_INTSTAT,  &group)) return g->stat[group];
    if (decode(off, GPIOIC_INTEN,    &group)) return g->en[group];
    if (decode(off, GPIOIC_INTTYPE,  &group)) return g->type[group];
    g->unknown_reads++;
    note_unknown(g, off);
    return 0u;
}

void s5l_gpioic_write(s5l_gpioic_t *g, uint32_t off, uint32_t val) {
    unsigned group;
    if (!g) return;
    if (decode(off, GPIOIC_INTLEVEL, &group)) {
        /* Configuration. Stored whole: the driver only ever reaches it through
         * a read-modify-write of one bit, so storing the word it hands back is
         * the same thing and needs no bit interpretation. */
        g->level[group] = val;
        return;
    }
    if (decode(off, GPIOIC_INTSTAT, &group)) {
        /*
         * WRITE-ONE-TO-CLEAR, and nothing else. This is not a guess: the enable
         * path at 0xc05a56a0 writes exactly `1 << bit` here immediately before
         * enabling that one line, which is only meaningful as "discard the
         * stale pending bit for the line I am about to arm". A register that
         * SET pending bits instead would have the driver raising an interrupt
         * every time it enabled a line.
         */
        /* And it CLEARS. It does not re-latch from what is still driven —
         * see the note above, and run71's 1,193,122 acknowledges. */
        g->stat[group] &= ~val;
        return;
    }
    if (decode(off, GPIOIC_INTEN, &group)) {
        /*
         * A plain mask, not a set/clear pair. The driver keeps its own shadow
         * word per group at object+0x88 and stores the whole shadow here on
         * both paths — `orr` then store when enabling (0xc05a56b0-0xc05a56d8),
         * `bic` then store when disabling (0xc05a5748-0xc05a5788) — and the
         * init loop stores a bare zero (0xc05a51a8). A write-one-to-set model
         * would make that disable path a no-op and leave every line armed.
         */
        g->en[group] = val;
        return;
    }
    if (decode(off, GPIOIC_INTTYPE, &group)) {
        g->type[group] = val;
        return;
    }
    g->unknown_writes++;
    note_unknown(g, off);
}

void s5l_gpioic_set_line(s5l_gpioic_t *g, unsigned line, bool level) {
    if (!g || line >= S5L_GPIOIC_LINES) return;
    unsigned group = line >> 5;
    uint32_t bit   = 1u << (line & 31u);
    bool was = (g->raw[group] & bit) != 0u;
    if (level) g->raw[group] |= bit;
    else       g->raw[group] &= ~bit;
    /*
     * A RISING EDGE latches. Two consequences worth stating, because callers
     * refresh this every tick with the current level:
     *   - a line that is merely STILL high sets nothing, which is what stops
     *     the acknowledge/re-assert livelock run71 measured;
     *   - deasserting does NOT clear the latch, so a pulse shorter than the
     *     guest's polling interval is still delivered. Only the guest's
     *     write-one-to-clear clears it.
     */
    if (level && !was) g->stat[group] |= bit;
}

bool s5l_gpioic_line(const s5l_gpioic_t *g, unsigned line) {
    if (!g || line >= S5L_GPIOIC_LINES) return false;
    return (g->raw[line >> 5] & (1u << (line & 31u))) != 0u;
}

bool s5l_gpioic_group_irq(const s5l_gpioic_t *g, unsigned group) {
    if (!g || group >= S5L_GPIOIC_GROUPS) return false;
    return (g->stat[group] & g->en[group]) != 0u;
}

/* ============================================================== pin block === */

void s5l_gpio_reset(s5l_gpio_t *g) {
    if (!g) return;
    memset(g, 0, sizeof *g);
}

/*
 * Honest byte-addressable storage over the whole page, assembled a byte at a
 * time exactly as the storage stub this replaces did — so an 8- or 16-bit
 * access updates the correct lanes and nothing can index past the array. The
 * page really is 4 KiB of registers we have not decoded; pretending otherwise
 * by answering an undecoded offset with a fabricated value is the failure the
 * stub design exists to avoid.
 */
uint32_t s5l_gpio_read(const s5l_gpio_t *g, uint32_t off, unsigned bytes) {
    uint32_t v = 0;
    if (!g || !bytes || bytes > 4u) return 0u;
    if ((uint64_t)off + bytes > (uint64_t)S5L8900_DEV_SIZE) return 0u;
    for (unsigned i = 0; i < bytes; i++) {
        uint32_t at = off + i;
        v |= ((g->regs[at >> 2] >> ((at & 3u) * 8u)) & 0xffu) << (i * 8u);
    }
    return v;
}

/* True if the byte at `off` belongs to some group's read-only level register. */
static bool is_pin_reg(uint32_t off) {
    uint32_t word = off & ~3u;
    if (word < 4u || ((word - 4u) % 32u) != 0u) return false;
    return ((word - 4u) / 32u) < S5L_GPIO_PORTS;
}

void s5l_gpio_write(s5l_gpio_t *g, uint32_t off, uint32_t val, unsigned bytes) {
    uint32_t before[S5L_GPIO_PORTS];
    if (!g || !bytes || bytes > 4u) return;
    if ((uint64_t)off + bytes > (uint64_t)S5L8900_DEV_SIZE) return;

    /* Snapshot every pin word so a watcher is told about a real change and not
     * merely about a store. */
    for (unsigned i = 0; i < S5L_GPIO_PORTS; i++)
        before[i] = g->regs[S5L_GPIO_PIN_REG(i) >> 2];

    for (unsigned i = 0; i < bytes; i++) {
        uint32_t at = off + i;
        /* The level registers are read-only. Dropping the store rather than
         * storing it is what makes the fsel path the ONLY way a pin moves, so
         * a model that quietly accepted both could not be wrong about which
         * one the guest actually uses. */
        if (is_pin_reg(at)) continue;
        uint32_t shift = (at & 3u) * 8u;
        uint32_t *reg = &g->regs[at >> 2];
        *reg = (*reg & ~(0xffu << shift)) |
               (((val >> (i * 8u)) & 0xffu) << shift);
    }

    /*
     * The function-select store is how a pin is driven:
     *   write32(base + 0x320, (group << 16) | (bit << 8) | 0xE | level)
     * One 32-bit store, no read-modify-write, and 0x320 is the only offset on
     * this page run59 observed the guest touch at all.
     *
     * A word store only: a byte or halfword cannot carry the group, the bit and
     * the function together, so treating a partial store as a pin command would
     * be acting on a value the guest has not finished assembling.
     */
    if (off == S5L_GPIO_FSEL && bytes == 4u) {
        unsigned group = (val >> 16) & 0xffu;
        unsigned bit   = (val >> 8) & 0xffu;
        if (group < S5L_GPIO_PORTS && bit < 32u &&
            ((val >> 1) & 0x7u) == S5L_GPIO_FUNC_OUT) {
            uint32_t *level = &g->regs[S5L_GPIO_PIN_REG(group) >> 2];
            if (val & 1u) *level |=  (1u << bit);
            else          *level &= ~(1u << bit);
        }
    }

    for (unsigned w = 0; w < S5L_GPIO_WATCHERS; w++) {
        s5l_gpio_watch_t *watch = &g->watch[w];
        if (!watch->armed || !watch->changed) continue;
        unsigned group = (unsigned)(watch->pin >> 8);
        if (group >= S5L_GPIO_PORTS) continue;
        uint32_t mask = 1u << (watch->pin & 0x1fu);
        uint32_t now  = g->regs[S5L_GPIO_PIN_REG(group) >> 2];
        if (((before[group] ^ now) & mask) == 0u) continue;
        watch->changed(watch->ctx, (now & mask) != 0u);
    }
}

bool s5l_gpio_pin(const s5l_gpio_t *g, uint16_t pin) {
    unsigned group = (unsigned)(pin >> 8);
    if (!g || group >= S5L_GPIO_PORTS) return false;
    return (g->regs[S5L_GPIO_PIN_REG(group) >> 2] &
            (1u << (pin & 0x1fu))) != 0u;
}

bool s5l_gpio_watch(s5l_gpio_t *g, uint16_t pin, void *ctx,
                    void (*changed)(void *ctx, bool level)) {
    if (!g || !changed) return false;
    if ((unsigned)(pin >> 8) >= S5L_GPIO_PORTS) return false;
    if ((pin & 0xffu) > 31u) return false;
    for (unsigned i = 0; i < S5L_GPIO_WATCHERS; i++)
        if (g->watch[i].armed && g->watch[i].pin == pin) return false;
    for (unsigned i = 0; i < S5L_GPIO_WATCHERS; i++)
        if (!g->watch[i].armed) {
            g->watch[i].ctx = ctx;
            g->watch[i].pin = pin;
            g->watch[i].changed = changed;
            g->watch[i].armed = true;
            return true;
        }
    return false;
}
