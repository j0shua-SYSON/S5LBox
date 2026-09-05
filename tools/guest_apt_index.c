/* See guest_apt_index.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "guest_apt_index.h"
#include <stddef.h>

struct guest_apt_index_node {
    guest_apt_index_node_t *left, *right;
    uint32_t item, length, height;
};
#define CHUNK_NODES 192u
struct guest_apt_index_chunk {
    guest_apt_index_chunk_t *next;
    uint32_t used;
    guest_apt_index_node_t nodes[CHUNK_NODES];
};

#if defined(S5LBOX_APT_INDEX_TESTING)
guest_apt_index_stats_t guest_apt_index_stats;
#define COUNT(field) (guest_apt_index_stats.field++)
#else
#define COUNT(field) ((void)0)
#endif

/* Old APT orders mismatching bytes as signed chars. At a common-prefix end,
 * however, the SHORTER range compares HIGHER (image 0x305c..0x3070 in the
 * pinned library). This is not ordinary strcmp ordering. Only sign/equality
 * are observable by the interning method. */
static int compare(const char *a, uint32_t an, const char *b, uint32_t bn) {
    uint32_t n = an < bn ? an : bn;
    COUNT(comparisons);
    for (uint32_t i = 0; i < n; ++i) {
        int x = (int)(unsigned char)a[i], y = (int)(unsigned char)b[i];
        if (x >= 128) x -= 256;
        if (y >= 128) y -= 256;
        COUNT(bytes);
        if (x != y) return x < y ? -1 : 1;
    }
    return an == bn ? 0 : an < bn ? 1 : -1;
}

static uint32_t length(const char *s) {
    uint32_t n = 0;
    while (s[n]) ++n;
    return n;
}

static void release_tree(guest_apt_index_t *s) {
    guest_apt_index_chunk_t *chunk = s->chunks;
    while (chunk) {
        guest_apt_index_chunk_t *next = chunk->next;
        guest_apt_index_free(chunk, (uint32_t)sizeof *chunk);
        chunk = next;
    }
    s->root = NULL;
    s->chunks = NULL;
    s->initialized = s->disabled = s->observed_head = 0;
}

void guest_apt_index_release(guest_apt_index_t *s) {
    release_tree(s);
    for (unsigned i = 0; i < 26; ++i) s->recent[i] = 0;
}

static void disable(guest_apt_index_t *s) {
    release_tree(s);
    s->initialized = s->disabled = 1;
}

static guest_apt_index_node_t *reserve(guest_apt_index_t *s) {
    guest_apt_index_chunk_t *chunk = s->chunks;
    if (!chunk || chunk->used == CHUNK_NODES) {
        chunk = guest_apt_index_alloc((uint32_t)sizeof *chunk);
        if (!chunk) return NULL;
        chunk->next = s->chunks;
        chunk->used = 0;
        s->chunks = chunk;
    }
    guest_apt_index_node_t *node = &chunk->nodes[chunk->used++];
    node->left = node->right = NULL;
    node->height = 1;
    node->item = node->length = 0;
    return node;
}

static uint32_t height(const guest_apt_index_node_t *n) {
    return n ? n->height : 0;
}

static void reheight(guest_apt_index_node_t *n) {
    uint32_t a = height(n->left), b = height(n->right);
    n->height = 1u + (a > b ? a : b);
}

static guest_apt_index_node_t *rotate_left(guest_apt_index_node_t *n) {
    guest_apt_index_node_t *r = n->right;
    n->right = r->left;
    r->left = n;
    reheight(n);
    reheight(r);
    return r;
}

static guest_apt_index_node_t *rotate_right(guest_apt_index_node_t *n) {
    guest_apt_index_node_t *l = n->left;
    n->left = l->right;
    l->right = n;
    reheight(n);
    reheight(l);
    return l;
}

static const char *node_string(const guest_apt_view_t *v,
                               const guest_apt_index_node_t *n) {
    return v->strings + v->items[n->item].string;
}

static guest_apt_index_node_t *insert(guest_apt_index_node_t *root,
                                     guest_apt_index_node_t *node,
                                     const guest_apt_view_t *v) {
    if (!root) return node;
    int cmp = compare(node_string(v, node), node->length,
                      node_string(v, root), root->length);
    if (cmp < 0) root->left = insert(root->left, node, v);
    else root->right = insert(root->right, node, v);
    reheight(root);
    int balance = (int)height(root->left) - (int)height(root->right);
    if (balance > 1) {
        if (height(root->left->left) < height(root->left->right))
            root->left = rotate_left(root->left);
        return rotate_right(root);
    }
    if (balance < -1) {
        if (height(root->right->right) < height(root->right->left))
            root->right = rotate_right(root->right);
        return rotate_left(root);
    }
    return root;
}

/* Historical files need not be trusted to have a strict unique ordering.
 * Duplicates, unsorted lists and allocation failure keep ordinary list
 * semantics. They never leave a partly seeded index in use. */
