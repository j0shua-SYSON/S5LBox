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

static void test_ram_boot_window(s5l8920_t *m) {
    s5l8920_t empty = {0};
    CHECK(!s5l8920_set_ram_boot_window(NULL,true) && !s5l8920_set_ram_boot_window(&empty,true) &&
          !empty.ram_boot_window, "uninitialized board accepted RAM mapping");
    CHECK(s5l8920_reset(m) && !m->ram_boot_window, "reset invented inherited boot mapping");
    (void)m->bus.read32(m,0u);
    s5l8920_bus_failure_t stopped = m->bus_failure;
    arm_cp15_t cp15 = m->cpu.cp15;
    m->cpu.a8_par=0x1234507cu;
    m->cpu.excl_valid=true; m->cpu.a8_excl_size=8u;
    uint32_t generation=m->cpu.tlb_gen;
    CHECK(s5l8920_set_ram_boot_window(m,true) && m->ram_boot_window &&
          m->cpu.tlb_gen!=generation && !m->cpu.excl_valid && !m->cpu.a8_excl_size &&
          !memcmp(&stopped,&m->bus_failure,sizeof stopped) &&
          !memcmp(&cp15,&m->cpu.cp15,sizeof cp15) && m->cpu.a8_par==0x1234507cu,
          "mapping change lost diagnostics/state or retained caches/monitor");
    generation=m->cpu.tlb_gen; m->cpu.excl_valid=true;
    CHECK(s5l8920_set_ram_boot_window(m,true) && m->cpu.tlb_gen==generation && m->cpu.excl_valid,
          "same mapping unnecessarily invalidated execution state");
    s5l8920_clear_bus_failure(m);
    const uint32_t offsets[]={0u,1u,0x3ffu,0xffeu,0x1000u,0x123456u,
                              S5L8920_RAM_SIZE-4u,S5L8920_RAM_SIZE-2u,S5L8920_RAM_SIZE-1u};
    for (unsigned n=0;n<sizeof offsets/sizeof offsets[0];n++) for (unsigned size=1;size<=4;size*=2) {
        uint32_t low=offsets[n],high=S5L8920_RAM_BASE+low;
        if ((uint64_t)low+size>S5L8920_RAM_SIZE) continue;
        if (size==1u) m->bus.write8(m,low,0x5au);
        else if (size==2u) m->bus.write16(m,low,0x7654u);
        else m->bus.write32(m,low,0x98765432u);
        uint32_t value=size==1u ? m->bus.read8(m,high) : size==2u ? m->bus.read16(m,high) : m->bus.read32(m,high);
        CHECK(value==(size==1u ? 0x5au : size==2u ? 0x7654u : 0x98765432u), "low-to-high RAM coherence");
        if (size==1u) m->bus.write8(m,high,0xa5u);
        else if (size==2u) m->bus.write16(m,high,0xabcd);
        else m->bus.write32(m,high,0x12345678u);
        value=size==1u ? m->bus.read8(m,low) : size==2u ? m->bus.read16(m,low) : m->bus.read32(m,low);
        CHECK(value==(size==1u ? 0xa5u : size==2u ? 0xabcdu : 0x12345678u) &&
              m->bus.host_ram(m,low,size)==m->ram+low && m->bus.host_ram_write(m,high,size)==m->ram+low,
              "high-to-low coherence or host-pointer alias");
    }
    CHECK(!m->bus.access_failed(m) && !m->bus.host_ram(m,S5L8920_RAM_SIZE-1u,2u) &&
          !m->bus.host_ram_write(m,S5L8920_RAM_SIZE,1u) && !m->bus.host_ram(m,0u,UINT32_MAX) &&
          !m->bus.host_ram(m,UINT32_MAX,4u) && !m->bus.host_ram(m,0u,0u), "alias bounds exposed wrapped/non-RAM ranges");
    for (unsigned n=0;n<16;n++) m->ram[0x20u+n]=(uint8_t)n;
    CHECK(s5l8920_load(m,0x22u,m->ram+0x20u,8u), "overlapping host alias load");
    for (unsigned n=0;n<8;n++) CHECK(m->ram[0x22u+n]==n, "alias host load lost memmove semantics");
    CHECK(s5l8920_load(m,S5L8920_RAM_SIZE,NULL,0u) && !s5l8920_load(m,0u,m->ram,SIZE_MAX) &&
          !s5l8920_load(m,S5L8920_RAM_SIZE-1u,m->ram,2u), "host alias load bounds");
    const uint32_t gaps[]={S5L8920_RAM_SIZE,S5L8920_RAM_BASE-4u,S5L8920_RAM_BASE+S5L8920_RAM_SIZE,UINT32_MAX};
    for (unsigned n=0;n<sizeof gaps/sizeof gaps[0];n++) {
        s5l8920_clear_bus_failure(m); m->bus.write8(m,gaps[n],0xa5u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_UNMAPPED && m->bus_failure.address==gaps[n], "alias escaped installed RAM");
    }
    s5l8920_clear_bus_failure(m); (void)m->bus.read32(m,S5L8920_PMGR_BASE);
    CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && m->ram_boot_window,
          "handoff selection fabricated the unmodeled remap register");
    s5l8920_clear_bus_failure(m); m->bus.write32(m,S5L8920_PMGR_BASE,2u);
    CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED, "unmodeled remap write silently succeeded");
    CHECK(s5l8920_reset(m) && !m->ram_boot_window && !m->bus.host_ram(m,0u,4u) &&
          !s5l8920_load(m,0u,m->ram+0x20u,1u), "functional reset retained explicit low mapping");
}

