/*
 * iOS3-VM — AppleMultitouchZ2SPI's device: focused tests.
 *
 * One property dominates this file: the answer to isInHBPP() (0xc0441008,
 * vtable slot 0x4d0 off base 0xc0449f40). finishStarting() at 0xc0442670 keeps
 * the driver attached only on a TRUE. attemptToBootloadDevice() at 0xc04414c4
 * pushes 54,156 bytes of firmware on a TRUE and logs "not in HBPP, so skipping
 * bootload" on a FALSE. The two probes are BYTE-IDENTICAL on the wire, so the
 * discrimination is one monotonic bit and four designs for it have now been
 * wrong -- twice in ways that only showed up in an 18-minute boot.
 *
 * What separates them is the reset line's LEVEL: resetDevice clocks a dummy
 * 16-byte transfer sending the same `1A A1 18 E1...` while the line is
 * asserted and discards the answer, then releases the line and probes. Held in
 * reset the part drives nothing, so the dummy cannot spend the claim. Three
 * tests below exist only to make each past mistake fail loudly:
 * test_the_dummy_transfer_cannot_spend_the_claim, test_reset_does_not_rearm,
 * and test_the_whole_bring_up_sequence.
 *
 * The tests build the driver's own probe bytes, frames and acceptance tests,
 * transcribed from the disassembly rather than reused from the model -- a
 * model checked against itself checks nothing.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include "snapshot.h"
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

/*
 * The driver's own acceptance test, transcribed from 0xc0440658 rather than
 * reused from the model. The literal pool at 0xc04406b4-0xc04406bc holds
 * 0x18E1, 0x1AA1 and 0x4879 and the code derives the other four by
 * +0x620 -> 0x1F01, +0x2CC0 -> 0x4BC1, +0xF0 -> 0x4969 and -0xF0 -> 0x4AD1.
 * The argument is uxth-truncated before every comparison.
 */
static bool driver_accepts_hbpp_word(uint32_t w) {
    static const uint16_t OK[] = {
        0x1aa1u, 0x18e1u, 0x1f01u, 0x4879u, 0x4969u, 0x4bc1u, 0x4ad1u
    };
    uint16_t v = (uint16_t)w;
    for (unsigned i = 0; i < sizeof OK / sizeof OK[0]; i++)
        if (v == OK[i]) return true;
    return false;
}

/* isInHBPP()'s transmit buffer, built the way 0xc0441014-0xc0441048 builds it:
 * tx[0]=0x1A, tx[1]=0xA1, then 0x18,0xE1 seven times. */
static void build_hbpp_probe(uint8_t *tx) {
    tx[0] = 0x1au;
    tx[1] = 0xa1u;
    for (unsigned i = 2; i < 16u; i += 2u) { tx[i] = 0x18u; tx[i + 1] = 0xe1u; }
}

/* The driver's verdict on a 16-byte response: BOTH big-endian halfwords, at
 * 0xc04410fc and 0xc044110c, and rx[4..15] is only printed. */
static bool driver_says_in_hbpp(const uint8_t *rx) {
    uint32_t a = ((uint32_t)rx[0] << 8) | rx[1];
    uint32_t b = ((uint32_t)rx[2] << 8) | rx[3];
    return driver_accepts_hbpp_word(a) && driver_accepts_hbpp_word(b);
}

/*
 * Release the reset line, as the guest's "Deasserting reset line" does. A
 * device fresh from s5l_mtz2_reset() is HELD in reset -- the pin block powers
 * up all-zero and the line is active low -- so a test that wants a live part
 * has to do what the driver does.
 */
static void release_reset(s5l_mtz2_t *dev) { s5l_mtz2_reset_pin(dev, true); }
static void assert_reset(s5l_mtz2_t *dev)  { s5l_mtz2_reset_pin(dev, false); }

/* Run one full-duplex transaction against the device. */
static void xfer(s5l_spi_slave_t *s, const uint8_t *tx, uint8_t *rx,
                 unsigned n) {
    for (unsigned i = 0; i < n; i++) rx[i] = s->transfer(s->ctx, tx[i]);
}

/* The checksum, written out here rather than called from the model: a checksum
 * verified against itself verifies nothing. Plain truncating 16-bit byte sum,
 * routine 0xc0445d74. */
static uint16_t ref_sum16(const uint8_t *p, unsigned n) {
    uint16_t s = 0;
    while (n--) s = (uint16_t)(s + *p++);
    return s;
}

/* A 16-byte command frame exactly as the driver builds one: opcode at [0], a
 * parameter at [1], LE16 sum of [0..13] at [14..15] (0xc0443288-0xc044329c). */
static void build_frame(uint8_t *tx, uint8_t op, uint8_t param) {
    memset(tx, 0, MTZ2_FRAME_LEN);
    tx[0] = op;
    tx[1] = param;
    uint16_t sum = ref_sum16(tx, MTZ2_FRAME_LEN - 2u);
    tx[MTZ2_FRAME_LEN - 2u] = (uint8_t)(sum & 0xffu);
    tx[MTZ2_FRAME_LEN - 1u] = (uint8_t)(sum >> 8);
}

/* The driver's two response checks, from 0xc04433e4-0xc0443438. */
static bool driver_accepts_reply(const uint8_t *tx, const uint8_t *rx) {
    if (rx[0] != tx[0]) return false;                    /* kIOReturnNotResponding */
    uint16_t carried = (uint16_t)(rx[MTZ2_FRAME_LEN - 2u] |
                                  (rx[MTZ2_FRAME_LEN - 1u] << 8));
    return carried == ref_sum16(rx, MTZ2_FRAME_LEN - 2u);
}

/* =========================================================== the frame path ===
 *
 * Everything below is transcribed from the two consumers, not called out of the
 * model: the KERNEL's two acceptance tests (0xc0442554 for the length read,
 * 0xc044130c for the data read) and the USERSPACE parser's own gates
 * (_MTProcess_0xCC_Data, reached from _mt_HandleMultitouchFrame 0x33cfb3ec).
 */

/* The frame read's transmit side. `phase` is tx[2]: 0 asks for the length,
 * 1 asks for the data. The checksum is sum16 of the first FOURTEEN bytes and
 * it goes at the end of the TRANSFER, which is why it moves — 0xc0441254 and
 * 0xc044126c store it at tx[len-2] and tx[len-1]. */
static void build_frame_read(uint8_t *tx, unsigned total, uint8_t toggle,
                             uint8_t phase) {
    memset(tx, 0, total);
    tx[0] = MTZ2_OP_FRAME_Z2;
    tx[1] = toggle;
    tx[2] = phase;
    uint16_t sum = ref_sum16(tx, 14u);
    tx[total - 2u] = (uint8_t)(sum & 0xffu);
    tx[total - 1u] = (uint8_t)(sum >> 8);
}

/* deviceReadResultLength's verdict, from 0xc0442554-0xc04425c0. */
static bool driver_accepts_length(const uint8_t *rx, unsigned *out_len) {
    if ((rx[0] & 0xf0u) != 0xe0u) return false;
    uint16_t carried = (uint16_t)(rx[14] | (rx[15] << 8));
    if (carried != ref_sum16(rx, 14u)) return false;
    *out_len = (unsigned)rx[1] | ((unsigned)rx[2] << 8);
    return true;
}

/*
 * deviceReadResultData's verdict, from 0xc044130c-0xc04413b8. `total` is the
 * transfer length the driver chose, which is L + 5 (0xc04430c4). Note that the
 * PAYLOAD length comes from the frame's own [2..3] while the CHECKSUM position
 * comes from the transfer length, so a device whose two disagree fails here.
 */
static bool driver_accepts_data(const uint8_t *rx, unsigned total,
                                const uint8_t **payload, unsigned *plen) {
    if (rx[0] != MTZ2_OP_FRAME_Z2) return false;
    uint8_t five = 0;
    for (unsigned i = 0; i < 5u; i++) five = (uint8_t)(five + rx[i]);
    if (five != 0u) return false;                /* 0xc0441330: sum(rx,5)==0 */
    unsigned L = (unsigned)rx[2] | ((unsigned)rx[3] << 8);
    if (L == 0u || L == 2u) return false;        /* 0xc0441384: "no payload" */
    if (total < L + 5u) return false;
    uint16_t carried = (uint16_t)(rx[total - 2u] | (rx[total - 1u] << 8));
    if (carried != ref_sum16(rx + 5, L - 2u)) return false;
    *payload = rx + 5;
    *plen    = L - 2u;
    return true;
}

/*
 * _MTProcess_0xCC_Data's own gates. Frame type at [0] chooses the parser
 * (0x33cfb430 switches on it), the header is a constant ten bytes for this
 * type, the contact count is [3], and the stride is a constant 32
 * (`lsl r2,r3,#5` at 0x33cfbbcc). It rejects a length at or below 9 and a
 * length below header + count * stride.
 */
static bool userspace_accepts_frame(const uint8_t *p, unsigned len,
                                    unsigned *count) {
    /* A frame the DRIVER already rejected never reaches userspace, and a test
     * whose earlier check failed must say so rather than segfault here: a
     * crash names no expectation, which is exactly what a mutation run needs
     * it to do. */
    if (!p) return false;
    if (p[0] != 0xccu && p[0] != 0xceu) return false;
    if (len <= 9u) return false;
    unsigned n = p[3];
    if (len < 10u + n * 32u) return false;
    *count = n;
    return true;
}

