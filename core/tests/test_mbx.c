/*
 * S5LBox -- the first measured PowerVR MBX2D command.
 *
 * This is intentionally not a generic "copy some bytes" test. It transcribes
 * the packet produced by iPhone OS 3's _pack2DCtxBlitCopy, builds the GART the
 * way AppleMBXMMU::map does, routes every command word through the real machine
 * aperture, and checks the two facts that make completion honest: translated
 * pixels moved, and bit 10 rose only after AppleMBX's separate ring+0 submit
 * store. Unknown packet modes and incomplete GARTs must move nothing and
 * complete nothing.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { \
        g_fail++; \
        printf("  FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

/* Literals, not model macros: these are the addresses/values recovered from
 * AppleMBX and the 64-byte r365 packet. A wrong constant in soc.h therefore
 * cannot make the test agree with the implementation by construction. */
#define RAM_BASE       0x08000000u
#define RAM_SIZE       0x00200000u
#define MBX_BASE       0x3b000000u
#define REG_STATUS     0x0000012cu
#define REG_MASK       0x00000130u
#define REG_ACK        0x00000134u
#define REG_KICK       0x000006d8u
#define REG_RGNBASE    0x00000608u
#define REG_OBJBASE    0x0000060cu
#define REG_PIXSAMP    0x0000061cu
#define REG_FBCTL      0x00000650u
#define REG_FBXCLIP    0x00000654u
#define REG_FBYCLIP    0x00000658u
#define REG_FBSTART    0x0000065cu
#define REG_FBSTRIDE   0x00000660u
#define REG_RENDER     0x00000680u
#define REG_GART0      0x00001000u
#define REG_GART2      0x00001008u
#define REG_GART3      0x0000100cu
#define RING           0x00a00000u
#define GART_TABLE     0x08001000u
#define SRC_GPU        0x00800080u
#define DST_GPU        0x00810000u

static const uint32_t PACKET[16] = {
    0xa0060500u, DST_GPU, 0x94060500u, SRC_GPU,
    0x30008003u, 0x60800200u, 0x8000ccccu, 0xffffffffu,
    0x00050006u, 0x00090008u, 0x70000000u, 0x70000000u,
    0x70000000u, 0x70000000u, 0x70000000u, 0x70000000u,
};

static void write_packet(s5l8900_t *m, uint32_t ring_off,
                         const uint32_t packet[16], unsigned words) {
    for (unsigned i = 0; i < words; i++)
        m->bus.write32(m->bus.ctx, MBX_BASE + ring_off + i * 4u, packet[i]);
}

static void test_translated_copy_and_completion_boundary(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE), "machine init failed");
    if (!m.ram) return;

    /* GPU VA 0x008xxxxx selects root register 2. Source rows intentionally
     * land on non-contiguous physical pages; destination rows do too. */
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, GART_TABLE);
    m.bus.write32(m.bus.ctx, GART_TABLE + 0x00u, 0x08020000u); /* GPU page 0 */
    m.bus.write32(m.bus.ctx, GART_TABLE + 0x04u, 0x08021000u); /* GPU page 1 */
    m.bus.write32(m.bus.ctx, GART_TABLE + 0x44u, 0x08040000u); /* GPU page 0x11 */
    m.bus.write32(m.bus.ctx, GART_TABLE + 0x48u, 0x08050000u); /* GPU page 0x12 */

    static const uint32_t row0[4] = {
        0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u
    };
    static const uint32_t row1[4] = {
        0x01234567u, 0x89abcdefu, 0x13579bdfu, 0x2468ace0u
    };
    const uint32_t src0 = 0x08020f88u; /* SRC + y*0x500 + x*4 */
    const uint32_t src1 = 0x08021488u;
    const uint32_t dst0 = 0x08040e14u; /* DST + y*0x500 + x*4 */
    const uint32_t dst1 = 0x08050314u;
    for (unsigned i = 0; i < 4; i++) {
        m.bus.write32(m.bus.ctx, src0 + i * 4u, row0[i]);
        m.bus.write32(m.bus.ctx, src1 + i * 4u, row1[i]);
    }

    /* 0x6d8 is the synchronous startup transfer. It owns bit 6, not a fake
     * bit-10 2D completion. */
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_KICK, 0x09000000u);
    uint32_t status = m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS);
    CHECK((status & 0x40u) != 0u && (status & 0x400u) == 0u,
          "startup kick status=%08x, expect bit 6 only", status);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x7ffu);

    /* Neither an incomplete packet nor its last terminator is a submission.
     * AppleMBX+0x1f58's separate fixed ring+0 store is the measured boundary. */
    write_packet(&m, RING, PACKET, 15u);
    CHECK(m.bus.read32(m.bus.ctx, dst0) == 0u &&
          m.bus.read32(m.bus.ctx, dst1) == 0u,
          "an incomplete packet changed destination RAM");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "an incomplete packet raised status");

    m.bus.write32(m.bus.ctx, MBX_BASE + RING + 15u * 4u, PACKET[15]);
    CHECK(m.bus.read32(m.bus.ctx, dst0) == 0u &&
          m.bus.read32(m.bus.ctx, dst1) == 0u,
          "packet terminators executed before the submit store");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "packet terminators raised completion before submit");

    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(m.bus.read32(m.bus.ctx, dst0 + i * 4u) == row0[i],
              "row 0 pixel %u did not cross the GART", i);
        CHECK(m.bus.read32(m.bus.ctx, dst1 + i * 4u) == row1[i],
              "row 1 pixel %u did not cross the GART", i);
    }
    status = m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS);
    CHECK((status & 0x400u) != 0u && (status & 0x40u) == 0u,
          "executed packet status=%08x, expect bit 10 only", status);

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_MASK, 0x400u);
    CHECK(s5l_mbx_irq(&m.mbx), "completed 2D packet did not assert masked IRQ");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x400u);
    CHECK(!s5l_mbx_irq(&m.mbx), "2D acknowledge did not lower IRQ");

    /* r372 disproved the old per-packet-header-rewrite interpretation. The
     * next packet begins at +0x40, but AppleMBX still submits through ring+0. */
    for (unsigned i = 0; i < 4; i++) {
        m.bus.write32(m.bus.ctx, dst0 + i * 4u, 0u);
        m.bus.write32(m.bus.ctx, dst1 + i * 4u, 0u);
    }
    write_packet(&m, RING + 0x40u, PACKET, 16u);
    CHECK(m.bus.read32(m.bus.ctx, dst0) == 0u &&
          m.bus.read32(m.bus.ctx, dst1) == 0u,
          "second packet executed before the fixed submit store");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "second packet raised completion before the fixed submit store");

    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(m.bus.read32(m.bus.ctx, dst0 + i * 4u) == row0[i],
              "second row 0 pixel %u did not cross the GART", i);
        CHECK(m.bus.read32(m.bus.ctx, dst1 + i * 4u) == row1[i],
              "second row 1 pixel %u did not cross the GART", i);
    }
    status = m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS);
    CHECK((status & 0x400u) != 0u,
          "fixed submit store did not raise 2D completion: %08x", status);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x400u);

    s5l8900_free(&m);
}

static void test_unknown_packet_and_bad_gart_are_atomic(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE), "machine init failed");
    if (!m.ram) return;
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, GART_TABLE);
    m.bus.write32(m.bus.ctx, GART_TABLE + 0x00u, 0x08020000u);
    m.bus.write32(m.bus.ctx, GART_TABLE + 0x04u, 0x08021000u);
    m.bus.write32(m.bus.ctx, GART_TABLE + 0x44u, 0x08040000u);
    m.bus.write32(m.bus.ctx, GART_TABLE + 0x48u, 0x08050000u);

    uint32_t unknown[16];
    memcpy(unknown, PACKET, sizeof unknown);
    unknown[5] ^= 1u; /* an unmeasured scale: reject, do not approximate */
    m.bus.write32(m.bus.ctx, 0x08040e14u, 0xa5a5a5a5u);
    write_packet(&m, RING + 0x40u, unknown, 16u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(m.bus.read32(m.bus.ctx, 0x08040e14u) == 0xa5a5a5a5u,
          "unknown scale changed a destination pixel");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "unknown scale raised completion");

    /* A second destination has a valid first row and a missing second-row
     * PTE. Validation must find the late hole before touching the first row. */
    uint32_t bad[16];
    memcpy(bad, PACKET, sizeof bad);
    bad[1] = 0x00820000u;
    m.bus.write32(m.bus.ctx, GART_TABLE + 0x84u, 0x08060000u); /* page 0x21 */
    /* page 0x22 at +0x88 deliberately remains zero */
    m.bus.write32(m.bus.ctx, 0x08060e14u, 0x5a5a5a5au);
    write_packet(&m, RING + 0x80u, bad, 16u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(m.bus.read32(m.bus.ctx, 0x08060e14u) == 0x5a5a5a5au,
          "late missing PTE left a partially written rectangle");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "bad GART raised completion");

    s5l8900_free(&m);
}

static void test_status_write_to_set_and_ack(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE), "machine init failed");
    if (!m.ram) return;

    /* AppleMBX recovery injects the three missing 3D events by writing them
     * separately to status. Each write must accumulate even while masked. */
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_STATUS, 0x08u); /* ISP */
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x08u,
          "first status write did not set ISP");
    CHECK(!s5l_mbx_irq(&m.mbx),
          "masked status write asserted the MBX interrupt");

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_STATUS, 0x04u); /* render */
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_STATUS, 0x40u); /* EVM */
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "consecutive status writes did not accumulate all 3D events");

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_MASK, 0x4cu);
    CHECK(s5l_mbx_irq(&m.mbx),
          "unmasking accumulated status did not assert the interrupt");

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x04u);
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x48u,
          "selective acknowledge cleared more than render-complete");
    CHECK(s5l_mbx_irq(&m.mbx),
          "interrupt fell while unacknowledged status remained");

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x48u);
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "final acknowledge left recovery status pending");
    CHECK(!s5l_mbx_irq(&m.mbx),
          "fully acknowledged recovery status left IRQ asserted");

    s5l8900_free(&m);
}

