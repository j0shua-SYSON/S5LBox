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
 *   finishStarting()        0xc0442670: `beq 0xc0442714` on false. False means
 *                           "Could not detect HBPP. Returning false from
 *                           finishStarting()" and the driver detaches. This
 *                           is the state run61 measured against the null
 *                           device, and it is the whole reason this file
 *                           exists.
 *   attemptToBootloadDevice 0xc04414c4: its first instruction after the
 *                           prologue is the same probe (`ldr pc,[r3,#0x4d0]`
 *                           at 0xc04414dc), and `subs r5,r0,#0 / bne` sends a
 *                           TRUE into the real HBPP firmware download — 54 KB
 *                           over a DMA controller this machine does not model.
 *                           It needs FALSE, whereupon it returns r5 = 0, the
 *                           retry loop at 0xc043a970 logs "Bootload attempt %d
 *                           of %d failed", and after three of those it calls
 *                           isBootloaded() (0xc043ac58) and believes the
 *                           firmware is already there.
 *
 * SO THIS DEVICE ANSWERS EVERY PROBE THE SAME WAY: it is in HBPP, and it stays
 * in HBPP. It is a part with no resident firmware sitting in its bootloader,
 * which is a state real silicon has. It is not a fabricated one-shot, and it
 * used to be — three earlier designs for this file were wrong, and the way each
 * was wrong is worth keeping:
 *
 *   1. "The second probe follows resetDevice(TRUE), so watch the reset line."
 *      There is no reset to watch. attemptToBootloadDevice's first act after
 *      its prologue is the probe (`ldr r3,[r0]` 0xc04414d0, `ldr pc,[r3,#0x4d0]`
 *      0xc04414dc) — no reset, no GPIO store, no delay, no power call — and
 *      resetDevice (0xc0443dd0) has one call site in the entire kext,
 *      0xc0438160, inside a virtual that neither finishStarting nor the
 *      bootload path reaches.
 *
 *   2. "Then accept the first probe and decline the rest." MEASURED WRONG.
 *      A run with `--call-probe-kernel 0xc0441008 --call-probe-kernel
 *      0xc0442670` shows finishStarting entered exactly once, at instruction
 *      220,700,146, and isInHBPP entered exactly once, at 316,898,121 with
 *      lr = 0xc04426cc — the return address of finishStarting's own dispatch.
 *      But spi1 carried TWO byte-identical 16-byte `1A A1 18 E1...` transfers,
 *      at 309.53M and 316.91M, from the same eleven call sites. So something
 *      inside finishStarting's preamble — one of v[0x4b4], v[0x42c], v[0x458]
 *      at 0xc0442688-0xc04426b0 — sends the probe pattern and never looks at
 *      the answer. A one-shot spends itself on that transfer and hands
 *      finishStarting the rejection. That is exactly the run this file was
 *      written to end, reproduced by the fix for it.
 *
 *   3. "Then decline at attemptToBootloadDevice, to skip the download."
 *      Backwards. Declining there makes it return 0 at 0xc04414e4 having done
 *      nothing, and the three "Bootload attempt %d of %d failed" lines never
 *      appear — because that printf is at 0xc043a9e8, inside v[0x36c]
 *      (0xc043a8d0), which is only reached ON A TRUE. The download is skipped
 *      by the retry loop failing, not by the probe failing, and the loop's
 *      per-attempt call at 0xc043a97c is v[0x378] = 0xc04382a0, a
 *      `callPlatformFunction` — not another probe and not a firmware push.
 *      v[0x36c] returns zero on every path it has (they all reach 0xc043aab4,
 *      `mov r0,#0`), so attemptToBootloadDevice is bounded whatever happens
 *      inside it.
 *
 * The cost of answering true is three cosmetic log lines. The 54 KB firmware
 * push and the unmodelled AppleARMPL080DMAC stay out of scope because the
 * driver has no firmware to push, not because we lied about the probe.
 *
 * ANSWERING TRUE ALSO CHOOSES THE FRAME OPCODE. The true return runs
 * `strb r3, [r4, #0x1bc]` at 0xc04426fc, and deviceReadResultData at
 * 0xc0441324 rejects 0xEB unless this+0x1bc is non-zero. So this device's Z2
 * frame opcode is downstream of the HBPP answer rather than independent of it,
 * and a model that declined the probe would also have to fall back to 0xEA.
 *
 * `hbpp` is still state rather than a constant, because the reset line really
 * does end it: a part that has just reset runs whatever firmware it has. On
 * this boot path nothing drives that line (see s5l_mtz2_reset_pin), so nothing
 * ends it, which is the correct behaviour for a device that has never been
 * given firmware.
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
         * The out-of-HBPP answer: sixteen zeros, opcode byte included, and
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
    if (dev->op == MTZ2_OP_HBPP && dev->hbpp) return out;
    return pos < S5L_MTZ2_BUF ? dev->rsp[pos] : 0u;
}

static uint8_t mtz2_transfer(void *ctx, uint8_t out) {
    s5l_mtz2_t *dev = ctx;
    if (!dev) return 0u;

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
        if (dev->op == MTZ2_OP_HBPP) dev->hbpp_probes++;
        dev->pos = 0u;
        dev->len = 0u;
    }
    return v;
}

/* ============================================================== lifecycle === */

void s5l_mtz2_reset(s5l_mtz2_t *dev) {
    if (!dev) return;
    memset(dev, 0, sizeof *dev);

    /* Power-on answer. finishStarting() is the first thing to probe and it is
     * the caller that needs a true. */
    dev->hbpp = true;

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
     * `function-reset`, GPIO 0x0606 = group 6 bit 6.
     *
     * THIS LINE IS REALLY DRIVEN, and finding that out cost a boot. An earlier
     * revision made a reset LEAVE HBPP, on the reasoning that a part which has
     * just reset is running its resident firmware. Measured: the guest writes
     * the GPIO function-select register 77 times during driver start, and one
     * of those stores lands on this pin BETWEEN the un-evaluated probe at
     * instruction 309.53M and finishStarting's own at 316.91M — so the device
     * left HBPP one transfer before the only transfer whose answer anybody
     * read, and the boot ended in exactly the "Could not detect HBPP" the
     * device exists to prevent. Both probes are visible in the spi1 register
     * trace: the first came back echoed, the second came back as sixteen zeros.
     *
     * The reasoning was wrong, not just the outcome. A part with resident
     * firmware boots it; a part with NONE — which is what this device is, and
     * why the driver is about to try to bootload it — comes out of reset back
     * into its bootloader, every time. So a reset RESTORES HBPP here. That is
     * the physically correct answer for an unprogrammed Z2 and it is also the
     * one that keeps the driver attached, which is a good sign rather than a
     * coincidence.
     *
     * Either edge, because the pulse is two stores and the model must not
     * depend on which of them it sees. A partly received packet does not
     * survive a reset.
     */
    (void)level;
    dev->hbpp = true;
    dev->pos  = 0u;
    dev->len  = 0u;
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