/*
 * The pixel a contact record maps back to, computed the way the guest computes
 * it and not the way the model computed it:
 *   X   = (int32)LE32(rec+4) >> 8          _MTProcess_0xCC_Data
 *   Y   = (int32)LE32(rec+8) >> 8
 *   nx  = (X - xMin) / (xMax - xMin)       _MTSurface_getBounds_mm
 *   px  = nx * W                           MultitouchHID
 *   py  = H * (1 - ny)                     MultitouchHID -- note the flip
 * with xMin/yMin zero and xMax/yMax the surface size reported in 0xD9.
 */
static void decode_contact_pixel(const uint8_t *rec, uint32_t surface_w,
                                 uint32_t surface_h, int *px, int *py) {
    int32_t rx32 = (int32_t)((uint32_t)rec[4] | ((uint32_t)rec[5] << 8) |
                             ((uint32_t)rec[6] << 16) | ((uint32_t)rec[7] << 24));
    int32_t ry32 = (int32_t)((uint32_t)rec[8] | ((uint32_t)rec[9] << 8) |
                             ((uint32_t)rec[10] << 16) | ((uint32_t)rec[11] << 24));
    double x = (double)(rx32 >> 8);
    double y = (double)(ry32 >> 8);
    double nx = x / (double)surface_w;
    double ny = y / (double)surface_h;
    *px = (int)(nx * (double)S5L_MT_PANEL_W);
    *py = (int)((double)S5L_MT_PANEL_H * (1.0 - ny));
}

/* Drive one complete frame read — length then data — and hand back what the
 * driver would have enqueued. Returns false with a reason the caller prints. */
static bool read_one_frame(s5l_spi_slave_t *s, uint8_t *rxdata,
                           unsigned *out_wire_len, const uint8_t **payload,
                           unsigned *plen) {
    uint8_t tx[MTZ2_FRAME_LEN + MTZ2_PAYLOAD_LIMIT + 8];
    uint8_t rxlen[MTZ2_FRAME_LEN];
    unsigned L = 0, total;
    build_frame_read(tx, MTZ2_FRAME_LEN, 1u, 0u);
    xfer(s, tx, rxlen, MTZ2_FRAME_LEN);
    if (!driver_accepts_length(rxlen, &L)) return false;
    *out_wire_len = L;
    if (L == 0u) { *payload = NULL; *plen = 0u; return true; }
    total = L + 5u;
    build_frame_read(tx, total, 1u, 1u);
    xfer(s, tx, rxdata, total);
    return driver_accepts_data(rxdata, total, payload, plen);
}

/* ------------------------------------------------------------------------- */

static void test_reset_is_total_and_null_safe(void) {
    s5l_mtz2_t dev;
    memset(&dev, 0x5a, sizeof dev);
    s5l_mtz2_reset(&dev);
    CHECK(!dev.hbpp_answered,
          "a reset device has already spent its HBPP claim — "
          "finishStarting() would detach the driver");
    CHECK(dev.pos == 0u && dev.len == 0u && dev.packets == 0u &&
          dev.hbpp_probes == 0u && dev.unknown_opcodes == 0u &&
          dev.resets == 0u && dev.reset_bytes == 0u,
          "reset did not clear the framer on a poisoned object");
    CHECK(dev.in_reset,
          "a reset device is not held in reset — the pin block powers up "
          "all-zero and this line is active low, and starting released means "
          "the dummy transfer gets answered");
    CHECK(!dev.atn && dev.contacts == 0u,
          "a reset device claims a pending contact");

    s5l_spi_slave_t slave;
    memset(&slave, 0x5a, sizeof slave);
    s5l_mtz2_bind(NULL, &slave);
    CHECK(slave.transfer == NULL && slave.ctx == NULL,
          "binding a NULL device left a live callback");
    s5l_mtz2_bind(&dev, NULL);
    s5l_mtz2_reset(NULL);
    s5l_mtz2_reset_pin(NULL, true);
    s5l_mtz2_select_pin(NULL, true);
    CHECK(!s5l_mtz2_irq(NULL), "NULL interrupt query unsafe");
    CHECK(s5l_mtz2_sum16(NULL, 4u) == 0u, "NULL checksum unsafe");
    uint8_t body[MTZ2_PAYLOAD_MAX];
    CHECK(s5l_mtz2_report(NULL, MTZ2_REPORT_GEOMETRY, body) == 0u &&
          s5l_mtz2_report(&dev, MTZ2_REPORT_GEOMETRY, NULL) == 0u,
          "NULL report query unsafe");
}

/* The checksum, against hand-computed values and against the one the driver
 * itself writes into a GET_REPORT_INFO frame. */
static void test_checksum_is_a_plain_sum_stored_little_endian(void) {
    static const uint8_t z[4] = { 0, 0, 0, 0 };
    static const uint8_t ff[4] = { 0xff, 0xff, 0xff, 0xff };
    CHECK(s5l_mtz2_sum16(z, 4u) == 0u, "sum of zeros is not zero");
    CHECK(s5l_mtz2_sum16(ff, 4u) == 0x03fcu,
          "0xff*4 summed to 0x%04x, expected 0x03fc — this is a truncating "
          "byte sum with no seed and no complement",
          s5l_mtz2_sum16(ff, 4u));
    CHECK(s5l_mtz2_sum16(ff, 0u) == 0u, "a zero-length sum is not zero");

    /* The driver builds GET_REPORT_INFO's checksum as LE16(0xE3 + id) — a
     * closed form, so this pins both the value and the byte order. */
    uint8_t tx[MTZ2_FRAME_LEN];
    build_frame(tx, MTZ2_OP_REPORT_INFO, MTZ2_REPORT_GEOMETRY);
    uint16_t want = (uint16_t)(0xe3u + MTZ2_REPORT_GEOMETRY);
    CHECK(tx[14] == (uint8_t)(want & 0xffu) && tx[15] == (uint8_t)(want >> 8),
          "the frame carries 0x%02x 0x%02x, but the driver writes LE16(0x%04x)",
          tx[14], tx[15], want);
    CHECK(s5l_mtz2_sum16(tx, 14u) == want,
          "the model's checksum disagrees with the driver's closed form");
    /* And the order really is little-endian: a sum above 0xff must split. */
    CHECK(tx[14] == 0xb6u && tx[15] == 0x01u,
          "0xE3 + 0xD3 = 0x01B6 was stored as 0x%02x 0x%02x — big-endian "
          "storage would put 0x01 first and every frame would be rejected",
          tx[14], tx[15]);
}

/*
 * THE probe, and the claim. The first probe answered out of reset must pass the
 * driver's own test; every later one must fail it. Checked with the driver's
 * transcribed acceptance function rather than against a golden byte string.
 */
static void test_the_claim_is_spent_exactly_once(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
    s5l_mtz2_reset(&dev);
    s5l_mtz2_bind(&dev, &s);
    release_reset(&dev);
    build_hbpp_probe(tx);

    /* The probe bytes themselves, so a change to the transcription is caught
     * rather than agreed with. */
    CHECK(tx[0] == 0x1au && tx[1] == 0xa1u && tx[2] == 0x18u && tx[3] == 0xe1u &&
          tx[14] == 0x18u && tx[15] == 0xe1u,
          "the probe pattern is wrong: %02x %02x %02x %02x ... %02x %02x",
          tx[0], tx[1], tx[2], tx[3], tx[14], tx[15]);

    /* finishStarting's probe. */
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_says_in_hbpp(rx),
          "the first probe was rejected: rx = %02x %02x %02x %02x -- "
          "finishStarting() takes beq 0xc0442714, prints its "
          "\"Could not detect HBPP\" line and DETACHES",
          rx[0], rx[1], rx[2], rx[3]);
    CHECK(memcmp(rx, tx, MTZ2_FRAME_LEN) == 0,
          "HBPP is a loopback and all sixteen bytes must come back");
    CHECK(dev.hbpp_answered, "the claim was not spent");

    /* attemptToBootloadDevice's three probes. Byte-identical requests; the
     * answer must invert, or the driver pushes 54,156 bytes of firmware. */
    for (unsigned attempt = 1; attempt <= 3u; attempt++) {
        memset(rx, 0xff, sizeof rx);
        xfer(&s, tx, rx, MTZ2_FRAME_LEN);
        CHECK(!driver_says_in_hbpp(rx),
              "bootload probe %u was accepted: rx = %02x %02x %02x %02x -- "
              "the driver logs \"attempting to bootload device\" and starts "
              "MTSPIBootloader_Z2::bootloadDevice()",
              attempt, rx[0], rx[1], rx[2], rx[3]);
        bool zeros = true;
        for (unsigned i = 0; i < MTZ2_FRAME_LEN; i++) if (rx[i]) zeros = false;
        CHECK(zeros, "bootload probe %u was answered with something other than "
              "the sixteen zeros run61 measured", attempt);
    }
    CHECK(dev.hbpp_probes == 4u, "%llu probes were counted, expected 4",
          (unsigned long long)dev.hbpp_probes);
    CHECK(dev.unknown_opcodes == 0u,
          "a probe was counted as %llu unknown opcodes -- 0x1A must be framed "
          "as a 16-byte packet, or the 0xE1 at offset 3 starts a phantom "
          "GET_CMD_STATUS and the framer never resynchronises",
          (unsigned long long)dev.unknown_opcodes);

    /* A machine reset is a fresh part. */
    s5l_mtz2_reset(&dev);
    release_reset(&dev);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_says_in_hbpp(rx), "a machine reset did not restore the claim");
}

