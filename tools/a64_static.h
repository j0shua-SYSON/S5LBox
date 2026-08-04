/*
 * S5LBox -- signed static AArch64 semantic-thread contract.
 *
 * The native handlers are ordinary build-time-generated, signed text. Runtime
 * decoding creates only sixteen-byte data records (handler id, immediate,
 * PC operand and metadata); it never emits code and never requests writable
 * executable memory.
 *
 * This remains a deliberately bounded semantic subset. The benchmark uses the
 * complete contract, while the optional SoC engine accepts only the part it
 * can prove exact against translated guest RAM and device-time boundaries.
 */
#ifndef S5LBOX_A64_STATIC_H
#define S5LBOX_A64_STATIC_H

#include "arm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define A64_STATIC_MAX_INSNS 16u
/* A conditional register-offset A32 load uses a guard, shifter, address and
 * read record. The final slot is the fixed block exit. */
#define A64_STATIC_MAX_UOPS (A64_STATIC_MAX_INSNS * 4u + 1u)
#define A64_STATIC_HANDLER_COUNT 24646u

typedef struct {
    uint32_t handler;
    uint32_t immediate;
    uint32_t pc_value;
    uint32_t metadata;
} a64_static_uop_t;

typedef struct {
    a64_static_uop_t uops[A64_STATIC_MAX_UOPS];
    unsigned insn_count;
    unsigned uop_count;
    uint32_t start_pc;
    uint32_t exit_pc;
    bool thumb;
    bool dynamic_exit;
    bool touches_memory;
    bool direct_reads;
    bool runtime_guards;
    bool vfp;
} a64_static_block_t;

/* Decode one host-native uint32_t/uint16_t instruction array beginning at
 * `pc`. A terminal A32 immediate B/BL may target any word-aligned address.
 * Conditional branches and every BL carry both their taken target and natural
 * fallthrough in a dynamic-exit record; exit_pc names that fallthrough. A
 * terminal unconditional B retains the compact fixed-exit form. A32 data
 * processing covers every opcode, condition and immediate/register
 * barrel-shifter form with r0-r14 destinations and the architecturally valid
 * source registers; writes to PC remain outside the contract. Thumb covers
 * its broad shift, small/immediate and register ALU, non-PC high-register,
 * PC/SP address and SP-adjust forms; flat-proof SP-relative word loads/stores
 * remain available. Every unsupported bit causes a clean false return. */
bool a64_static_decode_at(const void *program, unsigned insns, bool thumb,
                          uint32_t pc, a64_static_block_t *out);

/* Same contract for an unaligned guest little-endian byte stream. This is the
 * entry point for translated machine RAM; keeping it distinct preserves the
 * benchmark-array API on a big-endian host. */
bool a64_static_decode_bytes_at(const uint8_t *program, unsigned insns,
                                bool thumb, uint32_t pc,
                                a64_static_block_t *out);

/* Product decoder for the real SoC path. It admits the non-memory contract and
 * replaces exact A32 pre-indexed, no-writeback loads and ordinary Thumb
 * PC/register/immediate/SP-relative loads with read-cache records. Thumb word,
 * byte, halfword, signed-byte and signed-halfword results are covered; dynamic
 * alignment cases refuse at runtime before touching guest state. Exact VFPv2
 * core/register transfers, VMRS/VMSR, raw VMOV/VABS/VNEG and VCMP/VCMPE are
 * also admitted behind live CPACR/FPEXC/FPSCR guards. Compare and exact
 * single-to-double VCVT widening handle NaNs, signed zero, FZ and cumulative
 * IOC/IDC with integer bit logic. Single-register VLDR uses the same
 * already-proved plain-RAM read cache, including exact one- and two-word hit
 * accounting. Flat-proof memory handlers, stores, PC loads,
 * writeback/post-index forms and every other unsupported instruction refuse
 * cleanly. Runtime cache misses or VFP guard failures still return to arm_step(),
 * which alone owns exceptions, MMU walks, faults and MMIO. */
bool a64_static_decode_read_hits_bytes_at(const uint8_t *program,
                                          unsigned insns, bool thumb,
                                          uint32_t pc,
                                          a64_static_block_t *out);

/* Compatibility shorthand for a block beginning at address zero. */
bool a64_static_decode(const void *program, unsigned insns, bool thumb,
                       a64_static_block_t *out);

/* True only when the target was built with the generated AArch64 handler file. */
bool a64_static_host_available(void);

/* Execute against flat, power-of-two RAM. A non-loop block may be executed once;
 * repeated execution is accepted only when its exit PC equals its start PC.
 * The wrapper is intentionally narrower than arm_run: no MMU, faults, MMIO,
 * timer or IRQ path exists in this proof. */
bool a64_static_run(arm_cpu_t *cpu, const a64_static_block_t *block,
                    uint64_t blocks, uint8_t *ram, size_t ram_size);

/* Execute one product-decoded block against the CPU's existing data-read
 * cache. On a cache miss, `completed` is the exact prefix retired before the
 * load (possibly zero), PC names that load, and no miss/fault/MMIO side effect
 * has occurred; the caller must resume with arm_step(). A false return is a
 * pre-execution contract refusal and leaves guest state unchanged. */
bool a64_static_run_read_hits(arm_cpu_t *cpu,
                              const a64_static_block_t *block,
                              uint8_t *ram, size_t ram_size,
                              unsigned *completed);

/* Execute an unmodified block returned by
 * a64_static_decode_read_hits_bytes_at(). This retains the dynamic CPU/PC,
 * RAM and final-record checks but skips the full per-uop structural rescan;
 * callers must own the decoded block and must not mutate it. It exists so a
 * decode cache can validate structure once rather than on every hot hit. */
bool a64_static_run_read_hits_decoded(arm_cpu_t *cpu,
                                      const a64_static_block_t *block,
                                      uint8_t *ram, size_t ram_size,
                                      unsigned *completed);

#endif /* S5LBOX_A64_STATIC_H */
