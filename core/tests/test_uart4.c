/*
 * S5LBox — uart4, the guest's PPP line, focused tests.
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
#include "ppp.h"
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

static void test_transmitting_alone_raises_no_interrupt_line(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    /*
     * The nub's device-tree `interrupts` property says VIC line 28, and this
     * model now CAN assert it — but only for a receive FIFO with something in
     * it. Transmitting must still raise nothing: room is always available, so
     * there is no transmit-room event, and this port's whole S0 milestone was
     * measured on runs where nothing asserted anything.
     *
     * This test predates the receive path and was rewritten rather than deleted
     * when it landed, which is what its original comment asked for. The claim
     * it makes is narrower now and more useful: not "this port cannot
     * interrupt" but "transmitting does not".
     */
    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << 28);
    for (size_t i = 0; i < sizeof LCP_CONFREQ_PREFIX; i++)
        m.bus.write32(m.bus.ctx, S5L8900_UART4_BASE + UART_UTXH,
                      LCP_CONFREQ_PREFIX[i]);
    s5l8900_tick(&m, 1000u);

    uint32_t raw = m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_RAWINTR);
    CHECK((raw & (1u << 28)) == 0u,
          "transmitting asserted VIC line 28 (RAWINTR=0x%08x)", raw);
    CHECK(!m.cpu.irq_line,
          "transmitting raised the core's IRQ line");

    s5l8900_free(&m);
}

/* --------------------------------------------------------------------------
 * The receive path. Nothing in core/ can produce a byte for it, which is the
 * property most of these cases are about: a run with no host peer must be
 * indistinguishable from the transmit-only model this port used to be.
 */

static void test_no_host_peer_means_no_receive_anything(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    /*
     * Run a machine hard — a whole frame out, a thousand ticks, the interrupt
     * line armed — and require every receive-side observable to be exactly
     * what the transmit-only model answered. This is the regression that
     * protects ~90 recorded runs in docs/BOOTLOG.md from being retroactively
     * described by a different machine.
     */
    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE,
                  (1u << 28) | (1u << 24));
    for (size_t i = 0; i < sizeof LCP_CONFREQ_PREFIX; i++)
        m.bus.write32(m.bus.ctx, S5L8900_UART4_BASE + UART_UTXH,
                      LCP_CONFREQ_PREFIX[i]);
    s5l8900_tick(&m, 1000u);

    uint32_t trstat = m.bus.read32(m.bus.ctx,
                                   S5L8900_UART4_BASE + UART_UTRSTAT);
    CHECK((trstat & 1u) == 0u,
          "UTRSTAT claims receive data with no host peer (0x%08x)", trstat);
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_UART4_BASE + UART_UFSTAT) == 0u,
          "UFSTAT is non-zero with both FIFOs empty");
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_UART4_BASE + UART_URXH) == 0u,
          "URXH invented a byte");
    uint32_t raw = m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_RAWINTR);
    CHECK((raw & ((1u << 28) | (1u << 24))) == 0u,
          "a UART asserted its line with an empty FIFO (RAWINTR=0x%08x)", raw);
    CHECK(!m.cpu.irq_line, "the core's IRQ line rose with no host peer");
    CHECK(m.uart4.rx_pushed == 0u && m.uart4.rx_dropped == 0u &&
          m.uart4.rx_reads == 0u,
          "something inside core/ pushed a byte into uart4's receive FIFO");
    CHECK(m.uart4.rx_underruns == 1u,
          "the URXH read of an empty FIFO was not counted (%llu)",
          (unsigned long long)m.uart4.rx_underruns);

    s5l8900_free(&m);
}

