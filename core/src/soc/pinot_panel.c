/* Explicit Pinot panel reset, power, sleep commands and B1 identification.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "pinot_panel.h"
#include <string.h>

static void cancel_runtime(pinot_panel_t *p) {
    p->reset_low_elapsed=p->recovery_remaining=p->sleep_remaining=p->reply_remaining=0u;
    p->sleep=PINOT_PANEL_ASLEEP;p->reset_valid=p->display_on=p->reply_pending=false;
}
static bool available(const pinot_panel_t *p) {
    return p->configured && p->powered && p->reset_high && p->reset_valid && !p->recovery_remaining;
}
bool pinot_panel_configure(pinot_panel_t *p,const pinot_panel_input_t *input) {
    if (!p || !input || input->channel>3u || !input->reset_low_ns || !input->reset_release_ns ||
        !input->sleep_out_ns || !input->sleep_in_ns || !input->reply_ns) return false;
    if (p->configured) {
        const pinot_panel_input_t *a=&p->initial;
        return a->channel==input->channel && !memcmp(a->identity,input->identity,PINOT_PANEL_ID_BYTES) &&
            a->reset_low_ns==input->reset_low_ns && a->reset_release_ns==input->reset_release_ns &&
            a->sleep_out_ns==input->sleep_out_ns && a->sleep_in_ns==input->sleep_in_ns && a->reply_ns==input->reply_ns;
    }
    memset(p,0,sizeof *p);p->initial=*input;p->configured=true;return true;
}
void pinot_panel_reset(pinot_panel_t *p) {
    if (!p) return;
    if (!p->configured) { memset(p,0,sizeof *p);return; }
    pinot_panel_input_t input=p->initial;memset(p,0,sizeof *p);p->initial=input;p->configured=true;
}
bool pinot_panel_power(pinot_panel_t *p,bool on) {
    if (!p || !p->configured) return false;
    if (p->powered!=on) { cancel_runtime(p);p->powered=on; }
    return true;
}
bool pinot_panel_reset_pin(pinot_panel_t *p,bool high) {
    if (!p || !p->configured) return false;
    if (p->reset_high==high) return true;
    bool valid=p->powered && p->reset_low_elapsed>=p->initial.reset_low_ns;
    cancel_runtime(p);p->reset_high=high;
    if (high && valid) { p->reset_valid=true;p->recovery_remaining=p->initial.reset_release_ns; }
    return true;
}
static uint64_t remaining(uint64_t count,uint64_t elapsed) { return elapsed<count?count-elapsed:0u; }
bool pinot_panel_advance(pinot_panel_t *p,uint64_t nanoseconds) {
    if (!p || !p->configured) return false;
    if (!p->powered || !nanoseconds) return true;
    if (!p->reset_high) {
        uint64_t need=p->initial.reset_low_ns-p->reset_low_elapsed;
        p->reset_low_elapsed+=nanoseconds<need?nanoseconds:need;
        return true;
    }
    if (!p->reset_valid) return true;
    p->recovery_remaining=remaining(p->recovery_remaining,nanoseconds);
    p->reply_remaining=remaining(p->reply_remaining,nanoseconds);
    p->sleep_remaining=remaining(p->sleep_remaining,nanoseconds);
    if (!p->sleep_remaining) {
        if (p->sleep==PINOT_PANEL_WAKING) p->sleep=PINOT_PANEL_AWAKE;
        else if (p->sleep==PINOT_PANEL_SLEEPING) p->sleep=PINOT_PANEL_ASLEEP;
    }
    return true;
}
bool pinot_panel_command(pinot_panel_t *p,uint32_t header) {
    if (!p || !p->configured || (header&0xff000000u)) return false;
    if (!available(p) || ((header>>6)&3u)!=p->initial.channel) return true;
    unsigned type=header&63u,command=(header>>8)&255u;
    if (header&0xff0000u) return false;
    if (type==0x14u && command==0xb1u) {
        if (p->reply_pending) return false;
        p->reply_pending=true;p->reply_remaining=p->initial.reply_ns;return true;
    }
    if (type!=5u) return false;
    switch (command) {
    case 0u:return true;
    case 1u:
        cancel_runtime(p);p->reset_valid=true;p->recovery_remaining=p->initial.reset_release_ns;return true;
    case 0x11u:
        if (p->sleep==PINOT_PANEL_SLEEPING) return false;
        if (p->sleep==PINOT_PANEL_ASLEEP) { p->sleep=PINOT_PANEL_WAKING;p->sleep_remaining=p->initial.sleep_out_ns; }
        return true;
    case 0x10u:
        if (p->display_on || p->sleep==PINOT_PANEL_WAKING) return false;
        if (p->sleep==PINOT_PANEL_AWAKE) { p->sleep=PINOT_PANEL_SLEEPING;p->sleep_remaining=p->initial.sleep_in_ns; }
        return true;
    case 0x28u:p->display_on=false;return true;
    case 0x29u:
        if (p->sleep!=PINOT_PANEL_AWAKE) return false;
        p->display_on=true;return true;
    default:return false;
    }
}
bool pinot_panel_service(pinot_panel_t *p,s5l8920_dsim_t *d) {
    if (!p || !d || !p->configured || !d->configured || !d->packet.configured) return false;
    if (!d->packet.tx_count && d->packet.bus==S5L8920_DSIM_BUS_IDLE && !p->reply_pending) return true;
    pinot_panel_t next=*p;s5l8920_dsim_t controller=*d;uint32_t header;
    if (controller.packet.bus==S5L8920_DSIM_BUS_IDLE && next.reply_pending) {
        next.reply_pending=false;next.reply_remaining=0u;
    }
    if (s5l8920_dsim_take_packet(&controller,&header) && !pinot_panel_command(&next,header)) return false;
    if (next.reply_pending && available(&next)) {
        if (controller.packet.bus==S5L8920_DSIM_BUS_REQUEST)
            (void)s5l8920_dsim_receive_begin(&controller);
        if (controller.packet.bus==S5L8920_DSIM_BUS_RECEIVE && !next.reply_remaining &&
            s5l8920_dsim_receive(&controller,(PINOT_PANEL_ID_BYTES<<8)|0x1au|((uint32_t)next.initial.channel<<6),
                next.initial.identity,PINOT_PANEL_ID_BYTES)) next.reply_pending=false;
    }
    *p=next;*d=controller;return true;
}
