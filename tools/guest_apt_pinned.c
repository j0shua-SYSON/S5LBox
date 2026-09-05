/* Offline adapter for the exact apt7-lib 0.7.20.2-1 identity.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * Compile as armv6-apple-ios3.0, NOT as an EABI guest executable. */
#include "guest_apt_index.h"
#include <stddef.h>

_Static_assert(sizeof(void *) == 4, "pinned APT requires 32-bit pointers");
_Static_assert(sizeof(unsigned long) == 4, "pinned APT requires 32-bit long");

/* These imports are branches to the witnessed library's existing stubs. All
 * possible stack words for its zero off_t are supplied explicitly; no Darwin
 * off_t alignment is inferred from a cross-platform host's headers. */
extern void *guest_apt_mmap(void *, uint32_t, int, int, int,
                            uint32_t, uint32_t, uint32_t);
extern int guest_apt_munmap(void *, uint32_t);
extern uint32_t guest_apt_original_write(void *, const char *, uint32_t);

#define INDEX_TAG UINT32_C(0x53415049)

void *guest_apt_index_alloc(uint32_t size) {
    void *p = guest_apt_mmap(NULL, size, 3, 0x1002, -1, 0, 0, 0);
    return p == (void *)(uintptr_t)UINT32_MAX ? NULL : p;
}

void guest_apt_index_free(void *p, uint32_t size) {
    (void)guest_apt_munmap(p, size);
}

static guest_apt_view_t cache_view(uint32_t *generator) {
    guest_apt_view_t v;
    v.items = (guest_apt_item_t *)(uintptr_t)generator[0xa0 / 4];
    v.strings = (const char *)(uintptr_t)generator[0xa4 / 4];
    v.head = (uint32_t *)(uintptr_t)(generator[0x78 / 4] + 0x48u);
    v.map = (void *)(uintptr_t)generator[0x68 / 4];
    return v;
}

uint32_t guest_apt_pinned_write(uint32_t *generator, const char *text,
                                uint32_t size) {
    guest_apt_view_t v = cache_view(generator);
    guest_apt_index_t *state;
    if (generator[1] == INDEX_TAG) {
        state = (guest_apt_index_t *)(uintptr_t)generator[0];
    } else {
        /* The constructor clears all 26 legacy buckets. If allocation failed
         * on an earlier call, the unmodified method may have populated them.
         * Adopt those exact recent values before repurposing the two slots.
         * The odd tag cannot equal a valid aligned StringItem pointer. */
        state = guest_apt_index_alloc((uint32_t)sizeof *state);
        if (!state) return guest_apt_original_write(generator, text, size);
        state->root = NULL;
        state->chunks = NULL;
        state->initialized = state->disabled = state->observed_head = 0;
        for (unsigned i = 0; i < 26; ++i) {
            uint32_t pointer = generator[i];
            state->recent[i] = pointer
                ? (pointer - (uint32_t)(uintptr_t)v.items) / 8u : 0;
        }
        generator[0] = (uint32_t)(uintptr_t)state;
        generator[1] = INDEX_TAG;
    }
    return guest_apt_index_write(state, &v, text, size);
}

/* Both real destructor entry points need a detour through this function
 * before their original prologue. It is safe after allocation refusal and
 * resets the sentinel before freeing; repeated release cannot double-free. */
void guest_apt_pinned_release(uint32_t *generator) {
    if (generator[1] != INDEX_TAG) return;
    guest_apt_index_t *state = (guest_apt_index_t *)(uintptr_t)generator[0];
    for (unsigned i = 0; i < 26; ++i) generator[i] = 0;
    guest_apt_index_release(state);
    guest_apt_index_free(state, (uint32_t)sizeof *state);
}
