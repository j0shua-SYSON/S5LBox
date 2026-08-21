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
 * and memory record. A conditional LDM can instead use a guard, preflight,
 * fifteen destination commits and a finish record. Even after fifteen earlier
 * four-record instructions that remains below this ceiling; the final slot is
 * the fixed block exit. */
#define A64_STATIC_MAX_UOPS (A64_STATIC_MAX_INSNS * 4u + 16u)
#define A64_STATIC_HANDLER_COUNT 26509u
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
    bool indirect_exit;
    bool thumb_conditional_exit;
    bool touches_memory;
    bool direct_reads;
    bool direct_writes;
    bool runtime_guards;
    bool vfp;
    bool vfp_arithmetic;
    bool vfp_direct_writes;
    bool stm_direct_writes;
    bool ldm_direct_reads;
    bool vstm_direct_writes;
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
 * `pc`. A terminal A32 immediate B/BL may target any word-aligned address;
 * terminal Thumb conditional B covers all fourteen valid conditions and any
 * halfword-aligned target. Conditional branches and every A32 BL carry both
 * their taken target and natural fallthrough in a dynamic-exit record; exit_pc
 * names that fallthrough. A terminal unconditional B retains the compact
 * fixed-exit form. Terminal
 * A32 and Thumb BX/BLX register forms are guarded dynamic exits: the signed
 * handler validates the live target before changing LR, PC or instruction
 * state, and updates the persistent chain's ARM/Thumb state before selecting
 * another decoded head. A32 data
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
 * also admitted behind live CPACR/FPEXC/FPSCR guards. Guarded scalar VFPv2
 * arithmetic commits only the traced RN/FZ/DN, sticky-IXC, signed-zero/normal
 * finite contract; every special value or newly visible exception falls back
 * before guest mutation. The same audited contract admits double-to-single
 * VCVT only while its staged result remains signed-zero/finite-normal and the
 * host exposes no exception beyond already-sticky IXC. Compare and exact
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
 * is the final semantic instruction in the decoded head. Single-register VSTR
 * S/D with the same pre-indexed, no-writeback address form as VLDR is included;
 * a doubleword validates both word mappings before either word is changed.
 * Architectural VSTM S/D forms use a separately identifiable transactional
 * handler only when every word is aligned and covered by one already-proved
 * 1 KiB DWRITE entry. Cross-block transfers and deprecated FSTMX remain on the
 * literal interpreter. A
 * hit uses the CPU's separately consent-gated data-write cache; a miss changes
 * no guest state and returns to arm_step(), which owns translation, faults,
 * MMIO and observers.
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

/* Instruction-level admission result for the compact live-byte loop.  The
 * first two values retire exactly; every later value stops before mutation.
 * This classifier deliberately excludes the wrapper's machine-wide gates
 * (MMU, interrupts, byte window and flat-RAM ownership), so a profile can
 * separate missing instruction semantics from missing SoC integration. */
typedef enum {
    A64_COMPACT_RAW_ADMIT_EXECUTE = 0,
    A64_COMPACT_RAW_ADMIT_CONDITION_SKIP,
    A64_COMPACT_RAW_ADMITTED_COUNT,
    A64_COMPACT_RAW_REJECT_THUMB = A64_COMPACT_RAW_ADMITTED_COUNT,
    A64_COMPACT_RAW_REJECT_NV,
    A64_COMPACT_RAW_REJECT_DP_PC,
    A64_COMPACT_RAW_REJECT_DP_TEST_WITHOUT_S,
    A64_COMPACT_RAW_REJECT_DP_REGISTER_SHIFT,
    A64_COMPACT_RAW_REJECT_DP_RM_PC,
    A64_COMPACT_RAW_REJECT_MEMORY_FORM,
    A64_COMPACT_RAW_REJECT_MEMORY_PC,
    A64_COMPACT_RAW_REJECT_MEMORY_ALIGNMENT,
    A64_COMPACT_RAW_REJECT_VFP,
    A64_COMPACT_RAW_REJECT_CLASS,
    A64_COMPACT_RAW_ADMISSION_COUNT
} a64_compact_raw_admission_t;

a64_compact_raw_admission_t a64_compact_raw_classify_instruction(
    const arm_cpu_t *cpu, uint32_t insn, bool thumb);

/* Execute a deliberately bounded mixed A32/Thumb subset directly from live
 * instruction bytes, without decoded records or the static handler graph.
 * This is a feasibility/oracle boundary, not a product engine: MMU,
 * interrupts, MMIO, unsupported PC forms and unaligned data accesses are
 * refused. The exact no-side-effect CP14 probe, CP15 cache/barrier (excluding
 * WFI), and software thread-ID families are included; MMU/TLB/control
 * mutations remain interpreter-owned. A supported prefix may retire before
 * the first unsupported or out-of-window instruction; `completed` reports
 * that exact prefix. Runtime code generation is never used--the loop is
 * ordinary build-time-linked, signed AArch64 text. */
