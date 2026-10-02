/* N88 RAM, PL192, UART, timebase, GPIO and I2C, separate from S5L8900.
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
    (void)pl192_set_line(&m->vic[S5L8920_GPIO_IRQ/32u],S5L8920_GPIO_IRQ%32u,
                        (m->input_levels[S5L8920_GPIO_IRQ/32u]&(1u<<(S5L8920_GPIO_IRQ%32u)))!=0u ||
                        m->gpio_irq);
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

static void refresh_i2c_interrupt(s5l8920_t *m,unsigned bus) {
    unsigned source=S5L8920_I2C0_IRQ-bus;
    (void)pl192_set_line(&m->vic[0],source,
        (m->input_levels[0]&(1u<<source))!=0u ||
        (m->i2c[bus].control!=0u && m->i2c[bus].status!=0u));
    refresh_interrupts(m);
}

static bool gpio_control_supported(unsigned index,uint32_t control) {
    /* Preserve drive and peripheral fields without synthesizing pad signals.
     * The matching restore initializer also writes input-disabled/off pins;
     * accepting that configuration does not establish their data readback. */
    if ((control&~0xfffu) || (control&0x180u)==0x180u) return false;
    unsigned mode=control&0xeu;
    if (mode==0xeu) return (control&0x70u)==0x10u;
    if (!(control&0x200u)) return false;
    if (control&0x60u) return mode==0u && (control&0x10u)!=0u;
    if (mode<=2u) return (control&0x10u)!=0u;
    return index<S5L8920_GPIO_IRQ_PINS && mode<=0xcu;
}

static void gpio_latch_level(s5l8920_t *m,unsigned index) {
    if (index>=S5L8920_GPIO_IRQ_PINS) return;
    const s5l8920_gpio_pin_t *pin=&m->gpio[index];
    unsigned mode=pin->control&0xeu;
    if (pin->programmed && pin->input_valid &&
        ((mode==4u && pin->input_high) || (mode==6u && !pin->input_high)))
        m->gpio_pending[index/32u]|=1u<<(index%32u);
}

