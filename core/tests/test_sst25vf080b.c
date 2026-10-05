/* Command-sequence and storage-boundary tests; no target firmware/image.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "sst25vf080b.h"
#include "s5l8920_spi.h"
#include "s5l8920_spi_flash.h"
#include <stdio.h>
#include <string.h>

static unsigned passed, failed;
#define CHECK(x) do { if (x) ++passed; else { if (failed<24u) { \
    printf("FAIL %s:%d: %s\n",__func__,__LINE__,#x); } ++failed; } } while (0)
static uint8_t storage[SST25VF080B_SIZE+2u];
static uint8_t *const image=storage+1;
static const sst25vf080b_timing_t timing={7000u,18000000u,19000000u,20000000u,35000000u};

static bool fresh(sst25vf080b_t *f) {
    memset(f,0,sizeof *f); memset(storage,0xff,sizeof storage);
    storage[0]=0x3c; storage[sizeof storage-1u]=0xa5;
    bool ok=sst25vf080b_init(f,image,SST25VF080B_SIZE,&timing,true,true);
    CHECK(ok); if (!ok) return false;
    CHECK(sst25vf080b_advance(f,100000u));
    return true;
}
static void pins(sst25vf080b_t *f,bool ce) {
    CHECK(sst25vf080b_pins(f,ce,f->wp_high,f->hold_high));
}
static sst25vf080b_output_t byte(sst25vf080b_t *f,uint8_t v) {
    sst25vf080b_output_t out={0xa5,0x5a};
    CHECK(sst25vf080b_transfer(f,v,&out)); return out;
}
static void command(sst25vf080b_t *f,const uint8_t *cmd,size_t n) {
    pins(f,false);
    for (size_t i=0;i<n;++i) CHECK(byte(f,cmd[i]).driven==0u);
    pins(f,true);
}
static void opcode(sst25vf080b_t *f,uint8_t cmd) { command(f,&cmd,1u); }
static uint8_t status(sst25vf080b_t *f) {
    pins(f,false); CHECK(byte(f,5u).driven==0u);
    sst25vf080b_output_t out=byte(f,0xff); CHECK(out.driven==0xff);
    pins(f,true); return out.value;
}
static void unprotect(sst25vf080b_t *f,uint8_t sr) {
    const uint8_t cmd[]={1u,sr}; opcode(f,0x50); command(f,cmd,sizeof cmd);
}
static void addressed(sst25vf080b_t *f,uint8_t op,uint32_t addr) {
    pins(f,false); CHECK(byte(f,op).driven==0);
    CHECK(byte(f,(uint8_t)(addr>>16)).driven==0);
    CHECK(byte(f,(uint8_t)(addr>>8)).driven==0);
    CHECK(byte(f,(uint8_t)addr).driven==0);
}
static void program(sst25vf080b_t *f,uint32_t addr,uint8_t v) {
    addressed(f,2u,addr); CHECK(byte(f,v).driven==0); pins(f,true);
}
static void refusal(sst25vf080b_t *f,uint8_t v) {
    sst25vf080b_t before=*f;
    sst25vf080b_output_t out={0xa5,0x5a};
    CHECK(!sst25vf080b_transfer(f,v,&out));
    CHECK(!memcmp(f,&before,sizeof *f)); CHECK(out.value==0xa5 && out.driven==0x5a);
}

static void test_power_and_inputs(void) {
    sst25vf080b_t f,before; memset(&f,0xa5,sizeof f); before=f;
    CHECK(!sst25vf080b_init(&f,image,SST25VF080B_SIZE-1u,&timing,true,true));
    CHECK(!memcmp(&f,&before,sizeof f));
    CHECK(!sst25vf080b_init(NULL,image,SST25VF080B_SIZE,&timing,true,true));
    CHECK(!sst25vf080b_init(&f,NULL,SST25VF080B_SIZE,&timing,true,true));
    CHECK(!sst25vf080b_init(&f,image,SST25VF080B_SIZE,NULL,true,true));
    sst25vf080b_timing_t bad=timing; bad.program_ns=0;
    CHECK(!sst25vf080b_init(&f,image,SST25VF080B_SIZE,&bad,true,true));
    bad=timing;bad.chip_ns=50000001u;
    CHECK(!sst25vf080b_init(&f,image,SST25VF080B_SIZE,&bad,true,true));
    memset(image,0x69,SST25VF080B_SIZE);
    bool ok=sst25vf080b_init(&f,image,SST25VF080B_SIZE,&timing,false,true);
    CHECK(ok); if (!ok) return;
    CHECK(image[0]==0x69 && image[SST25VF080B_SIZE-1u]==0x69);
    before=f; CHECK(!sst25vf080b_pins(&f,false,false,true));
    CHECK(!memcmp(&f,&before,sizeof f));
    CHECK(sst25vf080b_advance(&f,99999));
    CHECK(!sst25vf080b_pins(&f,false,false,true));
    CHECK(sst25vf080b_advance(&f,1)); CHECK(status(&f)==0x1c);
    CHECK(byte(&f,0x06).driven==0); CHECK(status(&f)==0x1c);
    CHECK(!sst25vf080b_transfer(&f,0,NULL));
    CHECK(!sst25vf080b_advance(NULL,1));
}

static void test_reads_and_pins(void) {
    sst25vf080b_t f; if (!fresh(&f)) return;
    pins(&f,false); CHECK(byte(&f,0x9f).driven==0);
    const uint8_t id[]={0xbf,0x25,0x8e};
    for (unsigned i=0;i<3;++i) { sst25vf080b_output_t o=byte(&f,0xff); CHECK(o.driven==0xff && o.value==id[i]); }
    refusal(&f,0xff); pins(&f,true);
    pins(&f,false); refusal(&f,0x77); pins(&f,true);
    for (unsigned op=0;op<2;++op) {
        addressed(&f,op?0xab:0x90,1);
        CHECK(byte(&f,0).value==0x8e); CHECK(byte(&f,0).value==0xbf);
        CHECK(byte(&f,0).value==0x8e); pins(&f,true);
    }
    image[0xffffe]=0xa6;image[0xfffff]=0x32;image[0]=0x51;image[1]=0x79;
    for (unsigned fast=0;fast<2;++fast) {
        addressed(&f,fast?0x0b:0x03,0xfffffeu);
        if (fast) CHECK(byte(&f,0).driven==0);
        CHECK(byte(&f,0).value==0xa6);
        CHECK(sst25vf080b_pins(&f,false,true,false));
        sst25vf080b_t held=f; CHECK(byte(&f,0xff).driven==0); CHECK(!memcmp(&f,&held,sizeof f));
        CHECK(sst25vf080b_pins(&f,false,true,true));
        CHECK(byte(&f,0).value==0x32); CHECK(byte(&f,0).value==0x51); CHECK(byte(&f,0).value==0x79);
        pins(&f,true);
    }
    pins(&f,false); byte(&f,6); CHECK((f.status&2u)==0); pins(&f,false); CHECK((f.status&2u)==0);
    pins(&f,true); CHECK(status(&f)==0x1e); opcode(&f,4); CHECK(status(&f)==0x1c);
    addressed(&f,3,0); CHECK(sst25vf080b_pins(&f,true,true,false));
    CHECK(sst25vf080b_pins(&f,false,true,false)); CHECK(byte(&f,6).driven==0);
    CHECK(sst25vf080b_pins(&f,false,true,true)); CHECK(byte(&f,5).driven==0);
    CHECK(byte(&f,0).value==0x1c); pins(&f,true);
    CHECK(storage[0]==0x3c && storage[sizeof storage-1u]==0xa5);
}

static void test_status_authorization(void) {
    sst25vf080b_t f; if (!fresh(&f)) return;
    const uint8_t clear[]={1,0}; command(&f,clear,2); CHECK(status(&f)==0x1c);
    opcode(&f,0x50); CHECK(status(&f)==0x1c); command(&f,clear,2); CHECK(status(&f)==0x1c);
    unprotect(&f,0); CHECK(status(&f)==0);
    opcode(&f,6); const uint8_t lock[]={1,0xff}; command(&f,lock,2); CHECK(status(&f)==0xbc);
    CHECK(sst25vf080b_pins(&f,true,false,true));
    unprotect(&f,0); CHECK(status(&f)==0xbc);
    CHECK(sst25vf080b_pins(&f,true,true,true));
    unprotect(&f,0); CHECK(status(&f)==0);
    CHECK(sst25vf080b_pins(&f,true,false,true));
    unprotect(&f,0x80); CHECK(status(&f)==0x80);
    opcode(&f,6); command(&f,clear,2); CHECK(status(&f)==0x80);
    /* An incomplete WRSR cannot change protection, even with WREN. */
    opcode(&f,6); opcode(&f,1); CHECK(status(&f)==0x82);
    pins(&f,false); byte(&f,6); refusal(&f,0); pins(&f,true);
}

