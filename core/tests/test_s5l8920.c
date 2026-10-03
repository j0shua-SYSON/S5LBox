/* Partial N88 fabric: physical mapping, checked stops and CPU interrupt wiring.
 * Synthetic instructions establish component behavior, not a firmware boot.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920.h"
#include <stdio.h>
#include <string.h>

static unsigned passed, failed;
#define CHECK(c, msg) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: %s\n",__func__,__LINE__,msg); } } while (0)

static void test_empty_nvram_proxy(void) {
    /* Independent 8 KiB image accepted by the matching kernel's real proxy
     * copy/parser: common offset 0x30, length 0x7f0, Adler-32 0x34b80b21.
     * All unspecified bytes are zero, including the complete variable area. */
    static const uint8_t expected[8192]={
        [0x00]=0x5a,[0x01]=0x82,[0x02]=0x02,
        [0x04]='n',[0x05]='v',[0x06]='r',[0x07]='a',[0x08]='m',
        [0x10]=0x21,[0x11]=0x0b,[0x12]=0xb8,[0x13]=0x34,[0x14]=1,
        [0x20]=0x70,[0x21]=0x7c,[0x22]=0x80,
        [0x24]='c',[0x25]='o',[0x26]='m',[0x27]='m',[0x28]='o',[0x29]='n',
        [0x820]=0x7f,[0x821]=0x98,[0x822]=0x7e,[0x823]=0x01,
        [0x824]='w',[0x825]='w',[0x826]='w',[0x827]='w',[0x828]='w',[0x829]='w',
        [0x82a]='w',[0x82b]='w',[0x82c]='w',[0x82d]='w',[0x82e]='w',[0x82f]='w'
    };
    uint8_t buffer[8192+16], before[sizeof buffer];
    CHECK(S5L8920_NVRAM_PROXY_SIZE==sizeof expected,"N88 proxy geometry");
    CHECK(!s5l8920_build_empty_nvram_proxy(NULL,0u) &&
          !s5l8920_build_empty_nvram_proxy(NULL,sizeof expected),"NULL proxy accepted");
    const size_t invalid[]={0u,1u,15u,8191u,8193u,SIZE_MAX};
    memset(buffer,0xa5,sizeof buffer); memcpy(before,buffer,sizeof buffer);
    for (unsigned n=0;n<sizeof invalid/sizeof invalid[0];n++)
        CHECK(!s5l8920_build_empty_nvram_proxy(buffer+4,invalid[n]) &&
              !memcmp(buffer,before,sizeof buffer),"rejected size changed proxy buffer");
    for (unsigned alignment=0;alignment<4u;alignment++) {
        size_t start=4u+alignment;
        memset(buffer,0xa5,sizeof buffer); memcpy(before,buffer,sizeof buffer);
        CHECK(s5l8920_build_empty_nvram_proxy(buffer+start,sizeof expected) &&
              !memcmp(buffer+start,expected,sizeof expected),"whole N88 proxy image differs");
        CHECK(!memcmp(buffer,before,start) &&
              !memcmp(buffer+start+sizeof expected,before+start+sizeof expected,sizeof buffer-start-sizeof expected),
              "proxy construction crossed caller buffer");
        memset(buffer+start+0x30,0x5a,0x7f0);
        CHECK(s5l8920_build_empty_nvram_proxy(buffer+start,sizeof expected) &&
              !memcmp(buffer+start,expected,sizeof expected),"explicit reinitialization retained variables");
    }
}

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

static void test_gpio_output_registers(s5l8920_t *m) {
    /* Matching N88 DT has 46 ports of eight pins at 0x83000000. The iBoot
     * output helper writes 0x212/0x213, retaining the pull selection. These
     * tests program digital outputs; they do not assume inherited pin state. */
    CHECK(s5l8920_reset(m), "reset GPIO output fixture");
    (void)m->bus.read32(m,0x83000000u);
    CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,
          "unprepared GPIO read did not stop");
    const uint32_t pulls[]={0u,0x80u,0x100u};
    for (unsigned pin=0;pin<368u;pin++) {
        uint32_t address=0x83000000u+4u*pin;
        for (unsigned n=0;n<3u;n++) {
            uint32_t value=0x212u|pulls[n]|((pin+n)&1u);
            s5l8920_clear_bus_failure(m);
            m->bus.write32(m,address,value);
            CHECK(!m->bus_failure.reason && m->bus.read32(m,address)==value,
                  "programmed GPIO output/pull readback");
        }
    }
    s5l8920_clear_bus_failure(m);
    CHECK(!m->bus.host_ram(m,0x83000000u,4u) &&
          !m->bus.host_ram_write(m,0x83000000u,4u) &&
          !s5l8920_load(m,0x83000000u,m->ram,4u),"GPIO exposed as plain RAM");
    const uint32_t rejected[]={0u,1u,0x202u,0x014u,0x016u,0x018u,0x01au,
        0x01cu,0x20eu,0x232u,0x252u,0x272u,0x392u,0x1612u,0x1212u,
        0x10000212u,UINT32_MAX};
    for (unsigned n=0;n<sizeof rejected/sizeof rejected[0];n++) {
        s5l8920_clear_bus_failure(m); m->bus.write32(m,0x83000000u,0x213u);
        m->cpu.r[15]=0x1234u; m->bus.write32(m,0x83000000u,rejected[n]);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
              m->bus_failure.address==0x83000000u && m->bus_failure.value==rejected[n] &&
              m->bus_failure.pc==0x1234u && m->bus_failure.write,
              "unsupported GPIO configuration accepted or wrong diagnostic");
        m->bus.write32(m,0x83000000u,0x212u);
        s5l8920_clear_bus_failure(m);
        CHECK(m->bus.read32(m,0x83000000u)==0x213u,"rejected/latched write changed GPIO output");
    }
    const uint32_t selectors[]={0x5c0u,0x81cu,0x820u,0x900u,0xffcu};
    for (unsigned n=0;n<sizeof selectors/sizeof selectors[0];n++) {
        s5l8920_clear_bus_failure(m); (void)m->bus.read32(m,0x83000000u+selectors[n]);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"unsupported GPIO selector read supplied a value");
        s5l8920_clear_bus_failure(m); m->bus.write32(m,0x83000000u+selectors[n],0u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"unsupported GPIO selector write accepted");
    }
    for (unsigned kind=0;kind<6u;kind++) {
        s5l8920_clear_bus_failure(m);
        if (kind==0u) (void)m->bus.read8(m,0x83000000u);
        else if (kind==1u) (void)m->bus.read16(m,0x83000000u);
        else if (kind==2u) (void)m->bus.read32(m,0x83000001u);
        else if (kind==3u) m->bus.write8(m,0x83000000u,0u);
        else if (kind==4u) m->bus.write16(m,0x83000000u,0u);
        else m->bus.write32(m,0x83000002u,0u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_ACCESS_UNIMPLEMENTED,
              "unsupported GPIO width/alignment reached registers");
    }
    CHECK(s5l8920_reset(m),"reset programmed GPIO outputs");
    (void)m->bus.read32(m,0x83000000u);
    CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"reset retained programmed GPIO state");
    s5l8920_clear_bus_failure(m);
    (void)m->bus.read32(m,0x83001000u);
    CHECK(m->bus_failure.reason==S5L8920_BUS_UNMAPPED,"GPIO aperture expanded past DT range");
    CHECK(s5l8920_reset(m),"reset after GPIO fixture");
}

static void test_gpio_input_samples(s5l8920_t *m) {
    s5l8920_t empty={0};
    CHECK(S5L8920_GPIO_PIN_COUNT==368u && S5L8920_GPIO_BASE==0x83000000u,
          "GPIO geometry differs from matching DT");
    CHECK(!s5l8920_gpio_input(NULL,0u,true) && !s5l8920_gpio_input(&empty,0u,true) &&
          !empty.gpio[0].input_valid,"uninitialized GPIO host input accepted");
    CHECK(s5l8920_reset(m),"reset GPIO sample fixture");
    const uint32_t pulls[]={0u,0x80u,0x100u};
    for (unsigned pin=0;pin<368u;pin++) {
        uint32_t address=0x83000000u+4u*pin;
        m->bus.write32(m,address,0x211u);
        (void)m->bus.read32(m,address);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,
              "input data write fabricated an unsupplied sample");
        s5l8920_bus_failure_t stopped=m->bus_failure;
        CHECK(s5l8920_gpio_input(m,pin,false) &&
              !memcmp(&stopped,&m->bus_failure,sizeof stopped),"host sample lost checked stop");
        s5l8920_clear_bus_failure(m);
        for (unsigned pull=0;pull<3u;pull++) for (unsigned high=0;high<2u;high++) {
            CHECK(s5l8920_gpio_input(m,pin,high!=0u),"explicit GPIO sample refused");
            m->bus.write32(m,address,0x210u|pulls[pull]|(high^1u));
            CHECK(m->bus.read32(m,address)==(0x210u|pulls[pull]|high) && !m->bus_failure.reason,
                  "input read used written data/pull instead of supplied sample");
            m->bus.write32(m,address,0x212u|pulls[pull]|(high^1u));
            CHECK(m->bus.read32(m,address)==(0x212u|pulls[pull]|(high^1u)),
                  "external sample overrode driven output");
        }
    }
    s5l8920_gpio_pin_t before[368]; memcpy(before,m->gpio,sizeof before);
    CHECK(!s5l8920_gpio_input(m,368u,true) && !s5l8920_gpio_input(m,UINT32_MAX,true) &&
          !memcmp(before,m->gpio,sizeof before),"invalid pin changed GPIO state");
    CHECK(!m->vic[2].input && !m->cpu.irq_line && !m->cpu.fiq_line,
          "masked GPIO sample invented interrupt");
    CHECK(s5l8920_reset(m),"reset sampled inputs");
    for (unsigned pin=0;pin<368u;pin++) {
        uint32_t address=0x83000000u+4u*pin;
        (void)m->bus.read32(m,address);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,
              "sample alone supplied reset control state");
        s5l8920_clear_bus_failure(m); m->bus.write32(m,address,0x210u);
        CHECK(m->bus.read32(m,address)==0x211u,"functional reset lost external sample");
    }
    CHECK(s5l8920_reset(m),"reset after GPIO input fixture");
}

