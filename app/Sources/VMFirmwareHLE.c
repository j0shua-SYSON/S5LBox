/* See VMFirmwareHLE.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMFirmwareHLE.h"

#include "ios3_hle.h"

#include <stdint.h>
#include <string.h>

/*
 * ios3_hle.c itself owns one process-global site table, so pretending this
 * adapter supports two concurrently running guests would be dishonest.  The
 * app presents one machine controller at a time.  If that ever changes, an
 * older machine's still-installed hook sees that it is no longer `g_machine`
 * and declines; it can never borrow the newer machine's address-space state.
 */
static s5l8900_t *g_machine;
static uint32_t g_space;
static bool g_pending;

#define VM_HLE_MMU_CHUNK_BYTES 1024u

static uint32_t hle_chunk(uint32_t va, uint32_t remaining) {
    uint32_t to_boundary = VM_HLE_MMU_CHUNK_BYTES -
                           (va & (VM_HLE_MMU_CHUNK_BYTES - 1u));
    return remaining < to_boundary ? remaining : to_boundary;
}

static bool hle_read(void *opaque, uint32_t va, void *dst, uint32_t len) {
    s5l8900_t *m = (s5l8900_t *)opaque;
    uint8_t *out = (uint8_t *)dst;
    if (!m || m != g_machine || !m->ram || !out || !len || !va ||
        (uint64_t)va + len > UINT64_C(0x100000000))
        return false;

    for (uint32_t done = 0u; done < len;) {
        uint32_t current = va + done;
        uint32_t chunk = hle_chunk(current, len - done);
        uint32_t pa = 0u;
        if (arm_mmu_translate(&m->cpu, current, ARM_ACCESS_READ, false, &pa))
            return false;
        if (pa < m->ram_base ||
            (uint64_t)pa - m->ram_base + chunk > m->ram_size)
            return false;
        memcpy(out + done, m->ram + (pa - m->ram_base), chunk);
        done += chunk;
    }
    return true;
}

static bool hle_read_priv(void *opaque, uint32_t va, void *dst,
                          uint32_t len) {
    s5l8900_t *m = (s5l8900_t *)opaque;
    uint8_t *out = (uint8_t *)dst;
    if (!m || m != g_machine || !m->ram || !out || !len || !va ||
        (uint64_t)va + len > UINT64_C(0x100000000))
        return false;

    for (uint32_t done = 0u; done < len;) {
        uint32_t current = va + done;
        uint32_t chunk = hle_chunk(current, len - done);
        uint32_t pa = 0u;
        if (arm_mmu_translate(&m->cpu, current, ARM_ACCESS_READ, true, &pa))
            return false;
        if (pa < m->ram_base ||
            (uint64_t)pa - m->ram_base + chunk > m->ram_size)
            return false;
        memcpy(out + done, m->ram + (pa - m->ram_base), chunk);
        done += chunk;
    }
    return true;
}

static bool hle_read_phys(void *opaque, uint32_t pa, void *dst,
                          uint32_t len) {
    s5l8900_t *m = (s5l8900_t *)opaque;
    uint8_t *out = (uint8_t *)dst;
    if (!m || m != g_machine || !out || !len ||
        (uint64_t)pa + len > UINT64_C(0x100000000))
        return false;

    if (m->ram && pa >= m->ram_base &&
        (uint64_t)pa - m->ram_base + len <= m->ram_size) {
        memcpy(out, m->ram + (pa - m->ram_base), len);
        return true;
    }
    if (m->mbx.edram && pa >= S5L8900_MBX_BASE + S5L_MBX_SIZE &&
        (uint64_t)pa - (S5L8900_MBX_BASE + S5L_MBX_SIZE) + len <=
            S5L_MBX_EDRAM_SIZE) {
        memcpy(out, m->mbx.edram +
                    (pa - (S5L8900_MBX_BASE + S5L_MBX_SIZE)), len);
        return true;
    }
    return false;
}

