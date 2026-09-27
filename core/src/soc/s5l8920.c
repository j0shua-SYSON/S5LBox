/* N88 RAM, PL192, UART and timebase, separate from the S5L8900 machine.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920.h"
#include <stdlib.h>
#include <string.h>

static bool ram_offset(const s5l8920_t *m, uint32_t address, size_t size,
                        uint32_t *offset) {
    if (!m || !m->ram || size > S5L8920_RAM_SIZE) return false;
    if (address >= S5L8920_RAM_BASE &&
        address - S5L8920_RAM_BASE <= S5L8920_RAM_SIZE - size) {
        *offset = address - S5L8920_RAM_BASE;
        return true;
    }
    if (m->ram_boot_window && address <= S5L8920_RAM_BOOT_WINDOW - size) {
        *offset = address;
        return true;
    }
    return false;
}

/* Matching DT: three banks, 64 KiB stride, each with a 4 KiB PL192 aperture.
 * DDI0273A 2.3.2 blocking wiring: no acknowledgement is forwarded downstream.
 * Logical levels are propagated without asserting physical timing accuracy. */
static void refresh_interrupts(s5l8920_t *m) {
    (void)pl192_set_line(&m->vic[0],S5L8920_UART0_IRQ,
                        (m->input_levels[0]&(1u<<S5L8920_UART0_IRQ))!=0u ||
                        s5l8920_uart_irq(&m->uart0));
    pl192_set_daisy(&m->vic[S5L8920_VIC_COUNT - 1u],false,false,0u);
    for (unsigned bank = S5L8920_VIC_COUNT - 1u; bank > 0u; bank--)
        pl192_set_daisy(&m->vic[bank - 1u],pl192_irq(&m->vic[bank]),
                       pl192_fiq(&m->vic[bank]),pl192_vector(&m->vic[bank]));
    m->cpu.irq_line = pl192_irq(&m->vic[0]);
    m->cpu.fiq_line = pl192_fiq(&m->vic[0]);
}

static bool access_failed(void *ctx) {
    return ((const s5l8920_t *)ctx)->bus_failure.reason != S5L8920_BUS_OK;
}

static void fail(s5l8920_t *m, s5l8920_bus_reason_t reason, uint32_t address,
                 unsigned size, bool write, uint32_t value) {
    if (!access_failed(m))
        m->bus_failure = (s5l8920_bus_failure_t){reason,address,m->cpu.r[15],value,size,write};
}

static bool decode_vic(uint32_t address, unsigned *bank, uint32_t *offset) {
    if (address < S5L8920_VIC_BASE) return false;
    uint32_t relative = address - S5L8920_VIC_BASE;
    *bank = relative / S5L8920_VIC_STRIDE;
    *offset = relative % S5L8920_VIC_STRIDE;
    return *bank < S5L8920_VIC_COUNT && *offset < 0x1000u;
}

static bool vic_access_supported(const pl192_t *v, uint32_t offset) {
    /* The bus has no privilege sideband for LDRT/STRT or page-table walks.
     * Do not infer it from CPSR. Leave protection programming unavailable;
     * with protection clear, the other implemented selectors are accessible
     * to both privilege levels. The board's PL192 revision is also unverified. */
    return offset != PL192_PROTECTION && !v->protection && offset < 0xfe0u;
}

static uint32_t read_value(s5l8920_t *m, uint32_t address, unsigned size) {
    if (access_failed(m)) return 0u;
    uint32_t ram_at;
    if (ram_offset(m,address,size,&ram_at)) {
        const uint8_t *p = m->ram + ram_at;
        uint32_t value = 0u;
        for (unsigned n = 0; n < size; n++) value |= (uint32_t)p[n] << (8u * n);
        return value;
    }
    unsigned bank; uint32_t offset, value;
    if (address>=S5L8920_PMGR_BASE && address-S5L8920_PMGR_BASE<0x2000u) {
        offset=address-S5L8920_PMGR_BASE;
        if (size!=4u || (offset&3u))
            fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,false,0u);
        else if (offset==S5L8920_TIMEBASE_LOW) return (uint32_t)m->timebase_ticks;
        else if (offset==S5L8920_TIMEBASE_HIGH) return (uint32_t)(m->timebase_ticks>>32);
        else fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,false,0u);
        return 0u;
    }
    if (address>=S5L8920_UART0_BASE && address-S5L8920_UART0_BASE<0x1000u) {
        offset=address-S5L8920_UART0_BASE;
        if (size!=4u || (offset&3u))
            fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,false,0u);
        else if (!s5l8920_uart_read(&m->uart0,offset,&value))
            fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,false,0u);
        else { refresh_interrupts(m); return value; }
        return 0u;
    }
    if (!decode_vic(address,&bank,&offset)) {
        fail(m,S5L8920_BUS_UNMAPPED,address,size,false,0u);
    } else if (size != 4u || (offset & 3u)) {
        fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,false,0u);
    } else if (!vic_access_supported(&m->vic[bank],offset) ||
               !pl192_read(&m->vic[bank],offset,true,&value)) {
        fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,false,0u);
    } else {
        refresh_interrupts(m);
        return value;
    }
    return 0u; /* The checked-bus contract prevents the CPU consuming this. */
}

