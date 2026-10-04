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
    CHECK(m->bus_failure.reason==S5L8920_BUS_OK && m->uart0.ucon==0x1405u &&
          m->uart0.tx_busy && !m->cpu.irq_line && !m->cpu.fiq_line,
          "receive interrupt enable invented an event or changed active TX");
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

static unsigned pmu_read_bytes(s5l8920_t *m,unsigned length) {
    unsigned value=0u;
    for (unsigned n=0;n<length;n++) value|=m->bus.read32(m,S5L8920_I2C_BASE+32u)<<(8u*n);
    CHECK(m->i2c[0].status==0x10u,"PMU response lost completion status");
    m->bus.write32(m,S5L8920_I2C_BASE+12u,0x10u);
    return value;
}

static void test_pmu_adc(s5l8920_t *m) {
    static s5l8920_t before;
    s5l8920_t empty={0};s5l8920_pmu_adc_request_t request,canary;
    memset(&request,0xa5,sizeof request);memcpy(&canary,&request,sizeof canary);
    CHECK(!s5l8920_pmu_adc_request(NULL,&request) && !s5l8920_pmu_adc_request(&empty,&request) &&
          !s5l8920_pmu_adc_request(m,NULL) && !s5l8920_pmu_adc_complete(NULL,0u,0u,0u) &&
          !s5l8920_pmu_adc_complete(&empty,0u,0u,0u) && !s5l8920_pmu_adc_service(NULL,0u) &&
          !s5l8920_pmu_adc_service(&empty,0u) && !memcmp(&request,&canary,sizeof request),"invalid ADC objects/output");
    CHECK(s5l8920_reset(m) && !m->pmu_adc.programmed && !m->pmu_adc.result_valid,"invented ADC initial state");
    uint64_t seq=pmu_request(m,0x74u,0x30u,false,1u,0u);
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_adc_service(m,seq) && !s5l8920_pmu_adc_request(m,&request) &&
          !s5l8920_pmu_adc_complete(m,0u,1u,2u) && !memcmp(&before,m,sizeof before) &&
          !memcmp(&request,&canary,sizeof request),"unprogrammed ADC supplied state");
    CHECK(s5l8920_reset(m),"discard unprogrammed control read");
    uint64_t previous=0u;
    for (unsigned mode=0;mode<4u;mode++) for (unsigned ch=0;ch<16u;ch++) {
        unsigned control=ch|((mode&1u)?0x20u:0u)|((mode&2u)?0x80u:0u);
        seq=pmu_request(m,0x74u,0x30u,true,1u,control);
        CHECK(s5l8920_pmu_adc_service(m,seq) && m->pmu_adc.control==control &&
              !m->pmu_adc.result_valid && !s5l8920_pmu_adc_request(m,&request),"ADC nonbusy programming");
        seq=pmu_request(m,0x74u,0x30u,false,1u,0u);
        CHECK(s5l8920_pmu_adc_service(m,seq) && pmu_read_bytes(m,1u)==control,"ADC control readback");
        seq=pmu_request(m,0x74u,0x30u,true,1u,control|0x10u);
        vic_write(m,0u,PL192_INTENABLE,1u<<19);
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_adc_service(m,seq-1u) && !s5l8920_pmu_adc_service(m,seq+1u) &&
              !s5l8920_pmu_control_service(m,seq) && !s5l8920_pmu_rtc_service(m,seq) &&
              !memcmp(&before,m,sizeof before),"ADC transaction identity/endpoint isolation");
        CHECK(s5l8920_pmu_adc_service(m,seq) && m->cpu.irq_line &&
              s5l8920_pmu_adc_request(m,&request) && request.control==(control|0x10u) &&
              request.sequence>previous,"ADC start/token/IRQ");
        uint64_t token=request.sequence;previous=token;
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_adc_service(m,seq) && !s5l8920_pmu_adc_complete(m,token-1u,0u,0u) &&
              !s5l8920_pmu_adc_complete(m,token+1u,0u,0u) && !memcmp(&before,m,sizeof before),"ADC stale/duplicate changed state");
        for (unsigned poll=0;poll<3u;poll++) {
            CHECK(s5l8920_timebase_clock(m,1000000u),"advance unrelated clock");
            seq=pmu_request(m,0x74u,0x30u,false,1u,0u);
            CHECK(s5l8920_pmu_adc_service(m,seq) && pmu_read_bytes(m,1u)==(control|0x10u) &&
                  !m->pmu_adc.result_valid && s5l8920_pmu_adc_request(m,&request) &&
                  request.sequence==token,"poll/time invented conversion completion");
        }
        seq=pmu_request(m,0x74u,0x30u,true,1u,control|0x10u);
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_adc_service(m,seq) && !memcmp(&before,m,sizeof before),"busy conversion overwritten");
        CHECK(s5l8920_reset(m) && s5l8920_pmu_adc_request(m,&request) && request.sequence==token &&
              !s5l8920_pmu_adc_service(m,seq),"SoC reset lost external conversion or replayed I2C");
        seq=pmu_request(m,0x74u,0x31u,false,2u,0u);
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_adc_service(m,seq) && !memcmp(&before,m,sizeof before),"result supplied before completion");
        uint8_t low=(uint8_t)(0xbcu|ch),high=(uint8_t)(mode*64u+ch*3u);
        arm_cpu_t cpu=m->cpu;s5l8920_bus_failure_t diagnostic=m->bus_failure;
        CHECK(s5l8920_pmu_adc_complete(m,token,low,high) && m->pmu_adc.control==control &&
              m->pmu_adc.result_valid && !memcmp(&cpu,&m->cpu,sizeof cpu) &&
              !memcmp(&diagnostic,&m->bus_failure,sizeof diagnostic),"conversion completion/CPU preservation");
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_adc_complete(m,token,0u,0u) && !s5l8920_pmu_adc_request(m,&request) &&
              !memcmp(&before,m,sizeof before),"duplicate conversion accepted");
        CHECK(s5l8920_pmu_adc_service(m,seq) && pmu_read_bytes(m,2u)==(unsigned)(low|((unsigned)high<<8)),"raw result bits/order");
        CHECK(s5l8920_reset(m) && m->pmu_adc.result_valid,"SoC reset lost external result");
        seq=pmu_request(m,0x74u,0x31u,false,2u,0u);
        CHECK(s5l8920_pmu_adc_service(m,seq) && pmu_read_bytes(m,2u)==(unsigned)(low|((unsigned)high<<8)),"result retention/read consumed latch");
    }
    seq=pmu_request(m,0x74u,0x30u,true,1u,0x93u);
    CHECK(s5l8920_pmu_adc_service(m,seq) && s5l8920_pmu_adc_request(m,&request),"cancellation start");
    uint64_t cancelled=request.sequence;
    seq=pmu_request(m,0x74u,0x30u,true,1u,0x83u);
    CHECK(s5l8920_pmu_adc_service(m,seq) && !s5l8920_pmu_adc_request(m,&request) &&
          !m->pmu_adc.result_valid,"cancellation did not invalidate conversion/result");
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_adc_complete(m,cancelled,0u,0u) && !memcmp(&before,m,sizeof before),"cancelled result accepted");
    seq=pmu_request(m,0x74u,0x30u,true,1u,0xb3u);
    CHECK(s5l8920_pmu_adc_service(m,seq) && s5l8920_pmu_adc_request(m,&request) && request.sequence>cancelled,"cancelled token reused");
    (void)m->bus.read32(m,0u);s5l8920_bus_failure_t diagnostic=m->bus_failure;
    CHECK(diagnostic.reason && s5l8920_pmu_adc_complete(m,request.sequence,255u,255u) &&
          !memcmp(&diagnostic,&m->bus_failure,sizeof diagnostic),"completion repaired latched bus diagnostic");
    CHECK(s5l8920_reset(m),"clear diagnostic for refusal tests");
    const unsigned unsupported[][4]={{0x75u,0x30u,1u,0u},{0x74u,0x2fu,1u,0u},{0x74u,0x32u,1u,0u},
        {0x74u,0x30u,2u,0u},{0x74u,0x31u,1u,0u},{0x74u,0x31u,3u,0u},
        {0x74u,0x31u,2u,1u},{0x74u,0x30u,1u,1u}};
    for (unsigned n=0;n<sizeof unsupported/sizeof unsupported[0];n++) {
        seq=pmu_request(m,unsupported[n][0],unsupported[n][1],unsupported[n][3]!=0u,unsupported[n][2],0x40u);
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_adc_service(m,seq) && !memcmp(&before,m,sizeof before),"unsupported ADC request acknowledged");
        CHECK(s5l8920_reset(m),"cancel unsupported transfer");
    }
    seq=pmu_request(m,0x74u,0x30u,true,1u,0x80u);
    CHECK(s5l8920_pmu_adc_service(m,seq) && !m->pmu_adc.result_valid,"configuration retained stale result observation");
    m->pmu_adc.sequence=UINT64_MAX;
    seq=pmu_request(m,0x74u,0x30u,true,1u,0x90u);memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_adc_service(m,seq) && !memcmp(&before,m,sizeof before),"conversion token wrapped");
    CHECK(s5l8920_reset(m) && m->pmu_adc.sequence==UINT64_MAX,"reset reused exhausted token");
}

