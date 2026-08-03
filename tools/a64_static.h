/*
 * S5LBox -- benchmark-only static AArch64 semantic-thread proof.
 *
 * The native handlers are ordinary build-time-generated, signed text. Runtime
 * decoding creates only eight-byte data records (handler id + immediate); it
 * never emits code and never requests writable executable memory.
 *
 * This is deliberately not an emulator engine yet. It covers the bounded
 * ARM/Thumb forms used by jitbench so the register-pinned architecture can be
 * measured and its block contract made exact before product integration.
 */
#ifndef S5LBOX_A64_STATIC_H
#define S5LBOX_A64_STATIC_H

#include "arm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define A64_STATIC_MAX_INSNS 16u
#define A64_STATIC_MAX_UOPS (A64_STATIC_MAX_INSNS + 1u)
#define A64_STATIC_HANDLER_COUNT 2577u

typedef struct {
    uint32_t handler;
    uint32_t immediate;
} a64_static_uop_t;

typedef struct {
    a64_static_uop_t uops[A64_STATIC_MAX_UOPS];
    unsigned insn_count;
    unsigned uop_count;
    uint32_t start_pc;
    uint32_t exit_pc;
    bool thumb;
} a64_static_block_t;

/* Decode one block beginning at `pc`. A terminal unconditional branch may
 * target any address; otherwise the block exits at its natural fallthrough.
 * Supported data operands are r0-r7 plus Thumb SP-relative word loads/stores;
 * every unsupported bit causes a clean false return. */
bool a64_static_decode_at(const void *program, unsigned insns, bool thumb,
                          uint32_t pc, a64_static_block_t *out);

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

#endif /* S5LBOX_A64_STATIC_H */
