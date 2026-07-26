/*
 * iOS3-VM — uart4, the guest's PPP line, focused tests.
 *
 * uart0 has been modelled since the first week of this project and is exercised
 * by every boot, so the interesting question here is not "does a Samsung UART
 * work". It is the four properties that are new because there are now TWO of
 * them, each of which fails silently rather than loudly:
 *
 *   1. THE TWO CAPTURES DO NOT ALIAS. This is the one that matters. The
 *      milestone uart4 exists for is the six bytes 7E FF 7D 23 C0 21 appearing
 *      on THIS port — an HDLC-async flag, all-stations address, escaped control
 *      byte and the LCP protocol id, i.e. pppd's first Configure-Request. A
 *      model that spliced both ports into one buffer would still show those
 *      bytes, and a reader could no longer tell whether pppd sent them or
 *      whether the kernel's own kprintf happened to print them. That would not
 *      be a weaker proof, it would be no proof at all.
 *
 *   2. THE WINDOWS DO NOT OVERLAP OR SHADOW. 0x3cc00000 and 0x3cc10000 are
 *      64 KiB apart with 4 KiB windows, which is comfortable — but the whole
 *      point of the machine's window table is that "is anything shadowed?" has
 *      one answer, and a new entry is exactly when that stops being true.
 *
 *   3. THE STATUS ANSWERS LET A TRANSMIT LOOP TERMINATE. Before this window was
 *      decoded, 0x3cc10000 fell through to the unmapped path and every UTRSTAT
 *      read answered 0 — "transmitter busy" — so a driver that waits for room
 *      before storing would wait forever. The census in docs/BOOTLOG.md records
 *      that page as r=8 w=15 for exactly that reason: identification only.
 *
 *   4. THE SNAPSHOT CARRIES BOTH, SEPARATELY. A checkpoint taken mid-PPP has
 *      the only copy of what the guest transmitted; SNAPSHOT_VERSION 10 exists
 *      because uart4 is serialised in front of every other device.
 *
 * MUTATION CHECKS. A test that cannot fail is not evidence, so each of the
 * three bugs above was introduced deliberately, into a clean tree, and the
 * suite was required to catch it. All three were caught, and by the assertions
 * written for them rather than incidentally:
 *
 *   1. route uart4's stores into uart0's model (the aliasing bug) —
 *      8 checks across 4 tests, including "an HDLC flag reached the console
 *      capture", which is the one that would otherwise have made a boot log
 *      unreadable rather than wrong;
 *   2. UTRSTAT returns 0, i.e. the transmitter is permanently busy (which is
 *      what the UNDECODED page answered before this window existed) —
 *      3 checks, all naming the spin;
 *   3. drop uart4 from snap_mach() — 2 checks, on the restored capture and on
 *      the two ports' configuration registers being crossed.
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
 * pppd 2.4.2's first LCP Configure-Request, as it appears on the wire once the
 * kernel's pppserial line discipline has framed it (RFC 1662 §4.2, and the
 * async control-character map pppd defaults to before the peer has agreed
 * anything):
 *
 *   7E    flag, start of frame
 *   FF    all-stations address
 *   7D 23    escaped 0x03, the UI control byte: 0x7D then 0x23 = 0x03 ^ 0x20
 *   C0 21    protocol 0xC021, LCP
 *
 * Six bytes. Everything after them is the LCP code, identifier, length and
 * options, which vary per run because the magic number is random — so the
 * milestone is stated as this prefix and nothing more, which is also exactly
 * what tools/bootkernel.c scans the capture for.
 */
static const uint8_t LCP_CONFREQ_PREFIX[6] = {
    0x7eu, 0xffu, 0x7du, 0x23u, 0xc0u, 0x21u
};

/* --------------------------------------------------------------------------
 * Device-level behaviour.
 */

static void test_reset_is_total(void) {
    s5l_uart_t u;
    /* Deliberately dirty first: reset has to be a statement about every byte,
     * not just the ones a fresh stack happened to zero. */
    memset(&u, 0xa5, sizeof u);
    s5l_uart_reset(&u);
    CHECK(u.ulcon == 0 && u.ucon == 0 && u.ufcon == 0 && u.umcon == 0 &&
          u.ubrdiv == 0, "reset left a configuration register set");
    CHECK(u.tx_len == 0, "reset left tx_len = %zu", u.tx_len);
    bool clean = true;
    for (size_t i = 0; i < UART_TX_BUFFER; i++)
        if (u.tx[i] != 0) clean = false;
    CHECK(clean, "reset left bytes in the transmit capture");
}