static void test_pmu_config(s5l8920_t *m) {
    static s5l8920_t before;s5l8920_t empty={0};
    CHECK(!s5l8920_pmu_config_service(NULL,0u) && !s5l8920_pmu_config_service(&empty,0u),"invalid configuration object");
    CHECK(s5l8920_reset(m) && !m->pmu_config.control_programmed && !m->pmu_config.selectors_programmed,"invented PMU configuration");
    const unsigned missing[][2]={{0x24u,1u},{0x59u,1u},{0x5au,1u},{0x5bu,1u},{0x59u,3u}};
    uint64_t seq;
    for (unsigned n=0;n<5u;n++) {
        seq=pmu_request(m,0x74u,missing[n][0],false,missing[n][1],0u);memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_config_service(m,seq) && !memcmp(&before,m,sizeof before),"unprogrammed configuration read supplied data");
        CHECK(s5l8920_reset(m),"discard missing configuration request");
    }
    seq=pmu_request(m,0x74u,0x24u,true,1u,0x2au);vic_write(m,0u,PL192_INTENABLE,1u<<19);
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_config_service(m,seq-1u) && !s5l8920_pmu_config_service(m,seq+1u) &&
          !s5l8920_pmu_adc_service(m,seq) && !s5l8920_pmu_rtc_service(m,seq) &&
          !memcmp(&before,m,sizeof before),"configuration transaction identity/isolation");
    CHECK(s5l8920_pmu_config_service(m,seq) && m->cpu.irq_line && m->pmu_config.control_programmed &&
          m->pmu_config.control==0x2au,"observed configuration programming/IRQ");
    before.cpu.irq_line=m->cpu.irq_line;
    CHECK(!memcmp(&before.cpu,&m->cpu,sizeof m->cpu),"configuration service changed CPU");
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_config_service(m,seq) && !memcmp(&before,m,sizeof before),"configuration replay");
    seq=pmu_request(m,0x74u,0x24u,false,1u,0u);
    CHECK(s5l8920_pmu_config_service(m,seq) && pmu_read_bytes(m,1u)==0x2au,"configuration byte readback");
    for (unsigned value=0;value<256u;value++) if (value!=0x2au) {
        seq=pmu_request(m,0x74u,0x24u,true,1u,value);memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_config_service(m,seq) && !memcmp(&before,m,sizeof before),"unimplemented configuration/power transition acknowledged");
        CHECK(s5l8920_reset(m) && m->pmu_config.control==0x2au,"reset lost programmed control");
    }
    const unsigned order[]={0u,2u,1u},initial[]={0x82u,0x24u,8u};
    for (unsigned n=0;n<3u;n++) {
        unsigned index=order[n];seq=pmu_request(m,0x74u,0x59u+index,true,1u,initial[index]);
        CHECK(s5l8920_pmu_config_service(m,seq),"individual selector programming");
        seq=pmu_request(m,0x74u,0x59u,false,3u,0u);
        if (n<2u) {
            memcpy(&before,m,sizeof before);
            CHECK(!s5l8920_pmu_config_service(m,seq) && !memcmp(&before,m,sizeof before),"partial selector image invented remaining bytes");
            CHECK(s5l8920_reset(m),"reset partial selector request");
        } else CHECK(s5l8920_pmu_config_service(m,seq) && pmu_read_bytes(m,3u)==0x082482u,"complete packed selector image");
    }
    for (unsigned index=0;index<8u;index++) for (unsigned field=0;field<8u;field++) {
        uint32_t value=(0xa55aa5u&~(7u<<(index*3u)))|(field<<(index*3u));
        seq=pmu_request(m,0x74u,0x59u,true,3u,value);
        CHECK(s5l8920_pmu_config_service(m,seq) && m->pmu_config.selectors_programmed==7u,"packed selector write");
        for (unsigned byte=0;byte<3u;byte++) {
            seq=pmu_request(m,0x74u,0x59u+byte,false,1u,0u);
            CHECK(s5l8920_pmu_config_service(m,seq) && pmu_read_bytes(m,1u)==((value>>(byte*8u))&255u),"packed field byte order/neighbors");
        }
        CHECK(s5l8920_reset(m),"selector reset");
        seq=pmu_request(m,0x74u,0x59u,false,3u,0u);
        CHECK(s5l8920_pmu_config_service(m,seq) && pmu_read_bytes(m,3u)==value,"reset/read consumed selector programming");
    }
    const unsigned unsupported[][3]={{0x75u,0x24u,1u},{0x75u,0x59u,3u},{0x74u,0x23u,1u},
        {0x74u,0x25u,1u},{0x74u,0x58u,1u},{0x74u,0x5cu,1u},{0x74u,0x24u,2u},
        {0x74u,0x59u,2u},{0x74u,0x59u,4u},{0x74u,0x5au,3u},{0x74u,0x5bu,3u}};
    for (unsigned n=0;n<sizeof unsupported/sizeof unsupported[0];n++) for (unsigned write=0;write<2u;write++) {
        seq=pmu_request(m,unsupported[n][0],unsupported[n][1],write!=0u,unsupported[n][2],0x2au);
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_config_service(m,seq) && !memcmp(&before,m,sizeof before),"unsupported configuration request changed state");
        CHECK(s5l8920_reset(m),"cancel unsupported configuration transfer");
    }
    seq=pmu_request(m,0x74u,0x59u,true,3u,0xffffffu);
    memcpy(&before.pmu_config,&m->pmu_config,sizeof m->pmu_config);
    CHECK(s5l8920_reset(m) && !s5l8920_pmu_config_service(m,seq) &&
          !memcmp(&before.pmu_config,&m->pmu_config,sizeof m->pmu_config),"reset completed cancelled selector write");
    seq=pmu_request(m,0x74u,0x59u,true,3u,0xffffffu);
    (void)m->bus.read32(m,0u);s5l8920_bus_failure_t diagnostic=m->bus_failure;
    CHECK(diagnostic.reason && s5l8920_pmu_config_service(m,seq) &&
          !memcmp(&diagnostic,&m->bus_failure,sizeof diagnostic),"configuration service repaired bus failure");
    CHECK(s5l8920_reset(m),"final configuration reset");
}

static bool test_pmu_boot_state(s5l8920_t *m) {
    static s5l8920_t before;s5l8920_t empty={0};
    CHECK(s5l8920_reset(m),"clear diagnostic before boot-state tests");
    CHECK(!s5l8920_pmu_boot_state_configure(NULL,0u) && !s5l8920_pmu_boot_state_configure(&empty,0u) &&
          !s5l8920_pmu_boot_state_service(NULL,1u) && !s5l8920_pmu_boot_state_service(&empty,1u),"invalid boot-state board");
    uint64_t seq=pmu_request(m,0x74u,0x6fu,false,1u,0u);memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_boot_state_service(m,seq) && !memcmp(&before,m,sizeof before),"invented boot-state reset byte");
    CHECK(s5l8920_reset(m),"cancel unknown boot-state read");
    seq=pmu_request(m,0x74u,0x6fu,true,1u,0x90u);
    CHECK(s5l8920_pmu_boot_state_service(m,seq) && m->pmu_boot_programmed && !m->pmu_boot_configured,"full guest write required invented initial state");
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_boot_state_configure(m,0x90u) && !memcmp(&before,m,sizeof before),"late initial state replaced guest programming");
    seq=pmu_request(m,0x74u,0x6fu,false,1u,0u);
    CHECK(s5l8920_pmu_boot_state_service(m,seq) && pmu_read_bytes(m,1u)==0x90u,"guest-written boot state unreadable");
    s5l8920_free(m);
    if (!s5l8920_init(m)) { CHECK(false,"fresh boot-state board");return false; }
    CHECK(s5l8920_pmu_boot_state_configure(m,0x5au),"explicit boot-state initialization");
    memcpy(&before,m,sizeof before);
    CHECK(s5l8920_pmu_boot_state_configure(m,0x5au) && !s5l8920_pmu_boot_state_configure(m,0u) &&
          !memcmp(&before,m,sizeof before),"initial boot-state replacement");
    for(unsigned value=0;value<256u;value++) {
        seq=pmu_request(m,0x74u,0x6fu,true,1u,value);memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_boot_state_service(m,seq-1u) && !s5l8920_pmu_boot_state_service(m,seq+1u) &&
              !s5l8920_pmu_control_service(m,seq) && !s5l8920_pmu_events_service(m,seq) &&
              !s5l8920_pmu_config_service(m,seq) && !memcmp(&before,m,sizeof before),"boot-state request identity/isolation");
        CHECK(s5l8920_pmu_boot_state_service(m,seq) && m->pmu_boot_value==value,"boot-state byte write");
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_boot_state_service(m,seq) && !memcmp(&before,m,sizeof before),"replayed boot-state write");
        CHECK(s5l8920_pmu_boot_state_configure(m,0x5au) && m->pmu_boot_value==value,"configuration reloaded guest byte");
        for(unsigned repeat=0;repeat<2u;repeat++) {
            seq=pmu_request(m,0x74u,0x6fu,false,1u,0u);arm_cpu_t cpu=m->cpu;
            CHECK(s5l8920_pmu_boot_state_service(m,seq) && pmu_read_bytes(m,1u)==value && m->pmu_boot_value==value,"boot-state read consumed or changed byte");
            cpu.irq_line=m->cpu.irq_line;
            CHECK(!memcmp(&cpu,&m->cpu,sizeof cpu),"boot-state service changed CPU registers");
        }
    }
    const unsigned bad[][3]={{0x75u,0x6fu,1u},{0x74u,0x6fu,2u},{0x74u,0x6fu,4u},
        {0x74u,0x60u,1u},{0x74u,0x61u,1u},{0x74u,0x64u,4u},{0x74u,0x6du,1u},
        {0x74u,0x6eu,1u},{0x74u,0x70u,1u},{0x74u,0x73u,1u}};
    for(unsigned n=0;n<sizeof bad/sizeof bad[0];n++) for(unsigned wr=0;wr<2u;wr++) {
        seq=pmu_request(m,bad[n][0],bad[n][1],wr!=0u,bad[n][2],0u);memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_boot_state_service(m,seq) && !memcmp(&before,m,sizeof before),"boot-state unsupported transfer mutation");
        CHECK(s5l8920_reset(m),"cancel unsupported boot-state request");
    }
    for(unsigned bus=1u;bus<3u;bus++) {
        uint32_t base=S5L8920_I2C_BASE+bus*S5L8920_I2C_STRIDE;
        const unsigned offsets[]={8u,12u,0u,16u,20u,24u,32u,36u};
        const unsigned values[]={0x30u,0x37u,0x74u,0x6fu,0u,1u,0x80u,5u};
        for(unsigned n=0;n<8u;n++) m->bus.write32(m,base+offsets[n],values[n]);
        s5l8920_i2c_request_t request;CHECK(s5l8920_i2c_request(m,bus,&request),"other bus boot-state request");
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_boot_state_service(m,request.sequence) && !memcmp(&before,m,sizeof before),"boot-state service crossed controller");
        CHECK(s5l8920_reset(m),"cancel other controller boot-state request");
    }
    seq=pmu_request(m,0x74u,0x6fu,true,1u,0u);
    CHECK(s5l8920_reset(m) && !s5l8920_pmu_boot_state_service(m,seq) && m->pmu_boot_value==255u &&
          m->pmu_boot_programmed && m->pmu_boot_configured && m->pmu_boot_initial==0x5au,"reset committed pending write or lost external byte");
    seq=pmu_request(m,0x74u,0x6fu,false,1u,0u);(void)m->bus.read32(m,0u);
    s5l8920_bus_failure_t failure=m->bus_failure;
    CHECK(failure.reason && s5l8920_pmu_boot_state_service(m,seq) && !memcmp(&failure,&m->bus_failure,sizeof failure),"boot-state service repaired diagnostic");
    return true;
}

