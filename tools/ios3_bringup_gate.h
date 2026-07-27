/*
 * S5LBox -- the iPhone OS 3.1.3 (7E18) kernel gate, as a bring-up hook.
 *
 * core/src/boot/bringup.c holds no firmware addresses by design (see the rule
 * stated in core/include/md_bridge.h). It asks a caller-supplied gate to
 * authorize and patch the kernel it has just loaded, and this is that gate for
 * the one build this project supports: a six-line adapter over
 * ios3_kernel_patch_apply(), plus the four SVC site addresses the memory-disk
 * bridges must watch.
 *
 * It exists as its own translation unit rather than as a copy inside each
 * frontend so the desktop test and the iOS app authorize a kernel through the
 * same code. Nothing here allocates, opens a file, or knows what a machine is.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_IOS3_BRINGUP_GATE_H
#define S5LBOX_IOS3_BRINGUP_GATE_H

#include "bringup.h"
#include "ios3_kernel_patch.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Suitable as an s5l_bringup_kernel_gate_fn. `context` may be NULL, or may
 * point at an ios3_bringup_gate_report_t the caller wants the full subordinate
 * report copied into -- the one-line `detail` bringup carries is enough to
 * show a user, and this is for a log.
 *
 * Returns false without changing the kernel file or guest RAM on any rejection.
 */
typedef struct {
    ios3_kernel_patch_status_t status;
    ios3_kernel_patch_report_t report;
    bool ran;
} ios3_bringup_gate_report_t;

bool ios3_bringup_gate(void *context,
                       const uint8_t *kernel_file,
                       size_t kernel_file_size,
                       uint8_t *ram,
                       size_t ram_size,
                       uint64_t ram_base,
                       uint32_t virt_base,
                       char *detail,
                       size_t detail_capacity);

/*
 * Fill in `request`'s gate hook and the four 7E18 SVC site addresses in one
 * place, so a frontend cannot arm the bridges against a kernel patched
 * somewhere else. `gate_report` may be NULL.
 */
void ios3_bringup_gate_configure(s5l_bringup_request_t *request,
                                 ios3_bringup_gate_report_t *gate_report);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_IOS3_BRINGUP_GATE_H */