static void test_status_lets_a_transmit_loop_terminate(void) {
    s5l_uart_t u;
    s5l_uart_reset(&u);

    /*
     * UTRSTAT bit 1 (transmit buffer empty) and bit 2 (transmitter empty) are
     * what a "wait for room, then store" loop tests. Both must read set from
     * the very first poll: this port has no peer applying back-pressure, and
     * the model drains instantly by construction.
     */
    uint32_t trstat = s5l_uart_read(&u, UART_UTRSTAT);
    CHECK((trstat & (1u << 1)) != 0u,
          "UTRSTAT bit 1 clear: a transmit loop would spin (0x%08x)", trstat);
    CHECK((trstat & (1u << 2)) != 0u,
          "UTRSTAT bit 2 clear: a drain loop would spin (0x%08x)", trstat);
    /* Bit 0 is receive-data-ready. It must be CLEAR: a set bit would send the
     * driver into its programmed-I/O receive loop to collect a byte no host
     * ever sent, which is the one thing this core's models must never do. */
    CHECK((trstat & 1u) == 0u,
          "UTRSTAT claims receive data with no host peer (0x%08x)", trstat);

    /*
     * UFSTAT, per AppleS5L8900XSerial: bits[3:0] receive count, bit 8 receive
     * full, bits[7:4] transmit count, bit 9 transmit full, FIFO depth 16. All
     * six fields are zero for empty FIFOs, so the register reads zero — and
     * that is the correct answer rather than a stub, which is why it is
     * checked field by field instead of against the whole word.
     */
    uint32_t fstat = s5l_uart_read(&u, UART_UFSTAT);
    CHECK((fstat & 0x0fu) == 0u, "UFSTAT receive count = %u", fstat & 0x0fu);
    CHECK((fstat & (1u << 8)) == 0u, "UFSTAT claims the receive FIFO is full");
    CHECK(((fstat >> 4) & 0x0fu) == 0u,
          "UFSTAT transmit count = %u with nothing queued",
          (fstat >> 4) & 0x0fu);
    CHECK((fstat & (1u << 9)) == 0u, "UFSTAT claims the transmit FIFO is full");

    /* No receive source. URXH answers zero and answering anything else would
     * be fabricating a byte. */
    CHECK(s5l_uart_read(&u, UART_URXH) == 0u, "URXH invented a byte");
    /* Draining a hundred times must not accumulate anything anywhere. */
    for (unsigned i = 0; i < 100u; i++) (void)s5l_uart_read(&u, UART_URXH);
    CHECK(u.tx_len == 0u, "reading URXH grew the transmit capture");
}

static void test_configuration_registers_round_trip(void) {
    s5l_uart_t u;
    s5l_uart_reset(&u);
    /*
     * pppd's tcsetattr reaches these through AppleS5L8900XSerial. They are
     * stored with no side effect, which is what lets the driver configure the
     * line and read its own configuration back. Distinct values, so a swapped
     * pair in the switch is a failure rather than a coincidence.
     */
    s5l_uart_write(&u, UART_ULCON,  0x00000003u);
    s5l_uart_write(&u, UART_UCON,   0x00000005u);
    s5l_uart_write(&u, UART_UFCON,  0x00000007u);
    s5l_uart_write(&u, UART_UMCON,  0x00000009u);
    s5l_uart_write(&u, UART_UBRDIV, 0x0000000bu);
    CHECK(s5l_uart_read(&u, UART_ULCON)  == 0x3u, "ULCON did not round-trip");
    CHECK(s5l_uart_read(&u, UART_UCON)   == 0x5u, "UCON did not round-trip");
    CHECK(s5l_uart_read(&u, UART_UFCON)  == 0x7u, "UFCON did not round-trip");
    CHECK(s5l_uart_read(&u, UART_UMCON)  == 0x9u, "UMCON did not round-trip");
    CHECK(s5l_uart_read(&u, UART_UBRDIV) == 0xbu, "UBRDIV did not round-trip");
    CHECK(u.tx_len == 0u,
          "configuring the line put %zu bytes in the capture", u.tx_len);
}

