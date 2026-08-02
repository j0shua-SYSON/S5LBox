/*
 * S5LBox -- measured PowerVR MBX register block and its first decoded 2D copy.
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
 * WHAT THIS STILL IS NOT. It is not a general PowerVR renderer. The later
 * command consumer implements one packet family whose producer, bit fields,
 * MMU tables and pixels have all been recovered from the shipped binaries and
 * exact cold snapshots. One premultiplied source-over blend is decoded;
 * unknown formats, scaling, blend equations and geometry are rejected without
 * completion. Everything else remains a fresh question to
 * answer the same way -- observe it, read its producer, then model it. A
 * driver that needs an unknown operation must stop loudly instead of quietly
 * proceeding on fabricated pixels.
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

/* AppleMBX+0x1188 copies submitted words into this 64 KiB hardware ring. */
#define MBX_2D_COPY_WORDS      16u
#define MBX_2D_BLEND_WORDS     18u
#define MBX_2D_END             0x70000000u
#define MBX_2D_COMMAND_HEADER  0xa0060500u
#define MBX_2D_SUBMIT          0xf0000000u
#define MBX_2D_BLEND_TAG       0x20000004u
#define MBX_2D_BLEND_EQUATION  0x095ff000u
#define MBX_2D_OPAQUE_GLOBAL_FACTORS 0x0d500000u
#define MBX_2D_GLOBAL_ALPHA_MASK     0x000ff000u
#define MBX_2D_BLEND_MODE      0x8002ccccu
#define MBX_2D_FILL_MODE       0x8000f0f0u
#define MBX_2D_FILL_COLOR      0xff000000u

/*
 * The command-copy helper advances a software cursor, but the actual submit
 * boundary is a fixed write of 0xf0000000 to ring+0 by AppleMBX+0x1f58. r372
 * caught that same fixed store after packets beginning at +0 and +0x40, so
 * the earlier "rewrite this packet's header" interpretation was accidental:
 * packet zero merely aliases the doorbell.
 *
 * Between those writes the device has to remember where the copied command
 * began. Keep that state in the otherwise unread backing slot for STATUS:
 * s5l_mbx_read() serves the independent `status` field, so this marker is not
 * guest-visible, while snap_mbx() already serialises every reg[] word. That
 * preserves a snapshot taken between the command copy and the submit store
 * without changing the v32 format or stealing a real readable register.
 * r376 then caught two real multi-command submits: eight blended packets and
 * four simple copies were each followed by one fixed doorbell. Preserve both
 * the first ring word and the number of copied heads. A zero count is reserved
 * as the fail-closed overflow marker after 255 heads; the measured batches are
 * far smaller, and pretending to know where a larger one ends would be worse
 * than rejecting it.
 */
#define MBX_2D_PENDING_MAGIC       0xb5c00000u
#define MBX_2D_PENDING_MAGIC_M     0xffc00000u
#define MBX_2D_PENDING_COUNT_SHIFT 14u
#define MBX_2D_PENDING_COUNT_M     0x003fc000u
#define MBX_2D_PENDING_WORD_M      0x00003fffu
/* v32 snapshots made before r376 used a one-head/multiple marker. Decode a
 * saved single head exactly; an old multiple marker has lost the true count
 * and therefore remains a rejection rather than being guessed. */
#define MBX_2D_PENDING_V1_MAGIC    0x4d420000u
#define MBX_2D_PENDING_V1_MAGIC_M  0xffff0000u
#define MBX_2D_PENDING_V1_MULTI    0x00004000u

/* The first proved packet is a 320-pixel BGRA8 surface. There is no decoded
 * destination-stride field in its post-relocation packet, so widening this is
 * not justified until another captured packet supplies that missing fact. */
#define MBX_2D_BGRA_STRIDE     0x500u
#define MBX_2D_WIDTH           320u
#define MBX_2D_HEIGHT          480u
#define MBX_2D_SURFACE_BYTES   (MBX_2D_BGRA_STRIDE * MBX_2D_HEIGHT)

/* The first tiled render captured in r369. The source is an 8x128 BGRA8
 * texture with a 32-byte stride; the object samples its first column over a
 * 320x96 destination rectangle. Texture coordinates and the tile/object
 * stream below independently encode the same dimensions. */
#define MBX_3D_WIDTH           320u
#define MBX_3D_TOP             20u
#define MBX_3D_HEIGHT          96u
#define MBX_3D_SOURCE_STRIDE   0x20u
#define MBX_3D_TARGET_STRIDE   0x500u
#define MBX_3D_ADDRESS_MASK    0x0003ffffu

/* Later _mbx3DCtxQuadCopyPerspective records and live object streams recover
 * the padlock, `Searching...`, and battery status sprites, plus clipped
 * padlock and battery redraws. Their texture-control words encode different
 * padded dimensions/strides. Bit 18 is control rather than GPU address bit 18, so
 * all forms use the same exact 18-bit address field recovered above. */

static uint64_t mbx_rd[MBX_SLOTS];
static uint64_t mbx_wr[MBX_SLOTS];
static uint32_t mbx_last_val[MBX_SLOTS];
static uint32_t mbx_ring_off[MBX_RING];
static uint32_t mbx_ring_val[MBX_RING];
static uint8_t  mbx_ring_wr[MBX_RING];
static uint64_t mbx_ring_n;
static int      mbx_trace_state;   /* 0 unknown, 1 on, -1 off */
static uint64_t mbx_2d_candidates;
static uint64_t mbx_2d_completed;
static uint64_t mbx_2d_rejected;
static uint64_t mbx_2d_bytes;
static uint64_t mbx_3d_candidates;
static uint64_t mbx_3d_completed;
static uint64_t mbx_3d_rejected;
static uint64_t mbx_3d_pixels;

static void mbx_trace_dump(void);

static bool mbx_trace_enabled(void) {
    if (mbx_trace_state == 0) {
        const char *e = getenv("S5LBOX_MBX_TRACE");
        mbx_trace_state = (e && *e && *e != '0') ? 1 : -1;
        if (mbx_trace_state == 1) atexit(mbx_trace_dump);
    }
    return mbx_trace_state == 1;
}

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
    fprintf(stderr, "  2D packets candidates %llu completed %llu rejected %llu"
                    " bytes-committed %llu\n",
            (unsigned long long)mbx_2d_candidates,
            (unsigned long long)mbx_2d_completed,
            (unsigned long long)mbx_2d_rejected,
            (unsigned long long)mbx_2d_bytes);
    fprintf(stderr, "  3D renders candidates %llu completed %llu rejected %llu"
                    " pixels-blended %llu\n",
            (unsigned long long)mbx_3d_candidates,
            (unsigned long long)mbx_3d_completed,
            (unsigned long long)mbx_3d_rejected,
            (unsigned long long)mbx_3d_pixels);

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

    if (!mbx_trace_enabled()) return;

    if (is_write) { mbx_wr[off / 4u]++; mbx_last_val[off / 4u] = val; }
    else            mbx_rd[off / 4u]++;

    s = (unsigned)(mbx_ring_n % MBX_RING);
    mbx_ring_off[s] = off;
    mbx_ring_val[s] = val;
    mbx_ring_wr[s]  = is_write ? 1u : 0u;
    mbx_ring_n++;
}

static uint32_t mbx_load_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t mbx_edram_word(const s5l_mbx_t *m, uint32_t aperture_off) {
    uint32_t o = aperture_off - S5L_MBX_SIZE;
    return mbx_load_le32(m->edram + o);
}

static uint32_t *mbx_2d_pending_slot(s5l_mbx_t *m) {
    return &m->reg[S5L_MBX_STATUS / 4u];
}

static bool mbx_2d_pending_get(const s5l_mbx_t *m, uint32_t *packet_off,
                               uint32_t *command_count) {
    uint32_t state = m->reg[S5L_MBX_STATUS / 4u];
    bool current =
        (state & MBX_2D_PENDING_MAGIC_M) == MBX_2D_PENDING_MAGIC;
    bool v1 = (state & MBX_2D_PENDING_V1_MAGIC_M) ==
              MBX_2D_PENDING_V1_MAGIC;
    if (!current && !v1) return false;
    if (packet_off)
        *packet_off = S5L_MBX_2D_RING_BASE +
                      (state & MBX_2D_PENDING_WORD_M) * 4u;
    if (command_count) {
        if (current)
            *command_count = (state & MBX_2D_PENDING_COUNT_M) >>
                             MBX_2D_PENDING_COUNT_SHIFT;
        else
            *command_count = (state & MBX_2D_PENDING_V1_MULTI) ? 0u : 1u;
    }
    return true;
}

static void mbx_2d_pending_note(s5l_mbx_t *m, uint32_t packet_off) {
    uint32_t old, count;
    if (mbx_2d_pending_get(m, &old, &count)) {
        if (count == 0u) return; /* already overflowed */
        if ((*mbx_2d_pending_slot(m) & MBX_2D_PENDING_MAGIC_M) !=
            MBX_2D_PENDING_MAGIC) {
            uint32_t words = (old - S5L_MBX_2D_RING_BASE) / 4u;
            *mbx_2d_pending_slot(m) = MBX_2D_PENDING_MAGIC |
                (2u << MBX_2D_PENDING_COUNT_SHIFT) | words;
            return;
        }
        if (count == 255u) {
            /* Count zero with a valid magic is the explicit overflow state. */
            *mbx_2d_pending_slot(m) &= ~MBX_2D_PENDING_COUNT_M;
            return;
        }
        count++;
        *mbx_2d_pending_slot(m) =
            (*mbx_2d_pending_slot(m) & ~MBX_2D_PENDING_COUNT_M) |
            (count << MBX_2D_PENDING_COUNT_SHIFT);
        return;
    }
    uint32_t words = (packet_off - S5L_MBX_2D_RING_BASE) / 4u;
    *mbx_2d_pending_slot(m) = MBX_2D_PENDING_MAGIC |
                              (1u << MBX_2D_PENDING_COUNT_SHIFT) | words;
}

static void mbx_2d_pending_clear(s5l_mbx_t *m) {
    *mbx_2d_pending_slot(m) = 0u;
}

/*
 * Translate one GPU VA through the exact GART layout written by
 * AppleMBXMMU::map at 0xc0783570.
 *
 *   chunk    = gpu_va >> 22                         (one root per 4 MiB)
 *   pte      = root[chunk][(gpu_va >> 12) & 0x3ff]
 *   physical = pte + (gpu_va & 0xfff)
 *
 * The eight root-page physical addresses are registers 0x1000..0x101c. The
 * kext stores raw, page-aligned physical addresses in both roots and PTEs; no
 * flag bits were observed, so accepting them would be guessing. host_ram is
 * also the boundary that keeps a malformed packet out of MMIO.
 */
static bool mbx_gart_span(const s5l_mbx_t *m, const arm_bus_t *bus,
                          uint32_t gpu_va, uint32_t *physical,
                          uint32_t *available, const char **why) {
    uint32_t chunk = gpu_va >> 22;
    if (!bus || !bus->host_ram || chunk >= 8u) {
        if (why) *why = "GPU VA is outside the observed eight-root GART";
        return false;
    }

    uint32_t root = m->reg[(0x1000u / 4u) + chunk];
    uint32_t pte_off = ((gpu_va >> 12) & 0x3ffu) * 4u;
    if (!root || (root & 0xfffu) || root > UINT32_MAX - pte_off) {
        if (why) *why = "GART root is zero, unaligned, or wraps";
        return false;
    }

    const uint8_t *entry = bus->host_ram(bus->ctx, root + pte_off, 4u);
    if (!entry) {
        if (why) *why = "GART PTE is outside plain guest DRAM";
        return false;
    }
    uint32_t pte = mbx_load_le32(entry);
    if (!pte || (pte & 0xfffu)) {
        if (why) *why = "GART PTE is zero or not a raw page base";
        return false;
    }

    uint32_t in_page = gpu_va & 0xfffu;
    uint32_t pa = pte + in_page;
    uint32_t span = 0x1000u - in_page;
    if (!bus->host_ram(bus->ctx, pa, span)) {
        if (why) *why = "GART target is outside plain guest DRAM";
        return false;
    }
    *physical = pa;
    *available = span;
    return true;
}

static bool mbx_gart_validate(const s5l_mbx_t *m, const arm_bus_t *bus,
                              uint32_t gpu_va, uint32_t len,
                              const char **why) {
    while (len) {
        uint32_t pa, span;
        if (!mbx_gart_span(m, bus, gpu_va, &pa, &span, why)) return false;
        if (span > len) span = len;
        gpu_va += span;
        len -= span;
    }
    return true;
}

static bool mbx_gart_read(const s5l_mbx_t *m, const arm_bus_t *bus,
                          uint32_t gpu_va, uint8_t *dst, uint32_t len,
                          const char **why) {
    while (len) {
        uint32_t pa, span;
        if (!mbx_gart_span(m, bus, gpu_va, &pa, &span, why)) return false;
        if (span > len) span = len;
        const uint8_t *src = bus->host_ram(bus->ctx, pa, span);
        if (!src) {
            if (why) *why = "source mapping stopped being plain guest DRAM";
            return false;
        }
        memcpy(dst, src, span);
        dst += span;
        gpu_va += span;
        len -= span;
    }
    return true;
}

/* Writes deliberately use the bus, not host_ram. bootkernel interposes on RAM
 * stores to measure live scanout mutations; a direct memcpy would make the new
 * renderer fast by making its own validation blind. */
static bool mbx_gart_write(const s5l_mbx_t *m, const arm_bus_t *bus,
                           uint32_t gpu_va, const uint8_t *src, uint32_t len,
                           const char **why) {
    if (!bus || !bus->write32 || (gpu_va & 3u) || (len & 3u)) {
        if (why) *why = "destination is not a word-addressable bus range";
        return false;
    }
    while (len) {
        uint32_t pa, span;
        if (!mbx_gart_span(m, bus, gpu_va, &pa, &span, why)) return false;
        if (span > len) span = len;
        if ((pa & 3u) || (span & 3u)) {
            if (why) *why = "translated destination is not word aligned";
            return false;
        }
        for (uint32_t i = 0; i < span; i += 4u)
            bus->write32(bus->ctx, pa + i, mbx_load_le32(src + i));
        src += span;
        gpu_va += span;
        len -= span;
    }
    return true;
}

static uint32_t mbx_premultiplied_over(uint32_t dst, uint32_t src);
static uint32_t mbx_modulate_vertex_alpha(uint32_t src, uint32_t alpha);

struct mbx_2d_job {
    uint32_t target;
    uint32_t dst_x, dst_y;
    uint32_t width, height;
    uint32_t row_bytes, total;
    uint8_t *pixels;
};

static void mbx_2d_job_dispose(struct mbx_2d_job *job) {
    if (!job) return;
    free(job->pixels);
    memset(job, 0, sizeof *job);
}

static bool mbx_2d_ranges_overlap(uint32_t a, uint32_t a_len,
                                  uint32_t b, uint32_t b_len) {
    uint64_t a_end = (uint64_t)a + a_len;
    uint64_t b_end = (uint64_t)b + b_len;
    return (uint64_t)a < b_end && (uint64_t)b < a_end;
}

