/* PL192 register and guest interrupt-flow tests, DDI0273A chapters2/3.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "pl192.h"
#include "arm.h"
#include <stdio.h>
#include <string.h>

static unsigned passed, failed;
#define CHECK(test) do { if (test) passed++; else { failed++; \
    printf("FAIL %s:%d: %s\n", __func__, __LINE__, #test); } } while (0)

static uint32_t rd(pl192_t *v, uint32_t offset) {
    uint32_t result = 0xdeadbeefu;
    CHECK(pl192_read(v, offset, true, &result));
    return result;
}
static void wr(pl192_t *v, uint32_t offset, uint32_t value) {
    CHECK(pl192_write(v, offset, true, value));
}

static void test_reset_and_source_routing(void) {
    pl192_t v;
    memset(&v, 0xa5, sizeof v);
    pl192_reset(&v);
    CHECK(!pl192_irq(&v) && !pl192_fiq(&v) && pl192_vector(&v) == 0u);
    CHECK(rd(&v, PL192_INTENABLE) == 0u && rd(&v, PL192_INTSELECT) == 0u && rd(&v, PL192_SOFTINT) == 0u);
    CHECK(rd(&v, PL192_PROTECTION) == 0u && rd(&v, PL192_SWPRIORITYMASK) == 0xffffu && rd(&v, PL192_PRIORITYDAISY) == 15u);
    const uint32_t ids[] = {0x92u,0x11u,4u,0u,0x0du,0xf0u,5u,0xb1u};
    for (unsigned i = 0; i < 8u; i++) CHECK(rd(&v, 0xfe0u + 4u * i) == ids[i]);
    for (unsigned i = 0; i < 32u; i++) {
        pl192_reset(&v);
        uint32_t bit = 1u << i;
        CHECK(rd(&v, PL192_VECTADDR0 + 4u * i) == 0u && rd(&v, PL192_VECTPRIORITY0 + 4u * i) == 15u);
        CHECK(pl192_set_line(&v, i, true));
        CHECK(rd(&v, PL192_RAWINTR) == bit && !pl192_irq(&v));
        wr(&v, PL192_INTENABLE, bit); wr(&v, PL192_INTENABLE, 0u);
        CHECK(rd(&v, PL192_IRQSTATUS) == bit && pl192_irq(&v) && !pl192_fiq(&v));
        /* Address zero is a legitimate programmed vector, not "no IRQ". */
        CHECK(rd(&v, PL192_ADDRESS) == 0u && !pl192_irq(&v) && rd(&v, PL192_IRQSTATUS) == bit);
        wr(&v, PL192_ADDRESS, 0xffffffffu);
        CHECK(pl192_irq(&v));
        wr(&v, PL192_SOFTINT, bit); wr(&v, PL192_SOFTINT, 0u);
        CHECK(rd(&v, PL192_SOFTINT) == bit);
        wr(&v, PL192_SOFTINTCLEAR, bit);
        CHECK(rd(&v, PL192_SOFTINT) == 0u && rd(&v, PL192_RAWINTR) == bit);
        wr(&v, PL192_INTENCLEAR, bit);
        wr(&v, PL192_INTSELECT, bit); /* Change routing only while disabled. */
        wr(&v, PL192_INTENABLE, bit); wr(&v, PL192_SWPRIORITYMASK, 0u);
        CHECK(pl192_fiq(&v) && !pl192_irq(&v) && rd(&v, PL192_FIQSTATUS) == bit && rd(&v, PL192_IRQSTATUS) == 0u);
        CHECK(pl192_set_line(&v, i, false));
        CHECK(!pl192_fiq(&v));
        wr(&v, PL192_SOFTINT, bit);
        CHECK(pl192_fiq(&v));
        wr(&v, PL192_SOFTINTCLEAR, bit);
        CHECK(!pl192_fiq(&v) && rd(&v, PL192_RAWINTR) == 0u);
    }
}