static bool test_pmu_events(s5l8920_t *m) {
    static s5l8920_t before;s5l8920_t empty={0};
    s5l8920_free(m);
    if (!s5l8920_init(m)) { CHECK(false,"fresh PMU event board");return false; }
    CHECK(!s5l8920_pmu_events_configure(NULL,0u) && !s5l8920_pmu_events_configure(&empty,0u) &&
          !s5l8920_pmu_events_raise(m,1u) && !s5l8920_pmu_status_input(&empty,0u) &&
          !s5l8920_pmu_events_service(&empty,1u),"unconfigured/freed event APIs");
    const unsigned cold[][2]={{1u,4u},{2u,1u},{5u,4u},{9u,4u},{10u,1u}};
    uint64_t seq;
    for (unsigned n=0;n<5u;n++) {
        seq=pmu_request(m,0x74u,cold[n][0],false,cold[n][1],0u);memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_events_service(m,seq) && !memcmp(&before,m,sizeof before),"cold event/status/mask supplied data");
        CHECK(s5l8920_reset(m),"cancel cold read");
    }
    CHECK(s5l8920_pmu_events_configure(m,0x11223344u) && !m->gpio[S5L8920_PMU_IRQ_PIN].input_valid,"unknown masks drove pin");
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_events_configure(m,0u) && !memcmp(&before,m,sizeof before),"initial events replaced");
    seq=pmu_request(m,0x74u,2u,false,1u,0u);memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_events_service(m,seq-1u) && !s5l8920_pmu_events_service(m,seq+1u) &&
          !s5l8920_pmu_adc_service(m,seq) && !s5l8920_pmu_config_service(m,seq) &&
          !memcmp(&before,m,sizeof before),"event transaction identity/isolation");
    CHECK(s5l8920_pmu_events_service(m,seq) && pmu_read_bytes(m,1u)==0x33u &&
          m->pmu_events.pending==0x11220044u,"early byte02 consumed unrelated event bytes");
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_events_service(m,seq) && !memcmp(&before,m,sizeof before),"event replay");
    CHECK(s5l8920_pmu_events_configure(m,0x11223344u) && m->pmu_events.pending==0x11220044u,"configure reloaded consumed event");
    seq=pmu_request(m,0x74u,1u,false,4u,0u);
    CHECK(s5l8920_pmu_events_service(m,seq) && pmu_read_result(m)==0x11220044u && !m->pmu_events.pending,"block read-clear");
    seq=pmu_request(m,0x74u,1u,false,4u,0u);
    CHECK(s5l8920_pmu_events_service(m,seq) && pmu_read_result(m)==0u,"consumed events returned twice");
    for (unsigned n=0;n<4u;n++) {
        seq=pmu_request(m,0x74u,9u+n,true,1u,255u);
        CHECK(s5l8920_pmu_events_service(m,seq),"individual mask programming");
        seq=pmu_request(m,0x74u,9u,false,4u,0u);
        if (n<3u) {
            memcpy(&before,m,sizeof before);
            CHECK(!s5l8920_pmu_events_service(m,seq) && !memcmp(&before,m,sizeof before) &&
                  !m->gpio[S5L8920_PMU_IRQ_PIN].input_valid,"partial masks inferred remaining bytes/pin");
            CHECK(s5l8920_reset(m),"cancel partial mask read");
        } else CHECK(s5l8920_pmu_events_service(m,seq) && pmu_read_result(m)==UINT32_MAX &&
                     m->gpio[S5L8920_PMU_IRQ_PIN].input_high,"complete masked output");
    }
    const uint32_t pin_address=S5L8920_GPIO_BASE+S5L8920_PMU_IRQ_PIN*4u;
    const uint32_t pending_address=S5L8920_GPIO_BASE+0x810u,pin_bit=UINT32_C(1)<<29;
    m->bus.write32(m,pin_address,0x206u);vic_write(m,2u,PL192_INTENABLE,UINT32_C(1)<<30);
    for (unsigned bit=0;bit<32u;bit++) {
        uint32_t event=UINT32_C(1)<<bit;
        seq=pmu_request(m,0x74u,9u,true,4u,UINT32_MAX);CHECK(s5l8920_pmu_events_service(m,seq),"mask all events");
        CHECK(s5l8920_pmu_events_raise(m,event) && s5l8920_pmu_events_raise(m,event) &&
              m->pmu_events.pending==event && m->gpio[S5L8920_PMU_IRQ_PIN].input_high && !m->cpu.irq_line,"masked events lost/coalescing");
        unsigned byte=bit/8u;seq=pmu_request(m,0x74u,9u+byte,true,1u,255u^(1u<<(bit%8u)));
        CHECK(s5l8920_pmu_events_service(m,seq) && m->pmu_events.masks==~event &&
              !m->gpio[S5L8920_PMU_IRQ_PIN].input_high && m->cpu.irq_line,"unmasked event/GPIO157/source94");
        m->bus.write32(m,pending_address,pin_bit);
        CHECK(m->cpu.irq_line,"active PMU level failed to relatch GPIO acknowledgement");
        seq=pmu_request(m,0x74u,1u,false,4u,0u);arm_cpu_t cpu=m->cpu;
        CHECK(s5l8920_pmu_events_service(m,seq) && pmu_read_result(m)==event &&
              !m->pmu_events.pending && m->gpio[S5L8920_PMU_IRQ_PIN].input_high && m->cpu.irq_line,"PMU clear prematurely acknowledged GPIO");
        CHECK(!memcmp(&cpu,&m->cpu,sizeof cpu),"event service changed CPU registers");
        m->bus.write32(m,pending_address,pin_bit);CHECK(!m->cpu.irq_line,"released PMU level retained GPIO IRQ");
    }
    CHECK(s5l8920_pmu_status_input(m,0xa55a8008u),"explicit live status");
    for (unsigned n=0;n<3u;n++) {
        seq=pmu_request(m,0x74u,5u,false,4u,0u);
        CHECK(s5l8920_pmu_events_service(m,seq) && pmu_read_result(m)==0xa55a8008u && !m->pmu_events.pending,"status read created/consumed events");
    }
    CHECK(s5l8920_pmu_status_input(m,0x12345678u),"replace live status");
    seq=pmu_request(m,0x74u,9u,true,4u,0xffffdfffu);CHECK(s5l8920_pmu_events_service(m,seq),"original ADC mask");
    seq=pmu_request(m,0x74u,0x30u,true,1u,0x90u);s5l8920_pmu_adc_request_t adc;
    CHECK(s5l8920_pmu_adc_service(m,seq) && s5l8920_pmu_adc_request(m,&adc) &&
          s5l8920_timebase_clock(m,1000000u) && !m->pmu_events.pending,"ADC start/time invented completion event");
    CHECK(s5l8920_pmu_adc_complete(m,adc.sequence,3u,255u) && m->pmu_events.pending==0x2000u &&
          m->cpu.irq_line && !m->gpio[S5L8920_PMU_IRQ_PIN].input_high,"explicit ADC completion event");
    seq=pmu_request(m,0x74u,1u,false,4u,0u);
    CHECK(s5l8920_reset(m) && !s5l8920_pmu_events_service(m,seq) && m->pmu_events.pending==0x2000u &&
          m->pmu_events.status==0x12345678u && m->pmu_events.masks==0xffffdfffu,"reset consumed external PMU state");
    m->bus.write32(m,pin_address,0x206u);vic_write(m,2u,PL192_INTENABLE,UINT32_C(1)<<30);
    CHECK(m->cpu.irq_line,"retained PMU level after SoC reset");
    seq=pmu_request(m,0x74u,2u,false,1u,0u);
    CHECK(s5l8920_pmu_events_service(m,seq) && pmu_read_bytes(m,1u)==0x20u,"ADC event byte");
    m->bus.write32(m,pending_address,pin_bit);CHECK(!m->cpu.irq_line,"ADC IRQ release");
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_adc_complete(m,adc.sequence,0u,0u) && !memcmp(&before,m,sizeof before),"replayed ADC raised another event");
    const unsigned bad_requests[][3]={{0x75u,1u,4u},{0x74u,1u,1u},{0x74u,2u,4u},{0x74u,3u,1u},
        {0x74u,5u,1u},{0x74u,6u,4u},{0x74u,9u,2u},{0x74u,10u,4u},{0x74u,13u,1u}};
    for (unsigned n=0;n<sizeof bad_requests/sizeof bad_requests[0];n++) for (unsigned wr=0;wr<2u;wr++) {
        seq=pmu_request(m,bad_requests[n][0],bad_requests[n][1],wr!=0u,bad_requests[n][2],0u);memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_events_service(m,seq) && !memcmp(&before,m,sizeof before),"unsupported event transfer mutation");
        CHECK(s5l8920_reset(m),"cancel unsupported event transfer");
    }
    const unsigned read_only[][2]={{1u,4u},{2u,1u},{5u,4u}};
    for (unsigned n=0;n<3u;n++) {
        seq=pmu_request(m,0x74u,read_only[n][0],true,read_only[n][1],UINT32_MAX);memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_events_service(m,seq) && !memcmp(&before,m,sizeof before),"invented event/status write ACK");
        CHECK(s5l8920_reset(m),"cancel read-only register write");
    }
    CHECK(s5l8920_reset(m) && s5l8920_pmu_events_raise(m,0x2000u),"diagnostic setup");
    seq=pmu_request(m,0x74u,1u,false,4u,0u);(void)m->bus.read32(m,0u);
    s5l8920_bus_failure_t diagnostic=m->bus_failure;
    CHECK(diagnostic.reason && s5l8920_pmu_events_service(m,seq) &&
          !memcmp(&diagnostic,&m->bus_failure,sizeof diagnostic),"event service repaired bus failure");
    return true;
}

static void test_pmu_control(s5l8920_t *m) {
    s5l8920_t empty={0};static s5l8920_t before;
    CHECK(!s5l8920_pmu_control_configure(NULL,0u) && !s5l8920_pmu_control_configure(&empty,0u) &&
          !s5l8920_pmu_control_service(NULL,0u) && !s5l8920_pmu_control_service(&empty,0u),"invalid PMU control object");
    CHECK(s5l8920_reset(m) && !m->pmu_control_configured,"unconfigured PMU control reset");
    uint64_t seq=pmu_request(m,0x74u,0x0du,false,1u,0u);
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pmu_control_configure(m,0xa5u) && !s5l8920_pmu_control_service(m,seq) &&
          !memcmp(&before,m,sizeof before),"missing input or busy configure changed state");
    uint8_t sample=0x53u;
    CHECK(s5l8920_i2c_complete(m,0u,seq,true,&sample,1u) &&
          !s5l8920_pmu_control_configure(m,0xa5u),"pending status allowed first configuration");
    m->bus.write32(m,S5L8920_I2C_BASE+12u,0x10u);
    CHECK(!s5l8920_pmu_control_configure(m,0xa5u),"unread data allowed first configuration");
    CHECK(m->bus.read32(m,S5L8920_I2C_BASE+32u)==sample,"explicit response consumed");
    (void)m->bus.read32(m,0u);
    s5l8920_bus_failure_t diagnostic=m->bus_failure;arm_cpu_t cpu=m->cpu;
    CHECK(s5l8920_pmu_control_configure(m,0xa5u) && m->pmu_control_value==0xa5u &&
          !memcmp(&cpu,&m->cpu,sizeof cpu) && !memcmp(&diagnostic,&m->bus_failure,sizeof diagnostic),
          "configure changed CPU/diagnostic or supplied wrong byte");
    s5l8920_clear_bus_failure(m);
    for (unsigned n=0;n<3u;n++) {
        uint8_t value=n==2u?0xa5u:0xb5u;
        seq=pmu_request(m,0x74u,0x0du,true,1u,value);
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_control_service(m,seq-1u) && !s5l8920_pmu_control_service(m,seq+1u) &&
              s5l8920_pmu_control_configure(m,0xa5u) && !s5l8920_pmu_control_configure(m,0xb5u) &&
              !memcmp(&before,m,sizeof before),"stale sequence/reconfiguration changed pending control write");
        vic_write(m,0u,PL192_INTENABLE,1u<<19);
        cpu=m->cpu;cpu.irq_line=true;
        CHECK(s5l8920_pmu_control_service(m,seq) && m->pmu_control_value==value &&
              m->i2c[0].status==0x10u && !memcmp(&cpu,&m->cpu,sizeof cpu),"control write/IRQ/CPU state");
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_control_service(m,seq) && !memcmp(&before,m,sizeof before),"duplicate service mutated state");
        m->bus.write32(m,S5L8920_I2C_BASE+12u,0x10u);
        CHECK(!m->cpu.irq_line,"control write IRQ acknowledgement");
        seq=pmu_request(m,0x74u,0x0du,false,1u,0u);
        CHECK(!s5l8920_pmu_rtc_service(m,seq) && s5l8920_pmu_control_service(m,seq),"endpoint isolation/readback");
        CHECK(m->bus.read8(m,S5L8920_I2C_BASE+32u)==value && m->cpu.irq_line,"exact one-byte readback/status held");
        m->bus.write32(m,S5L8920_I2C_BASE+12u,0x10u);
        CHECK(s5l8920_reset(m) && m->pmu_control_value==value && m->pmu_control_initial==0xa5u &&
              m->pmu_control_configured && !s5l8920_pmu_control_service(m,seq),"SoC reset changed external PMU or replayed request");
    }
    for (unsigned bit=0;bit<8u;bit++) if (bit!=4u) {
        seq=pmu_request(m,0x74u,0x0du,true,1u,m->pmu_control_value^(1u<<bit));
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_control_service(m,seq) && !memcmp(&before,m,sizeof before),"unknown/power-transition bit acknowledged");
        CHECK(s5l8920_reset(m),"discard unsupported request");
    }
    const unsigned requests[][3]={{0x74u,2u,1u},{0x74u,0x30u,1u},{0x74u,0x4cu,4u},
        {0x73u,0x0du,1u},{0x74u,0x0du,2u},{0x74u,0x0cu,2u}};
    for (unsigned n=0;n<sizeof requests/sizeof *requests;n++) for (unsigned write=0;write<2u;write++) {
        seq=pmu_request(m,requests[n][0],requests[n][1],write!=0u,requests[n][2],0xa5a5a5a5u);
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_pmu_control_service(m,seq) && !memcmp(&before,m,sizeof before),"unsupported target/register/length acknowledged");
        CHECK(s5l8920_reset(m),"discard unsupported transfer");
    }
    seq=pmu_request(m,0x74u,0x0du,true,1u,0xb5u);
    (void)m->bus.read32(m,0u);diagnostic=m->bus_failure;
    CHECK(s5l8920_pmu_control_service(m,seq) && m->pmu_control_value==0xb5u &&
          !memcmp(&diagnostic,&m->bus_failure,sizeof diagnostic),"service cleared diagnostic");
    CHECK(s5l8920_reset(m) && m->pmu_control_value==0xb5u,"final control reset");
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

