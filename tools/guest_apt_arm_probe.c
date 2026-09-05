/* Execute the original and offline replacement ARM code against one bounded
 * test pool. This is a calling-convention/semantic probe, not a real OS timing.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "arm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE 0x800000u
#define GENERATOR 0x180000u
#define INPUT 0x181000u
#define RETURN_PC 0x182000u
#define STRLEN_CODE 0x183000u
#define MAP_TOKEN 0x190000u
#define CACHE 0x200000u
#define CACHE_SIZE 0x100000u
#define HEAP 0x400000u
#define STACK 0x7f0000u
#define WRITE_PC 0x65f50u
#define D1_PC 0x696d4u
#define D2_PC 0x68994u

static uint8_t ram[RAM_SIZE], expected_cache[CACHE_SIZE];
static arm_cpu_t cpu;
static unsigned failures, checks, fault, items, strings, mmap_calls;
static unsigned live, peak_live, max_heap_slot;
static int fail_mmap_at = -1, fail_pool_item, fail_pool_string;
static uint32_t heap_lengths[64];
static uint64_t retired;

#define CHECK(c, ...) do { checks++; if (!(c)) { \
    if (failures++ < 20) { printf("line %d: ", __LINE__); \
        printf(__VA_ARGS__); puts(""); } } } while (0)

static int bounds(uint32_t a, uint32_t n) {
    if (a > RAM_SIZE || n > RAM_SIZE - a) { fault = a ? a : 1; return 0; }
    return 1;
}
static uint32_t r32(void *c, uint32_t a) {
    uint32_t v = 0; (void)c; if (bounds(a, 4)) memcpy(&v, ram + a, 4); return v;
}
static uint16_t r16(void *c, uint32_t a) {
    uint16_t v = 0; (void)c; if (bounds(a, 2)) memcpy(&v, ram + a, 2); return v;
}
static uint8_t r8(void *c, uint32_t a) { (void)c; return bounds(a, 1) ? ram[a] : 0; }
static void w32(void *c, uint32_t a, uint32_t v) { (void)c; if (bounds(a, 4)) memcpy(ram + a, &v, 4); }
static void w16(void *c, uint32_t a, uint16_t v) { (void)c; if (bounds(a, 2)) memcpy(ram + a, &v, 2); }
static void w8(void *c, uint32_t a, uint8_t v) { (void)c; if (bounds(a, 1)) ram[a] = v; }
static const arm_bus_t bus = {.read32=r32, .read16=r16, .read8=r8,
                              .write32=w32, .write16=w16, .write8=w8};

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int load_library(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return 0; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 0; }
    long end = ftell(file);
    if (end < 28 || end > 0x200000 || fseek(file, 0, SEEK_SET)) { fclose(file); return 0; }
    size_t n = (size_t)end;
    uint8_t *data = malloc(n);
    if (!data) { fclose(file); return 0; }
    int ok = fread(data, 1, n, file) == n;
    fclose(file);
    if (!ok || le32(data) != 0xfeedface || le32(data+4) != 12 || le32(data+12) != 6) goto bad;
    uint32_t count = le32(data+16), command_size = le32(data+20), at = 28;
    if (count > 64 || command_size > n - 28) goto bad;
    memset(ram, 0, sizeof ram);
    for (uint32_t i = 0; i < count; ++i) {
        if (at > n - 8) goto bad;
        uint32_t cmd = le32(data+at), size = le32(data+at+4);
        if (size < 8 || size > n - at) goto bad;
        if (cmd == 1) {
            if (size < 56) goto bad;
            uint32_t vm = le32(data+at+24), vs = le32(data+at+28);
            uint32_t off = le32(data+at+32), fs = le32(data+at+36);
            if (vm > GENERATOR || vs > GENERATOR - vm || fs > vs || off > n || fs > n-off) goto bad;
            memcpy(ram+vm, data+off, fs);
        }
        at += size;
    }
    if (at != 28 + command_size) goto bad;
    free(data);
    return 1;
bad:
    free(data);
    fprintf(stderr, "invalid bounded ARM library: %s\n", path);
    return 0;
}

static const uint32_t strlen_words[] = {
    0xe1a0c000u, 0xe2103003u, 0xe3c00003u, 0xe4902004u,
    0x0a000003u, 0xe3530002u, 0xe38220ffu, 0xa3822cffu,
    0xc38228ffu, 0xe3a01001u, 0xe1811401u, 0xe1811801u,
    0xe0423001u, 0xe1c33002u, 0xe1130381u, 0x04902004u,
    0x0afffffau, 0xe2400001u, 0xe31200ffu, 0x02400001u,
    0x13120cffu, 0x02400001u, 0x131208ffu, 0x02400001u,
    0xe040000cu, 0xe12fff1eu,
};

static void generator_reset(void) {
    memset(ram + GENERATOR, 0, 256);
    w32(NULL, GENERATOR + 0x68, MAP_TOKEN);
    w32(NULL, GENERATOR + 0x78, CACHE);
    w32(NULL, GENERATOR + 0xa0, CACHE);
    w32(NULL, GENERATOR + 0xa4, CACHE);
}

static int setup(const char *path, int fail) {
    if (!load_library(path)) return 0;
    for (unsigned i = 0; i < sizeof strlen_words / 4; ++i)
        w32(NULL, STRLEN_CODE + i * 4, strlen_words[i]);
    w32(NULL, 0xd19a4, 0xea000000u | (((STRLEN_CODE - 0xd19a4u - 8u) >> 2) & 0xffffffu));
    memset(&cpu, 0, sizeof cpu);
    arm_reset(&cpu, &bus);
    generator_reset();
    items = 0; strings = 0x40000; fault = 0; retired = 0;
    live = peak_live = max_heap_slot = mmap_calls = 0;
    memset(heap_lengths, 0, sizeof heap_lengths);
    fail_mmap_at = fail;
    fail_pool_item = fail_pool_string = 0;
    return 1;
}

/* Only the allocator/system-call boundaries are host fixtures. All original
 * lookup, strlen, range comparison, replacement, and detour code executes as
 * ARM instructions. Poison caller-saved registers across each fixture call. */
