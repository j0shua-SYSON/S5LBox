/*
 * Host-side tests for the app's machine list.
 *
 * The interesting failures here are quiet ones. A serialiser that drops a
 * field, a parser that accepts a truncated file and returns a plausible short
 * list, a name containing a newline that reads back as two machines — none of
 * those crash, none of them log, and all of them destroy configuration the
 * user entered by hand. So the round trip is tested against hand-written text
 * rather than against the serialiser's own output, because a format checked
 * against itself checks nothing.
 */
#include "VMInstances.h"
#include "VMOptions.h"

#include <stdio.h>
#include <string.h>

static unsigned tests;
static unsigned failed;

#define CHECK(expr, ...) do {                                                \
    tests++;                                                                 \
    if (!(expr)) {                                                           \
        failed++;                                                            \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                 \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
    }                                                                        \
} while (0)

static const char *ID_A = "0123456789abcdef";
static const char *ID_B = "fedcba9876543210";
static const char *ID_C = "00000000000000ff";

/* --------------------------------------------------------- name checking --- */

static void test_name_check(void) {
    CHECK(vm_instance_name_check("iPhone OS 3.1.3") == VM_INSTANCE_OK,
          "an ordinary name was refused");
    CHECK(vm_instance_name_check("a") == VM_INSTANCE_OK, "one character");
    CHECK(vm_instance_name_check(NULL) == VM_INSTANCE_ERR_NULL, "NULL name");
    CHECK(vm_instance_name_check("") == VM_INSTANCE_ERR_NAME_EMPTY, "empty");
    CHECK(vm_instance_name_check("    ") == VM_INSTANCE_ERR_NAME_EMPTY,
          "a name of only spaces is empty");

    /* The newline case is the one that matters: the persisted form is line
     * oriented, so accepting it would write a record that reads back as two. */
    CHECK(vm_instance_name_check("two\nlines") == VM_INSTANCE_ERR_NAME_CONTROL,
          "a newline in a name must be refused, not escaped");
    CHECK(vm_instance_name_check("tab\there") == VM_INSTANCE_ERR_NAME_CONTROL,
          "a tab is a control character");
    CHECK(vm_instance_name_check("del\x7f") == VM_INSTANCE_ERR_NAME_CONTROL,
          "0x7f is a control character");

    char long_name[VM_INSTANCE_NAME_MAX + 2u];
    memset(long_name, 'x', sizeof long_name - 1u);
    long_name[sizeof long_name - 1u] = '\0';
    CHECK(vm_instance_name_check(long_name) == VM_INSTANCE_ERR_NAME_LONG,
          "one byte over the bound was accepted");
    long_name[VM_INSTANCE_NAME_MAX] = '\0';
    CHECK(vm_instance_name_check(long_name) == VM_INSTANCE_OK,
          "exactly the bound must be accepted");

    /* UTF-8 is bytes here, and multi-byte characters must survive. */
    CHECK(vm_instance_name_check("日本語のマシン") == VM_INSTANCE_OK,
          "a non-ASCII name was refused");
}

static void test_id_check(void) {
    CHECK(vm_instance_id_check(ID_A) == VM_INSTANCE_OK, "a valid id");
    CHECK(vm_instance_id_check(NULL) == VM_INSTANCE_ERR_NULL, "NULL id");
    CHECK(vm_instance_id_check("") == VM_INSTANCE_ERR_ID_INVALID, "empty id");
    CHECK(vm_instance_id_check("0123456789abcde") == VM_INSTANCE_ERR_ID_INVALID,
          "one character short");
    CHECK(vm_instance_id_check("0123456789abcdef0") == VM_INSTANCE_ERR_ID_INVALID,
          "one character long");
    CHECK(vm_instance_id_check("0123456789ABCDEF") == VM_INSTANCE_ERR_ID_INVALID,
          "upper case must be refused: the id names files, and two ids that "
          "differ only in case collide on a case-insensitive filesystem");
    CHECK(vm_instance_id_check("0123456789abcde/") == VM_INSTANCE_ERR_ID_INVALID,
          "a path separator in an id would escape the container directory");
    CHECK(vm_instance_id_check("0123456789abcde.") == VM_INSTANCE_ERR_ID_INVALID,
          "a dot is not hex");
}

/* ---------------------------------------------------------------- add/rm --- */

