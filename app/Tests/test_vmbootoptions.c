/*
 * Host-side tests for app/Sources/VMBootOptions.c — the join between the
 * settings screen's sixteen switches and the bring-up request.
 *
 * The property being defended is not "the right switches work". It is that the
 * app cannot show a switch in one position while the machine honours the
 * other WITHOUT SAYING SO. Every one of those facts is asserted here longhand,
 * so that a change to either side is a deliberate edit to a test rather than a
 * UI that silently starts lying again.
 *
 * The counts in this comment used to be "two rows reach the request; twelve do
 * not; six are already in conflict on an untouched installation", and they
 * were accurate when written. They are not the shape of the thing any more:
 * nine rows now reach bring-up or a runtime service (two opt-outs, six nubs,
 * and NAT), three are written into the work image when it is made, and four
 * remain fixed. Exactly one -- "nat" -- conflicts with the settings-time
 * prediction at its defaults before the actual work-image record is read. The
 * live numbers are asserted below; this paragraph is prose and must never be
 * the thing anyone trusts.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMBootOptions.h"

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

static int index_of(const char *name) {
    int i = vm_option_index(name);
    if (i < 0) printf("  (option \"%s\" is not in the table)\n", name);
    return i;
}

static void defaults_into(bool *values) {
    for (unsigned i = 0; i < vm_option_count(); i++)
        values[i] = vm_option_at(i)->def;
}

/* ------------------------------------------------------------------------- */

/*
 * The map and the option table must be the same set of names. A row in the
 * table with no map entry is a switch whose fate nobody decided; a map entry
 * with no table row is a decision about a switch that no longer exists.
 */
static void test_map_covers_the_table(void) {
    CHECK(vm_option_count() <= VM_BOOT_OPTION_MAX,
          "the option table (%u) outgrew VM_BOOT_OPTION_MAX (%u)",
          vm_option_count(), VM_BOOT_OPTION_MAX);

    CHECK(vm_boot_options_map_count() == vm_option_count(),
          "the map has %u rows and the option table %u",
          vm_boot_options_map_count(), vm_option_count());

    for (unsigned i = 0; i < vm_boot_options_map_count(); i++) {
        const char *name = vm_boot_options_map_name(i);
        CHECK(name != NULL, "map row %u has no name", i);
        if (!name) continue;
        CHECK(vm_option_index(name) >= 0,
              "the map decides \"%s\", which is not in the option table", name);
        for (unsigned j = i + 1u; j < vm_boot_options_map_count(); j++) {
            const char *other = vm_boot_options_map_name(j);
            CHECK(!other || strcmp(name, other) != 0,
                  "\"%s\" appears in the map at both %u and %u", name, i, j);
        }
    }

    CHECK(vm_boot_options_map_name(vm_boot_options_map_count()) == NULL,
          "one past the end of the map is not NULL");

    /* And the other direction, which is the one that matters: a row nobody
     * decided about must be impossible, not merely reported. */
    bool values[VM_BOOT_OPTION_MAX];
    vm_boot_options_report_t report;
    defaults_into(values);
    vm_boot_options_apply(values, vm_option_count(), NULL, &report);
    for (unsigned i = 0; i < report.count; i++) {
        const vm_option_t *option = vm_option_at(i);
        CHECK(report.row[i].note == NULL ||
              strstr(report.row[i].note, "no recorded effect") == NULL,
              "\"%s\" has no map entry", option ? option->name : "?");
    }
}

/*
 * The two scalar opt-outs that reach request fields directly, both ways. The
 * device-tree rows are tested against the request's un-match list below, and
 * NAT is a runtime service, so those have separate assertions.
 */
