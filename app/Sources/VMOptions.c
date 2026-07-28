/*
 * S5LBox — the settings screen's option table. See VMOptions.h.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMOptions.h"

#include <string.h>

/* The reasons below are compressed from bootkernel's own help text. They are
 * kept to one sentence each because a table row is not a manual page, and they
 * name the observed failure rather than a policy: "hangs the boot" is checkable
 * against docs/BOOTLOG.md, "recommended" is not. */
static const vm_option_t VM_OPTIONS[] = {
    { "mbx", "MBX GPU  ·  /arm-io/mbx",
      "Off: the PowerVR driver busy-polls a reset bit in a register block this "
      "VM does not model, and the boot hangs there.",
      false, VM_OPT_GROUP_HARDWARE, VM_OPT_IMPL_HARNESS },
    { "sha1", "SHA-1 engine  ·  /arm-io/sha1",
      "Off: matched, every 4096-byte cs_validate_page digest goes to an "
      "unmodelled register file and launchd's first text page fails signing.",
      false, VM_OPT_GROUP_HARDWARE, VM_OPT_IMPL_HARNESS },
    { "baseband", "Baseband  ·  /baseband",
      "Off: a declared-but-silent modem is worse than an absent one -- "
      "CommCenter retries forever and SpringBoard blocks behind its queue.",
      false, VM_OPT_GROUP_HARDWARE, VM_OPT_IMPL_HARNESS },
    { "spi2", "SPI2  ·  /arm-io/spi2",
      "Off: the transport underneath the baseband, split out so either half of "
      "that failure can be reproduced on its own.",
      false, VM_OPT_GROUP_HARDWARE, VM_OPT_IMPL_HARNESS },
    { "usb-otg", "USB OTG  ·  /arm-io/usb-otg",
      "Off: AppleSynopsysOTGDevice reads unmodelled configuration registers, "
      "derives a self-inconsistent endpoint count and panics.",
      false, VM_OPT_GROUP_HARDWARE, VM_OPT_IMPL_HARNESS },

    { "vram", "Publish /vram:reg",
      "On: this is the fix that made the guest render. Without it SpringBoard's "
      "compositor gets a read-only framebuffer and faults on its first store.",
      true, VM_OPT_GROUP_PATCH, VM_OPT_IMPL_HARNESS },
    { "lcd-panel-id", "LCD panel ID",
      "On: writes the N82 Syrah panel ID that iBoot would have read off the "
      "panel; Merlot rejects the zero the shipped tree carries.",
      true, VM_OPT_GROUP_PATCH, VM_OPT_IMPL_HARNESS },
    { "memory-reg", "Synthesise /memory:reg",
      "On: the shipped cell is zero, which would advertise a DRAM bank of no "
      "size at all.",
      true, VM_OPT_GROUP_PATCH, VM_OPT_IMPL_HARNESS },
    { "rtc-patch", "RTC wait patch",
      "On: one byte, IORTC waitForService 30 s -> 0, so a PMU publication "
      "failure stays visible instead of costing a 30-second stall.",
      true, VM_OPT_GROUP_PATCH, VM_OPT_IMPL_HARNESS },
    { "ca-software-render", "QuartzCore software renderer",
      "Off: sets CA_ENABLE_MBX2D=0 for SpringBoard, which is how QuartzCore "
      "picks its own software path. Needs a writable work image.",
      false, VM_OPT_GROUP_PATCH, VM_OPT_IMPL_HARNESS },

    { "activate", "Activation",
      "On, and NOT IMPLEMENTED ANYWHERE: writing ActivationState needs a file "
      "the source root filesystem does not contain and nothing can yet create.",
      true, VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_NOWHERE },
    { "jb-codesign", "Jailbreak: kernel half",
      "Off, and NOT IMPLEMENTED ANYWHERE: would disable the guest kernel's own "
      "code-signature enforcement. No exploit is involved; we load the kernel.",
      false, VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_NOWHERE },
    { "jb-payload", "Jailbreak: filesystem half",
      "Off, and NOT IMPLEMENTED ANYWHERE: would install the payload named below "
      "onto the work image, and waits on the same file provisioner.",
      false, VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_NOWHERE },
    { "ppp", "Guest networking (PPP over uart4)",
      "Off, and explicitly temporary: runs the guest's own pppd over an "
      "emulated UART until real drivers exist. Needs a writable work image.",
      false, VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_HARNESS },
    { "nat", "Route guest traffic to the internet",
      "On, but it does nothing without PPP, which is what carries the "
      "datagrams. Terminates ICMP echo, resolves names through the host, and "
      "turns guest TCP and UDP into ordinary unprivileged sockets.",
      true, VM_OPT_GROUP_GUEST_STATE, VM_OPT_IMPL_HARNESS }
};

#define VM_OPTION_COUNT ((unsigned)(sizeof VM_OPTIONS / sizeof VM_OPTIONS[0]))

static const char *const VM_OPTION_GROUP_TITLE[VM_OPT_GROUP_COUNT] = {
    "Guest hardware",
    "Compatibility patches",
    "Guest state"
};