static uint32_t test_gpu_pa(s5l8900_t *m, uint32_t gpu) {
    uint32_t chunk = gpu >> 22;
    uint32_t root = m->bus.read32(m->bus.ctx,
        MBX_BASE + REG_GART0 + chunk * 4u);
    uint32_t pte = m->bus.read32(m->bus.ctx,
        root + (((gpu >> 12) & 0x3ffu) * 4u));
    return pte + (gpu & 0xfffu);
}

static void test_gpu_write32(s5l8900_t *m, uint32_t gpu, uint32_t value) {
    m->bus.write32(m->bus.ctx, test_gpu_pa(m, gpu), value);
}

static uint32_t test_gpu_read32(s5l8900_t *m, uint32_t gpu) {
    return m->bus.read32(m->bus.ctx, test_gpu_pa(m, gpu));
}

static uint32_t test_over(uint32_t dst, uint32_t src) {
    uint32_t inv = 256u - (src >> 24);
    uint32_t out = 0u;
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
        uint32_t s = (src >> shift) & 0xffu;
        uint32_t d = (dst >> shift) & 0xffu;
        out |= (s + ((d * inv) >> 8)) << shift;
    }
    return out;
}

static uint32_t test_modulate_vertex_alpha(uint32_t src, uint32_t alpha) {
    uint32_t out = 0u;
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
        uint32_t component = (src >> shift) & 0xffu;
        out |= (((component + 1u) * alpha) >> 8) << shift;
    }
    return out;
}

static void test_map_gpu_page(s5l8900_t *m, uint32_t table,
                              uint32_t gpu, uint32_t pa) {
    m->bus.write32(m->bus.ctx,
        table + (((gpu >> 12) & 0x3ffu) * 4u), pa);
}

static void test_full_lower_surface_black_fill(void) {
    enum { STRIDE = 0x500u, WIDTH = 320u, HEIGHT = 480u, TOP = 20u };
    const uint32_t table2 = 0x08003000u;
    const uint32_t target = 0x00897000u;
    const uint32_t target_pa = 0x08040000u;
    uint32_t packet[16] = {
        0xa0060500u, target, 0x94060500u, 0x00000000u,
        0x30000000u, 0x60800200u, 0x8000f0f0u, 0xff000000u,
        0x00000014u, 0x014001e0u, 0x70000000u, 0x70000000u,
        0x70000000u, 0x70000000u, 0x70000000u, 0x70000000u,
    };

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE), "solid-fill machine init failed");
    if (!m.ram) return;
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    for (uint32_t page = 0; page < STRIDE * HEIGHT; page += 0x1000u)
        test_map_gpu_page(&m, table2, target + page, target_pa + page);

    test_gpu_write32(&m, target + (TOP - 1u) * STRIDE, 0xff112233u);
    test_gpu_write32(&m, target + (TOP - 1u) * STRIDE +
                     (WIDTH - 1u) * 4u, 0xff445566u);
    test_gpu_write32(&m, target + TOP * STRIDE, 0xffabcdefu);
    test_gpu_write32(&m, target + (HEIGHT - 1u) * STRIDE +
                     (WIDTH - 1u) * 4u, 0xff123456u);

    write_packet(&m, RING, packet, 16u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);

    uint32_t mismatches = 0u;
    for (uint32_t y = TOP; y < HEIGHT; y++)
        for (uint32_t x = 0; x < WIDTH; x++)
            mismatches += test_gpu_read32(&m,
                target + y * STRIDE + x * 4u) != 0xff000000u;
    CHECK(mismatches == 0u,
          "captured lower-screen fill left %u non-black pixels", mismatches);
    CHECK(test_gpu_read32(&m, target + (TOP - 1u) * STRIDE) == 0xff112233u &&
          test_gpu_read32(&m, target + (TOP - 1u) * STRIDE +
                          (WIDTH - 1u) * 4u) == 0xff445566u,
          "lower-screen fill changed the status-bar row");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x400u,
          "lower-screen fill did not raise exactly 2D completion");

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x400u);
    test_gpu_write32(&m, target + TOP * STRIDE, 0x89abcdefu);
    packet[7] ^= 1u;
    write_packet(&m, RING + 0x40u, packet, 16u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(test_gpu_read32(&m, target + TOP * STRIDE) == 0x89abcdefu,
          "unknown solid colour changed the destination");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "unknown solid colour raised completion");

    s5l8900_free(&m);
}

static void test_split_lower_surface_black_fill(void) {
    enum { STRIDE = 0x500u, WIDTH = 320u, HEIGHT = 480u, TOP = 20u };
    const uint32_t table2 = 0x08003000u;
    const uint32_t target = 0x00998000u;
    const uint32_t target_pa = 0x08040000u;
    uint32_t first[16] = {
        0xa0060500u, target, 0x94060500u, 0x00000000u,
        0x30000000u, 0x60800200u, 0x8000f0f0u, 0xff000000u,
        0x00000014u, 0x01400185u, 0x70000000u, 0x70000000u,
        0x70000000u, 0x70000000u, 0x70000000u, 0x70000000u,
    };
    uint32_t second[16];
    memcpy(second, first, sizeof second);
    second[8] = 0x00000185u;
    second[9] = 0x014001e0u;

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE),
          "split-fill machine init failed");
    if (!m.ram) return;
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    for (uint32_t page = 0; page < STRIDE * HEIGHT; page += 0x1000u)
        test_map_gpu_page(&m, table2, target + page, target_pa + page);

    test_gpu_write32(&m, target + (TOP - 1u) * STRIDE, 0xff112233u);
    write_packet(&m, RING + 0x40u, first, 16u);
    write_packet(&m, RING + 0x80u, second, 16u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);

    uint32_t mismatches = 0u;
    for (uint32_t y = TOP; y < HEIGHT; y++)
        for (uint32_t x = 0; x < WIDTH; x++)
            mismatches += test_gpu_read32(
                &m, target + y * STRIDE + x * 4u) != 0xff000000u;
    CHECK(mismatches == 0u,
          "captured split fill left %u non-black pixels", mismatches);
    CHECK(test_gpu_read32(&m, target + (TOP - 1u) * STRIDE) == 0xff112233u,
          "split fill changed the status-bar row");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x400u,
          "split fill batch did not raise exactly 2D completion");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x400u);

    uint32_t upper = target + TOP * STRIDE;
    uint32_t lower = target + 389u * STRIDE;
    test_gpu_write32(&m, upper, 0xffabcdefu);
    test_gpu_write32(&m, lower, 0xff123456u);
    second[9] ^= 1u;
    write_packet(&m, RING + 0xc0u, first, 16u);
    write_packet(&m, RING + 0x100u, second, 16u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(test_gpu_read32(&m, upper) == 0xffabcdefu &&
          test_gpu_read32(&m, lower) == 0xff123456u,
          "rejected split fill committed part of the batch");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "rejected split fill raised completion");

    s5l8900_free(&m);
}

static void test_premultiplied_2d_clock_form(void) {
    const uint32_t table2 = 0x08003000u;
    const uint32_t source = 0x00900000u;
    const uint32_t target = 0x00810000u;
    const uint32_t source_pa = 0x08070000u;
    const uint32_t target_pa = 0x08080000u;
    static const uint32_t source_pixels[4] = {
        0x80804020u, 0x00000000u, 0xffffffffu, 0x40201008u
    };
    static const uint32_t destination_pixels[4] = {
        0xff102030u, 0xff405060u, 0xff708090u, 0xffa0b0c0u
    };
    uint32_t packet[18] = {
        0xa0060500u, target, 0x94060010u, source,
        0x30004000u, 0x20000004u, 0x095ff000u, 0x60800200u,
        0x8002ccccu, 0xffffffffu, 0x00020003u, 0x00040005u,
        0x70000000u, 0x70000000u, 0x70000000u, 0x70000000u,
        0x70000000u, 0x70000000u,
    };

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE), "machine init failed");
    if (!m.ram) return;
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    test_map_gpu_page(&m, table2, source, source_pa);
    test_map_gpu_page(&m, table2, target, target_pa);
    test_map_gpu_page(&m, table2, target + 0x1000u, target_pa + 0x1000u);

    for (unsigned i = 0; i < 2u; i++) {
        test_gpu_write32(&m, source + 4u + i * 4u, source_pixels[i]);
        test_gpu_write32(&m, source + 0x10u + 4u + i * 4u,
                         source_pixels[2u + i]);
        test_gpu_write32(&m, target + 3u * 0x500u + (2u + i) * 4u,
                         destination_pixels[i]);
        test_gpu_write32(&m, target + 4u * 0x500u + (2u + i) * 4u,
                         destination_pixels[2u + i]);
    }
    test_gpu_write32(&m, target + 3u * 0x500u + 4u, 0x12345678u);

    /* A status W1S/ack pair between head and body must not destroy the pending
     * submit marker; that private state is snapshotted in STATUS's unread slot. */
    m.bus.write32(m.bus.ctx, MBX_BASE + RING + 0x40u, packet[0]);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_STATUS, 0x08u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x08u);
    for (unsigned i = 1u; i < 18u; i++)
        m.bus.write32(m.bus.ctx, MBX_BASE + RING + 0x40u + i * 4u,
                      packet[i]);
    CHECK(test_gpu_read32(&m, target + 3u * 0x500u + 2u * 4u) ==
              destination_pixels[0],
          "2D blend executed before the fixed submit store");
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);

    uint32_t mismatches = 0u;
    for (unsigned i = 0; i < 2u; i++) {
        mismatches += test_gpu_read32(
            &m, target + 3u * 0x500u + (2u + i) * 4u) !=
            test_over(destination_pixels[i], source_pixels[i]);
        mismatches += test_gpu_read32(
            &m, target + 4u * 0x500u + (2u + i) * 4u) !=
            test_over(destination_pixels[2u + i], source_pixels[2u + i]);
    }
    CHECK(mismatches == 0u, "%u premultiplied 2D pixels mismatched", mismatches);
    CHECK(test_gpu_read32(&m, target + 3u * 0x500u + 4u) == 0x12345678u,
          "2D blend changed a pixel outside its rectangle");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x400u,
          "completed 2D blend did not raise only 2D_SYNC");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x400u);

    uint32_t first = target + 3u * 0x500u + 2u * 4u;
    test_gpu_write32(&m, first, 0x89abcdefu);
    packet[6] ^= 1u;
    for (unsigned i = 0; i < 18u; i++)
        m.bus.write32(m.bus.ctx, MBX_BASE + RING + 0x88u + i * 4u,
                      packet[i]);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
          "unknown 2D blend equation changed the destination");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "unknown 2D blend equation raised completion");

    s5l8900_free(&m);
}