static void test_gpio_checked_cpu(s5l8920_t *m) {
    for (unsigned mapped=0;mapped<2u;mapped++) {
        CHECK(s5l8920_reset(m),"reset GPIO CPU fixture");
        uint32_t code=S5L8920_RAM_BASE+0x200u, pin=0x83000000u;
        if (mapped) {
            map_test_vectors(m); code=0x200u; pin=0xc5900000u;
            put(m,0x4000u+(pin>>20)*4u,0x83000c02u); /* Device section. */
        }
        put(m,0x200u,0xe4912004u); /* LDR r2,[r1],#4 */
        put(m,0x204u,0xe4812004u); /* STR r2,[r1],#4 */
        m->cpu.r[15]=code; m->cpu.r[1]=pin; m->cpu.r[2]=0xabcdef01u;
        CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.r[15]==code && !m->cpu.cycles &&
              m->cpu.r[1]==pin && m->cpu.r[2]==0xabcdef01u && !m->cpu.cp15.dfsr &&
              m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
              m->bus_failure.address==0x83000000u && !m->bus_failure.write,
              "unprepared GPIO load retired, wrote back or became guest abort");
        CHECK(s5l8920_gpio_input(m,0u,false),"supply CPU input sample");
        s5l8920_clear_bus_failure(m); m->bus.write32(m,0x83000000u,0x211u);
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[15]==code+4u && m->cpu.cycles==1u &&
              m->cpu.r[2]==0x210u && m->cpu.r[1]==pin+4u,"GPIO checked load retry");
        m->cpu.r[2]=0x202u; /* Interrupt-enabled form remains unsupported. */
        CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.r[15]==code+4u && m->cpu.cycles==1u &&
              m->cpu.r[1]==pin+4u && !m->gpio[1].programmed && !m->cpu.cp15.dfsr &&
              m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && m->bus_failure.write &&
              m->bus_failure.address==0x83000004u && m->bus_failure.value==0x202u,
              "unsupported GPIO store committed state/writeback");
        s5l8920_clear_bus_failure(m); m->cpu.r[2]=0x313u;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[15]==code+8u && m->cpu.cycles==2u &&
              m->cpu.r[1]==pin+8u && m->bus.read32(m,0x83000004u)==0x313u,
              "GPIO checked store retry");
    }
    CHECK(s5l8920_reset(m),"reset after GPIO CPU fixture");
}

static void test_gpio_interrupt_modes(s5l8920_t *m) {
    /* N88 has seven status words and source94. Modes encode high/low levels,
     * rising/falling/either edges. Explicit samples supply the transitions. */
    const uint32_t modes[]={0x204u,0x206u,0x208u,0x20au,0x20cu};
    for (unsigned pin=0;pin<224u;pin++) for (unsigned kind=0;kind<5u;kind++) {
        uint32_t address=0x83000000u+4u*pin, status=0x83000800u+4u*(pin/32u);
        uint32_t bit=1u<<(pin%32u), control=modes[kind];
        bool active=kind!=1u && kind!=3u;
        CHECK(s5l8920_reset(m) && s5l8920_gpio_input(m,pin,!active),"reset/sample GPIO interrupt fixture");
        vic_write(m,2u,PL192_INTENABLE,1u<<30);
        m->bus.write32(m,address,control|0x10u);
        CHECK(!m->bus_failure.reason && m->bus.read32(m,status)==0u && !m->cpu.irq_line,
              "IRQ configuration created a false event or was refused");
        s5l8920_clear_bus_failure(m);
        CHECK(s5l8920_gpio_input(m,pin,active) && m->bus.read32(m,status)==bit && !m->cpu.irq_line,
              "masked sample did not latch cause or incorrectly asserted parent");
        s5l8920_clear_bus_failure(m);
        m->bus.write32(m,address,control);
        CHECK(!m->bus_failure.reason && m->cpu.irq_line && m->bus.read32(m,status)==bit &&
              m->bus.read32(m,address)==(control|(active ? 1u:0u)),"unmask/input readback lost pending cause");
        s5l8920_clear_bus_failure(m);
        m->bus.write32(m,status,bit);
        CHECK(!m->bus_failure.reason && m->bus.read32(m,status)==(kind<2u ? bit:0u) &&
              m->cpu.irq_line==(kind<2u),"W1C edge clear/active-level reassertion");
        s5l8920_clear_bus_failure(m);
        CHECK(s5l8920_gpio_input(m,pin,active) &&
              m->bus.read32(m,status)==(kind<2u ? bit:0u),"repeated sample manufactured an edge");
        s5l8920_clear_bus_failure(m);
        CHECK(s5l8920_gpio_input(m,pin,!active) &&
              m->bus.read32(m,status)==((kind<2u || kind==4u) ? bit:0u),
              "wrong reverse-edge polarity or level event erased before ack");
        s5l8920_clear_bus_failure(m); m->bus.write32(m,status,bit);
        CHECK(!m->bus_failure.reason && m->bus.read32(m,status)==0u && !m->cpu.irq_line,
              "inactive source acknowledgement left an interrupt");
        s5l8920_clear_bus_failure(m);
        CHECK(s5l8920_gpio_input(m,pin,active),"second active sample");
        m->bus.write32(m,address,control|0x10u);
        CHECK(!m->bus_failure.reason && !m->cpu.irq_line && m->bus.read32(m,status)==bit,
              "mask erased cause or failed to suppress parent");
        CHECK(s5l8920_reset(m) && m->bus.read32(m,status)==0u && !m->cpu.irq_line,
              "reset retained GPIO pending/parent state");
    }
    CHECK(s5l8920_reset(m),"reset after GPIO interrupt modes");
}

static void test_gpio_irq_banks_and_bounds(s5l8920_t *m) {
    CHECK(s5l8920_reset(m),"reset GPIO bank fixture");
    CHECK(S5L8920_GPIO_IRQ==94u && S5L8920_GPIO_IRQ_PINS==224u &&
          S5L8920_GPIO_IRQ_GROUPS==7u && S5L8920_GPIO_IRQ_STATUS==0x800u,"GPIO IRQ geometry");
    vic_write(m,2u,PL192_INTENABLE,1u<<30);
    for (unsigned pin=0;pin<224u;pin++) {
        CHECK(s5l8920_gpio_input(m,pin,false),"bank initial sample");
        m->bus.write32(m,0x83000000u+4u*pin,0x208u);
        CHECK(s5l8920_gpio_input(m,pin,true),"bank rising sample");
    }
    for (unsigned group=0;group<7u;group++) {
        uint32_t status=0x83000800u+4u*group;
        CHECK(m->bus.read32(m,status)==UINT32_MAX && m->cpu.irq_line,"full pending bank");
        m->bus.write32(m,status,0x55555555u);
        CHECK(m->bus.read32(m,status)==0xaaaaaaaau,"partial W1C changed unselected causes");
        m->bus.write32(m,status,0u);
        CHECK(m->bus.read32(m,status)==0xaaaaaaaau,"zero W1C cleared causes");
        for (unsigned kind=0;kind<6u;kind++) {
            if (kind==0u) (void)m->bus.read8(m,status);
            else if (kind==1u) (void)m->bus.read16(m,status);
            else if (kind==2u) (void)m->bus.read32(m,status+1u);
            else if (kind==3u) m->bus.write8(m,status,0xffu);
            else if (kind==4u) m->bus.write16(m,status,0xffffu);
            else m->bus.write32(m,status+2u,UINT32_MAX);
            CHECK(m->bus_failure.reason==S5L8920_BUS_ACCESS_UNIMPLEMENTED,"IRQ status width/alignment accepted");
            s5l8920_clear_bus_failure(m);
            CHECK(m->bus.read32(m,status)==0xaaaaaaaau,"rejected IRQ status access changed causes");
        }
        m->bus.write32(m,status,UINT32_MAX);
        CHECK(m->bus.read32(m,status)==0u && m->cpu.irq_line==(group<6u),"bank W1C lost other bank interrupt");
    }
    for (unsigned pin=224u;pin<368u;pin++) {
        uint32_t address=0x83000000u+4u*pin;
        m->bus.write32(m,address,0x213u);
        for (unsigned mode=4u;mode<=0xcu;mode+=2u) {
            m->bus.write32(m,address,0x200u|mode);
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"non-interrupt pin accepted IRQ mode");
            s5l8920_clear_bus_failure(m);
            CHECK(m->bus.read32(m,address)==0x213u,"rejected IRQ mode changed ordinary output");
        }
    }
    CHECK(s5l8920_set_irq(m,94u,true),"assert external GPIO parent");
    m->bus.write32(m,0x83000800u,UINT32_MAX);
    CHECK(m->cpu.irq_line && s5l8920_set_irq(m,94u,false) && !m->cpu.irq_line,
          "GPIO acknowledgement erased external source94");
    CHECK(s5l8920_gpio_input(m,0u,false) && s5l8920_gpio_input(m,0u,true) &&
          s5l8920_set_irq(m,94u,false) && m->cpu.irq_line,"external update erased GPIO cause");
    (void)m->bus.read32(m,0x8300081cu);
    s5l8920_bus_failure_t stopped=m->bus_failure;
    CHECK(s5l8920_gpio_input(m,1u,false) && s5l8920_gpio_input(m,1u,true) &&
          !memcmp(&stopped,&m->bus_failure,sizeof stopped),"IRQ input event erased latched diagnostic");
    s5l8920_clear_bus_failure(m);
    CHECK(m->bus.read32(m,0x83000800u)==3u,"latched diagnostic blocked external GPIO event");
    CHECK(s5l8920_set_irq(m,94u,true) && s5l8920_reset(m) &&
          m->vic[2].input==(1u<<30) && !m->cpu.irq_line && m->bus.read32(m,0x83000800u)==0u,
          "reset lost external source94 or retained internal cause");
    CHECK(s5l8920_set_irq(m,94u,false),"withdraw external GPIO parent");
}

