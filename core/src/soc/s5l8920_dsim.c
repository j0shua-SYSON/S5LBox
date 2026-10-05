/* Bounded DSIM PLL control and system-clock stability timer.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_dsim.h"
#include <string.h>

#define PLL_ENABLE UINT32_C(0x00800000)
#define PLL_FIELDS UINT32_C(0x0f8ffffe)
#define CLOCK_FIELDS UINT32_C(0x1000ffff)
#define IDLE_FIELDS UINT32_C(0x0010010f)

static bool same_input(const s5l8920_dsim_clock_input_t *a,
                       const s5l8920_dsim_clock_input_t *b) {
    return a->reference_hz==b->reference_hz && a->idle_status==b->idle_status &&
        a->clkctrl==b->clkctrl && a->pllctrl==b->pllctrl && a->plltmr==b->plltmr;
}

bool s5l8920_dsim_clock_configure(s5l8920_dsim_clock_t *d,
    const s5l8920_dsim_clock_input_t *input) {
    if (!d || !input || !input->reference_hz ||
        (input->idle_status&~IDLE_FIELDS) || (input->clkctrl&~CLOCK_FIELDS) ||
        (input->pllctrl&~(PLL_FIELDS&~PLL_ENABLE))) return false;
    if (d->configured) return same_input(&d->initial,input);
    d->initial=*input; d->configured=true;
    s5l8920_dsim_clock_reset(d);
    return true;
}

void s5l8920_dsim_clock_reset(s5l8920_dsim_clock_t *d) {
    if (!d) return;
    if (!d->configured) { memset(d,0,sizeof *d); return; }
    d->clkctrl=d->initial.clkctrl; d->pllctrl=d->initial.pllctrl;
    d->plltmr=d->initial.plltmr; d->remaining=0u;
    d->stable=false; d->stable_event=false; d->timer_readable=true;
}

bool s5l8920_dsim_clock_read(const s5l8920_dsim_clock_t *d,
    uint32_t offset, uint32_t *value) {
    if (!d || !value || !d->configured) return false;
    switch (offset) {
    case 0u: *value=d->initial.idle_status|(d->stable?0x80000000u:0u); return true;
    case 8u: *value=d->clkctrl; return true;
    case 0x4cu: *value=d->pllctrl; return true;
    case 0x50u:
        if (!d->timer_readable) return false;
        *value=d->plltmr; return true;
    default: return false;
    }
}

bool s5l8920_dsim_clock_write(s5l8920_dsim_clock_t *d,
    uint32_t offset, uint32_t value) {
    if (!d || !d->configured) return false;
    switch (offset) {
    case 8u:
        if (value&~CLOCK_FIELDS) return false;
        d->clkctrl=value; return true;
    case 0x4cu: {
        if (value&~PLL_FIELDS) return false;
        bool enabled=(value&PLL_ENABLE)!=0u;
        bool was_enabled=(d->pllctrl&PLL_ENABLE)!=0u;
        if (enabled && (!((value>>14)&63u) || !((value>>4)&1023u))) return false;
        if (was_enabled && enabled && value!=d->pllctrl) return false;
        if (!enabled) { d->stable=false; d->remaining=0u; }
        else if (!was_enabled) {
            d->remaining=d->plltmr; d->stable=d->remaining==0u;
            d->timer_readable=false;
            if (d->stable) d->stable_event=true;
        }
        d->pllctrl=value; return true;
    }
    case 0x50u:
        if (d->pllctrl&PLL_ENABLE) return false;
        d->plltmr=value; d->timer_readable=true; return true;
    default: return false;
    }
}

bool s5l8920_dsim_clock_advance(s5l8920_dsim_clock_t *d, uint64_t cycles) {
    if (!d || !d->configured) return false;
    if (!(d->pllctrl&PLL_ENABLE) || d->stable || !cycles) return true;
    if (cycles<d->remaining) d->remaining-=(uint32_t)cycles;
    else { d->remaining=0u; d->stable=true; d->stable_event=true; }
    return true;
}

bool s5l8920_dsim_clock_rate(const s5l8920_dsim_clock_t *d,
    uint64_t *numerator, uint32_t *denominator) {
    if (!d || !numerator || !denominator || !d->configured || !d->stable) return false;
    *numerator=(uint64_t)d->initial.reference_hz*((d->pllctrl>>4)&1023u);
    *denominator=((d->pllctrl>>14)&63u)*(1u<<((d->pllctrl>>1)&7u));
    return true;
}