static void test_receive_fifo_is_ordered_and_bounded(void) {
    s5l_uart_t u;
    s5l_uart_reset(&u);

    CHECK(s5l_uart_rx_space(&u) == UART_RX_FIFO,
          "a reset port has %u bytes of receive room, expected %u",
          s5l_uart_rx_space(&u), (unsigned)UART_RX_FIFO);

    /* Fill it exactly. */
    for (unsigned i = 0; i < UART_RX_FIFO; i++)
        CHECK(s5l_uart_rx_push(&u, (uint8_t)(0x40u + i)),
              "push %u into an empty-enough FIFO was refused", i);
    CHECK(u.rx_pushed == UART_RX_FIFO, "rx_pushed = %llu",
          (unsigned long long)u.rx_pushed);
    CHECK(s5l_uart_rx_space(&u) == 0u, "a full FIFO reports room");

    /*
     * The seventeenth byte is REFUSED and counted, not stored, and not silently
     * overwriting the sixteenth. Back-pressure is the whole reason
     * s5l_uart_rx_push() returns a bool: a host that ignores it loses a frame,
     * and rx_dropped is what tells the operator afterwards.
     */
    CHECK(!s5l_uart_rx_push(&u, 0xffu), "a full FIFO accepted a byte");
    CHECK(u.rx_dropped == 1u, "the refused push was not counted (%llu)",
          (unsigned long long)u.rx_dropped);
    CHECK(u.rx_pushed == UART_RX_FIFO, "a refused push was counted as pushed");

    /* UFSTAT's split encoding: the count field is four bits and the depth is
     * sixteen, so a full FIFO is 0 in the field and 1 in the full bit. */
    uint32_t fstat = s5l_uart_read(&u, UART_UFSTAT);
    CHECK((fstat & 0x0fu) == 0u,
          "a full FIFO reports count %u in a four-bit field", fstat & 0x0fu);
    CHECK((fstat & (1u << 8)) != 0u, "a full FIFO does not set UFSTAT bit 8");
    CHECK((s5l_uart_read(&u, UART_UTRSTAT) & 1u) != 0u,
          "UTRSTAT bit 0 is clear with sixteen bytes waiting");

    /* Out in the order they went in. */
    bool ordered = true;
    for (unsigned i = 0; i < UART_RX_FIFO; i++)
        if (s5l_uart_read(&u, UART_URXH) != (uint32_t)(0x40u + i))
            ordered = false;
    CHECK(ordered, "the receive FIFO is not first-in first-out");
    CHECK(u.rx_reads == UART_RX_FIFO, "rx_reads = %llu",
          (unsigned long long)u.rx_reads);
    CHECK((s5l_uart_read(&u, UART_UTRSTAT) & 1u) == 0u,
          "UTRSTAT still claims data after the FIFO was drained");
    CHECK(u.rx_underruns == 0u,
          "draining exactly what was pushed counted an underrun");

    /*
     * The ring wraps. Push and pop past the end of the array many times over:
     * a head index that failed to wrap would start returning stale bytes here
     * and nowhere else.
     */
    bool wrapped_ok = true;
    for (unsigned i = 0; i < 200u; i++) {
        if (!s5l_uart_rx_push(&u, (uint8_t)i)) wrapped_ok = false;
        if (s5l_uart_read(&u, UART_URXH) != (uint32_t)(uint8_t)i)
            wrapped_ok = false;
    }
    CHECK(wrapped_ok, "the receive ring does not wrap correctly");

    /* Half-full, so the count field is exercised at a value that is neither 0
     * nor the full-bit special case. */
    for (unsigned i = 0; i < 5u; i++) (void)s5l_uart_rx_push(&u, 0x5au);
    fstat = s5l_uart_read(&u, UART_UFSTAT);
    CHECK((fstat & 0x0fu) == 5u, "UFSTAT reports %u of five queued bytes",
          fstat & 0x0fu);
    CHECK((fstat & (1u << 8)) == 0u, "a five-deep FIFO claims to be full");
    /* And the transmit half of UFSTAT stays zero throughout: this model drains
     * instantly, and a receive count leaking into the transmit field would send
     * the driver's flow control somewhere unpredictable. */
    CHECK(((fstat >> 4) & 0x0fu) == 0u && (fstat & (1u << 9)) == 0u,
          "receive state leaked into UFSTAT's transmit fields (0x%08x)", fstat);
}

static void test_a_utrstat_store_cannot_discard_a_queued_byte(void) {
    /*
     * AppleS5L8900XSerial's interrupt filter reads +0x10, masks, and writes the
     * result back (docs/AGENT_HANDOFF.md §23.5.1), so a real driver WILL store
     * a word with bit 0 set while a byte is still queued. If that store cleared
     * the ready bit, the byte would be stranded in the FIFO with nothing left to
     * announce it — the exact silent loss this model refuses.
     */
    s5l_uart_t u;
    s5l_uart_reset(&u);
    CHECK(s5l_uart_rx_push(&u, 0x7eu), "push failed");

    s5l_uart_write(&u, UART_UTRSTAT, 0xffffffffu);
    CHECK((s5l_uart_read(&u, UART_UTRSTAT) & 1u) != 0u,
          "a write-one-to-clear store dropped a byte still in the FIFO");
    CHECK(s5l_uart_read(&u, UART_URXH) == 0x7eu,
          "the queued byte did not survive the status store");
    CHECK((s5l_uart_read(&u, UART_UTRSTAT) & 1u) == 0u,
          "draining URXH did not clear the ready bit");
}

