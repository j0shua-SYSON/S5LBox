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
 * an exact cold snapshot. Unknown formats, scaling, blending and geometry are
 * rejected without completion. Everything else remains a fresh question to
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
#define MBX_2D_RING_BASE       0x00a00000u
#define MBX_2D_RING_SIZE       0x00010000u
#define MBX_2D_PACKET_WORDS    16u
#define MBX_2D_PACKET_BYTES    (MBX_2D_PACKET_WORDS * 4u)
#define MBX_2D_END             0x70000000u

/* The first proved packet is a 320-pixel BGRA8 surface. There is no decoded
 * destination-stride field in its post-relocation packet, so widening this is
 * not justified until another captured packet supplies that missing fact. */
#define MBX_2D_BGRA_STRIDE     0x500u
#define MBX_2D_WIDTH           320u
#define MBX_2D_HEIGHT          480u

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
                    " bytes-copied %llu\n",
            (unsigned long long)mbx_2d_candidates,
            (unsigned long long)mbx_2d_completed,
            (unsigned long long)mbx_2d_rejected,
            (unsigned long long)mbx_2d_bytes);

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

static bool mbx_execute_simple_copy(s5l_mbx_t *m, const arm_bus_t *bus,
                                    uint32_t packet_off, const char **why,
                                    uint32_t *copied) {
    uint32_t w[MBX_2D_PACKET_WORDS];
    for (unsigned i = 0; i < MBX_2D_PACKET_WORDS; i++)
        w[i] = mbx_edram_word(m, packet_off + i * 4u);

    /* These constants and masks are the stores/literal pool in
     * _pack2DCtxBlitCopy at 0x30e1c2ac..0x30e1c5a4. This deliberately accepts
     * coordinates but only the captured BGRA8, unity-scale, simple-copy mode.
     */
    if (w[0] != 0xf0000000u ||
        (w[2] & 0xffff8000u) != 0x94060000u ||
        (w[2] & 0x7fffu) != MBX_2D_BGRA_STRIDE ||
        (w[4] & 0xf8002000u) != 0x30000000u ||
        w[5] != 0x60800200u || w[6] != 0x8000ccccu ||
        w[7] != 0xffffffffu ||
        (w[8] & ~0x1fff1fffu) || (w[9] & ~0x1fff1fffu)) {
        if (why) *why = "packet is not the decoded BGRA8 unity simple-copy form";
        return false;
    }
    for (unsigned i = 10; i < MBX_2D_PACKET_WORDS; i++) {
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

    /* Validate every destination before changing one byte. A bad late PTE
     * must not leave a half-rendered frame and then withhold completion. */
    for (uint32_t row = 0; row < height; row++) {
        uint64_t src64 = (uint64_t)w[3] +
                         (uint64_t)(src_y + row) * MBX_2D_BGRA_STRIDE +
                         (uint64_t)src_x * 4u;
        uint64_t dst64 = (uint64_t)w[1] +
                         (uint64_t)(dst_y + row) * MBX_2D_BGRA_STRIDE +
                         (uint64_t)dst_x * 4u;
        if (src64 > UINT32_MAX || dst64 > UINT32_MAX ||
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
    for (uint32_t row = 0; row < height && ok; row++) {
        uint32_t dst = w[1] + (dst_y + row) * MBX_2D_BGRA_STRIDE + dst_x * 4u;
        ok = mbx_gart_write(m, bus, dst, pixels + row * row_bytes,
                            row_bytes, why);
    }
    free(pixels);
    if (ok && copied) *copied = total;
    return ok;
}

bool s5l_mbx_process_2d(s5l_mbx_t *m, const arm_bus_t *bus,
                        uint32_t written_off) {
    if (!m || !m->edram ||
        written_off < MBX_2D_RING_BASE + MBX_2D_PACKET_BYTES - 4u ||
        written_off >= MBX_2D_RING_BASE + MBX_2D_RING_SIZE ||
        mbx_edram_word(m, written_off) != MBX_2D_END)
        return false;

    uint32_t packet_off = written_off - (MBX_2D_PACKET_BYTES - 4u);
    if (mbx_edram_word(m, packet_off) != 0xf0000000u) return false;

    mbx_2d_candidates++;
    const char *why = "unknown rejection";
    uint32_t copied = 0;
    if (!mbx_execute_simple_copy(m, bus, packet_off, &why, &copied)) {
        mbx_2d_rejected++;
        if (mbx_trace_state == 1)
            fprintf(stderr, "MBX2D reject ring+0x%04x: %s\n",
                    packet_off - MBX_2D_RING_BASE, why);
        return false;
    }

    /* Completion belongs to the packet whose pixels were just committed, not
     * to the unrelated startup-transfer kick at 0x6d8. */
    m->status |= S5L_MBX_STATUS_2D;
    mbx_2d_completed++;
    mbx_2d_bytes += copied;
    if (mbx_trace_state == 1)
        fprintf(stderr, "MBX2D complete ring+0x%04x: %u bytes\n",
                packet_off - MBX_2D_RING_BASE, copied);
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
        if (off >= MBX_2D_RING_BASE &&
            off < MBX_2D_RING_BASE + MBX_2D_RING_SIZE &&
            mbx_trace_enabled()) {
            fprintf(stderr, "MBX2D ring write +0x%04x = 0x%08x\n",
                    off - MBX_2D_RING_BASE, val);
            fflush(stderr);
        }
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

    /* AppleMBX+0xe854 programs 0x824..0x83c, writes this kick, waits for bit 6
     * and acknowledges it synchronously. r365 proved those writes are startup
     * transfers, not 2D submissions. Bit 10 is therefore raised only by
     * s5l_mbx_process_2d(), after a decoded packet has moved its pixels. */
    if (off == S5L_MBX_KICK) m->status |= S5L_MBX_STATUS_DONE;
}
