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

/*
 * THE RASTERISER, which is where a frame that is not lockdownd's actually goes.
 *
 * The two sites above were chosen from a whole-boot call count and did not
 * survive being measured inside a frame. These three were chosen the other way
 * round: from r212/r213, which bucketed one home-screen swipe by 4K page and
 * attributed every sample to an address space. Three pages in SpringBoard's
 * space (ttbr0 0x0bf1b000) hold 39.8% of all user samples in that window --
 *
 *     19.6%   0x3122b000    sw_sample_nearest_BGRA8
 *     13.7%   0x3122d000    sw_scanline
 *      6.5%   0x311e2000    ogl_poly_scan
 *
 * -- and the profile named them before anything was known about who they
 * belonged to, which is the corroboration: the address-space scan independently
 * returned "SpringBoard" for a process already measured to be rasterising.
 *
 * This is the shape the header's opening paragraph describes. The MBX2D exists
 * so a real iPhone never runs these on its CPU; CA_ENABLE_MBX2D=0 forces the
 * software path this VM has no GPU for, and the interpreter then walks it one
 * guest instruction per pixel.
 *
 * THEY ARE OBSERVE, and the order-of-work rule in the header is the reason.
 * 39.8% of a profile is what a sampler attributes to three pages; it is not the
 * same number as "calls times cost", and the CGBlt sites are the standing proof
 * that those two can disagree by enough to reverse a decision. What these buy
 * first is a call count and an argument shape taken at the site itself. Only
 * then is there a basis for replacing one, and a replacement must additionally
 * satisfy contract items 4 and 6 -- every pixel touched through the guest MMU,
 * and a framebuffer diff against the same run with the site disarmed.
 *
 * The first three prologue words are IDENTICAL for all three of these AND for
 * _CGSFillDRAM8by1 above, because they are the same compiler's frame setup. The
 * gate is therefore five words deep, not three: the fourth word is where they
 * separate (a shift, a one-register vpush, and a five-register vpush).
 */
static const uint32_t PROLOGUE_SW_SAMPLE_NEAREST_BGRA8[] = {
    0xe92d40f0u,   /* push  {r4, r5, r6, r7, lr} */
    0xe28d700cu,   /* add   r7, sp, #0xc         */
    0xe92d0d00u,   /* push  {r8, sl, fp}         */
    0xe1a01181u,   /* lsl   r1, r1, #3           */
    0xe0812003u,   /* add   r2, r1, r3           */
};
static const uint32_t PROLOGUE_SW_SCANLINE[] = {
    0xe92d40f0u,   /* push  {r4, r5, r6, r7, lr} */
    0xe28d700cu,   /* add   r7, sp, #0xc         */
    0xe92d0d00u,   /* push  {r8, sl, fp}         */
    0xed2d8b02u,   /* vpush {d8}                 */
    0xe24dd060u,   /* sub   sp, sp, #0x60        */
};
static const uint32_t PROLOGUE_OGL_POLY_SCAN[] = {
    0xe92d40f0u,   /* push  {r4, r5, r6, r7, lr}   */
    0xe28d700cu,   /* add   r7, sp, #0xc           */
    0xe92d0d00u,   /* push  {r8, sl, fp}           */
    0xed2d8b0au,   /* vpush {d8, d9, d10, d11, d12} */
    0xe24ddfb9u,   /* sub   sp, sp, #0x2e4         */
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
    { "sw_sample_nearest_BGRA8", 0x3122b8bcu,
      PROLOGUE_SW_SAMPLE_NEAREST_BGRA8,
      (unsigned)(sizeof PROLOGUE_SW_SAMPLE_NEAREST_BGRA8 /
                 sizeof PROLOGUE_SW_SAMPLE_NEAREST_BGRA8[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "sw_scanline",        0x3122d180u, PROLOGUE_SW_SCANLINE,
      (unsigned)(sizeof PROLOGUE_SW_SCANLINE /
                 sizeof PROLOGUE_SW_SCANLINE[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "ogl_poly_scan",      0x311e2100u, PROLOGUE_OGL_POLY_SCAN,
      (unsigned)(sizeof PROLOGUE_OGL_POLY_SCAN /
                 sizeof PROLOGUE_OGL_POLY_SCAN[0]),
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
