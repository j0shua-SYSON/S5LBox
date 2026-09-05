/* Differential tests of the catalog algorithm, not physical performance.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "guest_apt_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ITEMS 20000u
#define STRINGS (2u * 1024u * 1024u)
typedef struct {
    guest_apt_item_t items[ITEMS];
    char strings[STRINGS];
    uint32_t head, used_items, used_strings, recent[26];
    int fail_item, fail_string;
} pool_t;
static pool_t reference_pool, indexed_pool;
static guest_apt_index_t state;
static unsigned checks, failures, outstanding, alloc_calls;
static int fail_alloc_at = -1;
static uint64_t reference_comparisons;

#define CHECK(c, ...) do { checks++; if (!(c)) { \
    if (failures++ < 20) { printf("line %d: ", __LINE__); \
        printf(__VA_ARGS__); puts(""); } } } while (0)

void *guest_apt_index_alloc(uint32_t size) {
    if ((int)alloc_calls++ == fail_alloc_at) return NULL;
    void *p = malloc(size);
    if (p) outstanding++;
    return p;
}
void guest_apt_index_free(void *p, uint32_t size) {
    (void)size;
    CHECK(p && outstanding, "invalid free");
    if (p) { outstanding--; free(p); }
}
uint32_t guest_apt_pool_allocate(void *map, uint32_t size) {
    pool_t *p = map;
    CHECK(size == sizeof(guest_apt_item_t), "pool allocation ABI");
    if (p->fail_item || p->used_items + 1 >= ITEMS) return 0;
    return ++p->used_items;
}
uint32_t guest_apt_pool_write_string(void *map, const char *s, uint32_t n) {
    pool_t *p = map;
    if (p->fail_string || n >= STRINGS - p->used_strings) return 0;
    uint32_t offset = p->used_strings;
    memcpy(p->strings + offset, s, n);
    p->strings[offset + n] = 0;
    p->used_strings += n + 1u;
    return offset;
}

static int cmp_ref(const char *a, uint32_t an, const char *b, uint32_t bn) {
    reference_comparisons++;
    uint32_t i = 0;
    while (i != an && i != bn) {
        int x = (signed char)a[i], y = (signed char)b[i];
        if (x < y) return -1;
        if (x > y) return 1;
        ++i;
    }
    /* The exact ARM method returns +1 when only the first range ends. */
    return an == bn ? 0 : an < bn ? 1 : -1;
}

static uint32_t reference_write(pool_t *p, const char *s, uint32_t n) {
    int a = n ? (signed char)s[0] : 0, b = n ? (signed char)s[1] : 0;
    uint32_t hash = (uint32_t)(a * 5 + b) % 26u;
    uint32_t i = p->recent[hash];
    if (i && cmp_ref(s, n, p->strings + p->items[i].string,
                     (uint32_t)strlen(p->strings + p->items[i].string)) == 0)
        return p->items[i].string;
    uint32_t previous = 0;
    i = p->head;
    while (i) {
        const char *text = p->strings + p->items[i].string;
        int cmp = cmp_ref(s, n, text, (uint32_t)strlen(text));
        if (cmp == 0) { p->recent[hash] = i; return p->items[i].string; }
        if (cmp > 0) break;
        previous = i;
        i = p->items[i].next;
    }
    uint32_t item = guest_apt_pool_allocate(p, sizeof(guest_apt_item_t));
    if (!item) return 0;
    p->items[item].next = i;
    if (previous) p->items[previous].next = item;
    else p->head = item;
    p->items[item].string = guest_apt_pool_write_string(p, s, n);
    if (!p->items[item].string) return 0;
    p->recent[hash] = item;
    return p->items[item].string;
}

static guest_apt_view_t view(void) {
    guest_apt_view_t v = {indexed_pool.items, indexed_pool.strings,
                          &indexed_pool.head, &indexed_pool};
    return v;
}

static void reset(void) {
    guest_apt_index_release(&state);
    CHECK(outstanding == 0, "leaked chunks");
    memset(&reference_pool, 0, sizeof reference_pool);
    memset(&indexed_pool, 0, sizeof indexed_pool);
    reference_pool.used_strings = indexed_pool.used_strings = 1;
    alloc_calls = 0;
    fail_alloc_at = -1;
    reference_comparisons = 0;
    memset(&guest_apt_index_stats, 0, sizeof guest_apt_index_stats);
}