static bool mbx_2d_shadow_source_ok(uint32_t source, uint32_t len,
                                    uint32_t target, const char **why) {
    if (mbx_2d_ranges_overlap(source, len, target, MBX_2D_SURFACE_BYTES)) {
        /* No captured batch reads its own render target. Rejecting that case
         * avoids silently choosing between pre-batch and sequential reads. */
        if (why) *why = "batched source aliases the destination surface";
        return false;
    }
    return true;
}

static bool mbx_2d_job_commit(const s5l_mbx_t *m, const arm_bus_t *bus,
                              const struct mbx_2d_job *job,
                              const char **why) {
    for (uint32_t row = 0; row < job->height; row++) {
        uint32_t dst = job->target +
                       (job->dst_y + row) * MBX_2D_BGRA_STRIDE +
                       job->dst_x * 4u;
        if (!mbx_gart_write(m, bus, dst,
                            job->pixels + row * job->row_bytes,
                            job->row_bytes, why))
            return false;
    }
    return true;
}

static bool mbx_stage_simple_copy(s5l_mbx_t *m, const arm_bus_t *bus,
                                  uint32_t packet_off,
                                  uint32_t shadow_target, uint8_t *shadow,
                                  struct mbx_2d_job *job,
                                  const char **why) {
    memset(job, 0, sizeof *job);
    uint32_t w[MBX_2D_COPY_WORDS];
    for (unsigned i = 0; i < MBX_2D_COPY_WORDS; i++)
        w[i] = mbx_edram_word(m, packet_off + i * 4u);

    /* These constants and masks are the stores/literal pool in
     * _pack2DCtxBlitCopy at 0x30e1c2ac..0x30e1c5a4. This deliberately accepts
     * coordinates but only the captured BGRA8, unity-scale, simple-copy mode.
     */
    if ((w[0] != MBX_2D_COMMAND_HEADER && w[0] != MBX_2D_SUBMIT) ||
        (w[2] & 0xffff8000u) != 0x94060000u ||
        (w[2] & 0x7fffu) != MBX_2D_BGRA_STRIDE ||
        (w[4] & 0xf8002000u) != 0x30000000u ||
        w[5] != 0x60800200u || w[6] != 0x8000ccccu ||
        w[7] != 0xffffffffu ||
        (w[8] & ~0x1fff1fffu) || (w[9] & ~0x1fff1fffu)) {
        if (why) *why = "packet is not the decoded BGRA8 unity simple-copy form";
        return false;
    }
    for (unsigned i = 10; i < MBX_2D_COPY_WORDS; i++) {
        if (w[i] != MBX_2D_END) {
            if (why) *why = "packet terminators are incomplete";
            return false;
        }
    }

    uint32_t src_x = (w[4] >> 14) & 0x1fffu;
    uint32_t src_y = w[4] & 0x1fffu;
    uint32_t dst_x = (w[8] >> 16) & 0x1fffu;
    uint32_t dst_y = w[8] & 0x1fffu;
    uint32_t dst_x1 = (w[9] >> 16) & 0x1fffu;
    uint32_t dst_y1 = w[9] & 0x1fffu;
    if (dst_x1 <= dst_x || dst_y1 <= dst_y) {
        if (why) *why = "destination rectangle is empty or reversed";
        return false;
    }
    uint32_t width = dst_x1 - dst_x;
    uint32_t height = dst_y1 - dst_y;
    if (dst_x1 > MBX_2D_WIDTH || dst_y1 > MBX_2D_HEIGHT ||
        src_x > MBX_2D_WIDTH || width > MBX_2D_WIDTH - src_x ||
        src_y > MBX_2D_HEIGHT || height > MBX_2D_HEIGHT - src_y) {
        if (why) *why = "rectangle exceeds the only measured 320x480 surfaces";
        return false;
    }

    uint32_t row_bytes = width * 4u;
    uint32_t total = row_bytes * height;
    if ((w[1] & 3u) || (w[3] & 3u)) {
        if (why) *why = "source or destination GPU base is not word aligned";
        return false;
    }
    if (shadow && w[1] != shadow_target) {
        if (why) *why = "batched commands do not share one destination surface";
        return false;
    }

    /* Validate every source and destination before changing one byte. A bad
     * late PTE must not leave a half-rendered frame and then withhold
     * completion. Batch staging also rejects unmeasured target-as-source
     * semantics before it mutates the private shadow. */
    for (uint32_t row = 0; row < height; row++) {
        uint64_t src64 = (uint64_t)w[3] +
                         (uint64_t)(src_y + row) * MBX_2D_BGRA_STRIDE +
                         (uint64_t)src_x * 4u;
        uint64_t dst64 = (uint64_t)w[1] +
                         (uint64_t)(dst_y + row) * MBX_2D_BGRA_STRIDE +
                         (uint64_t)dst_x * 4u;
        if (src64 + row_bytes > (uint64_t)UINT32_MAX + 1u ||
            dst64 + row_bytes > (uint64_t)UINT32_MAX + 1u ||
            (shadow && !mbx_2d_shadow_source_ok(
                (uint32_t)src64, row_bytes, shadow_target, why)) ||
            !mbx_gart_validate(m, bus, (uint32_t)src64, row_bytes, why) ||
            !mbx_gart_validate(m, bus, (uint32_t)dst64, row_bytes, why))
            return false;
    }

    uint8_t *pixels = malloc(total);
    if (!pixels) {
        if (why) *why = "host allocation for staged pixels failed";
        return false;
    }
    bool ok = true;
    for (uint32_t row = 0; row < height && ok; row++) {
        uint32_t src = w[3] + (src_y + row) * MBX_2D_BGRA_STRIDE + src_x * 4u;
        ok = mbx_gart_read(m, bus, src, pixels + row * row_bytes,
                           row_bytes, why);
    }
    if (!ok) {
        free(pixels);
        return false;
    }
    if (shadow) {
        for (uint32_t row = 0; row < height; row++)
            memcpy(shadow + (dst_y + row) * MBX_2D_BGRA_STRIDE + dst_x * 4u,
                   pixels + row * row_bytes, row_bytes);
    }

    job->target = w[1];
    job->dst_x = dst_x;
    job->dst_y = dst_y;
    job->width = width;
    job->height = height;
    job->row_bytes = row_bytes;
    job->total = total;
    job->pixels = pixels;
    return true;
}

static bool mbx_stage_black_rect_fill(s5l_mbx_t *m,
                                      const arm_bus_t *bus,
                                      uint32_t packet_off,
                                      uint32_t shadow_target,
                                      uint8_t *shadow,
                                      struct mbx_2d_job *job,
                                      const char **why) {
    memset(job, 0, sizeof *job);
    uint32_t w[MBX_2D_COPY_WORDS];
    for (unsigned i = 0; i < MBX_2D_COPY_WORDS; i++)
        w[i] = mbx_edram_word(m, packet_off + i * 4u);

    /* _pack2DCtxBlitColor at 0x30e1b080..0x30e1b0b4 packs (x, y) and
     * (x + width, y + height), masking each component with the literal
     * 0x1fff/0x1fff0000 pair. r408 then measured six non-full-width instances
     * with this same mode and black colour. Geometry is therefore decoded,
     * while colour, raster operation, surface format and bounds remain closed
     * to the captured iPhone OS 3 form. */
    if ((w[0] != MBX_2D_COMMAND_HEADER && w[0] != MBX_2D_SUBMIT) ||
        w[2] != 0x94060500u || w[3] != 0u || w[4] != 0x30000000u ||
        w[5] != 0x60800200u || w[6] != MBX_2D_FILL_MODE ||
        w[7] != MBX_2D_FILL_COLOR ||
        (w[8] & ~0x1fff1fffu) || (w[9] & ~0x1fff1fffu)) {
        if (why) *why = "packet is not the measured bounded black fill";
        return false;
    }
    for (unsigned i = 10; i < MBX_2D_COPY_WORDS; i++) {
        if (w[i] != MBX_2D_END) {
            if (why) *why = "solid-fill packet terminators are incomplete";
            return false;
        }
    }
    uint32_t left = (w[8] >> 16) & 0x1fffu;
    uint32_t top = w[8] & 0x1fffu;
    uint32_t right = (w[9] >> 16) & 0x1fffu;
    uint32_t bottom = w[9] & 0x1fffu;
    if (right <= left || bottom <= top || right > MBX_2D_WIDTH ||
        bottom > MBX_2D_HEIGHT) {
        if (why) *why = "solid-fill rectangle is empty, reversed, or outside 320x480";
        return false;
    }
    if ((w[1] & 3u) ||
        (uint64_t)w[1] + MBX_2D_SURFACE_BYTES >
            (uint64_t)UINT32_MAX + 1u ||
        (shadow && w[1] != shadow_target)) {
        if (why) *why = "solid-fill target is unaligned or differs within a batch";
        return false;
    }

    const uint32_t width = right - left;
    const uint32_t height = bottom - top;
    const uint32_t row_bytes = width * 4u;
    const uint32_t total = row_bytes * height;
    for (uint32_t row = 0; row < height; row++) {
        uint32_t dst = w[1] + (top + row) * MBX_2D_BGRA_STRIDE + left * 4u;
        if (!mbx_gart_validate(m, bus, dst, row_bytes, why))
            return false;
    }

    uint8_t *pixels = malloc(total);
    if (!pixels) {
        if (why) *why = "host allocation for staged solid fill failed";
        return false;
    }
    for (uint32_t i = 0; i < total; i += 4u) {
        pixels[i] = 0u;
        pixels[i + 1u] = 0u;
        pixels[i + 2u] = 0u;
        pixels[i + 3u] = 0xffu;
    }
    if (shadow) {
        for (uint32_t row = 0; row < height; row++)
            memcpy(shadow + (top + row) * MBX_2D_BGRA_STRIDE + left * 4u,
                   pixels + row * row_bytes, row_bytes);
    }

    job->target = w[1];
    job->dst_x = left;
    job->dst_y = top;
    job->width = width;
    job->height = height;
    job->row_bytes = row_bytes;
    job->total = total;
    job->pixels = pixels;
    return true;
}

static bool mbx_stage_premultiplied_copy(s5l_mbx_t *m,
                                         const arm_bus_t *bus,
                                         uint32_t packet_off,
                                         uint32_t shadow_target,
                                         uint8_t *shadow,
                                         struct mbx_2d_job *job,
                                         const char **why) {
    memset(job, 0, sizeof *job);
    uint32_t w[MBX_2D_BLEND_WORDS];
    for (unsigned i = 0; i < MBX_2D_BLEND_WORDS; i++)
        w[i] = mbx_edram_word(m, packet_off + i * 4u);

    /* _pack2DCtxBlitCopy at 0x30e1c3c4 inserts the two extra words only when
     * ctx+0x35 enables blending. The retained clock/date packets use the
     * 0x095 factors at global alpha 255. r395 exposed QuartzCore's other
     * simple branch from RenderMBX2D::set_tex_blend_mode (0x3123a9c8): fixed
     * 0x0d5 factors plus a variable byte at bits 12..19. Its captured source
     * is wholly opaque, so that branch is staged only as global-alpha
     * modulation followed by premultiplied source-over. */
    bool premultiplied_over = w[6] == MBX_2D_BLEND_EQUATION;
    bool opaque_global =
        (w[6] & ~MBX_2D_GLOBAL_ALPHA_MASK) ==
        MBX_2D_OPAQUE_GLOBAL_FACTORS;
    if ((w[0] != MBX_2D_COMMAND_HEADER && w[0] != MBX_2D_SUBMIT) ||
        (w[2] & 0xffff8000u) != 0x94060000u ||
        (w[4] & 0xf8002000u) != 0x30000000u ||
        w[5] != MBX_2D_BLEND_TAG ||
        (!premultiplied_over && !opaque_global) ||
        w[7] != 0x60800200u || w[8] != MBX_2D_BLEND_MODE ||
        w[9] != 0xffffffffu ||
        (w[10] & ~0x1fff1fffu) || (w[11] & ~0x1fff1fffu)) {
        if (why) *why = "packet is not a decoded blended-copy form";
        return false;
    }
    for (unsigned i = 12; i < MBX_2D_BLEND_WORDS; i++) {
        if (w[i] != MBX_2D_END) {
            if (why) *why = "premultiplied packet terminators are incomplete";
            return false;
        }
    }

    uint32_t source_stride = w[2] & 0x7fffu;
    uint32_t src_x = (w[4] >> 14) & 0x1fffu;
    uint32_t src_y = w[4] & 0x1fffu;
    uint32_t dst_x = (w[10] >> 16) & 0x1fffu;
    uint32_t dst_y = w[10] & 0x1fffu;
    uint32_t dst_x1 = (w[11] >> 16) & 0x1fffu;
    uint32_t dst_y1 = w[11] & 0x1fffu;
    if (!source_stride || (source_stride & 3u) ||
        dst_x1 <= dst_x || dst_y1 <= dst_y) {
        if (why) *why = "source stride or destination rectangle is invalid";
        return false;
    }
    uint32_t width = dst_x1 - dst_x;
    uint32_t height = dst_y1 - dst_y;
    if (dst_x1 > MBX_2D_WIDTH || dst_y1 > MBX_2D_HEIGHT ||
        src_x > source_stride / 4u || width > source_stride / 4u - src_x) {
        if (why) *why = "blend rectangle exceeds a measured surface";
        return false;
    }
    if ((w[1] & 3u) || (w[3] & 3u)) {
        if (why) *why = "blend source or destination GPU base is unaligned";
        return false;
    }
    if (shadow && w[1] != shadow_target) {
        if (why) *why = "batched commands do not share one destination surface";
        return false;
    }

    uint32_t row_bytes = width * 4u;
    uint32_t total = row_bytes * height;
    for (uint32_t row = 0; row < height; row++) {
        uint64_t src64 = (uint64_t)w[3] +
                         (uint64_t)(src_y + row) * source_stride +
                         (uint64_t)src_x * 4u;
        uint64_t dst64 = (uint64_t)w[1] +
                         (uint64_t)(dst_y + row) * MBX_2D_BGRA_STRIDE +
                         (uint64_t)dst_x * 4u;
        if (src64 + row_bytes > (uint64_t)UINT32_MAX + 1u ||
            dst64 + row_bytes > (uint64_t)UINT32_MAX + 1u ||
            (shadow && !mbx_2d_shadow_source_ok(
                (uint32_t)src64, row_bytes, shadow_target, why)) ||
            !mbx_gart_validate(m, bus, (uint32_t)src64, row_bytes, why) ||
            !mbx_gart_validate(m, bus, (uint32_t)dst64, row_bytes, why))
            return false;
    }

    uint8_t *source = malloc(total);
    uint8_t *pixels = malloc(total);
    if (!source || !pixels) {
        free(source);
        free(pixels);
        if (why) *why = "host allocation for staged blend failed";
        return false;
    }
    bool ok = true;
    for (uint32_t row = 0; row < height && ok; row++) {
        uint32_t src = w[3] + (src_y + row) * source_stride + src_x * 4u;
        uint32_t dst = w[1] + (dst_y + row) * MBX_2D_BGRA_STRIDE + dst_x * 4u;
        ok = mbx_gart_read(m, bus, src, source + row * row_bytes,
                           row_bytes, why);
        if (ok && shadow)
            memcpy(pixels + row * row_bytes,
                   shadow + (dst_y + row) * MBX_2D_BGRA_STRIDE + dst_x * 4u,
                   row_bytes);
        else if (ok)
            ok = mbx_gart_read(m, bus, dst, pixels + row * row_bytes,
                               row_bytes, why);
    }
    for (uint32_t i = 0; i < total && ok; i += 4u) {
        uint32_t src = mbx_load_le32(source + i);
        uint32_t alpha = src >> 24;
        if (opaque_global && alpha != 0xffu) {
            if (why) *why = "global-alpha 2D source is not opaque BGRA8";
            ok = false;
            break;
        }
        if (!opaque_global &&
            ((src & 0xffu) > alpha || ((src >> 8) & 0xffu) > alpha ||
             ((src >> 16) & 0xffu) > alpha)) {
            if (why) *why = "2D source is not premultiplied BGRA8";
            ok = false;
            break;
        }
        if (opaque_global)
            src = mbx_modulate_vertex_alpha(
                src, (w[6] & MBX_2D_GLOBAL_ALPHA_MASK) >> 12);
        uint32_t blended = mbx_premultiplied_over(
            mbx_load_le32(pixels + i), src);
        pixels[i] = (uint8_t)blended;
        pixels[i + 1u] = (uint8_t)(blended >> 8);
        pixels[i + 2u] = (uint8_t)(blended >> 16);
        pixels[i + 3u] = (uint8_t)(blended >> 24);
    }
    if (!ok) {
        free(source);
        free(pixels);
        return false;
    }
    if (shadow) {
        for (uint32_t row = 0; row < height; row++)
            memcpy(shadow + (dst_y + row) * MBX_2D_BGRA_STRIDE + dst_x * 4u,
                   pixels + row * row_bytes, row_bytes);
    }

    free(source);
    job->target = w[1];
    job->dst_x = dst_x;
    job->dst_y = dst_y;
    job->width = width;
    job->height = height;
    job->row_bytes = row_bytes;
    job->total = total;
    job->pixels = pixels;
    return true;
}

