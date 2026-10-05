/* SST25VF080B serial flash, decoded byte interface.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_SST25VF080B_H
#define S5LBOX_SST25VF080B_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SST25VF080B_SIZE UINT32_C(0x100000)
#define SST25VF080B_SECTOR_SIZE UINT32_C(0x1000)
#define SST25VF080B_KNOWN_WORDS 8u

typedef struct {
    uint64_t program_ns, sector_ns, block32_ns, block64_ns, chip_ns;
} sst25vf080b_timing_t;

/* Undriven bits have no received value. The bus must supply its own electrical
 * bias before using them as controller input. No implicit pull-up or zero. */
typedef struct { uint8_t value, driven; } sst25vf080b_output_t;

typedef struct {
    uint8_t *image;
    sst25vf080b_timing_t timing;
    uint64_t powerup_ns, busy_ns;
    uint32_t address, aai_next, operation_address, operation_size;
    uint8_t status, command, position, data[2], operation_data[2], operation;
    bool initialized, selected, wp_high, hold_high;
    bool ewsr, status_authorized, busy_output, aai_continuation;
    bool array_unavailable;
    uint32_t known_sectors[SST25VF080B_KNOWN_WORDS];
} sst25vf080b_t;

/* Cold power-on with CE high. Borrow exactly 1 MiB, never initialize, allocate,
 * free or persist it. Caller owns its lifetime and any file/snapshot isolation.
 * Status powers up to 1C; access waits 100 us after valid supply. Explicit
 * durations must be nonzero and within DS20005045D maximums: program10us,
 * sector/block25ms, chip50ms. No N88 identity or warm-handoff state is inferred.
 * Reinitializing during a pending operation is not a power-loss simulation. */
bool sst25vf080b_init(sst25vf080b_t *f, uint8_t *image, size_t size,
    const sst25vf080b_timing_t *timing, bool wp_high, bool hold_high);
/* Explicitly model this candidate chip without any known array contents.
 * ID/status/pins retain device semantics; array reads and authorized storage
 * mutations refuse transactionally. No blank image, erased bytes or persistent
 * state is invented. Existing image-backed initialization still requires 1 MiB.
 * This is not evidence of the actual board's chip identity or warm state. */
bool sst25vf080b_init_unbacked(sst25vf080b_t *f,
    const sst25vf080b_timing_t *timing, bool wp_high, bool hold_high);
/* Borrow a 1 MiB image with explicit availability per 4 KiB erase sector.
 * Copy eight mask words: bit (sector%32) of word (sector/32) means every byte
 * in that sector is supplied. Unknown reads/programs refuse even if the
 * backing bytes happen to be FF. Completed erases establish known FF bytes
 * for their whole extent; pending erases do not. No data is filled at init.
 * The caller still owns backing storage and persistence. This does not create
 * NVRAM, import firmware, or establish any target board's stored contents. */
bool sst25vf080b_init_partial(sst25vf080b_t *f, uint8_t *image, size_t size,
    const uint32_t known_sectors[SST25VF080B_KNOWN_WORDS],
    const sst25vf080b_timing_t *timing, bool wp_high, bool hold_high);
/* Pins supplied at byte boundaries, with HOLD changes already qualified at
 * SCK low by the caller. Selecting during power-up refuses. Deasserting CE
 * commits complete commands, discards incomplete commands and releases SO.
 * Repeating a pin level creates no edge. No partial-byte/analog timing model. */
bool sst25vf080b_pins(sst25vf080b_t *f, bool ce_high, bool wp_high, bool hold_high);
/* One complete MSB-first byte in mode0/3, after electrical timing validation.
 * Deselected/held flash drives nothing and consumes nothing. Unknown opcodes,
 * excess fixed-command bytes, JEDEC output beyond its specified three bytes,
 * invalid legacy-ID addresses and unsupported traffic during BUSY/AAI refuse
 * without changing state/output. Incomplete fixed commands have no effect at
 * CE rise. EWSR authorizes only the immediately following WRSR command.
 * Read/status streams continue until CE rises. No serial call advances time. */
bool sst25vf080b_transfer(sst25vf080b_t *f, uint8_t input,
    sst25vf080b_output_t *output);
/* Asynchronous SO level only for EBSY/AAI ready/busy mode; otherwise undriven.
 * Samples without shifting commands, permitting readiness checks without SCK. */
bool sst25vf080b_ready_pin(const sst25vf080b_t *f, sst25vf080b_output_t *output);
/* Real elapsed device time, independent of SPI periods and CPU instruction
 * counts. Program/erase changes the borrowed image only when BUSY expires.
 * Polling does not complete an operation. WRDI does not cancel a pending write.
 * Reaching the AAI limit clears WEL; WRDI exits AAI mode, including ready output.
 * Undefined attempts to program non-erased bytes refuse at CE rise. */
bool sst25vf080b_advance(sst25vf080b_t *f, uint64_t nanoseconds);
#endif
