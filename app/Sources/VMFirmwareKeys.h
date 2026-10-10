/* Public, build-specific metadata only. No Apple firmware is included. */
#ifndef S5LBOX_VM_FIRMWARE_KEYS_H
#define S5LBOX_VM_FIRMWARE_KEYS_H
#include "VMFirmwareImport.h"

/* Match the complete manifest identity, never the IPSW's filename. Unknown or
 * incomplete identities leave out untouched. Explicit caller keys can override
 * individual components after resolution; they are never added to this table. */
bool vm_fw_resolve_public_keys(const vm_fw_report_t *identity, vm_fw_keys_t *out);
const char *vm_fw_public_keys_source(void);
#endif
