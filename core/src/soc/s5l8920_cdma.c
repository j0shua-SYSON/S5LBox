/* Independently implemented from the N88 bootloader's register/descriptor
 * accesses and the matching platform's public driver. See docs/ios6.md.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_cdma.h"
#include <string.h>

#define RUNNING UINT32_C(0x10000)
#define DONE UINT32_C(0x80000)
#define CIR UINT32_C(0x100000)
#define EVENTS UINT32_C(0x1c0000)

static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void store_word(uint8_t *p,uint32_t value) {
    for(unsigned i=0;i<4u;++i)p[i]=(uint8_t)(value>>(8u*i));
}
static bool busy(const s5l8920_cdma_t *c) {
    return ((c->channel[0].control|c->channel[1].control)&RUNNING)!=0u;
}
static bool nand_channel(unsigned channel) {
    return channel>=S5L8920_CDMA_NAND_FIRST &&
        channel-S5L8920_CDMA_NAND_FIRST<S5L8920_CDMA_NAND_COUNT;
}
static bool nand_write(s5l8920_cdma_nand_t *n,unsigned channel,uint32_t reg,uint32_t value,
                       s5l8920_cdma_memory_fn memory,void *owner);
static bool amc_write(s5l8920_cdma_amc_t *n,uint32_t reg,uint32_t value,
                      s5l8920_cdma_memory_fn memory,void *owner);
void s5l8920_cdma_init(s5l8920_cdma_t *c) {
    if(c && !c->servicing) { memset(c,0,sizeof *c);c->initialized=true; }
}
bool s5l8920_cdma_read(const s5l8920_cdma_t *c,bool aes,uint32_t offset,uint32_t *value) {
    /* AES readback semantics have not been established for this target. */
    if(!c || !c->initialized || c->servicing || !value || aes || (offset&3u))return false;
    unsigned bank=offset/0x1000u;
    if((bank<1u || bank>2u) && !nand_channel(bank) && bank!=S5L8920_CDMA_AMC_CHANNEL)return false;
    const s5l8920_cdma_channel_t *ch=bank==S5L8920_CDMA_AMC_CHANNEL?&c->amc.state:nand_channel(bank)?
        &c->nand[bank-S5L8920_CDMA_NAND_FIRST].state:&c->channel[bank-1u];
    if(!ch->reset)return false;
    switch(offset%0x1000u) {
    case 0: *value=ch->control;return true;
    case 0x0c: if(!ch->size_programmed)return false;*value=ch->remaining;return true;
    case 0x10: if(!ch->started)return false;*value=ch->address;return true;
    case 0x14: if(!ch->descriptor_programmed)return false;*value=ch->descriptor;return true;
    default:return false;
    }
}
static bool aes_write(s5l8920_cdma_t *c,uint32_t offset,uint32_t value) {
    if(busy(c))return false;
    if(offset==0u) {
        /* Channel1, enabled CBC, supplied software key only; sizes0/1/2. */
        if((value&~UINT32_C(0xd0000))!=UINT32_C(0x120100) ||
           ((value>>18)&3u)==3u)return false;
        c->setup=value;c->setup_programmed=true;return true;
    }
    if(offset>=0x10u && offset<0x20u) {
        unsigned i=(offset-0x10u)/4u;store_word(c->iv+4u*i,value);
        c->iv_programmed|=(uint8_t)(1u<<i);return true;
    }
    if(offset>=0x20u && offset<0x40u) {
        unsigned i=(offset-0x20u)/4u;store_word(c->key+4u*i,value);
        c->key_programmed|=(uint8_t)(1u<<i);return true;
    }
    return false;
}
static bool descriptor(s5l8920_cdma_channel_t *ch,unsigned bank,
                       s5l8920_cdma_memory_fn memory,void *owner) {
    if(!memory || !ch->size_programmed || !ch->descriptor_programmed ||
       !ch->remaining || (ch->remaining&15u) || (ch->descriptor&31u))return false;
    const uint8_t *p=memory(owner,ch->descriptor,32u);
    if(!p || word(p+4u)!=(bank==0u?0x30103u:0x103u) || word(p+12u)!=ch->remaining)return false;
    uint32_t terminal=word(p),address=word(p+8u);
    if(terminal&31u)return false;
    const uint8_t *end=memory(owner,terminal,32u);
    if(!end || word(end+4u)!=0u || !memory(owner,address,ch->remaining))return false;
    ch->address=address;ch->terminal=terminal;return true;
}
static bool pair_valid(const s5l8920_cdma_t *c,s5l8920_cdma_memory_fn memory,void *owner) {
    const s5l8920_cdma_channel_t *a=&c->channel[0],*b=&c->channel[1];
    if(a->remaining!=b->remaining)return false;
    uint8_t *src=memory(owner,a->address,a->remaining),*dst=memory(owner,b->address,b->remaining);
    if(!src || !dst)return false;
    /* Compare backing views, including different physical aliases. */
    uintptr_t x=(uintptr_t)src,y=(uintptr_t)dst;
    if(x!=y && (x>y?x-y:y-x)<a->remaining)return false;
    return true;
}
static bool channel_write(s5l8920_cdma_t *c,unsigned bank,uint32_t reg,uint32_t value,
                          s5l8920_cdma_memory_fn memory,void *owner) {
    s5l8920_cdma_channel_t *ch=&c->channel[bank];
    if(reg==0u && value==2u) {
        if(busy(c))return false; /* Live reset/abort coupling remains unknown. */
        memset(ch,0,sizeof *ch);ch->reset=true;return true;
    }
    if(!ch->reset || (ch->control&RUNNING))return false;
    if(reg==0x0cu) { ch->remaining=value;ch->size_programmed=true;return true; }
    if(reg==0x14u) { ch->descriptor=value;ch->descriptor_programmed=true;return true; }
    if(reg!=0u || (value&~(EVENTS|0x181u)))return false;
    uint32_t mode=value&0x180u;
    if(mode!=0u && mode!=(bank==0u?0x180u:0x80u))return false;
    if(value&1u) {
        if(mode!=(bank==0u?0x180u:0x80u) || (ch->control&EVENTS&~value) ||
           !descriptor(ch,bank,memory,owner))return false;
        if(bank==0u) {
            unsigned words=4u+2u*((c->setup>>18)&3u);
            unsigned mask=(1u<<words)-1u;
            if(!c->setup_programmed || c->iv_programmed!=15u ||
               (c->key_programmed&mask)!=mask || !aes_init(&c->cipher,c->key,32u*words))return false;
            memcpy(c->chain,c->iv,16u);
        }
        ch->control=(ch->control&EVENTS&~value)|mode|RUNNING;
        ch->started=true;
        if((c->channel[1u-bank].control&RUNNING) && !pair_valid(c,memory,owner))return false;
    } else ch->control=(ch->control&EVENTS&~value)|mode;
    return true;
}
bool s5l8920_cdma_write(s5l8920_cdma_t *c,bool aes,uint32_t offset,uint32_t value,
                      s5l8920_cdma_memory_fn memory,void *owner) {
    if(!c || !c->initialized || c->servicing || (offset&3u))return false;
    if(aes)return aes_write(c,offset,value);
    unsigned channel=offset/0x1000u;
    if(channel==S5L8920_CDMA_AMC_CHANNEL) {
        s5l8920_cdma_amc_t next=c->amc;
        c->servicing=true;
        bool okay=amc_write(&next,offset%0x1000u,value,memory,owner);
        c->servicing=false;
        if(okay)c->amc=next;
        return okay;
    }
    if(nand_channel(channel)) {
        s5l8920_cdma_nand_t next=c->nand[channel-S5L8920_CDMA_NAND_FIRST];
        c->servicing=true;
        bool okay=nand_write(&next,channel,offset%0x1000u,value,memory,owner);
        c->servicing=false;
        if(okay)c->nand[channel-S5L8920_CDMA_NAND_FIRST]=next;
        return okay;
    }
    if(channel<1u || channel>2u)return false;
    /* A malformed GO must not publish a partial descriptor/context change. */
    s5l8920_cdma_t next=*c;
    if(!channel_write(&next,channel-1u,offset%0x1000u,value,memory,owner))return false;
    *c=next;return true;
}
bool s5l8920_cdma_service(s5l8920_cdma_t *c,s5l8920_cdma_memory_fn memory,void *owner) {
    if(!c || !c->initialized || c->servicing || !memory)return false;
    s5l8920_cdma_channel_t *a=&c->channel[0],*b=&c->channel[1];
    if(!(a->control&RUNNING) || !(b->control&RUNNING))return true;
    if(!pair_valid(c,memory,owner) || a->remaining<16u || (a->remaining&15u))return false;
    uint8_t *src=memory(owner,a->address,16u),*dst=memory(owner,b->address,16u);
    if(!src || !dst)return false;
    uint8_t input[16],output[16];memcpy(input,src,16u);
    if(c->setup&0x10000u) {
        for(unsigned i=0;i<16u;++i)input[i]^=c->chain[i];
        aes_encrypt_block(&c->cipher,input,output);memcpy(c->chain,output,16u);
    } else {
        aes_decrypt_block(&c->cipher,input,output);
        for(unsigned i=0;i<16u;++i)output[i]^=c->chain[i];
        memcpy(c->chain,input,16u);
    }
    memcpy(dst,output,16u);
    a->address+=16u;b->address+=16u;a->remaining-=16u;b->remaining-=16u;
    if(!a->remaining) {
        a->control=(a->control&~RUNNING)|DONE;b->control=(b->control&~RUNNING)|DONE;
        a->descriptor=a->terminal;b->descriptor=b->terminal;
        memset(&c->cipher,0,sizeof c->cipher);memset(c->chain,0,sizeof c->chain);
    }
    return true;
}

