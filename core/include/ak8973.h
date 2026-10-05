/* AK8973 reset, configuration and factory EEPROM access.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_AK8973_H
#define S5LBOX_AK8973_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AK8973_MAX_TRANSFER 128u
#define AK8973_EEPROM_SIZE 7u
#define AK8973_POWERDOWN_NS 100000u
#define AK8973_EEPROM_START_NS 300000u
typedef struct {
    uint64_t powerdown_remaining, eeprom_remaining;
    uint8_t address, ms1, dac[3], gain[3];
    uint8_t eeprom[AK8973_EEPROM_SIZE], eeprom_known;
    bool initialized;
} ak8973_t;

/* Explicit powered device after a qualified RSTN pulse. CAD pins select 1C..1F.
 * EEPROM bits0..6 identify supplied bytes at62..68; unknown bytes remain unknown.
 * A nonzero mask requires seven readable bytes, copied into the device. These
 * inputs establish neither physical chip identity nor a measured boot state.
 * Initialization starts the documented power-down wait. No default attachment. */
bool ak8973_init(ak8973_t *d, bool cad1, bool cad0,
                 const uint8_t *eeprom, uint8_t known);
/* Apply a qualified hardware reset, preserving address pins and EEPROM. */
bool ak8973_reset(ak8973_t *d);
/* Decoded random-address transactions; caller accounts for I2C wire timing.
 * C0..C4, E0..E6 and EEPROM62..68 increment and wrap within their own bank.
 * C1..C4 expose reset values only: measurement mode is not implemented.
 * Configuration is accessible in power-down; MS1/status in either modeled mode.
 * EEPROM reads need read mode, elapsed startup time and known requested bytes.
 * Unsupported measurement/programming/test operations refuse, as do prohibited
 * settings or premature mode transitions. Refusal preserves device and output.
 * Data buffers must not overlap the device. No access advances elapsed time. */
bool ak8973_read(const ak8973_t *d, uint8_t address, uint8_t subaddress,
                 uint8_t *data, size_t size);
bool ak8973_write(ak8973_t *d, uint8_t address, uint8_t subaddress,
                  const uint8_t *data, size_t size);
bool ak8973_advance(ak8973_t *d, uint64_t nanoseconds);
#endif
