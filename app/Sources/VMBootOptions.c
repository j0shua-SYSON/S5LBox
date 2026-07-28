/*
 * S5LBox — VMBootOptions. See the header for the three outcomes and why
 * `effective` is a separate field from `requested`.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMBootOptions.h"

#include <stdio.h>
#include <string.h>

/*
 * How each row of VMOptions.c reaches — or fails to reach — a bring-up.
 *
 * Keyed by NAME rather than by index. The option table is walked in order by
 * the settings screen and reordering it is a legitimate change; a positional
 * map would then silently point every row at the wrong fate, which is exactly
 * the class of bug this whole file exists to make impossible.
 */
typedef enum {
    /* request->no_lcd_panel_id and request->no_memory_node: the two opt-outs
     * bring-up actually reads that this table also offers. */
    MAP_NO_LCD_PANEL_ID = 0,
    MAP_NO_MEMORY_NODE,
    /* rootfs_work_create()'s ca_software_render, at image-creation time. */
    MAP_PROVISION_CA_SOFTWARE_RENDER,
    /* Bring-up always does the thing; the switch is not consulted. */
    MAP_FIXED_ON,
    /* Nothing in the app does the thing; the switch is not consulted. */
    MAP_FIXED_OFF
} vm_boot_map_kind_t;

static const struct {
    const char *name;             /* VMOptions.c's spelling, exactly */
    unsigned char kind;           /* vm_boot_map_kind_t              */
    const char *note;             /* NULL only for the two applied rows */
} VM_BOOT_MAP[] = {
    /*
     * THE FIVE NUBS. core/src/boot/bringup.c has no un-match step of any kind
     * -- grep it for "unmatch" and there is nothing to find. bootkernel.c does
     * this with a private dt_unmatch() at line 2308 and calls it five times;
     * none of that is in the portable core, so the app's boot leaves every one
     * of these matched. That is the OPPOSITE of the table's own default for
     * all five, so these are overrides on an installation nobody has touched.
     */
    { "mbx", MAP_FIXED_ON,
      "/arm-io/mbx stays matched: the app's bring-up has no device-tree "
      "un-match step, so the PowerVR driver probes whatever this says." },
    { "sha1", MAP_FIXED_ON,
      "/arm-io/sha1 stays matched, for the same reason: bring-up un-matches "
      "no nub." },
    { "baseband", MAP_FIXED_ON,
      "/baseband stays matched, for the same reason: bring-up un-matches no "
      "nub." },
    { "spi2", MAP_FIXED_ON,
      "/arm-io/spi2 stays matched, for the same reason: bring-up un-matches "
      "no nub." },
    { "usb-otg", MAP_FIXED_ON,
      "/arm-io/usb-otg stays matched, for the same reason: bring-up un-matches "
      "no nub." },

    /*
     * /vram:reg is published whenever the framebuffer is, and bring-up carries
     * no separate opt-out for it (bringup.c guards it on `want_fb` alone).
     * This app never sets no_framebuffer, so the answer is always yes. Turning
     * the row off is the A/B control for the read-only-framebuffer failure,
     * and it is not available here.
     */
    { "vram", MAP_FIXED_ON,
      "/vram:reg is always published: bring-up ties it to the framebuffer, "
      "which this app never turns off." },

    { "lcd-panel-id", MAP_NO_LCD_PANEL_ID, NULL },
    { "memory-reg",   MAP_NO_MEMORY_NODE,  NULL },

    /*
     * The IORTC halfword is entry zero of ios3_kernel_patch.c's fixed table,
     * applied by the same gate call that installs the memory-disk SVC sites.
     * The gate is all-or-nothing -- refusing it would take the root filesystem
     * with it -- so this row cannot be turned off without a second gate.
     */
    { "rtc-patch", MAP_FIXED_ON,
      "The IORTC wait patch is always applied: it is one entry in the kernel "
      "gate's fixed table, and the gate that installs the memory-disk sites "
      "cannot apply part of it." },

    { "ca-software-render", MAP_PROVISION_CA_SOFTWARE_RENDER,
      "Written into this machine's work image when that image is prepared, "
      "not at boot, so changing it does nothing to a machine that already has "
      "one. A new machine gets an image built with it as set now." },

    { "activate", MAP_FIXED_OFF,
      "Not implemented anywhere. Writing ActivationState needs a file the "
      "imported root filesystem does not contain and nothing can yet create." },
    { "jb-codesign", MAP_FIXED_OFF,
      "Not implemented anywhere. Nothing in this app disables the guest "
      "kernel's code-signature enforcement." },
    { "jb-payload", MAP_FIXED_OFF,
      "Not implemented anywhere. Nothing in this app installs a payload onto "
      "the work image." },

    /*
     * rootfs_work_options_t::ppp_launchd_job would give the guest its half.
     * The host half -- a PPP endpoint on the far side of the emulated uart4 --
     * is not wired into this app, and a guest pppd talking to a UART nobody
     * answers is the declared-but-silent-device failure this project already
     * refuses for the baseband. So it is offered as neither.
     */
    { "ppp", MAP_FIXED_OFF,
      "Not offered here. The guest half could be written into the work image, "
      "but nothing in this app terminates PPP on the host side, so the guest's "
      "pppd would talk to a UART nobody answers." },
    /*
     * The NAT itself is portable -- core/src/net/net.c has no socket in it and
     * tools/net_host.c needs nothing privileged -- so this one is fixed off
     * only because the thing that would carry its datagrams is. It becomes a
     * real toggle here on the day the app terminates PPP, and not before:
     * offering a route to the internet with no link under it would be the
     * declared-but-silent-device failure the row above refuses.
     */
    { "nat", MAP_FIXED_OFF,
      "Not offered here, because --ppp above is not. The NAT is what would "
      "answer the guest's datagrams, and without the link there are none." }
};

