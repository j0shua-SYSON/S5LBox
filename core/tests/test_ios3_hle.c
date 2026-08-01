/*
 * Tests for tools/ios3_hle.c — guest-function interception.
 *
 * What is worth asserting here is mostly the REFUSALS. A facility
 * that replaces a function in a running OS is dangerous in proportion to how
 * willing it is to fire, so every test below is a way of not firing:
 * without a recorded prologue, against the wrong bytes, in the wrong address
 * space, or when the handler declines.
 *
 * The native sampler fixtures also pin decoded pixel arithmetic and atomic
 * publication. They cannot prove equivalence to Apple's complete routine; that
 * still needs a real armed/disarmed frame diff, the oracle in the header.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "ios3_hle.h"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* A flat pretend address space: one page at a known base. */
#define FAKE_BASE 0x338f6000u
#define FAKE_LEN  0x1000u
static uint8_t g_fake[FAKE_LEN];
static bool    g_fail_reads;

static bool fake_read(void *ctx, uint32_t va, void *dst, uint32_t len) {
    (void)ctx;
    if (g_fail_reads) return false;
    if (va < FAKE_BASE || (uint64_t)(va - FAKE_BASE) + len > FAKE_LEN)
        return false;
    memcpy(dst, &g_fake[va - FAKE_BASE], len);
    return true;
}
static bool fake_write(void *ctx, uint32_t va, const void *src, uint32_t len) {
    (void)ctx;
    if (va < FAKE_BASE || (uint64_t)(va - FAKE_BASE) + len > FAKE_LEN)
        return false;
    memcpy(&g_fake[va - FAKE_BASE], src, len);
    return true;
}
static bool fake_writev(void *ctx, const ios3_hle_write_span_t *spans,
                        uint32_t count) {
    (void)ctx;
    if (!spans || count == 0u) return false;
    for (uint32_t i = 0; i < count; i++) {
        if (!spans[i].src || spans[i].len == 0u ||
            spans[i].va < FAKE_BASE ||
            (uint64_t)(spans[i].va - FAKE_BASE) + spans[i].len > FAKE_LEN)
            return false;
    }
    for (uint32_t i = 0; i < count; i++)
        (void)fake_write(NULL, spans[i].va, spans[i].src, spans[i].len);
    return true;
}
static const ios3_hle_mem_t FAKE = {
    NULL, fake_read, fake_write, NULL, NULL, fake_writev
};

static void poke(uint32_t va, uint32_t word) {
    (void)fake_write(NULL, va, &word, 4u);
}

/* Sparse memory for exercising the real sampler/scanline handlers. */
#define SCAN_MEM_BASE 0x00100000u
#define SCAN_MEM_LEN  0x00010000u
#define SCAN_STACK    (SCAN_MEM_BASE + 0x0000u)
#define SCAN_CTX      (SCAN_MEM_BASE + 0x1000u)
#define SCAN_RENDER   (SCAN_MEM_BASE + 0x2000u)
#define SCAN_STATE    (SCAN_MEM_BASE + 0x3000u)
#define SCAN_START    (SCAN_MEM_BASE + 0x4000u)
#define SCAN_DELTA    (SCAN_MEM_BASE + 0x4100u)
#define SCAN_TEXELS   (SCAN_MEM_BASE + 0x5000u)
#define SCAN_OUT      (SCAN_MEM_BASE + 0x6000u)
#define SCAN_POLY     (SCAN_MEM_BASE + 0x7000u)
#define SCAN_WRITEV_MAX 480u

static uint8_t  g_scan_mem[SCAN_MEM_LEN];
static uint8_t  g_scan_code[64];
static uint32_t g_scan_code_va;
static uint32_t g_scan_code_len;
static uint32_t g_scan_fail_read_va;
static bool     g_scan_refuse_cross_page_reads;
static unsigned g_scan_cross_page_refusals;
static unsigned g_scan_texel_read_calls;
static uint32_t g_scan_texel_read_bytes;
static uint32_t g_scan_texel_max_read_len;
static bool     g_scan_fail_write;
static unsigned g_scan_write_calls;
static uint32_t g_scan_write_va;
static uint32_t g_scan_write_len;
static unsigned g_scan_writev_calls;
static uint32_t g_scan_writev_span_count;
static uint32_t g_scan_writev_va[SCAN_WRITEV_MAX];
static uint32_t g_scan_writev_len[SCAN_WRITEV_MAX];
static uint32_t g_scan_fail_writev_span;

static bool scan_region(uint32_t base, uint32_t size, uint32_t va,
                        uint32_t len, uint32_t *offset) {
    uint64_t end = (uint64_t)va + len;
    if (va < base || end > (uint64_t)base + size) return false;
    if (offset) *offset = va - base;
    return true;
}

static bool scan_read(void *ctx, uint32_t va, void *dst, uint32_t len) {
    uint32_t off;
    (void)ctx;
    if (scan_region(SCAN_TEXELS, 0x1000u, va, len, &off)) {
        g_scan_texel_read_calls++;
        g_scan_texel_read_bytes += len;
        if (len > g_scan_texel_max_read_len) g_scan_texel_max_read_len = len;
    }
    if (g_scan_refuse_cross_page_reads && len &&
        (va >> 12) != ((uint32_t)((uint64_t)va + len - 1u) >> 12)) {
        g_scan_cross_page_refusals++;
        return false;
    }
    if (g_scan_fail_read_va && va <= g_scan_fail_read_va &&
        (uint64_t)va + len > g_scan_fail_read_va)
        return false;
    if (scan_region(g_scan_code_va, g_scan_code_len, va, len, &off)) {
        memcpy(dst, g_scan_code + off, len);
        return true;
    }
    if (!scan_region(SCAN_MEM_BASE, SCAN_MEM_LEN, va, len, &off))
        return false;
    memcpy(dst, g_scan_mem + off, len);
    return true;
}

static bool scan_write(void *ctx, uint32_t va, const void *src, uint32_t len) {
    uint32_t off;
    (void)ctx;
    g_scan_write_calls++;
    g_scan_write_va = va;
    g_scan_write_len = len;
    if (g_scan_fail_write ||
        !scan_region(SCAN_MEM_BASE, SCAN_MEM_LEN, va, len, &off))
        return false;
    memcpy(g_scan_mem + off, src, len);
    return true;
}

static bool scan_writev(void *ctx, const ios3_hle_write_span_t *spans,
                        uint32_t count) {
    uint32_t offsets[SCAN_WRITEV_MAX];
    (void)ctx;
    g_scan_writev_calls++;
    g_scan_writev_span_count = count;
    if (!spans || !count || count > SCAN_WRITEV_MAX || g_scan_fail_write)
        return false;

    /* Validate the complete transaction before changing the pretend guest. */
    for (uint32_t i = 0; i < count; i++) {
        g_scan_writev_va[i] = spans[i].va;
        g_scan_writev_len[i] = spans[i].len;
        if (i == g_scan_fail_writev_span || !spans[i].src || !spans[i].len ||
            !scan_region(SCAN_MEM_BASE, SCAN_MEM_LEN, spans[i].va,
                         spans[i].len, &offsets[i]))
            return false;
    }
    for (uint32_t i = 0; i < count; i++)
        memcpy(g_scan_mem + offsets[i], spans[i].src, spans[i].len);
    return true;
}

static const ios3_hle_mem_t SCAN_MEM = {
    NULL, scan_read, scan_write, NULL, NULL, scan_writev
};

