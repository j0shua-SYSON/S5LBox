/* Ordered catalog-string index experiment.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * Not linked into the host app or installed in a guest. */
#ifndef S5LBOX_GUEST_APT_INDEX_H
#define S5LBOX_GUEST_APT_INDEX_H
#include <stdint.h>

typedef struct { uint32_t string, next; } guest_apt_item_t;
typedef struct {
    guest_apt_item_t *items;
    const char *strings;
    uint32_t *head;
    void *map;
} guest_apt_view_t;

typedef struct guest_apt_index_node guest_apt_index_node_t;
typedef struct guest_apt_index_chunk guest_apt_index_chunk_t;
typedef struct {
    guest_apt_index_node_t *root;
    guest_apt_index_chunk_t *chunks;
    uint32_t initialized, disabled, observed_head;
    uint32_t recent[26];
} guest_apt_index_t;

/* Allocation failure is NULL, not an exception. These four adapters are
 * supplied by the host test; a future pinned guest adapter must prove its ABI.
 * Pool adapters must retain the fixed-map semantics of the pinned APT version. */
void *guest_apt_index_alloc(uint32_t size);
void guest_apt_index_free(void *ptr, uint32_t size);
uint32_t guest_apt_pool_allocate(void *map, uint32_t size);
uint32_t guest_apt_pool_write_string(void *map, const char *s, uint32_t size);

/* State starts zeroed and belongs to one generator lifetime. Release it on
 * every destructor path before that object can be reused. No global cache. */
void guest_apt_index_release(guest_apt_index_t *state);
uint32_t guest_apt_index_write(guest_apt_index_t *state,
                              const guest_apt_view_t *view,
                              const char *s, uint32_t size);

#if defined(S5LBOX_APT_INDEX_TESTING)
typedef struct {
    uint64_t comparisons, bytes, seeded, inserted, fallbacks;
} guest_apt_index_stats_t;
extern guest_apt_index_stats_t guest_apt_index_stats;
int guest_apt_index_validate(const guest_apt_index_t *state,
                             const guest_apt_view_t *view);
#endif
#endif
