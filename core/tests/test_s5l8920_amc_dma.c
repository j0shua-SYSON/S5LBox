/* Backed AMC DMA transfers and atomic refusals, without proprietary firmware.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_amc_dma.h"
#include <stdio.h>
#include <string.h>
static unsigned passed,failed;
#define CHECK(x) do { if (x) ++passed; else { if (failed<24u) { \
    printf("FAIL %s:%d %s\n",__func__,__LINE__,#x); } ++failed; } } while (0)
static uint8_t memory[0x50000],expected[0x50000],before_memory[0x50000];
static const uint32_t desc=0x48000u,source=0x8000u,dest=0x28000u;
static s5l8920_amc_dma_input_t input(void) {
    s5l8920_amc_dma_input_t in={memory,sizeof memory,
        {{0x4000u,0x18000u},{0x24000u,0x18000u},{0x48000u,0x1000u}},3u,
        0xa5a5a5a0u,0x5a5a5a5au,0x1234567fu};
    return in;
}
static void word_at(uint32_t a,uint32_t v) {
    for (unsigned i=0;i<4u;++i) memory[a+i]=(uint8_t)(v>>(8u*i));
}
static void prepare(unsigned count,unsigned form) {
    memset(memory,0xcc,sizeof memory);
    for (unsigned i=0;i<count*4u;++i) memory[source+i]=(uint8_t)(i*37u+(i>>8)+11u);
    word_at(desc,0u); word_at(desc+4u,(count<<18)|form);
    word_at(desc+8u,source); word_at(desc+12u,dest);
    memcpy(expected,memory,sizeof memory);
}
static uint32_t status(const s5l8920_amc_dma_t *d) {
    uint32_t value=0xdeadbeefu;
    CHECK(s5l8920_amc_dma_read(d,0x14u,4u,&value)); return value;
}
static void refuse(s5l8920_amc_dma_t *d,uint32_t at,unsigned size,uint32_t value) {
    s5l8920_amc_dma_t before=*d; memcpy(before_memory,memory,sizeof memory);
    CHECK(!s5l8920_amc_dma_write(d,at,size,value));
    CHECK(!memcmp(&before,d,sizeof before) && !memcmp(before_memory,memory,sizeof memory));
}
static void transfer(unsigned count,unsigned form) {
    prepare(count,form);
    s5l8920_amc_dma_t d={0}; s5l8920_amc_dma_input_t in=input();
    CHECK(s5l8920_amc_dma_configure(&d,&in));
    CHECK(status(&d)==in.idle_status);
    CHECK(s5l8920_amc_dma_write(&d,0x10u,4u,4u)); CHECK(status(&d)==in.stopped_status);
    refuse(&d,0u,4u,desc|2u); CHECK(s5l8920_amc_dma_service(&d,true,16u)==0u);
    CHECK(s5l8920_amc_dma_write(&d,0x10u,4u,0x10u)); CHECK(status(&d)==in.idle_status);
    CHECK(s5l8920_amc_dma_write(&d,0u,4u,desc|2u));
    CHECK(d.active && !d.completed && !d.copied && d.length==count*4u);
    CHECK(!memcmp(memory,expected,sizeof memory));
    for (unsigned i=0;i<8u;++i) CHECK(status(&d)==in.active_status && !d.copied);
    CHECK(s5l8920_amc_dma_service(&d,false,UINT32_MAX)==0u);
    for (unsigned i=0;i<4u;++i) CHECK(s5l8920_amc_dma_service(&d,true,i)==0u);
    refuse(&d,0u,4u,desc|2u); refuse(&d,0x10u,4u,4u); refuse(&d,0x10u,4u,0x10u);
    s5l8920_amc_dma_t before=d;
    CHECK(s5l8920_amc_dma_configure(&d,&in) && !memcmp(&before,&d,sizeof d));
    unsigned calls=0;
    while (d.active && calls<17000u) {
        unsigned budget=calls%3u==0u?5u:calls%3u==1u?15u:UINT32_MAX;
        unsigned wanted=budget>16u?16u:budget&~3u;
        if (wanted>d.length-d.copied) wanted=d.length-d.copied;
        unsigned prior=d.copied;
        CHECK(s5l8920_amc_dma_service(&d,true,budget)==wanted);
        memcpy(expected+dest+prior,expected+source+prior,wanted);
        CHECK(d.copied==prior+wanted && !memcmp(memory,expected,sizeof memory));
        CHECK(d.active==(d.copied<d.length) && d.completed==(d.copied==d.length));
        CHECK(status(&d)==(d.active?in.active_status:in.idle_status));
        if (d.active) CHECK(s5l8920_amc_dma_service(&d,false,16u)==0u);
        ++calls;
    }
    CHECK(!d.active && d.completed && d.copied==count*4u);
    CHECK(s5l8920_amc_dma_service(&d,true,16u)==0u && !memcmp(memory,expected,sizeof memory));
    s5l8920_amc_dma_reset(&d);
    CHECK(!d.active && !d.completed && !d.copied && status(&d)==in.idle_status);
    CHECK(!memcmp(memory,expected,sizeof memory));
}
static void rejections(void) {
    prepare(512u,0xfa5u); s5l8920_amc_dma_t d={0}; s5l8920_amc_dma_input_t in=input();
    uint32_t value=0xdeadbeefu;
    CHECK(!s5l8920_amc_dma_read(&d,0x14u,4u,&value) && value==0xdeadbeefu);
    refuse(&d,0u,4u,desc|2u); CHECK(!s5l8920_amc_dma_configure(NULL,&in));
    CHECK(!s5l8920_amc_dma_configure(&d,NULL));
    CHECK(!s5l8920_amc_dma_read(NULL,0x14u,4u,&value));
    CHECK(!s5l8920_amc_dma_write(NULL,0u,4u,desc|2u));
    CHECK(!s5l8920_amc_dma_service(NULL,true,16u)); s5l8920_amc_dma_reset(NULL);
    s5l8920_amc_dma_reset(&d); CHECK(!d.configured);
    for (unsigned i=0;i<15u;++i) {
        s5l8920_amc_dma_input_t invalid=in;
        switch (i) {
        case 0:invalid.memory=NULL;break;
        case 1:invalid.memory_size=0u;break;
        case 2:invalid.region_count=0u;break;
        case 3:invalid.region_count=4u;break;
        case 4:invalid.region[1].size=0u;break;
        case 5:invalid.region[1].offset|=1u;break;
        case 6:invalid.region[1].size|=1u;break;
        case 7:invalid.region[1].offset=0xfffffff0u;break;
        case 8:invalid.region[1].offset=0x8000u;break;
        case 9:invalid.region_count=2u;break;
        case 10:invalid.idle_status|=1u;break;
        case 11:invalid.active_status&=~7u;break;
        case 12:invalid.active_status|=7u;break;
        case 13:invalid.stopped_status&=~1u;break;
        case 14:invalid.region[2].size=0x10000u;break;
        }
        CHECK(!s5l8920_amc_dma_configure(&d,&invalid) && !d.configured);
    }
    CHECK(s5l8920_amc_dma_configure(&d,&in));
    s5l8920_amc_dma_t before=d; in.active_status^=8u;
    CHECK(!s5l8920_amc_dma_configure(&d,&in) && !memcmp(&before,&d,sizeof d)); in=input();
    CHECK(!s5l8920_amc_dma_read(&d,0x14u,4u,NULL));
    for (unsigned at=0;at<=0x40u;++at) for (unsigned size=0;size<=8u;++size) {
        if (at==0x14u && size==4u) continue;
        value=0xdeadbeefu; CHECK(!s5l8920_amc_dma_read(&d,at,size,&value));
        CHECK(value==0xdeadbeefu && !memcmp(&before,&d,sizeof d));
        if (at==0u && size==4u) continue;
        refuse(&d,at,size,desc|2u);
    }
    refuse(&d,0x10u,4u,0x10u); refuse(&d,0x10u,4u,0x20u); refuse(&d,0x10u,4u,0x60u);
    const uint32_t pointers[]={0u,1u,3u,desc,desc|1u,desc|3u,0x20002u,0x48ff6u,0xfffffffeu};
    for (unsigned i=0;i<sizeof pointers/sizeof pointers[0];++i) refuse(&d,0u,4u,pointers[i]);
    const uint32_t controls[]={0x25u,0x40024u,0x40021u,0x40035u,0x40005u,0x41fa5u,0x44025u};
    for (unsigned i=0;i<sizeof controls/sizeof controls[0];++i) {
        word_at(desc+4u,controls[i]); refuse(&d,0u,4u,desc|2u);
    }
    prepare(512u,0xfa5u); word_at(desc,desc|2u); refuse(&d,0u,4u,desc|2u);
    word_at(desc,0u);
    const uint32_t addresses[]={source+1u,0x20000u,0xfffffffcu,0x1bffcu,desc,source};
    for (unsigned i=0;i<sizeof addresses/sizeof addresses[0];++i) {
        word_at(desc+12u,addresses[i]); refuse(&d,0u,4u,desc|2u);
    }
    word_at(desc+12u,dest);word_at(desc+8u,dest+4u);refuse(&d,0u,4u,desc|2u);
    prepare(512u,0xfa5u);CHECK(s5l8920_amc_dma_write(&d,0u,4u,desc|2u));
    CHECK(s5l8920_amc_dma_service(&d,true,16u)==16u);
    memcpy(expected+dest,expected+source,16u);s5l8920_amc_dma_reset(&d);
    CHECK(!d.active && !d.completed && !d.copied && !memcmp(memory,expected,sizeof memory));
    CHECK(s5l8920_amc_dma_service(&d,true,16u)==0u);
    /* Descriptor metadata is latched, so later descriptor edits cannot redirect
     * a transfer into unbacked memory. Data bytes remain live backed memory. */
    CHECK(s5l8920_amc_dma_write(&d,0u,4u,desc|2u)); word_at(desc+12u,0xfffffffcu);
    CHECK(s5l8920_amc_dma_service(&d,true,16u)==16u && d.destination==dest);
}
int main(void) {
    rejections();
    const unsigned counts[]={1u,2u,7u,512u,16383u};
    for (unsigned i=0;i<sizeof counts/sizeof counts[0];++i) {
        transfer(counts[i],0x25u); transfer(counts[i],0xfa5u);
    }
    printf("AMC DMA: %u passed, %u failed\n",passed,failed); return failed?1:0;
}
