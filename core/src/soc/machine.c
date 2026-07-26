/*
 * iOS3-VM — S5L8900 machine: the system bus tying the CPU to RAM and devices.
 *
 * Every guest access lands here after MMU translation, and is routed by
 * physical address to either RAM or a peripheral window. Accesses outside the
 * map are counted rather than silently swallowed, so a misbehaving guest (or a
 * gap in our memory map) is visible instead of mysterious.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- the window map ---
 *
 * Every window this machine decodes, in one place. It exists so that "is
 * anything shadowed?" is a question with a single answer rather than a property
 * that has to be re-derived from the if-chain in bus_read() every time someone
 * changes the memory map — which is exactly how the NOR came to be unreachable
 * at -R 512 without anyone noticing.
 *
 * NOR's size here is the constant rather than m->nor.size. They are always
 * equal (s5l8900_init passes the constant), and using the constant is what lets
 * s5l8900_ram_conflict() answer before a machine exists.
 */
static const s5l_window_t DEVICE_WINDOWS[] = {
    { S5L8900_NOR_BASE,   S5L8900_NOR_SIZE,   "nor"   },
    { S5L8900_CLCD_BASE,  S5L8900_DEV_SIZE,   "clcd"  },
    { S5L8900_TVOUT_CTRL_BASE,  S5L_TVOUT_BANK_SIZE, "tvout-control" },
    { S5L8900_TVOUT_MIXER_BASE, S5L_TVOUT_BANK_SIZE, "tvout-mixer"   },
    { S5L8900_TVOUT_SDO_BASE,   S5L_TVOUT_BANK_SIZE, "tvout-sdo"     },
    { S5L8900_I2C0_BASE,  S5L8900_DEV_SIZE,   "i2c0"  },
    { S5L8900_I2C1_BASE,  S5L8900_DEV_SIZE,   "i2c1"  },
    { S5L8900_SPI0_BASE,  S5L8900_DEV_SIZE,   "spi0"  },
    { S5L8900_SPI1_BASE,  S5L8900_DEV_SIZE,   "spi1"  },
    { S5L8900_USB_OTG_BASE, S5L8900_DEV_SIZE, "usb-otg" },
    { S5L8900_VIC0_BASE,  S5L8900_DEV_SIZE,   "vic0"  },
    { S5L8900_VIC1_BASE,  S5L8900_DEV_SIZE,   "vic1"  },
    { S5L8900_POWER_BASE, S5L8900_POWER_SIZE, "power" },
    /* The rest of the power page. Two blocks, two drivers, one page: see the
     * GPIOIC section of soc.h for why the register offsets this window carries
     * are stated relative to the page base rather than to this base. */
    { S5L8900_GPIOIC_BASE, S5L8900_GPIOIC_SIZE, "gpioic" },
    { S5L8900_GPIO_BASE,  S5L8900_DEV_SIZE,   "gpio"  },
    { S5L8900_UART0_BASE, S5L8900_DEV_SIZE,   "uart0" },
    { S5L8900_TIMER_BASE, S5L8900_TIMER_SIZE, "timer" },
};
#define NDEVICE_WINDOWS (sizeof DEVICE_WINDOWS / sizeof DEVICE_WINDOWS[0])

/*
 * Physical regions the SoC has, from the shipped device tree. Modelled or not:
 * the point of listing the unmodelled ones is that an oversized DRAM window
 * covering them is a stated property of the configuration, not folklore. See
 * the memory-map block at the top of soc.h for the derivation of each.
 */
static const s5l_window_t SOC_REGIONS[] = {
    { S5L8900_SDRAM_BASE, 0x08000000u,        "dram (128 MB fitted)" },
    { S5L8900_EDRAM_BASE, S5L8900_EDRAM_SIZE, "edram"                },
    { S5L8900_VROM_BASE,  S5L8900_VROM_SIZE,  "vrom"                 },
    { S5L8900_SRAM_BASE,  S5L8900_SRAM_SIZE,  "sram/amc"             },
    { S5L8900_ARMIO_BASE, S5L8900_ARMIO_SIZE, "arm-io"               },
};

bool s5l8900_overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
    /* 64-bit throughout: a window near the top of the space must not wrap its
     * end below its base and read as disjoint from everything. */
    uint64_t a0 = a, a1 = a0 + alen;
    uint64_t b0 = b, b1 = b0 + blen;
    if (!alen || !blen) return false;
    return b0 < a1 && a0 < b1;
}

unsigned s5l8900_soc_regions(const s5l_window_t **out) {
    if (out) *out = SOC_REGIONS;
    return (unsigned)(sizeof SOC_REGIONS / sizeof SOC_REGIONS[0]);
}

const s5l_window_t *s5l8900_ram_conflict(uint32_t ram_base, uint32_t ram_size) {
    for (unsigned i = 0; i < NDEVICE_WINDOWS; i++)
        if (s5l8900_overlaps(ram_base, ram_size,
                             DEVICE_WINDOWS[i].base, DEVICE_WINDOWS[i].size))
            return &DEVICE_WINDOWS[i];
    return NULL;
}

unsigned s5l8900_windows(const s5l8900_t *m, s5l_window_t *out, unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < NDEVICE_WINDOWS; i++, n++)
        if (out && n < max) out[n] = DEVICE_WINDOWS[i];
    for (unsigned i = 0; i < m->stub_count; i++, n++)
        if (out && n < max) {
            out[n].base = m->stubs[i].base;
            out[n].size = m->stubs[i].size;
            out[n].name = m->stubs[i].name;
        }
    return n;
}

/*
 * Bounds checks are done in 64-bit deliberately. The guest controls every
 * address, so a 32-bit "(a - base) + len <= size" can be made to wrap: an
 * access at 0xFFFFFFFE with len 4 sums to 2, passes the test, and then indexes
 * ram[0xFFFFFFFE]. Widening makes that impossible.
 */
static inline bool in_ram(const s5l8900_t *m, uint32_t a, uint32_t len) {
    if (a < m->ram_base) return false;
    return (uint64_t)(a - m->ram_base) + (uint64_t)len <= (uint64_t)m->ram_size;
}
static inline bool in_window(uint32_t a, unsigned bytes,
                             uint32_t base, uint32_t size) {
    if (!bytes || a < base) return false;
    return (uint64_t)(a - base) + bytes <= (uint64_t)size;
}
static inline bool in_dev(uint32_t a, unsigned bytes, uint32_t base) {
    return in_window(a, bytes, base, S5L8900_DEV_SIZE);
}
static inline bool mmio_word(uint32_t a, unsigned bytes,
                             uint32_t base, uint32_t size) {
    return bytes == 4u && (a & 3u) == 0u && in_window(a, bytes, base, size);
}
/* The timer is the one peripheral that does not fit the uniform 4 KB window:
 * its interrupt-status alias sits at offset 0x10000. */
