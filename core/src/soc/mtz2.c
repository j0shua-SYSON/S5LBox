/*
 * iOS3-VM — the multi-touch controller on spi1 chip select 0.
 *
 * /arm-io/spi1/multi-touch, `compatible "multi-touch,n82"`, reg[0] = 0 (the
 * chip select — there is no `chip-select` property anywhere in the shipped
 * tree), `interrupts {0x9b, 0}` = GPIO interrupt line 155, `function-reset`
 * GPIO 0x0606 and `function-power_ldo` GPIO 0x0701.
 *
 * ===========================================================================
 * THE ONE THING THAT MUST NOT BE GOT BACKWARDS
 * ===========================================================================
 * AppleMultitouchZ2SPI probes this device with isInHBPP() — 0xc0441008,
 * reached by virtual dispatch through slot 0x4d0 of the vtable at 0xc0449f40,
 * never by a direct bl — and TWO CALLERS WANT OPPOSITE ANSWERS:
 *
 *   finishStarting() 0xc0442670 takes `beq 0xc0442714` on FALSE, prints
 *   "Could not detect HBPP. Returning false from finishStarting()" and
 *   DETACHES. That is the state run61 measured against the null device and the
 *   whole reason this file exists. It needs TRUE.
 *
 *   attemptToBootloadDevice 0xc04414c4 probes first thing
 *   (`ldr pc,[r3,#0x4d0]` at 0xc04414dc). On TRUE it logs "attempting to
 *   bootload device" and pushes 54,156 bytes of firmware — run65 watched it
 *   reach "MTSPIBootloader_Z2::bootloadDevice() / sending preconstructed
 *   firmware bytes". On FALSE it logs "not in HBPP, so skipping bootload"
 *   (literal 0xc04486f0, referenced only from this function's pool at
 *   0xc04415e4) and returns 0 — which the retry loop at 0xc043a980 counts as
 *   one failure, printing "Bootload attempt %d of %d failed". It needs FALSE.
 *
 * The two sites are BYTE-IDENTICAL on the wire, so the discrimination has to be
 * stateful. It is one monotonic bit, `hbpp_answered`, and getting the state it
 * keys on right took four wrong designs and two boots:
 *
 *   1. "Watch the reset line: the bootload probe follows resetDevice(TRUE)."
 *      Both sites reset. Worthless as a discriminator on its own.
 *
 *   2. "Accept the first probe, decline the rest." MEASURED WRONG. A run with
 *      `--call-probe-kernel 0xc0441008 --call-probe-kernel 0xc0442670` shows
 *      finishStarting entered once, at instruction 220,700,146, and isInHBPP
 *      entered once, at 316,898,121 with lr = 0xc04426cc. But spi1 carried TWO
 *      byte-identical 16-byte `1A A1 18 E1…` transfers, at 309.53M and 316.91M.
 *      The first is resetDevice's DUMMY TRANSFER (0xc0440d7c builds the same
 *      sixteen bytes and logs "initiating dummy transfer"), whose answer is
 *      discarded. A plain probe counter spends itself there and hands
 *      finishStarting the rejection.
 *
 *   3. "Never decline, then." That is what shipped in 098ce49, and run65 showed
 *      where it leads: the bootload probe answered TRUE and the driver began
 *      pushing firmware. Correct behaviour for an unprogrammed part; not what
 *      this model can survive.
 *
 *   4. "A reset restores HBPP, so a reset must also re-arm the claim." No. The
 *      guest pulses this line at every site, so re-arming the claim makes site
 *      2 identical to site 1 — which is bug 3 again. A reset restores the
 *      part's STATE; it does not un-ask the question the host already asked.
 *
 * WHAT ACTUALLY SEPARATES THE TWO EXCHANGES IS THE RESET LINE'S LEVEL, and the
 * guest's own log says so:
 *
 *      mtlog: enabling power
 *      mtlog: ensuring S_CLK is high
 *      mtlog: initiating dummy transfer      <- reset still ASSERTED
 *      mtlog: Deasserting reset line
 *      mtlog: checking if in HBPP            <- reset RELEASED
 *
 * confirmed against the GPIO trace to the instruction: fsel write 0x0006060e
 * (group 6, bit 6, level 0) at 220,635,069 asserts; the dummy runs at
 * 309.53M; 0x0006060f releases at 309,541,162; the probe runs at 316.91M;
 * 0x0006060e asserts again at 316,965,809. So the line is ACTIVE LOW, the
 * dummy is entirely inside the asserted window and the probe entirely outside
 * it.
 *
 * A part held in its reset pin drives nothing. Modelling that one physical
 * fact makes the dummy answer sixteen zeros — which the driver discards, as it
 * always did — and leaves `hbpp_answered` for the exchange that is actually
 * read. No magic constant, no counting of transfers, and every element of it
 * is visible in the guest's own log.
 *
 * `hbpp_answered` is still a lie: a real unprogrammed Z2 answers TRUE forever
 * and expects firmware. It is a bounded, named, single-bit lie that costs three
 * cosmetic log lines, and the alternative is in the note at the end of this
 * comment.
 *
 * ANSWERING TRUE ONCE ALSO CHOOSES THE FRAME OPCODE. The true return runs
 * `strb r3, [r4, #0x1bc]` at 0xc04426fc, and deviceReadResultData at
 * 0xc0441324 rejects 0xEB unless this+0x1bc is non-zero. So this device's Z2
 * frame opcode is downstream of the HBPP answer rather than independent of it.
 *
 * THE ALTERNATIVE, AND WHY NOT YET. Implementing the HBPP sink — accepting the
 * 54,156 bytes and afterwards reporting firmware resident — removes the lie
 * entirely. It is NOT blocked by DMA, which was the standing objection:
 * MTSPIBootloader_Z2 pushes through the ordinary SPI entry `v[0x368]` at
 * 0xc0444ff8, 0xc0445224 and 0xc04454d0, and the controller only arms DMA when
 * `this+0xf4` is non-zero (`ldr r5,[r4,#0xf4] / cmp r5,#0 / beq 0xc05a6cb4` at
 * 0xc05a6c24, the branch that skips `orr r2,r2,#0x40`) — which our 16-byte
 * transfers prove is zero, since they run in PIO today. What blocks it is that
 * the bootloader's own multi-stage protocol is unread, its failure mode is a
 * hang inside commandSleep rather than an error, and the feedback loop is an
 * 18-minute boot because the timer does not fire until instruction 1.11e9. It
 * is a fidelity upgrade, not a prerequisite: nothing above the bootloader can
 * tell a part that was programmed from one that says it was.
 *
 * ===========================================================================
 * THE WIRE PROTOCOL
 * ===========================================================================
 * Full duplex, one byte out for one byte in, framed by length. Command frames
 * are 16 bytes:
 *
 *     [0]      opcode
 *     [1..4]   parameters (report id in [1] for 0xE3/0xE6/0xE7)
 *     [5..13]  zero
 *     [14..15] LE16 sum16 of [0..13]
 *
 * CONFIRMED at 0xc0443288-0xc044329c, which builds GET_REPORT_INFO as
 * `tx[0]=0xE3; tx[1]=id; tx[14..15] = LE16(0xE3 + id)` — the plain byte sum of
 * a buffer that memset zeroed, with no seed and no complement.
 *
 * The driver validates a response two ways and only two ways
 * (0xc04433e4-0xc0443438 for 0xE3, 0xc04409b8-0xc04409f0 for 0xE1):
 *
 *     rx[0] == tx[0]                      else kIOReturnNotResponding
 *     LE16(rx[14..15]) == sum16(rx, 14)   else an error
 *
 * Both are satisfied by answering IN THE SAME TRANSFER: byte 0 of the reply is
 * the byte being received at that instant, and every later byte can depend on
 * every byte already received — including the report id at [1], which arrives
 * one slot before the first byte of the reply that needs it. The driver sends
 * 0xE3 twice with an IODelay(0x19) between (0xc04432ec and 0xc0443378) and
 * reads only the second, so answering both identically is correct and the
 * two-phase shape costs this model nothing.
 *
 * WE NEVER VALIDATE THE HOST'S CHECKSUM, and that is not laziness. The
 * control-read stage-2 packet ships a STALE one: 0xc0441ff8 stores tx[2] = 1
 * and the transfer at 0xc0442030 follows with no recomputation, so the frame
 * carries sum16 computed while tx[2] was still zero. 0xE5 by contrast does
 * recompute. A checksum-strict device would reject every control read the
 * driver makes, and whether real silicon checks it cannot be decided from
 * driver code.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <string.h>

uint16_t s5l_mtz2_sum16(const uint8_t *p, unsigned n) {
    uint16_t s = 0;
    if (!p) return 0u;
    while (n--) s = (uint16_t)(s + *p++);
    return s;
}

/* ============================================================ the reports ===
 *
 * ALL GEOMETRY COMES FROM THE DEVICE. The device tree carries none of it — no
 * row count, no column count, no surface size — so every number below is a
 * choice this project makes, and each one is justified where it is set in
 * s5l_mtz2_reset().
 */