static ios3_hle_site_t *site_named(const char *name) {
    for (unsigned i = 0; i < ios3_hle_site_count(); i++) {
        ios3_hle_site_t *s = ios3_hle_site_at(i);
        if (s && strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

static void scan_poke32(uint32_t va, uint32_t word) {
    uint32_t off;
    CHECK(scan_region(SCAN_MEM_BASE, SCAN_MEM_LEN, va, 4u, &off),
          "fixture word %08x is outside scan memory", va);
    if (scan_region(SCAN_MEM_BASE, SCAN_MEM_LEN, va, 4u, &off))
        memcpy(g_scan_mem + off, &word, sizeof word);
}

static void scan_poke16(uint32_t va, uint16_t halfword) {
    uint32_t off;
    CHECK(scan_region(SCAN_MEM_BASE, SCAN_MEM_LEN, va, 2u, &off),
          "fixture halfword %08x is outside scan memory", va);
    if (scan_region(SCAN_MEM_BASE, SCAN_MEM_LEN, va, 2u, &off))
        memcpy(g_scan_mem + off, &halfword, sizeof halfword);
}

static uint32_t scan_peek32(uint32_t va) {
    uint32_t off, word = 0;
    CHECK(scan_region(SCAN_MEM_BASE, SCAN_MEM_LEN, va, 4u, &off),
          "fixture read %08x is outside scan memory", va);
    if (scan_region(SCAN_MEM_BASE, SCAN_MEM_LEN, va, 4u, &off))
        memcpy(&word, g_scan_mem + off, sizeof word);
    return word;
}

static void scan_pokef(uint32_t va, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    scan_poke32(va, bits);
}

static void scan_select_code(ios3_hle_site_t *site) {
    CHECK(site != NULL, "fixture was given a null HLE site");
    memset(g_scan_code, 0, sizeof g_scan_code);
    g_scan_code_va = site ? site->va : 0u;
    g_scan_code_len = site ? site->prologue_n * 4u : 0u;
    CHECK(g_scan_code_len <= sizeof g_scan_code,
          "site prologue is too large for fixture (%u)", g_scan_code_len);
    if (site && g_scan_code_len <= sizeof g_scan_code)
        memcpy(g_scan_code, site->prologue, g_scan_code_len);
}

static void reset_scan_fixture(ios3_hle_site_t *site) {
    memset(g_scan_mem, 0, sizeof g_scan_mem);
    g_scan_fail_read_va = 0u;
    g_scan_refuse_cross_page_reads = false;
    g_scan_cross_page_refusals = 0u;
    g_scan_texel_read_calls = 0u;
    g_scan_texel_read_bytes = 0u;
    g_scan_texel_max_read_len = 0u;
    g_scan_fail_write = false;
    g_scan_write_calls = 0u;
    g_scan_write_va = 0u;
    g_scan_write_len = 0u;
    g_scan_writev_calls = 0u;
    g_scan_writev_span_count = 0u;
    memset(g_scan_writev_va, 0, sizeof g_scan_writev_va);
    memset(g_scan_writev_len, 0, sizeof g_scan_writev_len);
    g_scan_fail_writev_span = UINT32_MAX;
    scan_select_code(site);
    if (site)
        site->hits = site->handled = site->declined = site->wrong_space = 0u;
}

/*
 * A site with no recorded prologue must NOT arm -- the difference between
 * "identity verified" and "address looked plausible".
 *
 * This has to blank the prologues deliberately now. The shipped sites carry
 * real ones as of 2026-07-30, so simply arming against a zeroed page would
 * refuse for the WRONG reason (bytes mismatched) and the test would still
 * pass while checking nothing it claims to.
 */
static void test_a_site_without_a_prologue_refuses_to_arm(void) {
    memset(g_fake, 0, sizeof g_fake);
    g_fail_reads = false;

    const uint32_t *saved_p[16];
    unsigned saved_n[16], n = ios3_hle_site_count();
    CHECK(n <= 16u, "more sites (%u) than this test can save", n);
    for (unsigned i = 0; i < n && i < 16u; i++) {
        ios3_hle_site_t *s = ios3_hle_site_at(i);
        saved_p[i] = s->prologue; saved_n[i] = s->prologue_n;
        s->prologue = NULL; s->prologue_n = 0u;
    }

    unsigned armed = ios3_hle_arm(&FAKE, 0x1000u);
    CHECK(armed == 0u,
          "%u site(s) armed with no recorded prologue; an unverified address "
          "is not an identity", armed);
    for (unsigned i = 0; i < n; i++) {
        ios3_hle_site_t *s = ios3_hle_site_at(i);
        CHECK(!s->armed, "%s armed without a prologue", s->name);
        CHECK(s->identity_failed, "%s did not record its identity failure",
              s->name);
    }

    for (unsigned i = 0; i < n && i < 16u; i++) {
        ios3_hle_site_t *s = ios3_hle_site_at(i);
        s->prologue = saved_p[i]; s->prologue_n = saved_n[i];
    }
}

/*
 * The shipped sites must actually carry the bytes somebody read out of the
 * cache, and enough of them to tell the sites APART.
 *
 * Both CoreGraphics sites open with the identical
 * `push {r4,r5,r6,r7,lr}; add r7,sp,#0xc`, so a two-word prologue matches
 * either one. That is not a hypothetical: it is why four words were recorded.
 * This pins both facts, so blanking a prologue or trimming it back to the
 * ambiguous prefix fails here rather than silently in a run.
 */
static void test_the_shipped_sites_carry_a_distinguishing_prologue(void) {
    for (unsigned i = 0; i < ios3_hle_site_count(); i++) {
        ios3_hle_site_t *a = ios3_hle_site_at(i);
        CHECK(a->prologue && a->prologue_n >= 4u,
              "%s ships %u prologue word(s); it needs at least 4 to be an "
              "identity rather than a guess", a->name, a->prologue_n);
        for (unsigned j = i + 1u; j < ios3_hle_site_count(); j++) {
            ios3_hle_site_t *b = ios3_hle_site_at(j);
            if (!a->prologue || !b->prologue) continue;
            unsigned lim = a->prologue_n < b->prologue_n ? a->prologue_n
                                                         : b->prologue_n;
            bool differ = false;
            for (unsigned k = 0; k < lim; k++)
                if (a->prologue[k] != b->prologue[k]) { differ = true; break; }
            CHECK(differ,
                  "%s and %s share their whole recorded prologue; the identity "
                  "check cannot tell them apart", a->name, b->name);
        }
    }
}

/* And with nothing armed, the hot path must do nothing at all. */
static void test_nothing_armed_never_intercepts(void) {
    arm_cpu_t cpu; memset(&cpu, 0, sizeof cpu);
    ios3_hle_disarm();
    for (unsigned i = 0; i < ios3_hle_site_count(); i++) {
        ios3_hle_site_t *s = ios3_hle_site_at(i);
        CHECK(!ios3_hle_step(&cpu, &FAKE, s->va, 0x1000u),
              "%s intercepted while disarmed", s->name);
    }
}

/*
 * The rest needs a site that CAN arm, so build one locally rather than
 * inventing prologue bytes for the real addresses -- inventing them is exactly
 * the failure the identity gate exists to prevent.
 */
static uint32_t g_test_prologue[2];
static unsigned g_handler_calls;
static bool     g_handler_answer;

static bool test_handler(arm_cpu_t *cpu, const ios3_hle_mem_t *mem) {
    (void)mem;
    g_handler_calls++;
    if (!g_handler_answer) return false;
    cpu->r[0] = 0x5a5au;          /* a result */
    cpu->r[15] = cpu->r[14];      /* return via LR, as the contract requires */
    return true;
}

static bool g_trace_write_succeeded;
static bool g_trace_writev_succeeded;
static bool g_multiwrite_overlap;

static bool mutating_trace_handler(arm_cpu_t *cpu,
                                   const ios3_hle_mem_t *mem) {
    const uint32_t poison = 0xdeadbeefu;
    g_handler_calls++;
    cpu->r[0] = poison;
    cpu->r[15] = cpu->r[14];
    g_trace_write_succeeded = mem->write &&
        mem->write(mem->ctx, FAKE_BASE + 0x100u, &poison, sizeof poison);
    if (mem->writev) {
        ios3_hle_write_span_t span = {
            FAKE_BASE + 0x104u, &poison, sizeof poison
        };
        g_trace_writev_succeeded = mem->writev(mem->ctx, &span, 1u);
    }
    return true;
}

static bool multiwrite_handler(arm_cpu_t *cpu,
                               const ios3_hle_mem_t *mem) {
    static const uint32_t first = 0x11223344u;
    static const uint32_t second = 0xaabbccddu;
    ios3_hle_write_span_t spans[2] = {
        { FAKE_BASE + 0x200u, &first, sizeof first },
        { FAKE_BASE + (g_multiwrite_overlap ? 0x202u : 0x208u),
          &second, sizeof second }
    };

    if (!mem || !mem->writev || !mem->writev(mem->ctx, spans, 2u))
        return false;
    cpu->r[15] = cpu->r[14];
    return true;
}

static ios3_hle_site_t *install_test_site(ios3_hle_mode_t mode) {
    ios3_hle_site_t *s = ios3_hle_site_at(0);
    g_test_prologue[0] = 0xe92d4010u;   /* push {r4, lr} */
    g_test_prologue[1] = 0xe1a04000u;   /* mov r4, r0    */
    s->prologue = g_test_prologue;
    s->prologue_n = 2u;
    s->handler = test_handler;
    s->mode = mode;
    /*
     * Counters reset per case. Without this the cases share a site and each
     * one reads the previous one's totals -- which is how the first run of
     * this file "failed": OBSERVE saw a hit from the address-space case and
     * decline saw a handled from it too. Both were the test leaking state,
     * not the module counting wrongly, and a shared fixture that accumulates
     * is a test that reports the order it ran in.
     */
    s->hits = s->handled = s->declined = s->wrong_space = 0u;
    memset(g_fake, 0, sizeof g_fake);
    poke(s->va,      g_test_prologue[0]);
    poke(s->va + 4u, g_test_prologue[1]);
    return s;
}

/* Matching bytes arm; a single wrong word does not. */
static void test_identity_is_checked_word_for_word(void) {
    g_fail_reads = false;
    ios3_hle_site_t *s = install_test_site(IOS3_HLE_OBSERVE);
    CHECK(ios3_hle_arm(&FAKE, 0x1000u) == 1u, "the matching site did not arm");
    CHECK(s->armed && !s->identity_failed, "%s armed but flagged a failure",
          s->name);

    poke(s->va + 4u, 0xdeadbeefu);          /* second word now differs */
    CHECK(ios3_hle_arm(&FAKE, 0x1000u) == 0u,
          "a site armed against bytes that do not match its prologue");
    CHECK(s->identity_failed, "the mismatch was not recorded");

    /* A read that faults is a refusal too, not a pass by default. */
    poke(s->va + 4u, g_test_prologue[1]);
    g_fail_reads = true;
    CHECK(ios3_hle_arm(&FAKE, 0x1000u) == 0u,
          "a site armed although its identity could not be READ");
    g_fail_reads = false;
}

/*
 * A shared-cache address exists in every process. Firing in the wrong one
 * means executing SpringBoard's blitter on behalf of some other program, so it
 * must not fire -- and must be counted separately, or "aimed at the wrong
 * process" looks exactly like "never reached".
 */
static void test_the_wrong_address_space_is_refused_and_counted(void) {
    arm_cpu_t cpu; memset(&cpu, 0, sizeof cpu);
    ios3_hle_site_t *s = install_test_site(IOS3_HLE_REPLACE);
    g_handler_answer = true; g_handler_calls = 0;
    CHECK(ios3_hle_arm(&FAKE, 0x1000u) == 1u, "setup: site did not arm");

    CHECK(!ios3_hle_step(&cpu, &FAKE, s->va, 0x2000u),
          "the site fired in a different address space");
    CHECK(s->wrong_space == 1u, "the wrong-space visit was not counted (%llu)",
          (unsigned long long)s->wrong_space);
    CHECK(s->hits == 0u, "a wrong-space visit was counted as a hit");
    CHECK(g_handler_calls == 0u, "the handler ran in the wrong address space");

    /* And the right one does fire. */
    cpu.r[14] = 0xc0debabeu;
    CHECK(ios3_hle_step(&cpu, &FAKE, s->va, 0x1000u),
          "the site did not fire in its own address space");
    CHECK(cpu.r[15] == 0xc0debabeu,
          "the handler returned to %08x, not LR", cpu.r[15]);
    CHECK(s->handled == 1u && s->hits == 1u,
          "hit/handled accounting wrong: hits %llu handled %llu",
          (unsigned long long)s->hits, (unsigned long long)s->handled);
}

/* OBSERVE must never alter execution, however willing the handler is. */
static void test_observe_mode_never_takes_the_call(void) {
    arm_cpu_t cpu; memset(&cpu, 0, sizeof cpu);
    ios3_hle_site_t *s = install_test_site(IOS3_HLE_OBSERVE);
    g_handler_answer = true; g_handler_calls = 0;
    CHECK(ios3_hle_arm(&FAKE, 0x1000u) == 1u, "setup: site did not arm");

    CHECK(!ios3_hle_step(&cpu, &FAKE, s->va, 0x1000u),
          "an OBSERVE site intercepted the call");
    CHECK(g_handler_calls == 0u, "an OBSERVE site ran its handler");
    CHECK(s->hits == 1u, "an OBSERVE site did not count the visit");
    CHECK(cpu.r[15] == 0u, "an OBSERVE site moved the program counter");
}

/*
 * TRACE is allowed to print host diagnostics, not to perturb the experiment it
 * is measuring. Prove that even a badly behaved tracer sees a private CPU and
 * a write-denied guest-memory interface; merely discarding its bool is not
 * enough, because the handler could otherwise have changed both before return.
 */
static void test_trace_cannot_mutate_or_intercept_the_guest(void) {
    arm_cpu_t cpu, before;
    uint32_t memory_before = 0x12345678u, memory_after = 0u;
    ios3_hle_site_t *s;

    memset(&cpu, 0, sizeof cpu);
    cpu.r[0] = 0x11111111u;
    cpu.r[14] = 0xc0debabeu;
    cpu.r[15] = 0x22222222u;
    before = cpu;
    s = install_test_site(IOS3_HLE_TRACE);
    s->handler = mutating_trace_handler;
    g_handler_calls = 0u;
    g_trace_write_succeeded = false;
    g_trace_writev_succeeded = false;
    poke(FAKE_BASE + 0x100u, memory_before);

    CHECK(ios3_hle_arm(&FAKE, 0x1000u) == 1u, "setup: site did not arm");
    CHECK(!ios3_hle_step(&cpu, &FAKE, s->va, 0x1000u),
          "a TRACE site intercepted the guest call");
    CHECK(g_handler_calls == 1u, "the TRACE handler did not run");
    CHECK(memcmp(&cpu, &before, sizeof cpu) == 0,
          "a TRACE handler changed the live CPU state");
    CHECK(!g_trace_write_succeeded,
          "a TRACE handler's guest-memory write was accepted");
    CHECK(!g_trace_writev_succeeded,
          "a TRACE handler's transactional guest-memory write was accepted");
    CHECK(fake_read(NULL, FAKE_BASE + 0x100u, &memory_after,
                    sizeof memory_after) && memory_after == memory_before,
          "TRACE changed guest memory from %08x to %08x",
          memory_before, memory_after);
    CHECK(s->hits == 1u && s->handled == 0u && s->declined == 0u,
          "TRACE accounting wrong: hits %llu handled %llu declined %llu",
          (unsigned long long)s->hits, (unsigned long long)s->handled,
          (unsigned long long)s->declined);
}

/* A handler that declines leaves the guest to run its own code. */
static void test_declining_is_safe(void) {
    arm_cpu_t cpu; memset(&cpu, 0, sizeof cpu);
    cpu.r[15] = 0x1234u;
    ios3_hle_site_t *s = install_test_site(IOS3_HLE_REPLACE);
    g_handler_answer = false; g_handler_calls = 0;
    CHECK(ios3_hle_arm(&FAKE, 0x1000u) == 1u, "setup: site did not arm");

    CHECK(!ios3_hle_step(&cpu, &FAKE, s->va, 0x1000u),
          "a declining handler still reported the call as taken");
    CHECK(g_handler_calls == 1u, "the handler was not consulted");
    CHECK(s->declined == 1u && s->handled == 0u,
          "decline accounting wrong: declined %llu handled %llu",
          (unsigned long long)s->declined, (unsigned long long)s->handled);
    CHECK(cpu.r[15] == 0x1234u,
          "a declined call moved the program counter to %08x", cpu.r[15]);
}

static void test_oracle_captures_disjoint_transactional_spans(void) {
    ios3_hle_oracle_t oracle;
    ios3_hle_site_t *s;
    arm_cpu_t cpu;
    uint8_t expected[8];
    uint32_t first = 0u, second = 0u, guest = UINT32_MAX;

    memset(&cpu, 0, sizeof cpu);
    cpu.r[13] = FAKE_BASE + 0x800u;
    cpu.r[14] = 0xc0dec0deu;
    s = install_test_site(IOS3_HLE_REPLACE);
    s->handler = multiwrite_handler;
    g_multiwrite_overlap = false;
    CHECK(ios3_hle_arm(&FAKE, 0x1000u) == 1u,
          "setup: multi-span site did not arm");
    CHECK(ios3_hle_oracle_prepare(&cpu, &FAKE, s->va, 0x1000u, &oracle,
                                  expected, (uint32_t)sizeof expected),
          "oracle declined two disjoint transactional spans");
    CHECK(oracle.span_count == 2u && oracle.expected_len == sizeof expected,
          "multi-span oracle captured %u span(s) / %u bytes",
          oracle.span_count, oracle.expected_len);
    CHECK(oracle.spans[0].va == FAKE_BASE + 0x200u &&
          oracle.spans[0].len == 4u &&
          oracle.spans[0].expected_offset == 0u &&
          oracle.spans[1].va == FAKE_BASE + 0x208u &&
          oracle.spans[1].len == 4u &&
          oracle.spans[1].expected_offset == 4u,
          "multi-span oracle metadata is wrong");
    memcpy(&first, expected, sizeof first);
    memcpy(&second, expected + 4u, sizeof second);
    CHECK(first == 0x11223344u && second == 0xaabbccddu,
          "multi-span oracle bytes are %08x/%08x", first, second);
    CHECK(fake_read(NULL, FAKE_BASE + 0x200u, &guest, sizeof guest) &&
          guest == 0u,
          "multi-span oracle changed guest memory to %08x", guest);
    CHECK(s->hits == 0u && s->handled == 0u && s->declined == 0u,
          "multi-span oracle polluted replacement counters");

    s = install_test_site(IOS3_HLE_REPLACE);
    s->handler = multiwrite_handler;
    g_multiwrite_overlap = true;
    CHECK(ios3_hle_arm(&FAKE, 0x1000u) == 1u,
          "setup: overlap-refusal site did not arm");
    CHECK(!ios3_hle_oracle_prepare(&cpu, &FAKE, s->va, 0x1000u, &oracle,
                                   expected, (uint32_t)sizeof expected),
          "oracle accepted overlapping transactional spans");
    CHECK(oracle.site_index != UINT32_MAX && oracle.span_count == 0u,
          "overlap refusal left %u captured span(s)", oracle.span_count);

    g_multiwrite_overlap = false;
    CHECK(!ios3_hle_oracle_prepare(&cpu, &FAKE, s->va, 0x1000u, &oracle,
                                   expected, 4u),
          "oracle accepted a transaction larger than its storage");
    CHECK(oracle.span_count == 0u,
          "capacity refusal left %u captured span(s)", oracle.span_count);
    ios3_hle_disarm();
}

#define SAMPLE_BGRX8 0x3122b698u
#define SAMPLE_BGRA8 0x3122b8bcu
#define SAMPLE_BILINEAR_BGRA8 0x3122bad8u

static void configure_direct_scanline(arm_cpu_t *cpu, uint32_t sampler) {
    memset(cpu, 0, sizeof *cpu);
    cpu->r[0] = 1u;             /* destination x */
    cpu->r[1] = 1u;             /* destination y */
    cpu->r[2] = 3u;             /* three pixels  */
    cpu->r[3] = SCAN_START;
    cpu->r[13] = SCAN_STACK;
    cpu->r[14] = 0xc0dec0deu;
    cpu->r[15] = 0x3122d180u;

    scan_poke32(SCAN_STACK + 0x00u, SCAN_DELTA);
    scan_poke32(SCAN_STACK + 0x04u, 0u);
    scan_poke32(SCAN_STACK + 0x08u, 0u);
    scan_poke32(SCAN_STACK + 0x0cu, 0x0308u);
    scan_poke32(SCAN_STACK + 0x10u, SCAN_CTX);

    scan_poke32(SCAN_CTX + 0x00u, SCAN_RENDER);
    scan_poke32(SCAN_CTX + 0x04u, SCAN_TEXELS);
    scan_poke32(SCAN_CTX + 0x08u, 16u);
    scan_poke32(SCAN_CTX + 0x0cu, 0x0003ffffu);
    scan_poke32(SCAN_CTX + 0x10u, 0x0001ffffu);
    scan_poke32(SCAN_CTX + 0x14u, sampler);
    scan_poke32(SCAN_CTX + 0x18u, sampler);
    scan_poke32(SCAN_CTX + 0x64u, UINT32_MAX);
    scan_poke32(SCAN_CTX + 0x68u, 1u);

    scan_poke32(SCAN_RENDER + 0x04u, SCAN_STATE);
    scan_poke32(SCAN_RENDER + 0xfcu, SCAN_OUT);
    scan_poke32(SCAN_RENDER + 0x100u, 0u);
    scan_poke32(SCAN_RENDER + 0x104u, 16u);
    scan_poke32(SCAN_RENDER + 0x10cu, 4u);
    scan_poke32(SCAN_RENDER + 0x110u, 0u);
    scan_poke32(SCAN_RENDER + 0x114u, 4u);
    scan_poke32(SCAN_RENDER + 0x118u, 0u);
    scan_poke32(SCAN_RENDER + 0x11cu, 2u);
    scan_poke32(SCAN_STATE + 0x3cu, 0u);

    scan_pokef(SCAN_START + 0x0cu, 1.0f);
    scan_pokef(SCAN_START + 0x20u, 0.0f);
    scan_pokef(SCAN_START + 0x24u, 0.0f);
    scan_pokef(SCAN_DELTA + 0x0cu, 0.0f);
    scan_pokef(SCAN_DELTA + 0x20u, 1.0f);
    scan_pokef(SCAN_DELTA + 0x24u, 0.0f);

    scan_poke32(SCAN_TEXELS + 0x00u, 0x00112233u);
    scan_poke32(SCAN_TEXELS + 0x04u, 0x44556677u);
    scan_poke32(SCAN_TEXELS + 0x08u, 0x8899aabbu);
    scan_poke32(SCAN_TEXELS + 0x0cu, 0xccddeeffu);
    for (unsigned i = 0; i < 8u; i++)
        scan_poke32(SCAN_OUT + i * 4u, 0xdeadbeefu);
}

static bool arm_only(ios3_hle_site_t *site) {
    unsigned armed;
    ios3_hle_disarm();
    armed = ios3_hle_arm(&SCAN_MEM, 0x0bf1b000u);
    CHECK(armed == 1u, "fixture armed %u sites instead of one", armed);
    CHECK(site && site->armed, "selected site did not arm");
    return armed == 1u && site && site->armed;
}

static void test_direct_scanline_preserves_bgra_and_forces_bgrx_alpha(void) {
    static const uint32_t sampler[2] = { SAMPLE_BGRA8, SAMPLE_BGRX8 };
    static const uint32_t alpha[2] = { 0x00000000u, 0xff000000u };
    ios3_hle_site_t *site = site_named("sw_scanline");

    CHECK(site != NULL, "sw_scanline site is missing");
    CHECK(site && site->mode == IOS3_HLE_REPLACE,
          "sw_scanline common path is not in REPLACE mode");
    for (unsigned pass = 0; pass < 2u; pass++) {
        arm_cpu_t cpu;
        reset_scan_fixture(site);
        configure_direct_scanline(&cpu, sampler[pass]);
        if (!arm_only(site)) continue;

        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "%s direct scanline declined", pass ? "BGRX" : "BGRA");
        CHECK(cpu.r[15] == cpu.r[14],
              "direct scanline returned to %08x, not LR", cpu.r[15]);
        CHECK(site->hits == 1u && site->handled == 1u && site->declined == 0u,
              "direct accounting is %llu/%llu/%llu",
              (unsigned long long)site->hits,
              (unsigned long long)site->handled,
              (unsigned long long)site->declined);
        CHECK(g_scan_write_calls == 1u &&
              g_scan_write_va == SCAN_OUT + 20u && g_scan_write_len == 12u,
              "direct span published as %u write(s) at %08x len %u",
              g_scan_write_calls, g_scan_write_va, g_scan_write_len);
        CHECK(g_scan_texel_read_calls == 1u &&
              g_scan_texel_read_bytes == 12u &&
              g_scan_texel_max_read_len == 12u,
              "direct span used %u texture reads totalling %u, max %u",
              g_scan_texel_read_calls, g_scan_texel_read_bytes,
              g_scan_texel_max_read_len);
        CHECK(scan_peek32(SCAN_OUT + 20u) == (0x00112233u | alpha[pass]),
              "pixel 0 mismatch in pass %u", pass);
        CHECK(scan_peek32(SCAN_OUT + 24u) == (0x44556677u | alpha[pass]),
              "pixel 1 mismatch in pass %u", pass);
        CHECK(scan_peek32(SCAN_OUT + 28u) == (0x8899aabbu | alpha[pass]),
              "pixel 2 mismatch in pass %u", pass);
        CHECK(scan_peek32(SCAN_OUT + 16u) == 0xdeadbeefu,
              "direct span overwrote the pixel before it in pass %u", pass);
    }
    ios3_hle_disarm();
}

static void test_direct_scanline_accepts_the_measured_full_width_bound(void) {
    ios3_hle_site_t *site = site_named("sw_scanline");
    arm_cpu_t cpu;

    reset_scan_fixture(site);
    configure_direct_scanline(&cpu, SAMPLE_BGRA8);
    cpu.r[0] = 0u;
    cpu.r[1] = 0u;
    cpu.r[2] = 320u;
    scan_poke32(SCAN_CTX + 0x08u, 1280u);
    scan_poke32(SCAN_CTX + 0x0cu, 0x013fffffu);
    scan_poke32(SCAN_CTX + 0x10u, 0x0000ffffu);
    scan_poke32(SCAN_RENDER + 0x104u, 1280u);
    scan_poke32(SCAN_RENDER + 0x114u, 320u);
    scan_poke32(SCAN_RENDER + 0x11cu, 1u);
    for (uint32_t i = 0; i < 320u; i++) {
        scan_poke32(SCAN_TEXELS + i * 4u, UINT32_C(0x80000000) | i);
        scan_poke32(SCAN_OUT + i * 4u, UINT32_C(0xdeadbeef));
    }
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "direct 320-pixel scanline declined");
        CHECK(g_scan_write_calls == 1u && g_scan_write_va == SCAN_OUT &&
              g_scan_write_len == 1280u,
              "full-width span published as %u write(s) at %08x len %u",
              g_scan_write_calls, g_scan_write_va, g_scan_write_len);
        CHECK(g_scan_texel_read_calls == 1u &&
              g_scan_texel_read_bytes == 1280u &&
              g_scan_texel_max_read_len == 1280u,
              "full-width span used %u texture reads totalling %u, max %u",
              g_scan_texel_read_calls, g_scan_texel_read_bytes,
              g_scan_texel_max_read_len);
        CHECK(scan_peek32(SCAN_OUT) == UINT32_C(0x80000000),
              "full-width first pixel is %08x", scan_peek32(SCAN_OUT));
        CHECK(scan_peek32(SCAN_OUT + 319u * 4u) == UINT32_C(0x8000013f),
              "full-width last pixel is %08x",
              scan_peek32(SCAN_OUT + 319u * 4u));
    }

    reset_scan_fixture(site);
    configure_direct_scanline(&cpu, SAMPLE_BGRA8);
    cpu.r[0] = 0u;
    cpu.r[1] = 0u;
    cpu.r[2] = 321u;
    scan_poke32(SCAN_RENDER + 0x104u, 1284u);
    scan_poke32(SCAN_RENDER + 0x114u, 321u);
    if (arm_only(site)) {
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "direct scanline accepted 321 pixels past its host bound");
        CHECK(g_scan_write_calls == 0u,
              "oversize direct scanline published %u write(s)",
              g_scan_write_calls);
    }
    ios3_hle_disarm();
}

