/*
 * S5LBox — WM8991 codec and I2S window tests.
 *
 * The properties defended here are the ones the shipped driver actually
 * depends on, each traced to the instruction that depends on it:
 *
 *   - register 0 reads 0x8990, MSB first on the wire   (cmp at 0xc068b0b0)
 *   - register 1 bit 5 does NOT read back              (test at 0xc068b0ec)
 *   - register 0x12 bit 12 mirrors 0x17 bit 12         (poll at 0xc068d4ac)
 *   - both write encodings decode by length            (0xc0540050 / 0xc0540108)
 *   - the seven I2S offsets are storage, the rest visible
 *
 * They are driven through the real controller wherever possible, using the
 * exact stock register sequence, so a change to the I2C model that broke the
 * codec would fail here rather than only in a four-minute boot.
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

/* ------------------------------------------------- the stock bus driver --- */

static void ack(s5l_i2c_t *bus, uint32_t event) {
    uint32_t pending = s5l_i2c_read(bus, I2C_INT);
    CHECK((pending & event) != 0u, "pending=%08x missing event %08x",
          pending, event);
    s5l_i2c_write(bus, I2C_INT, event);
}

static void setup(s5l_i2c_t *bus, s5l_wm8991_t *codec) {
    s5l_i2c_reset(bus);
    s5l_wm8991_reset(codec);
    s5l_i2c_slave_t slave;
    s5l_wm8991_bind(codec, &slave);
    CHECK(s5l_i2c_attach(bus, &slave), "could not attach the WM8991");
    s5l_i2c_write(bus, I2C_ENABLE, 1u);
    s5l_i2c_write(bus, I2C_INT, I2C_INT_ALL);
}

/* Exactly what AppleS5L8900XI2CController does: address in DS, START in STAT,
 * one byte per CON.RESUME, STOP by dropping START. */
static void bus_write(s5l_i2c_t *bus, const uint8_t *data, unsigned count) {
    s5l_i2c_write(bus, I2C_STAT, I2C_STAT_MODE_MTX | I2C_STAT_ENABLE);
    s5l_i2c_write(bus, I2C_DS, (uint32_t)WM8991_I2C_ADDR << 1);
    s5l_i2c_write(bus, I2C_STAT, I2C_STAT_MODE_MTX |
                                      I2C_STAT_ENABLE | I2C_STAT_START);
    ack(bus, I2C_INT_BYTE);
    for (unsigned i = 0; i < count; i++) {
        s5l_i2c_write(bus, I2C_DS, data[i]);
        s5l_i2c_write(bus, I2C_CON, I2C_CON_ACKEN | I2C_CON_RESUME);
        ack(bus, I2C_INT_BYTE);
    }
    s5l_i2c_write(bus, I2C_STAT, I2C_STAT_MODE_MRX | I2C_STAT_ENABLE);
    ack(bus, I2C_INT_STOP);
}

