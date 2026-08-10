/*
 * S5LBox -- atomic installation of the one automatic resume checkpoint.
 *
 * The firmware adapter decides whether the running machine is at a safe disk
 * boundary and supplies the host-side memory-disk state. This file owns the
 * fallible file transaction: complete both payloads first, install them, and
 * publish the non-empty restore marker last. A crash before that last rename
 * leaves files on disk, but startup deliberately treats them as inert.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMRESUMECHECKPOINT_H
#define S5LBOX_APP_VMRESUMECHECKPOINT_H

#include "md_snapshot.h"
#include "soc.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Replace the single automatic resume checkpoint in `work_directory`.
 *
 * `machine` is read only. `sidecar` must describe the already-flushed live
 * work image. On success the state file, bridge sidecar and a non-empty
 * one-shot marker are all installed. On every failure the marker is absent,
 * so startup cannot consume a mismatched or partial pair.
 *
 * `detail` receives one user-facing sentence on failure and is cleared on
 * success. The function does not create the work directory.
 */
bool vm_resume_checkpoint_save(const s5l8900_t *machine,
                               const external_md_sidecar_t *sidecar,
                               const char *work_directory,
                               char *detail, size_t detail_capacity);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMRESUMECHECKPOINT_H */
