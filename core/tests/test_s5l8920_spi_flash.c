/* Raw source clocks and GPIO-controlled flash through board MMIO.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920.h"
#include <stdio.h>
#include <string.h>

static unsigned passed,failed;
#define CHECK(x) do { if(x)++passed; else { if(failed<30u) { \
    printf("FAIL %s:%d: %s\n",__func__,__LINE__,#x); } ++failed; } } while(0)
static s5l8920_t board;
static uint8_t image[SST25VF080B_SIZE];
static const sst25vf080b_timing_t timing={7000,18000000,19000000,20000000,35000000};
static const uint32_t cs=S5L8920_GPIO_BASE+148u*4u,gate=S5L8920_CLOCK_GATE_BASE+9u*4u;
static void write32(uint32_t address,uint32_t value) {
    board.bus.write32(&board,address,value); CHECK(!board.bus_failure.reason);
}
static void prepare(s5l8920_spi_t *s,unsigned bits,uint32_t divider,unsigned words) {
    s5l8920_spi_reset(s); CHECK(s5l8920_spi_configure_link(s,0,1));
    CHECK(s5l8920_spi_write(s,0,0)); CHECK(s5l8920_spi_write(s,12,0));
    CHECK(s5l8920_spi_write(s,4,0x4038u|(bits<<15)));
    CHECK(s5l8920_spi_write(s,0x30,divider)); CHECK(s5l8920_spi_write(s,0x38,0));
    CHECK(s5l8920_spi_write(s,0,13));CHECK(s5l8920_spi_write(s,0x4c,words));
    CHECK(s5l8920_spi_write(s,0x34,words));
    for(unsigned i=0;i<words;++i) CHECK(s5l8920_spi_write(s,0x10,0x12345678u+i));
}

static void test_source_divider(void) {
    const uint32_t divs[]={1,2,3,255,256,1024,UINT32_MAX};
    for(unsigned width=0;width<3;++width)for(unsigned d=0;d<7;++d) {
        s5l8920_spi_t s;prepare(&s,width,divs[d],2); s5l8920_spi_t before=s;
        unsigned bits=8u<<width;uint32_t rx[]={0x87654321u,0xabcdef98u},tx[2]={0};size_t count=7;
        bool ok=s5l8920_spi_source_clock(&s,false,UINT64_MAX,rx,2,tx,2,&count);
        CHECK(ok && count==0 && !memcmp(&s,&before,sizeof s));if(!ok)continue;
        uint64_t one=(uint64_t)bits*divs[d];
        CHECK(s5l8920_spi_source_clock(&s,true,one-1u,NULL,0,NULL,0,&count) && count==0);
        CHECK(s.busy && s.tx_words==2 && s.rx_words==2 && s.tx.count==1);
        CHECK(s.source_phase==divs[d]-1u);
        before=s;count=7;tx[0]=0xa5;
        CHECK(!s5l8920_spi_source_clock(&s,true,1,NULL,0,tx,2,&count));
        CHECK(count==7 && tx[0]==0xa5 && !memcmp(&s,&before,sizeof s));
        CHECK(s5l8920_spi_source_clock(&s,true,1,rx,1,tx,2,&count) && count==1);
        uint32_t mask=width==2?UINT32_MAX:((1u<<bits)-1u);
        CHECK(tx[0]==(0x12345678u&mask));uint32_t received=0;
        CHECK(s5l8920_spi_read(&s,0x20,&received) && received==(rx[0]&mask));
        CHECK(s5l8920_spi_source_clock(&s,true,UINT64_MAX,rx+1,1,tx,2,&count) && count==1);
        CHECK(!s.source_phase && !s.busy && !s.tx_words && !s.rx_words);
    }
}

static void test_phase_guards(void) {
    s5l8920_spi_t s;prepare(&s,0,3,2);size_t count=0;
    bool ok=s5l8920_spi_source_clock(&s,true,1,NULL,0,NULL,0,&count);CHECK(ok);if(!ok)return;
    CHECK(s.busy && s.bits_remaining==8 && s.source_phase==1 && s.tx.count==1);
    s5l8920_spi_t before=s;uint32_t rx=0xa5,tx=0;
    CHECK(!s5l8920_spi_serial_clock(&s,1,&rx,1,&tx,1,&count));CHECK(!memcmp(&s,&before,sizeof s));
    const uint32_t offsets[]={0,12,0x30,0x38,0x4c,0x34,4},values[]={13,2,4,1,3,3,0x403e};
    for(unsigned i=0;i<7;++i) { CHECK(!s5l8920_spi_write(&s,offsets[i],values[i]));CHECK(!memcmp(&s,&before,sizeof s)); }
    CHECK(s5l8920_spi_write(&s,0,0));before=s;
    CHECK(s5l8920_spi_source_clock(&s,true,UINT64_MAX,&rx,1,&tx,1,&count) && count==0);
    CHECK(!memcmp(&s,&before,sizeof s)); CHECK(s5l8920_spi_write(&s,0,1));
    CHECK(s5l8920_spi_source_clock(&s,true,23,&rx,1,&tx,1,&count) && count==1);
    CHECK(s.tx_words==1 && !s.source_phase);
    CHECK(s5l8920_spi_write(&s,0,0));CHECK(s5l8920_spi_write(&s,0x38,5));
    CHECK(s5l8920_spi_write(&s,0,1));
    /* Idle clocks cannot give the following request pre-earned divider phase. */
    CHECK(s5l8920_spi_source_clock(&s,true,UINT64_MAX,&rx,1,&tx,1,&count) && count==1);
    CHECK(!s.source_phase && !s.delay_remaining);
}