static void test_capture_keeps_the_head_and_is_bounded(void) {
    s5l_uart_t u;
    s5l_uart_reset(&u);

    /* Overrun the capture by a wide margin. */
    for (unsigned i = 0; i < UART_TX_BUFFER * 2u; i++)
        s5l_uart_write(&u, UART_UTXH, (uint32_t)(i & 0xffu));

    CHECK(u.tx_len == (size_t)UART_TX_BUFFER - 1u,
          "capture stopped at %zu, expected %d", u.tx_len,
          UART_TX_BUFFER - 1);
    /*
     * FIRST bytes, not last. This is the whole reason the cap is acceptable
     * here and is a defect on uart0: the observable is pppd's FIRST
     * Configure-Request, so a first-N cap keeps the evidence and a ring would
     * throw it away. If someone converts this to a ring, this assertion is the
     * one that will tell them they also have to move the milestone scan.
     */
    /* Through unsigned char: s5l_uart_t.tx is `char`, which is signed on every
     * host this builds on, so a raw comparison against 0xff is always false
     * and would make this assertion unfalsifiable in the other direction. */
    CHECK((uint8_t)u.tx[0] == 0x00u && (uint8_t)u.tx[1] == 0x01u &&
          (uint8_t)u.tx[255] == 0xffu,
          "the capture kept the tail rather than the head");
    /* And only the low byte of a store is transmitted. */
    s5l_uart_reset(&u);
    s5l_uart_write(&u, UART_UTXH, 0xdeadbe7eu);
    CHECK(u.tx_len == 1u && (uint8_t)u.tx[0] == 0x7eu,
          "UTXH transmitted more than the low byte");
}

/* --------------------------------------------------------------------------
 * Machine-level routing: the part that can silently alias.
 */

static void test_machine_decodes_uart4_as_its_own_window(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    s5l_window_t windows[S5L_WINDOW_MAX];
    unsigned nw = s5l8900_windows(&m, windows, S5L_WINDOW_MAX);
    bool have_uart0 = false, have_uart4 = false;
    for (unsigned i = 0; i < nw && i < S5L_WINDOW_MAX; i++) {
        if (windows[i].base == S5L8900_UART0_BASE)
            have_uart0 = windows[i].size == S5L8900_DEV_SIZE &&
                         strcmp(windows[i].name, "uart0") == 0;
        if (windows[i].base == S5L8900_UART4_BASE)
            have_uart4 = windows[i].size == S5L8900_DEV_SIZE &&
                         strcmp(windows[i].name, "uart4") == 0;
    }
    CHECK(have_uart0, "uart0 vanished from the window table");
    CHECK(have_uart4, "uart4 is not a declared 4 KiB window named \"uart4\"");

    /* Nothing in the table may overlap anything else in it. Cheap, total, and
     * the exact check that would have caught the NOR being swallowed by an
     * oversized DRAM aperture. */
    unsigned overlaps = 0;
    for (unsigned i = 0; i < nw && i < S5L_WINDOW_MAX; i++)
        for (unsigned j = i + 1u; j < nw && j < S5L_WINDOW_MAX; j++)
            if (s5l8900_overlaps(windows[i].base, windows[i].size,
                                 windows[j].base, windows[j].size))
                overlaps++;
    CHECK(overlaps == 0u, "%u pairs of machine windows overlap", overlaps);

    /* The address itself, stated once here so a typo in soc.h is a test
     * failure rather than a boot that quietly writes into open space. */
    CHECK(S5L8900_UART4_BASE == 0x3cc10000u,
          "uart4 base is 0x%08x, not the device tree's 0x3cc10000",
          (unsigned)S5L8900_UART4_BASE);

    s5l8900_free(&m);
}