static bool overlap(const uint8_t *a,uint32_t an,const uint8_t *b,uint32_t bn) {
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y?y-x<an:x-y<bn;
}
static bool nand_chain(s5l8920_cdma_nand_t *n,s5l8920_cdma_memory_fn memory,void *owner) {
    s5l8920_cdma_channel_t *ch=&n->state;
    if(!memory || !ch->remaining || !ch->size_programmed || !ch->descriptor_programmed ||
       !n->configuration_programmed || !n->peripheral_programmed)return false;
    uint32_t remaining=ch->remaining,at=ch->descriptor;
    const uint8_t *records[S5L8920_CDMA_MAX_SEGMENTS+1u],*outputs[S5L8920_CDMA_MAX_SEGMENTS];
    n->count=0u;
    while(n->count<S5L8920_CDMA_MAX_SEGMENTS) {
        if(at&31u)return false;
        const uint8_t *p=memory(owner,at,32u);if(!p)return false;
        uint32_t flags=word(p+4u),address=word(p+8u),length=word(p+12u);
        if((flags!=3u && flags!=0x103u) || !length || length>remaining ||
           address>UINT32_MAX-length)return false;
        const uint8_t *dest=memory(owner,address,length);if(!dest)return false;
        unsigned i=n->count++;records[i]=p;outputs[i]=dest;
        n->segments[i]=(s5l8920_cdma_segment_t){at,word(p),flags,address,length};
        remaining-=length;at=word(p);
        if(flags==0x103u) {
            if(remaining || (at&31u))return false;
            const uint8_t *end=memory(owner,at,32u);if(!end || word(end+4u))return false;
            records[n->count]=end;ch->terminal=at;
            for(unsigned a=0;a<n->count;a++)for(unsigned b=0;b<=n->count;b++)
                if(overlap(outputs[a],n->segments[a].length,records[b],32u))return false;
            n->index=0u;n->offset=0u;ch->address=n->segments[0].address;
            return true;
        }
        if(!remaining)return false;
    }
    return false;
}
static bool nand_write(s5l8920_cdma_nand_t *n,unsigned channel,uint32_t reg,uint32_t value,
                       s5l8920_cdma_memory_fn memory,void *owner) {
    s5l8920_cdma_channel_t *ch=&n->state;
    if(ch->control&RUNNING)return false;
    if(!reg && value==2u) { memset(n,0,sizeof *n);n->state.reset=true;return true; }
    if(!ch->reset)return false;
    if(reg==4u) {
        if(value!=((channel&1u)?0x58u:0u))return false;
        n->configuration=value;n->configuration_programmed=true;return true;
    }
    if(reg==8u) {
        uint32_t expected=UINT32_C(0x81200000)+((channel-5u)/2u)*0x100000u+
            ((channel&1u)?0x14u:0x18u);
        if(value!=expected)return false;
        n->peripheral=value;n->peripheral_programmed=true;return true;
    }
    if(reg==0xcu) { ch->remaining=value;ch->size_programmed=true;return true; }
    if(reg==0x14u) { ch->descriptor=value;ch->descriptor_programmed=true;return true; }
    if(reg || (value&~(EVENTS|9u)))return false;
    if(value&1u) {
        if(!(value&8u) || (ch->control&EVENTS&~value) || !nand_chain(n,memory,owner))return false;
        ch->control=RUNNING|8u;ch->started=true;
    }else ch->control=(ch->control&EVENTS&~value)|(value&8u);
    return true;
}
bool s5l8920_cdma_nand_irq(const s5l8920_cdma_t *c,unsigned channel) {
    if(!c || !c->initialized || !nand_channel(channel))return false;
    return (c->nand[channel-S5L8920_CDMA_NAND_FIRST].state.control&(DONE|8u))==(DONE|8u);
}
bool s5l8920_cdma_nand_service(s5l8920_cdma_t *c,unsigned channel,
                              s5l8920_cdma_memory_fn memory,void *memory_owner,
                              s5l8920_cdma_receive_fn receive,void *receive_owner) {
    if(!c || !c->initialized || c->servicing || !nand_channel(channel))return false;
    s5l8920_cdma_nand_t *n=&c->nand[channel-S5L8920_CDMA_NAND_FIRST];
    s5l8920_cdma_channel_t *ch=&n->state;
    if(!(ch->control&RUNNING))return true;
    if(!memory || !receive || n->index>=n->count)return false;
    const s5l8920_cdma_segment_t *s=&n->segments[n->index];
    if(n->offset>=s->length || ch->remaining<s->length-n->offset)return false;
    c->servicing=true;
    const uint8_t *p=memory(memory_owner,s->descriptor,32u);
    const uint8_t *end=memory(memory_owner,ch->terminal,32u);
    unsigned count=s->length-n->offset;if(count>16u)count=16u;
    uint8_t *dest=memory(memory_owner,s->address+n->offset,count);
    bool valid=p && end && dest && word(p)==s->next && word(p+4u)==s->flags &&
        word(p+8u)==s->address && word(p+12u)==s->length && !word(end+4u);
    if(!valid) { c->servicing=false;return false; }
    uint8_t bytes[16];
    s5l8920_cdma_input_t result=receive(receive_owner,n->peripheral,bytes,count);
    c->servicing=false;
    if(result==S5L8920_CDMA_WAIT)return true;
    if(result!=S5L8920_CDMA_READY)return false;
    memcpy(dest,bytes,count);n->offset+=count;ch->remaining-=count;ch->address+=count;
    if(n->offset==s->length) {
        ch->descriptor=s->next;n->index++;n->offset=0u;
        if(n->index==n->count)ch->control=(ch->control&~RUNNING)|DONE;
        else ch->address=n->segments[n->index].address;
    }
    return true;
}

