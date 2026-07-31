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
/*
 * sw_sample_nearest_BGRA8, FULLY DECODED. All 64 instructions, so the native
 * replacement is now transcription rather than reverse engineering.
 *
 * C++ signature, demangled:
 *   sw_sample_nearest_BGRA8(SWTexture *tex, int unit, int unused,
 *                           const ogl_poly_vert *start,
 *                           const ogl_poly_vert *delta,
 *                           const ogl_poly_vert *, const ogl_poly_vert *,
 *                           unsigned count, unsigned *out)
 *
 * `unit` is a TEXTURE UNIT INDEX, not a vertex index: the code computes
 * unit*8 and adds it to a vert pointer before reading +0x20/+0x24, so a vert
 * carries an ARRAY of (u,v) float pairs at +0x20 with an 8-byte stride. Args
 * 3, 6 and 7 are never read. This was the one thing that could not be guessed
 * from the signature and is why the shape looked wrong at first.
 *
 * SWTexture, from the four loads at entry:
 *   +0x00  uint8_t *base      pixel data
 *   +0x04  uint32_t pitch     BYTES per row (used as base + pitch*row)
 *   +0x08  int32_t  max_x     clamp, 16.16 fixed point
 *   +0x0c  int32_t  max_y     clamp, 16.16 fixed point
 *
 * ogl_poly_vert, only the fields this routine touches:
 *   +0x0c  float    w         perspective divisor
 *   +0x20  float[2] uv[unit]  texture coordinates, 8-byte stride
 *
 * Constants, read from the literal pool rather than assumed:
 *   0x3122b9c4 = 65536.0f     float -> 16.16 fixed point
 *   0x3122b9c8 = 1.0f         numerator of the reciprocal
 *
 * The whole routine, with u/v as 16.16 accumulators and w as a float:
 *
 *   u  = (int)(start->uv[unit][0] * 65536.0f);
 *   v  = (int)(start->uv[unit][1] * 65536.0f);
 *   du = (int)(delta->uv[unit][0] * 65536.0f);
 *   dv = (int)(delta->uv[unit][1] * 65536.0f);
 *   w  = start->w;  dw = delta->w;
 *   for (k = 0; k < count; k++) {
 *       float inv = 1.0f / w;
 *       int x = (int)((float)u * inv);
 *       int y = (int)((float)v * inv);
 *       x = x < 0 ? 0 : x;                  // bic rd, rn, rn asr #31
 *       y = y < 0 ? 0 : y;
 *       if (y >= max_y) y = max_y;          // movge, so CLAMPS AT max_y
 *       if (x >= max_x) x = max_x;          // and this one clamps too
 *       out[k] = *(uint32_t *)(base + pitch * (y >> 16) + ((x >> 16) << 2));
 *       u += du;  v += dv;  w += dw;
 *   }
 *
 * PERSPECTIVE-CORRECT, which is the part that must not be simplified away:
 * the divide is per pixel, not per span, so dropping it would drift on any
 * transformed layer. The clamps saturate AT the limit rather than one below
 * it, and the negative clamp is `bic rd, rn, rn asr #31`, which is
 * "0 if negative else unchanged" -- not an absolute value.
 *
 * WHAT REMAINS BEFORE THIS CAN BE REPLACE. Three things, none of them
 * unknowns: read every operand through the guest MMU page by page (contract
 * item 4), decline rather than fault when a page is not mapped (item 5), and
 * diff the framebuffer against the same run with the site disarmed (item 6).
 * The rounding is float-to-int truncation on both axes, which C's cast gives
 * exactly, so there is no rounding mode to match.
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

/*
 * MBX2D, WHICH IS A BETTER PLACE TO STAND THAN THE RASTERISER.
 *
 * The three sites above are the software rasteriser, and they were chosen
 * because a profile said they were expensive. They are also the WRONG LAYER,
 * and sizing the remaining two said so: sw_sample_nearest_BGRA8 transcribed in
 * 64 instructions, but sw_scanline is multi-path with a dozen context fields,
 * and ogl_poly_scan is ~3 KB with six calls out and a literal pool in the
 * middle. Neither is a transcription. Continuing down that road means hand-
 * porting a float rasteriser and diffing it pixel by pixel.
 *
 * The rasteriser only runs at all because CA_ENABLE_MBX2D=0 forces it. A real
 * iPhone composites through MBX2D, and THAT interface is small: the whole 2D
 * API is a handful of state setters and two operations. Measured from the
 * cache's own symbol table -- mbx2DCtxSetSourceSurface is 5 instructions,
 * SetScissor 7, SetDestinationSurface 7, and the two blits dispatch through a
 * function pointer in the context. So the same technique applied one layer up
 * replaces ALL of the rasteriser rather than a fraction of it, and does so at
 * an interface with names and an argument shape instead of a register file.
 *
 * WHAT THE DECODE ESTABLISHED, all of it read rather than assumed:
 *
 *   mbx2DCtxBlitColor  ldr ip,[r0,#0x5c] ; bx ip   -> ctx->fill_dispatch
 *   mbx2DCtxBlitCopy   ldr ip,[r0,#0x60] ; bx ip   -> ctx->copy_dispatch
 *
 * Both are two-instruction thunks, and the global-context entry points
 * (mbx2DBlitColor, mbx2DBlitCopy) reach the work by calling them, so one site
 * on each thunk catches every caller of either form. The dispatch targets are
 * installed by generateProfile() and on EVT2 they route into the 3D core --
 * mbx3DCtxBlitCopy alone is 890 instructions -- which is the size of what
 * standing here skips.
 *
 * mbx2DCtxInitialize allocates a 124-byte context, and its FIRST act is
 * `r4 = mbxConnectionOpen(); if (r4) return NULL`. That single call is why
 * mbx2DGlobalContext is NULL in this VM and why SpringBoard dies in
 * _mbx2DDisable: no connection, no context, no hardware path. It is included
 * here so the connection's behaviour is observed rather than guessed at.
 *
 * THESE ARE OBSERVE, under the same order-of-work rule as everything above.
 * What they buy first is a call count and an argument shape taken at the site
 * itself -- whether the connection is even attempted, whether it succeeds now
 * that the kext attaches, and whether any blit follows. A replacement needs
 * that evidence first, and then still has to satisfy contract items 4 and 6:
 * every pixel through the guest MMU, and a framebuffer diff against the same
 * run with the site disarmed.
 */