static bool test_gpio_inactive_readback(s5l8920_t *m) {
    static s5l8920_t empty,before;
    CHECK(s5l8920_reset(m),"inactive GPIO reset");
    CHECK(!s5l8920_gpio_inactive_readback(NULL,0u,false) &&
          !s5l8920_gpio_inactive_readback(&empty,0u,false) &&
          !s5l8920_gpio_inactive_readback(m,368u,false) &&
          !s5l8920_gpio_inactive_readback(m,UINT32_MAX,false) &&
          !s5l8920_gpio_inactive_readback(m,0u,false),"inactive readback accepted invalid pin/state");
    for (unsigned pin=0;pin<368u;pin++) for (unsigned high=0;high<2u;high++) {
        uint32_t address=S5L8920_GPIO_BASE+4u*pin;
        uint32_t control=0x1eu|((pin%4u)<<10)|((pin%3u)<<7)|(high^1u);
        m->bus.write32(m,address,control);
        CHECK(s5l8920_gpio_input(m,pin,high==0u),"inactive opposing live sample");
        (void)m->bus.read32(m,address);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && !m->gpio[pin].inactive_valid,
              "inactive data inferred from sample or guest bit");
        memcpy(&before,m,sizeof before);
        before.gpio[pin].inactive_valid=true;before.gpio[pin].inactive_high=high!=0u;
        CHECK(s5l8920_gpio_inactive_readback(m,pin,high!=0u) && !memcmp(&before,m,sizeof before),
              "inactive event changed unrelated state or latched diagnostic");
        s5l8920_clear_bus_failure(m);
        for (unsigned n=0;n<3u;n++) CHECK(m->bus.read32(m,address)==((control&~1u)|high) &&
            !m->bus_failure.reason && m->gpio[pin].control==control,"inactive observation read consumed data or changed control");
        m->bus.write32(m,address,control|0x1000u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && m->gpio[pin].inactive_valid &&
              m->gpio[pin].inactive_high==(high!=0u) && m->gpio[pin].control==control,"refused write invalidated observation");
        s5l8920_clear_bus_failure(m);m->bus.write32(m,address,control);
        CHECK(!m->gpio[pin].inactive_valid && !m->gpio[pin].inactive_high,"identical write retained stale observation");
        CHECK(s5l8920_gpio_inactive_readback(m,pin,high!=0u) && s5l8920_gpio_input(m,pin,high==0u) &&
              !m->gpio[pin].inactive_valid,"live event retained inactive observation");
        CHECK(s5l8920_gpio_inactive_readback(m,pin,high!=0u),"resupply inactive observation");
        m->bus.write32(m,address,(control&~0xeu)|0x200u);
        memcpy(&before,m,sizeof before);
        CHECK(!s5l8920_gpio_inactive_readback(m,pin,high!=0u) && !memcmp(&before,m,sizeof before) &&
              m->bus.read32(m,address)==(((control&~0xfu)|0x200u)|(high^1u)),"inactive event changed active sample behavior");
        m->bus.write32(m,address,control);
    }
    const uint32_t address=S5L8920_GPIO_BASE+60u;
    CHECK(s5l8920_gpio_inactive_readback(m,15u,true),"width fixture observation");
    for (unsigned offset=0;offset<4u;offset++) for (unsigned kind=0;kind<6u;kind++) {
        s5l8920_clear_bus_failure(m);uint32_t at=address+offset;
        if (kind==0u) (void)m->bus.read8(m,at);
        else if (kind==1u) (void)m->bus.read16(m,at);
        else if (kind==2u) (void)m->bus.read32(m,at);
        else if (kind==3u) m->bus.write8(m,at,0u);
        else if (kind==4u) m->bus.write16(m,at,0u);
        else m->bus.write32(m,at,0u);
        CHECK(m->bus_failure.reason==(kind==2u && !offset?S5L8920_BUS_OK:
              (kind==5u && !offset?S5L8920_BUS_REGISTER_REFUSED:S5L8920_BUS_ACCESS_UNIMPLEMENTED)) &&
              m->gpio[15].inactive_valid && m->gpio[15].inactive_high,"inactive access width/alignment changed observation");
    }
    for (unsigned thumb=0;thumb<2u;thumb++) {
        CHECK(s5l8920_reset(m),"inactive CPU reset");m->bus.write32(m,address,0xd1fu);
        put(m,0x100u,thumb?0x0000f8d1u:0xe5910000u);
        m->cpu.r[0]=0xa5a5u;m->cpu.r[1]=address;m->cpu.r[15]=S5L8920_RAM_BASE+0x100u;
        m->cpu.cpsr=ARM_MODE_SVC|ARM_CPSR_I|ARM_CPSR_F|ARM_CPSR_C|(thumb?ARM_CPSR_T:0u);
        uint32_t pc=m->cpu.r[15],flags=m->cpu.cpsr;
        CHECK(arm_step(&m->cpu)==ARM_HALT && !m->cpu.cycles && m->cpu.r[15]==pc && m->cpu.r[0]==0xa5a5u &&
              m->cpu.cpsr==flags && m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"missing inactive CPU load retired");
        CHECK(s5l8920_gpio_inactive_readback(m,15u,false) && m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,
              "inactive event cleared CPU diagnostic");
        s5l8920_clear_bus_failure(m);
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==1u && m->cpu.r[15]==pc+4u && m->cpu.r[0]==0xd1eu &&
              m->cpu.cpsr==flags,"inactive CPU retry failed");
    }
    CHECK(s5l8920_reset(m),"inactive final reset");
    for (unsigned pin=0;pin<368u;pin++) CHECK(!m->gpio[pin].inactive_valid && !m->gpio[pin].inactive_high &&
        !m->gpio[pin].programmed,"reset retained inactive observation");
    return true;
}

static bool test_powerid(s5l8920_t *m) {
    static s5l8920_t empty, before;
    const uint32_t initial=0xa5c369fcu;
    CHECK(!s5l8920_powerid_configure(NULL,0u) && !s5l8920_powerid_configure(&empty,0u),"invalid POWERID object");
    CHECK(s5l8920_reset(m),"POWERID initial reset");
    (void)m->bus.read32(m,S5L8920_POWERID);
    CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && !m->powerid.configured,"invented POWERID initial read");
    s5l8920_clear_bus_failure(m);m->bus.write32(m,S5L8920_POWERID,0u);
    CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && !m->powerid.configured,"unconfigured POWERID write");
    memcpy(&before,m,sizeof before);
    CHECK(s5l8920_powerid_configure(m,initial) && m->powerid.configured && m->powerid.value==initial &&
          !memcmp(&before.cpu,&m->cpu,sizeof m->cpu) &&
          !memcmp(&before.bus_failure,&m->bus_failure,sizeof m->bus_failure),"POWERID configuration changed CPU/diagnostic");
    m->bus.write32(m,S5L8920_POWERID,initial^1u);
    CHECK(m->powerid.value==initial,"latched failure allowed POWERID mutation");
    s5l8920_clear_bus_failure(m);
    for (unsigned bit=0;bit<32u;bit++) {
        uint32_t value=initial^(1u<<bit);
        s5l8920_clear_bus_failure(m);m->bus.write32(m,S5L8920_POWERID,initial);
        m->bus.write32(m,S5L8920_POWERID,value);
        if ((1u<<bit)&0xfcu) CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
            m->powerid.value==initial,"unknown POWERID bit changed");
        else CHECK(!m->bus_failure.reason && m->bus.read32(m,S5L8920_POWERID)==value,"POWERID writable field");
    }
    s5l8920_clear_bus_failure(m);
    for (unsigned value=0;value<256u;value++) for (unsigned byte=1;byte<4u;byte++) {
        uint32_t word=(initial&~(255u<<(8u*byte)))|((uint32_t)value<<(8u*byte))|(value&3u);
        m->bus.write32(m,S5L8920_POWERID,word);
        CHECK(!m->bus_failure.reason && m->bus.read32(m,S5L8920_POWERID)==word,"POWERID bytes/flags or preserved unknown bits");
    }
    memcpy(&before,m,sizeof before);
    CHECK(s5l8920_powerid_configure(m,initial) && !s5l8920_powerid_configure(m,initial^1u) &&
          !memcmp(&before,m,sizeof before),"POWERID reconfiguration changed guest state");
    for (unsigned offset=0;offset<4u;offset++) for (unsigned kind=0;kind<6u;kind++) {
        s5l8920_clear_bus_failure(m);
        uint32_t address=S5L8920_POWERID+offset,value=m->powerid.value;
        if (kind==0u) (void)m->bus.read8(m,address);
        else if (kind==1u) (void)m->bus.read16(m,address);
        else if (kind==2u) (void)m->bus.read32(m,address);
        else if (kind==3u) m->bus.write8(m,address,0u);
        else if (kind==4u) m->bus.write16(m,address,0u);
        else m->bus.write32(m,address,value);
        CHECK(m->bus_failure.reason==((kind%3u==2u && !offset)?S5L8920_BUS_OK:S5L8920_BUS_ACCESS_UNIMPLEMENTED) &&
              m->powerid.value==value,"POWERID width/alignment mutation");
    }
    for (unsigned side=0;side<2u;side++) {
        s5l8920_clear_bus_failure(m);m->bus.write32(m,side?S5L8920_POWERID+4u:S5L8920_POWERID-4u,0u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"POWERID invaded adjacent register");
    }
    CHECK(!m->bus.host_ram(m,S5L8920_POWERID,4u) && !m->bus.host_ram_write(m,S5L8920_POWERID,4u),"POWERID exposed as RAM");
    CHECK(s5l8920_reset(m) && m->powerid.configured && m->powerid.value==initial &&
          m->powerid.initial==initial && m->bus.read32(m,S5L8920_POWERID)==initial,"POWERID reset lost input or retained writes");
    for (unsigned thumb=0;thumb<2u;thumb++) {
        CHECK(s5l8920_reset(m),"POWERID CPU reset");
        put(m,0x100u,thumb?0x0000f8c1u:0xe5810000u);
        put(m,0x104u,thumb?0x0000f8d1u:0xe5910000u);
        m->cpu.r[0]=initial^4u;m->cpu.r[1]=S5L8920_POWERID;m->cpu.r[15]=S5L8920_RAM_BASE+0x100u;
        m->cpu.cpsr=ARM_MODE_SVC|ARM_CPSR_I|ARM_CPSR_F|ARM_CPSR_C|(thumb?ARM_CPSR_T:0u);
        uint32_t flags=m->cpu.cpsr,pc=m->cpu.r[15];
        CHECK(arm_step(&m->cpu)==ARM_HALT && !m->cpu.cycles && m->cpu.r[15]==pc &&
              m->cpu.r[0]==(initial^4u) && m->cpu.cpsr==flags && m->powerid.value==initial &&
              m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"unknown POWERID CPU store retired");
        s5l8920_clear_bus_failure(m);m->cpu.r[0]=0x041234fdu;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==1u && m->cpu.r[15]==pc+4u &&
              m->cpu.cpsr==flags && m->powerid.value==0x041234fdu,"POWERID CPU store retry");
        m->cpu.r[0]=0u;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==2u && m->cpu.r[15]==pc+8u &&
              m->cpu.r[0]==0x041234fdu && m->cpu.cpsr==flags,"POWERID CPU readback");
    }
    CHECK(s5l8920_reset(m) && m->powerid.value==initial,"POWERID final reset");
    return true;
}