static void test_add_and_bounds(void) {
    vm_instance_list_t list;
    vm_instance_list_reset(&list);
    CHECK(list.count == 0u, "a reset list is not empty");

    unsigned idx = 99;
    CHECK(vm_instance_add(&list, ID_A, "First", NULL, 0, 1000, &idx) == VM_INSTANCE_OK,
          "the first add failed");
    CHECK(idx == 0u, "the first index is %u", idx);
    CHECK(list.count == 1u, "count is %u", list.count);

    const vm_instance_t *row = vm_instance_at(&list, 0);
    CHECK(row && strcmp(row->id, ID_A) == 0, "the id did not survive");
    CHECK(row && strcmp(row->name, "First") == 0, "the name did not survive");
    CHECK(row && row->created_unix == 1000u, "the created stamp did not survive");
    CHECK(row && row->retired_total == 0u, "a new machine has retired nothing");

    CHECK(vm_instance_add(&list, ID_A, "Same id", NULL, 0, 0, NULL)
              == VM_INSTANCE_ERR_ID_TAKEN,
          "a duplicate id was accepted");
    CHECK(list.count == 1u, "a refused add still changed the count");

    /* A malformed name must be reported as a name problem even when other
     * things are also wrong. */
    CHECK(vm_instance_add(&list, "nothex", "Fine", NULL, 0, 0, NULL)
              == VM_INSTANCE_ERR_ID_INVALID, "bad id");
    CHECK(vm_instance_add(&list, ID_B, "", NULL, 0, 0, NULL)
              == VM_INSTANCE_ERR_NAME_EMPTY, "bad name");
    CHECK(list.count == 1u, "a refused add changed the list");

    /* Fill to the bound. */
    char id[VM_INSTANCE_ID_LEN + 1u];
    for (unsigned i = 1; i < VM_INSTANCE_MAX; i++) {
        snprintf(id, sizeof id, "%016x", i);
        CHECK(vm_instance_add(&list, id, "Filler", NULL, 0, 0, NULL) == VM_INSTANCE_OK,
              "add %u below the bound failed", i);
    }
    CHECK(list.count == VM_INSTANCE_MAX, "count is %u", list.count);
    snprintf(id, sizeof id, "%016x", 999u);
    CHECK(vm_instance_add(&list, id, "One too many", NULL, 0, 0, NULL)
              == VM_INSTANCE_ERR_FULL,
          "the bound was not enforced");

    /* Fullness must not mask a validity error — a caller with a bad name and a
     * full list should still be told the name is bad. */
    CHECK(vm_instance_add(&list, id, "", NULL, 0, 0, NULL)
              == VM_INSTANCE_ERR_NAME_EMPTY,
          "a full list hid a malformed name");
}

static void test_remove_preserves_order(void) {
    vm_instance_list_t list;
    vm_instance_list_reset(&list);
    char id[VM_INSTANCE_ID_LEN + 1u], name[16];
    for (unsigned i = 0; i < 5; i++) {
        snprintf(id, sizeof id, "%016x", i);
        snprintf(name, sizeof name, "M%u", i);
        vm_instance_add(&list, id, name, NULL, 0, 0, NULL);
    }
    CHECK(vm_instance_remove(&list, 1) == VM_INSTANCE_OK, "remove failed");
    CHECK(list.count == 4u, "count is %u", list.count);

    const char *expect[] = { "M0", "M2", "M3", "M4" };
    for (unsigned i = 0; i < 4; i++) {
        const vm_instance_t *r = vm_instance_at(&list, i);
        CHECK(r && strcmp(r->name, expect[i]) == 0,
              "after removing index 1, position %u is '%s', expected '%s'",
              i, r ? r->name : "(null)", expect[i]);
    }
    CHECK(vm_instance_remove(&list, 4) == VM_INSTANCE_ERR_RANGE,
          "removing past the end was accepted");
    CHECK(vm_instance_remove(NULL, 0) == VM_INSTANCE_ERR_NULL, "NULL list");

    /* The vacated tail must be cleared, not left holding the old row: a stale
     * id there would be found by a later index_of_id scan if count ever grew
     * back over it without being written. */
    CHECK(list.slot[list.count].id[0] == '\0',
          "the slot past the end still holds an id");
}

