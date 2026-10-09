/* S5LBox -- bounded, untrusted guest IPA inspection. MIT licensed. */
#ifndef S5LBOX_VM_IPA_ARCHIVE_H
#define S5LBOX_VM_IPA_ARCHIVE_H
#include "VMFirmwareFormats.h"

#define VM_IPA_MAX_FILES 4096u
#define VM_IPA_MAX_BYTES (256u * 1024u * 1024u)
#define VM_IPA_MAX_FILE_BYTES (128u * 1024u * 1024u)

typedef struct {
    vmfw_zip_entry_t *files;
    unsigned count;
    unsigned info_index;
    uint64_t unpacked_bytes;
    char app_root[VMFW_ZIP_MAX_NAME];
} vm_ipa_archive_t;

/* No extraction or disk writes. Free a successful plan with close. */
bool vm_ipa_archive_open(const vmfw_zip_t *zip, vm_ipa_archive_t *plan,
                         char *detail, size_t capacity);
void vm_ipa_archive_close(vm_ipa_archive_t *plan);
bool vm_ipa_safe_path(const char *path);
/* Main executable must have an unencrypted ARMv6-compatible Mach-O slice,
 * with a deployment target no newer than iPhone OS 3.1.3. */
bool vm_ipa_check_executable(const uint8_t *bytes, size_t size,
                             char *detail, size_t capacity);
#endif
