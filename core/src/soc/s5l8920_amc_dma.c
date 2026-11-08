/* Bounded AMC internal-memory DMA.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_amc_dma.h"
#include <string.h>

static bool overlap(uint32_t a,uint32_t an,uint32_t b,uint32_t bn) {
    return (uint64_t)a+an>b && (uint64_t)b+bn>a;
}
static bool same(const s5l8920_amc_dma_input_t *a,const s5l8920_amc_dma_input_t *b) {
    if (a->memory!=b->memory || a->memory_size!=b->memory_size ||
        a->region_count!=b->region_count || a->idle_status!=b->idle_status ||
        a->active_status!=b->active_status || a->stopped_status!=b->stopped_status) return false;
    for (unsigned i=0;i<3u;++i)
        if (a->region[i].offset!=b->region[i].offset || a->region[i].size!=b->region[i].size) return false;
    return true;
}
bool s5l8920_amc_dma_configure(s5l8920_amc_dma_t *d,const s5l8920_amc_dma_input_t *input) {
    if (!d || !input || !input->memory || !input->memory_size ||
        !input->region_count || input->region_count>3u || (input->idle_status&7u) ||
        !(input->active_status&7u) || (input->active_status&7u)==7u ||
        (input->stopped_status&7u)!=7u) return false;
    for (unsigned i=0;i<3u;++i) {
        const s5l8920_amc_dma_region_t *r=&input->region[i];
        if (i>=input->region_count) { if (r->offset || r->size) return false; continue; }
        if (!r->size || ((r->offset|r->size)&3u) ||
            (uint64_t)r->offset+r->size>input->memory_size) return false;
        for (unsigned j=0;j<i;++j)
            if (overlap(r->offset,r->size,input->region[j].offset,input->region[j].size)) return false;
    }
    if (d->configured) return same(&d->input,input);
    s5l8920_amc_dma_t next; memset(&next,0,sizeof next);
    next.input=*input; next.configured=true; *d=next; return true;
}
void s5l8920_amc_dma_reset(s5l8920_amc_dma_t *d) {
    if (!d) return;
    if (!d->configured) { memset(d,0,sizeof *d); return; }
    d->source=d->destination=d->length=d->copied=0;
    d->active=d->stopped=d->completed=false;
}
static bool backed(const s5l8920_amc_dma_input_t *in,uint32_t a,uint32_t n) {
    for (unsigned i=0;i<in->region_count;++i) {
        const s5l8920_amc_dma_region_t *r=&in->region[i];
        if (a>=r->offset && (uint64_t)a+n<=(uint64_t)r->offset+r->size) return true;
    }
    return false;
}
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
bool s5l8920_amc_dma_read(const s5l8920_amc_dma_t *d,uint32_t offset,unsigned size,uint32_t *value) {
    if (!d || !d->configured || !value || offset!=0x14u || size!=4u) return false;
    *value=d->active?d->input.active_status:d->stopped?d->input.stopped_status:d->input.idle_status;
    return true;
}
bool s5l8920_amc_dma_write(s5l8920_amc_dma_t *d,uint32_t offset,unsigned size,uint32_t value) {
    if (!d || !d->configured || size!=4u || d->active) return false;
    if (offset==0x10u) {
        if (value==4u) { d->stopped=true; return true; }
        if (value!=0x10u || !d->stopped) return false;
        s5l8920_amc_dma_reset(d); return true;
    }
    if (offset || d->stopped || (value&3u)!=2u) return false;
    const uint32_t descriptor=value&~3u;
    if (!backed(&d->input,descriptor,16u)) return false;
    const uint8_t *p=d->input.memory+descriptor;
    const uint32_t control=word(p+4),source=word(p+8),destination=word(p+12);
    const uint32_t count=control>>18,form=control&0x3ffffu,length=count*4u;
    if (word(p) || !count || (form!=0x25u && form!=0xfa5u) || ((source|destination)&3u) ||
        !backed(&d->input,source,length) || !backed(&d->input,destination,length) ||
        overlap(source,length,destination,length) || overlap(source,length,descriptor,16u) ||
        overlap(destination,length,descriptor,16u)) return false;
    d->source=source; d->destination=destination; d->length=length; d->copied=0;
    d->completed=false; d->active=true; return true;
}
unsigned s5l8920_amc_dma_service(s5l8920_amc_dma_t *d,bool clock_available,unsigned budget) {
    if (!d || !d->configured || !d->active || !clock_available) return 0;
    unsigned n=budget>16u?16u:budget&~3u;
    if (n>d->length-d->copied) n=d->length-d->copied;
    if (!n) return 0;
    memcpy(d->input.memory+d->destination+d->copied,d->input.memory+d->source+d->copied,n);
    d->copied+=n;
    if (d->copied==d->length) { d->active=false; d->completed=true; }
    return n;
}