#define VM_BOOT_MAP_COUNT \
    ((unsigned)(sizeof VM_BOOT_MAP / sizeof VM_BOOT_MAP[0]))

/*
 * The map entry for option row `index`. False means the option table has grown
 * a row nobody has decided about; the caller must not invent a fate for it.
 */
static bool mapped(unsigned index, unsigned char *out_kind,
                   const char **out_note) {
    const vm_option_t *option = vm_option_at(index);
    if (!option || !option->name) return false;
    for (unsigned i = 0; i < VM_BOOT_MAP_COUNT; i++) {
        if (strcmp(option->name, VM_BOOT_MAP[i].name) != 0) continue;
        if (out_kind) *out_kind = VM_BOOT_MAP[i].kind;
        if (out_note) *out_note = VM_BOOT_MAP[i].note;
        return true;
    }
    return false;
}

/* How many rows the map has, for the drift test: the map and the option table
 * must be the same set, and only a test can say so. */
unsigned vm_boot_options_map_count(void) { return VM_BOOT_MAP_COUNT; }

const char *vm_boot_options_map_name(unsigned index) {
    if (index >= VM_BOOT_MAP_COUNT) return NULL;
    return VM_BOOT_MAP[index].name;
}

/* The value a row carries when the caller supplied none: the table's default,
 * never false. */
static bool requested_value(const bool *values, unsigned count,
                            unsigned index) {
    if (values && index < count) return values[index];
    const vm_option_t *option = vm_option_at(index);
    return option ? option->def : false;
}

/* snprintf-style append that keeps counting past the end, so a truncated
 * summary is still a terminated prefix rather than a lie about the count. */
static void append(char *out, size_t cap, size_t *used, const char *text) {
    if (!out || !cap || !text) return;
    if (*used + 1u >= cap) { *used += strlen(text); return; }
    int written = snprintf(out + *used, cap - *used, "%s", text);
    if (written < 0) return;
    *used += (size_t)written;
    if (*used >= cap) *used = cap - 1u;
}