static void test_identity_bulk_read_falls_back_to_exact_texels(void) {
    const uint32_t source = SCAN_MEM_BASE + 0x8f80u;
    const uint32_t output = SCAN_MEM_BASE + 0xa000u;
    ios3_hle_site_t *site = site_named("sw_scanline");
    arm_cpu_t cpu;

    reset_scan_fixture(site);
    configure_direct_scanline(&cpu, SAMPLE_BGRA8);
    cpu.r[0] = 0u;
    cpu.r[1] = 0u;
    cpu.r[2] = 64u;
    scan_poke32(SCAN_CTX + 0x04u, source);
    scan_poke32(SCAN_CTX + 0x08u, 256u);
    scan_poke32(SCAN_CTX + 0x0cu, UINT32_C(0x003fffff));
    scan_poke32(SCAN_CTX + 0x10u, UINT32_C(0x0000ffff));
    scan_poke32(SCAN_RENDER + 0xfcu, output);
    scan_poke32(SCAN_RENDER + 0x104u, 256u);
    scan_poke32(SCAN_RENDER + 0x114u, 64u);
    scan_poke32(SCAN_RENDER + 0x11cu, 1u);
    for (uint32_t i = 0; i < 64u; i++) {
        scan_poke32(source + i * 4u, UINT32_C(0xff000000) | i);
        scan_poke32(output + i * 4u, 0u);
    }
    g_scan_refuse_cross_page_reads = true;

    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "identity sampler declined after its bulk read was refused");
        CHECK(g_scan_cross_page_refusals == 1u,
              "identity sampler observed %u cross-page bulk refusals",
              g_scan_cross_page_refusals);
        CHECK(g_scan_write_calls == 1u && g_scan_write_va == output &&
              g_scan_write_len == 256u,
              "scalar fallback published %u write(s) at %08x/%u",
              g_scan_write_calls, g_scan_write_va, g_scan_write_len);
        for (uint32_t i = 0; i < 64u; i++)
            CHECK(scan_peek32(output + i * 4u) ==
                      (UINT32_C(0xff000000) | i),
                  "scalar fallback pixel %u is %08x", i,
                  scan_peek32(output + i * 4u));
    }
    ios3_hle_disarm();
}

static void configure_solid_scanline(arm_cpu_t *cpu, uint32_t pixel) {
    configure_direct_scanline(cpu, SAMPLE_BGRA8);
    scan_poke32(SCAN_STACK + 0x0cu, 0x0008u);
    scan_poke32(SCAN_CTX + 0x14u, 0u);
    scan_poke32(SCAN_CTX + 0x18u, 0u);
    scan_poke32(SCAN_CTX + 0x64u, pixel);
    scan_poke32(SCAN_CTX + 0x68u, 0u);
    scan_poke32(SCAN_STATE + 0x3cu, 0x02u);
}

