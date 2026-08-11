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

#include "VMBootOptions.h"
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
#define VM_FW_BOOT_PATH_CAPACITY    1024u

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
/* Host metadata proving that this work image contains the stock pppd launchd
 * job. Runtime networking follows this record, not a setting changed after the
 * filesystem was made. The marker contains no credential or network state. */
#define VM_FW_BOOT_PPP_FILE         "network.ppp-v1"
#define VM_FW_BOOT_PPP_TMP          "network.ppp-v1.partial"
/*
 * The automatic suspend-to-disk pair. A complete pair is consumed once when
 * VM_FW_BOOT_RESTORE_ONCE_FILE is also present. No marker means no restore,
 * even if stale or partial state files happen to exist.
 *
 * WHY THIS IS TWO FILES AND NOT THREE. bootkernel writes a 466 MB `.mdimage`
 * sidecar beside every snapshot because it builds a FRESH work image on every
 * run, so a restore has to carry the disk with it. A machine in this app keeps
 * VM_FW_BOOT_WORK_FILE for its whole life, so the snapshot and the disk are
 * coherent by construction and only the host-side bridge state -- counters and
 * the allocation-tail overlay, about 131 KB -- has to travel.
 *
 * THE TEMP NAME IS NOT A CONVENIENCE. A snapshot is ~100 MB and iOS may
 * suspend the app part-way through writing one. A truncated file that got
 * loaded would restore a machine whose RAM disagrees with its disk, which
 * corrupts the guest filesystem quietly -- the exact failure fsck exists to
 * catch and which a resume never runs fsck to catch. So the write goes to
 * VM_FW_BOOT_STATE_TMP and is renamed only once complete.
 */
#define VM_FW_BOOT_STATE_FILE       "state.snap"
#define VM_FW_BOOT_STATE_MD_FILE    "state.snap.mdstate"
#define VM_FW_BOOT_STATE_TMP        "state.snap.partial"
#define VM_FW_BOOT_STATE_MD_TMP     "state.snap.mdstate.partial"
#define VM_FW_BOOT_RESTORE_ONCE_FILE "state.snap.restore-once"
#define VM_FW_BOOT_RESTORE_ONCE_TMP  "state.snap.restore-once.partial"

/* Physical A/B control for the signed-static engine.  This marker is read only
 * in builds which contain that engine.  It never enables executable memory;
 * it only disables the compiled-in engine so the same binary can provide an
 * interpreter control.  Ordinary machines never contain this file. */
#define VM_FW_BOOT_INTERPRETER_FILE  "engine.interpreter"
/* Same-binary performance control for the compact engine. A nonempty marker
 * keeps compact execution enabled but restores the former User-mode-only
 * admission gate, isolating privileged-prefix value from every other build,
 * checkpoint, rendering, and firmware variable. */
#define VM_FW_BOOT_COMPACT_USER_ONLY_FILE "engine.compact-user-only"
/* Same-binary performance control for resident code-window continuation. A
 * nonempty marker keeps compact execution enabled but makes every 1 KiB
 * crossing use the former interpreter-first path. It is diagnostic host
 * policy, never guest state or a stock-device dependency. */
#define VM_FW_BOOT_COMPACT_WINDOW_REFILL_OFF_FILE \
    "engine.compact-window-refill-off"
/* Opt-in same-binary experiment for switching among repeated, already-proved
 * full User-mode FETCH windows inside signed text. It is invocation-local and
 * never snapshot state. A three-pair physical-A9 Settings replay cut C fast
 * refills by about 42%, but made the same 160 M-instruction interval about 9.5%
 * slower on average and worsened the median changed-frame gap. It therefore
 * remains diagnostic-only and off by default. */
#define VM_FW_BOOT_COMPACT_WINDOW_CACHE_FILE \
    "engine.compact-window-cache-on"
/* Opt-in rollout control for privileged window continuation. The code remains
 * available because it substantially reduces engine work, but it is not the
 * stock product default after a three-pair physical-A9 Settings replay showed
 * worse displayed cadence. User-mode continuation remains enabled without
 * this marker. */
#define VM_FW_BOOT_COMPACT_PRIVILEGED_WINDOW_REFILL_FILE \
    "engine.compact-privileged-window-refill-on"
/* Explicit statistical diagnosis of where compact-runner CPU time lands.
 * The ordinary marker-free product installs no signal handler or host timer.
 * A nonempty marker is accepted only by the Apple-AArch64 compact engine and
 * reports broad signed-text regions; it never changes guest semantics. */
#define VM_FW_BOOT_COMPACT_PC_PROFILE_FILE \
    "engine.compact-pc-profile-on"