static void test_program_and_busy(void) {
    sst25vf080b_t f; if (!fresh(&f)) return;
    program(&f,0x123,0x56); CHECK(image[0x123]==0xff && status(&f)==0x1c);
    opcode(&f,6); program(&f,0x123,0x56); CHECK(image[0x123]==0xff && status(&f)==0x1e);
    unprotect(&f,0); program(&f,0x123,0x56); CHECK(image[0x123]==0xff && status(&f)==0);
    opcode(&f,6); addressed(&f,2,0x123); pins(&f,true); CHECK(status(&f)==2);
    program(&f,0x123,0x56); CHECK(status(&f)==3 && image[0x123]==0xff);
    for (unsigned i=0;i<16;++i) CHECK(status(&f)==3);
    pins(&f,false); refusal(&f,3); pins(&f,true);
    CHECK(sst25vf080b_advance(&f,6999)); CHECK(status(&f)==3 && image[0x123]==0xff);
    /* WRDI clears authorization, but does not cancel the in-flight program. */
    opcode(&f,4); CHECK(status(&f)==1);
    CHECK(sst25vf080b_advance(&f,1)); CHECK(status(&f)==0 && image[0x123]==0x56);
    opcode(&f,6); addressed(&f,2,0x123); byte(&f,0);
    sst25vf080b_t before=f; CHECK(!sst25vf080b_pins(&f,true,true,true));
    CHECK(!memcmp(&f,&before,sizeof f)); CHECK(image[0x123]==0x56);
}