unsigned s5l_mtz2_report(const s5l_mtz2_t *dev, uint8_t id, uint8_t *out) {
    if (!dev || !out) return 0u;
    memset(out, 0, MTZ2_PAYLOAD_MAX);
    switch (id) {
        case MTZ2_REPORT_FAMILY_ID:
            /* 0xc0438670 reads data[0] and publishes it as "Family ID". */
            out[0] = dev->family_id;
            return 1u;
        case MTZ2_REPORT_GEOMETRY:
            /*
             * 0xc04386e0: data[0] -> "Endianness" (also cached at this+0x1a0),
             * data[1] -> "Sensor Rows", data[2] -> "Sensor Columns", and
             * data[3..4] -> "bcdVersion" assembled BIG-endian
             * (`orr r3, data[4], data[3] lsl #8`). Note the byte order: this
             * is the one field in the whole protocol that is not little-endian.
             */
            out[0] = dev->endianness;
            out[1] = dev->rows;
            out[2] = dev->columns;
            out[3] = (uint8_t)(dev->bcd_version >> 8);
            out[4] = (uint8_t)(dev->bcd_version & 0xffu);
            return 5u;
        case MTZ2_REPORT_BUTTONS:
            /* 0xc043898c reads data[0] and publishes it as "Buttons". */
            out[0] = dev->buttons;
            return 1u;
        case MTZ2_REPORT_SURFACE:
            /*
             * 0xc04388dc requires length >= 8 (`cmp r3,#7 / bls`) and reads two
             * UNALIGNED little-endian 32-bit words, `ldr r2,[sp,#5]` and
             * `ldr r8,[r2,#5]`, publishing them as "Sensor Surface Width" and
             * "Sensor Surface Height". A shorter report is dropped silently.
             */
            out[0] = (uint8_t)(dev->surface_width);
            out[1] = (uint8_t)(dev->surface_width >> 8);
            out[2] = (uint8_t)(dev->surface_width >> 16);
            out[3] = (uint8_t)(dev->surface_width >> 24);
            out[4] = (uint8_t)(dev->surface_height);
            out[5] = (uint8_t)(dev->surface_height >> 8);
            out[6] = (uint8_t)(dev->surface_height >> 16);
            out[7] = (uint8_t)(dev->surface_height >> 24);
            return 8u;
        case MTZ2_REPORT_REGION_DESC:
        case MTZ2_REPORT_REGION_PARAM:
            /*
             * 0xc043880c and 0xc0438874 publish the whole blob as OSData under
             * "Sensor Region Descriptor" and "Sensor Region Param" and read no
             * field of it. Eight zero bytes is a well-formed answer of the
             * right shape; what the contents MEAN is part of the frame payload
             * format, which is the one genuine unknown left and belongs to the
             * step that reverses _MT_ParsedMultitouchFrameRepCreate. Answering
             * with a length rather than refusing keeps the driver's property
             * table complete without claiming to know the encoding.
             */
            return 8u;
        default:
            /* A report this device does not have. getReportInfo answers with a
             * length of zero, which is what a real device does for an
             * unimplemented id, and the driver moves on. */
            return 0u;
    }
}

