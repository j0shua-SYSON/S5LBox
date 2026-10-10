// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#ifndef VM_FIRMWARE_AUDIO_H
#define VM_FIRMWARE_AUDIO_H
#include "soc.h"

/* Call only at the authenticated 7E18 boot/restore boundary, before running
 * the CPU. Firmware-neutral cores and other firmware frontends stay unchanged.
 * Does not edit firmware/disk files or disable any execution engine. */
void vm_firmware_audio_enable(s5l8900_t *machine);
#endif
