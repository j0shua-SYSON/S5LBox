/*
 * S5LBox — high-level emulation of iPhone OS 3 userspace routines.
 * See ios3_hle.h for the contract every site has to satisfy.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "ios3_hle.h"

#include <stdio.h>
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
static const uint32_t PROLOGUE_SW_SAMPLE_COLOR[] = {
    0xe92d40f0u,   /* push  {r4, r5, r6, r7, lr}   */
    0xe28d700cu,   /* add   r7, sp, #0xc           */
    0xe92d0d00u,   /* push  {r8, sl, fp}           */
    0xed9f7a4du,   /* vldr  s14, [pc, #0x134]      */
    0xedd17a04u,   /* vldr  s15, [r1, #0x10]       */
};

/*
 * THE REST OF THE SAMPLER FAMILY, AS OBSERVE FIRST -- and sw_sample_color is
 * why. It was transcribed on the strength of a PAGE profile (13.7% on
 * 0x3122d000) and r261 then counted it ZERO times in a whole boot: the page's
 * cost is entirely sw_scanline, which shares it. The transcription is correct
 * and worth nothing, and with no calls the framebuffer diff never exercised it
 * either, so it goes back to OBSERVE rather than shipping on no evidence.
 *
 * So these three get counted before anyone transcribes them. sw_sample_texture
 * is included even though it is a DISPATCHER rather than a sampler -- it
 * computes |du|+|dv| and picks a path -- because its count says how often the
 * family is entered at all.
 */
static const uint32_t PROLOGUE_SW_SAMPLE_TEXTURE[] = {
    0xe92d40f0u,   /* push {r4, r5, r6, r7, lr}    */
    0xe28d700cu,   /* add  r7, sp, #0xc            */
    0xe92d0d00u,   /* push {r8, sl, fp}            */
    0xe24dd004u,   /* sub  sp, sp, #4              */
    0xe1a08003u,   /* mov  r8, r3                  */
};
static const uint32_t PROLOGUE_SW_SAMPLE_CIRCLE[] = {
    0xe92d4080u,   /* push {r7, lr}                */
    0xe28d7000u,   /* add  r7, sp, #0              */
    0xe1a01181u,   /* lsl  r1, r1, #3              */
    0xe0812003u,   /* add  r2, r1, r3              */
    0xe59de008u,   /* ldr  lr, [sp, #8]            */
};
static const uint32_t PROLOGUE_SW_SAMPLE_SQUARE[] = {
    0xe92d4080u,   /* push {r7, lr}                */
    0xe28d7000u,   /* add  r7, sp, #0              */
    0xed2d8b02u,   /* vpush {d8}                   */
    0xe1a01181u,   /* lsl  r1, r1, #3              */
    0xe0812003u,   /* add  r2, r1, r3              */
};

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

static bool read_u16(const ios3_hle_mem_t *mem, uint32_t va, uint16_t *out) {
    return mem->read(mem->ctx, va, out, 2u);
}

static bool read_f32(const ios3_hle_mem_t *mem, uint32_t va, float *out) {
    uint32_t bits;
    if (!mem->read(mem->ctx, va, &bits, 4u)) return false;
    memcpy(out, &bits, sizeof bits);
    return true;
}

/*
 * sw_sample_color, FULLY DECODED. All 85 instructions.
 *
 * CA::OGL::sw_sample_color(unsigned, const ogl_poly_vert *start,
 *                          const ogl_poly_vert *delta, unsigned count,
 *                          unsigned *out)
 *
 * The first argument is never read. Arguments 1-4 are r0-r3 and `out` is the
 * fifth, which at entry is [sp+0]: the function's own [sp,#0x20] references are
 * AFTER it pushes 0x20 bytes, and this fires before any of that has happened.
 *
 * A sibling of the sampler above and the same span shape -- walk `count`
 * pixels, divide by w per pixel, write one word each -- but it interpolates a
 * COLOUR rather than reading a texture, so no memory is read inside the loop at
 * all. It sits at 0x3122d02c, inside the 0x3122d000 page the profile put at
 * 13.7% of a swipe.
 *
 * ogl_poly_vert, the fields this one uses:
 *   +0x0c  float w         perspective divisor
 *   +0x10  float c[4]      colour components, in the order R, G, B, A
 *
 * Constants read from the literal pool rather than assumed:
 *   0x3122d174 = 0x4b7f0000 = 16711680.0f   which is 255.0f * 65536.0f
 *   0x3122d178 = 0x3f800000 = 1.0f          numerator of the reciprocal
 *   0x3122d17c = 0xffff0000                 the packing mask
 *
 * So a component is carried as 16.16 fixed point scaled to 0..255, and the
 * pack drops each one's fraction:
 *
 *   px = (r & 0xffff0000) | (b >> 16) | ((a >> 16) << 24) | ((g >> 16) << 8)
 *
 * which is 0xAARRGGBB -- the driver's own 'ARGB', matching what clcd.c decodes
 * a 32-bit window as. The R term is MASKED rather than shifted because its
 * integer part already sits at bits 16-23.
 *
 * THE SHIFTS ARE ARITHMETIC AND THERE IS NO CLAMP, which is the one thing that
 * must not be tidied up. The sampler above saturates with `bic`/`movge`; this
 * routine does not, so a component that goes negative sign-extends into the
 * neighbouring channel and that is the behaviour to reproduce exactly. Writing
 * the "obviously intended" clamp here would produce a different pixel from the
 * guest's on any span that overshoots, which is precisely what the framebuffer
 * diff would then catch -- and it is cheaper to be right than to be caught.
 */