static bool mbx_stage_2d_packet(s5l_mbx_t *m, const arm_bus_t *bus,
                                uint32_t packet_off,
                                uint32_t shadow_target, uint8_t *shadow,
                                struct mbx_2d_job *job,
                                uint32_t *packet_words,
                                const char **why) {
    const uint32_t ring_end = S5L_MBX_2D_RING_BASE + S5L_MBX_2D_RING_SIZE;
    if (packet_off < S5L_MBX_2D_RING_BASE ||
        packet_off > ring_end - MBX_2D_COPY_WORDS * 4u) {
        if (why) *why = "2D packet crosses the observed 64 KiB ring";
        return false;
    }

    /* Only packet zero aliases the fixed doorbell. Every other counted head
     * must still be present exactly where the preceding decoded length puts
     * it; this rejects gaps, reordering and ring wrap rather than inventing a
     * command-list format. */
    uint32_t expected_head = packet_off == S5L_MBX_2D_RING_BASE
        ? MBX_2D_SUBMIT : MBX_2D_COMMAND_HEADER;
    if (mbx_edram_word(m, packet_off) != expected_head) {
        if (why) *why = "counted command heads are not contiguous and ordered";
        return false;
    }

    if (mbx_edram_word(m, packet_off + 5u * 4u) == MBX_2D_BLEND_TAG) {
        if (packet_off > ring_end - MBX_2D_BLEND_WORDS * 4u) {
            if (why) *why = "blended 2D packet crosses the observed 64 KiB ring";
            return false;
        }
        if (!mbx_stage_premultiplied_copy(m, bus, packet_off,
                                           shadow_target, shadow, job, why))
            return false;
        *packet_words = MBX_2D_BLEND_WORDS;
    } else if (mbx_edram_word(m, packet_off + 6u * 4u) ==
               MBX_2D_FILL_MODE) {
        if (!mbx_stage_black_rect_fill(m, bus, packet_off,
                                        shadow_target, shadow, job, why))
            return false;
        *packet_words = MBX_2D_COPY_WORDS;
    } else {
        if (!mbx_stage_simple_copy(m, bus, packet_off,
                                   shadow_target, shadow, job, why))
            return false;
        *packet_words = MBX_2D_COPY_WORDS;
    }
    return true;
}

static bool mbx_execute_2d_submit(s5l_mbx_t *m, const arm_bus_t *bus,
                                  uint32_t packet_off,
                                  uint32_t command_count,
                                  const char **why,
                                  uint64_t *committed) {
    *committed = 0u;
    if (!command_count) {
        if (why) *why = "pending batch exceeded 255 heads or came from an old count-less snapshot";
        return false;
    }

    if (command_count == 1u) {
        struct mbx_2d_job job;
        uint32_t packet_words;
        if (!mbx_stage_2d_packet(m, bus, packet_off, 0u, NULL,
                                 &job, &packet_words, why))
            return false;
        bool ok = mbx_2d_job_commit(m, bus, &job, why);
        if (ok) *committed = job.total;
        mbx_2d_job_dispose(&job);
        return ok;
    }

    const uint32_t ring_end = S5L_MBX_2D_RING_BASE + S5L_MBX_2D_RING_SIZE;
    if (packet_off > ring_end - MBX_2D_COPY_WORDS * 4u) {
        if (why) *why = "batched 2D packet starts beyond the observed ring";
        return false;
    }
    uint32_t target = mbx_edram_word(m, packet_off + 4u);
    if ((target & 3u) ||
        (uint64_t)target + MBX_2D_SURFACE_BYTES >
            (uint64_t)UINT32_MAX + 1u) {
        if (why) *why = "batched destination surface is unaligned or wraps";
        return false;
    }

    struct mbx_2d_job *jobs = calloc(command_count, sizeof *jobs);
    uint8_t *shadow = malloc(MBX_2D_SURFACE_BYTES);
    if (!jobs || !shadow) {
        free(jobs);
        free(shadow);
        if (why) *why = "host allocation for atomic 2D batch failed";
        return false;
    }
    if (!mbx_gart_read(m, bus, target, shadow, MBX_2D_SURFACE_BYTES, why)) {
        free(jobs);
        free(shadow);
        return false;
    }

    bool ok = true;
    uint32_t staged = 0u;
    uint32_t cursor = packet_off;
    uint64_t total = 0u;
    for (; staged < command_count; staged++) {
        uint32_t packet_words = 0u;
        if (!mbx_stage_2d_packet(m, bus, cursor, target, shadow,
                                 &jobs[staged], &packet_words, why)) {
            ok = false;
            break;
        }
        cursor += packet_words * 4u;
        total += jobs[staged].total;
    }

    /* Every packet has now parsed, every source pixel is known, every target
     * PTE has validated, and sequential overlaps have been evaluated in the
     * private shadow. Only now may guest RAM change. Since the handler is
     * synchronous, the validated GART cannot mutate between staging and these
     * ordered writes. Each job retains its post-command rectangle, preserving
     * intermediate overlap semantics and the scanout write observer. */
    for (uint32_t i = 0; i < command_count && ok; i++)
        ok = mbx_2d_job_commit(m, bus, &jobs[i], why);

    for (uint32_t i = 0; i < command_count; i++)
        mbx_2d_job_dispose(&jobs[i]);
    free(jobs);
    free(shadow);
    if (ok) *committed = total;
    return ok;
}

bool s5l_mbx_process_2d(s5l_mbx_t *m, const arm_bus_t *bus,
                        uint32_t written_off, uint32_t value) {
    if (!m || !m->edram || written_off < S5L_MBX_2D_RING_BASE ||
        written_off >= S5L_MBX_2D_RING_BASE + S5L_MBX_2D_RING_SIZE ||
        (written_off & 3u))
        return false;

    /* AppleMBX+0x1188 copies one or more commands per selector-3 submission.
     * Count their heads, but do not execute while any body is still arriving. */
    if (value == MBX_2D_COMMAND_HEADER) {
        mbx_2d_pending_note(m, written_off);
        return false;
    }

    /* AppleMBX+0x1f58 performs this fixed ring+0 write after the complete
     * command sequence. r376 measured the same boundary for 1, 4 and 8 heads. */
    if (written_off != S5L_MBX_2D_RING_BASE || value != MBX_2D_SUBMIT)
        return false;

    uint32_t packet_off, command_count;
    if (!mbx_2d_pending_get(m, &packet_off, &command_count)) return false;
    mbx_2d_pending_clear(m);

    /* Count zero is the explicit >=256/legacy-unknown marker. Recording 256
     * is a truthful lower bound; normal measured batches retain exact counts. */
    uint32_t metric_count = command_count ? command_count : 256u;
    mbx_2d_candidates += metric_count;

    const char *why = "unknown rejection";
    uint64_t committed = 0u;
    bool ok = mbx_execute_2d_submit(m, bus, packet_off, command_count,
                                    &why, &committed);
    if (!ok) {
        mbx_2d_rejected += metric_count;
        if (mbx_trace_state == 1)
            fprintf(stderr, "MBX2D reject ring+0x%04x (%u commands): %s\n",
                    packet_off - S5L_MBX_2D_RING_BASE,
                    metric_count, why);
        return false;
    }

    /* Completion belongs to the whole atomic submit whose pixels were just
     * committed, not to the unrelated startup BACKGROUND_TAG write at 0x6d8. */
    m->status |= S5L_MBX_STATUS_2D_SYNC;
    mbx_2d_completed += command_count;
    mbx_2d_bytes += committed;
    if (mbx_trace_state == 1)
        fprintf(stderr,
                "MBX2D complete ring+0x%04x (%u commands): %llu bytes\n",
                packet_off - S5L_MBX_2D_RING_BASE, command_count,
                (unsigned long long)committed);
    return true;
}

static bool mbx_gart_u32(const s5l_mbx_t *m, const arm_bus_t *bus,
                         uint32_t gpu_va, uint32_t *value,
                         const char **why) {
    uint8_t bytes[4];
    if (!mbx_gart_read(m, bus, gpu_va, bytes, sizeof bytes, why)) return false;
    *value = mbx_load_le32(bytes);
    return true;
}

static uint32_t mbx_3d_decode_address(uint32_t word) {
    return (word & MBX_3D_ADDRESS_MASK) << 7;
}

/* QuartzCore selects source ONE and destination ONE_MINUS_SRC_ALPHA for this
 * object. Its shipped software compositor at 0x3122dcf4 uses the same 8-bit
 * fixed-point equation, with 256-alpha rather than an inferred /255 rule. */
static uint32_t mbx_premultiplied_over(uint32_t dst, uint32_t src) {
    uint32_t inv = 256u - (src >> 24);
    uint32_t out = 0u;
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
        uint32_t s = (src >> shift) & 0xffu;
        uint32_t d = (dst >> shift) & 0xffu;
        out |= (s + ((d * inv) >> 8)) << shift;
    }
    return out;
}

/* QuartzCore's software rasterizer modulates a sampled channel as
 * `vertex * (texture + 1) >> 8`. The r180 software frame and r382 MBX frame
 * independently pin that equation for these status sprites: an unmodulated
 * source channel 255 becomes 191 under the captured 0xbf vertex alpha, and
 * 183 becomes 137. Applying one alpha to every premultiplied BGRA8 channel
 * preserves the source-over invariant and the identity endpoint at 255. */
static uint32_t mbx_modulate_vertex_alpha(uint32_t src, uint32_t alpha) {
    uint32_t out = 0u;
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
        uint32_t component = (src >> shift) & 0xffu;
        out |= (((component + 1u) * alpha) >> 8) << shift;
    }
    return out;
}

struct mbx_3d_word {
    uint16_t off;
    uint32_t value;
};

struct mbx_3d_background_form {
    uint32_t xclip, yclip;
    uint32_t tile_x0, tile_x1, tile_y0, tile_y1;
    uint32_t left, top, width, height;
    uint32_t source_row0;
    uint32_t boundary[8];
};

/* The first entry is r369's full 320x96 overlay. r379 captured the next redraw
 * as two literal dirty rectangles over the same quad/source: source rows 0..76
 * across the full width, then rows 77..88 inset by eight pixels. Keeping all
 * three forms exact makes those clipped redraws possible without accepting an
 * arbitrary tile stream or inferring a generic PowerVR rasterizer. */
static const struct mbx_3d_background_form mbx_3d_background_forms[] = {
    {
        .xclip = 0x01400000u, .yclip = 0x00800010u,
        .tile_x0 = 0u, .tile_x1 = 39u,
        .tile_y0 = 1u, .tile_y1 = 7u,
        .left = 0u, .top = 20u, .width = 320u, .height = 96u,
        .source_row0 = 0u,
        .boundary = {
            0x00000000u, 0x42e80000u, 0x00000000u, 0x41a00000u,
            0x43a00000u, 0x42e80000u, 0x43a00000u, 0x41a00000u,
        },
    },
    {
        .xclip = 0x01400000u, .yclip = 0x00700010u,
        .tile_x0 = 0u, .tile_x1 = 39u,
        .tile_y0 = 1u, .tile_y1 = 6u,
        .left = 0u, .top = 20u, .width = 320u, .height = 77u,
        .source_row0 = 0u,
        .boundary = {
            0x00000000u, 0x42c20000u, 0x00000000u, 0x41a00000u,
            0x43a00000u, 0x42c20000u, 0x43a00000u, 0x41a00000u,
        },
    },
    {
        .xclip = 0x01380008u, .yclip = 0x00700060u,
        .tile_x0 = 1u, .tile_x1 = 38u,
        .tile_y0 = 6u, .tile_y1 = 6u,
        .left = 8u, .top = 97u, .width = 304u, .height = 12u,
        .source_row0 = 77u,
        .boundary = {
            0x41000000u, 0x42da0000u, 0x41000000u, 0x42c20000u,
            0x439c0000u, 0x42da0000u, 0x439c0000u, 0x42c20000u,
        },
    },
};

static const struct mbx_3d_background_form *
mbx_3d_find_background_form(const s5l_mbx_t *m) {
    uint32_t xclip = m->reg[S5L_MBX_FBXCLIP / 4u];
    uint32_t yclip = m->reg[S5L_MBX_FBYCLIP / 4u];
    for (unsigned i = 0;
         i < sizeof mbx_3d_background_forms /
             sizeof mbx_3d_background_forms[0]; i++) {
        const struct mbx_3d_background_form *form =
            &mbx_3d_background_forms[i];
        if (form->xclip == xclip && form->yclip == yclip) return form;
    }
    return NULL;
}

