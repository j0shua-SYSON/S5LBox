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

/* The wire length L of the pending report: payload plus its two checksum
 * bytes, and zero when nothing is pending. This is the number the length read
 * answers with and the number the data read's transfer size is derived from. */
static unsigned wire_len(const s5l_mtz2_t *dev) {
    return dev->frame_len ? (unsigned)dev->frame_len + 2u : 0u;
}

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
             * A frame read has TWO lengths and the byte that chooses between
             * them — tx[2] — has not arrived yet. Start as the LENGTH read,
             * which is always sixteen bytes (0xc0442440 loads `r4 = 0x10` and
             * never changes it), and let the framer lengthen the packet to
             * L + 5 when tx[2] turns out to be 1. Starting the other way round
             * is not possible: L is not known to the host until the length read
             * has answered, so the sixteen-byte form is the one a device with
             * no context must assume.
             */
            (void)dev;
            return MTZ2_FRAME_LEN;
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
        case MTZ2_OP_FRAME_Z2: {
            unsigned L = wire_len(dev);
            /*
             * rx[1] is the low byte of L in BOTH forms, and that is what lets
             * one composer serve both. The length read wants LE16 L at
             * rx[1..2] (0xc04425b0: `rx[1] | rx[2] << 8`); the data read leaves
             * rx[1] free, constrained only by the five-byte sum. Driving the
             * same byte either way means position 1 — which goes out on the
             * wire BEFORE tx[2] discloses which read this is — never has to
             * know.
             */
            dev->rsp[1] = (uint8_t)(L & 0xffu);
            if (dev->frame_phase == 1u) {
                /*
                 * THE DATA READ. rx[2..3] is LE16 L again — a different
                 * position from the length read's, which is not a
                 * transcription slip: 0xc0441370 loads rx[3] and rx[2] where
                 * 0xc04425b0 loads rx[2] and rx[1]. rx[4] then carries whatever
                 * makes the first five bytes sum to zero mod 256, which is the
                 * only header check the driver makes (0xc0441330, `sum(rx,5)`
                 * must be 0) — the usual trailing sum16 covers the payload
                 * instead.
                 */
                dev->rsp[2] = (uint8_t)(L & 0xffu);
                dev->rsp[3] = (uint8_t)(L >> 8);
                dev->rsp[4] = (uint8_t)(0u - (uint8_t)(dev->rsp[0] +
                                                       dev->rsp[1] +
                                                       dev->rsp[2] +
                                                       dev->rsp[3]));
                if (dev->frame_len) {
                    uint16_t sum;
                    memcpy(&dev->rsp[5], dev->frame, dev->frame_len);
                    sum = s5l_mtz2_sum16(dev->frame, dev->frame_len);
                    dev->rsp[5u + dev->frame_len]      = (uint8_t)(sum & 0xffu);
                    dev->rsp[5u + dev->frame_len + 1u] = (uint8_t)(sum >> 8);
                }
                return;    /* its own header check byte and payload checksum */
            }
            /*
             * THE LENGTH READ. rx[0] only has to satisfy `(rx[0] & 0xF0) ==
             * 0xE0` (0xc0442558), which the echoed opcode does, and the
             * ordinary trailing sum16 over the first fourteen bytes applies.
             * L == 0 lands here too and is a complete, valid answer: the caller
             * tests it at 0xc04430bc and stops without reading data.
             */
            dev->rsp[2] = (uint8_t)(L >> 8);
            break;
        }
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
    return pos < S5L_MTZ2_RSP ? dev->rsp[pos] : 0u;
}