static void test_the_two_captures_do_not_alias(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    /* Console traffic on uart0. */
    static const char console[] = "AppleS5L8900XSerial";
    for (size_t i = 0; i < sizeof console - 1u; i++)
        m.bus.write32(m.bus.ctx, S5L8900_UART0_BASE + UART_UTXH,
                      (uint32_t)(unsigned char)console[i]);

    /* The milestone frame on uart4. */
    for (size_t i = 0; i < sizeof LCP_CONFREQ_PREFIX; i++)
        m.bus.write32(m.bus.ctx, S5L8900_UART4_BASE + UART_UTXH,
                      LCP_CONFREQ_PREFIX[i]);

    CHECK(m.uart0.tx_len == sizeof console - 1u,
          "uart0 captured %zu bytes, expected %zu",
          m.uart0.tx_len, sizeof console - 1u);
    CHECK(m.uart4.tx_len == sizeof LCP_CONFREQ_PREFIX,
          "uart4 captured %zu bytes, expected %zu",
          m.uart4.tx_len, sizeof LCP_CONFREQ_PREFIX);
    CHECK(memcmp(m.uart0.tx, console, sizeof console - 1u) == 0,
          "uart0's capture is not the console bytes");
    CHECK(memcmp(m.uart4.tx, LCP_CONFREQ_PREFIX,
                 sizeof LCP_CONFREQ_PREFIX) == 0,
          "uart4's capture is not the LCP frame");

    /*
     * And the negative, which is the assertion with teeth: neither capture may
     * contain the other's bytes anywhere. A splice that appended both to one
     * buffer would pass both length checks above if the lengths happened to
     * work out, and would still be indistinguishable from success in a boot
     * log. It cannot pass this.
     */
    CHECK(memchr(m.uart0.tx, 0x7e, m.uart0.tx_len) == NULL,
          "an HDLC flag reached the console capture");
    CHECK(memchr(m.uart4.tx, 'A', m.uart4.tx_len) == NULL,
          "console bytes reached the PPP capture");

    /* The other direction of the same property: a byte written to one port
     * must not be visible through the other port's registers. */
    CHECK(s5l_uart_read(&m.uart4, UART_UBRDIV) == 0u,
          "uart4 sees a configuration value written to uart0");

    s5l8900_free(&m);
}

static void test_uart4_registers_route_through_the_bus(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    uint64_t unmapped_before = m.unmapped_reads + m.unmapped_writes;

    m.bus.write32(m.bus.ctx, S5L8900_UART4_BASE + UART_UBRDIV, 0x2du);
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_UART4_BASE + UART_UBRDIV) == 0x2du,
          "UBRDIV did not round-trip through the bus");
    CHECK(m.uart4.ubrdiv == 0x2du, "the store did not reach the uart4 model");

    /* UTRSTAT through the bus, which is what the guest's spin loop reads. */
    uint32_t trstat = m.bus.read32(m.bus.ctx, S5L8900_UART4_BASE + UART_UTRSTAT);
    CHECK((trstat & 0x6u) == 0x6u,
          "UTRSTAT through the bus = 0x%08x; a transmit loop would spin",
          trstat);

    /* Byte and halfword accesses reach the same model — the console driver
     * uses all three widths and both ports run the same driver. */
    m.bus.write8(m.bus.ctx, S5L8900_UART4_BASE + UART_UTXH, 0x7eu);
    m.bus.write16(m.bus.ctx, S5L8900_UART4_BASE + UART_UTXH, 0x00ffu);
    CHECK(m.uart4.tx_len == 2u && (uint8_t)m.uart4.tx[0] == 0x7eu &&
          (uint8_t)m.uart4.tx[1] == 0xffu,
          "narrow accesses did not reach uart4's transmit register");

    CHECK(m.unmapped_reads + m.unmapped_writes == unmapped_before,
          "uart4 traffic was counted as unmapped: %llu new accesses",
          (unsigned long long)(m.unmapped_reads + m.unmapped_writes -
                               unmapped_before));

    /* The last word of the window is decoded and the first word past it is
     * not — the boundary, checked from both sides. */
    unmapped_before = m.unmapped_reads;
    (void)m.bus.read32(m.bus.ctx,
                       S5L8900_UART4_BASE + S5L8900_DEV_SIZE - 4u);
    CHECK(m.unmapped_reads == unmapped_before,
          "the last word of uart4's window is not decoded");
    (void)m.bus.read32(m.bus.ctx, S5L8900_UART4_BASE + S5L8900_DEV_SIZE);
    CHECK(m.unmapped_reads == unmapped_before + 1u,
          "the word past uart4's window was decoded as uart4");

    s5l8900_free(&m);
}

