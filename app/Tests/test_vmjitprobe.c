/*
 * Host-side tests for app/Sources/VMJitProbe.c — the "can this device execute
 * code it wrote itself?" probe.
 *
 * WHAT THIS CAN AND CANNOT ASSERT. The probe's whole purpose is to report a
 * platform policy, and a test cannot assert what that policy IS without
 * becoming a test of the machine it happens to run on. Asserting
 * VM_JIT_RESULT_OK would fail correctly on a hardened host and tell us nothing
 * about the one device the answer is wanted for.
 *
 * So the contract asserted here is the one that has to hold everywhere:
 *
 *   - every strategy returns a value from the enum, and does not crash;
 *   - a refusal is REPORTED rather than fatal -- which is the entire safety
 *     claim, since this code is meant to run on a user's phone;
 *   - OK implies the sentinel really came back, so a pass can never be a
 *     false positive from an untouched variable;
 *   - the names are total, including for out-of-range input, because they
 *     reach a log line the user is asked to read back.
 *
 * On the Windows dev box VM_JIT_POSIX is 0 and every run returns UNSUPPORTED,
 * so the mechanism is exercised by CI instead: the core-tests matrix runs
 * x86-64 Linux (which takes the x86 payload) and arm64 macOS (the aarch64 one).
 * That is the only place the emitted bytes are really executed.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMJitProbe.h"

#include <stdio.h>
#include <string.h>

static unsigned checks;
static unsigned failures;

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

static bool is_known_result(vm_jit_result_t r) {
    switch (r) {
        case VM_JIT_RESULT_UNTESTED:
        case VM_JIT_RESULT_UNSUPPORTED:
        case VM_JIT_RESULT_MAP_REFUSED:
        case VM_JIT_RESULT_PROTECT_REFUSED:
        case VM_JIT_RESULT_FAULTED:
        case VM_JIT_RESULT_WRONG_VALUE:
        case VM_JIT_RESULT_OK:
            return true;
        default:
            return false;
    }
}

/* Names are total. An unnamed enum value would reach the user's log as a
 * crash or an empty string, and this line is the one they read back to us. */
static void test_names_are_total(void) {
    for (int i = -3; i < (int)VM_JIT_STRATEGY_COUNT + 3; i++) {
        const char *n = vm_jit_strategy_name((vm_jit_strategy_t)i);
        CHECK(n != NULL && *n != '\0', "strategy %d has no name", i);
    }
    for (int i = -3; i < (int)VM_JIT_RESULT_OK + 3; i++) {
        const char *n = vm_jit_result_text((vm_jit_result_t)i);
        CHECK(n != NULL && *n != '\0', "result %d has no text", i);
    }
    /* The three strategies must not share a name; the report names which one
     * worked, and two identical labels would make that report useless. */
    for (int a = 0; a < (int)VM_JIT_STRATEGY_COUNT; a++)
        for (int b = a + 1; b < (int)VM_JIT_STRATEGY_COUNT; b++)
            CHECK(strcmp(vm_jit_strategy_name((vm_jit_strategy_t)a),
                         vm_jit_strategy_name((vm_jit_strategy_t)b)) != 0,
                  "strategies %d and %d share a name", a, b);
}

/* Out-of-range input is rejected without touching mmap or a signal handler. */
static void test_out_of_range_is_refused(void) {
    uint32_t observed = 0xdeadbeefu;
    vm_jit_result_t r = vm_jit_probe_run((vm_jit_strategy_t)-1, &observed);
    CHECK(r == VM_JIT_RESULT_UNSUPPORTED,
          "strategy -1 returned %d, expected UNSUPPORTED", (int)r);
    r = vm_jit_probe_run((vm_jit_strategy_t)VM_JIT_STRATEGY_COUNT, &observed);
    CHECK(r == VM_JIT_RESULT_UNSUPPORTED,
          "an out-of-range strategy returned %d, expected UNSUPPORTED", (int)r);
    CHECK(observed == 0xdeadbeefu,
          "a refused strategy wrote to `observed` (0x%08x)", observed);
}