void vm_boot_options_apply(const bool *values, unsigned count,
                           s5l_bringup_request_t *request,
                           vm_boot_options_report_t *report) {
    if (!report) return;
    memset(report, 0, sizeof *report);

    unsigned rows = vm_option_count();
    if (rows > VM_BOOT_OPTION_MAX) rows = VM_BOOT_OPTION_MAX;
    report->count = rows;

    for (unsigned i = 0; i < rows; i++) {
        vm_boot_option_status_t *row = &report->row[i];
        unsigned char kind = MAP_FIXED_OFF;
        const char *note = NULL;

        row->requested = requested_value(values, count, i);

        if (!mapped(i, &kind, &note)) {
            /*
             * A row nobody has decided about. Reported as ignored with an
             * explicit note rather than quietly given a plausible fate: the
             * test below fails on this, and until somebody fixes the map the
             * user is told the truth, which is that the app does not know.
             */
            row->outcome = VM_BOOT_OPTION_IGNORED;
            row->effective = false;
            row->note = "This switch has no recorded effect on the app's "
                        "bring-up. Nothing reads it.";
            report->ignored++;
            if (row->effective != row->requested) report->overridden++;
            continue;
        }

        row->note = note;
        switch (kind) {
            case MAP_NO_LCD_PANEL_ID:
                row->outcome = VM_BOOT_OPTION_APPLIED;
                row->effective = row->requested;
                if (request) request->no_lcd_panel_id = !row->requested;
                report->applied++;
                break;
            case MAP_NO_MEMORY_NODE:
                row->outcome = VM_BOOT_OPTION_APPLIED;
                row->effective = row->requested;
                if (request) request->no_memory_node = !row->requested;
                report->applied++;
                break;
            case MAP_PROVISION_CA_SOFTWARE_RENDER:
                row->outcome = VM_BOOT_OPTION_PROVISIONED;
                row->effective = row->requested;
                report->provisioned++;
                break;
            case MAP_FIXED_ON:
                row->outcome = VM_BOOT_OPTION_IGNORED;
                row->effective = true;
                report->ignored++;
                break;
            case MAP_FIXED_OFF:
            default:
                row->outcome = VM_BOOT_OPTION_IGNORED;
                row->effective = false;
                report->ignored++;
                break;
        }
        if (row->effective != row->requested) report->overridden++;
    }

    /* The summary. Two clauses, both optional, because they are two different
     * problems: a switch the machine contradicts, and a switch that needs the
     * work image remade before it means anything. */
    size_t used = 0u;
    if (report->overridden > 0u) {
        char head[96];
        (void)snprintf(head, sizeof head,
                       "%u switch%s not applied as set: ",
                       report->overridden,
                       report->overridden == 1u ? " is" : "es are");
        append(report->summary, sizeof report->summary, &used, head);
        unsigned printed = 0u;
        for (unsigned i = 0; i < rows; i++) {
            if (report->row[i].effective == report->row[i].requested) continue;
            const vm_option_t *option = vm_option_at(i);
            if (printed++) append(report->summary, sizeof report->summary,
                                  &used, ", ");
            append(report->summary, sizeof report->summary, &used,
                   option && option->name ? option->name : "?");
        }
        append(report->summary, sizeof report->summary, &used, ".");
    }
    if (report->provisioned > 0u) {
        if (used > 0u)
            append(report->summary, sizeof report->summary, &used, " ");
        append(report->summary, sizeof report->summary, &used,
               "Fixed when the work image was made: ");
        unsigned printed = 0u;
        for (unsigned i = 0; i < rows; i++) {
            if (report->row[i].outcome != VM_BOOT_OPTION_PROVISIONED) continue;
            const vm_option_t *option = vm_option_at(i);
            if (printed++) append(report->summary, sizeof report->summary,
                                  &used, ", ");
            append(report->summary, sizeof report->summary, &used,
                   option && option->name ? option->name : "?");
        }
        append(report->summary, sizeof report->summary, &used, ".");
    }
    report->summary[sizeof report->summary - 1u] = '\0';
}

void vm_boot_options_for_provisioning(const bool *values, unsigned count,
                                      vm_boot_provision_options_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);

    int index = vm_option_index("ca-software-render");
    if (index < 0) return;
    out->ca_software_render = requested_value(values, count, (unsigned)index);
}