/* ========================================================== packet framing === */

/* How many bytes the packet starting with `op` occupies. Zero means "not an
 * opcode this device answers". */
static unsigned frame_len(const s5l_mtz2_t *dev, uint8_t op) {
    switch (op) {
        case MTZ2_OP_CMD_STATUS:
        case MTZ2_OP_DEVICE_INFO:
        case MTZ2_OP_REPORT_INFO:
        case MTZ2_OP_WRITE_SHORT:
        case MTZ2_OP_WRITE_LONG:
        case MTZ2_OP_READ_SHORT:
        case MTZ2_OP_READ_LONG:
        case MTZ2_OP_WAKE:
            return MTZ2_FRAME_LEN;
        case MTZ2_OP_REQ_WAKEUP:
            return 2u;   /* {0x19, 0xC1} */
        case MTZ2_OP_HBPP:
            /*
             * The HBPP loopback probe, framed as a 16-byte packet like any
             * other command rather than handled by putting the whole device
             * into a loopback mode. That distinction matters twice over. It
             * means a probe cannot swallow a following command, and it means
             * the ordinary protocol keeps working while the device is in HBPP
             * — which it now is for the whole boot, so a loopback mode would
             * have echoed every GET_REPORT_INFO frame back at the driver.
             *
             * It also must be framed rather than skipped a byte at a time: the
             * probe pattern `1A A1 18 E1 ...` CONTAINS 0xE1, so a device that
             * treated 0x1A as an unknown byte would reach the 0xE1 at offset 3
             * and start parsing the rest of the probe as a GET_CMD_STATUS
             * frame, ending twelve bytes into a frame that never completes.
             */
            return MTZ2_FRAME_LEN;
        case MTZ2_OP_FRAME_Z1:
        case MTZ2_OP_FRAME_Z2:
            /*
             * A frame read is `frameLen + 5` bytes and this device has no
             * frame to give: `contacts` is always zero until step 4. Answer the
             * shortest well-formed header — length zero — rather than refusing,
             * so a driver that reads speculatively gets a valid empty frame
             * instead of a corrupt one.
             */
            (void)dev;
            return 5u;
        default:
            return 0u;
    }
}