static void test_applied_rows_reach_the_request(void) {
    bool values[VM_BOOT_OPTION_MAX];
    s5l_bringup_request_t request;
    vm_boot_options_report_t report;

    const int lcd = index_of("lcd-panel-id");
    const int mem = index_of("memory-reg");
    if (lcd < 0 || mem < 0) return;

    /* Defaults are both on, and an opt-out request must stay zeroed: a zeroed
     * request is run89-base, and the defaults ARE run89-base. */
    defaults_into(values);
    memset(&request, 0, sizeof request);
    vm_boot_options_apply(values, vm_option_count(), &request, &report);
    CHECK(!request.no_lcd_panel_id,
          "lcd-panel-id on set no_lcd_panel_id");
    CHECK(!request.no_memory_node, "memory-reg on set no_memory_node");
    CHECK(report.row[lcd].outcome == VM_BOOT_OPTION_APPLIED,
          "lcd-panel-id is not applied");
    CHECK(report.row[mem].outcome == VM_BOOT_OPTION_APPLIED,
          "memory-reg is not applied");
    CHECK(report.row[lcd].effective == report.row[lcd].requested &&
          report.row[mem].effective == report.row[mem].requested,
          "an applied row's effect differs from what was requested");
    CHECK(report.row[lcd].note == NULL && report.row[mem].note == NULL,
          "an applied row carries a caveat it does not need");

    /* And off, which is the half that was previously impossible to express. */
    defaults_into(values);
    values[lcd] = false;
    values[mem] = false;
    memset(&request, 0, sizeof request);
    vm_boot_options_apply(values, vm_option_count(), &request, &report);
    CHECK(request.no_lcd_panel_id,
          "turning lcd-panel-id off did not set no_lcd_panel_id");
    CHECK(request.no_memory_node,
          "turning memory-reg off did not set no_memory_node");
    CHECK(!report.row[lcd].effective && !report.row[mem].effective,
          "a row turned off still reports as effective");
    /* The two request opt-outs, the SIX nubs, and live NAT are applied rows.
     * Counted rather than left open so that
     * a row quietly joining them has to be a deliberate edit here -- which is
     * what this is: `multitouch` was added on 2026-07-29 and it is applied
     * like the rest, even at its default, because "left matched because you
     * asked for it" is the switch working rather than being ignored. */
    CHECK(report.applied == 9u, "%u rows applied, expected 9", report.applied);

    /* Nothing else in the request may be touched. no_framebuffer in
     * particular: the app never turns the display off, and a mapping that
     * reached it would blank the screen from the settings table. */
    CHECK(!request.no_framebuffer,
          "the option mapping set no_framebuffer");
    CHECK(request.kernel == NULL && request.root_media == NULL &&
          request.cmdline == NULL && request.kernel_gate == NULL,
          "the option mapping wrote a field it does not own");
    CHECK(request.v_display == 0u && request.boot_args_version == 0u,
          "the option mapping disturbed the boot_args defaults");
}

/*
 * A fresh settings table, before a particular work image is examined. NAT is
 * the only default mismatch: its switch is on while PPP defaults off, so there
 * is no carrier. vm_boot_options_reconcile_network() later replaces that
 * prediction with the image's recorded PPP state. Keeping the pre-image case
 * explicit prevents the settings screen from promising a route with no link.
 */
static const char *const EXPECTED_OVERRIDDEN_AT_DEFAULT[] = {
    "nat"
};

/*
 * THE FIVE NUBS REACH THE MACHINE, and this is the test that says so.
 *
 * It used to assert the exact opposite -- that every nub stayed matched
 * whatever its switch said -- and it passed for as long as that was true. That
 * was not a harmless inaccuracy. At the time, /arm-io/mbx hung the boot in the
 * PowerVR driver's reset poll, and /arm-io/sha1 sent cs_validate_page's
 * 4096-byte digests to a register file this VM did not model, so launchd's
 * first text page failed its signature. An iPhone running the app reached
 * 11.5 G instructions without launchd ever starting, while the desktop -- which
 * un-matched both by default -- was at launchd before 1.5 G. MBX is modelled
 * much further now, but its accepted default stays off until final cold/FPS
 * validation; this test still verifies that the requested default reaches the
 * device tree rather than being ignored.
 *
 * A test that pins a bug in place is worse than no test, because it makes the
 * bug look like a decision. This one pins the fix.
 */
