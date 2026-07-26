/*
 * Host-side tests for the app's mirror of bootkernel's boot-toggle table.
 *
 * The point of these is drift. tools/bootkernel.c owns the real table; this one
 * exists so a phone can show the same switches, with the same names and the
 * same defaults, and render a command line a desktop run can be compared
 * against. Two hand-maintained copies of anything diverge, so the expectations
 * below are written out longhand: changing a name or a default in VMOptions.c
 * without changing it here is a test failure, which is exactly the moment to
 * check whether bootkernel changed too.
 */
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

/* The table as it must be, in order. Longhand on purpose: see the file note. */
static const struct {
    const char *name;
    bool        def;
    unsigned    group;
    unsigned    impl;
} EXPECTED[] = {
    { "mbx",                false, VM_OPT_GROUP_HARDWARE,    VM_OPT_IMPL_HARNESS },
    { "sha1",               false, VM_OPT_GROUP_HARDWARE,    VM_OPT_IMPL_HARNESS },
    { "baseband",           false, VM_OPT_GROUP_HARDWARE,    VM_OPT_IMPL_HARNESS },
    { "spi2",               false, VM_OPT_GROUP_HARDWARE,    VM_OPT_IMPL_HARNESS },
    { "usb-otg",            false, VM_OPT_GROUP_HARDWARE,    VM_OPT_IMPL_HARNESS },
    { "vram",               true,  VM_OPT_GROUP_PATCH,       VM_OPT_IMPL_HARNESS },
    { "lcd-panel-id",       true,  VM_OPT_GROUP_PATCH,       VM_OPT_IMPL_HARNESS },
    { "memory-reg",         true,  VM_OPT_GROUP_PATCH,       VM_OPT_IMPL_HARNESS },
    { "rtc-patch",          true,  VM_OPT_GROUP_PATCH,       VM_OPT_IMPL_HARNESS },
    { "ca-software-render", false, VM_OPT_GROUP_PATCH,       VM_OPT_IMPL_HARNESS },
    { "activate",           true,  VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_NOWHERE },
    { "jb-codesign",        false, VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_NOWHERE },
    { "jb-payload",         false, VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_NOWHERE },
    { "ppp",                false, VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_HARNESS }
};

#define NEXPECTED ((unsigned)(sizeof EXPECTED / sizeof EXPECTED[0]))

static void test_table_matches_bootkernel(void) {
    CHECK(vm_option_count() == NEXPECTED,
          "table has %u rows, expected %u", vm_option_count(), NEXPECTED);
    if (vm_option_count() != NEXPECTED) return;

    for (unsigned i = 0; i < NEXPECTED; i++) {
        const vm_option_t *row = vm_option_at(i);
        CHECK(row != NULL, "row %u is missing", i);
        if (!row) continue;

        CHECK(row->name && !strcmp(row->name, EXPECTED[i].name),
              "row %u is \"%s\", expected \"%s\"",
              i, row->name ? row->name : "(null)", EXPECTED[i].name);
        CHECK(row->def == EXPECTED[i].def,
              "%s defaults to %s, expected %s",
              EXPECTED[i].name, row->def ? "on" : "off",
              EXPECTED[i].def ? "on" : "off");
        CHECK(row->group == EXPECTED[i].group,
              "%s is in group %u, expected %u",
              EXPECTED[i].name, (unsigned)row->group, EXPECTED[i].group);
        CHECK(row->impl == EXPECTED[i].impl,
              "%s claims implementation state %u, expected %u",
              EXPECTED[i].name, (unsigned)row->impl, EXPECTED[i].impl);

        /* A row with no words on it is a switch nobody can interpret. */
        CHECK(row->title && row->title[0], "%s has no title", EXPECTED[i].name);
        CHECK(row->detail && strlen(row->detail) > 30,
              "%s has no usable explanation", EXPECTED[i].name);
    }
}

static void test_rows_are_grouped_and_unique(void) {
    unsigned previous = 0;
    for (unsigned i = 0; i < vm_option_count(); i++) {
        const vm_option_t *row = vm_option_at(i);
        if (!row) continue;
        /* The settings screen walks the table once per section, so a group may
         * not reappear after it has been left. */
        CHECK(row->group >= previous,
              "row %u (%s) revisits group %u after group %u",
              i, row->name, (unsigned)row->group, previous);
        previous = row->group;
        CHECK(row->group < (unsigned)VM_OPT_GROUP_COUNT,
              "row %u has group %u, out of range", i, (unsigned)row->group);

        for (unsigned j = i + 1; j < vm_option_count(); j++) {
            const vm_option_t *other = vm_option_at(j);
            CHECK(!other || strcmp(row->name, other->name) != 0,
                  "\"%s\" appears at both %u and %u", row->name, i, j);
        }
    }
}

static void test_lookup(void) {
    CHECK(vm_option_index("vram") >= 0, "vram is not findable by name");
    CHECK(vm_option_index("mbx") == 0, "mbx is not the first row");
    CHECK(vm_option_index("ppp") == (int)NEXPECTED - 1,
          "ppp is not the last row");
    CHECK(vm_option_index("jb-payload") == (int)NEXPECTED - 2,
          "jb-payload is not the second-to-last row");
    CHECK(vm_option_index("no-vram") == -1, "a negated spelling resolved");
    CHECK(vm_option_index("--vram") == -1, "a dashed spelling resolved");
    CHECK(vm_option_index("") == -1, "the empty name resolved");
    CHECK(vm_option_index(NULL) == -1, "a null name resolved");

    CHECK(vm_option_at(vm_option_count()) == NULL, "one past the end resolved");
    CHECK(vm_option_at(0xffffffffu) == NULL, "a huge index resolved");

    for (unsigned g = 0; g < (unsigned)VM_OPT_GROUP_COUNT; g++) {
        CHECK(vm_option_group_title(g) && vm_option_group_title(g)[0],
              "group %u has no title", g);
        CHECK(vm_option_group_note(g) && strlen(vm_option_group_note(g)) > 40,
              "group %u has no standing caveat", g);
    }
    CHECK(vm_option_group_title(VM_OPT_GROUP_COUNT) == NULL,
          "a group past the end has a title");
    CHECK(vm_option_group_note(VM_OPT_GROUP_COUNT) == NULL,
          "a group past the end has a note");
}