/* Finish a reply: zero the tail and stamp the little-endian checksum. */
static void seal(s5l_mtz2_t *dev) {
    if (dev->len < 3u) return;
    uint16_t sum = s5l_mtz2_sum16(dev->rsp, (unsigned)(dev->len - 2u));
    dev->rsp[dev->len - 2u] = (uint8_t)(sum & 0xffu);
    dev->rsp[dev->len - 1u] = (uint8_t)(sum >> 8);
}

/*
 * Build the reply once the parameter byte has arrived.
 *
 * Called from byte 1, which is the last moment at which every byte the reply
 * needs is already in hand and the first byte that depends on it — rx[2] — has
 * not yet been driven.
 */
static void compose(s5l_mtz2_t *dev) {
    uint8_t body[MTZ2_PAYLOAD_MAX];
    unsigned n;
    memset(dev->rsp, 0, sizeof dev->rsp);

    if (dev->op == MTZ2_OP_HBPP) {
        /*
         * The declining answer: sixteen zeros, opcode byte included, and
         * deliberately NOT an echo. The driver tests BE16(rx[0..1]) and
         * BE16(rx[2..3]) against a set containing neither 0x0000 nor 0x1A00,
         * so this is a definite rejection — and it is exactly the sixteen
         * zeros the null device produced in run61, which is the measurement
         * this half of the behaviour is anchored to. The in-HBPP answer is not
         * built here at all: it is the incoming byte, returned as it arrives
         * by mtz2_transfer(), because a loopback has no buffer.
         */
        return;
    }

    dev->rsp[0] = dev->op;          /* the driver's rx[0] == tx[0] test */

    switch (dev->op) {
        case MTZ2_OP_REPORT_INFO:
            /*
             * 0xc0443440-0xc0443460: rx[2] is stored as the record's first byte
             * and is only ever tested against zero, and rx[3..4] is the
             * report's length as a little-endian 16-bit word. rx[1] is never
             * loaded at all — its meaning is not determinable from the driver,
             * so it stays zero rather than being invented.
             */
            n = s5l_mtz2_report(dev, dev->req[1], body);
            dev->rsp[2] = 0u;
            dev->rsp[3] = (uint8_t)(n & 0xffu);
            dev->rsp[4] = (uint8_t)(n >> 8);
            break;
        case MTZ2_OP_READ_SHORT:
        case MTZ2_OP_READ_LONG:
            /* The payload begins at rx[3]; the two bytes before it are never
             * loaded. Every report this device publishes is at most
             * MTZ2_PAYLOAD_MAX bytes, which is exactly the driver's own
             * short-form cut, so the long form never carries a longer one. */
            n = s5l_mtz2_report(dev, dev->req[1], body);
            if (n > MTZ2_PAYLOAD_MAX) n = MTZ2_PAYLOAD_MAX;
            memcpy(&dev->rsp[MTZ2_PAYLOAD_AT], body, n);
            break;
        case MTZ2_OP_FRAME_Z1:
        case MTZ2_OP_FRAME_Z2:
            /*
             * An empty frame. The driver checks that the low byte of the sum of
             * the first five bytes is zero (0xc0441330-0xc044133c) before it
             * trusts the length at [2..3], so the header carries its own
             * complement in [4] rather than a checksum in the usual place.
             */
            dev->rsp[1] = 0u;
            dev->rsp[2] = 0u;
            dev->rsp[3] = 0u;
            dev->rsp[4] = (uint8_t)(0u - (uint8_t)(dev->rsp[0] + dev->rsp[1] +
                                                   dev->rsp[2] + dev->rsp[3]));
            return;   /* its own check byte; no trailing sum16 */
        default:
            /* GET_CMD_STATUS, GET_DEVICE_INFO, the control writes and the wake:
             * an echoed opcode, a zero body meaning "no error", and a valid
             * checksum is the whole of a successful answer. */
            break;
    }
    seal(dev);
}