/*
 * THE BUG run65 FOUND, in miniature. resetDevice clocks a dummy 16-byte
 * transfer of the SAME `1A A1 18 E1...` bytes while the reset line is asserted
 * and throws the answer away, then releases the line and probes. A model that
 * answers while held in reset spends the claim on the dummy and hands
 * finishStarting the rejection.
 */
static void test_the_dummy_transfer_cannot_spend_the_claim(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
    s5l_mtz2_reset(&dev);
    s5l_mtz2_bind(&dev, &s);
    build_hbpp_probe(tx);

    /* resetDevice: the line is already asserted, then the dummy. */
    assert_reset(&dev);
    memset(rx, 0xff, sizeof rx);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    bool zeros = true;
    for (unsigned i = 0; i < MTZ2_FRAME_LEN; i++) if (rx[i]) zeros = false;
    CHECK(zeros, "a part held in its reset pin drove the bus: "
          "%02x %02x %02x %02x", rx[0], rx[1], rx[2], rx[3]);
    CHECK(!dev.hbpp_answered,
          "the dummy transfer spent the claim -- finishStarting's own probe is "
          "next and would be rejected, which is the run61 failure this device "
          "exists to end");
    CHECK(dev.reset_bytes == MTZ2_FRAME_LEN,
          "%llu bytes were swallowed while held in reset, expected %u",
          (unsigned long long)dev.reset_bytes, (unsigned)MTZ2_FRAME_LEN);
    CHECK(dev.len == 0u && dev.pos == 0u && dev.packets == 0u,
          "a transfer during reset advanced the framer");

    /* Deassert, then the real probe. */
    release_reset(&dev);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_says_in_hbpp(rx),
          "the probe after the dummy was rejected: %02x %02x %02x %02x",
          rx[0], rx[1], rx[2], rx[3]);
}

/*
 * The other half of the same bug: a reset must NOT re-arm the claim. The guest
 * pulses this line before EVERY probe site, so a reset that restores the claim
 * makes the bootload site identical to the first one -- which is what shipped
 * in 098ce49 and what run65 caught pushing firmware.
 */
static void test_reset_does_not_rearm_the_claim(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
    s5l_mtz2_reset(&dev);
    s5l_mtz2_bind(&dev, &s);
    build_hbpp_probe(tx);
    release_reset(&dev);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_says_in_hbpp(rx), "the first probe was rejected");

    /* Four more full reset pulses, each with its dummy and its probe, exactly
     * as the bootload attempts do. None may be accepted. */
    for (unsigned site = 1; site <= 4u; site++) {
        assert_reset(&dev);
        xfer(&s, tx, rx, MTZ2_FRAME_LEN);          /* the dummy */
        release_reset(&dev);
        memset(rx, 0xff, sizeof rx);
        xfer(&s, tx, rx, MTZ2_FRAME_LEN);          /* the probe */
        CHECK(!driver_says_in_hbpp(rx),
              "site %u was accepted after a reset pulse: %02x %02x %02x %02x "
              "-- a reset restores the part's state, it does not un-ask a "
              "question the host already had answered",
              site, rx[0], rx[1], rx[2], rx[3]);
        CHECK(dev.hbpp_answered, "site %u cleared the claim", site);
    }
    CHECK(dev.resets == 9u, "%llu reset edges were seen, expected 9",
          (unsigned long long)dev.resets);
}

/*
 * The guest's whole bring-up sequence, in its own order, transcribed from the
 * mtlog stream of a boot with `-D chosen:debug-enabled=1`:
 *
 *      setPowerEnabled[false] / Asserting reset line / disabling power
 *      setPowerEnabled[true]  / enabling power / ensuring S_CLK is high
 *      initiating dummy transfer
 *      Deasserting reset line
 *      checking if in HBPP                       -> must be accepted
 *      ... three bootload attempts, each the same shape ...
 *
 * If this passes and the boot still fails, the fault is in the wiring rather
 * than in the device.
 */
static void test_the_whole_bring_up_sequence(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
    s5l_mtz2_reset(&dev);
    s5l_mtz2_bind(&dev, &s);
    build_hbpp_probe(tx);

    static const bool WANT[4] = { true, false, false, false };
    for (unsigned site = 0; site < 4u; site++) {
        assert_reset(&dev);                        /* Asserting reset line   */
        xfer(&s, tx, rx, MTZ2_FRAME_LEN);          /* dummy transfer         */
        release_reset(&dev);                       /* Deasserting reset line */
        memset(rx, 0xff, sizeof rx);
        xfer(&s, tx, rx, MTZ2_FRAME_LEN);          /* checking if in HBPP    */
        CHECK(driver_says_in_hbpp(rx) == WANT[site],
              "site %u answered %s, wanted %s -- site 0 is finishStarting and "
              "the rest are bootload attempts",
              site, driver_says_in_hbpp(rx) ? "in HBPP" : "not in HBPP",
              WANT[site] ? "in HBPP" : "not in HBPP");
    }

    /* And after all of it the report protocol still answers, because that is
     * what isBootloaded() asks next and it decides between "Device has
     * firmware?!" and "No firmware running, and couldn't load any". */
    build_frame(tx, MTZ2_OP_REPORT_INFO, MTZ2_REPORT_GEOMETRY);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_accepts_reply(tx, rx) && rx[3] == 5u,
          "getReportInfo(0xD3) stopped answering after the bootload attempts");
}

/*
 * Each of the seven accepted constants, and the fact that BOTH halfwords are
 * tested. A model that got only rx[0..1] right would pass a casual test and
 * still fail 0xc0441108's `beq`.
 */
static void test_the_driver_tests_both_halfwords(void) {
    uint8_t rx[MTZ2_FRAME_LEN];
    memset(rx, 0, sizeof rx);

    static const uint16_t OK[] = {
        0x1aa1u, 0x18e1u, 0x1f01u, 0x4879u, 0x4969u, 0x4bc1u, 0x4ad1u
    };
    for (unsigned i = 0; i < sizeof OK / sizeof OK[0]; i++)
        CHECK(driver_accepts_hbpp_word(OK[i]),
              "0x%04x is not in the accepted set", OK[i]);
    CHECK(!driver_accepts_hbpp_word(0x0000u) &&
          !driver_accepts_hbpp_word(0xa11au) &&
          !driver_accepts_hbpp_word(0xe118u),
          "a byte-swapped or zero word was accepted");

    /* First halfword right, second wrong: rejected. */
    rx[0] = 0x1au; rx[1] = 0xa1u; rx[2] = 0x00u; rx[3] = 0x00u;
    CHECK(!driver_says_in_hbpp(rx),
          "only the first halfword was tested — the second `bl 0xc0440658` at "
          "0xc0441110 is not optional");
    /* Second right, first wrong: also rejected. */
    rx[0] = 0x00u; rx[1] = 0x00u; rx[2] = 0x18u; rx[3] = 0xe1u;
    CHECK(!driver_says_in_hbpp(rx), "only the second halfword was tested");
    /* Both right: accepted, and rx[4..15] is irrelevant. */
    rx[0] = 0x1au; rx[1] = 0xa1u; rx[2] = 0x18u; rx[3] = 0xe1u;
    memset(&rx[4], 0xa5, MTZ2_FRAME_LEN - 4u);
    CHECK(driver_says_in_hbpp(rx), "a valid pair with noise after it failed");
}

/*
 * GET_REPORT_INFO for 0xD3 is what isBootloaded() (0xc043ac58) runs, and its
 * return is the whole of that predicate: `rsbs r0,r0,#1 / movlo r0,#0` makes it
 * true exactly when the call returns kIOReturnSuccess. So a reply that fails
 * either of the driver's two checks is the difference between "Device has
 * firmware?!" and "No firmware running, and couldn't load any".
 */
static void test_get_report_info_satisfies_isbootloaded(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN], probe[MTZ2_FRAME_LEN];
    s5l_mtz2_reset(&dev);
    s5l_mtz2_bind(&dev, &s);
    release_reset(&dev);

    /* Probe first, exactly as the boot does — the device stays in HBPP and
     * the ordinary protocol has to keep working alongside it. A model that put
     * the whole device into a loopback mode would echo these frames instead of
     * answering them. */
    build_hbpp_probe(probe);
    xfer(&s, probe, rx, MTZ2_FRAME_LEN);

    build_frame(tx, MTZ2_OP_REPORT_INFO, MTZ2_REPORT_GEOMETRY);

    /* The driver sends the frame, delays 25 us, and sends the IDENTICAL frame
     * again; only the second response is read. Both must be acceptable. */
    for (unsigned pass = 0; pass < 2u; pass++) {
        memset(rx, 0xff, sizeof rx);
        xfer(&s, tx, rx, MTZ2_FRAME_LEN);
        CHECK(rx[0] == MTZ2_OP_REPORT_INFO,
              "pass %u: rx[0] = 0x%02x, not the echoed opcode — the driver "
              "returns kIOReturnNotResponding (0xE00002ED)", pass, rx[0]);
        CHECK(driver_accepts_reply(tx, rx),
              "pass %u: the reply failed the driver's checksum test "
              "(0xE00002BC)", pass);
        CHECK(rx[2] == 0u,
              "pass %u: rx[2] = 0x%02x, and the driver only ever tests it "
              "against zero", pass, rx[2]);
        unsigned len = (unsigned)rx[3] | ((unsigned)rx[4] << 8);
        CHECK(len == 5u,
              "pass %u: report 0xD3 was announced as %u bytes; it carries "
              "Endianness, Rows, Columns and a 16-bit bcdVersion", pass, len);
    }

    /* A report the device does not have answers with a length of zero and is
     * still a well-formed, accepted reply. */
    build_frame(tx, MTZ2_OP_REPORT_INFO, 0x5au);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_accepts_reply(tx, rx) && rx[3] == 0u && rx[4] == 0u,
          "an unknown report id produced a malformed reply");

    /* Every report the driver interrogates announces the length it will then
     * read, and every one fits the short form the driver picks below 12. */
    static const struct { uint8_t id; unsigned len; const char *what; } R[] = {
        { MTZ2_REPORT_FAMILY_ID,    1u, "Family ID"               },
        { MTZ2_REPORT_GEOMETRY,     5u, "Rows/Columns/bcdVersion" },
        { MTZ2_REPORT_BUTTONS,      1u, "Buttons"                 },
        { MTZ2_REPORT_SURFACE,      8u, "Sensor Surface W/H"      },
        { MTZ2_REPORT_REGION_DESC,  8u, "Region Descriptor"       },
        { MTZ2_REPORT_REGION_PARAM, 8u, "Region Param"            },
    };
    for (unsigned i = 0; i < sizeof R / sizeof R[0]; i++) {
        build_frame(tx, MTZ2_OP_REPORT_INFO, R[i].id);
        xfer(&s, tx, rx, MTZ2_FRAME_LEN);
        unsigned len = (unsigned)rx[3] | ((unsigned)rx[4] << 8);
        CHECK(driver_accepts_reply(tx, rx) && len == R[i].len,
              "%s (0x%02x) announced %u bytes, expected %u",
              R[i].what, R[i].id, len, R[i].len);
        CHECK(len <= MTZ2_PAYLOAD_MAX,
              "%s does not fit the short control-read form (%u > %u), so the "
              "driver would use 0xE7 and a longer frame than this model builds",
              R[i].what, len, (unsigned)MTZ2_PAYLOAD_MAX);
    }
}