/* Fill `values` with the defaults, so a test only has to say what it changed. */
static void defaults_into(bool *values) {
    for (unsigned i = 0; i < vm_option_count(); i++)
        values[i] = vm_option_at(i)->def;
}

static void expect_command_line(const bool *values, const char *want,
                                const char *what) {
    char buf[512];
    size_t need = vm_option_command_line(values, vm_option_count(),
                                         buf, sizeof buf);
    CHECK(need == strlen(want) && !strcmp(buf, want),
          "%s: got \"%s\" (%u chars), wanted \"%s\"",
          what, buf, (unsigned)need, want);
}

static void test_command_line(void) {
    bool values[64];
    CHECK(vm_option_count() <= 64, "the table outgrew this test's array");
    if (vm_option_count() > 64) return;

    defaults_into(values);
    expect_command_line(values, "", "everything at its default");

    defaults_into(values);
    values[vm_option_index("mbx")] = true;
    expect_command_line(values, "--mbx", "an off default turned on");

    defaults_into(values);
    values[vm_option_index("vram")] = false;
    expect_command_line(values, "--no-vram", "an on default turned off");

    /* Order follows the table, not the order they were changed. */
    defaults_into(values);
    values[vm_option_index("activate")] = false;
    values[vm_option_index("vram")] = false;
    values[vm_option_index("usb-otg")] = true;
    expect_command_line(values, "--usb-otg --no-vram --no-activate",
                        "three changes in table order");

    /* Every row flipped, so the separator logic is exercised at full length. */
    for (unsigned i = 0; i < vm_option_count(); i++)
        values[i] = !vm_option_at(i)->def;
    char full[512];
    size_t need = vm_option_command_line(values, vm_option_count(),
                                         full, sizeof full);
    CHECK(need > 0 && need < sizeof full, "everything flipped needs %u chars",
          (unsigned)need);
    CHECK(strstr(full, "  ") == NULL, "double space in \"%s\"", full);
    CHECK(full[0] == '-', "leading separator in \"%s\"", full);
    CHECK(full[need - 1] != ' ', "trailing separator in \"%s\"", full);
    /* Flipped means the opposite spelling of the default: an off default is
     * now asserted with --name, an on default negated with --no-name. */
    CHECK(strstr(full, "--mbx") != NULL, "off default not asserted: %s", full);
    CHECK(strstr(full, "--no-vram") != NULL, "on default not negated: %s", full);
    CHECK(strstr(full, "--no-mbx") == NULL, "off default also negated: %s", full);
}

static void test_command_line_truncation_and_nulls(void) {
    bool values[64];
    defaults_into(values);
    values[vm_option_index("mbx")] = true;
    values[vm_option_index("sha1")] = true;

    const char *want = "--mbx --sha1";
    const size_t want_len = strlen(want);

    /* A dry run has to report the length a real run would need. */
    CHECK(vm_option_command_line(values, vm_option_count(), NULL, 0) == want_len,
          "dry run reported the wrong length");

    /* And every partial buffer must stay a terminated prefix of the answer. */
    for (size_t cap = 1; cap <= want_len + 2; cap++) {
        char buf[64];
        memset(buf, '?', sizeof buf);
        size_t need = vm_option_command_line(values, vm_option_count(),
                                             buf, cap);
        CHECK(need == want_len, "cap %u reported %u, expected %u",
              (unsigned)cap, (unsigned)need, (unsigned)want_len);
        CHECK(strlen(buf) < cap, "cap %u produced an unterminated buffer",
              (unsigned)cap);
        CHECK(strncmp(buf, want, strlen(buf)) == 0,
              "cap %u produced \"%s\", not a prefix of \"%s\"",
              (unsigned)cap, buf, want);
    }

    /* No values is not the same as all-false: it must render nothing. */
    char buf[64];
    memset(buf, '?', sizeof buf);
    CHECK(vm_option_command_line(NULL, vm_option_count(), buf, sizeof buf) == 0,
          "a null value array rendered arguments");
    CHECK(buf[0] == '\0', "a null value array left the buffer untouched");

    /* A short array describes only the rows it covers. */
    CHECK(vm_option_command_line(values, 1, NULL, 0) == strlen("--mbx"),
          "a one-row array looked past its end");
    CHECK(vm_option_command_line(values, 0, NULL, 0) == 0,
          "an empty array rendered arguments");

    /* An over-long count is clamped rather than read past the table. */
    CHECK(vm_option_command_line(values, 10000u, NULL, 0) == want_len,
          "an over-long count was not clamped to the table");
}

int main(void) {
    test_table_matches_bootkernel();
    test_rows_are_grouped_and_unique();
    test_lookup();
    test_command_line();
    test_command_line_truncation_and_nulls();

    printf("vmoptions: %u checks, %u failed\n", tests, failed);
    return failed ? 1 : 0;
}