/* Same-binary timing control for an exact navigation replay. A nonempty
 * marker leaves real-time WFI pacing enabled but does not install the active
 * host clock, so the guest advances active time from retired instructions as
 * it did before that optional product policy. Ordinary machines never contain
 * this file; it is diagnostic host policy, not guest state. */
#define VM_FW_BOOT_ACTIVE_CLOCK_OFF_FILE "clock.active-host-off"

/* How much free space rootfs_work_create() adds to the volume. The stock
 * rootfs ships with ZERO free blocks, so without this launchd can create
 * nothing and the boot stops early. 32 MB is the harness's long-standing
 * value and is what run89-base's work image was built with. */
#define VM_FW_BOOT_GROWTH_BYTES  (32u * 1024u * 1024u)

/* What rootfs_work_activation_entries() returns: the Lockdown directory and
 * the data_ark.plist inside it. Asserted against the library at provision
 * time rather than trusted, because a short array silently fills nothing. */
#define VM_FW_BOOT_ACTIVATION_ENTRIES 2u
/* The PPP options, resolver fallback, and SystemConfiguration service are an
 * all-or-nothing image-time payload. Keep this asserted against the helper at
 * runtime so adding a fourth file cannot silently truncate phone images. */
#define VM_FW_BOOT_PPP_ENTRIES 3u
#define VM_FW_BOOT_PROVISION_ENTRIES \
    (VM_FW_BOOT_ACTIVATION_ENTRIES + VM_FW_BOOT_PPP_ENTRIES)

/*
 * TWO DIRECTORIES, NOT ONE, and the split is the difference between two
 * machines and two names for one.
 *
 * `firmware` holds the importer's three artefacts. They are read-only for the
 * whole life of the app, they are the expensive thing a user obtained (an IPSW
 * unpacked into ~450 MB), and there is exactly one useful copy of them: every
 * machine reads the same three files and none of them can damage the others.
 *
 * `work` holds ONE machine's writable root filesystem. This is the file the
 * guest mounts read-write and scribbles all over, so it is the file that must
 * not be shared -- two machines pointed at one work image are one machine with
 * two names, and worse, two machines that can corrupt each other's disk.
 *
 * Both may be the same directory, which is the layout the app had before
 * instances existed; vm_firmware_boot_paths_shared() builds exactly that.
 */
typedef struct {
    char firmware[VM_FW_BOOT_PATH_CAPACITY];
    char work[VM_FW_BOOT_PATH_CAPACITY];
} vm_firmware_boot_paths_t;

/* Both halves in one directory. False, with `out` zeroed, if it does not fit. */
bool vm_firmware_boot_paths_shared(vm_firmware_boot_paths_t *out,
                                   const char *directory);

/* Read-only artefacts in one directory, this machine's work image in another.
 * False, with `out` zeroed, if either does not fit. */
bool vm_firmware_boot_paths_split(vm_firmware_boot_paths_t *out,
                                  const char *firmware_directory,
                                  const char *work_directory);

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
 * Look at `paths` and report what is there. Opens nothing for writing and
 * reads no file contents, so it is cheap enough to call before every start.
 * A zero-length file counts as ABSENT: the settings screen's existence check
 * would otherwise call a failed import "present". A NULL `paths`, or either
 * directory empty, is INCOMPLETE with a reason rather than a crash.
 */
void vm_firmware_boot_probe(const vm_firmware_boot_paths_t *paths,
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
    /*
     * What became of every settings switch. Filled in whenever the request was
     * built at all -- that is, on every outcome after the files were found --
     * so a caller can say which switches the machine contradicted even on a
     * run that succeeded. See VMBootOptions.h.
     */
    vm_boot_options_report_t options;
} vm_firmware_boot_report_t;

/*
 * Opaque owner of the open work image and the ~300 KiB of bridge storage the
 * memory-disk bridges need. It must stay alive for exactly as long as the
 * machine does, and be destroyed AFTER s5l8900_free().
 */
typedef struct vm_firmware_boot vm_firmware_boot_t;

vm_firmware_boot_t *vm_firmware_boot_create(void);

/*
 * Bring `machine` up on the firmware `paths` names.
 *
 * `machine` must already be s5l8900_init()'d at S5L_BRINGUP_PHYS_BASE with
 * S5L_BRINGUP_RAM_SIZE bytes and must not be running. On success the CPU is
 * pointed at Apple's kernel and `report->summary` says so. On failure the
 * machine has been partially written and the caller must free and rebuild it
 * before falling back to the demo guest.
 *
 * `options` is the settings screen's values in option-table order; NULL means
 * every row at its table default. They are resolved HERE rather than by the
 * caller so that the mapping, and the record of which switches the machine
 * contradicted, is the same code on the phone and under the test suite.
 *
 * Reads the kernel (about 8 MB) and the device tree into memory for the
 * duration of the call only; the root filesystem is never read into memory.
 */