static int external_call(void) {
    uint32_t pc = cpu.r[15], result = 0;
    if (pc == 0x1750) {
        CHECK(cpu.r[0] == MAP_TOKEN && cpu.r[1] == 8, "pool allocation ABI");
        if (!fail_pool_item) result = (0x2000u + items++ * 8u) / 8u;
        CHECK(items < 8192, "item pool exhausted");
    } else if (pc == 0x1a00) {
        CHECK(cpu.r[0] == MAP_TOKEN, "pool string ABI");
        uint32_t n = cpu.r[2];
        if (!fail_pool_string) {
            CHECK(n < 4096 && strings + n + 1 < CACHE_SIZE && bounds(cpu.r[1], n), "string bounds");
            if (fault || n >= 4096 || strings + n + 1 >= CACHE_SIZE) return -1;
            result = strings;
            memcpy(ram + CACHE + strings, ram + cpu.r[1], n);
            ram[CACHE + strings + n] = 0;
            strings += n + 1;
        }
    } else if (pc == 0xd1764) {
        CHECK(cpu.r[0] == 0 && cpu.r[1] > 0 && cpu.r[1] <= 4096 &&
              cpu.r[2] == 3 && cpu.r[3] == 0x1002, "private non-executable mmap arguments");
        CHECK(r32(NULL, cpu.r[13]) == UINT32_MAX && !r32(NULL, cpu.r[13]+4) &&
              !r32(NULL, cpu.r[13]+8) && !r32(NULL, cpu.r[13]+12), "Darwin mmap stack arguments");
        if ((int)mmap_calls++ == fail_mmap_at) result = UINT32_MAX;
        else {
            unsigned slot = 0;
            while (slot < 64 && heap_lengths[slot]) ++slot;
            CHECK(slot < 64, "test heap exhausted");
            if (slot == 64) return -1;
            heap_lengths[slot] = cpu.r[1];
            result = HEAP + slot * 4096;
            memset(ram + result, 0, 4096);
            live++;
            if (live > peak_live) peak_live = live;
            if (slot + 1 > max_heap_slot) max_heap_slot = slot + 1;
        }
    } else if (pc == 0xd1784) {
        uint32_t a = cpu.r[0];
        CHECK(a >= HEAP && a < HEAP + 64 * 4096 && !(a & 4095), "munmap address");
        if (a < HEAP || a >= HEAP + 64 * 4096 || (a & 4095)) return -1;
        unsigned slot = (a - HEAP) / 4096;
        CHECK(heap_lengths[slot] && heap_lengths[slot] == cpu.r[1], "munmap lifetime/length");
        if (heap_lengths[slot]) { heap_lengths[slot] = 0; live--; }
    } else return 0;
    uint32_t target = cpu.r[14];
    cpu.r[0] = result;
    cpu.r[1] = 0xfbad0001; cpu.r[2] = 0xfbad0002; cpu.r[3] = 0xfbad0003;
    /* Apple ARMv6 differs from generic AAPCS: R9 is volatile in iOS 3.
     * https://developer.apple.com/documentation/xcode/writing-armv6-code-for-ios */
    cpu.r[9] = 0xfbad0009;
    cpu.r[12] = 0xfbad000c; cpu.r[14] = 0xfbad000e;
    cpu.cpsr = (cpu.cpsr & 0x0fffffff) | 0xa0000000;
    cpu.r[15] = target;
    return 1;
}

