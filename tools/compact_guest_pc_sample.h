/* Register-only guest cursor attribution for the opt-in host sampler.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_COMPACT_GUEST_PC_SAMPLE_H
#define S5LBOX_COMPACT_GUEST_PC_SAMPLE_H
#include <stdbool.h>
#include <stdint.h>

/* The generated runner owns W26 only within [live_begin, live_end).
 * The complete register snapshot comes from one thread_get_state call.
 * Never dereference a running thread's CPU/context/code pointers here.
 * W26 writes zero-extend; ARM and Thumb architectural cursors are even.
 * A cursor can already name the next instruction during retirement, and
 * virtual addresses alone do not identify a guest process or code image. */
static inline bool compact_guest_pc_sample(uintptr_t host_pc,
                                            uintptr_t live_begin,
                                            uintptr_t live_end,
                                            uint64_t x26,
                                            uint32_t *guest_pc) {
    if (!guest_pc || !live_begin || live_begin >= live_end ||
        (live_begin & 3u) || (live_end & 3u) || (host_pc & 3u) ||
        host_pc < live_begin || host_pc >= live_end ||
        x26 > UINT32_MAX || (x26 & 1u))
        return false;
    *guest_pc = (uint32_t)x26;
    return true;
}
#endif