static void gpio_refresh_irq(s5l8920_t *m) {
    m->gpio_irq=false;
    for (unsigned index=0;index<S5L8920_GPIO_IRQ_PINS;index++) {
        const s5l8920_gpio_pin_t *pin=&m->gpio[index];
        unsigned mode=pin->control&0xeu;
        if ((m->gpio_pending[index/32u]&(1u<<(index%32u))) && pin->programmed &&
            !(pin->control&0x10u) && mode>=4u && mode<=0xcu) {
            m->gpio_irq=true;
            break;
        }
    }
    refresh_interrupts(m);
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

static bool decode_i2c(uint32_t address,unsigned *bus,uint32_t *offset) {
    if (address<S5L8920_I2C_BASE) return false;
    uint32_t relative=address-S5L8920_I2C_BASE;
    *bus=relative/S5L8920_I2C_STRIDE; *offset=relative%S5L8920_I2C_STRIDE;
    return *bus<S5L8920_I2C_COUNT && *offset<0x1000u;
}

static bool i2c_read(s5l8920_i2c_t *i,uint32_t offset,uint32_t *value) {
    if (offset==0xcu) { *value=i->status; return true; }
    if (offset==0x20u && i->rx_cursor<i->rx_count) {
        *value=i->rx[i->rx_cursor++]; return true;
    }
    return false;
}

static bool i2c_write(s5l8920_i2c_t *i,uint32_t offset,uint32_t value) {
    if (value>255u) return false;
    if (offset==0xcu) {
        if (value&~0x37u) return false;
        i->status&=(uint8_t)~value;
        return true;
    }
    if (i->active) return false;
    if (offset==8u) {
        if (value!=0u && value!=0x30u && value!=0xf0u) return false;
        i->control=(uint8_t)value; i->programmed|=16u;
        return true;
    }
    if (i->status || i->rx_cursor<i->rx_count) return false;
    switch (offset) {
    case 0u:
        if (value>0x7fu) return false;
        i->address=(uint8_t)value; i->programmed|=1u; return true;
    case 0x10u:
        i->subaddress=(uint8_t)value; i->programmed|=2u; return true;
    case 0x14u:
        if (value) return false;
        i->programmed|=4u; return true;
    case 0x18u:
        if (value>S5L8920_I2C_CAPACITY || value<i->tx_count) return false;
        i->length=(uint8_t)value; i->programmed|=8u; return true;
    case 0x20u:
        if (!(i->programmed&8u) || i->tx_count>=i->length) return false;
        i->tx[i->tx_count++]=(uint8_t)value; return true;
    case 0x24u:
        if ((value!=4u && value!=5u) || i->programmed!=31u || !i->control ||
            i->sequence==UINT64_MAX ||
            (value==4u ? (!i->length || i->tx_count!=0u) : i->tx_count!=i->length)) return false;
        i->write=value==5u; i->active=true; i->sequence++;
        i->rx_count=0u; i->rx_cursor=0u;
        return true;
    default: return false;
    }
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
    if (decode_i2c(address,&bank,&offset)) {
        if ((size!=1u && size!=4u) || (offset&3u))
            fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,false,0u);
        else if (!i2c_read(&m->i2c[bank],offset,&value))
            fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,false,0u);
        else return value;
        return 0u;
    }
    if (address>=S5L8920_GPIO_BASE && address-S5L8920_GPIO_BASE<0x1000u) {
        offset=address-S5L8920_GPIO_BASE;
        if (size!=4u || (offset&3u)) {
            fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,false,0u);
        } else if (offset/4u<S5L8920_GPIO_PIN_COUNT) {
            const s5l8920_gpio_pin_t *pin=&m->gpio[offset/4u];
            bool output=(pin->control&0xeu)==2u;
            if (pin->programmed && (pin->control&0x200u) && (output || pin->input_valid))
                return output ? pin->control :
                       (pin->control&~1u)|(pin->input_high ? 1u:0u);
            fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,false,0u);
        } else if (offset>=S5L8920_GPIO_IRQ_STATUS &&
                   offset-S5L8920_GPIO_IRQ_STATUS<4u*S5L8920_GPIO_IRQ_GROUPS)
            return m->gpio_pending[(offset-S5L8920_GPIO_IRQ_STATUS)/4u];
        else fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,false,0u);
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
    if (decode_i2c(address,&bank,&offset)) {
        if ((size!=1u && size!=4u) || (offset&3u))
            fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,true,value);
        else if (!i2c_write(&m->i2c[bank],offset,value))
            fail(m,S5L8920_BUS_REGISTER_REFUSED,address,size,true,value);
        else refresh_i2c_interrupt(m,bank);
        return;
    }
    if (address>=S5L8920_GPIO_BASE && address-S5L8920_GPIO_BASE<0x1000u) {
        offset=address-S5L8920_GPIO_BASE;
        if (size!=4u || (offset&3u)) {
            fail(m,S5L8920_BUS_ACCESS_UNIMPLEMENTED,address,size,true,value);
        } else if (offset/4u<S5L8920_GPIO_PIN_COUNT && gpio_control_supported(offset/4u,value)) {
            s5l8920_gpio_pin_t *pin=&m->gpio[offset/4u];
            pin->control=(uint16_t)value;
            pin->programmed=true;
            gpio_latch_level(m,offset/4u);
            gpio_refresh_irq(m);
        } else if (offset>=S5L8920_GPIO_IRQ_STATUS &&
                   offset-S5L8920_GPIO_IRQ_STATUS<4u*S5L8920_GPIO_IRQ_GROUPS) {
            unsigned group=(offset-S5L8920_GPIO_IRQ_STATUS)/4u;
            m->gpio_pending[group]&=~value;
            for (unsigned index=32u*group;index<32u*(group+1u);index++) gpio_latch_level(m,index);
            gpio_refresh_irq(m);
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
    unsigned mode=m->gpio[pin].control&0xeu;
    if (pin<S5L8920_GPIO_IRQ_PINS && m->gpio[pin].programmed &&
        m->gpio[pin].input_valid && high!=m->gpio[pin].input_high &&
        ((mode==8u && high) || (mode==0xau && !high) || mode==0xcu))
        m->gpio_pending[pin/32u]|=1u<<(pin%32u);
    m->gpio[pin].input_high=high;
    m->gpio[pin].input_valid=true;
    gpio_latch_level(m,pin);
    gpio_refresh_irq(m);
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
    if (source<=S5L8920_I2C0_IRQ && S5L8920_I2C0_IRQ-source<S5L8920_I2C_COUNT)
        refresh_i2c_interrupt(m,S5L8920_I2C0_IRQ-source);
    else refresh_interrupts(m);
    return true;
}

bool s5l8920_i2c_request(const s5l8920_t *m,unsigned bus,s5l8920_i2c_request_t *request) {
    if (!m || !m->ram || bus>=S5L8920_I2C_COUNT || !request || !m->i2c[bus].active) return false;
    const s5l8920_i2c_t *i=&m->i2c[bus];
    s5l8920_i2c_request_t result={0};
    result.sequence=i->sequence; result.address=i->address;
    result.subaddress=i->subaddress; result.length=i->length; result.write=i->write;
    if (i->write) memcpy(result.data,i->tx,i->length);
    *request=result;
    return true;
}

bool s5l8920_i2c_complete(s5l8920_t *m,unsigned bus,uint64_t sequence,
                         bool success,const uint8_t *data,size_t size) {
    if (!m || !m->ram || bus>=S5L8920_I2C_COUNT) return false;
    s5l8920_i2c_t *i=&m->i2c[bus];
    if (!i->active || sequence!=i->sequence) return false;
    if (success && !i->write) {
        if (!data || size!=i->length) return false;
    } else if (data || size) return false;
    if (size) memmove(i->rx,data,size);
    i->rx_count=(unsigned)size; i->rx_cursor=0u; i->tx_count=0u;
    i->active=false; i->status=success ? 0x10u:0x20u;
    refresh_i2c_interrupt(m,bus);
    return true;
}

bool s5l8920_pmu_rtc_configure(s5l8920_t *m,uint32_t counter,uint32_t offset) {
    if (!m || !m->ram || m->i2c[0].active || m->i2c[0].status ||
        m->i2c[0].rx_cursor<m->i2c[0].rx_count) return false;
    m->pmu_rtc_counter=counter; m->pmu_rtc_offset=offset;
    m->pmu_rtc_configured=true;
    return true;
}

bool s5l8920_pmu_rtc_advance(s5l8920_t *m,uint32_t units) {
    if (!m || !m->ram || !m->pmu_rtc_configured) return false;
    m->pmu_rtc_counter+=units;
    return true;
}

bool s5l8920_pmu_rtc_service(s5l8920_t *m,uint64_t sequence) {
    if (!m || !m->ram || !m->pmu_rtc_configured) return false;
    const s5l8920_i2c_t *i=&m->i2c[0];
    if (!i->active || i->sequence!=sequence || i->address!=0x74u || i->length!=4u ||
        (i->subaddress!=0x64u && (i->subaddress!=0x4cu || i->write))) return false;
    uint32_t value;
    if (i->write) {
        value=(uint32_t)i->tx[0]|((uint32_t)i->tx[1]<<8)|
              ((uint32_t)i->tx[2]<<16)|((uint32_t)i->tx[3]<<24);
        if (!s5l8920_i2c_complete(m,0u,sequence,true,NULL,0u)) return false;
        m->pmu_rtc_offset=value;
    } else {
        uint8_t response[4];
        value=i->subaddress==0x4cu ? m->pmu_rtc_counter:m->pmu_rtc_offset;
        for (unsigned n=0;n<4u;n++) response[n]=(uint8_t)(value>>(n*8u));
        if (!s5l8920_i2c_complete(m,0u,sequence,true,response,sizeof response)) return false;
    }
    return true;
}

bool s5l8920_reset(s5l8920_t *m) {
    if (!m || !m->ram) return false;
    if (!arm_reset_profile(&m->cpu,&m->bus,ARM_ARCH_V7_CORTEX_A8)) return false;
    m->ram_boot_window = false;
    s5l8920_uart_reset(&m->uart0);
    m->timebase_ticks=0u;
    memset(&m->deadline,0,sizeof m->deadline);
    for (unsigned bus=0;bus<S5L8920_I2C_COUNT;bus++) {
        uint64_t sequence=m->i2c[bus].sequence;
        memset(&m->i2c[bus],0,sizeof m->i2c[bus]);
        m->i2c[bus].sequence=sequence;
    }
    memset(m->gpio_pending,0,sizeof m->gpio_pending);
    m->gpio_irq=false;
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