static void write_value(s5l8920_t *m, uint32_t address, unsigned size, uint32_t value) {
    if (access_failed(m)) return;
    uint32_t ram_at;
    if (ram_offset(m,address,size,&ram_at)) {
        uint8_t *p = m->ram + ram_at;
        for (unsigned n = 0; n < size; n++) p[n] = (uint8_t)(value >> (8u * n));
        return;
    }
    unsigned bank; uint32_t offset;
    if (address>=S5L8920_PMGR_BASE && address-S5L8920_PMGR_BASE<0x2000u) {
        fail(m,(size!=4u || (address&3u))?S5L8920_BUS_ACCESS_UNIMPLEMENTED:
             S5L8920_BUS_REGISTER_REFUSED,address,size,true,value);
        return;
    }
    if (address>=S5L8920_UART0_BASE && address-S5L8920_UART0_BASE<0x1000u) {
        offset=address-S5L8920_UART0_BASE;
        if (size!=4u || (offset&3u))
            fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,true,value);
        else if (!s5l8920_uart_write(&m->uart0,offset,value))
            fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,true,value);
        else refresh_interrupts(m);
        return;
    }
    if (!decode_vic(address,&bank,&offset)) {
        fail(m,S5L8920_BUS_UNMAPPED,address,size,true,value);
    } else if (size != 4u || (offset & 3u)) {
        fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,true,value);
    } else if (!vic_access_supported(&m->vic[bank],offset) ||
               !pl192_write(&m->vic[bank],offset,true,value)) {
        fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,true,value);
    } else refresh_interrupts(m);
}

#define ACCESSORS(bits) \
static uint##bits##_t read##bits(void *ctx, uint32_t address) { \
    return (uint##bits##_t)read_value(ctx,address,(bits)/8u); \
} \
static void write##bits(void *ctx, uint32_t address, uint##bits##_t value) { \
    write_value(ctx,address,(bits)/8u,value); \
}
ACCESSORS(8)
ACCESSORS(16)
ACCESSORS(32)

static uint8_t *host_ram(void *ctx, uint32_t address, uint32_t size) {
    s5l8920_t *m = ctx;
    uint32_t offset;
    return size && ram_offset(m,address,size,&offset) ? m->ram + offset : NULL;
}

bool s5l8920_load(s5l8920_t *m, uint32_t address, const void *data, size_t size) {
    uint32_t offset;
    if ((!data && size) || !ram_offset(m,address,size,&offset)) return false;
    if (size) memmove(m->ram + offset,data,size);
    return true;
}

bool s5l8920_set_ram_boot_window(s5l8920_t *m, bool enabled) {
    if (!m || !m->ram) return false;
    if (m->ram_boot_window != enabled) {
        m->ram_boot_window = enabled;
        arm_mmu_tlb_flush(&m->cpu);
        m->cpu.excl_valid = false;
        m->cpu.a8_excl_size = 0u;
    }
    return true;
}

void s5l8920_clear_bus_failure(s5l8920_t *m) {
    if (m) memset(&m->bus_failure,0,sizeof m->bus_failure);
}

bool s5l8920_uart0_clock(s5l8920_t *m,bool nclk,uint64_t ticks,
                        uint8_t *output,size_t capacity,size_t *count) {
    if (!m || !m->ram || !s5l8920_uart_clock(&m->uart0,nclk,ticks,output,capacity,count)) return false;
    refresh_interrupts(m);
    return true;
}

bool s5l8920_uart0_receive(s5l8920_t *m,uint8_t byte) {
    if (!m || !m->ram || !s5l8920_uart_receive(&m->uart0,byte)) return false;
    refresh_interrupts(m);
    return true;
}

bool s5l8920_timebase_clock(s5l8920_t *m,uint64_t ticks) {
    if (!m || !m->ram) return false;
    m->timebase_ticks+=ticks;
    return true;
}

bool s5l8920_set_irq(s5l8920_t *m, unsigned source, bool asserted) {
    if (!m || !m->ram || source >= S5L8920_IRQ_COUNT) return false;
    unsigned bank = source / 32u, line = source % 32u;
    if (asserted) m->input_levels[bank] |= 1u << line;
    else m->input_levels[bank] &= ~(1u << line);
    (void)pl192_set_line(&m->vic[bank],line,asserted);
    refresh_interrupts(m);
    return true;
}

bool s5l8920_reset(s5l8920_t *m) {
    if (!m || !m->ram) return false;
    if (!arm_reset_profile(&m->cpu,&m->bus,ARM_ARCH_V7_CORTEX_A8)) return false;
    m->ram_boot_window = false;
    s5l8920_uart_reset(&m->uart0);
    m->timebase_ticks=0u;
    for (unsigned bank = 0; bank < S5L8920_VIC_COUNT; bank++) {
        pl192_reset(&m->vic[bank]);
        for (unsigned line = 0; line < 32u; line++)
            (void)pl192_set_line(&m->vic[bank],line,(m->input_levels[bank] & (1u << line)) != 0u);
    }
    s5l8920_clear_bus_failure(m);
    refresh_interrupts(m);
    return true;
}

bool s5l8920_init(s5l8920_t *m) {
    if (!m || m->ram) return false;
    uint8_t *ram = calloc(1u,S5L8920_RAM_SIZE);
    if (!ram) return false;
    memset(m,0,sizeof *m);
    m->ram = ram;
    m->bus = (arm_bus_t){.ctx=m,.read8=read8,.read16=read16,.read32=read32,
                        .write8=write8,.write16=write16,.write32=write32,
                        .host_ram=host_ram,.host_ram_write=host_ram,.access_failed=access_failed};
    if (s5l8920_reset(m)) return true;
    s5l8920_free(m);
    return false;
}

void s5l8920_free(s5l8920_t *m) {
    if (!m) return;
    free(m->ram);
    memset(m,0,sizeof *m);
}