/*
 * A control read carries its payload from rx[3]. Report 0xD9 is the one whose
 * contents decide where a tap lands, so its bytes are checked against the
 * chosen geometry rather than against the model's own accessor.
 */
static void test_control_read_returns_the_report_body(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN], probe[MTZ2_FRAME_LEN];
    s5l_mtz2_reset(&dev);
    s5l_mtz2_bind(&dev, &s);
    release_reset(&dev);
    build_hbpp_probe(probe);
    xfer(&s, probe, rx, MTZ2_FRAME_LEN);

    /* 0xD3: Endianness, Rows, Columns, then bcdVersion BIG-endian — the one
     * field in this protocol that is not little-endian (0xc043845c reads it as
     * `data[4] | data[3] << 8`). */
    build_frame(tx, MTZ2_OP_READ_SHORT, MTZ2_REPORT_GEOMETRY);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_accepts_reply(tx, rx), "the control read was rejected");
    const uint8_t *body = &rx[MTZ2_PAYLOAD_AT];
    CHECK(body[1] == 15u && body[2] == 10u,
          "Rows/Columns came back %u/%u, expected 15/10 — the same 2:3 ratio "
          "as the 320x480 panel", body[1], body[2]);
    CHECK(((unsigned)body[3] << 8 | body[4]) == 0x0100u,
          "bcdVersion came back 0x%04x; a little-endian model would give "
          "0x0001", (unsigned)body[3] << 8 | body[4]);
    CHECK(body[0] == 1u, "Endianness came back %u", body[0]);

    /* 0xD9: two UNALIGNED little-endian 32-bit words, and the driver drops the
     * report entirely if it is shorter than 8 bytes. */
    build_frame(tx, MTZ2_OP_READ_SHORT, MTZ2_REPORT_SURFACE);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_accepts_reply(tx, rx), "the surface read was rejected");
    body = &rx[MTZ2_PAYLOAD_AT];
    uint32_t w = (uint32_t)body[0] | ((uint32_t)body[1] << 8) |
                 ((uint32_t)body[2] << 16) | ((uint32_t)body[3] << 24);
    uint32_t h = (uint32_t)body[4] | ((uint32_t)body[5] << 8) |
                 ((uint32_t)body[6] << 16) | ((uint32_t)body[7] << 24);
    CHECK(w == 4800u && h == 7200u,
          "the surface came back %ux%u, expected 4800x7200", w, h);
    /* The whole reason for those two numbers: an exact, equal, whole-number
     * scale to the panel in both axes. Unequal scales are aspect distortion
     * and a tap lands further from the finger the nearer the screen edge. */
    CHECK(w % 320u == 0u && h % 480u == 0u && (w / 320u) == (h / 480u),
          "the surface is not a whole and EQUAL multiple of 320x480: "
          "%u/320 = %u but %u/480 = %u", w, w / 320u, h, h / 480u);
    CHECK(w / 320u == 15u, "the scale is %u units per point, expected 15",
          w / 320u);

    /* 0xD1 and 0xD7 are single bytes. */
    build_frame(tx, MTZ2_OP_READ_SHORT, MTZ2_REPORT_BUTTONS);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_accepts_reply(tx, rx) && rx[MTZ2_PAYLOAD_AT] == 0u,
          "the digitizer claims a button");
    build_frame(tx, MTZ2_OP_READ_SHORT, MTZ2_REPORT_FAMILY_ID);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_accepts_reply(tx, rx) && rx[MTZ2_PAYLOAD_AT] != 0u,
          "the family id is zero, which is indistinguishable from an "
          "unanswered report");

    /* The long form is answered identically, since nothing this device
     * publishes exceeds the short form's capacity. */
    build_frame(tx, MTZ2_OP_READ_LONG, MTZ2_REPORT_GEOMETRY);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_accepts_reply(tx, rx) && rx[MTZ2_PAYLOAD_AT + 1] == 15u,
          "the long control read did not answer");
}

/* The remaining opcodes must at least be well-formed, and an unknown one must
 * neither answer nor derail the framer. */
static void test_the_other_opcodes_are_well_formed(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
    s5l_mtz2_reset(&dev);
    s5l_mtz2_bind(&dev, &s);
    release_reset(&dev);
    build_hbpp_probe(tx);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);

    static const uint8_t OPS[] = {
        MTZ2_OP_CMD_STATUS, MTZ2_OP_DEVICE_INFO,
        MTZ2_OP_WRITE_SHORT, MTZ2_OP_WRITE_LONG, MTZ2_OP_WAKE
    };
    for (unsigned i = 0; i < sizeof OPS / sizeof OPS[0]; i++) {
        build_frame(tx, OPS[i], 0u);
        xfer(&s, tx, rx, MTZ2_FRAME_LEN);
        CHECK(driver_accepts_reply(tx, rx),
              "opcode 0x%02x produced a reply the driver rejects", OPS[i]);
    }

    /* An unrecognised opcode answers zero and leaves the framer idle, so the
     * very next byte is still read as an opcode. That is what makes a SECOND
     * HBPP probe — whose first byte is 0x1A — come back as sixteen zeros
     * instead of desynchronising everything after it. */
    uint64_t before = dev.unknown_opcodes;
    CHECK(s.transfer(s.ctx, 0x77u) == 0u, "an unknown opcode answered");
    CHECK(dev.unknown_opcodes == before + 1u && dev.last_unknown_op == 0x77u,
          "an unknown opcode was not recorded");
    CHECK(dev.len == 0u && dev.pos == 0u, "an unknown opcode consumed state");
    build_frame(tx, MTZ2_OP_CMD_STATUS, 0u);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_accepts_reply(tx, rx),
          "the framer did not resynchronise after an unknown opcode");

    /* 0x1A is special: it is the HBPP probe's first byte and must not be
     * counted as a protocol error once the device has left HBPP. */
    before = dev.unknown_opcodes;
    build_hbpp_probe(tx);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(dev.unknown_opcodes == before,
          "a later HBPP probe was counted as %llu unknown opcodes",
          (unsigned long long)(dev.unknown_opcodes - before));
}

/* Packets are framed by the length their own opcode implies, and the chip
 * select is a resynchronisation and not the framing — because the driver may
 * never drive it. */
static void test_framing_survives_without_a_chip_select(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
    s5l_mtz2_reset(&dev);
    s5l_mtz2_bind(&dev, &s);
    release_reset(&dev);
    build_hbpp_probe(tx);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);

    /* Four back-to-back frames with no select edge anywhere. */
    for (unsigned i = 0; i < 4u; i++) {
        build_frame(tx, MTZ2_OP_REPORT_INFO, MTZ2_REPORT_GEOMETRY);
        xfer(&s, tx, rx, MTZ2_FRAME_LEN);
        CHECK(driver_accepts_reply(tx, rx),
              "frame %u was malformed without a select edge", i);
    }
    CHECK(dev.packets == 5u,
          "%llu packets were framed, expected 5 — the probe plus four frames; "
          "the probe is a framed 16-byte packet like any other command",
          (unsigned long long)dev.packets);

    /* A select edge in the middle of a frame discards it, and the next frame
     * is still read correctly — which is all a resync has to do. */
    build_frame(tx, MTZ2_OP_REPORT_INFO, MTZ2_REPORT_GEOMETRY);
    for (unsigned i = 0; i < 6u; i++) (void)s.transfer(s.ctx, tx[i]);
    s5l_mtz2_select_pin(&dev, false);
    CHECK(dev.len == 0u && dev.pos == 0u, "the select edge did not resync");
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_accepts_reply(tx, rx), "the frame after a resync was lost");

    /* The two-byte wakeup is framed by its own length. */
    uint8_t wake[2] = { MTZ2_OP_REQ_WAKEUP, 0xc1u };
    uint8_t wrx[2];
    xfer(&s, wake, wrx, 2u);
    CHECK(wrx[0] == MTZ2_OP_REQ_WAKEUP,
          "the wakeup opcode was not echoed (0x%02x)", wrx[0]);
    CHECK(dev.len == 0u && dev.pos == 0u,
          "the two-byte wakeup was framed as something longer");
}