static void configure_rect_root(arm_cpu_t *cpu, uint16_t flags) {
    static const float vertex[4][4] = {
        { 1.0f, 0.0f, 0.0f, 0.0f },
        { 4.0f, 0.0f, 3.0f, 0.0f },
        { 4.0f, 2.0f, 3.0f, 2.0f },
        { 1.0f, 2.0f, 0.0f, 2.0f }
    };

    configure_direct_scanline(cpu, SAMPLE_BGRA8);
    scan_poke32(SCAN_TEXELS + 0x10u, UINT32_C(0xccddeeff));
    scan_poke32(SCAN_TEXELS + 0x14u, UINT32_C(0x10203040));
    scan_poke32(SCAN_TEXELS + 0x18u, UINT32_C(0x50607080));
    scan_poke32(SCAN_TEXELS + 0x1cu, UINT32_C(0x90a0b0c0));
    scan_poke16(SCAN_POLY + 0x00u, 4u);
    scan_poke16(SCAN_POLY + 0x02u, flags);
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t base = SCAN_POLY + 4u + i * 0x38u;
        scan_pokef(base + 0x00u, vertex[i][0]);
        scan_pokef(base + 0x04u, vertex[i][1]);
        scan_pokef(base + 0x0cu, 1.0f);
        scan_pokef(base + 0x20u, vertex[i][2]);
        scan_pokef(base + 0x24u, vertex[i][3]);
    }
    scan_poke32(SCAN_STACK + 0x00u, 320u);
    scan_poke32(SCAN_STACK + 0x04u, 480u);
    scan_poke32(SCAN_STACK + 0x08u, UINT32_C(0x3122d180));
    scan_poke32(SCAN_STACK + 0x0cu, SCAN_CTX);

    cpu->r[0] = SCAN_POLY;
    cpu->r[1] = 0u;
    cpu->r[2] = 0u;
    cpu->r[3] = 0u;
    cpu->r[13] = SCAN_STACK;
    cpu->r[14] = UINT32_C(0xc0dec0de);
    cpu->r[15] = UINT32_C(0x311e2100);
}

static void configure_full_width_mode_two_root(arm_cpu_t *cpu,
                                                uint32_t sampler) {
    static const float vertex[4][4] = {
        {   0.0f, 0.0f,   0.0f, 0.0f },
        { 320.0f, 0.0f, 320.0f, 0.0f },
        { 320.0f, 1.0f, 320.0f, 1.0f },
        {   0.0f, 1.0f,   0.0f, 1.0f }
    };
    uint32_t input_alpha = sampler == SAMPLE_BGRX8 ? 0u : UINT32_C(0xff000000);

    configure_rect_root(cpu, 0x030bu);
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t base = SCAN_POLY + 4u + i * 0x38u;
        scan_pokef(base + 0x00u, vertex[i][0]);
        scan_pokef(base + 0x04u, vertex[i][1]);
        scan_pokef(base + 0x20u, vertex[i][2]);
        scan_pokef(base + 0x24u, vertex[i][3]);
    }
    scan_poke32(SCAN_CTX + 0x08u, 1280u);
    scan_poke32(SCAN_CTX + 0x0cu, UINT32_C(0x013fffff));
    scan_poke32(SCAN_CTX + 0x10u, UINT32_C(0x0000ffff));
    scan_poke32(SCAN_CTX + 0x14u, sampler);
    scan_poke32(SCAN_CTX + 0x18u, sampler);
    scan_poke32(SCAN_CTX + 0x64u, UINT32_MAX);
    scan_poke32(SCAN_CTX + 0x68u, 1u);
    scan_poke32(SCAN_RENDER + 0x104u, 1280u);
    scan_poke32(SCAN_RENDER + 0x114u, 320u);
    scan_poke32(SCAN_RENDER + 0x11cu, 1u);
    scan_poke32(SCAN_STATE + 0x08u, 2u);
    scan_poke32(SCAN_STATE + 0x3cu, 0x12u);
    for (uint32_t i = 0; i < 320u; i++) {
        scan_poke32(SCAN_TEXELS + i * 4u, input_alpha | i);
        scan_poke32(SCAN_OUT + i * 4u, 0u);
    }
}

static void test_rect_root_replaces_textured_and_solid_rows_atomically(void) {
    static const uint32_t textured[6] = {
        UINT32_C(0x00112233), UINT32_C(0x44556677),
        UINT32_C(0x8899aabb), UINT32_C(0xccddeeff),
        UINT32_C(0x10203040), UINT32_C(0x50607080)
    };
    static const uint32_t solid_source[2] = {
        UINT32_C(0xff123456), UINT32_C(0xfd000000)
    };
    static const uint32_t solid_expected[2] = {
        UINT32_C(0xff123456), UINT32_C(0xff020202)
    };
    static const uint32_t mode_two_expected[6] = {
        UINT32_C(0xdeb9d713), UINT32_C(0xe4c8e316),
        UINT32_C(0xead8ef19), UINT32_C(0xf1e7fc1c),
        UINT32_C(0xdfbcd811), UINT32_C(0xe5cbe414)
    };
    ios3_hle_site_t *site = site_named("ogl_poly_scan");
    arm_cpu_t cpu;

    CHECK(site != NULL, "ogl_poly_scan site is missing");
    CHECK(site && site->mode == IOS3_HLE_REPLACE,
          "bounded rectangle root is not in REPLACE mode");
    reset_scan_fixture(site);
    configure_rect_root(&cpu, 0x030bu);
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "textured rectangle root declined");
        CHECK(cpu.r[15] == cpu.r[14],
              "textured rectangle returned to %08x, not LR", cpu.r[15]);
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 1u &&
              g_scan_writev_span_count == 2u,
              "textured rectangle published scalar=%u vector=%u/%u",
              g_scan_write_calls, g_scan_writev_calls,
              g_scan_writev_span_count);
        CHECK(g_scan_texel_read_calls == 2u &&
              g_scan_texel_read_bytes == 24u &&
              g_scan_texel_max_read_len == 12u,
              "textured rectangle used %u texture reads totalling %u, max %u",
              g_scan_texel_read_calls, g_scan_texel_read_bytes,
              g_scan_texel_max_read_len);
        CHECK(g_scan_writev_va[0] == SCAN_OUT + 4u &&
              g_scan_writev_va[1] == SCAN_OUT + 20u &&
              g_scan_writev_len[0] == 12u && g_scan_writev_len[1] == 12u,
              "textured rectangle spans are %08x/%u and %08x/%u",
              g_scan_writev_va[0], g_scan_writev_len[0],
              g_scan_writev_va[1], g_scan_writev_len[1]);
        for (uint32_t i = 0; i < 3u; i++) {
            CHECK(scan_peek32(SCAN_OUT + 4u + i * 4u) == textured[i],
                  "textured row 0 pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + 4u + i * 4u));
            CHECK(scan_peek32(SCAN_OUT + 20u + i * 4u) == textured[i + 3u],
                  "textured row 1 pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + 20u + i * 4u));
        }
        CHECK(scan_peek32(SCAN_OUT) == UINT32_C(0xdeadbeef) &&
              scan_peek32(SCAN_OUT + 16u) == UINT32_C(0xdeadbeef),
              "textured rectangle changed a pixel outside its two spans");
    }

    for (uint32_t pass = 0; pass < 2u; pass++) {
        reset_scan_fixture(site);
        configure_rect_root(&cpu, 0x000bu);
        scan_poke32(SCAN_CTX + 0x14u, 0u);
        scan_poke32(SCAN_CTX + 0x18u, 0u);
        scan_poke32(SCAN_CTX + 0x64u, solid_source[pass]);
        scan_poke32(SCAN_CTX + 0x68u, 0u);
        scan_poke32(SCAN_STATE + 0x3cu, pass ? 0x12u : 0x02u);
        if (!arm_only(site)) continue;
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "solid rectangle root pass %u declined", pass);
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 1u &&
              g_scan_writev_span_count == 2u,
              "solid rectangle pass %u published scalar=%u vector=%u/%u",
              pass, g_scan_write_calls, g_scan_writev_calls,
              g_scan_writev_span_count);
        for (uint32_t row = 0; row < 2u; row++)
            for (uint32_t x = 0; x < 3u; x++)
                CHECK(scan_peek32(SCAN_OUT + 4u + row * 16u + x * 4u) ==
                          solid_expected[pass],
                      "solid pass %u row %u pixel %u is %08x", pass, row, x,
                      scan_peek32(SCAN_OUT + 4u + row * 16u + x * 4u));
    }

    reset_scan_fixture(site);
    configure_rect_root(&cpu, 0x030bu);
    scan_poke32(SCAN_CTX + 0x64u, UINT32_C(0xb4b4b4b4));
    scan_poke32(SCAN_STATE + 0x08u, 2u);
    scan_poke32(SCAN_STATE + 0x3cu, 0x12u);
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "mode-two rectangle root declined");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 1u &&
              g_scan_writev_span_count == 2u,
              "mode-two root published scalar=%u vector=%u/%u",
              g_scan_write_calls, g_scan_writev_calls,
              g_scan_writev_span_count);
        for (uint32_t row = 0; row < 2u; row++)
            for (uint32_t x = 0; x < 3u; x++) {
                uint32_t index = row * 3u + x;
                uint32_t actual =
                    scan_peek32(SCAN_OUT + 4u + row * 16u + x * 4u);
                CHECK(actual == mode_two_expected[index],
                      "mode-two row %u pixel %u is %08x, expected %08x",
                      row, x, actual, mode_two_expected[index]);
            }
    }
    ios3_hle_disarm();
}

