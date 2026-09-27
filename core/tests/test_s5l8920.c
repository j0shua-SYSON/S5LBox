/* Partial N88 fabric: physical mapping, checked stops and CPU interrupt wiring.
 * Synthetic instructions establish component behavior, not a firmware boot.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920.h"
#include <stdio.h>
#include <string.h>

static unsigned passed, failed;
#define CHECK(c, msg) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: %s\n",__func__,__LINE__,msg); } } while (0)

static void put(s5l8920_t *m, uint32_t offset, uint32_t value) {
    m->bus.write32(m->bus.ctx,S5L8920_RAM_BASE + offset,value);
}
static uint32_t vic_address(unsigned bank, uint32_t offset) {
    return S5L8920_VIC_BASE + bank * S5L8920_VIC_STRIDE + offset;
}
static void vic_write(s5l8920_t *m, unsigned bank, uint32_t offset, uint32_t value) {
    m->bus.write32(m->bus.ctx,vic_address(bank,offset),value);
}

static void test_geometry_and_refusals(s5l8920_t *m) {
    CHECK(m->cpu.arch == ARM_ARCH_V7_CORTEX_A8 && m->cpu.bus == &m->bus &&
          m->bus.ctx == m && m->bus.access_failed && !m->bus.privileged_svc_handler &&
          !m->bus.wait_for_interrupt, "wrong CPU or inherited legacy host services");
    uint8_t *ram = m->ram;
    CHECK(!s5l8920_init(m) && m->ram == ram, "double initialization lost the allocation");
    CHECK(arm_step(&m->cpu) == ARM_HALT && m->cpu.r[15] == 0u && m->cpu.cycles == 0u &&
          m->bus_failure.reason == S5L8920_BUS_UNMAPPED && m->bus_failure.address == 0u,
          "missing reset ROM did not stop before executing invented data");
    CHECK(s5l8920_reset(m), "reset");
    m->bus.write32(m->bus.ctx,S5L8920_RAM_BASE + 3u,0x87654321u);
    CHECK(m->ram[3] == 0x21u && m->ram[4] == 0x43u && m->ram[5] == 0x65u && m->ram[6] == 0x87u &&
          m->bus.read16(m->bus.ctx,S5L8920_RAM_BASE + 4u) == 0x6543u &&
          m->bus.read8(m->bus.ctx,S5L8920_RAM_BASE + 6u) == 0x87u, "little-endian RAM widths");
    CHECK(m->bus.host_ram(m,S5L8920_RAM_BASE,0x400u) == m->ram &&
          m->bus.host_ram_write(m,S5L8920_RAM_BASE,0x400u) == m->ram &&
          !m->bus.host_ram(m,S5L8920_VIC_BASE,4u) &&
          !m->bus.host_ram(m,S5L8920_RAM_BASE - 1u,4u) &&
          !m->bus.host_ram(m,UINT32_MAX,4u), "RAM shortcuts exposed devices or wrapped ranges");
    CHECK(!s5l8920_load(m,S5L8920_RAM_BASE,m->ram,SIZE_MAX) &&
          !s5l8920_load(m,S5L8920_VIC_BASE,m->ram,4u) &&
          !s5l8920_load(m,S5L8920_RAM_BASE,NULL,1u) &&
          s5l8920_load(m,S5L8920_RAM_BASE + S5L8920_RAM_SIZE,NULL,0u), "host load bounds");
    const uint32_t invalid[] = {
        S5L8920_RAM_BASE - 4u, S5L8920_RAM_BASE + S5L8920_RAM_SIZE,
        UINT32_MAX - 1u, 0x82501000u, 0x3cc00000u,
        S5L8920_VIC_BASE + 0x1000u, S5L8920_VIC_BASE + 0xffffu,
        S5L8920_VIC_BASE + 3u * S5L8920_VIC_STRIDE
    };
    for (unsigned n = 0; n < sizeof invalid/sizeof invalid[0]; n++) {
        s5l8920_clear_bus_failure(m);
        m->cpu.r[15] = 0x1234u;
        m->bus.write32(m,invalid[n],0xdeadbeefu);
        CHECK(m->bus_failure.reason == S5L8920_BUS_UNMAPPED && m->bus_failure.address == invalid[n] &&
              m->bus_failure.pc == 0x1234u && m->bus_failure.size == 4u && m->bus_failure.write &&
              m->bus_failure.value == 0xdeadbeefu, "unmapped write diagnostics");
        m->bus.write32(m,S5L8920_RAM_BASE + 3u,0u);
        (void)m->bus.read8(m,0u);
        CHECK(m->ram[3] == 0x21u && m->bus_failure.address == invalid[n] && m->bus_failure.write,
              "latched bus failure allowed later effects or lost first diagnostics");
    }
    s5l8920_clear_bus_failure(m);
    const uint32_t refused[] = {PL192_PROTECTION,0xfe0u,0xffcu,0x02cu};
    for (unsigned bank = 0; bank < S5L8920_VIC_COUNT; bank++)
     for (unsigned n = 0; n < sizeof refused/sizeof refused[0]; n++) {
        s5l8920_clear_bus_failure(m);
        (void)m->bus.read32(m,vic_address(bank,refused[n]));
        CHECK(m->bus_failure.reason == S5L8920_BUS_REGISTER_REFUSED && !m->vic[bank].protection,
              "unverified revision/protection or unknown selector returned a value");
        s5l8920_clear_bus_failure(m);
        vic_write(m,bank,refused[n],1u);
        CHECK(m->bus_failure.reason == S5L8920_BUS_REGISTER_REFUSED && !m->vic[bank].protection,
              "unsupported register write committed");
    }
    for (unsigned kind = 0; kind < 3u; kind++) {
        s5l8920_clear_bus_failure(m);
        if (!kind) (void)m->bus.read8(m,S5L8920_VIC_BASE);
        else if (kind == 1u) m->bus.write16(m,S5L8920_VIC_BASE + PL192_INTENABLE,0xffffu);
        else m->bus.write32(m,S5L8920_VIC_BASE + PL192_INTENABLE + 1u,UINT32_MAX);
        CHECK(m->bus_failure.reason == S5L8920_BUS_ACCESS_UNIMPLEMENTED && m->vic[0].enable == 0u,
              "wrong MMIO width/alignment reached a device");
    }
    s5l8920_clear_bus_failure(m);
    vic_write(m,0u,PL192_VECTPRIORITY0,16u);
    CHECK(m->bus_failure.reason == S5L8920_BUS_REGISTER_REFUSED && m->vic[0].priority[0] == 15u,
          "reserved register bits changed device state");
    CHECK(s5l8920_reset(m), "reset after refusals");
}

static void test_cpu_access_stops(s5l8920_t *m) {
    put(m,0u,0xe5912000u); /* LDR r2,[r1] */
    m->cpu.r[15] = S5L8920_RAM_BASE; m->cpu.r[1] = 0x82500000u; m->cpu.r[2] = 0xdeadbeefu;
    CHECK(arm_step(&m->cpu) == ARM_HALT && m->cpu.r[15] == S5L8920_RAM_BASE &&
          m->cpu.r[2] == 0xdeadbeefu && m->cpu.cycles == 0u && m->cpu.cp15.dfsr == 0u &&
          m->bus_failure.address == 0x82500000u, "unimplemented UART read retired or became a guest abort");
    s5l8920_clear_bus_failure(m);
    put(m,0x100u,0x12345678u); m->cpu.r[1] = S5L8920_RAM_BASE + 0x100u;
    CHECK(arm_step(&m->cpu) == ARM_OK && m->cpu.r[2] == 0x12345678u && m->cpu.cycles == 1u,
          "explicitly repaired access did not retry through a warm fetch cache");
    CHECK(s5l8920_reset(m), "reset cross-page test");
    put(m,0u,0xe4812004u); /* STR r2,[r1],#4 */
    m->cpu.r[15] = S5L8920_RAM_BASE; m->cpu.r[1] = S5L8920_RAM_BASE + S5L8920_RAM_SIZE - 2u;
    m->cpu.r[2] = 0xabcdef12u;
    CHECK(arm_step(&m->cpu) == ARM_HALT && m->cpu.cycles == 0u &&
          m->cpu.r[1] == S5L8920_RAM_BASE + S5L8920_RAM_SIZE - 2u &&
          m->ram[S5L8920_RAM_SIZE - 2u] == 0x12u && m->ram[S5L8920_RAM_SIZE - 1u] == 0xefu &&
          m->bus_failure.address == S5L8920_RAM_BASE + S5L8920_RAM_SIZE &&
          m->bus_failure.size == 1u && m->bus_failure.value == 0xcdu,
          "cross-page failure lost committed prefix or retired writeback");
    CHECK(s5l8920_reset(m), "reset walk test");
    m->cpu.cp15.sctlr |= ARM_SCTLR_M; m->cpu.cp15.ttbr0 = 0x82500000u;
    CHECK(arm_step(&m->cpu) == ARM_HALT && m->cpu.cp15.ifsr == 0u && m->cpu.cycles == 0u &&
          m->bus_failure.address == 0x82500000u && !m->bus_failure.write,
          "unimplemented table backing was decoded as a zero descriptor");
    CHECK(s5l8920_reset(m), "reset after walk test");
}

static void map_test_vectors(s5l8920_t *m) {
    put(m,0x4000u,S5L8920_RAM_BASE | 0xc0au); /* VA0 -> first RAM section, Normal RW. */
    put(m,0x4000u + (S5L8920_VIC_BASE >> 20) * 4u,S5L8920_VIC_BASE | 0xc02u);
    m->cpu.cp15.ttbr0 = S5L8920_RAM_BASE + 0x4000u;
    m->cpu.cp15.dacr = 1u; m->cpu.cp15.sctlr |= ARM_SCTLR_M | ARM_SCTLR_XP;
    m->cpu.cpsr = ARM_MODE_SYS | ARM_CPSR_C;
    m->cpu.r[15] = 0x200u;
}

static void test_uart_checked_bus(s5l8920_t *m) {
    const uint32_t uart=S5L8920_UART0_BASE;
    CHECK(s5l8920_reset(m), "UART test reset");
    CHECK(!m->bus.host_ram(m,uart,4u) && !m->bus.host_ram_write(m,uart,4u) &&
          !s5l8920_load(m,uart,m->ram,4u), "UART exposed as host RAM");
    put(m,0u,0xe5812000u); /* STR r2,[r1] */
    m->cpu.r[15]=S5L8920_RAM_BASE; m->cpu.r[1]=uart; m->cpu.r[2]=3u;
    CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==1u && m->uart0.ulcon==3u &&
          m->bus.read32(m,uart)==3u, "CPU did not program/read actual UART");
    const uint32_t registers[]={4u,12u,40u,8u};
    const uint32_t values[]={0x405u,1u,0x80019u,3u};
    put(m,4u,0xe5912000u); /* LDR r2,[r1] */
    m->cpu.r[1]=uart+16u; m->cpu.r[2]=0xdeadbeefu;
    CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.r[2]==0xdeadbeefu && m->cpu.cycles==1u &&
          m->cpu.r[15]==S5L8920_RAM_BASE+4u && m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,
          "partially configured UART fabricated status");
    s5l8920_clear_bus_failure(m);
    for (unsigned n=0;n<4u;n++) m->bus.write32(m,uart+registers[n],values[n]);
    CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[2]==6u && m->cpu.cycles==2u,
          "configuration completion did not repair checked read");
    CHECK(s5l8920_uart_receive(&m->uart0,0xa5u), "host completed receive frame");
    put(m,8u,0xe5d12000u); /* LDRB r2,[r1] */
    m->cpu.r[1]=uart+36u; m->cpu.r[2]=0xdeadbeefu;
    s5l8920_uart_t before=m->uart0;
    CHECK(arm_step(&m->cpu)==ARM_HALT && m->bus_failure.reason==S5L8920_BUS_ACCESS_UNIMPLEMENTED &&
          m->bus_failure.address==uart+36u && m->bus_failure.size==1u &&
          m->cpu.r[2]==0xdeadbeefu && m->cpu.cycles==2u && !memcmp(&before,&m->uart0,sizeof before),
          "unsupported data width consumed RX byte or retired");
    m->bus.write32(m,uart+8u,7u);
    CHECK(!memcmp(&before,&m->uart0,sizeof before), "latched failure allowed later UART effects");
    s5l8920_clear_bus_failure(m); put(m,8u,0xe5912000u);
    CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[2]==0xa5u && m->cpu.cycles==3u &&
          m->uart0.rx_count==0u, "word retry did not consume exactly one byte");
    for (unsigned n=0;n<17u;n++) m->bus.write32(m,uart+32u,n);
    CHECK(m->bus_failure.reason==S5L8920_BUS_OK && m->uart0.tx_count==16u && m->uart0.tx_busy,
          "fill actual UART FIFO and shifter");
    put(m,12u,0xe4812004u); /* STR r2,[r1],#4 */
    m->cpu.r[1]=uart+32u; m->cpu.r[2]=0xb5u; before=m->uart0;
    CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.r[1]==uart+32u && m->cpu.cycles==3u &&
          m->cpu.r[15]==S5L8920_RAM_BASE+12u && m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
          m->bus_failure.write && m->bus_failure.value==0xb5u && !memcmp(&before,&m->uart0,sizeof before),
          "full transmitter accepted data or retired postindex");
    uint8_t output[17]; size_t count=0u;
    CHECK(s5l8920_uart_clock(&m->uart0,true,UINT64_MAX,output,sizeof output,&count) && count==17u,
          "external selected-clock advance");
    for (unsigned n=0;n<17u;n++) CHECK(output[n]==n, "board TX output order");
    s5l8920_clear_bus_failure(m);
    CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[1]==uart+36u && m->cpu.cycles==4u &&
          m->uart0.tx_shift==0xb5u && m->uart0.tx_busy, "full FIFO repair did not retry once");
    CHECK(!m->cpu.irq_line && !m->cpu.fiq_line && m->input_levels[0]==0u,
          "polled UART asserted an unimplemented interrupt");
    s5l8920_clear_bus_failure(m); m->bus.write32(m,uart+4u,0x1405u);
    CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && m->uart0.ucon==0x405u,
          "unsupported receive interrupt mode silently enabled");
    s5l8920_clear_bus_failure(m); (void)m->bus.read32(m,uart+0xfffu);
    CHECK(m->bus_failure.reason==S5L8920_BUS_ACCESS_UNIMPLEMENTED, "aperture end alignment");
    CHECK(s5l8920_reset(m) && !m->uart0.tx_busy && !m->uart0.programmed &&
          m->bus_failure.reason==S5L8920_BUS_OK, "functional board reset retained UART state");
}