static inline bool in_power(uint32_t a, unsigned bytes) {
    return mmio_word(a, bytes, S5L8900_POWER_BASE, S5L8900_POWER_SIZE);
}
static inline bool in_gpioic(uint32_t a, unsigned bytes) {
    return mmio_word(a, bytes, S5L8900_GPIOIC_BASE, S5L8900_GPIOIC_SIZE);
}
static inline bool in_timer(uint32_t a, unsigned bytes) {
    return mmio_word(a, bytes, S5L8900_TIMER_BASE, S5L8900_TIMER_SIZE);
}

/* ------------------------------------------------------- stub windows --- */

bool s5l8900_add_stub(s5l8900_t *m, uint32_t base, uint32_t size,
                      const char *name) {
    if (!m || m->stub_count >= S5L_STUB_MAX || !size) return false;
    /* A window that extends beyond the 32-bit physical address space is only
     * partially reachable and used to make the rounded register count wrap. */
    if ((uint64_t)base + size > 0x100000000ull) return false;
    /*
     * Refuse to shadow, or be shadowed by, anything already on the bus: a
     * modelled device, another stub, or RAM. A stub that quietly overlays a
     * real device model would be far harder to notice than a rejected call —
     * and a stub inside the RAM aperture would be unreachable, because RAM is
     * on the fast path (see the routing contract in soc.h).
     */
    for (unsigned i = 0; i < NDEVICE_WINDOWS; i++)
        if (s5l8900_overlaps(base, size,
                             DEVICE_WINDOWS[i].base, DEVICE_WINDOWS[i].size))
            return false;
    for (unsigned i = 0; i < m->stub_count; i++)
        if (s5l8900_overlaps(base, size, m->stubs[i].base, m->stubs[i].size))
            return false;
    if (s5l8900_overlaps(base, size, m->ram_base, m->ram_size)) return false;
    uint64_t nregs64 = ((uint64_t)size + 3u) / 4u;
    if (nregs64 > 0xffffffffu ||
        nregs64 > (uint64_t)SIZE_MAX / sizeof(uint32_t)) return false;
    uint32_t nregs = (uint32_t)nregs64;
    uint32_t *regs = calloc((size_t)nregs, sizeof *regs);
    if (!regs) return false;

    s5l_stub_t *s = &m->stubs[m->stub_count++];
    memset(s, 0, sizeof *s);
    s->base = base; s->size = size; s->name = name;
    s->regs = regs; s->nregs = nregs;
    return true;
}

static s5l_stub_t *find_stub(s5l8900_t *m, uint32_t a, unsigned bytes) {
    for (unsigned i = 0; i < m->stub_count; i++)
        if (in_window(a, bytes, m->stubs[i].base, m->stubs[i].size))
            return &m->stubs[i];
    return NULL;
}

/* Stub windows are honest byte-addressable storage. Assemble accesses a byte
 * at a time so 8/16-bit and unaligned transactions update the correct lanes
 * without ever indexing beyond the rounded backing-register array. */
static uint32_t stub_read(const s5l_stub_t *s, uint32_t addr, unsigned bytes) {
    uint32_t v = 0;
    uint32_t rel = addr - s->base;
    for (unsigned i = 0; i < bytes; i++) {
        uint32_t at = rel + i;
        uint32_t byte = (s->regs[at >> 2] >> ((at & 3u) * 8u)) & 0xffu;
        v |= byte << (i * 8u);
    }
    return v;
}

static void stub_write(s5l_stub_t *s, uint32_t addr,
                       uint32_t val, unsigned bytes) {
    uint32_t rel = addr - s->base;
    for (unsigned i = 0; i < bytes; i++) {
        uint32_t at = rel + i;
        uint32_t shift = (at & 3u) * 8u;
        uint32_t *reg = &s->regs[at >> 2];
        *reg = (*reg & ~(0xffu << shift)) |
               (((val >> (i * 8u)) & 0xffu) << shift);
    }
}

/* ------------------------------------------------------------- reads --- */

/* Record the first distinct out-of-map addresses so a wandering guest tells us
 * which peripheral is missing rather than leaving us to guess. */
static void note_unmapped(s5l8900_t *m, uint32_t addr) {
    uint32_t page = addr & ~0xfffu;
    for (unsigned i = 0; i < m->unmapped_addr_count; i++)
        if (m->unmapped_addr[i] == page) return;
    if (m->unmapped_addr_count < S5L_UNMAPPED_LOG)
        m->unmapped_addr[m->unmapped_addr_count++] = page;
}

static void note_device(s5l8900_t *m, uint32_t addr, uint32_t val, bool is_write) {
    if (!m->trace_devices || m->dev_count >= S5L_DEVLOG) return;
    m->dev_addr[m->dev_count]     = addr;
    m->dev_value[m->dev_count]    = val;
    m->dev_is_write[m->dev_count] = is_write;
    m->dev_count++;
}

/*
 * RAM is tested first, and that is safe ONLY because s5l8900_init() has already
 * refused to build a machine whose RAM aperture overlaps a device window (and
 * s5l8900_add_stub() refuses a window inside RAM). Under that invariant "RAM
 * first" and "device first" route identically, so the cheap test can go first.
 *
 * Do not relax the invariant and leave this ordering. It used to be the case
 * that RAM won by accident rather than by proof, and at -R 512 the DRAM window
 * reached 0x28000000 and swallowed the NOR: every NOR read returned RAM-disk
 * bytes, no fault, no log, nothing to find.
 */