bool vm_firmware_boot_start(vm_firmware_boot_t *boot,
                            s5l8900_t *machine,
                            const vm_firmware_boot_paths_t *paths,
                            const bool *options, unsigned option_count,
                            vm_firmware_boot_report_t *report);

/*
 * Save the one automatic resume point while the emulator thread exclusively
 * owns `machine` between instructions.
 *
 * BUSY means a native memory-disk continuation is in flight. Nothing was
 * written and the caller should execute a little more guest code before
 * retrying. ERROR is terminal for this attempt and `detail` says why. OK means
 * the complete checkpoint and its one-shot marker are durable, after which the
 * current machine must stop before it can change its disk again.
 */
typedef enum {
    VM_FW_CHECKPOINT_OK = 0,
    VM_FW_CHECKPOINT_BUSY,
    VM_FW_CHECKPOINT_ERROR
} vm_firmware_checkpoint_status_t;

vm_firmware_checkpoint_status_t vm_firmware_boot_save_resume(
    vm_firmware_boot_t *boot, const s5l8900_t *machine,
    char *detail, size_t detail_capacity);

/* Close the work image and release the bridge storage. Safe on a NULL slot. */
/*
 * Arm copy-on-write recording into `overlay_path`, between _create() and
 * _start(). Pass NULL or "" to disarm.
 *
 * Armed only when a snapshot exists to protect: a machine with no saved
 * states pays nothing at all, because no block is read before it is written
 * and the bringup gets the file adapter's own descriptor.
 *
 * If arming fails, _start() FAILS. Running on unrecorded writes while the
 * snapshots still claim to be restorable would be discovered by the user at
 * the exact moment that claim mattered.
 *
 * False if the path does not fit; the previous setting is then unchanged.
 */
/*
 * The RAW media descriptor, bypassing any overlay.
 *
 * Replay writes historical contents back over the image. Routing that through
 * the overlay would record every block it restores as if the guest had just
 * changed it -- the restore would append its own undo to the snapshot it is
 * restoring from.
 */
const vm_block_t *vm_firmware_boot_media(const vm_firmware_boot_t *boot);

/*
 * Point recording at a different overlay: after taking a snapshot, so writes
 * accrue to the new one, and after restoring, so they accrue to the one just
 * restored. Closes the current adapter first -- two over one file would each
 * keep a private bitmap, and the second would append a newer, wrong copy of a
 * block the first already held, which replay applies last.
 *
 * NULL or "" disarms.
 */
bool vm_firmware_boot_rearm_overlay(vm_firmware_boot_t *boot,
                                    const char *overlay_path,
                                    char *detail, size_t capacity);

/* Overlay then image, in that order. See VMSnapshotCow.h.
 *
 * These report bool rather than the overlay layer's own status: this header
 * is included BY VMSnapshotStore.h, which VMSnapshotCow.h includes in turn,
 * so naming that type here is a cycle. It is also the wrong layering -- the
 * boot object reports whether IT succeeded, and puts the underlying reason
 * in `detail`. */
bool vm_firmware_boot_flush_overlay(vm_firmware_boot_t *boot);

bool vm_firmware_boot_arm_overlay(vm_firmware_boot_t *boot,
                                  const char *overlay_path);

void vm_firmware_boot_destroy(vm_firmware_boot_t **boot);

/*
 * Make this machine's writable work image from the shared imported rootfs.img:
 * reads paths->firmware/rootfs.img, writes paths->work/rootfs-work.img.
 *
 * SLOW: it copies ~433 MB and then grows the volume. Call it on a background
 * thread, never from a UI callback or from anything holding the engine lock.
 * Refuses rather than replaces if the destination already exists, so calling
 * it twice is safe. `detail` always receives a reason on failure.
 *
 * `options` selects the work-image transformations: QuartzCore's renderer,
 * offline activation data, and the stock pppd launchd job. NULL is the
 * table's defaults.
 * The directory paths->work names must already exist; nothing here creates
 * directories, because C has no portable way to.
 */
/*
 * `progress` is optional and may be NULL. It is called during the ~433 MB copy
 * with the bytes done and the total, on THIS thread -- which is a background
 * thread, never the UI one, so an implementation that touches UIKit has to
 * hop to the main queue itself. It cannot cancel: this is a report, so a
 * progress bar can never leave a half-built image behind.
 */
bool vm_firmware_boot_provision(const vm_firmware_boot_paths_t *paths,
                                const bool *options, unsigned option_count,
                                void (*progress)(void *ctx, uint64_t done,
                                                 uint64_t total),
                                void *progress_ctx,
                                char *detail, size_t detail_capacity);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMFIRMWAREBOOT_H */