/* The attention line, and the state step 4 will drive. */
static void test_attention_line_is_quiet_until_a_frame_exists(void) {
    s5l_mtz2_t dev;
    s5l_mtz2_reset(&dev);
    CHECK(!s5l_mtz2_irq(&dev),
          "a device with nothing to report is asking to be read");
    dev.atn = true;
    CHECK(s5l_mtz2_irq(&dev), "the attention line cannot be asserted");
    dev.atn = false;
    CHECK(!s5l_mtz2_irq(&dev), "the attention line cannot be released");
}

/* Bring a device all the way up, the way the guest does. */
static void bring_up(s5l_mtz2_t *dev, s5l_spi_slave_t *s) {
    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
    s5l_mtz2_reset(dev);
    s5l_mtz2_bind(dev, s);
    build_hbpp_probe(tx);
    xfer(s, tx, rx, MTZ2_FRAME_LEN);      /* the dummy, inside reset */
    release_reset(dev);
    build_hbpp_probe(tx);
    xfer(s, tx, rx, MTZ2_FRAME_LEN);      /* the probe, out of reset */
}

static s5l_mt_contact_t one_finger(uint16_t x, uint16_t y, uint8_t phase) {
    s5l_mt_contact_t c;
    memset(&c, 0, sizeof c);
    c.id = 1u; c.x = x; c.y = y; c.phase = phase;
    c.pressure = phase == MTZ2_PHASE_BREAK_TOUCH ? 0u : 160u;
    c.major = 24u; c.minor = 20u;
    return c;
}

/*
 * An idle device answers the length read with zero, and that is not an error:
 * the caller tests L at 0xc04430bc and stops. It has to be CHEAP -- one
 * sixteen-byte transfer and no second one -- because it is what every spurious
 * edge of a level-triggered line costs.
 */
static void test_an_idle_device_answers_a_length_read_with_zero(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
    unsigned L = 0xffffu;
    bring_up(&dev, &s);

    build_frame_read(tx, MTZ2_FRAME_LEN, 1u, 0u);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    CHECK(driver_accepts_length(rx, &L),
          "the idle length answer was malformed: %02x %02x %02x ... %02x %02x",
          rx[0], rx[1], rx[2], rx[14], rx[15]);
    CHECK(L == 0u, "an idle device claims %u bytes of frame", L);
    CHECK(dev.len == 0u && dev.pos == 0u,
          "the length read left the framer mid-packet (len=%u pos=%u)",
          dev.len, dev.pos);
    CHECK(dev.length_reads == 1u && dev.data_reads == 0u,
          "an idle length read was counted as %llu length and %llu data reads",
          (unsigned long long)dev.length_reads,
          (unsigned long long)dev.data_reads);
    CHECK(!s5l_mtz2_irq(&dev), "an idle device raised its attention line");
}

/*
 * The whole delivery: a host injection, an attention line, a length read, a
 * data read, and a payload the driver would enqueue verbatim.
 */
static void test_an_injected_contact_is_delivered_over_the_wire(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    uint8_t rxdata[MTZ2_PAYLOAD_LIMIT + 8];
    const uint8_t *payload = NULL;
    unsigned L = 0, plen = 0, count = 0;
    bring_up(&dev, &s);

    s5l_mt_contact_t c = one_finger(160u, 240u, MTZ2_PHASE_MAKE_TOUCH);
    CHECK(s5l_mtz2_set_contacts(&dev, &c, 1u),
          "a brought-up device refused a contact (refused=%llu)",
          (unsigned long long)dev.injects_refused);
    CHECK(s5l_mtz2_irq(&dev),
          "a queued report did not raise the attention line");
    CHECK(dev.frames_queued == 1u && dev.contacts == 1u,
          "the queue counters disagree: queued=%llu contacts=%u",
          (unsigned long long)dev.frames_queued, dev.contacts);

    if (!read_one_frame(&s, rxdata, &L, &payload, &plen) || !payload) {
        CHECK(false, "the driver rejected the frame this device produced "
                     "(wire length %u)", L);
        return;
    }
    CHECK(L == MTZ2_FRAME_HEADER + MTZ2_CONTACT_STRIDE + 2u,
          "the wire length is %u, expected header+one contact+checksum = %u",
          L, MTZ2_FRAME_HEADER + MTZ2_CONTACT_STRIDE + 2u);
    CHECK(plen == MTZ2_FRAME_HEADER + MTZ2_CONTACT_STRIDE,
          "the payload is %u bytes, expected %u", plen,
          MTZ2_FRAME_HEADER + MTZ2_CONTACT_STRIDE);
    CHECK(payload && payload[0] != 0x50u,
          "the payload's first byte is 0x50, which 0xc0438a1c diverts to the "
          "status decoder instead of the touch queue");
    CHECK(userspace_accepts_frame(payload, plen, &count),
          "the userspace parser would reject this frame");
    CHECK(count == 1u, "the parser reads %u contacts, expected 1", count);

    /* The line falls when the host has taken the report, and not before. */
    CHECK(!s5l_mtz2_irq(&dev),
          "the attention line is still up after a complete data read");
    CHECK(dev.frames_read == 1u && dev.frame_len == 0u,
          "the report was not consumed: read=%llu len=%u",
          (unsigned long long)dev.frames_read, dev.frame_len);

    /* And the next length read is the cheap zero again. */
    unsigned again = 0xffffu;
    payload = NULL;
    CHECK(read_one_frame(&s, rxdata, &again, &payload, &plen) && again == 0u,
          "a second read after delivery found %u bytes still pending", again);
}

/*
 * The payload, byte for byte, against a hand-written expectation. Nothing here
 * calls the encoder to decide what the encoder should produce.
 */
static void test_the_payload_is_the_frame_userspace_parses(void) {
    s5l_mtz2_t dev;
    uint8_t out[MTZ2_PAYLOAD_LIMIT];
    s5l_mtz2_reset(&dev);
    s5l_mt_contact_t c = one_finger(160u, 240u, MTZ2_PHASE_MAKE_TOUCH);
    unsigned len = s5l_mtz2_encode(&dev, &c, 1u, 7u, 0x1234u, out);

    CHECK(len == 42u, "a one-contact 0xCC frame is %u bytes, expected 42", len);
    CHECK(out[0] == 0xccu, "frame type is 0x%02x, expected 0xCC", out[0]);
    CHECK(out[1] == 7u, "frame counter is %u, expected the 7 passed in", out[1]);
    CHECK(out[2] == 0u, "button state is %u; this digitizer has no buttons",
          out[2]);
    CHECK(out[3] == 1u, "contact count is %u, expected 1", out[3]);
    CHECK(out[6] == 0x34u && out[7] == 0x12u && out[8] == 0u && out[9] == 0u,
          "the timestamp is not LE32 0x1234 at the unaligned offset 6: "
          "%02x %02x %02x %02x", out[6], out[7], out[8], out[9]);

    const uint8_t *r = &out[10];
    CHECK(r[0] == 1u && r[1] == MTZ2_PHASE_MAKE_TOUCH,
          "the record starts id=%u stage=%u, expected 1 and MakeTouch(3)",
          r[0], r[1]);
    CHECK(r[3] == 1u, "hand id is %u, expected one hand", r[3]);

    /*
     * 160 of 320 across a 4800-unit surface is 160*15 + 7 = 2407, and the
     * field is that scaled by 256 because the parser shifts right by eight.
     * 240 of 480 down a 7200-unit surface is FLIPPED: 7200 - 240*15 - 7 =
     * 3593. Written out as arithmetic rather than as a constant so that a
     * changed surface size changes this expectation too.
     */
    uint32_t want_x = (uint32_t)(160u * 15u + 7u) << 8;
    uint32_t want_y = (uint32_t)(7200u - 240u * 15u - 7u) << 8;
    uint32_t got_x = (uint32_t)r[4] | ((uint32_t)r[5] << 8) |
                     ((uint32_t)r[6] << 16) | ((uint32_t)r[7] << 24);
    uint32_t got_y = (uint32_t)r[8] | ((uint32_t)r[9] << 8) |
                     ((uint32_t)r[10] << 16) | ((uint32_t)r[11] << 24);
    CHECK(got_x == want_x, "X is 0x%08x, expected 0x%08x", got_x, want_x);
    CHECK(got_y == want_y, "Y is 0x%08x, expected 0x%08x", got_y, want_y);

    uint16_t z = (uint16_t)(r[20] | (r[21] << 8));
    CHECK(z == 160u, "Z total is %u, expected the caller's 160", z);
    uint16_t major = (uint16_t)(r[28] | (r[29] << 8));
    uint16_t minor = (uint16_t)(r[30] | (r[31] << 8));
    CHECK(major == 24u * 15u && minor == 20u * 15u,
          "the contact ellipse is %u x %u, expected %u x %u in the same "
          "hundredths of a millimetre the position uses",
          major, minor, 24u * 15u, 20u * 15u);
    CHECK(r[26] == 0u && r[27] == 0u,
          "an orientation was invented for a circular contact");

    /* A lift carries zero amplitude AND says BreakTouch: two statements of
     * the same fact, so a consumer that reads either one agrees. */
    c = one_finger(160u, 240u, MTZ2_PHASE_BREAK_TOUCH);
    len = s5l_mtz2_encode(&dev, &c, 1u, 8u, 0x1244u, out);
    CHECK(len == 42u, "the lift frame is %u bytes", len);
    CHECK(out[10 + 1] == MTZ2_PHASE_BREAK_TOUCH &&
          out[10 + 20] == 0u && out[10 + 21] == 0u,
          "a lift reports stage %u with amplitude %u",
          out[11], (unsigned)(out[30] | (out[31] << 8)));
}