static void test_untouched_installation_reaches_the_machine(void) {
    bool values[VM_BOOT_OPTION_MAX];
    vm_boot_options_report_t report;
    s5l_bringup_request_t request;
    const unsigned want = (unsigned)(sizeof EXPECTED_OVERRIDDEN_AT_DEFAULT /
                                     sizeof EXPECTED_OVERRIDDEN_AT_DEFAULT[0]);

    defaults_into(values);
    memset(&request, 0, sizeof request);
    vm_boot_options_apply(values, vm_option_count(), &request, &report);

    CHECK(report.overridden == want,
          "%u rows are overridden at their defaults, expected %u",
          report.overridden, want);

    for (unsigned i = 0; i < want; i++) {
        int index = index_of(EXPECTED_OVERRIDDEN_AT_DEFAULT[i]);
        if (index < 0) continue;
        CHECK(report.row[index].effective != report.row[index].requested,
              "\"%s\" is not reported as overridden at its default",
              EXPECTED_OVERRIDDEN_AT_DEFAULT[i]);
        CHECK(report.row[index].note && report.row[index].note[0],
              "\"%s\" is overridden and says nothing about it",
              EXPECTED_OVERRIDDEN_AT_DEFAULT[i]);
        CHECK(strstr(report.summary, EXPECTED_OVERRIDDEN_AT_DEFAULT[i]) != NULL,
              "the summary does not name \"%s\": \"%s\"",
              EXPECTED_OVERRIDDEN_AT_DEFAULT[i], report.summary);
    }

    /*
     * `multitouch` LEFT THIS LIST on 2026-07-30, which the entry that used to
     * sit here said it would do "the day the bootload completes". It did:
     * run163 drove every stage of the HBPP state machine once and in order,
     * and r181/r182 slid the lock screen open. It no longer costs the display
     * either -- matched and un-matched both render 273,206 bytes -- so the
     * trade it was hidden for does not exist any more. It is asserted as
     * MATCHED below rather than simply deleted, because "it quietly stopped
     * being applied" and "it is now on by default" would otherwise look the
     * same here.
     */
    static const char *const NUBS[] = {
        "mbx", "sha1", "baseband", "spi2", "usb-otg"
    };
    const unsigned nub_n = (unsigned)(sizeof NUBS / sizeof NUBS[0]);

    for (unsigned i = 0; i < nub_n; i++) {
        int index = index_of(NUBS[i]);
        if (index < 0) continue;
        CHECK(report.row[index].outcome == VM_BOOT_OPTION_APPLIED,
              "\"%s\" does not reach the machine", NUBS[i]);
        CHECK(report.row[index].effective == report.row[index].requested,
              "\"%s\" is not honoured as set", NUBS[i]);
        CHECK(!report.row[index].effective,
              "\"%s\" is matched at its default", NUBS[i]);
        CHECK(report.row[index].note == NULL,
              "\"%s\" is applied and still carries a caveat", NUBS[i]);
    }

    /* Cleared is the default for all five, so all five are struck, and the
     * request points at exactly the list the report published. */
    CHECK(report.unmatch_count == nub_n,
          "%u nodes un-matched at defaults, expected %u",
          report.unmatch_count, nub_n);
    CHECK(request.unmatch_count == report.unmatch_count,
          "the request carries %u un-match paths, the report %u",
          request.unmatch_count, report.unmatch_count);
    CHECK(request.unmatch == report.unmatch,
          "the request does not point at the report's list");

    /*
     * The digitizer is the other half of that change: still APPLIED, so the
     * switch reaches the machine, but now effective at its default and
     * therefore absent from the un-match list. Checked explicitly, because a
     * row that silently stopped being applied would also produce a shorter
     * list.
     */
    {
        int mt = index_of("multitouch");
        CHECK(mt >= 0, "the multitouch row is gone from the table");
        if (mt >= 0) {
            CHECK(report.row[mt].outcome == VM_BOOT_OPTION_APPLIED,
                  "\"multitouch\" does not reach the machine");
            CHECK(report.row[mt].effective,
                  "\"multitouch\" is un-matched at its default; the bootload "
                  "completes and a slide-to-unlock works, so the digitizer "
                  "should be present");
            for (unsigned j = 0; j < report.unmatch_count; j++)
                CHECK(!report.unmatch[j] ||
                          strcmp(report.unmatch[j], "arm-io/spi1/multi-touch"),
                      "the digitizer is still being struck from the tree");
        }
    }

    /* By path, not by count: five of the wrong nodes would pass a count. */
    static const char *const PATHS[] = {
        "arm-io/mbx", "arm-io/sha1", "baseband", "arm-io/spi2",
        "arm-io/usb-otg"
    };
    for (unsigned i = 0; i < nub_n; i++) {
        bool found = false;
        for (unsigned j = 0; j < report.unmatch_count; j++)
            if (report.unmatch[j] && !strcmp(report.unmatch[j], PATHS[i]))
                found = true;
        CHECK(found, "/%s is not in the un-match list", PATHS[i]);
    }

    /*
     * Setting a nub asks for it to stay matched, and that is honoured too: it
     * leaves the list, and it is still not an override, because the machine
     * did what the switch said.
     */
    defaults_into(values);
    for (unsigned i = 0; i < nub_n; i++) {
        int index = index_of(NUBS[i]);
        if (index >= 0) values[index] = true;
    }
    memset(&request, 0, sizeof request);
    vm_boot_options_apply(values, vm_option_count(), &request, &report);
    CHECK(report.unmatch_count == 0u,
          "matching every nub still un-matches %u", report.unmatch_count);
    CHECK(request.unmatch == NULL,
          "an empty list is published as a pointer rather than NULL");
    CHECK(report.overridden == want,
          "matching all five nubs leaves %u overrides, expected %u",
          report.overridden, want);
}

