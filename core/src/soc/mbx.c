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
#define MBX_2D_BLEND_MODE      0x8002ccccu

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

/* Three later _mbx3DCtxQuadCopyPerspective records and live object streams
 * recover the padlock, `Searching...`, and battery status sprites. Their
 * texture-control words encode different padded dimensions/strides. Bit 18
 * is control rather than GPU address bit 18, so all three use the same exact
 * 18-bit address field recovered above. */

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
     * ctx+0x35 enables blending. The retained clock/date packets carry the
     * same factors QuartzCore passed to _mbx2DSetBlendEquation: source ONE,
     * destination ONE_MINUS_SRC_ALPHA, and global alpha 255. */
    if ((w[0] != MBX_2D_COMMAND_HEADER && w[0] != MBX_2D_SUBMIT) ||
        (w[2] & 0xffff8000u) != 0x94060000u ||
        (w[4] & 0xf8002000u) != 0x30000000u ||
        w[5] != MBX_2D_BLEND_TAG || w[6] != MBX_2D_BLEND_EQUATION ||
        w[7] != 0x60800200u || w[8] != MBX_2D_BLEND_MODE ||
        w[9] != 0xffffffffu ||
        (w[10] & ~0x1fff1fffu) || (w[11] & ~0x1fff1fffu)) {
        if (why) *why = "packet is not the decoded premultiplied-over copy form";
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
        if ((src & 0xffu) > alpha || ((src >> 8) & 0xffu) > alpha ||
            ((src >> 16) & 0xffu) > alpha) {
            if (why) *why = "2D source is not premultiplied BGRA8";
            ok = false;
            break;
        }
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

/* The first entry is r369's full 320x96 overlay. The second is r377's exact
 * retry after the repaired 2D batch: the same quad/source, but a one-tile-row
 * dirty clip whose boundary object narrows the write to x=8..312, y=97..109.
 * Keeping both literal forms makes clipped redraws possible without accepting
 * arbitrary tile streams or inferring a generic PowerVR rasterizer. */
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

static uint32_t mbx_3d_background_boundary_expected(
    const struct mbx_3d_background_form *form, uint32_t off) {
    if (off >= 0x0b8u && off <= 0x0d4u)
        return form->boundary[(off - 0x0b8u) / 4u];
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
    uint32_t xclip;
    uint32_t tile_x0, tile_x1, tile_y0, tile_y1;
    uint32_t left, top, width, height;
    uint32_t source_stride;
    uint32_t source_control;
    uint32_t quad[44];
};

/* These are literal transcriptions of the three live object streams. Words 2
 * and 5 are address fields and are validated separately against each form's
 * control bits and FBSTART; every other word must match exactly. */
static const struct mbx_3d_status_form mbx_3d_status_forms[] = {
    {
        .xclip = 0x00a80098u,
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
        .xclip = 0x00500000u,
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
        .xclip = 0x01400128u,
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
};

static const struct mbx_3d_status_form *
mbx_3d_find_status_form(const s5l_mbx_t *m) {
    uint32_t xclip = m->reg[S5L_MBX_FBXCLIP / 4u];
    for (unsigned i = 0;
         i < sizeof mbx_3d_status_forms / sizeof mbx_3d_status_forms[0]; i++)
        if (mbx_3d_status_forms[i].xclip == xclip)
            return &mbx_3d_status_forms[i];
    return NULL;
}

static uint32_t
mbx_3d_status_boundary_expected(const struct mbx_3d_status_form *form,
                                uint32_t off) {
    switch (off) {
    case 0x0b8u: return form->quad[8];
    case 0x0bcu: return form->quad[9];
    case 0x0c0u: return form->quad[10];
    case 0x0c4u: return form->quad[11];
    case 0x0c8u: return form->quad[12];
    case 0x0ccu: return form->quad[13];
    case 0x0d0u: return form->quad[14];
    case 0x0d4u: return form->quad[15];
    default: break;
    }
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
        m->reg[S5L_MBX_FBYCLIP / 4u] != 0x00200000u ||
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

    static const uint32_t list_words[4] = {
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
                if (why) *why = "glyph destination does not resolve to FBSTART";
                return false;
            }
        } else if (value != form->quad[i]) {
            if (why) *why = "textured status sprite differs from its captured form";
            return false;
        }
    }
    if (!source) {
        if (why) *why = "status source resolves to GPU address zero";
        return false;
    }

    uint32_t row_bytes = form->width * 4u;
    uint32_t total = row_bytes * form->height;
    uint64_t source_end = (uint64_t)source +
                          (uint64_t)(form->height - 1u) *
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
        uint32_t src = source + row * form->source_stride;
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        if (!mbx_gart_validate(m, bus, src, row_bytes, why) ||
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
        uint32_t src = source + row * form->source_stride;
        uint32_t dst = target + (form->top + row) * MBX_3D_TARGET_STRIDE +
                       form->left * 4u;
        ok = mbx_gart_read(m, bus, src, source_pixels + row * row_bytes,
                           row_bytes, why) &&
             mbx_gart_read(m, bus, dst, pixels + row * row_bytes,
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

bool s5l_mbx_process_3d(s5l_mbx_t *m, const arm_bus_t *bus,
                        uint32_t written_off, uint32_t value) {
    if (!m || written_off != S5L_MBX_STARTRENDER || value != 1u) return false;

    mbx_3d_candidates++;
    const char *why = "unknown rejection";
    uint32_t pixels = 0u;
    if (!mbx_execute_first_tiled_over(m, bus, &why, &pixels) &&
        !mbx_execute_status_sprite(m, bus, &why, &pixels)) {
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
