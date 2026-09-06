#include "native_pc_histogram.h"
#include "compact_guest_pc_sample.h"
#include <stdio.h>

static native_pc_histogram_t histogram;
static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
    printf("line %d: %s\n", __LINE__, #c); } } while (0)

static native_pc_histogram_bucket_t *find(uintptr_t key) {
    for (unsigned i = 0; i < NATIVE_PC_HISTOGRAM_CAPACITY; ++i)
        if (histogram.bucket[i].samples && histogram.bucket[i].key == key)
            return &histogram.bucket[i];
    return NULL;
}

static uint64_t sum(void) {
    uint64_t count = 0;
    for (unsigned i = 0; i < NATIVE_PC_HISTOGRAM_CAPACITY; ++i)
        count += histogram.bucket[i].samples;
    return count;
}

int main(void) {
    uint32_t guest_pc = 0xdeadbeefu;
    CHECK(compact_guest_pc_sample(0x104u, 0x104u, 0x200u, 0u, &guest_pc));
    CHECK(guest_pc == 0u);
    CHECK(compact_guest_pc_sample(0x1fcu, 0x104u, 0x200u,
                                  UINT32_C(0xfffffffe), &guest_pc));
    CHECK(guest_pc == UINT32_C(0xfffffffe));
    const struct { uintptr_t pc, begin, end; uint64_t x26; } invalid[] = {
        {0x100u, 0x104u, 0x200u, 0x1200u}, /* prologue */
        {0x200u, 0x104u, 0x200u, 0x1200u}, /* epilogue */
        {0x204u, 0x104u, 0x200u, 0x1200u}, /* callback/outside */
        {0x106u, 0x104u, 0x200u, 0x1200u}, /* invalid host PC */
        {0x104u, 0u, 0x200u, 0x1200u},
        {0x104u, 0x104u, 0x104u, 0x1200u},
        {0x104u, 0x200u, 0x104u, 0x1200u},
        {0x108u, 0x106u, 0x200u, 0x1200u},
        {0x108u, 0x104u, 0x202u, 0x1200u},
        {0x104u, 0x104u, 0x200u, 0x1201u},
        {0x104u, 0x104u, 0x200u, UINT64_C(0x100001200)},
        {0x104u, 0x104u, 0x200u, UINT64_MAX},
    };
    for (unsigned i = 0u; i < sizeof invalid / sizeof invalid[0]; ++i) {
        guest_pc = 0xdeadbeefu;
        CHECK(!compact_guest_pc_sample(invalid[i].pc, invalid[i].begin,
                                       invalid[i].end, invalid[i].x26,
                                       &guest_pc));
        CHECK(guest_pc == 0xdeadbeefu);
    }
    CHECK(!compact_guest_pc_sample(0x104u, 0x104u, 0x200u, 0x1200u, NULL));
    native_pc_histogram_reset(&histogram);
    /* The former 4096-sample capture filled during startup and missed all of
     * this later hot phase. Capture must not depend on snapshot polling. */
    for (unsigned i = 0; i < 8192u; ++i)
        CHECK(native_pc_histogram_note(&histogram, 0x104u));
    for (unsigned i = 0; i < 100000u; ++i)
        CHECK(native_pc_histogram_note(&histogram, 0x20cu + (i & 3u) * 4u));
    native_pc_histogram_bucket_t *early = find(0x100u), *late = find(0x200u);
    CHECK(early && early->samples == 8192u && early->pc == 0x104u);
    CHECK(late && late->samples == 100000u && late->pc == 0x20cu);
    CHECK(histogram.captured == 108192u && histogram.dropped == 0u);
    CHECK(sum() == histogram.captured);

    native_pc_histogram_reset(&histogram);
    for (unsigned i = 0; i < NATIVE_PC_HISTOGRAM_CAPACITY; ++i)
        CHECK(native_pc_histogram_note(&histogram, (uintptr_t)i * 256u));
    CHECK(histogram.captured == NATIVE_PC_HISTOGRAM_CAPACITY);
    CHECK(!native_pc_histogram_note(&histogram,
                                   (uintptr_t)NATIVE_PC_HISTOGRAM_CAPACITY * 256u));
    CHECK(histogram.dropped == 1u);
    /* Even a completely full table must find every existing collision-chain
     * entry and accept its samples; key zero is not the empty sentinel. */
    for (unsigned i = NATIVE_PC_HISTOGRAM_CAPACITY; i-- > 0u;)
        CHECK(native_pc_histogram_note(&histogram, (uintptr_t)i * 256u + 252u));
    CHECK(histogram.captured == 2u * NATIVE_PC_HISTOGRAM_CAPACITY);
    CHECK(sum() == histogram.captured);
    for (unsigned i = 0; i < NATIVE_PC_HISTOGRAM_CAPACITY; ++i) {
        native_pc_histogram_bucket_t *b = find((uintptr_t)i * 256u);
        CHECK(b && b->samples == 2u && b->pc == (uintptr_t)i * 256u);
    }
    native_pc_histogram_bucket_t *zero = find(0u);
    CHECK(zero != NULL);
    if (zero) zero->samples = UINT64_MAX;
    histogram.captured = histogram.dropped = UINT64_MAX;
    CHECK(native_pc_histogram_note(&histogram, 4u));
    CHECK(zero && zero->samples == UINT64_MAX);
    CHECK(histogram.captured == UINT64_MAX);
    CHECK(!native_pc_histogram_note(&histogram, UINTPTR_MAX));
    CHECK(histogram.dropped == UINT64_MAX);
    native_pc_histogram_reset(&histogram);
    CHECK(histogram.captured == 0u && histogram.dropped == 0u && sum() == 0u);
    CHECK(native_pc_histogram_note(&histogram, UINTPTR_MAX));
    CHECK(sum() == 1u);
    printf("native PC streaming histogram: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