static void test_uart4_asserts_vic_line_28_for_a_waiting_byte(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << 28);
    s5l8900_tick(&m, 1u);
    CHECK(!m.cpu.irq_line, "arming the line alone raised an interrupt");

    /* A host peer delivers one byte between run slices, which is the only way
     * this can ever happen (docs/AGENT_HANDOFF.md §23.5.1). */
    CHECK(s5l_uart_rx_push(&m.uart4, 0xffu), "push into an empty FIFO failed");
    s5l8900_tick(&m, 1u);
    uint32_t raw = m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_RAWINTR);
    CHECK((raw & (1u << 28)) != 0u,
          "a queued byte did not assert VIC line 28 (RAWINTR=0x%08x)", raw);
    CHECK(m.cpu.irq_line, "line 28 was asserted but the core saw no IRQ");

    /* The guest's driver drains it through the bus, and the LEVEL drops. Level
     * and not edge: core/src/soc/vic.c's own reason — a source that re-raised
     * inside the handler would lose the interrupt. */
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_UART4_BASE + UART_URXH) == 0xffu,
          "the guest read the wrong byte back through the bus");
    s5l8900_tick(&m, 1u);
    raw = m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_RAWINTR);
    CHECK((raw & (1u << 28)) == 0u,
          "the line stayed asserted after the FIFO was drained (0x%08x)", raw);
    CHECK(!m.cpu.irq_line, "the core's IRQ line stayed high after the drain");

    /*
     * Two bytes, one read: the line must STAY asserted. A model that dropped it
     * after any read would strand the second byte, and a driver that reads one
     * byte per interrupt is the common shape.
     */
    CHECK(s5l_uart_rx_push(&m.uart4, 0x01u) &&
          s5l_uart_rx_push(&m.uart4, 0x02u), "pushes failed");
    (void)m.bus.read32(m.bus.ctx, S5L8900_UART4_BASE + UART_URXH);
    s5l8900_tick(&m, 1u);
    raw = m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_RAWINTR);
    CHECK((raw & (1u << 28)) != 0u,
          "the line dropped with a byte still queued (0x%08x)", raw);

    s5l8900_free(&m);
}

static void test_suppression_withholds_the_line_and_nothing_else(void) {
    /*
     * --no-uart4-rx-irq is a CONTROL, and a control is only worth running if it
     * changes exactly one thing. This pins that: with the interrupt suppressed,
     * every other observable of the receive path must be bit for bit what it is
     * with the interrupt on, and only VIC line 28 may differ. If this test ever
     * fails, the control run stops being evidence about the interrupt and
     * becomes evidence about whatever else drifted.
     */
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");
    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << 28);

    s5l_uart_set_rx_irq(&m.uart4, false);
    CHECK(s5l_uart_rx_push(&m.uart4, 0x7eu), "push into an empty FIFO failed");
    s5l8900_tick(&m, 1u);

    /* The line, and the core, stay down. */
    uint32_t raw = m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_RAWINTR);
    CHECK((raw & (1u << 28)) == 0u,
          "suppressed, but a queued byte still asserted line 28 (0x%08x)", raw);
    CHECK(!m.cpu.irq_line, "suppressed, but the core saw an IRQ");

    /* Everything the guest can actually observe is unchanged: the byte is in
     * the FIFO, UTRSTAT still says "receive data ready", UFSTAT still counts
     * it, and URXH still hands it over. */
    CHECK(s5l_uart_rx_irq(&m.uart4) == false, "s5l_uart_rx_irq() ignored it");
    uint32_t tr = m.bus.read32(m.bus.ctx, S5L8900_UART4_BASE + UART_UTRSTAT);
    CHECK((tr & 1u) != 0u,
          "suppression changed UTRSTAT bit 0 (0x%08x) -- it must not", tr);
    uint32_t fs = m.bus.read32(m.bus.ctx, S5L8900_UART4_BASE + UART_UFSTAT);
    CHECK((fs & 0x0fu) == 1u,
          "suppression changed UFSTAT's count (0x%08x) -- it must not", fs);
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_UART4_BASE + UART_URXH) == 0x7eu,
          "suppression changed what URXH dequeued");
    CHECK(m.uart4.rx_reads == 1u && m.uart4.rx_pushed == 1u &&
          m.uart4.rx_dropped == 0u && m.uart4.rx_underruns == 0u,
          "suppression disturbed the receive counters");

    /* And it is reversible in the same process, so a harness cannot get stuck
     * in the control configuration. */
    s5l_uart_set_rx_irq(&m.uart4, true);
    CHECK(s5l_uart_rx_push(&m.uart4, 0x5eu), "push failed after re-enable");
    s5l8900_tick(&m, 1u);
    raw = m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_RAWINTR);
    CHECK((raw & (1u << 28)) != 0u,
          "re-enabling did not restore line 28 (0x%08x)", raw);

    s5l8900_free(&m);
}

