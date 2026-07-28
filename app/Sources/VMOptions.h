/*
 * S5LBox — the settings screen's option table.
 *
 * tools/bootkernel.c holds every boolean the emulator has in one table
 * (BOOT_TOGGLES) precisely so that its help text and its run header cannot
 * drift from what the code does. A settings screen that re-typed those names,
 * defaults and reasons in Objective-C would be a second copy free to drift from
 * the first, which is the failure that table was built to end.
 *
 * So the app's copy is a table too, in the same shape, in plain C11: one row
 * per toggle, the name spelled exactly as bootkernel spells it, and the same
 * default. app/Tests/test_vmoptions.c pins the whole thing -- names, order,
 * defaults and the rendered command line -- so a change on either side is a
 * deliberate change to a test rather than a silent divergence.
 *
 * SOURCE OF TRUTH: tools/bootkernel.c, BOOT_TOGGLES. When that table changes,
 * this one and its test change with it.
 *
 * DELIBERATELY NOT MIRRORED HERE, because they describe a desktop run rather
 * than a setting a phone screen can meaningfully offer. That list used to be a
 * sentence in this comment, and a sentence cannot fail a build: `ppp` was added
 * to BOOT_TOGGLES and mirrored here by hand, and nothing anywhere would have
 * complained if it had not been. So the omissions are a TABLE now, and
 * core/tests/check_option_mirror.cmake requires bootkernel's live table to
 * partition exactly into the mirrored rows and the omitted ones -- a new
 * toggle on either side fails until somebody decides which it is.
 *
 * "Missing" and "deliberately not offered" are different, and only one of them
 * is a bug. The reason string is what tells them apart, so every omission
 * carries one.
 *
 * WHICH ROWS ARE APPLIED IS NOT DECIDED HERE, and this file deliberately does
 * not say. It used to: it said NOTHING IN THIS TABLE IS APPLIED BY THE APP,
 * which was true when the app booted no firmware and became false the day
 * bring-up landed, with nothing anywhere to notice. A sentence in a comment
 * cannot fail a build.
 *
 * app/Sources/VMBootOptions.c is the answer instead. It maps every row of this
 * table onto what a bring-up request can express, and its test fails when a
 * row has no map entry -- so "does this switch do anything" is a fact with a
 * test attached rather than a paragraph that rots. Today two rows reach the
 * request, one is written into the work image, and eleven do not reach the
 * machine at all; the settings screen shows that per row, from the same C.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMOPTIONS_H
#define S5LBOX_APP_VMOPTIONS_H

#include <stdbool.h>
#include <stddef.h>

/* Same grouping bootkernel prints its help under, minus the two groups whose
 * rows are not mirrored (memory layout, diagnostics). */
typedef enum {
    VM_OPT_GROUP_HARDWARE = 0,
    VM_OPT_GROUP_PATCH,
    VM_OPT_GROUP_GUEST_STATE,
    VM_OPT_GROUP_COUNT
} vm_option_group_t;

/*
 * Whether a row is implemented ANYWHERE, which is a different question from
 * whether it reaches this particular boot. The second question is answered per
 * row at runtime by vm_boot_options_report_t::outcome, and the settings screen
 * counts that rather than this field -- so this field only has to separate
 * "somebody wrote this" from "nobody has".
 *
 * The HARNESS name is historical and was once literally true: these rows were
 * bootkernel-only and the app could not perform them. That stopped being the
 * shape of the project when the app began booting through the same
 * core/src/boot/bringup.c the harness does, and the device-tree un-match that
 * fixed the on-device hang on 2026-07-28 is in the shared core, not the tool.
 * Read it as "implemented", not as "desktop only".
 */
typedef enum {
    VM_OPT_IMPL_HARNESS = 0,   /* implemented -- in the harness, the shared core, or both */
    VM_OPT_IMPL_NOWHERE        /* not implemented in either, and says so                  */
} vm_option_impl_t;

typedef struct {
    const char   *name;    /* --name enables, --no-name disables; bootkernel's spelling */
    const char   *title;   /* short label for a table row                               */
    const char   *detail;  /* one sentence: what it does and why the default is that    */
    bool          def;     /* bootkernel's default, and therefore the app's             */
    unsigned char group;   /* vm_option_group_t                                         */
    unsigned char impl;    /* vm_option_impl_t                                          */
} vm_option_t;

/*
 * A toggle bootkernel has that this app does not offer, and the reason. The
 * reason is not decoration: it is the difference between a considered decision
 * and a row somebody forgot, and the mirror check cannot tell those apart on
 * its own.
 */
typedef struct {
    const char *name;      /* bootkernel's spelling, exactly */
    const char *reason;    /* one clause, lower case, no full stop */
} vm_option_omission_t;

/* How many rows the table has. */
unsigned vm_option_count(void);

/* How many toggles are deliberately not offered. */
unsigned vm_option_omitted_count(void);

/* Omission `index`, or NULL when out of range. */
const vm_option_omission_t *vm_option_omitted_at(unsigned index);

/* Row `index`, or NULL when out of range. */
const vm_option_t *vm_option_at(unsigned index);

/* Index of the row called `name`, or -1 for NULL and unknown names. */
int vm_option_index(const char *name);

/* Heading and standing caveat for a group, or NULL when out of range. */
const char *vm_option_group_title(unsigned group);
const char *vm_option_group_note(unsigned group);

/*
 * Render the bootkernel arguments that `values` corresponds to: "--mbx" for a
 * row turned on against an off default, "--no-vram" for the reverse, nothing at
 * all for a row left at its default, single-space separated in table order.
 *
 * snprintf semantics, so a caller can size the buffer in one dry run: returns
 * the length the full string would have had, excluding the terminator, and
 * writes at most `cap` bytes including it. `out` may be NULL when `cap` is 0.
 * Rows past `count`, and every row when `values` is NULL, are treated as absent
 * rather than as false -- a short array must not read as "everything off".
 */
size_t vm_option_command_line(const bool *values, unsigned count,
                              char *out, size_t cap);

#endif /* S5LBOX_APP_VMOPTIONS_H */