static bool hle_sw_sample_color(arm_cpu_t *cpu, const ios3_hle_mem_t *mem) {
    const float K = 16711680.0f;          /* 255.0f * 65536.0f */
    uint32_t start = cpu->r[1], delta = cpu->r[2], count = cpu->r[3];
    uint32_t sp = cpu->r[13];
    uint32_t out = 0;
    float w = 0.0f, dw = 0.0f;
    float sc[4], dc[4];
    int32_t c[4], d[4];
    unsigned i;

    if (!mem || !mem->read || !mem->write) return false;

    if (!read_u32(mem, sp + 0x00u, &out)) return false;

    if (!read_f32(mem, start + 0x0cu, &w) ||
        !read_f32(mem, delta + 0x0cu, &dw))
        return false;

    for (i = 0; i < 4u; i++) {
        if (!read_f32(mem, start + 0x10u + i * 4u, &sc[i]) ||
            !read_f32(mem, delta + 0x10u + i * 4u, &dc[i]))
            return false;
        c[i] = (int32_t)(sc[i] * K);
        d[i] = (int32_t)(dc[i] * K);
    }

    for (uint32_t k = 0; k < count; k++) {
        float inv = 1.0f / w;
        int32_t r = (int32_t)((float)c[0] * inv);
        int32_t g = (int32_t)((float)c[1] * inv);
        int32_t b = (int32_t)((float)c[2] * inv);
        int32_t a = (int32_t)((float)c[3] * inv);
        uint32_t px;

        /* Exactly the guest's OR chain, arithmetic shifts included. */
        px  = ((uint32_t)r & 0xffff0000u) | (uint32_t)(b >> 16);
        px |= (uint32_t)(a >> 16) << 24;
        px |= (uint32_t)(g >> 16) << 8;

        if (!mem->write(mem->ctx, out + k * 4u, &px, 4u)) return false;

        c[0] += d[0]; c[1] += d[1]; c[2] += d[2]; c[3] += d[3];
        w += dw;
    }

    cpu->r[15] = cpu->r[14];
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

/*
 * THE ARGUMENT-SHAPE TRACERS. Each prints the first few calls and returns
 * false, which under IOS3_HLE_TRACE is discarded anyway -- the guest runs its
 * own body every time, so nothing here can change what the screen shows.
 *
 * The static analysis got as far as the CALL SHAPE and stops there.
 * CA::RenderMBX2D::blit_copy_simple builds six arguments out of coordinate
 * differences on its CA::Render::Data --
 *
 *   a0 = d->[0x78] - d->[0x50]      a3 = d->[0x7c] - d->[0x18]
 *   a1 = d->[0x7c] - d->[0x54]      a4 = d->[0x80]
 *   a2 = d->[0x78] - d->[0x14]      a5 = d->[0x84]
 *
 * -- which fixes how many arguments there are and that four of them are
 * differences of a right/bottom against a left/top. It does NOT say which pair
 * is the destination origin, which is the source origin, and which is the
 * extent, and guessing between those three is exactly how a blit ends up
 * transposed or offset in a way that still looks plausible. Real values
 * separate them immediately: an extent is small and positive, an origin tracks
 * where the layer is on screen, and a stride-like number is large.
 */
static unsigned g_trace_budget[8];
static uint32_t g_sw_ctx_seen[8];
static unsigned g_sw_ctx_seen_n;

static bool trace_args(const char *what, arm_cpu_t *cpu,
                       const ios3_hle_mem_t *mem, unsigned slot, unsigned nstack) {
    uint32_t sp = cpu->r[13];
    unsigned i;

    if (slot >= (unsigned)(sizeof g_trace_budget / sizeof g_trace_budget[0]))
        return false;
    if (g_trace_budget[slot] >= 12u) return false;
    g_trace_budget[slot]++;

    fprintf(stderr,
            "hle-trace %-20s at=%llu r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x",
            what, (unsigned long long)cpu->cycles,
            cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3], cpu->r[14]);
    for (i = 0; i < nstack; i++) {
        uint32_t w = 0;
        if (!read_u32(mem, sp + i * 4u, &w)) { fprintf(stderr, " sp+%u=??", i * 4u); continue; }
        fprintf(stderr, " sp+%u=%08x", i * 4u, w);
    }
    fprintf(stderr, "\n");
    return false;
}