/* The driver's readReg16 (0xc053ff94): one index byte out, two bytes in. */
static uint16_t bus_read16(s5l_i2c_t *bus, uint8_t reg, uint8_t *raw) {
    uint8_t idx = reg;
    bus_write(bus, &idx, 1u);

    s5l_i2c_write(bus, I2C_CON, I2C_CON_ACKEN);
    s5l_i2c_write(bus, I2C_DS, (uint32_t)WM8991_I2C_ADDR << 1);
    s5l_i2c_write(bus, I2C_STAT, I2C_STAT_MODE_MRX |
                                      I2C_STAT_ENABLE | I2C_STAT_START);
    ack(bus, I2C_INT_BYTE);
    uint8_t b[2];
    for (unsigned i = 0; i < 2u; i++) {
        s5l_i2c_write(bus, I2C_CON,
                      (i == 0u ? I2C_CON_ACKEN : 0u) | I2C_CON_RESUME);
        ack(bus, I2C_INT_BYTE);
        b[i] = (uint8_t)s5l_i2c_read(bus, I2C_DS);
    }
    s5l_i2c_write(bus, I2C_STAT, I2C_STAT_MODE_MRX | I2C_STAT_ENABLE);
    ack(bus, I2C_INT_STOP);
    if (raw) { raw[0] = b[0]; raw[1] = b[1]; }
    /* The driver's own assembly, verbatim: orr r0, r0, r3, lsl #8. */
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

/* writeReg16 (0xc0540050): index byte, then the value MSB-first. */
static void bus_write16(s5l_i2c_t *bus, uint8_t reg, uint16_t val) {
    uint8_t seq[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };
    bus_write(bus, seq, 3u);
}

/* writeReg9 (0xc0540108): (reg << 1) | value[8], then value[7:0]. */
static void bus_write9(s5l_i2c_t *bus, uint8_t reg, uint16_t val) {
    uint8_t seq[2] = { (uint8_t)((reg << 1) | ((val >> 8) & 1u)),
                       (uint8_t)val };
    bus_write(bus, seq, 2u);
}

/* --------------------------------------------------------------- tests --- */

/*
 * The constants themselves, against the firmware they were read out of. Every
 * other test in this file uses the symbols, so none of them can notice if a
 * symbol's VALUE is wrong — a codec bound at 0x1a would answer every test here
 * and no device on the guest's bus. These are the literals, spelled out, each
 * with the place in the shipped image that fixes it.
 */
static void test_constants_match_the_shipped_firmware(void) {
    /* /device-tree/arm-io/i2c0/audio0, `reg` cell 0. Same shape as the PMU's
     * 0x73 and the accelerometer's 0x1d on the same controller. */
    CHECK(WM8991_I2C_ADDR == 0x1bu,
          "codec address is 0x%02x, the device tree says 0x1b", WM8991_I2C_ADDR);
    /* The literal at 0xc068b124, loaded only by 0xc068b0ac and compared only at
     * 0xc068b0b0. Also entry 0 of the driver's own default table at
     * 0xc0691030, which is an independent second source for the same number. */
    CHECK(WM8991_ID_VALUE == 0x8990u,
          "identity is 0x%04x, the probe compares against 0x8990",
          WM8991_ID_VALUE);
    /* `mov r2, #0x20` at 0xc068b0bc, tested by `ands r0, r0, #0x20`. */
    CHECK(WM8991_REG_PWR1 == 0x01u && WM8991_PWR1_PROBE == 0x0020u,
          "the discriminator is R%u bit mask 0x%04x, expect R1 / 0x0020",
          WM8991_REG_PWR1, WM8991_PWR1_PROBE);
    /* `mov r1, #0x12` / `mov r1, #0x17` and the 0x1000 mask in the poll. */
    CHECK(WM8991_REG_GPSTAT == 0x12u && WM8991_REG_GPCTRL == 0x17u &&
          WM8991_GP_BIT == 0x1000u,
          "the polled pair is 0x%02x/0x%02x mask 0x%04x, expect 0x12/0x17/0x1000",
          WM8991_REG_GPSTAT, WM8991_REG_GPCTRL, WM8991_GP_BIT);
    /* Seven-bit index space: the packed encoding cannot address more. */
    CHECK(WM8991_NREG == 0x80u, "register file is %u deep, expect 128",
          WM8991_NREG);
    /* /arm-io/i2s0 reg {0x04a00000,0x1000} and /arm-io/i2s1 {0x04d00000,0x1000},
     * with arm-io mapping child + 0x38000000. The guest's own driver prints the
     * mapped VA for each on every boot, which is the second source. */
    CHECK(S5L8900_I2S0_BASE == 0x3ca00000u && S5L8900_I2S1_BASE == 0x3cd00000u,
          "I2S windows are 0x%08x/0x%08x, expect 0x3ca00000/0x3cd00000",
          S5L8900_I2S0_BASE, S5L8900_I2S1_BASE);
    /* The codec must not collide with anything else the board puts on i2c0:
     * the PMU at 0x73, and the unmodelled accelerometer 0x1d, ALS 0x44 and
     * tethered 0x29 whose addresses are recorded here so a future attach
     * cannot silently duplicate one. */
    CHECK(WM8991_I2C_ADDR != PCF50635_I2C_ADDR &&
          WM8991_I2C_ADDR != 0x1du && WM8991_I2C_ADDR != 0x44u &&
          WM8991_I2C_ADDR != 0x29u,
          "the codec address collides with another i2c0 node");
}

static void test_identity_is_the_hard_gate(void) {
    s5l_i2c_t bus; s5l_wm8991_t codec;
    setup(&bus, &codec);

    uint8_t raw[2] = { 0, 0 };
    uint16_t id = bus_read16(&bus, WM8991_REG_ID, raw);
    CHECK(id == 0x8990u, "register 0 read %04x, the probe demands 8990", id);
    /* Byte order is a separate claim from the assembled value: a model that
     * emitted 0x90 then 0x89 would still assemble to 0x8990 under a swapped
     * reader. Pin the wire bytes themselves. */
    CHECK(raw[0] == 0x89u && raw[1] == 0x90u,
          "wire bytes %02x %02x, expect 89 90 (MSB first)", raw[0], raw[1]);
    CHECK(codec.id_reads == 1u, "identity read not counted");

    /* The identity must survive a guest write in both encodings: the probe can
     * run again, and a model whose ID could be overwritten would pass once. */
    bus_write16(&bus, WM8991_REG_ID, 0x1234u);
    CHECK(bus_read16(&bus, WM8991_REG_ID, NULL) == 0x8990u,
          "a wide write changed the part identity");
    bus_write9(&bus, WM8991_REG_ID, 0x01ffu);
    CHECK(bus_read16(&bus, WM8991_REG_ID, NULL) == 0x8990u,
          "a packed write changed the part identity");
    CHECK(s5l_wm8991_peek(&codec, WM8991_REG_ID) == 0x8990u,
          "peek disagrees with the bus about the identity");
    CHECK(codec.written[WM8991_REG_ID] == 0u,
          "the identity register was marked as written storage");

    /* Not a slave at any other address on this bus. */
    s5l_i2c_write(&bus, I2C_STAT, I2C_STAT_MODE_MTX | I2C_STAT_ENABLE);
    s5l_i2c_write(&bus, I2C_DS, 0x1cu << 1);
    s5l_i2c_write(&bus, I2C_STAT, I2C_STAT_MODE_MTX |
                                       I2C_STAT_ENABLE | I2C_STAT_START);
    CHECK((s5l_i2c_read(&bus, I2C_STAT) & I2C_STAT_NAK) != 0u,
          "the codec answered an address that is not 0x1b");
}

static void test_probe_bit_selects_wm8991_not_wm1817(void) {
    s5l_i2c_t bus; s5l_wm8991_t codec;
    setup(&bus, &codec);

    /* The probe's exact sequence: set R1 bit 5, read R1 back, test bit 5.
     * `ands r0, r0, #0x20` must be ZERO, which is what makes the getter at
     * 0xc068b044 return "WM8991" rather than "WM1817". */
    bus_write16(&bus, WM8991_REG_PWR1, WM8991_PWR1_PROBE);
    uint16_t back = bus_read16(&bus, WM8991_REG_PWR1, NULL);
    CHECK((back & WM8991_PWR1_PROBE) == 0u,
          "R1 bit 5 read back set (%04x): the driver would call this a WM1817",
          back);
    CHECK(codec.probe_bit_writes == 1u,
          "attempt to set the discriminator bit was not counted");

    /* Every other bit of R1 is ordinary storage — the probe's own cleanup
     * writes it, and the power-up sequence writes R1 constantly. */
    bus_write16(&bus, WM8991_REG_PWR1, 0xffffu);
    CHECK(bus_read16(&bus, WM8991_REG_PWR1, NULL) == (0xffffu & ~WM8991_PWR1_PROBE),
          "R1 masked more than bit 5");
    bus_write16(&bus, WM8991_REG_PWR1, 0x0015u);
    CHECK(bus_read16(&bus, WM8991_REG_PWR1, NULL) == 0x0015u,
          "a real R1 value from the power-up sequence did not round-trip");
}

static void test_status_bit_mirrors_control_so_the_poll_terminates(void) {
    s5l_i2c_t bus; s5l_wm8991_t codec;
    setup(&bus, &codec);

    /*
     * The loop at 0xc068d4ac, reproduced. It writes 0x12 with bit 12 forced
     * clear and then waits for bit 12 of 0x12 to equal the level commanded
     * through bit 12 of 0x17. Run it for both levels with a hard iteration cap:
     * a storage model does not terminate for level 1, so the cap is what turns
     * "hangs the boot" into "fails this test".
     */
    for (unsigned level = 0; level <= 1u; level++) {
        s5l_wm8991_reset(&codec);
        /* rmw(0x17, 0x1000, level << 12) — 0xc068d44c */
        uint16_t ctrl = bus_read16(&bus, WM8991_REG_GPCTRL, NULL);
        ctrl = (uint16_t)((ctrl & ~WM8991_GP_BIT) |
                          (level ? WM8991_GP_BIT : 0u));
        bus_write16(&bus, WM8991_REG_GPCTRL, ctrl);

        unsigned spins = 0;
        uint16_t v;
        do {
            uint16_t last = bus_read16(&bus, WM8991_REG_GPSTAT, NULL);
            bus_write16(&bus, WM8991_REG_GPSTAT, (uint16_t)(last & 0xefffu));
            v = bus_read16(&bus, WM8991_REG_GPSTAT, NULL);
        } while ((v & WM8991_GP_BIT) != (level << 12) && ++spins < 8u);
        CHECK(spins == 0u,
              "level %u poll took %u extra iterations (0 = first pass)",
              level, spins);
        CHECK((v & WM8991_GP_BIT) == (level << 12),
              "level %u: 0x12 bit 12 read %04x, does not track 0x17", level, v);
    }

    /* The mirror is exactly one bit wide in each direction. */
    s5l_wm8991_reset(&codec);
    bus_write16(&bus, WM8991_REG_GPCTRL, 0xffffu);
    CHECK(bus_read16(&bus, WM8991_REG_GPCTRL, NULL) == 0xffffu,
          "0x17 is not plain storage");
    bus_write16(&bus, WM8991_REG_GPSTAT, 0xffffu);
    CHECK(bus_read16(&bus, WM8991_REG_GPSTAT, NULL) == 0xffffu,
          "0x12 lost a bit other than the mirrored one");
    bus_write16(&bus, WM8991_REG_GPCTRL, 0x0000u);
    CHECK(bus_read16(&bus, WM8991_REG_GPSTAT, NULL) == (0xffffu & ~WM8991_GP_BIT),
          "clearing 0x17 bit 12 did not clear the mirrored bit in 0x12");
    CHECK(codec.status_mirror_reads > 0u, "mirrored reads were not counted");
    CHECK(codec.regs[WM8991_REG_GPSTAT] == (0xffffu & ~WM8991_GP_BIT),
          "the mirrored bit was stored rather than derived");
}

static void test_both_write_encodings_decode_by_length(void) {
    s5l_i2c_t bus; s5l_wm8991_t codec;
    setup(&bus, &codec);

    /* Wide: three bytes after the address, value MSB-first. */
    bus_write16(&bus, 0x2bu, 0xbeefu);
    CHECK(s5l_wm8991_peek(&codec, 0x2bu) == 0xbeefu,
          "wide write stored %04x", s5l_wm8991_peek(&codec, 0x2bu));
    CHECK(codec.wide_writes == 1u && codec.packed_writes == 0u,
          "wide write accounted as w=%llu p=%llu",
          (unsigned long long)codec.wide_writes,
          (unsigned long long)codec.packed_writes);

    /* Packed: two bytes, seven-bit register and nine-bit datum. Bit 8 of the
     * value rides in bit 0 of the first byte, so a model that took the first
     * byte as a plain index would store to the wrong register AND lose bit 8. */
    bus_write9(&bus, 0x3cu, 0x01a5u);
    CHECK(s5l_wm8991_peek(&codec, 0x3cu) == 0x01a5u,
          "packed write stored %04x at 0x3c", s5l_wm8991_peek(&codec, 0x3cu));
    CHECK(codec.packed_writes == 1u, "packed write not accounted");
    CHECK(s5l_wm8991_peek(&codec, 0x78u) == 0u,
          "packed first byte was mistaken for a raw register index");

    /* One byte is the read setup, not a store. */
    uint64_t writes = codec.reg_writes;
    uint8_t idx = 0x2bu;
    bus_write(&bus, &idx, 1u);
    CHECK(codec.reg_writes == writes,
          "a pointer-only write was committed as a register store");
    CHECK(s5l_wm8991_peek(&codec, 0x2bu) == 0xbeefu,
          "the read setup overwrote the register it was pointing at");

    /* A fourth byte is longer than any shipped encoding: NAK it and refuse the
     * transfer rather than storing a value from a form nothing sends. */
    uint8_t toolong[4] = { 0x2bu, 0x11u, 0x22u, 0x33u };
    s5l_i2c_write(&bus, I2C_STAT, I2C_STAT_MODE_MTX | I2C_STAT_ENABLE);
    s5l_i2c_write(&bus, I2C_DS, (uint32_t)WM8991_I2C_ADDR << 1);
    s5l_i2c_write(&bus, I2C_STAT, I2C_STAT_MODE_MTX |
                                       I2C_STAT_ENABLE | I2C_STAT_START);
    s5l_i2c_write(&bus, I2C_INT, I2C_INT_ALL);
    for (unsigned i = 0; i < 4u; i++) {
        s5l_i2c_write(&bus, I2C_DS, toolong[i]);
        s5l_i2c_write(&bus, I2C_CON, I2C_CON_ACKEN | I2C_CON_RESUME);
        s5l_i2c_write(&bus, I2C_INT, I2C_INT_ALL);
    }
    CHECK((s5l_i2c_read(&bus, I2C_STAT) & I2C_STAT_NAK) != 0u,
          "an over-long transfer was acknowledged");
    s5l_i2c_write(&bus, I2C_STAT, I2C_STAT_MODE_MRX | I2C_STAT_ENABLE);
    CHECK(codec.refused_writes == 1u,
          "over-long transfer refusal was not counted");
    CHECK(s5l_wm8991_peek(&codec, 0x2bu) == 0xbeefu,
          "an over-long transfer mutated a register");
}

static void test_pointer_survives_stop_and_reads_auto_increment(void) {
    s5l_i2c_t bus; s5l_wm8991_t codec;
    setup(&bus, &codec);
    bus_write16(&bus, 0x20u, 0x1122u);
    bus_write16(&bus, 0x21u, 0x3344u);

    /* Set the pointer in one transaction and read in the next — the stock
     * sequence. If the pointer did not survive STOP, every read would answer
     * register 0. */
    uint8_t idx = 0x20u;
    bus_write(&bus, &idx, 1u);
    s5l_i2c_write(&bus, I2C_CON, I2C_CON_ACKEN);
    s5l_i2c_write(&bus, I2C_DS, (uint32_t)WM8991_I2C_ADDR << 1);
    s5l_i2c_write(&bus, I2C_STAT, I2C_STAT_MODE_MRX |
                                       I2C_STAT_ENABLE | I2C_STAT_START);
    ack(&bus, I2C_INT_BYTE);
    uint8_t got[4];
    for (unsigned i = 0; i < 4u; i++) {
        s5l_i2c_write(&bus, I2C_CON,
                      (i + 1u < 4u ? I2C_CON_ACKEN : 0u) | I2C_CON_RESUME);
        ack(&bus, I2C_INT_BYTE);
        got[i] = (uint8_t)s5l_i2c_read(&bus, I2C_DS);
    }
    s5l_i2c_write(&bus, I2C_STAT, I2C_STAT_MODE_MRX | I2C_STAT_ENABLE);
    ack(&bus, I2C_INT_STOP);
    CHECK(got[0] == 0x11u && got[1] == 0x22u,
          "first register read %02x %02x, expect 11 22", got[0], got[1]);
    CHECK(got[2] == 0x33u && got[3] == 0x44u,
          "auto-increment gave %02x %02x, expect 33 44", got[2], got[3]);

    /* The index space is seven bits: 0x7f must wrap to 0x00 rather than run off
     * the register file. Register 0 answers its identity, which is also the
     * cheapest observable proof that the wrap landed at 0. */
    s5l_wm8991_reset(&codec);
    idx = 0x7fu;
    bus_write(&bus, &idx, 1u);
    s5l_i2c_write(&bus, I2C_CON, I2C_CON_ACKEN);
    s5l_i2c_write(&bus, I2C_DS, (uint32_t)WM8991_I2C_ADDR << 1);
    s5l_i2c_write(&bus, I2C_STAT, I2C_STAT_MODE_MRX |
                                       I2C_STAT_ENABLE | I2C_STAT_START);
    ack(&bus, I2C_INT_BYTE);
    for (unsigned i = 0; i < 4u; i++) {
        s5l_i2c_write(&bus, I2C_CON,
                      (i + 1u < 4u ? I2C_CON_ACKEN : 0u) | I2C_CON_RESUME);
        ack(&bus, I2C_INT_BYTE);
        got[i] = (uint8_t)s5l_i2c_read(&bus, I2C_DS);
    }
    s5l_i2c_write(&bus, I2C_STAT, I2C_STAT_MODE_MRX | I2C_STAT_ENABLE);
    ack(&bus, I2C_INT_STOP);
    CHECK(got[2] == 0x89u && got[3] == 0x90u,
          "seven-bit wrap gave %02x %02x, expect the identity 89 90",
          got[2], got[3]);

    /*
     * An index byte with the top bit set. The guest can send one — the wide
     * encoding carries a full byte — and the pointer it leaves behind is
     * snapshotted, so it has to land inside the seven-bit space at the moment
     * it is stored rather than at the moment it is used. Storing it raw made
     * the machine unable to checkpoint at all, which is a far worse failure
     * than the wrong register: the boot would run and the checkpoint would
     * refuse, thousands of instructions later and for no visible reason.
     */
    s5l_wm8991_reset(&codec);
    bus_write16(&bus, 0x7fu, 0xa5a5u);
    idx = 0xffu;
    bus_write(&bus, &idx, 1u);
    CHECK(codec.ptr == 0x7fu, "index byte 0xff left ptr=%02x, expect 7f",
          codec.ptr);
    s5l_i2c_write(&bus, I2C_CON, I2C_CON_ACKEN);
    s5l_i2c_write(&bus, I2C_DS, (uint32_t)WM8991_I2C_ADDR << 1);
    s5l_i2c_write(&bus, I2C_STAT, I2C_STAT_MODE_MRX |
                                       I2C_STAT_ENABLE | I2C_STAT_START);
    ack(&bus, I2C_INT_BYTE);
    for (unsigned i = 0; i < 2u; i++) {
        s5l_i2c_write(&bus, I2C_CON,
                      (i == 0u ? I2C_CON_ACKEN : 0u) | I2C_CON_RESUME);
        ack(&bus, I2C_INT_BYTE);
        got[i] = (uint8_t)s5l_i2c_read(&bus, I2C_DS);
    }
    s5l_i2c_write(&bus, I2C_STAT, I2C_STAT_MODE_MRX | I2C_STAT_ENABLE);
    ack(&bus, I2C_INT_STOP);
    CHECK(got[0] == 0xa5u && got[1] == 0xa5u,
          "index 0xff read %02x %02x, expect register 0x7f's a5 a5",
          got[0], got[1]);
}

static void test_unwritten_registers_are_visible_and_bounded(void) {
    s5l_i2c_t bus; s5l_wm8991_t codec;
    setup(&bus, &codec);

    for (unsigned i = 0; i < 20u; i++)
        (void)bus_read16(&bus, (uint8_t)(0x40u + i), NULL);
    CHECK(codec.unknown_reads == 20u,
          "unknown codec reads=%llu expect 20",
          (unsigned long long)codec.unknown_reads);
    CHECK(codec.unknown_reg_count == WM8991_UNKNOWN_REGS,
          "bounded unknown-register set grew to %u", codec.unknown_reg_count);

    uint64_t before = codec.unknown_reads;
    bus_write16(&bus, 0x40u, 0x0055u);
    CHECK(bus_read16(&bus, 0x40u, NULL) == 0x0055u,
          "written storage did not round-trip");
    CHECK(codec.unknown_reads == before,
          "a written register was still counted as unknown");

    CHECK(s5l_wm8991_peek(&codec, 0xffu) == 0u &&
          s5l_wm8991_peek(NULL, 0u) == 0u,
          "peek was not bounded against a bad index or NULL");
    s5l_wm8991_reset(NULL);
    s5l_wm8991_bind(&codec, NULL);
}

static void test_reset_is_total(void) {
    s5l_wm8991_t codec, expected;
    memset(&codec, 0xa5, sizeof codec);
    memset(&expected, 0, sizeof expected);
    s5l_wm8991_reset(&codec);
    CHECK(memcmp(&codec, &expected, sizeof codec) == 0,
          "reset did not totally initialize a poisoned object");
}

/* ------------------------------------------------------------- the I2S --- */

static void test_i2s_stores_the_seven_and_shows_the_rest(void) {
    s5l_i2s_t i2s, expected;
    memset(&i2s, 0x5a, sizeof i2s);
    memset(&expected, 0, sizeof expected);
    s5l_i2s_reset(&i2s);
    CHECK(memcmp(&i2s, &expected, sizeof i2s) == 0,
          "I2S reset did not totally initialize a poisoned object");

    /* The exact seven the driver writes, and only those, are storage. */
    static const uint32_t want[S5L_I2S_REGS] = {
        0x00u, 0x04u, 0x08u, 0x30u, 0x34u, 0x3cu, 0x40u
    };
    for (unsigned i = 0; i < S5L_I2S_REGS; i++)
        CHECK(s5l_i2s_offset(i) == want[i],
              "offset map slot %u = 0x%x, expect 0x%x",
              i, s5l_i2s_offset(i), want[i]);
    CHECK(s5l_i2s_offset(S5L_I2S_REGS) == UINT32_MAX,
          "offset map did not bound its index");

    for (unsigned i = 0; i < S5L_I2S_REGS; i++)
        s5l_i2s_write(&i2s, want[i], 0xa0000000u + i);
    for (unsigned i = 0; i < S5L_I2S_REGS; i++)
        CHECK(s5l_i2s_read(&i2s, want[i]) == 0xa0000000u + i,
              "offset 0x%x did not read back", want[i]);
    CHECK(i2s.unknown_reads == 0u && i2s.unknown_writes == 0u,
          "a known offset was classified unknown");

    /* The real configure() constants, so a reordering of the storage map is
     * caught rather than merely a missing entry. */
    s5l_i2s_reset(&i2s);
    s5l_i2s_write(&i2s, 0x08u, 6u);
    s5l_i2s_write(&i2s, 0x34u, 6u);
    s5l_i2s_write(&i2s, 0x3cu, 1u);
    CHECK(s5l_i2s_read(&i2s, 0x08u) == 6u && s5l_i2s_read(&i2s, 0x34u) == 6u &&
          s5l_i2s_read(&i2s, 0x3cu) == 1u,
          "startTransfer's constants did not land on their own offsets");

    /* The FIFOs at +0x10 and +0x38 belong to the PL080 and reach this window
     * only as physical addresses in a descriptor. A CPU access to either is a
     * real finding, so it must be counted and named, not stored. */
    (void)s5l_i2s_read(&i2s, 0x10u);
    (void)s5l_i2s_read(&i2s, 0x38u);
    s5l_i2s_write(&i2s, 0x10u, 0xdeadbeefu);
    CHECK(i2s.unknown_reads == 2u && i2s.unknown_writes == 1u,
          "FIFO traffic r=%llu w=%llu, expect 2/1",
          (unsigned long long)i2s.unknown_reads,
          (unsigned long long)i2s.unknown_writes);
    CHECK(i2s.unknown_off_count == 2u,
          "distinct unknown offsets=%u expect 2", i2s.unknown_off_count);
    CHECK(s5l_i2s_read(&i2s, 0x10u) == 0u,
          "an unmodelled offset fabricated a stored value");

    for (unsigned i = 0; i < 40u; i++) s5l_i2s_write(&i2s, 0x100u + i * 4u, i);
    CHECK(i2s.unknown_off_count == S5L_I2S_UNKNOWN_OFF,
          "the unknown-offset log grew past its bound (%u)",
          i2s.unknown_off_count);

    CHECK(s5l_i2s_read(NULL, 0u) == 0u, "NULL I2S read was unsafe");
    s5l_i2s_write(NULL, 0u, 0u);
    s5l_i2s_reset(NULL);
}

/* -------------------------------------------------------- the machine --- */

static void test_machine_routes_the_codec_and_both_windows(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0u, 1u << 20), "machine init failed");

    s5l_window_t windows[S5L_WINDOW_MAX];
    unsigned count = s5l8900_windows(&m, windows, S5L_WINDOW_MAX);
    unsigned seen0 = 0, seen1 = 0;
    for (unsigned i = 0; i < count && i < S5L_WINDOW_MAX; i++) {
        if (windows[i].base == S5L8900_I2S0_BASE &&
            windows[i].size == S5L8900_DEV_SIZE) seen0++;
        if (windows[i].base == S5L8900_I2S1_BASE &&
            windows[i].size == S5L8900_DEV_SIZE) seen1++;
    }
    CHECK(seen0 == 1u && seen1 == 1u,
          "fixed window map has i2s0=%u i2s1=%u", seen0, seen1);
    CHECK(!s5l8900_add_stub(&m, S5L8900_I2S0_BASE, S5L8900_DEV_SIZE, "shadow") &&
          !s5l8900_add_stub(&m, S5L8900_I2S1_BASE, S5L8900_DEV_SIZE, "shadow"),
          "a stub was allowed to shadow an I2S window");

    /* Both slaves live on i2c0 and nothing landed on i2c1. */
    CHECK(m.i2c[0].slave_count == 2u &&
          m.i2c[0].slaves[0].addr == PCF50635_I2C_ADDR &&
          m.i2c[0].slaves[1].addr == WM8991_I2C_ADDR &&
          m.i2c[1].slave_count == 0u,
          "i2c0 wiring is wrong (count=%u)", m.i2c[0].slave_count);

    /* Drive the identity read through the real bus and the real window. */
    uint16_t id = bus_read16(&m.i2c[0], WM8991_REG_ID, NULL);
    CHECK(id == 0x8990u, "identity through the machine read %04x", id);
    CHECK(m.codec.id_reads == 1u, "machine-level identity read not counted");

    uint64_t ur = m.unmapped_reads, uw = m.unmapped_writes;
    m.bus.write32(m.bus.ctx, S5L8900_I2S0_BASE + 0x08u, 6u);
    m.bus.write32(m.bus.ctx, S5L8900_I2S1_BASE + 0x34u, 6u);
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_I2S0_BASE + 0x08u) == 6u &&
          m.bus.read32(m.bus.ctx, S5L8900_I2S1_BASE + 0x34u) == 6u,
          "I2S windows did not route to their own controllers");
    CHECK(m.i2s[0].regs[2] == 6u && m.i2s[1].regs[4] == 6u,
          "I2S writes crossed between controllers");
    CHECK(m.unmapped_reads == ur && m.unmapped_writes == uw,
          "valid I2S MMIO was classified unmapped");

    /* Word accesses only, exactly as for the I2C controllers. */
    s5l_i2s_t before = m.i2s[0];
    m.bus.write8(m.bus.ctx, S5L8900_I2S0_BASE + 0x08u, 0xaau);
    m.bus.write16(m.bus.ctx, S5L8900_I2S0_BASE + 0x08u, 0xbbccu);
    m.bus.write32(m.bus.ctx, S5L8900_I2S0_BASE + 1u, 0xdeadbeefu);
    (void)m.bus.read16(m.bus.ctx, S5L8900_I2S0_BASE + 0x08u);
    CHECK(memcmp(&before, &m.i2s[0], sizeof before) == 0,
          "an invalid-width or unaligned access mutated the I2S window");
    CHECK(m.unmapped_writes == uw + 3u && m.unmapped_reads == ur + 1u,
          "malformed I2S MMIO counts r=%llu w=%llu",
          (unsigned long long)(m.unmapped_reads - ur),
          (unsigned long long)(m.unmapped_writes - uw));

    s5l8900_free(&m);
}