static void match(void) {
    CHECK(reference_pool.head == indexed_pool.head, "list head differs");
    CHECK(reference_pool.used_items == indexed_pool.used_items, "item count differs");
    CHECK(reference_pool.used_strings == indexed_pool.used_strings, "string size differs");
    CHECK(!memcmp(reference_pool.items, indexed_pool.items,
                   (indexed_pool.used_items + 1) * sizeof(guest_apt_item_t)),
          "serialized item identities or links differ");
    CHECK(!memcmp(reference_pool.strings, indexed_pool.strings,
                   indexed_pool.used_strings), "serialized strings differ");
    guest_apt_view_t v = view();
    CHECK(guest_apt_index_validate(&state, &v) >= 0, "AVL ordering or height broken");
}

static void query(const char *s, uint32_t n) {
    guest_apt_view_t v = view();
    uint32_t wanted = reference_write(&reference_pool, s, n);
    uint32_t got = guest_apt_index_write(&state, &v, s, n);
    CHECK(got == wanted, "returned handle %u != %u", got, wanted);
}

static uint32_t random_state = 0x7359acd1u;
static uint32_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void orders(void) {
    for (unsigned order = 0; order < 3; ++order) {
        reset();
        for (unsigned i = 0; i < 4096; ++i) {
            unsigned value = order == 0 ? i : order == 1 ? 4095u - i : next_random() % 4096u;
            char key[40];
            int n = snprintf(key, sizeof key, "org.example.section.%04u", value);
            query(key, (uint32_t)n);
        }
        uint64_t tree_compares = guest_apt_index_stats.comparisons;
        uint64_t list_compares = reference_comparisons;
        match();
        printf("order=%u entries=%u list_compares=%llu index_compares=%llu\n",
               order, indexed_pool.used_items,
               (unsigned long long)list_compares, (unsigned long long)tree_compares);
        if (order == 1) CHECK(list_compares > tree_compares * 20u,
                              "descending catalog did not remove repeated search");
        /* Fresh generator indexing a preexisting valid catalog. */
        guest_apt_index_release(&state);
        memset(reference_pool.recent, 0, sizeof reference_pool.recent);
        query("org.example.section.2048", 24);
        query("before", 6);
        query("zz-after", 8);
        match();
    }
}

static void byte_cases(void) {
    reset();
    query("prefix", 6);
    query("prefix-longer", 13);
    CHECK(!strcmp(indexed_pool.strings + indexed_pool.items[indexed_pool.head].string, "prefix"),
          "pinned APT orders the shorter prefix first, unlike strcmp");
    query("", 0);
    CHECK(!indexed_pool.strings[indexed_pool.items[indexed_pool.head].string],
          "pinned APT orders an empty range before nonempty ranges");
    for (unsigned a = 1; a < 256; ++a) {
        char key[5] = {(char)a, 'x', 0, 0, 0};
        query(key, 2);
        query(key, 2);
        key[1] = (char)(256u - a);
        query(key, 2);
        key[2] = 'z';
        query(key, 3);
    }
    match();
    /* Supplied ranges can contain NUL and must disable ordered inference. */
    const char odd[] = {'a', 0, (char)0x80, 0};
    query(odd, 3);
    query("a", 1);
    query(odd, 3);
    query("a", 1);
    CHECK(state.disabled, "embedded NUL did not select reference path");
    match();
}

static void allocation_failures(void) {
    for (int fail = 0; fail < 4; ++fail) {
        reset();
        fail_alloc_at = fail;
        for (unsigned i = 0; i < 650; ++i) {
            char key[24];
            int n = snprintf(key, sizeof key, "entry-%08u", i);
            query(key, (uint32_t)n);
        }
        CHECK(state.disabled && !outstanding, "failed index allocation leaked or stayed active");
        match();
    }
    for (unsigned fail = 0; fail < 2; ++fail) {
        reset();
        query("existing", 8);
        reference_pool.fail_item = indexed_pool.fail_item = fail == 0;
        reference_pool.fail_string = indexed_pool.fail_string = fail == 1;
        query("new", 3);
        CHECK(state.disabled, "pool failure left index active");
        match();
        reference_pool.fail_item = indexed_pool.fail_item = 0;
        reference_pool.fail_string = indexed_pool.fail_string = 0;
        query("existing", 8);
        match();
    }
}