static void test_boot_window_cached_access(s5l8920_t *m) {
    arm_bus_t original=m->bus;
    for (unsigned host=0;host<2;host++) for (unsigned mmu=0;mmu<2;mmu++) for (unsigned store=0;store<2;store++) {
        CHECK(s5l8920_reset(m) && s5l8920_set_ram_boot_window(m,true), "prepare alias CPU access");
        m->bus.host_ram=host ? original.host_ram : NULL;
        m->bus.host_ram_write=host ? original.host_ram_write : NULL;
        put(m,0x300u,store ? 0xe5812000u : 0xe5912000u); /* STR/LDR r2,[r1] */
        put(m,0x1000u,0x12345678u);
        uint32_t code=S5L8920_RAM_BASE+0x300u,target=0x1000u;
        if (mmu) {
            put(m,0x4000u+0x800u*4u,S5L8920_RAM_BASE|0xc0eu);
            put(m,0x4000u+0x900u*4u,0xc0eu);
            m->cpu.cp15.ttbr0=S5L8920_RAM_BASE+0x4000u; m->cpu.cp15.dacr=1u;
            m->cpu.cp15.sctlr|=ARM_SCTLR_M|ARM_SCTLR_XP;
            code=0x80000300u; target=0x90001000u;
        }
        m->cpu.r[15]=code; m->cpu.r[1]=target; m->cpu.r[2]=0x12345678u;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[2]==0x12345678u, "warm low RAM data cache");
        CHECK(s5l8920_set_ram_boot_window(m,false), "remove low RAM mapping");
        m->cpu.r[15]=code; m->cpu.r[2]=0xabcdef01u;
        uint64_t cycles=m->cpu.cycles;
        CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.r[15]==code && m->cpu.r[2]==0xabcdef01u &&
              m->cpu.cycles==cycles && m->bus_failure.address==0x1000u &&
              m->bus_failure.reason==S5L8920_BUS_UNMAPPED && m->ram[0x1000u]==0x78u,
              "removed alias leaked through a cached read/write pointer");
        CHECK(s5l8920_set_ram_boot_window(m,true), "repair low RAM mapping");
        s5l8920_clear_bus_failure(m);
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==cycles+1u &&
              (store ? m->ram[0x1000u]==1u : m->cpu.r[2]==0x12345678u), "repaired mapping did not retry");
    }
    m->bus=original;
    CHECK(s5l8920_reset(m) && s5l8920_set_ram_boot_window(m,true), "prepare low instruction fetch");
    put(m,0x100u,0xe3a00011u); m->cpu.r[15]=0x100u;
    CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[0]==0x11u, "low alias instruction fetch");
    CHECK(s5l8920_set_ram_boot_window(m,false), "remove warm fetch alias");
    m->cpu.r[15]=0x100u; uint64_t cycles=m->cpu.cycles;
    CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.r[0]==0x11u && m->cpu.r[15]==0x100u &&
          m->cpu.cycles==cycles && m->bus_failure.address==0x100u, "stale alias instruction fetched");
    CHECK(s5l8920_set_ram_boot_window(m,true), "repair fetch alias");
    s5l8920_clear_bus_failure(m); put(m,0x100u,0xe3a00022u);
    CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[0]==0x22u, "repaired alias did not fetch fresh code");
    CHECK(s5l8920_reset(m) && s5l8920_set_ram_boot_window(m,true), "prepare alias-end partial store");
    put(m,0u,0xe4812004u); m->cpu.r[15]=S5L8920_RAM_BASE;
    m->cpu.r[1]=S5L8920_RAM_SIZE-2u; m->cpu.r[2]=0xabcdef12u;
    CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.r[1]==S5L8920_RAM_SIZE-2u && !m->cpu.cycles &&
          m->ram[S5L8920_RAM_SIZE-2u]==0x12u && m->ram[S5L8920_RAM_SIZE-1u]==0xefu &&
          m->bus_failure.address==S5L8920_RAM_SIZE && m->bus_failure.size==1u && m->bus_failure.value==0xcdu,
          "cross-alias-end store lost prefix, wrapped, or retired writeback");
    CHECK(s5l8920_reset(m), "reset after alias tests");
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