static void test_rect_root_chunks_measured_full_width_mode_two(void) {
    static const uint32_t sampler[2] = { SAMPLE_BGRA8, SAMPLE_BGRX8 };
    ios3_hle_site_t *site = site_named("ogl_poly_scan");
    arm_cpu_t cpu;

    for (uint32_t pass = 0; pass < 2u; pass++) {
        reset_scan_fixture(site);
        configure_full_width_mode_two_root(&cpu, sampler[pass]);
        if (!arm_only(site)) continue;
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "full-width mode-two %s root declined",
              pass ? "BGRX" : "BGRA");
        CHECK(cpu.r[15] == cpu.r[14],
              "full-width mode-two root returned to %08x, not LR", cpu.r[15]);
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 1u &&
              g_scan_writev_span_count == 1u &&
              g_scan_writev_va[0] == SCAN_OUT &&
              g_scan_writev_len[0] == 1280u,
              "full-width mode-two root published scalar=%u vector=%u/%u "
              "at %08x/%u", g_scan_write_calls, g_scan_writev_calls,
              g_scan_writev_span_count, g_scan_writev_va[0],
              g_scan_writev_len[0]);
        CHECK(g_scan_texel_read_calls == 2u &&
              g_scan_texel_read_bytes == 1280u &&
              g_scan_texel_max_read_len == 1024u,
              "full-width mode-two root used %u reads totalling %u, max %u",
              g_scan_texel_read_calls, g_scan_texel_read_bytes,
              g_scan_texel_max_read_len);
        for (uint32_t i = 0; i < 320u; i++) {
            uint32_t expected = UINT32_C(0xff000000) | i;
            CHECK(scan_peek32(SCAN_OUT + i * 4u) == expected,
                  "full-width mode-two pass %u pixel %u is %08x, expected %08x",
                  pass, i, scan_peek32(SCAN_OUT + i * 4u), expected);
        }
    }

    /* Apple exposes the first 256-pixel result before sampling the remainder.
     * Shift the texture back by 288 pixels so the second bulk read straddles
     * 32 untouched source pixels and 32 pixels produced by the first chunk.
     * The root still publishes only after both chunks have succeeded. */
    reset_scan_fixture(site);
    configure_full_width_mode_two_root(&cpu, SAMPLE_BGRA8);
    scan_poke32(SCAN_CTX + 0x04u, SCAN_OUT - 1152u);
    for (uint32_t i = 0; i < 288u; i++)
        scan_poke32(SCAN_OUT - 1152u + i * 4u,
                    UINT32_C(0xff000000) | i);
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "full-width prior-chunk alias root declined");
        CHECK(g_scan_writev_calls == 1u && g_scan_writev_span_count == 1u,
              "prior-chunk alias published %u transaction(s) / %u span(s)",
              g_scan_writev_calls, g_scan_writev_span_count);
        CHECK(g_scan_texel_read_calls == 1u &&
              g_scan_texel_read_bytes == 1024u,
              "mixed prior-chunk read used %u counted call(s) / %u bytes",
              g_scan_texel_read_calls, g_scan_texel_read_bytes);
        for (uint32_t i = 0; i < 320u; i++) {
            uint32_t source_index = i < 288u ? i : i - 288u;
            uint32_t expected = UINT32_C(0xff000000) | source_index;
            CHECK(scan_peek32(SCAN_OUT + i * 4u) == expected,
                  "mixed prior-chunk pixel %u is %08x, expected %08x", i,
                  scan_peek32(SCAN_OUT + i * 4u), expected);
        }
    }

    reset_scan_fixture(site);
    configure_full_width_mode_two_root(&cpu, SAMPLE_BGRA8);
    g_scan_fail_read_va = SCAN_TEXELS + 1024u;
    if (arm_only(site)) {
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "root handled although its second mode-two chunk faulted");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 0u,
              "second-chunk fault published scalar=%u vector=%u",
              g_scan_write_calls, g_scan_writev_calls);
        CHECK(scan_peek32(SCAN_OUT) == 0u &&
              scan_peek32(SCAN_OUT + 319u * 4u) == 0u,
              "second-chunk fault changed the guest destination");
    }

    for (uint32_t pass = 0; pass < 2u; pass++) {
        uint32_t count = pass ? 257u : 320u;

        reset_scan_fixture(site);
        configure_full_width_mode_two_root(&cpu, SAMPLE_BGRA8);
        scan_poke32(SCAN_STATE + 0x08u, 1u);
        if (count != 320u) {
            uint32_t right = SCAN_POLY + 4u + 0x38u;
            uint32_t bottom_right = SCAN_POLY + 4u + 2u * 0x38u;
            scan_pokef(right + 0x00u, (float)count);
            scan_pokef(right + 0x20u, (float)count);
            scan_pokef(bottom_right + 0x00u, (float)count);
            scan_pokef(bottom_right + 0x20u, (float)count);
        }
        if (!arm_only(site)) continue;
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "mode-one %u-pixel chunked root declined", count);
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 1u &&
              g_scan_writev_span_count == 1u &&
              g_scan_writev_va[0] == SCAN_OUT &&
              g_scan_writev_len[0] == count * 4u,
              "mode-one %u root published scalar=%u vector=%u/%u at %08x/%u",
              count, g_scan_write_calls, g_scan_writev_calls,
              g_scan_writev_span_count, g_scan_writev_va[0],
              g_scan_writev_len[0]);
        CHECK(g_scan_texel_read_calls == 2u &&
              g_scan_texel_read_bytes == count * 4u &&
              g_scan_texel_max_read_len == 1024u,
              "mode-one %u root used %u reads totalling %u, max %u", count,
              g_scan_texel_read_calls, g_scan_texel_read_bytes,
              g_scan_texel_max_read_len);
        for (uint32_t i = 0; i < count; i++)
            CHECK(scan_peek32(SCAN_OUT + i * 4u) ==
                      (UINT32_C(0xff000000) | i),
                  "mode-one %u root pixel %u is %08x", count, i,
                  scan_peek32(SCAN_OUT + i * 4u));
    }

    /* A synthetic nearest-sampler counterpart to the live 320-to-1 shape.
     * Pin both Apple's pixel-centre interpolation and the second-chunk start. */
    reset_scan_fixture(site);
    configure_full_width_mode_two_root(&cpu, SAMPLE_BGRA8);
    scan_pokef(SCAN_POLY + 4u + 0x38u + 0x20u, 1.0f);
    scan_pokef(SCAN_POLY + 4u + 2u * 0x38u + 0x20u, 1.0f);
    scan_poke32(SCAN_TEXELS, UINT32_C(0xff335577));
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "measured 320-to-1 textured root declined");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 1u &&
              g_scan_writev_span_count == 1u &&
              g_scan_writev_va[0] == SCAN_OUT &&
              g_scan_writev_len[0] == 1280u,
              "320-to-1 root published scalar=%u vector=%u/%u at %08x/%u",
              g_scan_write_calls, g_scan_writev_calls,
              g_scan_writev_span_count, g_scan_writev_va[0],
              g_scan_writev_len[0]);
        CHECK(g_scan_texel_read_calls == 2u &&
              g_scan_texel_read_bytes == 8u &&
              g_scan_texel_max_read_len == 4u,
              "320-to-1 root used %u reads totalling %u, max %u",
              g_scan_texel_read_calls, g_scan_texel_read_bytes,
              g_scan_texel_max_read_len);
        for (uint32_t i = 0; i < 320u; i++)
            CHECK(scan_peek32(SCAN_OUT + i * 4u) == UINT32_C(0xff335577),
                  "320-to-1 root pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + i * 4u));
    }

    reset_scan_fixture(site);
    configure_full_width_mode_two_root(&cpu, SAMPLE_BGRA8);
    scan_pokef(SCAN_POLY + 4u + 0x38u + 0x20u, 1.0f);
    scan_pokef(SCAN_POLY + 4u + 2u * 0x38u + 0x20u, 1.0f);
    scan_poke32(SCAN_STATE + 0x3cu, 0u);
    scan_poke32(SCAN_TEXELS, UINT32_C(0xff775533));
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "direct 320-to-1 textured root declined");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 1u &&
              g_scan_writev_span_count == 1u,
              "direct 320-to-1 root published scalar=%u vector=%u/%u",
              g_scan_write_calls, g_scan_writev_calls,
              g_scan_writev_span_count);
        CHECK(g_scan_texel_read_calls == 1u &&
              g_scan_texel_read_bytes == 4u &&
              g_scan_texel_max_read_len == 4u,
              "direct 320-to-1 root used %u reads totalling %u, max %u",
              g_scan_texel_read_calls, g_scan_texel_read_bytes,
              g_scan_texel_max_read_len);
        for (uint32_t i = 0; i < 320u; i++)
            CHECK(scan_peek32(SCAN_OUT + i * 4u) == UINT32_C(0xff775533),
                  "direct 320-to-1 root pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + i * 4u));
    }

    reset_scan_fixture(site);
    configure_full_width_mode_two_root(&cpu, SAMPLE_BGRA8);
    scan_pokef(SCAN_POLY + 4u + 0x38u + 0x20u, 1.0f);
    scan_pokef(SCAN_POLY + 4u + 2u * 0x38u + 0x20u, 1.0f);
    scan_poke32(SCAN_CTX + 0x04u, SCAN_OUT);
    if (arm_only(site)) {
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "320-to-1 same-row alias was buffered and handled");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 0u,
              "320-to-1 alias published scalar=%u vector=%u",
              g_scan_write_calls, g_scan_writev_calls);
        CHECK(scan_peek32(SCAN_OUT) == 0u &&
              scan_peek32(SCAN_OUT + 319u * 4u) == 0u,
              "320-to-1 alias changed its destination");
    }

    /* r317 identified the real scaled surface: bilinear BGRA, one 16.16
     * texel column, and exact vertical pixel centres. Exercise two rows so
     * the first probes its next row and the last clamps that probe in place. */
    reset_scan_fixture(site);
    configure_full_width_mode_two_root(&cpu, SAMPLE_BILINEAR_BGRA8);
    scan_pokef(SCAN_POLY + 4u + 0x38u + 0x20u, 1.0f);
    scan_pokef(SCAN_POLY + 4u + 2u * 0x38u + 0x04u, 2.0f);
    scan_pokef(SCAN_POLY + 4u + 2u * 0x38u + 0x20u, 1.0f);
    scan_pokef(SCAN_POLY + 4u + 2u * 0x38u + 0x24u, 2.0f);
    scan_pokef(SCAN_POLY + 4u + 3u * 0x38u + 0x04u, 2.0f);
    scan_pokef(SCAN_POLY + 4u + 3u * 0x38u + 0x24u, 2.0f);
    scan_poke32(SCAN_CTX + 0x08u, 4u);
    scan_poke32(SCAN_CTX + 0x0cu, UINT32_C(0x0000ffff));
    scan_poke32(SCAN_CTX + 0x10u, UINT32_C(0x0001ffff));
    scan_poke32(SCAN_RENDER + 0x104u, 1280u);
    scan_poke32(SCAN_RENDER + 0x11cu, 2u);
    scan_poke32(SCAN_STATE + 0x08u, 1u);
    scan_poke32(SCAN_TEXELS + 0u, UINT32_C(0xff335577));
    scan_poke32(SCAN_TEXELS + 4u, UINT32_C(0xff775533));
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "live one-column bilinear root declined");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 1u &&
              g_scan_writev_span_count == 2u &&
              g_scan_writev_va[0] == SCAN_OUT &&
              g_scan_writev_va[1] == SCAN_OUT + 1280u &&
              g_scan_writev_len[0] == 1280u &&
              g_scan_writev_len[1] == 1280u,
              "bilinear root published scalar=%u vector=%u/%u at "
              "%08x/%u and %08x/%u",
              g_scan_write_calls, g_scan_writev_calls,
              g_scan_writev_span_count, g_scan_writev_va[0],
              g_scan_writev_len[0], g_scan_writev_va[1],
              g_scan_writev_len[1]);
        CHECK(g_scan_texel_read_calls == 8u &&
              g_scan_texel_read_bytes == 32u &&
              g_scan_texel_max_read_len == 4u,
              "bilinear root used %u reads totalling %u, max %u",
              g_scan_texel_read_calls, g_scan_texel_read_bytes,
              g_scan_texel_max_read_len);
        for (uint32_t i = 0; i < 320u; i++) {
            CHECK(scan_peek32(SCAN_OUT + i * 4u) ==
                      UINT32_C(0xff335577),
                  "bilinear row zero pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + i * 4u));
            CHECK(scan_peek32(SCAN_OUT + 1280u + i * 4u) ==
                      UINT32_C(0xff775533),
                  "bilinear row one pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + 1280u + i * 4u));
        }
    }

    reset_scan_fixture(site);
    configure_full_width_mode_two_root(&cpu, SAMPLE_BILINEAR_BGRA8);
    scan_pokef(SCAN_POLY + 4u + 0x38u + 0x20u, 1.0f);
    scan_pokef(SCAN_POLY + 4u + 2u * 0x38u + 0x20u, 1.0f);
    scan_poke32(SCAN_CTX + 0x08u, 8u);
    scan_poke32(SCAN_CTX + 0x0cu, UINT32_C(0x0001ffff));
    scan_poke32(SCAN_CTX + 0x10u, UINT32_C(0x0000ffff));
    scan_poke32(SCAN_STATE + 0x08u, 1u);
    if (arm_only(site)) {
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "bilinear shortcut accepted a scaled two-column descriptor");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 0u,
              "two-column refusal published scalar=%u vector=%u",
              g_scan_write_calls, g_scan_writev_calls);
    }

    reset_scan_fixture(site);
    configure_full_width_mode_two_root(&cpu, SAMPLE_BILINEAR_BGRA8);
    scan_pokef(SCAN_POLY + 4u + 0x38u + 0x20u, 1.0f);
    scan_pokef(SCAN_POLY + 4u + 2u * 0x38u + 0x20u, 1.0f);
    scan_poke32(SCAN_CTX + 0x04u, SCAN_OUT);
    scan_poke32(SCAN_CTX + 0x08u, 4u);
    scan_poke32(SCAN_CTX + 0x0cu, UINT32_C(0x0000ffff));
    scan_poke32(SCAN_CTX + 0x10u, UINT32_C(0x0000ffff));
    scan_poke32(SCAN_STATE + 0x08u, 1u);
    if (arm_only(site)) {
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "bilinear same-row alias was buffered and handled");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 0u,
              "bilinear alias published scalar=%u vector=%u",
              g_scan_write_calls, g_scan_writev_calls);
    }

    reset_scan_fixture(site);
    configure_full_width_mode_two_root(&cpu, SAMPLE_BGRA8);
    scan_poke32(SCAN_STATE + 0x08u, 3u);
    if (arm_only(site)) {
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "root broadened the 320-pixel chunk path to unmeasured mode three");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 0u,
              "unmeasured full-width mode three published scalar=%u vector=%u",
              g_scan_write_calls, g_scan_writev_calls);
    }
    ios3_hle_disarm();
}

static void test_rect_root_applies_integer_clip_bounds(void) {
    ios3_hle_site_t *site = site_named("ogl_poly_scan");
    arm_cpu_t cpu;

    reset_scan_fixture(site);
    configure_rect_root(&cpu, 0x030bu);
    /* The polygon covers x=[1,4), y=[0,2), with matching texture coordinates.
     * Clip all four sides to x=[2,3), y=[1,2). The surviving pixel is source
     * row one, column one and destination row one, x two. */
    cpu.r[2] = 2u;
    cpu.r[3] = 1u;
    scan_poke32(SCAN_STACK + 0x00u, 3u);
    scan_poke32(SCAN_STACK + 0x04u, 2u);
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "integer-clipped rectangle root declined");
        CHECK(cpu.r[15] == cpu.r[14],
              "integer-clipped root returned to %08x, not LR", cpu.r[15]);
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 1u &&
              g_scan_writev_span_count == 1u &&
              g_scan_writev_va[0] == SCAN_OUT + 24u &&
              g_scan_writev_len[0] == 4u,
              "integer clip published scalar=%u vector=%u/%u at %08x/%u",
              g_scan_write_calls, g_scan_writev_calls,
              g_scan_writev_span_count, g_scan_writev_va[0],
              g_scan_writev_len[0]);
        CHECK(scan_peek32(SCAN_OUT + 24u) == UINT32_C(0x10203040),
              "integer clip sampled %08x, expected row-one column-one texel",
              scan_peek32(SCAN_OUT + 24u));
        for (uint32_t i = 0; i < 8u; i++) {
            if (i == 6u) continue;
            CHECK(scan_peek32(SCAN_OUT + i * 4u) == UINT32_C(0xdeadbeef),
                  "integer clip changed destination pixel %u", i);
        }
    }

    reset_scan_fixture(site);
    configure_rect_root(&cpu, 0x030bu);
    cpu.r[2] = 4u;
    scan_poke32(SCAN_STACK + 0x00u, 4u);
    if (arm_only(site)) {
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "empty clip rectangle was handled");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 0u,
              "empty clip published scalar=%u vector=%u",
              g_scan_write_calls, g_scan_writev_calls);
    }
    ios3_hle_disarm();
}