static void test_priorities_vectors_and_nesting(void) {
    pl192_t v;
    for (unsigned source = 0; source < 32u; source++) {
     for (unsigned priority = 0; priority < 16u; priority++) {
        pl192_reset(&v);
        uint32_t vector = 0x12340001u + 4u * source;
        wr(&v, PL192_VECTADDR0 + 4u * source, vector);
        wr(&v, PL192_VECTPRIORITY0 + 4u * source, priority);
        CHECK(rd(&v, PL192_VECTADDR0 + 4u * source) == vector && rd(&v, PL192_VECTPRIORITY0 + 4u * source) == priority);
        wr(&v, PL192_INTENABLE, 1u << source); wr(&v, PL192_SOFTINT, 1u << source);
        wr(&v, PL192_SWPRIORITYMASK, 0xffffu & ~(1u << priority));
        CHECK(!pl192_irq(&v) && rd(&v, PL192_IRQSTATUS) == (1u << source));
        wr(&v, PL192_SWPRIORITYMASK, 1u << priority);
        CHECK(pl192_irq(&v) && pl192_vector(&v) == vector && rd(&v, PL192_ADDRESS) == vector);
        CHECK(!pl192_irq(&v));
        wr(&v, PL192_SOFTINTCLEAR, 1u << source); wr(&v, PL192_ADDRESS, 0u);
        CHECK(!pl192_irq(&v) && pl192_vector(&v) == vector);
     }
    }
    pl192_reset(&v);
    wr(&v, PL192_VECTADDR0 + 12u, 0x12345678u); wr(&v, PL192_VECTADDR0 + 36u, 0x87654321u);
    wr(&v, PL192_VECTPRIORITY0 + 12u, 10u); wr(&v, PL192_VECTPRIORITY0 + 36u, 2u);
    wr(&v, PL192_INTENABLE, (1u << 3) | (1u << 9));
    wr(&v, PL192_SOFTINT, (1u << 3) | (1u << 9));
    CHECK(rd(&v, PL192_ADDRESS) == 0x87654321u && !pl192_irq(&v));
    wr(&v, PL192_SOFTINTCLEAR, 1u << 9);
    CHECK(!pl192_irq(&v)); /* Lower priority remains pending but cannot preempt. */
    wr(&v, PL192_ADDRESS, 0u);
    CHECK(pl192_irq(&v) && rd(&v, PL192_ADDRESS) == 0x12345678u);

    /* All sixteen priority levels can nest; EOI restores the outer mask. */
    pl192_reset(&v);
    for (unsigned i = 0; i < 16u; i++) {
        wr(&v, PL192_VECTPRIORITY0 + 4u * i, i);
        wr(&v, PL192_VECTADDR0 + 4u * i, 0x2000u + 4u * i);
    }
    wr(&v, PL192_INTENABLE, 0xffffu);
    for (unsigned i = 16u; i-- > 0u;) {
        wr(&v, PL192_SOFTINT, 1u << i);
        CHECK(pl192_irq(&v) && rd(&v, PL192_ADDRESS) == 0x2000u + 4u * i && !pl192_irq(&v));
    }
    for (unsigned i = 0; i < 16u; i++) {
        wr(&v, PL192_SOFTINTCLEAR, 1u << i);
        wr(&v, PL192_ADDRESS, 0xffffffffu);
        CHECK(!pl192_irq(&v));
    }
    CHECK(v.in_service == 0u);
    /* Equal-priority source order applies only before acknowledgement. */
    pl192_reset(&v);
    wr(&v, PL192_VECTADDR0, 0x11111111u); wr(&v, PL192_VECTADDR0 + 124u, 0xeeeeeeeeu);
    wr(&v, PL192_INTENABLE, 0x80000001u); wr(&v, PL192_SOFTINT, 0x80000000u);
    CHECK(rd(&v, PL192_ADDRESS) == 0xeeeeeeeeu);
    wr(&v, PL192_SOFTINT, 1u);
    CHECK(!pl192_irq(&v));
    wr(&v, PL192_SOFTINTCLEAR, 0x80000000u); wr(&v, PL192_ADDRESS, 0u);
    CHECK(rd(&v, PL192_ADDRESS) == 0x11111111u);
    wr(&v, PL192_SOFTINTCLEAR, 1u); wr(&v, PL192_ADDRESS, 0u);
    wr(&v, PL192_SOFTINT, 0x80000001u);
    CHECK(rd(&v, PL192_ADDRESS) == 0x11111111u);
    /* A request withdrawn before acknowledgement still leaves its vector. */
    pl192_reset(&v); wr(&v, PL192_VECTADDR0, 0xabcdef01u); wr(&v, PL192_INTENABLE, 1u);
    CHECK(pl192_set_line(&v, 0u, true) && pl192_vector(&v) == 0xabcdef01u);
    CHECK(pl192_set_line(&v, 0u, false) && !pl192_irq(&v) && pl192_vector(&v) == 0xabcdef01u);
}