static void test_suppression_defaults_off_and_reset_clears_it(void) {
    /*
     * The default is what makes this flag safe to land: a port nobody has
     * spoken to asserts, so a run that never names --no-uart4-rx-irq is byte
     * for byte the run it was before the flag existed.
     *
     * And s5l_uart_reset() CLEARS the policy rather than preserving it. That is
     * deliberate and was the second attempt: preserving it meant reading a
     * field before the memset, and reset's only caller is s5l8900_init(), which
     * runs on storage that has not been initialised yet -- so the read was of
     * uninitialised memory, and it also falsified test_reset_is_total's claim
     * that reset is a statement about every byte. The consequence is an
     * ordering requirement, pinned here: set the flag AFTER init. If a second
     * reset caller ever appears it will clear the flag mid-run and a control
     * run will silently stop being one, which is why this asserts the clearing
     * out loud instead of leaving it to be discovered.
     */
    s5l_uart_t u;
    memset(&u, 0xa5, sizeof u);
    s5l_uart_reset(&u);
    CHECK(!u.rx_irq_suppressed, "a reset port defaulted to SUPPRESSED");
    CHECK(s5l_uart_rx_push(&u, 0x41u), "push failed");
    CHECK(s5l_uart_rx_irq(&u), "the default did not assert for a queued byte");

    s5l_uart_set_rx_irq(&u, false);
    CHECK(u.rx_irq_suppressed, "the setter did not record the suppression");
    CHECK(!s5l_uart_rx_irq(&u), "suppression did not take");

    s5l_uart_reset(&u);
    CHECK(!u.rx_irq_suppressed,
          "reset left the policy bit set -- reset must be total, and the flag "
          "must therefore be applied after s5l8900_init()");
    CHECK(s5l_uart_rx_push(&u, 0x42u), "push failed after reset");
    CHECK(s5l_uart_rx_irq(&u), "reset did not restore the default");

    /* And a machine's uart4 is in the default state straight out of init, so
     * bootkernel's apply-after-init ordering starts from a known place. */
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");
    CHECK(!m.uart4.rx_irq_suppressed && !m.uart0.rx_irq_suppressed,
          "a freshly initialised machine came up SUPPRESSED");
    s5l8900_free(&m);
}

static void test_uart0_receive_is_deliberately_not_wired(void) {
    /*
     * uart0 runs the identical model and has an identical FIFO. Its VIC line is
     * 24 and it is NOT connected, because uart0 is the kprintf console and
     * nothing in this project has any business injecting console input. This is
     * a decision, so it is asserted rather than left to be rediscovered.
     */
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");
    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << 24);
    CHECK(s5l_uart_rx_push(&m.uart0, 0x41u),
          "uart0's FIFO refused a byte; the model is shared and should accept");
    s5l8900_tick(&m, 1u);
    uint32_t raw = m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_RAWINTR);
    CHECK((raw & (1u << 24)) == 0u,
          "uart0's receive FIFO reached VIC line 24 (RAWINTR=0x%08x)", raw);
    /* The byte is still readable — the port works, it simply cannot interrupt. */
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_UART0_BASE + UART_URXH) == 0x41u,
          "uart0 did not deliver the byte a host pushed");
    /* And the two FIFOs are separate, exactly as the two captures are. */
    CHECK(m.uart4.rx_pushed == 0u,
          "a push into uart0 reached uart4's receive FIFO");

    s5l8900_free(&m);
}