static void test_protection_table(void) {
    static const uint32_t boundary[]={0x100000,0xf0000,0xe0000,0xc0000,0x80000,0,0,0};
    for (unsigned bp=0;bp<16;++bp) {
        sst25vf080b_t f; if (!fresh(&f)) return;
        unprotect(&f,(uint8_t)(bp<<2)); uint32_t end=boundary[bp&7u];
        if (end<SST25VF080B_SIZE) {
            opcode(&f,6); program(&f,end,0x5a); CHECK(!f.busy_ns && image[end]==0xff);
        }
        if (end) {
            opcode(&f,6); program(&f,end-1u,0x5a); CHECK(f.busy_ns==7000);
            CHECK(sst25vf080b_advance(&f,UINT64_MAX)); CHECK(image[end-1u]==0x5a);
        }
        image[0]=0x71; opcode(&f,6); opcode(&f,0x60);
        CHECK((f.busy_ns!=0)==((bp&7u)==0));
        CHECK(sst25vf080b_advance(&f,UINT64_MAX)); CHECK(image[0]==((bp&7u)?0x71:0xff));
    }
}

static void test_erases(void) {
    static const uint8_t ops[]={0x20,0x52,0xd8};
    static const uint32_t sizes[]={0x1000,0x8000,0x10000};
    static const uint64_t durations[]={18000000,19000000,20000000};
    for (unsigned k=0;k<3;++k) {
        sst25vf080b_t f; if (!fresh(&f)) return;
        memset(image,0x36,SST25VF080B_SIZE); unprotect(&f,0); opcode(&f,6);
        addressed(&f,ops[k],0x23456); pins(&f,true);
        uint32_t first=0x23456u&~(sizes[k]-1u);
        CHECK(f.busy_ns==durations[k]); CHECK(sst25vf080b_advance(&f,durations[k]-1u));
        CHECK(image[first]==0x36); CHECK(sst25vf080b_advance(&f,1));
        bool good=true; for (uint32_t i=0;i<SST25VF080B_SIZE;++i)
            if (image[i]!=(i>=first && i<first+sizes[k]?0xff:0x36)) good=false;
        CHECK(good); CHECK(status(&f)==0);
    }
    for (unsigned k=0;k<2;++k) {
        sst25vf080b_t f; if (!fresh(&f)) return;
        memset(image,0,SST25VF080B_SIZE); unprotect(&f,0); opcode(&f,6); opcode(&f,k?0xc7:0x60);
        CHECK(f.busy_ns==35000000); CHECK(sst25vf080b_advance(&f,35000000));
        bool good=true; for (uint32_t i=0;i<SST25VF080B_SIZE;++i) if (image[i]!=0xff) good=false;
        CHECK(good); CHECK(storage[0]==0x3c && storage[sizeof storage-1u]==0xa5);
    }
}

static void aai(sst25vf080b_t *f,uint32_t addr,uint8_t a,uint8_t b,bool initial) {
    if (initial) addressed(f,0xad,addr); else { pins(f,false); CHECK(byte(f,0xad).driven==0); }
    byte(f,a); byte(f,b); pins(f,true);
}
static void test_aai(void) {
    sst25vf080b_t f; if (!fresh(&f)) return;
    unprotect(&f,0); opcode(&f,6); aai(&f,0x123,0x19,0x82,true);
    CHECK(status(&f)==0x43); CHECK(sst25vf080b_advance(&f,7000)); CHECK(status(&f)==0x42);
    CHECK(image[0x122]==0x19 && image[0x123]==0x82);
    pins(&f,false); refusal(&f,6); pins(&f,true);
    aai(&f,0,0x25,0x64,false); CHECK(sst25vf080b_advance(&f,7000));
    CHECK(image[0x124]==0x25 && image[0x125]==0x64); opcode(&f,4); CHECK(status(&f)==0);
    opcode(&f,6); aai(&f,0xfffff,0xa5,0x5a,true); CHECK(sst25vf080b_advance(&f,7000));
    CHECK(status(&f)==0x40 && image[0xffffe]==0xa5 && image[0xfffff]==0x5a && image[0]==0xff);
    /* The limit clears WEL, but the documented WRDI sequence exits AAI mode. */
    pins(&f,false); refusal(&f,6); pins(&f,true);
    opcode(&f,4); CHECK(status(&f)==0);
    unprotect(&f,4); opcode(&f,6); aai(&f,0xeffff,0x21,0x43,true);
    CHECK(sst25vf080b_advance(&f,7000)); CHECK(status(&f)==0x44 && image[0xf0000]==0xff);
    opcode(&f,4); CHECK(status(&f)==4);
    opcode(&f,6); aai(&f,0xf0000,0,0,true); CHECK(!f.busy_ns && status(&f)==6);
}

static void test_busy_stream_and_hardware_pin(void) {
    sst25vf080b_t f; if (!fresh(&f)) return;
    unprotect(&f,0); opcode(&f,6); program(&f,0,0x34);
    pins(&f,false); CHECK(byte(&f,5).driven==0); CHECK(byte(&f,0).value==3);
    CHECK(sst25vf080b_advance(&f,7000)); CHECK(byte(&f,0).value==0); pins(&f,true);
    opcode(&f,0x70); opcode(&f,6); aai(&f,2,0x32,0x78,true);
    sst25vf080b_output_t out={0,0};
    CHECK(sst25vf080b_ready_pin(&f,&out) && !out.driven); pins(&f,false);
    CHECK(sst25vf080b_ready_pin(&f,&out) && out.driven==0xff && out.value==0);
    refusal(&f,5); CHECK(sst25vf080b_advance(&f,7000));
    CHECK(sst25vf080b_ready_pin(&f,&out) && out.driven==0xff && out.value==0xff);
    CHECK(sst25vf080b_pins(&f,false,true,false)); CHECK(sst25vf080b_ready_pin(&f,&out) && !out.driven);
    CHECK(sst25vf080b_pins(&f,false,true,true));
    CHECK(byte(&f,4).value==0xff); pins(&f,true);
    opcode(&f,0x80); CHECK(status(&f)==0 && !f.busy_output);
    opcode(&f,0x70); opcode(&f,6); aai(&f,0xffffe,0x55,0xaa,true);
    CHECK(sst25vf080b_advance(&f,7000)); pins(&f,false);
    CHECK(sst25vf080b_ready_pin(&f,&out) && out.driven==0xff && out.value==0xff);
    CHECK(byte(&f,4).value==0xff); pins(&f,true); opcode(&f,0x80);
    CHECK(status(&f)==0);
}

static void prepare_spi(s5l8920_spi_t *s,const uint8_t *tx,unsigned words,unsigned rx_words) {
    s5l8920_spi_reset(s); CHECK(s5l8920_spi_configure_link(s,0,1));
    CHECK(s5l8920_spi_write(s,0,0)); CHECK(s5l8920_spi_write(s,12,0));
    CHECK(s5l8920_spi_write(s,4,0x4038)); CHECK(s5l8920_spi_write(s,0x30,2));
    CHECK(s5l8920_spi_write(s,0x38,0)); CHECK(s5l8920_spi_write(s,0,13));
    CHECK(s5l8920_spi_write(s,0x4c,words)); CHECK(s5l8920_spi_write(s,0x34,rx_words));
    CHECK(s5l8920_spi_write(s,4,0x2041b8));
    for (unsigned i=0;i<words;++i) CHECK(s5l8920_spi_write(s,0x10,tx[i]));
}

