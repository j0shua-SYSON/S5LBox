/*
 * S5LBox — the two S5L8900 I2S controller windows.
 *
 * Honest storage for the seven offsets AppleS5L8900XI2SController writes, and
 * bounded visibility for anything else. It stores rather than interprets: no
 * semantics are claimed for any of the seven, because the driver never reads
 * one back and nothing in the kernelcache documents them.
 *
 * See the I2S block in soc.h for the enumeration of the seven write sites and
 * for the four checks that establish `readRegister` is dead code.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <string.h>

/*
 * The offsets, in this model's storage order. This is the whole register map:
 * every writeRegister dispatch in the class resolves to one of these seven, so
 * a driver that touched an eighth would be doing something no shipped code path
 * does — which is exactly what the unknown-offset log exists to make visible.
 */
static const uint32_t I2S_OFFSETS[S5L_I2S_REGS] = {
    0x00u, 0x04u, 0x08u, 0x30u, 0x34u, 0x3cu, 0x40u
};

uint32_t s5l_i2s_offset(unsigned index) {
    if (index >= S5L_I2S_REGS) return UINT32_MAX;
    return I2S_OFFSETS[index];
}

static int slot_for(uint32_t off) {
    for (unsigned i = 0; i < S5L_I2S_REGS; i++)
        if (I2S_OFFSETS[i] == off) return (int)i;
    return -1;
}

void s5l_i2s_reset(s5l_i2s_t *i2s) {
    if (!i2s) return;
    memset(i2s, 0, sizeof *i2s);
}

static void note_unknown(s5l_i2s_t *i2s, uint32_t off) {
    for (unsigned i = 0; i < i2s->unknown_off_count; i++)
        if (i2s->unknown_off[i] == off) return;
    if (i2s->unknown_off_count < S5L_I2S_UNKNOWN_OFF)
        i2s->unknown_off[i2s->unknown_off_count++] = off;
}

uint32_t s5l_i2s_read(s5l_i2s_t *i2s, uint32_t off) {
    if (!i2s) return 0u;
    i2s->reads++;
    int slot = slot_for(off);
    if (slot >= 0) return i2s->regs[slot];
    /*
     * A read is already off the established path — the stock driver issues
     * none at all — so every one of them is worth recording, including reads of
     * the FIFO offsets 0x10 and 0x38. Those two are the PL080's, delivered to
     * it as physical addresses out of the device tree, and a CPU read of them
     * would mean something is doing programmed I/O where DMA was intended.
     */
    i2s->unknown_reads++;
    note_unknown(i2s, off);
    return 0u;
}

void s5l_i2s_write(s5l_i2s_t *i2s, uint32_t off, uint32_t val) {
    if (!i2s) return;
    i2s->writes++;
    int slot = slot_for(off);
    if (slot >= 0) {
        i2s->regs[slot] = val;
        return;
    }
    i2s->unknown_writes++;
    note_unknown(i2s, off);
}