static void trace_words(const ios3_hle_mem_t *mem, const char *what,
                        uint32_t base, const unsigned *off, unsigned n) {
    unsigned i;

    fprintf(stderr, "hle-trace   %s@%08x", what, base);
    for (i = 0; i < n; i++) {
        uint32_t w = 0;
        uint64_t va = (uint64_t)base + off[i];
        if (va <= UINT32_MAX && read_u32(mem, (uint32_t)va, &w))
            fprintf(stderr, " +%03x=%08x", off[i], w);
        else
            fprintf(stderr, " +%03x=??", off[i]);
    }
    fprintf(stderr, "\n");
}

static void trace_ogl_vert(const ios3_hle_mem_t *mem, const char *what,
                           uint32_t va) {
    float f[14];
    unsigned i;

    if (!va) {
        fprintf(stderr, "hle-trace   %s=NULL\n", what);
        return;
    }
    for (i = 0; i < 14u; i++) {
        uint64_t p = (uint64_t)va + i * 4u;
        if (p > UINT32_MAX || !read_f32(mem, (uint32_t)p, &f[i])) {
            fprintf(stderr, "hle-trace   %s@%08x=unreadable@+%02x\n",
                    what, va, i * 4u);
            return;
        }
    }
    fprintf(stderr,
            "hle-trace   %s@%08x xyzw=(%.9g,%.9g,%.9g,%.9g)"
            " rgba=(%.9g,%.9g,%.9g,%.9g)"
            " uv0=(%.9g,%.9g) uv1=(%.9g,%.9g) uv2=(%.9g,%.9g)\n",
            what, va,
            (double)f[0], (double)f[1], (double)f[2], (double)f[3],
            (double)f[4], (double)f[5], (double)f[6], (double)f[7],
            (double)f[8], (double)f[9], (double)f[10], (double)f[11],
            (double)f[12], (double)f[13]);
}

/*
 * The root trace proves which polygon enters the scan converter. This one
 * records what the converter hands to its only callback: x/y/count, the four
 * interpolant vectors, the active-field mask and the SW scan state. The first
 * twelve calls show the per-line evolution; each distinct context is dumped
 * once so a run with several texture units does not spend the whole budget on
 * the first one.
 */
static bool hle_trace_sw_scanline(arm_cpu_t *cpu,
                                  const ios3_hle_mem_t *mem) {
    static const unsigned CTX_OFF[] = {
        0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
        0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c,
        0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c,
        0x60, 0x64, 0x68
    };
    static const unsigned RENDER_OFF[] = {
        0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
        0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c,
        0xf8, 0xfc, 0x100, 0x104, 0x108, 0x10c, 0x110,
        0x114, 0x118, 0x11c
    };
    static const unsigned STATE_OFF[] = {
        0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
        0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c,
        0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c,
        0x60, 0x64, 0x68, 0x6c, 0x70, 0x74, 0x78, 0x7c
    };
    bool first = g_trace_budget[5] < 12u;
    uint32_t stack[5] = { 0, 0, 0, 0, 0 };
    uint32_t render = 0, state = 0;
    unsigned i;

    (void)trace_args("sw_scanline", cpu, mem, 5, 5);
    for (i = 0; i < 5u; i++) {
        uint64_t va = (uint64_t)cpu->r[13] + i * 4u;
        if (va <= UINT32_MAX)
            (void)read_u32(mem, (uint32_t)va, &stack[i]);
    }

    if (first) {
        trace_ogl_vert(mem, "scan.start", cpu->r[3]);
        trace_ogl_vert(mem, "scan.dx", stack[0]);
        trace_ogl_vert(mem, "scan.dy0", stack[1]);
        trace_ogl_vert(mem, "scan.dy1", stack[2]);
    }

    for (i = 0; i < g_sw_ctx_seen_n; i++)
        if (g_sw_ctx_seen[i] == stack[4]) return false;
    if (!stack[4] || g_sw_ctx_seen_n >=
            (unsigned)(sizeof g_sw_ctx_seen / sizeof g_sw_ctx_seen[0]))
        return false;
    g_sw_ctx_seen[g_sw_ctx_seen_n++] = stack[4];

    trace_words(mem, "scan.ctx", stack[4], CTX_OFF,
                (unsigned)(sizeof CTX_OFF / sizeof CTX_OFF[0]));
    if (read_u32(mem, stack[4], &render) && render) {
        trace_words(mem, "scan.render", render, RENDER_OFF,
                    (unsigned)(sizeof RENDER_OFF / sizeof RENDER_OFF[0]));
        if (render <= UINT32_MAX - 4u &&
            read_u32(mem, render + 4u, &state) && state)
            trace_words(mem, "scan.state", state, STATE_OFF,
                        (unsigned)(sizeof STATE_OFF / sizeof STATE_OFF[0]));
    }
    return false;
}