static uint32_t mbx_3d_boundary_fixed_expected(uint32_t off) {
    static const struct mbx_3d_word nonzero[] = {
        {0x080u, 0x22206f80u}, {0x088u, 0x45800000u},
        {0x094u, 0x45800000u}, {0x098u, 0x45800000u},
        {0x09cu, 0x45800000u}, {0x0b4u, 0x22207f80u},
        {0x0e8u, 0xe0000000u}, {0x0f4u, 0xa6887610u},
        {0x0f8u, 0x22220e80u}, {0x12cu, 0x3f800000u},
        {0x130u, 0x3f800000u}, {0x134u, 0x3f800000u},
        {0x138u, 0x3f800000u}, {0x198u, 0xe0000000u},
        {0x19cu, 0x22200e80u}, {0x1d0u, 0x3f800000u},
        {0x1d4u, 0x3f800000u}, {0x1d8u, 0x3f800000u},
        {0x1dcu, 0x3f800000u},
    };
    for (unsigned i = 0; i < sizeof nonzero / sizeof nonzero[0]; i++)
        if (nonzero[i].off == off) return nonzero[i].value;
    return 0u;
}

static uint32_t mbx_3d_background_boundary_expected(
    const struct mbx_3d_background_form *form, uint32_t off) {
    if (off >= 0x0b8u && off <= 0x0d4u)
        return form->boundary[(off - 0x0b8u) / 4u];
    return mbx_3d_boundary_fixed_expected(off);
}

static bool mbx_execute_first_tiled_over(s5l_mbx_t *m,
                                         const arm_bus_t *bus,
                                         const char **why,
                                         uint32_t *pixels_blended) {
    uint32_t region = m->reg[S5L_MBX_RGNBASE / 4u];
    uint32_t object = m->reg[S5L_MBX_OBJBASE / 4u];
    uint32_t target = m->reg[S5L_MBX_FBSTART / 4u];
    const struct mbx_3d_background_form *form =
        mbx_3d_find_background_form(m);

    if ((region & 3u) || (object & 3u) || (target & 3u) ||
        object > UINT32_MAX - 0x2a0u) {
        if (why) *why = "region, object, or framebuffer base is invalid";
        return false;
    }
    if (m->reg[S5L_MBX_3DPIXSAMP / 4u] != 0x00020007u ||
        m->reg[S5L_MBX_FBCTL / 4u] != 0x00000006u ||
        m->reg[S5L_MBX_FBLINESTRIDE / 4u] != MBX_3D_WIDTH || !form) {
        if (why) *why = "render registers are not a captured BGRA8 background form";
        return false;
    }

    /* Each captured form is an ordered rectangle of 8x16 tiles. Every tile
     * points at the same list, and only the final tile carries bit 31. */
    uint32_t list = object + 0x68u;
    uint32_t tile_count = (form->tile_x1 - form->tile_x0 + 1u) *
                          (form->tile_y1 - form->tile_y0 + 1u);
    uint32_t tile_index = 0u;
    for (uint32_t y = form->tile_y0; y <= form->tile_y1; y++) {
        for (uint32_t x = form->tile_x0; x <= form->tile_x1; x++) {
            uint32_t pair = tile_index * 8u;
            uint32_t code, pointer;
            if (!mbx_gart_u32(m, bus, region + pair, &code, why) ||
                !mbx_gart_u32(m, bus, region + pair + 4u, &pointer, why))
                return false;
            uint32_t expected = (y << 8) | x;
            if (tile_index + 1u == tile_count) expected |= 0x80000000u;
            if (code != expected || pointer != list) {
                if (why) *why = "region tile list differs from its captured background stream";
                return false;
            }
            tile_index++;
        }
    }

    static const uint32_t list_words[4] = {
        0x60200020u, 0x6020002du, 0x61a0007cu, 0xf0000000u
    };
    for (unsigned i = 0; i < 4u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, list + i * 4u, &value, why)) return false;
        if (value != list_words[i]) {
            if (why) *why = "object list is not the captured three-object list";
            return false;
        }
    }

    static const uint32_t background[26] = {
        0xe0000000u, 0xa7718000u, 0u, 0xd6887610u,
        0x22220e80u, 0u, 0u, 0x45000000u,
        0u, 0u, 0x45000000u, 0x3f800000u,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
        0x3f800000u, 0u, 0u, 0u,
        0u, 0x40000000u, 0u, 0u,
        0u, 0x40000000u,
    };
    for (unsigned i = 0; i < 26u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + i * 4u, &value, why)) return false;
        if (i == 2u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                mbx_3d_decode_address(value) != target) {
                if (why) *why = "background texture does not resolve to FBSTART";
                return false;
            }
        } else if (value != background[i]) {
            if (why) *why = "background object differs from the captured form";
            return false;
        }
    }

    /* The first two list entries are the rectangle's fixed boundary objects.
     * Check every word, including the measured zero padding, before accepting
     * the final textured quad. */
    for (uint32_t off = 0x80u; off < 0x1f0u; off += 4u) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + off, &value, why)) return false;
        if (value != mbx_3d_background_boundary_expected(form, off)) {
            if (why) *why = "rectangle boundary object differs from the captured form";
            return false;
        }
    }

    static const uint32_t quad[44] = {
        0xe0000000u, 0xa0418001u, 0u, 0xa6884710u,
        0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
        0x00000000u, 0x41a00000u, 0x43a00000u, 0x41a00000u,
        0x00000000u, 0x42e80000u, 0x43a00000u, 0x42e80000u,
        0u, 0u, 0u, 0u,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
        0xff000000u, 0x00000000u, 0x00000000u, 0x00000000u,
        0x3ca00000u, 0xff000000u, 0x3d800000u, 0x00000000u,
        0x3ea00000u, 0x3ca00000u, 0xff000000u, 0x00000000u,
        0x3f400000u, 0x00000000u, 0x3de80000u, 0xff000000u,
        0x3d800000u, 0x3f400000u, 0x3ea00000u, 0x3de80000u,
    };
    uint32_t source = 0u;
    for (unsigned i = 0; i < 44u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + 0x1f0u + i * 4u,
                          &value, why))
            return false;
        if (i == 2u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x8e000000u) {
                if (why) *why = "source texture address word has an unknown format";
                return false;
            }
            source = mbx_3d_decode_address(value);
        } else if (i == 5u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                mbx_3d_decode_address(value) != target) {
                if (why) *why = "destination texture does not resolve to FBSTART";
                return false;
            }
        } else if (value != quad[i]) {
            if (why) *why = "textured quad differs from the captured source-over form";
            return false;
        }
    }
    if (!source) {
        if (why) *why = "source texture resolves to GPU address zero";
        return false;
    }

    uint32_t row_bytes = form->width * 4u;
    uint32_t total = row_bytes * form->height;
    uint64_t source_end = (uint64_t)source +
                          (uint64_t)(form->source_row0 + form->height - 1u) *
                              MBX_3D_SOURCE_STRIDE + 4u;
    uint64_t target_end = (uint64_t)target +
                          (uint64_t)(form->top + form->height - 1u) *
                              MBX_3D_TARGET_STRIDE +
                          (uint64_t)(form->left + form->width) * 4u;
    if (source_end > UINT32_MAX || target_end > UINT32_MAX) {
        if (why) *why = "captured background rectangle overflows its surface";
        return false;
    }
    for (uint32_t row = 0; row < form->height; row++) {
        uint32_t src = source +
                       (form->source_row0 + row) * MBX_3D_SOURCE_STRIDE;
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        if (!mbx_gart_validate(m, bus, src, 4u, why) ||
            !mbx_gart_validate(m, bus, dst, row_bytes, why))
            return false;
    }

    uint8_t *pixels = malloc(total);
    if (!pixels) {
        if (why) *why = "host allocation for staged 3D pixels failed";
        return false;
    }
    bool ok = true;
    for (uint32_t row = 0; row < form->height && ok; row++) {
        uint8_t src_bytes[4];
        uint32_t src_addr = source +
                            (form->source_row0 + row) * MBX_3D_SOURCE_STRIDE;
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        ok = mbx_gart_read(m, bus, src_addr, src_bytes,
                           sizeof src_bytes, why) &&
             mbx_gart_read(m, bus, dst, pixels + row * row_bytes,
                           row_bytes, why);
        if (!ok) break;
        uint32_t src = mbx_load_le32(src_bytes);
        uint32_t alpha = src >> 24;
        if ((src & 0xffu) > alpha || ((src >> 8) & 0xffu) > alpha ||
            ((src >> 16) & 0xffu) > alpha) {
            if (why) *why = "source texture is not premultiplied BGRA8";
            ok = false;
            break;
        }
        for (uint32_t x = 0; x < form->width; x++) {
            uint8_t *pixel = pixels + row * row_bytes + x * 4u;
            uint32_t blended = mbx_premultiplied_over(
                mbx_load_le32(pixel), src);
            pixel[0] = (uint8_t)blended;
            pixel[1] = (uint8_t)(blended >> 8);
            pixel[2] = (uint8_t)(blended >> 16);
            pixel[3] = (uint8_t)(blended >> 24);
        }
    }
    for (uint32_t row = 0; row < form->height && ok; row++) {
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        ok = mbx_gart_write(m, bus, dst, pixels + row * row_bytes,
                            row_bytes, why);
    }
    free(pixels);
    if (ok && pixels_blended) *pixels_blended = form->width * form->height;
    return ok;
}

struct mbx_3d_status_form {
    uint32_t xclip, yclip;
    uint32_t target;
    bool variable_vertex_alpha;
    bool boundary_override;
    uint32_t tile_x0, tile_x1, tile_y0, tile_y1;
    uint32_t left, top, width, height;
    uint32_t source_row0;
    uint32_t source_stride;
    uint32_t source_control;
    uint32_t boundary[8];
    uint32_t quad[44];
};

/* These are literal transcriptions of the live object streams. Words 2 and 5
 * are address fields and are validated separately against each form's control
 * bits and FBSTART; every other word must match exactly. A zero target accepts
 * either surface used by the earlier status forms.
 *
 * The slider label is the one variable-alpha exception: r385/r387/r389
 * measured the same word at all four vertices while its high byte stepped b4,
 * 8a, 61, 37 and its low 24 bits stayed zero. Only those four words may vary,
 * must remain identical, and are consumed as the per-vertex alpha established
 * by the software-renderer pixel oracle. r402-r406's tutorial layers retain
 * their literal b7/05 alpha words; one capture is not treated as proof of an
 * arbitrary opacity range. Five former 0x612 forms were removed after r414
 * proved that they followed the stale +0x1f0 texture instead of the object
 * selected by the list pointer. */