static void test_snapshot_carries_the_codec_and_windows(void) {
    s5l8900_t src, dst;
    CHECK(s5l8900_init(&src, 0u, 1u << 16), "source init failed");
    CHECK(s5l8900_init(&dst, 0u, 1u << 16), "destination init failed");

    bus_write16(&src.i2c[0], 0x2bu, 0x2d5eu);
    src.bus.write32(src.bus.ctx, S5L8900_I2S0_BASE + 0x40u, 0x12345678u);
    src.bus.write32(src.bus.ctx, S5L8900_I2S1_BASE + 0x30u, 0x9abcdef0u);

    /* Leave a read genuinely in flight: pointer set, address phase done, MSB
     * already delivered. This is the state a checkpoint can land in, because
     * the stock controller splits the pointer and the data across two
     * transactions and sleeps inside the second. */
    uint8_t idx = 0x2bu;
    bus_write(&src.i2c[0], &idx, 1u);
    s5l_i2c_write(&src.i2c[0], I2C_CON, I2C_CON_ACKEN);
    s5l_i2c_write(&src.i2c[0], I2C_DS, (uint32_t)WM8991_I2C_ADDR << 1);
    s5l_i2c_write(&src.i2c[0], I2C_STAT, I2C_STAT_MODE_MRX |
                                              I2C_STAT_ENABLE | I2C_STAT_START);
    s5l_i2c_write(&src.i2c[0], I2C_INT, I2C_INT_ALL);
    s5l_i2c_write(&src.i2c[0], I2C_CON, I2C_CON_ACKEN | I2C_CON_RESUME);
    CHECK((uint8_t)s5l_i2c_read(&src.i2c[0], I2C_DS) == 0x2du,
          "the in-flight read did not deliver the MSB first");
    s5l_i2c_write(&src.i2c[0], I2C_INT, I2C_INT_ALL);

    uint8_t *snap = NULL; size_t len = 0;
    CHECK(snapshot_save_mem(&src, &snap, &len) == SNAP_OK,
          "could not save a mid-read snapshot");
    CHECK(snapshot_load_mem(&dst, snap, len) == SNAP_OK,
          "could not restore a mid-read snapshot");
    CHECK(dst.i2c[0].slaves[1].ctx == &dst.codec &&
          dst.i2c[0].slaves[1].ctx != &src.codec,
          "restore copied the source codec's callback context");
    CHECK(dst.i2s[0].regs[6] == 0x12345678u && dst.i2s[1].regs[3] == 0x9abcdef0u,
          "I2S window contents did not survive the round trip");
    CHECK(s5l_wm8991_peek(&dst.codec, 0x2bu) == 0x2d5eu,
          "codec storage did not survive the round trip");

    /* Resume the second half in the DESTINATION: it must yield the LSB, 0x5e,
     * which is only true if BOTH `second_byte` and `latch` crossed the
     * checkpoint. A file carrying neither would restart at the MSB and hand the
     * driver 0x2d twice; one carrying `second_byte` alone would read the
     * register again, which is right here only because nothing moved. */
    s5l_i2c_write(&dst.i2c[0], I2C_CON, I2C_CON_RESUME);
    CHECK((uint8_t)s5l_i2c_read(&dst.i2c[0], I2C_DS) == 0x5eu,
          "the restored read resumed at the wrong half of the register");
    CHECK(dst.codec.latch == 0x2d5eu,
          "the restored latch held %04x", dst.codec.latch);
    CHECK(dst.codec.second_byte == false,
          "the restored transfer did not complete its register");

    free(snap);
    s5l8900_free(&src);
    s5l8900_free(&dst);
}

