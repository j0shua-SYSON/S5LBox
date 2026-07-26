/*
 * iOS3-VM — Samsung S5L8900 system-on-chip model.
 *
 * The S5L8900 is the application processor in the original iPhone, the iPhone
 * 3G, and the iPod touch 1G — the silicon iPhone OS 1–3 ran on. This header
 * declares its memory map and the device models the CPU talks to over the bus.
 *
 * The peripheral base addresses below started as public reverse-engineering of
 * the S5L8900 and are now derived from the shipped firmware itself — see the
 * memory-map block below for the device-tree ranges every one of them is
 * resolved through, and for the two addresses that are ours rather than the
 * SoC's (the NOR window) or the SoC's but unmodelled (edram, vrom, SRAM).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_SOC_H
#define IOS3VM_SOC_H

#include <stddef.h>
#include <stdint.h>
#include "arm.h"

/* ------------------------------------------------------------ memory map
 *
 * CONFIRMED from the shipped device tree (firmware/devicetree.bin, iPhone1,2
 * 7E18). /arm-io carries two ranges triples — (child, parent, size):
 *
 *     00000000 38000000 08000000     child 0x00000000.. -> phys 0x38000000..
 *     10000000 18000000 10000000     child 0x10000000.. -> phys 0x18000000..
 *
 * so a peripheral's `reg` child address becomes a physical address by adding
 * 0x38000000 in the first window and 0x08000000 in the second. Two independent
 * anchors pin the second window, which is the one that matters here:
 *
 *   /arm-io/vrom  reg {0x18000000,0x10000}  -> phys 0x20000000  (the boot ROM,
 *                 the publicly known S5L8900 secure-ROM address)
 *   /arm-io/amc   reg[1] {0x1a000000,0x2c000} -> phys 0x22000000, and
 *                 firmware/llb.bin carries 0x22000000 at file+0x310 as its own
 *                 link address — the bootrom loads LLB into SRAM there.
 *
 * The resulting physical map, with what is and is not modelled:
 *
 *   0x08000000  DRAM base. 128 MB fitted on an iPhone1,2 (ends 0x10000000).
 *   0x18000000  edram, 0x140000                    NOT MODELLED
 *   0x20000000  vrom (boot ROM), 0x10000           NOT MODELLED
 *   0x22000000  SRAM / AMC window, 0x2c000         NOT MODELLED
 *   0x28000000  <- first physical byte the device tree assigns to NOTHING
 *   0x38000000  arm-io, 0x08000000 — every modelled peripheral lives here
 *
 * The NOR is NOT in this map. The device tree describes it as an SPI slave —
 * /arm-io/spi0/nor-flash, compatible "nor-flash,spi", with a flash-RELATIVE
 * address space (ranges {0,0,0x100000}, which is where S5L8900_NOR_SIZE's 1 MiB
 * comes from) and partitions at flash offsets (nvram @0xfc000, raw-device
 * @0x8000). See S5L8900_NOR_BASE for what our memory window therefore is.
 *
 * The DRAM aperture ceiling: DRAM starts at 0x08000000 and the next thing the
 * SoC decodes is edram at 0x18000000, so no DRAM configuration above 256 MB can
 * be physically real on this part. We allow larger anyway (a RAM-disk root does
 * not fit otherwise); s5l8900_ram_conflict() is what keeps that fiction from
 * becoming a silent alias.
 */
#define S5L8900_SRAM_BASE   0x22000000u   /* SRAM / AMC window, size 0x2c000 */
#define S5L8900_SRAM_SIZE   0x0002c000u
#define S5L8900_VROM_BASE   0x20000000u   /* boot ROM                        */
#define S5L8900_VROM_SIZE   0x00010000u
#define S5L8900_EDRAM_BASE  0x18000000u
#define S5L8900_EDRAM_SIZE  0x00140000u
#define S5L8900_SDRAM_BASE  0x08000000u
#define S5L8900_ARMIO_BASE  0x38000000u   /* /arm-io ranges parent           */
#define S5L8900_ARMIO_SIZE  0x08000000u
#define S5L8900_UART0_BASE  0x3cc00000u
#define S5L8900_VIC0_BASE   0x38e00000u
#define S5L8900_TIMER_BASE  0x3e200000u
#define S5L8900_CLOCK_BASE  0x3c500000u
/*
 * GPIO. CONFIRMED against two independent sources: the shipped device tree's
 * /arm-io/gpio has reg {0x6400000,0x1000} and arm-io maps child+0x38000000, and
 * a walk of the guest's live page tables from the VA AppleS5L8900XGPIOIC prints
 * lands on the same page. The value here was previously 0x3cf00000 — the
 * S5L8720-era GPIO address, wrong for this SoC. It was unused, so it was a
 * landmine rather than a live bug; it is corrected rather than left to be
 * "confirmed" by whoever wired it up next.
 */
#define S5L8900_GPIO_BASE   0x3e400000u
#define S5L8900_MIU_BASE    0x38100000u   /* clkrstgen's second reg range */
#define S5L8900_EDGEIC_BASE 0x38e02000u
#define S5L8900_DEV_SIZE    0x00001000u   /* per-peripheral window */

/*
 * The three SPI controllers, confirmed the same two ways GPIO was.
 * /arm-io/spi0, /arm-io/spi1 and /arm-io/spi2 carry reg {0x4300000,0x1000},
 * {0x4e00000,0x1000} and {0x5200000,0x1000}, and arm-io maps child+0x38000000;
 * run23's non-RAM page report independently attributes live guest traffic at
 * each resulting physical page to AppleS5L8900XSPIController (spi0/spi1) and
 * com.apple.driver.BasebandSPI (spi2).
 *
 * spi2 is the baseband transport: compatible "spi,s5l8900x,baseband", with the
 * SRDY and MRDY handshake lines exposed as GPIO platform functions and DMA
 * channel descriptors pointing at 0x3d200010/0x3d200020.
 *
 * Their device-tree interrupt numbers are NOT three numbers of the same kind,
 * and writing them as "9/10/7" conflated two different interrupt controllers.
 * From the shipped tree:
 *
 *   /arm-io/spi0  interrupts {0x9}      interrupt-parent 0x00b04cc0 = /arm-io/vic
 *   /arm-io/spi1  interrupts {0xa}      interrupt-parent 0x00b04cc0 = /arm-io/vic
 *   /arm-io/spi2  interrupts {0x7,0x2}  interrupt-parent 0x00b05320 = /arm-io/gpio
 *
 * so spi0 and spi1 really are VIC vectors 9 and 10, while spi2's 7 is a GPIO
 * interrupt — a two-cell specifier, /arm-io/gpio having #interrupt-cells 2 —
 * on a controller that is itself a VIC child. It is emphatically not VIC
 * vector 7, which on this SoC is the timer (S5L8900_IRQ_TIMER).
 *
 * spi0 and spi1 are now device models and their two VIC lines are defined and
 * wired (see the SPI section further down). spi2's is deliberately still not:
 * its interrupts are GPIO lines on a controller this machine models as storage,
 * so nothing could route them, and a constant that looks wired but is not is
 * the kind of landmine the GPIO base above was.
 *
 * spi2 therefore remains a declared window rather than a device model. It
 * exists so the traffic is named and stored rather than reading back as the
 * zero an unmapped access returns — run23 caught BasebandSPI writing a
 * configuration block to spi2 and later reading those same four registers back
 * to build a transfer descriptor, which an unmapped window answered with zeros.
 */
#define S5L8900_SPI0_BASE   0x3c300000u
#define S5L8900_SPI1_BASE   0x3ce00000u
#define S5L8900_SPI2_BASE   0x3d200000u
#define S5L8900_IRQ_SPI0    9u
#define S5L8900_IRQ_SPI1    10u
#define S5L8900_SPI_COUNT   2u

/*
 * The Synopsys DesignWare USB 2.0 OTG (DWC2) controller. CONFIRMED from the
 * shipped device tree the same way the SPI trio above was: /arm-io/usb-otg has
 * reg {0x00400000,0x1000} and compatible "usb-otg,s5l8900x", and /arm-io maps
 * child + 0x38000000.
 *
 * Only the hardware-configuration registers are modelled. See the DWC2 section
 * further down for exactly which four, why those values, and what this
 * deliberately does not claim to be.
 */
#define S5L8900_USB_OTG_BASE 0x38400000u

/*
 * There are TWO PL192 VICs, not one. The device tree gives the block as
 * reg {0xe00000, 0x2000} with vic-stride 0x1000, and AppleARMPL192VIC maps both
 * pages. The interrupt numbering is flat across the pair: /arm-io/gpio lists
 * lines 0x20 and 0x21, /arm-io/wdt line 0x33, /arm-io/sdio line 0x2a — all past
 * VIC0's 32 lines, so they can only be VIC1 lines 0, 1, 19 and 10.
 */
#define S5L8900_VIC1_BASE   0x38e01000u
#define S5L8900_VIC_COUNT   2u

/* --------------------------------------------------------------- UART ---
 * Samsung-style UART (as on S3C-family parts). Writing a byte to UTXH
 * transmits it; UTRSTAT reports the transmitter permanently ready, so guest
 * "wait until TX empty" spin loops make progress immediately.
 */