/*
 * The rows that are fixed ON however they are set, and the rows that are fixed
 * OFF. Both kinds are silent when the switch happens to agree and loud when it
 * does not; the loud case is the one asserted, because the silent one is what
 * the old code did for everything.
 */
static void test_fixed_rows(void) {
    bool values[VM_BOOT_OPTION_MAX];
    vm_boot_options_report_t report;

    /* vram and rtc-patch default ON and cannot be turned off. */
    static const char *const FIXED_ON[] = { "vram", "rtc-patch" };
    for (unsigned i = 0; i < sizeof FIXED_ON / sizeof FIXED_ON[0]; i++) {
        int index = index_of(FIXED_ON[i]);
        if (index < 0) continue;

        defaults_into(values);
        vm_boot_options_apply(values, vm_option_count(), NULL, &report);
        CHECK(report.row[index].effective && report.row[index].requested,
              "\"%s\" does not agree with the machine at its default",
              FIXED_ON[i]);

        defaults_into(values);
        values[index] = false;
        vm_boot_options_apply(values, vm_option_count(), NULL, &report);
        CHECK(report.row[index].outcome == VM_BOOT_OPTION_IGNORED,
              "\"%s\" claims it can be turned off", FIXED_ON[i]);
        CHECK(report.row[index].effective,
              "\"%s\" claims it was turned off", FIXED_ON[i]);
        CHECK(strstr(report.summary, FIXED_ON[i]) != NULL,
              "turning \"%s\" off is not reported: \"%s\"",
              FIXED_ON[i], report.summary);
    }

    /*
     * The two that are implemented nowhere. "activate" and "ppp" were here
     * and are not any more: both now reach rootfs_work at image time, and PPP
     * additionally has a live uart4 host peer.
     */
    static const char *const FIXED_OFF[] = {
        "jb-codesign", "jb-payload"
    };
    for (unsigned i = 0; i < sizeof FIXED_OFF / sizeof FIXED_OFF[0]; i++) {
        int index = index_of(FIXED_OFF[i]);
        if (index < 0) continue;

        defaults_into(values);
        values[index] = true;
        vm_boot_options_apply(values, vm_option_count(), NULL, &report);
        CHECK(report.row[index].outcome == VM_BOOT_OPTION_IGNORED,
              "\"%s\" claims to reach the machine", FIXED_OFF[i]);
        CHECK(!report.row[index].effective,
              "\"%s\" claims it was performed", FIXED_OFF[i]);
        CHECK(strstr(report.summary, FIXED_OFF[i]) != NULL,
              "asking for \"%s\" is not reported as refused: \"%s\"",
              FIXED_OFF[i], report.summary);
    }
}