static const struct mbx_3d_status_form mbx_3d_status_forms[] = {
    {
        .xclip = 0x00a80098u, .yclip = 0x00200000u,
        .tile_x0 = 0x13u, .tile_x1 = 0x14u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 155u, .top = 0u, .width = 10u, .height = 20u,
        .source_stride = 0x40u, .source_control = 0x0e040000u,
        .quad = {
            0xe0000000u, 0xa1218000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x431b0000u, 0x41a00000u, 0x431b0000u, 0x00000000u,
            0x43250000u, 0x41a00000u, 0x43250000u, 0x00000000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x3e1b0000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x3e1b0000u, 0x00000000u, 0xbf000000u, 0x3f200000u,
            0x3f200000u, 0x3e250000u, 0x3ca00000u, 0xbf000000u,
            0x3f200000u, 0x00000000u, 0x3e250000u, 0x00000000u,
        },
    },
    {
        .xclip = 0x00500000u, .yclip = 0x00200000u,
        .tile_x0 = 0u, .tile_x1 = 9u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 4u, .top = 1u, .width = 76u, .height = 16u,
        .source_stride = 0x140u, .source_control = 0x0e140000u,
        .quad = {
            0xe0000000u, 0xa4118000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x40800000u, 0x41880000u, 0x40800000u, 0x3f800000u,
            0x42a00000u, 0x41880000u, 0x42a00000u, 0x3f800000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f800000u, 0x3b800000u,
            0x3c880000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x3b800000u, 0x3a800000u, 0xbf000000u, 0x3f180000u,
            0x3f800000u, 0x3da00000u, 0x3c880000u, 0xbf000000u,
            0x3f180000u, 0x00000000u, 0x3da00000u, 0x3a800000u,
        },
    },
    {
        .xclip = 0x01400128u, .yclip = 0x00200000u,
        .tile_x0 = 0x25u, .tile_x1 = 0x27u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 296u, .top = 0u, .width = 21u, .height = 20u,
        .source_stride = 0x60u, .source_control = 0x0e040000u,
        .quad = {
            0xe0000000u, 0xa2218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x43940000u, 0x41a00000u, 0x43940000u, 0x00000000u,
            0x439e8000u, 0x41a00000u, 0x439e8000u, 0x00000000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x3e940000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x3e940000u, 0x00000000u, 0xbf000000u, 0x3f280000u,
            0x3f200000u, 0x3e9e8000u, 0x3ca00000u, 0xbf000000u,
            0x3f280000u, 0x00000000u, 0x3e9e8000u, 0x00000000u,
        },
    },
    {
        .xclip = 0x00a80098u, .yclip = 0x00200010u,
        .tile_x0 = 0x13u, .tile_x1 = 0x14u,
        .tile_y0 = 1u, .tile_y1 = 1u,
        .left = 155u, .top = 16u, .width = 10u, .height = 4u,
        .source_row0 = 16u,
        .source_stride = 0x40u, .source_control = 0x0e040000u,
        .quad = {
            0xe0000000u, 0xa1218000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x431b0000u, 0x41a00000u, 0x431b0000u, 0x41800000u,
            0x43250000u, 0x41a00000u, 0x43250000u, 0x41800000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x3e1b0000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x3f000000u,
            0x3e1b0000u, 0x3c800000u, 0xbf000000u, 0x3f200000u,
            0x3f200000u, 0x3e250000u, 0x3ca00000u, 0xbf000000u,
            0x3f200000u, 0x3f000000u, 0x3e250000u, 0x3c800000u,
        },
    },
    {
        .xclip = 0x01400128u, .yclip = 0x00200010u,
        .tile_x0 = 0x25u, .tile_x1 = 0x27u,
        .tile_y0 = 1u, .tile_y1 = 1u,
        .left = 296u, .top = 16u, .width = 21u, .height = 4u,
        .source_row0 = 16u,
        .source_stride = 0x60u, .source_control = 0x0e040000u,
        .quad = {
            0xe0000000u, 0xa2218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x43940000u, 0x41a00000u, 0x43940000u, 0x41800000u,
            0x439e8000u, 0x41a00000u, 0x439e8000u, 0x41800000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x3e940000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x3f000000u,
            0x3e940000u, 0x3c800000u, 0xbf000000u, 0x3f280000u,
            0x3f200000u, 0x3e9e8000u, 0x3ca00000u, 0xbf000000u,
            0x3f280000u, 0x3f000000u, 0x3e9e8000u, 0x3c800000u,
        },
    },
    {
        .xclip = 0x01400000u, .yclip = 0x00200000u,
        .target = 0x00998000u,
        .tile_x0 = 0u, .tile_x1 = 0x27u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 0u, .top = 0u, .width = 320u, .height = 20u,
        .source_stride = 0x500u, .source_control = 0x0e500000u,
        .quad = {
            0xe0000000u, 0xa6218000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x00000000u, 0x41a00000u, 0x00000000u, 0x00000000u,
            0x43a00000u, 0x41a00000u, 0x43a00000u, 0x00000000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x00000000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x00000000u, 0x00000000u, 0xbf000000u, 0x3f200000u,
            0x3f200000u, 0x3ea00000u, 0x3ca00000u, 0xbf000000u,
            0x3f200000u, 0x00000000u, 0x3ea00000u, 0x00000000u,
        },
    },
    {
        .xclip = 0x01400000u, .yclip = 0x00200000u,
        .target = 0x00897000u,
        .tile_x0 = 0u, .tile_x1 = 0x27u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 0u, .top = 0u, .width = 320u, .height = 20u,
        .source_stride = 0x500u, .source_control = 0x0e500000u,
        .quad = {
            0xe0000000u, 0xa6218000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x00000000u, 0x41a00000u, 0x00000000u, 0x00000000u,
            0x43a00000u, 0x41a00000u, 0x43a00000u, 0x00000000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x00000000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x00000000u, 0x00000000u, 0xbf000000u, 0x3f200000u,
            0x3f200000u, 0x3ea00000u, 0x3ca00000u, 0xbf000000u,
            0x3f200000u, 0x00000000u, 0x3ea00000u, 0x00000000u,
        },
    },
    {
        .xclip = 0x01400000u, .yclip = 0x00200000u,
        .target = 0x00a41000u,
        .tile_x0 = 0u, .tile_x1 = 0x27u,
        .tile_y0 = 0u, .tile_y1 = 1u,
        .left = 0u, .top = 0u, .width = 320u, .height = 20u,
        .source_stride = 0x500u, .source_control = 0x0e500000u,
        .quad = {
            0xe0000000u, 0xa6218000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x00000000u, 0x41a00000u, 0x00000000u, 0x00000000u,
            0x43a00000u, 0x41a00000u, 0x43a00000u, 0x00000000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xbf000000u, 0x00000000u, 0x3f200000u, 0x00000000u,
            0x3ca00000u, 0xbf000000u, 0x00000000u, 0x00000000u,
            0x00000000u, 0x00000000u, 0xbf000000u, 0x3f200000u,
            0x3f200000u, 0x3ea00000u, 0x3ca00000u, 0xbf000000u,
            0x3f200000u, 0x00000000u, 0x3ea00000u, 0x00000000u,
        },
    },
    {
        .xclip = 0x01400000u, .yclip = 0x01e00010u,
        .target = 0x00a41000u,
        .tile_x0 = 0u, .tile_x1 = 0x27u,
        .tile_y0 = 1u, .tile_y1 = 0x1du,
        .left = 0u, .top = 20u, .width = 320u, .height = 460u,
        .source_row0 = 20u,
        .source_stride = 0x500u, .source_control = 0x0e500000u,
        .quad = {
            0xe0000000u, 0xa6618000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x00000000u, 0x43f00000u, 0x00000000u, 0x41a00000u,
            0x43a00000u, 0x43f00000u, 0x43a00000u, 0x41a00000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0xb7000000u, 0x00000000u, 0x3f700000u, 0x00000000u,
            0x3ef00000u, 0xb7000000u, 0x00000000u, 0x3d200000u,
            0x00000000u, 0x3ca00000u, 0xb7000000u, 0x3f200000u,
            0x3f700000u, 0x3ea00000u, 0x3ef00000u, 0xb7000000u,
            0x3f200000u, 0x3d200000u, 0x3ea00000u, 0x3ca00000u,
        },
    },
    {
        .xclip = 0x01300010u, .yclip = 0x01800080u,
        .target = 0x00a41000u,
        .tile_x0 = 2u, .tile_x1 = 0x25u,
        .tile_y0 = 8u, .tile_y1 = 0x17u,
        .left = 18u, .top = 130u, .width = 284u, .height = 241u,
        .source_stride = 0x480u, .source_control = 0x0e480000u,
        .quad = {
            0xe0000000u, 0xa6518000u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x41900000u, 0x43b98000u, 0x41900000u, 0x43020000u,
            0x43970000u, 0x43b98000u, 0x43970000u, 0x43020000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0x05000000u, 0x00000000u, 0x3f710000u, 0x3c900000u,
            0x3eb98000u, 0x05000000u, 0x00000000u, 0x00000000u,
            0x3c900000u, 0x3e020000u, 0x05000000u, 0x3f0e0000u,
            0x3f710000u, 0x3e970000u, 0x3eb98000u, 0x05000000u,
            0x3f0e0000u, 0x00000000u, 0x3e970000u, 0x3e020000u,
        },
    },
    {
        .xclip = 0x01280018u, .yclip = 0x00b00090u,
        .target = 0x00a41000u,
        .tile_x0 = 3u, .tile_x1 = 0x24u,
        .tile_y0 = 9u, .tile_y1 = 0x0au,
        .left = 30u, .top = 145u, .width = 260u, .height = 23u,
        .source_stride = 0x420u, .source_control = 0x0e400000u,
        .quad = {
            0xe0000000u, 0xa6218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x41f00000u, 0x43280000u, 0x41f00000u, 0x43110000u,
            0x43910000u, 0x43280000u, 0x43910000u, 0x43110000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0x05000000u, 0x00000000u, 0x3f380000u, 0x3cf00000u,
            0x3e280000u, 0x05000000u, 0x00000000u, 0x00000000u,
            0x3cf00000u, 0x3e110000u, 0x05000000u, 0x3f020000u,
            0x3f380000u, 0x3e910000u, 0x3e280000u, 0x05000000u,
            0x3f020000u, 0x00000000u, 0x3e910000u, 0x3e110000u,
        },
    },
    {
        .xclip = 0x01280018u, .yclip = 0x013000a0u,
        .target = 0x00a41000u,
        .tile_x0 = 3u, .tile_x1 = 0x24u,
        .tile_y0 = 0x0au, .tile_y1 = 0x12u,
        .left = 30u, .top = 175u, .width = 260u, .height = 121u,
        .source_stride = 0x420u, .source_control = 0x0e400000u,
        .quad = {
            0xe0000000u, 0xa6418001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x41f00000u, 0x43940000u, 0x41f00000u, 0x432f0000u,
            0x43910000u, 0x43940000u, 0x43910000u, 0x432f0000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0x05000000u, 0x00000000u, 0x3f720000u, 0x3cf00000u,
            0x3e940000u, 0x05000000u, 0x00000000u, 0x00000000u,
            0x3cf00000u, 0x3e2f0000u, 0x05000000u, 0x3f020000u,
            0x3f720000u, 0x3e910000u, 0x3e940000u, 0x05000000u,
            0x3f020000u, 0x00000000u, 0x3e910000u, 0x3e2f0000u,
        },
    },
    {
        .xclip = 0x01280018u, .yclip = 0x01700130u,
        .target = 0x00a41000u,
        .tile_x0 = 3u, .tile_x1 = 0x24u,
        .tile_y0 = 0x13u, .tile_y1 = 0x16u,
        .left = 29u, .top = 312u, .width = 262u, .height = 43u,
        .source_stride = 0x420u, .source_control = 0x0e400000u,
        .quad = {
            0xe0000000u, 0xa6318001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x41e80000u, 0x43b18000u, 0x41e80000u, 0x439c0000u,
            0x43918000u, 0x43b18000u, 0x43918000u, 0x439c0000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0x05000000u, 0x00000000u, 0x3f2c0000u, 0x3ce80000u,
            0x3eb18000u, 0x05000000u, 0x00000000u, 0x00000000u,
            0x3ce80000u, 0x3e9c0000u, 0x05000000u, 0x3f030000u,
            0x3f2c0000u, 0x3e918000u, 0x3eb18000u, 0x05000000u,
            0x3f030000u, 0x00000000u, 0x3e918000u, 0x3e9c0000u,
        },
    },
    {
        .xclip = 0x01180070u, .yclip = 0x01c001a0u,
        .target = 0x00897000u,
        .variable_vertex_alpha = true,
        .tile_x0 = 0x0eu, .tile_x1 = 0x22u,
        .tile_y0 = 0x1au, .tile_y1 = 0x1bu,
        .left = 114u, .top = 417u, .width = 161u, .height = 30u,
        .source_stride = 0x2a0u, .source_control = 0x0e280000u,
        .quad = {
            0xe0000000u, 0xa5218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x42e40000u, 0x43df8000u, 0x42e40000u, 0x43d08000u,
            0x43898000u, 0x43df8000u, 0x43898000u, 0x43d08000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0u, 0u, 0x3f700000u, 0x3de40000u,
            0x3edf8000u, 0u, 0u, 0u,
            0x3de40000u, 0x3ed08000u, 0u, 0x3f210000u,
            0x3f700000u, 0x3e898000u, 0x3edf8000u, 0u,
            0x3f210000u, 0u, 0x3e898000u, 0x3ed08000u,
        },
    },
    {
        .xclip = 0x01180070u, .yclip = 0x01c001a0u,
        .target = 0x00a33000u,
        .variable_vertex_alpha = true,
        .tile_x0 = 0x0eu, .tile_x1 = 0x22u,
        .tile_y0 = 0x1au, .tile_y1 = 0x1bu,
        .left = 114u, .top = 417u, .width = 161u, .height = 30u,
        .source_stride = 0x2a0u, .source_control = 0x0e280000u,
        .quad = {
            0xe0000000u, 0xa5218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x42e40000u, 0x43df8000u, 0x42e40000u, 0x43d08000u,
            0x43898000u, 0x43df8000u, 0x43898000u, 0x43d08000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0u, 0u, 0x3f700000u, 0x3de40000u,
            0x3edf8000u, 0u, 0u, 0u,
            0x3de40000u, 0x3ed08000u, 0u, 0x3f210000u,
            0x3f700000u, 0x3e898000u, 0x3edf8000u, 0u,
            0x3f210000u, 0u, 0x3e898000u, 0x3ed08000u,
        },
    },
    {
        .xclip = 0x01180070u, .yclip = 0x01c001a0u,
        .target = 0x00998000u,
        .variable_vertex_alpha = true,
        .tile_x0 = 0x0eu, .tile_x1 = 0x22u,
        .tile_y0 = 0x1au, .tile_y1 = 0x1bu,
        .left = 114u, .top = 417u, .width = 161u, .height = 30u,
        .source_stride = 0x2a0u, .source_control = 0x0e280000u,
        .quad = {
            0xe0000000u, 0xa5218001u, 0u, 0xcd206c40u,
            0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
            0x42e40000u, 0x43df8000u, 0x42e40000u, 0x43d08000u,
            0x43898000u, 0x43df8000u, 0x43898000u, 0x43d08000u,
            0u, 0u, 0u, 0u,
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
            0u, 0u, 0x3f700000u, 0x3de40000u,
            0x3edf8000u, 0u, 0u, 0u,
            0x3de40000u, 0x3ed08000u, 0u, 0x3f210000u,
            0x3f700000u, 0x3e898000u, 0x3edf8000u, 0u,
            0x3f210000u, 0u, 0x3e898000u, 0x3ed08000u,
        },
    },
};

static const struct mbx_3d_status_form *
mbx_3d_find_status_form(const s5l_mbx_t *m) {
    uint32_t xclip = m->reg[S5L_MBX_FBXCLIP / 4u];
    uint32_t yclip = m->reg[S5L_MBX_FBYCLIP / 4u];
    uint32_t target = m->reg[S5L_MBX_FBSTART / 4u];
    for (unsigned i = 0;
         i < sizeof mbx_3d_status_forms / sizeof mbx_3d_status_forms[0]; i++) {
        const struct mbx_3d_status_form *form = &mbx_3d_status_forms[i];
        if (form->xclip == xclip && form->yclip == yclip &&
            (!form->target || form->target == target))
            return form;
    }
    return NULL;
}

static uint32_t
mbx_3d_status_boundary_expected(const struct mbx_3d_status_form *form,
                                 uint32_t off) {
    if (off >= 0x0b8u && off <= 0x0d4u)
        return form->boundary_override
            ? form->boundary[(off - 0x0b8u) / 4u]
            : form->quad[8u + (off - 0x0b8u) / 4u];
    return mbx_3d_boundary_fixed_expected(off);
}

