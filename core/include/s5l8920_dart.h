/* Bounded H2P DART programming and page translation for N88.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_DART_H
#define S5LBOX_S5L8920_DART_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S5L8920_DART_BASE UINT32_C(0xbfe00000)
#define S5L8920_DART_STRIDE UINT32_C(0x100000)
#define S5L8920_DART_COUNT 2u
#define S5L8920_DART_DVA_BASE UINT32_C(0x3c000000)
#define S5L8920_DART_DVA_SIZE UINT32_C(0x04000000)
#define S5L8920_DART_SEGMENTS 16u

typedef struct {
    uint32_t initial_config, initial_command, initial_table_port;
    uint32_t config, segment[S5L8920_DART_SEGMENTS];
    uint16_t programmed;
    bool configured, command_readable, table_port_readable;
} s5l8920_dart_t;

/* Explicit initial observations, not hardware reset defaults. Initial config
 * must disable translation. Identical repeats preserve all guest state;
 * conflicting input refuses. Reset restores these observations but invalidates
 * every segment entry. A zero object has no readable registers or mappings. */
bool s5l8920_dart_configure(s5l8920_dart_t *d, uint32_t config,
                           uint32_t command, uint32_t table_port);
void s5l8920_dart_reset(s5l8920_dart_t *d);

/* Aligned word offsets. Config0c retains the original driver's fields0..6/31,
 * preserving supplied opaque bits. Table port08 programs one segment: address
 * bits12..27, selector8..11, valid0, preserving the observed upper nibble.
 * Table writes require disabled translation. Low11 command bits702 flush TLBs;
 * the functional walker is uncached, so there is no stale translation to retain.
 * No cache topology, completion delay or DMA completion is inferred.
 *
 * A write INVALIDATES the corresponding command/table-port read observation.
 * Post-write readback, indexed data04, status10, miss counter14 and other
 * commands remain refused: the driver alone does not establish their values.
 * No last-written command is silently presented as hardware readback. Config
 * RMW retains its programmed value. Rejections preserve state/output. */
bool s5l8920_dart_read(const s5l8920_dart_t *d, uint32_t offset, uint32_t *value);
bool s5l8920_dart_write(s5l8920_dart_t *d, uint32_t offset, uint32_t value);

typedef enum {
    S5L8920_DART_OK = 0,
    S5L8920_DART_BAD_REQUEST,
    S5L8920_DART_UNCONFIGURED,
    S5L8920_DART_DISABLED,
    S5L8920_DART_UNKNOWN_SEGMENT,
    S5L8920_DART_INVALID_SEGMENT,
    S5L8920_DART_TABLE_UNAVAILABLE,
    S5L8920_DART_INVALID_PAGE,
    S5L8920_DART_UNSUPPORTED_PAGE,
    S5L8920_DART_TARGET_UNAVAILABLE
} s5l8920_dart_result_t;

/* Translate a nonempty span confined to one4KiB page, from the64MiB DVA window
 * into RAM at40000000. Each segment points to1024 little-endian32-bit PTEs.
 * Supported PTEs have valid0 and physical address12..27; all other bits refuse.
 * RAM backing may cover a prefix of the256MiB physical window. Both the table
 * read and resulting byte span must fit. Physical40000000 is a valid page.
 * No output or RAM is changed on failure; no host pointer is retained.
 * Results are HOST diagnostics, not invented fault-status/IRQ encodings.
 * Clients must split page crossings and handle failures before transferring;
 * no peripheral DMA client, fault IRQ, counters or cache timing is connected. */
s5l8920_dart_result_t s5l8920_dart_translate(const s5l8920_dart_t *d,
    const uint8_t *ram, size_t ram_size, uint32_t address, size_t size,
    uint32_t *physical);
#endif