/* =============================================================== the wire === */

/*
 * The byte this device drives while receiving `out` at position `pos`.
 *
 * An HBPP probe is a LOOPBACK: the answer is the byte arriving at that instant,
 * not a byte prepared earlier, which is exactly why echoing passes the driver's
 * test — the first two words it transmits, 0x1AA1 and 0x18E1, are both in the
 * accepted set. Everything else is answered from the composed reply.
 */
static uint8_t drive(const s5l_mtz2_t *dev, uint8_t out, unsigned pos) {
    if (dev->op == MTZ2_OP_HBPP && !dev->hbpp_answered) return out;
    return pos < S5L_MTZ2_BUF ? dev->rsp[pos] : 0u;
}

static uint8_t mtz2_transfer(void *ctx, uint8_t out) {
    s5l_mtz2_t *dev = ctx;
    if (!dev) return 0u;

    /*
     * Held in reset: drive nothing, remember nothing.
     *
     * This is one physical fact and it is what makes the whole skip work.
     * resetDevice clocks a 16-byte dummy transfer while the line is down —
     * `1A A1 18 E1…`, byte-identical to the real probe, answer discarded — and
     * then releases the line and probes. Swallowing the dummy here is what
     * leaves the one-time claim in `hbpp_answered` for the exchange whose
     * answer is actually read. Note the framer is deliberately NOT advanced:
     * a part in reset has no protocol position either.
     */
    if (dev->in_reset) {
        dev->reset_bytes++;
        (void)out;
        return 0u;
    }

    if (dev->len == 0u) {
        /* Between packets: this byte is an opcode or it is nothing. */
        unsigned n = frame_len(dev, out);
        if (!n) {
            /* Not an opcode. Stay idle and answer zero, so the next byte is
             * read as an opcode again rather than as this one's payload. */
            dev->unknown_opcodes++;
            dev->last_unknown_op = out;
            return 0u;
        }
        dev->op  = out;
        dev->len = (uint8_t)n;
        dev->pos = 0u;
        memset(dev->req, 0, sizeof dev->req);
        dev->req[0] = out;
        compose(dev);              /* enough for a parameterless opcode */
        dev->pos = 1u;
        /* Byte 0 of a command reply is the opcode itself — the driver's
         * rx[0] == tx[0] test — and byte 0 of an HBPP reply is the loopback. */
        return drive(dev, out, 0u);
    }

    if (dev->pos < S5L_MTZ2_BUF) dev->req[dev->pos] = out;
    /* The parameter byte has landed; rebuild before driving the first byte
     * that can depend on it. */
    if (dev->pos == 1u) compose(dev);

    uint8_t v = drive(dev, out, dev->pos);
    dev->pos++;
    if (dev->pos >= dev->len) {
        dev->packets++;
        if (dev->op == MTZ2_OP_HBPP) {
            dev->hbpp_probes++;
            /* The host has had its one look. Only reachable out of reset, so
             * the dummy transfer can never get here. */
            dev->hbpp_answered = true;
        }
        dev->pos = 0u;
        dev->len = 0u;
    }
    return v;
}