static void test_timebase(s5l8920_t *m) {
    const uint32_t low=S5L8920_PMGR_BASE+S5L8920_TIMEBASE_LOW;
    const uint32_t high=S5L8920_PMGR_BASE+S5L8920_TIMEBASE_HIGH;
    CHECK(s5l8920_reset(m) && !m->timebase_ticks, "functional timebase reset");
    CHECK(!s5l8920_timebase_clock(NULL,1u) && s5l8920_timebase_clock(m,UINT32_MAX), "explicit clock input");
    CHECK(m->bus.read32(m,high)==0u && m->bus.read32(m,low)==UINT32_MAX &&
          m->timebase_ticks==UINT32_MAX, "timebase read advanced time or latched halves");
    CHECK(s5l8920_timebase_clock(m,1u) && m->bus.read32(m,high)==1u && m->bus.read32(m,low)==0u,
          "low-word rollover did not carry");
    CHECK(s5l8920_timebase_clock(m,UINT64_MAX) && m->bus.read32(m,high)==0u && m->bus.read32(m,low)==UINT32_MAX,
          "large clock input overflowed intermediate arithmetic");
    CHECK(s5l8920_timebase_clock(m,UINT64_MAX-UINT32_MAX) && m->timebase_ticks==UINT64_MAX &&
          s5l8920_timebase_clock(m,1u) && m->timebase_ticks==0u, "full-width rollover");
    CHECK(s5l8920_timebase_clock(m,UINT64_C(0x1234567887654321)), "seed counter");
    put(m,0u,0xe5912000u); /* LDR r2,[r1] */
    m->cpu.r[15]=S5L8920_RAM_BASE;m->cpu.r[1]=low;
    CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[2]==0x87654321u &&
          m->timebase_ticks==UINT64_C(0x1234567887654321), "CPU read invented clock ticks");
    CHECK(!m->bus.host_ram(m,low,4u) && !m->bus.host_ram_write(m,low,4u) && !s5l8920_load(m,low,&low,4u),
          "timebase bypassed checked MMIO");
    const uint32_t offsets[]={0u,0x1fcu,0x200u,0x204u,0x20cu,0x220u,0x1ffcu};
    for (unsigned n=0;n<sizeof offsets/sizeof offsets[0];n++) {
        uint32_t a=S5L8920_PMGR_BASE+offsets[n];
        for (unsigned kind=0;kind<4;kind++) {
            s5l8920_clear_bus_failure(m);m->cpu.r[15]=0x1234u;
            if (!kind) m->bus.write32(m,a,0xabcdef01u);
            else if (kind==1u) (void)m->bus.read8(m,a);
            else if (kind==2u) m->bus.write16(m,a,0xcdefu);
            else (void)m->bus.read32(m,a+1u);
            CHECK(m->bus_failure.reason==(kind?S5L8920_BUS_ACCESS_UNIMPLEMENTED:S5L8920_BUS_REGISTER_REFUSED) &&
                  m->timebase_ticks==UINT64_C(0x1234567887654321) && m->bus_failure.pc==0x1234u,
                  "refused timer access changed state or lacked diagnostics");
        }
        if (offsets[n]!=0x200u && offsets[n]!=0x204u) {
            s5l8920_clear_bus_failure(m);m->cpu.r[15]=S5L8920_RAM_BASE;m->cpu.r[1]=a;m->cpu.r[2]=0xfeedbeefu;
            uint64_t cycles=m->cpu.cycles;
            CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.r[15]==S5L8920_RAM_BASE &&
                  m->cpu.r[2]==0xfeedbeefu && m->cpu.cycles==cycles &&
                  m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && m->bus_failure.address==a,
                  "unsupported PMGR/deadline read retired or returned invented data");
        }
    }
    s5l8920_bus_failure_t diagnostic=m->bus_failure;
    CHECK(s5l8920_timebase_clock(m,1u) && !memcmp(&diagnostic,&m->bus_failure,sizeof diagnostic),
          "host tick cleared a checked stop");
    uint64_t ticks=m->timebase_ticks;
    CHECK(m->bus.read32(m,low)==0u && m->timebase_ticks==ticks, "latched stop allowed counter read");
    s5l8920_clear_bus_failure(m);
    CHECK(m->bus.read32(m,low)==0x87654322u && !m->cpu.irq_line && !m->cpu.fiq_line,
          "explicit ticks were lost or generated an unimplemented timer interrupt");
    CHECK(s5l8920_reset(m) && !m->timebase_ticks && !m->bus_failure.reason, "timebase reset retained state");
}