static void test_opaque_global_alpha_2d_form(void) {
    const uint32_t table2 = 0x08003000u;
    const uint32_t source = 0x00900000u;
    const uint32_t target = 0x00810000u;
    const uint32_t source_pa = 0x08070000u;
    const uint32_t target_pa = 0x08080000u;
    static const uint32_t source_pixels[4] = {
        0xff804020u, 0xff000000u, 0xffffffffu, 0xff201008u
    };
    static const uint32_t destination_pixels[4] = {
        0xff102030u, 0x80405060u, 0xff708090u, 0x40a0b0c0u
    };
    uint32_t packet[18] = {
        0xa0060500u, target, 0x94060010u, source,
        0x30004000u, 0x20000004u, 0x0d5f8000u, 0x60800200u,
        0x8002ccccu, 0xffffffffu, 0x00020003u, 0x00040005u,
        0x70000000u, 0x70000000u, 0x70000000u, 0x70000000u,
        0x70000000u, 0x70000000u,
    };

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE),
          "opaque-global machine init failed");
    if (!m.ram) return;
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    test_map_gpu_page(&m, table2, source, source_pa);
    test_map_gpu_page(&m, table2, target, target_pa);
    test_map_gpu_page(&m, table2, target + 0x1000u, target_pa + 0x1000u);

    for (unsigned i = 0; i < 2u; i++) {
        test_gpu_write32(&m, source + 4u + i * 4u, source_pixels[i]);
        test_gpu_write32(&m, source + 0x10u + 4u + i * 4u,
                         source_pixels[2u + i]);
        test_gpu_write32(&m, target + 3u * 0x500u + (2u + i) * 4u,
                         destination_pixels[i]);
        test_gpu_write32(&m, target + 4u * 0x500u + (2u + i) * 4u,
                         destination_pixels[2u + i]);
    }

    write_packet(&m, RING + 0x40u, packet, 18u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    uint32_t mismatches = 0u;
    for (unsigned i = 0; i < 2u; i++) {
        uint32_t modulated = test_modulate_vertex_alpha(source_pixels[i],
                                                         0xf8u);
        mismatches += test_gpu_read32(
            &m, target + 3u * 0x500u + (2u + i) * 4u) !=
            test_over(destination_pixels[i], modulated);
        modulated = test_modulate_vertex_alpha(source_pixels[2u + i], 0xf8u);
        mismatches += test_gpu_read32(
            &m, target + 4u * 0x500u + (2u + i) * 4u) !=
            test_over(destination_pixels[2u + i], modulated);
    }
    CHECK(mismatches == 0u,
          "%u opaque global-alpha 2D pixels mismatched", mismatches);
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x400u,
          "opaque global-alpha blend did not raise exactly 2D completion");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x400u);

    uint32_t first = target + 3u * 0x500u + 2u * 4u;
    test_gpu_write32(&m, first, 0x89abcdefu);
    test_gpu_write32(&m, source + 4u, 0x80804020u);
    write_packet(&m, RING + 0x88u, packet, 18u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
          "non-opaque global-alpha source changed the destination");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "non-opaque global-alpha source raised completion");

    test_gpu_write32(&m, source + 4u, source_pixels[0]);
    packet[6] ^= 0x00100000u;
    write_packet(&m, RING + 0xd0u, packet, 18u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
          "unknown global-alpha factors changed the destination");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "unknown global-alpha factors raised completion");

    s5l8900_free(&m);
}

static void test_ordered_atomic_2d_batches(void) {
    enum {
        TARGET_STRIDE = 0x500u,
        TARGET_HEIGHT = 480u,
        TARGET_BYTES = TARGET_STRIDE * TARGET_HEIGHT,
    };
    const uint32_t table2 = 0x08003000u;
    const uint32_t source = 0x00900000u;
    const uint32_t target = 0x00810000u;
    const uint32_t source_pa = 0x08130000u;
    const uint32_t target_pa = 0x08080000u;
    const uint32_t destination = target + 10u * TARGET_STRIDE + 10u * 4u;
    const uint32_t original = 0xff102030u;
    const uint32_t first_source = 0x80804020u;
    const uint32_t second_source = 0x40201008u;
    uint32_t first[18] = {
        0xa0060500u, target, 0x94060010u, source,
        0x30000000u, 0x20000004u, 0x095ff000u, 0x60800200u,
        0x8002ccccu, 0xffffffffu, 0x000a000au, 0x000b000bu,
        0x70000000u, 0x70000000u, 0x70000000u, 0x70000000u,
        0x70000000u, 0x70000000u,
    };
    uint32_t second[18];
    memcpy(second, first, sizeof second);
    second[4] = 0x30004000u; /* source x=1, same destination pixel */

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE), "machine init failed");
    if (!m.ram) return;
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    test_map_gpu_page(&m, table2, source, source_pa);
    for (uint32_t page = 0; page < (TARGET_BYTES + 0xfffu) / 0x1000u;
         page++)
        test_map_gpu_page(&m, table2, target + page * 0x1000u,
                          target_pa + page * 0x1000u);

    test_gpu_write32(&m, source, first_source);
    test_gpu_write32(&m, source + 4u, second_source);
    test_gpu_write32(&m, destination, original);

    /* r376's first newly exposed submit used eight adjacent 18-word blend
     * packets. Two overlapping packets are enough to prove the critical
     * ordering property: command two must see command one's result. */
    write_packet(&m, RING + 0x2c0u, first, 18u);
    write_packet(&m, RING + 0x308u, second, 18u);
    CHECK(test_gpu_read32(&m, destination) == original,
          "batched blend executed before the fixed submit store");
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(test_gpu_read32(&m, destination) ==
              test_over(test_over(original, first_source), second_source),
          "overlapping blend batch did not execute in copied order");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x400u,
          "completed blend batch did not raise exactly 2D_SYNC");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x400u);

    /* A later invalid command must reject the whole submit. If the first
     * command leaks through, this pixel changes and the test catches the
     * non-atomic implementation directly. */
    test_gpu_write32(&m, destination, original);
    second[6] ^= 1u;
    write_packet(&m, RING + 0x2c0u, first, 18u);
    write_packet(&m, RING + 0x308u, second, 18u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(test_gpu_read32(&m, destination) == original,
          "rejected blend batch committed an earlier command");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "rejected blend batch raised completion");

    /* The second r376 batch used adjacent 16-word simple copies. Exercise its
     * different cursor length instead of relying on the live replay alone. */
    uint32_t simple_first[16] = {
        0xa0060500u, target, 0x94060500u, source,
        0x30000000u, 0x60800200u, 0x8000ccccu, 0xffffffffu,
        0x00140014u, 0x00150015u, 0x70000000u, 0x70000000u,
        0x70000000u, 0x70000000u, 0x70000000u, 0x70000000u,
    };
    uint32_t simple_second[16];
    memcpy(simple_second, simple_first, sizeof simple_second);
    simple_second[4] = 0x30004000u;
    simple_second[8] = 0x00150014u;
    simple_second[9] = 0x00160015u;
    uint32_t simple_dst0 = target + 20u * TARGET_STRIDE + 20u * 4u;
    uint32_t simple_dst1 = simple_dst0 + 4u;
    test_gpu_write32(&m, simple_dst0, 0u);
    test_gpu_write32(&m, simple_dst1, 0u);
    write_packet(&m, RING + 0x500u, simple_first, 16u);
    write_packet(&m, RING + 0x540u, simple_second, 16u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(test_gpu_read32(&m, simple_dst0) == first_source &&
          test_gpu_read32(&m, simple_dst1) == second_source,
          "adjacent simple-copy batch did not copy both packets");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x400u,
          "completed simple-copy batch did not raise exactly 2D_SYNC");

    s5l8900_free(&m);
}