/*
 * The safety claim, and the reason this file exists: running every strategy
 * must be survivable. If any of them can take the process down, this test
 * does not report a failure -- it never returns at all, and that is the
 * signal.
 */
static void test_every_strategy_is_survivable(void) {
    for (int i = 0; i < (int)VM_JIT_STRATEGY_COUNT; i++) {
        uint32_t observed = 0u;
        bool touched = false;
        vm_jit_result_t r = vm_jit_probe_run((vm_jit_strategy_t)i, &observed);

        CHECK(is_known_result(r), "%s returned unknown result %d",
              vm_jit_strategy_name((vm_jit_strategy_t)i), (int)r);
        CHECK(r != VM_JIT_RESULT_UNTESTED,
              "%s returned UNTESTED, which means it fell through its own switch",
              vm_jit_strategy_name((vm_jit_strategy_t)i));

        /* A pass must be a real pass. */
        if (r == VM_JIT_RESULT_OK) {
            touched = true;
            CHECK(observed == VM_JIT_PROBE_SENTINEL,
                  "%s reported OK but observed 0x%08x, not the sentinel 0x%04x",
                  vm_jit_strategy_name((vm_jit_strategy_t)i),
                  observed, (unsigned)VM_JIT_PROBE_SENTINEL);
        }
        if (r == VM_JIT_RESULT_WRONG_VALUE) {
            touched = true;
            CHECK(observed != VM_JIT_PROBE_SENTINEL,
                  "%s reported WRONG_VALUE but observed the sentinel",
                  vm_jit_strategy_name((vm_jit_strategy_t)i));
        }
        /* Anything that never ran must not have invented a value. */
        if (!touched)
            CHECK(observed == 0u,
                  "%s did not execute but wrote 0x%08x to `observed`",
                  vm_jit_strategy_name((vm_jit_strategy_t)i), observed);

        printf("  [info] %-16s -> %s\n",
               vm_jit_strategy_name((vm_jit_strategy_t)i),
               vm_jit_result_text(r));
    }
}

/* NULL `observed` is legal -- the caller may only want the verdict. */
static void test_null_observed_is_allowed(void) {
    for (int i = 0; i < (int)VM_JIT_STRATEGY_COUNT; i++) {
        vm_jit_result_t r = vm_jit_probe_run((vm_jit_strategy_t)i, NULL);
        CHECK(is_known_result(r),
              "%s with NULL observed returned unknown result %d",
              vm_jit_strategy_name((vm_jit_strategy_t)i), (int)r);
    }
}

/* supported() must agree with what run() actually does, or a caller that hides
 * the control on !supported() would be hiding a working feature. */
static void test_supported_agrees_with_run(void) {
    const bool supported = vm_jit_probe_supported();
    unsigned unsupported_count = 0;
    for (int i = 0; i < (int)VM_JIT_STRATEGY_COUNT; i++)
        if (vm_jit_probe_run((vm_jit_strategy_t)i, NULL)
            == VM_JIT_RESULT_UNSUPPORTED)
            unsupported_count++;

    if (!supported)
        CHECK(unsupported_count == (unsigned)VM_JIT_STRATEGY_COUNT,
              "probe_supported() is false but only %u of %d strategies said "
              "UNSUPPORTED", unsupported_count, (int)VM_JIT_STRATEGY_COUNT);
    else
        CHECK(unsupported_count < (unsigned)VM_JIT_STRATEGY_COUNT,
              "probe_supported() is true but every strategy said UNSUPPORTED");
}

int main(void) {
    printf("S5LBox JIT executability probe tests\n");
    printf("  [info] probe supported on this host: %s\n",
           vm_jit_probe_supported() ? "yes" : "no");

    test_names_are_total();
    test_out_of_range_is_refused();
    test_every_strategy_is_survivable();
    test_null_observed_is_allowed();
    test_supported_agrees_with_run();

    printf("== vm jit probe: %u checks, %u failure(s) ==\n", checks, failures);
    return failures ? 1 : 0;
}