static void test_miu(s5l8920_t *m) {
    static s5l8920_t empty, before;
    const uint32_t initial=0xa5c369fcu, control=S5L8920_MIU_CONTROL;
    CHECK(!s5l8920_miu_configure(NULL,0u) && !s5l8920_miu_configure(&empty,0u),"invalid MIU object");
    CHECK(s5l8920_reset(m),"MIU initial reset");
    (void)m->bus.read32(m,control);
    CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && !m->miu.configured,"invented MIU initial read");
    s5l8920_clear_bus_failure(m);m->bus.write32(m,control,2u);
    CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && !m->ram_boot_window,"unconfigured MIU write");
    memcpy(&before,m,sizeof before);
    CHECK(s5l8920_miu_configure(m,initial) && m->miu.value==initial &&
          !memcmp(&before.cpu,&m->cpu,sizeof m->cpu) &&
          !memcmp(&before.bus_failure,&m->bus_failure,sizeof m->bus_failure),"MIU input changed CPU/diagnostic");
    m->bus.write32(m,control,initial|2u);
    CHECK(m->miu.value==initial && !m->ram_boot_window,"latched fault allowed remap");
    for (unsigned bit=2u;bit<32u;bit++) {
        s5l8920_clear_bus_failure(m);uint32_t generation=m->cpu.tlb_gen;
        m->bus.write32(m,control,(initial^(1u<<bit))|2u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && m->miu.value==initial &&
              !m->ram_boot_window && m->cpu.tlb_gen==generation,"unknown MIU bit changed state");
    }
    for (unsigned mode=0;mode<4u;mode+=3u) {
        s5l8920_clear_bus_failure(m);m->bus.write32(m,control,initial|mode);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && m->miu.value==initial,"unsupported written boot selection");
    }
    s5l8920_clear_bus_failure(m);m->cpu.excl_valid=true;m->cpu.a8_excl_size=4u;
    uint32_t generation=m->cpu.tlb_gen;
    m->bus.write32(m,control,initial|2u);
    CHECK(!m->bus_failure.reason && m->ram_boot_window && m->miu.value==(initial|2u) &&
          m->cpu.tlb_gen!=generation && !m->cpu.excl_valid && !m->cpu.a8_excl_size,"MIU RAM selection/invalidation");
    generation=m->cpu.tlb_gen;m->cpu.excl_valid=true;
    m->bus.write32(m,control,initial|2u);
    memcpy(&before,m,sizeof before);
    CHECK(m->cpu.tlb_gen==generation && m->cpu.excl_valid && s5l8920_miu_configure(m,initial) &&
          !s5l8920_miu_configure(m,initial^1u) && !s5l8920_set_ram_boot_window(m,false) &&
          s5l8920_set_ram_boot_window(m,true) && !memcmp(&before,m,sizeof before),"MIU reconfiguration/host override");
    for (unsigned offset=0;offset<4u;offset++) for (unsigned kind=0;kind<6u;kind++) {
        s5l8920_clear_bus_failure(m);uint32_t address=control+offset;
        if (kind==0u) (void)m->bus.read8(m,address);
        else if (kind==1u) (void)m->bus.read16(m,address);
        else if (kind==2u) (void)m->bus.read32(m,address);
        else if (kind==3u) m->bus.write8(m,address,0u);
        else if (kind==4u) m->bus.write16(m,address,0u);
        else m->bus.write32(m,address,initial|2u);
        CHECK(m->bus_failure.reason==((kind%3u==2u && !offset)?S5L8920_BUS_OK:S5L8920_BUS_ACCESS_UNIMPLEMENTED) &&
              m->miu.value==(initial|2u) && m->ram_boot_window && m->cpu.tlb_gen==generation,"MIU width/alignment mutation");
    }
    CHECK(!m->bus.host_ram(m,control,4u) && !m->bus.host_ram_write(m,control,4u),"MIU exposed as RAM");
    for (unsigned thumb=0;thumb<2u;thumb++) {
        CHECK(s5l8920_reset(m) && !m->ram_boot_window && m->miu.value==initial,"MIU reset restores initial mapping");
        put(m,0x100u,thumb?0x0000f8c1u:0xe5810000u); /* STR r0,[r1] */
        m->cpu.r[0]=initial|3u;m->cpu.r[1]=control;m->cpu.r[15]=S5L8920_RAM_BASE+0x100u;
        m->cpu.cpsr=ARM_MODE_SVC|ARM_CPSR_I|ARM_CPSR_F|ARM_CPSR_C|(thumb?ARM_CPSR_T:0u);
        uint32_t flags=m->cpu.cpsr,pc=m->cpu.r[15];
        CHECK(arm_step(&m->cpu)==ARM_HALT && !m->cpu.cycles && m->cpu.r[15]==pc &&
              m->cpu.cpsr==flags && m->miu.value==initial && !m->ram_boot_window,"refused MIU instruction retired");
        s5l8920_clear_bus_failure(m);m->cpu.r[0]=initial|2u;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==1u && m->cpu.r[15]==pc+4u &&
              m->cpu.cpsr==flags && m->ram_boot_window,"MIU instruction retry");
        /* Execute the store through the alias it removes. The next fetch must
         * stop even though the preceding instruction warmed the code pointer. */
        m->cpu.r[15]=0x100u;m->cpu.r[0]=initial|1u;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.r[15]==0x104u && !m->ram_boot_window,"guest self-remap");
        CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.cycles==2u && m->cpu.r[15]==0x104u &&
              m->bus_failure.reason==S5L8920_BUS_UNMAPPED,"stale instruction pointer survived MIU write");
    }
    arm_bus_t original=m->bus;
    for (unsigned host=0;host<2u;host++) for (unsigned mmu=0;mmu<2u;mmu++) for (unsigned store=0;store<2u;store++) {
        CHECK(s5l8920_reset(m),"MIU cached data reset");
        m->bus.host_ram=host?original.host_ram:NULL;m->bus.host_ram_write=host?original.host_ram_write:NULL;
        m->bus.write32(m,control,initial|2u);
        put(m,0x300u,store?0xe5812000u:0xe5912000u);put(m,0x1000u,0x12345678u);
        uint32_t code=S5L8920_RAM_BASE+0x300u,target=0x1000u;
        if (mmu) {
            put(m,0x6000u,S5L8920_RAM_BASE|0xc0eu);put(m,0x6400u,0xc0eu);
            m->cpu.cp15.ttbr0=S5L8920_RAM_BASE+0x4000u;m->cpu.cp15.dacr=1u;
            m->cpu.cp15.sctlr|=ARM_SCTLR_M|ARM_SCTLR_XP;code=0x80000300u;target=0x90001000u;
        }
        m->cpu.r[15]=code;m->cpu.r[1]=target;m->cpu.r[2]=0x12345678u;
        CHECK(arm_step(&m->cpu)==ARM_OK,"warm MIU alias data access");
        m->bus.write32(m,control,initial|1u);m->cpu.r[15]=code;m->cpu.r[2]=0xabcdef01u;
        CHECK(arm_step(&m->cpu)==ARM_HALT && m->cpu.cycles==1u && m->cpu.r[15]==code &&
              m->cpu.r[2]==0xabcdef01u && m->bus_failure.address==0x1000u && m->ram[0x1000u]==0x78u,
              "stale data pointer survived MIU write");
        s5l8920_clear_bus_failure(m);m->bus.write32(m,control,initial|2u);
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==2u &&
              (store?m->ram[0x1000u]==1u:m->cpu.r[2]==0x12345678u),"MIU data access retry");
    }
    m->bus=original;
    CHECK(s5l8920_reset(m) && m->miu.value==initial && !m->ram_boot_window,"MIU final reset");
    /* Other initial selections and RAM-selected reset use independent boards. */
    for (unsigned mode=0;mode<4u;mode++) {
        CHECK(s5l8920_init(&empty),"MIU reset fixture");
        if (!empty.ram) break;
        CHECK(s5l8920_set_ram_boot_window(&empty,mode!=2u) && s5l8920_miu_configure(&empty,mode) &&
              empty.ram_boot_window==(mode==2u),"explicit MIU input owns prior host mapping");
        empty.bus.write32(&empty,control,mode==2u?1u:2u);
        CHECK(s5l8920_reset(&empty) && empty.miu.value==mode && empty.ram_boot_window==(mode==2u),"MIU initial-mode reset");
        s5l8920_free(&empty);
        CHECK(!empty.miu.configured && !empty.miu.value && !empty.miu.initial &&
              !s5l8920_miu_configure(&empty,mode),"free retained MIU state");
    }
}

