/*
 * Tests for tools/ios3_hle.c — guest-function interception.
 *
 * What is worth asserting here is almost entirely the REFUSALS. A facility
 * that replaces a function in a running OS is dangerous in proportion to how
 * willing it is to fire, so every test below is a way of not firing:
 * without a recorded prologue, against the wrong bytes, in the wrong address
 * space, or when the handler declines.
 *
 * The one thing a unit test cannot check is whether a replacement computes the
 * same pixels as Apple's code. That needs a frame diff of a real run with the
 * site armed and disarmed, which is the oracle named in the header's contract.
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
static const ios3_hle_mem_t FAKE = { NULL, fake_read, fake_write };

static void poke(uint32_t va, uint32_t word) {
    (void)fake_write(NULL, va, &word, 4u);
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

int main(void) {
    printf("S5LBox iPhone OS 3 userspace HLE tests\n");
    test_a_site_without_a_prologue_refuses_to_arm();
    test_the_shipped_sites_carry_a_distinguishing_prologue();
    test_nothing_armed_never_intercepts();
    test_identity_is_checked_word_for_word();
    test_the_wrong_address_space_is_refused_and_counted();
    test_observe_mode_never_takes_the_call();
    test_declining_is_safe();
    printf("== ios3 hle: %u checks, %u failure(s) ==\n", checks, failures);
    return failures ? 1 : 0;
}
