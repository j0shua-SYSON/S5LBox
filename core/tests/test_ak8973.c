/* Device protocol and controller integration, with test-only calibration bytes.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920.h"
#include <stdio.h>
#include <string.h>
static unsigned passed,failed;
#define CHECK(x) do { if(x)++passed; else { if(failed<24u) { \
    printf("FAIL %s:%d: %s\n",__func__,__LINE__,#x); } ++failed; } } while(0)
static s5l8920_t board,board_before;
/* Deliberately distinctive host fixtures, never target calibration evidence. */
static const uint8_t fixture[7]={0x42,0x93,0xa4,0xb5,0xc6,0xd7,0xe8};
static bool fresh(ak8973_t *d,uint8_t known) {
    bool ok=ak8973_init(d,true,false,known?fixture:NULL,known);CHECK(ok);return ok;
}
static uint8_t read_reg(const ak8973_t *d,uint8_t reg) {
    uint8_t value=0xa5;CHECK(ak8973_read(d,d->address,reg,&value,1));return value;
}
static void write_reg(ak8973_t *d,uint8_t reg,uint8_t value) {
    CHECK(ak8973_write(d,d->address,reg,&value,1));
}
static void test_address_and_reset(void) {
    ak8973_t d,before;memset(&d,0xa5,sizeof d);before=d;
    CHECK(!ak8973_init(NULL,0,0,NULL,0));
    CHECK(!ak8973_init(&d,0,0,NULL,1));CHECK(!ak8973_init(&d,0,0,fixture,0x80));
    CHECK(!memcmp(&d,&before,sizeof d));
    for(unsigned pins=0;pins<4;++pins) {
        bool ok=ak8973_init(&d,(pins&2)!=0,(pins&1)!=0,fixture,0x7f);CHECK(ok);
        if(!ok)continue;
        CHECK(read_reg(&d,0xe0)==3 && read_reg(&d,0xc0)==0);
        CHECK(d.powerdown_remaining==100000 && !d.eeprom_remaining);
        before=d;
        for(unsigned a=0;a<256;++a) {
            uint8_t value=0xa5;bool expected=a==0x1cu+pins;
            CHECK(ak8973_read(&d,(uint8_t)a,0xe0,&value,1)==expected);
            CHECK(value==(expected?3:0xa5));CHECK(!memcmp(&d,&before,sizeof d));
        }
        write_reg(&d,0xe1,0x96);write_reg(&d,0xe4,0xf7);
        CHECK(read_reg(&d,0xe1)==0x96 && read_reg(&d,0xe4)==7);
        CHECK(ak8973_reset(&d));CHECK(d.address==0x1cu+pins);
        CHECK(!memcmp(d.eeprom,fixture,7) && d.eeprom_known==0x7f);
        CHECK(read_reg(&d,0xe1)==0 && read_reg(&d,0xe4)==0);
        CHECK(read_reg(&d,0xe0)==3 && d.powerdown_remaining==100000);
        uint8_t reset_data[10];memset(reset_data,0xa5,sizeof reset_data);
        CHECK(ak8973_read(&d,d.address,0xc0,reset_data,sizeof reset_data));
        for(unsigned n=0;n<sizeof reset_data;++n)CHECK(reset_data[n]==0);
    }
}
static void test_modes_timing_and_unknown(void) {
    ak8973_t d;if(!fresh(&d,0))return;
    uint8_t value=2,out[7];memset(out,0xa5,sizeof out);ak8973_t before=d;
    CHECK(!ak8973_write(&d,0x1e,0xe0,&value,1));CHECK(!memcmp(&d,&before,sizeof d));
    CHECK(ak8973_advance(&d,99999));CHECK(d.powerdown_remaining==1);
    before=d;
    for(unsigned n=0;n<100;++n)CHECK(read_reg(&d,0xc0)==0);
    CHECK(!memcmp(&d,&before,sizeof d));CHECK(ak8973_advance(&d,0));
    CHECK(!ak8973_write(&d,0x1e,0xe0,&value,1));
    CHECK(ak8973_advance(&d,1));write_reg(&d,0xe0,2);
    CHECK(d.eeprom_remaining==300000 && !d.powerdown_remaining);
    CHECK(read_reg(&d,0xe0)==2 && read_reg(&d,0xc0)==0);
    before=d;CHECK(!ak8973_read(&d,0x1e,0x66,out,1));
    CHECK(!ak8973_write(&d,0x1e,0xe1,&value,1));CHECK(!memcmp(&d,&before,sizeof d));
    CHECK(ak8973_advance(&d,UINT64_MAX));CHECK(!d.eeprom_remaining);
    before=d;CHECK(!ak8973_read(&d,0x1e,0x66,out,1));
    CHECK(!memcmp(&d,&before,sizeof d) && out[0]==0xa5);
    write_reg(&d,0xe0,0xaa);CHECK(read_reg(&d,0xc0)==2);
    before=d;CHECK(!ak8973_write(&d,0x1e,0x66,&value,1));
    CHECK(!ak8973_read(&d,0x1e,0x66,out,1));CHECK(!memcmp(&d,&before,sizeof d));
    write_reg(&d,0xe0,3);CHECK(read_reg(&d,0xc0)==0);
    CHECK(d.powerdown_remaining==100000 && !d.eeprom_remaining);
    CHECK(ak8973_advance(&d,UINT64_MAX));
    const uint8_t unsupported[]={0,1,4,7};before=d;
    for(unsigned n=0;n<sizeof unsupported;++n) {
        CHECK(!ak8973_write(&d,0x1e,0xe0,unsupported+n,1));
        CHECK(!memcmp(&d,&before,sizeof d));
    }
    CHECK(!ak8973_read(&d,0x1e,0x66,out,1));CHECK(!ak8973_read(&d,0x1e,0x5d,out,1));
}
static void test_calibration_and_rings(void) {
    ak8973_t d;if(!fresh(&d,0x70))return;
    CHECK(ak8973_advance(&d,100000));write_reg(&d,0xe0,2);
    uint8_t out[128];memset(out,0xa5,sizeof out);
    CHECK(ak8973_advance(&d,299999));ak8973_t before=d;
    CHECK(!ak8973_read(&d,0x1e,0x66,out,3));CHECK(!memcmp(&d,&before,sizeof d));
    CHECK(ak8973_advance(&d,1));
    CHECK(ak8973_read(&d,0x1e,0x66,out,3) && !memcmp(out,fixture+4,3));
    memset(out,0xa5,sizeof out);before=d;
    CHECK(!ak8973_read(&d,0x1e,0x66,out,4));CHECK(!memcmp(&d,&before,sizeof d));
    for(unsigned n=0;n<sizeof out;++n)CHECK(out[n]==0xa5);
    if(!fresh(&d,0x7f))return;
    CHECK(ak8973_advance(&d,100000));write_reg(&d,0xe0,2);
    CHECK(ak8973_advance(&d,300000));
    CHECK(ak8973_read(&d,0x1e,0x68,out,sizeof out));
    for(unsigned n=0;n<sizeof out;++n)CHECK(out[n]==fixture[(n+6)%7]);
    write_reg(&d,0xe0,3);
    /* Original initialization copies full EEPROM bytes into four-bit gains. */
    CHECK(ak8973_write(&d,0x1e,0xe4,fixture+4,3));
    CHECK(read_reg(&d,0xe4)==6 && read_reg(&d,0xe5)==7 && read_reg(&d,0xe6)==8);
    const uint8_t sequence[]={0x89,3,0xa2,0xb3,0xc4,0xd5,0xe6,0xf7};
    CHECK(ak8973_write(&d,0x1e,0xe6,sequence,sizeof sequence));
    const uint8_t expected[]={7,3,0xa2,0xb3,0xc4,5,6};
    CHECK(ak8973_read(&d,0x1e,0xe6,out,sizeof out));
    for(unsigned n=0;n<sizeof out;++n)CHECK(out[n]==expected[n%7]);
    before=d;const uint8_t bad[]={0x66,0};
    CHECK(!ak8973_write(&d,0x1e,0xe6,bad,2));CHECK(!memcmp(&d,&before,sizeof d));
    for(unsigned reg=0;reg<256;++reg) {
        uint8_t v=0xa5;bool known=(reg>=0xc0 && reg<=0xc4)||(reg>=0xe0 && reg<=0xe6);
        CHECK(ak8973_read(&d,0x1e,(uint8_t)reg,&v,1)==known);
        if(!known)CHECK(v==0xa5);
        CHECK(!memcmp(&d,&before,sizeof d));
    }
    CHECK(!ak8973_read(NULL,0x1e,0xc0,out,1));CHECK(!ak8973_reset(NULL));
    CHECK(!ak8973_read(&d,0x1e,0xc0,NULL,1));CHECK(!ak8973_read(&d,0x1e,0xc0,out,0));
    CHECK(!ak8973_read(&d,0x1e,0xc0,out,SIZE_MAX));
    CHECK(!ak8973_write(&d,0x1e,0xe1,NULL,1));CHECK(!ak8973_write(&d,0x1e,0xe1,out,0));
    CHECK(!ak8973_write(&d,0x1e,0xe1,out,SIZE_MAX));CHECK(!ak8973_advance(NULL,1));
    CHECK(!memcmp(&d,&before,sizeof d));
    memset(&d,0,sizeof d);before=d;
    CHECK(!ak8973_read(&d,0x1e,0xc0,out,1));CHECK(!ak8973_write(&d,0x1e,0xe1,out,1));
    CHECK(!ak8973_advance(&d,1));CHECK(!ak8973_reset(&d));CHECK(!memcmp(&d,&before,sizeof d));
}
static void write32(uint32_t address,uint32_t value) {
    board.bus.write32(&board,address,value);CHECK(!board.bus_failure.reason);
}
static uint64_t request(unsigned bus,uint8_t address,uint8_t sub,bool write,const uint8_t *data,unsigned n) {
    uint32_t base=S5L8920_I2C_BASE+bus*S5L8920_I2C_STRIDE;
    write32(base+12,0x37);write32(base+8,0xf0);write32(base,address);
    write32(base+0x10,sub);write32(base+0x14,0);write32(base+0x18,n);
    if(write)for(unsigned i=0;i<n;++i)write32(base+0x20,data[i]);
    write32(base+0x24,write?5:4);CHECK(board.i2c[bus].active);return board.i2c[bus].sequence;
}
static void unchanged(const ak8973_t *d,const ak8973_t *before) {
    CHECK(!memcmp(d,before,sizeof *d));CHECK(!memcmp(&board,&board_before,sizeof board));
}
static void test_board(void) {
    ak8973_t d;if(!fresh(&d,0))return;
    CHECK(s5l8920_init(&board));if(!board.ram)return;
    for(unsigned bus=0;bus<3;++bus) {
        CHECK(s5l8920_reset(&board));CHECK(ak8973_reset(&d));
        uint32_t base=S5L8920_I2C_BASE+bus*S5L8920_I2C_STRIDE;
        uint32_t line=1u<<(S5L8920_I2C0_IRQ-bus);
        write32(S5L8920_VIC_BASE+PL192_INTENABLE,line);
        uint64_t seq=request(bus,0x1e,0xc0,false,NULL,1);
        ak8973_t before=d;board_before=board;
        CHECK(!s5l8920_ak8973_service(&board,bus,seq+1,&d));unchanged(&d,&before);
        CHECK(!s5l8920_ak8973_service(&board,3,seq,&d));unchanged(&d,&before);
        CHECK(!s5l8920_ak8973_service(NULL,bus,seq,&d));unchanged(&d,&before);
        CHECK(!s5l8920_ak8973_service(&board,bus,seq,NULL));unchanged(&d,&before);
        CHECK(s5l8920_ak8973_service(&board,bus,seq,&d));
        CHECK(board.i2c[bus].status==0x10 && board.cpu.irq_line && !board.i2c[bus].active);
        CHECK(board.bus.read32(&board,base+0x20)==0 && !board.bus_failure.reason);
        CHECK(!memcmp(board.cpu.r,board_before.cpu.r,sizeof board.cpu.r));
        CHECK(board.cpu.cycles==board_before.cpu.cycles && !memcmp(&d,&before,sizeof d));
        CHECK(s5l8920_set_irq(&board,S5L8920_I2C0_IRQ-bus,true));write32(base+12,0x10);
        CHECK(board.cpu.irq_line);CHECK(s5l8920_set_irq(&board,S5L8920_I2C0_IRQ-bus,false));
        CHECK(!board.cpu.irq_line);
        CHECK(ak8973_advance(&d,100000));uint8_t mode=2;
        seq=request(bus,0x1e,0xe0,true,&mode,1);
        CHECK(s5l8920_ak8973_service(&board,bus,seq,&d));CHECK(read_reg(&d,0xe0)==2);
        CHECK(ak8973_advance(&d,300000));
        seq=request(bus,0x1e,0x66,false,NULL,1);before=d;board_before=board;
        CHECK(!s5l8920_ak8973_service(&board,bus,seq,&d));unchanged(&d,&before);
        CHECK(s5l8920_reset(&board));CHECK(!memcmp(&d,&before,sizeof d));
        seq=request(bus,0x1f,0xc0,false,NULL,1);board_before=board;
        CHECK(!s5l8920_ak8973_service(&board,bus,seq,&d));unchanged(&d,&before);
        CHECK(s5l8920_reset(&board));CHECK(ak8973_reset(&d));
        const uint8_t invalid[]={0x55,0};seq=request(bus,0x1e,0xe6,true,invalid,2);
        before=d;board_before=board;
        CHECK(!s5l8920_ak8973_service(&board,bus,seq,&d));unchanged(&d,&before);
    }
    ak8973_t before=d;s5l8920_free(&board);CHECK(!memcmp(&d,&before,sizeof d));
}
int main(void) {
    test_address_and_reset();test_modes_timing_and_unknown();test_calibration_and_rings();test_board();
    printf("ak8973: %u passed, %u failed\n",passed,failed);return failed?1:0;
}
