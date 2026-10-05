/* See ios3_bringup_gate.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "ios3_bringup_gate.h"

#include <stdio.h>
#include <string.h>

static bool apply_gate(bool extended, void *context,
                       const uint8_t *kernel_file,
                       size_t kernel_file_size,
                       uint8_t *ram,
                       size_t ram_size,
                       uint64_t ram_base,
                       uint32_t virt_base,
                       char *detail,
                       size_t detail_capacity) {
    ios3_bringup_gate_report_t *out = (ios3_bringup_gate_report_t *)context;
    ios3_kernel_patch_request_t request;
    ios3_kernel_patch_report_t report;
    ios3_kernel_patch_status_t status;

    memset(&request, 0, sizeof request);
    memset(&report, 0, sizeof report);
    request.kernel_file = kernel_file;
    request.kernel_file_size = kernel_file_size;
    request.ram = ram;
    request.ram_size = ram_size;
    request.ram_base = ram_base;
    request.virt_base = virt_base;
    request.extended_md_geometry = extended;

    status = ios3_kernel_patch_apply(&request, &report);
    if (out != NULL) {
        out->status = status;
        out->report = report;
        out->ran = true;
    }
    if (status == IOS3_KERNEL_PATCH_STATUS_OK) return true;

    /*
     * The site name is what makes this actionable rather than merely negative:
     * a digest mismatch means "this is a different kernel", while a site byte
     * mismatch on an otherwise-accepted digest would mean the image was
     * modified after it was measured.
     */
    if (detail != NULL && detail_capacity > 0u) {
        (void)snprintf(detail, detail_capacity,
                       "this is not the supported iPhone OS 3.1.3 kernel: %s "
                       "(site %s, va 0x%08llx)",
                       ios3_kernel_patch_status_string(status),
                       ios3_kernel_patch_site_string(report.site),
                       (unsigned long long)report.virtual_address);
        detail[detail_capacity - 1u] = '\0';
    }
    return false;
}

bool ios3_bringup_gate(void *context, const uint8_t *file, size_t file_size,
                      uint8_t *ram, size_t ram_size, uint64_t ram_base,
                      uint32_t virt_base, char *detail, size_t capacity) {
    return apply_gate(false, context, file, file_size, ram, ram_size, ram_base,
                      virt_base, detail, capacity);
}

static bool extended_gate(void *context, const uint8_t *file, size_t file_size,
                          uint8_t *ram, size_t ram_size, uint64_t ram_base,
                          uint32_t virt_base, char *detail, size_t capacity) {
    return apply_gate(true, context, file, file_size, ram, ram_size, ram_base,
                      virt_base, detail, capacity);
}

void ios3_bringup_gate_configure(s5l_bringup_request_t *request,
                                 ios3_bringup_gate_report_t *gate_report) {
    if (request == NULL) return;
    request->kernel_gate = ios3_bringup_gate;
    request->kernel_gate_context = gate_report;
    request->md_read_site_pc = IOS3_KERNEL_PATCH_MD_READ_VA;
    request->md_write_site_pc = IOS3_KERNEL_PATCH_MD_WRITE_VA;
    request->md_raw_site_pc = IOS3_KERNEL_PATCH_RAW_WATCHER_VA;
    request->uiomove_pc = IOS3_KERNEL_UIOMOVE_VA;
    request->md_geometry_sites = (md_geometry_sites_t){0};
    if (request->root_media &&
        request->root_media->size > MD_GEOMETRY_BOOTSTRAP_SIZE) {
        request->kernel_gate = extended_gate;
        request->md_geometry_sites = (md_geometry_sites_t){
            .register_pc = IOS3_KERNEL_PATCH_MD_REGISTER_VA,
            .bounds_pc = IOS3_KERNEL_PATCH_MD_BOUNDS_VA,
            .count64_pc = IOS3_KERNEL_PATCH_MD_COUNT64_VA,
            .count32_pc = IOS3_KERNEL_PATCH_MD_COUNT32_VA,
            .device_va = UINT32_C(0xc022d434),
            .map_pc = UINT32_C(0xc00740c2),
            .trim_pc = UINT32_C(0xc007428c),
            .eof_pc = UINT32_C(0xc007409e),
            .invalid_pc = UINT32_C(0xc0074096),
            .count64_done_pc = UINT32_C(0xc0073ebe),
            .count32_done_pc = UINT32_C(0xc0073f2c)
        };
    }
    request->packet_sites = (guest_packet_sites_t){
        .rx_pc = IOS3_KERNEL_PATCH_PACKET_RX_VA,
        .tx_pc = IOS3_KERNEL_PATCH_PACKET_TX_VA,
        .free_thumb_pc = IOS3_KERNEL_MBUF_FREE_VA,
        .tx_done_pc = IOS3_KERNEL_PACKET_TX_DONE_VA,
        .rx_drop_pc = IOS3_KERNEL_PACKET_RX_DROP_VA,
        .batch_pc = IOS3_KERNEL_PATCH_PACKET_BATCH_VA,
        .rx_enqueue_pc = IOS3_KERNEL_PACKET_RX_ENQUEUE_VA,
        .rx_unlock_pc = IOS3_KERNEL_PACKET_RX_UNLOCK_VA
    };
}
