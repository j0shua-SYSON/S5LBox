/*
 * S5LBox — PowerVR MBX register block, enough of it for the kext to start.
 *
 * WHY THIS EXISTS. QuartzCore composites every pixel on the CPU in this VM,
 * and that software path is 40.2% of the instructions in a home-screen swipe.
 * It is not the path a real iPhone takes: CA::WindowServer::MBXServer DEFAULTS
 * to the MBX2D hardware path and only falls back when its IOKit connection is
 * missing. The connection is missing because com.apple.driver.AppleMBX never
 * finishes starting, and it never finishes starting because it waits on a
 * register this VM did not have. So the software rasteriser is not something
 * to high-level-emulate around; it is a symptom of an unmodelled device, and
 * tools/ios3_hle.c's rule is that a model gap gets modelled.
 *
 * WHAT THE DRIVER ACTUALLY WAITS FOR, taken from its own code rather than
 * guessed -- the same standard core/src/soc/power.c held itself to. The reset
 * routine at AppleMBX+0xb400 (0xc0783440) reads:
 *
 *     mov  r2, #1
 *     ldr  r1, [pc, #0x2c]        ; r1 = 0x1020, from the literal pool
 *     blx  ip                     ; write(obj, 0x1020, 1)
 *   loop:
 *     ldr  r0, [r4, #4]
 *     ldr  r1, [pc, #0x14]        ; r1 = 0x1020 again
 *     blx  r5                     ; r0 = read(obj, 0x1020)
 *     tst  r0, #0x10000           ; bit 16
 *     beq  loop                   ; spin while it is CLEAR
 *     mov  r3, #1
 *     strb r3, [r4, #1]           ; mark the device ready, return
 *
 * So: write 1 to 0x1020, then spin until bit 16 reads back set. That is a
 * reset request and its completion acknowledgement, and it is the whole of
 * what this file has evidence for.
 *
 * THE EVIDENCE THAT THE ADDRESS IS RIGHT is independent of the disassembly.
 * r225 ran with the nub matched and counted accesses per page: 0x3b000000
 * took 2 reads and 3 writes, and 0x3b001000 took 157,441,382 reads and 9
 * writes. Offset 0x1020 from the block base falls in the second page, which
 * is the page that spun. Two instruments, one conclusion.
 *
 * WHAT THIS IS NOT. It is not a GPU, it does not render, and it does not
 * pretend to. It is the reset handshake and a register file that remembers
 * what was written to it. Everything the driver does after this returns is a
 * fresh question to be answered the same way -- let it run, see where it
 * stops, read its code, model what it waited for. Anything not understood
 * must read back as what was written rather than as an invented value, so a
 * driver that depends on something real gets stuck LOUDLY at a new address
 * instead of quietly proceeding on a fabrication.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * THE INSTRUMENT FOR THE NEXT STALL, and the file predicted needing it: "if the
 * driver later reads the region and depends on an effect that never happened,
 * it will stall or complain at a NEW address". Finding that address is a
 * counting problem, and the existing device log cannot count it -- it keeps the
 * FIRST S5L_DEVLOG accesses, and r225 measured 157 million reads on one page,
 * so the log is full of the reset spin before the driver has even started.
 *
 * A per-offset histogram answers it directly: whatever the driver is spinning
 * on is whatever has a count in the millions once the reset handshake is done.
 * The ring holds the last few accesses so the spin's SHAPE is visible too --
 * one offset read forever is a poll, a repeating group is a command sequence.
 *
 * This is deliberately file-static rather than a member of s5l_mbx_t. It is a
 * diagnostic and not machine state, so it must not enter a snapshot; keeping it
 * out of the struct also keeps sizeof(s5l_mbx_t), its SNAP_SIZE_GUARD and
 * SNAPSHOT_VERSION untouched, and a version bump would invalidate every
 * existing snapshot including one being captured while this was written.
 *
 * It counts and prints. It changes no value the guest can observe.
 */
#define MBX_SLOTS      (S5L_MBX_SIZE / 4u)
#define MBX_RING       64u

static uint64_t mbx_rd[MBX_SLOTS];
static uint64_t mbx_wr[MBX_SLOTS];
static uint32_t mbx_last_val[MBX_SLOTS];
static uint32_t mbx_ring_off[MBX_RING];
static uint32_t mbx_ring_val[MBX_RING];
static uint8_t  mbx_ring_wr[MBX_RING];
static uint64_t mbx_ring_n;
static int      mbx_trace_state;   /* 0 unknown, 1 on, -1 off */