/*
 * Three rows are written into the work image rather than a bring-up request,
 * so changing them later cannot rewrite a machine that already exists.
 */
static void test_provisioned_row(void) {
    bool values[VM_BOOT_OPTION_MAX];
    vm_boot_options_report_t report;
    vm_boot_provision_options_t provision;

    const int ca = index_of("ca-software-render");
    const int ppp = index_of("ppp");
    if (ca < 0 || ppp < 0) return;

    defaults_into(values);
    vm_boot_options_apply(values, vm_option_count(), NULL, &report);
    CHECK(report.row[ca].outcome == VM_BOOT_OPTION_PROVISIONED,
          "ca-software-render is not reported as an image-time decision");
    /* ca-software-render, activate, and the stock pppd launchd job. */
    CHECK(report.provisioned == 3u,
          "%u provisioned rows, expected 3", report.provisioned);
    CHECK(report.row[ppp].outcome == VM_BOOT_OPTION_PROVISIONED,
          "ppp is not reported as an image-time decision");
    CHECK(report.row[ca].note &&
          strstr(report.row[ca].note, "work image") != NULL,
          "ca-software-render does not say where its value lives");
    CHECK(strstr(report.summary, "ca-software-render") != NULL,
          "the summary does not mention the image-time row: \"%s\"",
          report.summary);

    /* And it must reach the provisioner, both ways. This is the switch that
     * was previously forced on no matter what the screen showed.
     *
     * Written against the row's own default rather than a literal polarity.
     * The default moved on 2026-07-29 -- run149 and run150 differ in this flag
     * alone, at the same budget, and draw 273206 bytes against 1890 -- and a
     * test that hardcodes one side has to be edited every time that happens,
     * which is how a test quietly stops checking the thing it is named for. */
    const bool ca_def = vm_option_at((unsigned)ca)->def;
    defaults_into(values);
    vm_boot_options_for_provisioning(values, vm_option_count(), &provision);
    CHECK(provision.ca_software_render == ca_def,
          "the default (%s) did not reach the provisioner",
          ca_def ? "on" : "off");

    values[ca] = !ca_def;
    vm_boot_options_for_provisioning(values, vm_option_count(), &provision);
    CHECK(provision.ca_software_render == !ca_def,
          "flipping ca-software-render off its default did not reach the "
          "provisioner");

    const bool ppp_def = vm_option_at((unsigned)ppp)->def;
    defaults_into(values);
    vm_boot_options_for_provisioning(values, vm_option_count(), &provision);
    CHECK(provision.ppp == ppp_def,
          "the default ppp value did not reach the provisioner");
    values[ppp] = !ppp_def;
    vm_boot_options_for_provisioning(values, vm_option_count(), &provision);
    CHECK(provision.ppp == !ppp_def,
          "flipping ppp did not reach the provisioner");

    /* NULL means the table's defaults, never all-false-by-accident. */
    vm_boot_options_for_provisioning(NULL, 0u, &provision);
    CHECK(provision.ca_software_render == vm_option_at((unsigned)ca)->def,
          "a NULL value array did not fall back to the table default");
}

