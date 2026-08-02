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

static void test_map_gpu_page(s5l8900_t *m, uint32_t table,
                              uint32_t gpu, uint32_t pa) {
    m->bus.write32(m->bus.ctx,
        table + (((gpu >> 12) & 0x3ffu) * 4u), pa);
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
            expected[y * WIDTH + x] = test_over(dst, src);
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
    uint32_t xclip;
    uint32_t tile_x0, tile_x1, tile_y0, tile_y1;
    uint32_t left, top, width, height;
    uint32_t source, source_stride, source_control;
    uint32_t quad[44];
};

static void test_captured_status_form(const struct mbx_test_status_form *form) {
    enum { TARGET_STRIDE = 0x500u, MAX_PIXELS = 76u * 20u };
    const uint32_t table0 = 0x08003000u;
    const uint32_t table2 = 0x08004000u;
    const uint32_t region = 0x00001000u;
    const uint32_t object = 0x00014000u;
    const uint32_t target = 0x00897000u;
    const uint32_t region_pa = 0x08010000u;
    const uint32_t object_pa = 0x08011000u;
    const uint32_t source_pa = 0x08012000u;
    const uint32_t target_pa = 0x08020000u;

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE), "%s machine init failed",
          form->name);
    if (!m.ram) return;

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART0, table0);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    test_map_gpu_page(&m, table0, region, region_pa);
    test_map_gpu_page(&m, table0, object, object_pa);
    uint32_t source_page0 = form->source & ~0xfffu;
    uint32_t source_last = form->source +
                           (form->height - 1u) * form->source_stride +
                           form->width * 4u - 1u;
    for (uint32_t page = source_page0; page <= (source_last & ~0xfffu);
         page += 0x1000u)
        test_map_gpu_page(&m, table2, page,
                          source_pa + (page - source_page0));
    for (uint32_t page = 0; page < 8u; page++)
        test_map_gpu_page(&m, table2, target + page * 0x1000u,
                          target_pa + page * 0x1000u);

    uint32_t list = object + 0x68u;
    uint32_t tile_count = (form->tile_x1 - form->tile_x0 + 1u) *
                          (form->tile_y1 - form->tile_y0 + 1u);
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
    test_gpu_write32(&m, object + 0x0b8u, form->quad[8]);
    test_gpu_write32(&m, object + 0x0bcu, form->quad[9]);
    test_gpu_write32(&m, object + 0x0c0u, form->quad[10]);
    test_gpu_write32(&m, object + 0x0c4u, form->quad[11]);
    test_gpu_write32(&m, object + 0x0c8u, form->quad[12]);
    test_gpu_write32(&m, object + 0x0ccu, form->quad[13]);
    test_gpu_write32(&m, object + 0x0d0u, form->quad[14]);
    test_gpu_write32(&m, object + 0x0d4u, form->quad[15]);

    for (unsigned i = 0; i < 44u; i++) {
        uint32_t value = form->quad[i];
        if (i == 2u) value = form->source_control | (form->source >> 7);
        if (i == 5u) value = 0x0e500000u | (target >> 7);
        test_gpu_write32(&m, object + 0x1f0u + i * 4u, value);
    }

    uint32_t expected[MAX_PIXELS];
    for (uint32_t y = 0; y < form->height; y++) {
        for (uint32_t x = 0; x < form->width; x++) {
            uint32_t alpha = 0x40u + ((x * 11u + y * 7u) & 0xbfu);
            uint32_t src = (alpha << 24) | ((alpha * 3u / 4u) << 16) |
                           ((alpha / 2u) << 8) | (alpha / 4u);
            uint32_t dst = 0xff102030u + y * 0x00010101u + x;
            test_gpu_write32(&m,
                form->source + y * form->source_stride + x * 4u, src);
            test_gpu_write32(&m,
                target + (form->top + y) * TARGET_STRIDE +
                    (form->left + x) * 4u, dst);
            expected[y * form->width + x] = test_over(dst, src);
        }
    }
    uint32_t before = target + form->top * TARGET_STRIDE +
                      (form->left - 1u) * 4u;
    uint32_t after = target + (form->top + form->height) * TARGET_STRIDE +
                     form->left * 4u;
    test_gpu_write32(&m, before, 0x11223344u);
    test_gpu_write32(&m, after, 0x55667788u);

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RGNBASE, region);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_OBJBASE, object);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_PIXSAMP, 0x00020007u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBCTL, 6u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBXCLIP, form->xclip);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBYCLIP, 0x00200000u);
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
    test_gpu_write32(&m, first, 0x89abcdefu);
    test_gpu_write32(&m, object + 0x1f4u, form->quad[1] ^ 1u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
          "%s unknown control changed the destination", form->name);
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "%s unknown control raised completion", form->name);

    s5l8900_free(&m);
}

static void test_later_tiled_status_sprites(void) {
    static const struct mbx_test_status_form forms[] = {
        {
            .name = "Searching status sprite", .xclip = 0x00500000u,
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
            .name = "battery status sprite", .xclip = 0x01400128u,
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
    };
    for (unsigned i = 0; i < sizeof forms / sizeof forms[0]; i++)
        test_captured_status_form(&forms[i]);
}

int main(void) {
    printf("PowerVR MBX2D tests\n");
    test_translated_copy_and_completion_boundary();
    test_unknown_packet_and_bad_gart_are_atomic();
    test_status_write_to_set_and_ack();
    test_premultiplied_2d_clock_form();
    test_first_tiled_premultiplied_over();
    test_second_tiled_status_glyph();
    test_later_tiled_status_sprites();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
