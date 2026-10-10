// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#include "VMFirmwareAudio.h"
#include "sha256.h"
#include <string.h>

/* 7E18 AudioQueueObject::ChooseCodec: the default-policy branch (3354ced8)
 * goes to the guest's existing UseSoftwareOnly path at 3354cf34. Explicit
 * policies 1..4 keep their switch entries. AMC decoding is not implemented;
 * claiming a working hardware decoder produces silent ALAC/AAC queues.
 *
 * This is an explicit compatibility policy, NOT AMC emulation or native
 * decoding. The guest still reads, decodes and mixes its own audio. The full
 * 1 KiB executable window is authenticated before changing one instruction.
 * Its digest was compared with both the cache file and physical-device RAM.
 * Selection occurs on a successful user FETCH miss, so remapping, eviction
 * and snapshot restore cannot borrow a different process's physical page.
 * No per-instruction hook, RAM scan, disk mutation or cached host pointer.
 */
#define AUDIO_WINDOW UINT32_C(0x3354cc00)
#define AUDIO_WINDOW_BYTES 1024u
#define AUDIO_BRANCH_OFFSET 0x2d8u
static const uint8_t original_branch[4] = {0x03, 0x00, 0x00, 0xea};
static const uint8_t software_branch[4] = {0x15, 0x00, 0x00, 0xea};
static const uint8_t original_digest[32] = {
    0x14,0x34,0xa7,0xa4,0xbd,0x9c,0x61,0x3d,
    0x08,0x91,0x84,0x44,0xa8,0x1e,0x90,0x5e,
    0x14,0xa1,0xd5,0xa6,0x9c,0x7f,0x4d,0x4c,
    0xff,0xda,0xfe,0xbb,0xfd,0x78,0xbb,0xbf
};

static void prepare_audio(void *opaque, uint32_t va, uint32_t pa, bool priv) {
    s5l8900_t *m = opaque;
    if (!m || !m->ram || priv || m->cpu.arch != ARM_ARCH_V6_ARM1176 ||
        (va & ~UINT32_C(1023)) != AUDIO_WINDOW ||
        (va & 1023u) != (pa & 1023u))
        return;
    uint32_t base = pa & ~UINT32_C(1023);
    if (base < m->ram_base ||
        (uint64_t)base - m->ram_base + AUDIO_WINDOW_BYTES > m->ram_size)
        return;
    const uint8_t *page = m->ram + (base - m->ram_base);
    if (memcmp(page + AUDIO_BRANCH_OFFSET, original_branch, 4u) != 0)
        return; /* Already patched, or a different instruction: never guess. */
    uint8_t digest[32];
    if (!ios3_sha256(page, AUDIO_WINDOW_BYTES, digest) ||
        memcmp(digest, original_digest, sizeof digest) != 0)
        return;
    /* Use the normal RAM publication path so native read proofs see the write.
     * The instruction has not been exposed by this FETCH translation yet. */
    s5l8900_load(m, base + AUDIO_BRANCH_OFFSET, software_branch, 4u);
}

void vm_firmware_audio_enable(s5l8900_t *m) {
    if (!m) return;
    m->bus.prepare_fetch = prepare_audio;
    m->bus.prepare_fetch_ctx = m;
    arm_mmu_tlb_flush(&m->cpu);
}