/* Is `op` one of the two frame-read opcodes? */
static bool is_frame_op(uint8_t op) {
    return op == MTZ2_OP_FRAME_Z1 || op == MTZ2_OP_FRAME_Z2;
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
        /* 0xFF, not 0: tx[2] == 0 is the LENGTH read, so a cleared field would
         * make every non-frame packet look like one. */
        dev->frame_phase = 0xffu;
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
    /*
     * tx[2] has landed, which for a frame read is the byte that says whether
     * this is the length read or the data read. Latch it and lengthen the
     * packet BEFORE driving position 2, which is the first byte whose value
     * differs between the two forms. Any value other than 1 is treated as the
     * length read, because that is the sixteen-byte form and reading sixteen
     * bytes of a shorter packet is the failure that cannot be recovered from.
     */
    if (dev->pos == 2u && is_frame_op(dev->op)) {
        dev->frame_phase = out;
        if (out == 1u) {
            unsigned total = wire_len(dev) + 5u;
            dev->len = (uint8_t)total;
            dev->data_reads++;
        } else {
            dev->length_reads++;
        }
        compose(dev);
    }

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
        /*
         * A COMPLETED data read is what drops the attention line. Not the
         * length read, which is only an enquiry, and not the first byte of the
         * data read either: a report the host began clocking out and abandoned
         * — the select moved, the part was reset — is still pending, and a
         * device that forgot it here would lose the finger-down half of a tap
         * with no trace. The line is a LEVEL and gpioic.c re-latches it every
         * refresh, so holding it up until the last byte is clocked costs at
         * most one extra interrupt in which the guest finds L == 0.
         */
        if (is_frame_op(dev->op) && dev->frame_phase == 1u && dev->frame_len) {
            dev->frames_read++;
            dev->frame_len = 0u;
            dev->contacts  = 0u;
            dev->atn       = false;
            memset(dev->frame, 0, sizeof dev->frame);
        }
        dev->pos = 0u;
        dev->len = 0u;
        dev->frame_phase = 0xffu;
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
    /* Not zero: zero is the LENGTH read's tx[2]. See the framer. */
    dev->frame_phase   = 0xffu;

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
    /* The attention line: asserted exactly while a queued report is waiting to
     * be clocked out. A device with no pending contact does not ask to be
     * read, and one whose report the host has already taken stops asking. */
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
    dev->frame_phase = 0xffu;
    dev->resets++;
    /*
     * A queued report does NOT survive a reset, and that is the physical
     * answer rather than the convenient one: the part's scan buffer is what
     * holds it, and a part in reset has cleared it. Keeping it would also let a
     * report queued before bring-up arrive after it, dated to a moment the
     * guest has no way to place. The attention line drops with it.
     */
    if (dev->in_reset) {
        dev->frame_len = 0u;
        dev->contacts  = 0u;
        dev->atn       = false;
        memset(dev->frame, 0, sizeof dev->frame);
    }
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
    dev->frame_phase = 0xffu;
    /* A report half-clocked-out is still pending. See the framer's note on why
     * the attention line falls at the LAST byte of a data read and not the
     * first. */
}

void s5l_mtz2_power_pin(void *ctx, bool level) {
    s5l_mtz2_t *dev = ctx;
    if (!dev) return;
    /*
     * MTZ2_PIN_POWER, group 7 bit 1, and RECORDED RATHER THAN ACTED ON.
     *
     * The device tree gives `function-power_ldo {phandle, 'GPIO', 0x0701,
     * 0x00000101}`; the reset line beside it is `{..., 0x0606, 0x00010001}`
     * and /arm-io/spi1's chip select is `{..., 0x1800, 0x00000001}`. Those
     * fourth words are the platform-function argument block, and which byte of
     * it names the polarity is not established anywhere this project can read
     * — the baseband node uses the SAME 0x00000101 for `function-bb_rst`,
     * which is a reset and not a supply, so the obvious "0x0101 means active
     * high" reading is already contradicted.
     *
     * The reset line's polarity was settled by MEASUREMENT (fsel 0x0006060e at
     * 220,635,069 straddling the dummy transfer), not by reading the tree, and
     * nothing equivalent has been measured for this pin. Gating frame delivery
     * on a guessed polarity would mean a wrong guess refuses every injection
     * for the whole boot with the device looking healthy — the exact silent
     * failure this file keeps being corrected for. So the level is tracked and
     * published, and `s5l_mtz2_set_contacts` does not consult it.
     */
    if (dev->power_level != level) dev->power_edges++;
    dev->power_level = level;
}