bool a64_compact_raw_run(arm_cpu_t *cpu, const uint8_t *code,
                         uint32_t code_base, uint32_t code_bytes,
                         unsigned max_insns, uint8_t *ram, size_t ram_size,
                         unsigned *completed);

/* Execute from a caller-proved live virtual-code window while the guest MMU
 * may be enabled. The pointer itself is the fetch translation witness. This
 * non-resident form supplies no fallback, so every condition-passed data
 * access still stops before mutation. Masked interrupt lines are harmless,
 * while a pending unmasked interrupt remains an entry refusal. */
bool a64_compact_raw_run_code_window(arm_cpu_t *cpu, const uint8_t *code,
                                     uint32_t code_base,
                                     uint32_t code_bytes,
                                     unsigned max_insns,
                                     unsigned *completed);

typedef struct {
    const uint8_t *code;
    uint32_t code_base;
    uint32_t code_bytes;
} a64_compact_raw_code_window_t;

/* Exact pre-mutation reason for a native data-cache refusal. `valid` is one
 * only when the compact runner reached a semantically admitted memory access
 * and its live DREAD/DWRITE lookup failed. The remaining fields bind that
 * refusal to the unchanged architectural PC and invocation TLB generation.
 * This is an input witness, not permission to walk or touch a device. */
typedef struct {
    uint32_t valid;
    uint32_t va;
    uint32_t pc;
    uint32_t access;
    uint32_t priv;
    uint32_t tlb_gen;
} a64_compact_raw_data_miss_t;

/* A resident code-window invocation may hand an instruction it cannot execute
 * to the architectural interpreter without returning through the outer SoC
 * loop. The callback normally owns that one instruction's complete semantics
 * and cycle accounting. NO_RETIRE leaves it untouched and stops; RETIRE_STOP
 * promises one successful retirement and ends the interval. RETIRE_CONTINUE
 * also promises one retirement and publishes the proven live fetch window
 * containing the next PC through `next_window`. NO_RETIRE_CONTINUE is narrower:
 * it promises no architectural mutation or retirement and publishes a proven
 * replacement window containing the unchanged PC. */
typedef enum {
    A64_COMPACT_RAW_FALLBACK_NO_RETIRE = 0,
    A64_COMPACT_RAW_FALLBACK_RETIRE_CONTINUE,
    A64_COMPACT_RAW_FALLBACK_RETIRE_STOP,
    A64_COMPACT_RAW_FALLBACK_NO_RETIRE_CONTINUE
} a64_compact_raw_fallback_result_t;

typedef a64_compact_raw_fallback_result_t
    (*a64_compact_raw_fallback_fn)(
        void *opaque, a64_compact_raw_code_window_t *next_window,
        const a64_compact_raw_data_miss_t *data_miss);

/* Keep one build-time-linked AArch64 invocation resident across exact
 * interpreter fallbacks. The caller still proves the live virtual-code window
 * and bounds the whole interval to a machine-safe retirement budget. Aligned
 * immediate word loads/stores may execute only when the interpreter-owned
 * DREAD/DWRITE entry proves the VA block, privilege and MMU generation;
 * stores additionally require live frontend write-pointer consent. Every
 * cache miss reaches the fallback before mutation. A PC or instruction-state
 * transition leaving the current window also reaches the fallback. A callback
 * may either retire one instruction before replacing the window, or replace it
 * without retirement when the unchanged PC is already covered by a new exact
 * witness. No unproved pointer is read. `native_completed` and
 * `fallback_completed` partition `completed` exactly. */
bool a64_compact_raw_run_code_window_resident(
    arm_cpu_t *cpu, const uint8_t *code, uint32_t code_base,
    uint32_t code_bytes, unsigned max_insns,
    a64_compact_raw_fallback_fn fallback, void *fallback_opaque,
    unsigned *completed, unsigned *native_completed,
    unsigned *fallback_completed);

/* Opt-in variant of the same contract. Its invocation-local cache may reuse
 * up to eight full, aligned User-mode windows that this same invocation
 * already proved; translation generation and privilege cannot change across
 * such a reuse. `window_cache_hits` counts only C callback transitions that
 * were avoided. */
bool a64_compact_raw_run_code_window_resident_cached(
    arm_cpu_t *cpu, const uint8_t *code, uint32_t code_base,
    uint32_t code_bytes, unsigned max_insns,
    a64_compact_raw_fallback_fn fallback, void *fallback_opaque,
    bool window_cache_enabled, uint64_t *window_cache_hits,
    unsigned *completed, unsigned *native_completed,
    unsigned *fallback_completed);