static s5l8920_cdma_segment_t segment_at(uint32_t address,const uint8_t *p) {
    return (s5l8920_cdma_segment_t){address,word(p),word(p+4u),word(p+8u),word(p+12u)};
}
static bool segment_matches(const s5l8920_cdma_segment_t *s,const uint8_t *p) {
    return p && word(p)==s->next && word(p+4u)==s->flags &&
        word(p+8u)==s->address && word(p+12u)==s->length;
}
static bool amc_commands(s5l8920_cdma_amc_t *n,s5l8920_cdma_memory_fn memory,void *owner,bool append) {
    s5l8920_cdma_channel_t *ch=&n->state;
    if(!memory || !ch->size_programmed || !ch->descriptor_programmed ||
       !n->configuration_programmed || !n->peripheral_programmed)return false;
    if(n->index>n->count)return false;
    unsigned old_index=n->index,pending=append?n->count-old_index:0u;
    uint32_t offset=append && pending?n->offset:0u;
    s5l8920_cdma_segment_t tail=n->boundary;
    const uint8_t *headers[S5L8920_CDMA_AMC_MAX_SEGMENTS+1u];
    const uint8_t *outputs[S5L8920_CDMA_AMC_MAX_SEGMENTS];
    uint32_t at=ch->descriptor,remaining=ch->remaining;
    n->count=0u;
    for(;;) {
        if(at&31u)return false;
        const uint8_t *p=memory(owner,at,32u);if(!p)return false;
        for(unsigned i=0;i<n->count;i++)if(overlap(p,32u,headers[i],32u))return false;
        headers[n->count]=p;
        s5l8920_cdma_segment_t s=segment_at(at,p);
        /* Pending entries are immutable. Only the old zero tail may become
         * newly published data; its already-linked next pointer is retained. */
        if(append && n->count<pending) {
            const s5l8920_cdma_segment_t *old=&n->segments[old_index+n->count];
            if(at!=old->descriptor || !segment_matches(old,p))return false;
        } else if(append && n->count==pending &&
                  (at!=tail.descriptor || s.next!=tail.next))return false;
        if(s.flags==0u) {
            if(!n->count || (append && n->count<=pending) ||
               n->segments[n->count-1u].flags!=0x303u)return false;
            n->boundary=s;ch->terminal=at;
            for(unsigned i=0;i<n->count;i++)for(unsigned j=0;j<=n->count;j++)
                if(overlap(outputs[i],n->segments[i].length,headers[j],32u))return false;
            n->index=0u;n->offset=offset;
            if(!append)ch->address=n->segments[0].address;
            return true;
        }
        uint32_t consumed=n->count?0u:offset;
        if((s.flags!=3u && s.flags!=0x303u) || n->count==S5L8920_CDMA_AMC_MAX_SEGMENTS ||
           s.length<=consumed || s.length-consumed>=remaining ||
           s.address>UINT32_MAX-s.length)return false;
        const uint8_t *dest=memory(owner,s.address,s.length);if(!dest)return false;
        outputs[n->count]=dest;n->segments[n->count++]=s;
        remaining-=s.length-consumed;at=s.next;
    }
}
static bool amc_write(s5l8920_cdma_amc_t *n,uint32_t reg,uint32_t value,
                      s5l8920_cdma_memory_fn memory,void *owner) {
    s5l8920_cdma_channel_t *ch=&n->state;
    if(!reg && value==2u && !(ch->control&RUNNING)) {
        memset(n,0,sizeof *n);n->state.reset=true;return true;
    }
    if(!ch->reset)return false;
    if(!reg && !(value&~(RUNNING|DONE|CIR|0x18u))) {
        /* IRQ consumer echoes read-only state while acknowledging both event
         * bits. DONE can be cleared but is never generated by this subset. */
        ch->control=(ch->control&(RUNNING|CIR)&~(value&CIR))|(value&0x18u);
        return true;
    }
    if(!reg && value==0x19u) {
        if(!amc_commands(n,memory,owner,(ch->control&RUNNING)!=0u))return false;
        /* GO publishes descriptors; it neither consumes bytes nor acknowledges
         * a command event. Preserve pending CIR until an explicit W1C. */
        ch->control=(ch->control&CIR)|RUNNING|0x18u;ch->started=true;return true;
    }
    if(ch->control&RUNNING)return false;
    if(reg==4u) {
        if(value!=0x300d8u)return false;
        n->configuration=value;n->configuration_programmed=true;return true;
    }
    if(reg==8u) {
        if(value!=0x8480002cu)return false;
        n->peripheral=value;n->peripheral_programmed=true;return true;
    }
    if(reg==0xcu) { ch->remaining=value;ch->size_programmed=true;return true; }
    if(reg==0x14u) { ch->descriptor=value;ch->descriptor_programmed=true;return true; }
    return false;
}
bool s5l8920_cdma_amc_service(s5l8920_cdma_t *c,bool clock_open,unsigned budget,
                             s5l8920_cdma_memory_fn memory,void *memory_owner,
                             s5l8920_cdma_receive_fn receive,void *receive_owner,
                             unsigned *bytes_moved) {
    if(!bytes_moved)return false;
    *bytes_moved=0u;
    if(!c || !c->initialized || c->servicing)return false;
    s5l8920_cdma_amc_t *n=&c->amc;
    s5l8920_cdma_channel_t *ch=&n->state;
    if(!clock_open || !budget || !(ch->control&RUNNING))return true;
    if(!memory || !receive || n->index>=n->count)return false;
    const s5l8920_cdma_segment_t *s=&n->segments[n->index];
    if(n->offset>=s->length || ch->remaining<=s->length-n->offset)return false;
    c->servicing=true;
    const uint8_t *headers[S5L8920_CDMA_AMC_MAX_SEGMENTS+1u];
    bool valid=true;
    /* Check all still-pending descriptors before consuming the producer. */
    for(unsigned i=n->index;i<n->count;i++) {
        headers[i]=memory(memory_owner,n->segments[i].descriptor,32u);
        if(!segment_matches(&n->segments[i],headers[i]))valid=false;
    }
    headers[n->count]=memory(memory_owner,n->boundary.descriptor,32u);
    if(!segment_matches(&n->boundary,headers[n->count]))valid=false;
    unsigned count=s->length-n->offset;
    if(count>16u)count=16u;
    if(count>budget)count=budget;
    uint8_t *dest=memory(memory_owner,s->address+n->offset,count);
    if(!dest)valid=false;
    if(valid)for(unsigned i=n->index;i<=n->count;i++)
        if(overlap(dest,count,headers[i],32u))valid=false;
    if(!valid) { c->servicing=false;return false; }
    uint8_t bytes[16];
    s5l8920_cdma_input_t result=receive(receive_owner,n->peripheral,bytes,count);
    if(result==S5L8920_CDMA_READY) {
        memcpy(dest,bytes,count);n->offset+=count;ch->remaining-=count;
        ch->address=s->address+n->offset;*bytes_moved=count;
        if(n->offset==s->length) {
            ch->descriptor=s->next;n->index++;n->offset=0u;
            if(s->flags==0x303u)ch->control|=CIR;
        }
    }
    c->servicing=false;
    return result==S5L8920_CDMA_READY || result==S5L8920_CDMA_WAIT;
}
bool s5l8920_cdma_amc_irq(const s5l8920_cdma_t *c) {
    return c && c->initialized && (c->amc.state.control&(CIR|0x10u))==(CIR|0x10u);
}
