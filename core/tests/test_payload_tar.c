/*
 * Tests for tools/payload_tar.c — a jailbreak payload turned into provisioner
 * entries.
 *
 * EVERY FIXTURE IS BUILT HERE. No payload is committed and none is needed: the
 * cases below write their own tars byte by byte, which is also the only way to
 * test the refusals, since a real archive is by construction well-formed.
 *
 * What is worth asserting is mostly the REFUSALS and the metadata. A reader that
 * silently dropped the payload's 88 symlinks, or lost the setuid bit on
 * MobileCydia, would produce an image that looks provisioned and does not work —
 * and the provisioner downstream cannot tell it was lied to.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "payload_tar.h"

#include <stdio.h>
#include <stdlib.h>
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

#define BLOCK 512u

/* ---------------------------------------------------------- tar authoring ---
 *
 * Written rather than shelled out to, so the test runs identically on a host
 * with no tar and can produce archives a real tar would refuse to create.
 */
typedef struct { unsigned char *buf; size_t used, cap; } tarbuf_t;

static void tb_init(tarbuf_t *t) {
    t->cap = 1u << 20; t->used = 0;
    t->buf = (unsigned char *)calloc(1u, t->cap);
}
static void tb_free(tarbuf_t *t) { free(t->buf); t->buf = NULL; }

static void tb_octal(unsigned char *p, size_t n, unsigned long long v) {
    /* tar writes N-1 octal digits then a NUL. */
    for (size_t i = n - 1u; i-- > 0;) { p[i] = (unsigned char)('0' + (v & 7u)); v >>= 3; }
    p[n - 1u] = '\0';
}

static void tb_add(tarbuf_t *t, const char *name, char type,
                   unsigned mode, const void *body, size_t body_len,
                   const char *link) {
    unsigned char *h = t->buf + t->used;
    memset(h, 0, BLOCK);
    (void)snprintf((char *)h + 0, 100u, "%s", name);
    tb_octal(h + 100, 8, mode);
    tb_octal(h + 108, 8, 0);            /* uid  */
    tb_octal(h + 116, 8, 0);            /* gid  */
    tb_octal(h + 124, 12, body_len);
    tb_octal(h + 136, 12, 0);           /* mtime */
    h[156] = (unsigned char)type;
    if (link) (void)snprintf((char *)h + 157, 100u, "%s", link);
    memcpy(h + 257, "ustar  ", 8);
    /* Checksum last, over the header with the field as spaces. */
    memset(h + 148, ' ', 8);
    unsigned long sum = 0;
    for (size_t i = 0; i < BLOCK; i++) sum += h[i];
    tb_octal(h + 148, 7, sum);
    h[155] = ' ';
    t->used += BLOCK;
    if (body_len) {
        memcpy(t->buf + t->used, body, body_len);
        t->used += (body_len + BLOCK - 1u) / BLOCK * BLOCK;
    }
}

static void tb_end(tarbuf_t *t) { t->used += 2u * BLOCK; }

static const char *tb_write(tarbuf_t *t, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    (void)fwrite(t->buf, 1u, t->used, f);
    fclose(f);
    return path;
}

/* ------------------------------------------------------------- the cases --- */

/*
 * THE PAYLOAD'S OWN SHAPES. Not hypothetical: these are the four things the
 * acquired Cydia tar actually contains that a naive reader gets wrong --
 * a setuid binary, a relative symlink, an absolute symlink, and a directory.
 */