static void seed_refusal(void) {
    for (unsigned kind = 0; kind < 2; ++kind) {
        reset();
        reference_write(&reference_pool, "z", 1);
        reference_write(&reference_pool, "m", 1);
        reference_write(&reference_pool, "a", 1);
        memset(reference_pool.recent, 0, sizeof reference_pool.recent);
        /* Duplicate or unsorted old list; preserve its ordinary traversal. */
        reference_pool.strings[reference_pool.items[2].string] = kind ? 'Z' : 'z';
        memcpy(&indexed_pool, &reference_pool, sizeof indexed_pool);
        query("b", 1);
        CHECK(state.disabled && !outstanding, "invalid seed was indexed");
        query("z", 1);
        match();
    }
    reset();
    query("old", 3);
    guest_apt_item_t *old_items = indexed_pool.items;
    guest_apt_item_t *moved = malloc(sizeof indexed_pool.items);
    CHECK(moved != NULL, "test allocation");
    if (moved) {
        memcpy(moved, old_items, sizeof indexed_pool.items);
        guest_apt_view_t v = view();
        v.items = moved;
        uint32_t expected = reference_write(&reference_pool, "old", 3);
        CHECK(guest_apt_index_write(&state, &v, "old", 3) == expected,
              "index retained a stale cache item pointer after view relocation");
        free(moved);
    }
}

/* Representative pinned-APT NewVersion field replay, not a complete parser or
 * a wall-time prediction. Deliberately outside the default CI invocation. */
static int replay_packages(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return 1; }
    char line[16384], package[4096] = "", name[4096] = "", maemo[4096] = "";
    char section[4096] = "", arch[4096] = "";
    unsigned records = 0;
    reset();
    for (;;) {
        char *got = fgets(line, sizeof line, file);
        if (!got || line[0] == '\n' || line[0] == '\r') {
            if (*package) {
                const char *display = *name ? name : maemo;
                if (*display) query(display, (uint32_t)strlen(display));
                if (*section) query(section, (uint32_t)strlen(section));
                if (*arch) query(arch, (uint32_t)strlen(arch));
                records++;
            }
            package[0] = name[0] = maemo[0] = section[0] = arch[0] = 0;
            if (!got) break;
            continue;
        }
        const char *value = NULL;
        char *target = NULL;
        if (!strncmp(line, "Package:", 8)) { value = line + 8; target = package; }
        else if (!strncmp(line, "Name:", 5)) { value = line + 5; target = name; }
        else if (!strncmp(line, "Maemo-Display-Name:", 19)) { value = line + 19; target = maemo; }
        else if (!strncmp(line, "Section:", 8)) { value = line + 8; target = section; }
        else if (!strncmp(line, "Architecture:", 13)) { value = line + 13; target = arch; }
        if (!value) continue;
        while (*value == ' ' || *value == '\t') ++value;
        size_t n = strlen(value);
        while (n && (value[n-1] == '\r' || value[n-1] == '\n' ||
                       value[n-1] == ' ' || value[n-1] == '\t')) --n;
        if (n >= 4096) { fclose(file); fprintf(stderr, "field too long\n"); return 1; }
        memcpy(target, value, n);
        target[n] = 0;
    }
    int input_error = ferror(file);
    fclose(file);
    uint64_t list_count = reference_comparisons;
    uint64_t tree_count = guest_apt_index_stats.comparisons;
    match();
    printf("field_replay records=%u unique=%u list_compares=%llu index_compares=%llu\n",
           records, indexed_pool.used_items,
           (unsigned long long)list_count, (unsigned long long)tree_count);
    guest_apt_index_release(&state);
    CHECK(!outstanding, "replay leak");
    printf("%u checks, %u failures; not a physical Cydia timing\n", checks, failures);
    return failures || input_error ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--packages")) return replay_packages(argv[2]);
    if (argc != 1) { fprintf(stderr, "usage: test_guest_apt_index [--packages FILE]\n"); return 2; }
    orders();
    byte_cases();
    allocation_failures();
    seed_refusal();
    guest_apt_index_release(&state);
    CHECK(outstanding == 0, "final leak");
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
