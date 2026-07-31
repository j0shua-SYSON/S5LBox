/*
 * S5LBox — copy-on-write overlays.
 *
 * The property under test is not "a write is recorded". It is that the disk
 * can be put back, and that every way of half-recording it is refused rather
 * than replayed. A snapshot that restores a past which never existed is worse
 * than one that refuses to restore at all.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMSnapshotCow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_checks, g_failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* ------------------------------------------- a disk that lives in memory ---
 *
 * Deliberately returns SHORT counts on alternate calls. The real adapter may
 * satisfy part of a request, and code that assumes otherwise records a block
 * padded with stale scratch -- a corruption that only appears at restore time,
 * long after anything can point at the cause. */
typedef struct {
    uint8_t *bytes;
    uint64_t size;
    unsigned calls;
    bool     be_awkward;
} ram_disk_t;

static vm_block_io_status_t ram_read(void *ctx, uint64_t off, void *dst,
                                     size_t want, size_t *actual) {
    ram_disk_t *d = (ram_disk_t *)ctx;
    if (off + want > d->size) return VM_BLOCK_IO_ERROR;
    size_t give = want;
    if (d->be_awkward && (++d->calls & 1u) && want > 1u) give = want / 2u;
    memcpy(dst, d->bytes + off, give);
    *actual = give;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t ram_write(void *ctx, uint64_t off, const void *src,
                                      size_t want, size_t *actual) {
    ram_disk_t *d = (ram_disk_t *)ctx;
    if (off + want > d->size) return VM_BLOCK_IO_ERROR;
    size_t take = want;
    if (d->be_awkward && (++d->calls & 1u) && want > 1u) take = want / 2u;
    memcpy(d->bytes + off, src, take);
    *actual = take;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t ram_flush(void *ctx) { (void)ctx; return VM_BLOCK_IO_OK; }

static void ram_open(ram_disk_t *d, vm_block_t *b, uint64_t size, bool awkward) {
    d->bytes = (uint8_t *)malloc((size_t)size);
    d->size = size;
    d->calls = 0;
    d->be_awkward = awkward;
    for (uint64_t i = 0; i < size; i++) d->bytes[i] = (uint8_t)(i * 7u + 3u);
    memset(b, 0, sizeof *b);
    b->context = d;
    b->size = size;
    b->read_at = ram_read;
    b->write_at = ram_write;
    b->flush = ram_flush;
}

static void ram_close(ram_disk_t *d) { free(d->bytes); d->bytes = NULL; }

#define DISK_BLOCKS 8u
#define DISK_BYTES  ((uint64_t)VM_COW_BLOCK_BYTES * DISK_BLOCKS)

static void path_in(char *out, size_t cap, const char *dir, const char *leaf) {
    snprintf(out, cap, "%s/%s", dir, leaf);
}

/* ------------------------------------------------------------- the tests --- */

static void test_restores_the_disk(const char *dir) {
    char overlay[512];
    path_in(overlay, sizeof overlay, dir, "cow-restore.bin");
    remove(overlay);

    ram_disk_t disk; vm_block_t under;
    ram_open(&disk, &under, DISK_BYTES, true);      /* awkward on purpose */

    uint8_t *original = (uint8_t *)malloc((size_t)DISK_BYTES);
    memcpy(original, disk.bytes, (size_t)DISK_BYTES);

    vm_cow_t *cow = NULL;
    CHECK(vm_cow_open(&cow, &under, overlay, NULL, 0) == VM_COW_OK,
          "the overlay must open");
    const vm_block_t *front = vm_cow_block(cow);
    CHECK(front != NULL, "a facade must be returned");

    /* Scribble over three separate places, one of them twice and one of them
     * straddling a block boundary. */
    uint8_t junk[VM_COW_BLOCK_BYTES];
    memset(junk, 0xA5, sizeof junk);
    size_t actual = 0;
    CHECK(front->write_at(front->context, 0, junk, 64u, &actual) == VM_BLOCK_IO_OK,
          "write at 0 must succeed");
    CHECK(front->write_at(front->context, VM_COW_BLOCK_BYTES - 32u, junk, 64u,
                          &actual) == VM_BLOCK_IO_OK,
          "a straddling write must succeed");
    CHECK(front->write_at(front->context, 5u * VM_COW_BLOCK_BYTES, junk,
                          VM_COW_BLOCK_BYTES, &actual) == VM_BLOCK_IO_OK,
          "a whole-block write must succeed");
    CHECK(front->write_at(front->context, 0, junk, 16u, &actual) == VM_BLOCK_IO_OK,
          "re-writing a saved block must succeed");

    CHECK(memcmp(disk.bytes, original, (size_t)DISK_BYTES) != 0,
          "the disk must actually have changed");

    uint64_t bytes = vm_cow_overlay_bytes(cow);
    CHECK(vm_cow_close(&cow) == VM_COW_OK, "close must succeed");
    CHECK(cow == NULL, "close must clear the caller's pointer");

    /* Blocks 0, 1 and 5 were touched; the second write to block 0 must not
     * have added a second record, because the older copy is the one that
     * matters and a newer one replayed after it would undo the restore. */
    uint64_t expect = 32u + 3u * (8u + VM_COW_BLOCK_BYTES);
    CHECK(bytes == expect, "overlay is %llu bytes, expected %llu",
          (unsigned long long)bytes, (unsigned long long)expect);

    CHECK(vm_cow_replay(overlay, &under, NULL, NULL, NULL, 0) == VM_COW_OK,
          "replay must succeed");
    CHECK(memcmp(disk.bytes, original, (size_t)DISK_BYTES) == 0,
          "the disk must be byte-identical to before");

    free(original);
    ram_close(&disk);
    remove(overlay);
}

static void test_untouched_blocks_are_not_recorded(const char *dir) {
    char overlay[512];
    path_in(overlay, sizeof overlay, dir, "cow-sparse.bin");
    remove(overlay);

    ram_disk_t disk; vm_block_t under;
    ram_open(&disk, &under, DISK_BYTES, false);

    vm_cow_t *cow = NULL;
    CHECK(vm_cow_open(&cow, &under, overlay, NULL, 0) == VM_COW_OK, "open");
    /* Reading everything must record nothing: an overlay that grew on reads
     * would be the size of the disk and the whole scheme pointless. */
    uint8_t *sink = (uint8_t *)malloc((size_t)DISK_BYTES);
    size_t actual = 0;
    const vm_block_t *front = vm_cow_block(cow);
    for (uint64_t off = 0; off < DISK_BYTES; off += VM_COW_BLOCK_BYTES)
        front->read_at(front->context, off, sink, VM_COW_BLOCK_BYTES, &actual);
    CHECK(vm_cow_overlay_bytes(cow) == 32u,
          "reads must not grow the overlay: %llu",
          (unsigned long long)vm_cow_overlay_bytes(cow));
    free(sink);
    vm_cow_close(&cow);
    ram_close(&disk);
    remove(overlay);
}

static void test_reopen_appends_rather_than_duplicating(const char *dir) {
    char overlay[512];
    path_in(overlay, sizeof overlay, dir, "cow-reopen.bin");
    remove(overlay);

    ram_disk_t disk; vm_block_t under;
    ram_open(&disk, &under, DISK_BYTES, false);
    uint8_t *original = (uint8_t *)malloc((size_t)DISK_BYTES);
    memcpy(original, disk.bytes, (size_t)DISK_BYTES);

    uint8_t junk[64];
    memset(junk, 0x5A, sizeof junk);
    size_t actual = 0;

    vm_cow_t *cow = NULL;
    CHECK(vm_cow_open(&cow, &under, overlay, NULL, 0) == VM_COW_OK, "first open");
    const vm_block_t *front = vm_cow_block(cow);
    front->write_at(front->context, 0, junk, sizeof junk, &actual);
    vm_cow_close(&cow);

    /* Reopen -- as a relaunched app would -- and write the SAME block again.
     * If the bitmap were not rebuilt, this would append a second, newer copy,
     * and replay would apply it last and restore the wrong contents. */
    CHECK(vm_cow_open(&cow, &under, overlay, NULL, 0) == VM_COW_OK, "reopen");
    front = vm_cow_block(cow);
    memset(junk, 0x11, sizeof junk);
    front->write_at(front->context, 0, junk, sizeof junk, &actual);
    /* ...and a block it has never seen, which must be recorded. */
    front->write_at(front->context, 3u * VM_COW_BLOCK_BYTES, junk, sizeof junk,
                    &actual);
    uint64_t bytes = vm_cow_overlay_bytes(cow);
    vm_cow_close(&cow);

    uint64_t expect = 32u + 2u * (8u + VM_COW_BLOCK_BYTES);
    CHECK(bytes == expect, "after reopen overlay is %llu, expected %llu",
          (unsigned long long)bytes, (unsigned long long)expect);

    CHECK(vm_cow_replay(overlay, &under, NULL, NULL, NULL, 0) == VM_COW_OK,
          "replay after reopen");
    CHECK(memcmp(disk.bytes, original, (size_t)DISK_BYTES) == 0,
          "a reopened overlay must still restore exactly");

    free(original);
    ram_close(&disk);
    remove(overlay);
}

static void test_refusals(const char *dir) {
    char overlay[512];
    path_in(overlay, sizeof overlay, dir, "cow-refuse.bin");
    remove(overlay);

    ram_disk_t disk; vm_block_t under;
    ram_open(&disk, &under, DISK_BYTES, false);

    vm_cow_t *cow = NULL;
    CHECK(vm_cow_open(&cow, &under, overlay, NULL, 0) == VM_COW_OK, "open");
    const vm_block_t *front = vm_cow_block(cow);
    uint8_t junk[64];
    memset(junk, 0x77, sizeof junk);
    size_t actual = 0;
    front->write_at(front->context, 0, junk, sizeof junk, &actual);
    vm_cow_close(&cow);

    /* A disk of a different length. Replaying would put every block index past
     * the shorter length at the wrong offset. */
    ram_disk_t other; vm_block_t other_block;
    ram_open(&other, &other_block, DISK_BYTES * 2u, false);
    CHECK(vm_cow_replay(overlay, &other_block, NULL, NULL, NULL, 0)
              == VM_COW_SIZE_MISMATCH,
          "a different-sized disk must be refused");
    ram_close(&other);

    /* A trailing partial record: a crash during append. Reported, never
     * skipped, because skipping restores a half-recorded past. */
    FILE *f = fopen(overlay, "ab");
    if (f) { fputc(0x00, f); fclose(f); }
    CHECK(vm_cow_replay(overlay, &under, NULL, NULL, NULL, 0) == VM_COW_TRUNCATED,
          "a trailing partial record must be refused");
    /* And the same file must be refused when reopened for writing, rather than
     * appended to as if it were whole. */
    CHECK(vm_cow_open(&cow, &under, overlay, NULL, 0) == VM_COW_TRUNCATED,
          "reopening a truncated overlay must be refused");

    /* Something that is not an overlay at all. */
    char bogus[512];
    path_in(bogus, sizeof bogus, dir, "cow-bogus.bin");
    f = fopen(bogus, "wb");
    if (f) { fwrite("not an overlay at all, honestly", 1, 31, f); fclose(f); }
    CHECK(vm_cow_replay(bogus, &under, NULL, NULL, NULL, 0) == VM_COW_BAD_FORMAT,
          "a non-overlay must be refused");

    /* A disk that is not a whole number of blocks. */
    ram_disk_t ragged; vm_block_t ragged_block;
    ram_open(&ragged, &ragged_block, VM_COW_BLOCK_BYTES + 17u, false);
    char ragged_path[512];
    path_in(ragged_path, sizeof ragged_path, dir, "cow-ragged.bin");
    remove(ragged_path);
    CHECK(vm_cow_open(&cow, &ragged_block, ragged_path, NULL, 0)
              == VM_COW_BAD_ARGUMENT,
          "a ragged disk must be refused");
    ram_close(&ragged);

    ram_close(&disk);
    remove(overlay);
    remove(bogus);
    remove(ragged_path);
}

static uint64_t g_progress_calls, g_progress_last, g_progress_total;
static void note_progress(void *ctx, uint64_t done, uint64_t total) {
    (void)ctx;
    g_progress_calls++;
    g_progress_last = done;
    g_progress_total = total;
}

static void test_progress_reaches_the_end(const char *dir) {
    char overlay[512];
    path_in(overlay, sizeof overlay, dir, "cow-progress.bin");
    remove(overlay);

    ram_disk_t disk; vm_block_t under;
    ram_open(&disk, &under, DISK_BYTES, false);

    vm_cow_t *cow = NULL;
    vm_cow_open(&cow, &under, overlay, NULL, 0);
    const vm_block_t *front = vm_cow_block(cow);
    uint8_t junk[16];
    memset(junk, 0x33, sizeof junk);
    size_t actual = 0;
    for (uint64_t b = 0; b < 4u; b++)
        front->write_at(front->context, b * VM_COW_BLOCK_BYTES, junk,
                        sizeof junk, &actual);
    vm_cow_close(&cow);

    g_progress_calls = g_progress_last = g_progress_total = 0;
    CHECK(vm_cow_replay(overlay, &under, note_progress, NULL, NULL, 0)
              == VM_COW_OK, "replay with progress");
    /* The bar must actually arrive: one call per record plus the initial zero,
     * and the last one must equal the total rather than stopping short. */
    CHECK(g_progress_calls == 5u, "progress called %llu times, expected 5",
          (unsigned long long)g_progress_calls);
    CHECK(g_progress_total == 4u * VM_COW_BLOCK_BYTES,
          "total was %llu", (unsigned long long)g_progress_total);
    CHECK(g_progress_last == g_progress_total,
          "progress ended at %llu of %llu",
          (unsigned long long)g_progress_last,
          (unsigned long long)g_progress_total);

    ram_close(&disk);
    remove(overlay);
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    printf("== snapshot cow ==\n");
    test_restores_the_disk(dir);
    test_untouched_blocks_are_not_recorded(dir);
    test_reopen_appends_rather_than_duplicating(dir);
    test_refusals(dir);
    test_progress_reaches_the_end(dir);
    printf("== snapshot cow: %u checks, %u failure(s) ==\n",
           g_checks, g_failures);
    return g_failures ? 1 : 0;
}
