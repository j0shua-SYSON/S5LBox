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
    CHECK(s5l8900_set_direct_ram_writes(&m, false),
          "observer-path fixture could not revoke direct RAM writes");

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

static uint32_t test_float_word(float value) {
    uint32_t word = 0u;
    memcpy(&word, &value, sizeof word);
    return word;
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

static float test_float_value(uint32_t word) {
    float value = 0.0f;
    memcpy(&value, &word, sizeof value);
    return value;
}

static uint32_t test_sprite_source_pixel(uint32_t x, uint32_t y) {
    uint32_t alpha = 0x40u + ((x * 11u + y * 7u) & 0xbfu);
    return (alpha << 24) | ((alpha * 3u / 4u) << 16) |
           ((alpha / 2u) << 8) | (alpha / 4u);
}

struct test_bilinear_axis {
    uint32_t first, second, weight;
};

static bool test_bilinear_coordinate(float coordinate, uint32_t dimension,
                                     struct test_bilinear_axis *axis) {
    if (!axis || !dimension || coordinate < 0.0f) return false;
    float fixed_float = coordinate * 65536.0f;
    if (fixed_float > (float)INT32_MAX) return false;
    int64_t raw = (int64_t)(int32_t)fixed_float - INT64_C(32768);
    int64_t neighbour = raw + INT64_C(65536);
    uint32_t maximum = (dimension << 16) - 1u;
    uint32_t first_fixed = raw < 0 ? 0u :
        (uint64_t)raw > maximum ? maximum : (uint32_t)raw;
    uint32_t second_fixed = neighbour < 0 ? 0u :
        (uint64_t)neighbour > maximum ? maximum : (uint32_t)neighbour;
    axis->first = first_fixed >> 16;
    axis->second = second_fixed >> 16;
    axis->weight = (first_fixed & 0xffffu) >> 8;
    return true;
}

static bool test_bilinear_axis(float origin, float span,
                               float texel_origin, float texel_span,
                               uint32_t pixel, uint32_t dimension,
                               struct test_bilinear_axis *axis) {
    if (!axis || !dimension || span <= 0.0f) return false;
    float coordinate = texel_origin +
                       ((float)pixel + 0.5f - origin) *
                       (texel_span / span);
    return test_bilinear_coordinate(coordinate, dimension, axis);
}

struct test_affine_transform {
    float origin_x, origin_y;
    float u_x, u_y;
    float v_x, v_y;
    float determinant;
};

static bool test_affine_pixel(const struct test_affine_transform *transform,
                              uint32_t x, uint32_t y,
                              float *u_fraction, float *v_fraction) {
    if (!transform || !u_fraction || !v_fraction ||
        transform->determinant <= 0.0f)
        return false;
    float dx = (float)x + 0.5f - transform->origin_x;
    float dy = (float)y + 0.5f - transform->origin_y;
    float u = (dx * transform->v_y - dy * transform->v_x) /
              transform->determinant;
    float v = (transform->u_x * dy - transform->u_y * dx) /
              transform->determinant;
    if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f)
        return false;
    *u_fraction = u;
    *v_fraction = v;
    return true;
}

/* Independent scalar reference for QuartzCore's packed interpolation.  Signed
 * floor division is written out so this test does not merely copy the packed
 * production implementation it is meant to check. */
static uint32_t test_linear_bgra8(uint32_t first, uint32_t second,
                                  uint32_t weight) {
    uint32_t out = 0u;
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
        int32_t a = (int32_t)((first >> shift) & 0xffu);
        int32_t b = (int32_t)((second >> shift) & 0xffu);
        int32_t product = (int32_t)weight * (b - a);
        int32_t quotient = product >= 0
            ? product / 256 : -((-product + 255) / 256);
        out |= (uint32_t)((a + quotient) & 0xff) << shift;
    }
    return out;
}

static uint32_t test_bilinear_sprite_pixel(
        const struct test_bilinear_axis *x,
        const struct test_bilinear_axis *y) {
    uint32_t top_left = test_sprite_source_pixel(x->first, y->first);
    uint32_t bottom_left = test_sprite_source_pixel(x->first, y->second);
    uint32_t top_right = test_sprite_source_pixel(x->second, y->first);
    uint32_t bottom_right = test_sprite_source_pixel(x->second, y->second);
    uint32_t vertical_left =
        test_linear_bgra8(top_left, bottom_left, y->weight);
    uint32_t vertical_right =
        test_linear_bgra8(top_right, bottom_right, y->weight);
    return test_linear_bgra8(vertical_left, vertical_right, x->weight);
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

static void test_springboard_settings_black_fill_batch(void) {
    enum { STRIDE = 0x500u, WIDTH = 320u, HEIGHT = 480u };
    const uint32_t table2 = 0x08003000u;
    const uint32_t target = 0x00897000u;
    const uint32_t target_pa = 0x08040000u;
    static const uint16_t rects[6][4] = {
        { 3u, 32u, 317u, 107u },
        { 79u, 107u, 165u, 108u },
        { 3u, 120u, 317u, 195u },
        { 3u, 208u, 317u, 283u },
        { 3u, 296u, 241u, 371u },
        { 136u, 375u, 178u, 385u },
    };
    static const uint32_t packet_template[16] = {
        0xa0060500u, 0x00897000u, 0x94060500u, 0x00000000u,
        0x30000000u, 0x60800200u, 0x8000f0f0u, 0xff000000u,
        0u, 0u, 0x70000000u, 0x70000000u,
        0x70000000u, 0x70000000u, 0x70000000u, 0x70000000u,
    };
    uint32_t packets[6][16];
    uint64_t expected_bytes = 0u;

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE),
          "Settings-fill machine init failed");
    if (!m.ram) return;
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    for (uint32_t page = 0; page < STRIDE * HEIGHT; page += 0x1000u)
        test_map_gpu_page(&m, table2, target + page, target_pa + page);

    for (unsigned i = 0; i < 6u; i++) {
        memcpy(packets[i], packet_template, sizeof packet_template);
        packets[i][8] = ((uint32_t)rects[i][0] << 16) | rects[i][1];
        packets[i][9] = ((uint32_t)rects[i][2] << 16) | rects[i][3];
        write_packet(&m, RING + i * 0x40u, packets[i], 16u);
        expected_bytes += (uint64_t)(rects[i][2] - rects[i][0]) *
                          (uint64_t)(rects[i][3] - rects[i][1]) * 4u;
    }
    test_gpu_write32(&m, target + 31u * STRIDE + 3u * 4u, 0xff112233u);
    test_gpu_write32(&m, target + 32u * STRIDE + 2u * 4u, 0xff445566u);
    test_gpu_write32(&m, target + 32u * STRIDE + 317u * 4u, 0xff778899u);
    test_gpu_write32(&m, target + 107u * STRIDE + 3u * 4u, 0xffaabbccu);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);

    uint32_t mismatches = 0u;
    for (unsigned i = 0; i < 6u; i++) {
        for (uint32_t y = rects[i][1]; y < rects[i][3]; y++) {
            for (uint32_t x = rects[i][0]; x < rects[i][2]; x++) {
                mismatches += test_gpu_read32(
                    &m, target + y * STRIDE + x * 4u) != 0xff000000u;
            }
        }
    }
    CHECK(mismatches == 0u,
          "measured Settings fill batch left %u non-black pixels", mismatches);
    CHECK(test_gpu_read32(&m, target + 31u * STRIDE + 3u * 4u) == 0xff112233u &&
          test_gpu_read32(&m, target + 32u * STRIDE + 2u * 4u) == 0xff445566u &&
          test_gpu_read32(&m, target + 32u * STRIDE + 317u * 4u) == 0xff778899u &&
          test_gpu_read32(&m, target + 107u * STRIDE + 3u * 4u) == 0xffaabbccu,
          "bounded Settings fills changed pixels outside their rectangles");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x400u,
          "Settings fill batch did not raise exactly 2D completion");
    CHECK(m.mbx_telemetry.candidates_2d == 6u &&
          m.mbx_telemetry.completed_2d == 6u &&
          m.mbx_telemetry.rejected_2d == 0u &&
          m.mbx_telemetry.bytes_2d == expected_bytes,
          "Settings 2D work ledger=%llu/%llu/%llu/%llu, expected 6/6/0/%llu",
          (unsigned long long)m.mbx_telemetry.candidates_2d,
          (unsigned long long)m.mbx_telemetry.completed_2d,
          (unsigned long long)m.mbx_telemetry.rejected_2d,
          (unsigned long long)m.mbx_telemetry.bytes_2d,
          (unsigned long long)expected_bytes);

    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x400u);
    test_gpu_write32(&m, target + 32u * STRIDE + 3u * 4u, 0xff123456u);
    test_gpu_write32(&m, target + 375u * STRIDE + 136u * 4u, 0xffabcdefu);
    packets[5][9] = (321u << 16) | 385u;
    for (unsigned i = 0; i < 6u; i++)
        write_packet(&m, RING + 0x200u + i * 0x40u, packets[i], 16u);
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(test_gpu_read32(&m, target + 32u * STRIDE + 3u * 4u) == 0xff123456u &&
          test_gpu_read32(&m, target + 375u * STRIDE + 136u * 4u) == 0xffabcdefu,
          "out-of-bounds late Settings fill committed part of the batch");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "out-of-bounds Settings fill raised completion");
    CHECK(m.mbx_telemetry.candidates_2d == 12u &&
          m.mbx_telemetry.completed_2d == 6u &&
          m.mbx_telemetry.rejected_2d == 6u &&
          m.mbx_telemetry.bytes_2d == expected_bytes,
          "rejected Settings batch ledger=%llu/%llu/%llu/%llu",
          (unsigned long long)m.mbx_telemetry.candidates_2d,
          (unsigned long long)m.mbx_telemetry.completed_2d,
          (unsigned long long)m.mbx_telemetry.rejected_2d,
          (unsigned long long)m.mbx_telemetry.bytes_2d);

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
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x400u);

    /* r432 reaches the end of AppleMBX's 64 KiB software cursor with a
     * command at +0xfff8. The kernel helper copies that command contiguously
     * into the EDRAM bytes beyond the nominal ring, then resets the *next*
     * command head to +0. Exercise both sides of that boundary; modulo-splitting
     * the first command or skipping it makes at least one pixel mismatch. */
    test_gpu_write32(&m, simple_dst0, 0u);
    test_gpu_write32(&m, simple_dst1, 0u);
    write_packet(&m, RING + 0xfff8u, simple_first, 16u);
    write_packet(&m, RING, simple_second, 16u);
    CHECK(test_gpu_read32(&m, simple_dst0) == 0u &&
          test_gpu_read32(&m, simple_dst1) == 0u,
          "ring-crossing batch executed before the fixed submit store");
    m.bus.write32(m.bus.ctx, MBX_BASE + RING, 0xf0000000u);
    CHECK(test_gpu_read32(&m, simple_dst0) == first_source &&
          test_gpu_read32(&m, simple_dst1) == second_source,
          "ring-crossing batch did not preserve both command copies");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x400u,
          "ring-crossing batch did not raise exactly 2D_SYNC");

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
    };
    for (unsigned i = 0; i < sizeof boundary / sizeof boundary[0]; i++)
        test_gpu_write32(&m, object + boundary[i].off, boundary[i].value);
    for (uint32_t off = 0x0e8u; off < 0x1f0u; off += 4u)
        test_gpu_write32(&m, object + off, 0x3c000000u ^ off);

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
    CHECK(m.mbx_telemetry.candidates_3d == 1u &&
          m.mbx_telemetry.completed_3d == 1u &&
          m.mbx_telemetry.rejected_3d == 0u &&
          m.mbx_telemetry.pixels_3d == WIDTH * HEIGHT,
          "first 3D work ledger=%llu/%llu/%llu/%llu",
          (unsigned long long)m.mbx_telemetry.candidates_3d,
          (unsigned long long)m.mbx_telemetry.completed_3d,
          (unsigned long long)m.mbx_telemetry.rejected_3d,
          (unsigned long long)m.mbx_telemetry.pixels_3d);

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
    CHECK(m.mbx_telemetry.candidates_3d == 3u &&
          m.mbx_telemetry.completed_3d == 1u &&
          m.mbx_telemetry.rejected_3d == 2u &&
          m.mbx_telemetry.pixels_3d == WIDTH * HEIGHT,
          "rejected 3D work ledger=%llu/%llu/%llu/%llu",
          (unsigned long long)m.mbx_telemetry.candidates_3d,
          (unsigned long long)m.mbx_telemetry.completed_3d,
          (unsigned long long)m.mbx_telemetry.rejected_3d,
          (unsigned long long)m.mbx_telemetry.pixels_3d);

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
    };
    for (unsigned i = 0; i < sizeof boundary / sizeof boundary[0]; i++)
        test_gpu_write32(&m, object + boundary[i].off, boundary[i].value);
    for (uint32_t off = 0x0e8u; off < 0x1f0u; off += 4u)
        test_gpu_write32(&m, object + off, 0xc3000000u ^ off);

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

