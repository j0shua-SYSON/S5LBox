/*
 * S5LBox — what the settings screen's switches do to a real bring-up, and what
 * they demonstrably do not.
 *
 * WHY THIS FILE EXISTS. VMOptions.c records sixteen toggles and
 * core/include/bringup.h accepts a request with three opt-outs on it. Between
 * those two facts sat a gap nobody could see from either side: the switches
 * were stored, shown, rendered back as a bootkernel command line, and never
 * read by the code that actually boots. This is the join, and it is written so
 * that the join is CHECKABLE — every row of the option table has exactly one
 * entry in the map below, and the test fails when a row has none.
 *
 * IT IS ALSO WHERE THE APP ADMITS WHAT IT CANNOT DO. Nine rows currently
 * reach bring-up or a live runtime service, three are provisioned into a work
 * image, and four are fixed or unavailable here. They fall into three kinds,
 * and the
 * difference between them is the whole point:
 *
 *   APPLIED      the request now carries this row's value, both ways.
 *   PROVISIONED  it is not a boot-time decision at all: it is written into the
 *                machine's work image when that image is made, so setting it
 *                today changes nothing until the image is remade.
 *   IGNORED      nothing in this app reads it. The machine does one fixed
 *                thing, which may or may not be what the switch says.
 *
 * The last of those is why `effective` exists next to `requested`. The app once
 * lacked the device-tree un-match step, so five OFF rows silently stayed ON;
 * that bug is fixed and all six hardware rows now reach bring-up. Rows whose
 * effect depends on persisted state still need the distinction: NAT, for
 * example, defaults ON but is effective only when this machine's work image
 * records the guest PPP job. A report that showed only the requested value
 * would still lie, so the override is counted and named.
 *
 * PLAIN C11, and for the usual reason: no Objective-C is compilable by a host
 * CI runner, and "which switch reached the machine" is exactly the claim a UI
 * can make falsely with nothing to catch it.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMBOOTOPTIONS_H
#define S5LBOX_APP_VMBOOTOPTIONS_H

#include "VMOptions.h"
#include "bringup.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sized above the option table for the same reason VM_INSTANCE_OPTION_MAX is:
 * adding a toggle must not silently truncate a report. The test asserts the
 * live table still fits.
 */
#define VM_BOOT_OPTION_MAX 32u

/* One line for a status bar or a bring-up note. Long enough to name every row
 * of the current table twice over; truncation is snprintf's, never silent
 * corruption. */
#define VM_BOOT_OPTIONS_SUMMARY_CAPACITY 320u

typedef enum {
    /* The bring-up request carries this row, in both directions. */
    VM_BOOT_OPTION_APPLIED = 0,
    /* Baked into the work image when that image is created; this start does
     * nothing about it and cannot read back what was done. */
    VM_BOOT_OPTION_PROVISIONED,
    /* Nothing in the app reads it. `effective` says what happens instead. */
    VM_BOOT_OPTION_IGNORED
} vm_boot_option_outcome_t;

typedef struct {
    unsigned char outcome;   /* vm_boot_option_outcome_t                     */
    bool  requested;         /* what the switch says                         */
    /*
     * What the machine will actually do about it on THIS start. Equal to
     * `requested` for APPLIED rows by construction. For IGNORED rows it is the
     * fixed behaviour, which is the whole reason this field is separate.
     *
     * For PROVISIONED rows it repeats `requested` and means only "this is what
     * a work image made now would get" -- an image made earlier carries
     * whatever was set then, and nothing here can inspect it. The note says so.
     */
    bool  effective;
    /* Why, in one sentence a user can act on. NULL only for APPLIED rows. */
    const char *note;
} vm_boot_option_status_t;