static void test_uart4_rx_is_a_declared_wake_source(void) {
    /*
     * The wake-source table is this machine's definition of what can interrupt
     * it. uart4's receive line answers S5L_WAKE_NEVER — a host-delivered byte
     * has no schedule, so there is no future edge to name — but it must be IN
     * the table, because a source missing from it is a source the next reader
     * has to rediscover. See wake_edge_uart4() in core/src/soc/machine.c.
     */
    const s5l_wake_source_t *src = NULL;
    unsigned n = s5l8900_wake_sources(&src);
    bool found = false;
    for (unsigned i = 0; i < n; i++)
        if (src[i].line == S5L8900_IRQ_UART4 &&
            strcmp(src[i].name, "uart4-rx") == 0)
            found = true;
    CHECK(found, "uart4-rx is not a declared wake source");
    CHECK(S5L8900_IRQ_UART4 == 28u,
          "uart4's interrupt is line %u, not the device tree's 28",
          (unsigned)S5L8900_IRQ_UART4);
}

/*
 * The 47 octets run80's real guest transmitted on this port, transcribed from
 * work/run80-ppp-tty/uart4-ppp.bin. core/tests/test_ppp.c checks the same
 * transcription against the file itself and decodes it option by option; here
 * they are only a realistically shaped stimulus for the TRANSPORT.
 */
static const uint8_t RUN80_CAPTURE[47] = {
    0x7e, 0xff, 0x7d, 0x23, 0xc0, 0x21, 0x7d, 0x21,
    0x7d, 0x21, 0x7d, 0x20, 0x7d, 0x34, 0x7d, 0x22,
    0x7d, 0x26, 0x7d, 0x20, 0x7d, 0x20, 0x7d, 0x20,
    0x7d, 0x20, 0x7d, 0x25, 0x7d, 0x26, 0x79, 0x61,
    0xf5, 0x7d, 0x3c, 0x7d, 0x27, 0x7d, 0x22, 0x7d,
    0x28, 0x7d, 0x22, 0x51, 0x7d, 0x39, 0x7e
};