static void test_rect_root_matches_fractional_bilinear_rows(void) {
    const uint32_t top = UINT32_C(0x10203040);
    const uint32_t bottom = UINT32_C(0x90a0b0c0);
    const uint32_t blended = UINT32_C(0x708090a0);
    static const uint32_t wide_top[2] = {
        UINT32_C(0x10203040), UINT32_C(0x20304050)
    };
    static const uint32_t wide_bottom[2] = {
        UINT32_C(0x90a0b0c0), UINT32_C(0xa0b0c0d0)
    };
    static const uint32_t wide_blended[2] = {
        UINT32_C(0x708090a0), UINT32_C(0x8090a0b0)
    };
    ios3_hle_site_t *site = site_named("ogl_poly_scan");
    arm_cpu_t cpu;

    /* A measured-style 320-to-1 surface translated by one quarter pixel.
     * Apple's y coverage selects rows 0 and 1. Their pixel-centre v values are
     * 0.25 and 1.25, so the second row uses the packed sampler's 192/256 blend. */
    reset_scan_fixture(site);
    configure_full_width_mode_two_root(&cpu, SAMPLE_BILINEAR_BGRA8);
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t base = SCAN_POLY + 4u + i * 0x38u;
        scan_pokef(base + 0x04u, i < 2u ? 0.25f : 2.25f);
        scan_pokef(base + 0x24u, i < 2u ? 0.0f : 2.0f);
    }
    scan_pokef(SCAN_POLY + 4u + 0x38u + 0x20u, 1.0f);
    scan_pokef(SCAN_POLY + 4u + 2u * 0x38u + 0x20u, 1.0f);
    scan_poke32(SCAN_CTX + 0x08u, 4u);
    scan_poke32(SCAN_CTX + 0x0cu, UINT32_C(0x0000ffff));
    scan_poke32(SCAN_CTX + 0x10u, UINT32_C(0x0001ffff));
    scan_poke32(SCAN_RENDER + 0x104u, 1280u);
    scan_poke32(SCAN_RENDER + 0x11cu, 2u);
    scan_poke32(SCAN_STATE + 0x08u, 1u);
    scan_poke32(SCAN_TEXELS + 0u, top);
    scan_poke32(SCAN_TEXELS + 4u, bottom);
    for (uint32_t i = 0; i < 640u; i++)
        scan_poke32(SCAN_OUT + i * 4u, 0u);
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "fractional bilinear rectangle root declined");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 1u &&
              g_scan_writev_span_count == 2u &&
              g_scan_writev_va[0] == SCAN_OUT &&
              g_scan_writev_va[1] == SCAN_OUT + 1280u &&
              g_scan_writev_len[0] == 1280u &&
              g_scan_writev_len[1] == 1280u,
              "fractional root published scalar=%u vector=%u/%u at "
              "%08x/%u and %08x/%u",
              g_scan_write_calls, g_scan_writev_calls,
              g_scan_writev_span_count, g_scan_writev_va[0],
              g_scan_writev_len[0], g_scan_writev_va[1],
              g_scan_writev_len[1]);
        CHECK(g_scan_texel_read_calls == 8u &&
              g_scan_texel_read_bytes == 32u &&
              g_scan_texel_max_read_len == 4u,
              "fractional bilinear root used %u reads totalling %u, max %u",
              g_scan_texel_read_calls, g_scan_texel_read_bytes,
              g_scan_texel_max_read_len);
        for (uint32_t i = 0; i < 320u; i++) {
            CHECK(scan_peek32(SCAN_OUT + i * 4u) == top,
                  "fractional top pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + i * 4u));
            CHECK(scan_peek32(SCAN_OUT + 1280u + i * 4u) == blended,
                  "fractional blended pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + 1280u + i * 4u));
        }
    }

    /* The remaining r323 rows use wider descriptors, but their root mapping
     * starts u at a pixel centre and advances by exactly one texel. The native
     * sampler still reads all four taps; horizontal weight zero leaves the
     * exact packed vertical blend from the left pair. Alternating texels prove
     * that the shortcut advances across the row rather than repeating one. */
    reset_scan_fixture(site);
    configure_full_width_mode_two_root(&cpu, SAMPLE_BILINEAR_BGRA8);
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t base = SCAN_POLY + 4u + i * 0x38u;
        scan_pokef(base + 0x04u, i < 2u ? 0.25f : 2.25f);
        scan_pokef(base + 0x24u, i < 2u ? 0.0f : 2.0f);
    }
    scan_poke32(SCAN_CTX + 0x10u, UINT32_C(0x0001ffff));
    scan_poke32(SCAN_RENDER + 0x11cu, 2u);
    scan_poke32(SCAN_STATE + 0x08u, 1u);
    for (uint32_t i = 0; i < 320u; i++) {
        scan_poke32(SCAN_TEXELS + i * 4u, wide_top[i & 1u]);
        scan_poke32(SCAN_TEXELS + 1280u + i * 4u,
                    wide_bottom[i & 1u]);
        scan_poke32(SCAN_OUT + i * 4u, 0u);
        scan_poke32(SCAN_OUT + 1280u + i * 4u, 0u);
    }
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "fractional pixel-aligned bilinear root declined");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 1u &&
              g_scan_writev_span_count == 2u &&
              g_scan_writev_va[0] == SCAN_OUT &&
              g_scan_writev_va[1] == SCAN_OUT + 1280u &&
              g_scan_writev_len[0] == 1280u &&
              g_scan_writev_len[1] == 1280u,
              "wide fractional root published scalar=%u vector=%u/%u at "
              "%08x/%u and %08x/%u",
              g_scan_write_calls, g_scan_writev_calls,
              g_scan_writev_span_count, g_scan_writev_va[0],
              g_scan_writev_len[0], g_scan_writev_va[1],
              g_scan_writev_len[1]);
        CHECK(g_scan_texel_read_calls == 2560u &&
              g_scan_texel_read_bytes == 10240u &&
              g_scan_texel_max_read_len == 4u,
              "wide fractional root used %u reads totalling %u, max %u",
              g_scan_texel_read_calls, g_scan_texel_read_bytes,
              g_scan_texel_max_read_len);
        for (uint32_t i = 0; i < 320u; i++) {
            CHECK(scan_peek32(SCAN_OUT + i * 4u) == wide_top[i & 1u],
                  "wide fractional top pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + i * 4u));
            CHECK(scan_peek32(SCAN_OUT + 1280u + i * 4u) ==
                      wide_blended[i & 1u],
                  "wide fractional blended pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + 1280u + i * 4u));
        }
    }

    /* Fractional solid geometry was not observed and remains outside scope. */
    reset_scan_fixture(site);
    configure_rect_root(&cpu, 0x000bu);
    for (uint32_t i = 0; i < 4u; i++) {
        uint32_t base = SCAN_POLY + 4u + i * 0x38u;
        scan_pokef(base + 0x04u, i < 2u ? 0.25f : 2.25f);
    }
    if (arm_only(site)) {
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "fractional solid rectangle was handled");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 0u,
              "fractional solid published scalar=%u vector=%u",
              g_scan_write_calls, g_scan_writev_calls);
    }
    ios3_hle_disarm();
}

static void test_rect_root_preserves_prior_row_alias_semantics(void) {
    static const uint32_t expected[3] = {
        UINT32_C(0x80112233), UINT32_C(0x80445566),
        UINT32_C(0x80778899)
    };
    ios3_hle_site_t *site = site_named("ogl_poly_scan");
    arm_cpu_t cpu;

    reset_scan_fixture(site);
    configure_rect_root(&cpu, 0x030bu);
    /* Row zero reads a separate source. Row one then reads row zero's output.
     * Apple's top-to-bottom writes make the new pixels visible to row one; the
     * transactional host scratch must preserve that dependency. */
    scan_poke32(SCAN_CTX + 0x04u, SCAN_OUT - 12u);
    for (uint32_t i = 0; i < 3u; i++)
        scan_poke32(SCAN_OUT - 12u + i * 4u, expected[i]);
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "prior-row alias rectangle declined");
        for (uint32_t row = 0; row < 2u; row++)
            for (uint32_t x = 0; x < 3u; x++)
                CHECK(scan_peek32(SCAN_OUT + 4u + row * 16u + x * 4u) ==
                          expected[x],
                      "alias row %u pixel %u is %08x", row, x,
                      scan_peek32(SCAN_OUT + 4u + row * 16u + x * 4u));
    }
    ios3_hle_disarm();
}

static void test_rect_root_refuses_unproved_or_nonatomic_shapes(void) {
    ios3_hle_site_t *site = site_named("ogl_poly_scan");
    arm_cpu_t cpu;

    for (uint32_t which = 0; which < 8u; which++) {
        reset_scan_fixture(site);
        configure_rect_root(&cpu, 0x030bu);
        switch (which) {
        case 0: scan_poke32(SCAN_STACK + 0x08u, UINT32_C(0x3122d184)); break;
        case 1: cpu.r[1] = 1u; break;
        case 2: scan_pokef(SCAN_POLY + 4u, 1.25f); break;
        case 3: scan_pokef(SCAN_POLY + 4u + 0x38u + 0x20u, 2.0f); break;
        case 4: scan_poke16(SCAN_POLY + 0x02u, 0x030fu); break;
        case 5: g_scan_fail_read_va = SCAN_TEXELS + 0x10u; break;
        case 6: g_scan_fail_writev_span = 1u; break;
        default:
            /* The leaf refuses same-row source/destination aliasing. */
            scan_poke32(SCAN_CTX + 0x04u, SCAN_OUT + 4u);
            break;
        }
        if (!arm_only(site)) continue;
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "unproved/faulting rectangle %u was handled", which);
        CHECK(g_scan_write_calls == 0u,
              "declining rectangle %u made %u scalar write(s)", which,
              g_scan_write_calls);
        CHECK(g_scan_writev_calls == (which == 6u ? 1u : 0u),
              "declining rectangle %u made %u vector write(s)", which,
              g_scan_writev_calls);
        for (uint32_t i = 0; i < 8u; i++)
            CHECK(scan_peek32(SCAN_OUT + i * 4u) == UINT32_C(0xdeadbeef),
                  "declining rectangle %u changed destination pixel %u",
                  which, i);
        CHECK(cpu.r[15] == site->va,
              "declining rectangle %u moved PC to %08x", which, cpu.r[15]);
        CHECK(site->handled == 0u && site->declined == 1u,
              "declining rectangle %u accounting is handled=%llu declined=%llu",
              which, (unsigned long long)site->handled,
              (unsigned long long)site->declined);
    }
    ios3_hle_disarm();
}

static void test_rect_root_oracle_captures_two_rows_without_publication(void) {
    static const uint32_t expected[6] = {
        UINT32_C(0x00112233), UINT32_C(0x44556677),
        UINT32_C(0x8899aabb), UINT32_C(0xccddeeff),
        UINT32_C(0x10203040), UINT32_C(0x50607080)
    };
    ios3_hle_site_t *site = site_named("ogl_poly_scan");
    ios3_hle_oracle_t oracle;
    uint8_t oracle_bytes[64];
    arm_cpu_t cpu, before;

    reset_scan_fixture(site);
    configure_rect_root(&cpu, 0x030bu);
    before = cpu;
    if (arm_only(site)) {
        CHECK(ios3_hle_oracle_prepare(&cpu, &SCAN_MEM, site->va,
                                      0x0bf1b000u, &oracle, oracle_bytes,
                                      (uint32_t)sizeof oracle_bytes),
              "rectangle oracle declined the proved two-row fixture");
        CHECK(oracle.site_name &&
              strcmp(oracle.site_name, "ogl_poly_scan") == 0,
              "rectangle oracle selected the wrong site");
        CHECK(oracle.span_count == 2u && oracle.expected_len == 24u &&
              oracle.spans[0].va == SCAN_OUT + 4u &&
              oracle.spans[1].va == SCAN_OUT + 20u &&
              oracle.spans[0].len == 12u && oracle.spans[1].len == 12u,
              "rectangle oracle captured %u spans totalling %u bytes",
              oracle.span_count, oracle.expected_len);
        CHECK(memcmp(oracle.expected, expected, sizeof expected) == 0,
              "rectangle oracle captured the wrong row bytes");
        CHECK(memcmp(&cpu, &before, sizeof cpu) == 0,
              "rectangle oracle changed the live CPU");
        CHECK(g_scan_write_calls == 0u && g_scan_writev_calls == 0u,
              "rectangle oracle forwarded scalar=%u vector=%u writes",
              g_scan_write_calls, g_scan_writev_calls);
        for (uint32_t i = 0; i < 8u; i++)
            CHECK(scan_peek32(SCAN_OUT + i * 4u) == UINT32_C(0xdeadbeef),
                  "rectangle oracle changed destination pixel %u", i);

        CHECK(!ios3_hle_oracle_prepare(&cpu, &SCAN_MEM, site->va,
                                       0x0bf1b000u, &oracle, oracle_bytes,
                                       20u),
              "rectangle oracle accepted storage smaller than both rows");
        CHECK(memcmp(&cpu, &before, sizeof cpu) == 0,
              "capacity-refusing rectangle oracle changed the CPU");
    }
    ios3_hle_disarm();
}

static void test_direct_solid_scanline_repeats_the_context_pixel(void) {
    static const uint32_t colors[2] = { 0xff000000u, 0xfd000000u };
    ios3_hle_site_t *site = site_named("sw_scanline");

    for (unsigned pass = 0; pass < 2u; pass++) {
        arm_cpu_t cpu;
        reset_scan_fixture(site);
        configure_solid_scanline(&cpu, colors[pass]);
        if (!arm_only(site)) continue;

        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "solid scanline %u declined", pass);
        CHECK(cpu.r[15] == cpu.r[14],
              "solid scanline returned to %08x, not LR", cpu.r[15]);
        CHECK(g_scan_write_calls == 1u &&
              g_scan_write_va == SCAN_OUT + 20u && g_scan_write_len == 12u,
              "solid span published as %u write(s) at %08x len %u",
              g_scan_write_calls, g_scan_write_va, g_scan_write_len);
        for (unsigned i = 0; i < 3u; i++)
            CHECK(scan_peek32(SCAN_OUT + 20u + i * 4u) == colors[pass],
                  "solid pass %u pixel %u is %08x", pass, i,
                  scan_peek32(SCAN_OUT + 20u + i * 4u));
        CHECK(scan_peek32(SCAN_OUT + 16u) == 0xdeadbeefu,
              "solid pass %u overwrote the preceding pixel", pass);
    }
    ios3_hle_disarm();
}

