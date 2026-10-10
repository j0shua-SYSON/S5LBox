/* Public firmware key catalog. Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * Facts transcribed from The Apple Wiki, revision 206002 (2024-01-23):
 * https://theapplewiki.com/index.php?title=Keys:SUNorthstarTwo_7E18_(iPhone1,2)&oldid=206002
 * Only the three components used by S5LBox are listed. Rootfs uses the published
 * AES+HMAC blob; its per-block IV is derived by the DMG reader, not a fixed IV.
 * Add builds as separate records with independent identity and boot validation.
 */
#include "VMFirmwareKeys.h"
#include <string.h>

typedef struct {
    const char *product, *version, *build, *board, *platform;
    const char *kernel_key, *kernel_iv, *tree_key, *tree_iv, *root_key;
} public_keys_t;

static const public_keys_t catalog[] = {
    { "iPhone1,2", "3.1.3", "7E18", "n82ap", "s5l8900x",
      "d0dfac22c03212f8a75fc9c69fe548b6", "31e711201cf4dcf47be5be2a5b1b87a1",
      "9532919c4b4ff636f0559ff25be64f35", "77cbb8d3e874efa1364cab1bbd38a8fc",
      "bf5eb72cd65e9c37cf9920707cb6b4f7ecc10b38cfec6b167002ac9fd6a3ab6643e45005" }
};

const char *vm_fw_public_keys_source(void) {
    return "https://theapplewiki.com/index.php?title=Keys:SUNorthstarTwo_7E18_(iPhone1,2)&oldid=206002";
}

bool vm_fw_resolve_public_keys(const vm_fw_report_t *id, vm_fw_keys_t *out) {
    if (!id || !out || !id->manifest_read) return false;
    for (size_t i = 0; i < sizeof catalog / sizeof catalog[0]; ++i) {
        const public_keys_t *record = &catalog[i];
        if (strcmp(id->product_type, record->product) ||
            strcmp(id->product_version, record->version) ||
            strcmp(id->build, record->build) || strcmp(id->board, record->board) ||
            strcmp(id->platform, record->platform)) continue;
        vm_fw_keys_t keys;
        vm_fw_keys_clear(&keys);
        bool valid = vm_fw_keys_set_img3(&keys, VM_FW_KERNEL,
                         record->kernel_key, record->kernel_iv) == VM_FW_OK &&
                     vm_fw_keys_set_img3(&keys, VM_FW_DEVICE_TREE,
                         record->tree_key, record->tree_iv) == VM_FW_OK &&
                     vm_fw_keys_set_root(&keys, record->root_key) == VM_FW_OK;
        if (valid) *out = keys;
        vm_fw_keys_clear(&keys);
        return valid;
    }
    return false;
}