static void test_usb_controls(s5l8920_t *m) {
    static s5l8920_t empty, before;
    const uint32_t addresses[]={0x86100e00u,0x86000000u,0x86000004u,0x86000008u,0x8600001cu,0x86000044u};
    const uint32_t masks[]={3u,0x1fu,3u,1u,6u,0xe3fu};
    CHECK(!s5l8920_usb_control_configure(NULL,addresses[0],0u) &&
          !s5l8920_usb_control_configure(&empty,addresses[0],0u),"invalid USB control object");
    CHECK(s5l8920_reset(m),"USB controls initial reset");
    const uint32_t unsupported[]={0u,UINT32_MAX,0x8600000cu,0x86000028u,0x86000048u,
        0x86001000u,0x86100000u,0x86100010u,0x86100014u,0x86100800u,0x86101000u,0x86100e04u};
    memcpy(&before,m,sizeof before);
    for (unsigned i=0;i<sizeof unsupported/sizeof unsupported[0];i++)
        CHECK(!s5l8920_usb_control_configure(m,unsupported[i],0u) && !memcmp(&before,m,sizeof before),"unsupported USB configuration mutated board");
    for (unsigned i=0;i<6u;i++) {
        uint32_t address=addresses[i],initial=0xa5a5f0e0u&~masks[i];
        s5l8920_clear_bus_failure(m);(void)m->bus.read32(m,address);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && !m->usb_control[i].configured,"invented USB reset word");
        s5l8920_clear_bus_failure(m);m->bus.write32(m,address,0u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && !m->usb_control[i].configured,"unconfigured USB write");
        memcpy(&before,m,sizeof before);
        before.usb_control[i].configured=true;before.usb_control[i].initial=before.usb_control[i].value=initial;
        CHECK(s5l8920_usb_control_configure(m,address,initial) && !memcmp(&before,m,sizeof before),"USB input changed CPU/IRQ/diagnostic");
        m->bus.write32(m,address,initial|masks[i]);
        CHECK(m->usb_control[i].value==initial,"latched fault allowed USB write");
        s5l8920_clear_bus_failure(m);
        for (unsigned bit=0;bit<32u;bit++) {
            uint32_t value=initial^(1u<<bit);
            s5l8920_clear_bus_failure(m);
            if (i!=5u) m->bus.write32(m,address,initial);
            m->bus.write32(m,address,value);
            bool accepted=(masks[i]&(1u<<bit)) && i!=5u;
            CHECK(m->bus_failure.reason==(accepted?S5L8920_BUS_OK:S5L8920_BUS_REGISTER_REFUSED) &&
                  m->usb_control[i].value==(accepted?value:initial),"USB unsupported field changed or known field refused");
        }
        s5l8920_clear_bus_failure(m);
        memcpy(&before,m,sizeof before);before.usb_control[i].value=initial|masks[i];
        m->bus.write32(m,address,initial|masks[i]);
        CHECK(!memcmp(&before,m,sizeof before) && m->bus.read32(m,address)==(initial|masks[i]),"USB control changed unrelated board state");
        CHECK(s5l8920_usb_control_configure(m,address,initial) && !s5l8920_usb_control_configure(m,address,initial^1u) &&
              !memcmp(&before,m,sizeof before),"USB reconfiguration replaced guest state");
        for (unsigned offset=0;offset<4u;offset++) for (unsigned kind=0;kind<6u;kind++) {
            s5l8920_clear_bus_failure(m);uint32_t at=address+offset,value=m->usb_control[i].value;
            if (kind==0u) (void)m->bus.read8(m,at);
            else if (kind==1u) (void)m->bus.read16(m,at);
            else if (kind==2u) (void)m->bus.read32(m,at);
            else if (kind==3u) m->bus.write8(m,at,0u);
            else if (kind==4u) m->bus.write16(m,at,0u);
            else m->bus.write32(m,at,value);
            CHECK(m->bus_failure.reason==((kind%3u==2u&&!offset)?S5L8920_BUS_OK:S5L8920_BUS_ACCESS_UNIMPLEMENTED) &&
                  m->usb_control[i].value==value,"USB width/alignment mutation");
            CHECK(!s5l8920_usb_control_configure(m,address+1u,0u),"misaligned USB input");
        }
        CHECK(!m->bus.host_ram(m,address,4u) && !m->bus.host_ram_write(m,address,4u),"USB MMIO exposed as RAM");
    }
    /* Status/FIFO/endpoint accesses never become successful because the
     * firmware has enabled clocks, released reset or set a software flag. */
    for (unsigned state=0;state<2u;state++) {
        CHECK(s5l8920_reset(m),"USB unsupported transfer fixture");
        for (unsigned i=0;i<6u;i++)m->bus.write32(m,addresses[i],m->usb_control[i].initial|(state?masks[i]:0u));
        for (unsigned i=2u;i<sizeof unsupported/sizeof unsupported[0];i++) {
            if (unsupported[i]==0x86001000u) continue;
            s5l8920_clear_bus_failure(m);(void)m->bus.read32(m,unsupported[i]);
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"invented USB status/transfer read");
            s5l8920_clear_bus_failure(m);m->bus.write32(m,unsupported[i],0u);
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"invented USB transfer write");
        }
    }
    for (unsigned thumb=0;thumb<2u;thumb++) {
        CHECK(s5l8920_reset(m),"USB CPU reset");
        uint32_t initial=m->usb_control[0].initial;
        put(m,0x100u,thumb?0x0000f8c1u:0xe5810000u);put(m,0x104u,thumb?0x0000f8d1u:0xe5910000u);
        m->cpu.r[0]=initial^4u;m->cpu.r[1]=addresses[0];m->cpu.r[15]=S5L8920_RAM_BASE+0x100u;
        m->cpu.cpsr=ARM_MODE_SVC|ARM_CPSR_I|ARM_CPSR_F|ARM_CPSR_C|(thumb?ARM_CPSR_T:0u);
        uint32_t flags=m->cpu.cpsr,pc=m->cpu.r[15];
        CHECK(arm_step(&m->cpu)==ARM_HALT && !m->cpu.cycles && m->cpu.r[15]==pc && m->cpu.cpsr==flags &&
              m->usb_control[0].value==initial,"unknown USB CPU write retired");
        s5l8920_clear_bus_failure(m);m->cpu.r[0]=initial|3u;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==1u && m->cpu.r[15]==pc+4u &&
              m->usb_control[0].value==(initial|3u),"USB CPU retry");
        m->cpu.r[0]=0u;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==2u && m->cpu.r[0]==(initial|3u) &&
              m->cpu.cpsr==flags && !m->timebase_ticks,"USB readback generated time or changed flags");
    }
    CHECK(s5l8920_reset(m),"USB final reset");
    for (unsigned i=0;i<6u;i++)CHECK(m->usb_control[i].configured &&
        m->usb_control[i].value==m->usb_control[i].initial,"USB reset lost input or retained writes");
}

