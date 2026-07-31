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
    /* Off, and unlike the others this default is a LOSS taken deliberately.
     * run140 and run151 are the clean pair -- same ca-software-render, same
     * usb-otg, same budget, differing only here: un-matched renders at 273206
     * framebuffer bytes, matched stops at 1821. Neither has touch.
     *
     * run151 also settles what was an open question: it carries the fixed HBPP
     * framing (one 30:54154 packet, zero unknown opcodes) and STILL costs the
     * display, so correct framing alone does not earn this switch back. Flip it
     * the day the bootload reaches EXEC, not before. */
    /* Default flipped ON 2026-07-30: the bootload completes (run163) and a
     * slide-to-unlock reaches the home screen (r181/r182). The old OFF default
     * was a trade against a black screen that no longer exists -- matched and
     * un-matched both render 273,206 bytes. */
    { "multitouch",         true,  VM_OPT_GROUP_HARDWARE,    VM_OPT_IMPL_HARNESS },
    { "vram",               true,  VM_OPT_GROUP_PATCH,       VM_OPT_IMPL_HARNESS },
    { "lcd-panel-id",       true,  VM_OPT_GROUP_PATCH,       VM_OPT_IMPL_HARNESS },
    { "memory-reg",         true,  VM_OPT_GROUP_PATCH,       VM_OPT_IMPL_HARNESS },
    { "rtc-patch",          true,  VM_OPT_GROUP_PATCH,       VM_OPT_IMPL_HARNESS },
    /* On since 2026-07-29. This machine un-matches /arm-io/mbx, and without
     * the renderer override CA::WindowServer takes MBX2D, whose global context
     * is NULL for exactly that reason, so SpringBoard composites nothing.
     * run149 and run150 differ in this flag alone, at the same budget, and
     * draw 273206 bytes against 1890. A default that guarantees a black screen
     * was not a neutral choice. */
    { "ca-software-render", true,  VM_OPT_GROUP_PATCH,       VM_OPT_IMPL_HARNESS },
    /* activate was NOWHERE until the app grew MAP_PROVISION_ACTIVATE and
     * VMFirmwareBoot.c began passing rootfs_work_activation_entries(). Its row
     * text claimed "NOT IMPLEMENTED ANYWHERE" while the app's own boot report
     * listed it as applied, in the same build. */
    { "activate",           true,  VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_HARNESS },
    { "jb-codesign",        false, VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_NOWHERE },
    { "jb-payload",         false, VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_NOWHERE },
    { "ppp",                false, VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_HARNESS },
    { "nat",                true,  VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_HARNESS }
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
    CHECK(vm_option_index("nat") == (int)NEXPECTED - 1,
          "nat is not the last row");
    CHECK(vm_option_index("ppp") == (int)NEXPECTED - 2,
          "ppp is not the second-to-last row");
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

/*
 * The omissions table, longhand, for the same reason the mirror is longhand:
 * changing what the app refuses to offer should be a deliberate edit to a
 * test, not a silent one.
 *
 * check_option_mirror.cmake enforces the other half -- that these names plus
 * the mirrored ones account for bootkernel's live table exactly. What THIS
 * checks is the part a cmake script cannot: that the list is the one that was
 * agreed, that no name is in both tables, and that every omission carries a
 * reason. An omission with an empty reason is indistinguishable from a row
 * somebody dropped to make a build pass.
 */
static const char *const EXPECTED_OMISSIONS[] = {
    "framebuffer", "iomfb-display", "hle", "fstab-fixup", "ramdisk-low",
    "stop-on-abort", "kext-map", "print-config", "call-probe-regs",
    "call-probe-live", "uart4-rx-irq",
};

static void test_omissions(void) {
    const unsigned want =
        (unsigned)(sizeof EXPECTED_OMISSIONS / sizeof EXPECTED_OMISSIONS[0]);
    CHECK(vm_option_omitted_count() == want,
          "omission count is %u, expected %u", vm_option_omitted_count(), want);

    for (unsigned i = 0; i < want && i < vm_option_omitted_count(); i++) {
        const vm_option_omission_t *o = vm_option_omitted_at(i);
        CHECK(o != NULL, "omission %u is NULL", i);
        if (!o) continue;
        CHECK(o->name && strcmp(o->name, EXPECTED_OMISSIONS[i]) == 0,
              "omission %u is '%s', expected '%s'", i,
              o->name ? o->name : "(null)", EXPECTED_OMISSIONS[i]);
        /* A reason is what separates "we decided not to" from "we forgot". */
        CHECK(o->reason && o->reason[0] != '\0',
              "omission '%s' carries no reason", EXPECTED_OMISSIONS[i]);
        CHECK(o->reason && strlen(o->reason) >= 20u,
              "omission '%s' has a reason too short to be one: '%s'",
              EXPECTED_OMISSIONS[i], o->reason ? o->reason : "");
    }

    CHECK(vm_option_omitted_at(vm_option_omitted_count()) == NULL,
          "one past the end of the omissions table is not NULL");

    /* Disjoint. A name in both tables is a contradiction: the app cannot both
     * offer a toggle and declare that it does not. */
    for (unsigned i = 0; i < vm_option_omitted_count(); i++) {
        const vm_option_omission_t *o = vm_option_omitted_at(i);
        if (!o || !o->name) continue;
        CHECK(vm_option_index(o->name) < 0,
              "'%s' is both mirrored and declared omitted", o->name);
    }

    /* And unique among themselves. */
    for (unsigned i = 0; i < vm_option_omitted_count(); i++)
        for (unsigned j = i + 1u; j < vm_option_omitted_count(); j++) {
            const vm_option_omission_t *a = vm_option_omitted_at(i);
            const vm_option_omission_t *b = vm_option_omitted_at(j);
            if (!a || !b || !a->name || !b->name) continue;
            CHECK(strcmp(a->name, b->name) != 0,
                  "omission '%s' appears twice", a->name);
        }
}

/*
 * `--list` prints the app's whole claim about bootkernel's table, one name per
 * line, for check_option_mirror.cmake to partition the real binary's
 * --print-config against. Printed by the TEST rather than by the app so that
 * nothing in the shipped code exists only to be tested.
 */
static int list_mode(void) {
    for (unsigned i = 0; i < vm_option_count(); i++) {
        const vm_option_t *row = vm_option_at(i);
        if (row && row->name) printf("mirror:%s\n", row->name);
    }
    for (unsigned i = 0; i < vm_option_omitted_count(); i++) {
        const vm_option_omission_t *o = vm_option_omitted_at(i);
        if (o && o->name) printf("omit:%s\n", o->name);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--list") == 0) return list_mode();

    test_table_matches_bootkernel();
    test_rows_are_grouped_and_unique();
    test_lookup();
    test_command_line();
    test_command_line_truncation_and_nulls();
    test_omissions();

    printf("vmoptions: %u checks, %u failed\n", tests, failed);
    return failed ? 1 : 0;
}