static void test_delay_and_rollback(void) {
    s5l8920_spi_t s;prepare(&s,0,3,3);
    CHECK(s5l8920_spi_write(&s,0,0));CHECK(s5l8920_spi_write(&s,0x38,5));
    CHECK(s5l8920_spi_write(&s,4,0x38));CHECK(s5l8920_spi_write(&s,0,1));
    uint32_t rx[]={1,2,3},tx[3]={0};size_t count=19;
    CHECK(s5l8920_spi_source_clock(&s,false,UINT64_MAX,rx,3,tx,3,&count) && count==1);
    CHECK(s.delay_remaining==5 && !s.busy && !s.source_phase);
    s5l8920_spi_t before=s;
    CHECK(s5l8920_spi_source_clock(&s,false,UINT64_MAX,NULL,0,NULL,0,&count) && !count);
    CHECK(!memcmp(&before,&s,sizeof s));CHECK(s5l8920_spi_delay_clock(&s,4));
    CHECK(s.delay_remaining==1);CHECK(s5l8920_spi_delay_clock(&s,1));
    CHECK(s5l8920_spi_source_clock(&s,false,1,NULL,0,NULL,0,&count) && !count);
    CHECK(s.source_phase==1 && s.bits_remaining==8);
    CHECK(s5l8920_spi_source_clock(&s,false,23,rx+1,1,tx,3,&count) && count==1);
    CHECK(s.delay_remaining==5);CHECK(s5l8920_spi_delay_clock(&s,5));
    CHECK(s5l8920_spi_source_clock(&s,false,24,rx+2,1,tx,3,&count) && count==1);
    CHECK(!s.delay_remaining && !s.source_phase && !s.busy);

    prepare(&s,0,3,3);before=s;count=19;tx[0]=tx[1]=tx[2]=0xa5;
    /* Failure after a valid word prefix must publish neither words nor phase. */
    CHECK(!s5l8920_spi_source_clock(&s,true,72,rx,2,tx,3,&count));
    CHECK(count==19 && tx[0]==0xa5 && tx[1]==0xa5 && tx[2]==0xa5 && !memcmp(&before,&s,sizeof s));
    CHECK(!s5l8920_spi_source_clock(&s,true,72,rx,3,tx,2,&count));
    CHECK(count==19 && tx[0]==0xa5 && !memcmp(&before,&s,sizeof s));
    CHECK(s5l8920_spi_source_clock(&s,true,25,rx,1,tx,3,&count) && count==1);
    CHECK(s.busy && s.source_phase==1 && s.bits_remaining==8 && s.tx.count==1);
    before=s;CHECK(s5l8920_spi_source_clock(&s,false,UINT64_MAX,rx,3,tx,3,&count) && !count);
    CHECK(!memcmp(&before,&s,sizeof s));
    CHECK(s5l8920_spi_source_clock(&s,true,47,rx+1,2,tx,3,&count) && count==2);
    CHECK(!s.busy && !s.source_phase);
}