static void test_adapter(void) {
    sst25vf080b_t f; if (!fresh(&f)) return;
    s5l8920_spi_t s;
    const uint8_t tx[]={0x9f,0xff,0xff,0xff},expected[]={0xff,0xbf,0x25,0x8e};
    prepare_spi(&s,tx,4,4); pins(&f,false);
    s5l8920_spi_t old_s=s; sst25vf080b_t old_f=f; size_t count=81;
    CHECK(!s5l8920_spi_flash_clock(&s,&f,32,0,0,&count));
    CHECK(count==81 && !memcmp(&s,&old_s,sizeof s) && !memcmp(&f,&old_f,sizeof f));
    CHECK(s5l8920_spi_flash_clock(&s,&f,7,0xff,0xff,&count) && count==0);
    CHECK(!memcmp(&f,&old_f,sizeof f) && s.busy && s.bits_remaining==1);
    CHECK(s5l8920_spi_flash_clock(&s,&f,25,0xff,0xff,&count) && count==4);
    for (unsigned i=0;i<4;++i) { uint32_t v=0; CHECK(s5l8920_spi_read(&s,0x20,&v) && v==expected[i]); }
    CHECK(s5l8920_spi_irq(&s)); pins(&f,true);

    /* Valid prefix plus unsupported fifth JEDEC byte: neither peer advances. */
    const uint8_t excess[]={0x9f,0xff,0xff,0xff,0xff};
    prepare_spi(&s,excess,5,5); pins(&f,false); old_s=s;old_f=f;count=81;
    CHECK(!s5l8920_spi_flash_clock(&s,&f,40,0xff,0xff,&count));
    CHECK(count==81 && !memcmp(&s,&old_s,sizeof s) && !memcmp(&f,&old_f,sizeof f));
    pins(&f,true);

    /* A transmit-only WREN needs no pull-up; execution waits for CS rising. */
    const uint8_t wren[]={6}; prepare_spi(&s,wren,1,0); pins(&f,false);
    CHECK(s5l8920_spi_flash_clock(&s,&f,8,0,0,&count) && count==1);
    CHECK(!(f.status&2)); CHECK(s5l8920_spi_write(&s,12,2)); pins(&f,true);
    CHECK(status(&f)==0x1e);

    /* Receive overflow is detected before consuming a flash command. */
    prepare_spi(&s,tx,4,4); s.rx.count=16; pins(&f,false); old_s=s;old_f=f;count=81;
    CHECK(!s5l8920_spi_flash_clock(&s,&f,32,0xff,0xff,&count));
    CHECK(count==81 && !memcmp(&s,&old_s,sizeof s) && !memcmp(&f,&old_f,sizeof f));
    pins(&f,true);
    CHECK(!s5l8920_spi_flash_clock(&s,&f,0,0xff,0xff,&count)); /* mismatched CE */
}

/* Feed the controller with responses computed by the actual device state.
 * This fixture's explicit high-Z pull-up is not an N88 electrical claim. */
static void test_spi_connection(void) {
    sst25vf080b_t f; if (!fresh(&f)) return;
    s5l8920_spi_t s; s5l8920_spi_reset(&s);
    CHECK(s5l8920_spi_configure_link(&s,0,1));
    CHECK(s5l8920_spi_write(&s,0,0)); CHECK(s5l8920_spi_write(&s,12,0));
    CHECK(s5l8920_spi_write(&s,4,0x4038)); CHECK(s5l8920_spi_write(&s,0x30,2));
    CHECK(s5l8920_spi_write(&s,0x38,0)); CHECK(s5l8920_spi_write(&s,0,13));
    CHECK(s5l8920_spi_write(&s,0x4c,4)); CHECK(s5l8920_spi_write(&s,0x34,4));
    CHECK(s5l8920_spi_write(&s,4,0x2041b8));
    const uint8_t tx[]={0x9f,0xff,0xff,0xff},expected[]={0xff,0xbf,0x25,0x8e};
    for (unsigned i=0;i<4;++i) CHECK(s5l8920_spi_write(&s,0x10,tx[i]));
    pins(&f,false);
    for (unsigned i=0;i<4;++i) {
        sst25vf080b_output_t out=byte(&f,tx[i]);
        uint32_t received=(out.value&out.driven)|(0xffu&~out.driven),sent=0;
        size_t count=0;
        CHECK(s5l8920_spi_serial_clock(&s,8,&received,1,&sent,1,&count));
        CHECK(count==1 && sent==tx[i]); uint32_t data=0;
        CHECK(s5l8920_spi_read(&s,0x20,&data)); CHECK(data==expected[i]);
    }
    CHECK(s5l8920_spi_irq(&s)); CHECK(!s.tx_words && !s.rx_words);
    CHECK(s5l8920_spi_write(&s,8,S5L8920_SPI_EVENT_MASK)); CHECK(!s5l8920_spi_irq(&s)); pins(&f,true);
}

