/*
 * S5LBox — the shared harness for the firmware-import suite.
 *
 * One test binary, several translation units, because the import core is
 * several decoders and each one is worth failing on its own line. A check that
 * fails prints the file, the line and what it expected, and the run keeps going
 * so a broken decoder reports all of its damage in one pass rather than one
 * failure per rebuild.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_VM_FIRMWARE_TEST_H
#define S5LBOX_VM_FIRMWARE_TEST_H

#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned checks;
    unsigned failures;
    const char *section;
} vmfw_test_t;

#define VMFW_T_SECTION(t, name) do { (t)->section = (name); } while (0)

#define VMFW_T_CHECK(t, cond, ...) do {                                    \
    (t)->checks++;                                                         \
    if (!(cond)) {                                                         \
        (t)->failures++;                                                   \
        fprintf(stderr, "FAIL %s:%d [%s] ", __FILE__, __LINE__,            \
                (t)->section ? (t)->section : "-");                        \
        fprintf(stderr, __VA_ARGS__);                                      \
        fputc('\n', stderr);                                               \
    }                                                                      \
} while (0)

#define VMFW_T_EQ_U(t, got, want, what)                                    \
    VMFW_T_CHECK(t, (unsigned long long)(got) == (unsigned long long)(want), \
                 "%s: got %llu want %llu", (what),                         \
                 (unsigned long long)(got), (unsigned long long)(want))

#define VMFW_T_EQ_STR(t, got, want, what)                                  \
    VMFW_T_CHECK(t, strcmp((got), (want)) == 0,                            \
                 "%s: got \"%s\" want \"%s\"", (what), (got), (want))

#define VMFW_T_EQ_MEM(t, got, want, n, what)                               \
    VMFW_T_CHECK(t, memcmp((got), (want), (size_t)(n)) == 0,               \
                 "%s: %zu bytes differ", (what), (size_t)(n))

/* Each translation unit contributes one of these. */
void vmfw_test_inflate(vmfw_test_t *t);
void vmfw_test_digest(vmfw_test_t *t);
void vmfw_test_plist(vmfw_test_t *t);
void vmfw_test_zip(vmfw_test_t *t);
void vmfw_test_dmg(vmfw_test_t *t);
void vmfw_test_import(vmfw_test_t *t);

#endif /* S5LBOX_VM_FIRMWARE_TEST_H */