static void test_first_tiled_premultiplied_over(void) {
    enum {
        WIDTH = 320u, TOP = 20u, HEIGHT = 96u,
        SOURCE_STRIDE = 0x20u, TARGET_STRIDE = 0x500u
    };
    const uint32_t table0 = 0x08003000u;
    const uint32_t table2 = 0x08004000u;
    const uint32_t region = 0x00001000u;
    const uint32_t object = 0x00014000u;
    const uint32_t source = 0x0092d000u;
    const uint32_t target = 0x00897000u;
    const uint32_t region_pa = 0x08010000u;
    const uint32_t object_pa = 0x08011000u;
    const uint32_t source_pa = 0x08012000u;
    const uint32_t target_pa = 0x08020000u;

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE), "machine init failed");
    if (!m.ram) return;

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART0, table0);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    test_map_gpu_page(&m, table0, region, region_pa);
    test_map_gpu_page(&m, table0, object, object_pa);
    test_map_gpu_page(&m, table2, source, source_pa);
    for (uint32_t page = 0; page < 40u; page++)
        test_map_gpu_page(&m, table2, target + page * 0x1000u,
                          target_pa + page * 0x1000u);

    uint32_t list = object + 0x68u;
    for (uint32_t y = 1u; y <= 7u; y++) {
        for (uint32_t x = 0u; x < 40u; x++) {
            uint32_t pair = ((y - 1u) * 40u + x) * 8u;
            uint32_t code = (y << 8) | x;
            if (y == 7u && x == 39u) code |= 0x80000000u;
            test_gpu_write32(&m, region + pair, code);
            test_gpu_write32(&m, region + pair + 4u, list);
        }
    }
    static const uint32_t list_words[4] = {
        0x60200020u, 0x6020002du, 0x61a0007cu, 0xf0000000u
    };
    for (unsigned i = 0; i < 4u; i++)
        test_gpu_write32(&m, list + i * 4u, list_words[i]);

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
        uint32_t value = i == 2u
            ? 0x0e500000u | (target >> 7) : background[i];
        test_gpu_write32(&m, object + i * 4u, value);
    }

    static const struct mbx_test_word {
        uint16_t off;
        uint32_t value;
    } boundary[] = {
        {0x080u, 0x22206f80u}, {0x088u, 0x45800000u},
        {0x094u, 0x45800000u}, {0x098u, 0x45800000u},
        {0x09cu, 0x45800000u}, {0x0b4u, 0x22207f80u},
        {0x0bcu, 0x42e80000u}, {0x0c4u, 0x41a00000u},
        {0x0c8u, 0x43a00000u}, {0x0ccu, 0x42e80000u},
        {0x0d0u, 0x43a00000u}, {0x0d4u, 0x41a00000u},
        {0x0e8u, 0xe0000000u}, {0x0f4u, 0xa6887610u},
        {0x0f8u, 0x22220e80u}, {0x12cu, 0x3f800000u},
        {0x130u, 0x3f800000u}, {0x134u, 0x3f800000u},
        {0x138u, 0x3f800000u}, {0x198u, 0xe0000000u},
        {0x19cu, 0x22200e80u}, {0x1d0u, 0x3f800000u},
        {0x1d4u, 0x3f800000u}, {0x1d8u, 0x3f800000u},
        {0x1dcu, 0x3f800000u},
    };
    for (unsigned i = 0; i < sizeof boundary / sizeof boundary[0]; i++)
        test_gpu_write32(&m, object + boundary[i].off, boundary[i].value);

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
    for (unsigned i = 0; i < 44u; i++) {
        uint32_t value = quad[i];
        if (i == 2u) value = 0x8e000000u | (source >> 7);
        if (i == 5u) value = 0x0e500000u | (target >> 7);
        test_gpu_write32(&m, object + 0x1f0u + i * 4u, value);
    }

    uint32_t expected[HEIGHT];
    for (uint32_t row = 0; row < HEIGHT; row++) {
        uint32_t src = ((0x80u + (row & 0x3fu)) << 24) |
                       ((0x30u + (row & 0x0fu)) << 16) |
                       ((0x20u + (row & 0x0fu)) << 8) |
                       (0x10u + (row & 0x0fu));
        test_gpu_write32(&m, source + row * SOURCE_STRIDE, src);
        uint32_t dst = 0xff204060u + row;
        expected[row] = test_over(dst, src);
        for (uint32_t x = 0; x < WIDTH; x++)
            test_gpu_write32(&m,
                target + (TOP + row) * TARGET_STRIDE + x * 4u, dst);
    }
    test_gpu_write32(&m, target + (TOP - 1u) * TARGET_STRIDE, 0x11223344u);
    test_gpu_write32(&m, target + (TOP + HEIGHT) * TARGET_STRIDE, 0x55667788u);

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RGNBASE, region);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_OBJBASE, object);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_PIXSAMP, 0x00020007u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBCTL, 6u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBXCLIP, 0x01400000u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBYCLIP, 0x00800010u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBSTART, target);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBSTRIDE, WIDTH);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);

    uint32_t mismatches = 0u;
    for (uint32_t row = 0; row < HEIGHT; row++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint32_t actual = test_gpu_read32(&m,
                target + (TOP + row) * TARGET_STRIDE + x * 4u);
            mismatches += actual != expected[row];
        }
    }
    CHECK(mismatches == 0u, "%u tiled source-over pixels mismatched", mismatches);
    CHECK(test_gpu_read32(&m, target + (TOP - 1u) * TARGET_STRIDE) ==
              0x11223344u &&
          test_gpu_read32(&m, target + (TOP + HEIGHT) * TARGET_STRIDE) ==
              0x55667788u,
          "tiled render changed a pixel outside its rectangle");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "completed tiled render status=%08x, expect ISP/render/EVM",
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS));

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);
    test_gpu_write32(&m, target + TOP * TARGET_STRIDE, 0x12345678u);
    test_gpu_write32(&m, object + 0x1f0u + 3u * 4u, 0xa6884711u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, target + TOP * TARGET_STRIDE) == 0x12345678u,
          "unknown 3D blend word changed the destination");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "unknown 3D blend word raised completion");

    test_gpu_write32(&m, object + 0x1f0u + 3u * 4u, 0xa6884710u);
    uint32_t late = target + (TOP + HEIGHT - 1u) * TARGET_STRIDE;
    uint32_t late_entry = table2 + (((late >> 12) & 0x3ffu) * 4u);
    m.bus.write32(m.bus.ctx, late_entry, 0u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, target + TOP * TARGET_STRIDE) == 0x12345678u,
          "late missing 3D PTE left a partially rendered rectangle");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "late missing 3D PTE raised completion");

    /* r379 captured the overwritten render before r377's retained one. It
     * reused this exact quad/source, but narrowed the tiles to y=1..6 and the
     * boundary to y=20..97. Restore the deliberately removed PTE, then prove
     * that exact 320x77 write before testing the following inset rectangle. */
    uint32_t late_page = late & ~0xfffu;
    test_map_gpu_page(&m, table2, late_page,
                      target_pa + (late_page - target));
    for (uint32_t y = 1u; y <= 6u; y++) {
        for (uint32_t x = 0u; x < 40u; x++) {
            uint32_t pair = ((y - 1u) * 40u + x) * 8u;
            uint32_t code = (y << 8) | x;
            if (y == 6u && x == 39u) code |= 0x80000000u;
            test_gpu_write32(&m, region + pair, code);
            test_gpu_write32(&m, region + pair + 4u, list);
        }
    }
    static const uint32_t upper_boundary[8] = {
        0x00000000u, 0x42c20000u, 0x00000000u, 0x41a00000u,
        0x43a00000u, 0x42c20000u, 0x43a00000u, 0x41a00000u,
    };
    for (unsigned i = 0; i < 8u; i++)
        test_gpu_write32(&m, object + 0x0b8u + i * 4u,
                         upper_boundary[i]);

    enum { UPPER_HEIGHT = 77u };
    uint32_t upper_expected[UPPER_HEIGHT];
    for (uint32_t row = 0; row < UPPER_HEIGHT; row++) {
        uint32_t src = ((0x80u + (row & 0x3fu)) << 24) |
                       ((0x30u + (row & 0x0fu)) << 16) |
                       ((0x20u + (row & 0x0fu)) << 8) |
                       (0x10u + (row & 0x0fu));
        uint32_t dst = 0xff305070u + row;
        upper_expected[row] = test_over(dst, src);
        for (uint32_t x = 0; x < WIDTH; x++)
            test_gpu_write32(&m,
                target + (TOP + row) * TARGET_STRIDE + x * 4u, dst);
    }
    uint32_t upper_above = target + (TOP - 1u) * TARGET_STRIDE;
    uint32_t upper_after = target + (TOP + UPPER_HEIGHT) * TARGET_STRIDE;
    test_gpu_write32(&m, upper_above, 0x11223344u);
    test_gpu_write32(&m, upper_after, 0x55667788u);

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBXCLIP, 0x01400000u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBYCLIP, 0x00700010u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    mismatches = 0u;
    for (uint32_t row = 0; row < UPPER_HEIGHT; row++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint32_t actual = test_gpu_read32(&m,
                target + (TOP + row) * TARGET_STRIDE + x * 4u);
            mismatches += actual != upper_expected[row];
        }
    }
    CHECK(mismatches == 0u,
          "%u upper clipped-background pixels mismatched", mismatches);
    CHECK(test_gpu_read32(&m, upper_above) == 0x11223344u &&
          test_gpu_read32(&m, upper_after) == 0x55667788u,
          "upper clipped background changed a pixel outside its boundary");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "upper clipped background did not raise all three 3D events");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);

    /* The immediately following render narrows the stream again to x=1..38,
     * y=6 and the boundary to x=8..312, y=97..109. Prove its source-row
     * offset and exact dirty rectangle rather than treating either redraw as
     * another full 320x96 pass. */
    for (uint32_t x = 1u; x <= 38u; x++) {
        uint32_t pair = (x - 1u) * 8u;
        uint32_t code = (6u << 8) | x;
        if (x == 38u) code |= 0x80000000u;
        test_gpu_write32(&m, region + pair, code);
        test_gpu_write32(&m, region + pair + 4u, list);
    }
    static const uint32_t partial_boundary[8] = {
        0x41000000u, 0x42da0000u, 0x41000000u, 0x42c20000u,
        0x439c0000u, 0x42da0000u, 0x439c0000u, 0x42c20000u,
    };
    for (unsigned i = 0; i < 8u; i++)
        test_gpu_write32(&m, object + 0x0b8u + i * 4u,
                         partial_boundary[i]);

    enum {
        PARTIAL_LEFT = 8u, PARTIAL_TOP = 97u,
        PARTIAL_WIDTH = 304u, PARTIAL_HEIGHT = 12u,
        PARTIAL_SOURCE_ROW = 77u,
    };
    uint32_t partial_expected[PARTIAL_HEIGHT];
    for (uint32_t row = 0; row < PARTIAL_HEIGHT; row++) {
        uint32_t source_row = PARTIAL_SOURCE_ROW + row;
        uint32_t src = ((0x80u + (source_row & 0x3fu)) << 24) |
                       ((0x30u + (source_row & 0x0fu)) << 16) |
                       ((0x20u + (source_row & 0x0fu)) << 8) |
                       (0x10u + (source_row & 0x0fu));
        partial_expected[row] = test_over(expected[source_row], src);
    }
    uint32_t partial_before = target + PARTIAL_TOP * TARGET_STRIDE +
                              (PARTIAL_LEFT - 1u) * 4u;
    uint32_t partial_above = target + (PARTIAL_TOP - 1u) * TARGET_STRIDE +
                             PARTIAL_LEFT * 4u;
    uint32_t partial_after = target +
                             (PARTIAL_TOP + PARTIAL_HEIGHT) * TARGET_STRIDE +
                             PARTIAL_LEFT * 4u;
    test_gpu_write32(&m, partial_before, 0x11223344u);
    test_gpu_write32(&m, partial_above, 0x55667788u);
    test_gpu_write32(&m, partial_after, 0x99aabbccu);

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBXCLIP, 0x01380008u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBYCLIP, 0x00700060u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    mismatches = 0u;
    for (uint32_t row = 0; row < PARTIAL_HEIGHT; row++) {
        for (uint32_t x = 0; x < PARTIAL_WIDTH; x++) {
            uint32_t actual = test_gpu_read32(&m,
                target + (PARTIAL_TOP + row) * TARGET_STRIDE +
                    (PARTIAL_LEFT + x) * 4u);
            mismatches += actual != partial_expected[row];
        }
    }
    CHECK(mismatches == 0u,
          "%u clipped background-overlay pixels mismatched", mismatches);
    CHECK(test_gpu_read32(&m, partial_before) == 0x11223344u &&
          test_gpu_read32(&m, partial_above) == 0x55667788u &&
          test_gpu_read32(&m, partial_after) == 0x99aabbccu,
          "clipped background overlay changed a pixel outside its boundary");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "clipped background overlay did not raise all three 3D events");

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);
    uint32_t partial_first = target + PARTIAL_TOP * TARGET_STRIDE +
                             PARTIAL_LEFT * 4u;
    test_gpu_write32(&m, partial_first, 0x89abcdefu);
    test_gpu_write32(&m, object + 0x0b8u, partial_boundary[0] ^ 1u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, partial_first) == 0x89abcdefu &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "unknown clipped boundary changed pixels or raised completion");

    s5l8900_free(&m);
}

