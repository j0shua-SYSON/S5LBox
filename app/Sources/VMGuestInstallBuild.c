/* See VMGuestInstallBuild.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMGuestInstallBuild.h"

#include "VMResumeCheckpoint.h"
#include "VMSnapshotStore.h"
#include "bringup.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(S5LBOX_GUEST_INSTALL_TESTING)
/* The policy test needs the real snapshot format and PMU state, not 128 MiB
 * of otherwise irrelevant zero RAM on every CI platform. Product builds never
 * define this test flag and always verify the hardware's exact geometry. */
#define BUILD_CHECKPOINT_RAM_BASE S5L_BRINGUP_PHYS_BASE
#define BUILD_CHECKPOINT_RAM_SIZE UINT32_C(0x00100000)
#else
#define BUILD_CHECKPOINT_RAM_BASE S5L_BRINGUP_PHYS_BASE
#define BUILD_CHECKPOINT_RAM_SIZE S5L_BRINGUP_RAM_SIZE
#endif

typedef struct {
    vm_guest_install_build_progress_t callback;
    void *context;
} build_progress_adapter_t;

/* Extracted from the exact pinned cydia_1.0.3044-66 package, then matched on
 * the retained physical guest that exposed the historical 0755 install. This
 * is the executable data-fork identity, not the .deb archive identity. */
static const uint8_t VM_CYDIA_EXECUTABLE_SHA256[
    IOS3_SHA256_DIGEST_SIZE] = {
    0x4cu, 0xa3u, 0xf7u, 0x0fu, 0xe5u, 0xcbu, 0x67u, 0x73u,
    0x76u, 0x88u, 0xabu, 0x06u, 0x14u, 0xc6u, 0x86u, 0xfeu,
    0x18u, 0xb1u, 0x24u, 0x84u, 0x43u, 0x69u, 0x84u, 0x8bu,
    0x76u, 0x84u, 0x6fu, 0x80u, 0xa5u, 0x2fu, 0x63u, 0x24u
};

static void build_cydia_privilege_repair(
    rootfs_work_file_repair_t *repair) {
    memset(repair, 0, sizeof *repair);
    repair->path = "/Applications/Cydia.app/Cydia_";
    repair->expected_size = UINT64_C(320704);
    memcpy(repair->expected_sha256, VM_CYDIA_EXECUTABLE_SHA256,
           sizeof repair->expected_sha256);
    repair->expected_owner_id = 0u;
    repair->expected_group_id = 0u;
    repair->expected_permissions = 0755u;
    repair->desired_owner_id = 0u;
    repair->desired_group_id = 0u;
    repair->desired_permissions = 06755u;
}

static const uint8_t VM_BIGBOSS_SOURCE[] =
    VM_GUEST_ROOTFS_BIGBOSS_SOURCE_LINE;
/* A short-lived compatibility build wrote this equivalent spelling before
 * packet-loss evidence disproved the URL rewrite as a remedy. It expands to
 * the same index URLs in old APT. Accept it on existing test/install media so
 * cache recovery does not needlessly refuse the disk, but never create it. */
static const uint8_t VM_BIGBOSS_SOURCE_COMPAT[] =
    "deb http://apt.thebigboss.org/repofiles/cydia/ "
    "dists/stable/main/binary-iphoneos-arm/\n";

static bool build_bigboss_source_probe_for(
    rootfs_work_file_repair_t *probe, const uint8_t *content,
    size_t content_size) {
    if (!probe || !content || !content_size) return false;
    memset(probe, 0, sizeof *probe);
    probe->path = VM_GUEST_ROOTFS_BIGBOSS_SOURCE_PATH;
    probe->expected_size = content_size;
    if (!ios3_sha256(content, content_size, probe->expected_sha256))
        return false;
    probe->expected_permissions = 0644u;
    probe->desired_permissions = 0644u;
    return true;
}

static bool build_bigboss_source_probe(
    rootfs_work_file_repair_t *probe) {
    return build_bigboss_source_probe_for(
        probe, VM_BIGBOSS_SOURCE, sizeof VM_BIGBOSS_SOURCE - 1u);
}

static const uint8_t VM_BIGBOSS_CACHE_CLEANUP[] =
    "#!/bin/sh\n"
    "PATH=/usr/bin:/bin:/usr/sbin:/sbin\n"
    "export PATH\n"
    "state=/private/var/lib/s5lbox\n"
    "marker=$state/cydia-index-cache-v2.complete\n"
    "[ -e \"$marker\" ] && exit 0\n"
    "log=/private/var/log/s5lbox-cydia-index-cache-v2.log\n"
    "exec >>\"$log\" 2>&1\n"
    "for cache in /private/var/lib/apt/lists/apt.thebigboss.org_repofiles_cydia_dists_stable_* /private/var/lib/apt/lists/partial/apt.thebigboss.org_repofiles_cydia_dists_stable_*; do\n"
    "    [ -e \"$cache\" ] || continue\n"
    "    /bin/rm -f \"$cache\" || exit 1\n"
    "done\n"
    "/bin/rm -f /private/var/cache/apt/pkgcache.bin /private/var/cache/apt/srcpkgcache.bin || exit 1\n"
    ": >\"$marker.partial\" || exit 1\n"
    "/bin/sync\n"
    "/bin/mv -f \"$marker.partial\" \"$marker\" || exit 1\n"
    "/bin/sync\n"
    "echo 'cached BigBoss package indexes removed for a clean retry'\n"
    "exit 0\n";

#define VM_BIGBOSS_CACHE_CLEANUP_PATH \
    "/private/var/lib/s5lbox/cydia-index-cache-v2"