/* ======================================================== host injection ===
 *
 * THE PAYLOAD FORMAT.
 *
 * The kext parses ONE byte of the payload — 0xc0438a1c tests
 * `payload[0] == 0x50` and diverts that case to a separate 8-byte status
 * decoder at 0xc043d1ac — and otherwise hands the buffer verbatim to
 * `IODataQueue::enqueue(payload, (len + 3) & ~3)`. Everything else is decided
 * by userspace, and the format below was read out of userspace by two
 * independent readers that were given the same question and no access to each
 * other's answer:
 *
 *   A. MultitouchSupport.framework's own parser, inside the shared cache at
 *      load address 0x33cf6000. `_mt_HandleMultitouchFrame` (0x33cfb3ec)
 *      switches on payload[0]; 0xCC and 0xCE go to `_MTProcess_0xCC_Data`
 *      (0x33cfba60).
 *   B. MultitouchHID.plugin, a different binary (VA == file offset), whose
 *      MTSimpleHIDManager consumes what A produces.
 *
 * They agree, independently, on: the dispatch being on payload[0]; the header
 * being counter / button state / contact count at [1] / [2] / [3]; the
 * per-contact stride of THIRTY-TWO for this frame type (`lsl r2, r3, #5` at
 * 0x33cfbbcc); the record beginning id / stage / fingerID / handID(s8); X as a
 * 32-bit value at +4 shifted right by 8 and Y the same at +8; Z total at +0x14;
 * the ellipse major and minor at +0x1c and +0x1e; coordinates in HUNDREDTHS OF
 * A MILLIMETRE, which is the same unit report 0xD9's surface size is already
 * published in; and the eight path stages 0..7 being NotTracking, StartInRange,
 * HoverInRange, MakeTouch, Touching, BreakTouch, LingerInRange, OutOfRange.
 *
 * Reader A additionally verified the byte-order selector, which settles a
 * question this file has carried as an open one since the geometry was chosen:
 * `_mt_SwapInt32DeviceToHost` (0x33cfb8e0) is `cmp r0,#1 / beq done / rev`,
 * with r0 taken from the device's cached `Endianness` property. So ONE means
 * "no swap" and every other value byte-swaps — and zero is NOT "native". The
 * model's `endianness = 1` is right, and it is right for a reason now rather
 * than by inheritance.
 *
 * WHY 0xCC AND NOT 0x44 OR 0x74. The parser accepts four contact-carrying
 * families and the device chooses by writing payload[0]; reader A's V2 (0x44)
 * table is the more capable one but only ONE reader produced it, while the
 * 0xCC record layout is what both readers derived separately. Where a wrong
 * guess costs an 18-minute boot, the doubly-attested shape is worth more than
 * the richer one. 0xCC also needs no negotiated header length and no stride
 * field: its header is a constant ten bytes and its stride a constant 32, so
 * there is no field whose default the two readings could differ about.
 *
 * WHAT REJECTS A FRAME, and it is a short list — there is no magic number and
 * no checksum anywhere in userspace: an unknown payload[0]; a length at or
 * below 9; a length below header + count * stride. Our shortest payload is
 * 10 + 32 = 42 bytes, and the kext rounds the enqueued length UP to a multiple
 * of four, so what userspace measures is 44 and both bounds hold with room.
 */

/*
 * A panel pixel in the surface units report 0xD9 published, hundredths of a
 * millimetre.
 *
 * Exact by construction: the surface is 15x the panel in both axes, so this is
 * an integer multiply with no rounding rule to get wrong, and the half-step
 * puts the reported point at the CENTRE of the pixel rather than its top-left
 * corner — which is what stops a tap on pixel 0 from reading as "just off the
 * edge" to anything that rounds.
 *
 * `flip` exists because the two coordinate systems disagree about which way Y
 * runs. MultitouchHID converts a normalised contact to a pixel row with
 * `py = H * (1 - norm.y)`, i.e. it treats device Y as increasing UPWARD — the
 * trackpad convention this whole stack came from — while a panel pixel row
 * increases downward. Reader B read that conversion; reader A read only the
 * normalisation either side of it, so this one step is SINGLY attested and is
 * called out as such. It is also the step a mutation test pins, because a
 * mirrored Y is the failure that would look exactly like "the tap went
 * somewhere else" rather than like a bug.
 */