static void test_unavailable_array(void) {
    sst25vf080b_t f;memset(&f,0xa5,sizeof f);sst25vf080b_t before=f;
    CHECK(!sst25vf080b_init_unbacked(NULL,&timing,true,true));
    CHECK(!sst25vf080b_init_unbacked(&f,NULL,true,true));CHECK(!memcmp(&before,&f,sizeof f));
    bool ok=sst25vf080b_init_unbacked(&f,&timing,true,true);CHECK(ok);if(!ok)return;
    CHECK(!f.image && f.array_unavailable && f.initialized && f.powerup_ns==100000);
    before=f;CHECK(!sst25vf080b_pins(&f,false,true,true));CHECK(!memcmp(&before,&f,sizeof f));
    CHECK(sst25vf080b_advance(&f,100000));CHECK(status(&f)==0x1c);
    pins(&f,false);CHECK(!byte(&f,0x9f).driven);
    const uint8_t id[]={0xbf,0x25,0x8e};
    for(unsigned i=0;i<3;++i) {sst25vf080b_output_t out=byte(&f,0xff);CHECK(out.value==id[i] && out.driven==0xff);}
    pins(&f,true);
    const uint8_t reads[]={3,0xb};
    for(unsigned i=0;i<2;++i) {
        addressed(&f,reads[i],0xfffff);if(reads[i]==0xb)CHECK(!byte(&f,0xff).driven);
        refusal(&f,0xff);pins(&f,true);
    }
    /* Protected writes are known to have no storage effect even without data. */
    opcode(&f,6);program(&f,0,0xa5);CHECK(!f.busy_ns && status(&f)==0x1e);opcode(&f,4);
    const uint8_t writes[]={2,0xad,0x20,0x52,0xd8,0x60,0xc7};
    for(unsigned i=0;i<sizeof writes;++i) {
        CHECK(sst25vf080b_init_unbacked(&f,&timing,true,true));CHECK(sst25vf080b_advance(&f,100000));
        unprotect(&f,0);opcode(&f,6);
        if(writes[i]==0x60 || writes[i]==0xc7) {pins(&f,false);CHECK(!byte(&f,writes[i]).driven);}
        else addressed(&f,writes[i],0x2000);
        if(writes[i]==2 || writes[i]==0xad)CHECK(!byte(&f,0x55).driven);
        if(writes[i]==0xad)CHECK(!byte(&f,0xaa).driven);
        before=f;CHECK(!sst25vf080b_pins(&f,true,true,true));
        CHECK(!memcmp(&before,&f,sizeof f) && !f.busy_ns && !f.operation);
        CHECK(sst25vf080b_advance(&f,UINT64_MAX));CHECK(!memcmp(&before,&f,sizeof f));
    }
    CHECK(!sst25vf080b_init(&f,NULL,SST25VF080B_SIZE,&timing,true,true));
    CHECK(!memcmp(&before,&f,sizeof f));
}

static bool partial(sst25vf080b_t *f,const uint32_t *known) {
    bool ok=sst25vf080b_init_partial(f,image,SST25VF080B_SIZE,known,&timing,true,true);
    CHECK(ok);if(!ok)return false;
    CHECK(sst25vf080b_advance(f,100000));return true;
}

