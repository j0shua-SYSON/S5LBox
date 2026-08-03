/*
 * S5LBox -- benchmark-only static AArch64 semantic-thread proof.
 *
 * The native handlers are ordinary build-time-generated, signed text. Runtime
 * decoding creates only eight-byte data records (handler id + immediate); it
 * never emits code and never requests writable executable memory.
 *
 * This is deliberately not an emulator engine yet. It covers the bounded
 * ARM/Thumb forms used by jitbench so the register-pinned architecture can be
 * measured before product integration is attempted.
 */
#ifndef S5LBOX_A64_STATIC_H
#define S5LBOX_A64_STATIC_H

#include "arm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define A64_STATIC_MAX_INSNS 16u
#define A64_STATIC_HANDLER_COUNT 2577u

typedef struct {
    uint32_t handler;
    uint32_t immediate;
} a64_static_uop_t;

typedef struct {
    a64_static_uop_t uops[A64_STATIC_MAX_INSNS];
    unsigned insn_count;
    bool thumb;
} a64_static_block_t;

/* Decode one loop block. The final instruction must be an unconditional branch
 * back to address zero. Supported data operands are r0-r7 plus Thumb SP-relative
 * word loads/stores; every unsupported bit causes a clean false return. */
bool a64_static_decode(const void *program, unsigned insns, bool thumb,
                       a64_static_block_t *out);

/* True only when the target was built with the generated AArch64 handler file. */
bool a64_static_host_available(void);

/* Execute `blocks` iterations against flat, power-of-two RAM. The wrapper is
 * intentionally narrower than arm_run: no MMU, faults, MMIO, timer or IRQ path
 * exists in this proof. */
bool a64_static_run(arm_cpu_t *cpu, const a64_static_block_t *block,
                    uint64_t blocks, uint8_t *ram, size_t ram_size);

#endif /* S5LBOX_A64_STATIC_H */