static uint32_t bus_read(void *ctx, uint32_t addr, unsigned bytes) {
    s5l8900_t *m = ctx;

    if (in_ram(m, addr, bytes)) {
        uint32_t v = 0;
        memcpy(&v, &m->ram[addr - m->ram_base], bytes);   /* little-endian host */
        return v;
    }
    uint32_t v;
    if ((bytes == 1u || bytes == 2u || bytes == 4u) && (addr & 3u) == 0u &&
        in_dev(addr, bytes, S5L8900_UART0_BASE)) {
        v = s5l_uart_read(&m->uart0, addr - S5L8900_UART0_BASE);
    } else if (mmio_word(addr, bytes, S5L8900_VIC0_BASE, S5L8900_DEV_SIZE)) {
        uint32_t off = addr - S5L8900_VIC0_BASE;
        /* VIC0's VICADDRESS surfaces its own sources first, then daisy-chains
         * to VIC1 (global sources 32-63) — the standard PL192 cascade, and how
         * the driver reads a single global 0-63 source number from VIC0. */
        if (off == VIC_VECTADDR) {
            v = s5l_vic_vectaddr(&m->vic[0], 0);
            if (!v) v = s5l_vic_vectaddr(&m->vic[1], 32);
        } else {
            v = s5l_vic_read(&m->vic[0], off);
        }
    } else if (mmio_word(addr, bytes, S5L8900_VIC1_BASE, S5L8900_DEV_SIZE)) {
        uint32_t off = addr - S5L8900_VIC1_BASE;
        if (off == VIC_VECTADDR) v = s5l_vic_vectaddr(&m->vic[1], 32);
        else                     v = s5l_vic_read(&m->vic[1], off);
    } else if (mmio_word(addr, bytes, S5L8900_CLCD_BASE, S5L8900_DEV_SIZE)) {
        v = s5l_clcd_read(&m->clcd, addr - S5L8900_CLCD_BASE);
    } else if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
               in_window(addr, bytes, S5L8900_TVOUT_CTRL_BASE,
                         S5L_TVOUT_BANK_SIZE)) {
        v = s5l_tvout_read(&m->tvout, S5L_TVOUT_BANK_CTRL,
                           addr - S5L8900_TVOUT_CTRL_BASE, bytes);
    } else if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
               in_window(addr, bytes, S5L8900_TVOUT_MIXER_BASE,
                         S5L_TVOUT_BANK_SIZE)) {
        v = s5l_tvout_read(&m->tvout, S5L_TVOUT_BANK_MIXER,
                           addr - S5L8900_TVOUT_MIXER_BASE, bytes);
    } else if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
               in_window(addr, bytes, S5L8900_TVOUT_SDO_BASE,
                         S5L_TVOUT_BANK_SIZE)) {
        v = s5l_tvout_read(&m->tvout, S5L_TVOUT_BANK_SDO,
                           addr - S5L8900_TVOUT_SDO_BASE, bytes);
    } else if (in_timer(addr, bytes)) {
        v = s5l_timer_read(&m->timer, addr - S5L8900_TIMER_BASE);
    } else if (in_power(addr, bytes)) {
        v = s5l_power_read(&m->power, addr - S5L8900_POWER_BASE);
    } else if (in_gpioic(addr, bytes)) {
        /* Offset from the PAGE, not from the window: the driver's own
         * 0x80/0xA0/0xC0/0xE0 are page-relative and rebasing them onto the
         * window would silently shift the whole map by 0x80. */
        v = s5l_gpioic_read(&m->gpioic, addr - S5L8900_GPIOIC_PAGE);
    } else if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
               in_dev(addr, bytes, S5L8900_GPIO_BASE)) {
        v = s5l_gpio_read(&m->gpio, addr - S5L8900_GPIO_BASE, bytes);
    } else if (mmio_word(addr, bytes, S5L8900_I2C0_BASE, S5L8900_DEV_SIZE)) {
        v = s5l_i2c_read(&m->i2c[0], addr - S5L8900_I2C0_BASE);
    } else if (mmio_word(addr, bytes, S5L8900_I2C1_BASE, S5L8900_DEV_SIZE)) {
        v = s5l_i2c_read(&m->i2c[1], addr - S5L8900_I2C1_BASE);
    } else if (mmio_word(addr, bytes, S5L8900_SPI0_BASE, S5L8900_DEV_SIZE)) {
        v = s5l_spi_read(&m->spi[0], addr - S5L8900_SPI0_BASE);
    } else if (mmio_word(addr, bytes, S5L8900_SPI1_BASE, S5L8900_DEV_SIZE)) {
        v = s5l_spi_read(&m->spi[1], addr - S5L8900_SPI1_BASE);
    } else if (mmio_word(addr, bytes, S5L8900_USB_OTG_BASE, S5L8900_DEV_SIZE)) {
        /* Word accesses only, as for the VICs, the CLCD and the I2C
         * controllers. The driver uses 32-bit accessors throughout; a narrower
         * or unaligned access to this page stays unmapped-and-counted rather
         * than being answered with a fabricated lane. */
        v = s5l_usbotg_read(&m->usbotg, addr - S5L8900_USB_OTG_BASE);
    } else if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
               in_window(addr, bytes, S5L8900_NOR_BASE, m->nor.size)) {
        v = s5l_nor_read(&m->nor, addr - S5L8900_NOR_BASE, bytes);
    } else {
        s5l_stub_t *s = find_stub(m, addr, bytes);
        if (!s) {
            m->unmapped_reads++;
            note_unmapped(m, addr);
            note_device(m, addr, 0, false);
            return 0;
        }
        s->reads++;
        v = stub_read(s, addr, bytes);
    }
    note_device(m, addr, v, false);
    return v;
}

