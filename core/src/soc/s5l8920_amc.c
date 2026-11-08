/* Bounded AMC r2 software control registers.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_amc.h"
#include <string.h>

static bool same_input(const s5l8920_amc_control_input_t *a,
                       const s5l8920_amc_control_input_t *b) {
    return a->start_address==b->start_address && a->start_status==b->start_status &&
        a->interrupt_mask[0]==b->interrupt_mask[0] && a->interrupt_mask[1]==b->interrupt_mask[1];
}
bool s5l8920_amc_control_configure(s5l8920_amc_control_t *d,
    const s5l8920_amc_control_input_t *input) {
    if (!d || !input) return false;
    if (d->configured) return same_input(&d->initial,input);
    s5l8920_amc_control_t next; memset(&next,0,sizeof next);
    next.initial=*input; next.current=*input; next.configured=true; *d=next;
    return true;
}
void s5l8920_amc_control_reset(s5l8920_amc_control_t *d) {
    if (!d) return;
    if (!d->configured) { memset(d,0,sizeof *d); return; }
    d->current=d->initial;
}
bool s5l8920_amc_control_read(const s5l8920_amc_control_t *d,
    uint32_t offset,unsigned size,uint32_t *value) {
    if (!d || !d->configured || !value || size!=4u) return false;
    switch (offset) {
    case 0x988u:*value=d->current.start_address;return true;
    case 0x998u:*value=d->current.start_status;return true;
    case 0xa94u:*value=d->current.interrupt_mask[0];return true;
    case 0xb14u:*value=d->current.interrupt_mask[1];return true;
    default:return false;
    }
}
bool s5l8920_amc_control_write(s5l8920_amc_control_t *d,
    uint32_t offset,unsigned size,uint32_t value) {
    if (!d || !d->configured || size!=4u) return false;
    switch (offset) {
    case 0x988u:
        if (value&0xffff0000u) return false;
        d->current.start_address=(d->current.start_address&0xffff0000u)|value;
        return true;
    case 0x990u:case 0x994u:
        if (value!=1u) return false;
        if (offset==0x990u) d->current.start_status|=1u;
        else d->current.start_status&=~1u;
        return true;
    case 0xa8cu:case 0xa90u:
        if (value&0xc0000000u) return false;
        if (offset==0xa8cu) d->current.interrupt_mask[0]|=value;
        else d->current.interrupt_mask[0]&=~value;
        return true;
    case 0xb0cu:d->current.interrupt_mask[1]|=value;return true;
    case 0xb10u:d->current.interrupt_mask[1]&=~value;return true;
    default:return false;
    }
}