/*
 * THE RASTERISER ROOT, no longer a call-count inference.
 *
 * The 2026-08-01 cache walk found exactly one direct caller of ogl_poly_scan:
 * CA::OGL::SWContext::draw_elements at 0x3122cdbc. Immediately before the BL,
 * it stores pc+0x3d8 = 0x3122d180 at incoming sp+8. That address is the exact
 * start of sw_scanline. ogl_poly_scan later loads that seventh argument from
 * its frame and invokes it at 0x311e2c04 with `blx ip`; it is the root callback,
 * not an unrelated helper. sw_scanline in turn calls sw_sample_texture and
 * sw_sample_color, and sw_sample_texture tail-calls the selected texture
 * sampler through its SWTexture function pointer.
 *
 * Therefore a COMPLETE native ogl_poly_scan replacement would subsume the hot
 * scanline and sampler subtree. Merely returning from this site would skip the
 * drawing and is not a replacement. This TRACE records the ABI and bounded
 * polygon shape while the guest still performs every draw. The first argument
 * is a header followed by at most ten 0x38-byte vertices; that count limit and
 * the x/y/w fields below are read directly from ogl_poly_scan's entry code.
 */
static void trace_poly_vert(const ios3_hle_mem_t *mem, uint32_t poly,
                            unsigned index) {
    uint64_t wide = (uint64_t)poly + 4u + (uint64_t)index * 0x38u;
    float f[14];
    unsigned i;

    if (wide > (uint64_t)UINT32_MAX - 0x34u) {
        fprintf(stderr, "hle-trace   poly.v[%u]=unreadable\n", index);
        return;
    }
    for (i = 0; i < 14u; i++) {
        if (!read_f32(mem, (uint32_t)wide + i * 4u, &f[i])) {
            fprintf(stderr, "hle-trace   poly.v[%u]=unreadable@+%02x\n",
                    index, i * 4u);
            return;
        }
    }
    fprintf(stderr,
            "hle-trace   poly.v[%u] xyzw=(%.9g,%.9g,%.9g,%.9g)"
            " rgba=(%.9g,%.9g,%.9g,%.9g)"
            " uv0=(%.9g,%.9g) uv1=(%.9g,%.9g) uv2=(%.9g,%.9g)\n",
            index,
            (double)f[0], (double)f[1], (double)f[2], (double)f[3],
            (double)f[4], (double)f[5], (double)f[6], (double)f[7],
            (double)f[8], (double)f[9], (double)f[10], (double)f[11],
            (double)f[12], (double)f[13]);
}

static bool hle_trace_ogl_poly_scan(arm_cpu_t *cpu,
                                    const ios3_hle_mem_t *mem) {
    bool first = g_trace_budget[4] < 12u;
    uint16_t count = 0, flags = 0;

    (void)trace_args("ogl_poly_scan", cpu, mem, 4, 4);
    if (!first) return false;

    if (cpu->r[0] > UINT32_MAX - 2u ||
        !read_u16(mem, cpu->r[0] + 0u, &count) ||
        !read_u16(mem, cpu->r[0] + 2u, &flags)) {
        fprintf(stderr, "hle-trace   poly.header=unreadable\n");
        return false;
    }
    fprintf(stderr, "hle-trace   poly.count=%u flags=%04x\n",
            (unsigned)count, (unsigned)flags);
    if (count > 0u && count <= 10u) {
        unsigned i;
        for (i = 0; i < (unsigned)count; i++)
            trace_poly_vert(mem, cpu->r[0], i);
    }
    return false;
}

