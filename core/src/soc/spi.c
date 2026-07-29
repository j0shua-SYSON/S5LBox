/*
 * S5LBox — Samsung S5L8900 SPI controller.
 *
 * See the SPI section of core/include/soc.h for the register map, the evidence
 * behind each field, and the one rule this model is built around: the stock
 * interrupt handler bails out without acknowledging anything when the receive
 * FIFO is empty, so a line raised over an empty FIFO reproduces the unbounded
 * commandSleep() rather than ending it.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <string.h>

void s5l_spi_reset(s5l_spi_t *bus) {
    if (!bus) return;
    /* Total, exactly as the I2C controller's reset is: besides making reset
     * semantics explicit, initializing everything makes this safe on a stack
     * object holding any prior byte pattern. Board wiring is attached after. */
    memset(bus, 0, sizeof *bus);
}

bool s5l_spi_attach(s5l_spi_t *bus, unsigned cs, const s5l_spi_slave_t *slave) {
    if (!bus || !slave || !slave->transfer || cs >= S5L_SPI_SLAVES)
        return false;
    if (bus->slaves[cs].transfer) return false;   /* never shadow silently */
    bus->slaves[cs] = *slave;
    return true;
}

static void note_unknown(s5l_spi_t *bus, uint32_t off) {
    for (unsigned i = 0; i < bus->unknown_off_count; i++)
        if (bus->unknown_off[i] == off) return;
    if (bus->unknown_off_count < S5L_SPI_UNKNOWN_OFF)
        bus->unknown_off[bus->unknown_off_count++] = off;
}

/* Depth eight, so shifting the array down on a pop costs seven bytes and buys a
 * representation with no head/tail to wrap — one a snapshot validator can check
 * with `level <= depth` and nothing else. */
static uint8_t fifo_pop(uint8_t *fifo, uint8_t *level) {
    uint8_t v = fifo[0];
    (*level)--;
    memmove(fifo, fifo + 1, *level);
    fifo[*level] = 0;
    return v;
}

/*
 * Move whole words between the two FIFOs.
 *
 * A byte leaves the transmit FIFO and one arrives in the receive FIFO at the
 * same instant, so the shifter can only run while the receive FIFO has room.
 * That is the hardware's own backpressure and it is why loading SPI_RXDATA
 * restarts a transfer that had backed up.
 *
 * A chip select with no attached device does NOT shift. Answering for a device
 * this machine does not have would be fabricating exactly the value the guest
 * is waiting for, which is the one thing this core's storage windows are
 * forbidden to do — and it would change spi0, whose panel is unmodelled and
 * which today stalls in precisely this way. Attaching a slave is the
 * declaration that a device exists; see s5l_spi_null_bind().
 */
