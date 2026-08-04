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
/* One decoded head remains capped at sixteen instructions. A callback-free
 * invocation may visit several already-validated heads, but this separate
 * ceiling bounds its 32-bit accounting even when the SoC timebase edge is
 * farther away. The generic engine keeps the historical sixteen-instruction
 * default; the iOS product explicitly selects this ceiling after a same-binary
 * Apple-host end-to-end gate. */
#define A64_STATIC_MAX_CHAIN_INSNS 256u
/* A conditional register-offset A32 load/store uses a guard, shifter, address
 * and memory record. The final slot is the fixed block exit. */
#define A64_STATIC_MAX_UOPS (A64_STATIC_MAX_INSNS * 4u + 1u)
#define A64_STATIC_HANDLER_COUNT 26198u
#define A64_STATIC_GRAPH_SLOTS 512u

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
    bool direct_writes;
    bool runtime_guards;
    bool vfp;
} a64_static_block_t;

/* Fixed-layout, data-only head descriptor for callback-free signed chaining.
 * The owner pointer is never dereferenced by generated assembly; it lets the C
 * cache invalidate a descriptor before its inline block storage is reused.
 * A 128-byte stride keeps the native lookup independent of private cache-entry
 * layout and makes every field offset compile-time auditable. */
typedef struct {
    const void *owner;
    const uint8_t *fetch_host;
    const a64_static_uop_t *uops;
    uint32_t pc;
    uint32_t fetch_gen;
    uint32_t insn_count;
    uint32_t raw_len;
    uint8_t raw[A64_STATIC_MAX_INSNS * 4u];
    uint8_t fetch_priv;
    uint8_t thumb;
    uint8_t valid;
    uint8_t supported;
    uint8_t reserved[20];
} a64_static_graph_node_t;

/* A product chain validates its next guest PC outside the generated handler
 * text. The callback may return only an immutable block owned by its caller;
 * NULL ends the chain after the block that just completed. */
typedef const a64_static_block_t *(*a64_static_chain_next_fn)(
    void *opaque, uint32_t pc, unsigned remaining);

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

/* Superset of the read-hit product decoder. In addition to the exact guarded
 * read/VFP subset above, this admits bounded A32 STR/STRB addressing-mode-2
 * forms and ordinary Thumb STR/STRB/STRH forms when, and only when, the store
 * is the final semantic instruction in the decoded head. A hit uses the CPU's
 * separately consent-gated data-write cache; a miss changes no guest state and
 * returns to arm_step(), which owns translation, faults, MMIO and observers.
 * Ending the head at every store makes self-modifying code fail closed: no
 * following cached head can execute before its complete raw-byte witness has
 * been checked against the now-live RAM bytes. */
bool a64_static_decode_memory_hits_bytes_at(const uint8_t *program,
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

/* Execute one block returned by a64_static_decode_memory_hits_bytes_at().
 * Direct stores are possible only while cpu->bus->host_ram_write is live and
 * the exact VA/privilege/generation DWRITE entry hits. On refusal, `completed`
 * has the same exact-prefix meaning as the read-only runner. */
bool a64_static_run_memory_hits(arm_cpu_t *cpu,
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

bool a64_static_run_memory_hits_decoded(arm_cpu_t *cpu,
                                        const a64_static_block_t *block,
                                        uint8_t *ram, size_t ram_size,
                                        unsigned *completed);

/* Execute one or more product-decoded blocks while keeping the pinned guest
 * register context live inside the signed function. `first` has already been
 * selected for cpu->r[15]. After each full block, `next` must repeat the
 * product's interrupt/fetch/cache/raw-byte checks and may select a block no
 * longer than `remaining`. The total never exceeds `budget`. Runtime
 * read/write/VFP guard misses stop at the exact completed prefix; false is
 * reserved for a pre-execution contract refusal and leaves guest state
 * unchanged. `blocks` counts only block entries that retired at least one
 * instruction. */
bool a64_static_run_read_hits_chain(arm_cpu_t *cpu,
                                    const a64_static_block_t *first,
                                    uint8_t *ram, size_t ram_size,
                                    unsigned budget,
                                    a64_static_chain_next_fn next,
                                    void *opaque, unsigned *completed,
                                    unsigned *blocks);

bool a64_static_run_memory_hits_chain(arm_cpu_t *cpu,
                                      const a64_static_block_t *first,
                                      uint8_t *ram, size_t ram_size,
                                      unsigned budget,
                                      a64_static_chain_next_fn next,
                                      void *opaque, unsigned *completed,
                                      unsigned *blocks);

/* Callback-free variant. `nodes` is the engine's fixed 512-slot table for the
 * current direct-offset head index. Generated assembly accepts a next head
 * only when its PC/fetch pointer/generation/privilege/state fields and complete
 * executing-block raw-byte witness match, and otherwise returns the exact
 * completed prefix. Bytes after the cached executable prefix are deliberately
 * outside that witness: changing them cannot alter the prefix's semantics and
 * arm_step() still owns the following instruction. Every admitted store ends
 * its head, so a store that changes code reaches this witness before another
 * cached head can execute.
 * Stable interrupt/MMU facts are proved by the caller before entry; the signed
 * subset cannot mutate them and no device ticks occur inside the bounded run. */
bool a64_static_run_read_hits_graph(
    arm_cpu_t *cpu, const a64_static_block_t *first,
    uint8_t *ram, size_t ram_size, unsigned budget,
    const a64_static_graph_node_t nodes[A64_STATIC_GRAPH_SLOTS],
    unsigned *completed, unsigned *blocks);

bool a64_static_run_memory_hits_graph(
    arm_cpu_t *cpu, const a64_static_block_t *first,
    uint8_t *ram, size_t ram_size, unsigned budget,
    const a64_static_graph_node_t nodes[A64_STATIC_GRAPH_SLOTS],
    unsigned *completed, unsigned *blocks);

#endif /* S5LBOX_A64_STATIC_H */