static bool test_pll(s5l8920_t *m) {
    static s5l8920_t empty, before;
    uint64_t numerator=123u;uint32_t denominator=456u;
    CHECK(!s5l8920_pll_configure(NULL,0u,0u,1u,1u) &&
          !s5l8920_pll_configure(&empty,0u,0u,1u,1u) &&
          !s5l8920_pll_reference_clock(NULL,0u,1u) &&
          !s5l8920_pll_reference_clock(&empty,0u,1u) &&
          !s5l8920_pll_rate(NULL,0u,&numerator,&denominator) &&
          !s5l8920_pll_rate(&empty,0u,&numerator,&denominator),"invalid PLL host object");
    CHECK(s5l8920_reset(m),"PLL initial reset");
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_pll_configure(m,3u,0u,1u,1u) &&
          !s5l8920_pll_configure(m,0u,0u,1u,0u) &&
          !s5l8920_pll_configure(m,0u,1u,1u,1u) &&
          !s5l8920_pll_configure(m,0u,0x20000u,1u,1u) &&
          !s5l8920_pll_configure(m,0u,0x80000000u,1u,1u) &&
          !s5l8920_pll_reference_clock(m,0u,1u) &&
          !s5l8920_pll_rate(m,0u,&numerator,&denominator) && numerator==123u && denominator==456u &&
          !memcmp(&before,m,sizeof before),"invalid PLL configuration mutated state");
    for (unsigned index=0;index<3u;index++) {
        uint32_t address=S5L8920_PLL_BASE+4u*index;
        s5l8920_clear_bus_failure(m);(void)m->bus.read32(m,address);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"invented PLL initial word");
        s5l8920_clear_bus_failure(m);m->bus.write32(m,address,0u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && !m->pll[index].configured,
              "unconfigured PLL write accepted");
        uint32_t hz=index==0u?24000000u:(index==1u?UINT32_MAX:0u);
        uint64_t interval=index==1u?UINT64_MAX:8u;
        CHECK(s5l8920_pll_configure(m,index,0u,hz,interval),"explicit PLL inputs");
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"configuration cleared diagnostic");
        m->bus.write32(m,address,0x40609601u);
        CHECK(!m->pll[index].value,"latched failure allowed PLL write");
        s5l8920_clear_bus_failure(m);
        CHECK(m->bus.read32(m,address)==0u && !m->bus_failure.reason &&
              s5l8920_pll_rate(m,index,&numerator,&denominator) && !numerator && denominator==1u,
              "explicit disabled PLL state");
        m->bus.write32(m,address,0x40629601u); /* A written status bit cannot complete settling. */
        CHECK(!m->bus_failure.reason && m->pll[index].remaining==interval &&
              m->bus.read32(m,address)==0x40609601u,"PLL write forged readiness");
        memcpy(&before,m,sizeof before);
        CHECK(s5l8920_pll_configure(m,index,0u,hz,interval) &&
              !s5l8920_pll_configure(m,index,2u,hz,interval) &&
              !s5l8920_pll_configure(m,index,0u,hz^1u,interval) &&
              !s5l8920_pll_configure(m,index,0u,hz,interval-1u) &&
              !memcmp(&before,m,sizeof before),"PLL reconfiguration changed guest state");
        numerator=123u;denominator=456u;
        CHECK(!s5l8920_pll_rate(m,index,&numerator,&denominator) && numerator==123u && denominator==456u,
              "pending PLL exposed a stable rate");
        CHECK(s5l8920_pll_reference_clock(m,index,0u) && m->pll[index].remaining==interval,"zero cycles advanced PLL");
        CHECK(s5l8920_timebase_clock(m,UINT64_MAX) && m->pll[index].remaining==interval,"timebase advanced PLL");
        for (unsigned i=0;i<10u;i++) CHECK(!(m->bus.read32(m,address)&0x20000u),"polling advanced PLL");
        if (!hz) {
            CHECK(!s5l8920_pll_reference_clock(m,index,UINT64_MAX) && m->pll[index].remaining==interval &&
                  !(m->bus.read32(m,address)&0x20000u),"absent source produced readiness");
        } else {
            CHECK(s5l8920_pll_reference_clock(m,index,interval-1u) && m->pll[index].remaining==1u &&
                  !(m->bus.read32(m,address)&0x20000u),"PLL settled before interval");
            CHECK(s5l8920_pll_reference_clock(m,index,1u) && m->bus.read32(m,address)==0x40629601u &&
                  s5l8920_pll_rate(m,index,&numerator,&denominator) &&
                  numerator==(uint64_t)hz*150u && denominator==6u,"PLL source/rate at interval");
            CHECK(s5l8920_pll_reference_clock(m,index,UINT64_MAX) && !m->pll[index].remaining,"PLL clock overflow");
        }
        for (unsigned bit=0;bit<32u;bit++) if (!((1u<<bit)&0x43f2ff0fu)) {
            s5l8920_pll_t prior=m->pll[index];
            s5l8920_clear_bus_failure(m);m->bus.write32(m,address,0x40609601u|(1u<<bit));
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
                  !memcmp(&prior,&m->pll[index],sizeof prior),"unknown PLL bit mutated state");
        }
        static const uint32_t invalid[]={1u,0x00609601u,0x40009601u,0x40600001u};
        for (unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) {
            s5l8920_pll_t prior=m->pll[index];
            s5l8920_clear_bus_failure(m);m->bus.write32(m,address,invalid[i]);
            CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
                  !memcmp(&prior,&m->pll[index],sizeof prior),"unsupported enabled PLL program");
        }
        s5l8920_clear_bus_failure(m);m->bus.write32(m,address,0x43f2ff0eu);
        CHECK(!m->bus_failure.reason && m->bus.read32(m,address)==0x43f0ff0eu && !m->pll[index].remaining &&
              s5l8920_pll_rate(m,index,&numerator,&denominator) && !numerator && denominator==1u,
              "disable retained PLL status/output");
        for (unsigned later=index+1u;later<3u;later++) CHECK(!m->pll[later].configured,"PLL neighbor configured");
    }
    CHECK(!s5l8920_pll_rate(m,3u,&numerator,&denominator) &&
          !s5l8920_pll_rate(m,0u,NULL,&denominator) && !s5l8920_pll_rate(m,0u,&numerator,NULL) &&
          !s5l8920_pll_reference_clock(m,3u,1u),"invalid PLL query/clock");
    for (unsigned offset=0;offset<12u;offset++) for (unsigned kind=0;kind<6u;kind++) {
        uint32_t address=S5L8920_PLL_BASE+offset;
        s5l8920_clear_bus_failure(m);
        s5l8920_pll_t prior[3];memcpy(prior,m->pll,sizeof prior);
        if (kind==0u) (void)m->bus.read8(m,address);
        else if (kind==1u) (void)m->bus.read16(m,address);
        else if (kind==2u) (void)m->bus.read32(m,address);
        else if (kind==3u) m->bus.write8(m,address,0u);
        else if (kind==4u) m->bus.write16(m,address,0u);
        else m->bus.write32(m,address,0x43f0ff0eu);
        CHECK(m->bus_failure.reason==((kind%3u==2u && !(offset&3u))?S5L8920_BUS_OK:S5L8920_BUS_ACCESS_UNIMPLEMENTED) &&
              !memcmp(prior,m->pll,sizeof prior),"PLL width/alignment changed state");
    }
    CHECK(!m->bus.host_ram(m,S5L8920_PLL_BASE,12u) && !m->bus.host_ram_write(m,S5L8920_PLL_BASE,12u),"PLL exposed as RAM");
    static const unsigned divisors[]={1u,31u,32u,63u}, multipliers[]={1u,81u,150u,255u};
    for (unsigned index=0;index<2u;index++) for (unsigned p=0;p<4u;p++)
    for (unsigned v=0;v<4u;v++) for (unsigned shift=0;shift<8u;shift++) {
        uint32_t value=0x40000001u|(divisors[p]<<20)|(multipliers[v]<<8)|(shift<<1);
        s5l8920_clear_bus_failure(m);m->bus.write32(m,S5L8920_PLL_BASE+4u*index,value);
        CHECK(!m->bus_failure.reason && !s5l8920_pll_rate(m,index,&numerator,&denominator),"rewritten PLL stayed ready");
        CHECK(s5l8920_pll_reference_clock(m,index,UINT64_MAX) &&
              s5l8920_pll_rate(m,index,&numerator,&denominator) &&
              numerator==(uint64_t)m->pll[index].reference_hz*multipliers[v] &&
              denominator==divisors[p]*(1u<<shift),"six-bit divisor/three-bit shift rational rate");
    }
    CHECK(s5l8920_reset(m),"PLL functional reset");
    for (unsigned index=0;index<3u;index++) CHECK(m->pll[index].configured && !m->pll[index].value &&
        !m->pll[index].remaining && m->bus.read32(m,S5L8920_PLL_BASE+4u*index)==0u,"PLL reset lost input or retained program");
    for (unsigned thumb=0;thumb<2u;thumb++) {
        CHECK(s5l8920_reset(m),"PLL CPU reset");
        put(m,0x100u,thumb?0x0000f8c1u:0xe5810000u);
        m->cpu.r[0]=0x10000u;m->cpu.r[1]=S5L8920_PLL_BASE;m->cpu.r[15]=S5L8920_RAM_BASE+0x100u;
        m->cpu.cpsr=ARM_MODE_SVC|ARM_CPSR_I|ARM_CPSR_F|ARM_CPSR_C|(thumb?ARM_CPSR_T:0u);
        uint32_t flags=m->cpu.cpsr,pc=m->cpu.r[15];
        CHECK(arm_step(&m->cpu)==ARM_HALT && !m->cpu.cycles && m->cpu.r[15]==pc &&
              m->cpu.r[0]==0x10000u && m->cpu.cpsr==flags && !m->pll[0].value &&
              m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"invalid PLL CPU store retired");
        memcpy(&before,m,sizeof before);
        CHECK(s5l8920_pll_reference_clock(m,0u,1u) && !memcmp(&before,m,sizeof before),"host PLL clock changed stopped CPU/diagnostic");
        s5l8920_clear_bus_failure(m);m->cpu.r[0]=0x40609601u;
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==1u && m->cpu.r[15]==pc+4u &&
              m->cpu.cpsr==flags && m->pll[0].remaining==8u && m->bus.read32(m,S5L8920_PLL_BASE)==0x40609601u,
              "PLL CPU retry/implicit reference cycles");
        s5l8920_pll_t prior=m->pll[1];
        CHECK(s5l8920_pll_reference_clock(m,0u,3u),"first PLL clock chunk");
        (void)m->bus.read8(m,S5L8920_PLL_BASE);memcpy(&before,m,sizeof before);
        CHECK(s5l8920_pll_reference_clock(m,0u,5u) && !m->pll[0].remaining &&
              !memcmp(&before.cpu,&m->cpu,sizeof m->cpu) &&
              !memcmp(&before.bus_failure,&m->bus_failure,sizeof m->bus_failure) &&
              !memcmp(&prior,&m->pll[1],sizeof prior),"active PLL event changed CPU/diagnostic/neighbor");
        s5l8920_clear_bus_failure(m);
        CHECK(m->bus.read32(m,S5L8920_PLL_BASE)==0x40629601u,"chunked reference cycles");
    }
    CHECK(s5l8920_reset(m),"PLL final reset");
    return true;
}

static void test_clock_selectors(s5l8920_t *m) {
    CHECK(s5l8920_reset(m),"selector initial reset");
    for (unsigned index=0;index<S5L8920_CLOCK_SELECT_COUNT;index++) {
        uint32_t address=S5L8920_CLOCK_SELECT_BASE+4u*index;
        uint32_t mask=index==15u?0xfffffu:(index==10u?0xff0fffu:0xfffu);
        s5l8920_clear_bus_failure(m);(void)m->bus.read32(m,address);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
              !m->clock_selector[index].programmed,"invented selector reset value");
        m->bus.write32(m,address,mask);
        CHECK(!m->clock_selector[index].programmed,"latched access allowed selector mutation");
        for (unsigned bit=0;bit<32u;bit++) {
            uint32_t value=1u<<bit,previous=m->clock_selector[index].value;
            bool programmed=m->clock_selector[index].programmed;
            s5l8920_clear_bus_failure(m);m->bus.write32(m,address,value);
            if (value&mask) {
                CHECK(!m->bus_failure.reason && m->clock_selector[index].programmed &&
                      m->bus.read32(m,address)==value,"supported selector field not retained");
            } else CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED &&
                         m->clock_selector[index].value==previous &&
                         m->clock_selector[index].programmed==programmed,"unknown selector bit changed state");
        }
        for (unsigned zero=0;zero<2u;zero++) {
            uint32_t value=zero?0u:mask;
            s5l8920_clear_bus_failure(m);m->bus.write32(m,address,value);
            CHECK(!m->bus_failure.reason && m->bus.read32(m,address)==value &&
                  m->clock_selector[index].programmed,"explicit selector zero/full fields");
        }
        for (unsigned later=index+1u;later<S5L8920_CLOCK_SELECT_COUNT;later++)
            CHECK(!m->clock_selector[later].programmed,"selector write invented neighboring state");
    }
    for (unsigned offset=0;offset<4u*S5L8920_CLOCK_SELECT_COUNT;offset++) for (unsigned kind=0;kind<6u;kind++) {
        uint32_t address=S5L8920_CLOCK_SELECT_BASE+offset;
        s5l8920_clock_selector_t before[S5L8920_CLOCK_SELECT_COUNT];
        memcpy(before,m->clock_selector,sizeof before);s5l8920_clear_bus_failure(m);
        if (kind==0u) (void)m->bus.read8(m,address);
        else if (kind==1u) (void)m->bus.read16(m,address);
        else if (kind==2u) (void)m->bus.read32(m,address);
        else if (kind==3u) m->bus.write8(m,address,0u);
        else if (kind==4u) m->bus.write16(m,address,0u);
        else m->bus.write32(m,address,0u);
        CHECK(m->bus_failure.reason==((kind%3u==2u && !(offset&3u))?S5L8920_BUS_OK:S5L8920_BUS_ACCESS_UNIMPLEMENTED) &&
              !memcmp(before,m->clock_selector,sizeof before),"selector width/alignment changed state");
    }
    for (unsigned side=0;side<2u;side++) {
        uint32_t address=side?S5L8920_CLOCK_SELECT_BASE+100u:S5L8920_CLOCK_SELECT_BASE-4u;
        s5l8920_clear_bus_failure(m);m->bus.write32(m,address,0u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"selector span invaded unknown PMGR");
    }
    CHECK(!m->bus.host_ram(m,S5L8920_CLOCK_SELECT_BASE,4u) &&
          !m->bus.host_ram_write(m,S5L8920_CLOCK_SELECT_BASE,4u),"selector exposed as RAM");
    CHECK(s5l8920_reset(m),"selector reset");
    for (unsigned index=0;index<S5L8920_CLOCK_SELECT_COUNT;index++) {
        CHECK(!m->clock_selector[index].programmed,"reset retained selector programming");
        s5l8920_clear_bus_failure(m);(void)m->bus.read32(m,S5L8920_CLOCK_SELECT_BASE+4u*index);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"reset selector read supplied a value");
    }
    for (unsigned thumb=0;thumb<2u;thumb++) for (unsigned write=0;write<2u;write++) {
        CHECK(s5l8920_reset(m),"selector CPU reset");
        put(m,0x100u,thumb?(write?0x0000f8c1u:0x0000f8d1u):(write?0xe5810000u:0xe5910000u));
        m->cpu.r[0]=0x1000u;m->cpu.r[1]=S5L8920_CLOCK_SELECT_BASE;
        m->cpu.r[15]=S5L8920_RAM_BASE+0x100u;
        m->cpu.cpsr=ARM_MODE_SVC|ARM_CPSR_I|ARM_CPSR_F|ARM_CPSR_C|(thumb?ARM_CPSR_T:0u);
        uint32_t flags=m->cpu.cpsr,pc=m->cpu.r[15];
        CHECK(arm_step(&m->cpu)==ARM_HALT && !m->cpu.cycles && m->cpu.r[15]==pc &&
              m->cpu.r[0]==0x1000u && m->cpu.cpsr==flags &&
              m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED && !m->clock_selector[0].programmed,
              "refused selector CPU access retired");
        s5l8920_clear_bus_failure(m);
        if (write) m->cpu.r[0]=0xb03u;else m->bus.write32(m,S5L8920_CLOCK_SELECT_BASE,0xb03u);
        CHECK(arm_step(&m->cpu)==ARM_OK && m->cpu.cycles==1u && m->cpu.r[15]==pc+4u &&
              m->cpu.r[0]==0xb03u && m->cpu.cpsr==flags && !m->bus_failure.reason &&
              m->clock_selector[0].programmed && m->clock_selector[0].value==0xb03u,"selector CPU retry");
    }
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