static void test_write_compact_blit_copy(s5l8900_t *m,
                                         uint32_t region,
                                         uint32_t object,
                                         uint32_t source,
                                         uint32_t target,
                                         uint32_t record_offset,
                                         uint32_t left,
                                         uint32_t top,
                                         uint32_t width,
                                         uint32_t height,
                                         uint32_t source_stride) {
    for (uint32_t off = 0u; off < 0x500u; off += 4u)
        test_gpu_write32(m, object + off, 0u);

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
        test_gpu_write32(m, object + i * 4u, value);
    }

    uint32_t list = object + 0x68u;
    test_gpu_write32(m, list, 0x60200020u);
    test_gpu_write32(m, list + 4u, 0x6020002du);
    test_gpu_write32(m, list + 8u,
                     0x61200000u | (record_offset / 4u));
    test_gpu_write32(m, list + 12u, 0xf0000000u);

    static const struct {
        uint16_t off;
        uint32_t value;
    } boundary_fixed[] = {
        {0x080u, 0x22206f80u}, {0x088u, 0x45800000u},
        {0x094u, 0x45800000u}, {0x098u, 0x45800000u},
        {0x09cu, 0x45800000u}, {0x0b4u, 0x22207f80u},
    };
    for (unsigned i = 0;
         i < sizeof boundary_fixed / sizeof boundary_fixed[0]; i++)
        test_gpu_write32(m, object + boundary_fixed[i].off,
                         boundary_fixed[i].value);
    const uint32_t boundary[8] = {
        left, top + height, left, top,
        left + width, top + height, left + width, top,
    };
    for (unsigned i = 0; i < 8u; i++)
        test_gpu_write32(m, object + 0x0b8u + i * 4u,
                         test_float_word((float)boundary[i]));

    uint32_t pitch_pixels = source_stride / 4u;
    uint32_t texture_width = 8u, width_field = 0u;
    while (texture_width < pitch_pixels) {
        texture_width <<= 1;
        width_field++;
    }
    uint32_t texture_height = 8u, height_field = 0u;
    while (texture_height < height) {
        texture_height <<= 1;
        height_field++;
    }
    uint32_t pitch_units = source_stride / 16u;
    uint32_t header = 0xa0018000u |
        (width_field << 24) | (height_field << 20) |
        ((pitch_units & 2u) >> 1);
    uint32_t source_word = 0x8e000000u |
        ((pitch_units & ~3u) << 16) | (source >> 7);
    const float x0 = (float)left;
    const float y0 = (float)top;
    const float x1 = (float)(left + width);
    const float y1 = (float)(top + height);
    const float vertices[8] = {
        x0, y0, x1, y0, x0, y1, x1, y1,
    };
    const float u0 = 0.0f, v0 = 0.0f;
    const float u1 = ((float)width - 0.5f) / (float)texture_width;
    const float v1 = ((float)height - 0.5f) / (float)texture_height;
    const float uv[8] = {
        u0, v0, u1, v0, u0, v1, u1, v1,
    };
    uint32_t record[33] = {
        [0] = 0xe0000000u,
        [1] = header,
        [2] = source_word,
        [3] = 0xa6887610u,
        [4] = 0x22220e80u,
        [17] = 0x3f800000u,
        [18] = 0x3f800000u,
        [19] = 0x3f800000u,
        [20] = 0x3f800000u,
    };
    for (unsigned i = 0; i < 8u; i++)
        record[5u + i] = test_float_word(vertices[i]);
    for (unsigned vertex = 0; vertex < 4u; vertex++) {
        unsigned attribute = 21u + vertex * 3u;
        record[attribute] = 0xff000000u;
        record[attribute + 1u] = test_float_word(uv[vertex * 2u]);
        record[attribute + 2u] = test_float_word(uv[vertex * 2u + 1u]);
    }
    for (unsigned i = 0; i < 33u; i++)
        test_gpu_write32(m, object + record_offset + i * 4u, record[i]);

    const float lower_bias = 0.468505859375f;
    const float upper_bias = 0.531494140625f;
    uint32_t guard_left = (uint32_t)(x0 + lower_bias);
    uint32_t guard_top = (uint32_t)(y0 + lower_bias);
    uint32_t guard_right = (uint32_t)(x1 + upper_bias);
    uint32_t guard_bottom = (uint32_t)(y1 + upper_bias);
    uint32_t clip_left = guard_left & ~7u;
    uint32_t clip_right = (guard_right + 7u) & ~7u;
    uint32_t clip_top = guard_top & ~15u;
    uint32_t clip_bottom = (guard_bottom + 15u) & ~15u;
    uint32_t tile_x0 = clip_left / 8u;
    uint32_t tile_x1 = clip_right / 8u - 1u;
    uint32_t tile_y0 = clip_top / 16u;
    uint32_t tile_y1 = clip_bottom / 16u - 1u;
    uint32_t tile_count = (tile_x1 - tile_x0 + 1u) *
                          (tile_y1 - tile_y0 + 1u);
    uint32_t tile_index = 0u;
    for (uint32_t y = tile_y0; y <= tile_y1; y++) {
        for (uint32_t x = tile_x0; x <= tile_x1; x++) {
            uint32_t code = (y << 8) | x;
            if (tile_index + 1u == tile_count) code |= 0x80000000u;
            test_gpu_write32(m, region + tile_index * 8u, code);
            test_gpu_write32(m, region + tile_index * 8u + 4u, list);
            tile_index++;
        }
    }

    m->bus.write32(m->bus.ctx, MBX_BASE + REG_RGNBASE, region);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_OBJBASE, object);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_PIXSAMP, 0x00020007u);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_FBCTL, 6u);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_FBXCLIP,
                   (clip_right << 16) | clip_left);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_FBYCLIP,
                   (clip_bottom << 16) | clip_top);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_FBSTART, target);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_FBSTRIDE, 320u);
}