#define UART_ULCON   0x00u
#define UART_UCON    0x04u
#define UART_UFCON   0x08u
#define UART_UMCON   0x0cu
#define UART_UTRSTAT 0x10u
#define UART_UERSTAT 0x14u
#define UART_UFSTAT  0x18u
#define UART_UMSTAT  0x1cu
#define UART_UTXH    0x20u
#define UART_URXH    0x24u
#define UART_UBRDIV  0x28u

#define UART_TX_BUFFER 8192

typedef struct {
    uint32_t ulcon, ucon, ufcon, umcon, ubrdiv;
    char     tx[UART_TX_BUFFER];   /* everything the guest has printed */
    size_t   tx_len;
} s5l_uart_t;

void     s5l_uart_reset(s5l_uart_t *u);
uint32_t s5l_uart_read(s5l_uart_t *u, uint32_t off);
void     s5l_uart_write(s5l_uart_t *u, uint32_t off, uint32_t val);

/* ---------------------------------------------------------------- VIC ---
 * PL190-style vectored interrupt controller. Devices assert lines into `raw`;
 * the controller ORs in software interrupts, masks by `enable`, and routes each
 * line to IRQ or FIQ according to `select`.
 */
#define VIC_IRQSTATUS    0x00u
#define VIC_FIQSTATUS    0x04u
#define VIC_RAWINTR      0x08u
#define VIC_INTSELECT    0x0cu
#define VIC_INTENABLE    0x10u
#define VIC_INTENCLEAR   0x14u
#define VIC_SOFTINT      0x18u
#define VIC_SOFTINTCLEAR 0x1cu
#define VIC_VECTADDR0    0x100u  /* per-source ISR address bank, 32 entries    */
#define VIC_VECTADDR     0xf00u  /* PL192 vectored dispatch: read = source|bit31, write = EOI */

typedef struct { uint32_t raw, enable, select, soft; } s5l_vic_t;

void     s5l_vic_reset(s5l_vic_t *v);
uint32_t s5l_vic_read(s5l_vic_t *v, uint32_t off);
void     s5l_vic_write(s5l_vic_t *v, uint32_t off, uint32_t val);
void     s5l_vic_set_line(s5l_vic_t *v, unsigned line, bool level);
bool     s5l_vic_irq(const s5l_vic_t *v);
bool     s5l_vic_fiq(const s5l_vic_t *v);
/* PL192 VICADDRESS: highest-priority pending source tagged with bit 31, or 0.
 * base_source positions this VIC in the daisy chain (0 for VIC0, 32 for VIC1). */
uint32_t s5l_vic_vectaddr(const s5l_vic_t *v, unsigned base_source);

/* -------------------------------------------------------------- timer ---
 * The real S5L8900 timer block, as used by XNU rather than as invented by us.
 *
 * The layout below is not guesswork: instrumenting the bus and correlating each
 * access against the kernel's own symbols shows exactly which registers matter
 * and what it expects of them.
 *
 *   _s5l8900x_get_timebase    reads 0x080/0x084 as a free-running 64-bit
 *                             counter. This is mach_absolute_time(). It must
 *                             count whether or not any timer is "enabled", or
 *                             every delay loop in the kernel spins forever.
 *   _pe_arm_init_interrupts   programs timer 4 at 0x0A0-0x0AF and then routes
 *                             VIC line 7 to *FIQ*, not IRQ.
 *   _s5l8900x_set_decrementer writes the next deadline to 0x0A8.
 *   _fleh_fiq_s5l8900x        acknowledges by writing 0x00030000 to the latch.
 *
 * That acknowledge mask is load-bearing. Latching any other bit pattern leaves
 * the line asserted after the handler returns, which produces an interrupt
 * storm rather than a scheduler tick.
 *
 * The status alias at 0x10000 sits outside the usual 4 KB peripheral window,
 * which is why the timer's window is widened (see S5L8900_TIMER_SIZE).
 */
#define TIMER_TICKSHIGH  0x0080u   /* free-running counter, high word */
#define TIMER_TICKSLOW   0x0084u   /* free-running counter, low word  */
#define TIMER_CONFIG     0x0088u
#define TIMER4_CONFIG    0x00a0u
#define TIMER4_STATE     0x00a4u   /* bit0 start, bit1 update-from-buffer */
#define TIMER4_COUNTBUF  0x00a8u   /* deadline written by set_decrementer  */
#define TIMER4_COUNTBUF2 0x00acu
#define TIMER4_VALUE     0x00b4u   /* live down-count */
#define TIMER_IRQACK     0x00f4u   /* write-1-to-clear */
#define TIMER_IRQLATCH   0x00f8u
#define TIMER_IRQSTATUS  0x10000u  /* alias, outside the 4 KB window */

#define TIMER4_STATE_START  (1u << 0)
#define TIMER4_STATE_UPDATE (1u << 1)
#define TIMER4_IRQ_BITS  0x00030000u  /* what _fleh_fiq_s5l8900x acks */

/* Widened so TIMER_IRQSTATUS at 0x10000 falls inside the timer's window. */
#define S5L8900_TIMER_SIZE 0x00011000u

#define S5L8900_IRQ_TIMER 7u     /* VIC line the timer drives (routed to FIQ) */

/*
 * The guest's clocks, as we advertise them to it in the device tree.
 *
 * This ratio is a real design parameter, not bookkeeping. On the hardware the
 * CPU runs at 412 MHz while the timebase counts at 6 MHz, so one timebase tick
 * costs about 68 CPU cycles. Advancing the timebase once per retired
 * instruction instead makes guest time run ~68x fast relative to guest work,
 * and the kernel then cannot finish servicing one timer deadline before the
 * next one is already in the past: it clamps the decrementer to its minimum,
 * re-enters immediately, and livelocks. That is not a hypothetical -- it burned
 * 66% of a 200M-instruction boot in the FIQ handler.
 *
 * During active execution one retired instruction is treated as one CPU
 * cycle, which is optimistic for an ARM1176 but keeps the ratio honest in the
 * direction that matters.  WFI can advance the same clock without retiring
 * instructions; cpu.cycles remains a retired-instruction counter.
 */
#define S5L8900_CPU_HZ 412000000u
#define S5L8900_TB_HZ    6000000u

typedef struct {
    uint64_t ticks;              /* free-running; never gated by any enable */
    uint32_t config;
    uint32_t t4_config, t4_state;
    uint32_t t4_count, t4_count2, t4_value;
    uint32_t irqlatch;
} s5l_timer_t;

void s5l_timer_reset(s5l_timer_t *t);
uint32_t s5l_timer_read(s5l_timer_t *t, uint32_t off);
void s5l_timer_write(s5l_timer_t *t, uint32_t off, uint32_t val);
/* Advance by `ticks`; returns true while an interrupt is pending. */
bool s5l_timer_tick(s5l_timer_t *t, uint32_t ticks);

/* ----------------------------------------------------------- NOR flash ---
 * On the S5L8900 the low-level boot images (LLB, iBoot, the device tree, the
 * boot logo) live in a small NOR flash reached over SPI. We model it as a
 * read-only memory-mapped region plus a directory built by *scanning* for IMG3
 * containers.
 *
 * Scanning is deliberate: Apple's exact NOR image-table layout for this SoC
 * could not be verified from a primary source, and guessing a structure would
 * be a silent source of wrong behaviour. Scanning for the IMG3 magic works
 * whatever the surrounding directory format turns out to be, and can be
 * replaced with a real table reader once the layout is confirmed against a
 * genuine dump.
 */
/*
 * WHERE THE NOR WINDOW IS, AND WHY IT MOVED.
 *
 * This aperture is OURS, not the SoC's. The shipped device tree does not map
 * the NOR into the physical address space at all: it is an SPI slave under
 * /arm-io/spi0 with a flash-relative 1 MiB space (see the memory-map block at
 * the top of this header). We expose it as memory because that is the cheapest
 * honest way to give a guest payload the read/program path an untethered
 * jailbreak needs, without a full SPI protocol model.
 *
 * Because the address is ours, it has to be chosen so that it cannot collide
 * with anything — and 0x24000000, the value this had until now, failed that.
 * It sat inside the DRAM window we actually boot with (-R 512 gives DRAM
 * 0x08000000..0x28000000), and since bus_read tested RAM first, every NOR read
 * silently returned RAM-disk bytes. Nothing faulted and nothing logged.
 * 0x24000000 also had no source: it appears in no shipped artifact — not in the
 * device tree, not in firmware/llb.bin — and the commit that introduced it
 * cited none.
 *
 * 0x28000000 is picked from evidence, and two independent lines land on it:
 *
 *   - it is the first physical byte the shipped device tree assigns to nothing:
 *     /arm-io's second range covers phys 0x18000000..0x28000000 and its first
 *     covers 0x38000000..0x40000000;
 *   - it is the first byte above the largest DRAM the kernel can use. xnu-1357
 *     arm_vm_init fixes virtual_avail at 0xe0000000, so with gVirtBase
 *     0xc0000000 mem_size cannot exceed 512 MB, and DRAM cannot reach past
 *     0x08000000 + 512 MB = 0x28000000.
 *
 * So no RAM aperture this machine will accept can reach it, and no device tree
 * region claims it. Should either of those stop being true, s5l8900_init()
 * refuses to build the machine rather than aliasing anything — see
 * s5l8900_ram_conflict().
 */