static bool test_gpio_irq_first_samples(s5l8920_t *m) {
    /* Fresh board state without two simultaneous 256 MiB allocations. */
    s5l8920_free(m);
    bool ready=s5l8920_init(m);
    CHECK(ready,"fresh GPIO sample board");
    if (!ready) return false;
    const uint32_t modes[]={0x208u,0x20au,0x20cu,0x204u,0x206u};
    for (unsigned n=0;n<5u;n++) for (unsigned high=0;high<2u;high++) {
        unsigned pin=2u*n+high; uint32_t bit=1u<<pin;
        m->bus.write32(m,0x83000000u+4u*pin,modes[n]);
        CHECK(!m->bus_failure.reason && m->bus.read32(m,0x83000800u)==0u,
              "unsampled input generated an interrupt");
        (void)m->bus.read32(m,0x83000000u+4u*pin);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"IRQ config invented input sample");
        CHECK(s5l8920_gpio_input(m,pin,high!=0u),"first sample");
        s5l8920_clear_bus_failure(m);
        bool level=(n==3u && high) || (n==4u && !high);
        CHECK(m->bus.read32(m,0x83000800u)==(level ? bit:0u),"initial sample created an edge or missed active level");
        m->bus.write32(m,0x83000000u+4u*pin,0x210u);
        m->bus.write32(m,0x83000800u,UINT32_MAX);
    }
    CHECK(s5l8920_gpio_input(m,10u,true),"sample before IRQ programming");
    m->bus.write32(m,0x83000028u,0x214u);
    CHECK(m->bus.read32(m,0x83000800u)==(1u<<10) && !m->gpio_irq,
          "programming active masked level missed its cause");
    m->bus.write32(m,0x83000028u,0x210u); m->bus.write32(m,0x83000800u,UINT32_MAX);
    m->bus.write32(m,0x83000028u,0x208u);
    CHECK(m->bus.read32(m,0x83000800u)==0u && !m->gpio_irq,"reconfiguration fabricated an edge");
    m->bus.write32(m,0x83000028u,0x204u);
    CHECK(m->bus.read32(m,0x83000800u)==(1u<<10) && m->gpio_irq,
          "programming active unmasked level missed its cause");
    CHECK(s5l8920_reset(m),"reset first-sample board");
    return true;
}

static void test_gpio_irq_cpu(s5l8920_t *m) {
    for (unsigned fiq=0;fiq<2u;fiq++) {
        CHECK(s5l8920_reset(m) && s5l8920_gpio_input(m,223u,false),"reset GPIO CPU IRQ fixture");
        m->bus.write32(m,0x8300037cu,0x208u);
        vic_write(m,2u,PL192_VECTADDR0+4u*30u,0x8000005eu);
        vic_write(m,2u,PL192_INTSELECT,fiq ? 1u<<30:0u);
        vic_write(m,2u,PL192_INTENABLE,1u<<30);
        map_test_vectors(m);
        put(m,0x4000u+(0x830u*4u),0x83000c02u);
        uint32_t vector=fiq ? 0x1cu:0x18u;
        put(m,vector,0xea000000u|((0x1000u-vector-8u)>>2));
        uint32_t at=0x1000u;
        if (!fiq) {
            put(m,at,0xe5910000u); at+=4u; /* Read root vector. */
            put(m,at,0xe2811801u); at+=4u; /* Next cascade bank. */
            put(m,at,0xe5910000u); at+=4u;
        }
        put(m,at,0xe5845000u); at+=4u; /* GPIO W1C. */
        if (!fiq) for (unsigned bank=0;bank<3u;bank++) {
            if (bank) { put(m,at,0xe2422801u); at+=4u; }
            put(m,at,0xe5823000u); at+=4u; /* Cascade EOI. */
        }
        put(m,at,0xe25ef004u);
        m->cpu.r[1]=vic_address(0u,PL192_ADDRESS); m->cpu.r[2]=vic_address(2u,PL192_ADDRESS);
        m->cpu.r[3]=0u; m->cpu.r[4]=0x83000818u; m->cpu.r[5]=0x80000000u;
        CHECK(s5l8920_gpio_input(m,223u,true) && m->cpu.irq_line==(fiq==0u) &&
              m->cpu.fiq_line==(fiq!=0u),"GPIO source94 IRQ/FIQ routing");
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[15]==vector && m->cpu.r[14]==0x204u &&
              (m->cpu.cpsr&ARM_CPSR_MODE_MASK)==(fiq ? ARM_MODE_FIQ:ARM_MODE_IRQ),"GPIO CPU exception entry");
        unsigned steps=0;
        while (m->cpu.r[15]!=0x200u && steps++<20u) if (arm_step(&m->cpu)!=ARM_OK) break;
        CHECK(m->cpu.r[15]==0x200u && m->cpu.cpsr==(ARM_MODE_SYS|ARM_CPSR_C) &&
              !m->bus_failure.reason && m->bus.read32(m,0x83000818u)==0u &&
              !m->cpu.irq_line && !m->cpu.fiq_line,"GPIO acknowledge/EOI/exception return");
        for (unsigned bank=0;bank<3u;bank++) CHECK(!m->vic[bank].in_service,"GPIO cascade left in service");
        CHECK(s5l8920_gpio_input(m,223u,false) && !m->cpu.irq_line && !m->cpu.fiq_line &&
              s5l8920_gpio_input(m,223u,true) && m->cpu.irq_line==(fiq==0u) &&
              m->cpu.fiq_line==(fiq!=0u),"GPIO edge failed to rearm after handler");
    }
    CHECK(s5l8920_reset(m),"reset after GPIO CPU IRQ fixture");
}

