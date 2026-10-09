/* DSIM reset, clock gating and lane handshake tests; no firmware image.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_dsim.h"
#include <stdio.h>
#include <string.h>
static unsigned passed,failed;
#define CHECK(x) do { if (x) ++passed; else { if (failed<24u) { \
    printf("FAIL %s:%d %s\n",__func__,__LINE__,#x); } ++failed; } } while (0)
static const s5l8920_dsim_input_t input={{24000000u,0x0010010fu,0xffffu,0u,UINT32_MAX},3u,2u,5u,7u,11u};
static s5l8920_dsim_t fresh(void) {
    s5l8920_dsim_t d;memset(&d,0,sizeof d);
    CHECK(s5l8920_dsim_configure(&d,&input));return d;
}
static void put(s5l8920_dsim_t *d,uint32_t offset,uint32_t value) {
    CHECK(s5l8920_dsim_write(d,offset,value));
}
static uint32_t get(const s5l8920_dsim_t *d,uint32_t offset) {
    uint32_t value=0x5a5a5a5au;CHECK(s5l8920_dsim_read(d,offset,&value));return value;
}
static void refuse_read(s5l8920_dsim_t *d,uint32_t offset) {
    uint32_t value=0xa5a5a5a5u;s5l8920_dsim_t before=*d;
    CHECK(!s5l8920_dsim_read(d,offset,&value));
    CHECK(value==0xa5a5a5a5u && !memcmp(&before,d,sizeof before));
}
static void refuse_write(s5l8920_dsim_t *d,uint32_t offset,uint32_t value) {
    s5l8920_dsim_t before=*d;CHECK(!s5l8920_dsim_write(d,offset,value));
    CHECK(!memcmp(&before,d,sizeof before));
}
static s5l8920_dsim_t ready(void) {
    s5l8920_dsim_t d=fresh();
    put(&d,0x50u,4u);put(&d,0x4cu,0x06831572u);
    CHECK(s5l8920_dsim_system_clock(&d,4u));
    put(&d,4u,1u);CHECK(s5l8920_dsim_system_clock(&d,3u));
    put(&d,0x10u,0x07807027u);put(&d,8u,0x11380004u);put(&d,0x14u,0x80u);
    CHECK(get(&d,0u)==0x8010010fu);return d;
}
static void test_inputs_and_unknowns(void) {
    s5l8920_dsim_t d={0},before=d;
    refuse_read(&d,0u);refuse_write(&d,4u,1u);
    CHECK(!s5l8920_dsim_system_clock(&d,1u));CHECK(!s5l8920_dsim_phy_clock(&d,1u));
    CHECK(!s5l8920_dsim_configure(NULL,&input));CHECK(!s5l8920_dsim_configure(&d,NULL));
    CHECK(!s5l8920_dsim_read(NULL,0u,NULL));CHECK(!s5l8920_dsim_write(NULL,0u,0u));
    CHECK(!s5l8920_dsim_system_clock(NULL,1u));CHECK(!s5l8920_dsim_phy_clock(NULL,1u));
    for (unsigned i=0;i<8u;++i) {
        s5l8920_dsim_input_t bad=input;
        switch (i) {
        case 0:bad.reset_cycles=0u;break;case 1:bad.stop_cycles=0u;break;
        case 2:bad.entry_cycles=0u;break;case 3:bad.exit_cycles=0u;break;
        case 4:bad.wakeup_cycles=0u;break;case 5:bad.clock.idle_status&=~0x100u;break;
        case 6:bad.clock.idle_status&=~0x100000u;break;
        default:bad.clock.reference_hz=0u;break;
        }
        CHECK(!s5l8920_dsim_configure(&d,&bad));CHECK(!memcmp(&before,&d,sizeof d));
    }
    d=fresh();before=d;
    CHECK(s5l8920_dsim_configure(&d,&input));CHECK(!memcmp(&before,&d,sizeof d));
    s5l8920_dsim_input_t conflict=input;++conflict.entry_cycles;
    CHECK(!s5l8920_dsim_configure(&d,&conflict));CHECK(!memcmp(&before,&d,sizeof d));
    CHECK(!s5l8920_dsim_read(&d,0u,NULL));
    CHECK(get(&d,0u)==input.clock.idle_status && get(&d,8u)==0xffffu);
    CHECK(get(&d,4u)==0u);
    const uint32_t unknown[]={0xcu,0x10u,0x14u,0x18u,0x1cu,0x20u,0x24u,0x28u,
        0x2cu,0x30u,0x34u,0x38u,0x3cu,0x40u,0x44u,0x48u,0x54u,0x58u,0x6cu,0x7cu};
    for (unsigned i=0;i<sizeof unknown/sizeof unknown[0];++i)refuse_read(&d,unknown[i]);
    put(&d,0x2cu,UINT32_MAX);refuse_read(&d,0x2cu);
    put(&d,0x44u,0x1fu);refuse_read(&d,0x44u);
    put(&d,0x44u,0u);CHECK(get(&d,0x44u)==0x01555500u);
    put(&d,0x44u,0x1fu);CHECK(get(&d,0x44u)==0x0155551fu);
    s5l8920_dsim_reset(NULL);
}
static void test_reset_domains(void) {
    s5l8920_dsim_t d=fresh();put(&d,0x18u,0x01e00140u);put(&d,4u,1u);
    CHECK(d.reset_pending && !d.reset_released && d.reset_remaining==3u);
    refuse_read(&d,0x18u);CHECK(get(&d,4u)==1u);refuse_write(&d,0x18u,0u);refuse_write(&d,4u,1u);
    s5l8920_dsim_t before=d;
    for (unsigned i=0;i<1000u;++i)CHECK(!(get(&d,0u)&0x100000u));
    CHECK(!memcmp(&before,&d,sizeof d));
    CHECK(s5l8920_dsim_system_clock(&d,UINT64_MAX));
    CHECK(s5l8920_dsim_phy_clock(&d,UINT64_MAX));
    CHECK(d.reset_pending && d.reset_remaining==3u);
    put(&d,0x50u,4u);put(&d,0x4cu,0x06831572u);
    s5l8920_dsim_t split=d;
    CHECK(s5l8920_dsim_system_clock(&d,6u));CHECK(d.reset_remaining==1u && d.clock.stable);
    CHECK(s5l8920_dsim_system_clock(&d,1u));
    CHECK(s5l8920_dsim_system_clock(&split,1u));CHECK(s5l8920_dsim_system_clock(&split,6u));
    CHECK(!memcmp(&d,&split,sizeof d));
    CHECK(!d.reset_pending && d.reset_released && get(&d,0x2cu)==0xc0000000u);
    CHECK(get(&d,4u)==0u);
    put(&d,0x2cu,0x40000000u);CHECK(get(&d,0x2cu)==0x80000000u);
    CHECK(s5l8920_dsim_system_clock(&d,UINT64_MAX));
    put(&d,0x2cu,UINT32_MAX);CHECK(get(&d,0x2cu)==0u);
    put(&d,0x18u,0x01e00140u);put(&d,0x14u,0x001000c0u);put(&d,0x44u,0x1fu);
    put(&d,4u,0x10000u);CHECK(get(&d,0x18u)==0x01e00140u && get(&d,0x14u)==0x001000c0u);
    CHECK(get(&d,0x44u)==0x0155551fu);CHECK(s5l8920_dsim_system_clock(&d,3u));
    CHECK(get(&d,0x2cu)==0x40000000u);
    put(&d,4u,1u);refuse_read(&d,0x18u);refuse_read(&d,0x44u);
    CHECK(get(&d,0x4cu)==0x06831572u);refuse_read(&d,0x50u);
    CHECK(s5l8920_dsim_system_clock(&d,3u));
    put(&d,0x44u,0x1fu);CHECK(get(&d,0x44u)==0x0155551fu);refuse_read(&d,0x3cu);
    s5l8920_dsim_reset(&d);before=fresh();CHECK(!memcmp(&d,&before,sizeof d));
}
static void test_ulps_sequence(void) {
    s5l8920_dsim_t d=ready();put(&d,0x14u,0x8au);
    CHECK((get(&d,0u)&0x333u)==0x103u);
    CHECK(s5l8920_dsim_system_clock(&d,UINT64_MAX));CHECK((get(&d,0u)&0x333u)==0x103u);
    CHECK(s5l8920_dsim_phy_clock(&d,4u));CHECK((get(&d,0u)&0x333u)==0u);
    s5l8920_dsim_t before=d;
    put(&d,0x14u,0x8au);CHECK(!memcmp(&before,&d,sizeof d));
    refuse_write(&d,0x14u,0x8fu);refuse_write(&d,0x10u,0x07807003u);refuse_write(&d,4u,1u);
    CHECK(s5l8920_dsim_phy_clock(&d,1u));CHECK((get(&d,0u)&0x333u)==0x230u);
    put(&d,0x14u,0x8fu);CHECK(s5l8920_dsim_phy_clock(&d,6u));
    CHECK((get(&d,0u)&0x333u)==0x230u);
    CHECK(s5l8920_dsim_phy_clock(&d,1u));CHECK((get(&d,0u)&0x333u)==0u);
    put(&d,0x14u,0x8au);CHECK(s5l8920_dsim_phy_clock(&d,UINT64_MAX));
    CHECK((get(&d,0u)&0x333u)==0u); /* Request bits still held, no re-entry. */
    put(&d,0x14u,0x80u);CHECK(s5l8920_dsim_phy_clock(&d,1u));
    CHECK((get(&d,0u)&0x333u)==0u);CHECK(s5l8920_dsim_phy_clock(&d,1u));
    CHECK((get(&d,0u)&0x333u)==0x103u);
    d=ready();put(&d,0x14u,0x8au);CHECK(s5l8920_dsim_phy_clock(&d,5u));
    put(&d,0x10u,0x07807001u);put(&d,0x10u,0x07807003u);put(&d,8u,0x11180004u);
    CHECK((get(&d,0u)&0x333u)==0x230u); /* Temporary gating retains ULPS. */
    put(&d,0x14u,0x8fu);CHECK(s5l8920_dsim_phy_clock(&d,7u));
    CHECK((get(&d,0u)&0x333u)==0x20u);
    put(&d,0x14u,0x8au);CHECK(s5l8920_dsim_phy_clock(&d,11u));put(&d,0x14u,0x80u);
    CHECK(s5l8920_dsim_phy_clock(&d,2u));CHECK((get(&d,0u)&0x333u)==0x121u);
    put(&d,4u,1u);CHECK(s5l8920_dsim_system_clock(&d,3u)); /* Inactive ULPS lane does not block reset. */
}
static void test_gates_and_partition(void) {
    const uint32_t gates[]={0x10000000u,0x01000000u,0x00080000u,0x00100000u,0x00200000u,4u};
    for (unsigned i=0;i<6u;++i) {
        s5l8920_dsim_t d=ready();put(&d,8u,0x11380004u&~gates[i]);put(&d,0x14u,0x8au);
        CHECK(s5l8920_dsim_phy_clock(&d,UINT64_MAX));
        uint32_t expected=i==2u?0x130u:i==3u?0x221u:i==4u?0x212u:0x103u;
        CHECK((get(&d,0u)&0x333u)==expected);
        put(&d,8u,0x11380004u);CHECK(s5l8920_dsim_phy_clock(&d,5u));
        CHECK((get(&d,0u)&0x333u)==0x230u);
    }
    for (uint64_t count=0;count<24u;++count) for (uint64_t first=0;first<=count;++first) {
        s5l8920_dsim_t whole=ready();put(&whole,0x14u,0x8au);
        CHECK(s5l8920_dsim_phy_clock(&whole,5u));put(&whole,0x14u,0x8fu);
        s5l8920_dsim_t split=whole;
        CHECK(s5l8920_dsim_phy_clock(&whole,count));
        CHECK(s5l8920_dsim_phy_clock(&split,first));CHECK(s5l8920_dsim_phy_clock(&split,count-first));
        CHECK(!memcmp(&whole,&split,sizeof whole));
    }
    s5l8920_dsim_t d=ready();put(&d,0x14u,0x8au);CHECK(s5l8920_dsim_phy_clock(&d,5u));
    put(&d,0x14u,0x00100080u);CHECK(s5l8920_dsim_phy_clock(&d,1u));
    CHECK((get(&d,0u)&0x333u)==0u);CHECK(s5l8920_dsim_phy_clock(&d,1u));
    CHECK((get(&d,0u)&0x333u)==0x103u);
}
static void test_refusals(void) {
    s5l8920_dsim_t d=ready();
    const uint32_t offsets[]={0u,1u,2u,3u,5u,6u,7u,9u,0x30u,0x34u,0x38u,0x3cu,0x48u,0x54u,0x58u,0x6cu,0x7cu,0x1000u,UINT32_MAX};
    for (unsigned i=0;i<sizeof offsets/sizeof offsets[0];++i)refuse_write(&d,offsets[i],0u);
    for (uint32_t off=0x51u;off<0x100u;++off)refuse_read(&d,off);
    refuse_write(&d,4u,0u);refuse_write(&d,4u,0x10001u);
    refuse_write(&d,8u,0x91380004u);refuse_write(&d,8u,0x19380004u);
    refuse_write(&d,0x10u,0x07807047u);refuse_write(&d,0x10u,0x0780703fu);
    refuse_write(&d,0x14u,0x10000u);refuse_write(&d,0x14u,0x10u);
    refuse_write(&d,0x18u,0x81e00140u);refuse_write(&d,0x40u,0x200u);
    refuse_write(&d,0x44u,0x20u);refuse_read(&d,0x50u);CHECK(get(&d,4u)==0u);
    d=(s5l8920_dsim_t){0};s5l8920_dsim_reset(&d);CHECK(!d.configured);
}
static void test_snapshot_inputs(void) {
    s5l8920_dsim_t d={0},before=d;s5l8920_dsim_readback_input_t r={0};
    CHECK(!s5l8920_dsim_configure_readback(NULL,&r));
    CHECK(!s5l8920_dsim_configure_readback(&d,&r));CHECK(!memcmp(&d,&before,sizeof d));
    d=fresh();CHECK(!s5l8920_dsim_configure_readback(&d,NULL));
    const unsigned passive[]={0x0cu,0x30u,0x34u,0x38u,0x3cu,0x48u,0x54u,0x58u,
        0x5cu,0x60u,0x64u,0x68u,0x6cu,0x70u,0x74u,0x78u,0x7cu};
    uint32_t allowed=0u;
    for (unsigned i=0;i<sizeof passive/sizeof passive[0];++i) allowed|=UINT32_C(1)<<(passive[i]/4u);
    for (unsigned i=0;i<32u;++i) {
        d=fresh();before=d;r=(s5l8920_dsim_readback_input_t){0};r.known=UINT32_C(1)<<i;
        if (r.known&allowed) CHECK(s5l8920_dsim_configure_readback(&d,&r));
        else {
            CHECK(!s5l8920_dsim_configure_readback(&d,&r));CHECK(!memcmp(&d,&before,sizeof d));
        }
        d=fresh();before=d;r.known=0u;r.word[i]=1u;
        CHECK(!s5l8920_dsim_configure_readback(&d,&r));CHECK(!memcmp(&d,&before,sizeof d));
    }
    for (unsigned i=0;i<5u;++i) {
        r=(s5l8920_dsim_readback_input_t){0};r.known=allowed;d=fresh();before=d;
        if (i<2u) r.timer=(s5l8920_dsim_timer_readback_t)(i?99:-1);
        else if (i==2u) r.word[3]=0x01000000u;
        else r.word[18]=i==3u?0x80u:0x8000u;
        CHECK(!s5l8920_dsim_configure_readback(&d,&r));CHECK(!memcmp(&d,&before,sizeof d));
    }
    r=(s5l8920_dsim_readback_input_t){0};r.known=allowed;
    for (unsigned i=0;i<sizeof passive/sizeof passive[0];++i) r.word[passive[i]/4u]=0x12340000u|passive[i];
    r.word[3]=0x00abcdefu;r.word[18]=0x4040u;
    d=fresh();CHECK(s5l8920_dsim_configure_readback(&d,&r));
    before=d;CHECK(s5l8920_dsim_configure_readback(&d,&r));CHECK(!memcmp(&before,&d,sizeof d));
    for (unsigned i=0;i<sizeof passive/sizeof passive[0];++i) {
        unsigned off=passive[i];
        if (off==0x3cu) refuse_read(&d,off);
        else CHECK(get(&d,off)==r.word[off/4u]);
        if (off!=0x0cu) refuse_write(&d,off,r.word[off/4u]);
    }
    put(&d,0x0cu,0x10203u);put(&d,4u,0x10000u);
    CHECK(get(&d,4u)==0x10000u && get(&d,0x0cu)==0x10203u);
    CHECK(get(&d,0x3cu)==r.word[15]);before=d;
    for (unsigned i=0;i<1000u;++i) CHECK(get(&d,4u)==0x10000u);
    CHECK(!memcmp(&d,&before,sizeof d));
    CHECK(s5l8920_dsim_system_clock(&d,UINT64_MAX) && get(&d,4u)==0x10000u);
    put(&d,0x50u,4u);put(&d,0x4cu,0x06831572u);
    CHECK(s5l8920_dsim_system_clock(&d,6u) && get(&d,4u)==0x10000u);
    CHECK(s5l8920_dsim_system_clock(&d,1u) && get(&d,4u)==0u);
    put(&d,4u,1u);CHECK(get(&d,0x0cu)==r.word[3]);
    CHECK(get(&d,0x54u)==r.word[21]);CHECK(s5l8920_dsim_system_clock(&d,3u));
    before=d;CHECK(s5l8920_dsim_configure_readback(&d,&r));CHECK(!memcmp(&before,&d,sizeof d));
    s5l8920_dsim_readback_input_t conflict=r;conflict.word[21]^=1u;
    CHECK(!s5l8920_dsim_configure_readback(&d,&conflict));CHECK(!memcmp(&before,&d,sizeof d));
    s5l8920_dsim_reset(&d);CHECK(get(&d,4u)==0u && get(&d,0x0cu)==r.word[3] && get(&d,0x7cu)==r.word[31]);
    refuse_read(&d,0x3cu);
    const uint32_t writes[][2]={{0x4cu,0u},{0x50u,1u},{8u,0u},{0x2cu,0u},{4u,1u},{0x0cu,1u}};
    for (unsigned i=0;i<sizeof writes/sizeof writes[0];++i) {
        d=fresh();put(&d,writes[i][0],writes[i][1]);before=d;
        CHECK(!s5l8920_dsim_configure_readback(&d,&r));CHECK(!memcmp(&before,&d,sizeof d));
    }
}
static void test_timer_readback_choices(void) {
    for (unsigned mode=0;mode<3u;++mode) for (uint32_t timer=0;timer<7u;++timer) {
        s5l8920_dsim_t d=fresh();s5l8920_dsim_readback_input_t r={0};
        r.timer=(s5l8920_dsim_timer_readback_t)mode;CHECK(s5l8920_dsim_configure_readback(&d,&r));
        CHECK(get(&d,0x50u)==UINT32_MAX);put(&d,0x50u,timer);CHECK(get(&d,0x50u)==timer);
        put(&d,0x4cu,0x06831572u);
        for (uint32_t elapsed=0;elapsed<=timer+1u;++elapsed) {
            s5l8920_dsim_t before=d;
            if (!mode) refuse_read(&d,0x50u);
            else CHECK(get(&d,0x50u)==(mode==1u?timer:(elapsed<timer?timer-elapsed:0u)));
            CHECK(!memcmp(&d,&before,sizeof d));CHECK(s5l8920_dsim_system_clock(&d,1u));
        }
        put(&d,0x4cu,0x06031572u);
        if (!mode) refuse_read(&d,0x50u);
        else CHECK(get(&d,0x50u)==(mode==1u?timer:0u));
        put(&d,0x50u,19u);CHECK(get(&d,0x50u)==19u);
        s5l8920_dsim_reset(&d);CHECK(get(&d,0x50u)==UINT32_MAX && d.readback_initial.timer==r.timer);
    }
}
int main(void) {
    test_inputs_and_unknowns();test_reset_domains();test_ulps_sequence();
    test_gates_and_partition();test_refusals();test_snapshot_inputs();test_timer_readback_choices();
    printf("DSIM control: %u passed, %u failed\n",passed,failed);return failed?1:0;
}