static const char *const VM_OPTION_GROUP_NOTE[VM_OPT_GROUP_COUNT] = {
    "Device-tree nubs this VM hides from the guest by default. Each one is "
    "hidden because leaving it matched is known to hang or panic a real boot, "
    "so the guest is told it has less hardware than a real iPhone.",

    "Work iBoot would have done, done by the emulator instead because it jumps "
    "straight to the kernel. Each patch has its own switch so a boot can be "
    "bisected against it.",

    "Persistent changes to the guest or its work image. Activation and both "
    "jailbreak halves exist in no part of this project: the desktop harness "
    "prints \"requested but NOT APPLIED\" for exactly those rows, and so does "
    "this screen. Guest networking exists on the desktop only."
};

/*
 * The toggles this app deliberately does not offer.
 *
 * Together with VM_OPTIONS this must account for EVERY row of bootkernel's
 * BOOT_TOGGLES, exactly once each -- check_option_mirror.cmake runs the real
 * binary's --print-config and enforces the partition. A toggle that is in
 * neither table is the bug this exists to catch; a toggle in both is a
 * contradiction and fails just as loudly.
 */
static const vm_option_omission_t VM_OMITTED[] = {
    { "framebuffer",
      "the app IS a framebuffer viewer, so turning the display off would leave "
      "it with nothing to show" },
    { "iomfb-display",
      "meaningful only alongside --framebuffer, which this app does not offer" },
    { "fstab-fixup",
      "turning it off halts the boot at fsck by design, which is a bisection "
      "step rather than a setting" },
    { "ramdisk-low",
      "a desktop memory-layout experiment; the app's guest RAM is fixed at the "
      "128 MB the hardware shipped with" },
    { "stop-on-abort",
      "a debugger behaviour for a terminal, with no console here to stop into" },
    { "kext-map",
      "prints a load-address table to stdout, which this app has no reader "
      "for" },
    { "print-config",
      "resolves the command line and exits without booting; the settings "
      "screen already shows the resolved configuration" },
    { "call-probe-regs",
      "formats the terminal report for --call-probe, and this app offers no "
      "way to arm a probe in the first place" },
    { "call-probe-live",
      "streams --call-probe captures to stdout as they happen, for the same "
      "absent probe and the same absent terminal" },
    { "uart4-rx-irq",
      "a control for one bisection of the uart4 receive path, and it only "
      "means anything alongside --ppp, which this app does not offer" },
};

#define VM_OMITTED_COUNT \
    ((unsigned)(sizeof VM_OMITTED / sizeof VM_OMITTED[0]))

unsigned vm_option_count(void) {
    return VM_OPTION_COUNT;
}

unsigned vm_option_omitted_count(void) {
    return VM_OMITTED_COUNT;
}

const vm_option_omission_t *vm_option_omitted_at(unsigned index) {
    if (index >= VM_OMITTED_COUNT) return NULL;
    return &VM_OMITTED[index];
}

const vm_option_t *vm_option_at(unsigned index) {
    if (index >= VM_OPTION_COUNT) return NULL;
    return &VM_OPTIONS[index];
}

int vm_option_index(const char *name) {
    if (!name) return -1;
    for (unsigned i = 0; i < VM_OPTION_COUNT; i++)
        if (!strcmp(name, VM_OPTIONS[i].name)) return (int)i;
    return -1;
}

const char *vm_option_group_title(unsigned group) {
    if (group >= (unsigned)VM_OPT_GROUP_COUNT) return NULL;
    return VM_OPTION_GROUP_TITLE[group];
}

const char *vm_option_group_note(unsigned group) {
    if (group >= (unsigned)VM_OPT_GROUP_COUNT) return NULL;
    return VM_OPTION_GROUP_NOTE[group];
}

/* Append with snprintf's contract: always count, write only what fits, always
 * terminate. Keeping the counting and the copying in one place is what lets
 * the dry run (cap == 0) and the real run agree by construction. */
static void vm_option_append(const char *text, char *out, size_t cap,
                             size_t *written) {
    const size_t n = strlen(text);
    if (out && cap > 0 && *written < cap - 1) {
        size_t room = cap - 1 - *written;
        size_t copy = (n < room) ? n : room;
        memcpy(out + *written, text, copy);
        out[*written + copy] = '\0';
    }
    *written += n;
}

size_t vm_option_command_line(const bool *values, unsigned count,
                              char *out, size_t cap) {
    size_t written = 0;

    if (out && cap > 0) out[0] = '\0';
    if (!values) return 0;
    if (count > VM_OPTION_COUNT) count = VM_OPTION_COUNT;

    for (unsigned i = 0; i < count; i++) {
        if (values[i] == VM_OPTIONS[i].def) continue;
        if (written > 0) vm_option_append(" ", out, cap, &written);
        vm_option_append(values[i] ? "--" : "--no-", out, cap, &written);
        vm_option_append(VM_OPTIONS[i].name, out, cap, &written);
    }
    return written;
}
