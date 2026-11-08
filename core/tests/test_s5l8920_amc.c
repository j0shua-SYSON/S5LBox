/* AMC software control and refusal tests; no proprietary firmware required.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_amc.h"
#include <stdio.h>
#include <string.h>
static unsigned passed,failed;
#define CHECK(x) do { if (x) ++passed; else { if (failed<24u) { \
    printf("FAIL %s:%d %s\n",__func__,__LINE__,#x); } ++failed; } } while (0)
static const s5l8920_amc_control_input_t input={0xa5a51234u,0x5a5a5a5au,{0xc1234567u,0x89abcdefu}};
static uint32_t get(const s5l8920_amc_control_t *d,uint32_t at) {
    uint32_t value=0xccccccccu;CHECK(s5l8920_amc_control_read(d,at,4u,&value));return value;
}
static void put(s5l8920_amc_control_t *d,uint32_t at,uint32_t value) {
    CHECK(s5l8920_amc_control_write(d,at,4u,value));
}
static void refuse_read(s5l8920_amc_control_t *d,uint32_t at,unsigned size) {
    s5l8920_amc_control_t before=*d;uint32_t value=0xdeadbeefu;
    CHECK(!s5l8920_amc_control_read(d,at,size,&value));
    CHECK(value==0xdeadbeefu && !memcmp(&before,d,sizeof before));
}
static void refuse_write(s5l8920_amc_control_t *d,uint32_t at,unsigned size,uint32_t value) {
    s5l8920_amc_control_t before=*d;CHECK(!s5l8920_amc_control_write(d,at,size,value));
    CHECK(!memcmp(&before,d,sizeof before));
}
int main(void) {
    s5l8920_amc_control_t d={0},other={0};uint32_t value=0xdeadbeefu;
    refuse_read(&d,0x988u,4u);refuse_write(&d,0x990u,4u,1u);
    CHECK(!s5l8920_amc_control_configure(NULL,&input));CHECK(!s5l8920_amc_control_configure(&d,NULL));
    CHECK(!s5l8920_amc_control_read(NULL,0x988u,4u,&value) && value==0xdeadbeefu);
    CHECK(!s5l8920_amc_control_write(NULL,0x990u,4u,1u));s5l8920_amc_control_reset(NULL);
    s5l8920_amc_control_reset(&d);CHECK(!d.configured);
    CHECK(s5l8920_amc_control_configure(&d,&input));CHECK(s5l8920_amc_control_configure(&other,&input));
    CHECK(get(&d,0x988u)==input.start_address && get(&d,0x998u)==input.start_status);
    CHECK(get(&d,0xa94u)==input.interrupt_mask[0] && get(&d,0xb14u)==input.interrupt_mask[1]);
    CHECK(!s5l8920_amc_control_read(&d,0x988u,4u,NULL));
    put(&d,0x988u,0x100u);CHECK(get(&d,0x988u)==0xa5a50100u);
    put(&d,0x988u,0xf00u);CHECK(get(&d,0x988u)==0xa5a50f00u);
    put(&d,0x988u,0xffffu);CHECK(get(&d,0x988u)==0xa5a5ffffu);
    put(&d,0x988u,0u);CHECK(get(&d,0x988u)==0xa5a50000u);
    put(&d,0x994u,1u);CHECK(get(&d,0x998u)==0x5a5a5a5au);
    put(&d,0x990u,1u);put(&d,0x990u,1u);CHECK(get(&d,0x998u)==0x5a5a5a5bu);
    put(&d,0x994u,1u);put(&d,0x994u,1u);CHECK(get(&d,0x998u)==0x5a5a5a5au);
    for (unsigned bank=0;bank<2u;++bank) {
        uint32_t set=bank?0xb0cu:0xa8cu,clear=set+4u,read=set+8u;
        uint32_t supported=bank?UINT32_MAX:0x3fffffffu,padding=input.interrupt_mask[bank]&~supported;
        put(&d,clear,supported);CHECK(get(&d,read)==padding);
        for (unsigned bit=0;bit<(bank?32u:30u);++bit) {
            uint32_t mask=UINT32_C(1)<<bit;
            put(&d,set,mask);put(&d,set,mask);CHECK(get(&d,read)==(padding|mask));
            put(&d,clear,0u);CHECK(get(&d,read)==(padding|mask));
            put(&d,clear,mask);put(&d,clear,mask);CHECK(get(&d,read)==padding);
        }
        put(&d,set,supported);put(&d,clear,0x15555555u&supported);
        CHECK(get(&d,read)==(padding|(supported&~0x15555555u)));
        put(&d,set,0u);CHECK(get(&d,read)==(padding|(supported&~0x15555555u)));
    }
    CHECK(!memcmp(&other.current,&input,sizeof input));
    s5l8920_amc_control_t before=d;
    CHECK(s5l8920_amc_control_configure(&d,&input) && !memcmp(&before,&d,sizeof d));
    for (unsigned n=0;n<4u;++n) {
        s5l8920_amc_control_input_t conflict=input;
        if (!n) conflict.start_address^=1u;else if (n==1u) conflict.start_status^=1u;
        else conflict.interrupt_mask[n-2u]^=1u;
        CHECK(!s5l8920_amc_control_configure(&d,&conflict) && !memcmp(&before,&d,sizeof d));
    }
    const uint32_t offsets[]={0x988u,0x990u,0x994u,0x998u,0xa8cu,0xa90u,0xa94u,0xb0cu,0xb10u,0xb14u};
    for (unsigned n=0;n<sizeof offsets/sizeof offsets[0];++n) {
        for (unsigned size=0;size<=8u;++size)if (size!=4u) {
            refuse_read(&d,offsets[n],size);refuse_write(&d,offsets[n],size,1u);
        }
        for (unsigned delta=1;delta<4u;++delta) {
            refuse_read(&d,offsets[n]+delta,4u);refuse_write(&d,offsets[n]+delta,4u,1u);
        }
    }
    const uint32_t unknown[]={0u,0x980u,0x984u,0x98cu,0x99cu,0xa44u,0xa48u,0xa98u,0xb18u,0xc48u,0xc80u,0x4000u,UINT32_MAX};
    for (unsigned n=0;n<sizeof unknown/sizeof unknown[0];++n) {
        refuse_read(&d,unknown[n],4u);refuse_write(&d,unknown[n],4u,1u);
    }
    refuse_read(&d,0x990u,4u);refuse_read(&d,0x994u,4u);refuse_read(&d,0xa8cu,4u);
    refuse_read(&d,0xa90u,4u);refuse_read(&d,0xb0cu,4u);refuse_read(&d,0xb10u,4u);
    refuse_write(&d,0x998u,4u,1u);refuse_write(&d,0xa94u,4u,1u);refuse_write(&d,0xb14u,4u,1u);
    refuse_write(&d,0x988u,4u,0x10000u);refuse_write(&d,0xa8cu,4u,0x40000000u);
    refuse_write(&d,0xa90u,4u,0x80000001u);refuse_write(&d,0x990u,4u,0u);
    refuse_write(&d,0x994u,4u,3u);
    CHECK(!memcmp(&before,&d,sizeof d));s5l8920_amc_control_reset(&d);
    CHECK(d.configured && !memcmp(&d.current,&input,sizeof input));
    CHECK(!memcmp(&d.initial,&input,sizeof input));
    printf("AMC control: %u passed, %u failed\n",passed,failed);return failed?1:0;
}