static void spi_shift(s5l_spi_t *bus) {
    const s5l_spi_slave_t *slave =
        bus->cs < S5L_SPI_SLAVES ? &bus->slaves[bus->cs] : NULL;
    if (!slave || !slave->transfer) return;

    /*
     * WHETHER A FULL RECEIVE FIFO STOPS THE SHIFTER, and it depends on who is
     * meant to be emptying it.
     *
     * In PIO the CPU is, one RXDATA load at a time, and stalling is what makes
     * a sixteen-octet command work against an eight-deep FIFO: the driver
     * writes sixteen, the first eight shift, the rest wait in the transmit
     * FIFO, and each RXDATA read makes room for one more. Shifting regardless
     * there would throw away half of every reply.
     *
     * In DMA there is NO reader. run104 measured what that costs: the DMAC
     * moved 812,340 octets into this port from a channel whose destination is
     * SPI1's TXDATA, and the digitizer received sixteen of them -- eight
     * shifted, eight sat in the transmit FIFO, and every write after that was
     * counted in tx_drops and discarded.
     *
     * That is why this loop does not stall in DMA mode, and it worked: as of
     * run151 the download delivers. `spi1 words 54236 tx-drops 0` -- nothing
     * discarded -- and the device frames all 54,154 octets as ONE HBPP DATA
     * packet. An earlier version of this comment ended "which is why the
     * bootload has never delivered a packet", and that sentence outlived the
     * problem it described by three commits.
     *
     * The bootload still does not COMPLETE, but the reason is no longer here:
     * the driver sleeps after the transfer waiting for spi1's interrupt to run
     * finishTransfer and call commandWakeup. See s5l_spi_irq() below, and
     * docs/multitouch.md 6.14.
     *
     * Silicon does not stall a shifter on a full receive FIFO; it overruns.
     * So in DMA mode this does too, and the answer that had nowhere to go is
     * counted rather than pretended away.
     */
    const bool dma = (bus->setup & SPI_SETUP_DMA) != 0u;
    while (bus->tx_level && (dma || bus->rx_level < S5L_SPI_FIFO_DEPTH)) {
        uint8_t out = fifo_pop(bus->tx, &bus->tx_level);
        uint8_t in  = slave->transfer(slave->ctx, out);
        if (bus->rx_level < S5L_SPI_FIFO_DEPTH) bus->rx[bus->rx_level++] = in;
        else                                    bus->rx_overruns++;
        bus->words++;
        if (bus->words_left) bus->words_left--;
        /*
         * Raise the whole event mask rather than one bit of it. The four
         * latches are not individually identified anywhere in the driver: it
         * decides what to do from the receive level and never tests an
         * individual latch, and the only two values it ever stores into STATUS
         * are the whole mask (0x0f, from this+0xAC at power-on, transfer setup
         * and finishTransfer) and the raw word it just read. Raising the set
         * together is a stated over-approximation whose only guest-visible
         * consequence is that one W1C clears them all — which is exactly what
         * the driver does anyway.
         */
        bus->status |= SPI_STATUS_EVENTS;
    }
}

static uint32_t spi_status(const s5l_spi_t *bus) {
    return (bus->status & SPI_STATUS_EVENTS) |
           ((uint32_t)bus->tx_level << SPI_STATUS_TX_SHIFT) |
           ((uint32_t)bus->rx_level << SPI_STATUS_RX_SHIFT);
}

uint32_t s5l_spi_read(s5l_spi_t *bus, uint32_t off) {
    if (!bus) return 0;
    switch (off) {
        case SPI_CONTROL: return bus->control;
        case SPI_SETUP:   return bus->setup;
        case SPI_STATUS:  return spi_status(bus);
        case SPI_PIN:     return bus->pin;
        case SPI_TXDATA:  return 0u;   /* write-only on the decoded path */
        case SPI_RXDATA: {
            if (!bus->rx_level) { bus->rx_underruns++; return 0u; }
            uint32_t v = fifo_pop(bus->rx, &bus->rx_level);
            spi_shift(bus);            /* the room this just made restarts it */
            return v;
        }
        case SPI_CLKDIV:  return bus->clkdiv;
        case SPI_CNT:     return bus->cnt;
        case SPI_IDD:     return bus->idd;
        default:
            bus->unknown_reads++;
            note_unknown(bus, off);
            return 0u;
    }
}

void s5l_spi_step(s5l_spi_t *bus) {
    if (bus) spi_shift(bus);
}

