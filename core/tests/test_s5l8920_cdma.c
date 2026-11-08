/* Board-level DMA/AES data, bounds, scheduling and refusal regressions.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920.h"
#include <stdio.h>
#include <string.h>
#define FEED (S5L8920_CDMA_BASE+0x1000u)
#define DRAIN (S5L8920_CDMA_BASE+0x2000u)
#define AES S5L8920_AES_CONTEXT1
#define SRC 0x41000000u
#define DST 0x41001000u
#define DESC 0x41002000u
#define GATE (S5L8920_CLOCK_GATE_BASE+4u*S5L8920_CDMA_GATE)
static s5l8920_t board;
static s5l8920_cdma_t cdma;
static unsigned passed,failed;
#define CHECK(x) do { if(x)passed++;else {failed++;printf("FAIL %s:%d: %s\n",__func__,__LINE__,#x);} } while(0)
/* NIST SP800-38A F.2.1/F.2.3/F.2.5. All 4 blocks, independent of aes.c. */
static const char *keys[]={
    "2b7e151628aed2a6abf7158809cf4f3c",
    "8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b",
    "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"};
static const char *ciphertexts[]={
    "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
    "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7",
    "4f021db243bc633d7178183a9fa071e8b4d9ada9ad7dedf4e5e738763f69145a"
    "571b242012fb7ae07fa9baac3df102e008b0e27988598881d920a9e64f5615cd",
    "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d"
    "39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b"};
