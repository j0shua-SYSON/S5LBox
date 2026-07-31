/*
 * S5LBox — the snapshot store's shape rules and ordering.
 *
 * Most of these are REFUSALS. An id becomes a path component and arrives from
 * a directory listing, which is to say from outside, so the interesting
 * property is not that a good id works -- it is that nothing else does.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMSnapshotStore.h"

#include <stdio.h>
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

static void test_id_shape(void) {
    CHECK(vm_snapshot_id_valid("20260731T142200Z"), "the bare form must pass");
    CHECK(vm_snapshot_id_valid("20260731T142200Z-07"),
          "the sequenced form must pass");

    /* Everything that is not exactly the shape. The traversal cases are not
     * rejected by a blacklist; they cannot pass the width and digit rules. */
    static const char *const BAD[] = {
        "", ".", "..", "../..", "/etc/passwd",
        "20260731T142200",          /* no Z                    */
        "20260731t142200Z",         /* lowercase T             */
        "20260731T142200Z-",        /* trailing hyphen         */
        "20260731T142200Z-7",       /* one-digit sequence      */
        "20260731T142200Z-007",     /* three-digit sequence    */
        "2026073 T142200Z",         /* space for a digit       */
        "20260731T142200Z/..",      /* separator after a good prefix */
        "20260731T142200Z.partial", /* the in-progress name    */
        "20260731X142200Z",         /* wrong separator         */
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++)
        CHECK(!vm_snapshot_id_valid(BAD[i]), "must reject \"%s\"", BAD[i]);

    CHECK(!vm_snapshot_id_valid(NULL), "NULL must be rejected, not crash");
}

static void test_id_make(void) {
    char id[VM_SNAPSHOT_ID_CAPACITY];

    /* The epoch, which pins the civil-date arithmetic at a known point. */
    CHECK(vm_snapshot_id_make(0, 0, id, sizeof id) == VM_SNAPSHOT_OK,
          "epoch must build");
    CHECK(strcmp(id, "19700101T000000Z") == 0, "epoch id was \"%s\"", id);

    /* One second before a leap day, and the leap day itself: the two places
     * the shifted-epoch arithmetic would break if it were wrong. */
    CHECK(vm_snapshot_id_make(951782399ull, 0, id, sizeof id) == VM_SNAPSHOT_OK,
          "pre-leap must build");
    CHECK(strcmp(id, "20000228T235959Z") == 0, "pre-leap id was \"%s\"", id);
    CHECK(vm_snapshot_id_make(951782400ull, 0, id, sizeof id) == VM_SNAPSHOT_OK,
          "leap day must build");
    CHECK(strcmp(id, "20000229T000000Z") == 0, "leap day id was \"%s\"", id);

    /* A non-leap century, which a naive every-four-years rule gets wrong. */
    CHECK(vm_snapshot_id_make(4107542400ull, 0, id, sizeof id) == VM_SNAPSHOT_OK,
          "2100 must build");
    CHECK(strcmp(id, "21000301T000000Z") == 0, "2100 id was \"%s\"", id);

    CHECK(vm_snapshot_id_make(0, 7, id, sizeof id) == VM_SNAPSHOT_OK,
          "sequenced must build");
    CHECK(strcmp(id, "19700101T000000Z-07") == 0, "sequenced id was \"%s\"", id);

    /* Everything it makes must be something it would accept. */
    for (uint64_t t = 0; t < 4000000000ull; t += 97654321ull) {
        CHECK(vm_snapshot_id_make(t, 0, id, sizeof id) == VM_SNAPSHOT_OK,
              "t=%llu must build", (unsigned long long)t);
        CHECK(vm_snapshot_id_valid(id), "t=%llu produced invalid \"%s\"",
              (unsigned long long)t, id);
    }

    CHECK(vm_snapshot_id_make(0, 100, id, sizeof id) == VM_SNAPSHOT_BAD_ARGUMENT,
          "a three-digit sequence must be refused");
    CHECK(vm_snapshot_id_make(0, 0, id, 4) == VM_SNAPSHOT_BAD_ARGUMENT,
          "a short buffer must be refused rather than truncated");
}