static bool mbx_execute_status_sprite(s5l_mbx_t *m,
                                      const arm_bus_t *bus,
                                      const char **why,
                                      uint32_t *pixels_blended) {
    uint32_t region = m->reg[S5L_MBX_RGNBASE / 4u];
    uint32_t object = m->reg[S5L_MBX_OBJBASE / 4u];
    uint32_t target = m->reg[S5L_MBX_FBSTART / 4u];

    if ((region & 3u) || (object & 3u) || (target & 3u) ||
        object > UINT32_MAX - 0x2a0u) {
        if (why) *why = "status-sprite region, object, or framebuffer base is invalid";
        return false;
    }
    const struct mbx_3d_status_form *form = mbx_3d_find_status_form(m);
    if (m->reg[S5L_MBX_3DPIXSAMP / 4u] != 0x00020007u ||
        m->reg[S5L_MBX_FBCTL / 4u] != 0x00000006u ||
        m->reg[S5L_MBX_FBLINESTRIDE / 4u] != MBX_3D_WIDTH || !form) {
        if (why) *why = "render registers are not a captured status-sprite form";
        return false;
    }

    uint32_t list = object + 0x68u;
    uint32_t tile_count = (form->tile_x1 - form->tile_x0 + 1u) *
                          (form->tile_y1 - form->tile_y0 + 1u);
    uint32_t tile_index = 0u;
    for (uint32_t y = form->tile_y0; y <= form->tile_y1; y++) {
        for (uint32_t x = form->tile_x0; x <= form->tile_x1; x++) {
            uint32_t code, pointer;
            if (!mbx_gart_u32(m, bus, region + tile_index * 8u,
                              &code, why) ||
                !mbx_gart_u32(m, bus, region + tile_index * 8u + 4u,
                              &pointer, why))
                return false;
            uint32_t expected = (y << 8) | x;
            if (tile_index + 1u == tile_count) expected |= 0x80000000u;
            if (code != expected || pointer != list) {
                if (why) *why = "status-sprite region list differs from its captured tiles";
                return false;
            }
            tile_index++;
        }
    }

    const uint32_t list_words[4] = {
        0x60200020u, 0x6020002du, 0x61a0007cu, 0xf0000000u
    };
    for (unsigned i = 0; i < 4u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, list + i * 4u, &value, why)) return false;
        if (value != list_words[i]) {
            if (why) *why = "status-sprite list is not the captured three-object list";
            return false;
        }
    }

    static const uint32_t background[26] = {
        0xe0000000u, 0xa7718000u, 0u, 0xd6887610u,
        0x22220e80u, 0u, 0u, 0x45000000u,
        0u, 0u, 0x45000000u, 0x3f800000u,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
        0x3f800000u, 0u, 0u, 0u,
        0u, 0x40000000u, 0u, 0u,
        0u, 0x40000000u,
    };
    for (unsigned i = 0; i < 26u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + i * 4u, &value, why)) return false;
        if (i == 2u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                mbx_3d_decode_address(value) != target) {
                if (why) *why = "status background does not resolve to FBSTART";
                return false;
            }
        } else if (value != background[i]) {
            if (why) *why = "status background object differs from the captured form";
            return false;
        }
    }

    for (uint32_t off = 0x80u; off < 0x1f0u; off += 4u) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + off, &value, why)) return false;
        if (value != mbx_3d_status_boundary_expected(form, off)) {
            if (why) *why = "status boundary object differs from the captured form";
            return false;
        }
    }

    uint32_t source = 0u;
    uint32_t vertex_alpha_word = 0u;
    bool vertex_alpha_seen = false;
    bool quad_matches = true;
    for (unsigned i = 0; i < 44u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + 0x1f0u + i * 4u,
                          &value, why))
            return false;
        if (i == 2u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != form->source_control) {
                if (why) *why = "status source address word has an unknown format";
                return false;
            }
            source = mbx_3d_decode_address(value);
        } else if (i == 5u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                mbx_3d_decode_address(value) != target) {
                if (why) *why = "status blend surface differs from the captured form";
                return false;
            }
        } else if (i == 24u || i == 29u || i == 34u || i == 39u) {
            if (!vertex_alpha_seen) {
                vertex_alpha_word = value;
                vertex_alpha_seen = true;
            }
            if (form->variable_vertex_alpha) {
                if (value & 0x00ffffffu) {
                    if (why)
                        *why = "status vertex alpha has nonzero colour bits";
                    return false;
                }
                if (value != vertex_alpha_word) {
                    if (why)
                        *why = "status vertex alpha differs between vertices";
                    return false;
                }
            } else {
                quad_matches = quad_matches && value == form->quad[i];
            }
        } else {
            quad_matches = quad_matches && value == form->quad[i];
        }
    }
    if (!source) {
        if (why) *why = "status source resolves to GPU address zero";
        return false;
    }
    if (!vertex_alpha_seen) {
        if (why) *why = "status sprite has no vertex alpha";
        return false;
    }
    if (!quad_matches) {
        if (why) *why = "textured status sprite differs from its captured form";
        return false;
    }

    uint32_t row_bytes = form->width * 4u;
    uint32_t total = row_bytes * form->height;
    uint64_t source_end = (uint64_t)source +
                          (uint64_t)(form->source_row0 + form->height - 1u) *
                              form->source_stride + row_bytes;
    uint64_t target_end = (uint64_t)target +
                          (uint64_t)(form->top + form->height - 1u) *
                              MBX_3D_TARGET_STRIDE +
                           (uint64_t)(form->left + form->width) * 4u;
    if (row_bytes > form->source_stride || source_end > UINT32_MAX ||
        target_end > UINT32_MAX) {
        if (why) *why = "status source or destination rectangle overflows";
        return false;
    }
    for (uint32_t row = 0; row < form->height; row++) {
        uint32_t src = source +
                       (form->source_row0 + row) * form->source_stride;
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        uint32_t background = target +
                              (form->top + row) * MBX_3D_TARGET_STRIDE +
                              form->left * 4u;
        if (!mbx_gart_validate(m, bus, src, row_bytes, why) ||
            !mbx_gart_validate(m, bus, background, row_bytes, why) ||
            !mbx_gart_validate(m, bus, dst, row_bytes, why))
            return false;
    }

    uint8_t *source_pixels = malloc(total);
    uint8_t *pixels = malloc(total);
    if (!source_pixels || !pixels) {
        free(source_pixels);
        free(pixels);
        if (why) *why = "host allocation for staged status sprite failed";
        return false;
    }
    bool ok = true;
    for (uint32_t row = 0; row < form->height && ok; row++) {
        uint32_t src = source +
                       (form->source_row0 + row) * form->source_stride;
        uint32_t background = target +
                              (form->top + row) * MBX_3D_TARGET_STRIDE +
                              form->left * 4u;
        ok = mbx_gart_read(m, bus, src, source_pixels + row * row_bytes,
                           row_bytes, why) &&
             mbx_gart_read(m, bus, background, pixels + row * row_bytes,
                           row_bytes, why);
    }
    for (uint32_t i = 0; i < total && ok; i += 4u) {
        uint32_t src = mbx_load_le32(source_pixels + i);
        uint32_t alpha = src >> 24;
        if ((src & 0xffu) > alpha || ((src >> 8) & 0xffu) > alpha ||
            ((src >> 16) & 0xffu) > alpha) {
            if (why) *why = "status source is not premultiplied BGRA8";
            ok = false;
            break;
        }
        src = mbx_modulate_vertex_alpha(src, vertex_alpha_word >> 24);
        uint32_t blended = mbx_premultiplied_over(
            mbx_load_le32(pixels + i), src);
        pixels[i] = (uint8_t)blended;
        pixels[i + 1u] = (uint8_t)(blended >> 8);
        pixels[i + 2u] = (uint8_t)(blended >> 16);
        pixels[i + 3u] = (uint8_t)(blended >> 24);
    }
    for (uint32_t row = 0; row < form->height && ok; row++) {
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        ok = mbx_gart_write(m, bus, dst, pixels + row * row_bytes,
                            row_bytes, why);
    }
    free(source_pixels);
    free(pixels);
    if (ok && pixels_blended)
        *pixels_blended = form->width * form->height;
    return ok;
}

/* r409-r412 initially supplied four literal app-icon/label forms. Reading the
 * shipped _mbx3DCtxQuadCopyPerspective producer explains the shared packet
 * instead: it is a 1:1, axis-aligned BGRA8 sprite whose independently encoded
 * destination, UV, texture-power, pitch, clip and tile rectangles must agree.
 *
 * The texture header stores log2(power-of-two dimension)-3 in nibbles 6 and 5.
 * Linear pitch is split because the source address consumes bits 0..17 of its
 * word: pitch_bytes/16 bits 2..7 remain in source-control bits 18..23, while
 * bit 1 is relocated to texture-header bit 0. All measured linear textures
 * use an eight-pixel padded pitch, and that reconstruction yields every
 * captured 0x40..0x500 stride exactly. r415 adds the producer's second vertex
 * order: control 0x0e uses full width/power and height/power UV extents, while
 * control 0x8e uses the previously recovered half-texel extents. Both orders
 * redundantly encode the same axis-aligned destination and are checked in
 * full. The second captured sampler carries one uniform alpha byte, which uses
 * the already recovered channel-modulation equation. The background object,
 * blend object and FBSTART must all resolve to the same mapped target, but its
 * GPU address is not a rendering semantic and is therefore not whitelisted.
 * This is not a perspective/scaling rasterizer; coloured vertices remain
 * rejected. r416's partly off-screen label is clipped only when normalized
 * coordinates, a strict one-to-one edge crop, integer boundary, clip and tiles
 * all independently agree. */
static bool mbx_3d_word_to_finite_float(uint32_t word, float *value) {
    if ((word & 0x7f800000u) == 0x7f800000u ||
        sizeof *value != sizeof word)
        return false;
    memcpy(value, &word, sizeof *value);
    return true;
}

static bool mbx_3d_word_to_nonnegative_float(uint32_t word, float *value) {
    return !(word & 0x80000000u) &&
           mbx_3d_word_to_finite_float(word, value);
}

static uint32_t mbx_3d_float_to_word(float value) {
    uint32_t word = 0u;
    memcpy(&word, &value, sizeof word);
    return word;
}

static int32_t mbx_3d_ceil_to_i32(float value) {
    int32_t integer = (int32_t)value;
    return integer + ((float)integer < value);
}

/* r414 exposed why the object-list word is not merely another packet
 * discriminator.  Its low twenty bits are a word offset from OBJBASE:
 * 0x61a0007c references the textured object at +0x1f0, while 0x612000a8
 * references a different object at +0x2a0.  The latter is the five-record
 * form emitted by the shipped _mbx3DCtxQuadColor producer.  The apparently
 * related texture object at +0x1f0 is stale and must not be executed.
 *
 * This decoder follows that pointer, then requires the independently encoded
 * main geometry, normalized coordinates, boundary object, clip registers and
 * row-major tile list to agree.  Disassembly of the shipped _mbx3DQuadColor
 * wrapper and producer proves that its second argument is copied to the main
 * record before every normalized vertex pair.  Those four words must be one
 * uniform premultiplied A8R8G8B8 colour.  The four trailing parameter records
 * carry fixed all-one words and exact controls; they are not the quad colour.
 * Non-axis-aligned quads remain rejected. */
static bool mbx_execute_solid_quad(s5l_mbx_t *m,
                                   const arm_bus_t *bus,
                                   const char **why,
                                   uint32_t *pixels_filled) {
    uint32_t region = m->reg[S5L_MBX_RGNBASE / 4u];
    uint32_t object = m->reg[S5L_MBX_OBJBASE / 4u];
    uint32_t target = m->reg[S5L_MBX_FBSTART / 4u];
    if ((region & 3u) || (object & 3u) || (target & 3u) ||
        object > UINT32_MAX - 0x68u) {
        if (why) *why = "solid-quad region, object, or framebuffer base is invalid";
        return false;
    }
    if (m->reg[S5L_MBX_3DPIXSAMP / 4u] != 0x00020007u ||
        m->reg[S5L_MBX_FBCTL / 4u] != 0x00000006u ||
        m->reg[S5L_MBX_FBLINESTRIDE / 4u] != MBX_3D_WIDTH) {
        if (why) *why = "render registers are not the measured solid-quad family";
        return false;
    }

    uint32_t list = object + 0x68u;
    uint32_t list_words[4];
    for (unsigned i = 0; i < 4u; i++)
        if (!mbx_gart_u32(m, bus, list + i * 4u, &list_words[i], why))
            return false;
    bool solid_pointer =
        (list_words[2] & 0xfff00000u) == 0x61200000u;
    if (list_words[0] != 0x60200020u ||
        list_words[1] != 0x6020002du || !solid_pointer ||
        list_words[3] != 0xf0000000u) {
        /* Preserve the textured decoder's more relevant reason when this is
         * plainly its 0x61a pointer family. */
        if (why && solid_pointer)
            *why = "solid-quad list is not the measured pointer form";
        return false;
    }
    uint64_t solid64 = (uint64_t)object +
                       (uint64_t)(list_words[2] & 0x000fffffu) * 4u;
    if (solid64 < (uint64_t)object + 0x1f0u ||
        solid64 + 5u * 33u * 4u > (uint64_t)UINT32_MAX + 1u) {
        if (why) *why = "solid-quad object pointer is outside its safe span";
        return false;
    }
    uint32_t solid = (uint32_t)solid64;

    uint32_t main[33];
    for (unsigned i = 0; i < 33u; i++)
        if (!mbx_gart_u32(m, bus, solid + i * 4u, &main[i], why))
            return false;
    if (main[0] != 0xe0000000u || main[1] != 0xa7718000u ||
        main[3] != 0xa6104620u || main[4] != 0x22220e80u) {
        if (why) *why = "solid-quad main record has unknown controls";
        return false;
    }
    if ((main[2] & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
        mbx_3d_decode_address(main[2]) != target) {
        if (why) *why = "solid-quad main record does not resolve to FBSTART";
        return false;
    }
    for (unsigned i = 13u; i < 17u; i++) {
        if (main[i] != 0u) {
            if (why) *why = "solid-quad perspective terms are nonzero";
            return false;
        }
    }
    for (unsigned i = 17u; i < 21u; i++) {
        if (main[i] != 0x3f800000u) {
            if (why) *why = "solid-quad perspective divisors are not one";
            return false;
        }
    }
    if (main[5] != main[7] || main[5] == main[9] ||
        main[9] != main[11] || main[6] != main[10] ||
        main[6] == main[8] || main[8] != main[12]) {
        if (why) *why = "solid-quad destination is not an axis-aligned rectangle";
        return false;
    }

    float x0, y0, x1, y1;
    if (!mbx_3d_word_to_nonnegative_float(main[5], &x0) ||
        !mbx_3d_word_to_nonnegative_float(main[8], &y0) ||
        !mbx_3d_word_to_nonnegative_float(main[9], &x1) ||
        !mbx_3d_word_to_nonnegative_float(main[6], &y1) ||
        x0 >= x1 || y0 >= y1 || x1 > 320.0f || y1 > 480.0f) {
        if (why) *why = "solid-quad destination coordinates are invalid";
        return false;
    }

    static const unsigned x_words[4] = {5u, 7u, 9u, 11u};
    static const unsigned y_words[4] = {6u, 8u, 10u, 12u};
    uint32_t colour = main[21];
    uint32_t alpha = colour >> 24;
    if ((colour & 0xffu) > alpha || ((colour >> 8) & 0xffu) > alpha ||
        ((colour >> 16) & 0xffu) > alpha) {
        if (why) *why = "solid-quad colour is not premultiplied A8R8G8B8";
        return false;
    }
    for (unsigned vertex = 0; vertex < 4u; vertex++) {
        float x, y;
        unsigned attribute = 21u + vertex * 3u;
        if (!mbx_3d_word_to_nonnegative_float(main[x_words[vertex]], &x) ||
            !mbx_3d_word_to_nonnegative_float(main[y_words[vertex]], &y) ||
            main[attribute] != colour ||
            main[attribute + 1u] != mbx_3d_float_to_word(x / 1024.0f) ||
            main[attribute + 2u] != mbx_3d_float_to_word(y / 1024.0f)) {
            if (why) *why =
                "solid-quad colour or normalized coordinates differ between vertices";
            return false;
        }
    }

    const float lower_bias = 0.468505859375f; /* producer 0x3eefe000 */
    const float upper_bias = 0.531494140625f; /* producer 0x3f081000 */
    uint32_t left = (uint32_t)(x0 + lower_bias);
    uint32_t top = (uint32_t)(y0 + lower_bias);
    uint32_t right = (uint32_t)(x1 + upper_bias);
    uint32_t bottom = (uint32_t)(y1 + upper_bias);
    if (left >= right || top >= bottom || right > MBX_3D_WIDTH ||
        bottom > 480u) {
        if (why) *why = "solid-quad producer bounds leave the 320x480 surface";
        return false;
    }
    uint32_t clip_left = left & ~7u;
    uint32_t clip_right = (right + 7u) & ~7u;
    uint32_t clip_top = top & ~15u;
    uint32_t clip_bottom = (bottom + 15u) & ~15u;
    if (m->reg[S5L_MBX_FBXCLIP / 4u] !=
            ((clip_right << 16) | clip_left) ||
        m->reg[S5L_MBX_FBYCLIP / 4u] !=
            ((clip_bottom << 16) | clip_top)) {
        if (why) *why = "solid-quad clip registers disagree with producer geometry";
        return false;
    }

    uint32_t tile_x0 = clip_left / 8u;
    uint32_t tile_x1 = clip_right / 8u - 1u;
    uint32_t tile_y0 = clip_top / 16u;
    uint32_t tile_y1 = clip_bottom / 16u - 1u;
    uint32_t tile_count = (tile_x1 - tile_x0 + 1u) *
                          (tile_y1 - tile_y0 + 1u);
    if (!tile_count || tile_count > 40u * 30u ||
        (uint64_t)region + (uint64_t)tile_count * 8u > UINT32_MAX) {
        if (why) *why = "solid-quad tile rectangle is invalid";
        return false;
    }
    uint32_t tile_index = 0u;
    for (uint32_t y = tile_y0; y <= tile_y1; y++) {
        for (uint32_t x = tile_x0; x <= tile_x1; x++) {
            uint32_t code, pointer;
            if (!mbx_gart_u32(m, bus, region + tile_index * 8u,
                              &code, why) ||
                !mbx_gart_u32(m, bus, region + tile_index * 8u + 4u,
                              &pointer, why))
                return false;
            uint32_t expected = (y << 8) | x;
            if (tile_index + 1u == tile_count) expected |= 0x80000000u;
            if (code != expected || pointer != list) {
                if (why) *why = "solid-quad region list disagrees with its clip tiles";
                return false;
            }
            tile_index++;
        }
    }

    static const uint32_t background[26] = {
        0xe0000000u, 0xa7718000u, 0u, 0xd6887610u,
        0x22220e80u, 0u, 0u, 0x45000000u,
        0u, 0u, 0x45000000u, 0x3f800000u,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
        0x3f800000u, 0u, 0u, 0u,
        0u, 0x40000000u, 0u, 0u,
        0u, 0x40000000u,
    };
    for (unsigned i = 0; i < 26u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + i * 4u, &value, why))
            return false;
        if (i == 2u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                mbx_3d_decode_address(value) != target) {
                if (why) *why = "solid-quad background does not resolve to FBSTART";
                return false;
            }
        } else if (value != background[i]) {
            if (why) *why = "solid-quad background object is unknown";
            return false;
        }
    }

    for (uint32_t off = 0x80u; off < 0x1f0u; off += 4u) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + off, &value, why)) return false;
        uint32_t expected = mbx_3d_boundary_fixed_expected(off);
        if (off >= 0x0b8u && off <= 0x0d4u)
            expected = main[5u + (off - 0x0b8u) / 4u];
        if (value != expected) {
            if (why) *why = "solid-quad boundary object disagrees with its geometry";
            return false;
        }
    }

    static const uint32_t parameter_controls[4] = {
        0x22620ea0u, 0x46622ea0u, 0x66622ea0u, 0x82622ea0u
    };
    for (unsigned record = 0; record < 4u; record++) {
        uint32_t base = solid + (record + 1u) * 33u * 4u;
        for (unsigned i = 0; i < 33u; i++) {
            uint32_t value;
            if (!mbx_gart_u32(m, bus, base + i * 4u, &value, why))
                return false;
            uint32_t expected = 0u;
            if (i == 0u) expected = 0xe0000000u;
            else if (i == 3u) expected = 0x86084610u;
            else if (i == 4u) expected = parameter_controls[record];
            else if (i >= 17u && i <= 20u) expected = 0x3f800000u;
            else if (i >= 21u && (i - 21u) % 3u == 0u)
                expected = 0xffffffffu;
            if (value != expected) {
                if (why) *why = "solid-quad parameter records are inconsistent";
                return false;
            }
        }
    }

    uint32_t width = right - left;
    uint32_t height = bottom - top;
    uint32_t row_bytes = width * 4u;
    uint32_t total = row_bytes * height;
    uint64_t target_end = (uint64_t)target +
        (uint64_t)(bottom - 1u) * MBX_3D_TARGET_STRIDE +
        (uint64_t)right * 4u;
    if (target_end > UINT32_MAX) {
        if (why) *why = "solid-quad destination rectangle overflows";
        return false;
    }
    for (uint32_t row = 0; row < height; row++) {
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        if (!mbx_gart_validate(m, bus, dst, row_bytes, why)) return false;
    }

    uint8_t *pixels = malloc(total);
    if (!pixels) {
        if (why) *why = "host allocation for staged solid quad failed";
        return false;
    }
    bool ok = true;
    for (uint32_t row = 0; row < height && ok; row++) {
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        ok = mbx_gart_read(m, bus, dst, pixels + row * row_bytes,
                           row_bytes, why);
    }
    for (uint32_t i = 0; i < total && ok; i += 4u) {
        uint32_t blended = mbx_premultiplied_over(
            mbx_load_le32(pixels + i), colour);
        pixels[i] = (uint8_t)blended;
        pixels[i + 1u] = (uint8_t)(blended >> 8);
        pixels[i + 2u] = (uint8_t)(blended >> 16);
        pixels[i + 3u] = (uint8_t)(blended >> 24);
    }
    for (uint32_t row = 0; row < height && ok; row++) {
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        ok = mbx_gart_write(m, bus, dst, pixels + row * row_bytes,
                            row_bytes, why);
    }
    free(pixels);
    if (ok && pixels_filled) *pixels_filled = width * height;
    return ok;
}