static bool test_gpio_configuration_fields(s5l8920_t *m) {
    s5l8920_free(m);
    bool ready=s5l8920_init(m);
    CHECK(ready,"fresh GPIO configuration fixture");
    if (!ready) return false;
    const uint32_t unsampled[]={0xe30u,0x650u,0xb70u,0xe1eu,0xd1fu};
    for (unsigned n=0;n<sizeof unsampled/sizeof unsampled[0];n++) {
        uint32_t address=0x83000000u+4u*n;
        m->bus.write32(m,address,unsampled[n]);
        CHECK(!m->bus_failure.reason,"unsampled configuration write refused");
        (void)m->bus.read32(m,address);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,
              "configuration write synthesized an input sample");
        s5l8920_clear_bus_failure(m);
    }
    const uint32_t pulls[]={0u,0x80u,0x100u};
    for (unsigned pin=0;pin<368u;pin++) for (unsigned drive=0;drive<4u;drive++)
     for (unsigned pull=0;pull<3u;pull++) {
        uint32_t address=0x83000000u+4u*pin, fields=(drive<<10)|pulls[pull];
        CHECK(s5l8920_gpio_input(m,pin,true),"configuration input sample");
        for (unsigned mode=0;mode<2u;mode++) {
            uint32_t control=fields|0x210u|(mode ? 2u:0u);
            s5l8920_clear_bus_failure(m); m->bus.write32(m,address,control);
            CHECK(!m->bus_failure.reason && m->bus.read32(m,address)==(control|(mode ? 0u:1u)),
                  "GPIO drive/pull storage altered digital input/output behavior");
        }
        for (unsigned selector=1;selector<4u;selector++) {
            uint32_t control=fields|0x210u|(selector<<5);
            s5l8920_clear_bus_failure(m); m->bus.write32(m,address,control);
            CHECK(!m->bus_failure.reason && m->bus.read32(m,address)==(control|1u),
                  "peripheral configuration lost explicit sample or fields");
        }
        s5l8920_clear_bus_failure(m); m->bus.write32(m,address,fields|0x21eu);
        CHECK(!m->bus_failure.reason && m->bus.read32(m,address)==(fields|0x21fu),
              "interrupt-off input configuration readback");
        s5l8920_clear_bus_failure(m); m->bus.write32(m,address,fields|0x1eu);
        CHECK(!m->bus_failure.reason && m->gpio[pin].programmed && m->gpio[pin].control==(fields|0x1eu),
              "disabled-input configuration write lost fields");
        (void)m->bus.read32(m,address);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,
              "disabled sampler fabricated bit0 from zero or external sample");
        s5l8920_clear_bus_failure(m);
    }
    CHECK(!m->gpio_irq && !m->cpu.irq_line && !m->cpu.fiq_line,
          "passive fields/off mode generated a GPIO interrupt");
    vic_write(m,2u,PL192_INTENABLE,1u<<30);
    const uint32_t passive[]={0xd1fu,0x61eu,0xa30u,0xe50u,0x670u};
    for (unsigned n=0;n<sizeof passive/sizeof passive[0];n++) {
        CHECK(s5l8920_gpio_input(m,0u,false),"transition initial sample");
        m->bus.write32(m,0x83000000u,0xe08u);
        CHECK(s5l8920_gpio_input(m,0u,true) && m->cpu.irq_line &&
              m->bus.read32(m,0x83000800u)==1u,"drive field broke rising IRQ");
        m->bus.write32(m,0x83000000u,passive[n]);
        CHECK(!m->bus_failure.reason && !m->cpu.irq_line &&
              m->bus.read32(m,0x83000800u)==1u,"passive mode lost pending or kept IRQ active");
        m->bus.write32(m,0x83000000u,0xe08u);
        CHECK(m->cpu.irq_line,"return to IRQ mode lost pending event");
        m->bus.write32(m,0x83000000u,passive[n]);
        m->bus.write32(m,0x83000800u,1u);
        CHECK(s5l8920_gpio_input(m,0u,false) && s5l8920_gpio_input(m,0u,true) &&
              !m->cpu.irq_line && m->bus.read32(m,0x83000800u)==0u,
              "off/peripheral mode latched an input transition");
        m->bus.write32(m,0x83000000u,0xe08u);
        CHECK(!m->cpu.irq_line,"configuration synthesized an edge");
        m->bus.write32(m,0x83000000u,0xe04u);
        CHECK(m->cpu.irq_line,"drive field broke level IRQ");
        m->bus.write32(m,0x83000000u,passive[n]);
        m->bus.write32(m,0x83000800u,1u);
        CHECK(!m->cpu.irq_line && m->bus.read32(m,0x83000800u)==0u,
              "off/peripheral mode relatched an active level");
    }
    const uint32_t unsupported[]={0xd0eu,0xd3eu,0xd1cu,0xe20u,0xe32u,
        0xe34u,0xe38u,0xe3eu,0x430u,0xff0u,0x1d1eu};
    for (unsigned n=0;n<sizeof unsupported/sizeof unsupported[0];n++) {
        m->bus.write32(m,0x83000000u,0xd1fu);
        m->bus.write32(m,0x83000000u,unsupported[n]);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
              m->gpio[0].control==0xd1fu && !m->gpio_irq && !m->gpio_pending[0],
              "invalid configuration mutated disabled control or IRQ state");
        s5l8920_clear_bus_failure(m);
    }
    CHECK(s5l8920_reset(m),"reset GPIO field configurations");
    for (unsigned pin=0;pin<368u;pin++) {
        CHECK(!m->gpio[pin].programmed && !m->gpio[pin].control &&
              m->gpio[pin].input_valid && m->gpio[pin].input_high,
              "reset retained fields or lost explicit sample");
    }
    CHECK(!m->gpio_irq && !m->cpu.irq_line && !m->cpu.fiq_line,"reset retained configuration IRQ");
    return true;
}

static void test_i2c_staging_bus(s5l8920_t *m) {
    for (unsigned bus=0;bus<3u;bus++) for (unsigned write=0;write<2u;write++)
     for (unsigned width=0;width<2u;width++) {
        CHECK(s5l8920_reset(m),"reset I2C staging fixture");
        uint32_t base=0x83200000u+0x100000u*bus;
        const uint32_t offsets[]={8u,12u,0u,16u,20u,24u,32u,36u};
        const uint32_t values[]={0x30u,0x37u,0x55u,0xaau,0u,1u,0x5au,4u|write};
        for (unsigned n=0;n<8u;n++) {
            if (n==6u && !write) continue;
            if (width) m->bus.write32(m,base+offsets[n],values[n]);
            else m->bus.write8(m,base+offsets[n],(uint8_t)values[n]);
            CHECK(!m->bus_failure.reason,"original I2C staging write refused");
        }
        CHECK(m->bus.read32(m,base+12u)==0u && !m->bus_failure.reason && !m->cpu.irq_line,
              "I2C start invented completion or slave response");
        m->bus.write32(m,base+36u,4u|write);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,
              "active I2C request accepted another command");
        s5l8920_clear_bus_failure(m);
        (void)m->bus.read8(m,base+32u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,
              "I2C FIFO fabricated bytes without endpoint completion");
    }
    CHECK(s5l8920_reset(m),"reset after I2C staging fixture");
}

static void i2c_program(s5l8920_t *m,unsigned bus,bool write,unsigned length,bool byte) {
    uint32_t base=0x83200000u+bus*0x100000u;
    const uint32_t offsets[]={8u,12u,0u,16u,20u,24u};
    const uint32_t values[]={byte ? 0x30u:0xf0u,0x37u,0x74u+bus,0xc3u,0u,length};
    for (unsigned n=0;n<6u;n++) {
        if (byte) m->bus.write8(m,base+offsets[n],(uint8_t)values[n]);
        else m->bus.write32(m,base+offsets[n],values[n]);
    }
    if (write) for (unsigned n=0;n<length;n++) {
        uint8_t value=(uint8_t)(n*73u+bus);
        if (byte) m->bus.write8(m,base+32u,value);
        else m->bus.write32(m,base+32u,value);
    }
    if (byte) m->bus.write8(m,base+36u,write ? 5u:4u);
    else m->bus.write32(m,base+36u,write ? 5u:4u);
}