static bool hle_writev(void *opaque, const ios3_hle_write_span_t *spans,
                       uint32_t count) {
    s5l8900_t *m = (s5l8900_t *)opaque;
    if (!m || m != g_machine || !m->ram || !spans || !count ||
        count > IOS3_HLE_ORACLE_MAX_SPANS)
        return false;

    /* Translate every byte range before publishing any one of them.  ARMv6
     * small/tiny-page permissions can change at 1 KiB, hence the chunk size. */
    for (uint32_t s = 0u; s < count; s++) {
        if (!spans[s].src || !spans[s].len || !spans[s].va ||
            (uint64_t)spans[s].va + spans[s].len >
                UINT64_C(0x100000000))
            return false;
        for (uint32_t done = 0u; done < spans[s].len;) {
            uint32_t current = spans[s].va + done;
            uint32_t chunk = hle_chunk(current, spans[s].len - done);
            uint32_t pa = 0u;
            if (arm_mmu_translate(&m->cpu, current, ARM_ACCESS_WRITE, false,
                                  &pa))
                return false;
            if (pa < m->ram_base ||
                (uint64_t)pa - m->ram_base + chunk > m->ram_size)
                return false;
            done += chunk;
        }
    }

    for (uint32_t s = 0u; s < count; s++) {
        const uint8_t *bytes = (const uint8_t *)spans[s].src;
        for (uint32_t done = 0u; done < spans[s].len;) {
            uint32_t current = spans[s].va + done;
            uint32_t chunk = hle_chunk(current, spans[s].len - done);
            uint32_t pa = 0u;
            (void)arm_mmu_translate(&m->cpu, current, ARM_ACCESS_WRITE,
                                    false, &pa);
            memcpy(m->ram + (pa - m->ram_base), bytes + done, chunk);
            done += chunk;
        }
    }
    return true;
}

static bool hle_write(void *opaque, uint32_t va, const void *src,
                      uint32_t len) {
    ios3_hle_write_span_t span = {va, src, len};
    return hle_writev(opaque, &span, 1u);
}

static uint32_t ttbr0_base(const arm_cpu_t *cpu) {
    unsigned n = cpu->cp15.ttbcr & 7u;
    return cpu->cp15.ttbr0 & (UINT32_MAX << (14u - n));
}

static void note_pending(void) {
    g_pending = false;
    for (unsigned i = 0u; i < ios3_hle_site_count(); i++) {
        const ios3_hle_site_t *site = ios3_hle_site_at(i);
        if (site && !site->armed) {
            g_pending = true;
            return;
        }
    }
}

static const ios3_hle_site_t *site_at_pc(uint32_t pc) {
    for (unsigned i = 0u; i < ios3_hle_site_count(); i++) {
        const ios3_hle_site_t *site = ios3_hle_site_at(i);
        if (site && site->va == pc) return site;
    }
    return NULL;
}

static bool hle_pre_step(void *opaque) {
    s5l8900_t *m = (s5l8900_t *)opaque;
    ios3_hle_mem_t mem;
    uint32_t pc;
    uint32_t space;
    const ios3_hle_site_t *site;

    if (!m || m != g_machine ||
        (m->cpu.cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR)
        return false;

    memset(&mem, 0, sizeof mem);
    mem.ctx = m;
    mem.read = hle_read;
    mem.write = hle_write;
    mem.read_priv = hle_read_priv;
    mem.read_phys = hle_read_phys;
    mem.writev = hle_writev;
    pc = m->cpu.r[15] & ~UINT32_C(1);
    space = ttbr0_base(&m->cpu);
    site = site_at_pc(pc);
    if (!site) return false;

    if (!g_space) {
        if (!ios3_hle_arm(&mem, space)) return false;
        g_space = space;
        note_pending();
    } else if (g_pending && space == g_space && !site->armed) {
        (void)ios3_hle_arm(&mem, g_space);
        note_pending();
    }
    return ios3_hle_step(&m->cpu, &mem, pc, space);
}

bool vm_firmware_hle_enable(s5l8900_t *machine) {
    uint32_t targets[S5L_PRE_STEP_TARGET_MAX];
    unsigned count = 0u;
    if (!machine) return false;

    for (unsigned i = 0u; i < ios3_hle_site_count(); i++) {
        const ios3_hle_site_t *site = ios3_hle_site_at(i);
        if (!site || site->mode != IOS3_HLE_REPLACE || !site->handler)
            continue;
        if (count >= S5L_PRE_STEP_TARGET_MAX) return false;
        targets[count++] = site->va;
    }
    if (!count) return false;

    ios3_hle_disarm();
    g_machine = machine;
    g_space = 0u;
    g_pending = false;
    if (!s5l8900_set_pre_step_hook(machine, hle_pre_step, machine,
                                   targets, count)) {
        g_machine = NULL;
        ios3_hle_disarm();
        return false;
    }
    return true;
}

void vm_firmware_hle_release(const s5l8900_t *machine) {
    if (!machine || machine != g_machine) return;
    /* The machine may already have been freed by the owning engine.  Do not
     * dereference it here; making the global gate reject it is sufficient. */
    g_machine = NULL;
    g_space = 0u;
    g_pending = false;
    ios3_hle_disarm();
}

bool vm_firmware_hle_active(const s5l8900_t *machine) {
    return machine && machine == g_machine;
}

uint32_t vm_firmware_hle_space(const s5l8900_t *machine) {
    return vm_firmware_hle_active(machine) ? g_space : 0u;
}
