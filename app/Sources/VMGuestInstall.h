/*
 * S5LBox -- crash-safe publication of a prepared guest filesystem.
 *
 * Package acquisition and HFS+ construction happen elsewhere. This layer has
 * one narrower job: replace one machine's live rootfs only when a complete
 * staged image exists, and leave enough durable evidence to finish or roll
 * back after termination at every rename boundary. A strict manifest record
 * is the sole authority for the matching boot policy.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMGUESTINSTALL_H
#define S5LBOX_APP_VMGUESTINSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VM_GUEST_INSTALL_LIVE_FILE       "rootfs-work.img"
#define VM_GUEST_INSTALL_BACKUP_FILE     "rootfs-work.pre-jailbreak-v1"
#define VM_GUEST_INSTALL_STAGE_DIRECTORY "guest.jailbreak-v1.stage"
#define VM_GUEST_INSTALL_NEXT_FILE       "rootfs-next.img"
#define VM_GUEST_INSTALL_MARKER_FILE     "guest.jailbreak-v1"
#define VM_GUEST_INSTALL_MARKER_TMP      "guest.jailbreak-v1.partial"
#define VM_GUEST_INSTALL_JOURNAL_FILE    "guest.jailbreak-v1.transaction"
#define VM_GUEST_INSTALL_JOURNAL_TMP     "guest.jailbreak-v1.transaction.partial"
/* A filesystem replacement can never resume a CPU/RAM image captured against
 * the old disk. The transaction removes only this one-shot authority; inert
 * checkpoint payloads are harmless and can be replaced by the next save. */
#define VM_GUEST_INSTALL_RESUME_ONCE_FILE "state.snap.restore-once"
#define VM_GUEST_INSTALL_RESUME_ONCE_TMP  "state.snap.restore-once.partial"

#define VM_GUEST_INSTALL_SHA256_SIZE     32u
#define VM_GUEST_INSTALL_PATH_CAPACITY   1200u

typedef enum {
    VM_GUEST_INSTALL_PROBE_ABSENT = 0,
    VM_GUEST_INSTALL_PROBE_VALID,
    VM_GUEST_INSTALL_PROBE_INVALID,
    VM_GUEST_INSTALL_PROBE_IO_ERROR
} vm_guest_install_probe_t;

typedef enum {
    VM_GUEST_INSTALL_OK = 0,
    VM_GUEST_INSTALL_ERR_ARGUMENT,
    VM_GUEST_INSTALL_ERR_PATH,
    VM_GUEST_INSTALL_ERR_RECORD,
    VM_GUEST_INSTALL_ERR_STATE,
    VM_GUEST_INSTALL_ERR_IO,
    /* Produced only by the deterministic test interruption hook. */
    VM_GUEST_INSTALL_ERR_INTERRUPTED
} vm_guest_install_status_t;

typedef struct {
    /* True only when a strict marker and a non-empty live image agree that the
     * new filesystem was published. It remains true if inert cleanup failed. */
    bool committed;
    /* True when recovery restored the pre-transaction image instead. */
    bool rolled_back;
    /* False means only backup/journal/stage debris remains; committed data is
     * still authoritative and the next recovery call may retry cleanup. */
    bool cleanup_complete;
    bool has_manifest;
    uint8_t manifest_sha256[VM_GUEST_INSTALL_SHA256_SIZE];
} vm_guest_install_result_t;

const char *vm_guest_install_status_text(vm_guest_install_status_t status);

/* Parse the exact committed-record format. An empty or malformed file is not
 * presence: it is INVALID, so boot cannot silently enable or disable only one
 * half of the installed policy. */
vm_guest_install_probe_t
vm_guest_install_probe(const char *work_directory,
                       uint8_t manifest_sha256[VM_GUEST_INSTALL_SHA256_SIZE],
                       char *detail, size_t detail_capacity);

/* Build the path at which the image constructor must publish its complete,
 * flushed candidate. This function creates no directory or file. */
bool vm_guest_install_stage_image_path(char *out, size_t capacity,
                                       const char *work_directory);

/* Recover first, then create one empty real stage directory for the image
 * constructor. A journal-free interrupted candidate is inert and is removed;
 * unexpected files or file types are refused. On OK with result->committed,
 * the existing installation remains authoritative and no stage is created.
 * Otherwise the path returned above is absent and ready for O_EXCL publish. */
vm_guest_install_status_t
vm_guest_install_prepare_stage(const char *work_directory,
                               vm_guest_install_result_t *result,
                               char *detail, size_t detail_capacity);

/* Recover a transaction left at any durable boundary. No journal means a
 * staged image is inert. Contradictory or malformed evidence is never guessed
 * through. A cleanup warning can accompany VM_GUEST_INSTALL_OK; consult the
 * result flags rather than treating leftover debris as an uncommitted image. */
vm_guest_install_status_t
vm_guest_install_recover(const char *work_directory,
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity);

/* Publish the already prepared stage image. Repeating this call with the same
 * manifest is idempotent; a different manifest cannot overwrite a committed
 * v1 installation. */
vm_guest_install_status_t
vm_guest_install_publish(const char *work_directory,
                         const uint8_t manifest_sha256[
                             VM_GUEST_INSTALL_SHA256_SIZE],
                         vm_guest_install_result_t *result,
                         char *detail, size_t detail_capacity);

#if defined(S5LBOX_GUEST_INSTALL_TESTING)
/* Stop publish after durable boundary 1..4: journal, backup, new live image,
 * marker. Zero disables the hook. It is absent from production builds. */
void vm_guest_install_test_interrupt_after(unsigned boundary);
#endif

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMGUESTINSTALL_H */