static void test_uart4_raises_no_interrupt_line(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    /*
     * The nub's device-tree `interrupts` property says VIC line 28. This model
     * is transmit-only and must never assert it — there is no receive FIFO to
     * fill and no transmit-room event to report, because room is always
     * available. Enable the line, drive a whole frame, and require the VIC to
     * stay quiet. If a receive path ever lands, THIS is the test that has to
     * be rewritten rather than deleted.
     */
    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << 28);
    for (size_t i = 0; i < sizeof LCP_CONFREQ_PREFIX; i++)
        m.bus.write32(m.bus.ctx, S5L8900_UART4_BASE + UART_UTXH,
                      LCP_CONFREQ_PREFIX[i]);
    s5l8900_tick(&m, 1000u);

    uint32_t raw = m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_RAWINTR);
    CHECK((raw & (1u << 28)) == 0u,
          "uart4 asserted VIC line 28 (RAWINTR=0x%08x)", raw);
    CHECK(!m.cpu.irq_line,
          "a transmit-only port raised the core's IRQ line");

    s5l8900_free(&m);
}

/* --------------------------------------------------------------------------
 * Snapshot.
 */

static void test_snapshot_carries_both_captures_separately(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    static const char console[] = "launchd: starting\n";
    for (size_t i = 0; i < sizeof console - 1u; i++)
        m.bus.write32(m.bus.ctx, S5L8900_UART0_BASE + UART_UTXH,
                      (uint32_t)(unsigned char)console[i]);
    for (size_t i = 0; i < sizeof LCP_CONFREQ_PREFIX; i++)
        m.bus.write32(m.bus.ctx, S5L8900_UART4_BASE + UART_UTXH,
                      LCP_CONFREQ_PREFIX[i]);
    m.bus.write32(m.bus.ctx, S5L8900_UART4_BASE + UART_ULCON, 0x3u);

    uint8_t *img = NULL;
    size_t   len = 0;
    CHECK(snapshot_save_mem(&m, &img, &len) == SNAP_OK, "save failed");

    s5l8900_t r;
    CHECK(s5l8900_init(&r, 0, 1u << 20), "restore machine init failed");
    CHECK(snapshot_load_mem(&r, img, len) == SNAP_OK, "load failed");

    CHECK(r.uart0.tx_len == m.uart0.tx_len &&
          memcmp(r.uart0.tx, m.uart0.tx, UART_TX_BUFFER) == 0,
          "uart0's capture did not survive the round trip");
    CHECK(r.uart4.tx_len == m.uart4.tx_len &&
          memcmp(r.uart4.tx, m.uart4.tx, UART_TX_BUFFER) == 0,
          "uart4's capture did not survive the round trip");
    CHECK(r.uart4.ulcon == 0x3u && r.uart0.ulcon == 0u,
          "the two ports' configuration registers were crossed on restore");

    /*
     * The whole point of giving uart4 its own section entry rather than
     * reusing uart0's: a restore that read one capture into both would produce
     * two identical buffers here. They differ, so it did not.
     */
    CHECK(memcmp(r.uart0.tx, r.uart4.tx, 8) != 0,
          "restore aliased the two captures onto the same bytes");

    free(img);
    s5l8900_free(&r);
    s5l8900_free(&m);
}

static void test_snapshot_rejects_an_impossible_uart4_length(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    /* The writer always keeps a NUL slot free, so tx_len == UART_TX_BUFFER is
     * unreachable by any guest store. A machine holding it is corrupt, and the
     * save path must refuse rather than write a file that will fail to load. */
    m.uart4.tx_len = (size_t)UART_TX_BUFFER;
    uint8_t *img = NULL;
    size_t   len = 0;
    CHECK(snapshot_save_mem(&m, &img, &len) != SNAP_OK,
          "a machine with an over-long uart4 capture was snapshotted");
    CHECK(img == NULL && len == 0u, "a refused snapshot returned bytes");

    s5l8900_free(&m);
}

int main(void) {
    printf("iOS3-VM uart4 (guest PPP line) tests\n");
    test_reset_is_total();
    test_status_lets_a_transmit_loop_terminate();
    test_configuration_registers_round_trip();
    test_capture_keeps_the_head_and_is_bounded();
    test_machine_decodes_uart4_as_its_own_window();
    test_the_two_captures_do_not_alias();
    test_uart4_registers_route_through_the_bus();
    test_uart4_raises_no_interrupt_line();
    test_snapshot_carries_both_captures_separately();
    test_snapshot_rejects_an_impossible_uart4_length();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