static const uint8_t VM_BIGBOSS_CACHE_CLEANUP_PLIST[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>Label</key>\n"
    "  <string>com.j0shua.s5lbox.cydia-index-cache-v2</string>\n"
    "  <key>ProgramArguments</key>\n"
    "  <array>\n"
    "    <string>" VM_BIGBOSS_CACHE_CLEANUP_PATH "</string>\n"
    "  </array>\n"
    "  <key>RunAtLoad</key>\n"
    "  <true/>\n"
    "</dict>\n"
    "</plist>\n";

enum { VM_BIGBOSS_MIGRATION_MAX_ENTRIES = 8u };

static size_t build_bigboss_source_entries(
    rootfs_work_entry_t *entries, size_t capacity, bool create_source) {
    static const char *directories[] = {
        "/private/etc/apt",
        "/private/etc/apt/sources.list.d",
        "/private/var/lib/s5lbox",
        "/private/var/log",
        "/System/Library/LaunchDaemons"
    };
    const size_t directory_count =
        sizeof directories / sizeof directories[0];
    const size_t required = directory_count + 2u + (create_source ? 1u : 0u);
    if (!entries || capacity < required) return required;
    memset(entries, 0, required * sizeof entries[0]);
    size_t count = 0u;
    for (size_t i = 0u; i < directory_count; i++) {
        entries[count].kind = ROOTFS_WORK_ENTRY_DIRECTORY;
        entries[count].path = directories[i];
        entries[count].permissions = 0755u;
        entries[count].existing_policy =
            ROOTFS_WORK_EXISTING_REUSE_DIRECTORY;
        count++;
    }
    if (create_source) {
        entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
        entries[count].path = VM_GUEST_ROOTFS_BIGBOSS_SOURCE_PATH;
        entries[count].content = VM_BIGBOSS_SOURCE;
        entries[count].content_size = sizeof VM_BIGBOSS_SOURCE - 1u;
        entries[count].permissions = 0644u;
        count++;
    }
    entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
    /* Cydia-era bootstraps may stash /usr/libexec behind a symlink. The
     * provisioner must not replace or traverse that guest-owned object, and
     * this one-shot helper belongs with the state it manages anyway. */
    entries[count].path = VM_BIGBOSS_CACHE_CLEANUP_PATH;
    entries[count].content = VM_BIGBOSS_CACHE_CLEANUP;
    entries[count].content_size = sizeof VM_BIGBOSS_CACHE_CLEANUP - 1u;
    entries[count].permissions = 0755u;
    count++;
    entries[count].kind = ROOTFS_WORK_ENTRY_FILE;
    entries[count].path =
        "/System/Library/LaunchDaemons/"
        "com.j0shua.s5lbox.cydia-index-cache-v2.plist";
    entries[count].content = VM_BIGBOSS_CACHE_CLEANUP_PLIST;
    entries[count].content_size =
        sizeof VM_BIGBOSS_CACHE_CLEANUP_PLIST - 1u;
    entries[count].permissions = 0644u;
    count++;
    return count;
}

#if defined(S5LBOX_GUEST_INSTALL_TESTING)
size_t vm_guest_install_build_test_bigboss_source_entries(
    rootfs_work_entry_t *entries, size_t capacity, bool create_source) {
    return build_bigboss_source_entries(entries, capacity, create_source);
}
#endif

static void build_detail(char *detail, size_t capacity, const char *text) {
    if (!detail || capacity == 0u) return;
    (void)snprintf(detail, capacity, "%s", text ? text : "");
    detail[capacity - 1u] = '\0';
}

static void build_result_clear(vm_guest_install_build_result_t *result) {
    if (result) memset(result, 0, sizeof *result);
}

static void build_progress(vm_guest_install_build_progress_t callback,
                           void *context,
                           vm_guest_install_build_phase_t phase,
                           uint64_t completed, uint64_t total) {
    if (callback) callback(context, phase, completed, total);
}

static void build_rootfs_progress(void *opaque, uint64_t done,
                                  uint64_t total) {
    build_progress_adapter_t *adapter = (build_progress_adapter_t *)opaque;
    if (!adapter) return;
    build_progress(adapter->callback, adapter->context,
                   VM_GUEST_INSTALL_BUILD_COPYING, done, total);
}

static bool build_join(char out[VM_GUEST_INSTALL_PATH_CAPACITY],
                       const char *directory, const char *leaf) {
    if (!out || !directory || !*directory || !leaf || !*leaf) return false;
    size_t length = strlen(directory);
    const char *separator = directory[length - 1u] == '/' ||
                            directory[length - 1u] == '\\' ? "" : "/";
    int written = snprintf(out, VM_GUEST_INSTALL_PATH_CAPACITY,
                           "%s%s%s", directory, separator, leaf);
    return written > 0 &&
           (size_t)written < VM_GUEST_INSTALL_PATH_CAPACITY;
}

static bool build_regular_file_size(const char *path, uint64_t *out_size) {
    if (out_size) *out_size = 0u;
    if (!path || !*path || !out_size) return false;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0 || (st.st_mode & _S_IFREG) == 0 ||
        st.st_size <= 0)
        return false;
#else
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return false;
#endif
    *out_size = (uint64_t)st.st_size;
    return true;
}

static vm_guest_install_build_status_t build_snapshot_gate(
    const char *work_directory, vm_guest_install_build_result_t *result,
    char *detail, size_t detail_capacity) {
    char directory[VM_GUEST_INSTALL_PATH_CAPACITY];
    vm_snapshot_status_t path = vm_snapshot_dir(
        work_directory, directory, sizeof directory);
    if (path != VM_SNAPSHOT_OK) {
        build_detail(detail, detail_capacity,
                     "The machine snapshot path is too long to inspect.");
        return VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS;
    }
    vm_snapshot_info_t snapshots[VM_SNAPSHOT_MAX];
    size_t count = 0u;
    char snapshot_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_snapshot_status_t listed = vm_snapshot_list(
        directory, snapshots, VM_SNAPSHOT_MAX, &count,
        snapshot_detail, sizeof snapshot_detail);
    if (listed != VM_SNAPSHOT_OK) {
        build_detail(detail, detail_capacity,
                     snapshot_detail[0] ? snapshot_detail
                                        : vm_snapshot_status_text(listed));
        return VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS;
    }
    if (result) result->historical_snapshots = count;
    if (count != 0u) {
        build_detail(detail, detail_capacity,
                     "Delete this machine's historical snapshots before replacing its guest disk.");
        return VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS;
    }
    return VM_GUEST_INSTALL_BUILD_OK;
}