/*
 * THE CONTEXT DUMP, which is the other half of what a native blit needs.
 *
 * r247 settled the argument shape from the values alone: the first call is
 * (0,0)->(0,0) 0x140 x 0x1e0, which is 320x480, the whole screen; a later one is
 * 320x96 at y=384, which is the dock. So the operation is
 *
 *   mbx2DCtxBlitCopy(ctx, srcX, srcY, dstX, dstY, width, height)
 *
 * and that agrees with how CA::RenderMBX2D::blit_copy_simple builds its six
 * arguments, with d->[0x80] and d->[0x84] as the extent. Source origin is always
 * (0,0) because each layer sets its own surface first.
 *
 * What the arguments do NOT say is where the pixels ARE. That lives in the
 * context, whose layout is read out of the setters:
 *
 *   ctx+0x00/04/08/0c   source surface      (SetSourceSurface's r1,r2,r3,byte)
 *   ctx+0x10/14/18/1c   destination surface (SetDestinationSurface, same shape)
 *   ctx+0x38/3c/40/44   scissor x,y,w,h
 *   ctx+0x4c/50/54      scaleX, scaleY (1.0f at init), "not unity" flag
 *   ctx+0x5c/60         fill and copy dispatch pointers
 *
 * Two of those words per surface are a base and a stride and one is a format
 * (it is compared against 0x68000 and 0x70000 in copyDispatchEVT2), but WHICH is
 * which cannot be read off the setters -- they only store. Printing the live
 * context at the moment of a blit answers it the same way the argument values
 * did: a base address is a large aligned number in the guest's address space, a
 * stride is a small multiple of the width, and a format is one of two constants.
 */
static void trace_ctx(const ios3_hle_mem_t *mem, uint32_t ctx) {
    /*
     * +0x20 IS THE BLEND EQUATION, NOT AN ADDRESS. The first reading of r248
     * took 0x090ff000 for a framebuffer base because it looks like one and sits
     * in the guest's DRAM range. mbx2DCtxSetBlendEquation says otherwise: it
     * stores `dst_factor | src_factor | (alpha << 12)` there, so the 0xff000 is
     * an alpha of 0xff shifted up and the rest are factor bits. It also sets
     * +0x34 to 1 for the one factor pair it treats as simple, and
     * copyDispatchEVT2 branches on +0x34 and +0x35.
     *
     * That distinction decides whether a native replacement is allowed to be a
     * copy at all: a blit carrying a real alpha blend is not a memcpy, and
     * replacing it with one would put opaque pixels where translucency belongs.
     * So the blend words are dumped alongside the surfaces, and the complex
     * form at +0x24..+0x30 with them.
     */
    static const unsigned off[] = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18,
                                    0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34,
                                    0x38, 0x3c, 0x40, 0x44, 0x4c, 0x50, 0x54,
                                    0x5c, 0x60 };
    unsigned i;

    fprintf(stderr, "hle-trace   ctx=%08x", ctx);
    for (i = 0; i < (unsigned)(sizeof off / sizeof off[0]); i++) {
        uint32_t w = 0;
        if (read_u32(mem, ctx + off[i], &w))
            fprintf(stderr, " +%02x=%08x", off[i], w);
        else
            fprintf(stderr, " +%02x=??", off[i]);
    }
    fprintf(stderr, "\n");
}

/*
 * FOLLOW THE SURFACE POINTERS, because the first dump proved they are pointers.
 *
 * r248 gave source (+00,+04,+08) = (0xc54bca00, 0x500, 0x60000) and destination
 * (+10,+14,+18) = (0xc54bc980, 0x500, 0x60000). The middle word reads as a
 * stride immediately -- 0x500 is 1280, which is 320 pixels at 4 bytes, and a
 * narrower source in the same run carries 0x340, which is 208 at 4 bytes. The
 * last is a format, one of the constants copyDispatchEVT2 tests.
 *
 * The FIRST word is not a pixel base, and the values say so: 0xc54bca00 and
 * 0xc54bc980 are 0x80 apart, and two 320x480 framebuffers cannot be 128 bytes
 * apart. They are descriptors in a table. So the base has to be read out of the
 * object, which is what this does -- eight words is enough to show which slot
 * holds an address in guest DRAM (0x08000000..0x10000000, where ctx+0x20's
 * 0x090ff000 already sits) and which hold width, height and flags.
 */