static void test_the_shapes_a_real_payload_has(void) {
    tarbuf_t t; tb_init(&t);
    const char body[] = "MZ-not-really";
    tb_add(&t, "Applications/",                 '5', 0775u, NULL, 0, NULL);
    tb_add(&t, "Applications/MobileCydia",      '0', 06755u, body, sizeof body - 1u, NULL);
    tb_add(&t, "bin/bzcat",                     '2', 0777u, NULL, 0, "bzip2");
    tb_add(&t, "Sections/x.png",                '2', 0777u, NULL, 0,
           "/usr/share/bigboss/icons/b.png");
    tb_end(&t);
    tb_write(&t, "payload-shapes.tar");
    tb_free(&t);

    char why[PAYLOAD_TAR_DETAIL_CAPACITY] = {0};
    payload_tar_t *p = payload_tar_open("payload-shapes.tar", "", why, sizeof why);
    CHECK(p != NULL, "a well-formed payload was refused: %s", why);
    if (!p) return;

    const rootfs_work_entry_t *e = payload_tar_entries(p);
    CHECK(payload_tar_entry_count(p) == 4u, "%zu entries, expected 4",
          payload_tar_entry_count(p));

    CHECK(e[0].kind == ROOTFS_WORK_ENTRY_DIRECTORY, "member 0 is not a directory");
    CHECK(!strcmp(e[0].path, "/Applications"),
          "directory path is \"%s\"; the trailing slash must go and a leading "
          "one must appear", e[0].path);

    /* The bit that matters most: 06755 survives. A reader that masked to 0777
     * would produce a MobileCydia that cannot elevate, and nothing downstream
     * could tell. */
    CHECK(e[1].kind == ROOTFS_WORK_ENTRY_FILE, "member 1 is not a file");
    CHECK(e[1].permissions == 06755u,
          "setuid lost: permissions are 0%o, expected 06755", e[1].permissions);
    CHECK(e[1].content_size == sizeof body - 1u,
          "content is %zu bytes, expected %zu", e[1].content_size,
          sizeof body - 1u);
    CHECK(e[1].content && !memcmp(e[1].content, body, sizeof body - 1u),
          "file content did not survive");

    /* A RELATIVE link target must stay relative. The name arena prepends '/'
     * for member paths, and a link that picked that up would resolve to
     * /bzip2 instead of the sibling. */
    CHECK(e[2].kind == ROOTFS_WORK_ENTRY_SYMLINK, "member 2 is not a symlink");
    CHECK(e[2].content && !strcmp((const char *)e[2].content, "bzip2"),
          "relative link target is \"%s\", expected \"bzip2\"",
          e[2].content ? (const char *)e[2].content : "(null)");
    CHECK(e[3].content &&
          !strcmp((const char *)e[3].content,
                  "/usr/share/bigboss/icons/b.png"),
          "absolute link target was altered: \"%s\"",
          e[3].content ? (const char *)e[3].content : "(null)");

    payload_tar_stats_t s;
    payload_tar_get_stats(p, &s);
    CHECK(s.files == 1u && s.directories == 1u && s.symlinks == 2u,
          "stats say files=%zu dirs=%zu links=%zu", s.files, s.directories,
          s.symlinks);
    payload_tar_close(&p);
    CHECK(p == NULL, "close did not clear the caller's slot");
    remove("payload-shapes.tar");
}

/* A hard link becomes a copy, and the count says so rather than hiding it. */
static void test_a_hard_link_is_materialised_and_counted(void) {
    tarbuf_t t; tb_init(&t);
    const char body[] = "gzip-bytes";
    tb_add(&t, "bin/gunzip",     '0', 0755u, body, sizeof body - 1u, NULL);
    tb_add(&t, "bin/uncompress", '1', 0755u, NULL, 0, "bin/gunzip");
    tb_end(&t);
    tb_write(&t, "payload-hard.tar");
    tb_free(&t);

    char why[PAYLOAD_TAR_DETAIL_CAPACITY] = {0};
    payload_tar_t *p = payload_tar_open("payload-hard.tar", "", why, sizeof why);
    CHECK(p != NULL, "a payload with a hard link was refused: %s", why);
    if (p) {
        const rootfs_work_entry_t *e = payload_tar_entries(p);
        CHECK(payload_tar_entry_count(p) == 2u, "expected 2 entries");
        CHECK(e[1].kind == ROOTFS_WORK_ENTRY_FILE,
              "the hard link did not become a regular file");
        CHECK(e[1].content_size == sizeof body - 1u &&
              e[1].content && !memcmp(e[1].content, body, sizeof body - 1u),
              "the hard link does not carry its target's bytes");
        payload_tar_stats_t s; payload_tar_get_stats(p, &s);
        CHECK(s.hardlinks_materialised == 1u,
              "%zu hard links reported, expected 1 -- an unreported "
              "approximation is one nobody knows they are relying on",
              s.hardlinks_materialised);
        payload_tar_close(&p);
    }
    remove("payload-hard.tar");

    /* And one naming a member that does not exist is refused, not silently
     * turned into an empty file. */
    tb_init(&t);
    tb_add(&t, "bin/uncompress", '1', 0755u, NULL, 0, "bin/absent");
    tb_end(&t);
    tb_write(&t, "payload-hard-bad.tar");
    tb_free(&t);
    why[0] = '\0';
    p = payload_tar_open("payload-hard-bad.tar", "", why, sizeof why);
    CHECK(p == NULL, "a hard link to a missing member was accepted");
    CHECK(strstr(why, "bin/absent") != NULL,
          "the refusal does not name the missing target: %s", why);
    payload_tar_close(&p);
    remove("payload-hard-bad.tar");
}