#define S5L8900_NOR_BASE 0x28000000u
#define S5L8900_NOR_SIZE 0x00100000u   /* 1 MiB — /arm-io/spi0/nor-flash ranges */
#define S5L_NOR_MAX_IMAGES 16

typedef struct {
    uint32_t ident;     /* IMG3 ident, e.g. 'illb', 'ibot', 'dtre' */
    uint32_t offset;    /* byte offset within the NOR              */
    uint32_t size;      /* container size (fullSize)               */
} s5l_nor_entry_t;

typedef struct {
    uint8_t        *data;
    uint32_t        size;
    s5l_nor_entry_t images[S5L_NOR_MAX_IMAGES];
    unsigned        image_count;
} s5l_nor_t;

/* NOR erase granularity. Programming can only clear bits (as on real flash);
 * an erase restores a whole sector to 0xFF. */
#define S5L8900_NOR_SECTOR 0x1000u

bool     s5l_nor_init(s5l_nor_t *n, uint32_t size);
void     s5l_nor_free(s5l_nor_t *n);
uint32_t s5l_nor_read(const s5l_nor_t *n, uint32_t off, unsigned bytes);
/* Copy `len` bytes into the NOR at `off` (as a factory flasher would). This is
 * an unconditional overwrite, used to lay down an initial image. */
void     s5l_nor_program(s5l_nor_t *n, uint32_t off, const void *src, size_t len);

/*
 * Flash-accurate programming: bits can only go 1 -> 0. Returns false if the
 * write would need to set a bit back to 1 (erase the sector first) or is out of
 * range. This is the path guest writes take, so a guest payload can persist
 * itself into NOR — which is exactly what an untethered jailbreak such as
 * 24kpwn does on this SoC.
 */
bool     s5l_nor_write(s5l_nor_t *n, uint32_t off, uint32_t val, unsigned bytes);

/* Erase one sector back to all-ones. */
bool     s5l_nor_erase_sector(s5l_nor_t *n, uint32_t off);
/* Rebuild the image directory by scanning for IMG3 containers. */
unsigned s5l_nor_scan(s5l_nor_t *n);
/* Find a scanned image by ident; returns NULL if absent. */
const s5l_nor_entry_t *s5l_nor_find(const s5l_nor_t *n, uint32_t ident);

#define S5L_UNMAPPED_LOG 32
#define S5L_DEVLOG        256

/* --------------------------------------------------------- stub windows ---
 * A named, register-backed MMIO window for a peripheral we have identified but
 * not yet modelled.
 *
 * This is a deliberate, bounded exception to this core's "trap what you don't
 * implement" rule, and it is worth being precise about why. For an MMIO window
 * there is no option that avoids making a claim: returning 0 for an unmapped
 * read is *already* a guess, and a demonstrably dangerous one — a driver
 * polling a status bit that reads 0 forever spun on one such window for
 * 3.9 million reads, about 2% of an entire boot.
 *
 * A stub is therefore honest storage rather than invented behaviour: reads
 * return what was last written, writes are recorded, and every window is named
 * and counted so it appears in the report instead of hiding. What a stub must
 * never do is fabricate a value the guest is waiting for. When a driver needs a
 * bit to change on its own, that is a real device model, not a stub, and it
 * belongs in its own file.
 */
/* ------------------------------------------------------ power controller ---
 * The power-gate block. See core/src/soc/power.c for why this is a real model
 * rather than a stub: the guest never writes STATE, so read-back storage would
 * leave it reading zero forever — which is exactly what wedged the boot.
 *
 * Note the page is SHARED. Only 0x00-0x7F belongs to the power controller;
 * 0x80 and above is the GPIO interrupt controller, a different block with a
 * different driver.
 */
#define S5L8900_POWER_BASE   0x39a00000u
#define S5L8900_POWER_SIZE   0x00000080u   /* the rest of the page is GPIOIC */
#define S5L8900_GPIOIC_BASE  0x39a00080u
#define S5L8900_GPIOIC_SIZE  0x00000f80u

#define POWER_CONFIG0   0x00u
#define POWER_CONFIG1   0x04u
#define POWER_SETSTATE  0x08u
#define POWER_ONCTRL    0x0cu   /* write 1 to ungate: clears the STATE bit */
#define POWER_OFFCTRL   0x10u   /* write 1 to gate:   sets the STATE bit   */
#define POWER_STATE     0x14u   /* bit n set == domain n is gated OFF      */
#define POWER_SRAM      0x20u
#define POWER_CFG24     0x24u
#define POWER_CFG28     0x28u

/* 14 domains; the driver masks with 0x3fff and the device tree lists 14. */
#define S5L_POWER_DOMAIN_MASK    0x00003fffu
/* The device tree's power-gate-defaults. See s5l_power_reset. */
#define S5L_POWER_GATE_DEFAULTS  0x000012fcu

typedef struct {
    uint32_t state, cfg0, cfg1, sram, cfg24, cfg28;
} s5l_power_t;

void     s5l_power_reset(s5l_power_t *p);
uint32_t s5l_power_read(s5l_power_t *p, uint32_t off);
void     s5l_power_write(s5l_power_t *p, uint32_t off, uint32_t val);

/* ---------------------------------------------------- CLCD display controller ---
 * The path to pixels. See core/src/soc/clcd.c for the evidence behind every
 * register below; the short version is that AppleH1CLCD's own code was read out
 * of the shipped kernelcache and each offset here is a line of that code.
 *
 * Base and interrupt line are CONFIRMED from the shipped device tree:
 *   /arm-io/clcd  reg {0x900000, 0x1000}  interrupts {0xd}
 * and /arm-io ranges maps child + 0x38000000.
 */
#define S5L8900_CLCD_BASE 0x38900000u
#define S5L8900_IRQ_CLCD  13u

#define CLCD_ENABLE      0x000u  /* write 1: start scanout (written last)      */
#define CLCD_DISABLE     0x004u  /* write 1: stop scanout                      */
#define CLCD_CTRL        0x008u  /* display + per-window enables               */
#define CLCD_FIFO        0x00cu  /* per-window FIFO thresholds                 */
#define CLCD_INTMASK     0x014u  /* interrupt enable mask                      */
#define CLCD_INTSTATUS   0x018u  /* interrupt status, WRITE-1-TO-CLEAR         */
#define CLCD_REG1C       0x01cu  /* cleared to 0 on start and on stop          */
#define CLCD_PREENABLE   0x020u  /* write 1 just before CLCD_ENABLE            */
#define CLCD_BACKDROP    0x024u  /* backdrop colour, ARGB                      */
#define CLCD_VIDEO_FIRST 0x028u  /* 11 video/YUV overlay regs, 0x028..0x054    */
#define CLCD_VIDEO_LAST  0x054u
#define CLCD_WIN_FIRST   0x058u  /* RGB window k at CLCD_WIN_FIRST + k*0x18    */
#define CLCD_WIN_STRIDE  0x018u
#define CLCD_WIN_COUNT   4u
#define CLCD_UPDATE      0x0d4u  /* write 2 at the head of every window update */
/*
 * Per-window auxiliary configuration pairs. openiBoot's S5L8900 LCD code
 * writes window k at 0x0d8 + k*8 and clears the following word. 0x0e8 is also
 * AppleH1CLCD's update word, so it is represented separately as CLCD_UPDATE2.
 * These were once mislabeled as panel timings; the actual timing registers are
 * VIDTCON0..3 at 0x20c..0x218.
 */
#define CLCD_WINCFG0     0x0d8u
#define CLCD_UPDATE2     0x0e8u  /* 0x50001000 on AppleH1CLCD window updates  */
#define CLCD_WINCFG2_AUX 0x0ecu
#define CLCD_CSC_FIRST   0x1c8u  /* 8 YUV->RGB matrix regs, 0x1c8..0x1e8       */
#define CLCD_CSC_LAST    0x1e8u
#define CLCD_GATE        0x200u  /* VIDCON0 clock/divisor + scanout bit 0      */
#define CLCD_STATUS      0x204u  /* VIDCON1 polarity; bits[7:6] are live state */
#define CLCD_UNKNOWN208  0x208u
#define CLCD_VIDTCON0    0x20cu  /* vertical porch/sync timing                 */
#define CLCD_VIDTCON1    0x210u  /* horizontal porch/sync timing               */
#define CLCD_VIDTCON2    0x214u  /* active width/height minus one              */
#define CLCD_VIDTCON3    0x218u  /* openiBoot writes 1                         */
#define CLCD_OPAQUE_FIRST CLCD_UNKNOWN208
#define CLCD_OPAQUE_LAST  CLCD_VIDTCON3
#define CLCD_GAMMA0      0x400u  /* three 256-entry u32 LUTs, 0x400/0x800/0xc00 */
#define CLCD_GAMMA_SIZE  0x400u