static void test_checked_access(void) {
    pl192_t v;
    pl192_reset(&v);
    uint32_t value = 0xdeadbeefu;
    CHECK(pl192_write(&v, PL192_INTENABLE, false, 1u));
    CHECK(pl192_read(&v, PL192_INTENABLE, false, &value) && value == 1u);
    CHECK(!pl192_read(&v, PL192_PROTECTION, false, &value) && value == 1u);
    CHECK(!pl192_write(&v, PL192_PROTECTION, false, 1u) && !v.protection);
    wr(&v, PL192_SOFTINT, 1u); wr(&v, PL192_PROTECTION, 1u);
    pl192_t before = v;
    for (unsigned offset = 0; offset < 0x1004u; offset++) {
        value = 0xdeadbeefu;
        CHECK(!pl192_read(&v, offset, false, &value) && value == 0xdeadbeefu);
        CHECK(!pl192_write(&v, offset, false, 0xffffffffu));
    }
    CHECK(!memcmp(&v, &before, sizeof v) && pl192_irq(&v));
    wr(&v, PL192_PROTECTION, 0u);
    const uint32_t bad[] = {3u,0x02cu,0x0fcu,0x180u,0x280u,0x300u,0x1000u,0xffffffffu};
    before = v;
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        value = 0x12345678u;
        CHECK(!pl192_read(&v, bad[i], true, &value) && value == 0x12345678u);
        CHECK(!pl192_write(&v, bad[i], true, 0u));
    }
    CHECK(!pl192_read(&v, PL192_ADDRESS, true, NULL));
    CHECK(!pl192_set_line(&v, 32u, true) && !pl192_set_line(&v, 0xffffffffu, true));
    CHECK(!pl192_write(&v, PL192_PROTECTION, true, 2u));
    CHECK(!pl192_write(&v, PL192_SWPRIORITYMASK, true, 0x10000u));
    CHECK(!pl192_write(&v, PL192_PRIORITYDAISY, true, 16u));
    CHECK(!pl192_write(&v, PL192_VECTPRIORITY0, true, 16u));
    CHECK(!pl192_write(&v, 0xfe0u, true, 0u));
    CHECK(!pl192_read(&v, PL192_INTENCLEAR, true, &value));
    CHECK(!pl192_read(&v, PL192_SOFTINTCLEAR, true, &value));
    CHECK(!memcmp(&v, &before, sizeof v));
}

static void connect_three(pl192_t v[3]) {
    for (unsigned i = 2u; i-- > 0u;)
        pl192_set_daisy(&v[i], pl192_irq(&v[i + 1u]), pl192_fiq(&v[i + 1u]), pl192_vector(&v[i + 1u]));
}

