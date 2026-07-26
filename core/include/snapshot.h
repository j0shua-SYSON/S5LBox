/*
 * iOS3-VM — full machine snapshot / restore.
 *
 * WHY THIS EXISTS. Reaching the current frontier of the boot costs ~2 billion
 * interpreted instructions, i.e. minutes of wall clock per debugging iteration.
 * docs/dynarec.md §11.2 concludes that the fix for that loop is snapshotting,
 * not a JIT: "Serialise arm_cpu_t, guest RAM, and every device's state to a
 * file, and restore it." This is that.
 *
 * WHAT IT PROMISES. A restored machine is bit-for-bit the machine that was
 * saved: every CPU register including all banked modes, CPSR and the SPSRs,
 * the whole cp15 struct, the exclusive monitor, the VFP control registers,
 * guest RAM, the NOR contents and its scanned directory, every device's
 * registers and counters and pending-interrupt state, and the machine's own
 * diagnostic counters. Continuing a restored machine must produce exactly the
 * output continuing the original would have produced; if it does not, this
 * file has a missing field and that is a bug, not a tolerance.
 *
 * WHAT IT DOES NOT COVER. Only the emulated machine (`s5l8900_t`). Host-side
 * state belonging to a *tool* — bootkernel's trace ring, its milestone hit
 * counts, its sampled profile — is not machine state and is not saved; a
 * restored process starts those counters fresh. Nothing the guest can observe
 * lives there.
 *
 * FAILURE POLICY. This core's rule is "trap what you don't implement, never
 * guess". A snapshot that half-loads is worse than one that refuses, because
 * the divergence it causes surfaces a billion instructions later. So: the
 * magic, the version, the payload length and a checksum over the whole payload
 * are all verified BEFORE any byte of the machine is touched, and any failure
 * is reported rather than papered over.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_SNAPSHOT_H
#define IOS3VM_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>
#include "soc.h"

/* 16 bytes, stored without a terminator. */
#define SNAPSHOT_MAGIC     "iOS3-VM SNAPSHOT"
#define SNAPSHOT_MAGIC_LEN 16u

/*
 * Bump this whenever the serialised form changes in any way — a new field, a
 * reordered field, a changed section. There is deliberately no compatibility
 * shim: an old snapshot restored into a newer emulator would be a machine with
 * one register quietly holding the wrong value, which is the single most
 * expensive class of bug this feature exists to prevent.
 */
/* v2: the VFP register file (arm_cpu_t.vfp_s, s0-s31 aliasing d0-d15) joined
 *     the CPU section when real VFPv2 arithmetic was implemented. */
/* v3: both I2C controllers and the PCF50635 PMU/RTC joined MACH. Old
 *     checkpoints cannot safely invent an in-flight transfer or RTC state. */
/* v4: the three-bank TV-out controller and its VSYNC phase joined MACH. */
/* v5: the Synopsys DWC2 USB OTG block's PCGCCTL joined MACH. Its GHWCFG straps
 *     are build constants rather than state, so PCGCCTL is the whole of it. */
/* v6: the two SPI controllers joined MACH, with their transmit and receive
 *     FIFOs. A transfer can be in flight across a checkpoint — that is the
 *     whole point of the model, since the guest sleeps inside one — so the FIFO
 *     bytes and levels are state and the payload genuinely grows. The stub
 *     windows for spi0 and spi1 also disappear from GEOM, which a v5 file would
 *     not agree with either. */
/* v7: the two halves of /arm-io/gpio and the multi-touch controller joined
 *     MACH. This one could not have been avoided by any encoding choice: the
 *     GPIO interrupt controller's pending latch and enable masks are guest
 *     state that decides whether a touch report can reach the CPU, the pin
 *     block is 4 KiB of guest-written levels one of which is the touch
 *     controller's reset line, and the Z2's own protocol position — which
 *     packet it is part way through, and whether it is still answering as its
 *     bootloader — is state a checkpoint cannot invent. The `gpio` and
 *     `gpioic` stub windows also disappear from GEOM, so a v6 file would fail
 *     the geometry check even if the payload happened to line up. */
/* v8: the touch controller's HBPP bookkeeping changed shape. One `hbpp` bool
 *     became `in_reset` plus a monotonic `hbpp_answered`, and a `reset_bytes`
 *     counter joined it. The byte format therefore differs even where the host
 *     ABI size does not, and a v7 file restored into a v8 build would land a
 *     stale claim in the wrong field -- which decides whether the driver stays
 *     attached or pushes 54 KB of firmware at a device that cannot take it. */
/* v9: the touch controller gained a report to deliver. The pending payload,
 *     its length, the frame sequence number, the framer's latched tx[2] and
 *     the power-line level all joined MACH, and the reply buffer grew from 40
 *     bytes to MTZ2_PAYLOAD_LIMIT + 7 because a data read is L + 5 bytes long.
 *     No encoding choice could have avoided this: a checkpoint taken between
 *     the length read and the data read has already told the guest a length,
 *     and a v8 file has no payload to answer the second half of that exchange
 *     with. Every field after the Z2's in MACH also shifts, so a v8 file read
 *     as v9 would misparse the USB OTG block onwards rather than fail. */
#define SNAPSHOT_VERSION   9u

typedef enum {
    SNAP_OK = 0,
    SNAP_ERR_IO,         /* could not open / read / write the file           */
    SNAP_ERR_MAGIC,      /* not a snapshot at all                            */
    SNAP_ERR_VERSION,    /* written by a different version of this format    */
    SNAP_ERR_TRUNCATED,  /* the file ends before the payload does            */
    SNAP_ERR_CHECKSUM,   /* payload does not match its recorded hash         */
    SNAP_ERR_CORRUPT,    /* section framing or a field value is nonsense     */
    SNAP_ERR_GEOMETRY,   /* the machine's RAM/NOR/stub layout does not match */
    SNAP_ERR_NOMEM
} snapshot_status_t;

const char *snapshot_strerror(snapshot_status_t st);

/*
 * Write the entire machine to `path`. The machine is not modified. The bytes
 * are first completed in the same directory and then atomically replace the
 * destination, so a failed save leaves an earlier checkpoint intact.
 */
snapshot_status_t snapshot_save(const s5l8900_t *m, const char *path);

/*
 * Restore `m` from `path`. `m` must already be a live machine built by
 * s5l8900_init() with the SAME ram_base/ram_size and the same set of stub
 * windows; a mismatch is refused with SNAP_ERR_GEOMETRY rather than silently
 * resized, because the alternative is a machine whose physical map does not
 * match the addresses baked into the guest's page tables.
 *
 * Host-owned pointers (RAM allocation, NOR allocation, stub backing stores,
 * the bus vtable and cpu->bus) are preserved; only their CONTENTS are
 * overwritten. That is what lets a tool interpose on the bus (as bootkernel
 * does) and still restore underneath it.
 *
 * Malformed data, checksum failures and geometry mismatches are rejected
 * before the applying pass and leave the machine untouched. A genuine file
 * read failure (or external in-place modification) during that final pass can
 * leave contents partially applied; in-memory loads are transactional for all
 * malformed inputs.
 */
snapshot_status_t snapshot_load(s5l8900_t *m, const char *path);

/* In-memory forms, used by the tests. `*out` is malloc'd and owned by the
 * caller. Identical byte stream to the file forms. */
snapshot_status_t snapshot_save_mem(const s5l8900_t *m,
                                    uint8_t **out, size_t *out_len);
snapshot_status_t snapshot_load_mem(s5l8900_t *m,
                                    const uint8_t *buf, size_t len);

#endif /* IOS3VM_SNAPSHOT_H */