static void test_compact_opaque_blit_copy(void) {
    enum {
        LEFT = 8u, TOP = 16u, WIDTH = 16u, HEIGHT = 8u,
        SOURCE_STRIDE = 0x40u, TARGET_STRIDE = 0x500u,
    };
    const uint32_t table0 = 0x08003000u;
    const uint32_t table2 = 0x08004000u;
    const uint32_t table3 = 0x08005000u;
    const uint32_t region = 0x00001000u;
    const uint32_t object = 0x00014000u;
    const uint32_t source = 0x00b72000u;
    const uint32_t target = 0x00c98000u;
    const uint32_t region_pa = 0x08010000u;
    const uint32_t object_pa = 0x08014000u;
    const uint32_t source_pa = 0x08020000u;
    const uint32_t target_pa = 0x080c0000u;

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE),
          "compact blit-copy machine init failed");
    if (!m.ram) return;
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART0, table0);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART3, table3);
    test_map_gpu_page(&m, table0, region, region_pa);
    test_map_gpu_page(&m, table0, object, object_pa);
    test_map_gpu_page(&m, table2, source, source_pa);
    uint32_t target_last = target +
        (TOP + HEIGHT - 1u) * TARGET_STRIDE + (LEFT + WIDTH) * 4u - 1u;
    for (uint32_t page = target; page <= (target_last & ~0xfffu);
         page += 0x1000u)
        test_map_gpu_page(&m, table3, page, target_pa + (page - target));

    test_write_compact_blit_copy(&m, region, object, source, target,
                                 0x0e8u, LEFT, TOP, WIDTH, HEIGHT,
                                 SOURCE_STRIDE);
    for (uint32_t y = 0; y < HEIGHT; y++)
        for (uint32_t x = 0; x < WIDTH; x++)
            test_gpu_write32(&m, source + y * SOURCE_STRIDE + x * 4u,
                             test_sprite_source_pixel(x, y));
    for (uint32_t y = 0; y < HEIGHT; y++)
        for (uint32_t x = 0; x < WIDTH; x++)
            test_gpu_write32(&m,
                target + (TOP + y) * TARGET_STRIDE + (LEFT + x) * 4u,
                0xff102030u);
    uint32_t before = target + TOP * TARGET_STRIDE + (LEFT - 1u) * 4u;
    uint32_t after = target + (TOP + HEIGHT) * TARGET_STRIDE + LEFT * 4u;
    test_gpu_write32(&m, before, 0x11223344u);
    test_gpu_write32(&m, after, 0x55667788u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    uint32_t mismatches = 0u;
    for (uint32_t y = 0; y < HEIGHT; y++) {
        struct test_bilinear_axis y_axis;
        bool y_ok = test_bilinear_axis(
            (float)TOP, (float)HEIGHT, 0.0f, (float)HEIGHT - 0.5f,
            TOP + y, HEIGHT, &y_axis);
        for (uint32_t x = 0; x < WIDTH; x++) {
            struct test_bilinear_axis x_axis;
            bool x_ok = test_bilinear_axis(
                (float)LEFT, (float)WIDTH, 0.0f, (float)WIDTH - 0.5f,
                LEFT + x, WIDTH, &x_axis);
            uint32_t expected = x_ok && y_ok
                ? test_bilinear_sprite_pixel(&x_axis, &y_axis) : 0u;
            uint32_t actual = test_gpu_read32(&m,
                target + (TOP + y) * TARGET_STRIDE + (LEFT + x) * 4u);
            mismatches += !x_ok || !y_ok || actual != expected;
        }
    }
    CHECK(mismatches == 0u,
          "compact filtered blit-copy mismatched %u pixels", mismatches);
    CHECK(test_gpu_read32(&m, before) == 0x11223344u &&
          test_gpu_read32(&m, after) == 0x55667788u,
          "compact blit-copy changed a pixel outside its rectangle");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "compact blit-copy did not raise all three completion events");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);

    /* Relocating the selected record and copying a transparent,
     * non-premultiplied word prove this is pointer/opaque-copy semantics,
     * not a literal +0xe8 packet or an accidental source-over path. */
    test_write_compact_blit_copy(&m, region, object, source, target,
                                 0x300u, LEFT, TOP, WIDTH, HEIGHT,
                                 SOURCE_STRIDE);
    for (uint32_t y = 0; y < HEIGHT; y++)
        for (uint32_t x = 0; x < WIDTH; x++) {
            test_gpu_write32(&m, source + y * SOURCE_STRIDE + x * 4u,
                             0x00112233u);
            test_gpu_write32(&m,
                target + (TOP + y) * TARGET_STRIDE + (LEFT + x) * 4u,
                0xffaabbccu);
        }
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    mismatches = 0u;
    for (uint32_t y = 0; y < HEIGHT; y++)
        for (uint32_t x = 0; x < WIDTH; x++)
            mismatches += test_gpu_read32(&m,
                target + (TOP + y) * TARGET_STRIDE + (LEFT + x) * 4u) !=
                    0x00112233u;
    CHECK(mismatches == 0u,
          "relocated compact opaque copy mismatched %u pixels", mismatches);
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "relocated compact blit-copy did not complete");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);

    uint32_t first = target + TOP * TARGET_STRIDE + LEFT * 4u;
    uint32_t last = target + (TOP + HEIGHT - 1u) * TARGET_STRIDE +
                    (LEFT + WIDTH - 1u) * 4u;
    uint32_t sampler = object + 0x300u + 3u * 4u;
    uint32_t sampler_value = test_gpu_read32(&m, sampler);
    test_gpu_write32(&m, first, 0x89abcdefu);
    test_gpu_write32(&m, sampler, sampler_value ^ 1u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "unknown compact sampler was not rejected atomically");
    test_gpu_write32(&m, sampler, sampler_value);

    uint32_t second_u = object + 0x300u + 25u * 4u;
    uint32_t second_u_value = test_gpu_read32(&m, second_u);
    test_gpu_write32(&m, first, 0x89abcdefu);
    test_gpu_write32(&m, second_u, second_u_value ^ 1u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "inconsistent compact UV was not rejected atomically");
    test_gpu_write32(&m, second_u, second_u_value);

    uint32_t boundary_setup = object + 0x84u;
    test_gpu_write32(&m, first, 0x89abcdefu);
    test_gpu_write32(&m, boundary_setup, 1u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "unknown compact boundary setup was not rejected atomically");
    test_gpu_write32(&m, boundary_setup, 0u);

    uint32_t region_word = test_gpu_read32(&m, region);
    test_gpu_write32(&m, first, 0x89abcdefu);
    test_gpu_write32(&m, region, region_word ^ 1u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "inconsistent compact tile was not rejected atomically");
    test_gpu_write32(&m, region, region_word);

    uint32_t source_pte_address = table2 +
        (((source >> 12) & 0x3ffu) * 4u);
    uint32_t source_pte = m.bus.read32(m.bus.ctx, source_pte_address);
    test_gpu_write32(&m, first, 0x89abcdefu);
    test_gpu_write32(&m, last, 0x76543210u);
    m.bus.write32(m.bus.ctx, source_pte_address, 0u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    m.bus.write32(m.bus.ctx, source_pte_address, source_pte);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu &&
          test_gpu_read32(&m, last) == 0x76543210u &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "missing compact source PTE partially committed or completed");

    uint32_t last_page = last & ~0xfffu;
    uint32_t target_pte_address = table3 +
        (((last_page >> 12) & 0x3ffu) * 4u);
    uint32_t target_pte = m.bus.read32(m.bus.ctx, target_pte_address);
    test_gpu_write32(&m, first, 0x89abcdefu);
    test_gpu_write32(&m, last, 0x76543210u);
    m.bus.write32(m.bus.ctx, target_pte_address, 0u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    m.bus.write32(m.bus.ctx, target_pte_address, target_pte);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu &&
          test_gpu_read32(&m, last) == 0x76543210u &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "late missing compact target PTE partially committed or completed");

    s5l8900_free(&m);
}

static void test_write_solid_quad(s5l8900_t *m,
                                  uint32_t region,
                                  uint32_t object,
                                  uint32_t target,
                                  uint32_t solid_offset,
                                  float x0, float y0,
                                  float x1, float y1,
                                  uint32_t colour) {
    for (uint32_t off = 0u; off < 0x700u; off += 4u)
        test_gpu_write32(m, object + off, 0u);

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
        test_gpu_write32(m, object + i * 4u, value);
    }

    uint32_t list = object + 0x68u;
    static const uint32_t fixed_list[2] = {0x60200020u, 0x6020002du};
    test_gpu_write32(m, list, fixed_list[0]);
    test_gpu_write32(m, list + 4u, fixed_list[1]);
    test_gpu_write32(m, list + 8u,
                     0x61200000u | (solid_offset / 4u));
    test_gpu_write32(m, list + 12u, 0xf0000000u);

    static const struct {
        uint16_t off;
        uint32_t value;
    } boundary_fixed[] = {
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
    for (unsigned i = 0;
         i < sizeof boundary_fixed / sizeof boundary_fixed[0]; i++)
        test_gpu_write32(m, object + boundary_fixed[i].off,
                         boundary_fixed[i].value);
    const float vertices[8] = {
        x0, y1, x0, y0, x1, y1, x1, y0,
    };
    for (unsigned i = 0; i < 8u; i++)
        test_gpu_write32(m, object + 0x0b8u + i * 4u,
                         test_float_word(vertices[i]));

    uint32_t main[33] = {
        [0] = 0xe0000000u,
        [1] = 0xa7718000u,
        [2] = 0x0e500000u | (target >> 7),
        [3] = 0xa6104620u,
        [4] = 0x22220e80u,
        [17] = 0x3f800000u,
        [18] = 0x3f800000u,
        [19] = 0x3f800000u,
        [20] = 0x3f800000u,
    };
    for (unsigned i = 0; i < 8u; i++) main[5u + i] =
        test_float_word(vertices[i]);
    for (unsigned vertex = 0; vertex < 4u; vertex++) {
        unsigned attribute = 21u + vertex * 3u;
        main[attribute] = colour;
        main[attribute + 1u] = test_float_word(
            vertices[vertex * 2u] / 1024.0f);
        main[attribute + 2u] = test_float_word(
            vertices[vertex * 2u + 1u] / 1024.0f);
    }
    for (unsigned i = 0; i < 33u; i++)
        test_gpu_write32(m, object + solid_offset + i * 4u, main[i]);

    static const uint32_t parameter_controls[4] = {
        0x22620ea0u, 0x46622ea0u, 0x66622ea0u, 0x82622ea0u
    };
    for (unsigned record = 0; record < 4u; record++) {
        uint32_t base = object + solid_offset +
                        (record + 1u) * 33u * 4u;
        for (unsigned i = 0; i < 33u; i++) {
            uint32_t value = 0u;
            if (i == 0u) value = 0xe0000000u;
            else if (i == 3u) value = 0x86084610u;
            else if (i == 4u) value = parameter_controls[record];
            else if (i >= 17u && i <= 20u) value = 0x3f800000u;
            else if (i >= 21u && (i - 21u) % 3u == 0u)
                value = 0xffffffffu;
            test_gpu_write32(m, base + i * 4u, value);
        }
    }

    const float lower_bias = 0.468505859375f;
    const float upper_bias = 0.531494140625f;
    uint32_t left = (uint32_t)(x0 + lower_bias);
    uint32_t top = (uint32_t)(y0 + lower_bias);
    uint32_t right = (uint32_t)(x1 + upper_bias);
    uint32_t bottom = (uint32_t)(y1 + upper_bias);
    uint32_t clip_left = left & ~7u;
    uint32_t clip_right = (right + 7u) & ~7u;
    uint32_t clip_top = top & ~15u;
    uint32_t clip_bottom = (bottom + 15u) & ~15u;
    uint32_t tile_x0 = clip_left / 8u;
    uint32_t tile_x1 = clip_right / 8u - 1u;
    uint32_t tile_y0 = clip_top / 16u;
    uint32_t tile_y1 = clip_bottom / 16u - 1u;
    uint32_t tile_count = (tile_x1 - tile_x0 + 1u) *
                          (tile_y1 - tile_y0 + 1u);
    uint32_t tile_index = 0u;
    for (uint32_t y = tile_y0; y <= tile_y1; y++) {
        for (uint32_t x = tile_x0; x <= tile_x1; x++) {
            uint32_t code = (y << 8) | x;
            if (tile_index + 1u == tile_count) code |= 0x80000000u;
            test_gpu_write32(m, region + tile_index * 8u, code);
            test_gpu_write32(m, region + tile_index * 8u + 4u, list);
            tile_index++;
        }
    }

    m->bus.write32(m->bus.ctx, MBX_BASE + REG_RGNBASE, region);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_OBJBASE, object);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_PIXSAMP, 0x00020007u);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_FBCTL, 6u);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_FBXCLIP,
                   (clip_right << 16) | clip_left);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_FBYCLIP,
                   (clip_bottom << 16) | clip_top);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_FBSTART, target);
    m->bus.write32(m->bus.ctx, MBX_BASE + REG_FBSTRIDE, 320u);
}