static void test_i2c_endpoints(s5l8920_t *m) {
    const unsigned lengths[]={0u,1u,128u};
    for (unsigned bus=0;bus<3u;bus++) for (unsigned write=0;write<2u;write++)
     for (unsigned success=0;success<2u;success++) for (unsigned byte=0;byte<2u;byte++)
      for (unsigned n=0;n<3u;n++) {
        unsigned length=lengths[n]; if (!write && !length) continue;
        CHECK(s5l8920_reset(m),"reset I2C endpoint fixture");
        uint32_t base=0x83200000u+bus*0x100000u,irq=1u<<(19u-bus);
        vic_write(m,0u,PL192_INTENABLE,irq);
        i2c_program(m,bus,write!=0u,length,byte!=0u);
        s5l8920_i2c_request_t request={0},again={0};
        CHECK(!m->bus_failure.reason && !m->cpu.irq_line &&
              s5l8920_i2c_request(m,bus,&request) && request.sequence &&
              request.address==0x74u+bus && request.subaddress==0xc3u &&
              request.length==length && request.write==(write!=0u),"I2C request fields or start status");
        for (unsigned j=0;j<128u;j++)
            CHECK(request.data[j]==(write && j<length ? (uint8_t)(j*73u+bus):0u),
                  "I2C request TX payload or stale unused bytes");
        CHECK(s5l8920_i2c_request(m,bus,&again) && again.sequence==request.sequence &&
              !memcmp(again.data,request.data,sizeof again.data),"I2C observation consumed request");
        uint8_t response[128]; for (unsigned j=0;j<128u;j++) response[j]=(uint8_t)(255u-j*37u-bus);
        s5l8920_i2c_t before=m->i2c[bus];
        CHECK(!s5l8920_i2c_complete(m,bus,request.sequence-1u,success!=0u,NULL,0u) &&
              !s5l8920_i2c_complete(m,bus,request.sequence,true,response,write ? length:129u) &&
              !memcmp(&before,&m->i2c[bus],sizeof before),"invalid I2C response mutated active request");
        m->bus.write32(m,base+36u,write ? 5u:4u);
        s5l8920_bus_failure_t stopped=m->bus_failure;
        CHECK(stopped.reason==S5L8920_BUS_REGISTER_REFUSED,"I2C busy diagnostic");
        size_t size=success && !write ? length:0u;
        CHECK(s5l8920_i2c_complete(m,bus,request.sequence,success!=0u,size ? response:NULL,size) &&
              !memcmp(&stopped,&m->bus_failure,sizeof stopped) &&
              s5l8920_set_irq(m,19u-bus,false) && m->cpu.irq_line,
              "I2C completion lost diagnostic or interrupt");
        before=m->i2c[bus]; again=request;
        CHECK(!s5l8920_i2c_complete(m,bus,request.sequence,success!=0u,size ? response:NULL,size) &&
              !s5l8920_i2c_request(m,bus,&again) && again.sequence==request.sequence &&
              !memcmp(&before,&m->i2c[bus],sizeof before),"I2C duplicate completion or false request mutated state");
        s5l8920_clear_bus_failure(m);
        uint32_t status=success ? 0x10u:0x20u;
        CHECK(m->bus.read8(m,base+12u)==status && m->bus.read32(m,base+12u)==status,
              "I2C completion byte/word status differs");
        m->bus.write32(m,base+12u,status^0x30u);
        CHECK(m->bus.read32(m,base+12u)==status && m->cpu.irq_line,"unselected W1C lost completion");
        m->bus.write32(m,base+8u,0u);
        CHECK(!m->cpu.irq_line && m->bus.read32(m,base+12u)==status,"disable lost latched I2C status");
        CHECK(s5l8920_set_irq(m,19u-bus,true) && m->cpu.irq_line,"external I2C source assertion");
        m->bus.write8(m,base+12u,(uint8_t)status);
        CHECK(m->cpu.irq_line && m->bus.read32(m,base+12u)==0u &&
              s5l8920_set_irq(m,19u-bus,false) && !m->cpu.irq_line,
              "I2C acknowledgement erased external source or retained cause");
        if (size) {
            m->bus.write32(m,base+8u,0x30u); m->bus.write32(m,base+36u,4u);
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
                  !m->i2c[bus].active && !m->i2c[bus].rx_cursor,
                  "new command discarded unread I2C bytes");
            s5l8920_clear_bus_failure(m);
        }
        for (unsigned j=0;j<size;j++) {
            uint32_t value=j&1u ? m->bus.read8(m,base+32u):m->bus.read32(m,base+32u);
            CHECK(!m->bus_failure.reason && value==response[j],"ack discarded RX or FIFO byte order changed");
        }
        (void)m->bus.read32(m,base+32u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"empty I2C FIFO supplied data");
        s5l8920_clear_bus_failure(m);
    }
    CHECK(s5l8920_reset(m),"reset I2C sequence fixture");
    i2c_program(m,0u,false,1u,false);
    s5l8920_i2c_request_t old={0},current={0};
    CHECK(s5l8920_i2c_request(m,0u,&old) && s5l8920_reset(m),"reset active request");
    i2c_program(m,0u,false,1u,false);
    uint8_t value=0x96u;
    CHECK(s5l8920_i2c_request(m,0u,&current) && current.sequence>old.sequence &&
          !s5l8920_i2c_complete(m,0u,old.sequence,true,&value,1u) && m->i2c[0].active,
          "stale pre-reset response completed a new request");
    CHECK(s5l8920_i2c_complete(m,0u,current.sequence,true,&value,1u),"current response refused after reset");
    vic_write(m,0u,PL192_INTENABLE,1u<<19);
    CHECK(m->cpu.irq_line,"pending completion lost before VIC enable");
    CHECK(s5l8920_set_irq(m,19u,true) && s5l8920_reset(m) && m->vic[0].input==(1u<<19) &&
          !m->i2c[0].active && !m->i2c[0].status && !m->i2c[0].programmed &&
          !m->i2c[0].rx_count && !m->i2c[0].tx_count && !m->cpu.irq_line,
          "reset retained I2C state or lost external line");
    CHECK(s5l8920_set_irq(m,19u,false),"withdraw reset I2C source");
    s5l8920_t empty={0}; s5l8920_i2c_request_t sentinel;
    memset(&sentinel,0xa5,sizeof sentinel); current=sentinel;
    CHECK(!s5l8920_i2c_request(NULL,0u,&current) && !s5l8920_i2c_request(&empty,0u,&current) &&
          !s5l8920_i2c_request(m,3u,&current) && !s5l8920_i2c_request(m,0u,NULL) &&
          !memcmp(&current,&sentinel,sizeof current),"invalid request observation changed output");
    CHECK(!s5l8920_i2c_complete(NULL,0u,0u,false,NULL,0u) &&
          !s5l8920_i2c_complete(&empty,0u,0u,false,NULL,0u) &&
          !s5l8920_i2c_complete(m,3u,0u,false,NULL,0u),"invalid completion accepted");
}

static void test_i2c_bounds(s5l8920_t *m) {
    for (unsigned bus=0;bus<3u;bus++) {
        CHECK(s5l8920_reset(m),"reset I2C bounds fixture");
        uint32_t base=0x83200000u+bus*0x100000u;
        const uint32_t bad[][2]={{0u,0x80u},{8u,0x10u},{8u,0x31u},{8u,0x10030u},
            {12u,8u},{12u,0x40u},{16u,256u},{20u,1u},{24u,129u},{32u,1u},
            {36u,4u},{36u,5u},{36u,0u},{36u,6u},{4u,0u},{28u,0u},{0xffcu,0u}};
        for (unsigned n=0;n<sizeof bad/sizeof bad[0];n++) {
            s5l8920_i2c_t before=m->i2c[bus];
            m->cpu.r[15]=0x1234u; m->bus.write32(m,base+bad[n][0],bad[n][1]);
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && m->bus_failure.write &&
                  m->bus_failure.pc==0x1234u && m->bus_failure.address==base+bad[n][0] &&
                  !memcmp(&before,&m->i2c[bus],sizeof before),"invalid I2C field mutated controller");
            s5l8920_clear_bus_failure(m);
        }
        for (unsigned offset=0;offset<40u;offset+=4u) {
            if (offset==12u) continue;
            (void)m->bus.read32(m,base+offset);
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"unverified I2C register read returned data");
            s5l8920_clear_bus_failure(m);
        }
        for (unsigned kind=0;kind<8u;kind++) {
            s5l8920_i2c_t before=m->i2c[bus];
            if (kind==0u) (void)m->bus.read16(m,base+12u);
            else if (kind==1u) m->bus.write16(m,base+8u,0x30u);
            else if (kind<5u) m->bus.write8(m,base+7u+kind,0x30u);
            else (void)m->bus.read32(m,base+8u+kind);
            CHECK(m->bus_failure.reason==S5L8920_BUS_ACCESS_UNIMPLEMENTED &&
                  !memcmp(&before,&m->i2c[bus],sizeof before),"I2C lane/width reached controller");
            s5l8920_clear_bus_failure(m);
        }
        (void)m->bus.read32(m,base+0x1000u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_UNMAPPED,"I2C aperture leaked into bank gap");
        s5l8920_clear_bus_failure(m);
        CHECK(!m->bus.host_ram(m,base,4u) && !m->bus.host_ram_write(m,base,4u) &&
              !s5l8920_load(m,base,m->ram,4u),"I2C exposed as host RAM");
        const uint32_t fields[]={8u,0u,16u,20u,24u};
        const uint32_t values[]={0x30u,0x74u,0xc3u,0u,1u};
        for (unsigned omitted=0;omitted<5u;omitted++) {
            CHECK(s5l8920_reset(m),"reset incomplete I2C fields");
            for (unsigned n=0;n<5u;n++) if (n!=omitted) m->bus.write32(m,base+fields[n],values[n]);
            s5l8920_i2c_t before=m->i2c[bus]; m->bus.write32(m,base+36u,4u);
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
                  !memcmp(&before,&m->i2c[bus],sizeof before),"incomplete I2C request started");
        }
        CHECK(s5l8920_reset(m),"reset I2C FIFO fixture");
        for (unsigned n=0;n<5u;n++) m->bus.write32(m,base+fields[n],values[n]);
        m->bus.write32(m,base+24u,2u); m->bus.write32(m,base+32u,0x12u);
        m->bus.write32(m,base+36u,5u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
              m->i2c[bus].tx_count==1u && !m->i2c[bus].active,"partial TX request started");
        s5l8920_clear_bus_failure(m); m->bus.write32(m,base+32u,0x34u);
        m->bus.write32(m,base+32u,0x56u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && m->i2c[bus].tx_count==2u,
              "I2C TX overflow changed FIFO");
        s5l8920_clear_bus_failure(m); m->bus.write32(m,base+36u,4u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && !m->i2c[bus].active,
              "RX command consumed staged TX data");
        s5l8920_clear_bus_failure(m); m->bus.write32(m,base+36u,5u);
        s5l8920_i2c_request_t request={0};
        CHECK(s5l8920_i2c_request(m,bus,&request) && request.data[0]==0x12u && request.data[1]==0x34u,
              "I2C TX retry lost committed FIFO prefix");
        for (unsigned n=0;n<5u;n++) {
            s5l8920_i2c_t before=m->i2c[bus]; m->bus.write32(m,base+fields[n],values[n]);
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
                  !memcmp(&before,&m->i2c[bus],sizeof before),"active request fields changed");
            s5l8920_clear_bus_failure(m);
        }
    }
    CHECK(s5l8920_reset(m),"reset multibank I2C fixture");
    uint32_t all=(1u<<19)|(1u<<18)|(1u<<17); vic_write(m,0u,PL192_INTENABLE,all);
    for (unsigned bus=0;bus<3u;bus++) {
        i2c_program(m,bus,true,0u,false); s5l8920_i2c_request_t request={0};
        CHECK(s5l8920_i2c_request(m,bus,&request) &&
              s5l8920_i2c_complete(m,bus,request.sequence,true,NULL,0u),"independent I2C bank completion");
    }
    CHECK((m->vic[0].input&all)==all && m->cpu.irq_line,"I2C banks lost simultaneous causes");
    for (unsigned bus=0;bus<3u;bus++) {
        m->bus.write32(m,0x8320000cu+bus*0x100000u,0x10u); all&=~(1u<<(19u-bus));
        CHECK((m->vic[0].input&((1u<<19)|(1u<<18)|(1u<<17)))==all && m->cpu.irq_line==(all!=0u),
              "I2C bank acknowledgement cleared a different source");
    }
    CHECK(s5l8920_reset(m),"reset after I2C bounds");
}