static bool mbx_execute_axis_aligned_sprite(s5l_mbx_t *m,
                                            const arm_bus_t *bus,
                                            const char **why,
                                            uint32_t *pixels_blended) {
    uint32_t region = m->reg[S5L_MBX_RGNBASE / 4u];
    uint32_t object = m->reg[S5L_MBX_OBJBASE / 4u];
    uint32_t target = m->reg[S5L_MBX_FBSTART / 4u];
    if ((region & 3u) || (object & 3u) || (target & 3u) ||
        object > UINT32_MAX - 0x2a0u) {
        if (why) *why = "sprite region, object, or framebuffer base is invalid";
        return false;
    }
    if (m->reg[S5L_MBX_3DPIXSAMP / 4u] != 0x00020007u ||
        m->reg[S5L_MBX_FBCTL / 4u] != 0x00000006u ||
        m->reg[S5L_MBX_FBLINESTRIDE / 4u] != MBX_3D_WIDTH) {
        if (why) *why = "render registers are not the measured 1:1 sprite family";
        return false;
    }

    uint32_t list = object + 0x68u;
    static const uint32_t list_words[4] = {
        0x60200020u, 0x6020002du, 0x61a0007cu, 0xf0000000u
    };
    for (unsigned i = 0; i < 4u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, list + i * 4u, &value, why)) return false;
        if (value != list_words[i]) {
            if (why) *why = "sprite list is not the measured three-object list";
            return false;
        }
    }

    static const uint32_t background[26] = {
        0xe0000000u, 0xa7718000u, 0u, 0xd6887610u,
        0x22220e80u, 0u, 0u, 0x45000000u,
        0u, 0u, 0x45000000u, 0x3f800000u,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
        0x3f800000u, 0u, 0u, 0u,
        0u, 0x40000000u, 0u, 0u,
        0u, 0x40000000u,
    };
    for (unsigned i = 0; i < 26u; i++) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + i * 4u, &value, why)) return false;
        if (i == 2u) {
            if ((value & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
                mbx_3d_decode_address(value) != target) {
                if (why) *why = "sprite background does not resolve to FBSTART";
                return false;
            }
        } else if (value != background[i]) {
            if (why) *why = "sprite background object is unknown";
            return false;
        }
    }

    uint32_t quad[44];
    for (unsigned i = 0; i < 44u; i++)
        if (!mbx_gart_u32(m, bus, object + 0x1f0u + i * 4u,
                          &quad[i], why))
            return false;
    bool direct_sampler = quad[3] == 0xa6884710u;
    bool modulated_sampler = quad[3] == 0xcd206c40u;
    if (quad[0] != 0xe0000000u ||
        (!direct_sampler && !modulated_sampler) ||
        quad[4] != 0xa7718000u || quad[6] != 0xae504ea0u ||
        quad[7] != 0x22250e80u) {
        if (why) *why = "sprite quad setup is not the measured BGRA8 source-over form";
        return false;
    }
    if ((quad[5] & ~MBX_3D_ADDRESS_MASK) != 0x0e500000u ||
        mbx_3d_decode_address(quad[5]) != target) {
        if (why) *why = "sprite blend surface differs from FBSTART";
        return false;
    }
    for (unsigned i = 16u; i < 20u; i++) {
        if (quad[i] != 0u) {
            if (why) *why = "sprite perspective terms are nonzero";
            return false;
        }
    }
    for (unsigned i = 20u; i < 24u; i++) {
        if (quad[i] != 0x3f800000u) {
            if (why) *why = "sprite perspective divisors are not one";
            return false;
        }
    }
    static const unsigned alpha_words[4] = {24u, 29u, 34u, 39u};
    uint32_t vertex_alpha_word = quad[alpha_words[0]];
    if ((vertex_alpha_word & 0x00ffffffu) ||
        (direct_sampler && vertex_alpha_word != 0xff000000u)) {
        if (why) *why = "sprite vertex modulation is invalid for its sampler";
        return false;
    }
    for (unsigned i = 1u; i < 4u; i++) {
        if (quad[alpha_words[i]] != vertex_alpha_word) {
            if (why) *why = "sprite vertex modulation differs between vertices";
            return false;
        }
    }

    uint32_t source_control = quad[2] & ~MBX_3D_ADDRESS_MASK;
    bool half_texel_layout = (source_control & 0x80000000u) != 0u;
    if (!half_texel_layout && !modulated_sampler) {
        if (why) *why = "full-extent sprite layout has an unmeasured sampler";
        return false;
    }
    bool axis_aligned = half_texel_layout
        ? quad[8] == quad[12] && quad[10] == quad[14] &&
          quad[9] == quad[11] && quad[13] == quad[15]
        : quad[8] == quad[10] && quad[12] == quad[14] &&
          quad[9] == quad[13] && quad[11] == quad[15];
    if (!axis_aligned) {
        if (why) *why = "sprite destination is not an axis-aligned quad";
        return false;
    }
    uint32_t x0_word = quad[8];
    uint32_t y0_word = half_texel_layout ? quad[9] : quad[11];
    uint32_t x1_word = half_texel_layout ? quad[10] : quad[12];
    uint32_t y1_word = half_texel_layout ? quad[13] : quad[9];
    float x0, y0, x1, y1;
    if (!mbx_3d_word_to_finite_float(x0_word, &x0) ||
        !mbx_3d_word_to_finite_float(y0_word, &y0) ||
        !mbx_3d_word_to_finite_float(x1_word, &x1) ||
        !mbx_3d_word_to_finite_float(y1_word, &y1) ||
        x0 >= x1 || y0 >= y1) {
        if (why) *why = "sprite destination coordinates are invalid";
        return false;
    }

    static const unsigned destination_words[8] = {
        8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u
    };
    static const unsigned normalized_words[8] = {
        27u, 28u, 32u, 33u, 37u, 38u, 42u, 43u
    };
    for (unsigned i = 0; i < 8u; i++) {
        float coordinate;
        if (!mbx_3d_word_to_finite_float(
                quad[destination_words[i]], &coordinate) ||
            quad[normalized_words[i]] !=
                mbx_3d_float_to_word(coordinate / 1024.0f)) {
            if (why) *why = "sprite normalized destination disagrees with its quad";
            return false;
        }
    }

    /* These are the exact positive float literals loaded at 0x30e1d258/25c
     * by _mbx3DCtxQuadCopyPerspective before VCVT.U32.F32 and subsequent 8x16
     * tile alignment. The producer first intersects its float extrema with the
     * context bounds. The surface intersection below avoids relying on a C
     * conversion whose negative-input behaviour differs from ARM saturation;
     * the boundary object then supplies any stricter integer context scissor. */
    const float lower_bias = 0.468505859375f; /* 0x3eefe000 */
    const float upper_bias = 0.531494140625f; /* 0x3f081000 */
    const float epsilon = 0.0009765625f;
    float dx = x1 - x0;
    float dy = y1 - y0;
    if (dx < 1.0f - epsilon || dy < 1.0f - epsilon ||
        dx > 512.0f + epsilon || dy > 512.0f + epsilon) {
        if (why) *why = "sprite transform is outside the bounded 1:1 family";
        return false;
    }
    uint32_t source_width = (uint32_t)(dx + 0.5f);
    uint32_t source_height = (uint32_t)(dy + 0.5f);
    if (dx < (float)source_width - epsilon ||
        dx > (float)source_width + epsilon ||
        dy < (float)source_height - epsilon ||
        dy > (float)source_height + epsilon) {
        if (why) *why = "sprite transform is not a measured 1:1 copy";
        return false;
    }
    float bounded_x0 = x0 < 0.0f ? 0.0f : x0;
    float bounded_y0 = y0 < 0.0f ? 0.0f : y0;
    float bounded_x1 = x1 > (float)MBX_3D_WIDTH
        ? (float)MBX_3D_WIDTH : x1;
    float bounded_y1 = y1 > 480.0f ? 480.0f : y1;
    if (bounded_x0 >= bounded_x1 || bounded_y0 >= bounded_y1) {
        if (why) *why = "sprite does not intersect the 320x480 surface";
        return false;
    }

    uint32_t natural_left = (uint32_t)bounded_x0;
    uint32_t natural_top = (uint32_t)bounded_y0;
    uint32_t natural_right = (uint32_t)bounded_x1 +
        ((float)(uint32_t)bounded_x1 != bounded_x1);
    uint32_t natural_bottom = (uint32_t)bounded_y1 +
        ((float)(uint32_t)bounded_y1 != bounded_y1);
    uint32_t boundary[8] = {0};
    for (uint32_t off = 0x80u; off < 0x1f0u; off += 4u) {
        uint32_t value;
        if (!mbx_gart_u32(m, bus, object + off, &value, why)) return false;
        if (off >= 0x0b8u && off <= 0x0d4u) {
            float coordinate;
            if (!mbx_3d_word_to_nonnegative_float(value, &coordinate) ||
                coordinate > 480.0f ||
                coordinate != (float)(uint32_t)coordinate) {
                if (why) *why =
                    "sprite boundary object is not an integer clipped quad";
                return false;
            }
            boundary[(off - 0x0b8u) / 4u] = (uint32_t)coordinate;
        } else if (value != mbx_3d_boundary_fixed_expected(off)) {
            if (why) *why = "sprite boundary object setup is unknown";
            return false;
        }
    }
    uint32_t boundary_left = boundary[0];
    uint32_t boundary_bottom = boundary[1];
    uint32_t boundary_top = boundary[3];
    uint32_t boundary_right = boundary[4];
    if (boundary[2] != boundary_left || boundary[5] != boundary_bottom ||
        boundary[6] != boundary_right || boundary[7] != boundary_top ||
        boundary_left >= boundary_right || boundary_top >= boundary_bottom ||
        boundary_right > MBX_3D_WIDTH || boundary_bottom > 480u ||
        boundary_left < natural_left || boundary_top < natural_top ||
        boundary_right > natural_right || boundary_bottom > natural_bottom) {
        if (why) *why =
            "sprite boundary object is not a clipped subset of its quad";
        return false;
    }

    /* An inward edge is the integer context scissor measured in r418. Natural
     * floor/ceil edges retain the original float extrema so the producer's
     * asymmetric guard rounding remains independently checkable. */
    float producer_x0 = boundary_left > natural_left
        ? (float)boundary_left : bounded_x0;
    float producer_y0 = boundary_top > natural_top
        ? (float)boundary_top : bounded_y0;
    float producer_x1 = boundary_right < natural_right
        ? (float)boundary_right : bounded_x1;
    float producer_y1 = boundary_bottom < natural_bottom
        ? (float)boundary_bottom : bounded_y1;
    uint32_t guard_left = (uint32_t)(producer_x0 + lower_bias);
    uint32_t guard_top = (uint32_t)(producer_y0 + lower_bias);
    uint32_t guard_right = (uint32_t)(producer_x1 + upper_bias);
    uint32_t guard_bottom = (uint32_t)(producer_y1 + upper_bias);
    if (guard_left >= guard_right || guard_top >= guard_bottom ||
        guard_right > MBX_3D_WIDTH || guard_bottom > 480u) {
        if (why) *why = "sprite producer bounds leave the 320x480 surface";
        return false;
    }

    /* The values above are conservative tile/clip bounds, not fragment
     * coverage. Standard pixel-centre coverage for the axis-aligned producer
     * quad is [ceil(min - 0.5), ceil(max - 0.5)). r417 is the first capture
     * where the producer's asymmetric guard biases add a row that this raster
     * interval does not cover. Keep the two rectangles independent. */
    int32_t raster_left_unclipped = mbx_3d_ceil_to_i32(x0 - 0.5f);
    int32_t raster_top_unclipped = mbx_3d_ceil_to_i32(y0 - 0.5f);
    int32_t raster_right_unclipped = mbx_3d_ceil_to_i32(x1 - 0.5f);
    int32_t raster_bottom_unclipped = mbx_3d_ceil_to_i32(y1 - 0.5f);
    int32_t raster_left = raster_left_unclipped < 0
        ? 0 : raster_left_unclipped;
    int32_t raster_top = raster_top_unclipped < 0
        ? 0 : raster_top_unclipped;
    int32_t raster_right = raster_right_unclipped > (int32_t)MBX_3D_WIDTH
        ? (int32_t)MBX_3D_WIDTH : raster_right_unclipped;
    int32_t raster_bottom = raster_bottom_unclipped > 480
        ? 480 : raster_bottom_unclipped;
    if (raster_left < (int32_t)boundary_left)
        raster_left = (int32_t)boundary_left;
    if (raster_top < (int32_t)boundary_top)
        raster_top = (int32_t)boundary_top;
    if (raster_right > (int32_t)boundary_right)
        raster_right = (int32_t)boundary_right;
    if (raster_bottom > (int32_t)boundary_bottom)
        raster_bottom = (int32_t)boundary_bottom;
    if (raster_left >= raster_right || raster_top >= raster_bottom) {
        if (why) *why = "sprite has no covered pixel centres on the surface";
        return false;
    }
    uint32_t left = (uint32_t)raster_left;
    uint32_t top = (uint32_t)raster_top;
    uint32_t right = (uint32_t)raster_right;
    uint32_t bottom = (uint32_t)raster_bottom;
    uint32_t width = right - left;
    uint32_t height = bottom - top;

    /* This is deliberately narrower than a texture sampler. Pixel-centre
     * coverage of an integer-sized unity transform must span the full source;
     * intersecting it with the surface and context boundary then selects the
     * same contiguous source subrectangle. Scaling and resampling still reject. */
    int32_t full_raster_width =
        raster_right_unclipped - raster_left_unclipped;
    int32_t full_raster_height =
        raster_bottom_unclipped - raster_top_unclipped;
    if (full_raster_width != (int32_t)source_width ||
        full_raster_height != (int32_t)source_height ||
        raster_left < raster_left_unclipped ||
        raster_top < raster_top_unclipped ||
        raster_right > raster_right_unclipped ||
        raster_bottom > raster_bottom_unclipped) {
        if (why) *why =
            "sprite pixel-centre coverage is not a contiguous 1:1 crop";
        return false;
    }
    uint32_t source_x0 =
        (uint32_t)(raster_left - raster_left_unclipped);
    uint32_t source_y0 =
        (uint32_t)(raster_top - raster_top_unclipped);
    if (source_x0 + width > source_width ||
        source_y0 + height > source_height) {
        if (why) *why = "sprite source crop exceeds its unity transform";
        return false;
    }

    uint32_t texture_width = 8u, texture_height = 8u;
    uint32_t width_field = 0u, height_field = 0u;
    while (texture_width < source_width) {
        texture_width <<= 1;
        width_field++;
    }
    while (texture_height < source_height) {
        texture_height <<= 1;
        height_field++;
    }
    if (texture_width > 512u || texture_height > 512u) {
        if (why) *why = "sprite texture power exceeds the measured screen bounds";
        return false;
    }
    uint32_t padded_width = (source_width + 7u) & ~7u;
    uint32_t source_stride = padded_width * 4u;
    uint32_t pitch_units = source_stride / 16u;
    uint32_t expected_header = 0xa0018000u |
        (width_field << 24) | (height_field << 20) |
        ((pitch_units & 2u) >> 1);
    uint32_t expected_source_control =
        (half_texel_layout ? 0x8e000000u : 0x0e000000u) |
        ((pitch_units & ~3u) << 16);
    if (quad[1] != expected_header ||
        (quad[2] & ~MBX_3D_ADDRESS_MASK) != expected_source_control) {
        if (why) *why = "sprite texture dimensions or split pitch are inconsistent";
        return false;
    }
    uint32_t source = mbx_3d_decode_address(quad[2]);
    if (!source) {
        if (why) *why = "sprite source resolves to GPU address zero";
        return false;
    }

    float u = half_texel_layout
        ? (float)source_width - 0.5f : (float)source_width;
    float v = half_texel_layout
        ? (float)source_height - 0.5f : (float)source_height;
    uint32_t expected_u = mbx_3d_float_to_word(u / (float)texture_width);
    uint32_t expected_v = mbx_3d_float_to_word(v / (float)texture_height);
    bool uv_matches = half_texel_layout
        ? quad[25] == 0u && quad[26] == 0u &&
          quad[30] == expected_u && quad[31] == 0u &&
          quad[35] == 0u && quad[36] == expected_v &&
          quad[40] == expected_u && quad[41] == expected_v
        : quad[25] == 0u && quad[26] == expected_v &&
          quad[30] == 0u && quad[31] == 0u &&
          quad[35] == expected_u && quad[36] == expected_v &&
          quad[40] == expected_u && quad[41] == 0u;
    if (!uv_matches) {
        if (why) *why = "sprite UV rectangle disagrees with its 1:1 source size";
        return false;
    }

    uint32_t clip_left = guard_left & ~7u;
    uint32_t clip_right = (guard_right + 7u) & ~7u;
    uint32_t clip_top = guard_top & ~15u;
    uint32_t clip_bottom = (guard_bottom + 15u) & ~15u;
    uint32_t expected_xclip = (clip_right << 16) | clip_left;
    uint32_t expected_yclip = (clip_bottom << 16) | clip_top;
    if (m->reg[S5L_MBX_FBXCLIP / 4u] != expected_xclip ||
        m->reg[S5L_MBX_FBYCLIP / 4u] != expected_yclip) {
        if (why) *why = "sprite clip registers disagree with producer geometry";
        return false;
    }

    uint32_t tile_x0 = clip_left / 8u;
    uint32_t tile_x1 = clip_right / 8u - 1u;
    uint32_t tile_y0 = clip_top / 16u;
    uint32_t tile_y1 = clip_bottom / 16u - 1u;
    uint32_t tile_count = (tile_x1 - tile_x0 + 1u) *
                          (tile_y1 - tile_y0 + 1u);
    if (!tile_count || tile_count > 40u * 30u ||
        (uint64_t)region + (uint64_t)tile_count * 8u > UINT32_MAX) {
        if (why) *why = "sprite tile rectangle is invalid";
        return false;
    }
    uint32_t tile_index = 0u;
    for (uint32_t y = tile_y0; y <= tile_y1; y++) {
        for (uint32_t x = tile_x0; x <= tile_x1; x++) {
            uint32_t code, pointer;
            if (!mbx_gart_u32(m, bus, region + tile_index * 8u,
                              &code, why) ||
                !mbx_gart_u32(m, bus, region + tile_index * 8u + 4u,
                              &pointer, why))
                return false;
            uint32_t expected = (y << 8) | x;
            if (tile_index + 1u == tile_count) expected |= 0x80000000u;
            if (code != expected || pointer != list) {
                if (why) *why = "sprite region list disagrees with its clip tiles";
                return false;
            }
            tile_index++;
        }
    }

    uint32_t row_bytes = width * 4u;
    uint32_t total = row_bytes * height;
    uint64_t source_end = (uint64_t)source +
        (uint64_t)(source_y0 + height - 1u) * source_stride +
        (uint64_t)(source_x0 + width) * 4u;
    uint64_t target_end = (uint64_t)target +
        (uint64_t)(top + height - 1u) * MBX_3D_TARGET_STRIDE +
        (uint64_t)(left + width) * 4u;
    if (row_bytes > source_stride || source_end > UINT32_MAX ||
        target_end > UINT32_MAX) {
        if (why) *why = "sprite source or destination rectangle overflows";
        return false;
    }
    for (uint32_t row = 0; row < height; row++) {
        uint32_t src = source + (source_y0 + row) * source_stride +
                       source_x0 * 4u;
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        if (!mbx_gart_validate(m, bus, src, row_bytes, why) ||
            !mbx_gart_validate(m, bus, dst, row_bytes, why))
            return false;
    }

    uint8_t *source_pixels = malloc(total);
    uint8_t *pixels = malloc(total);
    if (!source_pixels || !pixels) {
        free(source_pixels);
        free(pixels);
        if (why) *why = "host allocation for staged sprite failed";
        return false;
    }
    bool ok = true;
    for (uint32_t row = 0; row < height && ok; row++) {
        uint32_t src = source + (source_y0 + row) * source_stride +
                       source_x0 * 4u;
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        ok = mbx_gart_read(m, bus, src,
                           source_pixels + row * row_bytes,
                           row_bytes, why) &&
             mbx_gart_read(m, bus, dst, pixels + row * row_bytes,
                           row_bytes, why);
    }
    for (uint32_t i = 0; i < total && ok; i += 4u) {
        uint32_t src = mbx_load_le32(source_pixels + i);
        uint32_t alpha = src >> 24;
        if ((src & 0xffu) > alpha || ((src >> 8) & 0xffu) > alpha ||
            ((src >> 16) & 0xffu) > alpha) {
            if (why) *why = "sprite source is not premultiplied BGRA8";
            ok = false;
            break;
        }
        src = mbx_modulate_vertex_alpha(src, vertex_alpha_word >> 24);
        uint32_t blended = mbx_premultiplied_over(
            mbx_load_le32(pixels + i), src);
        pixels[i] = (uint8_t)blended;
        pixels[i + 1u] = (uint8_t)(blended >> 8);
        pixels[i + 2u] = (uint8_t)(blended >> 16);
        pixels[i + 3u] = (uint8_t)(blended >> 24);
    }
    for (uint32_t row = 0; row < height && ok; row++) {
        uint32_t dst = target + (top + row) * MBX_3D_TARGET_STRIDE +
                       left * 4u;
        ok = mbx_gart_write(m, bus, dst, pixels + row * row_bytes,
                            row_bytes, why);
    }
    free(source_pixels);
    free(pixels);
    if (ok && pixels_blended) *pixels_blended = width * height;
    return ok;
}

