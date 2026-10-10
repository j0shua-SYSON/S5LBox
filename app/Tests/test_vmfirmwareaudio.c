// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#include "VMFirmwareAudio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(c, msg) do { checks++; if (!(c)) { failures++; puts(msg); } } while (0)
#define VA UINT32_C(0x3354cc00)
#define PA UINT32_C(0x08004000)
#define OFF 0x2d8u

static void prepare(s5l8900_t *m, uint32_t va, uint32_t pa, bool priv) {
    m->bus.prepare_fetch(m->bus.prepare_fetch_ctx, va, pa, priv);
}

int main(void) {
    s5l8900_t *m = calloc(1u, sizeof *m);
    CHECK(m && s5l8900_init(m, 0x08000000u, 1u << 20), "machine init failed");
    if (!m || !m->ram) return 1;
    vm_firmware_audio_enable(NULL);
    vm_firmware_audio_enable(m);
    CHECK(m->bus.prepare_fetch && m->bus.prepare_fetch_ctx == m &&
          !m->pre_step_hook, "policy used an execution hook or wrong context");
    uint8_t page[1024] = {0};
    page[OFF] = 3u; page[OFF + 3u] = 0xeau;
    s5l8900_load(m, PA, page, sizeof page);
    prepare(m, VA, PA, false);
    CHECK(!memcmp(m->ram + 0x4000u, page, sizeof page),
          "branch match without full-window identity changed RAM");

    /* Apple bytes stay private. Public CI proves refusal; a local exact-cache
     * run additionally proves the positive identity and unchanged neighbours. */
    const char *path = getenv("S5LBOX_TEST_AUDIO_CACHE");
    if (path && *path) {
        FILE *f = fopen(path, "rb");
        CHECK(f != NULL, "private audio cache could not be opened");
        if (f) {
            bool read_ok = fseek(f, 0x0354cc00L, SEEK_SET) == 0 &&
                fread(page, 1u, sizeof page, f) == sizeof page;
            fclose(f);
            CHECK(read_ok, "private audio code window could not be read");
            if (read_ok) {
                s5l8900_load(m, PA, page, sizeof page);
                prepare(m, VA, PA, true);
                prepare(m, VA + 1024u, PA, false);
                prepare(m, VA, PA + 1u, false);
                prepare(m, VA, 0u, false);
                prepare(m, VA, m->ram_base + m->ram_size, false);
                m->cpu.arch = ARM_ARCH_V7_SWIFT;
                prepare(m, VA, PA, false);
                m->cpu.arch = ARM_ARCH_V6_ARM1176;
                CHECK(!memcmp(m->ram + 0x4000u, page, sizeof page),
                      "privilege/address/range refusal changed RAM");
                m->ram[0x4000u + 7u] ^= 1u;
                prepare(m, VA, PA, false);
                CHECK(m->ram[0x4000u + OFF] == 3u,
                      "wrong code-page digest was accepted");
                s5l8900_load(m, PA, page, sizeof page);
                prepare(m, VA + 0x200u, PA + 0x200u, false);
                page[OFF] = 0x15u;
                CHECK(!memcmp(m->ram + 0x4000u, page, sizeof page),
                      "exact code window did not receive only the default branch");
                prepare(m, VA, PA, false);
                CHECK(!memcmp(m->ram + 0x4000u, page, sizeof page),
                      "repeat FETCH changed an already-patched window");
                /* Restored/remapped original bytes get the same guarded fix,
                 * without retaining any physical page pointer or ASID. */
                page[OFF] = 3u;
                s5l8900_load(m, PA + 0x800u, page, sizeof page);
                prepare(m, VA, PA + 0x800u, false);
                page[OFF] = 0x15u;
                CHECK(!memcmp(m->ram + 0x4800u, page, sizeof page),
                      "remapped code window reused stale physical state");

                /* Execute the real switch, entering through an ordinary FETCH
                 * miss. Only default changes; the four explicit policies keep
                 * their original guest targets. No Apple bytes in public CI. */
                const uint32_t targets[5] = {
                    0x3354cf34u, 0x3354cf34u, 0x3354cf60u,
                    0x3354cf4cu, 0x3354cf54u
                };
                const uint8_t section[4] = {2u, 12u, 0u, 8u};
                s5l8900_load(m, m->ram_base + 0x335u * 4u,
                            section, sizeof section);
                for (unsigned policy = 0u; policy < 5u; policy++) {
                    page[OFF] = 3u;
                    s5l8900_load(m, 0x0804cc00u, page, sizeof page);
                    arm_reset(&m->cpu, &m->bus);
                    arm_set_mode(&m->cpu, ARM_MODE_USR);
                    m->cpu.cp15.ttbr0 = m->ram_base;
                    m->cpu.cp15.dacr = 1u;
                    m->cpu.cp15.sctlr = ARM_SCTLR_M;
                    m->cpu.r[15] = 0x3354ced0u;
                    m->cpu.r[3] = policy - 1u;
                    CHECK(arm_step(&m->cpu) == ARM_OK &&
                          arm_step(&m->cpu) == ARM_OK &&
                          arm_step(&m->cpu) == ARM_OK &&
                          m->cpu.r[15] == targets[policy],
                          "executed codec switch selected the wrong target");
                    CHECK(m->ram[0x4cc00u + OFF] == 0x15u,
                          "real FETCH did not prepare the firmware branch");
                }
            }
        }
    } else puts("SKIP exact 7E18 page: set S5LBOX_TEST_AUDIO_CACHE");
    s5l8900_free(m); free(m);
    printf("firmware audio: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