static void test_rename_and_duplicate(void) {
    vm_instance_list_t list;
    vm_instance_list_reset(&list);
    bool opts[VM_INSTANCE_OPTION_MAX];
    memset(opts, 0, sizeof opts);
    opts[0] = true; opts[3] = true;

    vm_instance_add(&list, ID_A, "Original", opts, VM_INSTANCE_OPTION_MAX, 500, NULL);
    list.slot[0].retired_total = 12345u;
    list.slot[0].last_opened_unix = 777u;

    CHECK(vm_instance_rename(&list, 0, "Renamed") == VM_INSTANCE_OK, "rename");
    CHECK(strcmp(list.slot[0].name, "Renamed") == 0, "the name did not change");
    CHECK(strcmp(list.slot[0].id, ID_A) == 0,
          "rename changed the id, which names files on disk");
    CHECK(vm_instance_rename(&list, 0, "bad\nname") == VM_INSTANCE_ERR_NAME_CONTROL,
          "rename accepted a newline");
    CHECK(strcmp(list.slot[0].name, "Renamed") == 0,
          "a refused rename still changed the name");

    unsigned dup = 0;
    CHECK(vm_instance_duplicate(&list, 0, ID_B, "Copy", 900, &dup) == VM_INSTANCE_OK,
          "duplicate failed");
    CHECK(dup == 1u, "the copy landed at %u", dup);
    const vm_instance_t *c = vm_instance_at(&list, 1);
    CHECK(c && c->options[0] && c->options[3], "the options were not copied");
    CHECK(c && !c->options[1], "an option was copied that was not set");
    CHECK(c && strcmp(c->id, ID_B) == 0, "the copy kept the source id");
    /* History is NOT copied: it describes a past the copy has not had. */
    CHECK(c && c->retired_total == 0u,
          "the copy inherited %llu retired instructions",
          (unsigned long long)c->retired_total);
    CHECK(c && c->last_opened_unix == 0u, "the copy inherited an open time");
    CHECK(c && c->created_unix == 900u, "the copy has the wrong created stamp");

    CHECK(vm_instance_duplicate(&list, 9, ID_C, "Nope", 0, NULL)
              == VM_INSTANCE_ERR_RANGE, "duplicating a missing row");
    CHECK(vm_instance_duplicate(&list, 0, ID_A, "Nope", 0, NULL)
              == VM_INSTANCE_ERR_ID_TAKEN, "duplicating onto a taken id");
}

static void test_lookup(void) {
    vm_instance_list_t list;
    vm_instance_list_reset(&list);
    vm_instance_add(&list, ID_A, "A", NULL, 0, 0, NULL);
    vm_instance_add(&list, ID_B, "B", NULL, 0, 0, NULL);

    CHECK(vm_instance_index_of_id(&list, ID_A) == 0, "A not found");
    CHECK(vm_instance_index_of_id(&list, ID_B) == 1, "B not found");
    CHECK(vm_instance_index_of_id(&list, ID_C) == -1, "an absent id was found");
    CHECK(vm_instance_index_of_id(&list, "garbage") == -1, "a bad id was found");
    CHECK(vm_instance_index_of_id(&list, NULL) == -1, "NULL was found");
    CHECK(vm_instance_index_of_id(NULL, ID_A) == -1, "found in a NULL list");
    CHECK(vm_instance_at(&list, 2) == NULL, "one past the end is not NULL");
    CHECK(vm_instance_at(NULL, 0) == NULL, "NULL list gave a row");
}

/* ----------------------------------------------------------- round trip --- */

static void test_serialize_shape(void) {
    vm_instance_list_t list;
    vm_instance_list_reset(&list);
    bool opts[VM_INSTANCE_OPTION_MAX];
    memset(opts, 0, sizeof opts);
    opts[0] = true;                       /* bit 0 -> 00000001 */
    vm_instance_add(&list, ID_A, "My Machine", opts, VM_INSTANCE_OPTION_MAX,
                    1700000000ull, NULL);
    list.slot[0].last_opened_unix = 1700000123ull;
    list.slot[0].retired_total = 4242ull;

    char buf[1024];
    size_t n = vm_instance_serialize(&list, buf, sizeof buf);
    CHECK(n > 0 && n < sizeof buf, "serialized length %zu", n);

    /* Checked against hand-written text, not against the writer's own output. */
    const char *want =
        "ios3vm-instances 1\n"
        "0123456789abcdef 1700000000 1700000123 4242 00000001 My Machine\n";
    CHECK(strcmp(buf, want) == 0,
          "serialized form differs.\n  got:  %s\n  want: %s", buf, want);

    /* Dry run: cap 0 must report the same length and write nothing. */
    CHECK(vm_instance_serialize(&list, NULL, 0) == n,
          "the dry run disagreed with the real one");

    /* Truncation must still report the full length, and must terminate. */
    char small[20];
    size_t want_len = vm_instance_serialize(&list, small, sizeof small);
    CHECK(want_len == n, "a truncated write reported %zu, expected %zu",
          want_len, n);
    CHECK(small[sizeof small - 1u] == '\0', "the truncated buffer is unterminated");
}