static void test_uart_interrupt_wiring(s5l8920_t *m) {
    const uint32_t uart=S5L8920_UART0_BASE, line=1u<<S5L8920_UART0_IRQ;
    for (unsigned fiq=0u;fiq<2u;fiq++) {
        CHECK(s5l8920_reset(m), "reset UART interrupt fixture");
        const uint32_t offsets[]={0u,4u,12u,40u,8u},values[]={3u,0x405u,1u,0x80019u,3u};
        for (unsigned n=0;n<5u;n++) m->bus.write32(m,uart+offsets[n],values[n]);
        vic_write(m,0u,PL192_VECTADDR0+4u*S5L8920_UART0_IRQ,0x80000018u);
        vic_write(m,0u,PL192_INTSELECT,fiq?line:0u);
        vic_write(m,0u,PL192_INTENABLE,line);
        m->bus.write32(m,uart+32u,0x41u); m->bus.write32(m,uart+32u,0x42u);
        m->bus.write32(m,uart+16u,0x20u); m->bus.write32(m,uart+4u,0x2405u);
        CHECK(!m->cpu.irq_line && !m->cpu.fiq_line && !(m->vic[0].input&line),
              "IRQ asserted before a new FIFO-empty event");
        uint8_t output[2]={0xeeu,0xeeu}; size_t count=777u;
        s5l8920_uart_t before=m->uart0;
        CHECK(!s5l8920_uart0_clock(m,true,2080u,output,0u,&count) && count==777u && output[0]==0xeeu &&
              !memcmp(&before,&m->uart0,sizeof before) && !(m->vic[0].input&line),
              "failed host output changed UART or board IRQ state");
        CHECK(s5l8920_uart0_clock(m,false,UINT64_MAX,output,sizeof output,&count) && count==0u &&
              !m->cpu.irq_line && !m->cpu.fiq_line, "unselected source reached interrupt fabric");
        CHECK(s5l8920_uart0_clock(m,true,2079u,output,sizeof output,&count) && count==0u &&
              !m->cpu.irq_line && !m->cpu.fiq_line, "board interrupt arrived a cycle early");
        CHECK(s5l8920_uart0_clock(m,true,1u,output,sizeof output,&count) && count==1u && output[0]==0x41u &&
              m->cpu.irq_line==(fiq==0u) && m->cpu.fiq_line==(fiq!=0u) && (m->vic[0].input&line) &&
              m->uart0.tx_busy && m->uart0.tx_shift==0x42u,
              "time-driven UART cause was not routed immediately to the selected CPU line");
        CHECK(s5l8920_set_irq(m,S5L8920_UART0_IRQ,true), "external shared input assert");
        m->bus.write32(m,uart+16u,0x20u);
        CHECK(!s5l8920_uart_irq(&m->uart0) && (m->vic[0].input&line) && (m->cpu.irq_line || m->cpu.fiq_line),
              "UART W1C erased externally held source24");
        CHECK(s5l8920_set_irq(m,S5L8920_UART0_IRQ,false) && !(m->vic[0].input&line) &&
              !m->cpu.irq_line && !m->cpu.fiq_line, "external withdrawal did not release acknowledged UART line");
        CHECK(s5l8920_uart0_receive(m,0x99u) && !m->cpu.irq_line && !m->cpu.fiq_line,
              "receive-only pending state incorrectly drove TX interrupt");
        m->bus.write32(m,uart+32u,0x43u);
        CHECK(s5l8920_uart0_clock(m,true,2080u,output,sizeof output,&count) && count==1u && output[0]==0x42u,
              "second time-driven event");
        CHECK(s5l8920_set_irq(m,S5L8920_UART0_IRQ,false) && (m->vic[0].input&line) &&
              (m->cpu.irq_line || m->cpu.fiq_line), "host input update erased internal UART cause");
        m->bus.write32(m,uart+4u,0x405u);
        CHECK(!m->cpu.irq_line && !m->cpu.fiq_line && m->uart0.tx_remaining==2080u && (m->uart0.pending&0x20u),
              "active interrupt disable altered pending frame/cause");
        m->bus.write32(m,uart+4u,0x2405u);
        CHECK(m->cpu.irq_line==(fiq==0u) && m->cpu.fiq_line==(fiq!=0u), "pending enable did not refresh board line");

        /* Explicit synthetic vectors validate CPU/device wiring, not N88's
         * unestablished real exception-vector setup or registered callbacks. */
        map_test_vectors(m);
        put(m,0x4000u+(uart>>20)*4u,uart|0xc02u);
        uint32_t vector_offset=fiq?0x1cu:0x18u;
        put(m,vector_offset,0xea000000u|((0x1000u-vector_offset-8u)>>2));
        uint32_t address=0x1000u;
        if (!fiq) { put(m,address,0xe5932000u); address+=4u; } /* LDR r2,[r3], VIC ACK */
        put(m,address,0xe5810000u); address+=4u;              /* STR r0,[r1], UART W1C */
        if (!fiq) { put(m,address,0xe5834000u); address+=4u; } /* STR r4,[r3], VIC EOI */
        put(m,address,0xe25ef004u);                          /* SUBS pc,lr,#4 */
        m->cpu.r[0]=0x20u; m->cpu.r[1]=uart+16u; m->cpu.r[2]=0x12345678u;
        m->cpu.r[3]=vic_address(0u,PL192_ADDRESS); m->cpu.r[4]=0u;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[15]==vector_offset && m->cpu.r[14]==0x204u &&
              (m->cpu.cpsr&ARM_CPSR_MODE_MASK)==(fiq?ARM_MODE_FIQ:ARM_MODE_IRQ), "UART-driven CPU exception entry");
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[15]==0x1000u, "synthetic UART vector branch");
        unsigned steps=0u;
        while (m->cpu.r[15]!=0x200u && steps++<5u) if (arm_step(&m->cpu)!=ARM_OK) break;
        CHECK(m->cpu.r[15]==0x200u && m->cpu.cpsr==(ARM_MODE_SYS|ARM_CPSR_C) &&
              m->bus_failure.reason==S5L8920_BUS_OK && !m->cpu.irq_line && !m->cpu.fiq_line &&
              !(m->uart0.pending&0x20u) && m->vic[0].in_service==0u && m->uart0.tx_remaining==2080u,
              "UART W1C/VIC EOI/exception return failed or CPU steps drained transmitter");
        if (!fiq) CHECK(m->cpu.r[2]==0x80000018u, "UART source24 vector acknowledgement");
        CHECK(s5l8920_set_irq(m,S5L8920_UART0_IRQ,true) && s5l8920_reset(m) &&
              m->input_levels[0]==line && m->vic[0].input==line && !m->uart0.pending &&
              !m->uart0.programmed && !m->cpu.irq_line && !m->cpu.fiq_line,
              "reset lost external source24 or retained UART enable/cause");
        CHECK(s5l8920_set_irq(m,S5L8920_UART0_IRQ,false), "release reset-held source24");
    }
    size_t count=999u;
    CHECK(!s5l8920_uart0_clock(NULL,true,1u,NULL,0u,&count) && count==999u && !s5l8920_uart0_receive(NULL,0u),
          "invalid board event changed output or succeeded");
}