/* openiBoot's N82 optC selects the 54 MHz display clock, divides it by five
 * for a 10.8 MHz pixel clock, inverts VCLK, and later sets ENVID_F. */
#define CLCD_N82_VIDCON0 0x00000441u
#define CLCD_N82_VIDCON1 0x00000008u

/*
 * Window register sub-offsets, from CLCD_WIN_FIRST + k*CLCD_WIN_STRIDE.
 *
 * CONFIRMED, not inferred, since 0xc0705f00 — AppleH1CLCD's "adopt whatever
 * iBoot left running" routine, the vtable slot IOMobileFramebuffer::start calls
 * immediately after start_hardware:
 *
 *     r2 = mapped register base
 *     if      (read32(r2+8) & 0x40) { sl=[r2+0x58] fp=[r2+0x5c] fb=[r2+0x60] r8=[r2+0x64] }
 *     else if (              & 0x20) { sl=[r2+0x70] fp=[r2+0x74] fb=[r2+0x78] r8=[r2+0x7c] }
 *     else if (              & 0x10) { sl=[r2+0x88] fp=[r2+0x8c] fb=[r2+0x90] r8=[r2+0x94] }
 *     else if (              & 0x08) { sl=[r2+0xa0] fp=[r2+0xa4] fb=[r2+0xa8] r8=[r2+0xac] }
 *
 * which pins the four window bases at 0x58/0x70/0x88/0xa0 (stride 0x18), pins
 * the CLCD_CTRL enable bits to 0x40/0x20/0x10/0x08 in that priority order, and
 * pins the field order. The driver then (0xc0706040..0xc0706068) does
 *
 *     width  = (r8 << 5) >> 21;      // geometry bits[26:16]
 *     height = uxth(r8 &~ 0xfc00);   // geometry bits[9:0]
 *     size   = round_up(sl * height, 0x1000);   // sl is bytes per row
 *
 * and wraps `fb` with IOMemoryDescriptor::withPhysicalAddress, so FBADDR is a
 * PHYSICAL address. Nothing here is a guess any more.
 */
#define CLCD_WIN_PITCH     0x00u  /* stride in BYTES                          */
#define CLCD_WIN_CONTROL   0x04u  /* bits[10:8] pixel format, [17:16] order   */
#define CLCD_WIN_FBADDR    0x08u  /* framebuffer PHYSICAL base                */
#define CLCD_WIN_GEOMETRY  0x0cu  /* (width << 16) | height                   */
#define CLCD_WIN_LINEWORDS 0x10u  /* line length in 32-bit words              */
#define CLCD_WIN_POSITION  0x14u  /* (dstX << 16) | dstY                      */

/* CLCD_CTRL window-enable bits, and the order the driver tests them in. */
#define CLCD_CTRL_WIN0   0x40u
#define CLCD_CTRL_WIN1   0x20u
#define CLCD_CTRL_WIN2   0x10u
#define CLCD_CTRL_WIN3   0x08u
#define CLCD_CTRL_VIDEO  0x80u
#define CLCD_CTRL_ENABLE 0x01u    /* start_hardware ORs this in */

/* CLCD_INTSTATUS / CLCD_INTMASK bits. */
#define CLCD_INT_FRAME    0x0001u /* frame (VBL) done — the swap completion   */
#define CLCD_INT_UNDERRUN 0x3f00u /* per-window FIFO underrun; we never set it */

/*
 * Pixel formats, from window control bits[10:8].
 *
 * The DEPTH is confirmed; the fine-grained names are not, and the difference
 * matters. At 0xc0705ff8 the driver switches on exactly this field to pick the
 * IOSurface pixel format it will publish, through a six-entry jump table:
 *
 *     f = (control >> 8) & 7;
 *     switch (f - 2) { case 0..3: fourcc = '565L'; case 4,5: fourcc = 'ARGB'; }
 *     default (f == 0 or 1):      fourcc = '565L';
 *
 * so the driver itself declares 6 and 7 to be 32 bits per pixel and everything
 * else to be 16-bit 5-6-5. It never distinguishes 2 from 3 from 4 from 5, so
 * neither do we: CLCD_FMT_IS_32BPP() is the only classification the binary
 * supports, and s5l_clcd_scanout() decodes on that and nothing finer.
 */
#define CLCD_FMT_SHIFT     8u
#define CLCD_FMT_MASK      0x7u
#define CLCD_FMT_32BPP     7u
#define CLCD_FMT_RGB565    3u
#define CLCD_FMT_RGB555    2u
#define CLCD_FMT_ARGB4444  5u
#define CLCD_FMT_IS_32BPP(f) ((f) == 6u || (f) == 7u)
/* Component order, from window control bits[17:16]; only meaningful at 32bpp. */
#define CLCD_ORDER_SHIFT   16u
#define CLCD_ORDER_MASK    0x3u
#define CLCD_ORDER_BGRA    0u
#define CLCD_ORDER_ARGB    3u

/* Refresh rate we present to the guest, in GUEST time. */
#define S5L_CLCD_REFRESH_HZ 60u

typedef struct {
    uint32_t stride, control, fbaddr, geometry, linewords, position;
} s5l_clcd_window_t;

typedef struct {
    uint32_t enable, disable, ctrl, fifo;
    uint32_t intmask, intstatus, reg1c, preenable, backdrop;
    uint32_t video[(CLCD_VIDEO_LAST - CLCD_VIDEO_FIRST) / 4u + 1u];
    s5l_clcd_window_t win[CLCD_WIN_COUNT];
    uint32_t update, update2;
    uint32_t wincfg_aux[5];                  /* 0x0d8,0x0dc,0x0e0,0x0e4,0x0ec */
    uint32_t csc[(CLCD_CSC_LAST - CLCD_CSC_FIRST) / 4u + 1u];
    uint32_t gate;
    uint32_t opaque[(CLCD_OPAQUE_LAST - CLCD_OPAQUE_FIRST) / 4u + 1u];
    uint32_t gamma[3][256];

    bool     scanning;        /* CLCD_ENABLE has been written 1, no stop since */
    uint32_t frame_ticks;     /* timebase ticks per frame; 0 disables the VBL  */
    uint32_t frame_accum;
    uint64_t frames;          /* elapsed VBL boundaries (host visibility)      */
} s5l_clcd_t;

void     s5l_clcd_reset(s5l_clcd_t *c);
uint32_t s5l_clcd_read(s5l_clcd_t *c, uint32_t off);
void     s5l_clcd_write(s5l_clcd_t *c, uint32_t off, uint32_t val);
/* Advance by `ticks` timebase ticks; returns true while the controller's
 * interrupt output is asserted (status AND mask, as the hardware ANDs them). */
bool     s5l_clcd_tick(s5l_clcd_t *c, uint32_t ticks);

/* True only while pixels can actually be scanned: start has been written,
 * CLCD_CTRL's global enable is set, and VIDCON0/gate bit 0 is set. Window
 * enable bits are intentionally separate because callers may need to diagnose
 * a running controller with no adoptable RGB window. */
bool     s5l_clcd_running(const s5l_clcd_t *c);

/*
 * Pre-seed the controller with an iBoot-compatible N82 display handoff:
 * window 0 programmed and enabled over an already-running scanout, plus the
 * clock and 320x480 porch/sync registers that openiBoot records for this panel.
 * IOMobileFramebuffer::start then adopts the framebuffer verbatim instead of
 * having to invent one.
 *
 * `format` is a CLCD_FMT_* value and `order` a CLCD_ORDER_* value; `stride` is
 * in bytes. This is a host-side call, not a guest-visible register: a boot stub
 * standing in for iBoot calls it before the guest runs. Returns false without
 * changing the controller if the geometry cannot be represented safely or if
 * AppleH1CLCD's page-rounded `stride * height` physical mapping would overflow
 * its 32-bit size/address space.
 */
bool     s5l_clcd_seed_window0(s5l_clcd_t *c, uint32_t fb_phys,
                               uint32_t width, uint32_t height,
                               uint32_t stride, uint32_t format, uint32_t order);

/* Read back a window's programming. Returns false if `k` is out of range or the
 * window is not enabled in CLCD_CTRL. */
bool     s5l_clcd_window(const s5l_clcd_t *c, unsigned k,
                         uint32_t *fb_phys, uint32_t *width, uint32_t *height,
                         uint32_t *stride, uint32_t *format, uint32_t *order);

/*
 * Which window the DRIVER would scan out, tested in the driver's own priority
 * order (window 0, then 1, then 2, then 3 — 0xc0705f10..0xc0705f94). Returns
 * CLCD_WIN_NONE when CLCD_CTRL enables no window at all, which is not a
 * cosmetic state: with no bit set the driver leaves its four locals holding
 * whatever the caller's frame did and builds an IOSurface out of that garbage,
 * so "no window enabled" is a bug in whatever stood in for iBoot, not a blank
 * screen. Callers must treat CLCD_WIN_NONE as an error.
 */
#define CLCD_WIN_NONE 0xffffffffu
uint32_t s5l_clcd_active_window(const s5l_clcd_t *c);