static void test_second_tiled_status_glyph(void) {
    enum {
        LEFT = 155u, WIDTH = 10u, HEIGHT = 20u,
        SOURCE_STRIDE = 0x40u, TARGET_STRIDE = 0x500u
    };
    const uint32_t table0 = 0x08003000u;
    const uint32_t table2 = 0x08004000u;
    const uint32_t region = 0x00001000u;
    const uint32_t object = 0x00014000u;
    const uint32_t source = 0x00994000u;
    const uint32_t target = 0x00897000u;
    const uint32_t region_pa = 0x08010000u;
    const uint32_t object_pa = 0x08011000u;
    const uint32_t source_pa = 0x08012000u;
    const uint32_t target_pa = 0x08020000u;

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE), "machine init failed");
    if (!m.ram) return;

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART0, table0);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    test_map_gpu_page(&m, table0, region, region_pa);
    test_map_gpu_page(&m, table0, object, object_pa);
    test_map_gpu_page(&m, table2, source, source_pa);
    for (uint32_t page = 0; page < 7u; page++)
        test_map_gpu_page(&m, table2, target + page * 0x1000u,
                          target_pa + page * 0x1000u);

    uint32_t list = object + 0x68u;
    static const uint32_t tile_codes[4] = {
        0x00000013u, 0x00000014u, 0x00000113u, 0x80000114u
    };
    for (unsigned i = 0; i < 4u; i++) {
        test_gpu_write32(&m, region + i * 8u, tile_codes[i]);
        test_gpu_write32(&m, region + i * 8u + 4u, list);
    }
    static const uint32_t list_words[4] = {
        0x60200020u, 0x6020002du, 0x61a0007cu, 0xf0000000u
    };
    for (unsigned i = 0; i < 4u; i++)
        test_gpu_write32(&m, list + i * 4u, list_words[i]);

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
        uint32_t value = i == 2u
            ? 0x0e500000u | (target >> 7) : background[i];
        test_gpu_write32(&m, object + i * 4u, value);
    }

    static const struct mbx_test_word {
        uint16_t off;
        uint32_t value;
    } boundary[] = {
        {0x080u, 0x22206f80u}, {0x088u, 0x45800000u},
        {0x094u, 0x45800000u}, {0x098u, 0x45800000u},
        {0x09cu, 0x45800000u}, {0x0b4u, 0x22207f80u},
        {0x0b8u, 0x431b0000u}, {0x0bcu, 0x41a00000u},
        {0x0c0u, 0x431b0000u}, {0x0c8u, 0x43250000u},
        {0x0ccu, 0x41a00000u}, {0x0d0u, 0x43250000u},
        {0x0e8u, 0xe0000000u}, {0x0f4u, 0xa6887610u},
        {0x0f8u, 0x22220e80u}, {0x12cu, 0x3f800000u},
        {0x130u, 0x3f800000u}, {0x134u, 0x3f800000u},
        {0x138u, 0x3f800000u}, {0x198u, 0xe0000000u},
        {0x19cu, 0x22200e80u}, {0x1d0u, 0x3f800000u},
        {0x1d4u, 0x3f800000u}, {0x1d8u, 0x3f800000u},
        {0x1dcu, 0x3f800000u},
    };
    for (unsigned i = 0; i < sizeof boundary / sizeof boundary[0]; i++)
        test_gpu_write32(&m, object + boundary[i].off, boundary[i].value);

    static const uint32_t quad[44] = {
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
    };
    for (unsigned i = 0; i < 44u; i++) {
        uint32_t value = quad[i];
        if (i == 2u) value = 0x0e040000u | (source >> 7);
        if (i == 5u) value = 0x0e500000u | (target >> 7);
        test_gpu_write32(&m, object + 0x1f0u + i * 4u, value);
    }

    uint32_t expected[WIDTH * HEIGHT];
    for (uint32_t y = 0; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint32_t alpha = 0x80u + ((x * 11u + y * 7u) & 0x7fu);
            uint32_t src = (alpha << 24) | ((alpha * 3u / 4u) << 16) |
                           ((alpha / 2u) << 8) | (alpha / 4u);
            uint32_t dst = 0xff102030u + y * 0x00010101u + x;
            test_gpu_write32(&m, source + y * SOURCE_STRIDE + x * 4u, src);
            test_gpu_write32(&m,
                target + y * TARGET_STRIDE + (LEFT + x) * 4u, dst);
            expected[y * WIDTH + x] = test_over(
                dst, test_modulate_vertex_alpha(src, quad[24] >> 24));
        }
    }
    test_gpu_write32(&m, target + (LEFT - 1u) * 4u, 0x11223344u);
    test_gpu_write32(&m, target + LEFT * 4u + HEIGHT * TARGET_STRIDE,
                     0x55667788u);

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RGNBASE, region);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_OBJBASE, object);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_PIXSAMP, 0x00020007u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBCTL, 6u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBXCLIP, 0x00a80098u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBYCLIP, 0x00200000u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBSTART, target);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBSTRIDE, 320u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);

    uint32_t mismatches = 0u;
    for (uint32_t y = 0; y < HEIGHT; y++) {
        for (uint32_t x = 0; x < WIDTH; x++) {
            uint32_t actual = test_gpu_read32(&m,
                target + y * TARGET_STRIDE + (LEFT + x) * 4u);
            mismatches += actual != expected[y * WIDTH + x];
        }
    }
    CHECK(mismatches == 0u, "%u status-glyph pixels mismatched", mismatches);
    CHECK(test_gpu_read32(&m, target + (LEFT - 1u) * 4u) == 0x11223344u &&
          test_gpu_read32(&m, target + LEFT * 4u + HEIGHT * TARGET_STRIDE) ==
              0x55667788u,
          "status-glyph render changed a pixel outside its rectangle");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "completed status-glyph render did not raise all three events");

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);
    uint32_t first = target + LEFT * 4u;
    test_gpu_write32(&m, first, 0x89abcdefu);
    test_gpu_write32(&m, object + 0x1f0u + 3u * 4u, 0xcd206c41u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
          "unknown glyph texture control changed the destination");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "unknown glyph texture control raised completion");

    s5l8900_free(&m);
}