static void test_daisy_blocking(void) {
    pl192_t v[3];
    for (unsigned i = 0; i < 3u; i++) pl192_reset(&v[i]);
    wr(&v[2], PL192_VECTADDR0 + 28u, 0x12345679u); wr(&v[2], PL192_INTENABLE, 1u << 7);
    wr(&v[2], PL192_SOFTINT, 1u << 7); connect_three(v);
    CHECK(pl192_irq(&v[0]) && rd(&v[0], PL192_IRQSTATUS) == 0u && rd(&v[1], PL192_IRQSTATUS) == 0u);
    CHECK(rd(&v[0], PL192_ADDRESS) == 0x12345679u && !pl192_irq(&v[0]));
    CHECK(pl192_irq(&v[1]) && pl192_irq(&v[2]) && !v[1].in_service && !v[2].in_service);
    wr(&v[0], PL192_VECTADDR0, 0x11223344u); wr(&v[0], PL192_VECTPRIORITY0, 2u);
    wr(&v[0], PL192_INTENABLE, 1u); wr(&v[0], PL192_SOFTINT, 1u);
    CHECK(pl192_irq(&v[0]) && rd(&v[0], PL192_ADDRESS) == 0x11223344u);
    wr(&v[0], PL192_SOFTINTCLEAR, 1u); wr(&v[0], PL192_ADDRESS, 0u);
    CHECK(!pl192_irq(&v[0])); /* Outer daisy ISR still owns its level. */
    wr(&v[2], PL192_SOFTINTCLEAR, 1u << 7); connect_three(v); wr(&v[0], PL192_ADDRESS, 0u);
    CHECK(!pl192_irq(&v[0]) && !v[0].in_service);
    /* Daisy zero vector, local ties, programmable daisy priority and FIQ. */
    pl192_reset(&v[0]); pl192_set_daisy(&v[0], true, true, 0u);
    CHECK(pl192_irq(&v[0]) && pl192_fiq(&v[0]) && pl192_vector(&v[0]) == 0u);
    CHECK(rd(&v[0], PL192_FIQSTATUS) == 0u); /* Daisy is not a local source. */
    wr(&v[0], PL192_VECTADDR0, 0x99887766u); wr(&v[0], PL192_INTENABLE, 1u); wr(&v[0], PL192_SOFTINT, 1u);
    CHECK(pl192_vector(&v[0]) == 0x99887766u);
    wr(&v[0], PL192_PRIORITYDAISY, 3u);
    CHECK(pl192_vector(&v[0]) == 0u && rd(&v[0], PL192_ADDRESS) == 0u && !pl192_irq(&v[0]));
    wr(&v[0], PL192_SWPRIORITYMASK, 0u);
    CHECK(pl192_fiq(&v[0]) && !pl192_irq(&v[0]));
}

typedef struct test_machine {
    arm_cpu_t cpu;
    pl192_t vic[3];
    uint8_t ram[256];
    unsigned waits, bad_accesses;
} test_machine_t;