/*
 * Scan out an enabled window into 24-bit RGB, one byte per channel, top row
 * first — the seam clcd.c's header has always named, now that the window layout
 * is confirmed rather than inferred.
 *
 * `ram`/`ram_base`/`ram_len` describe the guest DRAM the window's PHYSICAL
 * framebuffer address is resolved against; a window pointing outside it is an
 * error, not a black rectangle. `rgb` must hold at least width*height*3 bytes.
 *
 * Returns false — and writes NOTHING — if the window is disabled, the geometry
 * is empty, the stride cannot hold a row, the source does not lie wholly inside
 * guest DRAM, or `rgb` is too small. It never invents a pixel: every returned
 * byte comes from guest memory.
 */
bool     s5l_clcd_scanout(const s5l_clcd_t *c, unsigned k,
                          const uint8_t *ram, uint32_t ram_base, size_t ram_len,
                          uint8_t *rgb, size_t rgb_len,
                          uint32_t *out_w, uint32_t *out_h);

/* ---------------------------------------------------------- TV-out path ---
 *
 * The shipped iPhone1,2 7E18 device tree names one /arm-io/tv-out service with
 * three 4 KiB register ranges.  Resolving those child addresses through the
 * arm-io ranges gives, in the order AppleH1DisplayDrivers maps them:
 *
 *   0x39100000  TV control / coefficient-scaler block
 *   0x39200000  mixer
 *   0x39300000  SDO (standard-definition output)
 *
 * The same node gives interrupt lines {30,38}.  AppleH1DisplayDrivers uses
 * line 30 for SDO VSYNC/swap completion: its filter tests SDO_IRQ bit 0 and
 * its action writes that bit back (W1C) before dequeuing the swap.  Line 38 is
 * a separate mixer status source; this minimal model preserves its W1C status
 * register but does not invent a cable/hotplug event.
 *
 * Unknown registers remain byte-addressable backing storage.  That is
 * intentional: the model supplies only the side effects established by the
 * shipped driver while retaining every other guest write for later evidence.
 */
#define S5L8900_TVOUT_CTRL_BASE  0x39100000u
#define S5L8900_TVOUT_MIXER_BASE 0x39200000u
#define S5L8900_TVOUT_SDO_BASE   0x39300000u
#define S5L8900_IRQ_TVOUT        30u

#define S5L_TVOUT_BANK_SIZE      0x1000u
#define S5L_TVOUT_BANK_WORDS     (S5L_TVOUT_BANK_SIZE / 4u)
#define S5L_TVOUT_REFRESH_HZ     60u

#define TVOUT_CTRL_ENABLE        0x000u
#define TVOUT_MIXER_STATUS       0x04cu
#define TVOUT_SDO_CLKCON         0x000u
#define TVOUT_SDO_IRQ            0x280u
#define TVOUT_SDO_IRQMASK        0x284u

#define TVOUT_RUN                0x00000001u
#define TVOUT_READY              0x00000002u
#define TVOUT_SDO_VSYNC          0x00000001u
#define TVOUT_SDO_MASK_VSYNC     0x00000001u

typedef enum {
    S5L_TVOUT_BANK_CTRL = 0,
    S5L_TVOUT_BANK_MIXER,
    S5L_TVOUT_BANK_SDO,
    S5L_TVOUT_BANK_COUNT
} s5l_tvout_bank_t;

typedef struct {
    uint32_t regs[S5L_TVOUT_BANK_COUNT][S5L_TVOUT_BANK_WORDS];
    uint32_t frame_ticks;      /* guest timebase ticks per generated VSYNC */
    uint32_t frame_accum;
    uint64_t frames;           /* elapsed boundaries, even while pending */
} s5l_tvout_t;

void     s5l_tvout_reset(s5l_tvout_t *t, uint32_t tb_hz);
uint32_t s5l_tvout_read(const s5l_tvout_t *t, s5l_tvout_bank_t bank,
                        uint32_t off, unsigned bytes);
void     s5l_tvout_write(s5l_tvout_t *t, s5l_tvout_bank_t bank,
                         uint32_t off, uint32_t val, unsigned bytes);
/* True while the persistent mixer and SDO timing engines are live.  The
 * control/scaler block has an independent handshake and is not a VSYNC gate. */
bool     s5l_tvout_running(const s5l_tvout_t *t);
bool     s5l_tvout_irq(const s5l_tvout_t *t);
/* Advance guest time and return the current level of interrupt line 30. */
bool     s5l_tvout_tick(s5l_tvout_t *t, uint32_t ticks);
/* Distance to the next deliverable VSYNC, or zero if none can wake WFI. */
uint32_t s5l_tvout_ticks_to_vsync(const s5l_tvout_t *t);

/* ----------------------------------------------------------------- I2C ---
 * The two S5L8900 I2C controllers.  The physical windows and interrupt lines
 * come from the shipped iPhone1,2 device tree:
 *
 *   /arm-io/i2c0  reg {0x04600000,0x1000}  interrupts {0x15}
 *   /arm-io/i2c1  reg {0x04900000,0x1000}  interrupts {0x16}
 *
 * /arm-io maps those child addresses at physical +0x38000000.  Register
 * offsets and bits below are from AppleS5L8900XI2CController's 32-bit MMIO
 * accessors and transfer state machine in the shipped kernelcache.
 */
#define S5L8900_I2C0_BASE 0x3c600000u
#define S5L8900_I2C1_BASE 0x3c900000u
#define S5L8900_IRQ_I2C0  21u
#define S5L8900_IRQ_I2C1  22u
#define S5L8900_I2C_COUNT 2u

#define I2C_CON     0x00u
#define I2C_STAT    0x04u
#define I2C_ADD     0x08u
#define I2C_DS      0x0cu
#define I2C_BUSY    0x10u  /* read zero: register writes complete immediately */
#define I2C_ENABLE  0x14u
#define I2C_INT     0x20u  /* interrupt status, write-one-to-clear             */

#define I2C_CON_ACKEN     0x80u
#define I2C_CON_RESUME    0x10u
#define I2C_STAT_MODE     0xc0u
#define I2C_STAT_MODE_MRX 0x80u
#define I2C_STAT_MODE_MTX 0xc0u
#define I2C_STAT_START    0x20u
#define I2C_STAT_ENABLE   0x10u
#define I2C_STAT_NAK      0x01u  /* read-only: last address/byte was not ACKed */

#define I2C_INT_BYTE 0x0100u
#define I2C_INT_STOP 0x2000u
#define I2C_INT_ALL  0x3f00u  /* mask used by the stock driver's clear-all */

typedef struct {
    uint8_t  addr;                         /* seven-bit bus address */
    void    *ctx;
    bool   (*start)(void *ctx, bool read);
    bool   (*write)(void *ctx, uint8_t byte);
    uint8_t (*read)(void *ctx);
    void   (*stop)(void *ctx);
} s5l_i2c_slave_t;

#define S5L_I2C_SLAVES      4u
#define S5L_I2C_UNKNOWN_OFF 8u

typedef struct {
    uint32_t con, stat, add, ds, enable, intstat;
    bool     nak;
    bool     active;
    bool     reading;
    int32_t  sel;             /* selected slave index, or -1 */

    /* Bounded diagnostics: unknown traffic must be visible without allowing
     * a guest to grow host allocations. */
    uint64_t starts, bytes_tx, bytes_rx, naks;
    uint64_t unknown_reads, unknown_writes;
    uint32_t unknown_off[S5L_I2C_UNKNOWN_OFF];
    unsigned unknown_off_count;

    /* Host wiring. Snapshot code serializes `sel`, never these callbacks. */
    s5l_i2c_slave_t slaves[S5L_I2C_SLAVES];
    unsigned        slave_count;
} s5l_i2c_t;

/* Reset is total: it is valid on an uninitialized/poisoned object and removes
 * attached slaves. Callers attach board wiring after reset. */
void     s5l_i2c_reset(s5l_i2c_t *bus);
bool     s5l_i2c_attach(s5l_i2c_t *bus, const s5l_i2c_slave_t *slave);
uint32_t s5l_i2c_read(s5l_i2c_t *bus, uint32_t off);
void     s5l_i2c_write(s5l_i2c_t *bus, uint32_t off, uint32_t val);
bool     s5l_i2c_irq(const s5l_i2c_t *bus);

/* ------------------------------------------------- PCF50635 PMU / RTC ---
 * The device tree names `pmu,pcf50635` at seven-bit address 0x73 on i2c0.
 * Its register pointer is one byte and auto-increments.  Written registers are
 * persistent storage; reads of other unmodelled registers return zero but are
 * counted.  RTC registers 0x59..0x5f are fully defined by the stock driver's
 * decoder: BCD except for the binary weekday at 0x5c.
 */
#define PCF50635_I2C_ADDR 0x73u
#define PCF50635_NREG     0x100u
#define PCF50635_RTCSC    0x59u
#define PCF50635_RTCWD    0x5cu
#define PCF50635_RTCYR    0x5fu
#define PCF50635_MIN_TIME 946684800ull  /* 2000-01-01 00:00:00 UTC */
#define PCF50635_MAX_TIME 4102444799ull /* 2099-12-31 23:59:59 UTC */
#define PCF50635_DEFAULT_TIME 1262304000ull /* 2010-01-01 00:00:00 UTC */
#define PCF50635_UNKNOWN_REGS 16u

typedef struct {
    uint8_t  regs[PCF50635_NREG];
    uint8_t  written[PCF50635_NREG];

    uint64_t seconds;
    uint32_t tick_hz;
    uint64_t tick_accum;

    uint8_t  ptr;
    bool     have_ptr;
    bool     reading;

    uint64_t reg_reads, reg_writes, unknown_reads, unknown_writes;
    uint8_t  unknown_reg[PCF50635_UNKNOWN_REGS];
    unsigned unknown_reg_count;
} s5l_pcf50635_t;

void s5l_pcf50635_reset(s5l_pcf50635_t *pmu, uint32_t tick_hz);
void s5l_pcf50635_set_time(s5l_pcf50635_t *pmu, uint64_t unix_seconds);
void s5l_pcf50635_tick(s5l_pcf50635_t *pmu, uint32_t ticks);
void s5l_pcf50635_bind(s5l_pcf50635_t *pmu, s5l_i2c_slave_t *slave);
void s5l_pcf50635_civil(uint64_t unix_seconds, int *year, int *month, int *day,
                        int *hour, int *minute, int *second, int *weekday);

/* ----------------------------------------------------------------- SPI ---
 * The two S5L8900 SPI controllers AppleS5L8900XSPIController drives. Their
 * windows and VIC lines are derived at the top of this header; what follows is
 * the register model, and why it is a model rather than the stub it replaces.
 *
 * WHY THIS EXISTS. AppleMultitouchZ2SPI::finishStarting() @0xc0442670 probes
 * the touch controller through isInHBPP() @0xc0441008, which issues one 16-byte
 * full-duplex transfer on spi1 — `provider->vtbl[0x368](tx, 16, rx, 16, 0)` at
 * 0xc04410a4 — and then blocks at 0xc05a6d80 in IOCommandGate::commandSleep(
 * event, 0): the two-argument form, so no deadline. It loops there while its
 * done flag is clear (0xc05a6d8c) until the SPI completion interrupt sets the
 * flag and calls commandWakeup. Against a storage stub that interrupt can never
 * arrive, so the kext has been asleep since the first time it ran. run59
 * measured the shape of the stall exactly: spi1 `r=0 w=19`, which is the
 * driver's eleven configuration writes plus the eight bytes it pushes into an
 * eight-deep transmit FIFO before it starts and sleeps. `r=0` is the proof that
 * the interrupt handler never ran even once.
 *
 * THE ONE RULE THAT MATTERS. The interrupt routine is the FILTER half of an
 * IOFilterInterruptEventSource (registered at 0xc05a7150; body at 0xc05a6688,
 * action finishTransfer at 0xc05a6840). It reads STATUS and, when the
 * receive-FIFO level field is zero, returns false at 0xc05a66e4 having
 * acknowledged nothing — and a filter returning false does not schedule its
 * action, so finishTransfer never runs and nothing calls commandWakeup.
 * Raising the line with an empty receive FIFO therefore reproduces the
 * unbounded sleep exactly rather than ending it, which is why s5l_spi_irq()
 * requires a byte the filter can actually drain.
 *
 * WHAT THIS IS NOT. No clock rate, DMA channel, chip-select edge or bit-order
 * behaviour is emulated. The dividers at 0x30/0x38 are stored and never turned
 * into time: a word is shifted the instant the guest stores it, exactly as an
 * I2C transfer completes inside its command store.
 */
#define SPI_CONTROL 0x00u   /* run/mode; 0x0d starts, 0 powers the block down */
#define SPI_SETUP   0x04u
#define SPI_STATUS  0x08u   /* event latches + both FIFO levels               */
#define SPI_PIN     0x0cu   /* internal chip select in bit 1                  */
#define SPI_TXDATA  0x10u   /* transmit FIFO, write-only                      */
#define SPI_RXDATA  0x20u   /* receive FIFO, read-only and destructive        */
#define SPI_CLKDIV  0x30u
#define SPI_CNT     0x34u   /* word count = max(txLen, rxLen)                 */
#define SPI_IDD     0x38u   /* second divider                                 */

/*
 * CONTROL. The stock driver writes 0x0d to start a transfer and 0 to power the
 * block down; run23 caught BasebandSPI writing 0x0c to spi2 while configuring
 * it and never starting anything, so bit 0 is the run bit and the 0x0c above it
 * is a mode field both drivers share.
 */
#define SPI_CONTROL_RUN   0x0001u
#define SPI_CONTROL_START 0x000du

/*
 * SETUP. The driver builds a base of 0x1000 (0xc05a6fdc, the spi-version 0 arm
 * of start()) ORed with 0x18 (0xc05a64b4), ORs 0x20 to arm the transfer
 * (0xc05a6cb8), and then ORs 0x180 to go (0xc05a6d3c). 0x40 selects DMA
 * (0xc05a6c40), which this model does not implement.
 *
 * Of that 0x180, 0x100 is the completion-interrupt enable, and it is the one
 * SETUP bit s5l_spi_irq() consults. Two independent sites pin it: the filter
 * clears exactly 0x100 and nothing else when the transfer's counts run out
 * (`bic r3, r3, #0x100` at 0xc05a6808), and finishTransfer writes the bare base
 * word back (0xc05a685c), dropping it again.
 *
 * Gating on it is not caution, it is required. The driver's order is fill the
 * transmit FIFO, arm, THEN enable — precisely so the completion interrupt
 * cannot arrive while it is still pushing bytes. A line that ignored this bit
 * would fire on the driver's FIRST prefill store, and the filter would run
 * against half-initialised transfer counts.
 */
#define SPI_SETUP_BASE 0x1018u
#define SPI_SETUP_ARM  0x0020u
#define SPI_SETUP_RUN  0x0080u
#define SPI_SETUP_IRQ  0x0100u
#define SPI_SETUP_GO   (SPI_SETUP_RUN | SPI_SETUP_IRQ)
#define SPI_SETUP_DMA  0x0040u

/*
 * STATUS, for `spi-version 0` — which is what all three controllers are. The
 * shipped tree gives every one of them `spi-version {0}`, and the guest's own
 * start message agrees: `_spiVersion = 0 _spiInternalCS = 0`.
 *
 * The low four bits are event latches and are write-one-to-clear. Both FIFO
 * levels sit above them and are OCCUPANCY, not free space: the driver's
 * decoders at 0xc05a6904 and 0xc05a6928 read `(s >> 4) & 0xF` and
 * `(s >> 8) & 0xF` and the transmit one then subtracts from the depth itself
 * (`rsb r0, r0, #8`). Publishing free space in the transmit field instead would
 * make a drained FIFO look full and the filter would never refill it.
 *
 * The levels are computed from the FIFOs on every read rather than stored,
 * which is what makes the filter's acknowledge safe: it writes back the WHOLE
 * raw status word it read (0xc05a67d0, `mov r2, r8`), not the event mask, so a
 * model that stored the levels would zero them on the first acknowledge.
 *
 * Version 1 parts move both fields (`(s >> 6) & 0x1F` and `(s >> 11) & 0x1F`),
 * use a depth of 16, a SETUP base of 0x4000 and an event mask of 0x0040000F,
 * and add a register at 0x4c the driver writes only when _spiVersion is
 * non-zero. None of that is defined here: this SoC has no such controller, and
 * a constant that looks wired but is not is a landmine.
 */
#define SPI_STATUS_EVENTS   0x000fu
#define SPI_STATUS_TX_SHIFT 4u
#define SPI_STATUS_RX_SHIFT 8u
#define SPI_STATUS_LEVEL    0x000fu

/* Eight, from start()'s spi-version 0 arm at 0xc05a6fec — and the same 8 the
 * driver uses as its prefill limit, which is why run59's 19 writes decompose
 * as eleven configuration stores plus exactly eight FIFO bytes. */
#define S5L_SPI_FIFO_DEPTH  8u
/* Four chip selects, because the driver masks the select out of its per-device
 * configuration word with `and r3, r3, #3` (0xc05a64c0) and indexes a table at
 * this+0x84 by it. spi0 really does carry two devices, nor-flash and lcd0. */
#define S5L_SPI_SLAVES      4u
#define S5L_SPI_UNKNOWN_OFF 8u

/*
 * A device on the bus. SPI is full duplex: one byte leaves the master and one
 * arrives in the same word, so a slave is a function from the outgoing byte to
 * the incoming one and there is no separate read entry point as there is on
 * I2C.
 *
 * There is deliberately no chip-select callback. On this board the select lines
 * are GPIO platform functions (`function-spi_cs0`) and neither controller sets
 * `internal-cs`, so the controller cannot observe a select edge and a callback
 * for one could never fire. A device model that needs packet framing is what
 * makes the GPIO block a model rather than a stub; it is not something this
 * interface can fake.
 */
typedef struct {
    void   *ctx;
    uint8_t (*transfer)(void *ctx, uint8_t out);
} s5l_spi_slave_t;

typedef struct {
    uint32_t control, setup, pin, clkdiv, cnt, idd;
    uint32_t status;        /* event latches only; levels are computed        */
    uint32_t words_left;    /* latched from CNT. Visibility, not a gate — see
                             * s5l_spi_write() for why it must not be one.    */

    uint8_t  tx[S5L_SPI_FIFO_DEPTH];
    uint8_t  rx[S5L_SPI_FIFO_DEPTH];
    uint8_t  tx_level, rx_level;
    /*
     * Which chip select words are routed to. Nothing writes it yet, and that is
     * the honest state of the board rather than an oversight: the select lines
     * are GPIO functions this machine models as storage, so the controller
     * cannot see them move. The touch device's select is 0 — there is no
     * `chip-select` property anywhere in the shipped tree; the number is
     * reg[0] of /arm-io/spi1/multi-touch, whose reg begins {0, 0xa6, ...} —
     * so that is where a device attached at reset is. When the GPIO block
     * becomes a model, this field is what it drives and nothing else changes.
     */
    uint8_t  cs;

    /* Bounded diagnostics, as on I2C: unknown or refused traffic must be
     * visible without letting a guest grow host allocations. */
    uint64_t words;         /* completed shifts                               */
    uint64_t tx_drops;      /* TXDATA stores into a full transmit FIFO        */
    uint64_t rx_underruns;  /* RXDATA loads from an empty receive FIFO        */
    uint64_t unknown_reads, unknown_writes;
    uint32_t unknown_off[S5L_SPI_UNKNOWN_OFF];
    unsigned unknown_off_count;

    /* Host wiring. Snapshot code serializes `cs`, never these callbacks. */
    s5l_spi_slave_t slaves[S5L_SPI_SLAVES];
} s5l_spi_t;

/* Reset is total: valid on an uninitialized/poisoned object, and it removes
 * attached devices. Callers attach board wiring after reset. */
void     s5l_spi_reset(s5l_spi_t *bus);
/* Attach at a chip select. Refuses an out-of-range select, a slave with no
 * transfer callback, and any select that is already taken — silently shadowing
 * a device would be worse than refusing. */
bool     s5l_spi_attach(s5l_spi_t *bus, unsigned cs,
                        const s5l_spi_slave_t *slave);
/* Not const: reading SPI_RXDATA pops the receive FIFO, which is what makes room
 * for the rest of a backed-up transfer. */
uint32_t s5l_spi_read(s5l_spi_t *bus, uint32_t off);
void     s5l_spi_write(s5l_spi_t *bus, uint32_t off, uint32_t val);
/* A level, held until the guest clears the event latches through the W1C
 * register — and only while the completion interrupt is enabled in SETUP and
 * the receive FIFO holds a byte. See "THE ONE RULE THAT MATTERS" and the SETUP
 * note above; neither term is a decoration. */
bool     s5l_spi_irq(const s5l_spi_t *bus);

/*
 * The null device: it answers every word with 0x00.
 *
 * This is what proves the controller without claiming a touch controller.
 * isInHBPP() accepts the probe only if the two big-endian halfwords rx[0..1]
 * and rx[2..3] both pass its test at 0xc0440658, so an all-zero response is a
 * definite rejection: the driver returns false from finishStarting() and says
 * so, instead of sleeping with no deadline. A real device model attaches here
 * later.
 */
void     s5l_spi_null_bind(s5l_spi_slave_t *slave);

/* ------------------------------------ Synopsys DWC2 USB OTG (config only) ---
 * The four hardware-configuration registers AppleSynopsysOTGDevice reads out of
 * the block at S5L8900_USB_OTG_BASE, and nothing else. core/src/soc/usbotg.c
 * carries the evidence: which accesses the driver makes, the endpoint
 * derivation it runs on what it reads, and why each value below is the one
 * chosen. The short version is that an unmodelled window answered those reads
 * with zero, the driver believed it, and the boot panicked on the endpoint
 * count it computed — deterministically, at the same instruction every run.
 *
 * These are a legal and sufficient DWC2 configuration. They are NOT values
 * measured from real S5L8900 silicon; we have no dump of this part's
 * configuration registers, and nothing here pretends otherwise.
 *
 * This is a configuration-register model, not a USB controller. No transfer,
 * FIFO, endpoint, DMA, PHY or interrupt behaviour is emulated, and the device
 * tree's interrupt 0x13 is deliberately not defined as a constant: nothing here
 * ever asserts it, and a constant that looks wired but is not is the same
 * landmine the SPI note above describes.
 */
#define USBOTG_GHWCFG1 0x044u   /* per-endpoint direction, 2 bits per endpoint */
#define USBOTG_GHWCFG2 0x048u   /* architecture + counts, incl. NumDevEps      */
#define USBOTG_GHWCFG4 0x050u   /* driver reads it and uses only bit 25        */
#define USBOTG_PCGCCTL 0xe00u   /* power/clock gating; guest read-modify-write */

#define S5L_DWC2_GHWCFG1 0x00000000u   /* every endpoint bidirectional */
#define S5L_DWC2_GHWCFG2 0x228de550u   /* NumDevEps=9 -> 5 IN / 5 OUT  */
#define S5L_DWC2_GHWCFG4 0x00000000u

typedef struct {
    /* The only writable state in this model. Reset 0. */
    uint32_t pcgcctl;
} s5l_usbotg_t;

void     s5l_usbotg_reset(s5l_usbotg_t *u);
/* Unmodelled offsets read 0 and accept-and-discard writes, which is exactly
 * what an unmapped access did before this window existed. */
uint32_t s5l_usbotg_read(const s5l_usbotg_t *u, uint32_t off);
void     s5l_usbotg_write(s5l_usbotg_t *u, uint32_t off, uint32_t val);

#define S5L_STUB_MAX      16

typedef struct {
    uint32_t    base, size;
    const char *name;
    /* Backing store covers the WHOLE declared window. A fixed-size array was
     * the first design and it was wrong in a way worth remembering: at 64
     * registers it covered offsets 0x000-0x0FC, while the two registers we had
     * actually measured live at 0x320 (GPIO FSEL) and 0x404 (CLOCK0 ADJ2).
     * Both fell past the array and were counted but not stored, so read-back
     * returned 0 — the stub silently failed to be the honest storage it exists
     * to be, for precisely the registers that mattered. Size the backing to
     * the window instead of hoping the window is small. */
    uint32_t   *regs;
    uint32_t    nregs;
    uint64_t    reads, writes;
    uint64_t    oob;                  /* accesses past the backing store */
} s5l_stub_t;

/* ------------------------------------------------------------- machine ---
 * Wires the CPU to RAM and the peripherals through one arm_bus_t.
 */
typedef struct {
    arm_cpu_t  cpu;
    arm_bus_t  bus;
    uint8_t   *ram;
    uint32_t   ram_base;
    uint32_t   ram_size;
    s5l_uart_t  uart0;
    /*
     * Both PL192 VICs. vic[0] is the historical vic0; vic[1] backs the second
     * page at S5L8900_VIC1_BASE, which AppleARMPL192VIC maps and which carries
     * device-tree interrupt lines 32..63.
     */
    s5l_vic_t   vic[S5L8900_VIC_COUNT];
    s5l_timer_t timer;
    s5l_power_t power;
    s5l_clcd_t  clcd;
    s5l_tvout_t tvout;
    s5l_i2c_t   i2c[S5L8900_I2C_COUNT];
    s5l_pcf50635_t pmu;
    /* spi[0] is spi0 and spi[1] is spi1. spi2 is not here: it is the baseband
     * transport on GPIO interrupts this machine cannot route, so it stays a
     * declared stub window. */
    s5l_spi_t   spi[S5L8900_SPI_COUNT];
    s5l_usbotg_t usbotg;
    s5l_nor_t   nor;
    uint64_t   unmapped_reads;   /* visibility: accesses outside the map */
    uint64_t   unmapped_writes;

    /* Diagnostic: the first distinct addresses the guest touched outside the
     * memory map. When real firmware wanders off, these name the peripheral we
     * have not modelled yet — which is the next thing to build. */
    uint32_t   unmapped_addr[S5L_UNMAPPED_LOG];
    unsigned   unmapped_addr_count;

    /* Diagnostic: log accesses to device windows (not RAM). Real firmware
     * polls hardware to decide what to do next, so seeing exactly which
     * registers it reads is how we learn what it is waiting for. */
    bool       trace_devices;
    uint32_t   dev_addr[S5L_DEVLOG];
    uint32_t   dev_value[S5L_DEVLOG];
    bool       dev_is_write[S5L_DEVLOG];
    unsigned   dev_count;

    /*
     * How fast guest time runs relative to guest work. See S5L8900_CPU_HZ.
     * cpu_hz elapsed CPU-clock ticks advance the timebase by tb_hz ticks;
     * tb_accum carries the remainder so the ratio stays exact over time. Active
     * execution supplies one clock tick per retired instruction; WFI can
     * supply an idle interval without changing cpu.cycles.
     */
    uint32_t   cpu_hz, tb_hz;
    uint64_t   tb_accum;

    /* Identified-but-unmodelled peripheral windows. See s5l_stub_t. */
    s5l_stub_t stubs[S5L_STUB_MAX];
    unsigned   stub_count;
    /* Declarations that were refused (table full, overlap, allocation). Zero is
     * the only acceptable value after init; a non-zero count means a window we
     * meant to name is silently unmapped instead. */
    unsigned   stub_declare_failures;
} s5l8900_t;

/*
 * Declare a stub window. `name` must be a string literal or otherwise outlive
 * the machine. Returns false if the table is full, or if the window overlaps a
 * modelled device, another stub, or RAM — silently shadowing, or being silently
 * shadowed by, anything else on the bus would be worse than refusing. Windows
 * larger than S5L_STUB_REGS*4 are accepted; accesses beyond that are counted in
 * `oob` rather than stored, so the shortfall is visible.
 */
bool s5l8900_add_stub(s5l8900_t *m, uint32_t base, uint32_t size,
                      const char *name);

/* ------------------------------------------------------- address routing ---
 *
 * THE ROUTING CONTRACT. There is exactly one rule, and it is enforced at
 * construction rather than at every access:
 *
 *   RAM MAY NOT OVERLAP ANY WINDOW THIS MACHINE DECODES.
 *
 * s5l8900_init() refuses to build a machine whose RAM aperture would cover a
 * device window, and s5l8900_add_stub() refuses a window that would land inside
 * RAM. With that invariant held, "device wins" and "RAM wins" are the same
 * routing, so bus_read()/bus_write() are free to keep RAM on the fast path —
 * and a silent alias is not a bug that can be reintroduced, it is a machine
 * that cannot be constructed.
 *
 * The alternative contracts were considered and rejected:
 *
 *   - Let devices win at access time. The guest is still told through the
 *     device tree that it owns a contiguous DRAM bank, so the kernel would
 *     allocate pages inside the device window and quietly corrupt itself. It
 *     also costs a linear window scan on every RAM access.
 *   - Clamp RAM to the aperture. bootkernel publishes mem_size in boot_args and
 *     in /memory:reg from its OWN variable, so a clamp inside the machine just
 *     moves the lie: the guest would use DRAM the machine does not have.
 *
 * WHAT THIS DOES NOT CLAIM. A DRAM window larger than the SoC's real aperture
 * necessarily covers physical regions the S5L8900 has (edram, vrom, SRAM — see
 * the memory-map block at the top of this header). We do not model those, so we
 * do not decode them and nothing is shadowed. The day one of them becomes a
 * device model it joins this list, and an oversized -R stops constructing.
 */
typedef struct {
    uint32_t    base, size;
    const char *name;                 /* string literal; never owned */
} s5l_window_t;

/* Enough for every fixed device window plus every stub. The 15 is the length of
 * DEVICE_WINDOWS in core/src/soc/machine.c — nor, clcd, the three tv-out banks,
 * i2c0, i2c1, spi0, spi1, usb-otg, vic0, vic1, power, uart0, timer — and there
 * is no slack left in it, so a new device model has to raise this number too.
 * It was 13 until spi0 and spi1 stopped being stubs. */
#define S5L_WINDOW_MAX (S5L_STUB_MAX + 15u)

/*
 * Every window this machine decodes: the modelled devices first, then the
 * declared stubs. Writes at most `max` entries and returns how many windows
 * exist (which may exceed `max`, so a short buffer is detectable).
 */
unsigned s5l8900_windows(const s5l8900_t *m, s5l_window_t *out, unsigned max);

/*
 * The first device window a RAM aperture of [ram_base, ram_base+ram_size) would
 * shadow, or NULL if there is none. Pure: it depends only on the fixed device
 * map, so a caller can ask BEFORE building a machine (or before choosing -R).
 * The returned pointer is into a static table and outlives any machine.
 *
 * Stub windows are not consulted here because no stub exists before init; stubs
 * are checked against RAM as they are declared, by s5l8900_add_stub().
 */
const s5l_window_t *s5l8900_ram_conflict(uint32_t ram_base, uint32_t ram_size);

/*
 * Physical regions the S5L8900 itself decodes, as confirmed from the shipped
 * device tree — INCLUDING the ones we do not model. Sets `*out` to a static
 * table and returns its length. This exists so "does this RAM size cover
 * something real?" is a question the core can answer with evidence, instead of
 * a warning hardcoded in a tool.
 */
unsigned s5l8900_soc_regions(const s5l_window_t **out);

/* True if [a, a+alen) and [b, b+blen) intersect. Zero-length ranges never do.
 * 64-bit inside, so a range near the top of the space cannot wrap into a
 * false negative. */
bool s5l8900_overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen);

/* Advance devices by elapsed guest CPU-clock ticks and refresh the CPU's
 * interrupt lines.  This does not retire CPU instructions. */
void s5l8900_tick(s5l8900_t *m, uint32_t ticks);

/* ---------------------------------------------------------- wake sources ---
 *
 * How far a WFI may fast-forward, expressed as DATA rather than as a list of
 * devices baked into the wait itself.
 *
 * A core in WFI retires nothing, so the machine skips guest time to the first
 * moment an interrupt can arrive instead of spinning the host. That skip is an
 * optimisation, but it is one that decides what the guest can observe: a
 * source the wait does not know about is a source that cannot wake the CPU,
 * however correctly the device asserts its line, because time steps straight
 * over the tick it fired on. An idle SpringBoard sits in WFI, which is exactly
 * the state a touch, a GPIO edge or a USB event has to interrupt.
 *
 * So every modelled device that can raise an interrupt declares itself in the
 * table in core/src/soc/machine.c, and the wait takes the minimum. Adding a
 * device is a table entry, not an edit to the wait.
 *
 * The three-way answer is deliberately asymmetric, because the two ways of
 * being wrong are not equally bad. Waking too early only costs host time;
 * waking too late is a lost interrupt. S5L_WAKE_NEVER is therefore the ONLY
 * reply that lets guest time be skipped past a source, and it means "this
 * device cannot fire from the state it is in" — stopped, masked, not scanning.
 * A source that merely finds the distance hard to work out must answer
 * S5L_WAKE_UNKNOWN, and the machine then does not fast-forward at all.
 */
typedef enum {
    S5L_WAKE_NEVER = 0,  /* cannot fire from this state; safe to sleep past   */
    S5L_WAKE_AT,         /* fires in *ticks timebase ticks; *ticks is >= 1    */
    S5L_WAKE_UNKNOWN     /* cannot say — the machine must not sleep past it   */
} s5l_wake_kind_t;

typedef struct {
    const char *name;    /* string literal; never owned                       */
    /*
     * The flat device-tree interrupt number this source drives: 0-31 on VIC0,
     * 32-63 on VIC1 (see the VIC1 note above). A source whose line is not
     * enabled in its VIC cannot reach the CPU at all, so the wait skips it
     * without asking — which is why `next_edge` need not re-check the VIC.
     */
    unsigned    line;
    /* Distance to this source's next interrupt edge, in TIMEBASE ticks.
     * Called only when `line` is enabled. Must not mutate the machine. */
    s5l_wake_kind_t (*next_edge)(const s5l8900_t *m, uint32_t *ticks);
} s5l_wake_source_t;

/*
 * The machine's wake sources. Sets `*out` to a static table and returns its
 * length, exactly as s5l8900_soc_regions() does for physical regions.
 */
unsigned s5l8900_wake_sources(const s5l_wake_source_t **out);

/*
 * The earliest edge among `n` sources, in timebase ticks.
 *
 * Returns S5L_WAKE_AT and sets `*ticks` when every enabled source could say
 * where it stands and at least one has a future edge; S5L_WAKE_NEVER when the
 * enabled ones all decline (nothing to wait for, so nothing may be skipped
 * either); S5L_WAKE_UNKNOWN if any enabled source could not answer — including
 * one that answers S5L_WAKE_AT with a distance of zero, which is not a
 * statement about the future. `*ticks` is untouched unless S5L_WAKE_AT.
 *
 * Pure with respect to the machine. Exposed so the reduction can be tested
 * against sources the machine does not (yet) have.
 */
s5l_wake_kind_t s5l8900_next_wake(const s5l8900_t *m,
                                  const s5l_wake_source_t *src, unsigned n,
                                  uint32_t *ticks);

/*
 * ram_base/ram_size define where RAM appears. Returns false on allocation
 * failure, or if the RAM aperture would shadow a device window — see the
 * routing contract above and s5l8900_ram_conflict(), which a caller can use to
 * find out WHICH window before (or instead of) calling this. Call
 * s5l8900_free() when done.
 */
bool s5l8900_init(s5l8900_t *m, uint32_t ram_base, uint32_t ram_size);
void s5l8900_free(s5l8900_t *m);

/* Copy a blob into guest RAM at a physical address. */
void s5l8900_load(s5l8900_t *m, uint32_t addr, const void *data, size_t len);

/* Run up to max_steps instructions, stopping early on a non-OK status.
 * Returns the number of instructions retired. */
unsigned s5l8900_run(s5l8900_t *m, unsigned max_steps, arm_status_t *status);

#endif /* IOS3VM_SOC_H */
