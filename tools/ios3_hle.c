/*
 * S5LBox — high-level emulation of iPhone OS 3 userspace routines.
 * See ios3_hle.h for the contract every site has to satisfy.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "ios3_hle.h"

#include <string.h>

/* --------------------------------------------------------------- the sites --- */

/*
 * Addresses come from the resolved shared-cache map already in this tree
 * (docs/BOOTLOG.md, tools/dscmap.py). They are absolute and un-slid: iPhone
 * OS 3's dyld shared cache maps at a fixed address in every process, which is
 * what makes PC interception viable at all and also why the address-space
 * check in ios3_hle_step() is not optional.
 *
 * A prologue invented from what a compiler "would" emit is not an identity
 * check, it is a guess that fails open the first time it is wrong. A site with
 * prologue_n == 0 refuses to arm.
 *
 * READ OUT OF THE CACHE on 2026-07-30, which is what these sites were waiting
 * for. Both addresses resolve to a symbol at `va is +0x0` -- an exact function
 * start, not an address that merely landed inside one:
 *
 *   python tools/hfsx_extract.py <work.img> dyld_shared_cache_armv6 <cache>
 *   python tools/dscmap.py <cache> 0x338f61b0 --count 10   -> _CGBlt_fillBytes
 *   python tools/dscmap.py <cache> 0x338f63e8 --count 10   -> _CGSFillDRAM8by1
 *
 * FOUR words each, not two: both functions open with the identical
 * `push {r4,r5,r6,r7,lr}; add r7,sp,#0xc`, so a two-word prologue would match
 * either site -- and a check that cannot tell two neighbouring functions apart
 * is not an identity check. They first differ at word three.
 */

/*
 * CoreGraphics, the leaf fill. It is the sole caller of _CGSFillDRAM8by1 -- a
 * tight `stm fp!, {r1,r2,r3,r4,r5,r6,r8,sl}` loop moving 32 bytes per
 * instruction. Leaf, pure, explicit src/dst/length: the easiest thing here to
 * verify, and the reason it is first.
 *
 * THE CALL COUNT THAT MADE IT LOOK URGENT DOES NOT SURVIVE. run59's 61,289
 * calls were a whole cold boot; run168 measured the window that actually
 * decides frame rate -- 75 composites over 1.18 G instructions -- and counted
 * CGBlt_fillBytes just 43 times in it. So replacing this is NOT an fps win on
 * the evidence available, and these sites stay IOS3_HLE_OBSERVE. What the
 * recorded prologues buy is that they can now arm and COUNT, which turns a
 * dead site into instrumentation that can confirm or refute run168 at the
 * site itself rather than through a profiler bucket.
 */
static const uint32_t PROLOGUE_CGBLT_FILLBYTES[] = {
    0xe92d40f0u,   /* push {r4, r5, r6, r7, lr} */
    0xe28d700cu,   /* add  r7, sp, #0xc         */
    0xe24dd008u,   /* sub  sp, sp, #8           */
    0xe3510000u,   /* cmp  r1, #0               */
};
static const uint32_t PROLOGUE_CGSFILLDRAM8BY1[] = {
    0xe92d40f0u,   /* push {r4, r5, r6, r7, lr} */
    0xe28d700cu,   /* add  r7, sp, #0xc         */
    0xe92d0d00u,   /* push {r8, sl, fp}         */
    0xe24dd010u,   /* sub  sp, sp, #0x10        */
};

static ios3_hle_site_t g_sites[] = {
    { "CGBlt_fillBytes",    0x338f61b0u, PROLOGUE_CGBLT_FILLBYTES,
      (unsigned)(sizeof PROLOGUE_CGBLT_FILLBYTES /
                 sizeof PROLOGUE_CGBLT_FILLBYTES[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "_CGSFillDRAM8by1",   0x338f63e8u, PROLOGUE_CGSFILLDRAM8BY1,
      (unsigned)(sizeof PROLOGUE_CGSFILLDRAM8BY1 /
                 sizeof PROLOGUE_CGSFILLDRAM8BY1[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
};

#define SITE_N ((unsigned)(sizeof g_sites / sizeof g_sites[0]))

static uint32_t g_ttbr0;      /* the address space sites are restricted to */
static unsigned g_armed_n;    /* hot-path gate: zero means do nothing      */

unsigned ios3_hle_site_count(void) { return SITE_N; }

ios3_hle_site_t *ios3_hle_site_at(unsigned i) {
    return i < SITE_N ? &g_sites[i] : NULL;
}

/* ---------------------------------------------------------------- arming --- */

/*
 * A site arms only if every recorded prologue word is present at its address.
 *
 * Fails closed in three separate ways, because each is a different mistake:
 * no recorded prologue at all (nobody has read the bytes yet), a read that
 * faults (the cache is not mapped in this space), and a mismatch (this is not
 * the build these addresses describe).
 */
static bool identity_ok(const ios3_hle_mem_t *mem, ios3_hle_site_t *s) {
    if (!mem || !mem->read) return false;
    if (!s->prologue || s->prologue_n == 0u) return false;
    for (unsigned i = 0; i < s->prologue_n; i++) {
        uint32_t got = 0;
        if (!mem->read(mem->ctx, s->va + i * 4u, &got, 4u)) return false;
        if (got != s->prologue[i]) return false;
    }
    return true;
}

unsigned ios3_hle_arm(const ios3_hle_mem_t *mem, uint32_t ttbr0) {
    g_ttbr0 = ttbr0;
    g_armed_n = 0u;
    for (unsigned i = 0; i < SITE_N; i++) {
        ios3_hle_site_t *s = &g_sites[i];
        s->armed = identity_ok(mem, s);
        s->identity_failed = !s->armed;
        if (s->armed) g_armed_n++;
    }
    return g_armed_n;
}

void ios3_hle_disarm(void) {
    for (unsigned i = 0; i < SITE_N; i++) g_sites[i].armed = false;
    g_armed_n = 0u;
}

/* -------------------------------------------------------------- the step --- */

bool ios3_hle_step(arm_cpu_t *cpu, const ios3_hle_mem_t *mem, uint32_t pc,
                   uint32_t ttbr0) {
    if (!g_armed_n || !cpu) return false;

    ios3_hle_site_t *s = NULL;
    for (unsigned i = 0; i < SITE_N; i++)
        if (g_sites[i].armed && g_sites[i].va == pc) { s = &g_sites[i]; break; }
    if (!s) return false;

    /*
     * THE ADDRESS-SPACE GATE, and it is counted rather than silently skipped.
     * A shared-cache address is present in every process, so a site reached in
     * the wrong one is not a near miss -- it is another program about to have
     * its function replaced. Recording it separately is what makes "aimed at
     * the wrong process" distinguishable from "never reached".
     */
    if (g_ttbr0 && ttbr0 != g_ttbr0) { s->wrong_space++; return false; }

    s->hits++;
    if (s->mode != IOS3_HLE_REPLACE || !s->handler) return false;

    if (!s->handler(cpu, mem)) { s->declined++; return false; }
    s->handled++;
    return true;
}
