/* Cortex-A8 translation attributes must describe the cached physical mapping.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm.h"
#include <stdio.h>
#include <string.h>

static struct {
    arm_cpu_t cpu;
    arm_bus_t bus;
    uint32_t words[0x10000u/4u];
    uint32_t reject_address;
    unsigned reads;
    bool failed;
} f;
static unsigned passed, failed;
#define CHECK(c, msg) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: %s\n", __func__, __LINE__, msg); } } while (0)
static uint32_t read32(void *ctx, uint32_t address) {
    (void)ctx;
    f.reads++;
    if (f.failed || address==f.reject_address || (address&3u) || address>=sizeof f.words) {
        f.failed=true;
        return 0u;
    }
    return f.words[address/4u];
}
static bool access_failed(void *ctx) { (void)ctx; return f.failed; }

typedef struct { uint32_t va, pa, l1, descriptor; } mapping_t;
static mapping_t setup(unsigned shape, unsigned encoding, bool shareable) {
    /* Section, 16 MiB supersection, 64 KiB large page, 4 KiB small page.
     * Addresses deliberately contain nonzero offsets at each page scale. */
    static const uint32_t va[]={0x81234321u,0x81a34321u,0x812a4321u,0x81234321u};
    static const uint32_t pa[]={0x02034321u,0x02a34321u,0x002a4321u,0x002a0321u};
    static const uint32_t descriptor[]={0x02000c02u,0x02040c02u,0x002a0031u,0x002a0032u};
    memset(&f,0,sizeof f);
    f.reject_address=UINT32_MAX;
    f.bus=(arm_bus_t){.read32=read32,.access_failed=access_failed};
    CHECK(arm_reset_profile(&f.cpu,&f.bus,ARM_ARCH_V7_CORTEX_A8),"reset");
    f.cpu.cp15.sctlr=ARM_SCTLR_M|ARM_SCTLR_XP;
    f.cpu.cp15.ttbr0=0x4000u;
    f.cpu.cp15.dacr=1u;
    mapping_t m={va[shape],pa[shape],0x4000u+4u*(va[shape]>>20),0u};
    m.descriptor=shape<2u ? m.l1 : 0x9000u+4u*((m.va>>12)&255u);
    if (shape>=2u) f.words[m.l1/4u]=0x9001u;
    unsigned tex_shift=shape==3u ? 6u : 12u;
    f.words[m.descriptor/4u]=descriptor[shape]|((encoding>>2)<<tex_shift)|((encoding&3u)<<2)|
        (shareable ? 1u<<(shape<2u ? 16u : 10u) : 0u);
    return m;
}

static void test_all_descriptor_types(void) {
    /* Independent specification table: DDI0406C.b Table B3-10. */
    static const arm_memory_type_t expected[32]={
        ARM_MEMORY_STRONGLY_ORDERED,ARM_MEMORY_DEVICE,ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,
        ARM_MEMORY_NORMAL,ARM_MEMORY_UNIMPLEMENTED,ARM_MEMORY_UNIMPLEMENTED,ARM_MEMORY_NORMAL,
        ARM_MEMORY_DEVICE,ARM_MEMORY_UNIMPLEMENTED,ARM_MEMORY_UNIMPLEMENTED,ARM_MEMORY_UNIMPLEMENTED,
        ARM_MEMORY_UNIMPLEMENTED,ARM_MEMORY_UNIMPLEMENTED,ARM_MEMORY_UNIMPLEMENTED,ARM_MEMORY_UNIMPLEMENTED,
        ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,
        ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,
        ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,
        ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL,ARM_MEMORY_NORMAL
    };
    for (unsigned shape=0;shape<4u;shape++)
     for (unsigned encoding=0;encoding<32u;encoding++)
      for (unsigned share=0;share<2u;share++)
       for (unsigned access=0;access<3u;access++)
        for (unsigned priv=0;priv<2u;priv++) {
            mapping_t m=setup(shape,encoding,share!=0u);
            uint32_t pa=0u;
            CHECK(arm_mmu_translate(&f.cpu,m.va,(arm_access_t)access,priv!=0u,&pa)==0u && pa==m.pa,
                  "ordinary translation changed physical address");
            unsigned reads=f.reads;
            arm_memory_type_t type=(arm_memory_type_t)99;
            pa=0u;
            CHECK(arm_mmu_translate_type(&f.cpu,m.va,(arm_access_t)access,priv!=0u,&pa,&type)==0u &&
                  pa==m.pa && type==expected[encoding] && f.reads==reads && !f.failed,
                  "type query did not reuse the ordinary translation's attributes");
        }
}