static void seed(guest_apt_index_t *s, const guest_apt_view_t *v) {
    const char *previous = NULL;
    uint32_t previous_length = 0;
    s->initialized = 1;
    s->observed_head = *v->head;
    for (uint32_t item = *v->head; item; item = v->items[item].next) {
        const char *text = v->strings + v->items[item].string;
        uint32_t n = length(text);
        if (previous && compare(previous, previous_length, text, n) <= 0) {
            disable(s);
            return;
        }
        guest_apt_index_node_t *node = reserve(s);
        if (!node) {
            disable(s);
            return;
        }
        node->item = item;
        node->length = n;
        s->root = insert(s->root, node, v);
        previous = text;
        previous_length = n;
        COUNT(seeded);
    }
}

/* Preserve the old sequence of pool allocation, link update, and string
 * write, including the partially linked node when the final write fails. */
static uint32_t append_at(const guest_apt_view_t *v, uint32_t previous,
                           uint32_t next, const char *text, uint32_t n,
                           uint32_t *item_out) {
    uint32_t item = guest_apt_pool_allocate(v->map, sizeof(guest_apt_item_t));
    if (!item) return 0;
    v->items[item].next = next;
    if (previous) v->items[previous].next = item;
    else *v->head = item;
    v->items[item].string = guest_apt_pool_write_string(v->map, text, n);
    *item_out = item;
    return v->items[item].string;
}

static uint32_t linear(guest_apt_index_t *s, uint32_t hash,
                       const guest_apt_view_t *v, const char *text, uint32_t n) {
    uint32_t previous = 0, item = *v->head;
    COUNT(fallbacks);
    while (item) {
        const char *current = v->strings + v->items[item].string;
        int cmp = compare(text, n, current, length(current));
        if (!cmp) { s->recent[hash] = item; return v->items[item].string; }
        if (cmp > 0) break;
        previous = item;
        item = v->items[item].next;
    }
    uint32_t added = 0;
    uint32_t result = append_at(v, previous, item, text, n, &added);
    if (result) s->recent[hash] = added;
    return result;
}

uint32_t guest_apt_index_write(guest_apt_index_t *s, const guest_apt_view_t *v,
                              const char *text, uint32_t n) {
    if (s->initialized && !s->disabled && s->observed_head != *v->head)
        guest_apt_index_release(s);
    /* The pinned binary computes an unsigned modulus of signed first bytes.
     * An empty input has no defined second byte; use its zero terminator and
     * avoid the old out-of-bounds access. Valid unique empty keys keep the
     * same handle regardless of which recent-cache bucket holds it. */
    int a = n ? (int)(unsigned char)text[0] : 0;
    int b = n ? (int)(unsigned char)text[1] : 0;
    if (a >= 128) a -= 256;
    if (b >= 128) b -= 256;
    uint32_t hash = (uint32_t)(a * 5 + b) % 26u;
    uint32_t recent = s->recent[hash];
    if (recent) {
        const char *old = v->strings + v->items[recent].string;
        if (!compare(text, n, old, length(old))) return v->items[recent].string;
    }
    /* Embedded NUL keys are ordered using their supplied length by old APT,
     * but stored keys are later read with strlen. That can destroy visible
     * list ordering. Preserve it with the reference path for this lifetime. */
    for (uint32_t i = 0; i < n; ++i) {
        if (!text[i]) { disable(s); break; }
    }
    if (s->disabled) return linear(s, hash, v, text, n);
    if (!s->initialized) seed(s, v);
    if (s->disabled) return linear(s, hash, v, text, n);

    uint32_t previous = 0, next = 0;
    guest_apt_index_node_t *node = s->root;
    while (node) {
        int cmp = compare(text, n, node_string(v, node), node->length);
        if (!cmp) {
            s->recent[hash] = node->item;
            return v->items[node->item].string;
        }
        if (cmp < 0) { previous = node->item; node = node->left; }
        else { next = node->item; node = node->right; }
    }
    node = reserve(s);
    if (!node) {
        disable(s);
        return linear(s, hash, v, text, n);
    }
    uint32_t item = 0;
    uint32_t result = append_at(v, previous, next, text, n, &item);
    if (!result) { disable(s); return 0; }
    node->item = item;
    node->length = n;
    s->root = insert(s->root, node, v);
    s->observed_head = *v->head;
    s->recent[hash] = item;
    COUNT(inserted);
    return result;
}

#if defined(S5LBOX_APT_INDEX_TESTING)
static int validate_node(const guest_apt_index_node_t *n,
                          const guest_apt_view_t *v,
                          const guest_apt_index_node_t *lo,
                          const guest_apt_index_node_t *hi) {
    if (!n) return 0;
    if (lo && compare(node_string(v, n), n->length,
                      node_string(v, lo), lo->length) <= 0) return -1;
    if (hi && compare(node_string(v, n), n->length,
                      node_string(v, hi), hi->length) >= 0) return -1;
    int a = validate_node(n->left, v, lo, n);
    int b = validate_node(n->right, v, n, hi);
    if (a < 0 || b < 0 || a - b < -1 || a - b > 1) return -1;
    int h = 1 + (a > b ? a : b);
    return n->height == (uint32_t)h ? h : -1;
}

int guest_apt_index_validate(const guest_apt_index_t *s,
                             const guest_apt_view_t *v) {
    return validate_node(s->root, v, NULL, NULL);
}
#endif