static void test_paths_refuse_bad_ids(void) {
    char p[VM_FW_BOOT_PATH_CAPACITY];

    CHECK(vm_snapshot_dir("/m", p, sizeof p) == VM_SNAPSHOT_OK, "dir must build");
    CHECK(strcmp(p, "/m/snapshots") == 0, "dir was \"%s\"", p);

    CHECK(vm_snapshot_path("/s", "20260731T142200Z", false, p, sizeof p)
              == VM_SNAPSHOT_OK, "path must build");
    CHECK(strcmp(p, "/s/20260731T142200Z") == 0, "path was \"%s\"", p);

    CHECK(vm_snapshot_path("/s", "20260731T142200Z", true, p, sizeof p)
              == VM_SNAPSHOT_OK, "partial path must build");
    CHECK(strcmp(p, "/s/20260731T142200Z.partial") == 0,
          "partial path was \"%s\"", p);

    CHECK(vm_snapshot_member_path("/s", "20260731T142200Z", false,
                                  VM_SNAPSHOT_STATE_FILE, p, sizeof p)
              == VM_SNAPSHOT_OK, "member must build");
    CHECK(strcmp(p, "/s/20260731T142200Z/state.snap") == 0,
          "member was \"%s\"", p);

    /* A bad id must never reach a path, in either builder. */
    CHECK(vm_snapshot_path("/s", "..", false, p, sizeof p) == VM_SNAPSHOT_BAD_ID,
          "\"..\" must not become a path");
    CHECK(vm_snapshot_member_path("/s", "../..", false, VM_SNAPSHOT_META_FILE,
                                  p, sizeof p) == VM_SNAPSHOT_BAD_ID,
          "traversal must not become a member path");
    /* Nor a bad leaf, even though today's callers only pass constants. */
    CHECK(vm_snapshot_member_path("/s", "20260731T142200Z", false, "../x",
                                  p, sizeof p) == VM_SNAPSHOT_BAD_ARGUMENT,
          "a leaf with a separator must be refused");

    /* Truncation is a refusal and leaves no half-path behind. */
    char tiny[8];
    CHECK(vm_snapshot_path("/some/long/prefix", "20260731T142200Z", false,
                           tiny, sizeof tiny) == VM_SNAPSHOT_PATH_TOO_LONG,
          "overlong must be refused");
    CHECK(tiny[0] == '\0', "a refused path must be emptied, not truncated");
}

static void test_meta_roundtrip(const char *tmpdir) {
    char path[VM_FW_BOOT_PATH_CAPACITY];
    snprintf(path, sizeof path, "%s/meta.test", tmpdir);

    vm_snapshot_info_t in;
    memset(&in, 0, sizeof in);
    memcpy(in.id, "20260731T142200Z", 17);
    in.created_unix = 1785000000ull;
    in.retired = 5300000000ull;

    CHECK(vm_snapshot_meta_write(path, &in, NULL, 0) == VM_SNAPSHOT_OK,
          "meta must write");

    vm_snapshot_info_t back;
    CHECK(vm_snapshot_meta_read(path, &back, NULL, 0) == VM_SNAPSHOT_OK,
          "meta must read back");
    CHECK(strcmp(back.id, in.id) == 0, "id round trip: \"%s\"", back.id);
    CHECK(back.created_unix == in.created_unix, "created round trip: %llu",
          (unsigned long long)back.created_unix);
    CHECK(back.retired == in.retired, "retired round trip: %llu",
          (unsigned long long)back.retired);

    /* A missing required key is a refusal, not a zero: a snapshot whose
     * instruction count is unknown cannot be ordered against the others. */
    FILE *f = fopen(path, "wb");
    if (f) { fprintf(f, "version 1\nid 20260731T142200Z\n"); fclose(f); }
    CHECK(vm_snapshot_meta_read(path, &back, NULL, 0) == VM_SNAPSHOT_INCOMPLETE,
          "a meta missing `retired` must be refused");

    /* An unknown key must NOT break a file a newer build wrote. */
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "version 2\nid 20260731T142200Z\ncreated 5\nretired 6\n"
                   "somethingnew 42\n");
        fclose(f);
    }
    CHECK(vm_snapshot_meta_read(path, &back, NULL, 0) == VM_SNAPSHOT_OK,
          "an unknown key must be ignored, not fatal");

    /* A malformed id inside meta is refused even though the filename was fine. */
    f = fopen(path, "wb");
    if (f) { fprintf(f, "id ..\ncreated 5\nretired 6\n"); fclose(f); }
    CHECK(vm_snapshot_meta_read(path, &back, NULL, 0) == VM_SNAPSHOT_BAD_ID,
          "meta naming a bad id must be refused");

    remove(path);
}

static void test_list_of_a_missing_directory(void) {
    vm_snapshot_info_t items[VM_SNAPSHOT_MAX];
    size_t n = 12345;
    /* A machine that has never been snapshotted is the ordinary case, not a
     * failure, and must report zero rather than an error the UI would show. */
    CHECK(vm_snapshot_list("/no/such/directory/anywhere", items,
                           VM_SNAPSHOT_MAX, &n, NULL, 0) == VM_SNAPSHOT_OK,
          "a missing directory must list as empty");
    CHECK(n == 0, "count was %zu, expected 0", n);
}

int main(int argc, char **argv) {
    const char *tmpdir = (argc > 1) ? argv[1] : ".";
    printf("== snapshot store ==\n");
    test_id_shape();
    test_id_make();
    test_paths_refuse_bad_ids();
    test_meta_roundtrip(tmpdir);
    test_list_of_a_missing_directory();
    printf("== snapshot store: %u checks, %u failure(s) ==\n",
           g_checks, g_failures);
    return g_failures ? 1 : 0;
}