static void test_round_trip(void) {
    vm_instance_list_t a, b;
    vm_instance_list_reset(&a);
    bool opts[VM_INSTANCE_OPTION_MAX];
    memset(opts, 0, sizeof opts);
    opts[2] = true; opts[5] = true; opts[11] = true;

    vm_instance_add(&a, ID_A, "First machine", opts, VM_INSTANCE_OPTION_MAX, 111, NULL);
    vm_instance_add(&a, ID_B, "Second  with  spaces", NULL, 0, 222, NULL);
    vm_instance_add(&a, ID_C, "日本語", NULL, 0, 333, NULL);
    a.slot[0].retired_total = 999999999999ull;
    a.slot[1].last_opened_unix = 555;

    char buf[4096];
    vm_instance_serialize(&a, buf, sizeof buf);
    CHECK(vm_instance_deserialize(&b, buf) == VM_INSTANCE_OK, "parse failed");
    CHECK(b.count == a.count, "count %u != %u", b.count, a.count);

    for (unsigned i = 0; i < a.count && i < b.count; i++) {
        CHECK(strcmp(a.slot[i].id, b.slot[i].id) == 0, "id %u differs", i);
        CHECK(strcmp(a.slot[i].name, b.slot[i].name) == 0,
              "name %u differs: '%s' vs '%s'", i, a.slot[i].name, b.slot[i].name);
        CHECK(a.slot[i].created_unix == b.slot[i].created_unix,
              "created %u differs", i);
        CHECK(a.slot[i].last_opened_unix == b.slot[i].last_opened_unix,
              "last_opened %u differs", i);
        CHECK(a.slot[i].retired_total == b.slot[i].retired_total,
              "retired %u differs: %llu vs %llu", i,
              (unsigned long long)a.slot[i].retired_total,
              (unsigned long long)b.slot[i].retired_total);
        for (unsigned k = 0; k < VM_INSTANCE_OPTION_MAX && k < 32u; k++)
            CHECK(a.slot[i].options[k] == b.slot[i].options[k],
                  "option %u of machine %u differs", k, i);
    }

    /* Names with runs of spaces are the case an unquoted last field could
     * quietly collapse. */
    CHECK(strcmp(b.slot[1].name, "Second  with  spaces") == 0,
          "internal spaces were not preserved: '%s'", b.slot[1].name);
}