static void test_network_state_is_reconciled_with_the_work_image(void) {
    bool values[VM_BOOT_OPTION_MAX];
    vm_boot_options_report_t report;
    s5l_bringup_request_t request;
    const int ppp = index_of("ppp");
    const int nat = index_of("nat");
    if (ppp < 0 || nat < 0) return;

    defaults_into(values);
    values[ppp] = true;
    values[nat] = true;
    vm_boot_options_apply(values, vm_option_count(), NULL, &report);
    CHECK(report.row[ppp].effective && report.row[nat].effective,
          "a new PPP/NAT image is predicted off before reconciliation");

    memset(&request, 0, sizeof request);
    vm_boot_options_reconcile_network(&report, &request, false);
    CHECK(!report.row[ppp].effective && !report.row[nat].effective,
          "an image with no PPP marker still reports PPP/NAT on");
    CHECK(request.cmdline == NULL,
          "an image with no PPP job changed the kernel command line");
    CHECK(report.row[ppp].note && strstr(report.row[ppp].note, "without PPP") &&
          report.row[nat].note && strstr(report.row[nat].note, "no PPP job"),
          "the no-marker mismatch is not actionable");
    CHECK(strstr(report.summary, "ppp") && strstr(report.summary, "nat"),
          "the no-marker mismatch is absent from the summary: %s",
          report.summary);

    memset(&request, 0, sizeof request);
    vm_boot_options_reconcile_network(&report, &request, true);
    CHECK(report.row[ppp].effective && report.row[nat].effective,
          "a marked PPP/NAT image did not reconcile on");
    CHECK(request.cmdline &&
          strstr(request.cmdline, S5L_BRINGUP_DEFAULT_CMDLINE) ==
              request.cmdline &&
          strstr(request.cmdline, "uart4_dma_enable=0") != NULL,
          "a marked PPP image did not force uart4 onto its host-visible PIO "
          "path");

    defaults_into(values);                 /* PPP setting off, NAT setting on */
    vm_boot_options_apply(values, vm_option_count(), NULL, &report);
    memset(&request, 0, sizeof request);
    vm_boot_options_reconcile_network(&report, &request, true);
    CHECK(report.row[ppp].effective && report.row[nat].effective,
          "an existing PPP image followed a later PPP switch instead of its "
          "recorded filesystem");
    CHECK(request.cmdline &&
          strstr(request.cmdline, "uart4_dma_enable=0") != NULL,
          "an existing PPP image followed a later switch and lost its uart4 "
          "boot policy");
    CHECK(report.row[ppp].note && strstr(report.row[ppp].note, "contains"),
          "the recorded-on/settings-off mismatch has no explanation");

    values[nat] = false;
    vm_boot_options_apply(values, vm_option_count(), NULL, &report);
    memset(&request, 0, sizeof request);
    vm_boot_options_reconcile_network(&report, &request, true);
    CHECK(report.row[ppp].effective && !report.row[nat].effective,
          "the live NAT switch could not disable routing on a PPP image");
    CHECK(request.cmdline &&
          strstr(request.cmdline, "uart4_dma_enable=0") != NULL,
          "disabling NAT also disabled the PPP serial transport");
}