static void test_snapshot_rejects_impossible_codec_state(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0u, 1u << 16), "machine init failed");
    uint8_t *out = NULL; size_t out_len = 0;

    m.codec.regs[WM8991_REG_PWR1] = WM8991_PWR1_PROBE;
    CHECK(snapshot_save_mem(&m, &out, &out_len) == SNAP_ERR_CORRUPT,
          "a stored R1 bit 5 was snapshotted: the part would become a WM1817");
    m.codec.regs[WM8991_REG_PWR1] = 0u;

    m.codec.regs[WM8991_REG_GPSTAT] = WM8991_GP_BIT;
    CHECK(snapshot_save_mem(&m, &out, &out_len) == SNAP_ERR_CORRUPT,
          "a stored status-mirror bit was snapshotted");
    m.codec.regs[WM8991_REG_GPSTAT] = 0u;

    m.codec.regs[WM8991_REG_ID] = WM8991_ID_VALUE;
    CHECK(snapshot_save_mem(&m, &out, &out_len) == SNAP_ERR_CORRUPT,
          "the identity register was snapshotted as storage");
    m.codec.regs[WM8991_REG_ID] = 0u;

    m.codec.wlen = WM8991_MAX_WRITE + 1u;
    CHECK(snapshot_save_mem(&m, &out, &out_len) == SNAP_ERR_CORRUPT,
          "an over-long pending write was snapshotted");
    m.codec.wlen = 0u;

    m.codec.written[3] = 2u;
    CHECK(snapshot_save_mem(&m, &out, &out_len) == SNAP_ERR_CORRUPT,
          "a non-boolean written marker was snapshotted");
    m.codec.written[3] = 0u;

    m.codec.unknown_reg_count = WM8991_UNKNOWN_REGS + 1u;
    CHECK(snapshot_save_mem(&m, &out, &out_len) == SNAP_ERR_CORRUPT,
          "an overflowed unknown-register count was snapshotted");
    m.codec.unknown_reg_count = 0u;

    m.i2s[1].unknown_off_count = S5L_I2S_UNKNOWN_OFF + 1u;
    CHECK(snapshot_save_mem(&m, &out, &out_len) == SNAP_ERR_CORRUPT,
          "an overflowed I2S unknown-offset count was snapshotted");
    CHECK(out == NULL && out_len == 0u,
          "a failed snapshot returned an allocation");
    m.i2s[1].unknown_off_count = 0u;

    CHECK(snapshot_save_mem(&m, &out, &out_len) == SNAP_OK,
          "a clean machine failed to snapshot");
    free(out);
    s5l8900_free(&m);
}

int main(void) {
    printf("S5LBox WM8991 codec / I2S window tests\n");
    test_constants_match_the_shipped_firmware();
    test_identity_is_the_hard_gate();
    test_probe_bit_selects_wm8991_not_wm1817();
    test_status_bit_mirrors_control_so_the_poll_terminates();
    test_both_write_encodings_decode_by_length();
    test_pointer_survives_stop_and_reads_auto_increment();
    test_unwritten_registers_are_visible_and_bounded();
    test_reset_is_total();
    test_i2s_stores_the_seven_and_shows_the_rest();
    test_machine_routes_the_codec_and_both_windows();
    test_snapshot_carries_the_codec_and_windows();
    test_snapshot_rejects_impossible_codec_state();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