static void test_partial_reads(void) {
    sst25vf080b_t f,before;
    uint32_t known[SST25VF080B_KNOWN_WORDS];
    for(unsigned n=0;n<SST25VF080B_KNOWN_WORDS;++n)known[n]=0x96966969u^(n*0x1234567u);
    memset(image,0x6b,SST25VF080B_SIZE);
    if(!partial(&f,known))return;
    CHECK(!f.array_unavailable && f.image==image && !memcmp(f.known_sectors,known,sizeof known));
    /* Every bitmap position, both ends of each sector and both read opcodes. */
    for(unsigned sector=0;sector<256u;++sector)for(unsigned fast=0;fast<2u;++fast) {
        uint32_t address=sector*SST25VF080B_SECTOR_SIZE+(fast?4095u:0u);
        image[address]=(uint8_t)(sector^0xa5u);
        addressed(&f,fast?0x0b:3,address);
        if(fast)CHECK(!byte(&f,0xff).driven);
        if(known[sector/32]&(1u<<(sector%32))) {
            sst25vf080b_output_t out=byte(&f,0xff);
            CHECK(out.driven==0xff && out.value==(uint8_t)(sector^0xa5u));
        } else refusal(&f,0xff);
        pins(&f,true);
    }
    memset(known,0,sizeof known);known[0]=1;known[7]=0x80000000u;
    if(!partial(&f,known))return;
    known[0]=0;CHECK(f.known_sectors[0]==1); /* caller mask was copied */
    for(unsigned fast=0;fast<2u;++fast) {
        addressed(&f,fast?0x0b:3,4095);if(fast)CHECK(!byte(&f,0).driven);
        CHECK(byte(&f,0).value==image[4095]);refusal(&f,0);pins(&f,true);
        addressed(&f,fast?0x0b:3,0xffffff);if(fast)CHECK(!byte(&f,0).driven);
        CHECK(byte(&f,0).value==image[0xfffff]);CHECK(byte(&f,0).value==image[0]);pins(&f,true);
    }
    addressed(&f,3,0);f.address=SST25VF080B_SIZE;refusal(&f,0);pins(&f,true);
    before=f;
    CHECK(!sst25vf080b_init_partial(NULL,image,SST25VF080B_SIZE,known,&timing,true,true));
    CHECK(!sst25vf080b_init_partial(&f,NULL,SST25VF080B_SIZE,known,&timing,true,true));
    CHECK(!sst25vf080b_init_partial(&f,image,SST25VF080B_SIZE-1,known,&timing,true,true));
    CHECK(!sst25vf080b_init_partial(&f,image,SST25VF080B_SIZE,NULL,&timing,true,true));
    CHECK(!sst25vf080b_init_partial(&f,image,SST25VF080B_SIZE,known,NULL,true,true));
    sst25vf080b_timing_t bad=timing;bad.sector_ns=0;
    CHECK(!sst25vf080b_init_partial(&f,image,SST25VF080B_SIZE,known,&bad,true,true));
    CHECK(!memcmp(&f,&before,sizeof f));
    /* An aliased mask must be copied before initialization replaces state. */
    CHECK(sst25vf080b_init_partial(&f,image,SST25VF080B_SIZE,f.known_sectors,&f.timing,true,true));
    CHECK(!memcmp(f.known_sectors,before.known_sectors,sizeof known));
    CHECK(f.powerup_ns==100000u && image[0]==0xa5 && image[4095]==0xa5);
    CHECK(sst25vf080b_init(&f,image,SST25VF080B_SIZE,&timing,true,true));
    for(unsigned n=0;n<SST25VF080B_KNOWN_WORDS;++n)CHECK(f.known_sectors[n]==UINT32_MAX);
    CHECK(sst25vf080b_init_unbacked(&f,&timing,true,true));
    for(unsigned n=0;n<SST25VF080B_KNOWN_WORDS;++n)CHECK(f.known_sectors[n]==0);
}