static bool build_transaction_matches_install(
    const vm_guest_install_result_t *transaction,
    const vm_guest_install_result_t *install) {
    return transaction && install &&
           (!transaction->committed ||
            (transaction->has_manifest && install->has_manifest &&
             memcmp(transaction->manifest_sha256, install->manifest_sha256,
                    VM_GUEST_INSTALL_SHA256_SIZE) == 0));
}

static vm_guest_install_build_status_t build_rootfs_refusal(
    rootfs_work_status_t status, const rootfs_work_result_t *rootfs,
    char *detail, size_t detail_capacity) {
    if (status == ROOTFS_WORK_HFS_INVALID && rootfs &&
        strstr(rootfs->detail, "not cleanly unmounted") != NULL) {
        build_detail(detail, detail_capacity,
                     "Guest-disk maintenance needs a clean guest shutdown. Reopen this machine, hold Power, slide to power off, wait until the guest halts, return to Machines, and try again. S5LBox will not guess-repair this unjournaled HFS disk.");
        return VM_GUEST_INSTALL_BUILD_ERR_STORAGE_NOT_CLEAN;
    }
    build_detail(detail, detail_capacity,
                 rootfs && rootfs->detail[0]
                     ? rootfs->detail : rootfs_work_status_name(status));
    return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
}

static bool build_repair_proves_allocation_reconciliation(
    const rootfs_work_result_t *preflight,
    const rootfs_work_result_t *repair, uint64_t live_size) {
    if (!preflight || !repair || live_size == 0u ||
        preflight->status != ROOTFS_WORK_HFS_INVALID ||
        repair->status != ROOTFS_WORK_OK || repair->published ||
        preflight->source_size != live_size ||
        repair->source_size != live_size || repair->final_size != live_size ||
        !preflight->source_allocation_free_count_mismatch ||
        !repair->source_allocation_free_count_mismatch ||
        preflight->source_allocation_bitmap_used !=
            repair->source_allocation_bitmap_used ||
        preflight->source_allocation_header_used !=
            repair->source_allocation_header_used ||
        preflight->source_cleanly_unmounted !=
            repair->source_cleanly_unmounted)
        return false;

    /* The scanner has already performed a raw strict re-audit before it can
     * return OK. Keep the app-level publication gate independent: every
     * planned mutation must also be reported as applied, and the original
     * bitmap/header disagreement must have caused either a bitmap change or a
     * redundant freeBlocks change. A topology-only identity pass cannot make
     * an allocation mismatch safe to publish. */
    if (repair->catalog_backlinks_repairable !=
            repair->catalog_backlinks_repaired ||
        repair->catalog_topology_nodes_repairable !=
            repair->catalog_topology_nodes_repaired ||
        repair->catalog_extent_records_repairable !=
            repair->catalog_extent_records_repaired ||
        repair->allocation_bits_repairable !=
            repair->allocation_bits_repaired ||
        repair->allocation_free_count_repairable !=
            repair->allocation_free_count_repaired)
        return false;
    return repair->allocation_bits_repaired != 0u ||
           repair->allocation_free_count_repaired != 0u;
}

#if defined(S5LBOX_GUEST_INSTALL_TESTING)
bool vm_guest_install_build_test_allocation_repair_proven(
    const rootfs_work_result_t *preflight,
    const rootfs_work_result_t *repair, uint64_t live_size) {
    return build_repair_proves_allocation_reconciliation(
        preflight, repair, live_size);
}
#endif

/* The strict read-only probe admits this path only for a stale redundant
 * freeBlocks count. The powered-off scanner then proves the complete disk, so
 * it may also canonicalize derivable catalog topology or reconcile other
 * crash-consistent allocation state. Ambiguity is refused by that scanner;
 * nothing is published unless its raw strict re-audit succeeds. */
