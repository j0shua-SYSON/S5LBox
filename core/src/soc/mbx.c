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
#include <string.h>

void s5l_mbx_reset(s5l_mbx_t *m) {
    if (!m) return;
    /*
     * Total, like every other reset here: valid on a poisoned stack object.
     * Zero is also the honest power-on value for the one register whose
     * meaning is known -- reset has NOT completed until it is asked for, and
     * a device that reported completion before the request would let the
     * driver past a handshake that never happened.
     */
    memset(m, 0, sizeof *m);
}

uint32_t s5l_mbx_read(s5l_mbx_t *m, uint32_t off) {
    if (!m || off >= S5L_MBX_SIZE) return 0;

    if (off == S5L_MBX_RESET) {
        /*
         * The acknowledgement. Bit 16 reads set only once the reset has been
         * requested, so the driver's spin ends because of something it did,
         * not because of a constant this file chose.
         */
        uint32_t v = m->reg[off / 4u];
        return m->reset_done ? (v | S5L_MBX_RESET_DONE) : v;
    }
    return m->reg[off / 4u];
}

void s5l_mbx_write(s5l_mbx_t *m, uint32_t off, uint32_t val) {
    if (!m || off >= S5L_MBX_SIZE) return;

    m->reg[off / 4u] = val;

    /*
     * Bit 0 is the request, and it is treated as the trigger because it is
     * the bit the driver sets: it writes the literal 1. A write that does NOT
     * carry bit 0 deliberately leaves the acknowledgement alone rather than
     * clearing it -- nothing observed shows the driver taking reset back, and
     * inventing a way to un-complete a reset would be a behaviour with no
     * evidence behind it.
     */
    if (off == S5L_MBX_RESET && (val & 1u)) m->reset_done = true;
}