static void test_blended_solid_scanline_matches_selector_two(void) {
    ios3_hle_site_t *site = site_named("sw_scanline");
    arm_cpu_t cpu;

    reset_scan_fixture(site);
    configure_solid_scanline(&cpu, 0xfd000000u);
    scan_poke32(SCAN_STATE + 0x3cu, 0x12u);
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "blended solid scanline declined");
        CHECK(cpu.r[15] == cpu.r[14],
              "blended solid returned to %08x, not LR", cpu.r[15]);
        CHECK(g_scan_write_calls == 1u &&
              g_scan_write_va == SCAN_OUT + 20u && g_scan_write_len == 12u,
              "blended solid span published as %u write(s) at %08x len %u",
              g_scan_write_calls, g_scan_write_va, g_scan_write_len);
        for (unsigned i = 0; i < 3u; i++)
            CHECK(scan_peek32(SCAN_OUT + 20u + i * 4u) == 0xff020202u,
                  "blended solid pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + 20u + i * 4u));
    }
    ios3_hle_disarm();
}

static void test_blended_solid_scanline_covers_the_measured_display_width(void) {
    ios3_hle_site_t *site = site_named("sw_scanline");
    arm_cpu_t cpu;

    reset_scan_fixture(site);
    configure_solid_scanline(&cpu, UINT32_C(0xfd000000));
    cpu.r[0] = 0u;
    cpu.r[1] = 0u;
    cpu.r[2] = 320u;
    scan_poke32(SCAN_STATE + 0x3cu, 0x12u);
    scan_poke32(SCAN_RENDER + 0x104u, 1280u);
    scan_poke32(SCAN_RENDER + 0x114u, 320u);
    scan_poke32(SCAN_RENDER + 0x11cu, 1u);
    for (uint32_t i = 0; i < 320u; i++)
        scan_poke32(SCAN_OUT + i * 4u, UINT32_C(0xdeadbeef));
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "blended full-width solid scanline declined");
        CHECK(g_scan_write_calls == 1u && g_scan_write_va == SCAN_OUT &&
              g_scan_write_len == 1280u,
              "full-width solid published as %u write(s) at %08x len %u",
              g_scan_write_calls, g_scan_write_va, g_scan_write_len);
        for (uint32_t i = 0; i < 320u; i++)
            CHECK(scan_peek32(SCAN_OUT + i * 4u) == UINT32_C(0xff020202),
                  "full-width solid pixel %u is %08x", i,
                  scan_peek32(SCAN_OUT + i * 4u));
    }

    reset_scan_fixture(site);
    configure_solid_scanline(&cpu, UINT32_C(0xfd000000));
    cpu.r[0] = 0u;
    cpu.r[1] = 0u;
    cpu.r[2] = 321u;
    scan_poke32(SCAN_STATE + 0x3cu, 0x12u);
    scan_poke32(SCAN_RENDER + 0x104u, 1284u);
    scan_poke32(SCAN_RENDER + 0x114u, 321u);
    if (arm_only(site)) {
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "blended solid accepted 321 pixels past its host bound");
        CHECK(g_scan_write_calls == 0u,
              "oversize blended solid published %u write(s)",
              g_scan_write_calls);
    }
    ios3_hle_disarm();
}

static void test_solid_scanline_unknown_shapes_and_faults_decline(void) {
    ios3_hle_site_t *site = site_named("sw_scanline");
    arm_cpu_t cpu;

    for (unsigned which = 0; which < 4u; which++) {
        reset_scan_fixture(site);
        configure_solid_scanline(&cpu, 0xff123456u);
        switch (which) {
        case 0: scan_poke32(SCAN_STATE + 0x3cu, 0x00u); break;
        case 1: scan_poke32(SCAN_STACK + 0x0cu, 0x0308u); break;
        case 2: scan_poke32(SCAN_RENDER + 0x10cu, 2u); break;
        default: g_scan_fail_write = true; break;
        }
        if (!arm_only(site)) continue;
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "unproved/faulting solid state %u was handled", which);
        CHECK(g_scan_write_calls == (which == 3u ? 1u : 0u),
              "solid state %u attempted %u writes", which,
              g_scan_write_calls);
        CHECK(scan_peek32(SCAN_OUT + 20u) == 0xdeadbeefu,
              "solid state %u changed the destination", which);
        CHECK(cpu.r[15] == site->va,
              "solid state %u moved PC to %08x", which, cpu.r[15]);
    }
    ios3_hle_disarm();
}

static void configure_live_bgra_blend(arm_cpu_t *cpu) {
    configure_direct_scanline(cpu, SAMPLE_BGRA8);
    scan_poke32(SCAN_STATE + 0x08u, 1u);
    scan_poke32(SCAN_STATE + 0x3cu, 0x12u);

    /* Alpha 0, 128 and 255 pin all three boundaries of selector 2. The first
     * source is deliberately non-premultiplied: the expected value therefore
     * also proves that this is the guest's packed ADD, not a pretty rewrite. */
    scan_poke32(SCAN_TEXELS + 0x00u, 0x00112233u);
    scan_poke32(SCAN_TEXELS + 0x04u, 0x80402010u);
    scan_poke32(SCAN_TEXELS + 0x08u, 0xff010203u);
    scan_poke32(SCAN_OUT + 20u, 0x10203040u);
    scan_poke32(SCAN_OUT + 24u, 0x80a0c0e0u);
    scan_poke32(SCAN_OUT + 28u, 0xff112233u);
}

static void configure_live_mode_two(arm_cpu_t *cpu, uint32_t sampler,
                                    uint32_t color) {
    configure_live_bgra_blend(cpu);
    scan_poke32(SCAN_CTX + 0x14u, sampler);
    scan_poke32(SCAN_CTX + 0x18u, sampler);
    scan_poke32(SCAN_CTX + 0x64u, color);
    scan_poke32(SCAN_STATE + 0x08u, 2u);
}

static void test_live_mode_two_modulates_then_blends(void) {
    static const uint32_t sampler[2] = { SAMPLE_BGRA8, SAMPLE_BGRX8 };
    static const uint32_t color[2] = {
        UINT32_C(0x7f804020), UINT32_C(0xfdfdfdfd)
    };
    static const uint32_t expected[2][3] = {
        { UINT32_C(0x10293846), UINT32_C(0x9f9898aa),
          UINT32_C(0xff091119) },
        { UINT32_C(0xfd112233), UINT32_C(0xfe412212),
          UINT32_C(0xff010203) }
    };
    ios3_hle_site_t *site = site_named("sw_scanline");

    for (uint32_t pass = 0; pass < 2u; pass++) {
        arm_cpu_t cpu;
        reset_scan_fixture(site);
        configure_live_mode_two(&cpu, sampler[pass], color[pass]);
        if (!arm_only(site)) continue;
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "mode-two %s scanline declined", pass ? "BGRX" : "BGRA");
        CHECK(cpu.r[15] == cpu.r[14],
              "mode-two scanline returned to %08x, not LR", cpu.r[15]);
        CHECK(g_scan_write_calls == 1u &&
              g_scan_write_va == SCAN_OUT + 20u && g_scan_write_len == 12u,
              "mode-two span published as %u write(s) at %08x len %u",
              g_scan_write_calls, g_scan_write_va, g_scan_write_len);
        for (uint32_t i = 0; i < 3u; i++)
            CHECK(scan_peek32(SCAN_OUT + 20u + i * 4u) == expected[pass][i],
                  "mode-two pass %u pixel %u is %08x, expected %08x",
                  pass, i, scan_peek32(SCAN_OUT + 20u + i * 4u),
                  expected[pass][i]);
    }

    reset_scan_fixture(site);
    {
        arm_cpu_t cpu;
        configure_live_mode_two(&cpu, SAMPLE_BGRA8,
                                UINT32_C(0x7f804020));
        scan_poke32(SCAN_STACK + 0x0cu, 0x0318u);
        if (arm_only(site)) {
            CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
                  "mode two accepted an interpolated-colour mask");
            CHECK(g_scan_write_calls == 0u,
                  "interpolated mode two published %u write(s)",
                  g_scan_write_calls);
        }
    }
    ios3_hle_disarm();
}

static void test_live_bgra_blend_matches_arm_packed_selector_two(void) {
    static const uint32_t expected[3] = {
        0x10315273u, 0xc0908080u, 0xff010203u
    };
    ios3_hle_site_t *site = site_named("sw_scanline");
    arm_cpu_t cpu;

    reset_scan_fixture(site);
    configure_live_bgra_blend(&cpu);
    if (arm_only(site)) {
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "live BGRA selector-2 blend declined");
        CHECK(cpu.r[15] == cpu.r[14],
              "blended scanline returned to %08x, not LR", cpu.r[15]);
        CHECK(g_scan_write_calls == 1u &&
              g_scan_write_va == SCAN_OUT + 20u && g_scan_write_len == 12u,
              "blended span published as %u write(s) at %08x len %u",
              g_scan_write_calls, g_scan_write_va, g_scan_write_len);
        for (unsigned i = 0; i < 3u; i++)
            CHECK(scan_peek32(SCAN_OUT + 20u + i * 4u) == expected[i],
                  "selector-2 pixel %u is %08x, expected %08x", i,
                  scan_peek32(SCAN_OUT + 20u + i * 4u), expected[i]);
        CHECK(site->hits == 1u && site->handled == 1u && site->declined == 0u,
              "blend accounting is %llu/%llu/%llu",
              (unsigned long long)site->hits,
              (unsigned long long)site->handled,
              (unsigned long long)site->declined);
    }
    ios3_hle_disarm();
}

static void test_live_bgra_blend_faults_and_unproved_shapes_decline(void) {
    ios3_hle_site_t *site = site_named("sw_scanline");
    arm_cpu_t cpu;

    reset_scan_fixture(site);
    configure_live_bgra_blend(&cpu);
    if (arm_only(site)) {
        g_scan_fail_read_va = SCAN_OUT + 24u;
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "blend handled although the destination span faulted");
        CHECK(g_scan_write_calls == 0u,
              "destination-fault blend published %u write(s)",
              g_scan_write_calls);
        CHECK(scan_peek32(SCAN_OUT + 20u) == 0x10203040u,
              "destination-fault blend changed its first pixel");
        CHECK(cpu.r[15] == site->va,
              "destination-fault blend moved PC to %08x", cpu.r[15]);
    }

    for (unsigned which = 0; which < 5u; which++) {
        reset_scan_fixture(site);
        configure_live_bgra_blend(&cpu);
        switch (which) {
        case 0: scan_poke32(SCAN_STATE + 0x3cu, 0x13u); break;
        case 1: scan_poke32(SCAN_STATE + 0x08u, 3u); break;
        case 2:
            scan_poke32(SCAN_CTX + 0x14u, SAMPLE_BGRX8);
            scan_poke32(SCAN_CTX + 0x18u, SAMPLE_BGRX8);
            break;
        case 3: scan_poke32(SCAN_RENDER + 0x100u, SCAN_OUT + 0x100u); break;
        default:
            /* Keep the destination in bounds so this specifically exercises
             * the guest's 256-pixel chunk boundary. */
            cpu.r[2] = 257u;
            scan_poke32(SCAN_RENDER + 0x104u, 1280u);
            scan_poke32(SCAN_RENDER + 0x114u, 320u);
            break;
        }
        if (!arm_only(site)) continue;
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "unproved blended state %u was handled", which);
        CHECK(g_scan_write_calls == 0u,
              "unproved blended state %u published %u write(s)",
              which, g_scan_write_calls);
        CHECK(scan_peek32(SCAN_OUT + 20u) == 0x10203040u,
              "unproved blended state %u changed the destination", which);
        CHECK(site->declined == 1u && site->handled == 0u,
              "unproved blended state %u accounting is %llu/%llu", which,
              (unsigned long long)site->declined,
              (unsigned long long)site->handled);
    }
    ios3_hle_disarm();
}