static void test_deadline_countdown(s5l8920_t *m) {
    const uint32_t count=S5L8920_PMGR_BASE+S5L8920_DEADLINE_COUNT;
    const uint32_t control=S5L8920_PMGR_BASE+S5L8920_DEADLINE_CONTROL;
    const uint32_t line=1u<<S5L8920_DEADLINE_IRQ;
    CHECK(s5l8920_reset(m), "deadline reset");
    m->bus.write32(m,control,1u);
    CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && !m->deadline.enabled &&
          !m->deadline.programmed, "enable invented an unprogrammed reset count");
    s5l8920_clear_bus_failure(m); m->bus.write32(m,control,2u);
    CHECK(!m->bus_failure.reason && !m->deadline.pending, "unprogrammed disable/ack refused");
    put(m,0u,0xe4912004u); /* LDR r2,[r1],#4 */
    m->cpu.r[15]=S5L8920_RAM_BASE; m->cpu.r[1]=count; m->cpu.r[2]=0xdeadbeefu;
    CHECK(arm_step(&m->cpu)==ARM_HALT && !m->cpu.cycles && m->cpu.r[1]==count &&
          m->cpu.r[2]==0xdeadbeefu && m->bus_failure.address==count && !m->deadline.programmed,
          "unknown reset count retired a read/writeback");
    m->bus.write32(m,count,7u);
    CHECK(!m->deadline.programmed, "latched checked failure allowed deadline programming");
    s5l8920_clear_bus_failure(m); m->bus.write32(m,count,7u);
    CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==1u && m->cpu.r[2]==7u &&
          m->cpu.r[1]==count+4u && !m->deadline.enabled, "programmed count did not repair CPU read");
    CHECK(!m->bus.host_ram(m,count,4u) && !m->bus.host_ram_write(m,count,4u) &&
          !s5l8920_load(m,count,&count,4u), "deadline bypassed checked MMIO");

    const uint32_t intervals[]={0u,1u,2u,0x7fffffffu,UINT32_MAX};
    for (unsigned n=0;n<sizeof intervals/sizeof intervals[0];n++) {
        CHECK(s5l8920_reset(m) && s5l8920_timebase_clock(m,UINT64_MAX-3u), "prepare counter wrap");
        m->bus.write32(m,count,intervals[n]);
        CHECK(s5l8920_timebase_clock(m,UINT64_MAX) && m->bus.read32(m,count)==intervals[n] &&
              !m->deadline.pending && !m->deadline.expired, "disabled deadline advanced or overflowed");
        m->bus.write32(m,control,3u);
        uint64_t before=m->timebase_ticks;
        CHECK(s5l8920_timebase_clock(m,0u) && m->timebase_ticks==before &&
              m->bus.read32(m,count)==intervals[n] && !m->deadline.pending,
              "zero host ticks or a register read expired the deadline");
        put(m,0u,0xe5912000u); m->cpu.r[15]=S5L8920_RAM_BASE; m->cpu.r[1]=count;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[2]==intervals[n] &&
              m->timebase_ticks==before && !m->deadline.pending, "CPU read invented timer ticks");
        uint64_t early=intervals[n] ? intervals[n]-1u:0u;
        CHECK(s5l8920_timebase_clock(m,early) && !m->deadline.pending &&
              m->bus.read32(m,count)==(intervals[n] ? 1u:0u), "deadline expired early or lost countdown readback");
        CHECK(s5l8920_timebase_clock(m,1u) && m->deadline.expired && m->deadline.pending &&
              (m->vic[0].input&line) && !m->cpu.irq_line && !m->cpu.fiq_line &&
              m->timebase_ticks==before+early+1u, "logical expiry failed across timebase wrap");
        vic_write(m,0u,PL192_INTENABLE,line);
        CHECK(m->cpu.irq_line, "VIC enable lost a pending deadline");
        m->cpu.r[15]=S5L8920_RAM_BASE; m->cpu.r[2]=0xfeedbeefu;
        uint64_t cycles=m->cpu.cycles;
        CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.cycles==cycles && m->cpu.r[2]==0xfeedbeefu &&
              m->cpu.r[15]==S5L8920_RAM_BASE && m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
              m->bus_failure.address==count, "unestablished post-expiry value was consumed");
        s5l8920_bus_failure_t diagnostic=m->bus_failure;
        CHECK(s5l8920_timebase_clock(m,UINT64_MAX) && m->deadline.pending && m->cpu.irq_line &&
              !memcmp(&diagnostic,&m->bus_failure,sizeof diagnostic), "host event changed latched diagnostics/cause");
        s5l8920_clear_bus_failure(m); m->bus.write32(m,control,0u);
        CHECK(!m->deadline.enabled && m->deadline.pending && m->cpu.irq_line, "counter stop acknowledged an event");
        m->bus.write32(m,control,1u);
        CHECK(m->deadline.pending && m->cpu.irq_line, "enable acknowledged an event");
        m->bus.write32(m,control,3u); m->bus.write32(m,control,1u);
        CHECK(!m->deadline.pending && !m->cpu.irq_line && s5l8920_timebase_clock(m,UINT64_MAX) &&
              !m->deadline.pending, "acknowledgement rearmed an expired one-shot");
        m->bus.write32(m,count,5u);
        CHECK(s5l8920_timebase_clock(m,2u) && m->bus.read32(m,count)==3u && !m->deadline.pending,
              "enabled count write did not arm a fresh interval");
        m->bus.write32(m,count,2u);
        CHECK(s5l8920_timebase_clock(m,1u) && m->bus.read32(m,count)==1u &&
              s5l8920_timebase_clock(m,1u) && m->deadline.pending, "live interval replacement");
        m->bus.write32(m,count,9u);
        CHECK(m->deadline.pending && m->cpu.irq_line && m->bus.read32(m,count)==9u,
              "count write implicitly acknowledged the previous cause");
        m->bus.write32(m,control,2u);
        CHECK(!m->deadline.pending && !m->deadline.enabled && !m->cpu.irq_line &&
              s5l8920_timebase_clock(m,100u) && m->bus.read32(m,count)==9u, "disable/ack did not freeze remaining count");
        m->bus.write32(m,control,1u);
        CHECK(s5l8920_timebase_clock(m,UINT64_MAX) && m->deadline.expired && m->cpu.irq_line,
              "large supplied interval truncated before expiry");
    }
    CHECK(s5l8920_reset(m), "reset deadline refusal fixture");
    m->bus.write32(m,count,9u); m->bus.write32(m,control,3u);
    s5l8920_deadline_t before=m->deadline;
    for (unsigned bit=2u;bit<32u;bit++) {
        s5l8920_clear_bus_failure(m); m->bus.write32(m,control,(1u<<bit)|3u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
              !memcmp(&before,&m->deadline,sizeof before), "reserved control bits changed timer state");
    }
    for (unsigned reg=0;reg<2u;reg++) for (unsigned kind=0;kind<4u;kind++) {
        uint32_t address=reg ? control:count;
        s5l8920_clear_bus_failure(m);
        if (!kind) (void)m->bus.read8(m,address);
        else if (kind==1u) m->bus.write16(m,address,3u);
        else if (kind==2u) (void)m->bus.read32(m,address+1u);
        else m->bus.write32(m,address+1u,3u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_ACCESS_UNIMPLEMENTED &&
              !memcmp(&before,&m->deadline,sizeof before), "unsupported timer access width/alignment committed");
    }
    s5l8920_clear_bus_failure(m); (void)m->bus.read32(m,control);
    CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
          !memcmp(&before,&m->deadline,sizeof before), "control read invented register semantics");
    CHECK(s5l8920_reset(m) && !m->deadline.programmed && !m->deadline.enabled && !m->deadline.expired &&
          !m->deadline.pending && !m->timebase_ticks, "reset retained a timer configuration/cause");
}