static void test_cached_mapping_and_invalidation(void) {
    mapping_t m=setup(0u,3u,false);
    uint32_t pa;
    arm_memory_type_t type;
    CHECK(arm_mmu_translate_type(&f.cpu,m.va,ARM_ACCESS_READ,true,&pa,&type)==0u &&
          pa==m.pa && type==ARM_MEMORY_NORMAL,"initial Normal mapping");
    f.words[m.descriptor/4u]=0x03000c06u; /* Different PA and Device attributes. */
    unsigned reads=f.reads;
    CHECK(arm_mmu_translate_type(&f.cpu,m.va+1u,ARM_ACCESS_READ,true,&pa,&type)==0u &&
          pa==m.pa+1u && type==ARM_MEMORY_NORMAL && f.reads==reads,
          "fresh descriptor type was paired with stale cached PA");
    CHECK(arm_mmu_translate_type(&f.cpu,m.va,ARM_ACCESS_WRITE,true,&pa,&type)==0u &&
          pa==0x03034321u && type==ARM_MEMORY_DEVICE,"access class reused an unrelated cache entry");
    arm_mmu_tlb_flush(&f.cpu);
    CHECK(arm_mmu_translate_type(&f.cpu,m.va,ARM_ACCESS_READ,true,&pa,&type)==0u &&
          pa==0x03034321u && type==ARM_MEMORY_DEVICE,"TLBI retained stale type or PA");
    f.cpu.cp15.sctlr|=1u<<28;
    CHECK(!arm_mmu_translate_type(&f.cpu,m.va,ARM_ACCESS_READ,true,&pa,&type) &&
          type==ARM_MEMORY_UNIMPLEMENTED,"raw TEX remap reused cached attributes");
    f.cpu.cp15.sctlr&=~(1u<<28);
    f.cpu.cp15.sctlr|=1u<<25;
    CHECK(!arm_mmu_translate_type(&f.cpu,m.va,ARM_ACCESS_READ,true,&pa,&type) &&
          type==ARM_MEMORY_UNIMPLEMENTED,"unsupported big-endian tables certified a type");
    f.cpu.cp15.sctlr=ARM_SCTLR_M;
    CHECK(!arm_mmu_translate_type(&f.cpu,m.va,ARM_ACCESS_READ,true,&pa,&type) &&
          type==ARM_MEMORY_UNIMPLEMENTED,"legacy descriptor format certified an A8 type");
    f.cpu.cp15.sctlr|=ARM_SCTLR_XP;
    f.cpu.arch=ARM_ARCH_V6_ARM1176;
    CHECK(!arm_mmu_translate_type(&f.cpu,m.va,ARM_ACCESS_READ,true,&pa,&type) &&
          type==ARM_MEMORY_UNIMPLEMENTED,"legacy profile inherited A8 attributes");
    f.words[m.descriptor/4u]=0x04000c0eu;
    f.cpu.arch=ARM_ARCH_V7_CORTEX_A8;
    CHECK(!arm_mmu_translate_type(&f.cpu,m.va,ARM_ACCESS_READ,true,&pa,&type) &&
          pa==0x04034321u && type==ARM_MEMORY_NORMAL,"profile change retained another profile's cached result");
    /* Reproduce a stale generation-1 entry before counter wrap. */
    for (unsigned i=0;i<ARM_TLB_ENTRIES;i++)
        if (f.cpu.tlb[i].gen==f.cpu.tlb_gen) f.cpu.tlb[i].gen=1u;
    f.cpu.tlb_gen=UINT32_MAX;
    arm_mmu_tlb_flush(&f.cpu);
    f.words[m.descriptor/4u]=0x05000c06u;
    CHECK(f.cpu.tlb_gen==1u && !arm_mmu_translate_type(&f.cpu,m.va,ARM_ACCESS_READ,true,&pa,&type) &&
          pa==0x05034321u && type==ARM_MEMORY_DEVICE,"generation wrap resurrected stale attributes");
}