static void test_pointer_selected_solid_quad(void) {
    enum { TARGET_STRIDE = 0x500u };
    const uint32_t table0 = 0x08003000u;
    const uint32_t table2 = 0x08004000u;
    const uint32_t region = 0x00001000u;
    const uint32_t object = 0x00014000u;
    const uint32_t target = 0x00a41000u;
    const uint32_t region_pa = 0x08010000u;
    const uint32_t object_pa = 0x08014000u;
    const uint32_t target_pa = 0x08040000u;
    const uint32_t target_page0 = target & ~0xfffu;

    s5l8900_t m;
    CHECK(s5l8900_init(&m, RAM_BASE, RAM_SIZE), "solid-quad machine init failed");
    if (!m.ram) return;
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART0, table0);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_GART2, table2);
    test_map_gpu_page(&m, table0, region, region_pa);
    test_map_gpu_page(&m, table0, object, object_pa);
    for (uint32_t page = target_page0;
         page < target_page0 + 320u * 480u * 4u; page += 0x1000u)
        test_map_gpu_page(&m, table2, page,
                          target_pa + (page - target_page0));

    /* Exact r414 form.  The poisoned +0x1f0 texture is stale by construction;
     * only the 0x612... pointer-selected transparent colour object at +0x2a0
     * may execute and complete without changing its destination. */
    test_write_solid_quad(&m, region, object, target, 0x2a0u,
                          159.0f, 239.0f, 161.0f, 241.0f,
                          0x00000000u);
    for (uint32_t off = 0x0e8u; off < 0x1f0u; off += 4u)
        test_gpu_write32(&m, object + off, 0xa5000000u ^ off);
    for (unsigned i = 0; i < 44u; i++)
        test_gpu_write32(&m, object + 0x1f0u + i * 4u,
                         0xdead0000u + i);
    for (uint32_t y = 239u; y < 241u; y++)
        for (uint32_t x = 159u; x < 161u; x++)
            test_gpu_write32(&m, target + y * TARGET_STRIDE + x * 4u,
                             0xff102030u);
    uint32_t outside = target + 239u * TARGET_STRIDE + 158u * 4u;
    test_gpu_write32(&m, outside, 0xff123456u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    uint32_t mismatches = 0u;
    for (uint32_t y = 239u; y < 241u; y++)
        for (uint32_t x = 159u; x < 161u; x++)
            mismatches += test_gpu_read32(
                &m, target + y * TARGET_STRIDE + x * 4u) != 0xff102030u;
    CHECK(mismatches == 0u,
          "r414 transparent solid quad changed %u pixels", mismatches);
    CHECK(test_gpu_read32(&m, outside) == 0xff123456u,
          "r414 solid quad changed a pixel outside its rectangle");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "r414 solid quad did not raise all three 3D events");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);

    /* A deliberately synthetic second address, size, position and colour
     * proves that neither 0xa8 nor the centre pixel rectangle is a whitelist. */
    test_write_solid_quad(&m, region, object, target, 0x300u,
                          23.0f, 33.0f, 47.0f, 41.0f,
                          0xff336699u);
    for (uint32_t y = 33u; y < 41u; y++)
        for (uint32_t x = 23u; x < 47u; x++)
            test_gpu_write32(&m, target + y * TARGET_STRIDE + x * 4u,
                             0xff102030u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    mismatches = 0u;
    for (uint32_t y = 33u; y < 41u; y++)
        for (uint32_t x = 23u; x < 47u; x++)
            mismatches += test_gpu_read32(
                &m, target + y * TARGET_STRIDE + x * 4u) != 0xff336699u;
    CHECK(mismatches == 0u,
          "synthetic pointer-selected solid quad mismatched %u pixels",
          mismatches);
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "synthetic solid quad did not complete");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);

    uint32_t first = target + 33u * TARGET_STRIDE + 23u * 4u;
    uint32_t second_colour = object + 0x300u + 24u * 4u;
    uint32_t second_colour_value = test_gpu_read32(&m, second_colour);
    test_gpu_write32(&m, first, 0xffabcdefu);
    test_gpu_write32(&m, second_colour,
                     second_colour_value ^ 0x01000000u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0xffabcdefu &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "per-vertex solid-colour mismatch was not rejected atomically");
    test_gpu_write32(&m, second_colour, second_colour_value);

    test_gpu_write32(&m, first, 0xffabcdefu);
    uint32_t normalized = object + 0x300u + 22u * 4u;
    uint32_t normalized_value = test_gpu_read32(&m, normalized);
    test_gpu_write32(&m, normalized, normalized_value ^ 1u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0xffabcdefu &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "normalized-coordinate mutation was not rejected atomically");
    test_gpu_write32(&m, normalized, normalized_value);

    uint32_t late_parameter =
        object + 0x300u + 4u * 33u * 4u + 30u * 4u;
    test_gpu_write32(&m, late_parameter, 0xfffffffeu);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0xffabcdefu &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "late solid-parameter mutation was not rejected atomically");
    test_gpu_write32(&m, late_parameter, 0xffffffffu);

    test_write_solid_quad(&m, region, object, target, 0x300u,
                          23.0f, 33.0f, 47.0f, 41.0f,
                          0x80332211u);
    test_gpu_write32(&m, first, 0xffabcdefu);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) ==
              test_over(0xffabcdefu, 0x80332211u) &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "premultiplied translucent solid did not blend and complete");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);

    /* Exact r419 geometry and exported _mbx3DQuadColor argument. */
    test_write_solid_quad(&m, region, object, target, 0x2a0u,
                          145.0f, 218.0f, 175.0f, 262.0f,
                          0x17000000u);
    uint32_t r419_first = target + 218u * TARGET_STRIDE + 145u * 4u;
    test_gpu_write32(&m, r419_first, 0xff8090a0u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, r419_first) ==
              test_over(0xff8090a0u, 0x17000000u) &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0x4cu,
          "r419 translucent-black solid did not blend and complete");
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_ACK, 0x4cu);

    test_write_solid_quad(&m, region, object, target, 0x300u,
                          23.0f, 33.0f, 47.0f, 41.0f,
                          0x80112299u);
    test_gpu_write32(&m, first, 0xffabcdefu);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0xffabcdefu &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "non-premultiplied solid colour was not rejected atomically");

    test_write_solid_quad(&m, region, object, target, 0x300u,
                          23.0f, 33.0f, 47.0f, 41.0f,
                          0xff336699u);
    test_gpu_write32(&m, first, 0xffabcdefu);
    uint32_t last = target + 40u * TARGET_STRIDE + 46u * 4u;
    uint32_t late_page = last & ~0xfffu;
    uint32_t late_entry = table2 + (((late_page >> 12) & 0x3ffu) * 4u);
    m.bus.write32(m.bus.ctx, late_entry, 0u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0xffabcdefu &&
          m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "late missing solid-quad PTE left a partial fill or completion");
    test_map_gpu_page(&m, table2, late_page,
                      target_pa + (late_page - target_page0));

    s5l8900_free(&m);
}

struct mbx_test_status_form {
    const char *name;
    uint32_t xclip, yclip;
    uint32_t target;
    bool semantic_sprite;
    bool affine_sprite;
    bool scaled_sprite;
    bool variable_vertex_alpha;
    bool boundary_override;
    bool zero_coverage;
    uint32_t tile_x0, tile_x1, tile_y0, tile_y1;
    uint32_t left, top, width, height;
    uint32_t source, source_x0, source_row0, source_stride, source_control;
    uint32_t source_width, source_height;
    uint32_t expected_covered_pixels;
    uint32_t boundary[8];
    uint32_t quad[44];
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
    const uint32_t tile_count =
        (form->tile_x1 - form->tile_x0 + 1u) *
        (form->tile_y1 - form->tile_y0 + 1u);
    const bool filtered_sprite = form->semantic_sprite &&
        (form->source_control & 0x80000000u) != 0u;
    const uint32_t filtered_texture_height =
        8u << ((form->quad[1] >> 20) & 7u);
    const uint32_t filtered_pitch_pixels = form->source_stride / 4u;
    const uint32_t mapped_source_x0 =
        filtered_sprite ? 0u : form->source_x0;
    const uint32_t mapped_source_y0 =
        filtered_sprite ? 0u : form->source_row0;
    const uint32_t mapped_source_width =
        filtered_sprite ? filtered_pitch_pixels : form->width;
    const uint32_t mapped_source_height =
        filtered_sprite ? filtered_texture_height : form->height;

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
    CHECK(mapped_source_width != 0u && mapped_source_height != 0u,
          "%s has no source dimensions", form->name);
    if (!mapped_source_width || !mapped_source_height) {
        s5l8900_free(&m);
        return;
    }
    uint32_t source_page0 = form->source & ~0xfffu;
    uint32_t source_last = form->source +
                           (mapped_source_y0 + mapped_source_height - 1u) *
                               form->source_stride +
                           (mapped_source_x0 + mapped_source_width) * 4u - 1u;
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
    for (unsigned i = 0; i < 8u; i++)
        test_gpu_write32(&m, object + 0x0b8u + i * 4u,
            form->boundary_override ? form->boundary[i] : form->quad[8u + i]);

    for (unsigned i = 0; i < 44u; i++) {
        uint32_t value = form->quad[i];
        if (i == 2u) value = form->source_control | (form->source >> 7);
        if (i == 5u) value = 0x0e500000u | (target >> 7);
        test_gpu_write32(&m, object + 0x1f0u + i * 4u, value);
    }
    /* Only the +0x80, +0xb4 and pointer-selected +0x1f0 records are in the
     * list. Poison the intervening stale storage so a decoder cannot quietly
     * turn a historical allocation pattern back into a packet requirement. */
    for (uint32_t off = 0x0e8u; off < 0x1f0u; off += 4u)
        test_gpu_write32(&m, object + off, 0x5a000000u ^ off);

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
    float filter_x0 = 0.0f, filter_y0 = 0.0f;
    float filter_dx = 0.0f, filter_dy = 0.0f;
    float filter_u_start = 0.0f, filter_v_start = 0.0f;
    float filter_u_span = 0.0f, filter_v_span = 0.0f;
    struct test_affine_transform filter_affine = {0};
    if (filtered_sprite) {
        filter_x0 = test_float_value(form->quad[8]);
        filter_y0 = test_float_value(form->quad[9]);
        filter_dx = test_float_value(form->quad[10]) - filter_x0;
        filter_dy = test_float_value(form->quad[13]) - filter_y0;
        if (form->affine_sprite) {
            filter_affine.origin_x = filter_x0;
            filter_affine.origin_y = filter_y0;
            filter_affine.u_x = filter_dx;
            filter_affine.u_y =
                test_float_value(form->quad[11]) - filter_y0;
            filter_affine.v_x =
                test_float_value(form->quad[12]) - filter_x0;
            filter_affine.v_y = filter_dy;
            filter_affine.determinant =
                filter_affine.u_x * filter_affine.v_y -
                filter_affine.u_y * filter_affine.v_x;
        }
        uint32_t texture_width = 8u << ((form->quad[1] >> 24) & 7u);
        uint32_t texture_height = 8u << ((form->quad[1] >> 20) & 7u);
        filter_u_start = test_float_value(form->quad[25]) * texture_width;
        filter_v_start = test_float_value(form->quad[26]) * texture_height;
        float filter_u_end =
            test_float_value(form->quad[30]) * texture_width;
        float filter_v_end =
            test_float_value(form->quad[36]) * texture_height;
        filter_u_span = filter_u_end - filter_u_start;
        filter_v_span = filter_v_end - filter_v_start;
        uint32_t inferred_left = (uint32_t)filter_u_start;
        uint32_t inferred_top = (uint32_t)filter_v_start;
        uint32_t inferred_right = (uint32_t)filter_u_end;
        uint32_t inferred_bottom = (uint32_t)filter_v_end;
        inferred_right += (float)inferred_right < filter_u_end;
        inferred_bottom += (float)inferred_bottom < filter_v_end;
        uint32_t inferred_width = inferred_right - inferred_left;
        uint32_t inferred_height = inferred_bottom - inferred_top;
        CHECK(inferred_width == form->source_width &&
              inferred_height == form->source_height,
              "%s explicit source dimensions disagree with its UV rectangle",
              form->name);
        CHECK(filter_u_start >= 0.0f && filter_v_start >= 0.0f &&
              filter_u_end <= (float)filtered_pitch_pixels &&
              filter_v_end <= (float)filtered_texture_height,
              "%s filtered UV rectangle exceeds its allocation", form->name);
        for (uint32_t y = 0; y < filtered_texture_height; y++) {
            for (uint32_t x = 0; x < filtered_pitch_pixels; x++) {
                test_gpu_write32(&m,
                    form->source + y * form->source_stride + x * 4u,
                    test_sprite_source_pixel(x, y));
            }
        }
    }
    uint32_t covered_pixels = 0u;
    uint32_t first_covered_offset = UINT32_MAX;
    uint32_t last_covered_offset = 0u;
    uint32_t maximum_sampled_x = 0u, maximum_sampled_y = 0u;
    bool have_filtered_sample = false;
    for (uint32_t y = 0; y < form->height; y++) {
        for (uint32_t x = 0; x < form->width; x++) {
            uint32_t src = 0u;
            bool covered = !form->zero_coverage;
            if (filtered_sprite) {
                struct test_bilinear_axis x_axis, y_axis;
                bool axes_ok = false;
                if (form->affine_sprite) {
                    float u_fraction, v_fraction;
                    covered = test_affine_pixel(
                        &filter_affine, form->left + x, form->top + y,
                        &u_fraction, &v_fraction);
                    if (covered) {
                        axes_ok = test_bilinear_coordinate(
                            filter_u_start + u_fraction * filter_u_span,
                            filtered_pitch_pixels, &x_axis) &&
                            test_bilinear_coordinate(
                                filter_v_start + v_fraction * filter_v_span,
                                filtered_texture_height, &y_axis);
                    }
                } else {
                    axes_ok = test_bilinear_axis(
                        filter_x0, filter_dx, filter_u_start, filter_u_span,
                        form->left + x, filtered_pitch_pixels, &x_axis) &&
                        test_bilinear_axis(
                            filter_y0, filter_dy, filter_v_start, filter_v_span,
                            form->top + y, filtered_texture_height, &y_axis);
                }
                if (covered && !axes_ok) {
                    CHECK(false, "%s expected filter axis is invalid", form->name);
                    covered = false;
                }
                if (covered) {
                    src = test_bilinear_sprite_pixel(&x_axis, &y_axis);
                    if (!have_filtered_sample ||
                        x_axis.second > maximum_sampled_x)
                        maximum_sampled_x = x_axis.second;
                    if (!have_filtered_sample ||
                        y_axis.second > maximum_sampled_y)
                        maximum_sampled_y = y_axis.second;
                    have_filtered_sample = true;
                }
            } else {
                src = test_sprite_source_pixel(x, y);
                test_gpu_write32(&m,
                    form->source + (form->source_row0 + y) *
                        form->source_stride + (form->source_x0 + x) * 4u, src);
            }
            uint32_t dst = 0xff102030u + y * 0x00010101u + x;
            test_gpu_write32(&m,
                target + (form->top + y) * TARGET_STRIDE +
                    (form->left + x) * 4u, dst);
            uint32_t offset = y * form->width + x;
            if (covered) {
                uint32_t modulated = test_modulate_vertex_alpha(
                    src, form->quad[24] >> 24);
                expected[offset] = test_over(dst, modulated);
                if (first_covered_offset == UINT32_MAX)
                    first_covered_offset = offset;
                last_covered_offset = offset;
                covered_pixels++;
            } else {
                expected[offset] = dst;
            }
        }
    }
    CHECK(form->zero_coverage
              ? first_covered_offset == UINT32_MAX && covered_pixels == 0u
              : first_covered_offset != UINT32_MAX,
          "%s reference coverage disagrees with its lifecycle class",
          form->name);
    if (form->expected_covered_pixels) {
        CHECK(covered_pixels == form->expected_covered_pixels,
              "%s expected %u covered pixels but reference found %u",
              form->name, form->expected_covered_pixels, covered_pixels);
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
    uint32_t first_offset = first_covered_offset == UINT32_MAX
        ? 0u : first_covered_offset;
    uint32_t first = target +
        (form->top + first_offset / form->width) * TARGET_STRIDE +
        (form->left + first_offset % form->width) * 4u;
    uint32_t last_destination = target +
        (form->top + last_covered_offset / form->width) * TARGET_STRIDE +
        (form->left + last_covered_offset % form->width) * 4u;
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
    if (form->semantic_sprite) {
        uint32_t word_address = object + 0x1f0u + 27u * 4u;
        uint32_t saved_word = test_gpu_read32(&m, word_address);
        test_gpu_write32(&m, first, 0x89abcdefu);
        test_gpu_write32(&m, word_address, saved_word ^ 1u);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
        CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
              "%s inconsistent normalized position changed the destination",
              form->name);
        CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
              "%s inconsistent normalized position raised completion",
              form->name);
        test_gpu_write32(&m, word_address, saved_word);