static void test_deadline_interrupt_wiring(s5l8920_t *m) {
    const uint32_t count=S5L8920_PMGR_BASE+S5L8920_DEADLINE_COUNT;
    const uint32_t control=S5L8920_PMGR_BASE+S5L8920_DEADLINE_CONTROL;
    const uint32_t line=1u<<S5L8920_DEADLINE_IRQ;
    for (unsigned fiq=0;fiq<2u;fiq++) {
        CHECK(s5l8920_reset(m), "reset deadline interrupt fixture");
        vic_write(m,0u,PL192_VECTADDR0+4u*S5L8920_DEADLINE_IRQ,0x80000006u);
        vic_write(m,0u,PL192_INTSELECT,fiq ? line:0u); vic_write(m,0u,PL192_INTENABLE,line);
        m->bus.write32(m,count,2u); m->bus.write32(m,control,3u);
        CHECK(s5l8920_timebase_clock(m,1u) && !m->cpu.irq_line && !m->cpu.fiq_line,
              "deadline routed before expiry");
        CHECK(s5l8920_set_irq(m,S5L8920_DEADLINE_IRQ,true), "assert external source6");
        m->bus.write32(m,control,2u);
        CHECK((m->vic[0].input&line) && !m->deadline.pending &&
              m->cpu.irq_line==(fiq==0u) && m->cpu.fiq_line==(fiq!=0u), "timer ack erased external source6");
        CHECK(s5l8920_set_irq(m,S5L8920_DEADLINE_IRQ,false) && !m->cpu.irq_line && !m->cpu.fiq_line,
              "external withdrawal retained acknowledged source6");
        m->bus.write32(m,control,1u);
        CHECK(s5l8920_timebase_clock(m,1u) && s5l8920_set_irq(m,S5L8920_DEADLINE_IRQ,false) &&
              m->deadline.pending && m->cpu.irq_line==(fiq==0u) && m->cpu.fiq_line==(fiq!=0u),
              "countdown expiry or external update lost internal source6");
        /* Synthetic vectors test actual CPU/fabric delivery, not N88 scheduler setup. */
        map_test_vectors(m);
        put(m,0x4000u+(S5L8920_PMGR_BASE>>20)*4u,S5L8920_PMGR_BASE|0xc02u);
        uint32_t vector=fiq ? 0x1cu:0x18u;
        put(m,vector,0xea000000u|((0x1000u-vector-8u)>>2));
        uint32_t at=0x1000u;
        if (!fiq) { put(m,at,0xe5932000u); at+=4u; } /* VIC ACK */
        put(m,at,0xe5810000u); at+=4u;               /* timer control3 */
        put(m,at,0xe2400002u); at+=4u;               /* SUB r0,#2 */
        put(m,at,0xe5810000u); at+=4u;               /* timer control1 */
        put(m,at,0xe5856000u); at+=4u;               /* new count */
        if (!fiq) { put(m,at,0xe5834000u); at+=4u; } /* VIC EOI */
        put(m,at,0xe25ef004u);                        /* SUBS pc,lr,#4 */
        m->cpu.r[0]=3u; m->cpu.r[1]=control; m->cpu.r[2]=0x12345678u;
        m->cpu.r[3]=vic_address(0u,PL192_ADDRESS); m->cpu.r[4]=0u; m->cpu.r[5]=count; m->cpu.r[6]=5u;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[15]==vector && m->cpu.r[14]==0x204u &&
              (m->cpu.cpsr&ARM_CPSR_MODE_MASK)==(fiq ? ARM_MODE_FIQ:ARM_MODE_IRQ), "timer-driven CPU exception entry");
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[15]==0x1000u, "timer vector branch");
        unsigned steps=0u;
        while (m->cpu.r[15]!=0x200u && steps++<9u) if (arm_step(&m->cpu)!=ARM_OK) break;
        CHECK(m->cpu.r[15]==0x200u && m->cpu.cpsr==(ARM_MODE_SYS|ARM_CPSR_C) &&
              !m->bus_failure.reason && !m->deadline.pending && m->deadline.enabled &&
              m->bus.read32(m,count)==5u && !m->cpu.irq_line && !m->cpu.fiq_line && !m->vic[0].in_service,
              "timer ack/reprogram/EOI/return failed or guest instructions consumed time");
        if (!fiq) CHECK(m->cpu.r[2]==0x80000006u, "timer source6 acknowledgement vector");
        CHECK(s5l8920_timebase_clock(m,4u) && !m->deadline.pending &&
              s5l8920_timebase_clock(m,1u) && m->deadline.pending, "guest-rearmed timer did not fire once");
        CHECK(s5l8920_set_irq(m,S5L8920_DEADLINE_IRQ,true) && s5l8920_reset(m) &&
              m->vic[0].input==line && m->input_levels[0]==line && !m->deadline.programmed &&
              !m->deadline.pending && !m->cpu.irq_line && !m->cpu.fiq_line, "reset lost external source6 or retained timer cause");
        CHECK(s5l8920_set_irq(m,S5L8920_DEADLINE_IRQ,false), "release external source6");
    }
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
    test_ram_boot_window(&m);
    test_boot_window_cached_access(&m);
    test_cpu_access_stops(&m);
    test_uart_checked_bus(&m);
    test_uart_interrupt_wiring(&m);
    test_timebase(&m);
    test_deadline_countdown(&m);
    test_deadline_interrupt_wiring(&m);
    test_guest_irq_handler(&m);
    test_fiq_and_reset(&m);
    s5l8920_free(&m);
    CHECK(!m.ram && !m.cpu.bus && !m.bus.ctx && !s5l8920_reset(&m), "free left live host wiring");
    CHECK(!s5l8920_timebase_clock(&m,1u) && !m.timebase_ticks, "freed board accepted timebase input");
    size_t count=999u;
    CHECK(!s5l8920_uart0_clock(&m,true,1u,NULL,0u,&count) && count==999u && !s5l8920_uart0_receive(&m,0u),
          "freed board accepted a UART event");
    s5l8920_free(&m);
    printf("%u passed, %u failed\n",passed,failed);
    return failed ? 1 : 0;
}
