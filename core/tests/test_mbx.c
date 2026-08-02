/*
 * S5LBox -- the first measured PowerVR MBX2D command.
 *
 * This is intentionally not a generic "copy some bytes" test. It transcribes
 * the packet produced by iPhone OS 3's _pack2DCtxBlitCopy, builds the GART the
 * way AppleMBXMMU::map does, routes every command word through the real machine
 * aperture, and checks the two facts that make completion honest: translated
 * pixels moved, and bit 10 rose only after the final terminator. Unknown
 * packet modes and incomplete GARTs must move nothing and complete nothing.
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
#define REG_GART2      0x00001008u
#define RING           0x00a00000u
#define GART_TABLE     0x08001000u
#define SRC_GPU        0x00800080u
#define DST_GPU        0x00810000u

static const uint32_t PACKET[16] = {
    0xf0000000u, DST_GPU, 0x94060500u, SRC_GPU,
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

    /* Five terminators are not a packet. The sixth is the final sequential
     * store from AppleMBX+0x1188 and is the earliest safe execution boundary. */
    write_packet(&m, RING, PACKET, 15u);
    CHECK(m.bus.read32(m.bus.ctx, dst0) == 0u &&
          m.bus.read32(m.bus.ctx, dst1) == 0u,
          "an incomplete packet changed destination RAM");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "an incomplete packet raised status");

    m.bus.write32(m.bus.ctx, MBX_BASE + RING + 15u * 4u, PACKET[15]);
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

    /* The exact live order recovered in r368 is not the simple path above.
     * AppleMBX copies a relocation token at the head, copies the remaining
     * fifteen words, then rewrites only the head to its final opcode. The
     * terminators must not execute the token-headed packet; the rewrite must. */
    for (unsigned i = 0; i < 4; i++) {
        m.bus.write32(m.bus.ctx, dst0 + i * 4u, 0u);
        m.bus.write32(m.bus.ctx, dst1 + i * 4u, 0u);
    }
    uint32_t relocating[16];
    memcpy(relocating, PACKET, sizeof relocating);
    relocating[0] = 0xa0060500u;
    write_packet(&m, RING + 0x40u, relocating, 16u);
    CHECK(m.bus.read32(m.bus.ctx, dst0) == 0u &&
          m.bus.read32(m.bus.ctx, dst1) == 0u,
          "relocation-token terminators executed before the header rewrite");
    CHECK(m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS) == 0u,
          "relocation-token packet raised completion before final header");

    m.bus.write32(m.bus.ctx, MBX_BASE + RING + 0x40u, PACKET[0]);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(m.bus.read32(m.bus.ctx, dst0 + i * 4u) == row0[i],
              "relocated row 0 pixel %u did not cross the GART", i);
        CHECK(m.bus.read32(m.bus.ctx, dst1 + i * 4u) == row1[i],
              "relocated row 1 pixel %u did not cross the GART", i);
    }
    status = m.bus.read32(m.bus.ctx, MBX_BASE + REG_STATUS);
    CHECK((status & 0x400u) != 0u,
          "final header rewrite did not raise 2D completion: %08x", status);
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

int main(void) {
    printf("PowerVR MBX2D tests\n");
    test_translated_copy_and_completion_boundary();
    test_unknown_packet_and_bad_gart_are_atomic();
    test_status_write_to_set_and_ack();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