static vm_guest_install_build_status_t build_repair_allocation_accounting(
    const char *work_directory, uint64_t live_size,
    const rootfs_work_result_t *accounting_probe,
    vm_guest_install_build_progress_t progress, void *progress_context,
    vm_guest_install_build_result_t *result,
    char *detail, size_t detail_capacity) {
    char stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    char transaction_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    if (!vm_guest_recovery_stage_image_path(
            stage, sizeof stage, work_directory)) {
        build_detail(detail, detail_capacity,
                     "The filesystem-accounting recovery path is too long.");
        return VM_GUEST_INSTALL_BUILD_ERR_PATH;
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_STAGING, 0u, 1u);
    vm_guest_install_result_t transaction;
    vm_guest_install_status_t transaction_status =
        vm_guest_recovery_prepare_stage(
            work_directory, &transaction, transaction_detail,
            sizeof transaction_detail);
    if (transaction_status != VM_GUEST_INSTALL_OK || transaction.committed) {
        build_detail(detail, detail_capacity,
                     transaction_status == VM_GUEST_INSTALL_OK
                         ? "Filesystem recovery unexpectedly became committed before its clone was prepared."
                         : (transaction_detail[0]
                                ? transaction_detail
                                : vm_guest_install_status_text(
                                      transaction_status)));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    transaction_status = vm_guest_recovery_clone_live_to_stage(
        work_directory, transaction_detail, sizeof transaction_detail);
    if (transaction_status != VM_GUEST_INSTALL_OK) {
        char discard_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
        vm_guest_install_status_t discarded =
            vm_guest_recovery_discard_stage(
                work_directory, discard_detail, sizeof discard_detail);
        if (discarded != VM_GUEST_INSTALL_OK) {
            (void)snprintf(
                detail, detail_capacity,
                "The filesystem recovery clone failed and its inert stage could not be removed: %.120s",
                discard_detail[0] ? discard_detail
                                  : vm_guest_install_status_text(discarded));
        } else {
            build_detail(detail, detail_capacity,
                         transaction_detail[0]
                             ? transaction_detail
                             : vm_guest_install_status_text(
                                   transaction_status));
        }
        if (detail && detail_capacity) detail[detail_capacity - 1u] = '\0';
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    rootfs_work_result_t repair;
    rootfs_work_status_t repair_status =
        rootfs_work_repair_powered_off_clone(stage, &repair);
    if (result) result->filesystem_recovery = repair;
    bool geometry_ok = repair.source_size == live_size &&
                       repair.final_size == live_size;
    bool proven_repair = repair_status == ROOTFS_WORK_OK &&
        build_repair_proves_allocation_reconciliation(
            accounting_probe, &repair, live_size);
    if (!proven_repair) {
        char discard_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
        vm_guest_install_status_t discarded =
            vm_guest_recovery_discard_stage(
                work_directory, discard_detail, sizeof discard_detail);
        if (discarded != VM_GUEST_INSTALL_OK) {
            (void)snprintf(
                detail, detail_capacity,
                "The unpublished filesystem repair was refused and its stage could not be removed: %.112s",
                discard_detail[0] ? discard_detail
                                  : vm_guest_install_status_text(discarded));
            if (detail && detail_capacity)
                detail[detail_capacity - 1u] = '\0';
            return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
        }
        if (repair_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(
                repair_status, &repair, detail, detail_capacity);
        if (geometry_ok) {
            (void)snprintf(
                detail, detail_capacity,
                "Filesystem repair evidence mismatch: source bitmap/header "
                "%u/%u blocks; topology %u/%u, fork records %u/%u, bitmap "
                "bits %u/%u, and free count %u/%u. The clone was not "
                "published.",
                repair.source_allocation_bitmap_used,
                repair.source_allocation_header_used,
                repair.catalog_topology_nodes_repairable,
                repair.catalog_topology_nodes_repaired,
                repair.catalog_extent_records_repairable,
                repair.catalog_extent_records_repaired,
                repair.allocation_bits_repairable,
                repair.allocation_bits_repaired,
                repair.allocation_free_count_repairable,
                repair.allocation_free_count_repaired);
            if (detail && detail_capacity)
                detail[detail_capacity - 1u] = '\0';
        } else {
            build_detail(
                detail, detail_capacity,
                "The repaired clone changed guest-disk geometry; it was not published.");
        }
        return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 0u, 1u);
    transaction_status = vm_guest_recovery_publish(
        work_directory, &transaction, transaction_detail,
        sizeof transaction_detail);
    if (result) result->filesystem_recovery_transaction = transaction;
    if (transaction_status != VM_GUEST_INSTALL_OK || !transaction.committed) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(transaction_status));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    if (!transaction.cleanup_complete) {
        transaction_status = vm_guest_recovery_recover(
            work_directory, &transaction, transaction_detail,
            sizeof transaction_detail);
        if (result) result->filesystem_recovery_transaction = transaction;
        if (transaction_status != VM_GUEST_INSTALL_OK ||
            !transaction.committed || !transaction.cleanup_complete) {
            build_detail(detail, detail_capacity,
                         transaction_detail[0]
                             ? transaction_detail
                             : vm_guest_install_status_text(
                                   transaction_status));
            return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
        }
    }
    if (result) result->filesystem_repaired = true;
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 1u, 1u);
    return VM_GUEST_INSTALL_BUILD_OK;
}

static bool build_rootfs_is_unclean(rootfs_work_status_t status,
                                    const rootfs_work_result_t *rootfs) {
    return status == ROOTFS_WORK_HFS_INVALID && rootfs &&
           strstr(rootfs->detail, "not cleanly unmounted") != NULL;
}

static bool build_authorize_unclean_source(
    const char *work_directory, uint64_t live_size,
    bool *allow_unclean_source,
    vm_guest_install_build_result_t *result) {
    if (!allow_unclean_source) return false;
    if (*allow_unclean_source) return true;
    char checkpoint_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_resume_checkpoint_state_t checkpoint =
        vm_resume_checkpoint_probe_state(
            work_directory, live_size, BUILD_CHECKPOINT_RAM_BASE,
            BUILD_CHECKPOINT_RAM_SIZE, checkpoint_detail,
            sizeof checkpoint_detail);
    if (checkpoint != VM_RESUME_CHECKPOINT_POWERED_OFF) return false;
    *allow_unclean_source = true;
    if (result) result->powered_off_checkpoint_witnessed = true;
    return true;
}

static rootfs_work_status_t build_probe_file_repair(
    const char *work_directory, const char *live, uint64_t live_size,
    const rootfs_work_file_repair_t *repair,
    rootfs_work_file_repair_state_t *state,
    bool *allow_unclean_source,
    vm_guest_install_build_result_t *result,
    rootfs_work_result_t *probe) {
    bool authorized = allow_unclean_source && *allow_unclean_source;
    rootfs_work_status_t status = rootfs_work_probe_file_repair_policy(
        live, repair, authorized, authorized, state, probe);
    if (!build_rootfs_is_unclean(status, probe) ||
        !build_authorize_unclean_source(
            work_directory, live_size, allow_unclean_source, result))
        return status;
    return rootfs_work_probe_file_repair_policy(
        live, repair, true, true, state, probe);
}

static rootfs_work_status_t build_validate_source(
    const char *work_directory, const char *live, uint64_t live_size,
    bool *allow_unclean_source,
    vm_guest_install_build_result_t *result,
    rootfs_work_result_t *preflight) {
    rootfs_work_status_t status = rootfs_work_validate_source_ex(
        live, allow_unclean_source && *allow_unclean_source, preflight);
    if (!build_rootfs_is_unclean(status, preflight) ||
        !build_authorize_unclean_source(
            work_directory, live_size, allow_unclean_source, result))
        return status;
    return rootfs_work_validate_source_ex(live, true, preflight);
}

static vm_guest_install_build_status_t build_maintain_install(
    const char *work_directory,
    const vm_guest_install_result_t *install,
    const vm_guest_install_result_t *storage,
    const vm_guest_install_result_t *privilege,
    const vm_guest_install_result_t *sources,
    const vm_guest_install_result_t *sources_v2,
    vm_guest_install_build_progress_t progress, void *progress_context,
    vm_guest_install_build_result_t *result,
    char *detail, size_t detail_capacity) {
    if (!install || !install->committed || !install->has_manifest ||
        !storage || !privilege || !sources || !sources_v2 ||
        !build_transaction_matches_install(storage, install) ||
        !build_transaction_matches_install(privilege, install) ||
        !build_transaction_matches_install(sources, install) ||
        !build_transaction_matches_install(sources_v2, install)) {
        build_detail(detail, detail_capacity,
                     "A guest-disk maintenance record does not match the committed installation.");
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    if (!install->cleanup_complete ||
        (storage->committed && !storage->cleanup_complete) ||
        (privilege->committed && !privilege->cleanup_complete) ||
        (sources->committed && !sources->cleanup_complete) ||
        (sources_v2->committed && !sources_v2->cleanup_complete)) {
        build_detail(detail, detail_capacity,
                     "A committed guest-disk transaction still has cleanup residue; no new maintenance transaction was started.");
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    uint64_t live_size = 0u;
    if (!build_join(live, work_directory, VM_GUEST_INSTALL_LIVE_FILE) ||
        !build_regular_file_size(live, &live_size)) {
        build_detail(detail, detail_capacity,
                     "The committed installation has no valid live guest disk.");
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    bool grow_storage = live_size < VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES;
    if (grow_storage && storage->committed) {
        build_detail(detail, detail_capacity,
                     "The storage-upgrade record is committed, but the live guest disk is still smaller than 2 GiB.");
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    /* A clean HFS unmount can still leave its redundant freeBlocks value or
     * allocation bitmap one transaction behind the catalog. Detect only the
     * exact header/bitmap disagreement before any file-specific probe; the
     * complete powered-off scanner then determines which fully reconstructed
     * state is stale. Repair remains confined to the same unpublished-clone
     * transaction used by powered-off boot recovery. A dirty source receives
     * this privilege only when its exact powered-off checkpoint independently
     * authorizes the existing narrow exception. */
    bool allow_unclean_source = false;
    bool source_validated = false;
    bool snapshot_gate_passed = false;
    rootfs_work_result_t accounting_probe;
    rootfs_work_status_t accounting_status = build_validate_source(
        work_directory, live, live_size, &allow_unclean_source,
        result, &accounting_probe);
    if (result) result->rootfs = accounting_probe;
    if (accounting_status == ROOTFS_WORK_OK) {
        source_validated = true;
    } else if (accounting_status == ROOTFS_WORK_HFS_INVALID &&
               accounting_probe.source_allocation_free_count_mismatch &&
               (accounting_probe.source_cleanly_unmounted ||
                allow_unclean_source)) {
        vm_guest_install_build_status_t snapshot_gate = build_snapshot_gate(
            work_directory, result, detail, detail_capacity);
        if (snapshot_gate != VM_GUEST_INSTALL_BUILD_OK) return snapshot_gate;
        snapshot_gate_passed = true;
        vm_guest_install_build_status_t repaired =
            build_repair_allocation_accounting(
                work_directory, live_size, &accounting_probe,
                progress, progress_context, result, detail,
                detail_capacity);
        if (repaired != VM_GUEST_INSTALL_BUILD_OK) return repaired;
        accounting_status = build_validate_source(
            work_directory, live, live_size, &allow_unclean_source,
            result, &accounting_probe);
        if (result) result->rootfs = accounting_probe;
        if (accounting_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(
                accounting_status, &accounting_probe,
                detail, detail_capacity);
        source_validated = true;
    }

    rootfs_work_file_repair_t repair;
    build_cydia_privilege_repair(&repair);
    rootfs_work_file_repair_state_t repair_state =
        ROOTFS_WORK_FILE_REPAIR_MISSING;
    bool repair_needed = false;
    bool source_needed = false;
    bool source_preflighted = false;
    char transaction_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];

    if (privilege->committed) {
        if (result) result->cydia_privileges_verified = true;
    } else {
        rootfs_work_result_t probe;
        rootfs_work_status_t probe_status = build_probe_file_repair(
            work_directory, live, live_size, &repair, &repair_state,
            &allow_unclean_source, result, &probe);
        if (result) result->rootfs = probe;
        if (probe_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(probe_status, &probe,
                                        detail, detail_capacity);
        source_preflighted = true;
        repair_needed = repair_state == ROOTFS_WORK_FILE_REPAIR_NEEDED;
        if (repair_state == ROOTFS_WORK_FILE_REPAIR_SATISFIED) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_privilege_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->privilege_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK || !confirmed.committed) {
                build_detail(detail, detail_capacity,
                             transaction_detail[0]
                                 ? transaction_detail
                                 : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
            }
            if (result) result->cydia_privileges_verified = true;
        }
    }

    rootfs_work_file_repair_t source_probe;
    bool source_create = false;
    if (sources_v2->committed) {
        if (result) result->cydia_sources_verified = true;
    } else {
        if (!build_bigboss_source_probe(&source_probe)) {
            build_detail(detail, detail_capacity,
                         "The BigBoss source identity could not be computed.");
            return VM_GUEST_INSTALL_BUILD_ERR_MANIFEST;
        }
        rootfs_work_file_repair_state_t source_state =
            ROOTFS_WORK_FILE_REPAIR_MISSING;
        rootfs_work_result_t probe;
        rootfs_work_status_t probe_status = build_probe_file_repair(
            work_directory, live, live_size, &source_probe, &source_state,
            &allow_unclean_source, result, &probe);
        if (probe_status == ROOTFS_WORK_FILE_REPAIR_MISMATCH &&
            build_bigboss_source_probe_for(
                &source_probe, VM_BIGBOSS_SOURCE_COMPAT,
                sizeof VM_BIGBOSS_SOURCE_COMPAT - 1u)) {
            source_state = ROOTFS_WORK_FILE_REPAIR_MISSING;
            probe_status = build_probe_file_repair(
                work_directory, live, live_size, &source_probe, &source_state,
                &allow_unclean_source, result, &probe);
        }
        if (result) result->rootfs = probe;
        if (probe_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(probe_status, &probe,
                                        detail, detail_capacity);
        source_preflighted = true;
        source_create = source_state == ROOTFS_WORK_FILE_REPAIR_MISSING;
        if (!source_create &&
            source_state != ROOTFS_WORK_FILE_REPAIR_SATISFIED) {
            build_detail(detail, detail_capacity,
                         "The managed BigBoss source has unexpected metadata or contents.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }
        /* An already-correct source can coexist with an HTML list cached
         * after a compressed-index transfer failed. The v2 disk transaction
         * installs a one-shot cache purge; the source URL itself is unchanged
         * because changing its spelling does not change APT's fallback URLs. */
        source_needed = true;
    }

    if (!grow_storage && !repair_needed && !source_needed) {
        build_progress(progress, progress_context,
                       VM_GUEST_INSTALL_BUILD_COMPLETE, 1u, 1u);
        return VM_GUEST_INSTALL_BUILD_OK;
    }

    if (!snapshot_gate_passed) {
        vm_guest_install_build_status_t snapshot_gate = build_snapshot_gate(
            work_directory, result, detail, detail_capacity);
        if (snapshot_gate != VM_GUEST_INSTALL_BUILD_OK) return snapshot_gate;
    }
    if (!source_preflighted && !source_validated) {
        rootfs_work_result_t preflight;
        rootfs_work_status_t preflight_status = build_validate_source(
            work_directory, live, live_size, &allow_unclean_source,
            result, &preflight);
        if (result) result->rootfs = preflight;
        if (preflight_status != ROOTFS_WORK_OK)
            return build_rootfs_refusal(preflight_status, &preflight,
                                        detail, detail_capacity);
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_STAGING, 0u, 1u);
    vm_guest_install_result_t prepared;
    vm_guest_install_status_t preparation;
    if (grow_storage) {
        preparation = vm_guest_storage_prepare_stage(
            work_directory, &prepared, transaction_detail,
            sizeof transaction_detail);
    } else if (repair_needed) {
        preparation = vm_guest_privilege_prepare_stage(
            work_directory, &prepared, transaction_detail,
            sizeof transaction_detail);
    } else {
        preparation = vm_guest_sources_v2_prepare_stage(
            work_directory, &prepared, transaction_detail,
            sizeof transaction_detail);
    }
    if (preparation != VM_GUEST_INSTALL_OK || prepared.committed) {
        build_detail(detail, detail_capacity,
                     preparation == VM_GUEST_INSTALL_OK
                         ? "Guest-disk maintenance became committed while its disk was being prepared."
                         : (transaction_detail[0]
                                ? transaction_detail
                                : vm_guest_install_status_text(preparation)));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    char stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    bool stage_ok;
    if (grow_storage) {
        stage_ok = vm_guest_storage_stage_image_path(
            stage, sizeof stage, work_directory);
    } else if (repair_needed) {
        stage_ok = vm_guest_privilege_stage_image_path(
            stage, sizeof stage, work_directory);
    } else {
        stage_ok = vm_guest_sources_v2_stage_image_path(
            stage, sizeof stage, work_directory);
    }
    if (!stage_ok) {
        build_detail(detail, detail_capacity,
                     "The guest-disk maintenance stage path is too long.");
        return VM_GUEST_INSTALL_BUILD_ERR_PATH;
    }
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_STAGING, 1u, 1u);

    rootfs_work_options_t options;
    memset(&options, 0, sizeof options);
    options.preserve_fstab = true;
    options.allow_unclean_source = allow_unclean_source;
    options.repair_catalog_backlinks =
        allow_unclean_source && (repair_needed || source_needed);
    if (grow_storage)
        options.minimum_volume_bytes = VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES;
    if (repair_needed) {
        options.file_repairs = &repair;
        options.file_repair_count = 1u;
    }
    rootfs_work_entry_t
        source_entries[VM_BIGBOSS_MIGRATION_MAX_ENTRIES];
    size_t source_entry_count = 0u;
    if (source_needed) {
        source_entry_count = build_bigboss_source_entries(
            source_entries, VM_BIGBOSS_MIGRATION_MAX_ENTRIES,
            source_create);
        options.entries = source_entries;
        options.entry_count = source_entry_count;
    }
    build_progress_adapter_t adapter = {progress, progress_context};
    options.progress = build_rootfs_progress;
    options.progress_ctx = &adapter;
    rootfs_work_result_t rootfs;
    rootfs_work_status_t rootfs_status = rootfs_work_create(
        live, stage, &options, &rootfs);
    if (result) result->rootfs = rootfs;
    if (rootfs_status != ROOTFS_WORK_OK || !rootfs.published ||
        (grow_storage &&
         rootfs.final_size < VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES) ||
        (repair_needed && rootfs.file_repairs_applied != 1u) ||
        (source_needed &&
         (rootfs.provision_entries < 2u ||
          rootfs.provision_entries + rootfs.provision_reused_entries !=
              source_entry_count))) {
        if (rootfs_status == ROOTFS_WORK_OK) {
            build_detail(detail, detail_capacity,
                         "The completed guest-disk clone did not contain the requested maintenance result.");
            return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
        }
        return build_rootfs_refusal(rootfs_status, &rootfs,
                                    detail, detail_capacity);
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 0u, 1u);
    vm_guest_install_result_t published;
    vm_guest_install_status_t publication;
    if (grow_storage) {
        publication = vm_guest_storage_publish(
            work_directory, install->manifest_sha256, &published,
            transaction_detail, sizeof transaction_detail);
    } else if (repair_needed) {
        publication = vm_guest_privilege_publish(
            work_directory, install->manifest_sha256, &published,
            transaction_detail, sizeof transaction_detail);
    } else {
        publication = vm_guest_sources_v2_publish(
            work_directory, install->manifest_sha256, &published,
            transaction_detail, sizeof transaction_detail);
    }
    if (grow_storage) {
        if (result) result->storage_transaction = published;
    } else if (repair_needed) {
        if (result) result->privilege_transaction = published;
    } else if (result) {
        result->sources_v2_transaction = published;
    }
    if (publication != VM_GUEST_INSTALL_OK || !published.committed) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(publication));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    if (result && grow_storage) result->storage_upgraded = true;
    if (repair_needed) {
        if (result) result->cydia_privileges_repaired = true;
        if (grow_storage) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_privilege_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->privilege_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK || !confirmed.committed) {
                build_detail(detail, detail_capacity,
                             transaction_detail[0]
                                 ? transaction_detail
                                 : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
            }
        }
        if (result) result->cydia_privileges_verified = true;
    }
    if (source_needed) {
        if (result) result->cydia_sources_added = true;
        if (!sources->committed) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_sources_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->sources_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK ||
                !confirmed.committed) {
                build_detail(detail, detail_capacity,
                             transaction_detail[0]
                                 ? transaction_detail
                                 : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
            }
        }
        if (grow_storage || repair_needed) {
            vm_guest_install_result_t confirmed;
            vm_guest_install_status_t confirmation =
                vm_guest_sources_v2_confirm(
                    work_directory, install->manifest_sha256, &confirmed,
                    transaction_detail, sizeof transaction_detail);
            if (result) result->sources_v2_transaction = confirmed;
            if (confirmation != VM_GUEST_INSTALL_OK ||
                !confirmed.committed) {
                build_detail(detail, detail_capacity,
                             transaction_detail[0]
                                 ? transaction_detail
                                 : vm_guest_install_status_text(confirmation));
                return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
            }
        }
        if (result) result->cydia_sources_verified = true;
    }
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 1u, 1u);
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_COMPLETE, 1u, 1u);
    return VM_GUEST_INSTALL_BUILD_OK;
}

vm_guest_install_build_status_t
vm_guest_install_build_from_directory(
    const char *work_directory, const char *package_directory,
    vm_guest_install_build_progress_t progress, void *progress_context,
    vm_guest_install_build_result_t *result,
    char *detail, size_t detail_capacity) {
    build_result_clear(result);
    build_detail(detail, detail_capacity, "");
    if (!work_directory || !*work_directory) {
        build_detail(detail, detail_capacity,
                     "The machine work directory is missing.");
        return VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT;
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 0u, 4u);
    vm_guest_install_result_t privilege;
    vm_guest_install_result_t storage;
    vm_guest_install_result_t sources;
    char transaction_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_guest_install_status_t maintenance_recovery =
        vm_guest_maintenance_recover(
            work_directory, &privilege, &storage, &sources, transaction_detail,
            sizeof transaction_detail);
    if (maintenance_recovery != VM_GUEST_INSTALL_OK) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(maintenance_recovery));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    if (result) {
        result->privilege_transaction = privilege;
        result->storage_transaction = storage;
        result->sources_transaction = sources;
    }
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 2u, 4u);
    vm_guest_install_result_t sources_v2;
    maintenance_recovery = vm_guest_sources_v2_recover(
        work_directory, &sources_v2, transaction_detail,
        sizeof transaction_detail);
    if (maintenance_recovery != VM_GUEST_INSTALL_OK) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(maintenance_recovery));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    if (result) result->sources_v2_transaction = sources_v2;
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 3u, 4u);
    vm_guest_install_result_t recovered;
    vm_guest_install_status_t recovery = vm_guest_install_recover(
        work_directory, &recovered, transaction_detail,
        sizeof transaction_detail);
    if (recovery != VM_GUEST_INSTALL_OK) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0] ? transaction_detail
                                           : vm_guest_install_status_text(recovery));
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }
    if (result) result->transaction = recovered;
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_RECOVERING, 4u, 4u);
    if (recovered.committed) {
        if (result) {
            result->already_installed = true;
            if (recovered.has_manifest)
                memcpy(result->manifest_sha256, recovered.manifest_sha256,
                       VM_GUEST_INSTALL_SHA256_SIZE);
        }
        return build_maintain_install(
            work_directory, &recovered, &storage, &privilege, &sources,
            &sources_v2,
            progress, progress_context, result, detail, detail_capacity);
    }
    if (storage.committed || privilege.committed || sources.committed ||
        sources_v2.committed) {
        build_detail(detail, detail_capacity,
                     "A guest-disk maintenance record exists without a committed guest installation.");
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    vm_guest_install_build_status_t snapshot_gate = build_snapshot_gate(
        work_directory, result, detail, detail_capacity);
    if (snapshot_gate != VM_GUEST_INSTALL_BUILD_OK) return snapshot_gate;
    if (!package_directory || !*package_directory) {
        build_detail(detail, detail_capacity,
                     "The verified package directory is missing.");
        return VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT;
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PLANNING, 0u, 1u);
    vm_guest_rootfs_status_t plan_status = VM_GUEST_ROOTFS_ERR_ARGUMENT;
    char plan_detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
    vm_guest_rootfs_plan_t *plan = vm_guest_rootfs_plan_open_directory(
        package_directory, &plan_status, plan_detail, sizeof plan_detail);
    if (!plan) {
        build_detail(detail, detail_capacity,
                     plan_detail[0] ? plan_detail
                                    : vm_guest_rootfs_status_text(plan_status));
        return VM_GUEST_INSTALL_BUILD_ERR_PACKAGES;
    }
    if (result) vm_guest_rootfs_plan_get_stats(plan, &result->plan);
    uint8_t manifest[VM_GUEST_INSTALL_SHA256_SIZE];
    if (!vm_guest_rootfs_plan_manifest_sha256(plan, manifest)) {
        vm_guest_rootfs_plan_close(&plan);
        build_detail(detail, detail_capacity,
                     "The guest rootfs plan has no stable manifest identity.");
        return VM_GUEST_INSTALL_BUILD_ERR_MANIFEST;
    }
    if (result) memcpy(result->manifest_sha256, manifest, sizeof manifest);
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PLANNING, 1u, 1u);

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_STAGING, 0u, 1u);
    vm_guest_install_result_t prepared;
    vm_guest_install_status_t preparation = vm_guest_install_prepare_stage(
        work_directory, &prepared, transaction_detail,
        sizeof transaction_detail);
    if (preparation != VM_GUEST_INSTALL_OK || prepared.committed) {
        vm_guest_rootfs_plan_close(&plan);
        if (preparation == VM_GUEST_INSTALL_OK && prepared.committed) {
            build_detail(detail, detail_capacity,
                         "The machine became installed while its package plan was being built.");
        } else {
            build_detail(detail, detail_capacity,
                         transaction_detail[0]
                             ? transaction_detail
                             : vm_guest_install_status_text(preparation));
        }
        return VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION;
    }

    char live[VM_GUEST_INSTALL_PATH_CAPACITY];
    char stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    if (!build_join(live, work_directory, VM_GUEST_INSTALL_LIVE_FILE) ||
        !vm_guest_install_stage_image_path(stage, sizeof stage,
                                           work_directory)) {
        vm_guest_rootfs_plan_close(&plan);
        build_detail(detail, detail_capacity,
                     "The live or staged guest-disk path is too long.");
        return VM_GUEST_INSTALL_BUILD_ERR_PATH;
    }
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_STAGING, 1u, 1u);

    rootfs_work_options_t options;
    memset(&options, 0, sizeof options);
    options.preserve_fstab = true;
    options.minimum_volume_bytes = VM_GUEST_INSTALL_MINIMUM_VOLUME_BYTES;
    options.entries = vm_guest_rootfs_plan_entries(plan);
    options.entry_count = vm_guest_rootfs_plan_entry_count(plan);
    build_progress_adapter_t adapter = {progress, progress_context};
    options.progress = build_rootfs_progress;
    options.progress_ctx = &adapter;
    rootfs_work_result_t rootfs;
    rootfs_work_status_t rootfs_status = rootfs_work_create(
        live, stage, &options, &rootfs);
    if (result) result->rootfs = rootfs;
    vm_guest_rootfs_plan_close(&plan);
    if (rootfs_status != ROOTFS_WORK_OK || !rootfs.published) {
        build_detail(detail, detail_capacity,
                     rootfs.detail[0] ? rootfs.detail
                                      : rootfs_work_status_name(rootfs_status));
        return VM_GUEST_INSTALL_BUILD_ERR_ROOTFS;
    }

    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 0u, 1u);
    vm_guest_install_result_t published;
    vm_guest_install_status_t publication = vm_guest_install_publish(
        work_directory, manifest, &published, transaction_detail,
        sizeof transaction_detail);
    if (result) result->transaction = published;
    if (publication != VM_GUEST_INSTALL_OK || !published.committed) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(publication));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    vm_guest_install_result_t sources_confirmed;
    vm_guest_install_status_t sources_confirmation =
        vm_guest_sources_confirm(
            work_directory, manifest, &sources_confirmed, transaction_detail,
            sizeof transaction_detail);
    if (result) result->sources_transaction = sources_confirmed;
    if (sources_confirmation != VM_GUEST_INSTALL_OK ||
        !sources_confirmed.committed) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(sources_confirmation));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    sources_confirmation = vm_guest_sources_v2_confirm(
        work_directory, manifest, &sources_confirmed, transaction_detail,
        sizeof transaction_detail);
    if (result) result->sources_v2_transaction = sources_confirmed;
    if (sources_confirmation != VM_GUEST_INSTALL_OK ||
        !sources_confirmed.committed) {
        build_detail(detail, detail_capacity,
                     transaction_detail[0]
                         ? transaction_detail
                         : vm_guest_install_status_text(sources_confirmation));
        return VM_GUEST_INSTALL_BUILD_ERR_PUBLISH;
    }
    if (result) result->cydia_sources_verified = true;
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_PUBLISHING, 1u, 1u);
    build_progress(progress, progress_context,
                   VM_GUEST_INSTALL_BUILD_COMPLETE, 1u, 1u);
    return VM_GUEST_INSTALL_BUILD_OK;
}