static void bus_write(void *ctx, uint32_t addr, uint32_t val, unsigned bytes) {
    s5l8900_t *m = ctx;

    if (in_ram(m, addr, bytes)) {
        memcpy(&m->ram[addr - m->ram_base], &val, bytes);
        return;
    }
    if ((bytes == 1u || bytes == 2u || bytes == 4u) && (addr & 3u) == 0u &&
        in_dev(addr, bytes, S5L8900_UART0_BASE)) {
        note_device(m, addr, val, true);
        s5l_uart_write(&m->uart0, addr - S5L8900_UART0_BASE, val);
        return;
    }
    if (mmio_word(addr, bytes, S5L8900_VIC0_BASE, S5L8900_DEV_SIZE)) {
        s5l_vic_write(&m->vic[0], addr - S5L8900_VIC0_BASE, val);
        return;
    }
    if (mmio_word(addr, bytes, S5L8900_VIC1_BASE, S5L8900_DEV_SIZE)) {
        s5l_vic_write(&m->vic[1], addr - S5L8900_VIC1_BASE, val);
        return;
    }
    if (mmio_word(addr, bytes, S5L8900_CLCD_BASE, S5L8900_DEV_SIZE)) {
        note_device(m, addr, val, true);
        s5l_clcd_write(&m->clcd, addr - S5L8900_CLCD_BASE, val);
        return;
    }
    if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
        in_window(addr, bytes, S5L8900_TVOUT_CTRL_BASE,
                  S5L_TVOUT_BANK_SIZE)) {
        note_device(m, addr, val, true);
        s5l_tvout_write(&m->tvout, S5L_TVOUT_BANK_CTRL,
                        addr - S5L8900_TVOUT_CTRL_BASE, val, bytes);
        return;
    }
    if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
        in_window(addr, bytes, S5L8900_TVOUT_MIXER_BASE,
                  S5L_TVOUT_BANK_SIZE)) {
        note_device(m, addr, val, true);
        s5l_tvout_write(&m->tvout, S5L_TVOUT_BANK_MIXER,
                        addr - S5L8900_TVOUT_MIXER_BASE, val, bytes);
        return;
    }
    if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
        in_window(addr, bytes, S5L8900_TVOUT_SDO_BASE,
                  S5L_TVOUT_BANK_SIZE)) {
        note_device(m, addr, val, true);
        s5l_tvout_write(&m->tvout, S5L_TVOUT_BANK_SDO,
                        addr - S5L8900_TVOUT_SDO_BASE, val, bytes);
        return;
    }
    if (in_timer(addr, bytes)) {
        s5l_timer_write(&m->timer, addr - S5L8900_TIMER_BASE, val);
        return;
    }
    if (in_power(addr, bytes)) {
        note_device(m, addr, val, true);
        s5l_power_write(&m->power, addr - S5L8900_POWER_BASE, val);
        return;
    }
    if (in_gpioic(addr, bytes)) {
        note_device(m, addr, val, true);
        s5l_gpioic_write(&m->gpioic, addr - S5L8900_GPIOIC_PAGE, val);
        return;
    }
    if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
        in_dev(addr, bytes, S5L8900_GPIO_BASE)) {
        note_device(m, addr, val, true);
        s5l_gpio_write(&m->gpio, addr - S5L8900_GPIO_BASE, val, bytes);
        return;
    }
    if (mmio_word(addr, bytes, S5L8900_I2C0_BASE, S5L8900_DEV_SIZE)) {
        note_device(m, addr, val, true);
        s5l_i2c_write(&m->i2c[0], addr - S5L8900_I2C0_BASE, val);
        return;
    }
    if (mmio_word(addr, bytes, S5L8900_I2C1_BASE, S5L8900_DEV_SIZE)) {
        note_device(m, addr, val, true);
        s5l_i2c_write(&m->i2c[1], addr - S5L8900_I2C1_BASE, val);
        return;
    }
    if (mmio_word(addr, bytes, S5L8900_SPI0_BASE, S5L8900_DEV_SIZE)) {
        note_device(m, addr, val, true);
        s5l_spi_write(&m->spi[0], addr - S5L8900_SPI0_BASE, val);
        return;
    }
    if (mmio_word(addr, bytes, S5L8900_SPI1_BASE, S5L8900_DEV_SIZE)) {
        note_device(m, addr, val, true);
        s5l_spi_write(&m->spi[1], addr - S5L8900_SPI1_BASE, val);
        return;
    }
    if (mmio_word(addr, bytes, S5L8900_USB_OTG_BASE, S5L8900_DEV_SIZE)) {
        note_device(m, addr, val, true);
        s5l_usbotg_write(&m->usbotg, addr - S5L8900_USB_OTG_BASE, val);
        return;
    }
    if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
        in_window(addr, bytes, S5L8900_NOR_BASE, m->nor.size)) {
        /* Guest writes program the flash (bits can only be cleared). Note this
         * is a simplification: on real hardware the NOR is SPI-attached and is
         * programmed through controller commands rather than by storing to a
         * memory window. Modelling it as a direct write keeps the path a guest
         * payload needs — persisting itself into NOR, as an untethered
         * jailbreak does — without a full SPI protocol model. */
        s5l_nor_write(&m->nor, addr - S5L8900_NOR_BASE, val, bytes);
        return;
    }
    {
        s5l_stub_t *s = find_stub(m, addr, bytes);
        if (s) {
            s->writes++;
            note_device(m, addr, val, true);
            stub_write(s, addr, val, bytes);
            return;
        }
    }
    m->unmapped_writes++;
    note_unmapped(m, addr);
    note_device(m, addr, val, true);
}

static uint32_t r32(void *c, uint32_t a) { return bus_read(c, a, 4); }
static uint16_t r16(void *c, uint32_t a) { return (uint16_t)bus_read(c, a, 2); }
static uint8_t  r8 (void *c, uint32_t a) { return (uint8_t) bus_read(c, a, 1); }
static void w32(void *c, uint32_t a, uint32_t v) { bus_write(c, a, v, 4); }
static void w16(void *c, uint32_t a, uint16_t v) { bus_write(c, a, v, 2); }
static void w8 (void *c, uint32_t a, uint8_t  v) { bus_write(c, a, v, 1); }

/* -------------------------------------------------------- wake sources ---
 *
 * Which modelled devices can end a WFI, and how far away each one's next
 * interrupt edge is.  See the wake-source block in soc.h for the contract;
 * what follows is why this is a table at all.
 *
 * It used to be an if-chain inside machine_wait_for_interrupt() naming the
 * timer, the CLCD and TV-out.  That made the fast-forward the real definition
 * of "things that can interrupt this machine", and it was a definition nobody
 * would think to come and update: a device could be modelled correctly, assert
 * its line correctly, and still never be observed, because the idle skip
 * stepped over the tick it fired on.  Declaring a source here is now the whole
 * of wiring one into WFI, exactly as DEVICE_WINDOWS is the whole of putting a
 * window on the bus.
 *
 * These three functions are the old if-chain's conditions, unchanged.  Each
 * runs only when its VIC line is enabled, which is the test the chain applied
 * to all three by hand.
 */

/*
 * Can `line` reach the CPU at all?  The flat 0-63 numbering is the device
 * tree's own and both VICs drive the core (see s5l8900_tick), so the gate is
 * simply "enabled in the VIC that carries it".  A line outside the pair is not
 * one this machine can route: s5l_vic_set_line() drops it, so nothing could
 * ever assert it and there is nothing to wait for.
 *
 * CPSR is deliberately not consulted — an ARM1176 completes WFI on the raw
 * line whether or not the exception is masked — and neither is INTSELECT,
 * because IRQ and FIQ both wake the core.
 */