static void test_fault_outputs_and_retry(void) {
    for (unsigned shape=0;shape<4u;shape++)
     for (unsigned fault=0;fault<7u;fault++) {
        mapping_t m=setup(shape,3u,false);
        uint32_t good=f.words[m.descriptor/4u];
        unsigned ap_shift=shape<2u ? 10u : 4u;
        if (fault==0u) { f.cpu.cp15.sctlr|=ARM_SCTLR_FA; f.words[m.descriptor/4u]&=~(1u<<ap_shift); }
        if (fault==1u) f.cpu.cp15.dacr=0u;
        if (fault==2u) f.words[m.descriptor/4u]&=~(2u<<ap_shift); /* Privileged only. */
        if (fault==3u) f.words[m.descriptor/4u]=0u;
        if (fault==4u) f.words[m.descriptor/4u]|=1u<<(shape<2u ? 4u : shape==2u ? 15u : 0u);
        if (fault==5u) f.reject_address=m.l1;
        if (fault==6u) f.reject_address=m.descriptor;
        for (unsigned retry=0;retry<2u;retry++) {
            uint32_t pa=0xdeadbeefu;
            arm_memory_type_t type=(arm_memory_type_t)99;
            unsigned reads=f.reads;
            uint32_t fsr=arm_mmu_translate_type(&f.cpu,m.va,fault==4u ? ARM_ACCESS_FETCH : ARM_ACCESS_READ,false,&pa,&type);
            CHECK(fsr!=0u && (fault<5u || fsr==ARM_MMU_BUS_FAILURE) && pa==0xdeadbeefu && type==(arm_memory_type_t)99,
                  "fault changed translation outputs or lost checked-bus failure");
            if (retry && fault>0u) CHECK(f.reads==reads,"cached fault or latched host failure rewalked tables");
        }
        if (fault==0u || fault>=5u) {
            f.words[m.descriptor/4u]=good; f.failed=false; f.reject_address=UINT32_MAX;
            uint32_t pa=0u;
            arm_memory_type_t type=ARM_MEMORY_UNIMPLEMENTED;
            CHECK(!arm_mmu_translate_type(&f.cpu,m.va,ARM_ACCESS_READ,false,&pa,&type) &&
                  pa==m.pa && type==ARM_MEMORY_NORMAL,"Access flag or host failure was cached across explicit repair");
        }
     }
}

static void test_disabled_and_unsupported_profiles(void) {
    const arm_arch_t profiles[]={ARM_ARCH_V6_ARM1176,ARM_ARCH_V7_SWIFT,ARM_ARCH_V7_CORTEX_A8};
    for (unsigned p=0;p<3u;p++)
     for (unsigned access=0;access<3u;access++) {
        mapping_t m=setup(3u,3u,false);
        f.cpu.arch=profiles[p];
        uint32_t pa;
        arm_memory_type_t type;
        if (p<2u) CHECK(!arm_mmu_translate_type(&f.cpu,m.va,(arm_access_t)access,false,&pa,&type) &&
            type==ARM_MEMORY_UNIMPLEMENTED,"another profile was assigned A8 attributes");
        f.cpu.cp15.sctlr=0u;
        unsigned reads=f.reads;
        CHECK(!arm_mmu_translate_type(&f.cpu,UINT32_MAX,(arm_access_t)access,false,&pa,&type) &&
              pa==UINT32_MAX && f.reads==reads && type==(p<2u ? ARM_MEMORY_UNIMPLEMENTED :
                  access==ARM_ACCESS_FETCH ? ARM_MEMORY_NORMAL : ARM_MEMORY_STRONGLY_ORDERED),
              "MMU-disabled memory type or identity mapping is wrong");
    }
    mapping_t m=setup(1u,3u,false);
    f.words[m.descriptor/4u]|=0x00100020u; /* Extended physical-address bits. */
    uint32_t pa;
    arm_memory_type_t type;
    CHECK(!arm_mmu_translate_type(&f.cpu,m.va,ARM_ACCESS_READ,false,&pa,&type) &&
          type==ARM_MEMORY_UNIMPLEMENTED,"extended-PA mapping was certified by a 32-bit walker");
}

int main(void) {
    test_all_descriptor_types();
    test_cached_mapping_and_invalidation();
    test_fault_outputs_and_retry();
    test_disabled_and_unsupported_profiles();
    printf("%u passed, %u failed\n",passed,failed);
    return failed ? 1 : 0;
}