static bool attach(sst25vf080b_t *f,uint8_t known) {
    CHECK(s5l8920_reset(&board));memset(image,0xff,sizeof image);
    CHECK(sst25vf080b_init(f,image,sizeof image,&timing,true,true));CHECK(sst25vf080b_advance(f,100000));
    write32(cs,0x213);CHECK(s5l8920_clock_gate_configure(&board,9,0));write32(gate,15);
    bool ok=s5l8920_spi_attach_flash(&board,0,f,148,9,0,1,0xff,known);CHECK(ok);return ok;
}
static void request(const uint8_t *tx,unsigned n) {
    uint32_t base=S5L8920_SPI_BASE;
    write32(base,0);write32(base+12,0);write32(base+4,0x4038);
    write32(base+0x30,2);write32(base+0x38,0);write32(base,13);
    write32(base+0x4c,n);write32(base+0x34,n);write32(base+4,0x2041b8);
    for(unsigned i=0;i<n;++i)write32(base+0x10,tx[i]);
    write32(cs,0x212);
}

static void test_board_gpio_irq(void) {
    sst25vf080b_t f;if(!attach(&f,0xff))return;
    CHECK(!f.selected);CHECK(s5l8920_spi_attach_flash(&board,0,&f,148,9,0,1,0xff,0xff));
    CHECK(!s5l8920_spi_attach_flash(&board,1,&f,148,9,0,1,0xff,0xff));
    write32(S5L8920_VIC_BASE+0x10,1u<<29);
    const uint8_t sr[]={5,0xff};request(sr,2);CHECK(f.selected);
    size_t count=0;CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,1,&count) && !count);
    CHECK(board.spi[0].busy && board.spi[0].source_phase==1);
    board.bus.write32(&board,cs,0x213);CHECK(board.bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED);
    CHECK(board.gpio[148].control==0x212 && f.selected);s5l8920_clear_bus_failure(&board);
    write32(gate,0);s5l8920_spi_t held=board.spi[0];sst25vf080b_t peer=f;
    CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,UINT64_MAX,&count) && !count);
    CHECK(!memcmp(&held,&board.spi[0],sizeof held) && !memcmp(&peer,&f,sizeof f));write32(gate,15);
    CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,15,&count) && count==1);
    CHECK(board.cpu.irq_line && !board.cpu.fiq_line);
    CHECK(board.bus.read32(&board,S5L8920_SPI_BASE+0x20)==0xff);
    CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,16,&count) && count==1);
    CHECK(board.bus.read32(&board,S5L8920_SPI_BASE+0x20)==0x1c);
    write32(S5L8920_SPI_BASE+8,S5L8920_SPI_EVENT_MASK);CHECK(!board.cpu.irq_line);write32(cs,0x213);CHECK(!f.selected);
    board.bus.write32(&board,cs,0x210);CHECK(board.bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED);
    CHECK(board.gpio[148].control==0x213 && !f.selected);s5l8920_clear_bus_failure(&board);
    const uint8_t id[]={0x9f,0xff,0xff,0xff},want[]={0xff,0xbf,0x25,0x8e};
    request(id,4);write32(S5L8920_VIC_BASE+0xc,1u<<29);
    /* Host clock input preserves an unrelated already latched bus failure. */
    board.bus.read32(&board,0xdead0000);s5l8920_bus_failure_t failure=board.bus_failure;
    uint32_t registers[16];memcpy(registers,board.cpu.r,sizeof registers);
    CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,64,&count) && count==4);
    CHECK(!memcmp(registers,board.cpu.r,sizeof registers) && !memcmp(&failure,&board.bus_failure,sizeof failure));
    CHECK(board.cpu.fiq_line && !board.cpu.irq_line);s5l8920_clear_bus_failure(&board);
    for(unsigned i=0;i<4;++i)CHECK(board.bus.read32(&board,S5L8920_SPI_BASE+0x20)==want[i]);
    write32(cs,0x213);peer=f;CHECK(s5l8920_reset(&board));
    CHECK(!board.spi_flash[0] && !memcmp(&peer,&f,sizeof f));
    count=9;CHECK(!s5l8920_spi_bank_flash_clock(&board,0,true,1,&count) && count==9);
}

