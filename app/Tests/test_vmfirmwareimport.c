/*
 * S5LBox — the firmware-import suite.
 *
 * One binary over six translation units. Everything the importer touches comes
 * out of a file the user downloaded from somewhere, so the properties defended
 * here are mostly negative ones: a truncated archive is refused rather than
 * half-read, an impossible length is refused rather than clamped, a chunk that
 * decompresses to the wrong size is caught at the chunk rather than at the end
 * of a 433 MB file, and a key that belongs to another device produces a named
 * failure rather than a plausible-looking image.
 *
 * The suite deliberately contains no Apple data. Every fixture is either
 * synthetic or a manifest of version strings and member names, so it runs on a
 * public CI runner. The proof that this actually reproduces the real artefacts
 * byte for byte lives outside the suite, in the firmware the developer already
 * has; see the commit message.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareTest.h"

#include <stdio.h>

int main(void) {
    vmfw_test_t t = { 0u, 0u, NULL };

    struct { const char *name; void (*fn)(vmfw_test_t *); } parts[] = {
        { "inflate", vmfw_test_inflate },
        { "digest",  vmfw_test_digest  },
        { "plist",   vmfw_test_plist   },
        { "zip",     vmfw_test_zip     },
        { "dmg",     vmfw_test_dmg     },
        { "import",  vmfw_test_import  },
    };

    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        unsigned before_checks = t.checks, before_fail = t.failures;
        t.section = parts[i].name;
        parts[i].fn(&t);
        printf("  %-8s %5u checks, %u failed\n", parts[i].name,
               t.checks - before_checks, t.failures - before_fail);
    }

    printf("firmware import: %u checks, %u failures\n", t.checks, t.failures);
    return t.failures == 0 ? 0 : 1;
}
