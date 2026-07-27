/*
 * S5LBox — the app's firmware boot path, in plain C.
 *
 * Everything between "there are files in a directory" and "the machine is
 * pointed at Apple's kernel" lives here rather than in VMEngine.m, for the
 * reason VMTouchMap.c, VMOptions.c and VMFirmwareImport.c exist: Objective-C
 * cannot be compiled or run by a host CI runner, and this is exactly the kind
 * of code — path handling, sizes, an ordering of fallible steps — that has to
 * be testable somewhere other than a phone.
 *
 * The engine's whole involvement is: probe, and if the answer is READY, call
 * one function. Everything it needs to tell the user comes back in a report.
 *
 * NOTHING HERE PRETENDS. Every path that cannot produce a running kernel says
 * which file was missing or which step refused, and the app shows that string.
 * There is no code path that reports a boot that did not happen.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMFIRMWAREBOOT_H
#define S5LBOX_APP_VMFIRMWAREBOOT_H

#include "bringup.h"
#include "soc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VM_FW_BOOT_DETAIL_CAPACITY  256u
#define VM_FW_BOOT_SUMMARY_CAPACITY 128u

/*
 * The importer's three artefacts, plus the one file this layer makes itself.
 *
 * The work image is a WRITABLE COPY of rootfs.img. It has to exist because the
 * guest mounts its root filesystem read-write and because the stock
 * /private/etc/fstab names /dev/disk0s1, a NAND-backed device this machine
 * does not have; booting the pristine import would either corrupt the only
 * copy of it or halt in launchd's fsck. tools/rootfs_work.c makes the copy and
 * applies exactly that fstab line.
 */
#define VM_FW_BOOT_KERNEL_FILE      "kernel.macho"
#define VM_FW_BOOT_DEVICETREE_FILE  "devicetree.bin"
#define VM_FW_BOOT_ROOTFS_FILE      "rootfs.img"
#define VM_FW_BOOT_WORK_FILE        "rootfs-work.img"

/* How much free space rootfs_work_create() adds to the volume. The stock
 * rootfs ships with ZERO free blocks, so without this launchd can create
 * nothing and the boot stops early. 32 MB is the harness's long-standing
 * value and is what run89-base's work image was built with. */
#define VM_FW_BOOT_GROWTH_BYTES  (32u * 1024u * 1024u)

typedef enum {
    /* All four files are present: a real kernel boot is possible right now. */
    VM_FW_BOOT_READY = 0,
    /* The three imported artefacts are here but the writable work image has
     * not been made yet. One call to vm_firmware_boot_provision() fixes it,
     * and that call copies ~450 MB, so it must not run on a UI thread. */
    VM_FW_BOOT_NEEDS_WORK_IMAGE,
    /* At least one imported artefact is missing or empty. */
    VM_FW_BOOT_INCOMPLETE
} vm_firmware_boot_readiness_t;

typedef struct {
    vm_firmware_boot_readiness_t readiness;
    bool     kernel_present;
    bool     devicetree_present;
    bool     rootfs_present;
    bool     work_present;
    uint64_t kernel_size;
    uint64_t devicetree_size;
    uint64_t rootfs_size;
    uint64_t work_size;
    /* One line naming what is missing, for the UI. Empty when READY. */
    char     detail[VM_FW_BOOT_DETAIL_CAPACITY];
} vm_firmware_boot_state_t;

/*
 * Look at `directory` and report what is there. Opens nothing for writing and
 * reads no file contents, so it is cheap enough to call before every start.
 * A zero-length file counts as ABSENT: the settings screen's existence check
 * would otherwise call a failed import "present".
 */
void vm_firmware_boot_probe(const char *directory,
                            vm_firmware_boot_state_t *out);

typedef struct {
    bool ok;
    /* Short mode line for the status bar and the machine list, always set:
     * either what is running or why it is not. */
    char summary[VM_FW_BOOT_SUMMARY_CAPACITY];
    /* The failure reason. Empty on success. */
    char detail[VM_FW_BOOT_DETAIL_CAPACITY];
    /* The layout that was planned, whether or not it was reached. */
    s5l_bringup_result_t bringup;
} vm_firmware_boot_report_t;

/*
 * Opaque owner of the open work image and the ~300 KiB of bridge storage the
 * memory-disk bridges need. It must stay alive for exactly as long as the
 * machine does, and be destroyed AFTER s5l8900_free().
 */
typedef struct vm_firmware_boot vm_firmware_boot_t;

vm_firmware_boot_t *vm_firmware_boot_create(void);

/*
 * Bring `machine` up on the firmware in `directory`.
 *
 * `machine` must already be s5l8900_init()'d at S5L_BRINGUP_PHYS_BASE with
 * S5L_BRINGUP_RAM_SIZE bytes and must not be running. On success the CPU is
 * pointed at Apple's kernel and `report->summary` says so. On failure the
 * machine has been partially written and the caller must free and rebuild it
 * before falling back to the demo guest.
 *
 * Reads the kernel (about 8 MB) and the device tree into memory for the
 * duration of the call only; the root filesystem is never read into memory.
 */
bool vm_firmware_boot_start(vm_firmware_boot_t *boot,
                            s5l8900_t *machine,
                            const char *directory,
                            vm_firmware_boot_report_t *report);

/* Close the work image and release the bridge storage. Safe on a NULL slot. */
void vm_firmware_boot_destroy(vm_firmware_boot_t **boot);

/*
 * Make the writable work image from the imported rootfs.img.
 *
 * SLOW: it copies ~433 MB and then grows the volume. Call it on a background
 * thread, never from a UI callback or from anything holding the engine lock.
 * Refuses rather than replaces if the destination already exists, so calling
 * it twice is safe. `detail` always receives a reason on failure.
 */
bool vm_firmware_boot_provision(const char *directory,
                                char *detail, size_t detail_capacity);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMFIRMWAREBOOT_H */