/* ============================================================== lifecycle === */

void s5l_mtz2_reset(s5l_mtz2_t *dev) {
    if (!dev) return;
    memset(dev, 0, sizeof *dev);

    /*
     * Power-on state. `hbpp_answered` false means the next probe answered out
     * of reset gets the affirmative finishStarting() needs.
     *
     * `in_reset` true is not a stylistic choice: the GPIO pin block powers up
     * all-zero and MTZ2_PIN_RESET is active low, so a part on this board is
     * genuinely held down until the driver releases it. It also matters that
     * the model start here rather than at "released", because the guest's
     * first store to this pin writes level 0 — no change, so no watch callback
     * — and only the later release produces an edge.
     */
    dev->in_reset      = true;
    dev->hbpp_answered = false;

    /*
     * THE GEOMETRY. None of this is in the device tree, so all of it is our
     * choice, and the choice is made to keep a reported coordinate and a screen
     * pixel in an exact, distortion-free relationship — which is what decides
     * whether a tap lands on the icon under the finger.
     *
     * Surface 4800 x 7200, in hundredths of a millimetre: 48.00 mm by
     * 72.00 mm. Three properties earn it:
     *
     *   - It is EXACTLY 15 times the panel's 320 x 480 in BOTH axes. The two
     *     scale factors are identical, so no aspect distortion is
     *     representable; and 15 is a whole number, so a coordinate in these
     *     units converts to a pixel by an exact integer divide with no
     *     rounding rule to get wrong.
     *   - 15 units per point is 1/15 of a pixel of headroom, so when a later
     *     step reports sub-pixel contact positions nothing here has to change.
     *   - It is within 2.5% of the real iPhone 3G active area (about 49.2 mm by
     *     73.8 mm), so anything that treats these as physical dimensions — a
     *     gesture recogniser converting to millimetres per second, say — gets a
     *     sane answer rather than an absurd one. A pair like 32000 x 48000
     *     would satisfy the first two points and fail this one by claiming a
     *     320 mm wide phone.
     *
     * Sensor 10 columns by 15 rows: the same 2:3 ratio as the surface and the
     * panel, and a ~4.8 mm electrode pitch, which is the right order for this
     * generation of part.
     */
    dev->surface_width  = 4800u;
    dev->surface_height = 7200u;
    dev->columns        = 10u;
    dev->rows           = 15u;

    /*
     * bcdVersion 1.00. Read big-endian out of the report (see
     * s5l_mtz2_report), published as a number and not compared against
     * anything on this boot path.
     */
    dev->bcd_version = 0x0100u;
    /*
     * Endianness 1. The driver caches this byte at this+0x1a0 and hands it to
     * the frame parser, so it selects the byte order of a payload format this
     * model does not yet generate. 1 is the value that selects the
     * little-endian layout an ARM device would produce; the step that reverses
     * _MT_ParsedMultitouchFrameRepCreate is the step that can confirm it, and
     * it is called out here so that it is checked rather than inherited.
     */
    dev->endianness = 1u;
    /*
     * Family ID 1. Arbitrary and stated as arbitrary: nothing on the boot path
     * we can read compares it, the driver only republishes it as a property,
     * and no source we have gives the real n82 value. Zero was avoided because
     * a zero is indistinguishable from an unanswered report.
     */
    dev->family_id = 1u;
    /* No buttons on the digitizer. The home and lock buttons are elsewhere. */
    dev->buttons = 0u;
}