struct mbx_test_status_form {
    const char *name;
    uint32_t xclip, yclip;
    uint32_t target;
    uint32_t blend_surface;
    uint32_t list_word;
    bool variable_vertex_alpha;
    bool has_quad_variant;
    bool boundary_override;
    bool source_must_be_zero;
    uint32_t tile_x0, tile_x1, tile_y0, tile_y1;
    uint32_t left, top, width, height;
    uint32_t source, source_row0, source_stride, source_control;
    uint32_t boundary[8];
    uint32_t quad[44];
    uint32_t quad_variant[44];
};

static void test_captured_status_form(const struct mbx_test_status_form *form) {
    enum { TARGET_STRIDE = 0x500u, MAX_PIXELS = 320u * 480u };
    const uint32_t table0 = 0x08003000u;
    const uint32_t table2 = 0x08004000u;
    const uint32_t table3 = 0x08005000u;
    const uint32_t region = 0x00001000u;
    const uint32_t object = 0x00014000u;
    const uint32_t target = form->target ? form->target : 0x00897000u;
    const uint32_t region_pa = 0x08010000u;
    const uint32_t object_pa = 0x08014000u;
    const uint32_t source_pa = 0x08020000u;
    const uint32_t target_pa = 0x080c0000u;
    const uint32_t blend_pa = 0x08160000u;
    const uint32_t blend_surface = form->blend_surface
        ? form->blend_surface : target;
    const uint32_t tile_count =
        (form->tile_x1 - form->tile_x0 + 1u) *
        (form->tile_y1 - form->tile_y0 + 1u);

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE), "%s machine init failed",
          form->name);
    if (!m.ram) return;

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART0, table0);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART3, table3);
    uint32_t region_page0 = region & ~0xfffu;
    uint32_t region_last = region + tile_count * 8u - 1u;
    for (uint32_t page = region_page0; page <= (region_last & ~0xfffu);
         page += 0x1000u)
        test_map_gpu_page(&m, table0, page,
                          region_pa + (page - region_page0));
    test_map_gpu_page(&m, table0, object, object_pa);
    uint32_t source_page0 = form->source & ~0xfffu;
    uint32_t source_last = form->source +
                           (form->source_row0 + form->height - 1u) *
                               form->source_stride +
                           form->width * 4u - 1u;
    for (uint32_t page = source_page0; page <= (source_last & ~0xfffu);
         page += 0x1000u) {
        uint32_t table = (page >> 22) == 2u ? table2 : table3;
        test_map_gpu_page(&m, table, page,
                          source_pa + (page - source_page0));
    }
    uint32_t target_page0 = target & ~0xfffu;
    uint32_t target_last = target +
                           (form->top + form->height) * TARGET_STRIDE +
                           (form->left + form->width) * 4u - 1u;
    for (uint32_t page = target_page0; page <= (target_last & ~0xfffu);
         page += 0x1000u) {
        uint32_t table = (page >> 22) == 2u ? table2 : table3;
        test_map_gpu_page(&m, table, page,
                          target_pa + (page - target_page0));
    }
    if (blend_surface != target) {
        uint32_t blend_page0 = blend_surface & ~0xfffu;
        uint32_t blend_last = blend_surface +
                              (form->top + form->height - 1u) * TARGET_STRIDE +
                              (form->left + form->width) * 4u - 1u;
        for (uint32_t page = blend_page0; page <= (blend_last & ~0xfffu);
             page += 0x1000u) {
            uint32_t table = (page >> 22) == 2u ? table2 : table3;
            test_map_gpu_page(&m, table, page,
                              blend_pa + (page - blend_page0));
        }
    }

    uint32_t list = object + 0x68u;
    uint32_t tile_index = 0u;
    for (uint32_t y = form->tile_y0; y <= form->tile_y1; y++) {
        for (uint32_t x = form->tile_x0; x <= form->tile_x1; x++) {
            uint32_t code = (y << 8) | x;
            if (tile_index + 1u == tile_count) code |= 0x80000000u;
            test_gpu_write32(&m, region + tile_index * 8u, code);
            test_gpu_write32(&m, region + tile_index * 8u + 4u, list);
            tile_index++;
        }
    }
    CHECK(test_gpu_read32(&m, region) ==
              ((form->tile_y0 << 8) | form->tile_x0) &&
          test_gpu_read32(&m, region + (tile_count - 1u) * 8u) ==
              (0x80000000u | (form->tile_y1 << 8) | form->tile_x1),
          "%s region fixture lost its first or final tile", form->name);
    uint32_t region_mismatches = 0u;
    for (uint32_t i = 0; i < tile_count; i++) {
        uint32_t x = form->tile_x0 +
                     (i % (form->tile_x1 - form->tile_x0 + 1u));
        uint32_t y = form->tile_y0 +
                     (i / (form->tile_x1 - form->tile_x0 + 1u));
        uint32_t code = (y << 8) | x;
        if (i + 1u == tile_count) code |= 0x80000000u;
        region_mismatches += test_gpu_read32(&m, region + i * 8u) != code;
        region_mismatches += test_gpu_read32(&m, region + i * 8u + 4u) != list;
    }
    CHECK(region_mismatches == 0u,
          "%s region fixture has %u mismatched words",
          form->name, region_mismatches);
    const uint32_t list_words[4] = {
        0x60200020u, 0x6020002du,
        form->list_word ? form->list_word : 0x61a0007cu,
        0xf0000000u
    };
    for (unsigned i = 0; i < 4u; i++)
        test_gpu_write32(&m, list + i * 4u, list_words[i]);

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
        uint32_t value = i == 2u
            ? 0x0e500000u | (target >> 7) : background[i];
        test_gpu_write32(&m, object + i * 4u, value);
    }

    static const struct mbx_test_word {
        uint16_t off;
        uint32_t value;
    } boundary[] = {
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
    for (unsigned i = 0; i < sizeof boundary / sizeof boundary[0]; i++)
        test_gpu_write32(&m, object + boundary[i].off, boundary[i].value);
    for (unsigned i = 0; i < 8u; i++)
        test_gpu_write32(&m, object + 0x0b8u + i * 4u,
            form->boundary_override ? form->boundary[i] : form->quad[8u + i]);

    for (unsigned i = 0; i < 44u; i++) {
        uint32_t value = form->quad[i];
        if (i == 2u) value = form->source_control | (form->source >> 7);
        if (i == 5u) value = 0x0e500000u | (blend_surface >> 7);
        test_gpu_write32(&m, object + 0x1f0u + i * 4u, value);
    }

    uint64_t pixel_count = (uint64_t)form->width * form->height;
    CHECK(pixel_count <= MAX_PIXELS, "%s fixture rectangle is too large",
          form->name);
    if (pixel_count > MAX_PIXELS) {
        s5l8900_free(&m);
        return;
    }
    uint32_t *expected = malloc((size_t)pixel_count * sizeof *expected);
    CHECK(expected != NULL, "%s expected-pixel allocation failed", form->name);
    if (!expected) {
        s5l8900_free(&m);
        return;
    }
    for (uint32_t y = 0; y < form->height; y++) {
        for (uint32_t x = 0; x < form->width; x++) {
            uint32_t alpha = 0x40u + ((x * 11u + y * 7u) & 0xbfu);
            uint32_t src = form->source_must_be_zero ? 0u :
                (alpha << 24) | ((alpha * 3u / 4u) << 16) |
                ((alpha / 2u) << 8) | (alpha / 4u);
            uint32_t dst = 0xff102030u + y * 0x00010101u + x;
            uint32_t background = blend_surface == target ? dst :
                0xff405060u + y * 0x00010101u + x;
            test_gpu_write32(&m,
                form->source + (form->source_row0 + y) *
                    form->source_stride + x * 4u, src);
            test_gpu_write32(&m,
                target + (form->top + y) * TARGET_STRIDE +
                    (form->left + x) * 4u, dst);
            if (blend_surface != target)
                test_gpu_write32(&m,
                    blend_surface + (form->top + y) * TARGET_STRIDE +
                        (form->left + x) * 4u, background);
            uint32_t modulated = test_modulate_vertex_alpha(
                src, form->quad[24] >> 24);
            expected[y * form->width + x] = test_over(background, modulated);
        }
    }
    bool before_below = form->left == 0u;
    uint32_t before = target + (form->top + form->height) * TARGET_STRIDE +
                      form->left * 4u;
    if (!before_below)
        before = target + form->top * TARGET_STRIDE +
                 (form->left - 1u) * 4u;
    uint32_t after = target + (form->top + form->height) * TARGET_STRIDE +
                     (form->left + (before_below ? 1u : 0u)) * 4u;
    test_gpu_write32(&m, before, 0x11223344u);
    test_gpu_write32(&m, after, 0x55667788u);

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RGNBASE, region);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_OBJBASE, object);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_PIXSAMP, 0x00020007u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBCTL, 6u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBXCLIP, form->xclip);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBYCLIP, form->yclip);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBSTART, target);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBSTRIDE, 320u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);

    uint32_t mismatches = 0u;
    for (uint32_t y = 0; y < form->height; y++) {
        for (uint32_t x = 0; x < form->width; x++) {
            uint32_t actual = test_gpu_read32(&m,
                target + (form->top + y) * TARGET_STRIDE +
                    (form->left + x) * 4u);
            mismatches += actual != expected[y * form->width + x];
        }
    }
    CHECK(mismatches == 0u, "%s: %u pixels mismatched",
          form->name, mismatches);
    CHECK(test_gpu_read32(&m, before) == 0x11223344u &&
          test_gpu_read32(&m, after) == 0x55667788u,
          "%s changed a pixel outside its rectangle", form->name);
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "%s did not raise all three completion events", form->name);

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);
    uint32_t first = target + form->top * TARGET_STRIDE + form->left * 4u;
    if (form->has_quad_variant) {
        CHECK(form->source_must_be_zero && blend_surface != target,
              "%s variant fixture cannot verify its pixels safely", form->name);
        for (unsigned i = 0; i < 44u; i++) {
            uint32_t value = form->quad_variant[i];
            if (i == 2u)
                value = form->source_control | (form->source >> 7);
            if (i == 5u)
                value = 0x0e500000u | (blend_surface >> 7);
            test_gpu_write32(&m, object + 0x1f0u + i * 4u, value);
        }
        test_gpu_write32(&m, first, 0x89abcdefu);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
        CHECK(test_gpu_read32(&m, first) == expected[0],
              "%s captured quad variant rendered the wrong pixel", form->name);
        CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
              "%s captured quad variant did not complete", form->name);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);

        unsigned differing_words = 0u;
        unsigned mixed_word = 44u;
        for (unsigned i = 0; i < 44u; i++) {
            if (i != 2u && i != 5u &&
                form->quad[i] != form->quad_variant[i]) {
                if (mixed_word == 44u) mixed_word = i;
                differing_words++;
            }
        }
        CHECK(differing_words > 1u,
              "%s quad variants do not have a testable mixture", form->name);
        if (mixed_word < 44u)
            test_gpu_write32(&m, object + 0x1f0u + mixed_word * 4u,
                             form->quad[mixed_word]);
        test_gpu_write32(&m, first, 0x89abcdefu);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
        CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
              "%s mixed quad variants changed the destination", form->name);
        CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
              "%s mixed quad variants raised completion", form->name);

        for (unsigned i = 0; i < 44u; i++) {
            uint32_t value = form->quad[i];
            if (i == 2u)
                value = form->source_control | (form->source >> 7);
            if (i == 5u)
                value = 0x0e500000u | (blend_surface >> 7);
            test_gpu_write32(&m, object + 0x1f0u + i * 4u, value);
        }
    }
    if (form->variable_vertex_alpha) {
        test_gpu_write32(&m, first, 0x89abcdefu);
        test_gpu_write32(&m, object + 0x264u, form->quad[29] ^ 0x01000000u);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
        CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
              "%s mismatched vertex alpha changed the destination", form->name);
        CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
              "%s mismatched vertex alpha raised completion", form->name);
        test_gpu_write32(&m, object + 0x264u, form->quad[29]);

        test_gpu_write32(&m, object + 0x250u, form->quad[24] | 1u);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
        CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
              "%s vertex colour bits changed the destination", form->name);
        CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
              "%s vertex colour bits raised completion", form->name);
        test_gpu_write32(&m, object + 0x250u, form->quad[24]);
    }
    if (form->source_must_be_zero) {
        test_gpu_write32(&m, first, 0x89abcdefu);
        uint32_t first_source = form->source +
                                form->source_row0 * form->source_stride;
        test_gpu_write32(&m, first_source, 0x01010101u);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
        CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
              "%s nonzero transparent source changed the destination",
              form->name);
        CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
              "%s nonzero transparent source raised completion", form->name);
        test_gpu_write32(&m, first_source, 0u);
    }

    test_gpu_write32(&m, first, 0x89abcdefu);
    test_gpu_write32(&m, object + 0x1f4u, form->quad[1] ^ 1u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
          "%s unknown control changed the destination", form->name);
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "%s unknown control raised completion", form->name);

    free(expected);
    s5l8900_free(&m);
}