static const uint32_t PROLOGUE_MBX_CONNECTION_OPEN[] = {
    0xe92d40f0u,   /* push {r4, r5, r6, r7, lr}    */
    0xe28d700cu,   /* add  r7, sp, #0xc            */
    0xe92d0500u,   /* push {r8, sl}                */
    0xe24dd014u,   /* sub  sp, sp, #0x14           */
    0xe59f027cu,   /* ldr  r0, [pc, #0x27c]        */
};

/*
 * Two instructions is the WHOLE function for both thunks, which collides with
 * the four-word minimum the test enforces -- and the test is right to enforce
 * it, so the prologue is extended rather than the rule relaxed.
 *
 * Words three and four are therefore the opening of the NEXT function
 * (mbx2DBlitColor at +0x8, mbx2DBlitCopy at +0x8). That is deliberate and it
 * makes the check STRONGER, not weaker: the claim being verified becomes "this
 * thunk, immediately followed by that function's frame setup", which is a
 * statement about the layout of the image and not just about two words. The
 * bytes are as fixed as any others here -- the shared cache maps at a constant
 * address and is never written.
 *
 * The two sites remain distinguishable despite sharing words three and four,
 * because word ONE is the context offset the thunk loads -- 0x5c for the fill
 * dispatch, 0x60 for the copy dispatch -- and that is what a mixed-up pair
 * would have to get past.
 */
static const uint32_t PROLOGUE_MBX2D_CTX_BLIT_COLOR[] = {
    0xe590c05cu,   /* ldr  ip, [r0, #0x5c]         */
    0xe12fff1cu,   /* bx   ip                      */
    0xe92d4080u,   /* push {r7, lr}   (mbx2DBlitColor) */
    0xe28d7000u,   /* add  r7, sp, #0              */
};

static const uint32_t PROLOGUE_MBX2D_CTX_BLIT_COPY[] = {
    0xe590c060u,   /* ldr  ip, [r0, #0x60]         */
    0xe12fff1cu,   /* bx   ip                      */
    0xe92d4080u,   /* push {r7, lr}   (mbx2DBlitCopy)  */
    0xe28d7000u,   /* add  r7, sp, #0              */
};

static const uint32_t PROLOGUE_MBX2D_CTX_INITIALIZE[] = {
    0xe92d40f0u,   /* push {r4, r5, r6, r7, lr}    */
    0xe28d700cu,   /* add  r7, sp, #0xc            */
    0xe3a0007cu,   /* mov  r0, #0x7c  (sizeof ctx) */
    0xeb0016bcu,   /* bl   operator new            */
    0xe1a05000u,   /* mov  r5, r0                  */
};

/*
 * The native sw_sample_nearest_BGRA8, transcribed from the decode above.
 *
 * ATTACHED BUT NOT ARMED. The site below stays IOS3_HLE_OBSERVE, so
 * ios3_hle_step never calls this: contract item 6 requires a framebuffer diff
 * against the same run with the site disarmed BEFORE a replacement may draw,
 * and that diff has not been run. Shipping the code and arming it are two
 * decisions, and this is only the first.
 *
 * Every guest access goes through mem->read / mem->write, which translate one
 * page at a time, unprivileged. Any refusal returns false for the WHOLE call
 * so the guest runs its own code -- contract item 5. Declining is always safe;
 * a half-written span is not.
 */
static bool read_u32(const ios3_hle_mem_t *mem, uint32_t va, uint32_t *out) {
    return mem->read(mem->ctx, va, out, 4u);
}

static bool read_f32(const ios3_hle_mem_t *mem, uint32_t va, float *out) {
    uint32_t bits;
    if (!mem->read(mem->ctx, va, &bits, 4u)) return false;
    memcpy(out, &bits, sizeof bits);
    return true;
}

