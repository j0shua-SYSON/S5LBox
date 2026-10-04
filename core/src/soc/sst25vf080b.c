/* SST25VF080B command model.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "sst25vf080b.h"
#include <string.h>

/* Functional byte boundary model of DS20005045D, sections 4.3/4.4. The
 * embedding board supplies pin qualification, timing and the actual image. */
#define BUSY 1u
#define WEL 2u
#define AAI 0x40u
#define BPL 0x80u
#define WRITABLE 0xbcu

static bool valid(const sst25vf080b_t *f) {
    return f && f->initialized && ((f->image!=NULL)!=f->array_unavailable);
}

static bool duration(uint64_t n,uint64_t maximum) { return n && n<=maximum; }

static bool initialize(sst25vf080b_t *f, uint8_t *image,
    const sst25vf080b_timing_t *timing, bool wp_high, bool hold_high) {
    if (!f || !timing ||
        !duration(timing->program_ns,10000u) || !duration(timing->sector_ns,25000000u) ||
        !duration(timing->block32_ns,25000000u) || !duration(timing->block64_ns,25000000u) ||
        !duration(timing->chip_ns,50000000u)) return false;
    sst25vf080b_t next={0};
    next.image=image; next.timing=*timing; next.powerup_ns=100000u;
    next.status=0x1cu; next.wp_high=wp_high; next.hold_high=hold_high;
    next.array_unavailable=image==NULL;next.initialized=true; *f=next;
    return true;
}

bool sst25vf080b_init(sst25vf080b_t *f, uint8_t *image, size_t size,
    const sst25vf080b_timing_t *timing, bool wp_high, bool hold_high) {
    if (!image || size!=SST25VF080B_SIZE) return false;
    return initialize(f,image,timing,wp_high,hold_high);
}

bool sst25vf080b_init_unbacked(sst25vf080b_t *f,
    const sst25vf080b_timing_t *timing, bool wp_high, bool hold_high) {
    return initialize(f,NULL,timing,wp_high,hold_high);
}

/* First protected address. BP3 is reserved/don't-care in table4-3. */
static uint32_t protected_start(const sst25vf080b_t *f) {
    static const uint32_t boundaries[]={0x100000u,0xf0000u,0xe0000u,0xc0000u,0x80000u,0u,0u,0u};
    return boundaries[(f->status>>2)&7u];
}

static uint8_t status(const sst25vf080b_t *f) {
    return (uint8_t)(f->status|(f->busy_ns?BUSY:0u));
}

static bool start_program(sst25vf080b_t *f,uint32_t address,unsigned count) {
    if (!(f->status&WEL) || address+count>protected_start(f)) return true;
    if (!f->image) return false;
    /* The specified program precondition is erased storage. Preserve the
     * evidence boundary for undefined repeated programming, rather than RAM. */
    for (unsigned i=0;i<count;++i) if (f->image[address+i]!=0xffu) return false;
    f->operation=f->command; f->operation_address=address; f->operation_size=count;
    memcpy(f->operation_data,f->data,count); f->busy_ns=f->timing.program_ns;
    if (f->command==0xadu) f->status|=AAI;
    return true;
}

static bool start_erase(sst25vf080b_t *f,uint32_t size,uint64_t time) {
    uint32_t address=f->address&~(size-1u);
    if (!(f->status&WEL) || address+size>protected_start(f)) return true;
    if (!f->image) return false;
    f->operation=f->command; f->operation_address=address; f->operation_size=size;
    f->busy_ns=time;return true;
}

static bool commit(sst25vf080b_t *f) {
    switch (f->command) {
    case 0x06:
        if (f->position==1u) f->status|=WEL;
        break;
    case 0x04:
        if (f->position==1u) { f->status&=(uint8_t)~(WEL|AAI); f->ewsr=false; }
        break;
    case 0x50:
        if (f->position==1u) f->ewsr=true;
        break;
    case 0x70:
        if (f->position==1u) f->busy_output=true;
        break;
    case 0x80:
        if (f->position==1u) f->busy_output=false;
        break;
    case 0x01:
        if (f->position==2u) {
            if (f->status_authorized && (!(f->status&BPL) || f->wp_high))
                f->status=f->data[0]&WRITABLE;
            /* WREN authorization ends at the WRSR CE rising edge. */
            f->status&=(uint8_t)~WEL;
        }
        break;
    case 0x02:
        if (f->position==5u) return start_program(f,f->address,1u);
        break;
    case 0xad:
        if (f->position==(f->aai_continuation?3u:6u))
            return start_program(f,f->aai_continuation?f->aai_next:(f->address&~1u),2u);
        break;
    case 0x20:
        if (f->position==4u) return start_erase(f,0x1000u,f->timing.sector_ns);
        break;
    case 0x52:
        if (f->position==4u) return start_erase(f,0x8000u,f->timing.block32_ns);
        break;
    case 0xd8:
        if (f->position==4u) return start_erase(f,0x10000u,f->timing.block64_ns);
        break;
    case 0x60: case 0xc7:
        if (f->position==1u) return start_erase(f,SST25VF080B_SIZE,f->timing.chip_ns);
        break;
    default: break;
    }
    return true;
}