static void test_uart_banks(s5l8920_t *m) {
    static s5l8920_t before,empty;
    CHECK(s5l8920_reset(m),"UART bank fixture reset");
    CHECK(!s5l8920_uart_divisor_configure(NULL,0u,0u) &&
          !s5l8920_uart_divisor_configure(&empty,0u,0u),"invalid UART divisor object");
    m->bus.write32(m,S5L8920_UART0_BASE+40u,12u);
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_uart_divisor_configure(m,0u,12u) && !memcmp(&before,m,sizeof before),
          "initial input overwrote an already guest-programmed divisor");
    CHECK(s5l8920_reset(m),"clear guest-only divisor before input cases");
    size_t count=111u;uint8_t output[17]={0};
    memcpy(&before,m,sizeof before);
    CHECK(!s5l8920_uart_bank_clock(m,5u,true,1u,output,sizeof output,&count) && count==111u &&
          !s5l8920_uart_bank_receive(m,5u,0u) && !s5l8920_uart_bank_cts(m,5u,true) &&
          !s5l8920_uart_bank_receive_timeout(m,5u) && !s5l8920_uart_divisor_configure(m,5u,0u) &&
          !memcmp(&before,m,sizeof before),"invalid bank changed state/output");
    for (unsigned bank=0;bank<S5L8920_UART_COUNT;bank++) {
        uint32_t base=S5L8920_UART0_BASE+bank*S5L8920_UART_STRIDE,initial=(bank<<16)|(12u+bank);
        s5l8920_uart_t *u=bank?&m->uart_extra[bank-1u]:&m->uart0;
        s5l8920_clear_bus_failure(m);(void)m->bus.read32(m,base+40u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_REGISTER_REFUSED,"invented UART initial divisor");
        memcpy(&before,m,sizeof before);
        s5l8920_uart_t *expected=bank?&before.uart_extra[bank-1u]:&before.uart0;
        expected->ubrdiv=initial;expected->programmed|=16u;
        before.uart_divisor_initial[bank]=initial;before.uart_divisor_configured[bank]=true;
        CHECK(s5l8920_uart_divisor_configure(m,bank,initial) && !memcmp(&before,m,sizeof before),"divisor input disturbed CPU/IRQ/failure/other port");
        s5l8920_clear_bus_failure(m);m->bus.write32(m,base+40u,12u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_OK && m->bus.read32(m,base+40u)==12u,"bank divisor programming");
        memcpy(&before,m,sizeof before);
        CHECK(s5l8920_uart_divisor_configure(m,bank,initial) &&
              !s5l8920_uart_divisor_configure(m,bank,initial^1u) &&
              !s5l8920_uart_divisor_configure(m,bank,0x90000u) &&
              !s5l8920_uart_divisor_configure(m,bank,0x100000u) &&
              !memcmp(&before,m,sizeof before),"divisor reconfiguration replaced guest state");
        for (unsigned off=0;off<4u;off++) {
            s5l8920_clear_bus_failure(m);m->bus.write16(m,base+40u+off,0u);
            CHECK(m->bus_failure.reason==S5L8920_BUS_ACCESS_UNIMPLEMENTED && u->ubrdiv==12u,"bank width changed divisor");
        }
        s5l8920_clear_bus_failure(m);
        m->bus.write32(m,base,bank==4u?7u:3u);m->bus.write32(m,base+4u,0x5c85u);
        m->bus.write32(m,base+8u,bank?1u:0u);
        if (bank)m->bus.write32(m,base+12u,1u);
        CHECK(m->bus_failure.reason==S5L8920_BUS_OK && m->bus.read32(m,base+16u)==6u &&
              u->no_modem==(bank==0u),"bank initialization/capability");
        CHECK(!m->bus.host_ram(m,base,4u) && !m->bus.host_ram_write(m,base,4u),"UART bank exposed as RAM");
    }
    uint32_t all=0x1f00000u;
    vic_write(m,0u,PL192_INTENABLE,all);
    for (unsigned bank=0;bank<S5L8920_UART_COUNT;bank++) {
        uint32_t bit=1u<<(S5L8920_UART0_IRQ-bank);
        CHECK(s5l8920_uart_bank_receive(m,bank,(uint8_t)(0xa0u+bank)) &&
              (m->vic[0].input&bit) && m->cpu.irq_line,"bank RX did not reach own IRQ");
    }
    CHECK((m->vic[0].input&all)==all,"simultaneous UART IRQs lost a port");
    for (unsigned bank=0;bank<S5L8920_UART_COUNT;bank++) {
        uint32_t base=S5L8920_UART0_BASE+bank*S5L8920_UART_STRIDE,bit=1u<<(S5L8920_UART0_IRQ-bank);
        CHECK(s5l8920_set_irq(m,S5L8920_UART0_IRQ-bank,true),"external shared line");
        uint32_t status=m->bus.read32(m,base+16u);m->bus.write32(m,base+16u,status);
        CHECK((m->vic[0].input&bit)!=0u,"UART ack erased external level");
        CHECK(s5l8920_set_irq(m,S5L8920_UART0_IRQ-bank,false) && !(m->vic[0].input&bit),"external withdrawal left false UART event");
        CHECK(s5l8920_uart_bank_receive_timeout(m,bank) && (m->vic[0].input&bit),"bank timeout did not reach IRQ");
        m->bus.write32(m,base+16u,m->bus.read32(m,base+16u));
        CHECK(m->bus.read32(m,base+36u)==0xa0u+bank && !s5l8920_uart_bank_receive_timeout(m,bank),"RX data crossed banks or empty timeout accepted");
    }
    CHECK(!m->cpu.irq_line && !m->cpu.fiq_line && !(m->vic[0].input&all),"cleared bank events retained IRQ");
    CHECK(!s5l8920_uart_bank_cts(m,0u,true) && s5l8920_uart_bank_cts(m,3u,true),"board CTS capability routing");
    for (unsigned bank=0;bank<S5L8920_UART_COUNT;bank++) {
        uint32_t base=S5L8920_UART0_BASE+bank*S5L8920_UART_STRIDE;
        m->bus.write32(m,base+32u,0x30u+bank);
        uint32_t cycles=(bank==4u?11u:10u)*13u*16u;
        CHECK(s5l8920_uart_bank_clock(m,bank,true,cycles,output,sizeof output,&count) &&
              count==1u && output[0]==0x30u+bank && !m->timebase_ticks,"bank clock/output/timing");
    }
    CHECK(s5l8920_reset(m),"reset UART banks");
    for (unsigned bank=0;bank<S5L8920_UART_COUNT;bank++) {
        s5l8920_uart_t *u=bank?&m->uart_extra[bank-1u]:&m->uart0;
        CHECK(u->ubrdiv==m->uart_divisor_initial[bank] && u->programmed==16u &&
              !u->tx_busy && !u->tx_count && !u->rx_count && !u->pending && !u->cts_valid &&
              u->no_modem==(bank==0u),"reset did not restore explicit input or clear traffic");
    }
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
    if (!test_gpio_inactive_readback(&m)) return 1;
    test_i2c_staging_bus(&m);
    test_i2c_endpoints(&m);
    test_i2c_bounds(&m);
    test_i2c_checked_cpu(&m);
    test_pmu_rtc(&m);
    test_pmu_adc(&m);
    test_pmu_config(&m);
    test_pmu_control(&m);
    test_fiq_and_reset(&m);
    test_clock_selectors(&m);
    if (!test_clock_gates(&m)) return 1;
    if (!test_chipid(&m)) return 1;
    if (!test_pll(&m)) return 1;
    if (!test_powerid(&m)) return 1;
    test_miu(&m);
    test_usb_controls(&m);
    test_uart_banks(&m);
    if (!test_pmu_events(&m)) return 1;
    if (!test_pmu_boot_state(&m)) return 1;
    s5l8920_free(&m);
    CHECK(!m.ram && !m.cpu.bus && !m.bus.ctx && !s5l8920_reset(&m), "free left live host wiring");
    CHECK(!m.pmu_boot_configured && !m.pmu_boot_programmed && !m.pmu_boot_initial && !m.pmu_boot_value &&
          !s5l8920_pmu_boot_state_service(&m,1u) && !s5l8920_pmu_boot_state_configure(&m,0u),"free retained boot-state byte");
    CHECK(!m.pmu_adc.programmed && !m.pmu_adc.result_valid && !m.pmu_adc.sequence &&
          !s5l8920_pmu_adc_service(&m,1u) && !s5l8920_pmu_adc_complete(&m,1u,0u,0u),"free retained ADC state");
    CHECK(!m.pmu_config.control_programmed && !m.pmu_config.selectors_programmed &&
          !s5l8920_pmu_config_service(&m,1u),"free retained PMU configuration");
    CHECK(!m.pmu_events.configured && !m.pmu_events.status_valid && !m.pmu_events.masks_programmed &&
          !m.pmu_events.pending && !s5l8920_pmu_events_service(&m,1u) &&
          !s5l8920_pmu_events_raise(&m,1u),"free retained PMU event state");
    CHECK(!m.pmu_control_configured && !m.pmu_control_initial && !m.pmu_control_value &&
          !s5l8920_pmu_control_configure(&m,0u) && !s5l8920_pmu_control_service(&m,1u),"free retained PMU control");
    for (unsigned bank=0;bank<S5L8920_UART_COUNT;bank++)CHECK(!m.uart_divisor_configured[bank] &&
        !m.uart_divisor_initial[bank] && !s5l8920_uart_divisor_configure(&m,bank,0u),"free retained UART input");
    for (unsigned i=0;i<S5L8920_USB_CONTROL_COUNT;i++)CHECK(!m.usb_control[i].configured &&
        !m.usb_control[i].initial && !m.usb_control[i].value,"free retained USB input");
    CHECK(!s5l8920_usb_control_configure(&m,S5L8920_USB_PHY_BASE,0u),"freed board accepted USB input");
    CHECK(!m.gpio[15].inactive_valid && !m.gpio[15].inactive_high &&
          !s5l8920_gpio_inactive_readback(&m,15u,false),"free retained inactive observation");
    CHECK(!m.clock_selector[0].programmed && !m.clock_selector[24].programmed,
          "free retained selector programming");
    CHECK(!m.powerid.configured && !m.powerid.initial && !m.powerid.value &&
          !s5l8920_powerid_configure(&m,0u),"freed POWERID retained state or accepted configuration");
    CHECK(!m.pll[0].configured && !m.pll[2].configured && !m.pll[0].reference_hz &&
          !s5l8920_pll_configure(&m,0u,0u,24000000u,8u) &&
          !s5l8920_pll_reference_clock(&m,0u,1u),"freed PLL retained state or accepted input");
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