static const char *plaintext="6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
static unsigned nibble(char x) { return x<='9'?(unsigned)(x-'0'):(unsigned)(x-'a'+10); }
static void decode(uint8_t *p,const char *hex) {
    while(*hex) { *p++=(uint8_t)((nibble(hex[0])<<4)|nibble(hex[1]));hex+=2; }
}
static uint8_t *at(uint32_t address) { return board.ram+address-S5L8920_RAM_BASE; }
static void wr(uint32_t address,uint32_t value) { board.bus.write32(&board,address,value); }
static uint32_t rd(uint32_t address) { return board.bus.read32(&board,address); }
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void init(unsigned gate) {
    s5l8920_free(&board);CHECK(s5l8920_init(&board));s5l8920_cdma_init(&cdma);
    CHECK(s5l8920_clock_gate_configure(&board,S5L8920_CDMA_GATE,gate));
    CHECK(s5l8920_cdma_attach(&board,&cdma));
}
static void context(unsigned size,bool decrypt) {
    uint8_t key[32]={0};decode(key,keys[size]);
    for(unsigned i=0;i<4u;++i)wr(AES+0x10u+4u*i,0x03020100u+0x04040404u*i);
    for(unsigned i=0;i<4u+2u*size;++i)wr(AES+0x20u+4u*i,word(key+4u*i));
    wr(AES,0x120100u|(size<<18)|(decrypt?0u:0x10000u));
}
static void descriptors(uint32_t source,uint32_t dest,unsigned length) {
    memset(at(DESC),0,128u);
    wr(DESC,DESC+32u);wr(DESC+4u,0x30103u);wr(DESC+8u,source);wr(DESC+12u,length);
    wr(DESC+64u,DESC+96u);wr(DESC+68u,0x103u);wr(DESC+72u,dest);wr(DESC+76u,length);
    wr(FEED,2u);wr(DRAIN,2u);
    wr(FEED+12u,length);wr(FEED+20u,DESC);wr(FEED,0x1c0180u);
    wr(DRAIN+12u,length);wr(DRAIN+20u,DESC+64u);wr(DRAIN,0x1c0080u);
}
static void prepare(void) {
    init(15u);context(0u,false);descriptors(SRC,DST,64u);
    decode(at(SRC),plaintext);memset(at(DST),0xa5,80u);CHECK(!board.bus_failure.reason);
}
static void refuse(uint32_t address,unsigned bytes,uint32_t value,s5l8920_bus_reason_t reason) {
    s5l8920_cdma_t before=cdma;uint8_t output[80];memcpy(output,at(DST),sizeof output);
    if(bytes==1u)board.bus.write8(&board,address,(uint8_t)value);
    else if(bytes==2u)board.bus.write16(&board,address,(uint16_t)value);
    else wr(address,value);
    CHECK(board.bus_failure.reason==reason && board.bus_failure.address==address);
    CHECK(!memcmp(&cdma,&before,sizeof cdma) && !memcmp(output,at(DST),sizeof output));
    CHECK(!s5l8920_cdma_board_service(&board));
    CHECK(!memcmp(&cdma,&before,sizeof cdma));
    s5l8920_clear_bus_failure(&board);
}
static void vectors(void) {
    init(15u);
    for(unsigned size=0;size<3u;++size)for(unsigned decrypt=0;decrypt<2u;++decrypt)
    for(unsigned inplace=0;inplace<2u;++inplace)for(unsigned order=0;order<2u;++order) {
        s5l8920_cdma_init(&cdma);uint32_t dest=inplace?SRC:DST;
        context(size,decrypt!=0u);descriptors(SRC,dest,64u);
        uint8_t expected[64],input[64],guard[16];memset(guard,0xa5,sizeof guard);
        decode(input,decrypt?ciphertexts[size]:plaintext);decode(expected,decrypt?plaintext:ciphertexts[size]);
        memcpy(at(SRC),input,64u);memset(at(dest+64u),0xa5,16u);
        if(!inplace)memset(at(dest),0xa5,64u);
        uint8_t untouched[80];memcpy(untouched,at(dest),80u);
        wr(order?DRAIN:FEED,order?0x81u:0x181u);
        CHECK(!board.bus_failure.reason && (rd(order?DRAIN:FEED)&0x30000u)==0x10000u);
        CHECK(s5l8920_cdma_board_service(&board) && !memcmp(untouched,at(dest),80u));
        wr(order?FEED:DRAIN,order?0x181u:0x81u);
        CHECK(!board.bus_failure.reason && !(rd(DRAIN)&0x80000u));
        for(unsigned i=0;i<16u;++i) { (void)rd(FEED);(void)rd(DRAIN); }
        CHECK(!memcmp(untouched,at(dest),80u) && rd(FEED+12u)==64u);
        for(unsigned block=0;block<4u;++block) {
            board.cpu.excl_valid=true;board.cpu.a8_excl_size=4u;
            arm_cpu_t cpu=board.cpu;uint64_t ticks=board.timebase_ticks;
            CHECK(s5l8920_cdma_board_service(&board));
            cpu.excl_valid=false;cpu.a8_excl_size=0;
            CHECK(!memcmp(&cpu,&board.cpu,sizeof cpu) && ticks==board.timebase_ticks);
            unsigned processed=16u*(block+1u);
            CHECK(!memcmp(at(dest),expected,processed));
            CHECK(!memcmp(at(dest)+processed,untouched+processed,80u-processed));
            CHECK(rd(FEED+12u)==64u-processed && rd(DRAIN+12u)==64u-processed);
            CHECK(rd(FEED+16u)==SRC+processed && rd(DRAIN+16u)==dest+processed);
            CHECK(((rd(FEED)&0x30000u)!=0u)==(block<3u));
            CHECK(((rd(DRAIN)&0x80000u)!=0u)==(block==3u));
        }
        CHECK(rd(FEED+20u)==DESC+32u && rd(DRAIN+20u)==DESC+96u);
        CHECK(!memcmp(at(dest+64u),guard,16u));
        if(!inplace)CHECK(!memcmp(at(SRC),input,64u));
        s5l8920_cdma_t completed=cdma;
        CHECK(s5l8920_cdma_board_service(&board) && !memcmp(&completed,&cdma,sizeof cdma));
        CHECK((rd(DRAIN)&0x80000u)!=0u);wr(DRAIN,0x80080u);
        CHECK(rd(DRAIN)==0x80u);
        CHECK(!board.bus_failure.reason);
    }
}
static void guards(void) {
    prepare();
    refuse(AES,4u,0x230100u,S5L8920_BUS_REGISTER_REFUSED); /* GID */
    refuse(AES,4u,0x430100u,S5L8920_BUS_REGISTER_REFUSED); /* UID */
    refuse(AES,4u,0x1f0100u,S5L8920_BUS_REGISTER_REFUSED); /* Reserved key size */
    refuse(AES,4u,0x110100u,S5L8920_BUS_REGISTER_REFUSED); /* Disabled filter */
    refuse(AES,4u,0x130200u,S5L8920_BUS_REGISTER_REFUSED); /* Other channel */
    refuse(AES+0x10u,1u,0u,S5L8920_BUS_ACCESS_UNIMPLEMENTED);
    refuse(AES+0x10u,2u,0u,S5L8920_BUS_ACCESS_UNIMPLEMENTED);
    refuse(AES+0x11u,4u,0u,S5L8920_BUS_ACCESS_UNIMPLEMENTED);
    refuse(FEED,1u,2u,S5L8920_BUS_ACCESS_UNIMPLEMENTED);
    refuse(FEED+1u,4u,2u,S5L8920_BUS_ACCESS_UNIMPLEMENTED);
    refuse(FEED+4u,4u,0u,S5L8920_BUS_REGISTER_REFUSED); /* Peripheral settings */
    refuse(FEED+8u,4u,0u,S5L8920_BUS_REGISTER_REFUSED); /* Peripheral FIFO */
    refuse(FEED,4u,0x18u,S5L8920_BUS_REGISTER_REFUSED);
    refuse(DRAIN,4u,0x89u,S5L8920_BUS_REGISTER_REFUSED); /* Interrupt mode */
    refuse(FEED,4u,0x1a1u,S5L8920_BUS_REGISTER_REFUSED); /* Hold */
    refuse(FEED,4u,0x184u,S5L8920_BUS_REGISTER_REFUSED); /* Abort */
    refuse(FEED,4u,0x81u,S5L8920_BUS_REGISTER_REFUSED); /* Wrong context */
    refuse(S5L8920_CDMA_BASE+0x3000u,4u,2u,S5L8920_BUS_UNMAPPED);
    refuse(AES+0x1000u,4u,0u,S5L8920_BUS_UNMAPPED);
    (void)rd(AES+0x20u);CHECK(board.bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED);
    s5l8920_clear_bus_failure(&board);
    for(unsigned i=0;i<4u;++i) {
        uint8_t old=cdma.iv_programmed;cdma.iv_programmed&=(uint8_t)~(1u<<i);
        refuse(FEED,4u,0x181u,S5L8920_BUS_REGISTER_REFUSED);cdma.iv_programmed=old;
    }
    for(unsigned i=0;i<4u;++i) {
        uint8_t old=cdma.key_programmed;cdma.key_programmed&=(uint8_t)~(1u<<i);
        refuse(FEED,4u,0x181u,S5L8920_BUS_REGISTER_REFUSED);cdma.key_programmed=old;
    }
    const uint32_t bad_addresses[]={0u,0x87001000u,0x4ffffff0u,0xfffffff0u};
    for(unsigned i=0;i<4u;++i) {
        wr(DESC+8u,bad_addresses[i]);refuse(FEED,4u,0x181u,S5L8920_BUS_REGISTER_REFUSED);
    }
    wr(DESC+8u,SRC);
    for(unsigned i=0;i<3u;++i) {
        wr(DESC,i==0u?DESC:(i==1u?DESC+1u:0xffffffe0u));
        refuse(FEED,4u,0x181u,S5L8920_BUS_REGISTER_REFUSED);
    }
    wr(DESC,DESC+32u);wr(DESC+36u,0x103u);
    refuse(FEED,4u,0x181u,S5L8920_BUS_REGISTER_REFUSED);wr(DESC+36u,0u);
    wr(DESC+4u,0x30102u);refuse(FEED,4u,0x181u,S5L8920_BUS_REGISTER_REFUSED);wr(DESC+4u,0x30103u);
    wr(DESC+12u,48u);refuse(FEED,4u,0x181u,S5L8920_BUS_REGISTER_REFUSED);wr(DESC+12u,64u);
    wr(FEED+12u,63u);refuse(FEED,4u,0x181u,S5L8920_BUS_REGISTER_REFUSED);wr(FEED+12u,64u);
    wr(FEED+20u,DESC+4u);refuse(FEED,4u,0x181u,S5L8920_BUS_REGISTER_REFUSED);wr(FEED+20u,DESC);
    wr(FEED,0x181u);wr(DESC+72u,SRC+16u);
    refuse(DRAIN,4u,0x81u,S5L8920_BUS_REGISTER_REFUSED);wr(DESC+72u,DST);
    refuse(AES+0x20u,4u,0u,S5L8920_BUS_REGISTER_REFUSED);
    refuse(FEED,4u,2u,S5L8920_BUS_REGISTER_REFUSED);
    refuse(DRAIN,4u,2u,S5L8920_BUS_REGISTER_REFUSED);
    wr(DRAIN,0x81u);CHECK(!board.bus_failure.reason);
    wr(GATE,0u);s5l8920_cdma_t before=cdma;
    CHECK(s5l8920_cdma_board_service(&board) && !memcmp(&before,&cdma,sizeof cdma));
    refuse(AES+0x20u,4u,0u,S5L8920_BUS_REGISTER_REFUSED);
    wr(GATE,15u);CHECK(s5l8920_cdma_board_service(&board) && cdma.channel[0].remaining==48u);
    uint8_t output[80];memcpy(output,at(DST),80u);
    CHECK(s5l8920_reset(&board) && !board.cdma && !s5l8920_cdma_board_service(&board));
    CHECK(!memcmp(output,at(DST),80u));s5l8920_cdma_init(&cdma);
    CHECK(s5l8920_cdma_attach(&board,&cdma));
    (void)rd(FEED);CHECK(board.bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED);
    s5l8920_clear_bus_failure(&board);
}
static void aliases_and_irqs(void) {
    prepare();CHECK(s5l8920_set_ram_boot_window(&board,true));
    wr(DESC+8u,SRC-S5L8920_RAM_BASE);wr(DESC+72u,SRC); /* Same backing, in-place. */
    wr(FEED+20u,DESC-S5L8920_RAM_BASE);wr(FEED,0x181u);wr(DRAIN,0x81u);
    CHECK(!board.bus_failure.reason);
    CHECK(s5l8920_set_ram_boot_window(&board,false));s5l8920_cdma_t before=cdma;
    CHECK(!s5l8920_cdma_board_service(&board) && !memcmp(&cdma,&before,sizeof cdma));
    CHECK(s5l8920_set_ram_boot_window(&board,true));
    unsigned source=44u;
    uint32_t vic=S5L8920_VIC_BASE+S5L8920_VIC_STRIDE;
    wr(vic+0x10u,1u<<(source%32u));CHECK(!board.cpu.irq_line);
    for(unsigned i=0;i<4u;++i)CHECK(s5l8920_cdma_board_service(&board));
    CHECK(!board.cpu.irq_line && !(rd(vic+8u)&(1u<<(source%32u))));
    uint8_t expected[64];decode(expected,ciphertexts[0]);CHECK(!memcmp(at(SRC),expected,64u));
    wr(DRAIN,0x80080u);CHECK(!board.cpu.irq_line);
    CHECK(s5l8920_set_irq(&board,source,true));CHECK(board.cpu.irq_line);
    wr(DRAIN,0x80080u);CHECK(board.cpu.irq_line);
    CHECK(s5l8920_set_irq(&board,source,false));CHECK(!board.cpu.irq_line);
}
static void lifetime_and_gates(void) {
    s5l8920_cdma_t other={0};s5l8920_free(&board);CHECK(s5l8920_init(&board));
    CHECK(!s5l8920_cdma_attach(&board,&other));s5l8920_cdma_init(&other);
    CHECK(!s5l8920_cdma_attach(&board,&other));
    for(unsigned gate=0;gate<15u;++gate) {
        init(gate);s5l8920_cdma_t before=cdma;
        CHECK(s5l8920_cdma_board_service(&board)==(gate==0u));
        CHECK(!memcmp(&before,&cdma,sizeof cdma));
        refuse(FEED,4u,2u,S5L8920_BUS_REGISTER_REFUSED);
        CHECK(s5l8920_cdma_attach(&board,&cdma));
        CHECK(!s5l8920_cdma_attach(&board,&other));
    }
    init(15u);uint32_t value=0x12345678u;
    CHECK(!s5l8920_cdma_read(&cdma,false,0x1000u,&value) && value==0x12345678u);
    refuse(FEED,4u,0x181u,S5L8920_BUS_REGISTER_REFUSED);
    context(0u,false);descriptors(SRC,DST,0u);
    refuse(FEED,4u,0x181u,S5L8920_BUS_REGISTER_REFUSED);
    descriptors(SRC,DST,64u);wr(DRAIN+12u,48u);wr(DESC+76u,48u);
    wr(FEED,0x181u);refuse(DRAIN,4u,0x81u,S5L8920_BUS_REGISTER_REFUSED);
    wr(DRAIN+12u,64u);wr(DESC+76u,64u);wr(DESC+72u,0xfffffff0u);
    refuse(DRAIN,4u,0x81u,S5L8920_BUS_REGISTER_REFUSED);
    wr(DESC+72u,DST);wr(DRAIN,0x81u);CHECK(!board.bus_failure.reason);
    CHECK(s5l8920_cdma_board_service(&board));
    uint8_t output[64];memcpy(output,at(DST),64u);
    refuse(FEED+12u,4u,0u,S5L8920_BUS_REGISTER_REFUSED);
    refuse(DRAIN+20u,4u,DESC,S5L8920_BUS_REGISTER_REFUSED);
    CHECK(!memcmp(output,at(DST),64u));
    /* Exact RAM upper edge is valid, one extra block is not. */
    for(unsigned direction=0;direction<2u;++direction) {
        prepare();uint32_t edge=S5L8920_RAM_BASE+S5L8920_RAM_SIZE-64u;
        uint32_t source=direction?SRC:edge,dest=direction?edge:DST;
        decode(at(source),plaintext);descriptors(source,dest,64u);
        wr(FEED,0x181u);wr(DRAIN,0x81u);
        for(unsigned i=0;i<4u;++i)CHECK(s5l8920_cdma_board_service(&board));
        uint8_t expected[64];decode(expected,ciphertexts[0]);CHECK(!memcmp(at(dest),expected,64u));
        CHECK(!board.bus_failure.reason);
    }
}
static void nand_register_setup(void) {
    init(15u);
    for(unsigned channel=5;channel<=8;channel++) {
        uint32_t reg=S5L8920_CDMA_BASE+channel*0x1000u;
        wr(reg,2u);CHECK(!board.bus_failure.reason);
        s5l8920_clear_bus_failure(&board);
        wr(reg+4u,(channel&1u)?0x58u:0u);CHECK(!board.bus_failure.reason);
        s5l8920_clear_bus_failure(&board);
    }
}
#define NAND_OUT0 0x41010000u
#define NAND_OUT1 0x41012000u
static struct {
    uint8_t raw[4314],lane[2][4096];
    unsigned cursor[2],available[2],length[2],calls,bank;
    bool refuse,reenter,invalid;
} nand_input;
static s5l8920_cdma_input_t nand_receive(void *owner,uint32_t peripheral,uint8_t *bytes,unsigned count) {
    CHECK(owner==&nand_input && count && count<=16);
    unsigned lane=peripheral&4u?0u:1u;
    CHECK(peripheral==0x81200000u+nand_input.bank*0x100000u+(lane?0x18u:0x14u));
    nand_input.calls++;
    if(nand_input.reenter) {
        s5l8920_cdma_t before=cdma;uint32_t value=0x12345678u;uint8_t *ram=board.ram;
        s5l8920_cdma_init(&cdma);CHECK(!memcmp(&before,&cdma,sizeof cdma));
        CHECK(!s5l8920_cdma_write(&cdma,false,0x5000,2,NULL,NULL));
        CHECK(!s5l8920_cdma_read(&cdma,false,0x5000,&value) && value==0x12345678u);
        CHECK(!s5l8920_reset(&board));s5l8920_free(&board);CHECK(board.ram==ram);
        CHECK(!s5l8920_cdma_attach(&board,&cdma));CHECK(!s5l8920_cdma_board_service(&board));
        CHECK(!s5l8920_cdma_nand_board_service(&board,5,nand_receive,&nand_input));
        CHECK(!memcmp(&before,&cdma,sizeof cdma));
    }
    memset(bytes,0xff,count); /* WAIT/refusal must discard even plausible bytes. */
    if(nand_input.invalid)return (s5l8920_cdma_input_t)99;
    if(nand_input.refuse)return S5L8920_CDMA_UNAVAILABLE;
    if(count>nand_input.available[lane]-nand_input.cursor[lane])return S5L8920_CDMA_WAIT;
    memcpy(bytes,nand_input.lane[lane]+nand_input.cursor[lane],count);
    nand_input.cursor[lane]+=count;return S5L8920_CDMA_READY;
}
static void nand_segment(unsigned lane,unsigned index,uint32_t destination,unsigned length) {
    uint32_t d=DESC+lane*0x400u+index*32u;
    wr(d,d+32u);wr(d+4u,3u);wr(d+8u,destination);wr(d+12u,length);
}
static void nand_program(unsigned channel,unsigned length,uint32_t descriptor) {
    uint32_t reg=S5L8920_CDMA_BASE+channel*0x1000u;
    wr(reg,2u);wr(reg+4u,(channel&1u)?0x58u:0u);
    wr(reg+8u,0x81200000u+((channel-5u)/2u)*0x100000u+((channel&1u)?0x14u:0x18u));
    wr(reg+12u,length);wr(reg+20u,descriptor);
}
static void nand_prepare(unsigned bank) {
    init(15u);memset(&nand_input,0,sizeof nand_input);nand_input.bank=bank;
    nand_input.reenter=true;
    memset(at(NAND_OUT0),0xa5,4096+16);memset(at(NAND_OUT1),0x5a,218+16);
    memset(at(DESC),0,0x800);
    for(unsigned n=0;n<sizeof nand_input.raw;n++)nand_input.raw[n]=(uint8_t)(bank*19+n*37+(n>>8));
    unsigned offset=0,spare=218,segments[2]={0,0};
    for(unsigned sector=0;sector<8;sector++)for(unsigned lane=0;lane<2;lane++) {
        unsigned length=lane?(spare<32?spare:32):512;
        if(lane)spare-=length;
        memcpy(nand_input.lane[lane]+nand_input.length[lane],nand_input.raw+offset,length);
        nand_input.length[lane]+=length;
        while(length) {
            unsigned chunk=length;
            if(offset<4096 && chunk>4096-offset)chunk=4096-offset;
            uint32_t destination=offset<4096?NAND_OUT0+offset:NAND_OUT1+offset-4096;
            nand_segment(lane,segments[lane]++,destination,chunk);length-=chunk;offset+=chunk;
        }
    }
    CHECK(offset==4314 && segments[0]==9 && segments[1]==7);
    for(unsigned lane=0;lane<2;lane++) {
        wr(DESC+lane*0x400+(segments[lane]-1)*32+4,0x103);
        nand_program(5+bank*2+lane,nand_input.length[lane],DESC+lane*0x400);
    }
    CHECK(!board.bus_failure.reason);
}
static void nand_transfers(void) {
    for(unsigned bank=0;bank<2;bank++) {
        nand_prepare(bank);unsigned first=5+bank*2;
        uint32_t reg=S5L8920_CDMA_BASE+first*0x1000;
        for(unsigned lane=0;lane<2;lane++)wr(reg+lane*0x1000,0x1c0009);
        CHECK(!board.bus_failure.reason);
        s5l8920_cdma_t before=cdma;
        CHECK(s5l8920_cdma_board_service(&board) && !memcmp(&before,&cdma,sizeof cdma));
        CHECK(!s5l8920_cdma_nand_board_service(&board,first,NULL,NULL));
        CHECK(!memcmp(&before,&cdma,sizeof cdma));
        for(unsigned n=0;n<3;n++) {
            CHECK(s5l8920_cdma_nand_board_service(&board,first,nand_receive,&nand_input));
            CHECK(!memcmp(&before,&cdma,sizeof cdma));
        }
        unsigned calls=nand_input.calls;
        for(unsigned n=0;n<10;n++)CHECK(rd(reg)==0x10008 && rd(reg+12)==4096);
        CHECK(nand_input.calls==calls && !memcmp(&before,&cdma,sizeof cdma));
        nand_input.refuse=true;
        CHECK(!s5l8920_cdma_nand_board_service(&board,first,nand_receive,&nand_input));
        CHECK(!memcmp(&before,&cdma,sizeof cdma));nand_input.refuse=false;nand_input.invalid=true;
        CHECK(!s5l8920_cdma_nand_board_service(&board,first,nand_receive,&nand_input));
        CHECK(!memcmp(&before,&cdma,sizeof cdma));nand_input.invalid=false;
        for(unsigned n=0;n<32;n++)CHECK(at(NAND_OUT0)[n]==0xa5 && at(NAND_OUT1)[n]==0x5a);
        nand_input.available[0]=4096;nand_input.available[1]=218;
        wr(GATE,0);calls=nand_input.calls;
        CHECK(s5l8920_cdma_nand_board_service(&board,first,nand_receive,&nand_input));
        CHECK(nand_input.calls==calls && !memcmp(&before,&cdma,sizeof cdma));wr(GATE,15);
        for(unsigned quantum=0;quantum<300;quantum++)for(unsigned lane=0;lane<2;lane++) {
            s5l8920_cdma_nand_t *n=&cdma.nand[first-5+lane];
            uint32_t remaining=n->state.remaining;
            board.cpu.excl_valid=true;board.cpu.a8_excl_size=4;arm_cpu_t cpu=board.cpu;
            uint64_t ticks=board.timebase_ticks;
            CHECK(s5l8920_cdma_nand_board_service(&board,first+lane,nand_receive,&nand_input));
            if(n->state.remaining!=remaining) { cpu.excl_valid=false;cpu.a8_excl_size=0; }
            CHECK(!memcmp(&cpu,&board.cpu,sizeof cpu) && ticks==board.timebase_ticks);
            CHECK(remaining-n->state.remaining<=16);
            CHECK(s5l8920_cdma_nand_irq(&cdma,first+lane)==(n->state.remaining==0));
        }
        CHECK(nand_input.cursor[0]==4096 && nand_input.cursor[1]==218);
        CHECK(!memcmp(at(NAND_OUT0),nand_input.raw,4096));
        CHECK(!memcmp(at(NAND_OUT1),nand_input.raw+4096,218));
        for(unsigned n=0;n<16;n++)CHECK(at(NAND_OUT0)[4096+n]==0xa5 && at(NAND_OUT1)[218+n]==0x5a);
        for(unsigned lane=0;lane<2;lane++) {
            unsigned channel=first+lane,source=0x2a+channel;uint32_t control=reg+lane*0x1000;
            uint32_t vic=S5L8920_VIC_BASE+S5L8920_VIC_STRIDE,bit=1u<<(source%32);
            CHECK(rd(control)==0x80008 && rd(control+12)==0);
            CHECK(rd(control+20)==DESC+lane*0x400+(lane?7:9)*32);
            wr(vic+0x10,bit);CHECK(board.cpu.irq_line && (rd(vic+8)&bit));
            CHECK(s5l8920_set_irq(&board,source,true));wr(control,0x80000);
            CHECK(!s5l8920_cdma_nand_irq(&cdma,channel) && board.cpu.irq_line);
            CHECK(s5l8920_set_irq(&board,source,false));CHECK(!board.cpu.irq_line && rd(control)==0);
            wr(vic+0x14,bit);
        }
        CHECK(!board.bus_failure.reason);
    }
}
static void nand_guards(void) {
    nand_prepare(0);uint32_t reg=S5L8920_CDMA_BASE+0x5000;
    const uint32_t offsets[]={0,4,8,12,20};
    const uint32_t values[]={0x1c0001,0,0x81200018,0,0xffffffe0};
    for(unsigned i=0;i<5;i++) {
        nand_program(5,4096,DESC);
        if(i<3)refuse(reg+offsets[i],4,values[i],S5L8920_BUS_REGISTER_REFUSED);
        else { wr(reg+offsets[i],values[i]);refuse(reg,4,0x1c0009,S5L8920_BUS_REGISTER_REFUSED); }
    }
    for(unsigned omit=0;omit<4;omit++) {
        nand_program(5,4096,DESC);s5l8920_cdma_nand_t *n=&cdma.nand[0];
        if(omit==0)n->configuration_programmed=false;
        if(omit==1)n->peripheral_programmed=false;
        if(omit==2)n->state.size_programmed=false;
        if(omit==3)n->state.descriptor_programmed=false;
        refuse(reg,4,0x1c0009,S5L8920_BUS_REGISTER_REFUSED);
    }
    nand_program(5,4096,DESC);
    const unsigned off[]={0,4,8,12,9*32+4};
    const uint32_t bad[]={DESC,0x30103,0x87000000,0,3};
    for(unsigned i=0;i<5;i++) {
        uint32_t old=word(at(DESC+off[i]));wr(DESC+off[i],bad[i]);
        refuse(reg,4,0x1c0009,S5L8920_BUS_REGISTER_REFUSED);wr(DESC+off[i],old);
    }
    CHECK(s5l8920_set_ram_boot_window(&board,true));
    wr(DESC+8,DESC-S5L8920_RAM_BASE);refuse(reg,4,0x1c0009,S5L8920_BUS_REGISTER_REFUSED);
    wr(DESC+8,NAND_OUT0);CHECK(s5l8920_set_ram_boot_window(&board,false));
    wr(reg,0x1c0009);CHECK(!board.bus_failure.reason);
    refuse(reg,4,2,S5L8920_BUS_REGISTER_REFUSED);refuse(reg,4,4,S5L8920_BUS_REGISTER_REFUSED);
    refuse(reg+4,4,0x58,S5L8920_BUS_REGISTER_REFUSED);
    nand_input.available[0]=4096;s5l8920_cdma_t before=cdma;unsigned calls=nand_input.calls;
    wr(DESC+12,511);CHECK(!s5l8920_cdma_nand_board_service(&board,5,nand_receive,&nand_input));
    CHECK(!memcmp(&before,&cdma,sizeof cdma) && nand_input.calls==calls);wr(DESC+12,512);
    CHECK(s5l8920_cdma_nand_board_service(&board,5,nand_receive,&nand_input));
    CHECK(cdma.nand[0].state.remaining==4080);
    CHECK(s5l8920_reset(&board) && !board.cdma && cdma.nand[0].state.remaining==4080);
    CHECK(!s5l8920_cdma_nand_board_service(&board,5,nand_receive,&nand_input));
    s5l8920_cdma_init(&cdma);CHECK(!cdma.nand[0].state.reset && !cdma.nand[0].state.remaining);
    CHECK(s5l8920_cdma_attach(&board,&cdma));
    uint32_t value=0x12345678;CHECK(!s5l8920_cdma_read(&cdma,false,0x5000,&value) && value==0x12345678);
    CHECK(!s5l8920_cdma_nand_board_service(&board,4,nand_receive,&nand_input));
    CHECK(!s5l8920_cdma_nand_board_service(&board,9,nand_receive,&nand_input));
    CHECK(!s5l8920_cdma_nand_irq(NULL,5));CHECK(!s5l8920_cdma_nand_irq(&cdma,4));
}
static void nand_chain_limits(void) {
    nand_prepare(0);uint32_t reg=S5L8920_CDMA_BASE+0x5000;
    memset(at(DESC),0,0x800);
    for(unsigned n=0;n<31;n++)nand_segment(0,n,NAND_OUT0+n,1);
    wr(DESC+30*32+4,0x103);nand_program(5,31,DESC);
    uint32_t value=0x12345678;
    CHECK(!s5l8920_cdma_read(&cdma,false,0x5004,&value) && value==0x12345678);
    CHECK(!s5l8920_cdma_read(&cdma,false,0x5008,&value) && value==0x12345678);
    CHECK(s5l8920_set_ram_boot_window(&board,true));wr(reg+20,DESC-S5L8920_RAM_BASE);
    wr(reg,0x1c0009);CHECK(!board.bus_failure.reason && cdma.nand[0].count==31);
    nand_input.available[0]=31;unsigned calls=nand_input.calls;s5l8920_cdma_t before=cdma;
    CHECK(s5l8920_set_ram_boot_window(&board,false));
    CHECK(!s5l8920_cdma_nand_board_service(&board,5,nand_receive,&nand_input));
    CHECK(!memcmp(&before,&cdma,sizeof cdma) && nand_input.calls==calls);
    CHECK(s5l8920_set_ram_boot_window(&board,true));wr(DESC+31*32+4,3);
    CHECK(!s5l8920_cdma_nand_board_service(&board,5,nand_receive,&nand_input));
    CHECK(!memcmp(&before,&cdma,sizeof cdma) && nand_input.calls==calls);wr(DESC+31*32+4,0);
    for(unsigned n=0;n<31;n++) {
        CHECK(s5l8920_cdma_nand_board_service(&board,5,nand_receive,&nand_input));
        CHECK(cdma.nand[0].state.remaining==30-n && cdma.nand[0].index==n+1);
    }
    CHECK(!memcmp(at(NAND_OUT0),nand_input.lane[0],31) && at(NAND_OUT0)[31]==0xa5);
    wr(reg,2);CHECK(!s5l8920_cdma_nand_irq(&cdma,5));
    wr(DESC+30*32+4,3);nand_segment(0,31,NAND_OUT0+31,1);wr(DESC+31*32+4,0x103);
    nand_program(5,32,DESC);refuse(reg,4,0x1c0009,S5L8920_BUS_REGISTER_REFUSED);
    CHECK(!s5l8920_cdma_nand_service(NULL,5,NULL,NULL,NULL,NULL));
}
static void amc_register_setup(void) {
    s5l8920_cdma_init(&cdma);
    uint32_t value=0x12345678u;
    CHECK(!s5l8920_cdma_read(&cdma,false,0x18000u,&value) && value==0x12345678u);
    CHECK(s5l8920_cdma_write(&cdma,false,0x18000u,2u,NULL,NULL));
    CHECK(s5l8920_cdma_read(&cdma,false,0x18000u,&value) && value==0u);
    CHECK(s5l8920_cdma_write(&cdma,false,0x18004u,0x300d8u,NULL,NULL));
    CHECK(s5l8920_cdma_write(&cdma,false,0x18008u,0x8480002cu,NULL,NULL));
    CHECK(s5l8920_cdma_write(&cdma,false,0x1800cu,0x40000000u,NULL,NULL));
    value=0x12345678u;
    CHECK(!s5l8920_cdma_read(&cdma,false,0x18010u,&value) && value==0x12345678u);
}
#define AMC_RAM 0x60000000u
static uint8_t amc_ram[0x8000],amc_source[4096];
static unsigned amc_calls,amc_cursor,amc_reads,amc_reenter;
static uint32_t amc_deny;
static s5l8920_cdma_input_t amc_result;
static void amc_put(unsigned offset,uint32_t value) {
    for(unsigned i=0;i<4;i++)amc_ram[offset+i]=(uint8_t)(value>>(8*i));
}
static uint8_t *amc_memory(void *owner,uint32_t address,uint32_t size) {
    (void)owner;amc_reads++;
    if(address>=0x70000000u && address<0x70008000u)address-=0x10000000u;
    if(address<AMC_RAM || (uint64_t)address+size>AMC_RAM+sizeof amc_ram ||
       (amc_deny && address<=amc_deny && (uint64_t)address+size>amc_deny))return NULL;
    return amc_ram+address-AMC_RAM;
}
static s5l8920_cdma_input_t amc_receive(void *owner,uint32_t peripheral,uint8_t *bytes,unsigned count) {
    (void)owner;amc_calls++;
    CHECK(peripheral==0x8480002cu && count>0 && count<=16);
    if(amc_reenter) {
        s5l8920_cdma_t before=cdma;unsigned moved=99;uint32_t value=99;
        CHECK(!s5l8920_cdma_write(&cdma,false,0x18000u,2u,amc_memory,NULL));
        CHECK(!s5l8920_cdma_read(&cdma,false,0x18000u,&value) && value==99);
        CHECK(!s5l8920_cdma_amc_service(&cdma,true,16,amc_memory,NULL,amc_receive,NULL,&moved) && !moved);
        s5l8920_cdma_init(&cdma);CHECK(!memcmp(&cdma,&before,sizeof cdma));
    }
    if(amc_result==S5L8920_CDMA_READY) {
        CHECK(amc_cursor+count<=sizeof amc_source);
        memcpy(bytes,amc_source+amc_cursor,count);amc_cursor+=count;
    } else memset(bytes,0xee,count);
    return amc_result;
}
static bool amc_write_reg(unsigned reg,uint32_t value) {
    return s5l8920_cdma_write(&cdma,false,0x18000u+reg,value,amc_memory,NULL);
}
static void amc_fixture(unsigned head,unsigned count) {
    s5l8920_cdma_init(&cdma);memset(amc_ram,0xa5,sizeof amc_ram);
    for(unsigned i=0;i<sizeof amc_source;i++)amc_source[i]=(uint8_t)(i*37u+19u);
    amc_calls=amc_cursor=amc_reads=amc_reenter=0;amc_deny=0;amc_result=S5L8920_CDMA_READY;
    for(unsigned i=0;i<=count+1u;i++) {
        unsigned slot=(head+i)&127u;
        amc_put(slot*32,AMC_RAM+((slot+1)&127u)*32);
        amc_put(slot*32+4,i>count?0u:i==count?0x303u:3u);
        amc_put(slot*32+8,AMC_RAM+0x2000+i*32);
        amc_put(slot*32+12,i==0?20u:7u);
    }
    CHECK(amc_write_reg(0,2));CHECK(amc_write_reg(4,0x300d8));
    CHECK(amc_write_reg(8,0x8480002c));CHECK(amc_write_reg(12,0x40000000));
    CHECK(amc_write_reg(20,AMC_RAM+head*32));CHECK(amc_write_reg(0,0x18));
}
static bool amc_step(bool clock,unsigned budget,unsigned *moved) {
    return s5l8920_cdma_amc_service(&cdma,clock,budget,amc_memory,NULL,amc_receive,NULL,moved);
}
static void amc_transfers(void) {
    const unsigned budgets[]={1,15,16,17,0xffffffffu};
    for(unsigned head=0;head<128;head+=127)for(unsigned b=0;b<5;b++) {
        amc_fixture(head,2);CHECK(amc_write_reg(0,0x19));amc_reenter=1;
        s5l8920_cdma_t before=cdma;uint8_t expected_ram[sizeof amc_ram];
        memcpy(expected_ram,amc_ram,sizeof amc_ram);unsigned moved=99,reads=amc_reads;
        CHECK(amc_step(false,16,&moved) && !moved);
        CHECK(amc_step(true,0,&moved) && !moved);
        CHECK(!memcmp(&before,&cdma,sizeof cdma) && !amc_calls && reads==amc_reads);
        for(unsigned r=0;r<3;r++) {
            amc_result=r==0?S5L8920_CDMA_WAIT:r==1?S5L8920_CDMA_UNAVAILABLE:(s5l8920_cdma_input_t)99;
            CHECK(amc_step(true,16,&moved)==(r==0) && !moved);
            CHECK(!amc_cursor && !memcmp(&before,&cdma,sizeof cdma) && !memcmp(expected_ram,amc_ram,sizeof amc_ram));
        }
        amc_result=S5L8920_CDMA_READY;unsigned total=0;
        for(unsigned calls=0;calls<40 && total<34;calls++) {
            unsigned end=total<20?20:total<27?27:34,n=end-total;
            if(n>16)n=16;
            if(n>budgets[b])n=budgets[b];
            CHECK(amc_step(true,budgets[b],&moved) && moved==n);total+=moved;
            CHECK(cdma.amc.state.remaining==0x40000000u-total && cdma.amc.state.control==(total==34?0x110018u:0x10018u));
            CHECK(s5l8920_cdma_amc_irq(&cdma)==(total==34));
            CHECK(cdma.amc.state.address==AMC_RAM+(total<=20?0x2000+total:total<=27?0x2020+total-20:0x2040+total-27));
        }
        memcpy(expected_ram+0x2000,amc_source,20);memcpy(expected_ram+0x2020,amc_source+20,7);
        memcpy(expected_ram+0x2040,amc_source+27,7);
        CHECK(total==34 && amc_cursor==34 && !memcmp(expected_ram,amc_ram,sizeof amc_ram));
        CHECK(cdma.amc.state.descriptor==AMC_RAM+((head+3)&127u)*32);
        before=cdma;unsigned calls=amc_calls;reads=amc_reads;
        CHECK(!amc_step(true,16,&moved) && !moved);
        CHECK(!memcmp(&before,&cdma,sizeof cdma) && calls==amc_calls && reads==amc_reads);
        CHECK(!amc_write_reg(0,2) && !amc_write_reg(0,0x19) && !amc_write_reg(0,4));
        CHECK(!memcmp(&before,&cdma,sizeof cdma));
        s5l8920_cdma_init(&cdma);CHECK(!cdma.amc.state.reset && !cdma.amc.count);
    }
}
static void amc_refused_go(void) {
    s5l8920_cdma_t before=cdma;uint8_t old[sizeof amc_ram];memcpy(old,amc_ram,sizeof old);
    CHECK(!amc_write_reg(0,0x19));
    CHECK(!memcmp(&before,&cdma,sizeof cdma) && !memcmp(old,amc_ram,sizeof old) && !amc_calls);
}
static void amc_guards(void) {
    for(unsigned fault=0;fault<19;fault++) {
        amc_fixture(0,2);
        switch(fault) {
        case 0:cdma.amc.configuration_programmed=false;break;
        case 1:cdma.amc.peripheral_programmed=false;break;
        case 2:cdma.amc.state.size_programmed=false;break;
        case 3:cdma.amc.state.descriptor_programmed=false;break;
        case 4:amc_put(4,0x300);break;
        case 5:amc_put(4,0x103);break;
        case 6:amc_put(36,0);break;
        case 7:amc_put(68,0x203);break;
        case 8:amc_put(32,AMC_RAM);break; /* Cycle */
        case 9:amc_put(0,AMC_RAM+1);break;
        case 10:amc_put(8,0xfffffffcu);break;
        case 11:amc_put(12,0);break;
        case 12:amc_put(8,AMC_RAM+64);break;
        case 13:amc_put(40,0x70000000);break; /* Aliased metadata */
        case 14:amc_put(32,0x70000000);break; /* Aliased cycle */
        case 15:CHECK(amc_write_reg(12,27));break;
        case 16:amc_deny=AMC_RAM+0x2020;break;
        case 17:amc_deny=AMC_RAM+64;break;
        case 18:CHECK(amc_write_reg(20,AMC_RAM+1));break;
        }
        amc_refused_go();
    }
    for(unsigned fault=0;fault<8;fault++) {
        amc_fixture(127,2);CHECK(amc_write_reg(0,0x19));
        s5l8920_cdma_t before=cdma;unsigned moved=99;
        if(fault<4)amc_put(4*fault,word(amc_ram+4*fault)^1u); /* Later descriptor */
        if(fault==4)amc_put(36,3); /* Unimplemented boundary changed */
        if(fault==5)amc_deny=AMC_RAM+0xfe0;
        if(fault==6)amc_deny=AMC_RAM+0x2000;
        if(fault==7)amc_deny=AMC_RAM+32;
        uint8_t old[sizeof amc_ram];memcpy(old,amc_ram,sizeof old);
        CHECK(!amc_step(true,16,&moved) && !moved);
        CHECK(!memcmp(&before,&cdma,sizeof cdma) && !memcmp(old,amc_ram,sizeof old) && !amc_calls);
    }
    amc_fixture(0,2);s5l8920_cdma_t before=cdma;
    const unsigned regs[]={0,0,0,4,8,16,24};
    const uint32_t values[]={0x20,4,0x1c0019,0x300da,0x84800028,0,0};
    for(unsigned i=0;i<7;i++) { CHECK(!amc_write_reg(regs[i],values[i]));CHECK(!memcmp(&before,&cdma,sizeof cdma)); }
    unsigned moved=99;CHECK(!s5l8920_cdma_amc_service(NULL,true,16,NULL,NULL,NULL,NULL,&moved) && !moved);
    CHECK(!amc_step(true,16,NULL));CHECK(amc_write_reg(0,0x19));before=cdma;
    CHECK(!s5l8920_cdma_amc_service(&cdma,true,16,NULL,NULL,amc_receive,NULL,&moved) && !moved);
    CHECK(!s5l8920_cdma_amc_service(&cdma,true,16,amc_memory,NULL,NULL,NULL,&moved) && !moved);
    CHECK(!memcmp(&before,&cdma,sizeof cdma));
}
static void amc_limits(void) {
    amc_fixture(127,126);CHECK(amc_write_reg(0,0x19));
    unsigned total=0,moved=0;
    for(unsigned i=0;i<128;i++) { CHECK(amc_step(true,16,&moved));total+=moved; }
    CHECK(total==20+126*7 && cdma.amc.index==127 && !amc_step(true,16,&moved) && !moved);
    amc_fixture(0,127);amc_put(4,3);amc_put(8,AMC_RAM+0x2000);amc_put(12,20);
    amc_put(127*32,AMC_RAM+0x1000);amc_put(0x1004,0);amc_refused_go();
    /* Software-only descriptor words are not interpreted by the data mover. */
    amc_fixture(0,2);CHECK(amc_write_reg(0,0x19));amc_put(16,0);amc_put(24,0);
    CHECK(amc_step(true,16,&moved) && moved==16);
    /* Channel24 deliberately remains absent from the board's MMIO fabric. */
    init(3);refuse(S5L8920_CDMA_BASE+0x18000u,4,2,S5L8920_BUS_UNMAPPED);s5l8920_free(&board);
}
static void amc_single_command_setup(void) {
    for(unsigned enables=0;enables<=0x18;enables+=8) {
        amc_fixture(0,0);CHECK(amc_write_reg(0,0x19));
        CHECK(amc_write_reg(0,enables));
        unsigned moved=99;CHECK(amc_step(true,16,&moved) && moved==16);
        CHECK(!s5l8920_cdma_amc_irq(&cdma) && !(cdma.amc.state.control&0x180000u));
        s5l8920_cdma_t before=cdma;amc_result=S5L8920_CDMA_WAIT;
        CHECK(amc_step(true,16,&moved) && !moved && !memcmp(&before,&cdma,sizeof cdma));
        amc_result=S5L8920_CDMA_READY;CHECK(amc_step(true,16,&moved) && moved==4);
        CHECK(cdma.amc.state.control==(0x110000u|enables) && cdma.amc.state.remaining==0x40000000u-20u);
        CHECK(s5l8920_cdma_amc_irq(&cdma)==((enables&0x10)!=0));
        before=cdma;uint32_t control=0;
        for(unsigned i=0;i<8;i++)CHECK(s5l8920_cdma_read(&cdma,false,0x18000,&control) && control==(0x110000u|enables));
        CHECK(!memcmp(&before,&cdma,sizeof cdma) && amc_cursor==20);
        CHECK(amc_write_reg(0,0x80000u|enables)); /* A different W1C bit cannot clear CIR. */
        CHECK(cdma.amc.state.control==(0x110000u|enables));
        CHECK(amc_write_reg(0,0x18u) && s5l8920_cdma_amc_irq(&cdma));
        CHECK(amc_write_reg(0,0x190018u) && !s5l8920_cdma_amc_irq(&cdma)); /* Original IRQ echo. */
        CHECK(cdma.amc.state.control==0x10018u && cdma.amc.state.descriptor==AMC_RAM+32);
        before=cdma;unsigned calls=amc_calls;
        CHECK(!amc_step(true,16,&moved) && !moved && !memcmp(&before,&cdma,sizeof cdma) && amc_calls==calls);
        s5l8920_cdma_init(&cdma);CHECK(!s5l8920_cdma_amc_irq(&cdma));
    }
    CHECK(!s5l8920_cdma_amc_irq(NULL));
}
static void amc_live_publication(void) {
    amc_fixture(0,0);CHECK(amc_write_reg(0,0x19));
    unsigned moved=99;CHECK(amc_step(true,7,&moved) && moved==7);
    s5l8920_cdma_channel_t before=cdma.amc.state;
    amc_put(32,AMC_RAM+64);amc_put(36,0x303);amc_put(40,AMC_RAM+0x3000);amc_put(44,12);
    amc_put(64,AMC_RAM+96);amc_put(68,0);amc_put(72,0);amc_put(76,0);
    bool accepted=amc_write_reg(0,0x19);CHECK(accepted);if(!accepted)return;
    CHECK(cdma.amc.offset==7 && cdma.amc.state.remaining==before.remaining &&
        cdma.amc.state.address==before.address && cdma.amc.state.descriptor==before.descriptor);
    CHECK(amc_step(true,16,&moved) && moved==13 && s5l8920_cdma_amc_irq(&cdma));
    CHECK(amc_write_reg(0,0x190018) && !s5l8920_cdma_amc_irq(&cdma));
    CHECK(amc_step(true,16,&moved) && moved==12 && s5l8920_cdma_amc_irq(&cdma));
    CHECK(amc_cursor==32 && !memcmp(amc_ram+0x2000,amc_source,20) &&
        !memcmp(amc_ram+0x3000,amc_source+20,12));
}
static void amc_publish(unsigned head,unsigned count,unsigned data) {
    for(unsigned i=0;i<=count;i++) {
        unsigned slot=(head+i)&127u;
        amc_put(slot*32,AMC_RAM+((slot+1)&127u)*32);
        amc_put(slot*32+4,i==count?0u:i+1==count?0x303u:3u);
        amc_put(slot*32+8,i==count?0u:AMC_RAM+data+i*32);
        amc_put(slot*32+12,i==count?0u:i?21u:9u);
    }
}
static void amc_publication_progress(void) {
    const unsigned phases[]={0,7,20};
    for(unsigned head=0;head<128;head+=127)for(unsigned phase=0;phase<3;phase++)
     for(unsigned ack=0;ack<2;ack++)for(unsigned masked=0;masked<2;masked++) {
        amc_fixture(head,0);CHECK(amc_write_reg(0,0x19));
        unsigned total=0,moved=99;
        while(total<phases[phase]) {
            CHECK(amc_step(true,phases[phase]-total,&moved) && moved>0);
            if(!moved)break;
            total+=moved;
        }
        CHECK(total==phases[phase]);
        if(ack)CHECK(amc_write_reg(0,0x190018));
        if(masked)CHECK(amc_write_reg(0,0));
        s5l8920_cdma_t before=cdma;unsigned calls=amc_calls;
        amc_publish((head+1)&127u,2,0x3000);
        uint8_t expected_ram[sizeof amc_ram];memcpy(expected_ram,amc_ram,sizeof amc_ram);
        CHECK(!amc_step(true,16,&moved) && !moved && calls==amc_calls);
        CHECK(!memcmp(&before,&cdma,sizeof cdma) && !memcmp(expected_ram,amc_ram,sizeof amc_ram));
        bool accepted=amc_write_reg(0,0x19);CHECK(accepted);if(!accepted)continue;
        bool pending=(before.amc.state.control&0x100000u)!=0;
        CHECK(cdma.amc.state.remaining==before.amc.state.remaining &&
            cdma.amc.state.address==before.amc.state.address &&
            cdma.amc.state.descriptor==before.amc.state.descriptor && amc_calls==calls);
        CHECK(s5l8920_cdma_amc_irq(&cdma)==pending && cdma.amc.state.control==(0x10018u|(pending?0x100000u:0u)));
        before=cdma;amc_result=S5L8920_CDMA_WAIT;
        CHECK(amc_step(true,16,&moved) && !moved && !memcmp(&before,&cdma,sizeof cdma));
        amc_result=S5L8920_CDMA_UNAVAILABLE;
        CHECK(!amc_step(true,16,&moved) && !moved && !memcmp(&before,&cdma,sizeof cdma));
        calls=amc_calls;
        CHECK(amc_step(false,16,&moved) && !moved && calls==amc_calls);
        CHECK(amc_step(true,0,&moved) && !moved && calls==amc_calls);
        amc_result=S5L8920_CDMA_READY;
        for(unsigned work=0;work<32 && total<50;work++) {
            CHECK(amc_step(true,ack?7u:16u,&moved) && moved>0);
            if(!moved)break;
            total+=moved;
            if(total==20 || total==50)pending=true;
            CHECK(cdma.amc.state.remaining==0x40000000u-total && s5l8920_cdma_amc_irq(&cdma)==pending);
            if(ack && total==20){CHECK(amc_write_reg(0,0x190018));pending=false;}
        }
        memcpy(expected_ram+0x2000,amc_source,20);memcpy(expected_ram+0x3000,amc_source+20,9);
        memcpy(expected_ram+0x3020,amc_source+29,21);
        CHECK(total==50 && amc_cursor==50 && !memcmp(expected_ram,amc_ram,sizeof amc_ram));
        CHECK(cdma.amc.state.descriptor==AMC_RAM+((head+3)&127u)*32 &&
            cdma.amc.state.address==AMC_RAM+0x3020+21 && s5l8920_cdma_amc_irq(&cdma));
        before=cdma;calls=amc_calls;
        CHECK(!amc_step(true,16,&moved) && !moved && !amc_write_reg(0,0x19));
        CHECK(!memcmp(&before,&cdma,sizeof cdma) && calls==amc_calls);
    }
}
static void amc_publication_guards(void) {
    for(unsigned fault=0;fault<18;fault++) {
        amc_fixture(127,0);CHECK(amc_write_reg(0,0x19));
        unsigned moved=99;CHECK(amc_step(true,7,&moved) && moved==7);
        amc_publish(0,2,0x3000);
        uint8_t valid[sizeof amc_ram],invalid[sizeof amc_ram];memcpy(valid,amc_ram,sizeof valid);
        switch(fault) {
        case 0:amc_put(127*32,AMC_RAM+32);break;
        case 1:amc_put(127*32+4,3);break;
        case 2:amc_put(127*32+8,AMC_RAM+0x2020);break;
        case 3:amc_put(127*32+12,21);break;
        case 4:amc_put(0,AMC_RAM+64);break; /* Published tail link changed. */
        case 5:amc_put(4,0);break;
        case 6:amc_put(36,3);break;
        case 7:amc_put(4,0x103);break;
        case 8:amc_put(12,0);break;
        case 9:amc_put(32,0x70000000u+127*32);break;
        case 10:amc_put(8,0x70000000u+127*32);break;
        case 11:amc_put(40,AMC_RAM+64);break;
        case 12:amc_put(8,0xfffffffcu);break;
        case 13:amc_deny=AMC_RAM+0x3000;break;
        case 14:amc_deny=AMC_RAM+0x2000;break;
        case 15:amc_deny=AMC_RAM+64;break;
        case 16:cdma.amc.state.remaining=43;break;
        case 17:amc_put(68,0xdead);break;
        }
        s5l8920_cdma_t before=cdma;memcpy(invalid,amc_ram,sizeof invalid);unsigned calls=amc_calls;
        CHECK(!amc_write_reg(0,0x19));
        CHECK(!memcmp(&before,&cdma,sizeof cdma) && !memcmp(invalid,amc_ram,sizeof invalid) &&
            amc_cursor==7 && amc_calls==calls);
        memcpy(amc_ram,valid,sizeof valid);amc_deny=0;cdma.amc.state.remaining=0x40000000u-7;
        CHECK(amc_write_reg(0,0x19) && cdma.amc.offset==7 && amc_cursor==7);
        before=cdma;
        CHECK(!amc_write_reg(0,2) && !amc_write_reg(0,4) && !amc_write_reg(4,0x300d8) &&
            !amc_write_reg(8,0x8480002c) && !amc_write_reg(12,0x40000000) && !amc_write_reg(20,AMC_RAM));
        CHECK(!memcmp(&before,&cdma,sizeof cdma) && amc_calls==calls);
    }
}
static void amc_publication_retirement(void) {
    for(unsigned head=0;head<128;head+=127) {
        amc_fixture(head,2);CHECK(amc_write_reg(0,0x19));unsigned moved=99;
        CHECK(amc_step(true,16,&moved) && moved==16);
        CHECK(amc_step(true,16,&moved) && moved==4);
        CHECK(amc_step(true,16,&moved) && moved==7);
        CHECK(amc_step(true,3,&moved) && moved==3);
        CHECK(cdma.amc.index==2 && cdma.amc.offset==3);
        s5l8920_cdma_channel_t before=cdma.amc.state;
        amc_publish((head+3)&127u,1,0x3000);
        uint8_t want[sizeof amc_ram];memcpy(want,amc_ram,sizeof want);
        CHECK(amc_write_reg(0,0x19) && cdma.amc.index==0 && cdma.amc.offset==3 && cdma.amc.count==2);
        CHECK(cdma.amc.state.address==before.address && cdma.amc.state.descriptor==before.descriptor &&
            cdma.amc.state.remaining==before.remaining);
        CHECK(amc_step(true,16,&moved) && moved==4 && s5l8920_cdma_amc_irq(&cdma));
        CHECK(amc_step(true,16,&moved) && moved==9 && s5l8920_cdma_amc_irq(&cdma));
        memcpy(want+0x2040+3,amc_source+30,4);memcpy(want+0x3000,amc_source+34,9);
        CHECK(amc_cursor==43 && !memcmp(want,amc_ram,sizeof want));
    }
    /* The remaining budget includes only the unconsumed part of the first
     * descriptor. A valid append leaves exactly one unspent byte at the tail. */
    amc_fixture(0,0);CHECK(amc_write_reg(12,51));CHECK(amc_write_reg(0,0x19));
    unsigned moved=99;CHECK(amc_step(true,7,&moved) && moved==7);
    amc_publish(1,2,0x3000);CHECK(amc_write_reg(0,0x19) && cdma.amc.state.remaining==44);
    for(unsigned i=0;i<4;i++)CHECK(amc_step(true,16,&moved));
    CHECK(amc_cursor==50 && cdma.amc.state.remaining==1 && !amc_step(true,16,&moved) && !moved);
    /* No slot is reclaimed while its payload is only partly consumed. */
    /* Retain the original tail link by constructing that link before the first
     * GO below, so refusal tests capacity rather than a changed link. */
    amc_fixture(127,126);amc_put(126*32,AMC_RAM+0x1000);CHECK(amc_write_reg(0,0x19));
    CHECK(amc_step(true,7,&moved) && moved==7);
    amc_put(126*32+4,0x303);amc_put(126*32+8,AMC_RAM+0x5000);amc_put(126*32+12,9);
    amc_put(0x1000,AMC_RAM+0x1020);amc_put(0x1004,0);amc_put(0x1008,0);amc_put(0x100c,0);
    s5l8920_cdma_t before=cdma;unsigned calls=amc_calls;
    CHECK(!amc_write_reg(0,0x19) && !memcmp(&before,&cdma,sizeof cdma) && amc_calls==calls);
    /* In the real ring, publishing the new zero tail can reuse the old first
     * entry only after that entry has been consumed. */
    amc_fixture(127,126);CHECK(amc_write_reg(0,0x19));
    CHECK(amc_step(true,16,&moved) && moved==16);CHECK(amc_step(true,16,&moved) && moved==4);
    s5l8920_cdma_channel_t progress=cdma.amc.state;
    amc_publish(126,1,0x5000);CHECK(amc_write_reg(0,0x19));
    CHECK(cdma.amc.count==127 && cdma.amc.index==0 && cdma.amc.offset==0 &&
        cdma.amc.state.remaining==progress.remaining && cdma.amc.state.address==progress.address &&
        cdma.amc.state.descriptor==progress.descriptor);
}
static void amc_repeated_publication(void) {
    amc_fixture(127,0);CHECK(amc_write_reg(0,0x19));
    unsigned total=0,slot=127,data=0x2000,length=20;
    for(unsigned command=0;command<140;command++) {
        uint8_t want[sizeof amc_ram];memcpy(want,amc_ram,sizeof want);
        memcpy(want+data,amc_source+total,length);
        unsigned copied=0,moved=99;
        for(unsigned work=0;work<4 && copied<length;work++) {
            CHECK(amc_step(true,7,&moved) && moved>0);
            if(!moved)break;
            copied+=moved;
        }
        total+=copied;
        CHECK(copied==length && total==amc_cursor && !memcmp(want,amc_ram,sizeof want));
        CHECK(s5l8920_cdma_amc_irq(&cdma) && cdma.amc.state.remaining==0x40000000u-total &&
            cdma.amc.state.descriptor==AMC_RAM+((slot+1)&127u)*32);
        CHECK(amc_write_reg(0,0x190018) && !s5l8920_cdma_amc_irq(&cdma));
        if(command==139)break;
        slot=(slot+1)&127u;data=0x3000+(command&3u)*32;length=9;
        s5l8920_cdma_channel_t before=cdma.amc.state;unsigned calls=amc_calls;
        amc_publish(slot,1,data);bool accepted=amc_write_reg(0,0x19);CHECK(accepted);if(!accepted)break;
        CHECK(cdma.amc.count==1 && cdma.amc.index==0 && !cdma.amc.offset && calls==amc_calls &&
            cdma.amc.state.remaining==before.remaining && cdma.amc.state.address==before.address &&
            cdma.amc.state.descriptor==before.descriptor);
    }
    CHECK(total==1271 && cdma.amc.index==1 && cdma.amc.count==1);
}
int main(void) {
    vectors();guards();aliases_and_irqs();lifetime_and_gates();s5l8920_free(&board);
    CHECK(!s5l8920_cdma_board_service(&board));
    CHECK(!s5l8920_cdma_attach(&board,&cdma));
    nand_register_setup();nand_transfers();nand_guards();nand_chain_limits();s5l8920_free(&board);
    amc_register_setup();amc_transfers();amc_guards();amc_limits();
    amc_single_command_setup();
    amc_live_publication();
    amc_publication_progress();amc_publication_guards();
    amc_publication_retirement();amc_repeated_publication();
    printf("CDMA AES: %u passed, %u failed\n",passed,failed);return failed?1:0;
}
