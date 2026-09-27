/* Polled UART register, FIFO and explicit source-clock contracts.
 * No firmware bytes, inferred board frequency or host-wall-clock sleeps.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_uart.h"
#include <stdio.h>
#include <string.h>

static unsigned passed, failed;
#define CHECK(c,msg) do { if (c) passed++; else { failed++; printf("FAIL %s:%d %s\n",__func__,__LINE__,msg); } } while (0)
static uint32_t read_reg(s5l8920_uart_t *u,uint32_t offset) {
    uint32_t value=0xdeadbeefu;
    CHECK(s5l8920_uart_read(u,offset,&value),"register read");
    return value;
}
static void configure(s5l8920_uart_t *u,uint32_t control,uint32_t divisor) {
    s5l8920_uart_reset(u);
    /* Original N88 initializer's ordered writes, for the default8N1 path. */
    CHECK(s5l8920_uart_write(u,0u,3u),"8N1");
    CHECK(s5l8920_uart_write(u,4u,control),"polled channels and selected source");
    CHECK(s5l8920_uart_write(u,12u,0u),"manual RTS clear");
    CHECK(s5l8920_uart_write(u,40u,divisor),"baud divisor");
    CHECK(s5l8920_uart_write(u,8u,3u),"FIFO enable and RX reset");
    CHECK(s5l8920_uart_write(u,12u,1u),"manual RTS set");
}
static void rejected_write(s5l8920_uart_t *u,uint32_t offset,uint32_t value) {
    s5l8920_uart_t before=*u;
    CHECK(!s5l8920_uart_write(u,offset,value) && !memcmp(u,&before,sizeof before),"refused write changed state");
}
static void rejected_read(s5l8920_uart_t *u,uint32_t offset) {
    s5l8920_uart_t before=*u; uint32_t value=0x12345678u;
    CHECK(!s5l8920_uart_read(u,offset,&value) && value==0x12345678u && !memcmp(u,&before,sizeof before),"refused read changed state/output");
}
static void test_configuration(void) {
    s5l8920_uart_t u;
    memset(&u,0xff,sizeof u); s5l8920_uart_reset(&u);
    const uint32_t configs[]={0u,4u,8u,12u,40u};
    for (unsigned n=0;n<5u;n++) rejected_read(&u,configs[n]);
    rejected_read(&u,16u); rejected_read(&u,24u); rejected_read(&u,36u);
    rejected_write(&u,32u,65u);
    CHECK(!s5l8920_uart_receive(&u,65u),"unconfigured receive accepted");
    CHECK(s5l8920_uart_write(&u,0u,3u) && read_reg(&u,0u)==3u,"programmed line readable alone");
    rejected_read(&u,4u); rejected_read(&u,16u);
    configure(&u,0x405u,0x80019u);
    const uint32_t expected[]={3u,0x405u,1u,1u,0x80019u};
    for (unsigned n=0;n<5u;n++) CHECK(read_reg(&u,configs[n])==expected[n],"configuration readback/reset commands self-clear");
    CHECK(read_reg(&u,16u)==6u && read_reg(&u,24u)==0u,"functional empty UART status");
    for (uint32_t off=0;off<0x1000u;off++) {
        bool config=off==0u || off==4u || off==8u || off==12u || off==40u;
        bool readable=config || off==16u || off==24u;
        if (!readable) rejected_read(&u,off);
        if (!config && off!=16u && off!=32u) rejected_write(&u,off,0u);
    }
    const uint32_t bad[][2]={{0u,2u},{0u,7u},{0u,0x23u},{4u,2u},{4u,3u},{4u,8u},{4u,12u},
        {4u,0x1005u},{4u,0x2405u},{4u,0x485u},{8u,0x1c1u},{8u,8u},{12u,0x10u},
        {40u,0x90000u},{40u,0x100000u},{16u,1u},{16u,4u},{16u,0x40u},{32u,0x100u}};
    for (unsigned n=0;n<sizeof bad/sizeof bad[0];n++) rejected_write(&u,bad[n][0],bad[n][1]);
    for (unsigned field=0;field<=8u;field++) {
        CHECK(s5l8920_uart_write(&u,40u,(field<<16)|0xffffu),"supported sample rate/divider limit");
        CHECK(read_reg(&u,40u)==((field<<16)|0xffffu),"untruncated sample field");
    }
    s5l8920_uart_reset(&u);
    rejected_read(&u,0u); rejected_read(&u,16u);
    CHECK(!u.tx_busy && !u.rx_count && !u.tx_count && !u.pending,"reset retained traffic");
    s5l8920_uart_reset(NULL);
    CHECK(!s5l8920_uart_read(NULL,0u,NULL) && !s5l8920_uart_read(&u,0u,NULL) &&
          !s5l8920_uart_write(NULL,0u,3u) && !s5l8920_uart_receive(NULL,0u),"null access accepted");
}
static void test_clock_and_frame_boundaries(void) {
    for (unsigned nclk=0;nclk<2u;nclk++)
     for (unsigned field=0;field<=8u;field++) {
        s5l8920_uart_t u;
        uint32_t div=field%2u?0xffffu:25u;
        uint32_t cycles=10u*(div+1u)*(16u-field);
        configure(&u,5u|(nclk?0x400u:0u),(field<<16)|div);
        uint8_t out[20]; memset(out,0xa5,sizeof out); size_t count=999u;
        CHECK(s5l8920_uart_clock(&u,nclk!=0u,UINT64_MAX,out,sizeof out,&count) && count==0u,"idle clock credit");
        CHECK(s5l8920_uart_write(&u,32u,0xc3u),"write first frame");
        CHECK(read_reg(&u,16u)==0x22u && read_reg(&u,24u)==0u,"active shifter falsely reported empty/FIFO occupied");
        s5l8920_uart_t before=u;
        CHECK(s5l8920_uart_clock(&u,nclk==0u,UINT64_MAX,out,sizeof out,&count) && count==0u && !memcmp(&u,&before,sizeof u),"wrong source advanced transmitter");
        CHECK(s5l8920_uart_clock(&u,nclk!=0u,cycles-1u,out,sizeof out,&count) && count==0u && out[0]==0xa5u && !(read_reg(&u,16u)&4u),"frame completed early");
        CHECK(s5l8920_uart_clock(&u,nclk!=0u,1u,out,sizeof out,&count) && count==1u && out[0]==0xc3u && out[1]==0xa5u && read_reg(&u,16u)==0x26u,"final stop bit or output boundary");
        CHECK(s5l8920_uart_write(&u,16u,0x20u) && read_reg(&u,16u)==6u,"W1C changed live bits");
        CHECK(s5l8920_uart_write(&u,32u,0x5au),"next frame");
        CHECK(s5l8920_uart_clock(&u,nclk!=0u,0u,out,sizeof out,&count) && count==0u && u.tx_remaining==cycles,"future byte consumed idle credit");
        before=u; count=888u; out[0]=0xa5u;
        CHECK(!s5l8920_uart_clock(&u,nclk!=0u,cycles,out,0u,&count) && count==888u && out[0]==0xa5u && !memcmp(&u,&before,sizeof u),"output capacity failure was not atomic");
        CHECK(s5l8920_uart_clock(&u,nclk!=0u,cycles,out,1u,&count) && count==1u && out[0]==0x5au,"capacity retry lost byte/time");
     }
}
static void test_fifo_and_reset_commands(void) {
    s5l8920_uart_t u; configure(&u,0x405u,0x80019u);
    CHECK(s5l8920_uart_write(&u,32u,0xa0u),"shift first byte");
    for (unsigned n=0;n<16u;n++) CHECK(s5l8920_uart_write(&u,32u,n),"fill TX FIFO");
    CHECK(read_reg(&u,24u)==0x200u && !(read_reg(&u,16u)&6u),"TX full count/empty bits");
    rejected_write(&u,32u,0xffu);
    rejected_write(&u,40u,0x80020u); rejected_write(&u,4u,5u); rejected_write(&u,12u,0u); rejected_write(&u,8u,0u);
    CHECK(s5l8920_uart_write(&u,4u,0x405u),"unchanged active control rejected");
    uint8_t out[18]; memset(out,0xee,sizeof out); size_t count=0u;
    s5l8920_uart_t before=u;
    CHECK(!s5l8920_uart_clock(&u,true,UINT64_MAX,out,16u,&count) && !memcmp(&u,&before,sizeof u) && out[0]==0xeeu,"bulk capacity preflight lost prefix");
    CHECK(s5l8920_uart_clock(&u,true,UINT64_MAX,out,sizeof out,&count) && count==17u && out[0]==0xa0u && out[17]==0xeeu,"bulk clock completion count");
    for (unsigned n=0;n<16u;n++) CHECK(out[n+1u]==n,"FIFO byte order");
    CHECK(read_reg(&u,24u)==0u && (read_reg(&u,16u)&6u)==6u,"fully drained status");
    for (unsigned repeat=0;repeat<3u;repeat++) {
        for (unsigned n=0;n<16u;n++) CHECK(s5l8920_uart_receive(&u,(uint8_t)(n+repeat*16u)),"fill RX FIFO");
        CHECK(read_reg(&u,24u)==0x100u && (read_reg(&u,16u)&0x11u)==0x11u,"RX full and pending bits");
        before=u;
        CHECK(!s5l8920_uart_receive(&u,0xffu) && !memcmp(&u,&before,sizeof u),"unmodeled overrun silently dropped byte");
        CHECK(s5l8920_uart_write(&u,16u,0x10u) && (read_reg(&u,16u)&0x11u)==1u,"RX ack consumed ready data");
        for (unsigned n=0;n<16u;n++) CHECK(read_reg(&u,36u)==n+repeat*16u,"RX FIFO order/wrap");
        rejected_read(&u,36u);
    }
    CHECK(s5l8920_uart_receive(&u,0x55u),"receive before reset command");
    CHECK(s5l8920_uart_write(&u,32u,0x81u) && s5l8920_uart_write(&u,32u,0x82u),"queued bytes before FIFO reset");
    CHECK(s5l8920_uart_write(&u,8u,3u) && u.tx_count==1u && u.tx_busy && u.rx_count==0u && read_reg(&u,8u)==1u,"RX reset changed TX or retained command");
    CHECK(s5l8920_uart_write(&u,8u,5u) && u.tx_count==0u && u.tx_busy,"TX FIFO reset discarded active shifter");
    CHECK(s5l8920_uart_clock(&u,true,2080u,out,sizeof out,&count) && count==1u && out[0]==0x81u,"FIFO reset leaked queued byte or shortened frame");
    CHECK(s5l8920_uart_write(&u,16u,0x30u) && read_reg(&u,16u)==6u,"clear both pending causes");
}
static void test_channel_gates_and_partition(void) {
    for (unsigned c=0;c<8u;c++) {
        uint32_t control=(c&1u)|((c&2u)<<1)|((c&4u)<<8);
        s5l8920_uart_t u; configure(&u,control,0x80019u);
        CHECK(s5l8920_uart_receive(&u,0u)==((c&1u)!=0u),"receive gate");
        CHECK(s5l8920_uart_write(&u,32u,0xffu)==((c&2u)!=0u),"transmit gate");
        if (!(c&2u)) {
            CHECK(s5l8920_uart_write(&u,8u,0u),"disable idle FIFO");
            rejected_read(&u,16u); rejected_write(&u,32u,0u);
            CHECK(!s5l8920_uart_receive(&u,0u),"unsupported non-FIFO receive");
        }
    }
    s5l8920_uart_t whole,parts; configure(&whole,0x405u,0x80019u);
    for (unsigned n=0;n<10u;n++) CHECK(s5l8920_uart_write(&whole,32u,0x40u+n),"partition setup");
    parts=whole;
    uint8_t a[17],b[17]; size_t na=0u,nb=0u;
    CHECK(s5l8920_uart_clock(&whole,true,2080u*9u+137u,a,sizeof a,&na),"single clock chunk");
    const uint32_t chunks[]={1u,2078u,1u,17u,4137u,199u,8192u,4217u,15u};
    uint64_t total=0u;
    for (unsigned n=0;n<sizeof chunks/sizeof chunks[0];n++) {
        size_t produced=0u; total+=chunks[n];
        CHECK(s5l8920_uart_clock(&parts,true,chunks[n],b+nb,sizeof b-nb,&produced),"partition clock chunk");
        nb+=produced;
    }
    CHECK(total==2080u*9u+137u && na==nb && !memcmp(a,b,na) && !memcmp(&whole,&parts,sizeof whole),"clock partition changed observable state");
    size_t count=77u; parts=whole;
    CHECK(!s5l8920_uart_clock(&whole,true,0u,NULL,1u,&count) && count==77u && !memcmp(&whole,&parts,sizeof whole),"null output failure");
    CHECK(!s5l8920_uart_clock(&whole,true,0u,a,sizeof a,NULL) && !s5l8920_uart_clock(NULL,true,0u,a,sizeof a,&count),"null clock access");
}
int main(void) {
    test_configuration(); test_clock_and_frame_boundaries(); test_fifo_and_reset_commands(); test_channel_gates_and_partition();
    printf("%u passed, %u failed\n",passed,failed);
    return failed?1:0;
}