static void test_parse_refusals(void) {
    vm_instance_list_t list;

    CHECK(vm_instance_deserialize(&list, "") == VM_INSTANCE_ERR_PARSE,
          "empty text parsed");
    CHECK(vm_instance_deserialize(&list, "garbage\n") == VM_INSTANCE_ERR_PARSE,
          "a junk header parsed");
    CHECK(vm_instance_deserialize(&list, "ios3vm-instances\n") == VM_INSTANCE_ERR_PARSE,
          "a header with no version parsed");
    CHECK(vm_instance_deserialize(&list, "ios3vm-instances 2\n") == VM_INSTANCE_ERR_PARSE,
          "a FUTURE version parsed; a newer file read by an older app must "
          "load nothing rather than become a shorter list");
    CHECK(list.count == 0u, "a refused parse left rows behind");

    /* An empty but valid file is a legitimate state: the user deleted all
     * their machines. */
    CHECK(vm_instance_deserialize(&list, "ios3vm-instances 1\n") == VM_INSTANCE_OK,
          "a valid empty list was refused");
    CHECK(list.count == 0u, "an empty file produced rows");

    /* A truncated record invalidates the WHOLE file. */
    const char *truncated =
        "ios3vm-instances 1\n"
        "0123456789abcdef 111 222 333 00000001 Good\n"
        "fedcba9876543210 111 222\n";
    CHECK(vm_instance_deserialize(&list, truncated) == VM_INSTANCE_ERR_PARSE,
          "a truncated record parsed");
    CHECK(list.count == 0u,
          "a truncated file left %u machines, which reads as deletions the "
          "user did not make", list.count);

    /* A bad id in a record is equally fatal. */
    const char *bad_id =
        "ios3vm-instances 1\n"
        "NOTHEXNOTHEXNOTH 111 222 333 00000001 Bad\n";
    CHECK(vm_instance_deserialize(&list, bad_id) == VM_INSTANCE_ERR_PARSE,
          "a record with a malformed id parsed");
    CHECK(list.count == 0u, "a malformed id left %u machines", list.count);

    /*
     * A bad record AFTER a good one. This is the case that distinguishes "the
     * parse was refused" from "the parse was refused AND nothing was kept",
     * and it needs a preceding good record to test at all — the checks above
     * fail on their first line, so they would pass even if the parser kept
     * everything it had read.
     *
     * A mutation that deleted the reset on this path survived the suite until
     * this assertion existed: the status was still ERR_PARSE and only the
     * count gave it away.
     */
    const char *good_then_bad =
        "ios3vm-instances 1\n"
        "0123456789abcdef 111 222 333 00000001 Keeper\n"
        "NOTHEXNOTHEXNOTH 111 222 333 00000001 Bad\n";
    CHECK(vm_instance_deserialize(&list, good_then_bad) == VM_INSTANCE_ERR_PARSE,
          "a file whose second record is malformed parsed");
    CHECK(list.count == 0u,
          "a refused parse kept %u machine(s) from before the bad record; a "
          "partial list reads to the user as deletions they did not make",
          list.count);

    /* Duplicate ids in a file cannot be resolved, so the file is bad — and
     * again the first record is valid, so the count is the real assertion. */
    const char *dupe =
        "ios3vm-instances 1\n"
        "0123456789abcdef 1 0 0 00000000 One\n"
        "0123456789abcdef 2 0 0 00000000 Two\n";
    CHECK(vm_instance_deserialize(&list, dupe) == VM_INSTANCE_ERR_PARSE,
          "a file with duplicate ids parsed");
    CHECK(list.count == 0u,
          "a duplicate-id file left %u machine(s) behind", list.count);

    CHECK(vm_instance_deserialize(NULL, "x") == VM_INSTANCE_ERR_NULL, "NULL list");
    CHECK(vm_instance_deserialize(&list, NULL) == VM_INSTANCE_ERR_NULL, "NULL text");
}

static void test_crlf_tolerated(void) {
    /* A machine list that has been through a Windows editor is still the
     * user's machine list. */
    vm_instance_list_t list;
    const char *crlf =
        "ios3vm-instances 1\r\n"
        "0123456789abcdef 111 222 333 00000005 Windows Edited\r\n";
    CHECK(vm_instance_deserialize(&list, crlf) == VM_INSTANCE_OK,
          "CRLF was refused");
    CHECK(list.count == 1u, "count is %u", list.count);
    CHECK(strcmp(list.slot[0].name, "Windows Edited") == 0,
          "the name kept a carriage return: '%s'", list.slot[0].name);
    CHECK(list.slot[0].options[0] && list.slot[0].options[2],
          "option bits 0 and 2 did not survive CRLF");
}

/* The bound that protects every saved machine from a growing option table. */
static void test_options_fit_the_table(void) {
    CHECK(vm_instance_options_fit(vm_option_count()),
          "the option table has %u rows but an instance stores only %u; "
          "raising VM_INSTANCE_OPTION_MAX is required, and every saved list "
          "must be migrated rather than silently truncated",
          vm_option_count(), VM_INSTANCE_OPTION_MAX);
    CHECK(!vm_instance_options_fit(VM_INSTANCE_OPTION_MAX + 1u),
          "the fit check accepted one row too many");
}

int main(void) {
    test_name_check();
    test_id_check();
    test_add_and_bounds();
    test_remove_preserves_order();
    test_rename_and_duplicate();
    test_lookup();
    test_serialize_shape();
    test_round_trip();
    test_parse_refusals();
    test_crlf_tolerated();
    test_options_fit_the_table();

    printf("vminstances: %u checks, %u failed\n", tests, failed);
    return failed ? 1 : 0;
}
