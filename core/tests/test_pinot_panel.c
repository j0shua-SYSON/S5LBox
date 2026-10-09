/* Panel power/reset/lifecycle and actual decoded DSIM transport contracts.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "pinot_panel.h"
#include <stdio.h>
#include <string.h>
static unsigned passed,failed;
#define CHECK(x) do { if (x) ++passed; else { if (failed<24u) { \
    printf("FAIL %s:%d %s\n",__func__,__LINE__,#x); } ++failed; } } while (0)
static const pinot_panel_input_t input={{0,0xe5,0x4e,0,0,0,6,7,8,9,10,11,12,13,14},0u,3u,5u,7u,11u,13u};
static pinot_panel_t fresh(void) {
    pinot_panel_t p={0};CHECK(pinot_panel_configure(&p,&input));return p;
}
static pinot_panel_t ready(void) {
    pinot_panel_t p=fresh();CHECK(pinot_panel_power(&p,true));CHECK(pinot_panel_advance(&p,3u));
    CHECK(pinot_panel_reset_pin(&p,true));CHECK(pinot_panel_advance(&p,5u));return p;
}
static s5l8920_dsim_t controller(void) {
    s5l8920_dsim_t d={0};
    const s5l8920_dsim_input_t initial={{24000000u,0x10010fu,0xffffu,0u,UINT32_MAX},3u,2u,5u,7u,11u};
    const s5l8920_dsim_packet_input_t link={3u,4u,5u,2u,5u};
    CHECK(s5l8920_dsim_configure(&d,&initial));CHECK(s5l8920_dsim_configure_packet(&d,&link));
    CHECK(s5l8920_dsim_write(&d,0x50u,0u));CHECK(s5l8920_dsim_write(&d,0x4cu,0x06831572u));
    CHECK(s5l8920_dsim_write(&d,4u,1u));CHECK(s5l8920_dsim_system_clock(&d,3u));
    CHECK(s5l8920_dsim_write(&d,0x10u,0x07807003u));CHECK(s5l8920_dsim_write(&d,8u,0x11180004u));
    CHECK(s5l8920_dsim_write(&d,0x14u,0x80u));CHECK(s5l8920_dsim_write(&d,0x44u,0x1fu));
    CHECK(s5l8920_dsim_write(&d,0xcu,0x00030011u));CHECK(s5l8920_dsim_write(&d,0x2cu,UINT32_MAX));return d;
}
static void send(pinot_panel_t *p,s5l8920_dsim_t *d,uint32_t header) {
    CHECK(s5l8920_dsim_write(d,0x34u,header));CHECK(s5l8920_dsim_escape_clock(d,5u));
    CHECK(pinot_panel_service(p,d));
}
static void refused(pinot_panel_t *p,uint32_t header) {
    pinot_panel_t before=*p;CHECK(!pinot_panel_command(p,header));CHECK(!memcmp(p,&before,sizeof *p));
}
static void test_reset_and_inputs(void) {
    pinot_panel_t p={0},before=p;
    CHECK(!pinot_panel_configure(NULL,&input));CHECK(!pinot_panel_configure(&p,NULL));
    CHECK(!pinot_panel_power(&p,true) && !pinot_panel_reset_pin(&p,true) && !pinot_panel_advance(&p,1u));
    for (unsigned i=0;i<6u;++i) {
        pinot_panel_input_t bad=input;
        switch (i) {
        case 0:bad.channel=4u;break;case 1:bad.reset_low_ns=0u;break;case 2:bad.reset_release_ns=0u;break;
        case 3:bad.sleep_out_ns=0u;break;case 4:bad.sleep_in_ns=0u;break;default:bad.reply_ns=0u;break;
        }
        CHECK(!pinot_panel_configure(&p,&bad) && !memcmp(&p,&before,sizeof p));
    }
    p=fresh();before=p;CHECK(pinot_panel_advance(&p,UINT64_MAX) && !memcmp(&p,&before,sizeof p));
    CHECK(pinot_panel_power(&p,true) && pinot_panel_advance(&p,2u) && pinot_panel_reset_pin(&p,true));
    CHECK(!p.reset_valid);CHECK(pinot_panel_command(&p,0xb114u) && !p.reply_pending);
    CHECK(pinot_panel_advance(&p,UINT64_MAX) && !p.reset_valid);
    CHECK(pinot_panel_reset_pin(&p,false) && pinot_panel_advance(&p,UINT64_MAX) && p.reset_low_elapsed==3u);
    CHECK(pinot_panel_reset_pin(&p,true) && p.reset_valid && p.recovery_remaining==5u);
    before=p;CHECK(pinot_panel_reset_pin(&p,true) && !memcmp(&p,&before,sizeof p));
    CHECK(pinot_panel_command(&p,0xb114u) && !p.reply_pending);
    CHECK(pinot_panel_advance(&p,4u) && p.recovery_remaining==1u);
    CHECK(pinot_panel_advance(&p,1u) && pinot_panel_command(&p,0xb114u) && p.reply_pending);
    before=p;CHECK(pinot_panel_configure(&p,&input) && !memcmp(&p,&before,sizeof p));
    pinot_panel_input_t other=input;other.identity[0]=1u;
    CHECK(!pinot_panel_configure(&p,&other) && !memcmp(&p,&before,sizeof p));
    CHECK(pinot_panel_power(&p,false) && !p.reply_pending && !p.reset_valid && !p.display_on);
    CHECK(pinot_panel_power(&p,true) && pinot_panel_advance(&p,UINT64_MAX) && !p.reset_valid);
    pinot_panel_reset(&p);CHECK(p.configured && !p.powered && !p.reset_high && !p.reset_valid);
}
static void test_sleep_and_passive_commands(void) {
    pinot_panel_t p=ready(),before=p;
    for (unsigned i=0;i<100u;++i) CHECK(pinot_panel_command(&p,5u));
    CHECK(!memcmp(&p,&before,sizeof p));refused(&p,0x2905u);refused(&p,0xab05u);refused(&p,0x010005u);
    CHECK(pinot_panel_command(&p,0x1105u) && p.sleep==PINOT_PANEL_WAKING && p.sleep_remaining==7u);
    CHECK(pinot_panel_advance(&p,6u));before=p;
    CHECK(pinot_panel_command(&p,0x1105u) && !memcmp(&p,&before,sizeof p));refused(&p,0x1005u);
    refused(&p,0x2905u);CHECK(pinot_panel_advance(&p,1u) && p.sleep==PINOT_PANEL_AWAKE);
    CHECK(pinot_panel_command(&p,0x2905u) && p.display_on);refused(&p,0x1005u);
    CHECK(pinot_panel_command(&p,0x2805u) && !p.display_on);
    CHECK(pinot_panel_command(&p,0x1005u) && p.sleep_remaining==11u);refused(&p,0x1105u);
    CHECK(pinot_panel_advance(&p,10u));before=p;
    CHECK(pinot_panel_command(&p,0x1005u) && !memcmp(&p,&before,sizeof p));
    CHECK(pinot_panel_advance(&p,1u) && p.sleep==PINOT_PANEL_ASLEEP);
    CHECK(pinot_panel_command(&p,0xb114u) && p.reply_pending);
    CHECK(pinot_panel_command(&p,0x105u) && !p.reply_pending && p.recovery_remaining==5u);
    for (unsigned total=0u;total<20u;++total) for (unsigned split=0u;split<=total;++split) {
        p=ready();CHECK(pinot_panel_command(&p,0x1105u));CHECK(pinot_panel_command(&p,0xb114u));before=p;
        CHECK(pinot_panel_advance(&p,total));CHECK(pinot_panel_advance(&before,split));
        CHECK(pinot_panel_advance(&before,total-split));CHECK(!memcmp(&p,&before,sizeof p));
    }
}
static void test_link(void) {
    pinot_panel_t p=ready();s5l8920_dsim_t d=controller();
    send(&p,&d,0xb114u);CHECK(p.reply_pending && !d.packet.tx_count && !d.packet.rx_count);
    pinot_panel_t before=p;s5l8920_dsim_t previous=d;
    for (unsigned i=0;i<100u;++i) CHECK(pinot_panel_service(&p,&d));
    CHECK(!memcmp(&p,&before,sizeof p) && !memcmp(&d,&previous,sizeof d));
    CHECK(s5l8920_dsim_escape_clock(&d,2u) && pinot_panel_service(&p,&d));
    CHECK(d.packet.bus==S5L8920_DSIM_BUS_RECEIVE && !d.packet.rx_count);
    CHECK(pinot_panel_advance(&p,12u) && pinot_panel_service(&p,&d) && !d.packet.rx_count);
    CHECK(pinot_panel_advance(&p,1u) && pinot_panel_service(&p,&d));
    CHECK(d.packet.rx_count==5u && !p.reply_pending && d.events==0x02040000u);
    uint32_t value=0u;CHECK(s5l8920_dsim_read(&d,0x3cu,&value) && value==0xf1au);
    for (unsigned at=0;at<15u;at+=4u) {
        uint32_t expected=0u;
        for (unsigned j=0;j<4u && at+j<15u;++j) expected|=(uint32_t)input.identity[at+j]<<(8u*j);
        CHECK(s5l8920_dsim_read(&d,0x3cu,&value) && value==expected);
    }
    /* Unsupported command must not partially consume the physical TX queue. */
    CHECK(s5l8920_dsim_write(&d,0x34u,0xab05u) && s5l8920_dsim_escape_clock(&d,5u));
    before=p;previous=d;CHECK(!pinot_panel_service(&p,&d) && !memcmp(&p,&before,sizeof p) && !memcmp(&d,&previous,sizeof d));
    for (unsigned mode=0;mode<4u;++mode) {
        p=ready();d=controller();
        if (!mode) CHECK(pinot_panel_power(&p,false));
        send(&p,&d,mode==1u?0xb154u:0xb114u); /* Other virtual channel. */
        if (mode==2u) CHECK(pinot_panel_reset_pin(&p,false));
        CHECK(s5l8920_dsim_escape_clock(&d,2u) && pinot_panel_service(&p,&d));
        CHECK(s5l8920_dsim_escape_clock(&d,UINT64_MAX) && pinot_panel_service(&p,&d));
        CHECK(!p.reply_pending && !d.packet.rx_count && d.packet.bus==S5L8920_DSIM_BUS_IDLE);
        CHECK(d.events==(mode==3u?0x200000u:0x100000u));
    }
    /* A full FIFO backpressures a second response without losing it. */
    p=ready();d=controller();send(&p,&d,0xb114u);
    CHECK(s5l8920_dsim_escape_clock(&d,2u) && pinot_panel_advance(&p,13u) && pinot_panel_service(&p,&d));
    send(&p,&d,0xb114u);CHECK(s5l8920_dsim_escape_clock(&d,2u) && pinot_panel_advance(&p,13u) && pinot_panel_service(&p,&d));
    CHECK(p.reply_pending && d.packet.rx_count==5u);
    for (unsigned i=0;i<5u;++i) CHECK(s5l8920_dsim_read(&d,0x3cu,&value));
    CHECK(pinot_panel_service(&p,&d) && !p.reply_pending && d.packet.rx_count==5u);
}
int main(void) {
    test_reset_and_inputs();test_sleep_and_passive_commands();test_link();
    printf("Pinot panel: %u passed, %u failed\n",passed,failed);return failed?1:0;
}