static void test_i2c_checked_cpu(s5l8920_t *m) {
    for (unsigned mapped=0;mapped<2u;mapped++) for (unsigned byte=0;byte<2u;byte++) {
        CHECK(s5l8920_reset(m),"reset I2C CPU fixture");
        i2c_program(m,0u,false,1u,false);
        s5l8920_i2c_request_t request={0}; CHECK(s5l8920_i2c_request(m,0u,&request),"CPU pending I2C request");
        uint32_t code=S5L8920_RAM_BASE+0x200u,fifo=0x83200020u;
        if (mapped) {
            map_test_vectors(m);code=0x200u;fifo=0xc5900020u;
            put(m,0x4000u+(fifo>>20)*4u,0x83200c02u);
        }
        put(m,0x200u,byte ? 0xe4d12004u:0xe4912004u); /* LDR[B] r2,[r1],#4 */
        put(m,0x204u,byte ? 0xe4c12004u:0xe4812004u); /* STR[B] r2,[r1],#4 */
        m->cpu.r[15]=code;m->cpu.r[1]=fifo;m->cpu.r[2]=0xabcdef01u;
        CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.r[15]==code && !m->cpu.cycles &&
              m->cpu.r[1]==fifo && m->cpu.r[2]==0xabcdef01u && !m->cpu.cp15.dfsr &&
              m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
              m->bus_failure.address==0x83200020u && m->bus_failure.size==(byte ? 1u:4u),
              "empty I2C load retired or became guest data abort");
        uint8_t response=0x96u;
        CHECK(s5l8920_i2c_complete(m,0u,request.sequence,true,&response,1u),"supply CPU I2C response");
        s5l8920_clear_bus_failure(m);
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[15]==code+4u && m->cpu.cycles==1u &&
              m->cpu.r[1]==fifo+4u && m->cpu.r[2]==0x96u,"I2C receive retry did not consume exactly one byte");
        m->cpu.r[2]=4u;
        CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.r[15]==code+4u && m->cpu.cycles==1u &&
              m->cpu.r[1]==fifo+4u && !m->i2c[0].active && m->bus_failure.write,
              "unacknowledged I2C completion allowed next CPU command");
        s5l8920_clear_bus_failure(m);m->bus.write32(m,0x8320000cu,0x10u);
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[15]==code+8u && m->cpu.cycles==2u &&
              m->cpu.r[1]==fifo+8u && m->i2c[0].active && m->i2c[0].sequence>request.sequence,
              "I2C command retry did not start exactly once");
    }
    CHECK(s5l8920_reset(m),"reset after I2C CPU fixture");
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

static uint64_t pmu_request(s5l8920_t *m,unsigned address,unsigned reg,bool write,unsigned length,uint32_t data) {
    uint32_t base=S5L8920_I2C_BASE;
    const uint32_t offsets[]={8u,12u,0u,16u,20u,24u};
    const uint32_t values[]={0xf0u,0x37u,address,reg,0u,length};
    for (unsigned n=0;n<6u;n++) m->bus.write32(m,base+offsets[n],values[n]);
    if (write) for (unsigned n=0;n<length;n++) m->bus.write32(m,base+32u,(data>>((n%4u)*8u))&255u);
    m->bus.write32(m,base+36u,write ? 5u:4u);
    CHECK(!m->bus_failure.reason && m->i2c[0].active,"PMU request staging");
    return m->i2c[0].sequence;
}

static uint32_t pmu_read_result(s5l8920_t *m) {
    uint32_t result=0u;
    for (unsigned n=0;n<4u;n++) result|=m->bus.read32(m,S5L8920_I2C_BASE+32u)<<(n*8u);
    CHECK(m->i2c[0].status==0x10u,"FIFO consumption acknowledged PMU status");
    m->bus.write32(m,S5L8920_I2C_BASE+12u,0x10u);
    return result;
}

static void test_pmu_rtc(s5l8920_t *m) {
    s5l8920_t empty={0};
    CHECK(!s5l8920_pmu_rtc_configure(NULL,0u,0u) && !s5l8920_pmu_rtc_configure(&empty,0u,0u) &&
          !s5l8920_pmu_rtc_advance(NULL,1u) && !s5l8920_pmu_rtc_advance(&empty,1u) &&
          !s5l8920_pmu_rtc_service(NULL,0u) && !s5l8920_pmu_rtc_service(&empty,0u),"invalid PMU object");
    CHECK(s5l8920_reset(m) && !m->pmu_rtc_configured &&
          !s5l8920_pmu_rtc_advance(m,1u) && !s5l8920_pmu_rtc_service(m,0u),"unconfigured PMU supplied state");
    uint64_t seq=pmu_request(m,0x74u,0x4cu,false,4u,0u);
    s5l8920_i2c_t before=m->i2c[0];
    CHECK(!s5l8920_pmu_rtc_service(m,seq) && !memcmp(&before,&m->i2c[0],sizeof before) &&
          !s5l8920_pmu_rtc_configure(m,0u,0u),"unconfigured pending request changed");
    CHECK(s5l8920_reset(m),"PMU reset");
    const uint32_t values[]={0u,1u,0x12345678u,0x89abcdefu,UINT32_MAX};
    for (unsigned c=0;c<5u;c++) for (unsigned o=0;o<5u;o++) {
        uint32_t counter=values[c],offset=values[o];
        CHECK(s5l8920_reset(m) && s5l8920_pmu_rtc_configure(m,counter,offset),"explicit PMU configuration");
        for (unsigned reg=0;reg<2u;reg++) {
            seq=pmu_request(m,0x74u,reg ? 0x64u:0x4cu,false,4u,0u);
            before=m->i2c[0];
            CHECK(!s5l8920_pmu_rtc_configure(m,0u,0u) && !s5l8920_pmu_rtc_service(m,seq-1u) &&
                  !memcmp(&before,&m->i2c[0],sizeof before),"active configuration or stale completion");
            vic_write(m,0u,PL192_INTENABLE,1u<<19);
            CHECK(s5l8920_pmu_rtc_service(m,seq) && m->cpu.irq_line,"PMU completion IRQ");
            CHECK(!s5l8920_pmu_rtc_service(m,seq) && !s5l8920_pmu_rtc_configure(m,0u,0u),"pending PMU response replaced");
            CHECK(pmu_read_result(m)==(reg ? offset:counter) && !m->cpu.irq_line,"little endian PMU read/ack");
        }
        uint32_t written=counter^offset^0xa55a3cc3u;
        seq=pmu_request(m,0x74u,0x64u,true,4u,written);
        CHECK(s5l8920_pmu_rtc_service(m,seq) && m->pmu_rtc_offset==written && m->pmu_rtc_counter==counter,"offset write atomicity");
        m->bus.write32(m,S5L8920_I2C_BASE+12u,0x10u);
        seq=pmu_request(m,0x74u,0x64u,false,4u,0u);
        CHECK(s5l8920_pmu_rtc_service(m,seq) && pmu_read_result(m)==written,"offset read after write");
        CHECK(s5l8920_timebase_clock(m,UINT64_MAX) && m->pmu_rtc_counter==counter &&
              s5l8920_pmu_rtc_advance(m,1u) && m->pmu_rtc_counter==counter+1u &&
              s5l8920_pmu_rtc_advance(m,UINT32_MAX) && m->pmu_rtc_counter==counter &&
              s5l8920_pmu_rtc_advance(m,0u) && m->pmu_rtc_counter==counter,"independent modulo raw clock");
    }
    for (unsigned kind=0;kind<3u;kind++) for (unsigned n=0;n<(kind==1u ? 129u:(kind==2u ? 128u:256u));n++) for (unsigned write=0;write<2u;write++) {
        unsigned reg=kind ? 0x64u:n,address=kind==2u ? n:0x74u,length=kind==1u ? n:4u;
        if (!write && !length) continue;
        CHECK(s5l8920_reset(m) && s5l8920_pmu_rtc_configure(m,0x12345678u,0x89abcdefu),"PMU negative setup");
        seq=pmu_request(m,address,reg,write!=0u,length,0x01020304u);before=m->i2c[0];
        bool supported=address==0x74u && length==4u && (reg==0x64u || (reg==0x4cu && !write));
        CHECK(s5l8920_pmu_rtc_service(m,seq)==supported,"PMU register/address/length boundary");
        if (!supported) CHECK(!memcmp(&before,&m->i2c[0],sizeof before) &&
            m->pmu_rtc_counter==0x12345678u && m->pmu_rtc_offset==0x89abcdefu &&
            !m->bus_failure.reason,"unsupported request supplied data/status or mutated RTC");
    }
    CHECK(s5l8920_reset(m) && s5l8920_pmu_rtc_configure(m,UINT32_MAX,0x89abcdefu),"PMU reset setup");
    seq=pmu_request(m,0x74u,0x4cu,false,4u,0u);
    CHECK(s5l8920_pmu_rtc_advance(m,1u) && s5l8920_pmu_rtc_service(m,seq),"sample counter at service boundary");
    m->bus.write32(m,S5L8920_I2C_BASE+12u,0x10u);
    CHECK(!s5l8920_pmu_rtc_configure(m,1u,2u),"unread response replaced after ack");
    for (unsigned n=0;n<4u;n++) CHECK(!m->bus.read32(m,S5L8920_I2C_BASE+32u),"wrapped response bytes");
    uint64_t old=pmu_request(m,0x74u,0x64u,true,4u,0xdeadbeefu);
    CHECK(s5l8920_reset(m) && m->pmu_rtc_configured && m->pmu_rtc_counter==0u &&
          m->pmu_rtc_offset==0x89abcdefu && !s5l8920_pmu_rtc_service(m,old),"functional reset changed RTC or completed stale write");
    seq=pmu_request(m,0x74u,0x64u,false,4u,0u);
    CHECK(seq>old && !s5l8920_pmu_rtc_service(m,old),"reset request sequence reused");
    (void)m->bus.read32(m,0u);s5l8920_bus_failure_t diagnostic=m->bus_failure;
    CHECK(s5l8920_pmu_rtc_advance(m,1u) && s5l8920_pmu_rtc_service(m,seq) &&
          !memcmp(&diagnostic,&m->bus_failure,sizeof diagnostic),"PMU host event cleared bus diagnostic");
    s5l8920_clear_bus_failure(m);
    CHECK(pmu_read_result(m)==0x89abcdefu,"retained offset after reset");
    (void)m->bus.read32(m,0u);diagnostic=m->bus_failure;
    CHECK(s5l8920_pmu_rtc_configure(m,7u,8u) && !memcmp(&diagnostic,&m->bus_failure,sizeof diagnostic),"PMU configuration cleared bus diagnostic");
    CHECK(s5l8920_reset(m),"PMU other bus setup");
    i2c_program(m,1u,false,4u,false);before=m->i2c[1];
    CHECK(!s5l8920_pmu_rtc_service(m,before.sequence) && !memcmp(&before,&m->i2c[1],sizeof before),"PMU consumed another I2C bus");
    CHECK(s5l8920_reset(m),"PMU final reset");
}

