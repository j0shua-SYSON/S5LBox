/* DSIM reset, programmed configuration and idle/ULPS lane control.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_dsim.h"
#include <string.h>

#define KNOWN(o) (UINT32_C(1)<<((o)/4u))
#define CLK_FIELDS UINT32_C(0x1138ffff)
#define ESC_FIELDS UINT32_C(0x001000cf)

static bool same_input(const s5l8920_dsim_input_t *a,const s5l8920_dsim_input_t *b) {
    return a->clock.reference_hz==b->clock.reference_hz &&
        a->clock.idle_status==b->clock.idle_status && a->clock.clkctrl==b->clock.clkctrl &&
        a->clock.pllctrl==b->clock.pllctrl && a->clock.plltmr==b->clock.plltmr &&
        a->reset_cycles==b->reset_cycles && a->stop_cycles==b->stop_cycles &&
        a->entry_cycles==b->entry_cycles && a->exit_cycles==b->exit_cycles &&
        a->wakeup_cycles==b->wakeup_cycles;
}
static bool stopped(const s5l8920_dsim_t *d) {
    for (unsigned i=0;i<3u;++i)
        if ((!(d->known&KNOWN(0x10u)) || (d->reg[4]&(1u<<i))) &&
            d->lane[i].state!=S5L8920_DSIM_STOP) return false;
    return true;
}
static bool transitioning(const s5l8920_dsim_t *d) {
    for (unsigned i=0;i<3u;++i) {
        s5l8920_dsim_lane_state_t s=d->lane[i].state;
        if (s==S5L8920_DSIM_ENTERING_ULPS || s==S5L8920_DSIM_EXITING_ULPS ||
            s==S5L8920_DSIM_STOPPING) return true;
    }
    return false;
}
static void clock_event(s5l8920_dsim_t *d) {
    if (d->clock.stable_event) { d->events|=0x80000000u; d->clock.stable_event=false; }
}
void s5l8920_dsim_reset(s5l8920_dsim_t *d) {
    if (!d) return;
    if (!d->configured) { memset(d,0,sizeof *d); return; }
    s5l8920_dsim_input_t input=d->initial;
    memset(d,0,sizeof *d); d->initial=input; d->configured=true;
    d->clock.initial=input.clock; d->clock.configured=true;
    s5l8920_dsim_clock_reset(&d->clock);
    d->reg[2]=input.clock.clkctrl; d->known=KNOWN(8u);
    d->reset_released=(input.clock.idle_status&0x100000u)!=0u;
}
bool s5l8920_dsim_configure(s5l8920_dsim_t *d,const s5l8920_dsim_input_t *input) {
    if (!d || !input || !input->reset_cycles || !input->stop_cycles ||
        !input->entry_cycles || !input->exit_cycles || !input->wakeup_cycles ||
        (input->clock.idle_status&0x100103u)!=0x100103u) return false;
    if (d->configured) return same_input(&d->initial,input);
    s5l8920_dsim_t next; memset(&next,0,sizeof next);
    if (!s5l8920_dsim_clock_configure(&next.clock,&input->clock)) return false;
    next.initial=*input; next.configured=true;
    s5l8920_dsim_reset(&next); *d=next; return true;
}
static bool latch_mask(uint32_t offset,uint32_t *mask) {
    switch (offset) {
    case 0x0cu: *mask=0x00ffffffu; return true;
    case 0x10u: *mask=0x1fff7767u; return true; /* Two data lanes only. */
    case 0x14u: *mask=ESC_FIELDS; return true;
    case 0x18u: case 0x28u: *mask=0x07ff07ffu; return true; /* No image enable. */
    case 0x1cu: *mask=0xf7ff07ffu; return true;
    case 0x20u: *mask=UINT32_MAX; return true;
    case 0x24u: *mask=0xffc0ffffu; return true;
    case 0x40u: *mask=0x1ffu; return true;
    case 0x44u: *mask=0x1fu; return true;
    default: return false;
    }
}
bool s5l8920_dsim_read(const s5l8920_dsim_t *d,uint32_t offset,uint32_t *value) {
    if (!d || !value || !d->configured) return false;
    if (!offset) {
        uint32_t result=d->initial.clock.idle_status&0xcu;
        if (d->clock.stable) result|=0x80000000u;
        if (d->reset_released) result|=0x100000u;
        for (unsigned i=0;i<3u;++i) {
            if (d->lane[i].state==S5L8920_DSIM_STOP) result|=1u<<(i?i-1u:8u);
            if (d->lane[i].state==S5L8920_DSIM_ULPS || d->lane[i].state==S5L8920_DSIM_EXITING_ULPS)
                result|=1u<<(i?i+3u:9u);
        }
        *value=result; return true;
    }
    if (offset==0x4cu || offset==0x50u) return s5l8920_dsim_clock_read(&d->clock,offset,value);
    if (offset==0x2cu) {
        if (!d->events_known) return false;
        *value=d->events; return true;
    }
    uint32_t mask;
    if (offset!=8u && !latch_mask(offset,&mask)) return false;
    if (!(d->known&KNOWN(offset))) return false;
    if (offset==0x44u && !d->fifos_empty) return false;
    *value=d->reg[offset/4u]|(offset==0x44u?0x01555500u:0u); return true;
}
bool s5l8920_dsim_write(s5l8920_dsim_t *d,uint32_t offset,uint32_t value) {
    if (!d || !d->configured) return false;
    if (offset==0x4cu || offset==0x50u) {
        if (!s5l8920_dsim_clock_write(&d->clock,offset,value)) return false;
        clock_event(d); return true;
    }
    if (offset==8u) {
        if (value&~CLK_FIELDS) return false;
        if (!s5l8920_dsim_clock_write(&d->clock,offset,value&0x1000ffffu)) return false;
        d->reg[2]=value; return true;
    }
    if (offset==0x2cu) {
        d->events&=~value;
        return true;
    }
    if (offset==4u) {
        if ((value!=1u && value!=0x10000u) || d->reset_pending || !stopped(d)) return false;
        if (value==1u) d->known=KNOWN(8u);
        else if (d->known&KNOWN(0x14u)) d->reg[5]&=0x001000c0u;
        d->events=0u; d->events_known=true; d->clock.stable_event=false;
        d->fifos_empty=true; d->reset_released=false; d->reset_pending=true;
        d->reset_remaining=d->initial.reset_cycles;
        return true;
    }
    uint32_t mask;
    if (d->reset_pending || !latch_mask(offset,&mask) || (value&~mask)) return false;
    if (offset==0x10u) {
        if (((value>>5)&3u)>1u || (transitioning(d) && ((value^d->reg[4])&7u))) return false;
    }
    if (offset==0x14u && transitioning(d) && ((value^d->reg[5])&0x0010000fu)) return false;
    if (offset==0x44u && !value) d->fifos_empty=true;
    d->reg[offset/4u]=value; d->known|=KNOWN(offset); return true;
}
bool s5l8920_dsim_system_clock(s5l8920_dsim_t *d,uint64_t cycles) {
    if (!d || !d->configured) return false;
    uint64_t reset_cycles=cycles;
    if (!d->clock.stable) {
        if (!(d->clock.pllctrl&0x800000u) || cycles<=d->clock.remaining) reset_cycles=0u;
        else reset_cycles=cycles-d->clock.remaining;
    }
    if (!s5l8920_dsim_clock_advance(&d->clock,cycles)) return false;
    clock_event(d);
    if (d->reset_pending && reset_cycles) {
        if (reset_cycles<d->reset_remaining) d->reset_remaining-=(uint32_t)reset_cycles;
        else {
            d->reset_remaining=0u; d->reset_pending=false; d->reset_released=true;
            d->events|=0x40000000u;
        }
    }
    return true;
}
static void start_lane(s5l8920_dsim_lane_t *lane,s5l8920_dsim_lane_state_t state,uint32_t cycles) {
    lane->state=state; lane->remaining=cycles;
}
static void advance_lane(s5l8920_dsim_t *d,unsigned index,uint64_t cycles) {
    s5l8920_dsim_lane_t *lane=&d->lane[index];
    uint32_t esc=d->reg[5];
    bool enter=(esc&(index?8u:2u))!=0u, exit=(esc&(index?4u:1u))!=0u;
    while (cycles) {
        if (esc&0x100000u) {
            if (lane->state==S5L8920_DSIM_STOP) return;
            if (lane->state!=S5L8920_DSIM_STOPPING)
                start_lane(lane,S5L8920_DSIM_STOPPING,d->initial.stop_cycles);
        } else if (lane->state==S5L8920_DSIM_STOP) {
            if (!enter || exit) return;
            start_lane(lane,S5L8920_DSIM_ENTERING_ULPS,d->initial.entry_cycles);
        } else if (lane->state==S5L8920_DSIM_ULPS) {
            if (!exit) return;
            start_lane(lane,S5L8920_DSIM_EXITING_ULPS,d->initial.exit_cycles);
        } else if (lane->state==S5L8920_DSIM_WAKEUP && !lane->remaining) {
            if (enter || exit) return;
            start_lane(lane,S5L8920_DSIM_STOPPING,d->initial.stop_cycles);
        }
        if (cycles<lane->remaining) { lane->remaining-=(uint32_t)cycles; return; }
        cycles-=lane->remaining; lane->remaining=0u;
        switch (lane->state) {
        case S5L8920_DSIM_ENTERING_ULPS: lane->state=S5L8920_DSIM_ULPS; break;
        case S5L8920_DSIM_EXITING_ULPS:
            start_lane(lane,S5L8920_DSIM_WAKEUP,d->initial.wakeup_cycles); break;
        case S5L8920_DSIM_STOPPING: lane->state=S5L8920_DSIM_STOP; break;
        case S5L8920_DSIM_WAKEUP:
            if (enter || exit) return;
            start_lane(lane,S5L8920_DSIM_STOPPING,d->initial.stop_cycles); break;
        default: return;
        }
    }
}
bool s5l8920_dsim_phy_clock(s5l8920_dsim_t *d,uint64_t cycles) {
    if (!d || !d->configured) return false;
    if (!cycles || d->reset_pending || !d->reset_released || !d->clock.stable ||
        (d->known&(KNOWN(0x10u)|KNOWN(0x14u)))!=(KNOWN(0x10u)|KNOWN(0x14u)) ||
        (d->reg[2]&0x11000000u)!=0x11000000u || !(d->reg[2]&0xffffu)) return true;
    for (unsigned i=0;i<3u;++i)
        if ((d->reg[4]&(1u<<i)) && (d->reg[2]&(1u<<(19u+i)))) advance_lane(d,i,cycles);
    return true;
}