static bool wake_line_enabled(const s5l8900_t *m, unsigned line) {
    if (line >= 32u * S5L8900_VIC_COUNT) return false;
    return (m->vic[line / 32u].enable & (1u << (line % 32u))) != 0u;
}

/* Timer 4's decrementer.  A zero live value has not been loaded yet, so the
 * first tick reloads it from the buffered count; zero in both is a timer that
 * has stopped and will not expire again. */
static s5l_wake_kind_t wake_edge_timer(const s5l8900_t *m, uint32_t *ticks) {
    if ((m->timer.t4_state & TIMER4_STATE_START) == 0u) return S5L_WAKE_NEVER;
    uint32_t until = m->timer.t4_value ? m->timer.t4_value : m->timer.t4_count;
    if (until == 0u) return S5L_WAKE_NEVER;
    *ticks = until;
    return S5L_WAKE_AT;
}

/* The CLCD's frame (VBL) latch, which only exists while scanout is live and
 * the controller's own mask lets it through. */
static s5l_wake_kind_t wake_edge_clcd(const s5l8900_t *m, uint32_t *ticks) {
    if ((m->clcd.intmask & CLCD_INT_FRAME) == 0u ||
        !s5l_clcd_running(&m->clcd) || m->clcd.frame_ticks == 0u)
        return S5L_WAKE_NEVER;
    /* frame_accum is normally strictly below frame_ticks.  If a malformed
     * in-memory caller violates that invariant, one tick is the only safe
     * boundary: the CLCD normalizes it and asserts the frame latch there. */
    *ticks = m->clcd.frame_accum < m->clcd.frame_ticks
           ? m->clcd.frame_ticks - m->clcd.frame_accum
           : 1u;
    return S5L_WAKE_AT;
}

/* TV-out SDO VSYNC.  The device answers "no deliverable VSYNC" with zero,
 * which is a statement about its state (stopped, masked, already pending) and
 * therefore S5L_WAKE_NEVER rather than an unknown. */
static s5l_wake_kind_t wake_edge_tvout(const s5l8900_t *m, uint32_t *ticks) {
    uint32_t until = s5l_tvout_ticks_to_vsync(&m->tvout);
    if (until == 0u) return S5L_WAKE_NEVER;
    *ticks = until;
    return S5L_WAKE_AT;
}

/*
 * The two SPI controllers.
 *
 * Both answer S5L_WAKE_NEVER unconditionally, and that is a statement about the
 * model rather than a placeholder. An SPI word completes as a direct
 * consequence of a guest store into the transmit FIFO, and a core in WFI issues
 * no stores; a line that was already high before the WFI is caught by
 * machine_wait_for_interrupt()'s own pre-check, before any source is consulted.
 * So there is no future edge to name, and NEVER — the only answer that lets
 * guest time be skipped — is the correct one. Answering S5L_WAKE_UNKNOWN
 * instead would be safe but would stop the machine fast-forwarding ANY idle
 * period for the whole boot.
 *
 * They are declared anyway, because the table is this machine's definition of
 * what can interrupt it (see the wake-source block in soc.h). A touch device
 * that raises a report on its own schedule rather than in reply to a store is
 * exactly the case the table exists for, and this is the entry it edits.
 */
static s5l_wake_kind_t wake_edge_spi0(const s5l8900_t *m, uint32_t *ticks) {
    (void)m; (void)ticks;
    return S5L_WAKE_NEVER;
}
static s5l_wake_kind_t wake_edge_spi1(const s5l8900_t *m, uint32_t *ticks) {
    (void)m; (void)ticks;
    return S5L_WAKE_NEVER;
}

/*
 * The seven GPIO interrupt groups.
 *
 * All seven are declared, not just group 4. Nothing else in the shipped tree
 * claims VIC lines 0, 1, 2, 3, 31, 32 or 33, so declaring the whole cascade
 * costs nothing and removes the question of whether /arm-io/gpio's descending
 * `interrupts` array is indexed by group or reversed — get that wrong for one
 * entry and the touch line is routed to a VIC line nobody enabled, silently.
 * Lines 32 and 33 are on VIC1, which is why s5l8900_tick() has to route by
 * line/32 rather than hardcoding vic[0] the way every older device does.
 *
 * Every one answers S5L_WAKE_NEVER, and that survives the touch controller
 * gaining a report to deliver. The reasoning is the same as the SPI
 * controllers': a future edge is only worth naming if this machine can produce
 * one while the core sleeps, and the multitouch attention line moves at exactly
 * two moments — a host call to s5l_mtz2_set_contacts(), which happens BETWEEN
 * guest instructions and therefore never inside a skipped interval, and the
 * last byte of a data read, which is a consequence of a guest store the sleeping
 * core is not making. A line that is ALREADY asserted is caught by
 * machine_wait_for_interrupt()'s own pre-check — it refreshes at zero elapsed
 * time, which routes s5l_mtz2_irq() through the cascade before any source is
 * consulted — so NEVER cannot lose an interrupt that has already happened, and
 * an injection made during a WFI is observed on the very next refresh.
 *
 * The entry that matters is `gpio-group4`, and the number on it is 2 — the
 * CASCADE VIC line, not 155. wake_line_enabled() rejects anything at or above
 * 32 * S5L8900_VIC_COUNT, so a source written as `{ "multitouch", 155, ... }`
 * would return false silently and could never wake the core.
 */
static s5l_wake_kind_t wake_edge_gpio(const s5l8900_t *m, uint32_t *ticks) {
    (void)m; (void)ticks;
    return S5L_WAKE_NEVER;
}

static const s5l_wake_source_t WAKE_SOURCES[] = {
    { "timer", S5L8900_IRQ_TIMER, wake_edge_timer },
    { "clcd",  S5L8900_IRQ_CLCD,  wake_edge_clcd  },
    { "tvout", S5L8900_IRQ_TVOUT, wake_edge_tvout },
    { "spi0",  S5L8900_IRQ_SPI0,  wake_edge_spi0  },
    { "spi1",  S5L8900_IRQ_SPI1,  wake_edge_spi1  },
    /* Group order, so entry k is group k. The lines are /arm-io/gpio's own
     * `interrupts` = {33,32,31,3,2,1,0}; group 4 -> VIC line 2 carries touch. */
    { "gpio-group0", 33u, wake_edge_gpio },
    { "gpio-group1", 32u, wake_edge_gpio },
    { "gpio-group2", 31u, wake_edge_gpio },
    { "gpio-group3",  3u, wake_edge_gpio },
    { "gpio-group4",  2u, wake_edge_gpio },
    { "gpio-group5",  1u, wake_edge_gpio },
    { "gpio-group6",  0u, wake_edge_gpio },
};
#define NWAKE_SOURCES (sizeof WAKE_SOURCES / sizeof WAKE_SOURCES[0])

