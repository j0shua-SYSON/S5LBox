/* Checked SWI control and foreground requests; no implicit regulator peer.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_swi.h"
#include <string.h>
enum { CONTROL=1u, DELAY=2u, PRIMARY=4u, SECONDARY=8u };

bool s5l8920_swi_configure(s5l8920_swi_t *s,const s5l8920_swi_input_t *input) {
    if (!s || !input || !input->transfer_cycles || (input->idle_primary&~2u) ||
        (input->idle_secondary&~2u)) return false;
    if (s->configured) return s->initial.transfer_cycles==input->transfer_cycles &&
        s->initial.idle_primary==input->idle_primary && s->initial.idle_secondary==input->idle_secondary;
    memset(s,0,sizeof *s);s->initial=*input;s->configured=true;s->primary_status=input->idle_primary;
    return true;
}
void s5l8920_swi_reset(s5l8920_swi_t *s) {
    if (!s) return;
    s5l8920_swi_input_t input=s->initial;uint64_t sequence=s->sequence;bool configured=s->configured;
    memset(s,0,sizeof *s);
    if (configured) { s->initial=input;s->configured=true;s->sequence=sequence;s->primary_status=input.idle_primary; }
}
bool s5l8920_swi_read(const s5l8920_swi_t *s,uint32_t offset,uint32_t *value) {
    if (!s || !value || !s->configured) return false;
    uint32_t result;unsigned known=0u;
    switch (offset) {
    case 0u:result=s->control;known=CONTROL;break;
    case 0x14u:result=s->primary_status;break;
    case 0x18u:result=s->primary_data;known=PRIMARY;break;
    case 0x1cu:result=s->initial.idle_secondary;break;
    case 0x20u:result=s->secondary_data;known=SECONDARY;break;
    case 0x24u:result=s->str_delay;known=DELAY;break;
    default:return false;
    }
    if (known && !(s->known&known)) return false;
    *value=result;return true;
}
bool s5l8920_swi_write(s5l8920_swi_t *s,uint32_t offset,uint32_t value) {
    if (!s || !s->configured) return false;
    switch (offset) {
    case 0u:
        if ((value&~0xff03u) || ((value&3u)!=0u && (value&3u)!=3u) ||
            (s->pending && value!=s->control)) return false;
        s->control=value;s->known|=CONTROL;return true;
    case 0x24u:
        if (s->pending && value!=s->str_delay) return false;
        s->str_delay=value;s->known|=DELAY;return true;
    case 0x18u:
        if ((value&~0x7fffu) || s->pending) return false;
        s->primary_data=value;s->known|=PRIMARY;return true;
    case 0x20u:
        if ((value&~0x7fffu) || s->pending) return false;
        s->secondary_data=value;s->known|=SECONDARY;return true;
    case 0x1cu:return value==s->initial.idle_secondary;
    case 0x14u:
        if ((value!=1u && value!=3u) || s->pending || s->sequence==UINT64_MAX ||
            (s->known&(CONTROL|DELAY|PRIMARY))!=(CONTROL|DELAY|PRIMARY) || (s->control&3u)!=3u) return false;
        s->request=(s5l8920_swi_request_t){++s->sequence,s->control,s->str_delay,s->primary_data,value};
        s->primary_status=value;s->pending=true;s->remaining=s->initial.transfer_cycles;s->phase=0u;
        return true;
    default:return false;
    }
}
bool s5l8920_swi_source_clock(s5l8920_swi_t *s,uint64_t cycles) {
    if (!s || !s->configured) return false;
    if (!s->pending || !s->remaining || !cycles) return true;
    uint32_t divisor=(s->control>>8)+1u;
    uint64_t elapsed=cycles/divisor;
    uint32_t phase=s->phase+(uint32_t)(cycles%divisor);
    elapsed+=phase/divisor;
    if (elapsed>=s->remaining) { s->remaining=0u;s->phase=0u; }
    else { s->remaining-=(uint32_t)elapsed;s->phase=phase%divisor; }
    return true;
}
bool s5l8920_swi_peek(const s5l8920_swi_t *s,s5l8920_swi_request_t *request,bool *ready) {
    if (!s || !request || !ready || !s->configured || !s->pending) return false;
    *request=s->request;*ready=!s->remaining;return true;
}
bool s5l8920_swi_take(s5l8920_swi_t *s,uint64_t sequence,s5l8920_swi_request_t *request) {
    if (!s || !request || !s->configured || !s->pending || s->remaining || s->request.sequence!=sequence) return false;
    *request=s->request;s->pending=false;s->primary_status=s->initial.idle_primary;
    memset(&s->request,0,sizeof s->request);return true;
}