static void test_board_rollback(void) {
    sst25vf080b_t f;if(!attach(&f,0))return;
    const uint8_t sr[]={5,0xff};request(sr,2);
    s5l8920_spi_t before=board.spi[0];sst25vf080b_t peer=f;size_t count=19;
    CHECK(!s5l8920_spi_bank_flash_clock(&board,0,true,32,&count) && count==19);
    CHECK(!memcmp(&before,&board.spi[0],sizeof before) && !memcmp(&peer,&f,sizeof f));
    CHECK(!s5l8920_spi_attach_flash(&board,0,&f,148,9,0,1,0xff,0xff));
    CHECK(s5l8920_reset(&board));CHECK(!board.spi_flash[0]);
}

static void test_board_ce_commit(void) {
    sst25vf080b_t f;if(!attach(&f,0xff))return;
    size_t count=0;
    const uint8_t enable[]={6},unprotect[]={1,0},program[]={2,0,0x20,0,0xa5};
    request(enable,1);CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,16,&count) && count==1);
    CHECK(!(f.status&2));write32(cs,0x213);CHECK(f.status&2);
    request(unprotect,2);CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,32,&count) && count==2);
    write32(cs,0x213);CHECK(!f.status);
    request(enable,1);CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,16,&count));write32(cs,0x213);
    request(program,5);CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,80,&count) && count==5);
    CHECK(!f.busy_ns && image[0x2000]==0xff);write32(cs,0x213);
    CHECK(f.busy_ns==7000 && image[0x2000]==0xff);
    CHECK(sst25vf080b_advance(&f,7000));CHECK(image[0x2000]==0xa5);
    request(enable,1);CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,16,&count));write32(cs,0x213);
    request(program,5);CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,80,&count));
    sst25vf080b_t before=f;s5l8920_gpio_pin_t pin=board.gpio[148];
    board.bus.write32(&board,cs,0x213);
    CHECK(board.bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED);
    CHECK(!memcmp(&before,&f,sizeof f) && !memcmp(&pin,&board.gpio[148],sizeof pin));
    CHECK(image[0x2000]==0xa5);s5l8920_clear_bus_failure(&board);
    CHECK(s5l8920_reset(&board));CHECK(!memcmp(&before,&f,sizeof f));
}

static void test_external_ce_and_binding(void) {
    sst25vf080b_t f;if(!attach(&f,0xff))return;
    const uint8_t sr[]={5,0xff};request(sr,2);write32(cs,0x213);
    /* GPIO CE can be high while the controller's software CS remains zero. */
    CHECK(!f.selected && !board.spi[0].pin);size_t count=0;
    CHECK(s5l8920_spi_bank_flash_clock(&board,0,true,32,&count) && count==2);
    CHECK(!f.selected && !f.position);
    CHECK(board.bus.read32(&board,S5L8920_SPI_BASE+0x20)==0xff);
    CHECK(board.bus.read32(&board,S5L8920_SPI_BASE+0x20)==0xff);
    uint32_t tx=0xa5,rx=0x1c;count=19;
    CHECK(!s5l8920_spi_bank_serial_clock(&board,0,8,&rx,1,&tx,1,&count));
    CHECK(count==19 && tx==0xa5);
    sst25vf080b_t other=f;s5l8920_spi_t before=board.spi[1];
    CHECK(!s5l8920_spi_attach_flash(&board,1,&other,148,9,0,1,0xff,0xff));
    CHECK(!memcmp(&before,&board.spi[1],sizeof before) && !board.spi_flash[1]);
    CHECK(!s5l8920_spi_attach_flash(&board,1,&other,149,9,0,1,0xff,0xff));
    write32(S5L8920_GPIO_BASE+149u*4u,0x213);
    CHECK(!s5l8920_spi_attach_flash(&board,1,&other,149,9,16,1,0xff,0xff));
    CHECK(!memcmp(&before,&board.spi[1],sizeof before));
    write32(S5L8920_SPI_BASE+S5L8920_SPI_STRIDE,0);
    CHECK(!s5l8920_spi_attach_flash(&board,1,&other,149,9,0,1,0xff,0xff));
    CHECK(s5l8920_reset(&board));
}
int main(void) {
    CHECK(s5l8920_init(&board));if(!board.ram)return 1;
    test_source_divider();test_phase_guards();test_delay_and_rollback();
    test_board_gpio_irq();test_board_rollback();test_board_ce_commit();test_external_ce_and_binding();
    s5l8920_free(&board);printf("s5l8920_spi_flash: %u passed, %u failed\n",passed,failed);return failed?1:0;
}