static int execute(uint32_t entry, uint32_t stop, unsigned alignment) {
    cpu.cpsr = ARM_MODE_USR | ARM_CPSR_I | ARM_CPSR_F;
    cpu.r[13] = STACK - alignment;
    cpu.r[14] = RETURN_PC;
    cpu.r[15] = entry;
    for (unsigned i = 4; i <= 11; ++i) cpu.r[i] = 0x55000000u + i;
    uint64_t before = cpu.cycles;
    unsigned steps = 0;
    while (cpu.r[15] != stop && !fault && steps++ < 10000000u) {
        int ext = external_call();
        if (ext < 0) return 0;
        if (!ext) {
            arm_status_t status = arm_step(&cpu);
            if (status != ARM_OK || (cpu.cpsr & 31u) != ARM_MODE_USR) {
                CHECK(0, "ARM exit=%d mode=%x pc=%08x insn=%08x", (int)status,
                      cpu.cpsr & 31u, cpu.r[15], r32(NULL, cpu.r[15]));
                return 0;
            }
        }
    }
    retired += cpu.cycles - before;
    CHECK(!fault && cpu.r[15] == stop, "ARM run fault=%08x pc=%08x", fault, cpu.r[15]);
    for (unsigned i = 4; i <= 11; ++i)
        if (i != 9) CHECK(cpu.r[i] == 0x55000000u + i, "callee-saved r%u", i);
    uint32_t pushed = stop == RETURN_PC ? 0 : 20;
    CHECK(cpu.r[13] == STACK - alignment - pushed, "stack alignment/restoration");
    if (pushed) {
        for (unsigned i = 0; i < 4; ++i)
            CHECK(r32(NULL, cpu.r[13] + i * 4) == 0x55000004u + i,
                  "displaced prologue saved register %u", i + 4);
        CHECK(r32(NULL, cpu.r[13] + 16) == RETURN_PC, "displaced prologue saved return address");
    }
    return !fault && cpu.r[15] == stop;
}

static int call_write(const char *text, uint32_t n, unsigned ordinal, uint32_t *value) {
    if (n >= 4094) return 0;
    memset(ram + INPUT, 0, 4096);
    memcpy(ram + INPUT, text, n);
    cpu.r[0] = GENERATOR; cpu.r[1] = INPUT; cpu.r[2] = n;
    if (!execute(WRITE_PC, RETURN_PC, (ordinal & 1u) * 4u)) return 0;
    *value = cpu.r[0];
    return 1;
}

