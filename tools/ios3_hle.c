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
 * PROLOGUES ARE EMPTY UNTIL READ OUT OF THE CACHE. A prologue invented from
 * what a compiler "would" emit is not an identity check, it is a guess that
 * fails open the first time it is wrong. A site with prologue_n == 0 refuses
 * to arm; filling these in requires disassembling the extracted cache at each
 * address, which is a deliberate separate step.
 */

/* CoreGraphics, the leaf fill. run59 counted CGBlt_fillBytes 61,289 times in
 * one run, and it is the sole caller of _CGSFillDRAM8by1 -- a tight
 * `stm fp!, {r1,r2,r3,r4,r5,r6,r8,sl}` loop moving 32 bytes per instruction.
 * Leaf, pure, explicit src/dst/length: the easiest thing here to verify, and
 * the reason it is first. */
static const uint32_t PROLOGUE_CGBLT_FILLBYTES[] = { 0 };
static const uint32_t PROLOGUE_CGSFILLDRAM8BY1[] = { 0 };

static ios3_hle_site_t g_sites[] = {
    { "CGBlt_fillBytes",    0x338f61b0u, PROLOGUE_CGBLT_FILLBYTES,  0u,
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "_CGSFillDRAM8by1",   0x338f63e8u, PROLOGUE_CGSFILLDRAM8BY1, 0u,
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
