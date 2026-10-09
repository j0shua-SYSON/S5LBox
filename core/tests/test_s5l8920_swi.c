/* SWI command ownership, missing clocks/receiver and reset cancellation.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_swi.h"
#include "s5l8920.h"
#include <stdio.h>
#include <string.h>
static unsigned passed,failed;
#define CHECK(x) do { if (x) ++passed; else { if (failed<24u) { \
    printf("FAIL %s:%d %s\n",__func__,__LINE__,#x); } ++failed; } } while (0)
static s5l8920_swi_t configured(void) {
    s5l8920_swi_t s={0};const s5l8920_swi_input_t input={77u,0u,0u};
    CHECK(s5l8920_swi_configure(&s,&input));return s;
}
static s5l8920_swi_t queued(unsigned divider,unsigned command) {
    s5l8920_swi_t s=configured();
    CHECK(s5l8920_swi_write(&s,0x24u,16000u));
    CHECK(s5l8920_swi_write(&s,0u,((divider-1u)<<8)|3u));
    CHECK(s5l8920_swi_write(&s,0x18u,command==1u?0x5100u:0x0da5u));
    CHECK(s5l8920_swi_write(&s,0x14u,command));return s;
}
static void refuse(s5l8920_swi_t *s,uint32_t offset,uint32_t value) {
    s5l8920_swi_t before=*s;CHECK(!s5l8920_swi_write(s,offset,value));CHECK(!memcmp(&before,s,sizeof before));
}
static void test_inputs_and_registers(void) {
    s5l8920_swi_t s={0},before=s;uint32_t value=0xdeadbeefu;
    s5l8920_swi_input_t input={0u,0u,0u};
    CHECK(!s5l8920_swi_configure(&s,&input) && !memcmp(&s,&before,sizeof s));
    input.transfer_cycles=77u;input.idle_primary=1u;CHECK(!s5l8920_swi_configure(&s,&input));
    input.idle_primary=0u;input.idle_secondary=4u;CHECK(!s5l8920_swi_configure(&s,&input));
    input.idle_secondary=0u;
    CHECK(!s5l8920_swi_configure(NULL,&input) && !s5l8920_swi_configure(&s,NULL));
    CHECK(!s5l8920_swi_source_clock(&s,1u) && !s5l8920_swi_read(&s,0x14u,&value) && value==0xdeadbeefu);
    s=configured();before=s;
    for (unsigned offset=0;offset<0x30u;offset+=4u) {
        value=0xdeadbeefu;
        bool known=offset==0x14u || offset==0x1cu;
        CHECK(s5l8920_swi_read(&s,offset,&value)==known && value==(known?0u:0xdeadbeefu));
    }
    CHECK(!memcmp(&before,&s,sizeof s));
    refuse(&s,0x14u,1u);refuse(&s,0u,1u);refuse(&s,0u,2u);refuse(&s,0u,0x10003u);
    refuse(&s,0x18u,0x8000u);refuse(&s,0x20u,UINT32_MAX);refuse(&s,0x1cu,3u);refuse(&s,0xcu,UINT32_MAX);
    refuse(&s,1u,0u);refuse(&s,0x28u,0u);
    CHECK(s5l8920_swi_write(&s,0x24u,16000u) && s5l8920_swi_write(&s,0u,0xb03u));
    refuse(&s,0x14u,1u);
    CHECK(s5l8920_swi_write(&s,0x18u,0x5100u) && s5l8920_swi_write(&s,0x20u,0xda5u));
    CHECK(s5l8920_swi_read(&s,0u,&value) && value==0xb03u);
    CHECK(s5l8920_swi_read(&s,0x24u,&value) && value==16000u);
    CHECK(s5l8920_swi_read(&s,0x20u,&value) && value==0xda5u);
    CHECK(s5l8920_swi_write(&s,0u,0xb00u));refuse(&s,0x14u,1u);
    CHECK(s5l8920_swi_write(&s,0u,0xb03u) && s5l8920_swi_write(&s,0x14u,1u));
    before=s;CHECK(s5l8920_swi_configure(&s,&input) && !memcmp(&before,&s,sizeof s));
    input.transfer_cycles=78u;CHECK(!s5l8920_swi_configure(&s,&input) && !memcmp(&before,&s,sizeof s));
}
static void test_clock_receiver_and_reset(void) {
    for (unsigned mode=1u;mode<=3u;mode+=2u) {
        s5l8920_swi_t s=queued(12u,mode),before=s;
        s5l8920_swi_request_t request={0},guard=request;bool ready=false;uint32_t value;
        for (unsigned i=0;i<100u;++i) {
            CHECK(s5l8920_swi_read(&s,0x14u,&value) && value==mode);
            CHECK(s5l8920_swi_peek(&s,&request,&ready) && !ready);
        }
        CHECK(!memcmp(&s,&before,sizeof s));
        CHECK(request.sequence==1u && request.command==mode && request.control==0xb03u &&
            request.str_delay==16000u && request.word==(mode==1u?0x5100u:0xda5u));
        request=guard;CHECK(!s5l8920_swi_take(&s,1u,&request) && !memcmp(&request,&guard,sizeof request));
        CHECK(s5l8920_swi_source_clock(&s,923u));
        CHECK(s.remaining==1u && s.phase==11u && !s5l8920_swi_take(&s,1u,&request));
        CHECK(s5l8920_swi_source_clock(&s,1u) && s.pending && s5l8920_swi_peek(&s,&request,&ready) && ready);
        before=s;CHECK(s5l8920_swi_source_clock(&s,UINT64_MAX) && !memcmp(&s,&before,sizeof s));
        refuse(&s,0x18u,0x5200u);refuse(&s,0x20u,0x880u);refuse(&s,0x14u,mode);
        refuse(&s,0u,0xc03u);refuse(&s,0x24u,16001u);
        CHECK(s5l8920_swi_write(&s,0u,0xb03u) && s5l8920_swi_write(&s,0x24u,16000u) && !memcmp(&s,&before,sizeof s));
        CHECK(!s5l8920_swi_take(&s,2u,&request) && !memcmp(&s,&before,sizeof s));
        CHECK(s5l8920_swi_take(&s,1u,&request) && !s.pending && s5l8920_swi_read(&s,0x14u,&value) && value==0u);
        CHECK(!s5l8920_swi_take(&s,1u,&request));
        CHECK(s5l8920_swi_write(&s,0x14u,mode) && s.sequence==2u);
        s5l8920_swi_reset(&s);CHECK(s.configured && !s.pending && !s.known && s.sequence==2u);
        CHECK(s5l8920_swi_write(&s,0x24u,16000u) && s5l8920_swi_write(&s,0u,0xb03u) &&
            s5l8920_swi_write(&s,0x18u,0x5200u) && s5l8920_swi_write(&s,0x14u,mode));
        CHECK(s5l8920_swi_source_clock(&s,UINT64_MAX) && !s5l8920_swi_take(&s,2u,&request) &&
            s5l8920_swi_take(&s,3u,&request) && request.word==0x5200u);
        s.sequence=UINT64_MAX;refuse(&s,0x14u,mode);
    }
    s5l8920_swi_t s={0};const s5l8920_swi_input_t explicit_readback={1u,2u,2u};
    CHECK(s5l8920_swi_configure(&s,&explicit_readback));
    CHECK(s5l8920_swi_write(&s,0u,3u) && s5l8920_swi_write(&s,0x24u,0u) &&
        s5l8920_swi_write(&s,0x18u,0x7fffu) && s5l8920_swi_write(&s,0x14u,3u));
    s5l8920_swi_request_t request;uint32_t value;
    CHECK(s5l8920_swi_source_clock(&s,1u) && s5l8920_swi_take(&s,1u,&request));
    CHECK(s5l8920_swi_read(&s,0x14u,&value) && value==2u && s5l8920_swi_read(&s,0x1cu,&value) && value==2u);
}
static void test_split_clock(void) {
    const unsigned dividers[]={1u,12u,256u};
    for (unsigned d=0;d<3u;++d) {
        unsigned boundary=77u*dividers[d];
        for (unsigned delta=0;delta<5u;++delta) {
            uint64_t total=boundary-2u+delta;
            for (unsigned split=0;split<7u;++split) {
                s5l8920_swi_t a=queued(dividers[d],3u),b=a;
                uint64_t first=total*split/6u;
                CHECK(s5l8920_swi_source_clock(&a,total) && s5l8920_swi_source_clock(&b,first) &&
                    s5l8920_swi_source_clock(&b,total-first) && !memcmp(&a,&b,sizeof a));
                CHECK(a.pending && (a.remaining==0u)==(total>=boundary));
            }
        }
    }
}
static void test_board_gate_and_lifetime(void) {
    s5l8920_t m={0};s5l8920_swi_t s=configured(),other=configured();
    CHECK(!s5l8920_swi_attach(NULL,&s) && !s5l8920_swi_board_source_clock(&m,1u));
    CHECK(s5l8920_init(&m));if (!m.ram) return;
    m.bus.write32(&m,S5L8920_SWI_BASE+0x24u,16000u);
    CHECK(m.bus_failure.reason==S5L8920_BUS_UNMAPPED);s5l8920_clear_bus_failure(&m);
    CHECK(!s5l8920_swi_attach(&m,&s));
    CHECK(s5l8920_clock_gate_configure(&m,S5L8920_SWI_GATE,15u) && s5l8920_swi_attach(&m,&s));
    CHECK(s5l8920_swi_attach(&m,&s) && !s5l8920_swi_attach(&m,&other));
    m.bus.write32(&m,S5L8920_SWI_BASE+0x24u,16000u);
    m.bus.write32(&m,S5L8920_SWI_BASE,0xb03u);
    m.bus.write32(&m,S5L8920_SWI_BASE+0x18u,0xda5u);
    m.bus.write32(&m,S5L8920_SWI_BASE+0x14u,3u);
    CHECK(!m.bus_failure.reason && s.pending && s5l8920_swi_board_source_clock(&m,12u) && s.remaining==76u);
    for (unsigned gate=0u;gate<16u;++gate) {
        m.bus.write32(&m,S5L8920_CLOCK_GATE_BASE+4u*S5L8920_SWI_GATE,gate);
        CHECK((gate==0u || gate==15u)?!m.bus_failure.reason:
            m.bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED);
        s5l8920_clear_bus_failure(&m);
        /* Exercise an explicit intermediate hardware observation; the guest
         * gate programming API correctly refuses to create this state. */
        m.clock_gate[S5L8920_SWI_GATE].value=gate;
        s5l8920_swi_t before=s;
        CHECK(s5l8920_swi_board_source_clock(&m,1u)==(gate==0u || gate==15u));
        if (gate!=15u) CHECK(!memcmp(&s,&before,sizeof s));
        uint32_t value=m.bus.read32(&m,S5L8920_SWI_BASE+0x14u);
        CHECK((gate==15u && value==3u && !m.bus_failure.reason) ||
            (gate!=15u && m.bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED));
        s5l8920_clear_bus_failure(&m);
    }
    s5l8920_swi_t before=s;
    m.bus.write8(&m,S5L8920_SWI_BASE+0x18u,0u);
    CHECK(m.bus_failure.reason==S5L8920_BUS_ACCESS_UNIMPLEMENTED && !memcmp(&s,&before,sizeof s));
    s5l8920_clear_bus_failure(&m);
    m.bus.write32(&m,S5L8920_SWI_BASE+0x28u,0u);
    CHECK(m.bus_failure.reason==S5L8920_BUS_UNMAPPED && !memcmp(&s,&before,sizeof s));
    s5l8920_clear_bus_failure(&m);
    CHECK(s5l8920_reset(&m) && !m.swi && !memcmp(&s,&before,sizeof s));
    CHECK(s5l8920_swi_attach(&m,&s));s5l8920_free(&m);
    CHECK(!m.swi && !memcmp(&s,&before,sizeof s));
}
int main(void) {
    test_inputs_and_registers();test_clock_receiver_and_reset();test_split_clock();
    test_board_gate_and_lifetime();
    printf("SWI: %u passed, %u failed\n",passed,failed);return failed?1:0;
}