/* Opt-in statistical attribution for the build-time-linked compact runner. A
 * marker-created sampler polls only the pthread executing the public SoC run
 * slice, rejects observations while that thread is not running, and assigns
 * the captured PC to one ordered text region. A bounded outside-runner sample
 * retains raw PCs so ordinary code can rank them. The aliases exported by the
 * generator add no hot-path instructions. Unsupported hosts refuse enablement
 * and every snapshot remains zero. This is host diagnostic state only: it is
 * neither guest state nor part of a snapshot. */
typedef enum {
    A64_COMPACT_RAW_PC_PROFILE_ENTRY = 0,
    A64_COMPACT_RAW_PC_PROFILE_DP,
    A64_COMPACT_RAW_PC_PROFILE_MEMORY,
    A64_COMPACT_RAW_PC_PROFILE_BLOCK_CONTROL,
    A64_COMPACT_RAW_PC_PROFILE_SYSTEM,
    A64_COMPACT_RAW_PC_PROFILE_VFP,
    A64_COMPACT_RAW_PC_PROFILE_THUMB_DECODE,
    A64_COMPACT_RAW_PC_PROFILE_THUMB_LOW_ALU,
    A64_COMPACT_RAW_PC_PROFILE_THUMB_ALU_HIGH,
    A64_COMPACT_RAW_PC_PROFILE_THUMB_MEMORY_FORM,
    A64_COMPACT_RAW_PC_PROFILE_THUMB_MISC,
    A64_COMPACT_RAW_PC_PROFILE_THUMB_BRANCH,
    A64_COMPACT_RAW_PC_PROFILE_THUMB_MEMORY_ACCESS,
    A64_COMPACT_RAW_PC_PROFILE_THUMB_CONDITION,
    A64_COMPACT_RAW_PC_PROFILE_A32_CONDITION,
    A64_COMPACT_RAW_PC_PROFILE_RETIRE,
    A64_COMPACT_RAW_PC_PROFILE_FALLBACK,
    A64_COMPACT_RAW_PC_PROFILE_EXIT,
    A64_COMPACT_RAW_PC_PROFILE_REGION_COUNT
} a64_compact_raw_pc_profile_region_t;

#define A64_COMPACT_RAW_PC_PROFILE_HOT_COUNT 8u

typedef struct {
    uintptr_t pc;
    uint64_t samples;
} a64_compact_raw_pc_profile_hot_t;

typedef struct {
    bool enabled;
    uint64_t polls;
    uint64_t not_running;
    uint64_t state_failures;
    uint64_t target_races;
    uint64_t samples;
    uint64_t outside;
    uint64_t region[A64_COMPACT_RAW_PC_PROFILE_REGION_COUNT];
    uintptr_t reference_pc;
    uint64_t outside_pc_captured;
    uint64_t outside_pc_dropped;
    a64_compact_raw_pc_profile_hot_t
        outside_hot[A64_COMPACT_RAW_PC_PROFILE_HOT_COUNT];
} a64_compact_raw_pc_profile_t;

bool a64_compact_raw_pc_profile_enable(void);
void a64_compact_raw_pc_profile_slice_begin(void);
void a64_compact_raw_pc_profile_slice_end(void);
void a64_compact_raw_pc_profile_snapshot(
    a64_compact_raw_pc_profile_t *out);

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

/* `vfp_fp_session` selects lazy host FP-state preservation for guarded VFP
 * arithmetic. False retains exact per-operation preservation. */
bool a64_static_run_memory_hits_decoded(arm_cpu_t *cpu,
                                        const a64_static_block_t *block,
                                        uint8_t *ram, size_t ram_size,
                                        bool vfp_fp_session,
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

/* `vfp_fp_session` has the same meaning as for the decoded runner. */
bool a64_static_run_memory_hits_chain(arm_cpu_t *cpu,
                                      const a64_static_block_t *first,
                                      uint8_t *ram, size_t ram_size,
                                      unsigned budget,
                                      a64_static_chain_next_fn next,
                                      void *opaque, bool vfp_fp_session,
                                      unsigned *completed,
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

/* `vfp_fp_session` has the same meaning as for the decoded runner. */
bool a64_static_run_memory_hits_graph(
    arm_cpu_t *cpu, const a64_static_block_t *first,
    uint8_t *ram, size_t ram_size, unsigned budget,
    const a64_static_graph_node_t nodes[A64_STATIC_GRAPH_SLOTS],
    bool vfp_fp_session, unsigned *completed, unsigned *blocks);

#endif /* S5LBOX_A64_STATIC_H */
