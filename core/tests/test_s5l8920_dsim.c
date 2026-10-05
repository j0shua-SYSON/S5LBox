/* DSIM clock timing, refusal and retained-state boundaries; no target image.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_dsim.h"
#include <stdio.h>
#include <string.h>

static unsigned passed, failed;
#define CHECK(x) do { if (x) ++passed; else { if (failed<24u) { \
    printf("FAIL %s:%d %s\n",__func__,__LINE__,#x); } ++failed; } } while (0)
static const s5l8920_dsim_clock_input_t input={24000000u,0x0010010fu,0xffffu,0u,UINT32_MAX};
#define PMS ((12u<<14)|(343u<<4)|(1u<<1))
#define ENABLED (0x06800000u|PMS)

static s5l8920_dsim_clock_t fresh(void) {
    s5l8920_dsim_clock_t d; memset(&d,0,sizeof d);
    CHECK(s5l8920_dsim_clock_configure(&d,&input)); return d;
}
static uint32_t read_at(const s5l8920_dsim_clock_t *d,uint32_t offset) {
    uint32_t value=0xa5a5a5a5u;
    CHECK(s5l8920_dsim_clock_read(d,offset,&value)); return value;
}
static void refuse_read(s5l8920_dsim_clock_t *d,uint32_t offset) {
    s5l8920_dsim_clock_t before=*d; uint32_t value=0xa5a5a5a5u;
    CHECK(!s5l8920_dsim_clock_read(d,offset,&value));
    CHECK(value==0xa5a5a5a5u && !memcmp(d,&before,sizeof before));
}
static void refuse_write(s5l8920_dsim_clock_t *d,uint32_t offset,uint32_t value) {
    s5l8920_dsim_clock_t before=*d;
    CHECK(!s5l8920_dsim_clock_write(d,offset,value));
    CHECK(!memcmp(d,&before,sizeof before));
}
static void test_inputs(void) {
    s5l8920_dsim_clock_t d={0},before=d;
    refuse_read(&d,0u); refuse_write(&d,8u,0u);
    CHECK(!s5l8920_dsim_clock_advance(&d,UINT64_MAX));
    CHECK(!s5l8920_dsim_clock_configure(NULL,&input));
    CHECK(!s5l8920_dsim_clock_configure(&d,NULL));
    CHECK(!s5l8920_dsim_clock_read(NULL,0u,NULL));
    CHECK(!s5l8920_dsim_clock_write(NULL,0u,0u));
    CHECK(!s5l8920_dsim_clock_advance(NULL,1u));
    s5l8920_dsim_clock_input_t bad=input; bad.reference_hz=0u;
    CHECK(!s5l8920_dsim_clock_configure(&d,&bad));
    CHECK(!memcmp(&d,&before,sizeof d));
    for (unsigned bit=0;bit<32u;++bit) {
        uint32_t mask=UINT32_C(1)<<bit;
        if (!(mask&0x0010010fu)) {
            bad=input; bad.idle_status|=mask;
            CHECK(!s5l8920_dsim_clock_configure(&d,&bad));
        }
        if (!(mask&0x1000ffffu)) {
            bad=input; bad.clkctrl|=mask;
            CHECK(!s5l8920_dsim_clock_configure(&d,&bad));
        }
        if (!(mask&0x0f0ffffeu)) {
            bad=input; bad.pllctrl=mask;
            CHECK(!s5l8920_dsim_clock_configure(&d,&bad));
        }
        CHECK(!memcmp(&d,&before,sizeof d));
    }
    d=fresh(); before=d;
    CHECK(!s5l8920_dsim_clock_read(&d,0u,NULL));
    CHECK(s5l8920_dsim_clock_configure(&d,&input));
    bad=input; bad.idle_status^=1u;
    CHECK(!s5l8920_dsim_clock_configure(&d,&bad));
    bad=input; ++bad.reference_hz;
    CHECK(!s5l8920_dsim_clock_configure(&d,&bad));
    CHECK(!memcmp(&d,&before,sizeof d));
    CHECK(read_at(&d,0u)==input.idle_status && read_at(&d,8u)==input.clkctrl &&
          read_at(&d,0x4cu)==input.pllctrl && read_at(&d,0x50u)==input.plltmr);
    s5l8920_dsim_clock_reset(NULL);
}
static void test_programming_and_time(void) {
    s5l8920_dsim_clock_t d=fresh();
    CHECK(s5l8920_dsim_clock_write(&d,8u,0x10000004u));
    CHECK(s5l8920_dsim_clock_write(&d,0x4cu,0x06000000u));
    CHECK(s5l8920_dsim_clock_write(&d,0x50u,300000u));
    CHECK(s5l8920_dsim_clock_write(&d,0x4cu,0x06000000u|PMS));
    CHECK(s5l8920_dsim_clock_write(&d,0x4cu,ENABLED));
    CHECK(!d.stable && !d.stable_event && d.remaining==300000u);
    CHECK(read_at(&d,8u)==0x10000004u && read_at(&d,0x4cu)==ENABLED);
    s5l8920_dsim_clock_t before=d;
    for (unsigned i=0;i<10000u;++i) CHECK(read_at(&d,0u)==input.idle_status);
    CHECK(!memcmp(&d,&before,sizeof d));
    CHECK(s5l8920_dsim_clock_configure(&d,&input));
    CHECK(s5l8920_dsim_clock_write(&d,0x4cu,ENABLED));
    CHECK(s5l8920_dsim_clock_advance(&d,0u));
    CHECK(!memcmp(&d,&before,sizeof d));
    refuse_read(&d,0x50u); refuse_write(&d,0x50u,42u);
    refuse_write(&d,0x4cu,ENABLED^(1u<<4));
    refuse_write(&d,0x4cu,ENABLED^(1u<<24));
    CHECK(s5l8920_dsim_clock_advance(&d,299999u));
    CHECK(d.remaining==1u && !d.stable && !d.stable_event);
    uint64_t numerator=123u; uint32_t denominator=456u;
    CHECK(!s5l8920_dsim_clock_rate(&d,&numerator,&denominator));
    CHECK(numerator==123u && denominator==456u);
    CHECK(s5l8920_dsim_clock_advance(&d,1u));
    CHECK(d.stable && d.stable_event && !d.remaining);
    CHECK(read_at(&d,0u)==0x8010010fu);
    CHECK(s5l8920_dsim_clock_rate(&d,&numerator,&denominator));
    CHECK(numerator==UINT64_C(8232000000) && denominator==24u);
    CHECK(numerator/denominator==343000000u);
    before=d; CHECK(s5l8920_dsim_clock_advance(&d,UINT64_MAX));
    CHECK(!memcmp(&d,&before,sizeof d));
    CHECK(s5l8920_dsim_clock_write(&d,0x4cu,ENABLED&~0x800000u));
    CHECK(!d.stable && !d.remaining && d.stable_event);
    refuse_read(&d,0x50u);
    CHECK(!s5l8920_dsim_clock_rate(&d,&numerator,&denominator));
    CHECK(s5l8920_dsim_clock_write(&d,0x50u,17u));
    CHECK(read_at(&d,0x50u)==17u);
    CHECK(s5l8920_dsim_clock_write(&d,0x4cu,ENABLED));
    CHECK(!d.stable && d.remaining==17u);
    s5l8920_dsim_clock_reset(&d);
    before=fresh(); CHECK(!memcmp(&d,&before,sizeof d));
    CHECK(s5l8920_dsim_clock_advance(&d,UINT64_MAX));
    CHECK(!memcmp(&d,&before,sizeof d));
}
static void test_bounds(void) {
    s5l8920_dsim_clock_t d=fresh();
    for (uint32_t offset=0u;offset<0x1000u;++offset) {
        if (offset!=0u && offset!=8u && offset!=0x4cu && offset!=0x50u) refuse_read(&d,offset);
        if (offset!=8u && offset!=0x4cu && offset!=0x50u) refuse_write(&d,offset,0u);
    }
    refuse_read(&d,UINT32_MAX); refuse_write(&d,0x1000u,0u);
    for (unsigned bit=0;bit<32u;++bit) {
        uint32_t mask=UINT32_C(1)<<bit;
        if (!(mask&0x0f8ffffeu)) refuse_write(&d,0x4cu,mask);
        if (!(mask&0x1000ffffu)) refuse_write(&d,8u,mask);
    }
    refuse_write(&d,0x4cu,0x800000u);
    refuse_write(&d,0x4cu,0x800010u);
    refuse_write(&d,0x4cu,0x804000u);
    const uint32_t intervals[]={0u,1u,2u,300000u,UINT32_MAX};
    for (unsigned i=0;i<sizeof intervals/sizeof intervals[0];++i) {
        d=fresh(); CHECK(s5l8920_dsim_clock_write(&d,0x50u,intervals[i]));
        CHECK(s5l8920_dsim_clock_write(&d,0x4cu,ENABLED));
        s5l8920_dsim_clock_t split=d;
        CHECK(s5l8920_dsim_clock_advance(&d,UINT64_MAX));
        uint64_t first=(uint64_t)intervals[i]/2u;
        CHECK(s5l8920_dsim_clock_advance(&split,first));
        CHECK(s5l8920_dsim_clock_advance(&split,(uint64_t)intervals[i]-first));
        CHECK(!memcmp(&d,&split,sizeof d));
        CHECK(d.stable && d.stable_event && !d.remaining);
    }
    for (unsigned p=1u;p<=63u;++p) for (unsigned s=0u;s<8u;++s) {
        const unsigned multipliers[]={1u,343u,1023u};
        for (unsigned i=0;i<3u;++i) {
            d=fresh(); CHECK(s5l8920_dsim_clock_write(&d,0x50u,1u));
            CHECK(s5l8920_dsim_clock_write(&d,0x4cu,0x800000u|(p<<14)|(multipliers[i]<<4)|(s<<1)));
            CHECK(s5l8920_dsim_clock_advance(&d,1u));
            uint64_t numerator=0u; uint32_t denominator=0u;
            CHECK(s5l8920_dsim_clock_rate(&d,&numerator,&denominator));
            CHECK(numerator==(uint64_t)24000000u*multipliers[i]);
            CHECK(denominator==p*(1u<<s));
        }
    }
    uint64_t numerator=9u; uint32_t denominator=8u;
    CHECK(!s5l8920_dsim_clock_rate(NULL,&numerator,&denominator));
    CHECK(!s5l8920_dsim_clock_rate(&d,NULL,&denominator));
    CHECK(!s5l8920_dsim_clock_rate(&d,&numerator,NULL));
    CHECK(numerator==9u && denominator==8u);
    d=(s5l8920_dsim_clock_t){0}; s5l8920_dsim_clock_reset(&d);
    CHECK(!d.configured); refuse_read(&d,0u);
}
int main(void) {
    test_inputs(); test_programming_and_time(); test_bounds();
    printf("DSIM clock: %u passed, %u failed\n",passed,failed);
    return failed?1:0;
}