static void test_jailbreak_state_is_reconciled_with_the_work_image(void) {
    bool values[VM_BOOT_OPTION_MAX];
    vm_boot_options_report_t report;
    s5l_bringup_request_t request;
    const int codesign = index_of("jb-codesign");
    const int payload = index_of("jb-payload");
    if (codesign < 0 || payload < 0) return;

    /* A switch cannot manufacture the on-disk half during boot. Also start
     * request true so the absent record proves reconciliation actively clears
     * stale caller state rather than merely relying on memset. */
    defaults_into(values);
    values[codesign] = true;
    values[payload] = true;
    vm_boot_options_apply(values, vm_option_count(), NULL, &report);
    memset(&request, 0, sizeof request);
    request.guest_codesign_disabled = true;
    vm_boot_options_reconcile_jailbreak(&report, &request, false);
    CHECK(!request.guest_codesign_disabled,
          "an absent install record left guest code signing relaxed");
    CHECK(report.row[codesign].outcome == VM_BOOT_OPTION_APPLIED &&
          report.row[payload].outcome == VM_BOOT_OPTION_PROVISIONED,
          "the two install halves have the wrong runtime/image outcomes");
    CHECK(!report.row[codesign].effective &&
          !report.row[payload].effective,
          "settings alone claimed an installed guest payload");
    CHECK(report.row[codesign].note &&
          strstr(report.row[codesign].note, "without") &&
          report.row[payload].note &&
          strstr(report.row[payload].note, "no completed"),
          "the refused settings-only request is not actionable");
    CHECK(strstr(report.summary, "jb-codesign") &&
          strstr(report.summary, "jb-payload"),
          "the settings/record mismatch is absent from the summary: %s",
          report.summary);

    /* Conversely, a committed work image owns both halves even if the old
     * switches are off. This is the state the button-based UI will produce. */
    defaults_into(values);
    values[codesign] = false;
    values[payload] = false;
    vm_boot_options_apply(values, vm_option_count(), NULL, &report);
    memset(&request, 0, sizeof request);
    vm_boot_options_reconcile_jailbreak(&report, &request, true);
    CHECK(request.guest_codesign_disabled,
          "a completed install record did not reach bring-up");
    CHECK(report.row[codesign].effective && report.row[payload].effective,
          "a completed install record did not reconcile both halves on");
    CHECK(report.row[codesign].note &&
          strstr(report.row[codesign].note, "required") &&
          report.row[payload].note &&
          strstr(report.row[payload].note, "already contains"),
          "the recorded-on/settings-off mismatch has no explanation");
    CHECK(strstr(report.summary, "jb-codesign") &&
          strstr(report.summary, "jb-payload"),
          "the recorded install is absent from the summary: %s",
          report.summary);

    request.guest_codesign_disabled = false;
    vm_boot_options_reconcile_jailbreak(NULL, &request, true);
    CHECK(request.guest_codesign_disabled,
          "report-only omission prevented request reconciliation");
    vm_boot_options_reconcile_jailbreak(&report, NULL, true);
}

/*
 * Absent, short and over-long value arrays. A caller with no stored settings,
 * or one whose stored array predates a new row, must get each row's DEFAULT --
 * never false, which would read as "the user turned everything off".
 */
static void test_missing_and_short_value_arrays(void) {
    bool values[VM_BOOT_OPTION_MAX];
    vm_boot_options_report_t from_null, from_defaults, from_short;

    defaults_into(values);
    vm_boot_options_apply(values, vm_option_count(), NULL, &from_defaults);
    vm_boot_options_apply(NULL, 0u, NULL, &from_null);

    CHECK(from_null.count == from_defaults.count,
          "a NULL array reported %u rows, defaults reported %u",
          from_null.count, from_defaults.count);
    CHECK(from_null.overridden == from_defaults.overridden,
          "a NULL array is not the same as the defaults (%u vs %u overrides)",
          from_null.overridden, from_defaults.overridden);
    for (unsigned i = 0; i < from_null.count; i++)
        CHECK(from_null.row[i].requested == from_defaults.row[i].requested,
              "row %u differs between a NULL array and the defaults", i);

    /* A short array: the rows it covers are its own, the rest are defaults. */
    const int mbx = index_of("mbx");
    if (mbx == 0) {
        defaults_into(values);
        values[0] = true;                       /* mbx on, nothing else given */
        vm_boot_options_apply(values, 1u, NULL, &from_short);
        CHECK(from_short.row[0].requested,
              "a one-row array lost its only value");
        for (unsigned i = 1u; i < from_short.count; i++)
            CHECK(from_short.row[i].requested == vm_option_at(i)->def,
                  "row %u past a short array is not its default", i);
    }

    /* An over-long count must be clamped, not read past the table. */
    defaults_into(values);
    vm_boot_options_apply(values, 10000u, NULL, &from_short);
    CHECK(from_short.count == vm_option_count(),
          "an over-long count produced %u rows", from_short.count);
}

