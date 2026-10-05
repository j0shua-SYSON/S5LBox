/* AK8973 configuration; AKM MS0561-E-01, sections5..9.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "ak8973.h"
#include <string.h>

static bool valid(const ak8973_t *d) {
    if(!d || !d->initialized || d->address<0x1cu || d->address>0x1fu ||
       (d->eeprom_known&0x80u) || (d->ms1&4u) ||
       d->powerdown_remaining>AK8973_POWERDOWN_NS ||
       d->eeprom_remaining>AK8973_EEPROM_START_NS)return false;
    unsigned mode=d->ms1&3u;
    if((mode!=2u && mode!=3u) || (mode!=3u && d->powerdown_remaining) ||
       (mode!=2u && d->eeprom_remaining))return false;
    for(unsigned n=0;n<3u;++n)if(d->gain[n]>15u)return false;
    return true;
}
static bool addressed(const ak8973_t *d,uint8_t address,const void *data,size_t size) {
    return valid(d) && address==d->address && data && size && size<=AK8973_MAX_TRANSFER;
}
static bool write_enabled(const ak8973_t *d) {return (d->ms1>>3)==0x15u;}
static unsigned next_register(unsigned reg) {
    if(reg==0xc4u)return 0xc0u;
    if(reg==0xe6u)return 0xe0u;
    if(reg==0x68u)return 0x62u;
    return reg+1u;
}

bool ak8973_init(ak8973_t *d,bool cad1,bool cad0,const uint8_t *eeprom,uint8_t known) {
    if(!d || (known&0x80u) || (known && !eeprom))return false;
    ak8973_t initial={0};
    initial.initialized=true;initial.address=(uint8_t)(0x1cu|(cad1?2u:0u)|(cad0?1u:0u));
    initial.ms1=3u;initial.powerdown_remaining=AK8973_POWERDOWN_NS;
    initial.eeprom_known=known;
    for(unsigned n=0;n<AK8973_EEPROM_SIZE;++n)
        if(known&(1u<<n))initial.eeprom[n]=eeprom[n];
    *d=initial;return true;
}
bool ak8973_reset(ak8973_t *d) {
    if(!valid(d))return false;
    return ak8973_init(d,(d->address&2u)!=0u,(d->address&1u)!=0u,d->eeprom,d->eeprom_known);
}
bool ak8973_advance(ak8973_t *d,uint64_t ns) {
    if(!valid(d))return false;
    d->powerdown_remaining=ns>=d->powerdown_remaining?0u:d->powerdown_remaining-ns;
    d->eeprom_remaining=ns>=d->eeprom_remaining?0u:d->eeprom_remaining-ns;
    return true;
}
bool ak8973_read(const ak8973_t *d,uint8_t a,uint8_t r,uint8_t *p,size_t n) {
    if(!addressed(d,a,p,n))return false;
    uint8_t result[AK8973_MAX_TRANSFER];unsigned reg=r;
    for(size_t i=0;i<n;++i) {
        if(reg==0xc0u)result[i]=write_enabled(d)?2u:0u;
        /* These remain reset output registers; measurement requests refuse. */
        else if(reg>=0xc1u && reg<=0xc4u)result[i]=0u;
        else if(reg==0xe0u)result[i]=d->ms1;
        else if(reg>=0xe1u && reg<=0xe6u) {
            if((d->ms1&3u)!=3u)return false;
            result[i]=reg<=0xe3u?d->dac[reg-0xe1u]:d->gain[reg-0xe4u];
        } else if(reg>=0x62u && reg<=0x68u) {
            unsigned index=reg-0x62u;
            if((d->ms1&3u)!=2u || write_enabled(d) || d->eeprom_remaining ||
               !(d->eeprom_known&(1u<<index)))return false;
            result[i]=d->eeprom[index];
        } else return false;
        reg=next_register(reg);
    }
    memcpy(p,result,n);return true;
}
bool ak8973_write(ak8973_t *d,uint8_t a,uint8_t r,const uint8_t *p,size_t n) {
    if(!addressed(d,a,p,n))return false;
    ak8973_t next=*d;unsigned reg=r;
    for(size_t i=0;i<n;++i) {
        if(reg==0xe0u) {
            unsigned mode=p[i]&3u,old_mode=next.ms1&3u;
            if((p[i]&4u) || (mode!=2u && mode!=3u))return false;
            if(mode==2u && old_mode==3u) {
                if(next.powerdown_remaining)return false;
                next.eeprom_remaining=AK8973_EEPROM_START_NS;
            } else if(mode==3u) {
                next.powerdown_remaining=AK8973_POWERDOWN_NS;next.eeprom_remaining=0;
            }
            next.ms1=p[i];
        } else if(reg>=0xe1u && reg<=0xe6u && (next.ms1&3u)==3u) {
            if(reg<=0xe3u)next.dac[reg-0xe1u]=p[i];
            /* MS0561-E-01 section5.3.3 note8 explicitly discards upper bits. */
            else next.gain[reg-0xe4u]=p[i]&0x0fu;
        } else return false;
        reg=next_register(reg);
    }
    *d=next;return true;
}
