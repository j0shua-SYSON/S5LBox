/*
 * S5LBox — which files belong to which machine.
 *
 * THE STORAGE LAYOUT, AND WHY IT IS THIS ONE.
 *
 *   Documents/firmware/kernel.macho          shared, read-only
 *   Documents/firmware/devicetree.bin        shared, read-only
 *   Documents/firmware/rootfs.img            shared, read-only
 *   .../Machines/<id>/rootfs-work.img        one machine's writable disk
 *
 * The three imported artefacts stay exactly where VMFirmwareImporter already
 * writes them and are SHARED. They are ~450 MB the user obtained once from an
 * IPSW they already had; nothing ever writes to them after the import, so one
 * copy is not a compromise, it is the correct number. Copying them per machine
 * would multiply the largest thing on the device by the number of machines to
 * buy nothing at all.
 *
 * The work image is the opposite case and gets the opposite answer. It is the
 * disk the guest mounts read-WRITE: it is where the guest's changes live, and
 * it is the entire reason two machines are two machines rather than two names
 * for one. Sharing it means the second machine boots into the first machine's
 * filesystem and each can corrupt the other. So it is per-machine, named after
 * the instance id, and it costs what it costs — 466,825,216 bytes (445 MiB,
 * the figure README.md records for the harness's own image) per machine that
 * has actually been opened, because it is created lazily on first boot and a
 * machine you never open never pays.
 *
 * WHAT HAPPENS TO AN EXISTING INSTALLATION. Before instances, the work image
 * was Documents/firmware/rootfs-work.img and there was one. That file is the
 * user's guest disk with whatever they did in it, and it is not thrown away
 * and not copied: the first machine that needs a work image and finds none of
 * its own ADOPTS it, by renaming it into that machine's directory. A rename is
 * instant and costs no disk, and once it has happened the shared path is empty,
 * so exactly one machine can ever adopt. Every later machine provisions a
 * fresh image from the pristine rootfs.img, which is what a new machine should
 * get anyway. If the rename fails the legacy image is left untouched and the
 * failure names the path it is still at, because "we could not move your
 * 450 MB disk" must never come out as silence.
 *
 * WHY PLAIN C11. An identifier from the machine list becomes a PATH COMPONENT
 * here. Deriving that in Objective-C would put the one place a bad id turns
 * into a wrong directory outside every test this project can run without a
 * phone. Every path in this file goes through vm_instance_id_check(), which
 * admits exactly sixteen lower-case hex digits and therefore admits no
 * separator, no dot, and no "..".
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMINSTANCEPATHS_H
#define S5LBOX_APP_VMINSTANCEPATHS_H

#include "VMFirmwareBoot.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VM_INSTANCE_PATHS_OK = 0,
    VM_INSTANCE_PATHS_ERR_NULL,      /* a required argument was missing      */
    VM_INSTANCE_PATHS_ERR_ID,        /* not 16 lower-case hex digits         */
    VM_INSTANCE_PATHS_ERR_TOO_LONG   /* a joined path would not fit          */
} vm_instance_paths_status_t;

/* One line, lower case, no full stop — for a log or an alert body. */
const char *vm_instance_paths_status_text(vm_instance_paths_status_t status);

typedef struct {
    /* The shared, read-only artefacts. */
    char firmware[VM_FW_BOOT_PATH_CAPACITY];
    /* This machine's own directory: its work image and, later, its snapshots.
     * The CALLER must have created it; C has no portable mkdir. */
    char machine[VM_FW_BOOT_PATH_CAPACITY];
    /* machine/rootfs-work.img */
    char work_image[VM_FW_BOOT_PATH_CAPACITY];
    /* firmware/rootfs-work.img — the pre-instance location, which exists only
     * on an installation that ran an older build. */
    char legacy_work_image[VM_FW_BOOT_PATH_CAPACITY];
} vm_instance_paths_t;

/*
 * Derive every path one machine uses. `out` is zeroed first and left zeroed on
 * any refusal, so a caller that ignores the status cannot end up with a
 * half-built path that happens to look usable.
 */
vm_instance_paths_status_t
vm_instance_paths_derive(const char *firmware_directory,
                         const char *machines_directory,
                         const char *instance_id,
                         vm_instance_paths_t *out);

/* The two directories vm_firmware_boot_* wants: the shared firmware directory
 * and this machine's own. */
void vm_instance_paths_to_boot(const vm_instance_paths_t *paths,
                               vm_firmware_boot_paths_t *out);

/*
 * WHAT TO DO ABOUT THIS MACHINE'S WORK IMAGE, decided by looking at the disk.
 *
 * Ordered by cost, cheapest first, and that ordering is the whole design: a
 * machine that already has its own image does nothing, a machine that can
 * adopt the pre-instance one does a rename, and only a machine with neither
 * pays for a 450 MB copy.
 */
typedef enum {
    /* This machine already has one. Boot. */
    VM_INSTANCE_WORK_PRIVATE = 0,
    /* No image of its own, but the pre-instance shared one is still there.
     * vm_instance_work_adopt() moves it in. */
    VM_INSTANCE_WORK_ADOPT,
    /* Neither. vm_firmware_boot_provision() must make one, which is slow. */
    VM_INSTANCE_WORK_PROVISION
} vm_instance_work_plan_t;

/* Looks at the filesystem; opens nothing for writing. A zero-length file
 * counts as absent, the same rule vm_firmware_boot_probe() uses, because an
 * interrupted copy leaves exactly that and adopting it would hand a machine an
 * empty disk. */
vm_instance_work_plan_t vm_instance_work_plan(const vm_instance_paths_t *paths);

/*
 * Move the pre-instance shared work image into this machine's directory.
 *
 * Returns whether the file is now this machine's. On failure NOTHING has been
 * moved -- rename() either happens or does not -- and `detail` names the path
 * the image is still at, so a user can be told where their disk went rather
 * than being told it is gone. `detail` is set on success too, because "your
 * old machine's disk is now this machine's disk" is a thing a user is entitled
 * to be told once.
 *
 * Refuses when the plan is not VM_INSTANCE_WORK_ADOPT, so calling it
 * unconditionally cannot overwrite an image this machine already has.
 */
bool vm_instance_work_adopt(const vm_instance_paths_t *paths,
                            char *detail, size_t detail_capacity);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMINSTANCEPATHS_H */