unsigned s5l8900_wake_sources(const s5l_wake_source_t **out) {
    if (out) *out = WAKE_SOURCES;
    return (unsigned)NWAKE_SOURCES;
}

s5l_wake_kind_t s5l8900_next_wake(const s5l8900_t *m,
                                  const s5l_wake_source_t *src, unsigned n,
                                  uint32_t *ticks) {
    /* Nothing to reason about is not the same as nothing to wait for, so both
     * of these decline to skip time rather than reporting a clear horizon. */
    if (!m) return S5L_WAKE_UNKNOWN;
    if (n != 0u && !src) return S5L_WAKE_UNKNOWN;

    bool have = false;
    uint32_t best = 0;
    for (unsigned i = 0; i < n; i++) {
        /* A masked line cannot reach the CPU, so its device cannot end this
         * wait however close its next edge is.  Skipping it here is what makes
         * the old chain's per-source VIC test unnecessary. */
        if (!wake_line_enabled(m, src[i].line)) continue;
        if (!src[i].next_edge) return S5L_WAKE_UNKNOWN;   /* declared, silent */

        uint32_t at = 0;
        s5l_wake_kind_t kind = src[i].next_edge(m, &at);
        if (kind == S5L_WAKE_NEVER) continue;
        /* Anything that is not a future distance stops the fast-forward: an
         * unknown, or an "at" of zero, which cannot be a moment still to come
         * (the caller has already refreshed every line at zero elapsed time). */
        if (kind != S5L_WAKE_AT || at == 0u) return S5L_WAKE_UNKNOWN;
        if (!have || at < best) { best = at; have = true; }
    }

    if (!have) return S5L_WAKE_NEVER;
    if (ticks) *ticks = best;
    return S5L_WAKE_AT;
}

/*
 * Complete one ARM1176 Wait For Interrupt operation without manufacturing CPU
 * work.  The earliest edge any declared wake source can route through the VIC
 * is the earliest point at which this model can wake the core.  Advancing
 * farther would coalesce guest-visible work across an interrupt; advancing
 * less would merely replace the kernel's idle loop with a host idle loop.
 */
static bool machine_wait_for_interrupt(void *ctx) {
    s5l8900_t *m = ctx;
    if (!m) return false;

    /* A line that is already high completes WFI even when CPSR masks it.  This
     * check precedes the controller refresh so an externally injected CPU line
     * is not lost. */
    if (m->cpu.irq_line || m->cpu.fiq_line) return true;

    /* Guest writes can change a VIC enable/mask or acknowledge a source after
     * the preceding instruction's tick.  Refresh level outputs at zero elapsed
     * time before deciding whether a future edge is necessary. */
    s5l8900_tick(m, 0);
    if (m->cpu.irq_line || m->cpu.fiq_line) return true;

    /*
     * With no enabled source able to name a future edge — because none has one
     * (NEVER), or because one of them cannot say (UNKNOWN) — there is nothing
     * safe to fast-forward to.  Return to the interpreter's documented no-op
     * fallback rather than hanging the host forever, inventing an interrupt, or
     * skipping guest time past a source that might have fired inside it.  The
     * guest still makes progress: the WFI retires, and the run loop's own tick
     * moves time on by one.
     */
    uint32_t edge_tb = 0;
    const s5l_wake_source_t *sources = NULL;
    unsigned nsources = s5l8900_wake_sources(&sources);
    if (s5l8900_next_wake(m, sources, nsources, &edge_tb) != S5L_WAKE_AT)
        return false;

    uint64_t cpu_ticks;
    if (m->cpu_hz && m->tb_hz) {
        /* s5l8900_tick's integer converter can cross at most one timebase edge
         * per CPU tick only in this direction.  Refuse unusual inverted or
         * corrupt ratios instead of jumping past the earliest edge. */
        if (m->tb_hz > m->cpu_hz || m->tb_accum >= m->cpu_hz) return false;
        uint64_t need = (uint64_t)edge_tb * m->cpu_hz - m->tb_accum;
        cpu_ticks = need / m->tb_hz + (need % m->tb_hz != 0u);
    } else {
        /* Existing zero-clock fallback: s5l8900_tick treats its input as raw
         * timebase ticks when either advertised rate is zero. */
        cpu_ticks = edge_tb;
    }

    /* The public tick API is 32-bit.  Split a very long wait without changing
     * the exact fractional accumulator; no selected wake edge occurs before
     * the total derived above. */
    while (cpu_ticks > UINT32_MAX) {
        s5l8900_tick(m, UINT32_MAX);
        cpu_ticks -= UINT32_MAX;
        if (m->cpu.irq_line || m->cpu.fiq_line) return true;
    }
    if (cpu_ticks) s5l8900_tick(m, (uint32_t)cpu_ticks);
    return m->cpu.irq_line || m->cpu.fiq_line;
}

/* ----------------------------------------------------------- lifecycle --- */