/*
 * The round trip that decides whether a tap lands under the finger: a pixel in,
 * the guest's own arithmetic out, the same pixel back.
 */
static void test_a_coordinate_maps_back_to_the_pixel_it_came_from(void) {
    s5l_mtz2_t dev;
    uint8_t out[MTZ2_PAYLOAD_LIMIT];
    s5l_mtz2_reset(&dev);

    static const struct { uint16_t x, y; } P[] = {
        { 0u, 0u }, { 319u, 479u }, { 160u, 240u }, { 1u, 478u }, { 47u, 91u }
    };
    for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) {
        s5l_mt_contact_t c = one_finger(P[i].x, P[i].y, MTZ2_PHASE_TOUCHING);
        int px = -1, py = -1;
        CHECK(s5l_mtz2_encode(&dev, &c, 1u, 1u, 16u, out) == 42u,
              "encoding (%u,%u) failed", P[i].x, P[i].y);
        decode_contact_pixel(&out[10], dev.surface_width, dev.surface_height,
                             &px, &py);
        CHECK(px == (int)P[i].x && py == (int)P[i].y,
              "pixel (%u,%u) came back as (%d,%d) through the guest's own "
              "conversion", P[i].x, P[i].y, px, py);
    }
}

/*
 * The three-way honesty. Every one of these is a different fact about the
 * device and each of them must be a REFUSAL and not a silent drop.
 */
static void test_injection_refuses_when_the_device_cannot_report(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    s5l_mt_contact_t c = one_finger(10u, 20u, MTZ2_PHASE_MAKE_TOUCH);

    /* Held in reset. */
    s5l_mtz2_reset(&dev);
    s5l_mtz2_bind(&dev, &s);
    CHECK(!s5l_mtz2_set_contacts(&dev, &c, 1u),
          "a part held in its reset pin accepted a report");
    CHECK(dev.injects_refused == 1u && !dev.atn,
          "the refusal left no evidence (refused=%llu atn=%d)",
          (unsigned long long)dev.injects_refused, (int)dev.atn);

    /* Released, but the driver has not had its HBPP answer, so its this+0x1bc
     * is zero and deviceReadResultData would throw a 0xEB frame away. */
    release_reset(&dev);
    CHECK(!s5l_mtz2_set_contacts(&dev, &c, 1u),
          "a device the driver has not brought up accepted a report");
    CHECK(dev.injects_refused == 2u, "the second refusal was not counted");

    /* Brought up: accepted once. */
    bring_up(&dev, &s);
    CHECK(s5l_mtz2_set_contacts(&dev, &c, 1u), "a live device refused");
    /* And then refused, because one report is already pending. */
    CHECK(!s5l_mtz2_set_contacts(&dev, &c, 1u),
          "a second report overwrote an unread one — that is how the "
          "finger-down half of a tap goes missing");

    /* Malformed requests, each refused before anything is queued. */
    s5l_mtz2_reset(&dev); s5l_mtz2_bind(&dev, &s); bring_up(&dev, &s);
    s5l_mt_contact_t bad = c;
    bad.id = 0u;
    CHECK(!s5l_mtz2_set_contacts(&dev, &bad, 1u), "contact id 0 was accepted");
    bad = c; bad.x = S5L_MT_PANEL_W;
    CHECK(!s5l_mtz2_set_contacts(&dev, &bad, 1u), "x off the panel accepted");
    bad = c; bad.y = S5L_MT_PANEL_H;
    CHECK(!s5l_mtz2_set_contacts(&dev, &bad, 1u), "y off the panel accepted");
    bad = c; bad.phase = MTZ2_PHASE_OUT_OF_RANGE + 1u;
    CHECK(!s5l_mtz2_set_contacts(&dev, &bad, 1u), "an unknown phase accepted");
    CHECK(!s5l_mtz2_set_contacts(&dev, NULL, 1u), "a null contact accepted");
    CHECK(!s5l_mtz2_set_contacts(&dev, &c, MTZ2_CONTACT_MAX + 1u),
          "more contacts than this panel has were accepted");
    CHECK(!s5l_mtz2_set_contacts(NULL, &c, 1u), "a null device accepted");
    CHECK(!dev.atn && dev.frames_queued == 0u,
          "a malformed request queued something anyway");

    /* A reset drops a queued report AND the line with it: the part's scan
     * buffer does not survive its reset pin. */
    CHECK(s5l_mtz2_set_contacts(&dev, &c, 1u), "a live device refused");
    assert_reset(&dev);
    CHECK(!s5l_mtz2_irq(&dev) && dev.frame_len == 0u,
          "a report survived a reset with the attention line still up");
}

/*
 * Five contacts, the panel's limit, through the whole path — the case where
 * the payload is longest and every length in the model is closest to its
 * bound.
 */
static void test_five_contacts_fit_and_survive_the_wire(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    s5l_mt_contact_t c[MTZ2_CONTACT_MAX];
    uint8_t rxdata[MTZ2_PAYLOAD_LIMIT + 8];
    const uint8_t *payload = NULL;
    unsigned L = 0, plen = 0, count = 0;
    bring_up(&dev, &s);

    for (unsigned i = 0; i < MTZ2_CONTACT_MAX; i++) {
        c[i] = one_finger((uint16_t)(20u + i * 60u), (uint16_t)(30u + i * 80u),
                          MTZ2_PHASE_TOUCHING);
        c[i].id = (uint8_t)(i + 1u);
    }
    CHECK(s5l_mtz2_set_contacts(&dev, c, MTZ2_CONTACT_MAX),
          "five fingers were refused");
    if (!read_one_frame(&s, rxdata, &L, &payload, &plen) || !payload) {
        CHECK(false, "the driver rejected a five-contact frame");
        return;
    }
    CHECK(plen == 10u + 5u * 32u,
          "a five-contact payload is %u bytes, expected 170", plen);
    CHECK(userspace_accepts_frame(payload, plen, &count) && count == 5u,
          "the parser found %u contacts in a five-contact frame", count);
    for (unsigned i = 0; i < MTZ2_CONTACT_MAX; i++) {
        int px = -1, py = -1;
        decode_contact_pixel(&payload[10 + i * 32], dev.surface_width,
                             dev.surface_height, &px, &py);
        CHECK(payload[10 + i * 32] == (uint8_t)(i + 1u),
              "contact %u carries id %u", i, payload[10 + i * 32]);
        CHECK(px == (int)c[i].x && py == (int)c[i].y,
              "contact %u landed at (%d,%d), not (%u,%u)",
              i, px, py, c[i].x, c[i].y);
    }
    CHECK(!s5l_mtz2_irq(&dev), "the line stayed up after five were delivered");
}

/*
 * MUTATIONS. Each one breaks exactly one thing the driver or the parser checks
 * and proves the check is real -- a test that only ever sees correct bytes
 * cannot tell a validator from a comment.
 */