static void test_later_tiled_status_sprites(void) {
    static const struct mbx_test_status_form forms[] = {
        {
            .name = "Searching status sprite",
            .xclip = 0x00500000u, .yclip = 0x00200000u,
            .tile_x0 = 0u, .tile_x1 = 9u, .tile_y0 = 0u, .tile_y1 = 1u,
            .left = 4u, .top = 1u, .width = 76u, .height = 16u,
            .source = 0x00995080u, .source_stride = 0x140u,
            .source_control = 0x0e140000u,
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
            .name = "battery status sprite",
            .xclip = 0x01400128u, .yclip = 0x00200000u,
            .tile_x0 = 0x25u, .tile_x1 = 0x27u,
            .tile_y0 = 0u, .tile_y1 = 1u,
            .left = 296u, .top = 0u, .width = 21u, .height = 20u,
            .source = 0x00997000u, .source_stride = 0x60u,
            .source_control = 0x0e040000u,
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
            .name = "clipped padlock tail",
            .xclip = 0x00a80098u, .yclip = 0x00200010u,
            .tile_x0 = 0x13u, .tile_x1 = 0x14u,
            .tile_y0 = 1u, .tile_y1 = 1u,
            .left = 155u, .top = 16u, .width = 10u, .height = 4u,
            .source = 0x00994000u, .source_row0 = 16u,
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
            .name = "clipped battery tail",
            .xclip = 0x01400128u, .yclip = 0x00200010u,
            .tile_x0 = 0x25u, .tile_x1 = 0x27u,
            .tile_y0 = 1u, .tile_y1 = 1u,
            .left = 296u, .top = 16u, .width = 21u, .height = 4u,
            .source = 0x00997000u, .source_row0 = 16u,
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
            .name = "full-width status-bar time sprite",
            .xclip = 0x01400000u, .yclip = 0x00200000u,
            .target = 0x00998000u,
            .tile_x0 = 0u, .tile_x1 = 0x27u,
            .tile_y0 = 0u, .tile_y1 = 1u,
            .left = 0u, .top = 0u, .width = 320u, .height = 20u,
            .source = 0x00a3a080u, .source_stride = 0x500u,
            .source_control = 0x0e500000u,
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
            .name = "full-width status-bar time sprite on CLCD surface",
            .xclip = 0x01400000u, .yclip = 0x00200000u,
            .target = 0x00897000u,
            .tile_x0 = 0u, .tile_x1 = 0x27u,
            .tile_y0 = 0u, .tile_y1 = 1u,
            .left = 0u, .top = 0u, .width = 320u, .height = 20u,
            .source = 0x00a3a080u, .source_stride = 0x500u,
            .source_control = 0x0e500000u,
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
            .name = "full-width status-bar time sprite on transition surface",
            .xclip = 0x01400000u, .yclip = 0x00200000u,
            .target = 0x00a41000u,
            .tile_x0 = 0u, .tile_x1 = 0x27u,
            .tile_y0 = 0u, .tile_y1 = 1u,
            .left = 0u, .top = 0u, .width = 320u, .height = 20u,
            .source = 0x00a3a080u, .source_stride = 0x500u,
            .source_control = 0x0e500000u,
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
            .name = "transparent clipped battery tail on transition surface",
            .xclip = 0x01400000u, .yclip = 0x00200000u,
            .target = 0x00a41000u,
            .list_word = 0x612000a8u,
            .boundary_override = true, .source_must_be_zero = true,
            .tile_x0 = 0u, .tile_x1 = 0x27u,
            .tile_y0 = 0u, .tile_y1 = 1u,
            .left = 296u, .top = 16u, .width = 21u, .height = 4u,
            .source = 0x00986000u, .source_row0 = 16u,
            .source_stride = 0x60u, .source_control = 0x0e040000u,
            .boundary = {
                0x00000000u, 0x41a00000u, 0x00000000u, 0x00000000u,
                0x43a00000u, 0x41a00000u, 0x43a00000u, 0x00000000u,
            },
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
            .name = "transparent clipped battery tail on middle surface",
            .xclip = 0x01400000u, .yclip = 0x00200000u,
            .target = 0x00998000u,
            .list_word = 0x612000a8u,
            .boundary_override = true, .source_must_be_zero = true,
            .tile_x0 = 0u, .tile_x1 = 0x27u,
            .tile_y0 = 0u, .tile_y1 = 1u,
            .left = 296u, .top = 16u, .width = 21u, .height = 4u,
            .source = 0x00986000u, .source_row0 = 16u,
            .source_stride = 0x60u, .source_control = 0x0e040000u,
            .boundary = {
                0x00000000u, 0x41a00000u, 0x00000000u, 0x00000000u,
                0x43a00000u, 0x41a00000u, 0x43a00000u, 0x00000000u,
            },
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
            .name = "transparent clipped battery tail on CLCD surface",
            .xclip = 0x01400000u, .yclip = 0x00200000u,
            .target = 0x00897000u,
            .list_word = 0x612000a8u,
            .boundary_override = true, .source_must_be_zero = true,
            .tile_x0 = 0u, .tile_x1 = 0x27u,
            .tile_y0 = 0u, .tile_y1 = 1u,
            .left = 296u, .top = 16u, .width = 21u, .height = 4u,
            .source = 0x00986000u, .source_row0 = 16u,
            .source_stride = 0x60u, .source_control = 0x0e040000u,
            .boundary = {
                0x00000000u, 0x41a00000u, 0x00000000u, 0x00000000u,
                0x43a00000u, 0x41a00000u, 0x43a00000u, 0x00000000u,
            },
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
            .name = "lower-screen opacity composite on transition surface",
            .xclip = 0x01400000u, .yclip = 0x01e00010u,
            .target = 0x00a41000u,
            .tile_x0 = 0u, .tile_x1 = 0x27u,
            .tile_y0 = 1u, .tile_y1 = 0x1du,
            .left = 0u, .top = 20u, .width = 320u, .height = 460u,
            .source = 0x00b12080u, .source_row0 = 20u,
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
            .name = "tutorial popup on transition surface",
            .xclip = 0x01300010u, .yclip = 0x01800080u,
            .target = 0x00a41000u,
            .tile_x0 = 2u, .tile_x1 = 0x25u,
            .tile_y0 = 8u, .tile_y1 = 0x17u,
            .left = 18u, .top = 130u, .width = 284u, .height = 241u,
            .source = 0x00ba9080u,
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
            .name = "tutorial title on transition surface",
            .xclip = 0x01280018u, .yclip = 0x00b00090u,
            .target = 0x00a41000u,
            .tile_x0 = 3u, .tile_x1 = 0x24u,
            .tile_y0 = 9u, .tile_y1 = 0x0au,
            .left = 30u, .top = 145u, .width = 260u, .height = 23u,
            .source = 0x00bed080u,
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
            .name = "tutorial body on transition surface",
            .xclip = 0x01280018u, .yclip = 0x013000a0u,
            .target = 0x00a41000u,
            .tile_x0 = 3u, .tile_x1 = 0x24u,
            .tile_y0 = 0x0au, .tile_y1 = 0x12u,
            .left = 30u, .top = 175u, .width = 260u, .height = 121u,
            .source = 0x00bf3080u,
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
            .name = "tutorial dismiss button on transition surface",
            .xclip = 0x01280018u, .yclip = 0x01700130u,
            .target = 0x00a41000u,
            .tile_x0 = 3u, .tile_x1 = 0x24u,
            .tile_y0 = 0x13u, .tile_y1 = 0x16u,
            .left = 29u, .top = 312u, .width = 262u, .height = 43u,
            .source = 0x00c2b080u,
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
            .name = "transparent clipped battery transfer",
            .xclip = 0x01400000u, .yclip = 0x01e00010u,
            .target = 0x00897000u, .blend_surface = 0x00998000u,
            .list_word = 0x612000a8u,
            .has_quad_variant = true,
            .boundary_override = true, .source_must_be_zero = true,
            .tile_x0 = 0u, .tile_x1 = 0x27u,
            .tile_y0 = 1u, .tile_y1 = 0x1du,
            .left = 296u, .top = 16u, .width = 21u, .height = 4u,
            .source = 0x00986000u, .source_row0 = 16u,
            .source_stride = 0x60u, .source_control = 0x0e040000u,
            .boundary = {
                0x00000000u, 0x43f00000u, 0x00000000u, 0x41a00000u,
                0x43a00000u, 0x43f00000u, 0x43a00000u, 0x41a00000u,
            },
            .quad = {
                0xe0000000u, 0xa2218001u, 0u, 0xd6887610u,
                0xa7718000u, 0u, 0xab504a90u, 0x22250e80u,
                0x43940000u, 0x41a00000u, 0x43940000u, 0x00000000u,
                0x439e8000u, 0x41a00000u, 0x439e8000u, 0x00000000u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0x00000000u, 0x00000000u, 0x3f200000u, 0x3e940000u,
                0x3ca00000u, 0x00000000u, 0x00000000u, 0x00000000u,
                0x3e940000u, 0x00000000u, 0x00000000u, 0x3f280000u,
                0x3f200000u, 0x3e9e8000u, 0x3ca00000u, 0x00000000u,
                0x3f280000u, 0x00000000u, 0x3e9e8000u, 0x00000000u,
            },
            .quad_variant = {
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
            .name = "transparent clipped battery transfer to new surface",
            .xclip = 0x01400000u, .yclip = 0x01e00010u,
            .target = 0x00a41000u, .blend_surface = 0x00897000u,
            .list_word = 0x612000a8u,
            .boundary_override = true, .source_must_be_zero = true,
            .tile_x0 = 0u, .tile_x1 = 0x27u,
            .tile_y0 = 1u, .tile_y1 = 0x1du,
            .left = 296u, .top = 16u, .width = 21u, .height = 4u,
            .source = 0x00986000u, .source_row0 = 16u,
            .source_stride = 0x60u, .source_control = 0x0e040000u,
            .boundary = {
                0x00000000u, 0x43f00000u, 0x00000000u, 0x41a00000u,
                0x43a00000u, 0x43f00000u, 0x43a00000u, 0x41a00000u,
            },
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
            .name = "slider label on CLCD surface",
            .xclip = 0x01180070u, .yclip = 0x01c001a0u,
            .target = 0x00897000u,
            .variable_vertex_alpha = true,
            .tile_x0 = 0x0eu, .tile_x1 = 0x22u,
            .tile_y0 = 0x1au, .tile_y1 = 0x1bu,
            .left = 114u, .top = 417u, .width = 161u, .height = 30u,
            .source = 0x00a2e080u, .source_stride = 0x2a0u,
            .source_control = 0x0e280000u,
            .quad = {
                0xe0000000u, 0xa5218001u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x42e40000u, 0x43df8000u, 0x42e40000u, 0x43d08000u,
                0x43898000u, 0x43df8000u, 0x43898000u, 0x43d08000u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xb4000000u, 0u, 0x3f700000u, 0x3de40000u,
                0x3edf8000u, 0xb4000000u, 0u, 0u,
                0x3de40000u, 0x3ed08000u, 0xb4000000u, 0x3f210000u,
                0x3f700000u, 0x3e898000u, 0x3edf8000u, 0xb4000000u,
                0x3f210000u, 0u, 0x3e898000u, 0x3ed08000u,
            },
        },
        {
            .name = "slider label on back surface",
            .xclip = 0x01180070u, .yclip = 0x01c001a0u,
            .target = 0x00a33000u,
            .variable_vertex_alpha = true,
            .tile_x0 = 0x0eu, .tile_x1 = 0x22u,
            .tile_y0 = 0x1au, .tile_y1 = 0x1bu,
            .left = 114u, .top = 417u, .width = 161u, .height = 30u,
            .source = 0x00a2e080u, .source_stride = 0x2a0u,
            .source_control = 0x0e280000u,
            .quad = {
                0xe0000000u, 0xa5218001u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x42e40000u, 0x43df8000u, 0x42e40000u, 0x43d08000u,
                0x43898000u, 0x43df8000u, 0x43898000u, 0x43d08000u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0x8a000000u, 0u, 0x3f700000u, 0x3de40000u,
                0x3edf8000u, 0x8a000000u, 0u, 0u,
                0x3de40000u, 0x3ed08000u, 0x8a000000u, 0x3f210000u,
                0x3f700000u, 0x3e898000u, 0x3edf8000u, 0x8a000000u,
                0x3f210000u, 0u, 0x3e898000u, 0x3ed08000u,
            },
        },
        {
            .name = "slider label on middle surface",
            .xclip = 0x01180070u, .yclip = 0x01c001a0u,
            .target = 0x00998000u,
            .variable_vertex_alpha = true,
            .tile_x0 = 0x0eu, .tile_x1 = 0x22u,
            .tile_y0 = 0x1au, .tile_y1 = 0x1bu,
            .left = 114u, .top = 417u, .width = 161u, .height = 30u,
            .source = 0x00a2e080u, .source_stride = 0x2a0u,
            .source_control = 0x0e280000u,
            .quad = {
                0xe0000000u, 0xa5218001u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x42e40000u, 0x43df8000u, 0x42e40000u, 0x43d08000u,
                0x43898000u, 0x43df8000u, 0x43898000u, 0x43d08000u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0x8a000000u, 0u, 0x3f700000u, 0x3de40000u,
                0x3edf8000u, 0x8a000000u, 0u, 0u,
                0x3de40000u, 0x3ed08000u, 0x8a000000u, 0x3f210000u,
                0x3f700000u, 0x3e898000u, 0x3edf8000u, 0x8a000000u,
                0x3f210000u, 0u, 0x3e898000u, 0x3ed08000u,
            },
        },
        {
            .name = "slider label progressed back surface",
            .xclip = 0x01180070u, .yclip = 0x01c001a0u,
            .target = 0x00a33000u,
            .variable_vertex_alpha = true,
            .tile_x0 = 0x0eu, .tile_x1 = 0x22u,
            .tile_y0 = 0x1au, .tile_y1 = 0x1bu,
            .left = 114u, .top = 417u, .width = 161u, .height = 30u,
            .source = 0x00a2e080u, .source_stride = 0x2a0u,
            .source_control = 0x0e280000u,
            .quad = {
                0xe0000000u, 0xa5218001u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x42e40000u, 0x43df8000u, 0x42e40000u, 0x43d08000u,
                0x43898000u, 0x43df8000u, 0x43898000u, 0x43d08000u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0x61000000u, 0u, 0x3f700000u, 0x3de40000u,
                0x3edf8000u, 0x61000000u, 0u, 0u,
                0x3de40000u, 0x3ed08000u, 0x61000000u, 0x3f210000u,
                0x3f700000u, 0x3e898000u, 0x3edf8000u, 0x61000000u,
                0x3f210000u, 0u, 0x3e898000u, 0x3ed08000u,
            },
        },
    };
    for (unsigned i = 0; i < sizeof forms / sizeof forms[0]; i++)
        test_captured_status_form(&forms[i]);

    struct mbx_test_status_form phase =
        forms[sizeof forms / sizeof forms[0] - 1u];
    phase.name = "slider label later back-surface phase";
    phase.quad[24] = phase.quad[29] = phase.quad[34] = phase.quad[39] =
        0x37000000u;
    test_captured_status_form(&phase);

    phase.name = "slider label progressed CLCD-surface phase";
    phase.target = 0x00897000u;
    phase.quad[24] = phase.quad[29] = phase.quad[34] = phase.quad[39] =
        0x61000000u;
    test_captured_status_form(&phase);
}

int main(void) {
    printf("PowerVR MBX2D tests\n");
    test_translated_copy_and_completion_boundary();
    test_unknown_packet_and_bad_gart_are_atomic();
    test_full_lower_surface_black_fill();
    test_split_lower_surface_black_fill();
    test_status_write_to_set_and_ack();
    test_premultiplied_2d_clock_form();
    test_opaque_global_alpha_2d_form();
    test_ordered_atomic_2d_batches();
    test_first_tiled_premultiplied_over();
    test_second_tiled_status_glyph();
    test_later_tiled_status_sprites();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