static void mbx_trace_dump(void) {
    uint64_t total_r = 0, total_w = 0;
    unsigned i;

    fprintf(stderr, "\n=== MBX access histogram ===\n");
    for (i = 0; i < MBX_SLOTS; i++) {
        if (!mbx_rd[i] && !mbx_wr[i]) continue;
        total_r += mbx_rd[i];
        total_w += mbx_wr[i];
        fprintf(stderr, "  off 0x%04x  reads %-14llu writes %-8llu last-write 0x%08x\n",
                i * 4u, (unsigned long long)mbx_rd[i],
                (unsigned long long)mbx_wr[i], mbx_last_val[i]);
    }
    fprintf(stderr, "  TOTAL reads %llu writes %llu\n",
            (unsigned long long)total_r, (unsigned long long)total_w);

    fprintf(stderr, "=== MBX last %u accesses (oldest first) ===\n",
            (unsigned)(mbx_ring_n < MBX_RING ? mbx_ring_n : MBX_RING));
    {
        uint64_t start = mbx_ring_n > MBX_RING ? mbx_ring_n - MBX_RING : 0;
        uint64_t k;
        for (k = start; k < mbx_ring_n; k++) {
            unsigned s = (unsigned)(k % MBX_RING);
            fprintf(stderr, "  %s off 0x%04x val 0x%08x\n",
                    mbx_ring_wr[s] ? "W" : "R", mbx_ring_off[s],
                    mbx_ring_val[s]);
        }
    }
    fflush(stderr);
}

static void mbx_trace(uint32_t off, uint32_t val, bool is_write) {
    unsigned s;

    if (mbx_trace_state == 0) {
        const char *e = getenv("S5LBOX_MBX_TRACE");
        mbx_trace_state = (e && *e && *e != '0') ? 1 : -1;
        if (mbx_trace_state == 1) atexit(mbx_trace_dump);
    }
    if (mbx_trace_state != 1) return;

    if (is_write) { mbx_wr[off / 4u]++; mbx_last_val[off / 4u] = val; }
    else            mbx_rd[off / 4u]++;

    s = (unsigned)(mbx_ring_n % MBX_RING);
    mbx_ring_off[s] = off;
    mbx_ring_val[s] = val;
    mbx_ring_wr[s]  = is_write ? 1u : 0u;
    mbx_ring_n++;
}

void s5l_mbx_reset(s5l_mbx_t *m) {
    if (!m) return;
    /*
     * Total, like every other reset here: valid on a poisoned stack object.
     * Zero is also the honest power-on value for the one register whose
     * meaning is known -- reset has NOT completed until it is asked for, and
     * a device that reported completion before the request would let the
     * driver past a handshake that never happened.
     */
    /* The buffer belongs to the machine and outlives a device reset; its
     * CONTENTS are cleared, which is what a powered-down edram holds. */
    uint8_t *edram = m->edram;
    memset(m, 0, sizeof *m);
    m->edram = edram;
    if (edram) memset(edram, 0, S5L_MBX_EDRAM_SIZE);
}

uint32_t s5l_mbx_read(s5l_mbx_t *m, uint32_t off) {
    uint32_t v;

    if (!m || off >= S5L_MBX_APERTURE) return 0;

    /* Above the register block the aperture is edram: plain storage. */
    if (off >= S5L_MBX_SIZE) {
        uint32_t o = off - S5L_MBX_SIZE;
        if (!m->edram || o + 4u > S5L_MBX_EDRAM_SIZE) return 0;
        return (uint32_t)m->edram[o] | ((uint32_t)m->edram[o + 1u] << 8) |
               ((uint32_t)m->edram[o + 2u] << 16) |
               ((uint32_t)m->edram[o + 3u] << 24);
    }

    if (off == S5L_MBX_RESET) {
        /*
         * The acknowledgement. Bit 16 reads set only once the reset has been
         * requested, so the driver's spin ends because of something it did,
         * not because of a constant this file chose.
         *
         * BIT 16 IS OURS, AND THE STORED VALUE MUST NOT BE ABLE TO SUPPLY IT.
         * This used to OR reset_done onto whatever the register file held,
         * which is wrong the moment the guest writes the bit back -- and the
         * driver does exactly that. It deasserts by READ-MODIFY-WRITE: it reads
         * this register while bit 16 is set, clears bit 0, and writes the rest
         * back, so 0x00010000 lands in the register file. The old read then
         * returned that stored bit forever, and the deassert wait at
         * AppleMBX+0xb0c0 --
         *
         *     ands r0, r0, #0x10000
         *     bne  loop                  ; spin while bit 16 is SET
         *
         * -- could never end. r242 measured the consequence: 328,111,110 reads
         * of this one offset against 46 writes to the whole block, with the
         * last write to it being 0x00010000. That is not a driver polling for
         * progress, it is a driver that will never be told the reset finished.
         *
         * Masking first makes the bit a STATUS the model owns in both
         * directions: written bit 0 is the request, and bit 16 is the answer,
         * which is the coupling the driver expects and the one power.c holds
         * itself to.
         */
        v = m->reg[off / 4u] & ~S5L_MBX_RESET_DONE;
        if (m->reset_done) v |= S5L_MBX_RESET_DONE;
    } else if (off == S5L_MBX_STATUS) {
        v = m->status;
    } else if (off == S5L_MBX_REVISION) {
        /*
         * Identity, and it is read-only: the guest never writes here, so
         * serving it out of the register file would return the zero that made
         * AppleMBXDevice::start reject this device as unrecognised silicon.
         */
        v = S5L_MBX_REVISION_ID;
    } else {
        v = m->reg[off / 4u];
    }

    mbx_trace(off, v, false);
    return v;
}