static void test_guest_irq_handler(s5l8920_t *m) {
    for (unsigned bank = 0; bank < S5L8920_VIC_COUNT; bank++) {
        CHECK(s5l8920_reset(m), "reset IRQ fixture");
        unsigned source = bank * 32u + 7u;
        uint32_t vector = 0x80000000u | source;
        vic_write(m,bank,PL192_VECTADDR0 + 7u * 4u,vector);
        vic_write(m,bank,PL192_INTENABLE,1u << 7);
        map_test_vectors(m);
        put(m,0x18u,0xea0003f8u); /* B from IRQ vector to VA1000. */
        uint32_t offset = 0x1000u;
        for (unsigned ack = 0; ack < (bank ? bank : 1u); ack++) {
            if (ack) { put(m,offset,0xe2811801u); offset += 4u; } /* ADD r1,#10000 */
            put(m,offset,0xe5910000u); offset += 4u;             /* LDR r0,[r1] */
        }
        for (unsigned eoi = 0; eoi <= bank; eoi++) {
            if (eoi) { put(m,offset,0xe2422801u); offset += 4u; } /* SUB r2,#10000 */
            put(m,offset,0xe5823000u); offset += 4u;             /* STR r3,[r2] */
        }
        put(m,offset,0xe25ef004u); /* SUBS pc,lr,#4 */
        m->cpu.r[1] = vic_address(0u,PL192_ADDRESS);
        m->cpu.r[2] = vic_address(bank,PL192_ADDRESS); m->cpu.r[3] = 0u;
        CHECK(s5l8920_set_irq(m,source,true) && m->cpu.irq_line && !m->cpu.fiq_line, "IRQ not propagated to CPU");
        CHECK(arm_step(&m->cpu) == ARM_OK && m->cpu.r[15] == 0x18u && m->cpu.r[14] == 0x204u &&
              (m->cpu.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_IRQ &&
              m->cpu.spsr[ARM_BANK_IRQ] == (ARM_MODE_SYS | ARM_CPSR_C), "CPU IRQ exception entry");
        CHECK(arm_step(&m->cpu) == ARM_OK && m->cpu.r[15] == 0x1000u, "translated IRQ vector branch");
        CHECK(arm_step(&m->cpu) == ARM_OK && m->cpu.r[0] == vector &&
              m->vic[0].in_service == 0x8000u && !m->cpu.irq_line, "guest root acknowledgement");
        CHECK(s5l8920_set_irq(m,source,false), "external source withdraws after acknowledgement");
        unsigned steps = 0u;
        while (m->cpu.r[15] != 0x200u && steps++ < 16u) {
            if (arm_step(&m->cpu) != ARM_OK) break;
        }
        CHECK(m->cpu.r[15] == 0x200u && m->cpu.cpsr == (ARM_MODE_SYS | ARM_CPSR_C) &&
              m->bus_failure.reason == S5L8920_BUS_OK && !m->cpu.irq_line && !m->cpu.fiq_line,
              "guest EOI/exception return did not resume interrupted state");
        for (unsigned n = 0; n < S5L8920_VIC_COUNT; n++)
            CHECK(m->vic[n].in_service == 0u, "EOI left an intermediate bank in service");
    }
}

static void test_fiq_and_reset(s5l8920_t *m) {
    CHECK(s5l8920_reset(m), "reset FIQ fixture");
    map_test_vectors(m); put(m,0x1cu,0xe25ef004u);
    vic_write(m,2u,PL192_INTSELECT,1u << 31);
    vic_write(m,2u,PL192_INTENABLE,1u << 31);
    CHECK(s5l8920_set_irq(m,95u,true) && m->cpu.fiq_line && !m->cpu.irq_line, "downstream FIQ wiring");
    CHECK(arm_step(&m->cpu) == ARM_OK && m->cpu.r[15] == 0x1cu && m->cpu.r[14] == 0x204u &&
          (m->cpu.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_FIQ, "CPU FIQ entry");
    CHECK(s5l8920_set_irq(m,95u,false) && arm_step(&m->cpu) == ARM_OK &&
          m->cpu.r[15] == 0x200u && m->cpu.cpsr == (ARM_MODE_SYS | ARM_CPSR_C), "FIQ return");
    CHECK(s5l8920_set_irq(m,95u,true), "held reset input");
    m->ram[0x300u] = 0x5au;
    CHECK(s5l8920_reset(m) && m->ram[0x300u] == 0x5au && m->vic[2].input == (1u << 31) &&
          m->vic[2].enable == 0u && !m->cpu.irq_line && !m->cpu.fiq_line && m->cpu.r[15] == 0u,
          "functional reset lost RAM/external levels or retained controller enables");
    CHECK(!s5l8920_set_irq(m,96u,true) && m->input_levels[2] == (1u << 31), "out-of-range interrupt source");
    CHECK(s5l8920_set_irq(m,95u,false), "clear reset input");
}

int main(void) {
    s5l8920_t m = {0};
    CHECK(s5l8920_init(&m), "initialization");
    if (!m.ram) return 1;
    test_geometry_and_refusals(&m);
    test_cpu_access_stops(&m);
    test_uart_checked_bus(&m);
    test_uart_interrupt_wiring(&m);
    test_guest_irq_handler(&m);
    test_fiq_and_reset(&m);
    s5l8920_free(&m);
    CHECK(!m.ram && !m.cpu.bus && !m.bus.ctx && !s5l8920_reset(&m), "free left live host wiring");
    size_t count=999u;
    CHECK(!s5l8920_uart0_clock(&m,true,1u,NULL,0u,&count) && count==999u && !s5l8920_uart0_receive(&m,0u),
          "freed board accepted a UART event");
    s5l8920_free(&m);
    printf("%u passed, %u failed\n",passed,failed);
    return failed ? 1 : 0;
}