static bool test_clock_gates(s5l8920_t *m) {
    static s5l8920_t empty, before;
    CHECK(!s5l8920_clock_gate_configure(NULL,0u,0u) &&
          !s5l8920_clock_gate_configure(&empty,0u,0u),"uninitialized gate configuration");
    CHECK(s5l8920_reset(m),"gate initial reset");
    for (unsigned i=0;i<S5L8920_CLOCK_GATE_COUNT;i++) {
        uint32_t address=S5L8920_CLOCK_GATE_BASE+4u*i,initial=0xa55a0000u^(i*0x12345u);
        CHECK(!m->clock_gate[i].configured,"invented gate configuration");
        for (unsigned write=0;write<2u;write++) {
            s5l8920_clear_bus_failure(m);
            if (write) m->bus.write32(m,address,15u); else (void)m->bus.read32(m,address);
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
                  m->bus_failure.address==address && m->bus_failure.write==(write!=0u) &&
                  !m->clock_gate[i].configured,"unconfigured gate access accepted");
        }
        s5l8920_bus_failure_t stopped=m->bus_failure;
        CHECK(s5l8920_clock_gate_configure(m,i,initial) &&
              !memcmp(&stopped,&m->bus_failure,sizeof stopped),"gate configuration lost diagnostic");
        s5l8920_clear_bus_failure(m);
        CHECK(m->bus.read32(m,address)==initial,"raw gate input was interpreted");
        for (unsigned mode=0;mode<16u;mode++) {
            uint32_t previous=m->clock_gate[i].value,value=(initial&~15u)|mode;
            s5l8920_clear_bus_failure(m);m->bus.write32(m,address,value);
            CHECK(m->bus_failure.reason==((mode==0u || mode==15u)?S5L8920_BUS_OK:S5L8920_BUS_REGISTER_REFUSED) &&
                  m->clock_gate[i].value==((mode==0u || mode==15u)?value:previous),"gate mode or atomic rejection");
        }
        for (unsigned bit=4;bit<32u;bit++) {
            uint32_t previous=m->clock_gate[i].value;
            s5l8920_clear_bus_failure(m);m->bus.write32(m,address,previous^(1u<<bit));
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
                  m->clock_gate[i].value==previous,"unknown upper gate bit accepted");
        }
        before=*m;
        CHECK(s5l8920_clock_gate_configure(m,i,initial) && !memcmp(m,&before,sizeof before),
              "reapplying gate input erased guest state");
        CHECK(!s5l8920_clock_gate_configure(m,i,initial^1u) && !memcmp(m,&before,sizeof before),
              "changed gate initial state accepted");
    }
    before=*m;
    CHECK(!s5l8920_clock_gate_configure(m,52u,0u) &&
          !s5l8920_clock_gate_configure(m,UINT32_MAX,0u) && !memcmp(m,&before,sizeof before),
          "invalid gate selector changed board");
    for (unsigned offset=0;offset<4u*S5L8920_CLOCK_GATE_COUNT;offset++) for (unsigned kind=0;kind<6u;kind++) {
        uint32_t address=S5L8920_CLOCK_GATE_BASE+offset;
        before=*m;s5l8920_clear_bus_failure(m);
        if (kind==0u) (void)m->bus.read8(m,address);
        else if (kind==1u) (void)m->bus.read16(m,address);
        else if (kind==2u) (void)m->bus.read32(m,address);
        else if (kind==3u) m->bus.write8(m,address,0u);
        else if (kind==4u) m->bus.write16(m,address,0u);
        else m->bus.write32(m,address,m->clock_gate[offset/4u].value);
        CHECK(m->bus_failure.reason==((kind%3u==2u && !(offset&3u))?S5L8920_BUS_OK:S5L8920_BUS_ACCESS_UNIMPLEMENTED) &&
              !memcmp(m->clock_gate,before.clock_gate,sizeof m->clock_gate),"gate width/alignment changed state");
    }
    for (unsigned i=0;i<2u;i++) {
        s5l8920_clear_bus_failure(m);
        (void)m->bus.read32(m,i?S5L8920_CLOCK_GATE_BASE+4u*S5L8920_CLOCK_GATE_COUNT:S5L8920_CLOCK_GATE_BASE-4u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"gate span grew into unknown PMGR");
    }
    CHECK(s5l8920_reset(m),"gate reset");
    for (unsigned i=0;i<S5L8920_CLOCK_GATE_COUNT;i++)
        CHECK(m->clock_gate[i].configured && m->bus.read32(m,S5L8920_CLOCK_GATE_BASE+4u*i)==(0xa55a0000u^(i*0x12345u)),
              "functional reset did not restore supplied gate input");
    CHECK(!m->bus.host_ram(m,S5L8920_CLOCK_GATE_BASE,4u) &&
          !m->bus.host_ram_write(m,S5L8920_CLOCK_GATE_BASE,4u),"gate registers exposed as RAM");
    s5l8920_free(m);
    CHECK(!s5l8920_clock_gate_configure(m,0u,0u) && !m->clock_gate[0].configured &&
          !m->clock_gate[51].configured,"free retained gate configuration");
    bool ready=s5l8920_init(m);CHECK(ready,"gate reinitialize");if (!ready) return false;
    for (unsigned thumb=0;thumb<2u;thumb++) for (unsigned write=0;write<2u;write++) {
        unsigned gate=50u*thumb+write;
        CHECK(s5l8920_reset(m) && !m->clock_gate[gate].configured,"new board retained gate input");
        put(m,0x100u,thumb?(write?0x0000f8c1u:0x0000f8d1u):(write?0xe5810000u:0xe5910000u));
        m->cpu.r[0]=0xaabbccdfu;m->cpu.r[1]=S5L8920_CLOCK_GATE_BASE+4u*gate;
        m->cpu.r[15]=S5L8920_RAM_BASE+0x100u;
        m->cpu.cpsr=ARM_MODE_SVC|ARM_CPSR_I|ARM_CPSR_F|ARM_CPSR_C|(thumb?ARM_CPSR_T:0u);
        uint32_t flags=m->cpu.cpsr,pc=m->cpu.r[15];
        CHECK(arm_step(&m->cpu)==ARM_HALT && !m->cpu.cycles && m->cpu.r[15]==pc &&
              m->cpu.r[0]==0xaabbccdfu && m->cpu.cpsr==flags &&
              m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && m->bus_failure.write==(write!=0u),
              "unprepared gate CPU access retired");
        before=*m;
        CHECK(s5l8920_clock_gate_configure(m,gate,0xaabbccd0u) &&
              !memcmp(&m->cpu,&before.cpu,sizeof m->cpu) &&
              !memcmp(&m->bus_failure,&before.bus_failure,sizeof m->bus_failure),"gate preparation changed stopped CPU");
        s5l8920_clear_bus_failure(m);
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==1u && m->cpu.r[15]==pc+4u &&
              m->cpu.cpsr==flags && !m->bus_failure.reason &&
              m->cpu.r[0]==(write?0xaabbccdfu:0xaabbccd0u) &&
              m->clock_gate[gate].value==(write?0xaabbccdfu:0xaabbccd0u),"gate CPU retry failed");
    }
    return true;
}