        word_address = object + 0x1f0u + 29u * 4u;
        saved_word = test_gpu_read32(&m, word_address);
        test_gpu_write32(&m, first, 0x89abcdefu);
        test_gpu_write32(&m, word_address, saved_word ^ 0x01000000u);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
        CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
              "%s mismatched app vertex alpha changed the destination",
              form->name);
        CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
              "%s mismatched app vertex alpha raised completion", form->name);
        test_gpu_write32(&m, word_address, saved_word);

        word_address = object + 0x1f0u + 30u * 4u;
        saved_word = test_gpu_read32(&m, word_address);
        test_gpu_write32(&m, first, 0x89abcdefu);
        test_gpu_write32(&m, word_address, saved_word ^ 1u);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
        CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
              "%s inconsistent UV extent changed the destination", form->name);
        CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
              "%s inconsistent UV extent raised completion", form->name);
        test_gpu_write32(&m, word_address, saved_word);

        if (!filtered_sprite) {
            /* A half-texel UV origin has the same floor/ceil source bounds as
             * the captured integer crop.  Keep both redundant corners in
             * agreement so rejection proves the unfiltered path did not
             * silently invent nearest-neighbour rounding. */
            uint32_t first_v_word = object + 0x1f0u + 31u * 4u;
            uint32_t second_v_word = object + 0x1f0u + 41u * 4u;
            uint32_t first_v = test_gpu_read32(&m, first_v_word);
            uint32_t second_v = test_gpu_read32(&m, second_v_word);
            uint32_t texture_height =
                8u << ((form->quad[1] >> 20) & 7u);
            uint32_t fractional_v = test_float_word(
                test_float_value(first_v) + 0.5f / (float)texture_height);
            test_gpu_write32(&m, first, 0x89abcdefu);
            test_gpu_write32(&m, first_v_word, fractional_v);
            test_gpu_write32(&m, second_v_word, fractional_v);
            m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
            CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
                  "%s fractional unfiltered origin changed the destination",
                  form->name);
            CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
                  "%s fractional unfiltered origin raised completion",
                  form->name);
            test_gpu_write32(&m, first_v_word, first_v);
            test_gpu_write32(&m, second_v_word, second_v);
        }

        /* A different split pitch can be a valid padded allocation when it
         * remains inside the same power-of-two header.  Break the redundant
         * width/pitch relationship instead of assuming the minimal stride. */
        word_address = object + 0x1f0u + 1u * 4u;
        saved_word = test_gpu_read32(&m, word_address);
        test_gpu_write32(&m, first, 0x89abcdefu);
        test_gpu_write32(&m, word_address, saved_word ^ 0x01000000u);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
        CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
              "%s inconsistent texture width/pitch changed the destination",
              form->name);
        CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
              "%s inconsistent texture width/pitch raised completion",
              form->name);
        test_gpu_write32(&m, word_address, saved_word);

        test_gpu_write32(&m, first, 0x89abcdefu);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBXCLIP,
                      form->xclip ^ 8u);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
        CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
              "%s inconsistent clip changed the destination", form->name);
        CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
              "%s inconsistent clip raised completion", form->name);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_FBXCLIP, form->xclip);

        uint32_t first_region_word = test_gpu_read32(&m, region);
        test_gpu_write32(&m, first, 0x89abcdefu);
        test_gpu_write32(&m, region, first_region_word ^ 1u);
        m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
        CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
              "%s inconsistent tile region changed the destination", form->name);
        CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
              "%s inconsistent tile region raised completion", form->name);
        test_gpu_write32(&m, region, first_region_word);

        uint32_t last_source = 0u;
        if (!form->zero_coverage) {
            uint32_t sampled_x = form->source_x0 + form->width - 1u;
            uint32_t sampled_y = form->source_row0 + form->height - 1u;
            if (filtered_sprite) {
                CHECK(have_filtered_sample,
                      "%s has no filtered source sample", form->name);
                sampled_x = maximum_sampled_x;
                sampled_y = maximum_sampled_y;
            }
            last_source = form->source + sampled_y * form->source_stride +
                          sampled_x * 4u;
            uint32_t saved_source = test_gpu_read32(&m, last_source);
            test_gpu_write32(&m, first, 0x89abcdefu);
            test_gpu_write32(&m, last_destination, 0x76543210u);
            test_gpu_write32(&m, last_source, 0x01000002u);
            m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
            CHECK(test_gpu_read32(&m, first) == 0x89abcdefu &&
                  test_gpu_read32(&m, last_destination) == 0x76543210u,
                  "%s non-premultiplied final texel partially committed",
                  form->name);
            CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
                  "%s non-premultiplied final texel raised completion",
                  form->name);
            test_gpu_write32(&m, last_source, saved_source);
        } else {
            uint32_t source_table = m.bus.read32(m.bus.ctx,
                MBX_BASE + REG_GART0 + (form->source >> 22) * 4u);
            uint32_t source_pte_address = source_table +
                (((form->source >> 12) & 0x3ffu) * 4u);
            uint32_t source_pte =
                m.bus.read32(m.bus.ctx, source_pte_address);
            test_gpu_write32(&m, first, 0x89abcdefu);
            m.bus.write32(m.bus.ctx, source_pte_address, 0u);
            m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
            CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
                  "%s missing source allocation raised completion",
                  form->name);
            m.bus.write32(m.bus.ctx, source_pte_address, source_pte);
            CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
                  "%s missing source allocation changed the destination",
                  form->name);

            uint32_t target_table = m.bus.read32(m.bus.ctx,
                MBX_BASE + REG_GART0 + (target >> 22) * 4u);
            uint32_t target_pte_address = target_table +
                (((first >> 12) & 0x3ffu) * 4u);
            uint32_t target_pte =
                m.bus.read32(m.bus.ctx, target_pte_address);
            test_gpu_write32(&m, first, 0x76543210u);
            m.bus.write32(m.bus.ctx, target_pte_address, 0u);
            m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
            CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
                  "%s missing target boundary raised completion",
                  form->name);
            m.bus.write32(m.bus.ctx, target_pte_address, target_pte);
            CHECK(test_gpu_read32(&m, first) == 0x76543210u,
                  "%s missing target boundary changed the destination",
                  form->name);
        }

        if (form->affine_sprite) {
            /* Preserve the normalized duplicate while moving only p11.  The
             * resulting four-point warp still fits the same conservative
             * boundary, but it is no longer the measured parallelogram. */
            uint32_t changed_x11 = test_float_word(
                test_float_value(form->quad[14]) + 0.25f);
            uint32_t changed_normalized = test_float_word(
                test_float_value(changed_x11) / 1024.0f);
            test_gpu_write32(&m, first, 0x89abcdefu);
            test_gpu_write32(&m, last_destination, 0x76543210u);
            test_gpu_write32(&m, object + 0x1f0u + 14u * 4u,
                             changed_x11);
            test_gpu_write32(&m, object + 0x1f0u + 42u * 4u,
                             changed_normalized);
            m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
            CHECK(test_gpu_read32(&m, first) == 0x89abcdefu &&
                  test_gpu_read32(&m, last_destination) == 0x76543210u,
                  "%s non-parallelogram warp partially committed",
                  form->name);
            CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
                  "%s non-parallelogram warp raised completion", form->name);
            test_gpu_write32(&m, object + 0x1f0u + 14u * 4u,
                             form->quad[14]);
            test_gpu_write32(&m, object + 0x1f0u + 42u * 4u,
                             form->quad[42]);
        }

        if (form->scaled_sprite) {
            /* Keep geometry and normalized records mutually consistent while
             * making only X scale differ.  A literal-packet whitelist or an
             * unchecked generic scaler would both miss this rejection. */
            uint32_t changed_x1 = test_float_word(
                test_float_value(form->quad[10]) + 0.25f);
            uint32_t changed_normalized = test_float_word(
                test_float_value(changed_x1) / 1024.0f);
            test_gpu_write32(&m, first, 0x89abcdefu);
            test_gpu_write32(&m, object + 0x1f0u + 10u * 4u, changed_x1);
            test_gpu_write32(&m, object + 0x1f0u + 14u * 4u, changed_x1);
            test_gpu_write32(&m, object + 0x1f0u + 32u * 4u,
                             changed_normalized);
            test_gpu_write32(&m, object + 0x1f0u + 42u * 4u,
                             changed_normalized);
            m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
            CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
                  "%s nonuniform filtered scale changed the destination",
                  form->name);
            CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
                  "%s nonuniform filtered scale raised completion", form->name);
            test_gpu_write32(&m, object + 0x1f0u + 10u * 4u, form->quad[10]);
            test_gpu_write32(&m, object + 0x1f0u + 14u * 4u, form->quad[14]);
            test_gpu_write32(&m, object + 0x1f0u + 32u * 4u, form->quad[32]);
            test_gpu_write32(&m, object + 0x1f0u + 42u * 4u, form->quad[42]);

            uint32_t source_table = m.bus.read32(m.bus.ctx,
                MBX_BASE + REG_GART0 + (last_source >> 22) * 4u);
            uint32_t source_pte_address = source_table +
                (((last_source >> 12) & 0x3ffu) * 4u);
            uint32_t source_pte =
                m.bus.read32(m.bus.ctx, source_pte_address);
            test_gpu_write32(&m, first, 0x89abcdefu);
            test_gpu_write32(&m, last_destination, 0x76543210u);
            m.bus.write32(m.bus.ctx, source_pte_address, 0u);
            m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
            CHECK(test_gpu_read32(&m, first) == 0x89abcdefu &&
                  test_gpu_read32(&m, last_destination) == 0x76543210u,
                  "%s late missing filtered source PTE partially committed",
                  form->name);
            CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
                  "%s late missing filtered source PTE raised completion",
                  form->name);
            m.bus.write32(m.bus.ctx, source_pte_address, source_pte);
        }
    }

    test_gpu_write32(&m, first, 0x89abcdefu);
    test_gpu_write32(&m, object + 0x1f4u,
                     form->quad[1] ^ 0x00008000u);
    m.bus.write32(m.bus.ctx, MBX_BASE + REG_RENDER, 1u);
    CHECK(test_gpu_read32(&m, first) == 0x89abcdefu,
          "%s unknown control changed the destination", form->name);
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "%s unknown control raised completion", form->name);

    free(expected);
    s5l8900_free(&m);
}

