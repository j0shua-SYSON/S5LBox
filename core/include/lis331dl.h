/* LIS331DL identification and configuration through decoded I2C transactions.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_LIS331DL_H
#define S5LBOX_LIS331DL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIS331DL_MAX_TRANSFER 128u
typedef struct {
    uint64_t reboot_ns, reboot_remaining;
    uint8_t control[3], wake_config[2], wake_threshold[2], wake_duration[2];
    bool initialized, sdo_high;
} lis331dl_t;

/* Explicit cold, powered and initially calibrated device in I2C mode (CS high).
 * SDO selects address1C/1D. Caller supplies a nonzero calibration-reload duration;
 * no default duration, physical chip identity or prior boot state is inferred.
 * CTRL_REG1 starts at07, remaining implemented configuration registers at00.
 * This does not model power loss or make initial acceleration samples known. */
bool lis331dl_init(lis331dl_t *d, bool sdo_high, uint64_t reboot_ns);
/* Transaction boundary, after the caller accounts for bus timing. Subaddress
 * bit7 increments the register; without it, repeated bytes access one register.
 * ID0F, controls20..22 and wake configuration30/32..34/36..37 are implemented.
 * Reserved bits/addresses, writes to ID and acceleration/status/source/filter
 * reads refuse. Neither a sample nor a sensor interrupt is synthesized.
 * Writes during calibration reload refuse; configuration and ID remain readable.
 * A rejected transaction preserves the entire device and output buffer. The
 * buffers must not overlap the device. No transaction advances elapsed time. */
bool lis331dl_read(const lis331dl_t *d, uint8_t address, uint8_t subaddress,
                  uint8_t *data, size_t size);
bool lis331dl_write(lis331dl_t *d, uint8_t address, uint8_t subaddress,
                   const uint8_t *data, size_t size);
/* Elapsed time only drives the configured trim reload: BOOT stays set until
 * expiry, then clears without resetting user configuration. Motion, sample
 * conversion, interrupt generation, SPI and analog behavior remain unavailable. */
bool lis331dl_advance(lis331dl_t *d, uint64_t nanoseconds);
#endif
