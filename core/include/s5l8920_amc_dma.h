/* Bounded AMC internal-memory DMA.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_AMC_DMA_H
#define S5LBOX_S5L8920_AMC_DMA_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t offset, size; } s5l8920_amc_dma_region_t;
/* Host admission bound, not a claim about silicon descriptor capacity. */
#define S5L8920_AMC_DMA_MAX_DESCRIPTORS 64u
typedef struct {
    uint32_t offset, source, destination, length;
} s5l8920_amc_dma_descriptor_t;
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
    s5l8920_amc_dma_descriptor_t descriptor[S5L8920_AMC_DMA_MAX_DESCRIPTORS];
    unsigned descriptor_count, descriptor_index;
    uint32_t descriptor_copied;
    bool configured, active, stopped, completed;
} s5l8920_amc_dma_t;

/* Configure a zero object in an explicitly powered, idle context. The owner
 * supplies writable memory, disjoint backed regions and all three full status
 * words. Low three bits must be 0 (idle), 1..6 (active), 7 (stopped). The exact
 * active encoding and other status bits are inputs, not discovered defaults.
 * Memory must remain alive; guest CPU writes must use this same backing. The
 * owner must not change active source/destination bytes or configuration.
 * Copies within a chain may supply data consumed by later descriptors.
 * Identical repeats preserve progress; conflicts refuse. Host reset cancels
 * work, keeps configuration and all already copied memory, and restores idle. */
bool s5l8920_amc_dma_configure(s5l8920_amc_dma_t *d,
    const s5l8920_amc_dma_input_t *input);
void s5l8920_amc_dma_reset(s5l8920_amc_dma_t *d);

/* Channel0 only: aligned 32-bit status14, command10 (4=stop idle channel,
 * 10hex=reset stopped channel), and descriptor submission00. Submission uses
 * internal offsets tagged with low bits2 and ends at a zero next pointer.
 * Source/destination modes1/1 support zero flags/burst from the normal driver;
 * terminal descriptors also support flags20hex with burst0 or F80hex from its
 * DMA test. The role of flags20 on nonterminal links is unknown and refuses.
 * Each descriptor transfers1..16383 32-bit words. Up to64 descriptors may be
 * admitted, a host bound rather than inferred hardware capacity.
 * All descriptor/data spans must fit supplied regions. Cycles, overlapping
 * descriptor headers, data/header overlap, source/destination overlap within
 * one descriptor, external memory, other forms/channels and active stop/reset
 * refuse. Submission validates and latches the entire chain before any copy;
 * rejected later links leave state and memory unchanged. Data moves in link
 * order, including dependencies between descriptors. Metadata is latched;
 * owner modifications after admission cannot redirect pending work.
 * Status reads never service work. Unsupported operations are atomic refusals. */
bool s5l8920_amc_dma_read(const s5l8920_amc_dma_t *d,
    uint32_t offset, unsigned size, uint32_t *value);
bool s5l8920_amc_dma_write(s5l8920_amc_dma_t *d,
    uint32_t offset, unsigned size, uint32_t value);

/* Explicit available-clock service copies whole words, at most16 bytes and
 * no more than budget. Zero/subword budget or unavailable clock does nothing.
 * One call services at most the current descriptor. Idle/completed becomes
 * visible only after every byte of the last descriptor is copied. This is a
 * functional bound, not measured silicon timing. completed is a host event;
 * no IRQ cause/register/output, board mapping or clock source is supplied. */
unsigned s5l8920_amc_dma_service(s5l8920_amc_dma_t *d,
    bool clock_available, unsigned budget);
#endif