/* A right-edge conservative tile can be non-empty while the subpixel quad
 * contains no sample centre after the 320-pixel surface clip.  Completion is
 * a lifecycle property of this geometry, not of the captured addresses. */
static const struct mbx_test_status_form zero_coverage_status_form = {
    .name = "right-edge zero-coverage filtered sprite",
    .xclip = 0x01400138u, .yclip = 0x014000f0u,
    .target = 0x00998000u,
    .semantic_sprite = true,
    .boundary_override = true,
    .zero_coverage = true,
    .tile_x0 = 0x27u, .tile_x1 = 0x27u,
    .tile_y0 = 0x0fu, .tile_y1 = 0x13u,
    .left = 319u, .top = 242u, .width = 1u, .height = 63u,
    .source = 0x00972000u,
    .source_stride = 0x100u, .source_control = 0x8e100000u,
    .source_width = 59u, .source_height = 62u,
    .boundary = {
        0x439f8000u, 0x43988000u, 0x439f8000u, 0x43720000u,
        0x43a00000u, 0x43988000u, 0x43a00000u, 0x43720000u,
    },
    .quad = {
        0xe0000000u, 0xa3318000u, 0u, 0xa6884710u,
        0xa7718000u, 0x0e513300u, 0xae504ea0u, 0x22250e80u,
        0x439fc326u, 0x43729a02u, 0x43bd4326u, 0x43729a02u,
        0x439fc326u, 0x43984d01u, 0x43bd4326u, 0x43984d01u,
        0u, 0u, 0u, 0u,
        0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
        0xff000000u, 0u, 0u, 0x3e9fc326u,
        0x3e729a02u, 0xff000000u, 0x3f6a0000u, 0u,
        0x3ebd4326u, 0x3e729a02u, 0xff000000u, 0u,
        0x3f760000u, 0x3e9fc326u, 0x3e984d01u, 0xff000000u,
        0x3f6a0000u, 0x3f760000u, 0x3ebd4326u, 0x3e984d01u,
    },
};

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
            .name = "r430 unfiltered lower-screen crop on middle surface",
            .xclip = 0x01400000u, .yclip = 0x01e00010u,
            .target = 0x00998000u,
            .semantic_sprite = true,
            .tile_x0 = 0u, .tile_x1 = 0x27u,
            .tile_y0 = 1u, .tile_y1 = 0x1du,
            .left = 0u, .top = 20u, .width = 320u, .height = 460u,
            .source = 0x00a41080u, .source_row0 = 20u,
            .source_stride = 0x500u, .source_control = 0x0e500000u,
            .source_width = 320u, .source_height = 460u,
            .quad = {
                0xe0000000u, 0xa6618000u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x00000000u, 0x43f00000u, 0x00000000u, 0x41a00000u,
                0x43a00000u, 0x43f00000u, 0x43a00000u, 0x41a00000u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0x01000000u, 0x00000000u, 0x3f700000u, 0x00000000u,
                0x3ef00000u, 0x01000000u, 0x00000000u, 0x3d200000u,
                0x00000000u, 0x3ca00000u, 0x01000000u, 0x3f200000u,
                0x3f700000u, 0x3ea00000u, 0x3ef00000u, 0x01000000u,
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
            .name = "Messages label on transition surface",
            .xclip = 0x00600000u, .yclip = 0x00700050u,
            .target = 0x00a41000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0u, .tile_x1 = 0x0bu,
            .tile_y0 = 5u, .tile_y1 = 6u,
            .left = 3u, .top = 94u, .width = 86u, .height = 13u,
            .source = 0x00931080u,
            .source_stride = 0x160u, .source_control = 0x8e140000u,
            .source_width = 86u, .source_height = 13u,
            .boundary = {
                0x40000000u, 0x42d60000u, 0x40000000u, 0x42ba0000u,
                0x42b20000u, 0x42d60000u, 0x42b20000u, 0x42ba0000u,
            },
            .quad = {
                0xe0000000u, 0xa4118001u, 0u, 0xa6884710u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x403a2398u, 0x42bbcd39u, 0x42b1d11du, 0x42bbcd39u,
                0x403a2398u, 0x42d5cd39u, 0x42b1d11du, 0x42d5cd39u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xff000000u, 0u, 0u, 0x3b3a2398u,
                0x3dbbcd39u, 0xff000000u, 0x3f2b0000u, 0u,
                0x3db1d11du, 0x3dbbcd39u, 0xff000000u, 0u,
                0x3f480000u, 0x3b3a2398u, 0x3dd5cd39u, 0xff000000u,
                0x3f2b0000u, 0x3f480000u, 0x3db1d11du, 0x3dd5cd39u,
            },
        },
        {
            .name = "Messages icon on transition surface",
            .xclip = 0x00500010u, .yclip = 0x00600020u,
            .target = 0x00a41000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 2u, .tile_x1 = 9u,
            .tile_y0 = 2u, .tile_y1 = 5u,
            .left = 16u, .top = 32u, .width = 59u, .height = 62u,
            .source = 0x00933000u,
            .source_stride = 0x100u, .source_control = 0x8e100000u,
            .source_width = 59u, .source_height = 62u,
            .boundary = {
                0x41700000u, 0x42bc0000u, 0x41700000u, 0x41f80000u,
                0x42960000u, 0x42bc0000u, 0x42960000u, 0x41f80000u,
            },
            .quad = {
                0xe0000000u, 0xa3318000u, 0u, 0xa6884710u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x417e88e6u, 0x41ff34e4u, 0x4295d11du, 0x41ff34e4u,
                0x417e88e6u, 0x42bbcd39u, 0x4295d11du, 0x42bbcd39u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xff000000u, 0u, 0u, 0x3c7e88e6u,
                0x3cff34e4u, 0xff000000u, 0x3f6a0000u, 0u,
                0x3d95d11du, 0x3cff34e4u, 0xff000000u, 0u,
                0x3f760000u, 0x3c7e88e6u, 0x3dbbcd39u, 0xff000000u,
                0x3f6a0000u, 0x3f760000u, 0x3d95d11du, 0x3dbbcd39u,
            },
        },
        {
            .name = "Calendar label on transition surface",
            .xclip = 0x00a80048u, .yclip = 0x00700050u,
            .target = 0x00a41000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 9u, .tile_x1 = 0x14u,
            .tile_y0 = 5u, .tile_y1 = 6u,
            .left = 79u, .top = 94u, .width = 86u, .height = 13u,
            .source = 0x00937080u,
            .source_stride = 0x160u, .source_control = 0x8e140000u,
            .source_width = 86u, .source_height = 13u,
            .boundary = {
                0x429c0000u, 0x42d60000u, 0x429c0000u, 0x42ba0000u,
                0x43250000u, 0x42d60000u, 0x43250000u, 0x42ba0000u,
            },
            .quad = {
                0xe0000000u, 0xa4118001u, 0u, 0xa6884710u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x429deb82u, 0x42bbbdfeu, 0x4324f5c1u, 0x42bbbdfeu,
                0x429deb82u, 0x42d5bdfeu, 0x4324f5c1u, 0x42d5bdfeu,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xff000000u, 0u, 0u, 0x3d9deb82u,
                0x3dbbbdfeu, 0xff000000u, 0x3f2b0000u, 0u,
                0x3e24f5c1u, 0x3dbbbdfeu, 0xff000000u, 0u,
                0x3f480000u, 0x3d9deb82u, 0x3dd5bdfeu, 0xff000000u,
                0x3f2b0000u, 0x3f480000u, 0x3e24f5c1u, 0x3dd5bdfeu,
            },
        },
        {
            .name = "Calendar icon base on transition surface",
            .xclip = 0x00980058u, .yclip = 0x00600020u,
            .target = 0x00a41000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0x0bu, .tile_x1 = 0x12u,
            .tile_y0 = 2u, .tile_y1 = 5u,
            .left = 92u, .top = 32u, .width = 59u, .height = 62u,
            .source = 0x00939000u,
            .source_stride = 0x100u, .source_control = 0x8e100000u,
            .source_width = 59u, .source_height = 62u,
            .boundary = {
                0x42b60000u, 0x42bc0000u, 0x42b60000u, 0x41f80000u,
                0x43170000u, 0x42bc0000u, 0x43170000u, 0x41f80000u,
            },
            .quad = {
                0xe0000000u, 0xa3318000u, 0u, 0xa6884710u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x42b7eb82u, 0x41fef7fau, 0x4316f5c1u, 0x41fef7fau,
                0x42b7eb82u, 0x42bbbdfeu, 0x4316f5c1u, 0x42bbbdfeu,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xff000000u, 0u, 0u, 0x3db7eb82u,
                0x3cfef7fau, 0xff000000u, 0x3f6a0000u, 0u,
                0x3e16f5c1u, 0x3cfef7fau, 0xff000000u, 0u,
                0x3f760000u, 0x3db7eb82u, 0x3dbbbdfeu, 0xff000000u,
                0x3f6a0000u, 0x3f760000u, 0x3e16f5c1u, 0x3dbbbdfeu,
            },
        },
        {
            .name = "Stocks label with producer alpha modulation",
            .xclip = 0x00a80048u, .yclip = 0x00d000b0u,
            .target = 0x00a41000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 9u, .tile_x1 = 0x14u,
            .tile_y0 = 0x0bu, .tile_y1 = 0x0cu,
            .left = 79u, .top = 182u, .width = 86u, .height = 13u,
            .source = 0x00954080u,
            .source_stride = 0x160u, .source_control = 0x8e140000u,
            .source_width = 86u, .source_height = 13u,
            .boundary = {
                0x429c0000u, 0x43430000u, 0x429c0000u, 0x43350000u,
                0x43250000u, 0x43430000u, 0x43250000u, 0x43350000u,
            },
            .quad = {
                0xe0000000u, 0xa4118001u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x429dcd84u, 0x4335e866u, 0x4324e6c2u, 0x4335e866u,
                0x429dcd84u, 0x4342e866u, 0x4324e6c2u, 0x4342e866u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xfe000000u, 0u, 0u, 0x3d9dcd84u,
                0x3e35e866u, 0xfe000000u, 0x3f2b0000u, 0u,
                0x3e24e6c2u, 0x3e35e866u, 0xfe000000u, 0u,
                0x3f480000u, 0x3d9dcd84u, 0x3e42e866u, 0xfe000000u,
                0x3f2b0000u, 0x3f480000u, 0x3e24e6c2u, 0x3e42e866u,
            },
        },
        /* This is intentionally not described as a capture. It is constructed
         * from the recovered producer equations with a third size, pitch,
         * position and an otherwise unseen target, so a decoder that merely
         * moved the app or target literals into conditionals cannot pass it. */
        {
            .name = "producer-consistent 24x8 generic sprite",
            .xclip = 0x00e000c8u, .yclip = 0x00800070u,
            .target = 0x009b0000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 25u, .tile_x1 = 27u,
            .tile_y0 = 7u, .tile_y1 = 7u,
            .left = 200u, .top = 120u, .width = 24u, .height = 8u,
            .source = 0x0093b000u,
            .source_stride = 0x60u, .source_control = 0x8e040000u,
            .source_width = 24u, .source_height = 8u,
            .boundary = {
                0x43480000u, 0x43000000u, 0x43480000u, 0x42f00000u,
                0x43600000u, 0x43000000u, 0x43600000u, 0x42f00000u,
            },
            .quad = {
                0xe0000000u, 0xa2018001u, 0u, 0xa6884710u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x43480000u, 0x42f00000u, 0x43600000u, 0x42f00000u,
                0x43480000u, 0x43000000u, 0x43600000u, 0x43000000u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xff000000u, 0u, 0u, 0x3e480000u,
                0x3df00000u, 0xff000000u, 0x3f3c0000u, 0u,
                0x3e600000u, 0x3df00000u, 0xff000000u, 0u,
                0x3f700000u, 0x3e480000u, 0x3e000000u, 0xff000000u,
                0x3f3c0000u, 0x3f700000u, 0x3e600000u, 0x3e000000u,
            },
        },
        {
            .name = "r415 page indicator on middle surface",
            .xclip = 0x00980088u, .yclip = 0x01900170u,
            .target = 0x00998000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0x11u, .tile_x1 = 0x12u,
            .tile_y0 = 0x17u, .tile_y1 = 0x18u,
            .left = 136u, .top = 375u, .width = 10u, .height = 10u,
            .source = 0x0092e000u,
            .source_stride = 0x40u, .source_control = 0x0e040000u,
            .boundary = {
                0x43080000u, 0x43c08000u, 0x43080000u, 0x43bb8000u,
                0x43120000u, 0x43c08000u, 0x43120000u, 0x43bb8000u,
            },
            .quad = {
                0xe0000000u, 0xa1118000u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x43080000u, 0x43c08000u, 0x43080000u, 0x43bb8000u,
                0x43120000u, 0x43c08000u, 0x43120000u, 0x43bb8000u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xdd000000u, 0u, 0x3f200000u, 0x3e080000u,
                0x3ec08000u, 0xdd000000u, 0u, 0u,
                0x3e080000u, 0x3ebb8000u, 0xdd000000u, 0x3f200000u,
                0x3f200000u, 0x3e120000u, 0x3ec08000u, 0xdd000000u,
                0x3f200000u, 0u, 0x3e120000u, 0x3ebb8000u,
            },
        },
        {
            .name = "r416 clipped Messages label on middle surface",
            .xclip = 0x00480000u, .yclip = 0x00600040u,
            .target = 0x00998000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0u, .tile_x1 = 8u,
            .tile_y0 = 4u, .tile_y1 = 5u,
            .left = 0u, .top = 73u, .width = 70u, .height = 13u,
            .source = 0x00931080u, .source_x0 = 16u,
            .source_stride = 0x160u, .source_control = 0x8e140000u,
            .source_width = 86u, .source_height = 13u,
            .boundary = {
                0x00000000u, 0x42ae0000u, 0x00000000u, 0x42920000u,
                0x428c0000u, 0x42ae0000u, 0x428c0000u, 0x42920000u,
            },
            .quad = {
                0xe0000000u, 0xa4118001u, 0u, 0xa6884710u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0xc182c84cu, 0x42921807u, 0x428b4dedu, 0x42921807u,
                0xc182c84cu, 0x42ac1807u, 0x428b4dedu, 0x42ac1807u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xff000000u, 0u, 0u, 0xbc82c84cu,
                0x3d921807u, 0xff000000u, 0x3f2b0000u, 0u,
                0x3d8b4dedu, 0x3d921807u, 0xff000000u, 0u,
                0x3f480000u, 0xbc82c84cu, 0x3dac1807u, 0xff000000u,
                0x3f2b0000u, 0x3f480000u, 0x3d8b4dedu, 0x3dac1807u,
            },
        },
        {
            .name = "r417 guard-expanded Stocks label on middle surface",
            .xclip = 0x00900038u, .yclip = 0x00b000a0u,
            .target = 0x00998000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 7u, .tile_x1 = 0x11u,
            .tile_y0 = 0x0au, .tile_y1 = 0x0au,
            .left = 58u, .top = 163u, .width = 86u, .height = 13u,
            .source = 0x00954080u,
            .source_stride = 0x160u, .source_control = 0x8e140000u,
            .source_width = 86u, .source_height = 13u,
            .boundary = {
                0x42680000u, 0x43300000u, 0x42680000u, 0x43220000u,
                0x43110000u, 0x43300000u, 0x43110000u, 0x43220000u,
            },
            .quad = {
                0xe0000000u, 0xa4118001u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x4268aca2u, 0x43228572u, 0x43102b28u, 0x43228572u,
                0x4268aca2u, 0x432f8572u, 0x43102b28u, 0x432f8572u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xbd000000u, 0u, 0u, 0x3d68aca2u,
                0x3e228572u, 0xbd000000u, 0x3f2b0000u, 0u,
                0x3e102b28u, 0x3e228572u, 0xbd000000u, 0u,
                0x3f480000u, 0x3d68aca2u, 0x3e2f8572u, 0xbd000000u,
                0x3f2b0000u, 0x3f480000u, 0x3e102b28u, 0x3e2f8572u,
            },
        },
        {
            .name = "r418 dock-clipped Settings label on middle surface",
            .xclip = 0x00480000u, .yclip = 0x01900170u,
            .target = 0x00998000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0u, .tile_x1 = 8u,
            .tile_y0 = 0x17u, .tile_y1 = 0x18u,
            .left = 0u, .top = 380u, .width = 71u, .height = 9u,
            .source = 0x0097e080u, .source_x0 = 15u,
            .source_stride = 0x160u, .source_control = 0x8e140000u,
            .source_width = 86u, .source_height = 13u,
            .boundary = {
                0x00000000u, 0x43c28000u, 0x00000000u, 0x43be0000u,
                0x428e0000u, 0x43c28000u, 0x428e0000u, 0x43be0000u,
            },
            .quad = {
                0xe0000000u, 0xa4118001u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0xc170e2f4u, 0x43be09cau, 0x428de3a2u, 0x43be09cau,
                0xc170e2f4u, 0x43c489cau, 0x428de3a2u, 0x43c489cau,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xbd000000u, 0u, 0u, 0xbc70e2f4u,
                0x3ebe09cau, 0xbd000000u, 0x3f2b0000u, 0u,
                0x3d8de3a2u, 0x3ebe09cau, 0xbd000000u, 0u,
                0x3f480000u, 0xbc70e2f4u, 0x3ec489cau, 0xbd000000u,
                0x3f2b0000u, 0x3f480000u, 0x3d8de3a2u, 0x3ec489cau,
            },
        },
        {
            .name = "r420 scaled filtered background on middle surface",
            .xclip = 0x00b00090u, .yclip = 0x011000d0u,
            .target = 0x00998000u,
            .semantic_sprite = true,
            .scaled_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0x12u, .tile_x1 = 0x15u,
            .tile_y0 = 0x0du, .tile_y1 = 0x10u,
            .left = 146u, .top = 220u, .width = 28u, .height = 42u,
            .source = 0x00bad080u,
            .source_stride = 0x500u, .source_control = 0x8e500000u,
            .source_width = 320u, .source_height = 460u,
            .boundary = {
                0x43110000u, 0x43830000u, 0x43110000u, 0x435c0000u,
                0x432f0000u, 0x43830000u, 0x432f0000u, 0x435c0000u,
            },
            .quad = {
                0xe0000000u, 0xa6618000u, 0u, 0xd6887610u,
                0xa7718000u, 0u, 0xa3104620u, 0x22250e80u,
                0x4311982eu, 0x435c313fu, 0x432e67d2u, 0x435c313fu,
                0x4311982eu, 0x4382cddeu, 0x432e67d2u, 0x4382cddeu,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0x17000000u, 0u, 0u, 0x3e11982eu,
                0x3e5c313fu, 0x17000000u, 0x3f1fc000u, 0u,
                0x3e2e67d2u, 0x3e5c313fu, 0x17000000u, 0u,
                0x3f65c000u, 0x3e11982eu, 0x3e82cddeu, 0x17000000u,
                0x3f1fc000u, 0x3f65c000u, 0x3e2e67d2u, 0x3e82cddeu,
            },
        },
        {
            .name = "r421 scaled filtered status strip on middle surface",
            .xclip = 0x00b00090u, .yclip = 0x00e000d0u,
            .target = 0x00998000u,
            .semantic_sprite = true,
            .scaled_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0x12u, .tile_x1 = 0x15u,
            .tile_y0 = 0x0du, .tile_y1 = 0x0du,
            .left = 146u, .top = 219u, .width = 28u, .height = 1u,
            .source = 0x00c44080u,
            .source_stride = 0x500u, .source_control = 0x8e500000u,
            .source_width = 320u, .source_height = 20u,
            .boundary = {
                0x43110000u, 0x435d0000u, 0x43110000u, 0x435a0000u,
                0x432f0000u, 0x435d0000u, 0x432f0000u, 0x435a0000u,
            },
            .quad = {
                0xe0000000u, 0xa6218000u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x4311982eu, 0x435a99e7u, 0x432e67d2u, 0x435a99e7u,
                0x4311982eu, 0x435c66e1u, 0x432e67d2u, 0x435c66e1u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0x17000000u, 0u, 0u, 0x3e11982eu,
                0x3e5a99e7u, 0x17000000u, 0x3f1fc000u, 0u,
                0x3e2e67d2u, 0x3e5a99e7u, 0x17000000u, 0u,
                0x3f1c0000u, 0x3e11982eu, 0x3e5c66e1u, 0x17000000u,
                0x3f1fc000u, 0x3f1c0000u, 0x3e2e67d2u, 0x3e5c66e1u,
            },
        },
        {
            .name = "r422 mixed-edge filtered status sprite on middle surface",
            .xclip = 0x00a00090u, .yclip = 0x00e000d0u,
            .target = 0x00998000u,
            .semantic_sprite = true,
            .scaled_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0x12u, .tile_x1 = 0x13u,
            .tile_y0 = 0x0du, .tile_y1 = 0x0du,
            .left = 146u, .top = 219u, .width = 7u, .height = 1u,
            .source = 0x00981080u,
            .source_stride = 0x140u, .source_control = 0x8e140000u,
            .source_width = 76u, .source_height = 16u,
            .boundary = {
                0x43110000u, 0x435d0000u, 0x43110000u, 0x435a0000u,
                0x43190000u, 0x435d0000u, 0x43190000u, 0x435a0000u,
            },
            .quad = {
                0xe0000000u, 0xa4118000u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x4311f460u, 0x435ab0f4u, 0x4318cc17u, 0x435ab0f4u,
                0x4311f460u, 0x435c21bcu, 0x4318cc17u, 0x435c21bcu,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0x17000000u, 0u, 0u, 0x3e11f460u,
                0x3e5ab0f4u, 0x17000000u, 0x3f170000u, 0u,
                0x3e18cc17u, 0x3e5ab0f4u, 0x17000000u, 0u,
                0x3f800000u, 0x3e11f460u, 0x3e5c21bcu, 0x17000000u,
                0x3f170000u, 0x3f800000u, 0x3e18cc17u, 0x3e5c21bcu,
            },
        },
        {
            .name = "r426 direct filtered subrectangle on CLCD surface",
            .xclip = 0x00f000e0u, .yclip = 0x00700050u,
            .target = 0x00897000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0x1cu, .tile_x1 = 0x1du,
            .tile_y0 = 5u, .tile_y1 = 6u,
            .left = 229u, .top = 83u, .width = 5u, .height = 27u,
            .source = 0x00997000u,
            .source_stride = 0x40u, .source_control = 0x8e040000u,
            .source_width = 5u, .source_height = 27u,
            .boundary = {
                0x43650000u, 0x42dc0000u, 0x43650000u, 0x42a60000u,
                0x436a0000u, 0x42dc0000u, 0x436a0000u, 0x42a60000u,
            },
            .quad = {
                0xe0000000u, 0xa1218000u, 0u, 0xa6884710u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x43650000u, 0x42a60000u, 0x436a0000u, 0x42a60000u,
                0x43650000u, 0x42dc0000u, 0x436a0000u, 0x42dc0000u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xff000000u, 0u, 0u, 0x3e650000u,
                0x3da60000u, 0xff000000u, 0x3ea00000u, 0u,
                0x3e6a0000u, 0x3da60000u, 0xff000000u, 0u,
                0x3f580000u, 0x3e650000u, 0x3ddc0000u, 0xff000000u,
                0x3ea00000u, 0x3f580000u, 0x3e6a0000u, 0x3ddc0000u,
            },
        },
        {
            .name = "r427 magnified filtered column on CLCD surface",
            .xclip = 0x013000e8u, .yclip = 0x00700050u,
            .target = 0x00897000u,
            .semantic_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0x1du, .tile_x1 = 0x25u,
            .tile_y0 = 5u, .tile_y1 = 6u,
            .left = 234u, .top = 83u, .width = 63u, .height = 27u,
            .source = 0x00997000u,
            .source_stride = 0x40u, .source_control = 0x8e040000u,
            .source_width = 1u, .source_height = 27u,
            .boundary = {
                0x436a0000u, 0x42dc0000u, 0x436a0000u, 0x42a60000u,
                0x43948000u, 0x42dc0000u, 0x43948000u, 0x42a60000u,
            },
            .quad = {
                0xe0000000u, 0xa1218000u, 0u, 0xa6884710u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x436a0000u, 0x42a60000u, 0x43948000u, 0x42a60000u,
                0x436a0000u, 0x42dc0000u, 0x43948000u, 0x42dc0000u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xff000000u, 0x3ea00000u, 0u, 0x3e6a0000u,
                0x3da60000u, 0xff000000u, 0x3ec00000u, 0u,
                0x3e948000u, 0x3da60000u, 0xff000000u, 0x3ea00000u,
                0x3f580000u, 0x3e6a0000u, 0x3ddc0000u, 0xff000000u,
                0x3ec00000u, 0x3f580000u, 0x3e948000u, 0x3ddc0000u,
            },
        },
        {
            .name = "r434 rigid affine wiggle sprite",
            .xclip = 0x00600000u, .yclip = 0x01e001d0u,
            .target = 0x00998000u,
            .semantic_sprite = true,
            .affine_sprite = true,
            .scaled_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0u, .tile_x1 = 0x0bu,
            .tile_y0 = 0x1du, .tile_y1 = 0x1du,
            .left = 2u, .top = 464u, .width = 87u, .height = 15u,
            .source = 0x00932080u,
            .source_stride = 0x160u, .source_control = 0x8e140000u,
            .source_width = 86u, .source_height = 13u,
            .expected_covered_pixels = 1118u,
            .boundary = {
                0x40000000u, 0x43ef8000u, 0x40000000u, 0x43e80000u,
                0x42b20000u, 0x43ef8000u, 0x42b20000u, 0x43e80000u,
            },
            .quad = {
                0xe0000000u, 0xa4118001u, 0u, 0xa6884710u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x4019f46cu, 0x43e8bfdeu, 0x42b0ce8bu, 0x43e87240u,
                0x401fd241u, 0x43ef3fd3u, 0x42b0fd7au, 0x43eef235u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xff000000u, 0u, 0u, 0x3b19f46cu,
                0x3ee8bfdeu, 0xff000000u, 0x3f2b0000u, 0u,
                0x3db0ce8bu, 0x3ee87240u, 0xff000000u, 0u,
                0x3f480000u, 0x3b1fd241u, 0x3eef3fd3u, 0xff000000u,
                0x3f2b0000u, 0x3f480000u, 0x3db0fd7au, 0x3eeef235u,
            },
        },
        {
            .name = "r437 uniformly magnified modulated wiggle label",
            .xclip = 0x00600000u, .yclip = 0x00700050u,
            .target = 0x00897000u,
            .semantic_sprite = true,
            .scaled_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0u, .tile_x1 = 0x0bu,
            .tile_y0 = 5u, .tile_y1 = 6u,
            .left = 3u, .top = 94u, .width = 86u, .height = 13u,
            .source = 0x00bd7080u,
            .source_stride = 0x160u, .source_control = 0x8e140000u,
            .source_width = 86u, .source_height = 13u,
            .boundary = {
                0x40000000u, 0x42d80000u, 0x40000000u, 0x42bc0000u,
                0x42b40000u, 0x42d80000u, 0x42b40000u, 0x42bc0000u,
            },
            .quad = {
                0xe0000000u, 0xa4118001u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0x402e01fau, 0x42bc4de5u, 0x42b29353u, 0x42bc4de5u,
                0x402e01fau, 0x42d679ecu, 0x42b29353u, 0x42d679ecu,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0xfc000000u, 0u, 0u, 0x3b2e01fau,
                0x3dbc4de5u, 0xfc000000u, 0x3f2b0000u, 0u,
                0x3db29353u, 0x3dbc4de5u, 0xfc000000u, 0u,
                0x3f480000u, 0x3b2e01fau, 0x3dd679ecu, 0xfc000000u,
                0x3f2b0000u, 0x3f480000u, 0x3db29353u, 0x3dd679ecu,
            },
        },
        {
            .name = "r438 rotated magnified modulated wiggle label",
            .xclip = 0x00680000u, .yclip = 0x00800060u,
            .target = 0x00af8000u,
            .semantic_sprite = true,
            .affine_sprite = true,
            .scaled_sprite = true,
            .boundary_override = true,
            .tile_x0 = 0u, .tile_x1 = 0x0cu,
            .tile_y0 = 6u, .tile_y1 = 7u,
            .left = 0u, .top = 97u, .width = 102u, .height = 21u,
            .source = 0x00bd7080u,
            .source_stride = 0x160u, .source_control = 0x8e140000u,
            .source_width = 86u, .source_height = 13u,
            .expected_covered_pixels = 1645u,
            .boundary = {
                0x00000000u, 0x42ec0000u, 0x00000000u, 0x42c20000u,
                0x42cc0000u, 0x42ec0000u, 0x42cc0000u, 0x42c20000u,
            },
            .quad = {
                0xe0000000u, 0xa4118001u, 0u, 0xcd206c40u,
                0xa7718000u, 0u, 0xae504ea0u, 0x22250e80u,
                0xc0d283e2u, 0x42c99c02u, 0x42c9b58eu, 0x42c207f1u,
                0xc0c02fa1u, 0x42ea16d6u, 0x42cadad2u, 0x42e282c6u,
                0u, 0u, 0u, 0u,
                0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
                0x80000000u, 0u, 0u, 0xbbd283e2u,
                0x3dc99c02u, 0x80000000u, 0x3f2b0000u, 0u,
                0x3dc9b58eu, 0x3dc207f1u, 0x80000000u, 0u,
                0x3f480000u, 0xbbc02fa1u, 0x3dea16d6u, 0x80000000u,
                0x3f2b0000u, 0x3f480000u, 0x3dcadad2u, 0x3de282c6u,
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

    test_captured_status_form(&zero_coverage_status_form);
    struct mbx_test_status_form relocated_zero = zero_coverage_status_form;
    relocated_zero.name = "relocated right-edge zero-coverage sprite";
    relocated_zero.source = 0x00b72000u;
    relocated_zero.target = 0x00c98000u;
    test_captured_status_form(&relocated_zero);
}

int main(void) {
    printf("PowerVR MBX2D tests\n");
    test_translated_copy_and_completion_boundary();
    test_unknown_packet_and_bad_gart_are_atomic();
    test_full_lower_surface_black_fill();
    test_split_lower_surface_black_fill();
    test_springboard_settings_black_fill_batch();
    test_status_write_to_set_and_ack();
    test_premultiplied_2d_clock_form();
    test_opaque_global_alpha_2d_form();
    test_ordered_atomic_2d_batches();
    test_first_tiled_premultiplied_over();
    test_second_tiled_status_glyph();
    test_compact_opaque_blit_copy();
    test_pointer_selected_solid_quad();
    test_later_tiled_status_sprites();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