static bool test_chipid(s5l8920_t *m) {
    static s5l8920_t empty, before;
    const uint32_t words[]={0xfedcba98u,0x01234567u,0u,UINT32_MAX};
    CHECK(!s5l8920_chipid_configure(NULL,0u,1u) &&
          !s5l8920_chipid_configure(&empty,0u,1u) && !empty.chipid_configured,
          "uninitialized board accepted identification");
    CHECK(s5l8920_reset(m) && !m->chipid_configured,"invented identification defaults");
    for (unsigned i=0;i<4u;i++) {
        uint32_t address=S5L8920_CHIPID_BASE+4u*i;
        s5l8920_clear_bus_failure(m);
        (void)m->bus.read32(m,address);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
              m->bus_failure.address==address,"unconfigured word did not refuse");
        s5l8920_bus_failure_t stopped=m->bus_failure;
        CHECK(s5l8920_chipid_configure(m,4u*i,words[i]) &&
              !memcmp(&stopped,&m->bus_failure,sizeof stopped),"configuration lost stopped access");
        CHECK(m->chipid_configured==(1u<<(i+1u))-1u,"configuration invented neighboring words");
        s5l8920_clear_bus_failure(m);
        CHECK(m->bus.read32(m,address)==words[i] && !m->bus_failure.reason,
              "explicit identification word changed");
        before=*m;
        CHECK(s5l8920_chipid_configure(m,4u*i,words[i]) && !memcmp(m,&before,sizeof before),
              "identical configuration was not idempotent");
        CHECK(!s5l8920_chipid_configure(m,4u*i,words[i]^1u) && !memcmp(m,&before,sizeof before),
              "immutable identification changed");
    }
    CHECK(!m->bus.host_ram(m,S5L8920_CHIPID_BASE,4u) &&
          !m->bus.host_ram_write(m,S5L8920_CHIPID_BASE,4u) &&
          !s5l8920_load(m,S5L8920_CHIPID_BASE,words,sizeof words),"RAM shortcut exposed identification");
    for (unsigned offset=0;offset<32u;offset++) {
        if (offset<16u && !(offset&3u)) continue;
        before=*m;
        CHECK(!s5l8920_chipid_configure(m,offset,0u) && !memcmp(m,&before,sizeof before),
              "invalid selector changed board");
    }
    before=*m;
    CHECK(!s5l8920_chipid_configure(m,UINT32_MAX,0u) && !memcmp(m,&before,sizeof before),
          "wrapped selector changed board");
    for (unsigned offset=0;offset<16u;offset++) for (unsigned kind=0;kind<6u;kind++) {
        uint32_t address=S5L8920_CHIPID_BASE+offset;
        s5l8920_clear_bus_failure(m);
        if (kind==0u) (void)m->bus.read8(m,address);
        else if (kind==1u) (void)m->bus.read16(m,address);
        else if (kind==2u) (void)m->bus.read32(m,address);
        else if (kind==3u) m->bus.write8(m,address,0x5au);
        else if (kind==4u) m->bus.write16(m,address,0x5a5au);
        else m->bus.write32(m,address,0x5a5a5a5au);
        s5l8920_bus_reason_t expected=kind%3u!=2u || (offset&3u) ?
            S5L8920_BUS_ACCESS_UNIMPLEMENTED : (kind==2u ? S5L8920_BUS_OK:S5L8920_BUS_REGISTER_REFUSED);
        CHECK(m->bus_failure.reason==expected && m->chipid_configured==15u &&
              !memcmp(m->chipid_words,words,sizeof words),"unsupported access changed identification");
    }
    const uint32_t outside[]={S5L8920_CHIPID_BASE-4u,S5L8920_CHIPID_BASE+16u,S5L8920_CHIPID_BASE+0x1000u};
    for (unsigned i=0;i<sizeof outside/sizeof *outside;i++) {
        s5l8920_clear_bus_failure(m);(void)m->bus.read32(m,outside[i]);
        CHECK(m->bus_failure.reason==S5L8920_BUS_UNMAPPED,"identification aperture widened");
    }
    CHECK(s5l8920_reset(m) && m->chipid_configured==15u &&
          !memcmp(m->chipid_words,words,sizeof words),"reset changed fixed identification");
    for (unsigned thumb=0;thumb<2u;thumb++) {
        s5l8920_free(m);
        bool ready=s5l8920_init(m);CHECK(ready && !m->chipid_configured,"new board retained identification");
        if (!ready) return false;
        put(m,0x100u,thumb ? 0x0000f8d1u:0xe5910000u);
        m->cpu.r[0]=0xabcdef01u;m->cpu.r[1]=S5L8920_CHIPID_BASE+12u*thumb;
        m->cpu.r[15]=S5L8920_RAM_BASE+0x100u;
        m->cpu.cpsr=ARM_MODE_SVC|ARM_CPSR_I|ARM_CPSR_F|ARM_CPSR_C|(thumb ? ARM_CPSR_T:0u);
        uint32_t flags=m->cpu.cpsr,pc=m->cpu.r[15];
        CHECK(arm_step(&m->cpu)==ARM_HALT && !m->cpu.cycles && m->cpu.r[15]==pc &&
              m->cpu.r[0]==0xabcdef01u && m->cpu.cpsr==flags &&
              m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"unprepared CPU read retired");
        before=*m;
        CHECK(s5l8920_chipid_configure(m,12u*thumb,words[thumb]) &&
              !memcmp(&before.cpu,&m->cpu,sizeof m->cpu) &&
              !memcmp(&before.bus_failure,&m->bus_failure,sizeof m->bus_failure),"host preparation changed stopped CPU");
        s5l8920_clear_bus_failure(m);
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==1u && m->cpu.r[15]==pc+4u &&
              m->cpu.r[0]==words[thumb] && m->cpu.cpsr==flags && !m->bus_failure.reason,
              "checked CPU retry did not load supplied word");
    }
    return true;
}

int main(void) {
    test_empty_nvram_proxy();
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
    test_gpio_output_registers(&m);
    test_gpio_input_samples(&m);
    test_gpio_checked_cpu(&m);
    test_gpio_interrupt_modes(&m);
    test_gpio_irq_banks_and_bounds(&m);
    if (!test_gpio_irq_first_samples(&m)) return 1;
    test_gpio_irq_cpu(&m);
    if (!test_gpio_configuration_fields(&m)) return 1;
    test_i2c_staging_bus(&m);
    test_i2c_endpoints(&m);
    test_i2c_bounds(&m);
    test_i2c_checked_cpu(&m);
    test_pmu_rtc(&m);
    test_fiq_and_reset(&m);
    if (!test_clock_gates(&m)) return 1;
    if (!test_chipid(&m)) return 1;
    s5l8920_free(&m);
    CHECK(!m.ram && !m.cpu.bus && !m.bus.ctx && !s5l8920_reset(&m), "free left live host wiring");
    CHECK(!m.chipid_configured && !m.chipid_words[0] && !m.chipid_words[3] &&
          !s5l8920_chipid_configure(&m,0u,1u),"freed board retained identification");
    CHECK(!s5l8920_timebase_clock(&m,1u) && !m.timebase_ticks, "freed board accepted timebase input");
    CHECK(!m.pmu_rtc_configured && !m.pmu_rtc_counter && !m.pmu_rtc_offset &&
          !s5l8920_pmu_rtc_configure(&m,1u,2u) && !s5l8920_pmu_rtc_advance(&m,1u) &&
          !s5l8920_pmu_rtc_service(&m,1u),"freed board retained PMU state or accepted an event");
    size_t count=999u;
    CHECK(!s5l8920_uart0_clock(&m,true,1u,NULL,0u,&count) && count==999u && !s5l8920_uart0_receive(&m,0u),
          "freed board accepted a UART event");
    s5l8920_free(&m);
    printf("%u passed, %u failed\n",passed,failed);
    return failed ? 1 : 0;
}