static void test_mutations_are_caught(void) {
    s5l_mtz2_t dev;
    s5l_spi_slave_t s;
    uint8_t rxdata[MTZ2_PAYLOAD_LIMIT + 8];
    uint8_t good[MTZ2_PAYLOAD_LIMIT + 8];
    const uint8_t *payload = NULL;
    unsigned L = 0, plen = 0, count = 0, total;
    s5l_mt_contact_t c = one_finger(160u, 240u, MTZ2_PHASE_MAKE_TOUCH);

    bring_up(&dev, &s);
    CHECK(s5l_mtz2_set_contacts(&dev, &c, 1u), "setup: injection refused");
    if (!read_one_frame(&s, rxdata, &L, &payload, &plen) || !payload) {
        CHECK(false, "setup: the unmutated frame was rejected, so no mutation "
                     "below could be distinguished from it");
        return;
    }
    total = L + 5u;
    memcpy(good, rxdata, total);

    /* 1. A wrong HEADER check byte. The driver's only header test is that the
     *    first five bytes sum to zero mod 256 (0xc0441330). */
    memcpy(rxdata, good, total);
    rxdata[4] = (uint8_t)(rxdata[4] + 1u);
    CHECK(!driver_accepts_data(rxdata, total, &payload, &plen),
          "a header whose five bytes do not sum to zero was accepted");

    /* 2. A wrong LENGTH field. The payload length comes from the frame's own
     *    [2..3] while the checksum's position comes from the transfer, so a
     *    disagreement lands the checksum comparison on the wrong bytes. */
    memcpy(rxdata, good, total);
    rxdata[2] = (uint8_t)(rxdata[2] - 4u);
    rxdata[4] = (uint8_t)(rxdata[4] + 4u);     /* keep the header sum legal */
    CHECK(!driver_accepts_data(rxdata, total, &payload, &plen),
          "a length field that disagrees with the transfer was accepted");

    /* 3. A wrong PAYLOAD checksum. */
    memcpy(rxdata, good, total);
    rxdata[total - 2u] = (uint8_t)(rxdata[total - 2u] ^ 0xffu);
    CHECK(!driver_accepts_data(rxdata, total, &payload, &plen),
          "a frame with a corrupt payload checksum was accepted");

    /* 4. The frame type userspace diverts. 0x50 is the one payload byte the
     *    KERNEL itself reads, at 0xc0438a1c. */
    memcpy(rxdata, good, total);
    rxdata[5] = 0x50u;
    CHECK(!userspace_accepts_frame(&rxdata[5], plen ? plen : L - 2u, &count),
          "a payload beginning 0x50 was accepted as a touch frame");

    /* 5. A count the length cannot support. */
    memcpy(rxdata, good, total);
    rxdata[5 + 3] = 2u;
    CHECK(!userspace_accepts_frame(&rxdata[5], L - 2u, &count),
          "a frame claiming two contacts in a one-contact payload was "
          "accepted");

    /* 6. AN ATTENTION LINE THAT NEVER DEASSERTS. A device that leaves the line
     *    up after delivery re-enters the handler forever; the guest's next
     *    length read must be able to answer zero. */
    CHECK(!s5l_mtz2_irq(&dev), "setup: the line did not fall");
    dev.atn = true;                             /* the mutation */
    CHECK(s5l_mtz2_irq(&dev), "setup: the mutation did not take");
    {
        uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
        unsigned again = 0xffffu;
        build_frame_read(tx, MTZ2_FRAME_LEN, 2u, 0u);
        xfer(&s, tx, rx, MTZ2_FRAME_LEN);
        CHECK(driver_accepts_length(rx, &again) && again == 0u,
              "with the line stuck up, the length read answered %u instead of "
              "the zero that ends the handler", again);
    }
    dev.atn = false;

    /* 7. A COORDINATE THAT MAPS TO THE WRONG PIXEL. Flip the Y sense the model
     *    applies and the same request must come back somewhere else. */
    {
        uint8_t out[MTZ2_PAYLOAD_LIMIT];
        int px = -1, py = -1;
        s5l_mt_contact_t top = one_finger(0u, 0u, MTZ2_PHASE_TOUCHING);
        CHECK(s5l_mtz2_encode(&dev, &top, 1u, 1u, 16u, out) == 42u,
              "encoding the top-left corner failed");
        decode_contact_pixel(&out[10], dev.surface_width, dev.surface_height,
                             &px, &py);
        CHECK(px == 0 && py == 0,
              "the top-left pixel decoded to (%d,%d)", px, py);
        /*
         * Un-flip Y in the encoded record -- write what a device that treated
         * Y as increasing DOWNWARD would have written for the same pixel --
         * and the guest's own conversion must land somewhere else entirely.
         * If it does not, the flip is not load-bearing and the check above
         * proves nothing.
         */
        {
            uint32_t unflipped = (uint32_t)(0u * 15u + 7u) << 8;
            out[10 + 8]  = (uint8_t)(unflipped & 0xffu);
            out[10 + 9]  = (uint8_t)((unflipped >> 8) & 0xffu);
            out[10 + 10] = (uint8_t)((unflipped >> 16) & 0xffu);
            out[10 + 11] = (uint8_t)((unflipped >> 24) & 0xffu);
            decode_contact_pixel(&out[10], dev.surface_width,
                                 dev.surface_height, &px, &py);
            CHECK(py == (int)S5L_MT_PANEL_H - 1,
                  "un-flipping Y put the top-left pixel at row %d; a mirrored "
                  "digitizer should put it at %u", py, S5L_MT_PANEL_H - 1u);
        }
    }
}

/*
 * End to end through the real machine: a host injection reaches the CPU's IRQ
 * line through GPIO group 4 and VIC line 2, and the frame comes back out
 * through the real SPI controller.
 */
static void test_an_injection_reaches_the_cpu_through_the_cascade(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0u, 1u << 16), "machine init failed");
    void *ctx = m.bus.ctx;

    /* Arm the line the way run59 measured the guest arming it: INTEN for group
     * 4 holding 0x08000000, which is bit 27, which is line 155. The offset is
     * relative to the PAGE, not to the decoded window. */
    m.bus.write32(ctx, S5L8900_GPIOIC_PAGE + GPIOIC_INTEN + 4u * 4u,
                  1u << 27);
    m.bus.write32(ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << 2);
    CHECK(m.gpioic.en[4] == (1u << 27),
          "the group 4 enable read back 0x%08x", m.gpioic.en[4]);

    /* Bring the device up through the real pins and the real controller. */
    m.bus.write32(ctx, S5L8900_GPIO_BASE + S5L_GPIO_FSEL,
                  (6u << 16) | (6u << 8) | 0xfu);
    CHECK(!m.mtz2.in_reset, "the reset line did not release");
    {
        uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
        build_hbpp_probe(tx);
        unsigned sent = 0, got = 0;
        while (got < MTZ2_FRAME_LEN) {
            while (sent < MTZ2_FRAME_LEN &&
                   m.spi[1].tx_level + m.spi[1].rx_level < S5L_SPI_FIFO_DEPTH)
                m.bus.write32(ctx, S5L8900_SPI1_BASE + SPI_TXDATA, tx[sent++]);
            while (m.spi[1].rx_level && got < MTZ2_FRAME_LEN)
                rx[got++] =
                    (uint8_t)m.bus.read32(ctx, S5L8900_SPI1_BASE + SPI_RXDATA);
        }
        CHECK(driver_says_in_hbpp(rx), "bring-up failed through the machine");
    }

    s5l8900_tick(&m, 0);
    CHECK(!m.cpu.irq_line, "the CPU line is up before anything was injected");

    s5l_mt_contact_t c = one_finger(160u, 240u, MTZ2_PHASE_MAKE_TOUCH);
    CHECK(s5l_mtz2_set_contacts(&m.mtz2, &c, 1u),
          "the machine's device refused an injection (refused=%llu)",
          (unsigned long long)m.mtz2.injects_refused);
    s5l8900_tick(&m, 0);
    CHECK(m.gpioic.raw[4] & (1u << 27),
          "the attention line did not reach GPIO group 4 bit 27");
    CHECK(s5l_gpioic_group_irq(&m.gpioic, 4u),
          "group 4 did not assert with the line enabled");
    CHECK(m.cpu.irq_line,
          "the injection did not reach the CPU through VIC line 2");

    /*
     * The wake source. The multitouch group's entry must name the CASCADE VIC
     * line and not 155: wake_line_enabled() drops anything at or above
     * 32 * VIC_COUNT, so a 155 would be masked out silently and the core could
     * never be woken by a touch.
     */
    {
        const s5l_wake_source_t *src = NULL;
        unsigned n = s5l8900_wake_sources(&src);
        bool found = false;
        for (unsigned i = 0; i < n; i++) {
            if (strcmp(src[i].name, "gpio-group4")) continue;
            found = true;
            CHECK(src[i].line == s5l_gpioic_cascade(4u),
                  "gpio-group4's wake line is %u, but the cascade puts group 4 "
                  "on VIC line %u", src[i].line, s5l_gpioic_cascade(4u));
            CHECK(src[i].line < 32u * S5L8900_VIC_COUNT,
                  "gpio-group4's wake line is %u, which wake_line_enabled() "
                  "rejects outright", src[i].line);
        }
        CHECK(found, "there is no gpio-group4 wake source at all");
    }
    /* An already-asserted line completes a WFI with no future edge to name:
     * machine_wait_for_interrupt() refreshes at zero elapsed time before it
     * consults any source, so the group-4 entry's S5L_WAKE_NEVER cannot lose
     * an interrupt that has already happened. */
    CHECK(m.bus.wait_for_interrupt && m.bus.wait_for_interrupt(m.bus.ctx),
          "a pending touch report did not complete a WFI");

    s5l8900_free(&m);
}

/*
 * End to end through the machine: the device really is on spi1 chip select 0,
 * its attention line really reaches the CPU through GPIO group 4 and VIC line
 * 2, and the whole path is exercised by asserting `atn` — which is exactly the
 * one line step 4 adds.
 */