/*
 * The interrupt output. See the declaration in soc.h for why it exists and why
 * it is a level rather than a pulse.
 *
 * It reports the status word and nothing else, so it cannot assert without a
 * kick having set a bit and cannot stay asserted once the driver has
 * acknowledged. A device that raised its line on its own would be exactly the
 * fabrication the header of this file refuses.
 */
bool s5l_mbx_irq(const s5l_mbx_t *m) {
    /* status & mask, exactly as the driver's ISR computes `pending`. A
     * line that asserted on an unmasked bit would interrupt for something
     * the driver has not asked about and cannot acknowledge. */
    if (!m) return false;
    return (m->status & m->reg[S5L_MBX_INTMASK / 4u]) != 0u;
}

void s5l_mbx_write(s5l_mbx_t *m, uint32_t off, uint32_t val) {
    if (!m || off >= S5L_MBX_APERTURE) return;

    if (off >= S5L_MBX_SIZE) {
        uint32_t o = off - S5L_MBX_SIZE;
        if (!m->edram || o + 4u > S5L_MBX_EDRAM_SIZE) return;
        m->edram[o]      = (uint8_t)(val & 0xffu);
        m->edram[o + 1u] = (uint8_t)((val >> 8) & 0xffu);
        m->edram[o + 2u] = (uint8_t)((val >> 16) & 0xffu);
        m->edram[o + 3u] = (uint8_t)((val >> 24) & 0xffu);
        return;
    }

    mbx_trace(off, val, true);

    m->reg[off / 4u] = val;

    /*
     * Bit 0 is the request, and it is treated as the trigger because it is
     * the bit the driver sets: it writes the literal 1. A write that does NOT
     * carry bit 0 deliberately leaves the acknowledgement alone rather than
     * clearing it -- nothing observed shows the driver taking reset back, and
     * inventing a way to un-complete a reset would be a behaviour with no
     * evidence behind it.
     */
    /*
     * BIT 16 FOLLOWS THE REQUEST; it does not latch. The first version of this
     * file set it on a request and never cleared it, with the comment that
     * "nothing observed shows the driver taking a reset back". That was true
     * of the evidence then and false of the device, and the driver said so:
     * AppleMBX+0xb0c0 writes 0 to this same register and then spins
     *
     *     ands r0, r0, #0x10000
     *     bne  loop                  ; wait while bit 16 is SET
     *
     * which is the exact mirror of the assert wait at AppleMBX+0xb440. A latch
     * that never cleared turned that into an infinite loop -- the driver
     * attached, bound the display, and then hung here, which is how it was
     * found. So the bit tracks the request in both directions.
     */
    if (off == S5L_MBX_RESET) m->reset_done = (val & 1u) != 0u;

    /*
     * Write-one-to-clear on the status word, taken from the driver's own
     * acknowledgement: it clears exactly the bit it waited on (0x40), and
     * clears 0x7ff during init to drop everything pending at once.
     */
    if (off == S5L_MBX_STATUS_ACK) m->status &= ~val;

    /*
     * The completion this file CAN honestly assert, and the line it will not
     * cross.
     *
     * The driver programs an enable, a size and an address, then waits for bit
     * 6. Writing the address is the last step, so it is the go. Raising the
     * bit there models the COUPLING the driver expects -- the same thing
     * power.c does when it moves STATE only in response to ONCTRL/OFFCTRL, and
     * for the same reason: a status that never moves is a spin, and a status
     * that moves on its own is a fiction.
     *
     * WHAT IS NOT CLAIMED. Whatever transfer 0x838/0x83c/0x6d8 describe is NOT
     * performed. No byte is moved, nothing is written to 0x09000000, and this
     * file does not know whether the operation is a clear, a copy or a
     * self-test. Saying "done" is therefore a statement about the handshake
     * and not about memory.
     *
     * That is safe only because it invents no DATA. If the driver later reads
     * the region and depends on an effect that never happened, it will stall
     * or complain at a NEW address, which is observable and diagnosable --
     * unlike a fabricated buffer, which would look like success and be found
     * days later. The moment such a stall appears, the operation gets modelled
     * properly rather than acknowledged.
     */
    if (off == S5L_MBX_KICK)
        m->status |= S5L_MBX_STATUS_DONE | S5L_MBX_STATUS_2D;
}