bool sst25vf080b_pins(sst25vf080b_t *f, bool ce_high, bool wp_high, bool hold_high) {
    if (!valid(f) || (!ce_high && f->powerup_ns)) return false;
    sst25vf080b_t next=*f;
    next.wp_high=wp_high; next.hold_high=hold_high;
    if (next.selected!=!ce_high) {
        if (next.selected && !commit(&next)) return false;
        next.selected=!ce_high; next.position=0u; next.command=0u;
        next.address=0u; next.status_authorized=false; next.aai_continuation=false;
        memset(next.data,0,sizeof next.data);
    }
    *f=next; return true;
}

bool sst25vf080b_ready_pin(const sst25vf080b_t *f,sst25vf080b_output_t *output) {
    if (!valid(f) || !output) return false;
    *output=(sst25vf080b_output_t){0,0};
    if (f->selected && f->hold_high && f->busy_output && (f->status&AAI)) {
        output->value=f->busy_ns?0u:0xffu; output->driven=0xffu;
    }
    return true;
}

static bool begin(sst25vf080b_t *f,uint8_t input) {
    switch (input) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x0b:
    case 0x20: case 0x50: case 0x52: case 0x60: case 0x70: case 0x80: case 0x90:
    case 0x9f: case 0xab: case 0xad: case 0xc7: case 0xd8: break;
    default: return false;
    }
    if (f->busy_ns && input!=0x05u && input!=0x04u) return false;
    if ((f->status&AAI) && input!=0xadu && input!=0x04u &&
        (input!=0x05u || f->busy_output)) return false;
    f->command=input; f->position=1u; f->aai_continuation=(f->status&AAI)!=0u;
    f->status_authorized=(f->status&WEL)!=0u || f->ewsr;
    f->ewsr=false;
    return true;
}

static bool transfer(sst25vf080b_t *f,uint8_t input,sst25vf080b_output_t *out) {
    if (!f->position) return begin(f,input);
    switch (f->command) {
    case 0x05:
        out->value=status(f); out->driven=0xffu; return true;
    case 0x9f: {
        static const uint8_t id[]={0xbf,0x25,0x8e};
        if (f->position>3u) return false;
        out->value=id[f->position++-1u]; out->driven=0xffu; return true;
    }
    case 0x01:
        if (f->position!=1u) return false;
        f->data[0]=input; f->position++; return true;
    case 0x03: case 0x0b: case 0x02: case 0xad: case 0x20: case 0x52: case 0xd8:
    case 0x90: case 0xab:
        if (f->position<=3u && !(f->command==0xadu && f->aai_continuation)) {
            uint32_t address=(f->address<<8)|input;
            if ((f->command==0x90u || f->command==0xabu) && address>1u) return false;
            f->address=address&(SST25VF080B_SIZE-1u); f->position++; return true;
        }
        break;
    default: return false;
    }
    switch (f->command) {
    case 0x0b:
        if (f->position==4u) { f->position++; return true; }
        /* fall through */
    case 0x03:
        if (!f->image) return false;
        out->value=f->image[f->address]; out->driven=0xffu;
        f->address=(f->address+1u)&(SST25VF080B_SIZE-1u); return true;
    case 0x90: case 0xab:
        out->value=f->address?0x8eu:0xbfu; out->driven=0xffu;
        f->address^=1u; return true;
    case 0x02:
        if (f->position!=4u) return false;
        f->data[0]=input; f->position++; return true;
    case 0xad: {
        unsigned first=f->aai_continuation?1u:4u;
        if (f->position>=first+2u) return false;
        f->data[f->position++-first]=input; return true;
    }
    default: return false;
    }
}

bool sst25vf080b_transfer(sst25vf080b_t *f,uint8_t input,sst25vf080b_output_t *output) {
    if (!valid(f) || !output) return false;
    sst25vf080b_t next=*f;
    sst25vf080b_output_t out={0,0};
    if (next.selected && next.hold_high) {
        if (next.powerup_ns) return false;
        (void)sst25vf080b_ready_pin(&next,&out);
        if (!transfer(&next,input,&out)) return false;
    }
    *f=next; *output=out; return true;
}

bool sst25vf080b_advance(sst25vf080b_t *f, uint64_t nanoseconds) {
    if (!valid(f) || (f->busy_ns && !f->image)) return false;
    f->powerup_ns=nanoseconds>=f->powerup_ns?0u:f->powerup_ns-nanoseconds;
    if (!f->busy_ns) return true;
    if (nanoseconds<f->busy_ns) { f->busy_ns-=nanoseconds; return true; }
    if (f->operation==0x02u || f->operation==0xadu) {
        for (uint32_t i=0;i<f->operation_size;++i)
            f->image[f->operation_address+i]&=f->operation_data[i];
    } else memset(f->image+f->operation_address,0xff,f->operation_size);
    f->busy_ns=0u;
    if (f->operation==0xadu && (f->status&AAI)) {
        f->aai_next=f->operation_address+2u;
        /* Section4.3.2 clears WEL at the limit. Sections4.4.4/4.4.12
         * still require WRDI to exit AAI; retain its ready/busy pin mode. */
        if (f->aai_next>=protected_start(f)) f->status&=(uint8_t)~WEL;
    } else f->status&=(uint8_t)~WEL;
    f->operation=0u; f->operation_size=0u;
    return true;
}