static uint16_t to_surface(uint32_t pixel, uint32_t span_pixels,
                           uint32_t span_units, bool flip) {
    uint32_t scale = span_pixels ? span_units / span_pixels : 0u;
    uint32_t v;
    if (flip) {
        /* The centre of the pixel counted from the far edge. Written as one
         * subtraction from the span so that pixel 0 and pixel H-1 land the
         * same distance from their respective edges. */
        v = span_units - pixel * scale - scale / 2u;
    } else {
        v = pixel * scale + scale / 2u;
    }
    if (v > 0x7fffu) v = 0x7fffu;   /* the wire field is s16 */
    return (uint16_t)v;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

unsigned s5l_mtz2_encode(const s5l_mtz2_t *dev, const s5l_mt_contact_t *c,
                         unsigned n, uint8_t seq, uint32_t ms, uint8_t *out) {
    unsigned len;
    uint32_t scale_x;
    if (!dev || !out) return 0u;
    /*
     * A contact count of zero is refused, and that is a decision rather than an
     * oversight. The parser accepts a zero-contact frame — ten bytes clears its
     * `len > 9` bound — but it carries nothing a BreakTouch frame has not
     * already said, and allowing it would make "this device has no report" and
     * "this device reports nothing" two states a caller could confuse. Say a
     * finger left by saying so.
     */
    if (n == 0u || n > MTZ2_CONTACT_MAX) return 0u;
    if (!c) return 0u;
    scale_x = dev->surface_width / S5L_MT_PANEL_W;

    memset(out, 0, MTZ2_PAYLOAD_LIMIT);
    /*
     * THE HEADER, a constant ten bytes for this frame type.
     *
     *  [0]     frame type. 0xCC selects _MTProcess_0xCC_Data. It is also not
     *          0x50, which is the one value the KEXT would divert.
     *  [1]     frame counter, wrapping at 8 bits.
     *  [2]     button state. This digitizer has no buttons; report 0xD7 already
     *          says so with the same zero.
     *  [3]     contact count. A count, not a bitmask.
     *  [4..5]  a 16-bit field the parser copies to rep+0x42 and nothing in
     *          either reader's trace consumes. Zero.
     *  [6..9]  LE32 timestamp in milliseconds, at an UNALIGNED offset — the
     *          parser really does load a word at +6.
     */
    out[0] = MTZ2_FRAME_TYPE;
    out[1] = seq;
    out[2] = 0u;
    out[3] = (uint8_t)n;
    put32(&out[6], ms);
    len = MTZ2_FRAME_HEADER + n * MTZ2_CONTACT_STRIDE;

    for (unsigned i = 0; i < n; i++) {
        uint8_t *r = &out[MTZ2_FRAME_HEADER + i * MTZ2_CONTACT_STRIDE];
        uint16_t sx, sy;
        /* 1..11 is the path-identifier range _mt_getPathLifeCycle accepts
         * (0x33cfd5f4); this device never reports more than five fingers, so
         * the tighter bound is the honest one to enforce. */
        if (!c[i].id || c[i].id > MTZ2_CONTACT_MAX) return 0u;
        if (c[i].phase > MTZ2_PHASE_OUT_OF_RANGE) return 0u;
        if (c[i].x >= S5L_MT_PANEL_W || c[i].y >= S5L_MT_PANEL_H) return 0u;
        sx = to_surface(c[i].x, S5L_MT_PANEL_W, dev->surface_width, false);
        sy = to_surface(c[i].y, S5L_MT_PANEL_H, dev->surface_height, true);

        r[0] = c[i].id;            /* path identifier                        */
        r[1] = c[i].phase;         /* path stage; 0..7, see MTZ2_PHASE_*      */
        r[2] = c[i].id;            /* finger id; one finger per path here    */
        r[3] = 1;                  /* hand id, signed. One hand.             */
        /*
         * X and Y. This family stores them as 32-bit values that the parser
         * arithmetic-shifts right by eight, so the low byte is a fractional
         * part: the coordinate written here is the surface value scaled by
         * 256. The velocities beside them are shifted right by NINE and are
         * zero, because this device reports position only and a fabricated
         * velocity would be a number the guest could act on.
         */
        put32(&r[4],  (uint32_t)sx << 8);
        put32(&r[8],  (uint32_t)sy << 8);
        put32(&r[12], 0u);         /* velocity X */
        put32(&r[16], 0u);         /* velocity Y */
        /*
         * Z total, which the parser divides by 256 into a dimensionless
         * amplitude. The caller's 0..255 maps linearly onto 0..1. The only
         * property anything downstream was seen to require is that it be
         * greater than zero while the finger is down, which is why a lift
         * carries zero here and says BreakTouch in the stage byte: two
         * independent statements of the same fact, so a consumer that reads
         * either one agrees.
         */
        put16(&r[20], c[i].pressure);
        /* +0x16 and +0x18: fields both readers place but neither names. Zero
         * is a legal value for each and is what a device with nothing to say
         * about them reports. */
        put16(&r[22], 0u);
        put16(&r[24], 0u);
        /* Orientation, scaled by pi * 2^-15. This model reports circular
         * contacts, so it is zero — a real angle would be invented. */
        put16(&r[26], 0u);
        /*
         * The contact ellipse, in the same hundredths of a millimetre as the
         * position: the parser divides both by 100 to get millimetres. These
         * are LENGTHS and not positions, so they get the bare scale factor and
         * NOT to_surface()'s half-pixel centring — an axis is measured from
         * its own zero.
         */
        put16(&r[28], (uint16_t)(c[i].major * scale_x));
        put16(&r[30], (uint16_t)(c[i].minor * scale_x));
    }
    return len;
}


bool s5l_mtz2_set_contacts(s5l_mtz2_t *dev, const s5l_mt_contact_t *c,
                           unsigned n) {
    uint8_t payload[MTZ2_PAYLOAD_LIMIT];
    unsigned len;
    if (!dev) return false;

    /*
     * THREE-WAY HONESTY. Each refusal is a different fact about the device and
     * every one of them is checkable; none is defensive padding.
     */
    /* Held in reset: a part driving nothing cannot raise an attention line,
     * and this model already refuses to answer a byte in that state. */
    if (dev->in_reset)               { dev->injects_refused++; return false; }
    /*
     * Not brought up. `hbpp_answered` false means the driver has not yet had
     * the affirmative probe that sets its this+0x1bc, and deviceReadResultData
     * at 0xc0441324 REJECTS a 0xEB frame outright while that byte is zero
     * (`ldrb r3,[r4,#0x1bc] / cmp r3,#0 / beq 0xc0441408`). A report queued now
     * could be read off the wire and would still be thrown away.
     */
    if (!dev->hbpp_answered)         { dev->injects_refused++; return false; }
    /*
     * One report at a time. A real Z2 overwrites its scan buffer and drops
     * frames the host was too slow for; doing that here would silently discard
     * the finger-DOWN half of a tap and leave a lift the guest cannot explain.
     * Refusing makes the caller pace itself and leaves the reason visible.
     */
    if (dev->frame_len)              { dev->injects_refused++; return false; }

    len = s5l_mtz2_encode(dev, c, n, (uint8_t)(dev->frame_seq + 1u),
                          dev->frame_ms + MTZ2_FRAME_PERIOD_MS, payload);
    if (!len || len > MTZ2_PAYLOAD_LIMIT) { dev->injects_refused++; return false; }

    memcpy(dev->frame, payload, len);
    dev->frame_len = (uint8_t)len;
    dev->contacts  = (uint8_t)n;
    dev->frame_seq++;
    /* Monotone and never zero: the parser logs "timestamp invalid!" on a zero
     * and "time travel, eh?" on a decrease, and posts notification 0x66 to the
     * driver for either. */
    dev->frame_ms += MTZ2_FRAME_PERIOD_MS;
    dev->frames_queued++;
    /* The attention line is a LEVEL and it goes up here. gpioic.c latches it
     * into group 4 bit 27 on the next refresh; it comes down at the last byte
     * of the data read that carries this report. */
    dev->atn = true;
    return true;
}
