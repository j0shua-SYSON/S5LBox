/* Documented device dialogue and controller integration, no target firmware.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920.h"
#include <stdio.h>
#include <string.h>

static unsigned passed,failed;
#define CHECK(x) do { if(x)++passed; else { if(failed<24u) { \
    printf("FAIL %s:%d: %s\n",__func__,__LINE__,#x); } ++failed; } } while(0)
static s5l8920_t board,board_before;
static bool fresh(lis331dl_t *d,bool sdo) {
    bool ok=lis331dl_init(d,sdo,1234567u);CHECK(ok);return ok;
}
static uint8_t read_reg(const lis331dl_t *d,uint8_t reg) {
    uint8_t value=0xa5;
    CHECK(lis331dl_read(d,d->sdo_high?0x1d:0x1c,reg,&value,1));return value;
}
static void write_reg(lis331dl_t *d,uint8_t reg,uint8_t value) {
    CHECK(lis331dl_write(d,d->sdo_high?0x1d:0x1c,reg,&value,1));
}
static void test_init_and_address(void) {
    lis331dl_t d,before;memset(&d,0xa5,sizeof d);before=d;
    CHECK(!lis331dl_init(NULL,true,1));CHECK(!lis331dl_init(&d,true,0));
    CHECK(!memcmp(&d,&before,sizeof d));
    for(unsigned sdo=0;sdo<2;++sdo) {
        if(!fresh(&d,sdo!=0))continue;
        CHECK(read_reg(&d,0x0f)==0x3b && read_reg(&d,0x20)==7);
        CHECK(read_reg(&d,0x21)==0 && read_reg(&d,0x22)==0);
        before=d;
        for(unsigned a=0;a<256;++a) {
            uint8_t value=0xa5;
            bool expected=a==(sdo?0x1du:0x1cu);
            CHECK(lis331dl_read(&d,(uint8_t)a,0x0f,&value,1)==expected);
            CHECK(value==(expected?0x3b:0xa5));CHECK(!memcmp(&d,&before,sizeof d));
        }
    }
}
static void test_registers_and_atomicity(void) {
    lis331dl_t d;if(!fresh(&d,true))return;
    const uint8_t registers[]={0x20,0x21,0x22,0x30,0x32,0x33,0x34,0x36,0x37};
    for(unsigned n=0;n<sizeof registers;++n) {
        uint8_t reg=registers[n],value=reg==0x21?0x9f:0xa5;
        write_reg(&d,reg,value);CHECK(read_reg(&d,reg)==value);
    }
    lis331dl_t before=d;
    for(unsigned reg=0;reg<128;++reg) {
        bool known=reg==0x0f;
        for(unsigned n=0;n<sizeof registers;++n)known|=reg==registers[n];
        uint8_t value=0xa5;
        CHECK(lis331dl_read(&d,0x1d,(uint8_t)reg,&value,1)==known);
        if(!known)CHECK(value==0xa5);
        CHECK(!memcmp(&d,&before,sizeof d));
    }
    uint8_t values[]={0x87,0x80,0xc0},out[128];memset(out,0x5a,sizeof out);
    CHECK(lis331dl_write(&d,0x1d,0xa0,values,3));
    CHECK(lis331dl_read(&d,0x1d,0xa0,out,3) && !memcmp(out,values,3));
    CHECK(lis331dl_read(&d,0x1d,0x0f,out,sizeof out));
    for(unsigned n=0;n<sizeof out;++n)CHECK(out[n]==0x3b);
    uint8_t repeated[]={0x07,0x47,0x87};
    CHECK(lis331dl_write(&d,0x1d,0x20,repeated,3));CHECK(read_reg(&d,0x20)==0x87);
    before=d;memset(out,0x5a,sizeof out);
    CHECK(!lis331dl_read(&d,0x1d,0xa0,out,4));
    CHECK(!lis331dl_write(&d,0x1d,0xa0,out,4));
    CHECK(!memcmp(&d,&before,sizeof d));
    for(unsigned n=0;n<sizeof out;++n)CHECK(out[n]==0x5a);
    values[1]=0x20;CHECK(!lis331dl_write(&d,0x1d,0xa0,values,3));
    CHECK(!memcmp(&d,&before,sizeof d));
    CHECK(!lis331dl_write(&d,0x1d,0x0f,values,1));
    CHECK(!lis331dl_write(&d,0x1d,0xb7,values,2));
    CHECK(!memcmp(&d,&before,sizeof d));
    CHECK(!lis331dl_read(&d,0x1d,0x0f,NULL,1));
    CHECK(!lis331dl_write(&d,0x1d,0x20,NULL,1));
    CHECK(!lis331dl_read(&d,0x1d,0x0f,out,0));
    CHECK(!lis331dl_read(&d,0x1d,0x0f,out,SIZE_MAX));
    CHECK(!lis331dl_write(&d,0x1d,0x20,values,0));
    CHECK(!lis331dl_write(&d,0x1d,0x20,values,SIZE_MAX));
    CHECK(!lis331dl_read(NULL,0x1d,0x0f,out,1));
    CHECK(!lis331dl_write(NULL,0x1d,0x20,values,1));
    CHECK(!lis331dl_advance(NULL,1));CHECK(!memcmp(&d,&before,sizeof d));
    memset(&d,0,sizeof d);before=d;
    CHECK(!lis331dl_read(&d,0x1d,0x0f,out,1));
    CHECK(!lis331dl_write(&d,0x1d,0x20,values,1));
    CHECK(!lis331dl_advance(&d,1));CHECK(!memcmp(&d,&before,sizeof d));
}
static void test_reboot_and_unknown_samples(void) {
    lis331dl_t d;if(!fresh(&d,true))return;
    write_reg(&d,0x20,0x47);write_reg(&d,0x22,0xc0);write_reg(&d,0x32,0x93);
    write_reg(&d,0x21,0x40);CHECK(d.reboot_remaining==1234567u);
    lis331dl_t before=d;
    for(unsigned n=0;n<100;++n)CHECK(read_reg(&d,0x21)==0x40);
    CHECK(!memcmp(&d,&before,sizeof d));
    uint8_t value=0;
    CHECK(!lis331dl_write(&d,0x1d,0x21,&value,1));
    CHECK(!memcmp(&d,&before,sizeof d));
    CHECK(lis331dl_advance(&d,1234566u));CHECK(read_reg(&d,0x21)==0x40);
    CHECK(lis331dl_advance(&d,0));CHECK(d.reboot_remaining==1);
    CHECK(lis331dl_advance(&d,1));CHECK(read_reg(&d,0x21)==0);
    CHECK(read_reg(&d,0x20)==0x47 && read_reg(&d,0x22)==0xc0 && read_reg(&d,0x32)==0x93);
    write_reg(&d,0x21,0x40);CHECK(lis331dl_advance(&d,UINT64_MAX));
    CHECK(read_reg(&d,0x21)==0 && !d.reboot_remaining);
    const uint8_t unknown[]={0x23,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x31,0x35,0x39};
    before=d;
    for(unsigned n=0;n<sizeof unknown;++n) {
        value=0xa5;CHECK(!lis331dl_read(&d,0x1d,unknown[n],&value,1));
        CHECK(value==0xa5 && !memcmp(&d,&before,sizeof d));
    }
}
static void write32(uint32_t address,uint32_t value) {
    board.bus.write32(&board,address,value);CHECK(!board.bus_failure.reason);
}
static uint64_t request(unsigned bus,uint8_t address,uint8_t sub,bool write,const uint8_t *data,unsigned n) {
    uint32_t base=S5L8920_I2C_BASE+bus*S5L8920_I2C_STRIDE;
    write32(base+12,0x37);write32(base+8,0xf0);write32(base,address);
    write32(base+0x10,sub);write32(base+0x14,0);write32(base+0x18,n);
    if(write)for(unsigned i=0;i<n;++i)write32(base+0x20,data[i]);
    write32(base+0x24,write?5:4);CHECK(board.i2c[bus].active);
    return board.i2c[bus].sequence;
}
static void unchanged(lis331dl_t *d,const lis331dl_t *before) {
    CHECK(!memcmp(d,before,sizeof *d));CHECK(!memcmp(&board,&board_before,sizeof board));
}
static void test_board(void) {
    lis331dl_t d;if(!fresh(&d,true))return;
    CHECK(s5l8920_init(&board));if(!board.ram)return;
    for(unsigned bus=0;bus<3;++bus) {
        CHECK(s5l8920_reset(&board));
        uint32_t base=S5L8920_I2C_BASE+bus*S5L8920_I2C_STRIDE;
        uint32_t line=1u<<(S5L8920_I2C0_IRQ-bus);
        write32(S5L8920_VIC_BASE+PL192_INTENABLE,line);
        uint64_t sequence=request(bus,0x1d,0x0f,false,NULL,1);
        board_before=board;lis331dl_t before=d;
        CHECK(!s5l8920_lis331dl_service(&board,bus,sequence+1,&d));unchanged(&d,&before);
        CHECK(!s5l8920_lis331dl_service(&board,3,sequence,&d));unchanged(&d,&before);
        CHECK(!s5l8920_lis331dl_service(NULL,bus,sequence,&d));unchanged(&d,&before);
        CHECK(!s5l8920_lis331dl_service(&board,bus,sequence,NULL));unchanged(&d,&before);
        CHECK(s5l8920_lis331dl_service(&board,bus,sequence,&d));
        CHECK(!board.i2c[bus].active && board.i2c[bus].status==0x10 && board.cpu.irq_line);
        CHECK(board.bus.read32(&board,base+0x20)==0x3b && !board.bus_failure.reason);
        CHECK(!memcmp(board.cpu.r,board_before.cpu.r,sizeof board.cpu.r));
        CHECK(board.cpu.cycles==board_before.cpu.cycles && !memcmp(&d,&before,sizeof d));
        CHECK(s5l8920_set_irq(&board,S5L8920_I2C0_IRQ-bus,true));
        write32(base+12,0x10);CHECK(board.cpu.irq_line);
        CHECK(s5l8920_set_irq(&board,S5L8920_I2C0_IRQ-bus,false));CHECK(!board.cpu.irq_line);
        uint8_t value=0xc0;
        sequence=request(bus,0x1d,0x22,true,&value,1);
        CHECK(s5l8920_lis331dl_service(&board,bus,sequence,&d));CHECK(read_reg(&d,0x22)==0xc0);
        sequence=request(bus,0x74,0x0f,false,NULL,1);before=d;board_before=board;
        CHECK(!s5l8920_lis331dl_service(&board,bus,sequence,&d));unchanged(&d,&before);
        CHECK(s5l8920_reset(&board));CHECK(!memcmp(&d,&before,sizeof d));
        sequence=request(bus,0x1d,0xa0,false,NULL,4);before=d;board_before=board;
        CHECK(!s5l8920_lis331dl_service(&board,bus,sequence,&d));unchanged(&d,&before);
        CHECK(s5l8920_reset(&board));
        const uint8_t invalid[]={0x47,0x20,0xc0};
        sequence=request(bus,0x1d,0xa0,true,invalid,3);before=d;board_before=board;
        CHECK(!s5l8920_lis331dl_service(&board,bus,sequence,&d));unchanged(&d,&before);
    }
    lis331dl_t before=d;s5l8920_free(&board);CHECK(!memcmp(&d,&before,sizeof d));
}
int main(void) {
    test_init_and_address();test_registers_and_atomicity();
    test_reboot_and_unknown_samples();test_board();
    printf("lis331dl: %u passed, %u failed\n",passed,failed);return failed?1:0;
}
