/* N88 RAM, PL192, UART, timebase and GPIO, separate from the S5L8900 machine.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920.h"
#include "lzss.h"
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
    (void)pl192_set_line(&m->vic[0],S5L8920_DEADLINE_IRQ,
                        (m->input_levels[0]&(1u<<S5L8920_DEADLINE_IRQ))!=0u ||
                        m->deadline.pending);
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
    if (address>=S5L8920_GPIO_BASE && address-S5L8920_GPIO_BASE<0x1000u) {
        offset=address-S5L8920_GPIO_BASE;
        if (size!=4u || (offset&3u)) {
            fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,false,0u);
        } else if (offset/4u<S5L8920_GPIO_PIN_COUNT) {
            const s5l8920_gpio_pin_t *pin=&m->gpio[offset/4u];
            if (pin->programmed && ((pin->control&2u) || pin->input_valid))
                return (pin->control&2u) ? pin->control :
                       (pin->control&~1u)|(pin->input_high ? 1u:0u);
            fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,false,0u);
        } else fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,false,0u);
        return 0u;
    }
    if (address>=S5L8920_PMGR_BASE && address-S5L8920_PMGR_BASE<0x2000u) {
        offset=address-S5L8920_PMGR_BASE;
        if (size!=4u || (offset&3u))
            fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,false,0u);
        else if (offset==S5L8920_TIMEBASE_LOW) return (uint32_t)m->timebase_ticks;
        else if (offset==S5L8920_TIMEBASE_HIGH) return (uint32_t)(m->timebase_ticks>>32);
        else if (offset==S5L8920_DEADLINE_COUNT && m->deadline.programmed && !m->deadline.expired)
            return m->deadline.remaining;
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
    if (address>=S5L8920_GPIO_BASE && address-S5L8920_GPIO_BASE<0x1000u) {
        offset=address-S5L8920_GPIO_BASE;
        if (size!=4u || (offset&3u)) {
            fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,true,value);
        } else if (offset/4u<S5L8920_GPIO_PIN_COUNT && !(value&~0x393u) &&
                   (value&0x270u)==0x210u && (value&0x180u)!=0x180u) {
            /* Matching iBoot polling forms only; no IRQ or alternate mode. */
            s5l8920_gpio_pin_t *pin=&m->gpio[offset/4u];
            pin->control=(uint16_t)value;
            pin->programmed=true;
        } else fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,true,value);
        return;
    }
    if (address>=S5L8920_PMGR_BASE && address-S5L8920_PMGR_BASE<0x2000u) {
        offset=address-S5L8920_PMGR_BASE;
        if (size!=4u || (offset&3u)) {
            fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,true,value);
        } else if (offset==S5L8920_DEADLINE_COUNT) {
            m->deadline.remaining=value;
            m->deadline.programmed=true;
            m->deadline.expired=false;
        } else if (offset==S5L8920_DEADLINE_CONTROL && !(value&~3u) &&
                   (!(value&1u) || m->deadline.programmed)) {
            m->deadline.enabled=(value&1u)!=0u;
            if (value&2u) m->deadline.pending=false;
            refresh_interrupts(m);
        } else fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,true,value);
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

static void nvram_header(uint8_t *p, uint8_t signature, uint16_t blocks,
                         const char *name, size_t name_size) {
    p[0]=signature;
    p[2]=(uint8_t)blocks; p[3]=(uint8_t)(blocks>>8);
    memcpy(p+4,name,name_size);
    unsigned sum=p[0];
    for (unsigned n=2u;n<16u;n++) sum+=p[n];
    while (sum>255u) sum=(sum&255u)+(sum>>8);
    p[1]=(uint8_t)sum;
}

bool s5l8920_build_empty_nvram_proxy(void *data, size_t size) {
    if (!data || size!=S5L8920_NVRAM_PROXY_SIZE) return false;
    uint8_t *p=data;
    memset(p,0,size);
    /* Original serializer: 32-byte bank header, 16-byte common header with
     * 0x7f0 variable bytes, then a free partition covering the remainder.
     * The proxy starts zeroed; an empty variable list leaves its payload zero. */
    nvram_header(p,0x5au,2u,"nvram",6u);
    p[0x14u]=1u;
    nvram_header(p+0x20u,0x70u,0x80u,"common",7u);
    nvram_header(p+0x820u,0x7fu,0x17eu,"wwwwwwwwwwww",12u);
    uint32_t sum=lzss_adler32(p+0x14u,size-0x14u);
    for (unsigned n=0;n<4u;n++) p[0x10u+n]=(uint8_t)(sum>>(8u*n));
    return true;
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

bool s5l8920_gpio_input(s5l8920_t *m,unsigned pin,bool high) {
    if (!m || !m->ram || pin>=S5L8920_GPIO_PIN_COUNT) return false;
    m->gpio[pin].input_high=high;
    m->gpio[pin].input_valid=true;
    return true;
}

bool s5l8920_timebase_clock(s5l8920_t *m,uint64_t ticks) {
    if (!m || !m->ram) return false;
    m->timebase_ticks+=ticks;
    if (ticks && m->deadline.enabled && m->deadline.programmed && !m->deadline.expired) {
        if (ticks>=m->deadline.remaining) {
            m->deadline.remaining=0u;
            m->deadline.expired=true;
            m->deadline.pending=true;
        } else m->deadline.remaining-=(uint32_t)ticks;
    }
    refresh_interrupts(m);
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
    memset(&m->deadline,0,sizeof m->deadline);
    for (unsigned pin=0;pin<S5L8920_GPIO_PIN_COUNT;pin++) {
        m->gpio[pin].control=0u;
        m->gpio[pin].programmed=false;
    }
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