static void test_direct_scanline_faults_and_unknown_states_decline_cleanly(void) {
    ios3_hle_site_t *site = site_named("sw_scanline");
    arm_cpu_t cpu;

    reset_scan_fixture(site);
    configure_direct_scanline(&cpu, SAMPLE_BGRA8);
    if (arm_only(site)) {
        g_scan_fail_read_va = SCAN_TEXELS + 4u;
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "scanline handled although its second texel faulted");
        CHECK(g_scan_write_calls == 0u,
              "scanline published %u write(s) before a source fault",
              g_scan_write_calls);
        CHECK(scan_peek32(SCAN_OUT + 20u) == 0xdeadbeefu,
              "source-fault decline changed the destination");
        CHECK(cpu.r[15] == site->va,
              "source-fault decline moved PC to %08x", cpu.r[15]);
        CHECK(site->declined == 1u && site->handled == 0u,
              "source-fault accounting is declined=%llu handled=%llu",
              (unsigned long long)site->declined,
              (unsigned long long)site->handled);
    }

    for (unsigned which = 0; which < 6u; which++) {
        reset_scan_fixture(site);
        configure_direct_scanline(&cpu, SAMPLE_BGRA8);
        switch (which) {
        case 0: scan_poke32(SCAN_STATE + 0x3cu, 0x10u); break;
        case 1: scan_poke32(SCAN_CTX + 0x68u, 3u); break;
        case 2: scan_poke32(SCAN_STACK + 0x0cu, 0x0309u); break;
        case 3: scan_poke32(SCAN_STACK + 0x0cu, 0x0318u); break;
        case 4: scan_poke32(SCAN_CTX + 0x64u, 0u); break;
        default: scan_poke32(SCAN_CTX + 0x18u, SAMPLE_BGRX8); break;
        }
        if (!arm_only(site)) continue;
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "unproved scanline state %u was handled", which);
        CHECK(g_scan_write_calls == 0u,
              "unproved state %u published %u write(s)",
              which, g_scan_write_calls);
        CHECK(scan_peek32(SCAN_OUT + 20u) == 0xdeadbeefu,
              "unproved state %u changed the destination", which);
        CHECK(site->declined == 1u && site->handled == 0u,
              "unproved state %u accounting is %llu/%llu", which,
              (unsigned long long)site->declined,
              (unsigned long long)site->handled);
    }
    ios3_hle_disarm();
}

static void test_nearest_leaf_sites_match_the_decoded_32bit_formats(void) {
    static const char *const names[2] = {
        "sw_sample_nearest_BGRA8", "sw_sample_nearest_BGRX8"
    };
    static const uint32_t alpha[2] = { 0x00000000u, 0xff000000u };

    for (unsigned pass = 0; pass < 2u; pass++) {
        ios3_hle_site_t *site = site_named(names[pass]);
        arm_cpu_t cpu;

        reset_scan_fixture(site);
        configure_direct_scanline(&cpu, SAMPLE_BGRA8);
        cpu.r[0] = SCAN_CTX + 0x04u;
        cpu.r[1] = 0u;
        cpu.r[2] = 0xfeedfaceu; /* decoded as unused */
        cpu.r[3] = SCAN_START;
        cpu.r[13] = SCAN_STACK;
        cpu.r[14] = 0x1eafc0deu;
        cpu.r[15] = site ? site->va : 0u;
        scan_poke32(SCAN_STACK + 0x00u, SCAN_DELTA);
        scan_poke32(SCAN_STACK + 0x0cu, 3u);
        scan_poke32(SCAN_STACK + 0x10u, SCAN_OUT + 0x40u);

        CHECK(site != NULL, "%s site is missing", names[pass]);
        CHECK(site && site->mode == IOS3_HLE_REPLACE,
              "%s is not in REPLACE mode", names[pass]);
        if (!arm_only(site)) continue;
        CHECK(ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "%s leaf declined the decoded fixture", names[pass]);
        CHECK(cpu.r[15] == cpu.r[14], "%s returned to %08x, not LR",
              names[pass], cpu.r[15]);
        CHECK(g_scan_write_calls == 1u &&
              g_scan_write_va == SCAN_OUT + 0x40u &&
              g_scan_write_len == 12u,
              "%s published as %u write(s) at %08x len %u", names[pass],
              g_scan_write_calls, g_scan_write_va, g_scan_write_len);
        CHECK(g_scan_texel_read_calls == 1u &&
              g_scan_texel_read_bytes == 12u &&
              g_scan_texel_max_read_len == 12u,
              "%s used %u texture reads totalling %u, max %u", names[pass],
              g_scan_texel_read_calls, g_scan_texel_read_bytes,
              g_scan_texel_max_read_len);
        CHECK(scan_peek32(SCAN_OUT + 0x40u) ==
                  (0x00112233u | alpha[pass]),
              "%s pixel 0 mismatch", names[pass]);
        CHECK(scan_peek32(SCAN_OUT + 0x44u) ==
                  (0x44556677u | alpha[pass]),
              "%s pixel 1 mismatch", names[pass]);
        CHECK(scan_peek32(SCAN_OUT + 0x48u) ==
                  (0x8899aabbu | alpha[pass]),
              "%s pixel 2 mismatch", names[pass]);
    }
    ios3_hle_disarm();
}

static void test_buffering_never_changes_alias_or_write_fault_semantics(void) {
    ios3_hle_site_t *site = site_named("sw_scanline");
    arm_cpu_t cpu;

    reset_scan_fixture(site);
    configure_direct_scanline(&cpu, SAMPLE_BGRA8);
    /* At x=1,y=1 this makes the destination begin at the source texture. */
    scan_poke32(SCAN_RENDER + 0xfcu, SCAN_TEXELS - 20u);
    if (arm_only(site)) {
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "an aliased texture/destination span was buffered and handled");
        CHECK(g_scan_write_calls == 0u,
              "an aliased span published %u write(s)", g_scan_write_calls);
        CHECK(scan_peek32(SCAN_TEXELS) == 0x00112233u,
              "alias refusal changed the first source texel");
    }

    reset_scan_fixture(site);
    configure_direct_scanline(&cpu, SAMPLE_BGRA8);
    if (arm_only(site)) {
        g_scan_fail_write = true;
        CHECK(!ios3_hle_step(&cpu, &SCAN_MEM, site->va, 0x0bf1b000u),
              "scanline reported handled after its destination write failed");
        CHECK(g_scan_write_calls == 1u,
              "write-fault fixture observed %u writes", g_scan_write_calls);
        CHECK(scan_peek32(SCAN_OUT + 20u) == 0xdeadbeefu,
              "failed destination write changed the span");
        CHECK(cpu.r[15] == site->va,
              "write-fault decline moved PC to %08x", cpu.r[15]);
    }
    ios3_hle_disarm();
}

static void test_live_oracle_captures_without_touching_the_guest(void) {
    static const uint32_t EXPECTED_BGRX[3] = {
        0xff112233u, 0xff556677u, 0xff99aabbu
    };
    ios3_hle_site_t *site = site_named("sw_scanline");
    ios3_hle_oracle_t oracle;
    uint8_t oracle_bytes[1280];
    arm_cpu_t cpu, before;

    reset_scan_fixture(site);
    configure_direct_scanline(&cpu, SAMPLE_BGRX8);
    before = cpu;
    if (arm_only(site)) {
        CHECK(ios3_hle_oracle_prepare(&cpu, &SCAN_MEM, site->va,
                                      0x0bf1b000u, &oracle, oracle_bytes,
                                      (uint32_t)sizeof oracle_bytes),
              "live oracle declined the proved direct scanline");
        CHECK(oracle.site_index != UINT32_MAX &&
              oracle.site_name && strcmp(oracle.site_name, "sw_scanline") == 0,
              "oracle selected the wrong site");
        CHECK(oracle.return_pc == cpu.r[14],
              "oracle return PC is %08x, expected %08x",
              oracle.return_pc, cpu.r[14]);
        CHECK(oracle.return_sp == cpu.r[13],
              "oracle return SP is %08x, expected %08x",
              oracle.return_sp, cpu.r[13]);
        CHECK(oracle.span_count == 1u &&
              oracle.spans[0].va == SCAN_OUT + 20u &&
              oracle.spans[0].len == 12u && oracle.expected_len == 12u,
              "oracle captured %u span(s), first %08x/%u, total %u",
              oracle.span_count, oracle.spans[0].va, oracle.spans[0].len,
              oracle.expected_len);
        CHECK(memcmp(&cpu, &before, sizeof cpu) == 0,
              "oracle changed the live CPU");
        CHECK(g_scan_write_calls == 0u,
              "oracle forwarded %u write(s) into guest memory",
              g_scan_write_calls);
        CHECK(scan_peek32(SCAN_OUT + 20u) == 0xdeadbeefu,
              "oracle changed the guest destination");
        CHECK(memcmp(oracle.expected, EXPECTED_BGRX,
                     sizeof EXPECTED_BGRX) == 0,
              "oracle captured the wrong expected BGRX pixels");
        CHECK(site->hits == 0u && site->handled == 0u && site->declined == 0u,
              "oracle polluted replacement counters");
    }

    reset_scan_fixture(site);
    configure_direct_scanline(&cpu, SAMPLE_BGRA8);
    scan_poke32(SCAN_STACK + 0x0cu, 0x0309u);
    if (arm_only(site)) {
        CHECK(!ios3_hle_oracle_prepare(&cpu, &SCAN_MEM, site->va,
                                       0x0bf1b000u, &oracle, oracle_bytes,
                                       (uint32_t)sizeof oracle_bytes),
              "oracle accepted an unproved callback mask");
        CHECK(oracle.site_index != UINT32_MAX,
              "oracle did not distinguish handler refusal from no site");
        CHECK(g_scan_write_calls == 0u,
              "declining oracle touched guest memory");
        CHECK(!ios3_hle_oracle_prepare(&cpu, &SCAN_MEM, site->va,
                                       0x0bad0000u, &oracle, oracle_bytes,
                                       (uint32_t)sizeof oracle_bytes),
              "oracle ran in the wrong address space");
        CHECK(oracle.site_index == UINT32_MAX,
              "wrong-space oracle exposed a candidate site");
    }
    ios3_hle_disarm();
}

static void test_live_oracle_captures_direct_and_blended_solids(void) {
    static const uint32_t colors[2] = { 0xff123456u, 0xfd000000u };
    static const uint32_t expected[2] = { 0xff123456u, 0xff020202u };
    ios3_hle_site_t *site = site_named("sw_scanline");

    for (unsigned pass = 0; pass < 2u; pass++) {
        ios3_hle_oracle_t oracle;
        uint8_t oracle_bytes[1280];
        arm_cpu_t cpu, before;

        reset_scan_fixture(site);
        configure_solid_scanline(&cpu, colors[pass]);
        if (pass) scan_poke32(SCAN_STATE + 0x3cu, 0x12u);
        before = cpu;
        if (!arm_only(site)) continue;

        CHECK(ios3_hle_oracle_prepare(&cpu, &SCAN_MEM, site->va,
                                      0x0bf1b000u, &oracle, oracle_bytes,
                                      (uint32_t)sizeof oracle_bytes),
              "oracle declined solid pass %u", pass);
        CHECK(oracle.span_count == 1u &&
              oracle.spans[0].va == SCAN_OUT + 20u &&
              oracle.spans[0].len == 12u && oracle.expected_len == 12u,
              "solid oracle pass %u captured %u span(s), first %08x/%u", pass,
              oracle.span_count, oracle.spans[0].va, oracle.spans[0].len);
        for (unsigned i = 0; i < 3u; i++) {
            uint32_t got = 0u;
            memcpy(&got, oracle.expected + i * 4u, sizeof got);
            CHECK(got == expected[pass],
                  "solid oracle pass %u pixel %u is %08x", pass, i, got);
        }
        CHECK(memcmp(&cpu, &before, sizeof cpu) == 0,
              "solid oracle pass %u changed the CPU", pass);
        CHECK(g_scan_write_calls == 0u,
              "solid oracle pass %u forwarded %u write(s)", pass,
              g_scan_write_calls);
        CHECK(scan_peek32(SCAN_OUT + 20u) == 0xdeadbeefu,
              "solid oracle pass %u changed guest memory", pass);
    }
    ios3_hle_disarm();
}

int main(void) {
    printf("S5LBox iPhone OS 3 userspace HLE tests\n");
    test_a_site_without_a_prologue_refuses_to_arm();
    test_the_shipped_sites_carry_a_distinguishing_prologue();
    test_nothing_armed_never_intercepts();
    test_identity_is_checked_word_for_word();
    test_the_wrong_address_space_is_refused_and_counted();
    test_observe_mode_never_takes_the_call();
    test_trace_cannot_mutate_or_intercept_the_guest();
    test_declining_is_safe();
    test_oracle_captures_disjoint_transactional_spans();
    test_direct_scanline_preserves_bgra_and_forces_bgrx_alpha();
    test_direct_scanline_accepts_the_measured_full_width_bound();
    test_identity_bulk_read_falls_back_to_exact_texels();
    test_direct_solid_scanline_repeats_the_context_pixel();
    test_rect_root_replaces_textured_and_solid_rows_atomically();
    test_rect_root_chunks_measured_full_width_mode_two();
    test_rect_root_applies_integer_clip_bounds();
    test_rect_root_matches_fractional_bilinear_rows();
    test_rect_root_preserves_prior_row_alias_semantics();
    test_rect_root_refuses_unproved_or_nonatomic_shapes();
    test_rect_root_oracle_captures_two_rows_without_publication();
    test_blended_solid_scanline_matches_selector_two();
    test_blended_solid_scanline_covers_the_measured_display_width();
    test_solid_scanline_unknown_shapes_and_faults_decline();
    test_live_bgra_blend_matches_arm_packed_selector_two();
    test_live_mode_two_modulates_then_blends();
    test_live_bgra_blend_faults_and_unproved_shapes_decline();
    test_direct_scanline_faults_and_unknown_states_decline_cleanly();
    test_nearest_leaf_sites_match_the_decoded_32bit_formats();
    test_buffering_never_changes_alias_or_write_fault_semantics();
    test_live_oracle_captures_without_touching_the_guest();
    test_live_oracle_captures_direct_and_blended_solids();
    printf("== ios3 hle: %u checks, %u failure(s) ==\n", checks, failures);
    return failures ? 1 : 0;
}