bool s5l8900_init(s5l8900_t *m, uint32_t ram_base, uint32_t ram_size) {
    if (!m) return false;
    memset(m, 0, sizeof *m);

    /*
     * Refuse an aliasing configuration before allocating anything.
     *
     * This is the whole routing contract in three lines (soc.h explains why it
     * lives here rather than in bus_read). A machine whose DRAM window covers a
     * device window can only ever be one of two wrong things: a device that
     * cannot be reached, or a hole in the DRAM bank the guest was promised.
     * Neither is worth having, and both are invisible at run time, so the
     * machine simply does not exist.
     */
    if (!ram_size || (uint64_t)ram_base + ram_size > 0x100000000ull ||
        s5l8900_ram_conflict(ram_base, ram_size)) return false;

    m->ram = calloc(ram_size, 1);
    if (!m->ram) return false;
    m->ram_base = ram_base;
    m->ram_size = ram_size;
    m->cpu_hz   = S5L8900_CPU_HZ;
    m->tb_hz    = S5L8900_TB_HZ;

    s5l_uart_reset(&m->uart0);
    for (unsigned i = 0; i < S5L8900_VIC_COUNT; i++) s5l_vic_reset(&m->vic[i]);
    s5l_timer_reset(&m->timer);
    s5l_power_reset(&m->power);
    s5l_clcd_reset(&m->clcd);
    s5l_tvout_reset(&m->tvout, m->tb_hz);
    for (unsigned i = 0; i < S5L8900_I2C_COUNT; i++)
        s5l_i2c_reset(&m->i2c[i]);
    s5l_pcf50635_reset(&m->pmu, m->tb_hz);
    for (unsigned i = 0; i < S5L8900_SPI_COUNT; i++) s5l_spi_reset(&m->spi[i]);
    s5l_gpioic_reset(&m->gpioic);
    s5l_gpio_reset(&m->gpio);
    s5l_usbotg_reset(&m->usbotg);
    {
        s5l_i2c_slave_t pmu;
        s5l_pcf50635_bind(&m->pmu, &pmu);
        if (!s5l_i2c_attach(&m->i2c[0], &pmu)) {
            free(m->ram);
            m->ram = NULL;
            return false;
        }
    }
    /*
     * The touch controller on spi1 chip select 0 — where
     * /arm-io/spi1/multi-touch is; the select is reg[0] of that node, since
     * there is no `chip-select` property anywhere in the shipped tree. It
     * replaces the null device that proved the controller: that one answered
     * every word with 0x00, which made AppleMultitouchZ2SPI's HBPP probe
     * complete and fail cleanly instead of sleeping with no deadline, but a
     * clean failure still detaches the driver.
     *
     * spi0 gets nothing on purpose. Its devices — the NOR, which this machine
     * exposes as a memory window instead, and the lcd0 panel — are unmodelled,
     * and a controller with no attached device shifts no words, so spi0 behaves
     * exactly as its storage stub did rather than answering the panel driver
     * with a byte no device sent.
     */
    s5l_mtz2_reset(&m->mtz2);
    {
        s5l_spi_slave_t touch;
        s5l_mtz2_bind(&m->mtz2, &touch);
        if (!s5l_spi_attach(&m->spi[1], 0u, &touch)) {
            free(m->ram);
            m->ram = NULL;
            return false;
        }
    }
    /*
     * The three GPIO pins the touch controller can observe.
     *
     * `function-reset` (GPIO 0x0606 = group 6 bit 6) and `function-power_ldo`
     * (0x0701 = group 7 bit 1) are /arm-io/spi1/multi-touch's own;
     * `function-spi_cs0` (0x1800 = group 24 bit 0) is /arm-io/spi1's. Only the
     * reset line changes behaviour — it is what tells resetDevice's dummy
     * transfer from the probe that follows it. The select is a framer resync
     * the device does not need, and the power line is recorded and not acted
     * on (see s5l_mtz2_power_pin). A failure to subscribe is folded into the
     * same counter a refused stub declaration is, rather than refusing to build
     * a machine over a diagnostic.
     */
    if (!s5l_gpio_watch(&m->gpio, MTZ2_PIN_RESET, &m->mtz2, s5l_mtz2_reset_pin))
        m->stub_declare_failures++;
    if (!s5l_gpio_watch(&m->gpio, MTZ2_PIN_SELECT, &m->mtz2, s5l_mtz2_select_pin))
        m->stub_declare_failures++;
    if (!s5l_gpio_watch(&m->gpio, MTZ2_PIN_POWER, &m->mtz2, s5l_mtz2_power_pin))
        m->stub_declare_failures++;
    /* Refresh in the guest's own time: the CLCD is ticked with timebase ticks,
     * so the period is expressed in them. */
    m->clcd.frame_ticks = m->tb_hz / S5L_CLCD_REFRESH_HZ;
    if (!s5l_nor_init(&m->nor, S5L8900_NOR_SIZE)) { free(m->ram); m->ram = NULL; return false; }

    /*
     * Peripheral windows we have identified but not modelled. Each base was
     * resolved twice — from the VA the guest's own driver printed, walked
     * through its live page tables, and from the shipped device tree's arm-io
     * ranges (child + 0x38000000) — and the two agree. They are declared so
     * their traffic is named and stored instead of reading back as the zero an
     * unmapped access returns. See s5l_stub_t for why a stub is honest here and
     * where the line is that turns one into a real device model.
     *
     * "Declared" is deliberately weaker than "harmless". The SPI trio below is
     * on the current boot frontier: run23 proved that SpringBoard's first
     * CoreTelephony request lands on a launchd-held service queue that
     * CommCenter has never taken, and CommCenter's only IOKit subscription is
     * to AppleBaseband, whose reset IOInterruptEventSource sits on GPIO
     * interrupt 75 and has never fired. Storing spi2's registers does not fix
     * that and is not claimed to; it stops one identified peripheral from
     * answering the shipped driver with a fabricated zero.
     *
     * A failure to declare one is not fatal but must not be silent, so the
     * result is folded into a counter the caller can see.
     */
    {
        static const struct { uint32_t base, size; const char *name; } STUBS[] = {
            /* AppleS5L8900XClockController _ccBaseAddress. */
            { S5L8900_CLOCK_BASE,  S5L8900_DEV_SIZE,   "clkrstgen" },
            /* _miuBaseAddress, the clock controller's second reg range.
             * Offsets 0x008 and 0x404 are the ones actually touched. */
            { S5L8900_MIU_BASE,    S5L8900_DEV_SIZE,   "miu"       },
            { S5L8900_EDGEIC_BASE, S5L8900_DEV_SIZE,   "edgeic"    },
            /* gpio and gpioic used to be here. Both are device models now
             * (DEVICE_WINDOWS above) — a duplicate declaration would be
             * refused as an overlap and counted as a failure rather than
             * shadowing them. */
            /* spi2, the baseband transport com.apple.driver.BasebandSPI
             * configures and then reads back. spi0 and spi1 used to be here
             * too; they are device models now (DEVICE_WINDOWS above), and a
             * duplicate declaration here would be refused as an overlap and
             * silently counted as a failure rather than shadowing them. */
            { S5L8900_SPI2_BASE,   S5L8900_DEV_SIZE,   "spi2"      },
        };
        for (unsigned i = 0; i < sizeof STUBS / sizeof STUBS[0]; i++)
            if (!s5l8900_add_stub(m, STUBS[i].base, STUBS[i].size, STUBS[i].name))
                m->stub_declare_failures++;
    }

    m->bus.ctx = m;
    m->bus.read32 = r32; m->bus.read16 = r16; m->bus.read8 = r8;
    m->bus.write32 = w32; m->bus.write16 = w16; m->bus.write8 = w8;
    m->bus.wait_for_interrupt = machine_wait_for_interrupt;

    arm_reset(&m->cpu, &m->bus);
    return true;
}