void s5l_spi_write(s5l_spi_t *bus, uint32_t off, uint32_t val) {
    if (!bus) return;
    switch (off) {
        case SPI_CONTROL:
            /*
             * Stored, with no side effect. Nothing here flushes a FIFO on a
             * control or setup store: which bit does that on real silicon is
             * not established by any driver code we can read, and discarding a
             * byte the guest is about to wait for is the exact failure this
             * model exists to end. A guest that wants an empty receive FIFO
             * reads it empty.
             */
            bus->control = val;
            break;
        case SPI_SETUP:
            /*
             * The DMA bit is COUNTED, not just stored. Reporting it from the
             * register's value at the end of a run cannot tell "the driver
             * never asked for DMA" apart from "the driver armed DMA, ran a
             * transfer and disarmed it" -- and the first of those was recorded
             * as measured fact and used to retract a fix. See soc.h.
             */
            if ((val & SPI_SETUP_DMA) && !(bus->setup & SPI_SETUP_DMA))
                bus->dma_arms++;
            bus->setup = val;
            break;
        case SPI_STATUS:
            /* Write-one-to-clear, and only over the event latches. This is not
             * hypothetical robustness: the filter acknowledges by storing back
             * the whole raw word it just read (0xc05a67d0), levels included, so
             * anything that treated those bits as writable would zero them on
             * the first acknowledge. They are computed in spi_status(). */
            bus->status &= ~(val & SPI_STATUS_EVENTS);
            break;
        case SPI_PIN:
            /* The internal chip select, active low in bit 1 — the driver bics
             * it to assert and orrs it to release (0xc05a68a4). Storage here:
             * `internal-cs` appears nowhere in the shipped tree, so both
             * controllers report `_spiInternalCS = 0` and drive their real
             * select lines through GPIO functions we do not model. The only
             * store this register sees on the touch path is the zero written
             * at power-on. */
            bus->pin = val;
            break;
        case SPI_TXDATA:
            if (bus->tx_level >= S5L_SPI_FIFO_DEPTH) { bus->tx_drops++; break; }
            bus->tx[bus->tx_level++] = (uint8_t)val;
            spi_shift(bus);
            break;
        case SPI_RXDATA:
            /* Read-only. A guest store must not be able to inject a byte the
             * device never sent. */
            break;
        case SPI_CLKDIV:
            bus->clkdiv = val;
            break;
        case SPI_CNT:
            /*
             * The word count — max(txLen, rxLen), chosen at 0xc05a6b3c —
             * latched so the remaining length is visible.
             *
             * It deliberately does NOT gate the shifter, and that is settled
             * rather than cautious: the filter decides a transfer is over from
             * its own three software counters and never tests a hardware
             * "done" bit (0xc05a67d4-0xc05a67ec), so the count is not what ends
             * a transfer. It also cannot safely mean "shift nothing" when zero,
             * because the driver's own DMA path stores zero here (0xc05a6b10)
             * and run23 caught BasebandSPI doing the same while configuring a
             * controller it went on to use. A count this model misread into a
             * gate would stall a transfer the guest is asleep waiting for,
             * which is the failure being fixed.
             */
            bus->cnt = val;
            bus->words_left = val;
            break;
        case SPI_IDD:
            bus->idd = val;
            break;
        default:
            bus->unknown_writes++;
            note_unknown(bus, off);
            break;
    }
}

bool s5l_spi_irq(const s5l_spi_t *bus) {
    /*
     * A level, exactly as the I2C controller's is, held until the guest clears
     * the event latches. Three terms, and every one of them earns its place.
     *
     * The receive-level term is the whole point of this model. The stock filter
     * reads STATUS and returns false at 0xc05a66e4 without acknowledging
     * anything when ((status >> 8) & 0xF) is zero, so its action is never
     * scheduled, finishTransfer never runs and nothing calls commandWakeup:
     * raising the line over an empty receive FIFO would reproduce the unbounded
     * sleep instead of ending it.
     *
     * The SETUP term is what keeps the line out of the driver's own prefill.
     * The driver fills the transmit FIFO, arms, and only then enables the
     * interrupt (SETUP |= 0x180); firing before that last store would run the
     * filter against transfer counts it has not finished setting up. It also
     * drops the line for free at both places the driver stops wanting it — the
     * filter's `bic #0x100` when the counts run out, and finishTransfer's write
     * of the bare base word.
     */
    return bus && (bus->setup & SPI_SETUP_IRQ) != 0u &&
           (bus->status & SPI_STATUS_EVENTS) != 0u && bus->rx_level != 0u;
}

/* --------------------------------------------------------- the null device --- */

static uint8_t null_transfer(void *ctx, uint8_t out) {
    (void)ctx; (void)out;
    return 0x00u;
}

void s5l_spi_null_bind(s5l_spi_slave_t *slave) {
    if (!slave) return;
    memset(slave, 0, sizeof *slave);
    slave->transfer = null_transfer;
}
