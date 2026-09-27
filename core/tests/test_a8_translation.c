/* Cortex-A8 ATS/PAR: query mappings without performing the target access.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm.h"
#include <stdio.h>
#include <string.h>

static struct {
    arm_cpu_t cpu;
    arm_bus_t bus;
    uint8_t ram[0x10000];
    uint32_t reject, code_va;
    unsigned reject_size, table_reads, writes, after_failure;
    bool failed, thumb;
} f;
static unsigned passed, failed;
#define CHECK(c, ...) do { if (c) passed++; else { if (++failed <= 24u) { \
    printf("FAIL %s:%d: ",__func__,__LINE__); printf(__VA_ARGS__); puts(""); } } } while (0)
static bool reject(uint32_t address, unsigned size) {
    if (f.failed) { f.after_failure++; return true; }
    if ((uint64_t)address+size>sizeof f.ram ||
        (address==f.reject && size==f.reject_size)) { f.failed=true; return true; }
    if (address>=0x4000u) f.table_reads++;
    return false;
}
#define READ(bits) static uint##bits##_t read##bits(void *ctx,uint32_t address) { \
    (void)ctx; uint##bits##_t value=0u; \
    if (!reject(address,(bits)/8u)) { memcpy(&value,f.ram+address,(bits)/8u); } return value; }
READ(8)
READ(16)
READ(32)
static void write32(void *ctx,uint32_t address,uint32_t value) {
    (void)ctx; (void)address; (void)value; f.writes++; f.failed=true;
}
static bool access_failed(void *ctx) { (void)ctx; return f.failed; }
static uint8_t *host_ram(void *ctx,uint32_t address,uint32_t size) {
    (void)ctx;
    if (f.reject>=address && (uint64_t)f.reject<(uint64_t)address+size) return NULL;
    return (uint64_t)address+size<=sizeof f.ram ? f.ram+address : NULL;
}
static void put16(uint32_t address,uint16_t value) { memcpy(f.ram+address,&value,2u); }
static void put32(uint32_t address,uint32_t value) { memcpy(f.ram+address,&value,4u); }
static uint32_t get32(uint32_t address) { uint32_t value; memcpy(&value,f.ram+address,4u); return value; }
static uint32_t cp15(bool load,unsigned opc1,unsigned crn,unsigned rd,unsigned crm,unsigned opc2) {
    return 0xee000f10u | ((unsigned)load<<20) | (opc1<<21) | (crn<<16) | (rd<<12) | (opc2<<5) | crm;
}
static arm_status_t step(uint32_t offset,uint32_t insn) {
    if (f.thumb) { put16(offset,(uint16_t)(insn>>16)); put16(offset+2u,(uint16_t)insn); }
    else put32(offset,insn);
    f.cpu.r[15]=f.code_va+offset;
    return arm_step(&f.cpu);
}
static void reset(bool thumb,bool host,arm_arch_t profile) {
    memset(&f,0,sizeof f); f.reject=UINT32_MAX; f.thumb=thumb;
    f.bus=(arm_bus_t){.read8=read8,.read16=read16,.read32=read32,.write32=write32,.access_failed=access_failed};
    if (host) { f.bus.host_ram=host_ram; f.bus.host_ram_write=host_ram; }
    CHECK(arm_reset_profile(&f.cpu,&f.bus,profile),"reset");
    f.cpu.cpsr=ARM_MODE_SVC | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q | ARM_CPSR_I | ARM_CPSR_F |
               (thumb ? ARM_CPSR_T : 0u) | 0xa0000u;
    f.cpu.cp15.dfsr=0x123u; f.cpu.cp15.ifsr=0x456u;
    f.cpu.cp15.dfar=0x789u; f.cpu.cp15.ifar=0xabcu;
    f.cpu.excl_valid=true; f.cpu.excl_addr=0x2468u; f.cpu.a8_excl_size=8u;
    f.cpu.vfp_fpscr=0x0bc00080u; f.cpu.a8_vfp_hi[0]=UINT64_C(0x123456789abcdef0);
}
static void seed_par(uint32_t value) {
    f.cpu.r[2]=value;
    CHECK(step(0x100u,cp15(false,0,7,2,4,0))==ARM_OK,"PAR write");
}
static uint32_t read_par(void) {
    f.cpu.r[2]=0xdeadbeefu;
    CHECK(step(0x108u,cp15(true,0,7,2,4,0))==ARM_OK,"PAR read");
    return f.cpu.r[2];
}
static void unchanged(uint32_t flags,const arm_cp15_t *controls) {
    CHECK(f.cpu.cpsr==flags && memcmp(&f.cpu.cp15,controls,sizeof *controls)==0 && !f.cpu.abort_pending,
          "ATS altered flags, fault/control registers, or raised an exception");
    CHECK(f.cpu.excl_valid && f.cpu.excl_addr==0x2468u && f.cpu.a8_excl_size==8u &&
          f.cpu.vfp_fpscr==0x0bc00080u && f.cpu.a8_vfp_hi[0]==UINT64_C(0x123456789abcdef0),
          "ATS altered unrelated state");
    CHECK(!f.writes && !f.after_failure,"ATS wrote memory or accessed after failure");
}

typedef struct { uint32_t va,pa,l1,entry; unsigned shape; } mapping_t;
static mapping_t mapping(unsigned shape,unsigned encoding,bool share,bool ns,bool thumb,bool host) {
    static const uint32_t va[]={0x81234321u,0x81a34321u,0x812a4321u,0x81234321u};
    static const uint32_t pa[]={0x02034321u,0x02a34321u,0x002a4321u,0x002a0321u};
    static const uint32_t desc[]={0x02000c02u,0x02040c02u,0x002a0031u,0x002a0032u};
    reset(thumb,host,ARM_ARCH_V7_CORTEX_A8);
    f.cpu.cp15.sctlr=ARM_SCTLR_M | ARM_SCTLR_XP;
    f.cpu.cp15.ttbr0=f.cpu.cp15.ttbr1=0x4000u;
    f.cpu.cp15.dacr=0xc0000001u; /* Code domain15 manager; target domain0 client. */
    put32(0x4000u,0xde2u);
    mapping_t m={va[shape],pa[shape],0x4000u+4u*(va[shape]>>20),0u,shape};
    m.entry=shape<2u ? m.l1 : 0x9000u+4u*((m.va>>12)&255u);
    if (shape>=2u) put32(m.l1,0x9001u | (ns ? 8u : 0u));
    unsigned tex_shift=shape==3u ? 6u : 12u;
    uint32_t descriptor=desc[shape] | ((encoding>>2)<<tex_shift) | ((encoding&3u)<<2) |
        (share ? 1u<<(shape<2u ? 16u : 10u) : 0u) | (ns && shape<2u ? 1u<<19 : 0u);
    if (shape==1u || shape==2u)
        for (unsigned n=0;n<16u;n++) put32((m.entry&~63u)+4u*n,descriptor);
    else put32(m.entry,descriptor);
    return m;
}
static void test_mmu_off_and_register(void) {
    static const uint32_t addresses[]={0u,0xfffu,0x1000u,0x81234567u,UINT32_MAX};
    for (unsigned thumb=0;thumb<2u;thumb++) for (unsigned host=0;host<2u;host++)
     for (unsigned op=0;op<4u;op++) for (unsigned n=0;n<5u;n++) {
        reset(thumb!=0u,host!=0u,ARM_ARCH_V7_CORTEX_A8);
        CHECK(read_par()==0u,"documented choice for UNKNOWN reset");
        seed_par(0x1234507cu); CHECK(read_par()==0x1234507cu,"PAR context-switch round trip");
        uint32_t flags=f.cpu.cpsr; arm_cp15_t controls=f.cpu.cp15;
        f.cpu.r[1]=addresses[n]; uint64_t cycles=f.cpu.cycles;
        CHECK(step(0x104u,cp15(false,0,7,1,8,op))==ARM_OK && f.cpu.r[15]==0x108u &&
              f.cpu.r[1]==addresses[n] && f.cpu.cycles==cycles+1u,"MMU-off ATS op=%u",op);
        CHECK(read_par()==((addresses[n]&0xfffff000u)|0x90u) && !f.failed && !f.table_reads,
              "MMU-off identity, Strongly-ordered/Shareable, or unexpected target read");
        unchanged(flags,&controls);
        CHECK(arm_reset_profile(&f.cpu,&f.bus,ARM_ARCH_V7_CORTEX_A8),"reset after PAR use");
        f.cpu.cpsr|=thumb ? ARM_CPSR_T : 0u;
        CHECK(read_par()==0u,"reset retained PAR");
    }
}
static void test_descriptor_results(void) {
    /* Independent PAR attribute table from DDI0344K table3-78 and
     * DDI0406C.b tablesB3-10/11. 0xffff denotes an unimplemented encoding. */
    static const uint16_t attrs[32]={0x90,0xb0,0x68,0x7c,0,0xffff,0xffff,0x54,
        0x30,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,0xffff,
        0x00,0x50,0x60,0x70,0x04,0x54,0x64,0x74,0x08,0x58,0x68,0x78,0x0c,0x5c,0x6c,0x7c};
    for (unsigned shape=0;shape<4u;shape++) for (unsigned encoding=0;encoding<32u;encoding++)
     for (unsigned share=0;share<2u;share++) for (unsigned ns=0;ns<2u;ns++)
      for (unsigned op=0;op<4u;op++) for (unsigned thumb=0;thumb<2u;thumb++) {
        mapping_t m=mapping(shape,encoding,share!=0u,ns!=0u,thumb!=0u,(encoding&1u)!=0u);
        seed_par(0x1234507cu);
        uint32_t flags=f.cpu.cpsr; arm_cp15_t controls=f.cpu.cp15;
        f.cpu.r[1]=m.va;
        bool supported=attrs[encoding]!=0xffffu;
        arm_status_t status=step(0x104u,cp15(false,0,7,1,8,op));
        CHECK(status==(supported ? ARM_OK : ARM_UNDEFINED) && f.cpu.r[1]==m.va &&
              f.cpu.r[15]==(supported ? 0x108u : 0x104u),"shape=%u TEXCB=%u op=%u state=%u",shape,encoding,op,thumb);
        uint32_t expected=attrs[encoding] | (ns<<9);
        if (share && encoding!=0u && encoding!=1u && encoding!=8u) expected|=0x80u;
        expected|=shape==1u ? (m.pa&0xff000000u)|2u : m.pa&0xfffff000u;
        CHECK(read_par()==(supported ? expected : 0x1234507cu) && !f.failed,
              "PAR mapping/attributes or target access shape=%u TEXCB=%u share=%u ns=%u",shape,encoding,share,ns);
        unchanged(flags,&controls);
    }
}
static void test_permissions_and_faults(void) {
    /* Columns PR,PW,UR,UW. AP[2:0], with SCTLR.S/R disabled. */
    static const unsigned permissions[]={0u,3u,7u,15u,0u,1u,5u,5u};
    for (unsigned shape=0;shape<4u;shape++) for (unsigned ap=0;ap<8u;ap++)
     for (unsigned dac=0;dac<4u;dac++) for (unsigned afe=0;afe<2u;afe++) for (unsigned op=0;op<4u;op++) {
        mapping_t m=mapping(shape,3u,false,false,(op&1u)!=0u,false);
        unsigned domain=shape==1u ? 0u : 3u;
        if (shape!=1u) put32(m.l1,get32(m.l1)|(domain<<5));
        uint32_t d=get32(m.entry);
        if (shape<2u) d=(d&~0x8c00u)|((ap&3u)<<10)|((ap&4u)<<13)|0x10u;
        else d=(d&~0x230u)|((ap&3u)<<4)|((ap&4u)<<7)|(shape==2u ? 0x8000u : 1u);
        put32(m.entry,d); /* Only this queried VA; replicas are not accessed. */
        f.cpu.cp15.dacr=0xc0000000u|(dac<<(domain*2u));
        if (afe) f.cpu.cp15.sctlr|=ARM_SCTLR_FA;
        seed_par(0x1234507cu);
        uint32_t flags=f.cpu.cpsr; arm_cp15_t controls=f.cpu.cp15;
        unsigned fault=0u;
        if (afe && !(ap&1u)) fault=shape<2u ? 3u : 6u;
        else if (dac==0u || dac==2u) fault=shape<2u ? 9u : 11u;
        else if (dac!=3u && !(permissions[ap]&(1u<<op))) fault=shape<2u ? 13u : 15u;
        uint32_t expected=fault ? 1u+2u*fault :
            (shape==1u ? (m.pa&0xff000000u)|2u : m.pa&0xfffff000u)|0x7cu;
        f.cpu.r[1]=m.va;
        CHECK(step(0x104u,cp15(false,0,7,1,8,op))==ARM_OK && read_par()==expected && !f.failed,
              "permission/fault shape=%u AP=%u DAC=%u AFE=%u op=%u",shape,ap,dac,afe,op);
        unchanged(flags,&controls);
    }
    for (unsigned which=0;which<5u;which++) {
        mapping_t m=mapping(3u,3u,false,false,false,false);
        if (which<2u) put32(which ? m.entry : m.l1,0u);
        if (which==2u) put32(m.l1,3u); /* Reserved L1 format. */
        if (which>=3u) {
            f.cpu.cp15.ttbcr=2u;
            if (which==4u) {
                f.code_va=0x80000000u; put32(0x6000u,0xde2u);
                m.va-=0x80000000u; put32(0x4000u+4u*(m.va>>20),get32(m.l1));
            }
            /* First fetch and seed PAR while the code's selected walk is enabled. */
        }
        seed_par(0x1234507cu);
        if (which>=3u) f.cpu.cp15.ttbcr|=1u<<(which==3u ? 5u : 4u);
        uint32_t flags=f.cpu.cpsr; arm_cp15_t controls=f.cpu.cp15;
        f.cpu.r[1]=m.va;
        CHECK(step(0x104u,cp15(false,0,7,1,8,1))==ARM_OK && read_par()==(which==1u ? 15u : 11u),
              "translation/PD fault kind=%u",which);
        unchanged(flags,&controls);
    }
}
static void test_cached_attributes_and_retry(void) {
    mapping_t m=mapping(0u,3u,false,false,false,true);
    seed_par(0x1234507cu);
    uint32_t pa;
    CHECK(!arm_mmu_translate(&f.cpu,m.va,ARM_ACCESS_READ,true,&pa),"warm ordinary translation");
    unsigned reads=f.table_reads;
    put32(m.entry,0x03080c06u); /* New PA, NS, shareable Device. */
    f.cpu.r[1]=m.va;
    CHECK(step(0x104u,cp15(false,0,7,1,8,0))==ARM_OK && read_par()==0x0203407cu && f.table_reads==reads,
          "cached PA combined with fresh descriptor attributes");
    arm_mmu_tlb_flush(&f.cpu);
    CHECK(step(0x104u,cp15(false,0,7,1,8,0))==ARM_OK && read_par()==0x030342b0u,"TLBI retained stale PAR attributes");
    put32(m.entry,0x04011c02u); f.cpu.cp15.context_id=7u;
    CHECK(step(0x104u,cp15(false,0,7,1,8,0))==ARM_OK && read_par()==0x04034080u,"context change retained attributes");
    put32(m.entry,0x05000c02u); f.cpu.tlb_gen=UINT32_MAX; arm_mmu_tlb_flush(&f.cpu);
    CHECK(step(0x104u,cp15(false,0,7,1,8,0))==ARM_OK && read_par()==0x05034090u,"generation wrap reused metadata");

    for (unsigned thumb=0;thumb<2u;thumb++) for (unsigned where=0;where<2u;where++) {
        m=mapping(3u,3u,false,false,thumb!=0u,true); seed_par(0x1234507cu);
        f.cpu.r[1]=m.va; f.reject=where ? m.entry : m.l1; f.reject_size=4u;
        uint64_t cycles=f.cpu.cycles; uint32_t flags=f.cpu.cpsr; arm_cp15_t controls=f.cpu.cp15;
        CHECK(step(0x104u,cp15(false,0,7,1,8,0))==ARM_HALT && f.failed && f.cpu.r[15]==0x104u &&
              f.cpu.cycles==cycles,"table-walk host failure retired or became guest fault");
        unchanged(flags,&controls);
        f.failed=false; f.reject=UINT32_MAX;
        CHECK(read_par()==0x1234507cu,"host failure changed PAR");
        CHECK(step(0x104u,cp15(false,0,7,1,8,0))==ARM_OK && read_par()==0x002a007cu,"host failure cached or prevented retry");
    }
    m=mapping(3u,3u,false,false,false,false); seed_par(0x1234507cu);
    f.cpu.cp15.sctlr|=ARM_SCTLR_FA; put32(m.entry,0x002a002eu); f.cpu.r[1]=m.va;
    CHECK(step(0x104u,cp15(false,0,7,1,8,0))==ARM_OK && read_par()==13u,"access flag fault");
    put32(m.entry,0x002a003eu);
    CHECK(step(0x104u,cp15(false,0,7,1,8,0))==ARM_OK && read_par()==0x002a007cu,"AF repair required unnecessary TLBI");
}
static void test_refusals_and_legacy(void) {
    const uint32_t invalid[]={0xee170f18u,0xee070f98u,0xee070ff8u,0xee870f18u,
        0xee170f34u,0xee270f14u,0xee07ff14u,0xee17ff14u,0xee07ff18u};
    const uint32_t bad_values[]={0x100u,0x400u,0x800u,0x81u,0x1001u,0x80000001u};
    for (unsigned thumb=0;thumb<2u;thumb++) {
        for (unsigned n=0;n<sizeof invalid/sizeof invalid[0];n++) {
            reset(thumb!=0u,false,ARM_ARCH_V7_CORTEX_A8); seed_par(0x1234507cu);
            uint32_t flags=f.cpu.cpsr; arm_cp15_t controls=f.cpu.cp15;
            CHECK(step(0x104u,invalid[n])==ARM_UNDEFINED && read_par()==0x1234507cu,"reserved selector/PC n=%u",n);
            unchanged(flags,&controls);
        }
        for (unsigned n=0;n<sizeof bad_values/sizeof bad_values[0];n++) {
            reset(thumb!=0u,false,ARM_ARCH_V7_CORTEX_A8); seed_par(0x1234507cu);
            f.cpu.r[1]=bad_values[n];
            CHECK(step(0x104u,cp15(false,0,7,1,4,0))==ARM_UNDEFINED && read_par()==0x1234507cu,
                  "PAR reserved write n=%u",n);
        }
        for (unsigned op=0;op<6u;op++) {
            reset(thumb!=0u,false,ARM_ARCH_V7_CORTEX_A8); seed_par(0x1234507cu);
            f.cpu.cpsr=(f.cpu.cpsr&~ARM_CPSR_MODE_MASK)|ARM_MODE_USR;
            uint32_t flags=f.cpu.cpsr; arm_cp15_t controls=f.cpu.cp15;
            CHECK(step(0x104u,op<4u ? cp15(false,0,7,1,8,op) : cp15(op==5u,0,7,1,4,0))==ARM_UNDEFINED,
                  "User PAR/ATS op=%u",op);
            unchanged(flags,&controls);
            f.cpu.cpsr=(f.cpu.cpsr&~ARM_CPSR_MODE_MASK)|ARM_MODE_SVC;
            CHECK(read_par()==0x1234507cu,"User refusal changed PAR");
        }
        for (unsigned profile=0;profile<2u;profile++) {
            reset(false,false,profile ? ARM_ARCH_V7_SWIFT : ARM_ARCH_V6_ARM1176);
            CHECK(step(0x104u,cp15(false,0,7,1,8,0))==ARM_OK && read_par()==0u,"legacy CP15 behavior changed");
        }
    }
}
static void test_conditions_fetch_and_configuration(void) {
    for (unsigned thumb=0;thumb<2u;thumb++) for (unsigned z=0;z<2u;z++) for (unsigned invalid=0;invalid<2u;invalid++) {
        reset(thumb!=0u,false,ARM_ARCH_V7_CORTEX_A8); seed_par(0x1234507cu);
        if (z) f.cpu.cpsr|=ARM_CPSR_Z;
        uint32_t flags=f.cpu.cpsr;
        f.cpu.r[1]=0xabcde001u;
        uint32_t insn=cp15(false,0,7,invalid ? 15u : 1u,8,0), offset=0x104u;
        if (thumb) {
            put16(0x100u,0xbf08u); f.cpu.r[15]=0x100u;
            CHECK(arm_step(&f.cpu)==ARM_OK,"IT EQ"); offset=0x102u;
        } else insn&=0x0fffffffu; /* EQ */
        uint32_t it_flags=f.cpu.cpsr;
        bool refused=z && invalid;
        CHECK(step(offset,insn)==(refused ? ARM_UNDEFINED : ARM_OK) &&
              f.cpu.r[15]==offset+(refused ? 0u : 4u) && f.cpu.cpsr==(refused ? it_flags : flags),
              "condition/fetch order thumb=%u Z=%u invalid=%u",thumb,z,invalid);
        f.cpu.cpsr=flags; /* Inspect stored PAR outside a refused IT instruction. */
        CHECK(read_par()==(z && !invalid ? 0xabcde090u : 0x1234507cu),"conditional ATS changed wrong PAR");
    }
    for (unsigned thumb=0;thumb<2u;thumb++) for (unsigned half=0;half<1u+thumb;half++) {
        reset(thumb!=0u,true,ARM_ARCH_V7_CORTEX_A8); seed_par(0x1234507cu);
        f.reject=0x104u+half*2u; f.reject_size=thumb ? 2u : 4u;
        arm_mmu_tlb_flush(&f.cpu);
        uint64_t cycles=f.cpu.cycles; uint32_t flags=f.cpu.cpsr; arm_cp15_t controls=f.cpu.cp15;
        CHECK(step(0x104u,cp15(false,0,7,1,8,0))==ARM_HALT && f.cpu.cycles==cycles && f.cpu.r[15]==0x104u,
              "failed instruction fetch retired ATS");
        unchanged(flags,&controls);
        f.failed=false; f.reject=UINT32_MAX;
        CHECK(read_par()==0x1234507cu,"failed instruction fetch changed PAR");
    }
    for (unsigned fault=0;fault<4u;fault++) {
        reset(true,false,ARM_ARCH_V7_CORTEX_A8); seed_par(0x1234507cu);
        uint32_t insn=cp15(false,0,7,1,8,0);
        put16(0x9ffeu,(uint16_t)(insn>>16)); put16(0xa000u,(uint16_t)insn);
        put32(0x4000u,0x8001u); put32(0x8000u,fault==3u ? 0u : 0x9032u);
        put32(0x8004u,fault==0u ? 0u : fault==1u ? 0xa033u : 0xa012u);
        f.cpu.cp15.sctlr=ARM_SCTLR_M|ARM_SCTLR_XP; f.cpu.cp15.ttbr0=0x4000u; f.cpu.cp15.dacr=1u;
        f.cpu.cpsr=ARM_MODE_USR|ARM_CPSR_T; f.cpu.r[15]=0xffeu;
        f.reject=0xa000u; f.reject_size=2u;
        CHECK(arm_step(&f.cpu)==ARM_OK && f.cpu.r[15]==ARM_VEC_PREFETCH && !f.failed &&
              f.cpu.cp15.ifar==(fault==3u ? 0xffeu : 0x1000u),"split fetch fault priority=%u",fault);
        f.cpu.cp15.sctlr&=~ARM_SCTLR_M; f.cpu.cpsr=ARM_MODE_SVC|ARM_CPSR_T;
        CHECK(read_par()==0x1234507cu,"prefetch exception changed PAR");
    }
    for (unsigned option=0;option<4u;option++) {
        mapping_t m=mapping(option==3u ? 1u : 0u,3u,false,false,false,false);
        seed_par(0x1234507cu);
        if (option==0u) f.cpu.cp15.sctlr|=1u<<28;
        if (option==1u) f.cpu.cp15.sctlr|=ARM_SCTLR_EE;
        if (option==2u) f.cpu.cp15.sctlr&=~ARM_SCTLR_XP;
        if (option==3u) put32(m.entry,get32(m.entry)|0x00100000u); /* Unmodeled extended PA. */
        f.cpu.r[1]=m.va;
        CHECK(step(0x104u,cp15(false,0,7,1,8,0))==ARM_UNDEFINED && read_par()==0x1234507cu && !f.failed,
              "unsupported translation configuration invented a PAR option=%u",option);
    }
}
int main(void) {
    test_mmu_off_and_register(); test_descriptor_results(); test_permissions_and_faults();
    test_cached_attributes_and_retry(); test_refusals_and_legacy(); test_conditions_fetch_and_configuration();
    printf("%u passed, %u failed\n",passed,failed);
    return failed ? 1 : 0;
}