void s5l_mtz2_bind(s5l_mtz2_t *dev, s5l_spi_slave_t *slave) {
    if (!slave) return;
    memset(slave, 0, sizeof *slave);
    if (!dev) return;
    slave->ctx = dev;
    slave->transfer = mtz2_transfer;
}

bool s5l_mtz2_irq(const s5l_mtz2_t *dev) {
    /* The attention line. False for the whole of this step, and false for an
     * honest reason: a device with no pending contact does not ask to be read.
     * Step 4 sets `atn` when it queues a frame; nothing else changes. */
    return dev && dev->atn;
}

void s5l_mtz2_reset_pin(void *ctx, bool level) {
    s5l_mtz2_t *dev = ctx;
    if (!dev) return;
    /*
     * MTZ2_PIN_RESET, group 6 bit 6, ACTIVE LOW — so `level == false` holds the
     * part down. Measured, not assumed: fsel write 0x0006060e (level 0) at
     * instruction 220,635,069 is the guest's "Asserting reset line", and
     * 0x0006060f (level 1) at 309,541,162 is its "Deasserting reset line",
     * with the dummy transfer between them and the probe after.
     *
     * THIS FUNCTION MUST NOT TOUCH `hbpp_answered`, AND THAT IS THE FIX.
     * An earlier revision set it back to "in HBPP" here, reasoning that a part
     * with no firmware resets into its bootloader — which is true, and is
     * exactly why it breaks. The guest pulses this line before EVERY probe
     * site, so re-arming the claim makes the bootload site indistinguishable
     * from the first one, the bootload probe answers affirmatively, and the
     * driver starts pushing 54,156 bytes of firmware. run65 watched it do
     * precisely that. A reset restores the part's state; it does not un-ask a
     * question the host has already had answered.
     *
     * The part does lose its protocol position: a packet half-received when
     * the line drops did not survive.
     */
    dev->in_reset = !level;
    dev->pos = 0u;
    dev->len = 0u;
    dev->resets++;
}

void s5l_mtz2_select_pin(void *ctx, bool level) {
    s5l_mtz2_t *dev = ctx;
    if (!dev) return;
    /*
     * Resynchronise the framer, and nothing else. A chip-select edge means no
     * transfer is in flight, so discarding a part-received packet at one is
     * always safe — and because the framer already knows every packet's length
     * from its opcode, a driver that never drives this pin is not broken by its
     * absence. That is why this is a resync and not the framing itself.
     */
    (void)level;
    dev->pos = 0u;
    dev->len = 0u;
}
