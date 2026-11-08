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
    memset(d->descriptor,0,sizeof d->descriptor);
    d->descriptor_count=d->descriptor_index=0; d->descriptor_copied=0;
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
    s5l8920_amc_dma_t next=*d;
    s5l8920_amc_dma_reset(&next);
    do {
        if ((value&3u)!=2u || next.descriptor_count==S5L8920_AMC_DMA_MAX_DESCRIPTORS) return false;
        const uint32_t at=value&~3u;
        if (!backed(&d->input,at,16u)) return false;
        for (unsigned i=0;i<next.descriptor_count;++i)
            if (overlap(at,16u,next.descriptor[i].offset,16u)) return false;
        const uint8_t *p=d->input.memory+at;
        const uint32_t control=word(p+4),source=word(p+8),destination=word(p+12);
        const uint32_t count=control>>18,form=control&0x3ffffu,length=count*4u;
        value=word(p);
        if (!count || (form!=5u && (value || (form!=0x25u && form!=0xfa5u))) ||
            ((source|destination)&3u) || !backed(&d->input,source,length) ||
            !backed(&d->input,destination,length) || overlap(source,length,destination,length)) return false;
        s5l8920_amc_dma_descriptor_t entry={at,source,destination,length};
        next.descriptor[next.descriptor_count++]=entry; next.length+=length;
    } while (value);
    /* Protect every admitted header, including later metadata that an earlier
     * copy could otherwise alter. This also keeps descriptor reads separate
     * from the supported payload-memory operation. */
    for (unsigned i=0;i<next.descriptor_count;++i) {
        const s5l8920_amc_dma_descriptor_t *e=&next.descriptor[i];
        for (unsigned j=0;j<next.descriptor_count;++j)
            if (overlap(e->source,e->length,next.descriptor[j].offset,16u) ||
                overlap(e->destination,e->length,next.descriptor[j].offset,16u)) return false;
    }
    next.source=next.descriptor[0].source; next.destination=next.descriptor[0].destination;
    next.active=true; *d=next; return true;
}
unsigned s5l8920_amc_dma_service(s5l8920_amc_dma_t *d,bool clock_available,unsigned budget) {
    if (!d || !d->configured || !d->active || !clock_available) return 0;
    const s5l8920_amc_dma_descriptor_t *e=&d->descriptor[d->descriptor_index];
    unsigned n=budget>16u?16u:budget&~3u;
    if (n>e->length-d->descriptor_copied) n=e->length-d->descriptor_copied;
    if (!n) return 0;
    memcpy(d->input.memory+e->destination+d->descriptor_copied,
           d->input.memory+e->source+d->descriptor_copied,n);
    d->copied+=n; d->descriptor_copied+=n;
    if (d->descriptor_copied==e->length) {
        if (d->descriptor_index+1u==d->descriptor_count) { d->active=false; d->completed=true; }
        else {
            ++d->descriptor_index; d->descriptor_copied=0;
            d->source=d->descriptor[d->descriptor_index].source;
            d->destination=d->descriptor[d->descriptor_index].destination;
        }
    }
    return n;
}
