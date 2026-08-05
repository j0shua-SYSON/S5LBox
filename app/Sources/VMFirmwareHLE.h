/*
 * Experimental iPhone OS 3 userspace raster HLE for the firmware app path.
 *
 * This is deliberately not a general accelerator switch.  The implementation
 * accepts only tools/ios3_hle.c's exact 3.1.3 prologues, binds them to the
 * first matching userspace address space, and declines every unproved call.
 * The normal IPA does not call this API; the manual experimental build does.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef VM_FIRMWARE_HLE_H
#define VM_FIRMWARE_HLE_H

#include "soc.h"

#include <stdbool.h>

bool vm_firmware_hle_enable(s5l8900_t *machine);
void vm_firmware_hle_release(const s5l8900_t *machine);
bool vm_firmware_hle_active(const s5l8900_t *machine);
uint32_t vm_firmware_hle_space(const s5l8900_t *machine);

#endif /* VM_FIRMWARE_HLE_H */