/*
 * THE REFUSALS. A payload is untrusted input -- a user downloaded it -- and
 * every one of these would otherwise write outside the tree the caller asked
 * for or provision a name nobody chose.
 */
static void test_the_refusals(void) {
    struct { const char *what; const char *name; char type; const char *link;
             const char *expect; } cases[] = {
        { "an absolute member",  "/etc/passwd",     '0', NULL, "path" },
        { "a parent traversal",  "a/../../etc/x",   '0', NULL, "path" },
        { "a bare ..",           "..",              '0', NULL, "path" },
        { "a non-ASCII name",    "caf\xc3\xa9",     '0', NULL, "path" },
        { "an empty link target","bin/x",           '2', "",   "target" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        tarbuf_t t; tb_init(&t);
        tb_add(&t, cases[i].name, cases[i].type, 0644u, "x", 1u, cases[i].link);
        tb_end(&t);
        tb_write(&t, "payload-bad.tar");
        tb_free(&t);
        char why[PAYLOAD_TAR_DETAIL_CAPACITY] = {0};
        payload_tar_t *p = payload_tar_open("payload-bad.tar", "", why, sizeof why);
        CHECK(p == NULL, "%s was accepted", cases[i].what);
        CHECK(strstr(why, cases[i].expect) != NULL,
              "%s: refusal does not mention \"%s\": %s",
              cases[i].what, cases[i].expect, why);
        payload_tar_close(&p);
        remove("payload-bad.tar");
    }
}

/*
 * A CORRUPT HEADER MUST NOT PROVISION ANYTHING. Half a payload written into a
 * filesystem is worse than a refusal, and a bad checksum is what a truncated
 * download looks like.
 */
static void test_a_bad_checksum_refuses_the_whole_archive(void) {
    tarbuf_t t; tb_init(&t);
    tb_add(&t, "bin/a", '0', 0755u, "aaa", 3u, NULL);
    tb_add(&t, "bin/b", '0', 0755u, "bbb", 3u, NULL);
    tb_end(&t);
    /*
     * The SECOND header, which is at 2*BLOCK and not BLOCK: "bin/a" has a
     * three-byte body, so it occupies a header block AND a data block. The
     * first version of this line corrupted a data block instead and the archive
     * parsed cleanly -- the test failing is what said so.
     */
    t.buf[2u * BLOCK + 148u] = '9';
    tb_write(&t, "payload-crc.tar");
    tb_free(&t);

    char why[PAYLOAD_TAR_DETAIL_CAPACITY] = {0};
    payload_tar_t *p = payload_tar_open("payload-crc.tar", "", why, sizeof why);
    CHECK(p == NULL, "an archive with a bad header checksum was accepted");
    CHECK(strstr(why, "checksum") != NULL,
          "the refusal does not mention the checksum: %s", why);
    payload_tar_close(&p);
    remove("payload-crc.tar");
}

/* A missing file is an ordinary refusal that names the path, not a crash. */
static void test_a_missing_payload_says_so(void) {
    char why[PAYLOAD_TAR_DETAIL_CAPACITY] = {0};
    payload_tar_t *p = payload_tar_open("payload-does-not-exist.tar", "",
                                       why, sizeof why);
    CHECK(p == NULL, "a missing payload was accepted");
    CHECK(strstr(why, "payload-does-not-exist.tar") != NULL,
          "the refusal does not name the file: %s", why);
    payload_tar_close(&p);
    CHECK(payload_tar_entry_count(NULL) == 0u, "NULL must report no entries");
    CHECK(payload_tar_entries(NULL) == NULL, "NULL must report no array");
}

int main(void) {
    printf("S5LBox payload tar reader tests\n");
    test_the_shapes_a_real_payload_has();
    test_a_hard_link_is_materialised_and_counted();
    test_the_refusals();
    test_a_bad_checksum_refuses_the_whole_archive();
    test_a_missing_payload_says_so();
    printf("== payload tar: %u checks, %u failure(s) ==\n", checks, failures);
    return failures ? 1 : 0;
}