static void trace_surface(const ios3_hle_mem_t *mem, const char *tag,
                          uint32_t p) {
    unsigned i;
    if (!p) return;
    /*
     * Read PRIVILEGED. r253 proved the unprivileged path cannot see these at
     * all -- every word came back "??" because 0xc54bca00 is kernel space and
     * the compositing process genuinely cannot dereference it. See
     * ios3_hle_mem_t::read_priv for why reading a descriptor this way is
     * emulating the device rather than granting the guest anything, and for the
     * rule it does not relax: pixels still go through the unprivileged path.
     */
    uint32_t next = 0;
    bool got_next = false;

    fprintf(stderr, "hle-trace   %s@%08x", tag, p);
    for (i = 0; i < 16u; i++) {
        uint32_t w = 0;
        bool ok = mem->read_priv ? mem->read_priv(mem->ctx, p + i * 4u, &w, 4u)
                                 : read_u32(mem, p + i * 4u, &w);
        if (ok) fprintf(stderr, " +%02x=%08x", i * 4u, w);
        else    fprintf(stderr, " +%02x=??", i * 4u);
        if (ok && i == 3u) { next = w; got_next = true; }
    }
    fprintf(stderr, "\n");

    /*
     * ONE MORE LEVEL, because r263 showed the descriptor is itself a handle:
     * every other word was zero or 0xffffffff and +0x0c held another kernel
     * pointer (source -> 0xc54bcb00, destination -> 0xc3954240, and the two
     * land in different regions). A pixel base is not in the first object, so
     * the chain gets followed rather than guessed at -- the same discipline
     * that stopped the first surface word being read as a base.
     */
    if (got_next && next) {
        uint32_t parent = 0;
        bool got_parent = false;

        fprintf(stderr, "hle-trace   %s->%08x", tag, next);
        for (i = 0; i < 16u; i++) {
            uint32_t w = 0;
            bool ok = mem->read_priv
                        ? mem->read_priv(mem->ctx, next + i * 4u, &w, 4u)
                        : read_u32(mem, next + i * 4u, &w);
            if (ok) fprintf(stderr, " +%02x=%08x", i * 4u, w);
            else    fprintf(stderr, " +%02x=??", i * 4u);
            if (ok && i == 9u) { parent = w; got_parent = true; }
        }
        fprintf(stderr, "\n");

        /*
         * ONE MORE, and this time the object is NAMED rather than guessed at.
         * r264's vtable pointers resolve in the kernel's own symbol table:
         *
         *   0xc01fc6b0 -> __ZTV25IOGeneralMemoryDescriptor+0x8
         *   0xc01fcb18 -> __ZTV21IOSubMemoryDescriptor+0x8
         *
         * so the layout is IOKit's, not an inference. The destination reads as
         * a sub-range -- _length 0x96000 at +0x1c, which is exactly 320*480*4,
         * _parent at +0x24 and _start 0x1c2000 at +0x28 -- and that cross-checks
         * against the /vram survey independently: the pool starts at 0x0885c000
         * with 16 slots 0x96000 apart, and 0x0885c000 + 0x1c2000 is 0x08a1e000,
         * which the survey reports as slot 3 holding 606,681 non-zero bytes.
         *
         * So the base a blit needs is the PARENT's, plus _start. This follows
         * +0x24 to get it.
         */
        if (got_parent && parent) {
            fprintf(stderr, "hle-trace   %s=>%08x", tag, parent);
            for (i = 0; i < 16u; i++) {
                uint32_t w = 0;
                bool ok = mem->read_priv
                            ? mem->read_priv(mem->ctx, parent + i * 4u, &w, 4u)
                            : read_u32(mem, parent + i * 4u, &w);
                if (ok) fprintf(stderr, " +%02x=%08x", i * 4u, w);
                else    fprintf(stderr, " +%02x=??", i * 4u);
            }
            fprintf(stderr, "\n");
        }

        /*
         * SETTLE THE ADDRESS SPACE WITH DATA, not with an argument.
         *
         * An IOGeneralMemoryDescriptor's inline range sits at +0x34 as
         * {address, length}, which for the destination read {0x0885c000,
         * 0x960000} -- the /vram pool, a raw CPU physical. The source reads
         * {0x03a8a000, 0x97000}, and nothing is mapped at 0x03a8a000. The
         * device tree declares the aperture at CHILD address 0x03000000 while
         * this machine puts its registers at 0x3b000000, so the candidate
         * rebase is +0x38000000 -- and a blit written on the wrong one would
         * read pixels from a plausible wrong place.
         *
         * So both are dumped. Whichever holds pixels is the answer: a 32-bit
         * ARGB surface is mostly non-zero with 0xff alpha bytes, and an
         * unmapped or wrong region reads as a run of zeros.
         */
        {
            uint32_t range = 0;
            if ((mem->read_priv &&
                 mem->read_priv(mem->ctx, next + 0x34u, &range, 4u)) && range) {
                /* 0x3b000000 spelled out rather than included: this file
                 * deliberately does not depend on soc.h, and the number is the
                 * aperture base the machine already uses. */
                static const uint32_t REBASE = 0x3b000000u;
                uint32_t cand[2];
                unsigned c;
                cand[0] = range;
                cand[1] = (range >= 0x03000000u && range < 0x04000000u)
                            ? (range - 0x03000000u) + REBASE : 0u;
                for (c = 0; c < 2u; c++) {
                    if (!cand[c]) continue;
                    fprintf(stderr, "hle-trace   %s pix@%08x", tag, cand[c]);
                    for (i = 0; i < 8u; i++) {
                        uint32_t w = 0;
                        bool ok = mem->read_phys &&
                                  mem->read_phys(mem->ctx, cand[c] + i * 4u,
                                                 &w, 4u);
                        if (ok) fprintf(stderr, " %08x", w);
                        else    fprintf(stderr, " ????????");
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
    }
}

/* The two thunks take the context in r0 and the operation's own arguments
 * after it, so the stack words are the 5th argument onward. */
static bool hle_trace_blit_copy(arm_cpu_t *cpu, const ios3_hle_mem_t *mem) {
    bool first = g_trace_budget[0] < 12u;
    (void)trace_args("mbx2DCtxBlitCopy", cpu, mem, 0, 3);
    if (first) {
        uint32_t src = 0, dst = 0;
        trace_ctx(mem, cpu->r[0]);
        if (read_u32(mem, cpu->r[0] + 0x00u, &src)) trace_surface(mem, "src", src);
        if (read_u32(mem, cpu->r[0] + 0x10u, &dst)) trace_surface(mem, "dst", dst);
    }
    return false;
}
static bool hle_trace_blit_color(arm_cpu_t *cpu, const ios3_hle_mem_t *mem) {
    return trace_args("mbx2DCtxBlitColor", cpu, mem, 1, 2);
}
static bool hle_trace_conn_open(arm_cpu_t *cpu, const ios3_hle_mem_t *mem) {
    return trace_args("mbxConnectionOpen", cpu, mem, 2, 0);
}
static bool hle_trace_ctx_init(arm_cpu_t *cpu, const ios3_hle_mem_t *mem) {
    return trace_args("mbx2DCtxInitialize", cpu, mem, 3, 0);
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
      hle_sw_sample_nearest_bgra8, IOS3_HLE_REPLACE, false, false, 0, 0, 0, 0 },
    { "sw_sample_color",    0x3122d02cu, PROLOGUE_SW_SAMPLE_COLOR,
      (unsigned)(sizeof PROLOGUE_SW_SAMPLE_COLOR /
                 sizeof PROLOGUE_SW_SAMPLE_COLOR[0]),
      hle_sw_sample_color, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "sw_sample_texture",  0x3122cefcu, PROLOGUE_SW_SAMPLE_TEXTURE,
      (unsigned)(sizeof PROLOGUE_SW_SAMPLE_TEXTURE /
                 sizeof PROLOGUE_SW_SAMPLE_TEXTURE[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "sw_sample_circle",   0x3122e2f0u, PROLOGUE_SW_SAMPLE_CIRCLE,
      (unsigned)(sizeof PROLOGUE_SW_SAMPLE_CIRCLE /
                 sizeof PROLOGUE_SW_SAMPLE_CIRCLE[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "sw_sample_square",   0x3122e434u, PROLOGUE_SW_SAMPLE_SQUARE,
      (unsigned)(sizeof PROLOGUE_SW_SAMPLE_SQUARE /
                 sizeof PROLOGUE_SW_SAMPLE_SQUARE[0]),
      NULL, IOS3_HLE_OBSERVE, false, false, 0, 0, 0, 0 },
    { "sw_scanline",        0x3122d180u, PROLOGUE_SW_SCANLINE,
      (unsigned)(sizeof PROLOGUE_SW_SCANLINE /
                  sizeof PROLOGUE_SW_SCANLINE[0]),
      hle_trace_sw_scanline, IOS3_HLE_TRACE, false, false, 0, 0, 0, 0 },
    { "ogl_poly_scan",      0x311e2100u, PROLOGUE_OGL_POLY_SCAN,
      (unsigned)(sizeof PROLOGUE_OGL_POLY_SCAN /
                 sizeof PROLOGUE_OGL_POLY_SCAN[0]),
      hle_trace_ogl_poly_scan, IOS3_HLE_TRACE,
      false, false, 0, 0, 0, 0 },
    { "mbxConnectionOpen",  0x30e1fc90u, PROLOGUE_MBX_CONNECTION_OPEN,
      (unsigned)(sizeof PROLOGUE_MBX_CONNECTION_OPEN /
                 sizeof PROLOGUE_MBX_CONNECTION_OPEN[0]),
      hle_trace_conn_open, IOS3_HLE_TRACE, false, false, 0, 0, 0, 0 },
    { "mbx2DCtxInitialize", 0x30e1abe4u, PROLOGUE_MBX2D_CTX_INITIALIZE,
      (unsigned)(sizeof PROLOGUE_MBX2D_CTX_INITIALIZE /
                 sizeof PROLOGUE_MBX2D_CTX_INITIALIZE[0]),
      hle_trace_ctx_init, IOS3_HLE_TRACE, false, false, 0, 0, 0, 0 },
    { "mbx2DCtxBlitColor",  0x30e1ad6cu, PROLOGUE_MBX2D_CTX_BLIT_COLOR,
      (unsigned)(sizeof PROLOGUE_MBX2D_CTX_BLIT_COLOR /
                 sizeof PROLOGUE_MBX2D_CTX_BLIT_COLOR[0]),
      hle_trace_blit_color, IOS3_HLE_TRACE, false, false, 0, 0, 0, 0 },
    { "mbx2DCtxBlitCopy",   0x30e1adc0u, PROLOGUE_MBX2D_CTX_BLIT_COPY,
      (unsigned)(sizeof PROLOGUE_MBX2D_CTX_BLIT_COPY /
                 sizeof PROLOGUE_MBX2D_CTX_BLIT_COPY[0]),
      hle_trace_blit_copy, IOS3_HLE_TRACE, false, false, 0, 0, 0, 0 },
};

#define SITE_N ((unsigned)(sizeof g_sites / sizeof g_sites[0]))

static uint32_t g_ttbr0;      /* the address space sites are restricted to */
static unsigned g_armed_n;    /* hot-path gate: zero means do nothing      */

/* TRACE may inspect but cannot write guest memory. Passing a private CPU copy
 * below also prevents a diagnostic handler from changing registers. This makes
 * the header's "guest behaviour is bit-for-bit" promise an enforced boundary,
 * rather than a convention each new tracer has to remember. */
static bool trace_write_denied(void *ctx, uint32_t va, const void *src,
                               uint32_t len) {
    (void)ctx;
    (void)va;
    (void)src;
    (void)len;
    return false;
}

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
    /* bootkernel retries identity checks as shared-cache pages become resident.
     * That is still one trace session, so a retry in the SAME address space must
     * not reopen every diagnostic budget. r270 promised 12 polygon records and
     * printed 13 because this reset was unconditional. */
    if (g_ttbr0 != ttbr0)
        memset(g_trace_budget, 0, sizeof g_trace_budget);
    if (g_ttbr0 != ttbr0) {
        memset(g_sw_ctx_seen, 0, sizeof g_sw_ctx_seen);
        g_sw_ctx_seen_n = 0u;
    }
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
    g_ttbr0 = 0u;
    g_armed_n = 0u;
    memset(g_trace_budget, 0, sizeof g_trace_budget);
    memset(g_sw_ctx_seen, 0, sizeof g_sw_ctx_seen);
    g_sw_ctx_seen_n = 0u;
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

    /*
     * TRACE runs the handler and throws its answer away. The guest executes its
     * own body either way, so this cannot change behaviour -- which is the
     * point: it is how an argument shape gets read at the site without the site
     * yet being trusted to do the work.
     */
    if (s->mode == IOS3_HLE_TRACE) {
        if (s->handler && mem) {
            arm_cpu_t trace_cpu = *cpu;
            ios3_hle_mem_t trace_mem = *mem;
            trace_mem.write = trace_write_denied;
            (void)s->handler(&trace_cpu, &trace_mem);
        }
        return false;
    }

    if (s->mode != IOS3_HLE_REPLACE || !s->handler) return false;

    if (!s->handler(cpu, mem)) { s->declined++; return false; }
    s->handled++;
    return true;
}