static void test_machine_wires_the_device_and_its_attention_line(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0u, 1u << 16), "machine init failed");
    void *c = m.bus.ctx;

    CHECK(m.spi[1].slaves[0].transfer != NULL &&
          m.spi[1].slaves[0].ctx == &m.mtz2,
          "the touch controller is not on spi1 chip select 0");
    CHECK(m.spi[1].slaves[1].transfer == NULL &&
          m.spi[0].slaves[0].transfer == NULL,
          "something else was attached to an SPI bus");
    CHECK(!m.mtz2.hbpp_answered && m.mtz2.in_reset,
          "the machine's device did not power on unclaimed and held in "
          "reset: answered=%d in_reset=%d",
          (int)m.mtz2.hbpp_answered, (int)m.mtz2.in_reset);
    CHECK(m.stub_declare_failures == 0u,
          "%u declarations were refused, so a GPIO subscription is missing",
          m.stub_declare_failures);

    /* Drive the HBPP probe through the real controller, the way the driver
     * does: fill the transmit FIFO, drain the receive FIFO, repeat. */
    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
    build_hbpp_probe(tx);
    /* Release the reset line the way the guest does -- through the GPIO
     * function-select register, which is the only path that reaches the pin,
     * so this exercises the subscription as well as the device. */
    m.bus.write32(c, S5L8900_GPIO_BASE + S5L_GPIO_FSEL,
                  (6u << 16) | (6u << 8) | 0xfu);
    CHECK(!m.mtz2.in_reset && m.mtz2.resets == 1u,
          "an fsel store to group 6 bit 6 did not release the touch "
          "controller's reset: in_reset=%d resets=%llu",
          (int)m.mtz2.in_reset, (unsigned long long)m.mtz2.resets);
    m.bus.write32(c, S5L8900_SPI1_BASE + SPI_CNT, MTZ2_FRAME_LEN);
    m.bus.write32(c, S5L8900_SPI1_BASE + SPI_CONTROL, SPI_CONTROL_START);
    unsigned sent = 0, got = 0;
    while (got < MTZ2_FRAME_LEN) {
        while (sent < MTZ2_FRAME_LEN &&
               m.spi[1].tx_level + m.spi[1].rx_level < S5L_SPI_FIFO_DEPTH)
            m.bus.write32(c, S5L8900_SPI1_BASE + SPI_TXDATA, tx[sent++]);
        while (m.spi[1].rx_level && got < MTZ2_FRAME_LEN)
            rx[got++] = (uint8_t)m.bus.read32(c, S5L8900_SPI1_BASE + SPI_RXDATA);
    }
    CHECK(driver_says_in_hbpp(rx),
          "the probe failed through the real controller: %02x %02x %02x %02x",
          rx[0], rx[1], rx[2], rx[3]);

    /* The attention line. Nothing asserts it yet, so assert it by hand — this
     * is precisely the state step 4 creates. */
    CHECK(!m.cpu.irq_line, "the machine is interrupting with nothing pending");
    m.bus.write32(c, S5L8900_GPIOIC_PAGE + GPIOIC_INTEN + 4u * 4u, 0x08000000u);
    m.bus.write32(c, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << 2);
    s5l8900_tick(&m, 0u);
    CHECK(!m.cpu.irq_line,
          "arming the touch interrupt raised it with no contact pending");

    m.mtz2.atn = true;
    s5l8900_tick(&m, 0u);
    CHECK(m.gpioic.raw[4] == 0x08000000u,
          "the attention line did not reach GPIO group 4 bit 27 (raw=0x%08x)",
          m.gpioic.raw[4]);
    CHECK((m.vic[0].raw & (1u << 2)) != 0u,
          "the attention line did not reach VIC line 2");
    CHECK(m.cpu.irq_line, "the attention line did not reach the CPU");

    /*
     * Withdraw it and acknowledge, in that order and with a tick between —
     * which is the order a handler that drains the device produces. The
     * interrupt controller's pending latch is level-sensitive, so acknowledging
     * a source that is still driving re-latches it immediately; the device
     * dropping its attention line is what ends the interrupt, and that is the
     * correct protocol rather than an inconvenience.
     */
    m.mtz2.atn = false;
    s5l8900_tick(&m, 0u);
    CHECK(m.cpu.irq_line,
          "dropping the attention line cleared a latch the guest had not "
          "acknowledged — a report raised and withdrawn between two "
          "instructions would be lost");
    m.bus.write32(c, S5L8900_GPIOIC_PAGE + GPIOIC_INTSTAT + 4u * 4u,
                  0x08000000u);
    s5l8900_tick(&m, 0u);
    CHECK(!m.cpu.irq_line, "the acknowledged attention line stayed asserted");

    s5l8900_free(&m);
}

/*
 * The protocol position is state. A checkpoint can land mid-frame, and — far
 * worse — a restore that reset `hbpp` to its power-on true would make the next
 * probe, which is one of attemptToBootloadDevice's, say "in HBPP".
 */
static void test_snapshot_carries_the_protocol_position(void) {
    s5l8900_t src, dst;
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    CHECK(s5l8900_init(&src, 0u, 1u << 16), "source init failed");
    CHECK(s5l8900_init(&dst, 0u, 1u << 16), "destination init failed");

    uint8_t tx[MTZ2_FRAME_LEN], rx[MTZ2_FRAME_LEN];
    s5l_spi_slave_t s = src.spi[1].slaves[0];
    build_hbpp_probe(tx);
    release_reset(&src.mtz2);
    xfer(&s, tx, rx, MTZ2_FRAME_LEN);
    src.mtz2.hbpp_answered = true;             /* the claim is spent */
    build_frame(tx, MTZ2_OP_REPORT_INFO, MTZ2_REPORT_GEOMETRY);
    for (unsigned i = 0; i < 6u; i++) (void)s.transfer(s.ctx, tx[i]);
    CHECK(src.mtz2.len == MTZ2_FRAME_LEN && src.mtz2.pos == 6u &&
          src.mtz2.hbpp_answered,
          "the source is not mid-frame: len=%u pos=%u answered=%d",
          src.mtz2.len, src.mtz2.pos, (int)src.mtz2.hbpp_answered);

    CHECK(snapshot_save_mem(&src, &blob, &blob_len) == SNAP_OK, "save failed");
    CHECK(snapshot_load_mem(&dst, blob, blob_len) == SNAP_OK, "load failed");
    CHECK(dst.mtz2.hbpp_answered && dst.mtz2.in_reset == src.mtz2.in_reset,
          "the restored device forgot that its claim was spent -- the next "
          "probe is a bootload attempt's and would be answered affirmatively, "
          "sending the driver into a 54 KB firmware download");
    CHECK(dst.mtz2.pos == src.mtz2.pos && dst.mtz2.len == src.mtz2.len &&
          dst.mtz2.op == src.mtz2.op &&
          memcmp(dst.mtz2.rsp, src.mtz2.rsp, S5L_MTZ2_BUF) == 0,
          "the in-flight frame did not survive");
    CHECK(dst.mtz2.surface_width == src.mtz2.surface_width &&
          dst.mtz2.surface_height == src.mtz2.surface_height &&
          dst.mtz2.rows == src.mtz2.rows && dst.mtz2.columns == src.mtz2.columns,
          "the published geometry did not survive — a restored machine would "
          "put every tap somewhere else");
    CHECK(dst.spi[1].slaves[0].ctx == &dst.mtz2,
          "the restored controller points at the source's device");

    /* Finish the frame in the destination and it must still be accepted. */
    s5l_spi_slave_t d = dst.spi[1].slaves[0];
    for (unsigned i = 6; i < MTZ2_FRAME_LEN; i++)
        rx[i] = d.transfer(d.ctx, tx[i]);
    for (unsigned i = 0; i < 6u; i++) rx[i] = dst.mtz2.rsp[i];
    CHECK(driver_accepts_reply(tx, rx),
          "the frame resumed in the destination was malformed");

    free(blob);
    s5l8900_free(&src);
    s5l8900_free(&dst);
}

/* States the model cannot produce must not survive a round trip. */
static void test_snapshot_rejects_impossible_mtz2_state(void) {
    s5l8900_t src, dst;
    uint8_t *blob = NULL;
    size_t blob_len = 0;
    CHECK(s5l8900_init(&src, 0u, 1u << 16), "source init failed");
    CHECK(s5l8900_init(&dst, 0u, 1u << 16), "destination init failed");

    struct { const char *what; uint8_t pos, len; } BAD[] = {
        { "a position outside an idle framer",   3u, 0u  },
        { "a position past the end of a packet", 16u, 16u },
        /* The bound is the REPLY buffer, not the command buffer: a data read
         * is L + 5 bytes and every one of them is a framed packet position. */
        { "a packet longer than the buffer",     0u, S5L_MTZ2_RSP + 1u },
    };
    for (unsigned i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        s5l_mtz2_reset(&src.mtz2);
        src.mtz2.pos = BAD[i].pos;
        src.mtz2.len = BAD[i].len;
        blob = NULL; blob_len = 0;
        CHECK(snapshot_save_mem(&src, &blob, &blob_len) == SNAP_ERR_CORRUPT,
              "%s was snapshotted", BAD[i].what);
        CHECK(blob == NULL, "%s left an allocation behind", BAD[i].what);
    }

    /* Board wiring must match on both sides, as it must for I2C and SPI. */
    s5l_mtz2_reset(&src.mtz2);
    src.spi[1].slaves[0].ctx = NULL;
    blob = NULL; blob_len = 0;
    CHECK(snapshot_save_mem(&src, &blob, &blob_len) == SNAP_ERR_CORRUPT,
          "a machine whose spi1 device is not its own Z2 was snapshotted");

    s5l8900_free(&dst);
    s5l8900_free(&src);
}

int main(void) {
    printf("iOS3-VM AppleMultitouchZ2SPI device tests\n");
    test_reset_is_total_and_null_safe();
    test_checksum_is_a_plain_sum_stored_little_endian();
    test_the_claim_is_spent_exactly_once();
    test_the_dummy_transfer_cannot_spend_the_claim();
    test_reset_does_not_rearm_the_claim();
    test_the_whole_bring_up_sequence();
    test_the_driver_tests_both_halfwords();
    test_get_report_info_satisfies_isbootloaded();
    test_control_read_returns_the_report_body();
    test_the_other_opcodes_are_well_formed();
    test_framing_survives_without_a_chip_select();
    test_attention_line_is_quiet_until_a_frame_exists();
    test_an_idle_device_answers_a_length_read_with_zero();
    test_an_injected_contact_is_delivered_over_the_wire();
    test_the_payload_is_the_frame_userspace_parses();
    test_a_coordinate_maps_back_to_the_pixel_it_came_from();
    test_injection_refuses_when_the_device_cannot_report();
    test_five_contacts_fit_and_survive_the_wire();
    test_mutations_are_caught();
    test_an_injection_reaches_the_cpu_through_the_cascade();
    test_machine_wires_the_device_and_its_attention_line();
    test_snapshot_carries_the_protocol_position();
    test_snapshot_rejects_impossible_mtz2_state();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
