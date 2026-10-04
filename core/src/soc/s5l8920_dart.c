/* H2P DART: bounded command programming and an uncached page walker.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_dart.h"
#include <string.h>

bool s5l8920_dart_configure(s5l8920_dart_t *d, uint32_t config,
                           uint32_t command, uint32_t table_port) {
    if (!d || (config&0x80000000u)) return false;
    if (d->configured)
        return config==d->initial_config && command==d->initial_command &&
               table_port==d->initial_table_port;
    d->initial_config=config; d->initial_command=command;
    d->initial_table_port=table_port; d->configured=true;
    s5l8920_dart_reset(d);
    return true;
}

void s5l8920_dart_reset(s5l8920_dart_t *d) {
    if (!d) return;
    d->config=d->initial_config;
    memset(d->segment,0,sizeof d->segment);
    d->programmed=0u;
    d->command_readable=d->table_port_readable=d->configured;
}

bool s5l8920_dart_read(const s5l8920_dart_t *d, uint32_t offset, uint32_t *value) {
    if (!d || !value || !d->configured) return false;
    switch (offset) {
    case 0u:
        if (!d->command_readable) return false;
        *value=d->initial_command; return true;
    case 8u:
        if (!d->table_port_readable) return false;
        *value=d->initial_table_port; return true;
    case 12u: *value=d->config; return true;
    default: return false;
    }
}

bool s5l8920_dart_write(s5l8920_dart_t *d, uint32_t offset, uint32_t value) {
    if (!d || !d->configured) return false;
    switch (offset) {
    case 0u:
        if ((value&0x7ffu)!=0x702u ||
            ((value^d->initial_command)&~0x7ffu)) return false;
        /* All translations walk current RAM; flushing has no cache work.
         * Command-port readback after submission has not been established. */
        d->command_readable=false;
        return true;
    case 8u: {
        if ((d->config&0x80000000u) || (value&0xfeu) ||
            ((value^d->initial_table_port)&0xf0000000u) ||
            (!(value&1u) && (value&0x0ffff000u))) return false;
        unsigned index=(value>>8)&15u;
        d->segment[index]=value&0x0ffff001u;
        d->programmed|=(uint16_t)(1u<<index);
        d->table_port_readable=false;
        return true;
    }
    case 12u:
        if (((value^d->config)&~0x8000007fu) ||
            ((value&0x80000000u) && (value&0x7fu)!=0x70u)) return false;
        d->config=value;
        return true;
    default: return false;
    }
}

s5l8920_dart_result_t s5l8920_dart_translate(const s5l8920_dart_t *d,
    const uint8_t *ram, size_t ram_size, uint32_t address, size_t size,
    uint32_t *physical) {
    if (!d || !ram || !physical || ram_size>0x10000000u || !size ||
        address<S5L8920_DART_DVA_BASE ||
        address-S5L8920_DART_DVA_BASE>=S5L8920_DART_DVA_SIZE ||
        size>0x1000u-(address&0xfffu)) return S5L8920_DART_BAD_REQUEST;
    if (!d->configured) return S5L8920_DART_UNCONFIGURED;
    if (!(d->config&0x80000000u)) return S5L8920_DART_DISABLED;
    uint32_t relative=address-S5L8920_DART_DVA_BASE;
    unsigned index=relative>>22;
    if (!(d->programmed&(1u<<index))) return S5L8920_DART_UNKNOWN_SEGMENT;
    uint32_t ste=d->segment[index];
    if (!(ste&1u)) return S5L8920_DART_INVALID_SEGMENT;
    uint32_t pte_at=(ste&0x0ffff000u)+((relative>>10)&0xffcu);
    if (pte_at>ram_size || 4u>ram_size-pte_at) return S5L8920_DART_TABLE_UNAVAILABLE;
    const uint8_t *p=ram+pte_at;
    uint32_t pte=(uint32_t)p[0]|((uint32_t)p[1]<<8)|
                 ((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
    if (pte&~0x0ffff001u) return S5L8920_DART_UNSUPPORTED_PAGE;
    if (!(pte&1u)) return S5L8920_DART_INVALID_PAGE;
    uint32_t target=(pte&0x0ffff000u)|(address&0xfffu);
    if (target>ram_size || size>ram_size-target) return S5L8920_DART_TARGET_UNAVAILABLE;
    *physical=0x40000000u|target;
    return S5L8920_DART_OK;
}