void s5l8900_free(s5l8900_t *m) {
    if (!m) return;
    for (unsigned i = 0; i < m->stub_count; i++) {
        free(m->stubs[i].regs);
        m->stubs[i].regs = NULL;
    }
    m->stub_count = 0;
    free(m->ram);
    m->ram = NULL;
    s5l_nor_free(&m->nor);
}

void s5l8900_load(s5l8900_t *m, uint32_t addr, const void *data, size_t len) {
    if (!m || !data || !len) return;
    /* Check before narrowing: a >4 GiB length must not truncate into range. */
    if (len > 0xffffffffu) return;
    if (!in_ram(m, addr, (uint32_t)len)) return;
    memcpy(&m->ram[addr - m->ram_base], data, len);
}

void s5l8900_tick(s5l8900_t *m, uint32_t ticks) {
    /*
     * Convert elapsed emulated CPU-clock ticks into timebase ticks at the
     * guest's own CPU:timebase ratio, carrying the remainder so it stays exact
     * rather than drifting. Active execution contributes one such tick per
     * retired instruction; WFI contributes elapsed idle ticks without
     * changing cpu.cycles. Feeding ticks straight into the timebase runs guest
     * time ~68x fast and livelocks the kernel's decrementer.
     */
    uint32_t tb = ticks;
    if (m->cpu_hz && m->tb_hz) {
        m->tb_accum += (uint64_t)ticks * m->tb_hz;
        tb = (uint32_t)(m->tb_accum / m->cpu_hz);
        m->tb_accum %= m->cpu_hz;
    }

    /* Devices advance, then the controllers recompute what the CPU sees. */
    bool timer_irq = s5l_timer_tick(&m->timer, tb);
    s5l_vic_set_line(&m->vic[0], S5L8900_IRQ_TIMER, timer_irq);

    bool clcd_irq = s5l_clcd_tick(&m->clcd, tb);
    s5l_vic_set_line(&m->vic[0], S5L8900_IRQ_CLCD, clcd_irq);

    bool tvout_irq = s5l_tvout_tick(&m->tvout, tb);
    s5l_vic_set_line(&m->vic[0], S5L8900_IRQ_TVOUT, tvout_irq);

    /* I2C transfers complete synchronously with their command store, then
     * remain level-asserted until the driver's W1C acknowledge. A zero-tick
     * refresh is therefore enough for WFI to observe both assertion and
     * deassertion without advancing guest time. */
    s5l_pcf50635_tick(&m->pmu, tb);
    s5l_vic_set_line(&m->vic[0], S5L8900_IRQ_I2C0,
                     s5l_i2c_irq(&m->i2c[0]));
    s5l_vic_set_line(&m->vic[0], S5L8900_IRQ_I2C1,
                     s5l_i2c_irq(&m->i2c[1]));

    /* SPI words complete inside the store that pushes them, for the same reason
     * and with the same consequence: a zero-tick refresh is all WFI needs to
     * observe both the assertion and the guest's W1C acknowledge. */
    s5l_vic_set_line(&m->vic[0], S5L8900_IRQ_SPI0, s5l_spi_irq(&m->spi[0]));
    s5l_vic_set_line(&m->vic[0], S5L8900_IRQ_SPI1, s5l_spi_irq(&m->spi[1]));

    /*
     * The GPIO interrupt cascade. Seven group outputs, seven VIC lines, and
     * two of those lines are 32 and 33 — the FIRST sources this machine has
     * ever routed to VIC1. s5l_vic_set_line() takes a per-controller 0-31 line
     * and silently drops anything larger, so the flat device-tree number has
     * to be split here; passing 33 to vic[0] would enable nothing and fail
     * without a symptom.
     */
    /* The touch controller's attention line first, so the cascade below sees
     * it in the same refresh. It is asserted only when the device has a frame
     * waiting, which nothing produces yet. */
    s5l_gpioic_set_line(&m->gpioic, S5L_GPIOIC_LINE_MULTITOUCH,
                        s5l_mtz2_irq(&m->mtz2));

    for (unsigned g = 0; g < S5L_GPIOIC_GROUPS; g++) {
        unsigned line = s5l_gpioic_cascade(g);
        if (line >= 32u * S5L8900_VIC_COUNT) continue;
        s5l_vic_set_line(&m->vic[line / 32u], line % 32u,
                         s5l_gpioic_group_irq(&m->gpioic, g));
    }

    /*
     * BOTH VICs drive the CPU.
     *
     * Only VIC0 used to, which was fine while VIC0 was the only one mapped. It
     * is not defensible now: the device tree numbers interrupts flat across the
     * pair — /arm-io/gpio lists lines 0x20 and 0x21, /arm-io/wdt 0x33,
     * /arm-io/sdio 0x2a, /arm-io/edgeic 0x23 and 0x29 — and every one of those
     * is past VIC0's 32 lines, so it lives on VIC1. Leaving VIC1's outputs
     * disconnected would mean the watchdog, the SD controller and the GPIO
     * interrupt controller could be correctly programmed, correctly asserted,
     * and still never reach the CPU: a silent failure, and precisely the kind
     * this core exists to avoid.
     *
     * OR-ing the two is the standard PL192 cascade. Nothing asserts a VIC1 line
     * today, so this changes no behaviour now; it removes a trap for the next
     * device that needs a line above 31.
     */
    bool irq = false, fiq = false;
    for (unsigned i = 0; i < S5L8900_VIC_COUNT; i++) {
        irq |= s5l_vic_irq(&m->vic[i]);
        fiq |= s5l_vic_fiq(&m->vic[i]);
    }
    m->cpu.irq_line = irq;
    m->cpu.fiq_line = fiq;
}

unsigned s5l8900_run(s5l8900_t *m, unsigned max_steps, arm_status_t *status) {
    arm_status_t st = ARM_OK;
    unsigned n = 0;
    for (; n < max_steps; n++) {
        st = arm_step(&m->cpu);
        if (st != ARM_OK) break;
        s5l8900_tick(m, 1);
    }
    if (status) *status = st;
    return n;
}
