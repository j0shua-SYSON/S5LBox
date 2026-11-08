/* Bounded AMC r2 software control registers.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_AMC_H
#define S5LBOX_S5L8920_AMC_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t start_address, start_status, interrupt_mask[2];
} s5l8920_amc_control_input_t;

typedef struct {
    s5l8920_amc_control_input_t initial, current;
    bool configured;
} s5l8920_amc_control_t;

/* Standalone, powered/idle component; no board attachment or inferred reset
 * defaults. The owner supplies all four initial read words, including stable
 * readback bits outside the supported fields. Configure a zero object once;
 * identical repeats preserve current state, conflicting inputs refuse.
 * Host reset restores the supplied words. Guest engine/reset commands remain
 * unsupported; no clock, DSP, memory-window, FIFO or interrupt output exists. */
bool s5l8920_amc_control_configure(s5l8920_amc_control_t *d,
    const s5l8920_amc_control_input_t *input);
void s5l8920_amc_control_reset(s5l8920_amc_control_t *d);

/* Aligned 32-bit offsets only. Start address988 supports low16 programming;
 * 990/994 accept command1 to set/clear status998 bit0. Interrupt mask0 has
 * set/clear/read A8C/A90/A94 (low30), mask1 B0C/B10/B14 (all32). Set/clear
 * operations affect only written one bits. Unsupported fields, directions,
 * widths and addresses refuse without changing state or the read output.
 * These masks store software requests; they do not create pending events,
 * acknowledge causes, assert an IRQ, or start/complete an engine program. */
bool s5l8920_amc_control_read(const s5l8920_amc_control_t *d,
    uint32_t offset, unsigned size, uint32_t *value);
bool s5l8920_amc_control_write(s5l8920_amc_control_t *d,
    uint32_t offset, unsigned size, uint32_t value);
#endif