static bool hle_sw_sample_nearest_bgra8(arm_cpu_t *cpu,
                                        const ios3_hle_mem_t *mem) {
    uint32_t tex = cpu->r[0], unit = cpu->r[1], start = cpu->r[3];
    uint32_t sp = cpu->r[13];
    uint32_t delta = 0, count = 0, out = 0;
    uint32_t base = 0, pitch = 0;
    uint32_t max_x = 0, max_y = 0;
    float w = 0.0f, dw = 0.0f;
    float su = 0.0f, sv = 0.0f, du_f = 0.0f, dv_f = 0.0f;
    int32_t u, v, du, dv;

    if (!mem || !mem->read || !mem->write) return false;

    /* AAPCS: args 5-9 are at the entry sp, which is where this fires. */
    if (!read_u32(mem, sp + 0x00u, &delta) ||
        !read_u32(mem, sp + 0x0cu, &count) ||
        !read_u32(mem, sp + 0x10u, &out))
        return false;

    if (!read_u32(mem, tex + 0x00u, &base)  ||
        !read_u32(mem, tex + 0x04u, &pitch) ||
        !read_u32(mem, tex + 0x08u, &max_x) ||
        !read_u32(mem, tex + 0x0cu, &max_y))
        return false;

    if (!read_f32(mem, start + 0x0cu, &w)  ||
        !read_f32(mem, delta + 0x0cu, &dw) ||
        !read_f32(mem, start + 0x20u + unit * 8u,        &su) ||
        !read_f32(mem, start + 0x20u + unit * 8u + 4u,   &sv) ||
        !read_f32(mem, delta + 0x20u + unit * 8u,        &du_f) ||
        !read_f32(mem, delta + 0x20u + unit * 8u + 4u,   &dv_f))
        return false;

    u  = (int32_t)(su   * 65536.0f);
    v  = (int32_t)(sv   * 65536.0f);
    du = (int32_t)(du_f * 65536.0f);
    dv = (int32_t)(dv_f * 65536.0f);

    for (uint32_t k = 0; k < count; k++) {
        float inv = 1.0f / w;
        int32_t x = (int32_t)((float)u * inv);
        int32_t y = (int32_t)((float)v * inv);
        uint32_t texel = 0, addr;

        /* `bic rd, rn, rn asr #31` is "zero if negative", not abs(). */
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        /* movge, so these saturate AT the limit rather than one below it. */
        if ((uint32_t)y >= max_y) y = (int32_t)max_y;
        if ((uint32_t)x >= max_x) x = (int32_t)max_x;

        addr = base + pitch * (uint32_t)(y >> 16) +
               (((uint32_t)(x >> 16)) << 2);
        if (!read_u32(mem, addr, &texel)) return false;
        if (!mem->write(mem->ctx, out + k * 4u, &texel, 4u)) return false;

        u += du; v += dv; w += dw;
    }

    /* Returned to LR without executing the body, which is what makes the
     * caller skip the instruction. r0-r3 are caller-saved and the guest's
     * own routine returns nothing, so nothing else needs restoring. */
    cpu->r[15] = cpu->r[14];
    return true;
}

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
      hle_sw_sample_nearest_bgra8, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "sw_scanline",        0x3122d180u, PROLOGUE_SW_SCANLINE,
      (unsigned)(sizeof PROLOGUE_SW_SCANLINE /
                 sizeof PROLOGUE_SW_SCANLINE[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "ogl_poly_scan",      0x311e2100u, PROLOGUE_OGL_POLY_SCAN,
      (unsigned)(sizeof PROLOGUE_OGL_POLY_SCAN /
                 sizeof PROLOGUE_OGL_POLY_SCAN[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "mbxConnectionOpen",  0x30e1fc90u, PROLOGUE_MBX_CONNECTION_OPEN,
      (unsigned)(sizeof PROLOGUE_MBX_CONNECTION_OPEN /
                 sizeof PROLOGUE_MBX_CONNECTION_OPEN[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "mbx2DCtxInitialize", 0x30e1abe4u, PROLOGUE_MBX2D_CTX_INITIALIZE,
      (unsigned)(sizeof PROLOGUE_MBX2D_CTX_INITIALIZE /
                 sizeof PROLOGUE_MBX2D_CTX_INITIALIZE[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "mbx2DCtxBlitColor",  0x30e1ad6cu, PROLOGUE_MBX2D_CTX_BLIT_COLOR,
      (unsigned)(sizeof PROLOGUE_MBX2D_CTX_BLIT_COLOR /
                 sizeof PROLOGUE_MBX2D_CTX_BLIT_COLOR[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "mbx2DCtxBlitCopy",   0x30e1adc0u, PROLOGUE_MBX2D_CTX_BLIT_COPY,
      (unsigned)(sizeof PROLOGUE_MBX2D_CTX_BLIT_COPY /
                 sizeof PROLOGUE_MBX2D_CTX_BLIT_COPY[0]),
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