static void sync_interrupts(test_machine_t *m) {
    connect_three(m->vic);
    m->cpu.irq_line = pl192_irq(&m->vic[0]);
    m->cpu.fiq_line = pl192_fiq(&m->vic[0]);
}
static uint32_t bus_read32(void *ctx, uint32_t address) {
    test_machine_t *m = ctx;
    uint32_t value = 0;
    if (address <= sizeof m->ram - 4u) memcpy(&value, m->ram + address, 4u);
    else if (address >= 0xbf200000u && address < 0xbf230000u) {
        unsigned bank = (address - 0xbf200000u) >> 16;
        if (!pl192_read(&m->vic[bank], address & 0xffffu,
                        (m->cpu.cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR, &value)) m->bad_accesses++;
        sync_interrupts(m);
    } else m->bad_accesses++;
    return value;
}
static uint16_t bus_read16(void *ctx, uint32_t address) { return (uint16_t)(bus_read32(ctx, address & ~3u) >> (8u * (address & 2u))); }
static uint8_t bus_read8(void *ctx, uint32_t address) { return (uint8_t)(bus_read32(ctx, address & ~3u) >> (8u * (address & 3u))); }
static void bus_write32(void *ctx, uint32_t address, uint32_t value) {
    test_machine_t *m = ctx;
    if (address <= sizeof m->ram - 4u) memcpy(m->ram + address, &value, 4u);
    else if (address >= 0xbf200000u && address < 0xbf230000u) {
        unsigned bank = (address - 0xbf200000u) >> 16;
        if (!pl192_write(&m->vic[bank], address & 0xffffu,
                         (m->cpu.cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR, value)) m->bad_accesses++;
        sync_interrupts(m);
    } else m->bad_accesses++;
}
static void bus_write16(void *ctx, uint32_t address, uint16_t value) { (void)address; (void)value; ((test_machine_t *)ctx)->bad_accesses++; }
static void bus_write8(void *ctx, uint32_t address, uint8_t value) { (void)address; (void)value; ((test_machine_t *)ctx)->bad_accesses++; }
static bool wake_machine(void *ctx) {
    test_machine_t *m = ctx;
    m->waits++;
    CHECK(pl192_set_line(&m->vic[2], 7u, true));
    sync_interrupts(m);
    return m->cpu.irq_line;
}

static void test_cortex_a8_guest_interrupt_flow(void) {
    /* N88 DT: three banks at bf200000 + bank*10000. Guest programs a
     * vector in bank2, waits, dispatches through bank0, masks the source,
     * ends the outer daisy service and returns from IRQ to the WFI successor. */
    test_machine_t m;
    memset(&m, 0, sizeof m);
    for (unsigned i = 0; i < 3u; i++) pl192_reset(&m.vic[i]);
    arm_bus_t bus = {.ctx=&m, .read32=bus_read32, .read16=bus_read16, .read8=bus_read8,
                    .write32=bus_write32, .write16=bus_write16, .write8=bus_write8, .wait_for_interrupt=wake_machine};
    CHECK(arm_reset_profile(&m.cpu, &bus, ARM_ARCH_V7_CORTEX_A8));
    m.cpu.cpsr = ARM_MODE_SYS | ARM_CPSR_F;
    m.cpu.r[0] = 0xbf200000u; m.cpu.r[1] = 0xbf220000u; m.cpu.r[2] = 1u << 7; m.cpu.r[3] = 0x40u;
    const uint32_t main_code[] = {0xe581311cu,0xe5812010u,0xe320f003u,0xe3a0405au};
    const uint32_t handler[] = {0xe2866001u,0xe5812014u,0xe5807f00u,0xe25ef004u};
    memcpy(m.ram, main_code, sizeof main_code); memcpy(m.ram + 0x40u, handler, sizeof handler);
    bus_write32(&m, ARM_VEC_IRQ, 0xe590ff00u); /* LDR pc,[r0,#0xf00] */
    for (unsigned step = 0; step < 10u; step++) {
        sync_interrupts(&m);
        CHECK(arm_step(&m.cpu) == ARM_OK && !m.bad_accesses);
        if (step == 2u) CHECK(m.waits == 1u && m.cpu.r[15] == 12u && m.cpu.irq_line);
        if (step == 3u) CHECK(m.cpu.r[15] == ARM_VEC_IRQ && m.cpu.r[14] == 16u);
        if (step == 4u) CHECK(m.cpu.r[15] == 0x40u && !m.cpu.irq_line);
    }
    CHECK(m.cpu.r[4] == 0x5au && m.cpu.r[6] == 1u && m.cpu.r[15] == 16u && m.cpu.cpsr == (ARM_MODE_SYS | ARM_CPSR_F));
    CHECK(m.waits == 1u && !m.bad_accesses && !m.cpu.irq_line && !m.vic[0].in_service);
    CHECK(rd(&m.vic[2], PL192_RAWINTR) == (1u << 7) && rd(&m.vic[2], PL192_INTENABLE) == 0u);
}

int main(void) {
    test_reset_and_source_routing();
    test_priorities_vectors_and_nesting();
    test_checked_access();
    test_daisy_blocking();
    test_cortex_a8_guest_interrupt_flow();
    printf("%u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