const char *vm_guest_install_build_status_text(
    vm_guest_install_build_status_t status) {
    switch (status) {
        case VM_GUEST_INSTALL_BUILD_OK:              return "ok";
        case VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT:    return "invalid argument";
        case VM_GUEST_INSTALL_BUILD_ERR_TRANSACTION: return "transaction recovery";
        case VM_GUEST_INSTALL_BUILD_ERR_SNAPSHOTS:   return "historical snapshots";
        case VM_GUEST_INSTALL_BUILD_ERR_STORAGE_NOT_CLEAN:
            return "guest shutdown required";
        case VM_GUEST_INSTALL_BUILD_ERR_PACKAGES:    return "package plan";
        case VM_GUEST_INSTALL_BUILD_ERR_MANIFEST:    return "manifest identity";
        case VM_GUEST_INSTALL_BUILD_ERR_PATH:        return "path unavailable";
        case VM_GUEST_INSTALL_BUILD_ERR_ROOTFS:      return "rootfs construction";
        case VM_GUEST_INSTALL_BUILD_ERR_PUBLISH:     return "transaction publish";
        default:                                     return "unknown status";
    }
}

const char *vm_guest_install_build_phase_text(
    vm_guest_install_build_phase_t phase) {
    switch (phase) {
        case VM_GUEST_INSTALL_BUILD_RECOVERING: return "Checking installation";
        case VM_GUEST_INSTALL_BUILD_PLANNING:   return "Verifying packages";
        case VM_GUEST_INSTALL_BUILD_STAGING:    return "Preparing guest disk";
        case VM_GUEST_INSTALL_BUILD_COPYING:    return "Building guest disk";
        case VM_GUEST_INSTALL_BUILD_PUBLISHING: return "Installing guest disk";
        case VM_GUEST_INSTALL_BUILD_COMPLETE:   return "Installation ready";
        default:                                return "Installing";
    }
}
