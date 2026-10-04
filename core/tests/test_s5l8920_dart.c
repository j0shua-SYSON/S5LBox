/* Page translation, isolated banks, unknown readback and checked MMIO.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920.h"
#include <stdio.h>
#include <string.h>
static unsigned passed,failed;
static s5l8920_t m;
#define CHECK(c,msg) do { if (c) passed++; else { failed++; printf("FAIL %s:%d %s\n",__func__,__LINE__,msg); } } while (0)
static void rejected_write(s5l8920_dart_t *d,uint32_t off,uint32_t val) {
    s5l8920_dart_t before=*d;
    CHECK(!s5l8920_dart_write(d,off,val) && !memcmp(d,&before,sizeof before),"rejected write atomic");
}
static void rejected_read(s5l8920_dart_t *d,uint32_t off) {
    s5l8920_dart_t before=*d;uint32_t value=0xdeadbeefu;
    CHECK(!s5l8920_dart_read(d,off,&value) && value==0xdeadbeefu &&
          !memcmp(d,&before,sizeof before),"unavailable read preserves output/state");
}
static void put(uint32_t address,uint32_t word) {
    m.bus.write32(&m,address,word);
    CHECK(!m.bus.access_failed(&m),"RAM/table write");
}
static void expect_translation(s5l8920_dart_t *d,uint32_t address,size_t size,
    size_t backing,s5l8920_dart_result_t result,uint32_t expected) {
    uint32_t physical=0xfeedfaceu;s5l8920_dart_t before=*d;
    CHECK(s5l8920_dart_translate(d,m.ram,backing,address,size,&physical)==result,
          "translation result");
    CHECK(physical==(result==S5L8920_DART_OK?expected:0xfeedfaceu) &&
          !memcmp(d,&before,sizeof before),"translation output/state contract");
}
static void configuration(void) {
    s5l8920_dart_t d={0};uint32_t value;
    for(unsigned off=0;off<28;off+=4) {
        rejected_read(&d,off);rejected_write(&d,off,0);
    }
    CHECK(!s5l8920_dart_configure(NULL,0,0,0),"null configure");
    CHECK(!s5l8920_dart_configure(&d,0x80000000u,0,0) && !d.configured,"initial active state refused");
    CHECK(s5l8920_dart_configure(&d,0x1234560fu,0xabcdf800u,0xd1234567u),"explicit initial words");
    CHECK(s5l8920_dart_read(&d,0,&value) && value==0xabcdf800u,"initial command observation");
    CHECK(s5l8920_dart_read(&d,8,&value) && value==0xd1234567u,"initial table-port observation");
    rejected_write(&d,0,0xabcdf801u);rejected_write(&d,0,0xabcde702u);
    rejected_write(&d,8,0xc0010001u);rejected_write(&d,8,0xd0010003u);
    rejected_write(&d,8,0xd0010000u);rejected_write(&d,12,0x1234570fu);
    rejected_write(&d,12,0x9234567fu);rejected_write(&d,12,0x92345600u);
    for(unsigned off=1;off<24;off++) if(off!=8 && off!=12) {
        rejected_read(&d,off);rejected_write(&d,off,0);
    }
    CHECK(s5l8920_dart_write(&d,0,0xabcdff02u),"supported synchronous uncached flush");
    rejected_read(&d,0);
    CHECK(s5l8920_dart_write(&d,8,0xd0010301u),"program selected segment3");
    CHECK(d.programmed==8u && d.segment[3]==0x10001u,"selector excluded from table entry");
    rejected_read(&d,8);
    CHECK(s5l8920_dart_write(&d,12,0x92345670u),"enabled observed mode");
    CHECK(s5l8920_dart_read(&d,12,&value) && value==0x92345670u,"config RMW value");
    rejected_write(&d,8,0xd0020401u);
    s5l8920_dart_t before=d;
    CHECK(s5l8920_dart_configure(&d,0x1234560fu,0xabcdf800u,0xd1234567u) &&
          !memcmp(&d,&before,sizeof d),"identical configure does not restore consumed observation");
    CHECK(!s5l8920_dart_configure(&d,0x1234560eu,0xabcdf800u,0xd1234567u) &&
          !memcmp(&d,&before,sizeof d),"conflicting initial state atomic");
    s5l8920_dart_reset(&d);
    CHECK(d.config==0x1234560fu && !d.programmed && !d.segment[3] &&
          d.command_readable && d.table_port_readable,"reset discards mappings and restores supplied observations");
}
static void page_walk(void) {
    s5l8920_dart_t d={0};uint32_t va=S5L8920_DART_DVA_BASE;
    expect_translation(&d,va,4,S5L8920_RAM_SIZE,S5L8920_DART_UNCONFIGURED,0);
    CHECK(s5l8920_dart_configure(&d,0,0,0),"configure walker");
    expect_translation(&d,va,4,S5L8920_RAM_SIZE,S5L8920_DART_DISABLED,0);
    CHECK(s5l8920_dart_write(&d,12,0x80000070u),"enable");
    expect_translation(&d,va,4,S5L8920_RAM_SIZE,S5L8920_DART_UNKNOWN_SEGMENT,0);
    CHECK(s5l8920_dart_write(&d,12,0x70u),"disable");
    for(unsigned i=0;i<16;i++) {
        /* Deliberately noncontiguous/reversed segment tables: the walker must
         * use the programmed selector, not assume the driver's allocation. */
        uint32_t table=0x40100000u+(15u-i)*0x2000u;
        CHECK(s5l8920_dart_write(&d,8,(table&0x0ffff000u)|(i<<8)|1u),"segment programming");
        for(unsigned page=0;page<1024;page++) {
            uint32_t physical=0x40000000u+((i*1024u+page)*0x3000u);
            put(table+page*4u,(physical&0x0ffff000u)|1u);
        }
    }
    CHECK(s5l8920_dart_write(&d,12,0x80000070u),"all segments enabled");
    for(unsigned i=0;i<16;i++) for(unsigned page=0;page<1024;page++) {
        uint32_t physical=0x40000000u+((i*1024u+page)*0x3000u);
        uint32_t address=va+i*0x400000u+page*0x1000u;
        expect_translation(&d,address,0x1000u,S5L8920_RAM_SIZE,S5L8920_DART_OK,physical);
        expect_translation(&d,address+4095u,1,S5L8920_RAM_SIZE,S5L8920_DART_OK,physical+4095u);
    }
    uint32_t first_table=0x4011e000u;
    put(first_table,0x0ffff001u);
    expect_translation(&d,va+4095u,1,S5L8920_RAM_SIZE,S5L8920_DART_OK,0x4fffffffu);
    CHECK(s5l8920_dart_write(&d,0,0x702u),"flush while enabled");
    put(first_table,1u);
    expect_translation(&d,va,4096,S5L8920_RAM_SIZE,S5L8920_DART_OK,0x40000000u);
    put(first_table,0u);
    expect_translation(&d,va,1,S5L8920_RAM_SIZE,S5L8920_DART_INVALID_PAGE,0);
    for(unsigned bit=1;bit<32;bit++) if(bit<12 || bit>=28) {
        put(first_table,1u|(UINT32_C(1)<<bit));
        expect_translation(&d,va,1,S5L8920_RAM_SIZE,S5L8920_DART_UNSUPPORTED_PAGE,0);
    }
    put(first_table,0x0ffff001u);
    expect_translation(&d,va,1,0x200000u,S5L8920_DART_TARGET_UNAVAILABLE,0);
    expect_translation(&d,va,1,0x11e003u,S5L8920_DART_TABLE_UNAVAILABLE,0);
    expect_translation(&d,va,1,0,S5L8920_DART_TABLE_UNAVAILABLE,0);
    expect_translation(&d,va-1u,1,S5L8920_RAM_SIZE,S5L8920_DART_BAD_REQUEST,0);
    expect_translation(&d,0x40000000u,1,S5L8920_RAM_SIZE,S5L8920_DART_BAD_REQUEST,0);
    expect_translation(&d,UINT32_MAX,1,S5L8920_RAM_SIZE,S5L8920_DART_BAD_REQUEST,0);
    expect_translation(&d,va,0,S5L8920_RAM_SIZE,S5L8920_DART_BAD_REQUEST,0);
    expect_translation(&d,va,SIZE_MAX,S5L8920_RAM_SIZE,S5L8920_DART_BAD_REQUEST,0);
    expect_translation(&d,va+4095u,2,S5L8920_RAM_SIZE,S5L8920_DART_BAD_REQUEST,0);
    expect_translation(&d,va,1,(size_t)S5L8920_RAM_SIZE+1u,S5L8920_DART_BAD_REQUEST,0);
    uint32_t out=123;
    CHECK(s5l8920_dart_translate(NULL,m.ram,S5L8920_RAM_SIZE,va,1,&out)==S5L8920_DART_BAD_REQUEST && out==123,"null controller");
    CHECK(s5l8920_dart_translate(&d,NULL,S5L8920_RAM_SIZE,va,1,&out)==S5L8920_DART_BAD_REQUEST && out==123,"null RAM");
    CHECK(s5l8920_dart_translate(&d,m.ram,S5L8920_RAM_SIZE,va,1,NULL)==S5L8920_DART_BAD_REQUEST,"null result");
    CHECK(s5l8920_dart_write(&d,12,0x70u) && s5l8920_dart_write(&d,8,0),"invalidate segment0 while disabled");
    CHECK(s5l8920_dart_write(&d,12,0x80000070u),"reenable");
    expect_translation(&d,va,1,S5L8920_RAM_SIZE,S5L8920_DART_INVALID_SEGMENT,0);
    expect_translation(&d,va+0x400000u,1,S5L8920_RAM_SIZE,S5L8920_DART_OK,0x40c00000u);
}
static void board(void) {
    for(unsigned bank=0;bank<2;bank++) {
        uint32_t base=S5L8920_DART_BASE+bank*S5L8920_DART_STRIDE;
        s5l8920_clear_bus_failure(&m);
        (void)m.bus.read32(&m,base+12);
        CHECK(m.bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
              m.bus_failure.address==base+12 && !m.bus_failure.write,"cold first read guarded");
        s5l8920_bus_failure_t first=m.bus_failure;
        m.bus.write32(&m,base+8,1u);
        CHECK(!memcmp(&first,&m.bus_failure,sizeof first) && !m.dart[bank].programmed,"first failure retained");
        s5l8920_clear_bus_failure(&m);
        CHECK(s5l8920_dart_configure(&m.dart[bank],0,0,bank<<28),"independent bank initial state");
        m.bus.write32(&m,base+8,(bank<<28)|0x00200001u);
        m.bus.write32(&m,base+12,0x80000070u);
        CHECK(!m.bus.access_failed(&m) && m.dart[bank].segment[0]==0x200001u,"bus programming");
        s5l8920_dart_t before=m.dart[bank];
        (void)m.bus.read32(&m,base+8);
        CHECK(m.bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"post-command table read is unknown");
        CHECK(!memcmp(&before,&m.dart[bank],sizeof before),"readback guard preserves programmed mappings");
        for(unsigned off=0;off<24;off++) {
            s5l8920_clear_bus_failure(&m);m.bus.write8(&m,base+off,1);
            CHECK(m.bus_failure.reason==S5L8920_BUS_ACCESS_UNIMPLEMENTED,"byte MMIO refused");
            s5l8920_clear_bus_failure(&m);(void)m.bus.read16(&m,base+off);
            CHECK(m.bus_failure.reason==S5L8920_BUS_ACCESS_UNIMPLEMENTED,"halfword MMIO refused");
            if(off&3u) {
                s5l8920_clear_bus_failure(&m);m.bus.write32(&m,base+off,1);
                CHECK(m.bus_failure.reason==S5L8920_BUS_ACCESS_UNIMPLEMENTED,"unaligned MMIO refused");
            }
        }
        for(unsigned off=4;off<=20;off+=4) if(off!=8 && off!=12) {
            s5l8920_clear_bus_failure(&m);(void)m.bus.read32(&m,base+off);
            CHECK(m.bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"unknown indexed data/status/counter guarded");
        }
        CHECK(!memcmp(&before,&m.dart[bank],sizeof before),"invalid bus operations atomic");
    }
    CHECK(m.dart[0].config==0x80000070u && m.dart[0].initial_table_port==0 &&
          m.dart[1].initial_table_port==0x10000000u,"bank isolation");
    const uint32_t unmapped[]={0xbfdffffcu,0xbfe00018u,0xbfe00ffcu,0xbfe01000u,0xbfeffffcu,0xbff01000u,0xc0000000u};
    for(unsigned i=0;i<sizeof unmapped/sizeof unmapped[0];i++) {
        s5l8920_clear_bus_failure(&m);(void)m.bus.read32(&m,unmapped[i]);
        CHECK(m.bus_failure.reason==S5L8920_BUS_UNMAPPED,"no aperture spill or unknown-register zeros");
    }
    CHECK(s5l8920_reset(&m),"SoC reset");
    for(unsigned i=0;i<2;i++) CHECK(m.dart[i].configured && !m.dart[i].programmed &&
        !m.dart[i].config && m.dart[i].command_readable && m.dart[i].table_port_readable,"SoC resets each DART");
}
int main(void) {
    if(!s5l8920_init(&m)) return 1;
    configuration();page_walk();board();s5l8920_free(&m);
    CHECK(!m.dart[0].configured && !m.dart[1].configured,"free invalidates external observations");
    printf("s5l8920_dart: %u passed, %u failed\n",passed,failed);
    return failed?1:0;
}