bool s5l_mbx_process_3d(s5l_mbx_t *m, const arm_bus_t *bus,
                        uint32_t written_off, uint32_t value) {
    if (!m || written_off != S5L_MBX_STARTRENDER || value != 1u) return false;

    mbx_3d_candidates++;
    const char *why = "unknown rejection";
    uint32_t pixels = 0u;
    if (!mbx_execute_first_tiled_over(m, bus, &why, &pixels) &&
        !mbx_execute_status_sprite(m, bus, &why, &pixels) &&
        !mbx_execute_axis_aligned_sprite(m, bus, &why, &pixels) &&
        !mbx_execute_solid_quad(m, bus, &why, &pixels)) {
        mbx_3d_rejected++;
        if (mbx_trace_state == 1)
            fprintf(stderr, "MBX3D reject STARTRENDER: %s\n", why);
        return false;
    }

    /* AppleMBX's ISR records these independently and declares 3DIdle only
     * after all three have arrived. None is raised until the pixels above have
     * crossed the GART and committed through the observer-aware bus. */
    m->status |= S5L_MBX_STATUS_ISP |
                 S5L_MBX_STATUS_RENDER_COMPLETE |
                 S5L_MBX_STATUS_EVM_DALLOC;
    mbx_3d_completed++;
    mbx_3d_pixels += pixels;
    if (mbx_trace_state == 1)
        fprintf(stderr, "MBX3D complete STARTRENDER: %u pixels\n", pixels);
    return true;
}

void s5l_mbx_reset(s5l_mbx_t *m) {
    if (!m) return;
    /*
     * Unlike the pointer-free device resets, this requires an initialized
     * machine-owned block: edram is an allocation that must survive a device
     * reset. Zero is the honest power-on value for the one register whose
     * meaning is known -- reset has NOT completed until it is asked for, and
     * a device that reported completion before the request would let the
     * driver past a handshake that never happened.
     *
     * The buffer belongs to the machine and outlives a device reset; its
     * CONTENTS are cleared, which is what a powered-down edram holds.
     */
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
        /* The register histogram deliberately covers only the 8 KiB register
         * block. The command ring is 64 KiB inside the much larger EDRAM
         * aperture, so record its sparse live writes separately when the same
         * opt-in trace is enabled. r366 proved that a complete packet can be
         * present in a checkpoint while the execution trigger reports zero
         * candidates; exact offset/value order is the missing fact. */
        if (off >= S5L_MBX_2D_RING_BASE &&
            off < S5L_MBX_2D_RING_BASE + S5L_MBX_2D_RING_SIZE &&
            mbx_trace_enabled()) {
            fprintf(stderr, "MBX2D ring write +0x%04x = 0x%08x\n",
                    off - S5L_MBX_2D_RING_BASE, val);
            fflush(stderr);
        }
        return;
    }

    mbx_trace(off, val, true);

    /* STATUS reads come from m->status. Its otherwise unreachable reg[] slot
     * carries the snapshotted pending-2D marker described above, so a recovery
     * W1S must not overwrite that private state between packet copy and submit. */
    if (off != S5L_MBX_STATUS) m->reg[off / 4u] = val;

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

    /* 0x12c is write-one-to-set as well as readable status. AppleMBX recovery
     * injects missing 0x08, 0x04 and 0x40 events here in consecutive writes;
     * OR preserves the accumulated word that it subsequently reads. 0x134 is
     * the separate write-one-to-clear acknowledgement: it clears exactly the
     * bit the driver waited on (0x40), or 0x7ff during initialization. */
    if (off == S5L_MBX_STATUS) m->status |= val;
    if (off == S5L_MBX_STATUS_ACK) m->status &= ~val;

    /* AppleMBX+0xe854 programs 0x824..0x83c, writes BACKGROUND_TAG, waits for
     * EVM_DALLOC and acknowledges it synchronously. r365 proved those writes
     * are startup transfers, not 2D submissions. 2D_SYNC is therefore raised
     * only by s5l_mbx_process_2d(), after a decoded packet moved its pixels. */
    if (off == S5L_MBX_BACKGROUND_TAG)
        m->status |= S5L_MBX_STATUS_EVM_DALLOC;
}
