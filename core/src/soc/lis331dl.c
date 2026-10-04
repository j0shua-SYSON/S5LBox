/* LIS331DL configuration; ST AN2960, sections4..8.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "lis331dl.h"
#include <string.h>

static bool valid(const lis331dl_t *d) {
    return d && d->initialized && d->reboot_ns &&
        d->reboot_remaining<=d->reboot_ns && !(d->control[1]&0x20u) &&
        ((d->control[1]&0x40u)!=0u)==(d->reboot_remaining!=0u);
}

static uint8_t *configuration(lis331dl_t *d,unsigned reg) {
    if(reg>=0x20u && reg<=0x22u)return &d->control[reg-0x20u];
    switch(reg) {
        case 0x30: return &d->wake_config[0];
        case 0x32: return &d->wake_threshold[0];
        case 0x33: return &d->wake_duration[0];
        case 0x34: return &d->wake_config[1];
        case 0x36: return &d->wake_threshold[1];
        case 0x37: return &d->wake_duration[1];
        default: return NULL;
    }
}

static bool addressed(const lis331dl_t *d,uint8_t address,const void *data,size_t size) {
    return valid(d) && address==(d->sdo_high?0x1du:0x1cu) &&
        data && size && size<=LIS331DL_MAX_TRANSFER;
}

bool lis331dl_init(lis331dl_t *d, bool sdo_high, uint64_t reboot_ns) {
    if(!d || !reboot_ns)return false;
    lis331dl_t initial={0};
    initial.initialized=true;initial.sdo_high=sdo_high;initial.reboot_ns=reboot_ns;
    initial.control[0]=7u;*d=initial;return true;
}
bool lis331dl_read(const lis331dl_t *d, uint8_t address, uint8_t subaddress,
                  uint8_t *data, size_t size) {
    if(!addressed(d,address,data,size))return false;
    lis331dl_t observed=*d;
    uint8_t result[LIS331DL_MAX_TRANSFER];
    unsigned reg=subaddress&0x7fu;
    for(size_t n=0;n<size;++n) {
        uint8_t *value=configuration(&observed,reg);
        if(reg==0x0fu)result[n]=0x3bu;
        else if(value)result[n]=*value;
        else return false;
        if(subaddress&0x80u)++reg;
    }
    memcpy(data,result,size);return true;
}
bool lis331dl_write(lis331dl_t *d, uint8_t address, uint8_t subaddress,
                   const uint8_t *data, size_t size) {
    if(!addressed(d,address,data,size))return false;
    lis331dl_t next=*d;
    unsigned reg=subaddress&0x7fu;
    for(size_t n=0;n<size;++n) {
        uint8_t *value=configuration(&next,reg);
        if(!value || next.reboot_remaining || (reg==0x21u && (data[n]&0x20u)))return false;
        *value=data[n];
        if(reg==0x21u && (data[n]&0x40u))next.reboot_remaining=next.reboot_ns;
        if(subaddress&0x80u)++reg;
    }
    *d=next;return true;
}
bool lis331dl_advance(lis331dl_t *d, uint64_t nanoseconds) {
    if(!valid(d))return false;
    if(nanoseconds>=d->reboot_remaining) {
        d->reboot_remaining=0;d->control[1]&=(uint8_t)~0x40u;
    } else d->reboot_remaining-=nanoseconds;
    return true;
}