static void test_the_loop_closes_through_the_uart(void) {
    /*
     * THE SEAM NEITHER OTHER SUITE COVERS. test_ppp.c drives the peer with
     * arrays and never touches a register; the cases above drive the registers
     * and never touch the peer. What is left is the wiring in
     * tools/bootkernel.c — feed each transmitted octet into the peer, move what
     * the peer produces into a SIXTEEN-BYTE FIFO, let the guest drain it — and
     * the peer's first Configure-Request alone is more than twice that FIFO.
     * A pump that pushed without checking room, or a guest loop that read one
     * byte per interrupt, would both work perfectly on an infinite queue and
     * lose data here.
     *
     * The "guest" below is deliberately the awkward shape rather than the
     * convenient one: it stores one octet, then drains only what UTRSTAT says
     * is ready, one byte at a time, through the bus.
     */
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");
    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << 28);

    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);                       /* the guest's first octet does this */

    uint8_t  got[512];
    size_t   ngot = 0;
    unsigned irq_seen = 0;

    for (size_t i = 0; i < sizeof RUN80_CAPTURE + 400u; i++) {
        if (i < sizeof RUN80_CAPTURE) {
            /* The guest transmits. bootkernel's bus interposer tees the octet
             * into the peer and does nothing else — no machine state changes
             * from this direction. */
            m.bus.write32(m.bus.ctx, S5L8900_UART4_BASE + UART_UTXH,
                          RUN80_CAPTURE[i]);
            ppp_input_byte(&p, RUN80_CAPTURE[i]);
        }

        /* ppp_pump_step(), byte for byte. */
        unsigned room = s5l_uart_rx_space(&m.uart4);
        while (room--) {
            int b = ppp_output_byte(&p);
            if (b < 0) break;
            if (!s5l_uart_rx_push(&m.uart4, (uint8_t)b)) break;
        }
        s5l8900_tick(&m, 1u);

        uint32_t raw = m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_RAWINTR);
        if (raw & (1u << 28)) {
            irq_seen++;
            /* One byte per interrupt, which is the harshest realistic shape. */
            if ((m.bus.read32(m.bus.ctx, S5L8900_UART4_BASE + UART_UTRSTAT) & 1u)
                && ngot < sizeof got)
                got[ngot++] = (uint8_t)m.bus.read32(
                    m.bus.ctx, S5L8900_UART4_BASE + UART_URXH);
        }
    }

    CHECK(irq_seen > 0u, "the receive line never asserted across the whole run");
    CHECK(ngot > 0u, "nothing the peer produced ever reached the guest");
    /*
     * NOT ONE OCTET LOST, and none dropped by the FIFO: the pump asks for room
     * before it pushes, so back-pressure is a delay and never a loss.
     */
    CHECK(m.uart4.rx_dropped == 0u,
          "%llu octets were dropped by a FIFO the pump should never overfill",
          (unsigned long long)m.uart4.rx_dropped);
    CHECK(ngot == p.stats.tx_bytes,
          "the guest received %zu of the %llu octets the peer queued",
          ngot, (unsigned long long)p.stats.tx_bytes);
    CHECK(ppp_output_pending(&p) == 0u,
          "%zu octets are still stuck in the peer's ring",
          ppp_output_pending(&p));

    /*
     * And what arrived is two whole frames in order: the peer's own
     * Configure-Request, then its Configure-Ack for run80's. Checked by
     * structure rather than by decoding — test_ppp.c owns the decode — but the
     * flags and the two LCP codes are enough to say the FIFO did not reorder or
     * splice anything.
     */
    unsigned flags = 0;
    for (size_t i = 0; i < ngot; i++) if (got[i] == 0x7eu) flags++;
    CHECK(flags == 4u,
          "%u flag octets arrived; two whole frames carry four", flags);
    CHECK(ngot > 6u && got[0] == 0x7eu && got[1] == 0xffu &&
          got[2] == 0x7du && got[3] == 0x23u &&
          got[4] == 0xc0u && got[5] == 0x21u,
          "the first frame the guest received is not an HDLC-framed LCP packet");
    CHECK(got[ngot - 1u] == 0x7eu, "the last octet received is not a flag");

    /* The line is quiet again once everything has been read. */
    s5l8900_tick(&m, 1u);
    uint32_t raw = m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_RAWINTR);
    CHECK((raw & (1u << 28)) == 0u,
          "the receive line is still asserted with an empty FIFO (0x%08x)", raw);
    CHECK(m.uart4.rx_reads == ngot, "rx_reads (%llu) disagrees with %zu octets",
          (unsigned long long)m.uart4.rx_reads, ngot);

    /* The transmit capture is untouched by any of this: 47 octets out, and the
     * received stream must not have leaked into it. */
    CHECK(m.uart4.tx_len == sizeof RUN80_CAPTURE,
          "the transmit capture holds %zu octets, not 47", m.uart4.tx_len);
    CHECK(memcmp(m.uart4.tx, RUN80_CAPTURE, sizeof RUN80_CAPTURE) == 0,
          "the receive path corrupted the transmit capture");

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

