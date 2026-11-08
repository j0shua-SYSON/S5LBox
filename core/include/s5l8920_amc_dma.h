/* Bounded AMC internal-memory DMA.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_AMC_DMA_H
#define S5LBOX_S5L8920_AMC_DMA_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t offset, size; } s5l8920_amc_dma_region_t;
typedef struct {
    uint8_t *memory;
    uint32_t memory_size;
    s5l8920_amc_dma_region_t region[3];
    unsigned region_count;
    uint32_t idle_status, active_status, stopped_status;
} s5l8920_amc_dma_input_t;
typedef struct {
    s5l8920_amc_dma_input_t input;
    uint32_t source, destination, length, copied;
    bool configured, active, stopped, completed;
} s5l8920_amc_dma_t;

/* Configure a zero object in an explicitly powered, idle context. The owner
 * supplies writable memory, disjoint backed regions and all three full status
 * words. Low three bits must be 0 (idle), 1..6 (active), 7 (stopped). The exact
 * active encoding and other status bits are inputs, not discovered defaults.
 * Memory must remain alive; guest CPU writes must use this same backing. The
 * owner must not change active source/destination bytes or configuration.
 * Identical repeats preserve progress; conflicts refuse. Host reset cancels
 * work, keeps configuration and all already copied memory, and restores idle. */
bool s5l8920_amc_dma_configure(s5l8920_amc_dma_t *d,
    const s5l8920_amc_dma_input_t *input);
void s5l8920_amc_dma_reset(s5l8920_amc_dma_t *d);

/* Channel0 only: aligned 32-bit status14, command10 (4=stop idle channel,
 * 10hex=reset stopped channel), and descriptor submission00. Submission uses
 * an internal offset with low bits2, a terminal next pointer0, internal source
 * and destination modes1/1, flags20hex, burst field0 or F80hex, and 1..16383
 * 32-bit words. These two forms come from the matching driver's DMA test.
 * All descriptor/data spans must fit supplied regions. Overlap, chaining,
 * external memory, other modes/flags/channels and active stop/reset refuse.
 * Submission validates and latches the 16-byte descriptor, but copies nothing.
 * Status reads never service work. Unsupported operations are atomic refusals. */
bool s5l8920_amc_dma_read(const s5l8920_amc_dma_t *d,
    uint32_t offset, unsigned size, uint32_t *value);
bool s5l8920_amc_dma_write(s5l8920_amc_dma_t *d,
    uint32_t offset, unsigned size, uint32_t value);

/* Explicit available-clock service copies whole words, at most16 bytes and
 * no more than budget. Zero/subword budget or unavailable clock does nothing.
 * Idle/completed becomes visible only after every byte is copied. This is a
 * functional bound, not measured silicon timing. completed is a host event;
 * no IRQ cause/register/output, board mapping or clock source is supplied. */
unsigned s5l8920_amc_dma_service(s5l8920_amc_dma_t *d,
    bool clock_available, unsigned budget);
#endif