/* Every refusal has to be sayable, and every sentence has to be a sentence. */
static void test_every_note_is_usable(void) {
    bool values[VM_BOOT_OPTION_MAX];
    vm_boot_options_report_t report;

    defaults_into(values);
    vm_boot_options_apply(values, vm_option_count(), NULL, &report);

    CHECK(report.applied + report.provisioned + report.ignored == report.count,
          "the outcome counts (%u+%u+%u) do not add up to %u rows",
          report.applied, report.provisioned, report.ignored, report.count);

    for (unsigned i = 0; i < report.count; i++) {
        const vm_option_t *option = vm_option_at(i);
        const char *name = option && option->name ? option->name : "?";
        if (report.row[i].outcome == VM_BOOT_OPTION_APPLIED) {
            CHECK((report.row[i].effective == report.row[i].requested) ==
                      (report.row[i].note == NULL),
                  "\"%s\" applied note does not match whether it was "
                  "honoured", name);
            continue;
        }
        CHECK(report.row[i].note != NULL, "\"%s\" refuses without a reason",
              name);
        if (!report.row[i].note) continue;
        CHECK(strlen(report.row[i].note) >= 30u,
              "\"%s\" has a reason too short to be one: \"%s\"",
              name, report.row[i].note);
        /* A sentence a user reads, not a log token. */
        CHECK(report.row[i].note[strlen(report.row[i].note) - 1u] == '.',
              "\"%s\" has a reason that is not a sentence: \"%s\"",
              name, report.row[i].note);
    }

    CHECK(report.summary[0] != '\0',
          "an untouched installation has an override and says nothing");
    CHECK(strlen(report.summary) < VM_BOOT_OPTIONS_SUMMARY_CAPACITY,
          "the summary is not terminated inside its buffer");
}

/*
 * Every row flipped at once, which is the longest the summary can be. It has
 * to stay terminated inside its buffer whatever it costs to say.
 */
static void test_summary_bounds(void) {
    bool values[VM_BOOT_OPTION_MAX];
    vm_boot_options_report_t report;

    for (unsigned i = 0; i < vm_option_count(); i++)
        values[i] = !vm_option_at(i)->def;
    vm_boot_options_apply(values, vm_option_count(), NULL, &report);

    CHECK(strlen(report.summary) < VM_BOOT_OPTIONS_SUMMARY_CAPACITY,
          "everything flipped overran the summary");
    CHECK(report.overridden > 0u, "everything flipped overrides nothing");
    printf("  (everything flipped: %s)\n", report.summary);

    /* A NULL report is the one argument that has nowhere to report to, and it
     * must not be a crash. */
    vm_boot_options_apply(values, vm_option_count(), NULL, NULL);
    vm_boot_options_for_provisioning(values, vm_option_count(), NULL);
}

int main(void) {
    printf("== vm boot options ==\n");

    test_map_covers_the_table();
    test_applied_rows_reach_the_request();
    test_untouched_installation_reaches_the_machine();
    test_fixed_rows();
    test_provisioned_row();
    test_network_state_is_reconciled_with_the_work_image();
    test_jailbreak_state_is_reconciled_with_the_work_image();
    test_missing_and_short_value_arrays();
    test_every_note_is_usable();
    test_summary_bounds();

    printf("== vm boot options: %u checks, %u failure(s) ==\n",
           checks, failures);
    return failures ? 1 : 0;
}