static int edge_cases(const char *original, const char *candidate) {
    uint32_t expected[270];
    for (unsigned variant = 0; variant < 2; ++variant) {
        if (!setup(variant ? candidate : original, -1)) return 0;
        for (unsigned i = 0; i < 262; ++i) {
            char text[8] = {(char)(i + 1u), 'x', 0};
            uint32_t n = 2;
            if (i == 255) { text[0] = 0; n = 0; }
            if (i >= 256) { text[0] = 'a'; text[1] = 0; text[2] = (char)0x80; n = (i & 1u) ? 1 : 3; }
            uint32_t value;
            if (!call_write(text, n, i, &value)) return 0;
            if (!variant) expected[i] = value;
            else CHECK(value == expected[i], "signed byte/empty/embedded-NUL handle at %u", i);
        }
        cpu.r[0] = GENERATOR;
        if (!execute(D1_PC, D1_PC + 4, 0)) return 0;
        CHECK(!live, "edge-case cleanup leak");
        if (!variant) memcpy(expected_cache, ram + CACHE, CACHE_SIZE);
        else {
            CHECK(!memcmp(expected_cache, ram + CACHE, CACHE_SIZE), "edge-case serialized cache mismatch");
            unsigned reported = 0;
            for (unsigned off = 0; off < CACHE_SIZE && reported < 12; off += 4) {
                if (memcmp(expected_cache + off, ram + CACHE + off, 4)) {
                    printf("cache difference offset=%x original=%08x candidate=%08x\n",
                           off, le32(expected_cache + off), r32(NULL, CACHE + off));
                    reported++;
                }
            }
        }
    }
    for (unsigned failure = 0; failure < 2; ++failure) {
        for (unsigned variant = 0; variant < 2; ++variant) {
            if (!setup(variant ? candidate : original, -1)) return 0;
            for (unsigned i = 0; i < 3; ++i) {
                fail_pool_item = i == 1 && !failure;
                fail_pool_string = i == 1 && failure;
                const char *text = i == 1 ? "new" : "existing";
                uint32_t value;
                if (!call_write(text, (uint32_t)strlen(text), i, &value)) return 0;
                if (!variant) expected[i] = value;
                else CHECK(value == expected[i], "pool failure result");
            }
            cpu.r[0] = GENERATOR;
            if (!execute(D2_PC, D2_PC + 4, 4)) return 0;
            CHECK(!live, "pool-failure cleanup leak");
            if (!variant) memcpy(expected_cache, ram + CACHE, CACHE_SIZE);
            else CHECK(!memcmp(expected_cache, ram + CACHE, CACHE_SIZE), "pool-failure serialized cache mismatch");
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3 && !(argc == 4 && !strcmp(argv[3], "--edges-only"))) {
        fprintf(stderr, "usage: guest_apt_arm_probe ORIGINAL DYLIB_EXPERIMENT [--edges-only]\n"); return 2;
    }
    uint32_t handles[770];
    uint64_t reference_steps = 0;
    for (unsigned scenario = 0; scenario < (argc == 4 ? 0u : 3u); ++scenario) {
        for (unsigned variant = 0; variant < 2; ++variant) {
            if (!setup(argv[variant + 1], variant ? (int)scenario - 1 : -1)) return 2;
            for (unsigned i = 0; i < 768; ++i) {
                char key[64];
                unsigned k = i < 384 ? 383u - i : (i * 71u) % 384u;
                int n = snprintf(key, sizeof key, "Name common-prefix %04u", k);
                uint32_t value;
                if (!call_write(key, (uint32_t)n, i, &value)) return 1;
                if (!variant) handles[i] = value;
                else CHECK(value == handles[i], "ARM returned different item at %u", i);
            }
            /* Normal destructor interception and a repeated alternate entry. */
            cpu.r[0] = GENERATOR;
            if (!execute(D1_PC, D1_PC + 4, 4)) return 1;
            cpu.r[0] = GENERATOR;
            if (!execute(D2_PC, D2_PC + 4, 0)) return 1;
            CHECK(live == 0, "generator destructor leaked %u mappings", live);
            generator_reset();
            uint32_t value;
            if (!call_write("Name common-prefix 0001", 23, 0, &value)) return 1;
            if (!variant) handles[768] = value;
            else CHECK(value == handles[768], "object reuse/preexisting cache handle");
            cpu.r[0] = GENERATOR;
            if (!execute(D2_PC, D2_PC + 4, 4)) return 1;
            CHECK(live == 0, "reused generator leaked mappings");
            if (!variant) { memcpy(expected_cache, ram + CACHE, CACHE_SIZE); reference_steps = retired; }
            else {
                CHECK(!memcmp(expected_cache, ram + CACHE, CACHE_SIZE), "ARM serialized cache mismatch");
                printf("scenario=%u original_lookup_insns=%llu indexed_lookup_insns=%llu peak_pages=%u\n",
                       scenario, (unsigned long long)reference_steps,
                       (unsigned long long)retired, peak_live);
            }
        }
    }
    if (!edge_cases(argv[1], argv[2])) return 1;
    printf("%u checks, %u failures; allocator fixtures, not a full OS or physical timing\n", checks, failures);
    return failures ? 1 : 0;
}