static void test_snapshot_carries_the_receive_fifo(void) {
    /*
     * A checkpoint taken mid-negotiation holds bytes the host peer has ALREADY
     * transmitted and will never transmit again: its restart timer counted them
     * as delivered. A restore that dropped them would resume a link that stalls
     * for one restart interval and then renegotiates — indistinguishable, in a
     * log, from a bug in the peer. That is why SNAPSHOT_VERSION went 12 -> 13.
     */
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    /* Leave the FIFO wrapped and partly drained, so head, count and the ring
     * contents are all non-trivial and a restore that reconstructed them from
     * the array alone would be caught. */
    for (unsigned i = 0; i < UART_RX_FIFO; i++)
        (void)s5l_uart_rx_push(&m.uart4, (uint8_t)(0x10u + i));
    for (unsigned i = 0; i < 10u; i++)
        (void)s5l_uart_read(&m.uart4, UART_URXH);   /* six left, head at 10 */
    for (unsigned i = 0; i < 10u; i++)
        (void)s5l_uart_rx_push(&m.uart4, (uint8_t)(0xa0u + i));  /* full again */
    (void)s5l_uart_rx_push(&m.uart4, 0xffu);        /* refused, and counted   */

    uint8_t *img = NULL;
    size_t   len = 0;
    CHECK(snapshot_save_mem(&m, &img, &len) == SNAP_OK, "save failed");

    s5l8900_t r;
    CHECK(s5l8900_init(&r, 0, 1u << 20), "restore machine init failed");
    CHECK(snapshot_load_mem(&r, img, len) == SNAP_OK, "load failed");

    CHECK(r.uart4.rx_count == m.uart4.rx_count &&
          r.uart4.rx_head == m.uart4.rx_head,
          "the FIFO's head/count did not survive (%u/%u vs %u/%u)",
          r.uart4.rx_head, r.uart4.rx_count,
          m.uart4.rx_head, m.uart4.rx_count);
    CHECK(memcmp(r.uart4.rx, m.uart4.rx, UART_RX_FIFO) == 0,
          "the queued bytes did not survive the round trip");
    CHECK(r.uart4.rx_pushed == m.uart4.rx_pushed &&
          r.uart4.rx_dropped == m.uart4.rx_dropped &&
          r.uart4.rx_reads == m.uart4.rx_reads &&
          r.uart4.rx_underruns == m.uart4.rx_underruns,
          "the receive counters did not survive; a restored run would report a "
          "host that had never been told 'no'");
    CHECK(r.uart4.rx_dropped == 1u,
          "the refused push was not recorded (%llu)",
          (unsigned long long)r.uart4.rx_dropped);

    /* And the restored port really delivers the same next byte. */
    CHECK(s5l_uart_read(&r.uart4, UART_URXH) ==
          s5l_uart_read(&m.uart4, UART_URXH),
          "the restored FIFO's next byte differs from the original's");
    /* uart0's FIFO must be untouched: two ports, two FIFOs, one visitor. */
    CHECK(r.uart0.rx_count == 0u && r.uart0.rx_pushed == 0u,
          "uart4's receive state was restored into uart0");

    free(img);
    s5l8900_free(&r);
    s5l8900_free(&m);
}

static void test_snapshot_rejects_an_impossible_receive_fifo(void) {
    /* rx_count > UART_RX_FIFO and rx_head >= UART_RX_FIFO are unreachable by
     * any push or read, so a machine holding either is corrupt and the save
     * path must refuse rather than write a file that cannot be loaded. */
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    uint8_t *img = NULL;
    size_t   len = 0;
    m.uart4.rx_count = (uint8_t)(UART_RX_FIFO + 1u);
    CHECK(snapshot_save_mem(&m, &img, &len) != SNAP_OK,
          "a machine with an over-full receive FIFO was snapshotted");
    m.uart4.rx_count = 0;
    m.uart4.rx_head  = (uint8_t)UART_RX_FIFO;
    CHECK(snapshot_save_mem(&m, &img, &len) != SNAP_OK,
          "a machine with an out-of-range FIFO head was snapshotted");
    CHECK(img == NULL && len == 0u, "a refused snapshot returned bytes");

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
    printf("S5LBox uart4 (guest PPP line) tests\n");
    test_reset_is_total();
    test_status_lets_a_transmit_loop_terminate();
    test_configuration_registers_round_trip();
    test_capture_keeps_the_head_and_is_bounded();
    test_machine_decodes_uart4_as_its_own_window();
    test_the_two_captures_do_not_alias();
    test_uart4_registers_route_through_the_bus();
    test_transmitting_alone_raises_no_interrupt_line();
    test_no_host_peer_means_no_receive_anything();
    test_receive_fifo_is_ordered_and_bounded();
    test_a_utrstat_store_cannot_discard_a_queued_byte();
    test_uart4_asserts_vic_line_28_for_a_waiting_byte();
    test_suppression_withholds_the_line_and_nothing_else();
    test_suppression_defaults_off_and_reset_clears_it();
    test_uart0_receive_is_deliberately_not_wired();
    test_uart4_rx_is_a_declared_wake_source();
    test_the_loop_closes_through_the_uart();
    test_snapshot_carries_both_captures_separately();
    test_snapshot_rejects_an_impossible_uart4_length();
    test_snapshot_carries_the_receive_fifo();
    test_snapshot_rejects_an_impossible_receive_fifo();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