static void test_partial_mutations(void) {
    const uint32_t known[SST25VF080B_KNOWN_WORDS]={1};
    sst25vf080b_t f;
    memset(image,0xff,SST25VF080B_SIZE);
    if(!partial(&f,known))return;
    unprotect(&f,0);opcode(&f,6);program(&f,0,0x51);
    CHECK(sst25vf080b_advance(&f,timing.program_ns) && image[0]==0x51);
    CHECK(f.known_sectors[0]==1);
    for(unsigned aai_mode=0;aai_mode<2;++aai_mode) {
        opcode(&f,6);addressed(&f,aai_mode?0xad:2,4096);
        CHECK(!byte(&f,0x55).driven);if(aai_mode)CHECK(!byte(&f,0xaa).driven);
        sst25vf080b_t before=f;
        CHECK(!sst25vf080b_pins(&f,true,true,true));
        CHECK(!memcmp(&f,&before,sizeof f) && image[4096]==0xff && image[4097]==0xff);
        /* Restore a fresh configuration instead of forcing a refused CE edge. */
        if(!partial(&f,known))return;
        unprotect(&f,0);
    }
    const uint8_t ops[]={0x20,0x52,0xd8,0x60,0xc7};
    const uint32_t sizes[]={0x1000,0x8000,0x10000,0x100000,0x100000};
    const uint64_t times[]={18000000,19000000,20000000,35000000,35000000};
    const uint32_t unknown[SST25VF080B_KNOWN_WORDS]={0};
    for(unsigned n=0;n<sizeof ops;++n) {
        memset(image,0x69,SST25VF080B_SIZE);
        uint32_t seeded[SST25VF080B_KNOWN_WORDS]={0};
        if(n){seeded[0]=1;seeded[7]=0x80000000u;}
        if(!partial(&f,seeded))return;
        uint32_t start=n<3?0x38000u&~(sizes[n]-1u):0u;
        unprotect(&f,0);opcode(&f,6);
        if(n<3){addressed(&f,ops[n],0x38abcu);pins(&f,true);}else opcode(&f,ops[n]);
        CHECK(f.busy_ns==times[n] && f.operation_address==start && f.operation_size==sizes[n]);
        CHECK(sst25vf080b_advance(&f,times[n]-1) && f.busy_ns==1);
        CHECK(!memcmp(f.known_sectors,seeded,sizeof seeded));
        CHECK(image[start]==0x69 && image[start+sizes[n]-1]==0x69);
        CHECK(sst25vf080b_advance(&f,1) && !f.busy_ns);
        for(unsigned sector=0;sector<256;++sector) {
            bool erased=sector*4096u>=start && sector*4096u<start+sizes[n];
            bool supplied=(seeded[sector/32]&(1u<<(sector%32)))!=0;
            CHECK(((f.known_sectors[sector/32]>>(sector%32))&1u)==(unsigned)(erased||supplied));
            CHECK(image[sector*4096u]==(erased?0xff:0x69));
            CHECK(image[sector*4096u+4095u]==(erased?0xff:0x69));
        }
        opcode(&f,6);program(&f,start,0x73);
        CHECK(sst25vf080b_advance(&f,timing.program_ns) && image[start]==0x73);
    }
    if(!partial(&f,unknown))return;
    opcode(&f,6);addressed(&f,0x20,0);pins(&f,true); /* cold protection ignores erase */
    CHECK(!f.busy_ns && !f.known_sectors[0]);
    unprotect(&f,0);opcode(&f,4);addressed(&f,0x20,0);pins(&f,true); /* WEL clear */
    CHECK(!f.busy_ns && !f.known_sectors[0]);
    memset(image,0xff,SST25VF080B_SIZE);
    if(!partial(&f,known))return;
    unprotect(&f,0);opcode(&f,6);aai(&f,4094,0x12,0x34,true);
    CHECK(sst25vf080b_advance(&f,timing.program_ns) && f.aai_next==4096);
    CHECK(image[4094]==0x12 && image[4095]==0x34);
    pins(&f,false);CHECK(!byte(&f,0xad).driven);
    CHECK(!byte(&f,0x56).driven);CHECK(!byte(&f,0x78).driven);
    sst25vf080b_t before=f;
    CHECK(!sst25vf080b_pins(&f,true,true,true));
    CHECK(!memcmp(&f,&before,sizeof f) && image[4096]==0xff && image[4097]==0xff);
    CHECK(storage[0]==0x3c && storage[sizeof storage-1u]==0xa5);
}

static void test_partial_adapter(void) {
    sst25vf080b_t f;const uint32_t known[SST25VF080B_KNOWN_WORDS]={1};
    if(!partial(&f,known))return;
    image[4095]=0x62;
    s5l8920_spi_t s;
    const uint8_t tx[]={3,0,0x0f,0xff,0xff,0xff};
    prepare_spi(&s,tx,sizeof tx,sizeof tx);pins(&f,false);
    sst25vf080b_t old_f=f;s5l8920_spi_t old_s=s;size_t count=81;
    CHECK(!s5l8920_spi_flash_clock(&s,&f,48,0xff,0xff,&count));
    CHECK(count==81 && !memcmp(&f,&old_f,sizeof f) && !memcmp(&s,&old_s,sizeof s));
    CHECK(s5l8920_spi_flash_clock(&s,&f,40,0xff,0xff,&count) && count==5);
    for(unsigned n=0;n<5;++n){uint32_t value=0;CHECK(s5l8920_spi_read(&s,0x20,&value) && value==(n<4?0xffu:0x62u));}
    old_f=f;old_s=s;count=81;
    CHECK(!s5l8920_spi_flash_clock(&s,&f,8,0xff,0xff,&count));
    CHECK(count==81 && !memcmp(&f,&old_f,sizeof f) && !memcmp(&s,&old_s,sizeof s));
    pins(&f,true);
}

int main(void) {
    test_power_and_inputs(); test_reads_and_pins(); test_status_authorization();
    test_program_and_busy(); test_protection_table(); test_erases(); test_aai();
    test_busy_stream_and_hardware_pin(); test_spi_connection(); test_adapter();test_unavailable_array();
    test_partial_reads();test_partial_mutations();test_partial_adapter();
    printf("sst25vf080b: %u passed, %u failed\n",passed,failed);
    return failed?1:0;
}
