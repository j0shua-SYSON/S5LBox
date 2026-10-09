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
    if (d->packet.hs!=S5L8920_DSIM_HS_OFF) return false;
    for (unsigned i=0;i<3u;++i)
        if ((!(d->known&KNOWN(0x10u)) || (d->reg[4]&(1u<<i))) &&
            d->lane[i].state!=S5L8920_DSIM_STOP) return false;
    return true;
}
static bool transitioning(const s5l8920_dsim_t *d) {
    if (d->packet.hs==S5L8920_DSIM_HS_ENTER || d->packet.hs==S5L8920_DSIM_HS_EXIT) return true;
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
static void clear_packet(s5l8920_dsim_t *d) {
    s5l8920_dsim_packet_input_t input=d->packet.initial;
    bool configured=d->packet.configured;
    memset(&d->packet,0,sizeof d->packet);
    d->packet.initial=input;d->packet.configured=configured;
}
static bool packet_busy(const s5l8920_dsim_t *d) {
    return d->packet.tx_count || d->packet.bus!=S5L8920_DSIM_BUS_IDLE;
}
static bool link_clocked(const s5l8920_dsim_t *d) {
    return d->packet.configured && !d->reset_pending && d->reset_released && d->clock.stable &&
        (d->known&(KNOWN(0x10u)|KNOWN(0x14u)))==(KNOWN(0x10u)|KNOWN(0x14u)) &&
        (d->reg[2]&0x11180000u)==0x11180000u && (d->reg[2]&0xffffu) &&
        (d->reg[4]&3u)==3u && d->lane[0].state==S5L8920_DSIM_STOP &&
        d->lane[1].state==S5L8920_DSIM_STOP && !(d->reg[5]&0x0010000fu);
}
static bool read_command(uint32_t header) {
    unsigned type=header&63u;
    return type==4u || type==0x14u || type==0x24u || type==6u;
}
static bool short_command(uint32_t header) {
    switch (header&63u) {
    case 3u: case 0x13u: case 0x23u: case 5u: case 0x15u: case 0x37u:
    case 4u: case 0x14u: case 0x24u: case 6u:return true;
    default:return false;
    }
}
void s5l8920_dsim_reset(s5l8920_dsim_t *d) {
    if (!d) return;
    if (!d->configured) { memset(d,0,sizeof *d); return; }
    s5l8920_dsim_input_t input=d->initial;
    s5l8920_dsim_readback_input_t readback=d->readback_initial;
    bool readback_configured=d->readback_configured;
    s5l8920_dsim_packet_input_t packet=d->packet.initial;
    bool packet_configured=d->packet.configured;
    memset(d,0,sizeof *d); d->initial=input; d->configured=true;
    d->readback_initial=readback;d->readback_configured=readback_configured;
    d->packet.initial=packet;d->packet.configured=packet_configured;
    d->clock.initial=input.clock; d->clock.configured=true;
    s5l8920_dsim_clock_reset(&d->clock);
    d->reg[2]=input.clock.clkctrl; d->known=KNOWN(8u);
    if (readback_configured && (readback.known&KNOWN(0x0cu))) {
        d->reg[3]=readback.word[3];d->known|=KNOWN(0x0cu);
    }
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
static bool passive_offset(uint32_t offset) {
    return offset==0x0cu || offset==0x30u || offset==0x34u || offset==0x38u ||
        offset==0x3cu || offset==0x48u || (offset>=0x54u && offset<=0x7cu);
}
bool s5l8920_dsim_configure_readback(s5l8920_dsim_t *d,const s5l8920_dsim_readback_input_t *input) {
    if (!d || !input || !d->configured || input->timer<S5L8920_DSIM_TIMER_UNKNOWN ||
        input->timer>S5L8920_DSIM_TIMER_REMAINING) return false;
    for (unsigned i=0;i<32u;++i) {
        bool known=(input->known&(UINT32_C(1)<<i))!=0u;
        if ((known && !passive_offset(4u*i)) || (!known && input->word[i])) return false;
    }
    if ((input->word[3]&0xff000000u) || (input->word[18]&0xffff8080u)) return false;
    if (d->readback_configured) {
        if (input->known!=d->readback_initial.known || input->timer!=d->readback_initial.timer) return false;
        for (unsigned i=0;i<32u;++i) if (input->word[i]!=d->readback_initial.word[i]) return false;
        return true;
    }
    if (d->readback_locked) return false;
    d->readback_initial=*input;d->readback_configured=true;
    if (input->known&KNOWN(0x0cu)) { d->reg[3]=input->word[3];d->known|=KNOWN(0x0cu); }
    return true;
}
bool s5l8920_dsim_configure_packet(s5l8920_dsim_t *d,const s5l8920_dsim_packet_input_t *input) {
    if (!d || !input || !d->configured || !input->hs_enter_cycles || !input->hs_exit_cycles ||
        !input->short_packet_cycles || !input->tx_capacity || input->tx_capacity>S5L8920_DSIM_TX_LIMIT ||
        !input->rx_capacity || input->rx_capacity>S5L8920_DSIM_RX_LIMIT) return false;
    if (d->packet.configured) {
        const s5l8920_dsim_packet_input_t *p=&d->packet.initial;
        return p->hs_enter_cycles==input->hs_enter_cycles && p->hs_exit_cycles==input->hs_exit_cycles &&
            p->short_packet_cycles==input->short_packet_cycles && p->tx_capacity==input->tx_capacity &&
            p->rx_capacity==input->rx_capacity;
    }
    if (d->readback_locked) return false;
    d->packet.initial=*input;d->packet.configured=true;return true;
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
bool s5l8920_dsim_read(s5l8920_dsim_t *d,uint32_t offset,uint32_t *value) {
    if (!d || !value || !d->configured || (offset&3u) || offset>=0x80u) return false;
    if (!offset) {
        uint32_t result=d->initial.clock.idle_status&0xcu;
        if (d->clock.stable) result|=0x80000000u;
        if (d->reset_released) result|=0x100000u;
        for (unsigned i=0;i<3u;++i) {
            if (d->lane[i].state==S5L8920_DSIM_STOP && (i || d->packet.hs==S5L8920_DSIM_HS_OFF))
                result|=1u<<(i?i-1u:8u);
            if (d->lane[i].state==S5L8920_DSIM_ULPS || d->lane[i].state==S5L8920_DSIM_EXITING_ULPS)
                result|=1u<<(i?i+3u:9u);
        }
        if (d->packet.hs==S5L8920_DSIM_HS_ON || d->packet.hs==S5L8920_DSIM_HS_EXIT) result|=0x400u;
        if (d->packet.tx_active || d->packet.bus==S5L8920_DSIM_BUS_REQUEST ||
            d->packet.bus==S5L8920_DSIM_BUS_RECEIVE) result&=~1u;
        *value=result; return true;
    }
    if (offset==4u) { *value=d->reset_request;return true; }
    if (offset==0x50u && !d->clock.timer_readable && d->readback_configured) {
        if (d->readback_initial.timer==S5L8920_DSIM_TIMER_RELOAD) { *value=d->clock.plltmr;return true; }
        if (d->readback_initial.timer==S5L8920_DSIM_TIMER_REMAINING) { *value=d->clock.remaining;return true; }
    }
    if (offset==0x4cu || offset==0x50u) return s5l8920_dsim_clock_read(&d->clock,offset,value);
    if (offset==0x2cu) {
        if (!d->events_known) return false;
        *value=d->events; return true;
    }
    if (offset==0x3cu && d->packet.rx_count) {
        *value=d->packet.rx[d->packet.rx_head];
        d->packet.rx_head=(d->packet.rx_head+1u)%d->packet.initial.rx_capacity;
        --d->packet.rx_count;return true;
    }
    uint32_t mask;
    if (offset!=8u && !latch_mask(offset,&mask)) {
        if (!d->readback_configured || !(d->readback_initial.known&KNOWN(offset)) ||
            (offset==0x3cu && !d->fifos_empty)) return false;
        *value=d->readback_initial.word[offset/4u];return true;
    }
    if (!(d->known&KNOWN(offset))) return false;
    if (offset==0x44u && !d->fifos_empty) return false;
    uint32_t result=d->reg[offset/4u];
    if (offset==0x44u) {
        result|=0x01555500u;
        if (d->packet.tx_count) result&=~0x00400000u;
        if (d->packet.rx_count) result&=~0x01000000u;
        if (d->packet.configured) {
            if (d->packet.tx_count==d->packet.initial.tx_capacity) result|=0x00800000u;
            if (d->packet.rx_count==d->packet.initial.rx_capacity) result|=0x02000000u;
        }
    }
    *value=result;return true;
}
bool s5l8920_dsim_write(s5l8920_dsim_t *d,uint32_t offset,uint32_t value) {
    if (!d || !d->configured) return false;
    if (offset==0x4cu || offset==0x50u) {
        if (packet_busy(d) || d->packet.hs!=S5L8920_DSIM_HS_OFF) return false;
        if (!s5l8920_dsim_clock_write(&d->clock,offset,value)) return false;
        clock_event(d); d->readback_locked=true;return true;
    }
    if (offset==8u) {
        if (value&~(CLK_FIELDS|(d->packet.configured?0x80000000u:0u))) return false;
        bool change=((value^d->reg[2])&0x80000000u)!=0u;
        if (change && ((value&0x80000000u)?(!link_clocked(d) || transitioning(d)):
            d->packet.hs!=S5L8920_DSIM_HS_ON)) return false;
        if (!s5l8920_dsim_clock_write(&d->clock,offset,value&0x1000ffffu)) return false;
        if (change) {
            d->packet.hs=(value&0x80000000u)?S5L8920_DSIM_HS_ENTER:S5L8920_DSIM_HS_EXIT;
            d->packet.hs_remaining=(value&0x80000000u)?d->packet.initial.hs_enter_cycles:d->packet.initial.hs_exit_cycles;
        }
        d->reg[2]=value; d->readback_locked=true;return true;
    }
    if (offset==0x2cu) {
        d->events&=~value;
        d->readback_locked=true;
        return true;
    }
    if (offset==4u) {
        if ((value!=1u && value!=0x10000u) || d->reset_pending || !stopped(d)) return false;
        if (value==1u) {
            d->known=KNOWN(8u);
            if (d->readback_configured && (d->readback_initial.known&KNOWN(0x0cu))) {
                d->reg[3]=d->readback_initial.word[3];d->known|=KNOWN(0x0cu);
            }
        }
        else if (d->known&KNOWN(0x14u)) d->reg[5]&=0xfff000c0u;
        d->events=0u; d->events_known=true; d->clock.stable_event=false;
        d->fifos_empty=true; d->reset_released=false; d->reset_pending=true;
        d->reset_remaining=d->initial.reset_cycles;
        d->reset_request=value;d->readback_locked=true;
        clear_packet(d);
        return true;
    }
    if (offset==0x34u) {
        if (!link_clocked(d) || !d->events_known || !d->fifos_empty ||
            !(d->known&KNOWN(0x44u)) || !(d->reg[17]&8u) ||
            !(d->reg[5]&0xc0u) || (d->reg[4]&0x10000000u) ||
            d->packet.tx_count==d->packet.initial.tx_capacity || (value&0xff000000u) ||
            !short_command(value)) return false;
        /* set_tear_on also starts BTA; TE/forced turnaround is not modeled yet. */
        if ((value&63u)==0x15u && ((value>>8)&255u)==0x35u) return false;
        if (read_command(value) && (!(d->known&KNOWN(0x0cu)) ||
            !(d->reg[3]&0xffffu) || !(d->reg[3]&0xff0000u))) return false;
        unsigned tail=(d->packet.tx_head+d->packet.tx_count)%d->packet.initial.tx_capacity;
        d->packet.tx[tail]=value;
        if (!d->packet.tx_count) d->packet.tx_remaining=d->packet.initial.short_packet_cycles;
        ++d->packet.tx_count;d->readback_locked=true;return true;
    }
    uint32_t mask;
    if (d->reset_pending || !latch_mask(offset,&mask)) return false;
    if (offset==0x14u && d->packet.configured) mask|=0xffe00000u;
    if (value&~mask) return false;
    if (packet_busy(d) && (offset==0x0cu || offset==0x10u || offset==0x14u) &&
        (!(d->known&KNOWN(offset)) || value!=d->reg[offset/4u])) return false;
    if (offset==0x44u && d->packet.configured) {
        if (packet_busy(d) && value!=d->reg[17]) return false;
        if (!(value&8u)) {
            d->packet.tx_count=0u;d->packet.tx_head=0u;d->packet.tx_remaining=0u;d->packet.tx_active=false;
        }
        if (!(value&16u)) { d->packet.rx_count=0u;d->packet.rx_head=0u; }
    }
    if (offset==0x10u) {
        if (((value>>5)&3u)>1u || (transitioning(d) && ((value^d->reg[4])&7u))) return false;
        if (d->packet.hs!=S5L8920_DSIM_HS_OFF && ((value^d->reg[4])&1u)) return false;
    }
    if (offset==0x14u && ((transitioning(d) && ((value^d->reg[5])&0x0010000fu)) ||
        (d->packet.hs!=S5L8920_DSIM_HS_OFF && (value&0x00100003u)))) return false;
    if (offset==0x44u && !value) d->fifos_empty=true;
    d->reg[offset/4u]=value; d->known|=KNOWN(offset); d->readback_locked=true;return true;
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
            d->reset_request=0u;
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
    if (d->packet.hs==S5L8920_DSIM_HS_ENTER || d->packet.hs==S5L8920_DSIM_HS_EXIT) {
        if ((d->reg[4]&1u) && (d->reg[2]&0x80000u)) {
            if (cycles<d->packet.hs_remaining) d->packet.hs_remaining-=(uint32_t)cycles;
            else { d->packet.hs_remaining=0u;d->packet.hs=(d->reg[2]&0x80000000u)?S5L8920_DSIM_HS_ON:S5L8920_DSIM_HS_OFF; }
        }
    }
    for (unsigned i=0;i<3u;++i)
        if (i || d->packet.hs==S5L8920_DSIM_HS_OFF)
        if ((d->reg[4]&(1u<<i)) && (d->reg[2]&(1u<<(19u+i)))) advance_lane(d,i,cycles);
    return true;
}

bool s5l8920_dsim_escape_clock(s5l8920_dsim_t *d,uint64_t cycles) {
    if (!d || !d->configured || !d->packet.configured) return false;
    if (!link_clocked(d) || !cycles) return true;
    if (d->packet.bus!=S5L8920_DSIM_BUS_IDLE) {
        if (cycles<d->packet.bus_remaining) { d->packet.bus_remaining-=(uint32_t)cycles;return true; }
        else {
            cycles-=d->packet.bus_remaining;d->packet.bus_remaining=0u;
            if (d->packet.bus==S5L8920_DSIM_BUS_DELAY) {
                d->packet.bus=S5L8920_DSIM_BUS_REQUEST;
                d->packet.bus_remaining=(d->reg[3]>>16)&255u;
                if (cycles<d->packet.bus_remaining) { d->packet.bus_remaining-=(uint32_t)cycles;return true; }
                else {
                    cycles-=d->packet.bus_remaining;d->packet.bus=S5L8920_DSIM_BUS_IDLE;
                    d->packet.bus_remaining=0u;d->events|=0x100000u;
                }
            } else {
                d->events|=d->packet.bus==S5L8920_DSIM_BUS_REQUEST?0x100000u:0x200000u;
                d->packet.bus=S5L8920_DSIM_BUS_IDLE;
            }
        }
    }
    if (d->packet.tx_count && cycles) {
        d->packet.tx_active=true;
        if (cycles<d->packet.tx_remaining) d->packet.tx_remaining-=(uint32_t)cycles;
        else d->packet.tx_remaining=0u;
    }
    return true;
}
bool s5l8920_dsim_take_packet(s5l8920_dsim_t *d,uint32_t *header) {
    if (!d || !header || !link_clocked(d) || !d->packet.tx_count || d->packet.tx_remaining ||
        d->packet.bus!=S5L8920_DSIM_BUS_IDLE) return false;
    uint32_t value=d->packet.tx[d->packet.tx_head];
    d->packet.tx_head=(d->packet.tx_head+1u)%d->packet.initial.tx_capacity;--d->packet.tx_count;
    d->packet.tx_remaining=d->packet.tx_count?d->packet.initial.short_packet_cycles:0u;
    d->packet.tx_active=false;
    if (read_command(value)) {
        d->packet.bus=S5L8920_DSIM_BUS_DELAY;d->packet.channel=(uint8_t)(value&0xc0u);
        d->packet.bus_remaining=2u+(d->reg[5]>>21);
    }
    *header=value;return true;
}
bool s5l8920_dsim_receive_begin(s5l8920_dsim_t *d) {
    if (!d || !link_clocked(d) || d->packet.bus!=S5L8920_DSIM_BUS_REQUEST) return false;
    d->packet.bus=S5L8920_DSIM_BUS_RECEIVE;d->packet.bus_remaining=d->reg[3]&0xffffu;return true;
}
bool s5l8920_dsim_receive(s5l8920_dsim_t *d,uint32_t header,const uint8_t *payload,unsigned length) {
    if (!d || !link_clocked(d) || d->packet.bus!=S5L8920_DSIM_BUS_RECEIVE ||
        (header&0xc0u)!=d->packet.channel || !(d->reg[17]&16u)) return false;
    unsigned type=header&63u;
    bool long_packet=type==0x1au || type==0x1cu;
    if (long_packet) {
        if (length!=((header>>8)&0xffffu) || (length && !payload)) return false;
    } else if (length || (type!=0x11u && type!=0x12u && type!=0x21u && type!=0x22u && type!=2u)) return false;
    unsigned words=1u+(length+3u)/4u;
    if (words>d->packet.initial.rx_capacity-d->packet.rx_count) return false;
    unsigned tail=(d->packet.rx_head+d->packet.rx_count)%d->packet.initial.rx_capacity;
    for (unsigned i=0;i<words;++i) {
        uint32_t value=header;
        if (i) {
            value=0u;
            for (unsigned j=0;j<4u && 4u*(i-1u)+j<length;++j)
                value|=(uint32_t)payload[4u*(i-1u)+j]<<(8u*j);
        }
        d->packet.rx[tail]=value;tail=(tail+1u)%d->packet.initial.rx_capacity;
    }
    d->packet.rx_count+=words;d->packet.bus=S5L8920_DSIM_BUS_IDLE;d->packet.bus_remaining=0u;
    d->events|=0x02000000u|(type==2u?0x10000u:0x40000u);return true;
}
bool s5l8920_dsim_receive_error(s5l8920_dsim_t *d,uint32_t errors) {
    if (!d || !link_clocked(d) || d->packet.bus!=S5L8920_DSIM_BUS_RECEIVE || !errors || (errors&~0xc000u)) return false;
    d->packet.bus=S5L8920_DSIM_BUS_IDLE;d->packet.bus_remaining=0u;d->events|=errors|0x02000000u;return true;
}