typedef struct {
    vm_boot_option_status_t row[VM_BOOT_OPTION_MAX];
    unsigned count;          /* rows filled in; == vm_option_count()         */
    unsigned applied;
    unsigned provisioned;
    unsigned ignored;
    /*
     * Rows where `effective` differs from `requested`. This is the number that
     * matters: it is the count of switches the screen shows in one position
     * and the machine honours in the other, and it is non-zero on a completely
     * untouched installation.
     */
    unsigned overridden;
    /* One line naming the overridden and provisioned rows. Empty when there is
     * nothing to say, which today never happens -- see the note above. */
    char summary[VM_BOOT_OPTIONS_SUMMARY_CAPACITY];
    /*
     * Backing store for s5l_bringup_request_t::unmatch, which is only a pointer
     * and does not own what it points at. It lives here rather than in a static
     * because two machines may be configured at once, and the report already
     * outlives the bring-up call that reads it -- VMFirmwareBoot.c holds both
     * across s5l_bringup().
     *
     * The strings themselves are literals from this file's table, so only the
     * pointers need somewhere to sit.
     */
    const char *unmatch[VM_BOOT_OPTION_MAX];
    unsigned unmatch_count;
} vm_boot_options_report_t;

/*
 * Fill in the parts of `request` these values control, and report what became
 * of every row.
 *
 * `values` is in option-table order. NULL means "every row at its table
 * default", and rows past `count` are likewise their default -- a short array
 * must never read as "everything off", which is the same rule
 * vm_option_command_line() follows and for the same reason.
 *
 * `request` may be NULL, which computes the report without building anything:
 * that is how the settings screen asks what a row would do without starting a
 * machine. Fields of `request` this function does not own are left alone, so
 * it may be called after the caller has filled in the kernel and the media.
 *
 * `report` is required and is fully reset first.
 */
void vm_boot_options_apply(const bool *values, unsigned count,
                           s5l_bringup_request_t *request,
                           vm_boot_options_report_t *report);

/*
 * The work-image transformations these same values ask for.
 *
 * Separate from the above because it happens at a different time, on a
 * different thread, and about a different file: rootfs_work_create() applies
 * these once when the ~450 MB image is made, and a boot cannot revisit them.
 */
typedef struct {
    bool ca_software_render;
    /*
     * Adds the two catalog objects lockdownd needs -- the Lockdown directory
     * the stock image does not have, and the data_ark.plist inside it -- so
     * SpringBoard does not sit at "connect to iTunes". Offline provisioning of
     * a file on this machine's own writable image: no Apple record is applied
     * and none is verified.
     */
    bool activate;
    /* Installs the stock pppd launchd job into a newly made work image. */
    bool ppp;
} vm_boot_provision_options_t;

void vm_boot_options_for_provisioning(const bool *values, unsigned count,
                                      vm_boot_provision_options_t *out);

/*
 * Replace the settings-time prediction for PPP with what this work image
 * actually records. NAT is then effective only when both its switch and that
 * recorded PPP job are on. A recorded PPP job also selects the serial
 * driver's PIO boot argument: the guest job and the uart4 host peer cannot
 * exchange bytes if AppleS5L8900XSerial takes its DMA path. Rebuilds counts and
 * summary so a boot report cannot claim that changing an image-time switch
 * rewrote an existing filesystem.
 *
 * `request` may be NULL when only the report is needed. When supplied, it must
 * still use bring-up's default command line; this function owns the one app
 * extension to that line.
 */
void vm_boot_options_reconcile_network(vm_boot_options_report_t *report,
                                       s5l_bringup_request_t *request,
                                       bool ppp_provisioned);

/*
 * Reconcile both guest-jailbreak rows with a completed per-machine install
 * record. The filesystem payload and relaxed guest code-signing policy are a
 * single persisted state: a settings value cannot install either one during
 * boot, and bring-up must never enable only the kernel half.
 *
 * `installed` means that the rootfs transaction completed and published its
 * host-side record. It does not claim that Cydia launched, that an unsigned
 * binary ran, or that a tweak loaded; those require separate runtime proof.
 * `request` may be NULL when only the report is needed.
 */
void vm_boot_options_reconcile_jailbreak(vm_boot_options_report_t *report,
                                         s5l_bringup_request_t *request,
                                         bool installed);

/*
 * The map, for its own test only. The app calls neither: the property they
 * defend is that the map and VMOptions.c's table are the SAME SET of names,
 * which nothing at run time can check and nothing at run time should have to.
 * A row in one and not the other is a switch whose fate nobody decided.
 */
unsigned vm_boot_options_map_count(void);
const char *vm_boot_options_map_name(unsigned index);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMBOOTOPTIONS_H */
