/*
 * S5LBox -- HFS+ catalog provisioning tests.
 *
 * These build whole HFSX volumes in memory, catalog B-tree and all, so the
 * suite runs in public CI with no Apple firmware anywhere near it.  The
 * fixtures are small (64 KiB) but structurally real: a B-tree header node with
 * a node-allocation map, leaf nodes carrying folder, file and thread records
 * in HFSX binary key order, an allocation bitmap the provisioner must keep
 * consistent, and matching primary and alternate volume headers.
 *
 * Everything is read back through tr_*(), a reader written against the
 * on-disk format rather than against the writer: it walks the leaf CHAIN from
 * the B-tree header's firstLeafNode, the way tools/hfsx_extract.py does, so a
 * writer that only looks correct to its own descent code cannot pass.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifdef _WIN32
/* Guarded: core/CMakeLists.txt now defines this for the whole directory, and an
 * unguarded redefinition is C4005, which /WX makes an error. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "rootfs_work.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* ------------------------------- fixture geometry ----------------------- */

#define FX_BLOCK_SIZE 4096u
#define FX_BLOCKS 16u
#define FX_SIZE (FX_BLOCK_SIZE * FX_BLOCKS)
#define FX_NODE_SIZE 4096u
#define FX_BITMAP_BLOCK 1u
#define FX_BITMAP_BYTES 8u
#define FX_CATALOG_BLOCK 2u
#define FX_CATALOG_BLOCKS 8u
#define FX_CATALOG_BYTES (FX_CATALOG_BLOCKS * FX_BLOCK_SIZE)
#define FX_CATALOG_NODES (FX_CATALOG_BYTES / FX_NODE_SIZE)
#define FX_DATA_FIRST 10u
#define FX_DATA_BLOCKS 5u
#define FX_TAIL_BLOCK 15u

#define VH_OFF 1024u
#define VH_LEN 512u

#define FX_MAX_RECORDS 48u
#define FX_MAX_RECORD 800u

/* CNIDs the fixture ships with. */
#define FX_ROOT 2u
#define FX_ALPHA 16u
#define FX_BETA 17u
#define FX_DUP 18u
#define FX_NOTE 19u
#define FX_NEXT_CNID 20u

static int g_pass;
static int g_fail;
static unsigned g_serial;

#define CHECK(condition, ...) do {                                           \
    if (condition) {                                                         \
        g_pass++;                                                            \
    } else {                                                                 \
        g_fail++;                                                            \
        printf("  FAIL %s:%d: ", __func__, __LINE__);                        \
        printf(__VA_ARGS__);                                                 \
        printf("\n");                                                        \
    }                                                                        \
} while (0)

static unsigned long process_id(void) {
#ifdef _WIN32
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

static void put_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void put_be64(uint8_t *bytes, uint64_t value) {
    put_be32(bytes, (uint32_t)(value >> 32));
    put_be32(bytes + 4, (uint32_t)value);
}

static uint16_t get_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t get_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static uint64_t get_be64(const uint8_t *bytes) {
    return ((uint64_t)get_be32(bytes) << 32) | get_be32(bytes + 4);
}

/* ------------------------------- fixture builder ------------------------ */

typedef struct fixture {
    uint8_t image[FX_SIZE];
    uint8_t record[FX_MAX_RECORDS][FX_MAX_RECORD];
    uint16_t length[FX_MAX_RECORDS];
    size_t count;
    uint32_t file_count;
    uint32_t folder_count;
    uint32_t next_cnid;
    uint32_t free_data_blocks;
    uint16_t leaf_free;
    /*
     * Node size is a fixture parameter, not a constant, because the split
     * tests need an index node that can actually be filled.  At 4096 bytes an
     * index node holds hundreds of children and the fixture has eight nodes in
     * total, so the index-split path would be unreachable; at 512 -- the
     * smallest nodeSize the provisioner accepts -- two or three children fill
     * one, and the same eight catalog blocks become 64 nodes.
     */
    uint16_t node_size;
    uint32_t catalog_nodes;
    /* Shape of the tree the builder produced, for the tests to assert on. */
    uint32_t leaf_nodes;
    uint32_t index_nodes;
    uint32_t root_node;
    uint32_t first_leaf;
    uint32_t last_leaf;
    uint32_t used_nodes;
    uint16_t index_free;
    uint16_t tree_depth;
    /*
     * How many records the LAST leaf holds before anything is provisioned.
     * The split under test claims to move no existing record, so a test that
     * only counted the tree's total would not notice records sliding from the
     * old leaf into the new one.  This is the number the old leaf must still
     * have afterwards.
     */
    uint32_t last_leaf_records;
    uint32_t root_children;
} fixture_t;

static uint16_t fx_key(uint8_t *record, uint32_t parent, const char *name) {
    size_t units = name ? strlen(name) : 0u;
    size_t index;

    put_be16(record, (uint16_t)(6u + 2u * units));
    put_be32(record + 2, parent);
    put_be16(record + 6, (uint16_t)units);
    for (index = 0; index < units; index++)
        put_be16(record + 8 + index * 2u,
                 (uint16_t)(unsigned char)name[index]);
    return (uint16_t)(8u + 2u * units);
}

static uint8_t *fx_next(fixture_t *fx) {
    return fx->record[fx->count];
}

static void fx_commit(fixture_t *fx, uint16_t length) {
    fx->length[fx->count] = length;
    fx->count++;
}

static void fx_bsd(uint8_t *data, uint16_t mode) {
    put_be32(data + 32, 0u);
    put_be32(data + 36, 0u);
    put_be16(data + 42, mode);
    put_be32(data + 44, 1u);
}

/*
 * The three record shapes, written into a caller-supplied buffer and returning
 * their length.  The fx_* wrappers below park one in the standard fixture's
 * record store; the scale fixture, whose image is far too large to live in that
 * struct, calls these directly.  One definition either way.
 */
static uint16_t rec_folder(uint8_t *record, uint32_t parent, const char *name,
                           uint32_t cnid, uint32_t valence, uint16_t flags,
                           uint32_t folder_count) {
    uint16_t key = fx_key(record, parent, name);
    uint8_t *data = record + key;

    memset(data, 0, 88u);
    put_be16(data, 1u);
    put_be16(data + 2, flags);
    put_be32(data + 4, valence);
    put_be32(data + 8, cnid);
    fx_bsd(data, (uint16_t)(0040000u | 0755u));
    put_be32(data + 84, folder_count);
    return (uint16_t)(key + 88u);
}

static uint16_t rec_file(uint8_t *record, uint32_t parent, const char *name,
                         uint32_t cnid, uint64_t logical, uint32_t start,
                         uint32_t blocks) {
    uint16_t key = fx_key(record, parent, name);
    uint8_t *data = record + key;

    memset(data, 0, 248u);
    put_be16(data, 2u);
    put_be16(data + 2, 0x0002u);
    put_be32(data + 8, cnid);
    fx_bsd(data, (uint16_t)(0100000u | 0644u));
    put_be64(data + 88, logical);
    put_be32(data + 100, blocks);
    put_be32(data + 104, start);
    put_be32(data + 108, blocks);
    return (uint16_t)(key + 248u);
}

static uint16_t rec_thread(uint8_t *record, uint32_t cnid, uint16_t type,
                           uint32_t parent, const char *name) {
    uint16_t key = fx_key(record, cnid, NULL);
    uint8_t *data = record + key;
    size_t units = strlen(name);
    size_t index;

    put_be16(data, type);
    put_be16(data + 2, 0u);
    put_be32(data + 4, parent);
    put_be16(data + 8, (uint16_t)units);
    for (index = 0; index < units; index++)
        put_be16(data + 10 + index * 2u, (uint16_t)(unsigned char)name[index]);
    return (uint16_t)(key + 10u + 2u * units);
}

static void fx_folder(fixture_t *fx, uint32_t parent, const char *name,
                      uint32_t cnid, uint32_t valence, uint16_t flags,
                      uint32_t folder_count) {
    fx_commit(fx, rec_folder(fx_next(fx), parent, name, cnid, valence, flags,
                             folder_count));
}

static void fx_file(fixture_t *fx, uint32_t parent, const char *name,
                    uint32_t cnid, uint64_t logical, uint32_t start,
                    uint32_t blocks) {
    fx_commit(fx, rec_file(fx_next(fx), parent, name, cnid, logical, start,
                           blocks));
}

static void fx_thread(fixture_t *fx, uint32_t cnid, uint16_t type,
                      uint32_t parent, const char *name) {
    fx_commit(fx, rec_thread(fx_next(fx), cnid, type, parent, name));
}

static uint8_t *fx_node(fixture_t *fx, uint32_t index) {
    return fx->image + FX_CATALOG_BLOCK * FX_BLOCK_SIZE +
           (size_t)index * fx->node_size;
}

/* Free bytes in one node of the fixture, read back out of the image the way
 * the writer's own fit test computes it. */
static uint16_t fx_node_free(const fixture_t *fx, uint32_t node_index) {
    const uint8_t *node = fx->image + FX_CATALOG_BLOCK * FX_BLOCK_SIZE +
                          (size_t)node_index * fx->node_size;
    uint16_t count = get_be16(node + 10);
    uint16_t end = get_be16(node + fx->node_size -
                            2u * ((uint32_t)count + 1u));

    return (uint16_t)(fx->node_size - 2u * ((uint32_t)count + 1u) - end);
}

static void fx_bitmap_set(fixture_t *fx, uint32_t block, int set) {
    uint8_t *byte = fx->image + FX_BITMAP_BLOCK * FX_BLOCK_SIZE + (block >> 3);
    uint8_t mask = (uint8_t)(1u << (7u - (block & 7u)));

    *byte = set ? (uint8_t)(*byte | mask) : (uint8_t)(*byte & (uint8_t)~mask);
}

/* Lay records [from, to) into one leaf node.  Returns the free bytes left. */
static uint16_t fx_emit_leaf(fixture_t *fx, uint32_t node_index, size_t from,
                             size_t to, uint32_t flink, uint32_t blink) {
    uint8_t *node = fx_node(fx, node_index);
    uint16_t offset = 14u;
    uint16_t count = (uint16_t)(to - from);
    size_t index;

    memset(node, 0, fx->node_size);
    put_be32(node, flink);
    put_be32(node + 4, blink);
    node[8] = 0xffu;
    node[9] = 1u;
    put_be16(node + 10, count);
    for (index = from; index < to; index++) {
        put_be16(node + fx->node_size - 2u * (index - from + 1u), offset);
        memcpy(node + offset, fx->record[index], fx->length[index]);
        offset = (uint16_t)(offset + fx->length[index]);
    }
    put_be16(node + fx->node_size - 2u * ((size_t)count + 1u), offset);
    return (uint16_t)(fx->node_size - 2u * ((uint32_t)count + 1u) - offset);
}

/*
 * A root index node over `children`, keyed by each child's first record.  The
 * `key_override` argument exists only for the malformed-index fixture that the
 * leaf-head refusal test needs; pass NULL for a well-formed tree.
 */
static uint16_t fx_emit_index(fixture_t *fx, uint32_t node_index,
                              const uint32_t *children,
                              const uint8_t *const *first_key,
                              size_t children_count, uint8_t level,
                              uint32_t flink, uint32_t blink) {
    uint8_t *node = fx_node(fx, node_index);
    uint16_t offset = 14u;
    size_t index;

    memset(node, 0, fx->node_size);
    put_be32(node, flink);
    put_be32(node + 4, blink);
    node[8] = 0x00u;
    node[9] = level;
    put_be16(node + 10, (uint16_t)children_count);
    for (index = 0; index < children_count; index++) {
        const uint8_t *source = first_key[index];
        uint16_t key_bytes = (uint16_t)(2u + get_be16(source));

        if ((key_bytes & 1u) != 0u)
            key_bytes++;
        put_be16(node + fx->node_size - 2u * (index + 1u), offset);
        memcpy(node + offset, source, key_bytes);
        put_be32(node + offset + key_bytes, children[index]);
        offset = (uint16_t)(offset + key_bytes + 4u);
    }
    put_be16(node + fx->node_size - 2u * (children_count + 1u), offset);
    return (uint16_t)(fx->node_size - 2u * ((uint32_t)children_count + 1u) -
                      offset);
}

/*
 * `used` is a bitmap of nodes to mark in use, or NULL to mark 0..used_nodes-1.
 * The split tests need the second form because their trees do not occupy a
 * contiguous prefix of the node space.
 */
static void fx_emit_header(fixture_t *fx, uint16_t depth, uint32_t root,
                           uint32_t first_leaf, uint32_t last_leaf,
                           uint32_t leaf_records, uint32_t used_nodes,
                           const uint8_t *used) {
    uint8_t *node = fx_node(fx, 0u);
    uint8_t *header = node + 14;
    uint32_t index;
    uint32_t catalog_nodes = fx->catalog_nodes;

    memset(node, 0, fx->node_size);
    node[8] = 0x01u;
    node[9] = 0u;
    put_be16(node + 10, 3u);
    put_be16(header, depth);
    put_be32(header + 2, root);
    put_be32(header + 6, leaf_records);
    put_be32(header + 10, first_leaf);
    put_be32(header + 14, last_leaf);
    put_be16(header + 18, fx->node_size);
    put_be16(header + 20, 516u);
    put_be32(header + 22, catalog_nodes);
    put_be32(header + 26, catalog_nodes - used_nodes);
    put_be32(header + 32, FX_CATALOG_BYTES);
    header[36] = 0u;                 /* btreeType: hfs */
    header[37] = 0xbcu;              /* keyCompareType: HFSX binary */
    put_be32(header + 38, 0x00000006u); /* big keys + variable index keys */
    /* Node-allocation map record; the header node's third record. */
    if (used) {
        memcpy(node + 248u, used, (size_t)((catalog_nodes + 7u) / 8u));
    } else {
        for (index = 0; index < used_nodes; index++)
            node[248u + (index >> 3)] |= (uint8_t)(1u << (7u - (index & 7u)));
    }
    put_be16(node + fx->node_size - 2u, 14u);
    put_be16(node + fx->node_size - 4u, 120u);
    put_be16(node + fx->node_size - 6u, 248u);
    put_be16(node + fx->node_size - 8u, (uint16_t)(fx->node_size - 8u));
}

/*
 * The provisioner runs inside the same pipeline as the fstab rewrite, which
 * refuses unless the stock record appears exactly once.  Park a copy in the
 * boot-block area: those 1024 bytes are reserved, are never an allocation
 * candidate, and are outside every structure the catalog writer touches.
 */
#define FX_FSTAB_OFFSET 512u
static const uint8_t FX_FSTAB_STOCK[] =
    "/dev/disk0s1 / hfs ro 0 1\n"
    "/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2\n";

static void fx_emit_volume(fixture_t *fx) {
    uint8_t *header = fx->image + VH_OFF;
    uint32_t used = FX_BLOCKS - fx->free_data_blocks;
    uint32_t block;

    memcpy(fx->image + FX_FSTAB_OFFSET, FX_FSTAB_STOCK,
           sizeof(FX_FSTAB_STOCK) - 1u);
    memset(header, 0, VH_LEN);
    put_be16(header, 0x4858u);              /* HFSX */
    put_be16(header + 2, 5u);
    put_be32(header + 4, 1u << 8);          /* cleanly unmounted */
    memcpy(header + 8, "10.0", 4u);
    put_be32(header + 32, fx->file_count);
    put_be32(header + 36, fx->folder_count);
    put_be32(header + 40, FX_BLOCK_SIZE);
    put_be32(header + 44, FX_BLOCKS);
    put_be32(header + 48, fx->free_data_blocks);
    put_be32(header + 52, FX_DATA_FIRST);
    put_be32(header + 64, fx->next_cnid);
    put_be64(header + 112, FX_BITMAP_BYTES);
    put_be32(header + 124, 1u);
    put_be32(header + 128, FX_BITMAP_BLOCK);
    put_be32(header + 132, 1u);
    put_be64(header + 272, FX_CATALOG_BYTES);
    put_be32(header + 284, FX_CATALOG_BLOCKS);
    put_be32(header + 288, FX_CATALOG_BLOCK);
    put_be32(header + 292, FX_CATALOG_BLOCKS);
    memset(fx->image + FX_BITMAP_BLOCK * FX_BLOCK_SIZE, 0, FX_BITMAP_BYTES);
    fx_bitmap_set(fx, 0u, 1);
    fx_bitmap_set(fx, FX_BITMAP_BLOCK, 1);
    for (block = 0; block < FX_CATALOG_BLOCKS; block++)
        fx_bitmap_set(fx, FX_CATALOG_BLOCK + block, 1);
    for (block = 0; block < FX_DATA_BLOCKS - fx->free_data_blocks; block++)
        fx_bitmap_set(fx, FX_DATA_FIRST + block, 1);
    fx_bitmap_set(fx, FX_TAIL_BLOCK, 1);
    (void)used;
    memcpy(fx->image + FX_SIZE - VH_OFF, header, VH_LEN);
}

/*
 * The stock fixture tree, in HFSX binary key order:
 *
 *   (1,"TestVol")  folder  cnid 2   valence 2  folderCount 2
 *   (2,"")         thread  -> 1 "TestVol"
 *   (2,"alpha")    folder  cnid 16  valence 1  folderCount 1
 *   (2,"beta")     folder  cnid 17  valence 1  folderCount 0
 *   (16,"")        thread  -> 2 "alpha"
 *   (16,"dup")     folder  cnid 18  valence 0  folderCount 0
 *   (17,"")        thread  -> 2 "beta"
 *   (17,"note.txt") file   cnid 19  5 bytes at block 10
 *   (18,"")        thread  -> 16 "dup"
 *   (19,"")        thread  -> 17 "note.txt"
 */
static void fx_records(fixture_t *fx) {
    fx_folder(fx, 1u, "TestVol", FX_ROOT, 2u, 0x0010u, 2u);
    fx_thread(fx, FX_ROOT, 3u, 1u, "TestVol");
    fx_folder(fx, FX_ROOT, "alpha", FX_ALPHA, 1u, 0x0010u, 1u);
    fx_folder(fx, FX_ROOT, "beta", FX_BETA, 1u, 0x0010u, 0u);
    fx_thread(fx, FX_ALPHA, 3u, FX_ROOT, "alpha");
    fx_folder(fx, FX_ALPHA, "dup", FX_DUP, 0u, 0x0010u, 0u);
    fx_thread(fx, FX_BETA, 3u, FX_ROOT, "beta");
    fx_file(fx, FX_BETA, "note.txt", FX_NOTE, 5u, FX_DATA_FIRST, 1u);
    fx_thread(fx, FX_DUP, 3u, FX_ALPHA, "dup");
    fx_thread(fx, FX_NOTE, 4u, FX_BETA, "note.txt");
}

static void fx_init(fixture_t *fx, uint16_t node_size) {
    fx->node_size = node_size;
    fx->catalog_nodes = FX_CATALOG_BYTES / node_size;
}

static fixture_t *fx_create(uint32_t free_data_blocks) {
    fixture_t *fx = (fixture_t *)calloc(1u, sizeof(*fx));

    if (!fx)
        return NULL;
    fx_init(fx, (uint16_t)FX_NODE_SIZE);
    /* note.txt occupies the first data block, so it is never free. */
    if (free_data_blocks > FX_DATA_BLOCKS - 1u)
        free_data_blocks = FX_DATA_BLOCKS - 1u;
    fx->free_data_blocks = free_data_blocks;
    fx->file_count = 1u;
    fx->folder_count = 3u;
    fx->next_cnid = FX_NEXT_CNID;
    fx_records(fx);
    memcpy(fx->image + FX_DATA_FIRST * FX_BLOCK_SIZE, "note\n", 5u);
    (void)fx_emit_leaf(fx, 1u, 0u, fx->count, 0u, 0u);
    fx_emit_header(fx, 1u, 1u, 1u, 1u, (uint32_t)fx->count, 2u, NULL);
    fx_emit_volume(fx);
    return fx;
}

/* Depth-2 variant: records 0..5 in leaf 1, 6..end in leaf 2, index root 3. */
static fixture_t *fx_create_depth2(int malformed_index) {
    fixture_t *fx = (fixture_t *)calloc(1u, sizeof(*fx));
    uint32_t children[2];
    const uint8_t *first[2];
    uint8_t override_key[16];

    if (!fx)
        return NULL;
    fx_init(fx, (uint16_t)FX_NODE_SIZE);
    fx->free_data_blocks = FX_DATA_BLOCKS - 1u;
    fx->file_count = 1u;
    fx->folder_count = 3u;
    fx->next_cnid = FX_NEXT_CNID;
    fx_records(fx);
    memcpy(fx->image + FX_DATA_FIRST * FX_BLOCK_SIZE, "note\n", 5u);
    (void)fx_emit_leaf(fx, 1u, 0u, 6u, 2u, 0u);
    (void)fx_emit_leaf(fx, 2u, 6u, fx->count, 0u, 1u);
    children[0] = 1u;
    children[1] = 2u;
    first[0] = fx->record[0];
    first[1] = fx->record[6];
    if (malformed_index) {
        /*
         * Leaf 2 really begins at (17,""), but tell the index it begins at
         * (16,"e").  A search for (16,"zzz") then descends into leaf 2 and
         * lands at record 0.  This shape cannot arise from a well-formed
         * volume -- the catalog's minimum key is always the root folder's own
         * (kHFSRootParentID, volume name), and every provisionable key has a
         * parent CNID of at least 2 -- so it is the only way to reach the
         * leaf-head guard at all, which is the point of testing it.
         */
        (void)fx_key(override_key, FX_ALPHA, "e");
        first[1] = override_key;
    }
    (void)fx_emit_index(fx, 3u, children, first, 2u, 2u, 0u, 0u);
    fx_emit_header(fx, 2u, 3u, 1u, 2u, (uint32_t)fx->count, 4u, NULL);
    fx_emit_volume(fx);
    return fx;
}

/*
 * Squeeze leaf 1 down to `leave_free` bytes with filler folder records under a
 * dedicated parent, so the node-full refusal is provoked by real records
 * rather than by a doctored header.
 */
/*
 * A real depth-3 tree, small enough to fill.
 *
 * The fixtures above use 4096-byte nodes, where one index node holds hundreds
 * of children and the whole catalog is eight nodes -- so an index node can
 * never be filled and the index-split path is unreachable.  This builds the
 * same kind of volume with 512-byte nodes, the smallest the provisioner
 * accepts, which turns the same eight catalog blocks into 64 nodes and lets
 * two or three children fill an index node.
 *
 * It is a real B-tree builder, not a hand-tuned layout: records are packed
 * into leaves until they stop fitting, leaves are packed into level-2 index
 * nodes the same way, and a root is laid over those.  `leaf_slack` and
 * `index_slack` are the free bytes deliberately left in the LAST leaf and the
 * LAST level-2 index node, which is what decides whether the next append fits,
 * splits the leaf only, or splits the leaf and the index node above it.
 *
 * The filler records carry long names and rising CNIDs, so they sort after
 * everything the standard fixture ships and after each other -- which is what
 * makes every provisioned record a rightmost append.
 *
 * `levels` is 3 for leaves -> index -> root, and 2 for leaves -> root, where
 * the single index node IS the root.  The second shape is the only way to
 * reach the "a full root would have to grow a new root" refusal at this size:
 * at 512 bytes a root over 40-child index nodes would need well over a
 * thousand leaves to fill.
 */
static fixture_t *fx_create_deep(uint16_t leaf_slack, uint16_t index_slack,
                                 unsigned filler_pairs,
                                 uint16_t leaf_headroom, uint16_t levels) {
    fixture_t *fx = (fixture_t *)calloc(1u, sizeof(*fx));
    uint8_t used[(FX_CATALOG_BYTES / 512u + 7u) / 8u];
    uint32_t leaves[FX_MAX_RECORDS];
    size_t leaf_first[FX_MAX_RECORDS];
    size_t leaf_last[FX_MAX_RECORDS];
    const uint8_t *leaf_key[FX_MAX_RECORDS];
    uint32_t level2[FX_MAX_RECORDS];
    const uint8_t *level2_key[FX_MAX_RECORDS];
    size_t leaf_count = 0;
    size_t level2_count = 0;
    size_t last_index_first = 0;
    size_t cursor = 0;
    size_t index;
    uint32_t next_node = 1u;
    unsigned filler;
    uint16_t last_leaf_free = 0;
    uint16_t last_index_free = 0;

    if (!fx)
        return NULL;
    fx_init(fx, 512u);
    memset(used, 0, sizeof(used));
    used[0] |= 0x80u;                       /* node 0, the header node */
    fx->free_data_blocks = FX_DATA_BLOCKS - 1u;
    fx->file_count = 1u;
    fx->folder_count = 3u;
    fx_records(fx);
    memcpy(fx->image + FX_DATA_FIRST * FX_BLOCK_SIZE, "note\n", 5u);
    for (filler = 0; filler < filler_pairs; filler++) {
        char name[64];
        uint32_t parent = 100u + 2u * filler;

        memset(name, 'a' + (int)(filler % 26u), 24u);
        name[24] = '\0';
        fx_folder(fx, parent, name, parent + 1u, 0u, 0x0010u, 0u);
        fx_thread(fx, parent + 1u, 3u, parent, name);
        fx->folder_count++;
    }
    fx->next_cnid = 100u + 2u * filler_pairs + 2u;

    /*
     * Pack records into leaves.  A record needs its own bytes plus its offset
     * slot, and the node keeps one trailing slot for the free-space pointer.
     */
    while (cursor < fx->count) {
        size_t take = 0;
        uint32_t used_bytes = 14u;

        while (cursor + take < fx->count) {
            uint32_t want = used_bytes + fx->length[cursor + take] +
                            2u * ((uint32_t)take + 2u);

            /* Headroom keeps the non-last leaves able to accept an interior
             * insert, which is what a provisioned folder record is: only its
             * THREAD record is a rightmost append. */
            if (want + leaf_headroom > fx->node_size)
                break;
            used_bytes += fx->length[cursor + take];
            take++;
        }
        if (take == 0u || leaf_count + 1u >= FX_MAX_RECORDS) {
            free(fx);
            return NULL;
        }
        leaf_key[leaf_count] = fx->record[cursor];
        leaf_first[leaf_count] = cursor;
        leaf_last[leaf_count] = cursor + take;
        leaves[leaf_count] = next_node;
        used[next_node >> 3] |= (uint8_t)(1u << (7u - (next_node & 7u)));
        last_leaf_free = fx_emit_leaf(fx, next_node, cursor, cursor + take, 0u,
                                      0u);
        cursor += take;
        leaf_count++;
        next_node++;
    }

    /* Pack leaves into level-2 index nodes the same way. */
    cursor = 0;
    while (cursor < leaf_count) {
        size_t take = 0;
        uint32_t used_bytes = 14u;

        while (cursor + take < leaf_count) {
            uint16_t key_bytes = (uint16_t)(2u +
                                            get_be16(leaf_key[cursor + take]));
            uint32_t want;

            if ((key_bytes & 1u) != 0u)
                key_bytes++;
            want = used_bytes + key_bytes + 4u + 2u * ((uint32_t)take + 2u);
            if (want > fx->node_size)
                break;
            used_bytes += (uint32_t)key_bytes + 4u;
            take++;
        }
        if (take == 0u) {
            free(fx);
            return NULL;
        }
        level2_key[level2_count] = leaf_key[cursor];
        level2[level2_count] = next_node;
        last_index_first = cursor;
        used[next_node >> 3] |= (uint8_t)(1u << (7u - (next_node & 7u)));
        last_index_free = fx_emit_index(fx, next_node, &leaves[cursor],
                                        &leaf_key[cursor], take, 2u, 0u, 0u);
        cursor += take;
        level2_count++;
        next_node++;
    }

    /*
     * Squeeze the LAST level-2 index node down to `index_slack` by giving it
     * more children: one extra single-record leaf each time, whose name length
     * is chosen so the index record it costs lands on the requested slack
     * exactly.  An index record for a thread key costs 2 + (6 + 2*len) + 4 and
     * one 2-byte offset slot, so 2*len + 14 bytes.
     */
    while (last_index_free > index_slack && fx->count + 1u <= FX_MAX_RECORDS &&
           leaf_count + 1u < FX_MAX_RECORDS) {
        unsigned budget = (unsigned)(last_index_free - index_slack);
        char name[128];
        size_t length;

        if (budget < 16u)
            break;
        length = (size_t)((budget - 14u) / 2u);
        if (length > 100u)
            length = 100u;
        memset(name, 'y', length);
        name[length] = '\0';
        leaf_key[leaf_count] = fx_next(fx);
        leaf_first[leaf_count] = fx->count;
        fx_thread(fx, fx->next_cnid, 3u, FX_ROOT, name);
        fx->next_cnid++;
        leaf_last[leaf_count] = fx->count;
        leaves[leaf_count] = next_node;
        used[next_node >> 3] |= (uint8_t)(1u << (7u - (next_node & 7u)));
        last_leaf_free = fx_emit_leaf(fx, next_node, leaf_first[leaf_count],
                                      fx->count, 0u, 0u);
        leaf_count++;
        next_node++;
        last_index_free = fx_emit_index(fx, level2[level2_count - 1u],
                                        &leaves[last_index_first],
                                        &leaf_key[last_index_first],
                                        leaf_count - last_index_first, 2u, 0u,
                                        0u);
    }
    fx->index_free = last_index_free;

    /*
     * Then squeeze the last LEAF down to `leaf_slack` with more thread
     * records, which cost 20 + 2*len each with their offset slot.
     */
    while (last_leaf_free > leaf_slack && fx->count + 1u <= FX_MAX_RECORDS) {
        unsigned budget = (unsigned)(last_leaf_free - leaf_slack);
        char name[128];
        size_t length;

        if (budget < 22u)
            break;
        length = (size_t)((budget - 20u) / 2u);
        if (length > 100u)
            length = 100u;
        memset(name, 'z', length);
        name[length] = '\0';
        fx_thread(fx, fx->next_cnid, 3u, FX_ROOT, name);
        fx->next_cnid++;
        leaf_last[leaf_count - 1u] = fx->count;
        last_leaf_free = fx_emit_leaf(fx, leaves[leaf_count - 1u],
                                      leaf_first[leaf_count - 1u], fx->count,
                                      0u, 0u);
    }
    fx->leaf_free = last_leaf_free;

    /* Chain both levels once every node's contents are final. */
    for (index = 0; index < leaf_count; index++) {
        uint8_t *node = fx_node(fx, leaves[index]);

        put_be32(node, index + 1u < leaf_count ? leaves[index + 1u] : 0u);
        put_be32(node + 4, index != 0u ? leaves[index - 1u] : 0u);
    }
    for (index = 0; index < level2_count; index++) {
        uint8_t *node = fx_node(fx, level2[index]);

        put_be32(node, index + 1u < level2_count ? level2[index + 1u] : 0u);
        put_be32(node + 4, index != 0u ? level2[index - 1u] : 0u);
    }

    if (levels >= 3u) {
        fx->root_node = next_node;
        used[next_node >> 3] |= (uint8_t)(1u << (7u - (next_node & 7u)));
        (void)fx_emit_index(fx, next_node, level2, level2_key, level2_count,
                            3u, 0u, 0u);
        next_node++;
        fx->root_children = (uint32_t)level2_count;
    } else {
        /* The index level IS the root, so there must be exactly one of it. */
        if (level2_count != 1u) {
            free(fx);
            return NULL;
        }
        fx->root_node = level2[0];
        fx->root_children = (uint32_t)(leaf_count - last_index_first);
    }

    fx->tree_depth = levels;
    fx->leaf_nodes = (uint32_t)leaf_count;
    fx->index_nodes = (uint32_t)level2_count;
    fx->first_leaf = leaves[0];
    fx->last_leaf = leaves[leaf_count - 1u];
    fx->last_leaf_records = (uint32_t)(leaf_last[leaf_count - 1u] -
                                       leaf_first[leaf_count - 1u]);
    fx->used_nodes = next_node;
    fx_emit_header(fx, levels, fx->root_node, fx->first_leaf, fx->last_leaf,
                   (uint32_t)fx->count, next_node, used);
    fx_emit_volume(fx);
    return fx;
}

/*
 * Mark every node of the catalog's node-allocation map in use and set
 * freeNodes to match.  A B-tree that is structurally fine but has no spare
 * node is a real state -- it is what a catalog looks like just before its fork
 * has to grow -- and it is the only way to reach the BTREE_FULL refusal.
 */
static void fx_exhaust_node_map(fixture_t *fx) {
    uint8_t *node = fx_node(fx, 0u);
    uint32_t map_bytes = (fx->catalog_nodes + 7u) / 8u;
    uint32_t index;

    for (index = 0; index < map_bytes; index++)
        node[248u + index] = 0xffu;
    put_be32(node + 14 + 26, 0u);
}

static fixture_t *fx_create_tight(uint16_t leave_free) {
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    uint16_t free_bytes;
    unsigned filler = 0;

    if (!fx)
        return NULL;
    free_bytes = fx_emit_leaf(fx, 1u, 0u, fx->count, 0u, 0u);
    /*
     * Filler records sort after everything the fixture ships and after each
     * other, because every one of them takes the next CNID from a rising
     * counter.  A folder-plus-thread pair costs 118 + 4*len bytes including
     * both offset slots; a lone thread costs 20 + 2*len.  Using pairs for the
     * bulk and a thread for the tail lands on the requested free space
     * exactly, which is what makes the node-full case a statement about size
     * rather than about luck.
     */
    while (free_bytes > leave_free && fx->count + 2u <= FX_MAX_RECORDS) {
        char name[128];
        uint32_t parent = 100u + 2u * filler;
        unsigned budget = (unsigned)(free_bytes - leave_free);
        size_t length;

        if (budget >= 122u) {
            length = (size_t)((budget - 118u) / 4u);
            if (length > 100u)
                length = 100u;
            /* Never leave a remainder too small for the next filler to use:
             * shortening this name by 11 units frees 44 more bytes. */
            while (length >= 11u && budget - (118u + 4u * (unsigned)length) > 0u
                   && budget - (118u + 4u * (unsigned)length) < 22u)
                length -= 11u;
            memset(name, 'a' + (int)(filler % 26u), length);
            name[length] = '\0';
            fx_folder(fx, parent, name, parent + 1u, 0u, 0x0010u, 0u);
            fx_thread(fx, parent + 1u, 3u, parent, name);
            fx->folder_count++;
        } else if (budget >= 22u) {
            length = (size_t)((budget - 20u) / 2u);
            if (length > 100u)
                length = 100u;
            while (length >= 11u && budget - (20u + 2u * (unsigned)length) > 0u
                   && budget - (20u + 2u * (unsigned)length) < 22u)
                length -= 11u;
            memset(name, 'z', length);
            name[length] = '\0';
            fx_thread(fx, parent, 3u, FX_ROOT, name);
        } else {
            break;
        }
        free_bytes = fx_emit_leaf(fx, 1u, 0u, fx->count, 0u, 0u);
        filler++;
    }
    fx->leaf_free = free_bytes;
    /* Keep nextCatalogID above every CNID the fillers consumed. */
    fx->next_cnid = 100u + 2u * filler + 2u;
    fx_emit_header(fx, 1u, 1u, 1u, 1u, (uint32_t)fx->count, 2u, NULL);
    fx_emit_volume(fx);
    return fx;
}

/* ------------------------------- scale fixture -------------------------- */

/*
 * A volume big enough to carry a real jailbreak payload, so the entry cap can
 * be tested rather than asserted.
 *
 * GEOMETRY, and why each number is what it is.  512-byte catalog nodes -- the
 * smallest this writer accepts -- because they are the worst case for splitting:
 * a 280-byte file record and its thread nearly fill one, so 1300 new records
 * force splits at the leaf level constantly, at level 2 about eighty times, and
 * at level 3 several times.  The shipping 7E18 catalog uses 4096-byte nodes and
 * would split an order of magnitude less; testing the small end proves the
 * large end.  4096-byte allocation blocks, matching the shipping volume, so a
 * symlink target still occupies exactly one block the way it does there.  The
 * tree starts at DEPTH 4 with one node per index level, which is a legal HFS+
 * shape and leaves the root room to take the level-3 nodes the run creates --
 * splitting the root is the one shape still refused, and this fixture is sized
 * so the run does not need it.
 */
#define SX_BLOCK_SIZE 4096u
#define SX_NODE_SIZE 512u
#define SX_TOTAL_BLOCKS 512u
#define SX_SIZE ((size_t)SX_BLOCK_SIZE * SX_TOTAL_BLOCKS)
#define SX_BITMAP_BLOCK 1u
#define SX_BITMAP_BYTES (SX_TOTAL_BLOCKS / 8u)
#define SX_CATALOG_BLOCK 2u
#define SX_CATALOG_BLOCKS 200u
#define SX_CATALOG_BYTES ((size_t)SX_CATALOG_BLOCKS * SX_BLOCK_SIZE)
#define SX_CATALOG_NODES ((uint32_t)(SX_CATALOG_BYTES / SX_NODE_SIZE))
#define SX_DATA_FIRST (SX_CATALOG_BLOCK + SX_CATALOG_BLOCKS)
#define SX_TAIL_BLOCK (SX_TOTAL_BLOCKS - 1u)
#define SX_DATA_BLOCKS (SX_TAIL_BLOCK - SX_DATA_FIRST)
#define SX_MAX_BASE_RECORDS 16u
/*
 * Free bytes left in every base leaf.  A real volume's leaves are not packed to
 * the last byte, and leaving room here matters for what this fixture proves:
 * without it the very first entry's folder record would not fit the leaf it
 * belongs in, and the run would fail on the shipped tree's fill rather than on
 * the payload's own growth, which is the thing under test.
 */
#define SX_LEAF_HEADROOM 200u

typedef struct scale_fixture {
    uint8_t *image;
    uint8_t record[SX_MAX_BASE_RECORDS][512];
    uint16_t length[SX_MAX_BASE_RECORDS];
    size_t count;
    uint32_t file_count;
    uint32_t folder_count;
    uint32_t next_cnid;
    uint32_t free_blocks;
    uint16_t tree_depth;
    uint32_t root_node;
    uint32_t first_leaf;
    uint32_t last_leaf;
    uint32_t leaf_nodes;
    uint32_t used_nodes;
} scale_fixture_t;

static uint8_t *sx_node(scale_fixture_t *sx, uint32_t index) {
    return sx->image + (size_t)SX_CATALOG_BLOCK * SX_BLOCK_SIZE +
           (size_t)index * SX_NODE_SIZE;
}

static void sx_bitmap_set(scale_fixture_t *sx, uint32_t block) {
    uint8_t *byte = sx->image + (size_t)SX_BITMAP_BLOCK * SX_BLOCK_SIZE +
                    (block >> 3);

    *byte = (uint8_t)(*byte | (uint8_t)(1u << (7u - (block & 7u))));
}

static void sx_emit_leaf(scale_fixture_t *sx, uint32_t node_index, size_t from,
                         size_t to, uint32_t flink, uint32_t blink) {
    uint8_t *node = sx_node(sx, node_index);
    uint16_t offset = 14u;
    size_t index;

    memset(node, 0, SX_NODE_SIZE);
    put_be32(node, flink);
    put_be32(node + 4, blink);
    node[8] = 0xffu;
    node[9] = 1u;
    put_be16(node + 10, (uint16_t)(to - from));
    for (index = from; index < to; index++) {
        put_be16(node + SX_NODE_SIZE - 2u * (index - from + 1u), offset);
        memcpy(node + offset, sx->record[index], sx->length[index]);
        offset = (uint16_t)(offset + sx->length[index]);
    }
    put_be16(node + SX_NODE_SIZE - 2u * ((to - from) + 1u), offset);
}

/* An index node over `count` children, each keyed by its own first record. */
static void sx_emit_index(scale_fixture_t *sx, uint32_t node_index,
                          const uint32_t *children,
                          const uint8_t *const *first_key, size_t count,
                          uint8_t level) {
    uint8_t *node = sx_node(sx, node_index);
    uint16_t offset = 14u;
    size_t index;

    memset(node, 0, SX_NODE_SIZE);
    node[8] = 0x00u;
    node[9] = level;
    put_be16(node + 10, (uint16_t)count);
    for (index = 0; index < count; index++) {
        uint16_t key_bytes = (uint16_t)(2u + get_be16(first_key[index]));

        if ((key_bytes & 1u) != 0u)
            key_bytes++;
        put_be16(node + SX_NODE_SIZE - 2u * (index + 1u), offset);
        memcpy(node + offset, first_key[index], key_bytes);
        put_be32(node + offset + key_bytes, children[index]);
        offset = (uint16_t)(offset + key_bytes + 4u);
    }
    put_be16(node + SX_NODE_SIZE - 2u * (count + 1u), offset);
}

static scale_fixture_t *sx_create(void) {
    scale_fixture_t *sx = (scale_fixture_t *)calloc(1u, sizeof(*sx));
    static const char FSTAB[] =
        "/dev/disk0s1 / hfs ro 0 1\n"
        "/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2\n";
    uint8_t *header;
    /* Zeroed because `sx->first_leaf = leaves[0]` below is only reachable with
     * at least one leaf, and GCC cannot prove that: it warns
     * -Wmaybe-uninitialized at -O2, which is -Werror in CI. The guard after the
     * emit loop makes the invariant explicit; this makes it cheap. */
    uint32_t leaves[SX_MAX_BASE_RECORDS] = { 0 };
    size_t leaf_first[SX_MAX_BASE_RECORDS];
    size_t leaf_count = 0;
    size_t cursor = 0;
    size_t index;
    uint32_t next_node = 1u;
    uint32_t block;

    if (!sx)
        return NULL;
    sx->image = (uint8_t *)calloc(1u, SX_SIZE);
    if (!sx->image) {
        free(sx);
        return NULL;
    }

    /* The same starter tree the standard fixture ships, so the CNIDs the tests
     * already name mean the same thing here. */
    sx->length[sx->count] = rec_folder(sx->record[sx->count], 1u, "TestVol",
                                       FX_ROOT, 2u, 0x0010u, 2u);
    sx->count++;
    sx->length[sx->count] = rec_thread(sx->record[sx->count], FX_ROOT, 3u, 1u,
                                       "TestVol");
    sx->count++;
    sx->length[sx->count] = rec_folder(sx->record[sx->count], FX_ROOT, "alpha",
                                       FX_ALPHA, 1u, 0x0010u, 1u);
    sx->count++;
    sx->length[sx->count] = rec_folder(sx->record[sx->count], FX_ROOT, "beta",
                                       FX_BETA, 1u, 0x0010u, 0u);
    sx->count++;
    sx->length[sx->count] = rec_thread(sx->record[sx->count], FX_ALPHA, 3u,
                                       FX_ROOT, "alpha");
    sx->count++;
    sx->length[sx->count] = rec_folder(sx->record[sx->count], FX_ALPHA, "dup",
                                       FX_DUP, 0u, 0x0010u, 0u);
    sx->count++;
    sx->length[sx->count] = rec_thread(sx->record[sx->count], FX_BETA, 3u,
                                       FX_ROOT, "beta");
    sx->count++;
    sx->length[sx->count] = rec_file(sx->record[sx->count], FX_BETA, "note.txt",
                                     FX_NOTE, 5u, SX_DATA_FIRST, 1u);
    sx->count++;
    sx->length[sx->count] = rec_thread(sx->record[sx->count], FX_DUP, 3u,
                                       FX_ALPHA, "dup");
    sx->count++;
    sx->length[sx->count] = rec_thread(sx->record[sx->count], FX_NOTE, 4u,
                                       FX_BETA, "note.txt");
    sx->count++;
    sx->file_count = 1u;
    sx->folder_count = 3u;
    sx->next_cnid = FX_NEXT_CNID;
    memcpy(sx->image + (size_t)SX_DATA_FIRST * SX_BLOCK_SIZE, "note\n", 5u);

    /* Pack the base records into leaves, filling each until the next record
     * plus its offset slot stops fitting. */
    while (cursor < sx->count) {
        size_t take = 0;
        uint32_t used_bytes = 14u;

        while (cursor + take < sx->count) {
            uint32_t want = used_bytes + sx->length[cursor + take] +
                            2u * ((uint32_t)take + 2u);

            if (want + SX_LEAF_HEADROOM > SX_NODE_SIZE)
                break;
            used_bytes += sx->length[cursor + take];
            take++;
        }
        if (take == 0u || leaf_count + 1u >= SX_MAX_BASE_RECORDS) {
            free(sx->image);
            free(sx);
            return NULL;
        }
        leaf_first[leaf_count] = cursor;
        leaves[leaf_count] = next_node;
        sx_emit_leaf(sx, next_node, cursor, cursor + take, 0u, 0u);
        cursor += take;
        leaf_count++;
        next_node++;
    }
    for (index = 0; index < leaf_count; index++) {
        uint8_t *node = sx_node(sx, leaves[index]);

        put_be32(node, index + 1u < leaf_count ? leaves[index + 1u] : 0u);
        put_be32(node + 4, index != 0u ? leaves[index - 1u] : 0u);
    }
    if (leaf_count == 0u) {               /* the invariant leaves[0] needs */
        free(sx->image);
        free(sx);
        return NULL;
    }
    sx->leaf_nodes = (uint32_t)leaf_count;
    sx->first_leaf = leaves[0];
    sx->last_leaf = leaves[leaf_count - 1u];

    /*
     * Levels 2, 3 and 4.  Level 2 names every leaf; levels 3 and 4 name one
     * child each, which is unusual but legal, and it is what leaves the root
     * the room this run needs -- splitting the root is the one shape still
     * refused, so the fixture must not start near it.
     */
    {
        const uint8_t *keys[SX_MAX_BASE_RECORDS];
        uint32_t child;
        const uint8_t *child_key;

        for (index = 0; index < leaf_count; index++)
            keys[index] = sx->record[leaf_first[index]];
        sx_emit_index(sx, next_node, leaves, keys, leaf_count, 2u);
        child_key = keys[0];
        child = next_node;
        next_node++;
        sx_emit_index(sx, next_node, &child, &child_key, 1u, 3u);
        child = next_node;
        next_node++;
        sx_emit_index(sx, next_node, &child, &child_key, 1u, 4u);
        sx->root_node = next_node;
        next_node++;
    }
    sx->tree_depth = 4u;
    sx->used_nodes = next_node;

    /* The B-tree header node. */
    {
        uint8_t *node = sx_node(sx, 0u);
        uint8_t *bt = node + 14;
        uint32_t used;

        memset(node, 0, SX_NODE_SIZE);
        node[8] = 0x01u;
        node[9] = 0u;
        put_be16(node + 10, 3u);
        put_be16(bt, sx->tree_depth);
        put_be32(bt + 2, sx->root_node);
        put_be32(bt + 6, (uint32_t)sx->count);
        put_be32(bt + 10, sx->first_leaf);
        put_be32(bt + 14, sx->last_leaf);
        put_be16(bt + 18, SX_NODE_SIZE);
        put_be16(bt + 20, 516u);
        put_be32(bt + 22, SX_CATALOG_NODES);
        put_be32(bt + 26, SX_CATALOG_NODES - sx->used_nodes);
        put_be32(bt + 32, (uint32_t)SX_CATALOG_BYTES);
        bt[36] = 0u;                        /* btreeType: hfs */
        bt[37] = 0xbcu;                     /* keyCompareType: HFSX binary */
        put_be32(bt + 38, 0x00000006u);     /* big keys + variable index keys */
        for (used = 0; used < sx->used_nodes; used++)
            node[248u + (used >> 3)] |= (uint8_t)(1u << (7u - (used & 7u)));
        put_be16(node + SX_NODE_SIZE - 2u, 14u);
        put_be16(node + SX_NODE_SIZE - 4u, 120u);
        put_be16(node + SX_NODE_SIZE - 6u, 248u);
        put_be16(node + SX_NODE_SIZE - 8u, (uint16_t)(SX_NODE_SIZE - 8u));
    }

    /* Bitmap: everything that is not free data. */
    memcpy(sx->image + 512u, FSTAB, sizeof(FSTAB) - 1u);
    sx_bitmap_set(sx, 0u);
    sx_bitmap_set(sx, SX_BITMAP_BLOCK);
    for (block = 0; block < SX_CATALOG_BLOCKS; block++)
        sx_bitmap_set(sx, SX_CATALOG_BLOCK + block);
    sx_bitmap_set(sx, SX_DATA_FIRST);       /* note.txt */
    sx_bitmap_set(sx, SX_TAIL_BLOCK);
    sx->free_blocks = SX_DATA_BLOCKS - 1u;

    header = sx->image + VH_OFF;
    memset(header, 0, VH_LEN);
    put_be16(header, 0x4858u);              /* HFSX */
    put_be16(header + 2, 5u);
    put_be32(header + 4, 1u << 8);          /* cleanly unmounted */
    memcpy(header + 8, "10.0", 4u);
    put_be32(header + 32, sx->file_count);
    put_be32(header + 36, sx->folder_count);
    put_be32(header + 40, SX_BLOCK_SIZE);
    put_be32(header + 44, SX_TOTAL_BLOCKS);
    put_be32(header + 48, sx->free_blocks);
    put_be32(header + 52, SX_DATA_FIRST);
    put_be32(header + 64, sx->next_cnid);
    put_be64(header + 112, SX_BITMAP_BYTES);
    put_be32(header + 124, 1u);
    put_be32(header + 128, SX_BITMAP_BLOCK);
    put_be32(header + 132, 1u);
    put_be64(header + 272, SX_CATALOG_BYTES);
    put_be32(header + 284, SX_CATALOG_BLOCKS);
    put_be32(header + 288, SX_CATALOG_BLOCK);
    put_be32(header + 292, SX_CATALOG_BLOCKS);
    memcpy(sx->image + SX_SIZE - VH_OFF, header, VH_LEN);
    return sx;
}

static void sx_release(scale_fixture_t *sx) {
    if (sx)
        free(sx->image);
    free(sx);
}

/* ------------------------------- independent reader --------------------- */

typedef struct tr_record {
    uint32_t parent;
    uint16_t name_length;
    uint16_t name[255];
    uint16_t type;
    uint16_t data_length;
    uint8_t data[768];
} tr_record_t;

typedef struct tr_volume {
    const uint8_t *image;
    size_t size;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t file_count;
    uint32_t folder_count;
    uint32_t next_cnid;
    uint8_t *catalog;
    size_t catalog_bytes;
    uint16_t node_size;
    uint16_t tree_depth;
    uint32_t root_node;
    uint32_t first_leaf;
    uint32_t last_leaf;
    uint32_t leaf_records;
    uint32_t total_nodes;
    uint32_t free_nodes;
    /* Byte offset of the allocation bitmap, read from the volume header's own
     * allocation-file extent rather than assumed, so the reader works on the
     * scale fixture's geometry as well as the standard one. */
    uint64_t bitmap_offset;
} tr_volume_t;

static int tr_open(const uint8_t *image, size_t size, tr_volume_t *vol) {
    const uint8_t *header = image + VH_OFF;
    uint64_t logical;
    size_t done = 0;
    unsigned extent;

    memset(vol, 0, sizeof(*vol));
    if (size < VH_OFF + VH_LEN)
        return 0;
    vol->image = image;
    vol->size = size;
    vol->block_size = get_be32(header + 40);
    vol->total_blocks = get_be32(header + 44);
    vol->free_blocks = get_be32(header + 48);
    vol->file_count = get_be32(header + 32);
    vol->folder_count = get_be32(header + 36);
    vol->next_cnid = get_be32(header + 64);
    vol->bitmap_offset = (uint64_t)get_be32(header + 128) * vol->block_size;
    logical = get_be64(header + 272);
    if (logical == 0u || logical > size)
        return 0;
    vol->catalog = (uint8_t *)malloc((size_t)logical);
    if (!vol->catalog)
        return 0;
    vol->catalog_bytes = (size_t)logical;
    for (extent = 0; extent < 8u && done < vol->catalog_bytes; extent++) {
        uint64_t start = get_be32(header + 288 + extent * 8u);
        uint64_t count = get_be32(header + 292 + extent * 8u);
        size_t span = (size_t)(count * vol->block_size);
        size_t take = span;

        if (count == 0u)
            continue;
        if (take > vol->catalog_bytes - done)
            take = vol->catalog_bytes - done;
        if (start * vol->block_size + take > size) {
            free(vol->catalog);
            vol->catalog = NULL;
            return 0;
        }
        memcpy(vol->catalog + done, image + start * vol->block_size, take);
        done += take;
    }
    vol->tree_depth = get_be16(vol->catalog + 14);
    vol->root_node = get_be32(vol->catalog + 14 + 2);
    vol->leaf_records = get_be32(vol->catalog + 14 + 6);
    vol->first_leaf = get_be32(vol->catalog + 14 + 10);
    vol->last_leaf = get_be32(vol->catalog + 14 + 14);
    vol->node_size = get_be16(vol->catalog + 14 + 18);
    vol->total_nodes = get_be32(vol->catalog + 14 + 22);
    vol->free_nodes = get_be32(vol->catalog + 14 + 26);
    return vol->node_size != 0u;
}

/* ---- direct node access, so a test can name the exact pointer it means ---- */

static const uint8_t *tr_node(const tr_volume_t *vol, uint32_t index) {
    if ((size_t)(index + 1u) * vol->node_size > vol->catalog_bytes)
        return NULL;
    return vol->catalog + (size_t)index * vol->node_size;
}

static uint32_t tr_flink(const tr_volume_t *vol, uint32_t index) {
    const uint8_t *node = tr_node(vol, index);
    return node ? get_be32(node) : UINT32_MAX;
}

static uint32_t tr_blink(const tr_volume_t *vol, uint32_t index) {
    const uint8_t *node = tr_node(vol, index);
    return node ? get_be32(node + 4) : UINT32_MAX;
}

static uint16_t tr_records(const tr_volume_t *vol, uint32_t index) {
    const uint8_t *node = tr_node(vol, index);
    return node ? get_be16(node + 10) : 0u;
}

/* Bit `index` of the node-allocation map in the header node's third record. */
static int tr_node_used(const tr_volume_t *vol, uint32_t index) {
    const uint8_t *node = tr_node(vol, 0u);
    uint16_t map_offset;

    if (!node)
        return -1;
    map_offset = get_be16(node + vol->node_size - 2u * 3u);
    return (node[map_offset + (index >> 3)] >> (7u - (index & 7u))) & 1;
}

/*
 * The child pointers of one index node, in record order.  Returns the count,
 * or 0 if the node is not an index node at `level`.
 */
static uint16_t tr_children(const tr_volume_t *vol, uint32_t index,
                            uint8_t level, uint32_t *out, size_t capacity) {
    const uint8_t *node = tr_node(vol, index);
    uint16_t count;
    uint16_t record;

    if (!node || node[8] != 0x00u || node[9] != level)
        return 0u;
    count = get_be16(node + 10);
    for (record = 0; record < count && (size_t)record < capacity; record++) {
        uint16_t offset = get_be16(node + vol->node_size -
                                   2u * ((uint32_t)record + 1u));
        uint16_t key_bytes = (uint16_t)(2u + get_be16(node + offset));

        if ((key_bytes & 1u) != 0u)
            key_bytes++;
        out[record] = get_be32(node + offset + key_bytes);
    }
    return count;
}

/* Walk one level by fLink from `first`, checking bLink on the way. */
static uint32_t tr_level_chain(const tr_volume_t *vol, uint32_t first,
                               uint32_t *out, size_t capacity, int *links_ok) {
    uint32_t index = first;
    uint32_t previous = 0;
    uint32_t count = 0;

    *links_ok = 1;
    while (index != 0u && count < capacity && count <= vol->total_nodes) {
        if (tr_blink(vol, index) != previous)
            *links_ok = 0;
        out[count++] = index;
        previous = index;
        index = tr_flink(vol, index);
    }
    if (index != 0u)
        *links_ok = 0;
    return count;
}

static void tr_close(tr_volume_t *vol) {
    free(vol->catalog);
    vol->catalog = NULL;
}

typedef struct tr_walk {
    uint32_t records;
    uint32_t nodes;
    uint32_t final_node;
    int order_ok;
    int shape_ok;
} tr_walk_t;

static int tr_key_less(uint32_t left_parent, const uint16_t *left,
                       uint16_t left_len, uint32_t right_parent,
                       const uint16_t *right, uint16_t right_len) {
    uint16_t limit = left_len < right_len ? left_len : right_len;
    uint16_t index;

    if (left_parent != right_parent)
        return left_parent < right_parent;
    for (index = 0; index < limit; index++)
        if (left[index] != right[index])
            return left[index] < right[index];
    return left_len < right_len;
}

/*
 * Walk the leaf CHAIN, exactly the way tools/hfsx_extract.py does, and hand
 * every record to `visit`.  Nothing here consults the writer's descent path.
 */
static void tr_walk(tr_volume_t *vol, tr_walk_t *walk,
                    void (*visit)(const tr_record_t *, void *), void *context) {
    uint32_t node_index = vol->first_leaf;
    uint32_t previous_parent = 0;
    uint16_t previous_name[255];
    uint16_t previous_length = 0;
    int have_previous = 0;

    memset(walk, 0, sizeof(*walk));
    walk->order_ok = 1;
    walk->shape_ok = 1;
    while (node_index != 0u && walk->nodes <= vol->total_nodes) {
        const uint8_t *node = vol->catalog + (size_t)node_index *
                              vol->node_size;
        uint16_t count;
        uint16_t index;

        if ((size_t)(node_index + 1u) * vol->node_size > vol->catalog_bytes ||
            node[8] != 0xffu || node[9] != 1u) {
            walk->shape_ok = 0;
            return;
        }
        count = get_be16(node + 10);
        for (index = 0; index < count; index++) {
            uint16_t offset = get_be16(node + vol->node_size - 2u *
                                       ((uint32_t)index + 1u));
            uint16_t end = get_be16(node + vol->node_size - 2u *
                                    ((uint32_t)index + 2u));
            const uint8_t *record = node + offset;
            tr_record_t parsed;
            uint16_t key_length;
            uint16_t data_offset;
            uint16_t unit;

            if (offset < 14u || end <= offset || end > vol->node_size) {
                walk->shape_ok = 0;
                return;
            }
            memset(&parsed, 0, sizeof(parsed));
            key_length = get_be16(record);
            parsed.parent = get_be32(record + 2);
            parsed.name_length = get_be16(record + 6);
            if (parsed.name_length > 255u ||
                6u + 2u * (uint32_t)parsed.name_length > key_length) {
                walk->shape_ok = 0;
                return;
            }
            for (unit = 0; unit < parsed.name_length; unit++)
                parsed.name[unit] = get_be16(record + 8 + unit * 2u);
            data_offset = (uint16_t)(2u + key_length);
            if ((data_offset & 1u) != 0u)
                data_offset++;
            if ((uint32_t)data_offset + 2u > (uint32_t)(end - offset)) {
                walk->shape_ok = 0;
                return;
            }
            parsed.type = get_be16(record + data_offset);
            parsed.data_length = (uint16_t)(end - offset - data_offset);
            if (parsed.data_length > sizeof(parsed.data))
                parsed.data_length = (uint16_t)sizeof(parsed.data);
            memcpy(parsed.data, record + data_offset, parsed.data_length);
            if (have_previous &&
                !tr_key_less(previous_parent, previous_name, previous_length,
                             parsed.parent, parsed.name, parsed.name_length))
                walk->order_ok = 0;
            previous_parent = parsed.parent;
            previous_length = parsed.name_length;
            memcpy(previous_name, parsed.name, sizeof(previous_name));
            have_previous = 1;
            walk->records++;
            if (visit)
                visit(&parsed, context);
        }
        walk->nodes++;
        walk->final_node = node_index;
        node_index = get_be32(node);
    }
    if (node_index != 0u)
        walk->shape_ok = 0;
}

typedef struct tr_find_context {
    uint32_t parent;
    uint16_t name[255];
    uint16_t name_length;
    int found;
    tr_record_t record;
} tr_find_context_t;

static void tr_find_visit(const tr_record_t *record, void *context) {
    tr_find_context_t *want = (tr_find_context_t *)context;

    if (want->found || record->parent != want->parent ||
        record->name_length != want->name_length)
        return;
    if (want->name_length != 0u &&
        memcmp(record->name, want->name,
               (size_t)want->name_length * sizeof(uint16_t)) != 0)
        return;
    want->found = 1;
    want->record = *record;
}

static int tr_find(tr_volume_t *vol, uint32_t parent, const char *name,
                   tr_record_t *out) {
    tr_find_context_t want;
    tr_walk_t walk;
    size_t index;
    size_t units = name ? strlen(name) : 0u;

    memset(&want, 0, sizeof(want));
    want.parent = parent;
    want.name_length = (uint16_t)units;
    for (index = 0; index < units; index++)
        want.name[index] = (uint16_t)(unsigned char)name[index];
    tr_walk(vol, &walk, tr_find_visit, &want);
    if (!walk.shape_ok || !walk.order_ok)
        return 0;
    if (want.found && out)
        *out = want.record;
    return want.found;
}

static int tr_bitmap(const tr_volume_t *vol, uint32_t block) {
    const uint8_t *bitmap = vol->image + vol->bitmap_offset;

    return (bitmap[block >> 3] >> (7u - (block & 7u))) & 1;
}

/* ------------------------------- host file helpers ---------------------- */

static int make_path(char *path, size_t capacity, const char *tag) {
    int length;

    g_serial++;
    length = snprintf(path, capacity, "rootfs_prov_%lu_%u_%s.img",
                      process_id(), g_serial, tag);
    return length > 0 && (size_t)length < capacity;
}

static int write_file(const char *path, const uint8_t *bytes, size_t size) {
    FILE *stream = fopen(path, "wb");
    int okay = stream != NULL;

    if (okay && size != 0u && fwrite(bytes, 1u, size, stream) != size)
        okay = 0;
    if (stream && fclose(stream) != 0)
        okay = 0;
    return okay;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *stream = fopen(path, "rb");
    uint8_t *bytes = NULL;
    long length;

    if (!stream)
        return NULL;
    if (fseek(stream, 0, SEEK_END) != 0 || (length = ftell(stream)) < 0 ||
        fseek(stream, 0, SEEK_SET) != 0) {
        (void)fclose(stream);
        return NULL;
    }
    bytes = (uint8_t *)malloc((size_t)length ? (size_t)length : 1u);
    if (bytes && (size_t)length != 0u &&
        fread(bytes, 1u, (size_t)length, stream) != (size_t)length) {
        free(bytes);
        bytes = NULL;
    }
    (void)fclose(stream);
    if (bytes)
        *size = (size_t)length;
    return bytes;
}

static int path_exists(const char *path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat info;
    return lstat(path, &info) == 0;
#endif
}

static void remove_if_present(const char *path) {
    if (path)
        (void)remove(path);
}

/* ------------------------------- test harness --------------------------- */

typedef struct run {
    char source[256];
    char destination[256];
    rootfs_work_options_t options;
    rootfs_work_result_t result;
    rootfs_work_status_t status;
    uint8_t *output;
    size_t output_size;
} run_t;

static int run_provision_image(run_t *run, const uint8_t *image, size_t size,
                               const char *tag,
                               const rootfs_work_entry_t *entries, size_t count,
                               uint64_t growth) {
    memset(run, 0, sizeof(*run));
    if (!make_path(run->source, sizeof(run->source), tag) ||
        !make_path(run->destination, sizeof(run->destination), tag))
        return 0;
    if (!write_file(run->source, image, size))
        return 0;
    run->options.fstab_line = ROOTFS_WORK_DEFAULT_FSTAB;
    run->options.entries = entries;
    run->options.entry_count = count;
    run->options.growth_bytes = growth;
    run->status = rootfs_work_create(run->source, run->destination,
                                     &run->options, &run->result);
    if (run->status == ROOTFS_WORK_OK)
        run->output = read_file(run->destination, &run->output_size);
    return 1;
}

static int run_provision(run_t *run, const fixture_t *fx, const char *tag,
                         const rootfs_work_entry_t *entries, size_t count,
                         uint64_t growth) {
    return run_provision_image(run, fx->image, FX_SIZE, tag, entries, count,
                               growth);
}

static void run_release(run_t *run) {
    free(run->output);
    run->output = NULL;
    remove_if_present(run->source);
    remove_if_present(run->destination);
}

/*
 * Every refusal is checked the same way: the call failed with the expected
 * named status, nothing was published, the destination entry does not exist,
 * no temporary was left behind, and the source image the provisioner was
 * pointed at is byte-for-byte what it was.  All provisioning refusals are
 * decided in the read-only plan phase, before the writer's first store, which
 * is what makes that last claim structural rather than lucky.
 */
static int is_provision_status(rootfs_work_status_t status) {
    switch (status) {
    case ROOTFS_WORK_PROVISION_INVALID:
    case ROOTFS_WORK_PROVISION_UNSUPPORTED:
    case ROOTFS_WORK_PROVISION_CATALOG_CORRUPT:
    case ROOTFS_WORK_PROVISION_PARENT_MISSING:
    case ROOTFS_WORK_PROVISION_EXISTS:
    case ROOTFS_WORK_PROVISION_NODE_FULL:
    case ROOTFS_WORK_PROVISION_LEAF_HEAD:
    case ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED:
    case ROOTFS_WORK_PROVISION_BTREE_FULL:
    case ROOTFS_WORK_PROVISION_NO_SPACE:
    case ROOTFS_WORK_PROVISION_LIMIT:
        return 1;
    default:
        return 0;
    }
}

static void expect_refusal(run_t *run, const fixture_t *fx,
                           rootfs_work_status_t expected, const char *what) {
    uint8_t *after = NULL;
    size_t after_size = 0;

    CHECK(run->status == expected,
          "%s: expected %s, got %s at %s (%s)", what,
          rootfs_work_status_name(expected),
          rootfs_work_status_name(run->status),
          rootfs_work_stage_name(run->result.stage), run->result.detail);
    /*
     * The load-bearing assertion.  Every provisioning refusal must be reported
     * against the PLAN stage: the plan is read-only, so a refusal that names
     * the write stage would mean bytes had already been stored before the
     * problem was noticed.  Nothing else in this file can observe the
     * unpublished work image, but this can observe when the decision was made.
     */
    if (is_provision_status(run->status))
        CHECK(run->result.stage == ROOTFS_WORK_STAGE_PROVISION_PLAN,
              "%s: refused at the %s stage, not before the first write", what,
              rootfs_work_stage_name(run->result.stage));
    CHECK(!run->result.published, "%s: refusal published a work image", what);
    CHECK(!run->result.temporary_left, "%s: refusal left a temporary", what);
    CHECK(!path_exists(run->destination),
          "%s: refusal created the destination entry", what);
    after = read_file(run->source, &after_size);
    CHECK(after != NULL && after_size == FX_SIZE &&
          memcmp(after, fx->image, FX_SIZE) == 0,
          "%s: the source image changed across the refusal", what);
    free(after);
}

static void expect_success(run_t *run, const char *what) {
    CHECK(run->status == ROOTFS_WORK_OK,
          "%s: expected ok, got %s at %s (%s)", what,
          rootfs_work_status_name(run->status),
          rootfs_work_stage_name(run->result.stage), run->result.detail);
    CHECK(run->result.published, "%s: success did not publish", what);
    CHECK(run->output != NULL, "%s: published image could not be read", what);
}

static void entry_directory(rootfs_work_entry_t *entry, const char *path,
                            uint16_t permissions) {
    memset(entry, 0, sizeof(*entry));
    entry->kind = ROOTFS_WORK_ENTRY_DIRECTORY;
    entry->path = path;
    entry->permissions = permissions;
}

static void entry_file(rootfs_work_entry_t *entry, const char *path,
                       const void *content, size_t size,
                       uint16_t permissions) {
    memset(entry, 0, sizeof(*entry));
    entry->kind = ROOTFS_WORK_ENTRY_FILE;
    entry->path = path;
    entry->content = (const uint8_t *)content;
    entry->content_size = size;
    entry->permissions = permissions;
}

/* For a symlink the content IS the target, and its length is the fork's
 * logicalSize -- the trailing NUL is deliberately not counted. */
static void entry_symlink(rootfs_work_entry_t *entry, const char *path,
                          const char *target, uint16_t permissions) {
    memset(entry, 0, sizeof(*entry));
    entry->kind = ROOTFS_WORK_ENTRY_SYMLINK;
    entry->path = path;
    entry->content = (const uint8_t *)target;
    entry->content_size = target ? strlen(target) : 0u;
    entry->permissions = permissions;
}

static int all_zero(const uint8_t *bytes, size_t count) {
    size_t index;

    for (index = 0; index < count; index++)
        if (bytes[index] != 0u)
            return 0;
    return 1;
}

/* ------------------------------- the tests ------------------------------ */

static void test_fixture_is_a_valid_volume(void) {
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    tr_volume_t vol;
    tr_walk_t walk;
    tr_record_t record;
    run_t run;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    CHECK(tr_open(fx->image, FX_SIZE, &vol), "fixture does not open");
    tr_walk(&vol, &walk, NULL, NULL);
    CHECK(walk.shape_ok, "fixture leaf chain is malformed");
    CHECK(walk.order_ok, "fixture keys are not in ascending order");
    CHECK(walk.records == vol.leaf_records,
          "fixture chain holds %u records, header says %u", walk.records,
          vol.leaf_records);
    CHECK(tr_find(&vol, FX_ALPHA, "dup", &record) && record.type == 1u,
          "fixture is missing /alpha/dup");
    CHECK(tr_find(&vol, FX_BETA, "note.txt", &record) && record.type == 2u,
          "fixture is missing /beta/note.txt");
    CHECK(!tr_find(&vol, FX_ROOT, "gamma", NULL),
          "fixture already has /gamma");
    tr_close(&vol);

    /* The provisioner must also accept it with no entries at all: that is
     * the control every refusal test is measured against. */
    if (run_provision(&run, fx, "control", NULL, 0u, 0u)) {
        expect_success(&run, "no-entry control");
        CHECK(run.result.provision_entries == 0u,
              "no-entry control reported %u provisioned entries",
              run.result.provision_entries);
        run_release(&run);
    }
    free(fx);
}

static void test_create_directory(void) {
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entry;
    run_t run;
    tr_volume_t vol;
    tr_walk_t walk;
    tr_record_t folder;
    tr_record_t thread;
    tr_record_t parent;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    entry_directory(&entry, "/gamma", 0750u);
    if (!run_provision(&run, fx, "mkdir", &entry, 1u, 0u)) {
        CHECK(0, "mkdir fixture setup failed");
        free(fx);
        return;
    }
    expect_success(&run, "create directory");
    CHECK(run.result.provision_entries == 1u &&
          run.result.provision_first_cnid == FX_NEXT_CNID &&
          run.result.provision_blocks == 0u,
          "directory report: entries=%u cnid=%u blocks=%u",
          run.result.provision_entries, run.result.provision_first_cnid,
          run.result.provision_blocks);
    if (run.output && tr_open(run.output, run.output_size, &vol)) {
        tr_walk(&vol, &walk, NULL, NULL);
        CHECK(walk.shape_ok && walk.order_ok,
              "provisioned catalog: shape_ok=%d order_ok=%d", walk.shape_ok,
              walk.order_ok);
        CHECK(walk.records == vol.leaf_records &&
              walk.records == (uint32_t)fx->count + 2u,
              "chain holds %u records, header %u, expected %u", walk.records,
              vol.leaf_records, (uint32_t)fx->count + 2u);
        CHECK(walk.final_node == vol.last_leaf,
              "chain ends at %u, header lastLeafNode %u", walk.final_node,
              vol.last_leaf);
        CHECK(tr_find(&vol, FX_ROOT, "gamma", &folder), "/gamma is missing");
        CHECK(folder.type == 1u, "/gamma is record type %u", folder.type);
        CHECK(get_be32(folder.data + 8) == FX_NEXT_CNID,
              "/gamma has CNID %u, expected %u", get_be32(folder.data + 8),
              FX_NEXT_CNID);
        CHECK(get_be32(folder.data + 4) == 0u,
              "/gamma valence is %u, expected 0", get_be32(folder.data + 4));
        CHECK(get_be16(folder.data + 42) == (0040000u | 0750u),
              "/gamma mode is 0%o", get_be16(folder.data + 42));
        CHECK((get_be16(folder.data + 2) & 0x0010u) != 0u,
              "/gamma did not inherit the volume's folder-count convention");
        CHECK(get_be32(folder.data + 84) == 0u, "/gamma folderCount is not 0");
        CHECK(get_be32(folder.data + 12) == ROOTFS_WORK_DEFAULT_MAC_TIME &&
              get_be32(folder.data + 16) == ROOTFS_WORK_DEFAULT_MAC_TIME,
              "/gamma dates are %u/%u", get_be32(folder.data + 12),
              get_be32(folder.data + 16));
        CHECK(tr_find(&vol, FX_NEXT_CNID, NULL, &thread),
              "/gamma has no thread record");
        CHECK(thread.type == 3u && get_be32(thread.data + 4) == FX_ROOT &&
              get_be16(thread.data + 8) == 5u,
              "/gamma thread: type=%u parent=%u nameLen=%u", thread.type,
              get_be32(thread.data + 4), get_be16(thread.data + 8));
        CHECK(tr_find(&vol, 1u, "TestVol", &parent), "root record vanished");
        CHECK(get_be32(parent.data + 4) == 3u,
              "root valence is %u, expected 3", get_be32(parent.data + 4));
        CHECK(get_be32(parent.data + 84) == 3u,
              "root folderCount is %u, expected 3",
              get_be32(parent.data + 84));
        CHECK(get_be32(parent.data + 16) == ROOTFS_WORK_DEFAULT_MAC_TIME,
              "root contentModDate was not touched");
        CHECK(vol.folder_count == 4u, "volume folderCount is %u, expected 4",
              vol.folder_count);
        CHECK(vol.file_count == 1u, "volume fileCount changed to %u",
              vol.file_count);
        CHECK(vol.next_cnid == FX_NEXT_CNID + 1u,
              "nextCatalogID is %u, expected %u", vol.next_cnid,
              FX_NEXT_CNID + 1u);
        CHECK(vol.free_blocks == FX_DATA_BLOCKS - 1u,
              "a directory consumed %u allocation blocks",
              (FX_DATA_BLOCKS - 1u) - vol.free_blocks);
        CHECK(memcmp(run.output + VH_OFF, run.output + run.output_size -
                     VH_OFF, VH_LEN) == 0,
              "primary and alternate volume headers diverged");
        tr_close(&vol);
    }
    run_release(&run);
    free(fx);
}

static void test_create_file(void) {
    static const char body[] = "hello from the provisioner\n";
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entry;
    run_t run;
    tr_volume_t vol;
    tr_record_t file;
    tr_record_t thread;
    tr_record_t parent;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    entry_file(&entry, "/alpha/hello.txt", body, sizeof(body) - 1u, 0600u);
    if (!run_provision(&run, fx, "mkfile", &entry, 1u, 0u)) {
        CHECK(0, "mkfile fixture setup failed");
        free(fx);
        return;
    }
    expect_success(&run, "create file");
    CHECK(run.result.provision_blocks == 1u,
          "file consumed %u allocation blocks", run.result.provision_blocks);
    if (run.output && tr_open(run.output, run.output_size, &vol)) {
        uint32_t start;
        uint32_t blocks;

        CHECK(tr_find(&vol, FX_ALPHA, "hello.txt", &file),
              "/alpha/hello.txt is missing");
        CHECK(file.type == 2u, "/alpha/hello.txt is record type %u", file.type);
        CHECK(get_be32(file.data + 8) == FX_NEXT_CNID,
              "file CNID is %u", get_be32(file.data + 8));
        CHECK((get_be16(file.data + 2) & 0x0002u) != 0u,
              "file record does not advertise its thread");
        CHECK(get_be16(file.data + 42) == (0100000u | 0600u),
              "file mode is 0%o", get_be16(file.data + 42));
        CHECK(get_be64(file.data + 88) == sizeof(body) - 1u,
              "logicalSize is %llu, expected %llu",
              (unsigned long long)get_be64(file.data + 88),
              (unsigned long long)(sizeof(body) - 1u));
        blocks = get_be32(file.data + 100);
        start = get_be32(file.data + 104);
        CHECK(blocks == 1u && get_be32(file.data + 108) == 1u,
              "file owns %u blocks, extent count %u", blocks,
              get_be32(file.data + 108));
        CHECK(get_be64(file.data + 168) == 0u &&
              get_be32(file.data + 180) == 0u,
              "the resource fork is not empty");
        CHECK(start >= FX_DATA_FIRST && start < FX_DATA_FIRST + FX_DATA_BLOCKS,
              "file was placed in block %u, outside the data area", start);
        CHECK(tr_bitmap(&vol, start),
              "allocation bit for block %u was not set", start);
        CHECK(vol.free_blocks == FX_DATA_BLOCKS - 2u,
              "freeBlocks is %u, expected %u", vol.free_blocks,
              FX_DATA_BLOCKS - 2u);
        CHECK(memcmp(run.output + (size_t)start * FX_BLOCK_SIZE, body,
                     sizeof(body) - 1u) == 0,
              "the file's bytes are not in its allocation block");
        {
            size_t slack = FX_BLOCK_SIZE - (sizeof(body) - 1u);
            size_t index;
            int zero = 1;

            for (index = 0; index < slack; index++)
                if (run.output[(size_t)start * FX_BLOCK_SIZE +
                               sizeof(body) - 1u + index] != 0u)
                    zero = 0;
            CHECK(zero, "the tail of the file's block is not zeroed");
        }
        CHECK(tr_find(&vol, FX_NEXT_CNID, NULL, &thread),
              "the file has no thread record");
        CHECK(thread.type == 4u && get_be32(thread.data + 4) == FX_ALPHA,
              "file thread: type=%u parent=%u", thread.type,
              get_be32(thread.data + 4));
        CHECK(tr_find(&vol, FX_ROOT, "alpha", &parent), "alpha vanished");
        CHECK(get_be32(parent.data + 4) == 2u,
              "alpha valence is %u, expected 2", get_be32(parent.data + 4));
        CHECK(get_be32(parent.data + 84) == 1u,
              "alpha folderCount changed to %u for a FILE child",
              get_be32(parent.data + 84));
        CHECK(vol.file_count == 2u, "volume fileCount is %u, expected 2",
              vol.file_count);
        CHECK(vol.folder_count == 3u, "volume folderCount changed to %u",
              vol.folder_count);
        tr_close(&vol);
    }
    run_release(&run);

    /*
     * A zero-byte file needs no allocation block at all, and the Cydia
     * payload has several.  It must still get a record, a thread and an
     * empty fork rather than a one-block extent full of nothing.
     */
    entry_file(&entry, "/alpha/empty", NULL, 0u, 0u);
    if (run_provision(&run, fx, "empty", &entry, 1u, 0u)) {
        tr_volume_t emptyVol;
        tr_record_t emptyFile;

        expect_success(&run, "zero-byte file");
        CHECK(run.result.provision_blocks == 0u,
              "an empty file consumed %u allocation blocks",
              run.result.provision_blocks);
        if (run.output && tr_open(run.output, run.output_size, &emptyVol)) {
            CHECK(tr_find(&emptyVol, FX_ALPHA, "empty", &emptyFile) && emptyFile.type == 2u,
                  "/alpha/empty is missing");
            CHECK(get_be64(emptyFile.data + 88) == 0u &&
                  get_be32(emptyFile.data + 100) == 0u &&
                  get_be32(emptyFile.data + 104) == 0u &&
                  get_be32(emptyFile.data + 108) == 0u,
                  "an empty file was given a fork: size=%llu blocks=%u "
                  "ext=(%u,%u)", (unsigned long long)get_be64(emptyFile.data + 88),
                  get_be32(emptyFile.data + 100), get_be32(emptyFile.data + 104),
                  get_be32(emptyFile.data + 108));
            CHECK(tr_find(&emptyVol, FX_NEXT_CNID, NULL, &emptyFile) && emptyFile.type == 4u,
                  "the empty file has no thread record");
            CHECK(emptyVol.free_blocks == FX_DATA_BLOCKS - 1u,
                  "an empty file changed freeBlocks to %u", emptyVol.free_blocks);
            tr_close(&emptyVol);
        }
        run_release(&run);
    }
    free(fx);
}

static void test_create_directory_and_file_together(void) {
    static const char body[] = "nested\n";
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entries[2];
    run_t run;
    tr_volume_t vol;
    tr_record_t folder;
    tr_record_t file;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    /* The second entry's parent does not exist until the first entry is
     * planned, which is the whole point of resolving against planned state. */
    entry_directory(&entries[0], "/gamma", 0u);
    entry_file(&entries[1], "/gamma/inner.txt", body, sizeof(body) - 1u, 0u);
    if (!run_provision(&run, fx, "nested", entries, 2u, 0u)) {
        CHECK(0, "nested fixture setup failed");
        free(fx);
        return;
    }
    expect_success(&run, "directory then file inside it");
    CHECK(run.result.provision_entries == 2u &&
          run.result.provision_first_cnid == FX_NEXT_CNID &&
          run.result.provision_last_cnid == FX_NEXT_CNID + 1u,
          "nested report: entries=%u cnids %u..%u",
          run.result.provision_entries, run.result.provision_first_cnid,
          run.result.provision_last_cnid);
    if (run.output && tr_open(run.output, run.output_size, &vol)) {
        tr_walk_t walk;

        tr_walk(&vol, &walk, NULL, NULL);
        CHECK(walk.shape_ok && walk.order_ok && walk.records ==
              vol.leaf_records && walk.records == (uint32_t)fx->count + 4u,
              "nested chain: shape=%d order=%d records=%u header=%u",
              walk.shape_ok, walk.order_ok, walk.records, vol.leaf_records);
        CHECK(tr_find(&vol, FX_ROOT, "gamma", &folder) && folder.type == 1u,
              "/gamma is missing");
        CHECK(get_be32(folder.data + 4) == 1u,
              "/gamma valence is %u, expected 1 after gaining a child",
              get_be32(folder.data + 4));
        CHECK(get_be32(folder.data + 84) == 0u,
              "/gamma folderCount changed for a FILE child");
        CHECK(tr_find(&vol, FX_NEXT_CNID, "inner.txt", &file) &&
              file.type == 2u, "/gamma/inner.txt is missing");
        CHECK(get_be32(file.data + 8) == FX_NEXT_CNID + 1u,
              "inner.txt CNID is %u", get_be32(file.data + 8));
        CHECK(vol.next_cnid == FX_NEXT_CNID + 2u,
              "nextCatalogID is %u, expected %u", vol.next_cnid,
              FX_NEXT_CNID + 2u);
        CHECK(vol.file_count == 2u && vol.folder_count == 4u,
              "counts are file=%u folder=%u", vol.file_count,
              vol.folder_count);
        CHECK(memcmp(run.output + (size_t)get_be32(file.data + 104) *
                     FX_BLOCK_SIZE, body, sizeof(body) - 1u) == 0,
              "inner.txt's bytes are not where its extent says");
        tr_close(&vol);
    }
    run_release(&run);
    free(fx);
}

static void test_deep_existing_parents(void) {
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entry;
    run_t run;
    tr_volume_t vol;
    tr_record_t folder;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    entry_directory(&entry, "/alpha/dup/deep", 0u);
    if (!run_provision(&run, fx, "deep", &entry, 1u, 0u)) {
        CHECK(0, "deep fixture setup failed");
        free(fx);
        return;
    }
    expect_success(&run, "directory two levels down");
    if (run.output && tr_open(run.output, run.output_size, &vol)) {
        CHECK(tr_find(&vol, FX_DUP, "deep", &folder) && folder.type == 1u,
              "/alpha/dup/deep is missing");
        CHECK(tr_find(&vol, FX_ALPHA, "dup", &folder) &&
              get_be32(folder.data + 4) == 1u &&
              get_be32(folder.data + 84) == 1u,
              "dup's valence/folderCount were not both bumped");
        CHECK(tr_find(&vol, FX_ROOT, "alpha", &folder) &&
              get_be32(folder.data + 4) == 1u,
              "alpha's valence changed even though it gained no direct child");
        tr_close(&vol);
    }
    run_release(&run);
    free(fx);
}

static void test_missing_parent_is_refused(void) {
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entry;
    run_t run;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    entry_directory(&entry, "/nowhere/deep", 0u);
    if (run_provision(&run, fx, "noparent", &entry, 1u, 0u)) {
        expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_PARENT_MISSING,
                       "missing intermediate directory");
        run_release(&run);
    }
    /* A path component that exists but is a FILE is equally not a parent. */
    entry_directory(&entry, "/beta/note.txt/deeper", 0u);
    if (run_provision(&run, fx, "fileparent", &entry, 1u, 0u)) {
        expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_PARENT_MISSING,
                       "file used as a parent directory");
        run_release(&run);
    }
    /* And the reverse ordering of a two-entry request: the child first. */
    {
        rootfs_work_entry_t entries[2];

        entry_directory(&entries[0], "/gamma/inner", 0u);
        entry_directory(&entries[1], "/gamma", 0u);
        if (run_provision(&run, fx, "misordered", entries, 2u, 0u)) {
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_PARENT_MISSING,
                           "child requested before its parent");
            run_release(&run);
        }
    }
    free(fx);
}

static void test_duplicate_name_is_refused(void) {
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entry;
    rootfs_work_entry_t entries[2];
    run_t run;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    entry_directory(&entry, "/alpha/dup", 0u);
    if (run_provision(&run, fx, "dupdir", &entry, 1u, 0u)) {
        expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_EXISTS,
                       "directory that already exists");
        run_release(&run);
    }
    entry_file(&entry, "/beta/note.txt", "x", 1u, 0u);
    if (run_provision(&run, fx, "dupfile", &entry, 1u, 0u)) {
        expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_EXISTS,
                       "file that already exists");
        run_release(&run);
    }
    /* Same name twice in one request: the second must see the first. */
    entry_directory(&entries[0], "/gamma", 0u);
    entry_directory(&entries[1], "/gamma", 0u);
    if (run_provision(&run, fx, "duptwice", entries, 2u, 0u)) {
        expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_EXISTS,
                       "the same path requested twice");
        run_release(&run);
    }
    /*
     * The same LEAF NAME under a different parent is NOT a duplicate.  A key
     * comparison that ignored the parent CNID would refuse this, so it is the
     * discriminating case for that mutation.
     */
    entry_directory(&entry, "/beta/dup", 0u);
    if (run_provision(&run, fx, "sameleaf", &entry, 1u, 0u)) {
        tr_volume_t vol;
        tr_record_t folder;

        expect_success(&run, "same leaf name under a different parent");
        if (run.output && tr_open(run.output, run.output_size, &vol)) {
            tr_walk_t walk;

            tr_walk(&vol, &walk, NULL, NULL);
            CHECK(walk.order_ok,
                  "inserting a same-named sibling broke global key order");
            CHECK(tr_find(&vol, FX_BETA, "dup", &folder) && folder.type == 1u,
                  "/beta/dup is missing");
            CHECK(get_be32(folder.data + 8) == FX_NEXT_CNID,
                  "/beta/dup has CNID %u", get_be32(folder.data + 8));
            CHECK(tr_find(&vol, FX_ALPHA, "dup", &folder) &&
                  get_be32(folder.data + 8) == FX_DUP,
                  "/alpha/dup was disturbed");
            tr_close(&vol);
        }
        run_release(&run);
    }
    free(fx);
}

static void test_full_leaf_is_refused_not_split(void) {
    fixture_t *tight = fx_create_tight(64u);
    fixture_t *roomy = fx_create_tight(330u);
    rootfs_work_entry_t entry;
    run_t run;

    if (!tight || !roomy) {
        CHECK(0, "tight fixture allocation failed");
        free(tight);
        free(roomy);
        return;
    }
    /*
     * The padding must land exactly, or the two cases below stop meaning what
     * they say: 64 bytes is under the 108 a folder record plus its offset slot
     * needs, and 330 is over that but under the 414 the file case needs.
     */
    CHECK(tight->leaf_free == 64u, "tight fixture left %u free bytes, not 64",
          tight->leaf_free);
    CHECK(roomy->leaf_free == 330u, "roomy fixture left %u free bytes, not 330",
          roomy->leaf_free);
    /*
     * A folder record plus its thread needs well over 64 bytes.
     *
     * The refusal is SPLIT_UNSUPPORTED and the reason is the fixture's shape,
     * not the writer's reach: these are depth-1 volumes, so the one leaf IS the
     * root, and splitting a root means growing a new one and incrementing
     * treeDepth -- the one split shape that is still refused by name.  In a
     * tree with an index level above the leaf this same request is served by
     * the general split; test_split_boundaries_are_refused() below runs exactly
     * that case and it succeeds.
     */
    entry_directory(&entry, "/gamma", 0u);
    if (run_provision(&run, tight, "tight", &entry, 1u, 0u)) {
        expect_refusal(&run, tight, ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED,
                       "leaf with no room for the record");
        run_release(&run);
    }
    /*
     * The same request against a leaf with room must succeed, so the refusal
     * above is about the space and not about the padded fixture.
     */
    if (run_provision(&run, roomy, "roomy", &entry, 1u, 0u)) {
        expect_success(&run, "leaf with room for the record");
        run_release(&run);
    }
    /*
     * A 34-character file name makes the file record 326 bytes with its slot
     * and its thread record 88, so 330 free bytes take the first and refuse
     * the second.  That is the case where an entry is half-representable:
     * the plan must abandon the whole entry, not leave a file record whose
     * thread is missing.
     *
     * The refusal is SPLIT_UNSUPPORTED rather than NODE_FULL, and the
     * distinction is the point.  The file record lands INSIDE the leaf, among
     * keys under CNID 16; the thread record's key is (nextCatalogID, "") which
     * sorts after every key the fixture holds, so it is a rightmost append and
     * the writer does try to split for it.  In this depth-1 fixture the leaf
     * IS the root, so the split would have to grow the tree a level -- which
     * is the one rightmost case that is still refused, by name.
     */
    {
        static const char body[] = "x";
        rootfs_work_entry_t big;

        entry_file(&big, "/alpha/a-file-with-a-fairly-long-name.bin", body, 1u,
                   0u);
        if (run_provision(&run, roomy, "halfway", &big, 1u, 0u)) {
            expect_refusal(&run, roomy,
                           ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED,
                           "leaf that fits the record but not the thread");
            run_release(&run);
        }
    }
    /*
     * The boundary itself.  /gamma costs 106 + 2 for the folder record and its
     * offset slot, then 28 + 2 for the thread: 138 bytes exactly.  138 free
     * must fit and 136 must not, which pins the comparison rather than just
     * its direction.  At 136 it is again the THREAD that does not fit, and a
     * thread key is always a rightmost append, so the two-bytes-short refusal
     * is SPLIT_UNSUPPORTED in this root-is-a-leaf fixture.
     */
    {
        fixture_t *exact = fx_create_tight(138u);
        fixture_t *short_by_two = fx_create_tight(136u);

        if (exact && short_by_two) {
            CHECK(exact->leaf_free == 138u && short_by_two->leaf_free == 136u,
                  "boundary fixtures left %u and %u free bytes",
                  exact->leaf_free, short_by_two->leaf_free);
            entry_directory(&entry, "/gamma", 0u);
            if (run_provision(&run, exact, "exactfit", &entry, 1u, 0u)) {
                expect_success(&run, "leaf with exactly enough room");
                run_release(&run);
            }
            if (run_provision(&run, short_by_two, "twoshort", &entry, 1u, 0u)) {
                expect_refusal(&run, short_by_two,
                               ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED,
                               "leaf two bytes short of enough room");
                run_release(&run);
            }
        } else {
            CHECK(0, "boundary fixture allocation failed");
        }
        free(exact);
        free(short_by_two);
    }
    free(tight);
    free(roomy);
}

static void test_out_of_space_is_refused(void) {
    fixture_t *empty = fx_create(0u);
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entry;
    run_t run;
    uint8_t *big = NULL;

    if (!empty || !fx) {
        CHECK(0, "fixture allocation failed");
        free(empty);
        free(fx);
        return;
    }
    /*
     * freeBlocks = 0 is exactly what the shipping rootfs looks like.  Asking
     * for a file without asking for growth must name the problem, not guess.
     */
    entry_file(&entry, "/alpha/needs-space.bin", "x", 1u, 0u);
    if (run_provision(&run, empty, "nospace", &entry, 1u, 0u)) {
        expect_refusal(&run, empty, ROOTFS_WORK_PROVISION_NO_SPACE,
                       "file on a volume with freeBlocks = 0");
        run_release(&run);
    }
    /* A directory needs no blocks, so the same volume still accepts one. */
    entry_directory(&entry, "/gamma", 0u);
    if (run_provision(&run, empty, "dironfull", &entry, 1u, 0u)) {
        expect_success(&run, "directory on a volume with freeBlocks = 0");
        run_release(&run);
    }
    /*
     * ORDERING.  The same file request succeeds once growth has run first,
     * which is the enforcement this pipeline is built around: provisioning
     * observes the post-growth header, never the caller's intent.
     */
    entry_file(&entry, "/alpha/needs-space.bin", "x", 1u, 0u);
    if (run_provision(&run, empty, "grown", &entry, 1u,
                      (uint64_t)8u * FX_BLOCK_SIZE)) {
        expect_success(&run, "file on a volume grown first");
        if (run.output) {
            tr_volume_t vol;
            tr_record_t file;

            if (tr_open(run.output, run.output_size, &vol)) {
                CHECK(tr_find(&vol, FX_ALPHA, "needs-space.bin", &file),
                      "the grown volume did not get the file");
                CHECK(vol.total_blocks > FX_BLOCKS,
                      "the volume was not actually grown (%u blocks)",
                      vol.total_blocks);
                tr_close(&vol);
            }
        }
        run_release(&run);
    }
    /* Content larger than the free space is a refusal, not a truncation. */
    big = (uint8_t *)calloc(1u, (size_t)FX_BLOCK_SIZE * (FX_DATA_BLOCKS + 4u));
    if (big) {
        entry_file(&entry, "/alpha/huge.bin", big,
                   (size_t)FX_BLOCK_SIZE * (FX_DATA_BLOCKS + 4u), 0u);
        if (run_provision(&run, fx, "toobig", &entry, 1u, 0u)) {
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_NO_SPACE,
                           "content larger than the free space");
            run_release(&run);
        }
        free(big);
    }
    free(empty);
    free(fx);
}

/*
 * The headline safety property: a catalog that cannot be walked must be
 * refused as broken, never answered as empty.  Each case below breaks the
 * tree a different way and asserts CATALOG_CORRUPT -- in particular, the
 * stale-firstLeafNode case is the one that made tools/hfsx_extract.py report
 * "not found" for files that were plainly there.
 */
static void test_broken_catalog_is_never_absence(void) {
    static const struct {
        const char *what;
        size_t offset;         /* relative to the catalog's first byte */
        uint32_t value;
        unsigned width;
    } damage[] = {
        {"stale firstLeafNode",        14u + 10u, 7u,     4u},
        {"firstLeafNode past the end", 14u + 10u, 9999u,  4u},
        {"wrong leafRecords",          14u + 6u,  4u,     4u},
        {"wrong lastLeafNode",         14u + 14u, 5u,     4u},
        {"rootNode past the end",      14u + 2u,  9999u,  4u},
        {"treeDepth 0",                14u + 0u,  0u,     2u},
        {"treeDepth 9",                14u + 0u,  9u,     2u}
    };
    size_t index;

    for (index = 0; index < sizeof(damage) / sizeof(damage[0]); index++) {
        fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
        rootfs_work_entry_t entry;
        run_t run;
        uint8_t *target;

        if (!fx) {
            CHECK(0, "fixture allocation failed");
            return;
        }
        target = fx->image + FX_CATALOG_BLOCK * FX_BLOCK_SIZE +
                 damage[index].offset;
        if (damage[index].width == 4u)
            put_be32(target, damage[index].value);
        else
            put_be16(target, (uint16_t)damage[index].value);
        entry_directory(&entry, "/gamma", 0u);
        if (run_provision(&run, fx, "broken", &entry, 1u, 0u)) {
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                           damage[index].what);
            run_release(&run);
        }
        free(fx);
    }

    /* A leaf node claiming to be an index node. */
    {
        fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
        rootfs_work_entry_t entry;
        run_t run;

        if (fx) {
            fx_node(fx, 1u)[8] = 0x00u;
            entry_directory(&entry, "/gamma", 0u);
            if (run_provision(&run, fx, "kind", &entry, 1u, 0u)) {
                expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                               "leaf node with an index node's kind");
                run_release(&run);
            }
            free(fx);
        }
    }
    /* A leaf node at the wrong height. */
    {
        fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
        rootfs_work_entry_t entry;
        run_t run;

        if (fx) {
            fx_node(fx, 1u)[9] = 3u;
            entry_directory(&entry, "/gamma", 0u);
            if (run_provision(&run, fx, "height", &entry, 1u, 0u)) {
                expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                               "leaf node at the wrong height");
                run_release(&run);
            }
            free(fx);
        }
    }
    /* Two records swapped, so the leaf's keys descend. */
    {
        fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
        rootfs_work_entry_t entry;
        run_t run;
        uint8_t *node;
        uint16_t a;
        uint16_t b;

        if (fx) {
            node = fx_node(fx, 1u);
            a = get_be16(node + FX_NODE_SIZE - 2u * 3u);
            b = get_be16(node + FX_NODE_SIZE - 2u * 4u);
            put_be16(node + FX_NODE_SIZE - 2u * 3u, b);
            put_be16(node + FX_NODE_SIZE - 2u * 4u, a);
            entry_directory(&entry, "/gamma", 0u);
            if (run_provision(&run, fx, "order", &entry, 1u, 0u)) {
                expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                               "leaf whose offset array descends");
                run_release(&run);
            }
            free(fx);
        }
    }
    /*
     * Keys out of order while the offset array stays perfectly monotonic.
     * Swapping the parent CNIDs of the two 8-byte thread keys (16,"") and
     * (17,"") leaves every record well-formed and every offset ascending, so
     * only a reader that actually compares adjacent KEYS can see it.
     */
    {
        fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
        rootfs_work_entry_t entry;
        run_t run;
        uint8_t *node;

        if (fx) {
            node = fx_node(fx, 1u);
            put_be32(node + get_be16(node + FX_NODE_SIZE - 2u * 5u) + 2u,
                     FX_BETA);
            put_be32(node + get_be16(node + FX_NODE_SIZE - 2u * 7u) + 2u,
                     FX_ALPHA);
            entry_directory(&entry, "/gamma", 0u);
            if (run_provision(&run, fx, "keyorder", &entry, 1u, 0u)) {
                expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                               "leaf whose keys descend inside a valid layout");
                run_release(&run);
            }
            free(fx);
        }
    }
    /* A record count the offset array cannot support. */
    {
        fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
        rootfs_work_entry_t entry;
        run_t run;

        if (fx) {
            put_be16(fx_node(fx, 1u) + 10, 900u);
            entry_directory(&entry, "/gamma", 0u);
            if (run_provision(&run, fx, "count", &entry, 1u, 0u)) {
                expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                               "leaf claiming impossibly many records");
                run_release(&run);
            }
            free(fx);
        }
    }
    /* A missing root thread record is a broken catalog, not a missing path. */
    {
        fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
        rootfs_work_entry_t entry;
        run_t run;
        uint8_t *node;

        if (fx) {
            /* Turn the root folder thread into a file thread. */
            node = fx_node(fx, 1u);
            put_be16(node + get_be16(node + FX_NODE_SIZE - 4u) + 8u, 4u);
            entry_directory(&entry, "/gamma", 0u);
            if (run_provision(&run, fx, "rootthread", &entry, 1u, 0u)) {
                expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                               "root thread record with the wrong type");
                run_release(&run);
            }
            free(fx);
        }
    }
    /* nextCatalogID pointing at a CNID that is already a thread key. */
    {
        fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
        rootfs_work_entry_t entry;
        run_t run;

        if (fx) {
            put_be32(fx->image + VH_OFF + 64, FX_DUP);
            memcpy(fx->image + FX_SIZE - VH_OFF, fx->image + VH_OFF, VH_LEN);
            entry_directory(&entry, "/gamma", 0u);
            if (run_provision(&run, fx, "cnidreuse", &entry, 1u, 0u)) {
                expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
                               "nextCatalogID naming a CNID already in use");
                run_release(&run);
            }
            free(fx);
        }
    }
}

static void test_unsupported_catalogs_are_refused(void) {
    static const struct {
        const char *what;
        size_t offset;
        uint32_t value;
        unsigned width;
    } damage[] = {
        {"case-folding key order", 14u + 37u, 0xcfu, 1u},
        {"unknown key order",      14u + 37u, 0x00u, 1u},
        {"non-HFS btreeType",      14u + 36u, 0xffu, 1u},
        {"fixed index keys",       14u + 38u, 0x02u, 4u},
        {"small keys",             14u + 38u, 0x04u, 4u},
        {"nodeSize 768",           14u + 18u, 768u,  2u},
        {"nodeSize 256",           14u + 18u, 256u,  2u}
    };
    size_t index;

    for (index = 0; index < sizeof(damage) / sizeof(damage[0]); index++) {
        fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
        rootfs_work_entry_t entry;
        run_t run;
        uint8_t *target;

        if (!fx) {
            CHECK(0, "fixture allocation failed");
            return;
        }
        target = fx->image + FX_CATALOG_BLOCK * FX_BLOCK_SIZE +
                 damage[index].offset;
        if (damage[index].width == 4u)
            put_be32(target, damage[index].value);
        else if (damage[index].width == 2u)
            put_be16(target, (uint16_t)damage[index].value);
        else
            *target = (uint8_t)damage[index].value;
        entry_directory(&entry, "/gamma", 0u);
        if (run_provision(&run, fx, "unsup", &entry, 1u, 0u)) {
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_UNSUPPORTED,
                           damage[index].what);
            run_release(&run);
        }
        free(fx);
    }
    /* A catalog whose inline extents do not cover its own block count is
     * spilled into the extents-overflow file; that is a real HFS+ shape this
     * writer refuses rather than guesses at. */
    {
        fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
        rootfs_work_entry_t entry;
        run_t run;

        if (fx) {
            put_be32(fx->image + VH_OFF + 284, FX_CATALOG_BLOCKS + 1u);
            memcpy(fx->image + FX_SIZE - VH_OFF, fx->image + VH_OFF, VH_LEN);
            entry_directory(&entry, "/gamma", 0u);
            if (run_provision(&run, fx, "spill", &entry, 1u, 0u)) {
                expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_UNSUPPORTED,
                               "catalog with an extents-overflow spill");
                run_release(&run);
            }
            free(fx);
        }
    }
}

static void test_index_descent_and_leaf_head(void) {
    fixture_t *fx = fx_create_depth2(0);
    fixture_t *bad = fx_create_depth2(1);
    rootfs_work_entry_t entry;
    run_t run;

    if (!fx || !bad) {
        CHECK(0, "depth-2 fixture allocation failed");
        free(fx);
        free(bad);
        return;
    }
    /* Landing in the FIRST leaf of a two-level tree. */
    entry_directory(&entry, "/alpha/zzz", 0u);
    if (run_provision(&run, fx, "d2first", &entry, 1u, 0u)) {
        tr_volume_t vol;
        tr_record_t folder;

        expect_success(&run, "insert into the first leaf of a depth-2 tree");
        if (run.output && tr_open(run.output, run.output_size, &vol)) {
            tr_walk_t walk;

            tr_walk(&vol, &walk, NULL, NULL);
            CHECK(walk.shape_ok && walk.order_ok && walk.nodes == 2u,
                  "depth-2 chain: shape=%d order=%d nodes=%u", walk.shape_ok,
                  walk.order_ok, walk.nodes);
            CHECK(tr_find(&vol, FX_ALPHA, "zzz", &folder) && folder.type == 1u,
                  "/alpha/zzz is missing from the depth-2 tree");
            tr_close(&vol);
        }
        run_release(&run);
    }
    /*
     * Landing in the SECOND leaf, past its first key.  beta's own folder
     * record lives in leaf 1 while its new child's records go to leaf 2, so
     * this is the case where the parent update touches a node the insert
     * never does -- the only shape that can catch a writer which forgets to
     * mark the parent's node dirty.
     */
    entry_directory(&entry, "/beta/zzz", 0u);
    if (run_provision(&run, fx, "d2second", &entry, 1u, 0u)) {
        tr_volume_t vol;
        tr_record_t folder;

        expect_success(&run, "insert into the second leaf of a depth-2 tree");
        if (run.output && tr_open(run.output, run.output_size, &vol)) {
            CHECK(tr_find(&vol, FX_BETA, "zzz", &folder) && folder.type == 1u,
                  "/beta/zzz is missing from the depth-2 tree");
            CHECK(tr_find(&vol, FX_ROOT, "beta", &folder) &&
                  get_be32(folder.data + 4) == 2u,
                  "beta's valence is %u in the other leaf, expected 2",
                  folder.type == 1u ? get_be32(folder.data + 4) : 0u);
            CHECK(get_be32(folder.data + 84) == 1u,
                  "beta's folderCount is %u, expected 1",
                  get_be32(folder.data + 84));
            CHECK(get_be32(folder.data + 16) == ROOTFS_WORK_DEFAULT_MAC_TIME,
                  "beta's contentModDate was not written to its own leaf");
            tr_close(&vol);
        }
        run_release(&run);
    }
    /*
     * The leaf-head guard.  Reaching it needs an index key that undersells its
     * child's first key, because in a well-formed volume the catalog's
     * smallest key is the root folder's own (kHFSRootParentID = 1, volume
     * name) and every provisionable key has a parent CNID of at least 2.  The
     * refusal is therefore a guard against a tree that is already wrong, and
     * it must still fail closed rather than write a record the index cannot
     * find again.
     */
    entry_directory(&entry, "/alpha/zzz", 0u);
    if (run_provision(&run, bad, "leafhead", &entry, 1u, 0u)) {
        expect_refusal(&run, bad, ROOTFS_WORK_PROVISION_LEAF_HEAD,
                       "record that would become a leaf's first key");
        run_release(&run);
    }
    free(fx);
    free(bad);
}

static void test_invalid_requests_are_refused(void) {
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entry;
    rootfs_work_entry_t many[ROOTFS_WORK_MAX_ENTRIES + 1u];
    run_t run;
    static const char *bad_paths[] = {
        "relative/path", "", "/", "//double", "/trailing/", "/a//b",
        "/a/./b", "/a/../b", "/a/b\x01c", "/a/b:c", "/a/b\x80z"
    };
    size_t index;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    for (index = 0; index < sizeof(bad_paths) / sizeof(bad_paths[0]); index++) {
        entry_directory(&entry, bad_paths[index], 0u);
        if (run_provision(&run, fx, "badpath", &entry, 1u, 0u)) {
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_INVALID,
                           bad_paths[index][0] ? bad_paths[index] :
                               "(empty path)");
            run_release(&run);
        }
    }
    entry_directory(&entry, NULL, 0u);
    if (run_provision(&run, fx, "nullpath", &entry, 1u, 0u)) {
        expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_INVALID, "NULL path");
        run_release(&run);
    }
    /* A directory cannot carry content. */
    entry_directory(&entry, "/gamma", 0u);
    entry.content = (const uint8_t *)"x";
    entry.content_size = 1u;
    if (run_provision(&run, fx, "dircontent", &entry, 1u, 0u)) {
        expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_INVALID,
                       "directory with content");
        run_release(&run);
    }
    /* A file that promises bytes it does not have. */
    entry_file(&entry, "/gamma", NULL, 4u, 0u);
    if (run_provision(&run, fx, "nullcontent", &entry, 1u, 0u)) {
        expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_INVALID,
                       "content_size without a buffer");
        run_release(&run);
    }
    /* Mode bits outside the low twelve would collide with S_IFMT. */
    entry_directory(&entry, "/gamma", 0u);
    entry.permissions = 0100755u;
    if (run_provision(&run, fx, "badmode", &entry, 1u, 0u)) {
        expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_INVALID,
                       "permissions carrying format bits");
        run_release(&run);
    }
    /* entry_count with a NULL array. */
    {
        memset(&run, 0, sizeof(run));
        if (make_path(run.source, sizeof(run.source), "nullarr") &&
            make_path(run.destination, sizeof(run.destination), "nullarr") &&
            write_file(run.source, fx->image, FX_SIZE)) {
            run.options.fstab_line = ROOTFS_WORK_DEFAULT_FSTAB;
            run.options.entries = NULL;
            run.options.entry_count = 1u;
            run.status = rootfs_work_create(run.source, run.destination,
                                            &run.options, &run.result);
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_INVALID,
                           "entry_count with a NULL array");
            run_release(&run);
        }
    }
    /* Above the entry cap. */
    for (index = 0; index < ROOTFS_WORK_MAX_ENTRIES + 1u; index++)
        entry_directory(&many[index], "/gamma", 0u);
    if (run_provision(&run, fx, "toomany", many,
                      ROOTFS_WORK_MAX_ENTRIES + 1u, 0u)) {
        expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_LIMIT,
                       "more entries than the cap");
        run_release(&run);
    }
    /* A path with more components than the depth cap. */
    {
        char deep[4u * (ROOTFS_WORK_MAX_PATH_DEPTH + 2u)];
        size_t cursor = 0;

        for (index = 0; index < ROOTFS_WORK_MAX_PATH_DEPTH + 2u; index++) {
            deep[cursor++] = '/';
            deep[cursor++] = 'd';
        }
        deep[cursor] = '\0';
        entry_directory(&entry, deep, 0u);
        if (run_provision(&run, fx, "deeppath", &entry, 1u, 0u)) {
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_LIMIT,
                           "path deeper than the component cap");
            run_release(&run);
        }
    }
    free(fx);
}

/*
 * Determinism: identical inputs must produce identical bytes.  This is worth
 * asserting on its own -- the timestamps stamped on new records come from a
 * fixed default rather than from the clock, which is what lets a work image be
 * compared across runs at all.
 */
static void test_output_is_deterministic(void) {
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entries[2];
    run_t first;
    run_t second;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    entry_directory(&entries[0], "/gamma", 0u);
    entry_file(&entries[1], "/gamma/inner.txt", "same bytes", 10u, 0u);
    if (run_provision(&first, fx, "det1", entries, 2u, 0u) &&
        run_provision(&second, fx, "det2", entries, 2u, 0u)) {
        expect_success(&first, "determinism run 1");
        expect_success(&second, "determinism run 2");
        CHECK(first.output && second.output &&
              first.output_size == second.output_size &&
              memcmp(first.output, second.output, first.output_size) == 0,
              "two identical requests produced different images");
        run_release(&first);
        run_release(&second);
    }
    free(fx);
}

/*
 * The activation payload itself: the exact directory and plist the guest is
 * missing.  This does not need the real rootfs -- the point is that the
 * helper's own bytes round-trip through the writer and come back identical.
 */
static void test_activation_entries(void) {
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entries[2];
    rootfs_work_entry_t local[4];
    rootfs_work_entry_t deep[6];
    run_t run;
    size_t needed;
    size_t index;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    needed = rootfs_work_activation_entries(NULL, 0u);
    CHECK(needed == 2u, "activation needs %zu entries, expected 2", needed);
    memset(local, 0, sizeof(local));
    CHECK(rootfs_work_activation_entries(local, 1u) == 2u &&
          local[0].path == NULL,
          "a too-small buffer was written to anyway");
    CHECK(rootfs_work_activation_entries(entries, 2u) == 2u,
          "activation entries were not produced");
    CHECK(entries[0].kind == ROOTFS_WORK_ENTRY_DIRECTORY &&
          strcmp(entries[0].path,
                 "/private/var/root/Library/Lockdown") == 0,
          "activation entry 0 is %s",
          entries[0].path ? entries[0].path : "(null)");
    CHECK(entries[1].kind == ROOTFS_WORK_ENTRY_FILE &&
          strcmp(entries[1].path,
                 "/private/var/root/Library/Lockdown/data_ark.plist") == 0,
          "activation entry 1 is %s",
          entries[1].path ? entries[1].path : "(null)");
    CHECK(entries[1].content != NULL && entries[1].content_size > 200u &&
          entries[1].content_size < 400u,
          "data_ark.plist is %zu bytes", entries[1].content_size);
    CHECK(memchr(entries[1].content, 0, entries[1].content_size) == NULL,
          "data_ark.plist content carries an embedded NUL");
    CHECK(strstr((const char *)entries[1].content,
                 "<key>-ActivationState</key>") != NULL &&
          strstr((const char *)entries[1].content,
                 "<string>FactoryActivated</string>") != NULL &&
          strstr((const char *)entries[1].content,
                 "<key>-BrickState</key>") != NULL &&
          strstr((const char *)entries[1].content, "<false/>") != NULL,
          "data_ark.plist does not carry the keys lockdownd reads");

    /* Rebuild the real path inside the fixture, then apply the real pair. */
    memset(deep, 0, sizeof(deep));
    entry_directory(&deep[0], "/private", 0755u);
    entry_directory(&deep[1], "/private/var", 0755u);
    entry_directory(&deep[2], "/private/var/root", 0700u);
    entry_directory(&deep[3], "/private/var/root/Library", 0750u);
    deep[4] = entries[0];
    deep[5] = entries[1];
    if (run_provision(&run, fx, "activation", deep, 6u, 0u)) {
        tr_volume_t vol;
        tr_record_t record;
        uint32_t cnid = 0;

        expect_success(&run, "activation payload");
        if (run.output && tr_open(run.output, run.output_size, &vol)) {
            tr_walk_t walk;

            tr_walk(&vol, &walk, NULL, NULL);
            CHECK(walk.shape_ok && walk.order_ok,
                  "activation catalog: shape=%d order=%d", walk.shape_ok,
                  walk.order_ok);
            cnid = FX_ROOT;
            for (index = 0; index < 5u; index++) {
                static const char *names[] = {"private", "var", "root",
                                              "Library", "Lockdown"};
                CHECK(tr_find(&vol, cnid, names[index], &record) &&
                      record.type == 1u, "%s is missing", names[index]);
                cnid = get_be32(record.data + 8);
            }
            CHECK(tr_find(&vol, cnid, "data_ark.plist", &record) &&
                  record.type == 2u, "data_ark.plist is missing");
            if (record.type == 2u) {
                uint64_t logical = get_be64(record.data + 88);
                uint32_t start = get_be32(record.data + 104);

                CHECK(logical == entries[1].content_size,
                      "data_ark.plist is %llu bytes on disk, %zu were given",
                      (unsigned long long)logical, entries[1].content_size);
                CHECK((size_t)start * FX_BLOCK_SIZE + logical <=
                          run.output_size &&
                      memcmp(run.output + (size_t)start * FX_BLOCK_SIZE,
                             entries[1].content, (size_t)logical) == 0,
                      "the extracted plist bytes differ from the source");
                CHECK(get_be16(record.data + 42) == (0100000u | 0644u),
                      "data_ark.plist mode is 0%o",
                      get_be16(record.data + 42));
            }
            tr_close(&vol);
        }
        run_release(&run);
    }
    free(fx);
}

/*
 * The guest network payload is one identity expressed three ways: pppd reads
 * it from options, configd reads it from preferences.plist, and libc retains a
 * resolver-file fallback. A partial table recreates the exact false-positive
 * state where the UI says PPP/NAT because the link opened but applications
 * cannot discover a usable network.
 */
static void test_ppp_entries(void) {
    static const char expected_options[] =
        "defaultroute\n"
        "usepeerdns\n"
        "serviceid 53354C42-4F58-4050-9000-000000000001\n";
    static const char service_id[] =
        "53354C42-4F58-4050-9000-000000000001";
    rootfs_work_entry_t entries[4];
    rootfs_work_entry_t short_table[4];
    size_t needed = rootfs_work_ppp_entries(NULL, 0u);

    CHECK(needed == 4u, "PPP needs %zu entries, expected 4", needed);
    memset(short_table, 0, sizeof short_table);
    CHECK(rootfs_work_ppp_entries(short_table, 3u) == 4u &&
          short_table[0].path == NULL && short_table[1].path == NULL &&
          short_table[2].path == NULL && short_table[3].path == NULL,
          "a short PPP table was partially filled");
    memset(entries, 0, sizeof entries);
    CHECK(rootfs_work_ppp_entries(entries, 4u) == 4u,
          "PPP entries were not produced");

    static const size_t file_indices[] = {0u, 1u, 3u};
    for (size_t n = 0; n < sizeof file_indices / sizeof file_indices[0]; n++) {
        size_t i = file_indices[n];
        CHECK(entries[i].kind == ROOTFS_WORK_ENTRY_FILE,
              "PPP entry %zu is kind %d, not a file", i,
              (int)entries[i].kind);
        CHECK(entries[i].permissions == 0644u,
              "PPP entry %zu has mode 0%o", i,
              (unsigned)entries[i].permissions);
        CHECK(entries[i].content != NULL && entries[i].content_size != 0u,
              "PPP entry %zu has no body", i);
        CHECK(entries[i].content == NULL ||
              memchr(entries[i].content, 0, entries[i].content_size) == NULL,
              "PPP entry %zu carries an embedded NUL", i);
    }
    CHECK(entries[2].kind == ROOTFS_WORK_ENTRY_DIRECTORY &&
          entries[2].permissions == 0755u && entries[2].content == NULL &&
          entries[2].content_size == 0u,
          "SystemConfiguration directory entry is not an empty 0755 directory");

    CHECK(entries[0].path &&
          strcmp(entries[0].path, "/private/etc/ppp/options") == 0,
          "PPP options path is %s",
          entries[0].path ? entries[0].path : "(null)");
    CHECK(entries[0].content_size == sizeof expected_options - 1u &&
          memcmp(entries[0].content, expected_options,
                 sizeof expected_options - 1u) == 0,
          "PPP options do not carry the exact route, DNS and service ID");

    CHECK(entries[1].path &&
          strcmp(entries[1].path, "/private/var/run/resolv.conf") == 0,
          "resolver path is %s",
          entries[1].path ? entries[1].path : "(null)");
    CHECK(entries[1].content_size == strlen("nameserver 10.0.2.3\n") &&
          memcmp(entries[1].content, "nameserver 10.0.2.3\n",
                 strlen("nameserver 10.0.2.3\n")) == 0,
          "resolver fallback does not name 10.0.2.3");

    CHECK(entries[2].path &&
          strcmp(entries[2].path,
                 "/private/var/preferences/SystemConfiguration") == 0,
          "SystemConfiguration directory path is %s",
          entries[2].path ? entries[2].path : "(null)");
    CHECK(entries[3].path &&
          strcmp(entries[3].path,
                 "/private/var/preferences/SystemConfiguration/"
                 "preferences.plist") == 0,
          "SystemConfiguration path is %s",
          entries[3].path ? entries[3].path : "(null)");
    const char *plist = (const char *)entries[3].content;
    const char *id1 = plist ? strstr(plist, service_id) : NULL;
    const char *id2 = id1 ? strstr(id1 + 1, service_id) : NULL;
    const char *id3 = id2 ? strstr(id2 + 1, service_id) : NULL;
    CHECK(entries[3].content_size > 1000u &&
          entries[3].content_size < 4096u,
          "SystemConfiguration plist is %zu bytes",
          entries[3].content_size);
    CHECK(plist && strstr(plist, "<key>CurrentSet</key>") &&
          strstr(plist, "<key>NetworkServices</key>") &&
          strstr(plist, "<key>ServiceOrder</key>") &&
          strstr(plist, "<key>__LINK__</key>"),
          "SystemConfiguration plist lacks its set/service graph");
    CHECK(plist && strstr(plist, "<string>tty.debug</string>") &&
          strstr(plist, "<string>PPPSerial</string>") &&
          strstr(plist, "<string>10.0.2.3</string>"),
          "SystemConfiguration plist lacks the serial interface or resolver");
    CHECK(id1 && id2 && id3,
          "the stable PPP service ID is not shared by service, order and link");
    CHECK(plist && entries[3].content_size >= 9u &&
          memcmp(plist + entries[3].content_size - 9u, "</plist>\n", 9u) == 0,
          "SystemConfiguration payload is not a complete plist document");

    rootfs_work_entry_t combined[6];
    rootfs_work_entry_t combined_short[6];
    memset(combined_short, 0, sizeof combined_short);
    CHECK(rootfs_work_standard_entries(true, true, NULL, 0u) == 6u,
          "the activation+PPP plan is not six entries");
    CHECK(rootfs_work_standard_entries(true, true, combined_short, 5u) == 6u &&
          combined_short[0].path == NULL && combined_short[5].path == NULL,
          "a short activation+PPP plan was partially filled");
    memset(combined, 0, sizeof combined);
    CHECK(rootfs_work_standard_entries(true, true, combined, 6u) == 6u &&
          combined[0].path &&
          strcmp(combined[0].path,
                 "/private/var/root/Library/Lockdown") == 0 &&
          combined[2].path &&
          strcmp(combined[2].path, "/private/etc/ppp/options") == 0 &&
          combined[4].path &&
          strcmp(combined[4].path,
                 "/private/var/preferences/SystemConfiguration") == 0 &&
          combined[5].path &&
          strcmp(combined[5].path,
                 "/private/var/preferences/SystemConfiguration/"
                 "preferences.plist") == 0,
          "the shared activation+PPP plan is missing or misordered");
    CHECK(rootfs_work_standard_entries(false, true, combined, 6u) == 4u &&
          combined[0].path &&
          strcmp(combined[0].path, "/private/etc/ppp/options") == 0,
          "the PPP-only standard plan is wrong");
    CHECK(rootfs_work_standard_entries(true, false, combined, 6u) == 2u &&
          combined[1].path &&
          strcmp(combined[1].path,
                 "/private/var/root/Library/Lockdown/data_ark.plist") == 0,
          "the activation-only standard plan is wrong");

    /* Reproduce the stock shape that the physical run exposed: preferences
     * exists, SystemConfiguration does not. The helper must create that
     * parent before inserting preferences.plist, or the whole transaction
     * fails with provision-parent-missing. */
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    if (!fx) {
        CHECK(0, "PPP fixture allocation failed");
        return;
    }
    rootfs_work_entry_t image_plan[10];
    memset(image_plan, 0, sizeof image_plan);
    entry_directory(&image_plan[0], "/private", 0755u);
    entry_directory(&image_plan[1], "/private/etc", 0755u);
    entry_directory(&image_plan[2], "/private/etc/ppp", 0755u);
    entry_directory(&image_plan[3], "/private/var", 0755u);
    entry_directory(&image_plan[4], "/private/var/run", 0755u);
    entry_directory(&image_plan[5], "/private/var/preferences", 0755u);
    memcpy(&image_plan[6], entries, sizeof entries);

    run_t run;
    if (run_provision(&run, fx, "ppp-layout", image_plan, 10u, 0u)) {
        tr_volume_t vol;
        tr_record_t record;
        expect_success(&run, "PPP stock-layout payload");
        if (run.output && tr_open(run.output, run.output_size, &vol)) {
            uint32_t cnid = FX_ROOT;
            static const char *parents[] = {
                "private", "var", "preferences", "SystemConfiguration"
            };
            for (size_t i = 0; i < sizeof parents / sizeof parents[0]; i++) {
                CHECK(tr_find(&vol, cnid, parents[i], &record) &&
                      record.type == 1u, "%s is missing", parents[i]);
                cnid = get_be32(record.data + 8);
            }
            CHECK(tr_find(&vol, cnid, "preferences.plist", &record) &&
                  record.type == 2u,
                  "SystemConfiguration/preferences.plist is missing");
            tr_close(&vol);
        }
        run_release(&run);
    }
    free(fx);
}

/* ------------------------------- B-tree splits -------------------------- */

/*
 * The level-2 index chain, walked from the ROOT's first child rather than from
 * anything the writer remembers.  Returns the node count.
 */
static uint32_t tr_index_chain(const tr_volume_t *vol, uint32_t *out,
                               size_t capacity, int *links_ok) {
    uint32_t kids[FX_MAX_RECORDS];
    uint16_t count;

    *links_ok = 0;
    if (vol->tree_depth < 3u)
        return 0u;
    count = tr_children(vol, vol->root_node, (uint8_t)vol->tree_depth, kids,
                        sizeof(kids) / sizeof(kids[0]));
    if (count == 0u)
        return 0u;
    return tr_level_chain(vol, kids[0], out, capacity, links_ok);
}

/*
 * THE ONE SPLIT, checked pointer by pointer.
 *
 * A provisioned directory costs two records with two different fates: the
 * folder record's key is (parentCNID, name), which lands among the fixture's
 * existing keys, while the thread record's key is (nextCatalogID, ""), which
 * sorts after every key in the tree.  So the thread record -- and only it --
 * is the rightmost append this split exists for.  Sizing the last leaf to 28
 * free bytes against the 30 that record needs is what provokes it; the control
 * fixture below differs only in that number.
 *
 * Every line the split's own comment claims is asserted here from the
 * PUBLISHED image, through the chain walker rather than the writer's descent:
 *
 *   old leaf   keeps every record it had, and its fLink now names the new node
 *   new node   bLink = old leaf, fLink = 0, exactly ONE record
 *   parent     one more child, appended last, pointing at the new node
 *   header     lastLeafNode = new node, freeNodes -= 1, its map bit set
 *   untouched  firstLeafNode, rootNode, treeDepth, totalNodes
 */
static void test_rightmost_leaf_split(void) {
    fixture_t *fits = fx_create_deep(40u, 400u, 0u, 170u, 3u);
    fixture_t *fx = fx_create_deep(28u, 400u, 0u, 170u, 3u);
    rootfs_work_entry_t entry;
    rootfs_work_entry_t pair[2];
    run_t run;

    if (!fits || !fx) {
        CHECK(0, "deep fixture allocation failed");
        free(fits);
        free(fx);
        return;
    }
    /*
     * The two fixtures must differ in exactly the way the test claims, or the
     * split below is a statement about luck.  A /gamma thread record is 28
     * bytes and its offset slot 2, so 30 free bytes fit and 28 do not.
     */
    CHECK(fits->leaf_free == 40u && fx->leaf_free == 28u,
          "split fixtures left %u and %u free leaf bytes, not 40 and 28",
          fits->leaf_free, fx->leaf_free);
    CHECK(fx->index_free >= 14u,
          "the leaf-split fixture's parent index node has %u free bytes, so "
          "the leaf split would drag an index split with it", fx->index_free);
    CHECK(fx->tree_depth == 3u && fx->index_nodes == 1u,
          "the leaf-split fixture is depth %u with %u index nodes",
          fx->tree_depth, fx->index_nodes);

    /* Control: the same request, against a last leaf with room. */
    entry_directory(&entry, "/gamma", 0u);
    if (run_provision(&run, fits, "nosplit", &entry, 1u, 0u)) {
        expect_success(&run, "append that fits the last leaf");
        CHECK(run.result.provision_leaf_splits == 0u &&
              run.result.provision_index_splits == 0u,
              "an append that fitted still reported %u leaf and %u index "
              "splits", run.result.provision_leaf_splits,
              run.result.provision_index_splits);
        if (run.output) {
            tr_volume_t vol;

            if (tr_open(run.output, run.output_size, &vol)) {
                CHECK(vol.last_leaf == fits->last_leaf &&
                      vol.free_nodes == fits->catalog_nodes -
                                        fits->used_nodes,
                      "an append that fitted moved lastLeafNode to %u or "
                      "consumed a node (freeNodes %u)", vol.last_leaf,
                      vol.free_nodes);
                tr_close(&vol);
            }
        }
        run_release(&run);
    }

    /* The split itself. */
    if (run_provision(&run, fx, "leafsplit", &entry, 1u, 0u)) {
        expect_success(&run, "rightmost append into a full last leaf");
        CHECK(run.result.provision_leaf_splits == 1u &&
              run.result.provision_index_splits == 0u,
              "split report: %u leaf splits, %u index splits",
              run.result.provision_leaf_splits,
              run.result.provision_index_splits);
        if (run.output) {
            tr_volume_t vol;

            if (tr_open(run.output, run.output_size, &vol)) {
                uint32_t chain[FX_MAX_RECORDS];
                uint32_t kids[FX_MAX_RECORDS];
                uint32_t level2[FX_MAX_RECORDS];
                int links_ok = 0;
                int index_links_ok = 0;
                uint32_t nodes;
                uint32_t index_nodes;
                uint32_t fresh = vol.last_leaf;
                tr_walk_t walk;
                tr_record_t folder;
                tr_record_t thread;

                /* --- what must NOT have moved --- */
                CHECK(vol.tree_depth == fx->tree_depth,
                      "treeDepth became %u, was %u", vol.tree_depth,
                      fx->tree_depth);
                CHECK(vol.root_node == fx->root_node,
                      "rootNode became %u, was %u", vol.root_node,
                      fx->root_node);
                CHECK(vol.first_leaf == fx->first_leaf,
                      "firstLeafNode became %u, was %u", vol.first_leaf,
                      fx->first_leaf);
                CHECK(vol.total_nodes == fx->catalog_nodes,
                      "totalNodes became %u, was %u", vol.total_nodes,
                      fx->catalog_nodes);

                /* --- the new node --- */
                CHECK(fresh != fx->last_leaf,
                      "lastLeafNode is still %u after a split", fresh);
                CHECK(vol.free_nodes ==
                          fx->catalog_nodes - fx->used_nodes - 1u,
                      "freeNodes is %u, expected %u after claiming one node",
                      vol.free_nodes,
                      fx->catalog_nodes - fx->used_nodes - 1u);
                CHECK(tr_node_used(&vol, fresh) == 1,
                      "the node map still calls the new leaf %u free", fresh);
                CHECK(tr_records(&vol, fresh) == 1u,
                      "the new leaf holds %u records, expected exactly the one "
                      "being inserted", tr_records(&vol, fresh));
                CHECK(tr_records(&vol, fx->last_leaf) ==
                          (uint16_t)fx->last_leaf_records,
                      "the old last leaf holds %u records, it had %u -- the "
                      "split moved existing records",
                      tr_records(&vol, fx->last_leaf), fx->last_leaf_records);

                /* --- the leaf chain, in both directions --- */
                CHECK(tr_flink(&vol, fx->last_leaf) == fresh,
                      "the old last leaf's fLink is %u, not the new node %u",
                      tr_flink(&vol, fx->last_leaf), fresh);
                CHECK(tr_blink(&vol, fresh) == fx->last_leaf,
                      "the new leaf's bLink is %u, not the old last leaf %u",
                      tr_blink(&vol, fresh), fx->last_leaf);
                CHECK(tr_flink(&vol, fresh) == 0u,
                      "the new leaf's fLink is %u, so it is not the chain end",
                      tr_flink(&vol, fresh));
                nodes = tr_level_chain(&vol, vol.first_leaf, chain,
                                       sizeof(chain) / sizeof(chain[0]),
                                       &links_ok);
                CHECK(links_ok && nodes == fx->leaf_nodes + 1u,
                      "the leaf chain is %u nodes with links_ok=%d, expected "
                      "%u", nodes, links_ok, fx->leaf_nodes + 1u);
                CHECK(nodes != 0u && chain[nodes - 1u] == fresh &&
                      fresh == vol.last_leaf,
                      "the chain ends at %u but the header says lastLeafNode "
                      "is %u", nodes ? chain[nodes - 1u] : 0u, vol.last_leaf);

                /* --- the level above --- */
                index_nodes = tr_index_chain(&vol, level2,
                                             sizeof(level2) /
                                                 sizeof(level2[0]),
                                             &index_links_ok);
                CHECK(index_links_ok && index_nodes == fx->index_nodes,
                      "the index level is %u nodes with links_ok=%d, expected "
                      "%u -- no index split was due", index_nodes,
                      index_links_ok, fx->index_nodes);
                if (index_nodes != 0u) {
                    uint16_t children = tr_children(&vol,
                                                    level2[index_nodes - 1u],
                                                    2u, kids,
                                                    sizeof(kids) /
                                                        sizeof(kids[0]));
                    CHECK(children == (uint16_t)fx->leaf_nodes + 1u,
                          "the parent index node names %u children, expected "
                          "%u", children, fx->leaf_nodes + 1u);
                    CHECK(children != 0u && kids[children - 1u] == fresh,
                          "the parent's LAST child is %u, not the new leaf %u",
                          children ? kids[children - 1u] : 0u, fresh);
                    CHECK(children >= 2u &&
                          kids[children - 2u] == fx->last_leaf,
                          "the parent's second-to-last child is %u, not the "
                          "old last leaf %u",
                          children >= 2u ? kids[children - 2u] : 0u,
                          fx->last_leaf);
                }
                CHECK(tr_children(&vol, vol.root_node, 3u, kids,
                                  sizeof(kids) / sizeof(kids[0])) ==
                          (uint16_t)fx->root_children,
                      "the root gained or lost a child across a leaf-only "
                      "split");

                /* --- and the records are all still findable, in order --- */
                tr_walk(&vol, &walk, NULL, NULL);
                CHECK(walk.shape_ok && walk.order_ok &&
                      walk.records == vol.leaf_records &&
                      walk.records == (uint32_t)fx->count + 2u,
                      "split catalog: shape=%d order=%d records=%u header=%u "
                      "expected=%u", walk.shape_ok, walk.order_ok,
                      walk.records, vol.leaf_records, (uint32_t)fx->count + 2u);
                CHECK(tr_find(&vol, FX_ROOT, "gamma", &folder) &&
                      folder.type == 1u, "/gamma is missing after the split");
                CHECK(folder.type == 1u &&
                      tr_find(&vol, get_be32(folder.data + 8), NULL, &thread) &&
                      thread.type == 3u,
                      "/gamma's thread record is missing after the split");
                tr_close(&vol);
            }
        }
        run_release(&run);
    }

    /*
     * A batch, which is the shape the payload actually arrives in.  /gamma's
     * thread splits the leaf; /gamma/inner's folder record and thread then sort
     * after it and must go into the NEW node.  If the writer did not move its
     * idea of the last leaf, the second append would try to split the old one
     * again -- so ONE split for three appended records is the assertion.
     */
    entry_directory(&pair[0], "/gamma", 0u);
    entry_directory(&pair[1], "/gamma/inner", 0u);
    if (run_provision(&run, fx, "batch", pair, 2u, 0u)) {
        expect_success(&run, "a two-entry ascending batch across a split");
        CHECK(run.result.provision_leaf_splits == 1u &&
              run.result.provision_index_splits == 0u,
              "the batch reported %u leaf and %u index splits, expected 1 and "
              "0", run.result.provision_leaf_splits,
              run.result.provision_index_splits);
        if (run.output) {
            tr_volume_t vol;

            if (tr_open(run.output, run.output_size, &vol)) {
                tr_walk_t walk;
                tr_record_t inner;

                tr_walk(&vol, &walk, NULL, NULL);
                CHECK(walk.shape_ok && walk.order_ok &&
                      walk.records == (uint32_t)fx->count + 4u,
                      "batch catalog: shape=%d order=%d records=%u expected %u",
                      walk.shape_ok, walk.order_ok, walk.records,
                      (uint32_t)fx->count + 4u);
                CHECK(tr_records(&vol, vol.last_leaf) == 3u,
                      "the new leaf holds %u records, expected the 3 that sort "
                      "after the split point", tr_records(&vol, vol.last_leaf));
                CHECK(tr_records(&vol, fx->last_leaf) ==
                          (uint16_t)fx->last_leaf_records,
                      "the batch moved records out of the old last leaf");
                CHECK(tr_find(&vol, FX_ROOT, "gamma", &inner) &&
                      inner.type == 1u &&
                      tr_find(&vol, get_be32(inner.data + 8), "inner", &inner),
                      "/gamma/inner is missing after the batch");
                tr_close(&vol);
            }
        }
        run_release(&run);
    }
    free(fits);
    free(fx);
}

/*
 * The same split, one level up.  Here the parent index node is 10 bytes short
 * of the 14 a new child pointer costs, so chaining the new leaf on forces the
 * index level to be extended too -- the recursion in catalog_level_extend().
 * It terminates at the root, which has room, so treeDepth never changes.
 */
static void test_rightmost_index_split(void) {
    fixture_t *fx = fx_create_deep(28u, 0u, 0u, 150u, 3u);
    rootfs_work_entry_t entry;
    run_t run;

    if (!fx) {
        CHECK(0, "index-split fixture allocation failed");
        return;
    }
    CHECK(fx->leaf_free == 28u && fx->index_free < 14u,
          "index-split fixture left %u free leaf bytes and %u free index "
          "bytes; the leaf needs 30 and the index record costs 14",
          fx->leaf_free, fx->index_free);
    CHECK(fx->tree_depth == 3u && fx->index_nodes == 1u,
          "index-split fixture is depth %u with %u index nodes",
          fx->tree_depth, fx->index_nodes);
    entry_directory(&entry, "/gamma", 0u);
    if (!run_provision(&run, fx, "idxsplit", &entry, 1u, 0u)) {
        CHECK(0, "index-split fixture setup failed");
        free(fx);
        return;
    }
    expect_success(&run, "a rightmost append that splits two levels");
    CHECK(run.result.provision_leaf_splits == 1u &&
          run.result.provision_index_splits == 1u,
          "index-split report: %u leaf splits, %u index splits",
          run.result.provision_leaf_splits, run.result.provision_index_splits);
    if (run.output) {
        tr_volume_t vol;

        if (tr_open(run.output, run.output_size, &vol)) {
            uint32_t chain[FX_MAX_RECORDS];
            uint32_t level2[FX_MAX_RECORDS];
            uint32_t kids[FX_MAX_RECORDS];
            int links_ok = 0;
            int index_links_ok = 0;
            uint32_t nodes;
            uint32_t index_nodes;
            uint32_t fresh = vol.last_leaf;
            uint16_t root_children;
            tr_walk_t walk;
            tr_record_t folder;

            CHECK(vol.tree_depth == 3u && vol.root_node == fx->root_node &&
                  vol.first_leaf == fx->first_leaf &&
                  vol.total_nodes == fx->catalog_nodes,
                  "the index split changed depth=%u root=%u first=%u nodes=%u",
                  vol.tree_depth, vol.root_node, vol.first_leaf,
                  vol.total_nodes);
            /* TWO nodes were claimed this time, one per level. */
            CHECK(vol.free_nodes == fx->catalog_nodes - fx->used_nodes - 2u,
                  "freeNodes is %u, expected %u after claiming a leaf and an "
                  "index node", vol.free_nodes,
                  fx->catalog_nodes - fx->used_nodes - 2u);
            CHECK(tr_node_used(&vol, fresh) == 1,
                  "the node map still calls the new leaf %u free", fresh);
            CHECK(tr_flink(&vol, fx->last_leaf) == fresh &&
                  tr_blink(&vol, fresh) == fx->last_leaf &&
                  tr_flink(&vol, fresh) == 0u,
                  "leaf chain after the index split: old.fLink=%u new.bLink=%u "
                  "new.fLink=%u", tr_flink(&vol, fx->last_leaf),
                  tr_blink(&vol, fresh), tr_flink(&vol, fresh));
            nodes = tr_level_chain(&vol, vol.first_leaf, chain,
                                   sizeof(chain) / sizeof(chain[0]),
                                   &links_ok);
            CHECK(links_ok && nodes == fx->leaf_nodes + 1u &&
                  chain[nodes - 1u] == fresh,
                  "leaf chain is %u nodes (links_ok=%d) ending at %u, expected "
                  "%u ending at %u", nodes, links_ok,
                  nodes ? chain[nodes - 1u] : 0u, fx->leaf_nodes + 1u, fresh);

            /* The index level grew by exactly one node, chained on the right. */
            index_nodes = tr_index_chain(&vol, level2,
                                         sizeof(level2) / sizeof(level2[0]),
                                         &index_links_ok);
            CHECK(index_links_ok && index_nodes == fx->index_nodes + 1u,
                  "the index level is %u nodes with links_ok=%d, expected %u",
                  index_nodes, index_links_ok, fx->index_nodes + 1u);
            if (index_nodes >= 2u) {
                uint32_t grown = level2[index_nodes - 1u];
                uint32_t previous = level2[index_nodes - 2u];
                uint16_t children;

                CHECK(tr_node_used(&vol, grown) == 1,
                      "the node map still calls the new index node %u free",
                      grown);
                CHECK(tr_flink(&vol, previous) == grown &&
                      tr_blink(&vol, grown) == previous &&
                      tr_flink(&vol, grown) == 0u,
                      "index chain: old.fLink=%u new.bLink=%u new.fLink=%u",
                      tr_flink(&vol, previous), tr_blink(&vol, grown),
                      tr_flink(&vol, grown));
                children = tr_children(&vol, grown, 2u, kids,
                                       sizeof(kids) / sizeof(kids[0]));
                CHECK(children == 1u && kids[0] == fresh,
                      "the new index node names %u children, first %u; "
                      "expected exactly the new leaf %u", children,
                      children ? kids[0] : 0u, fresh);
                children = tr_children(&vol, previous, 2u, kids,
                                       sizeof(kids) / sizeof(kids[0]));
                CHECK(children == (uint16_t)fx->leaf_nodes &&
                      kids[children - 1u] == fx->last_leaf,
                      "the old index node kept %u children ending at %u, "
                      "expected %u ending at %u", children,
                      children ? kids[children - 1u] : 0u, fx->leaf_nodes,
                      fx->last_leaf);
                /* The root learned about the new index node, and only that. */
                root_children = tr_children(&vol, vol.root_node, 3u, kids,
                                            sizeof(kids) / sizeof(kids[0]));
                CHECK(root_children == (uint16_t)fx->root_children + 1u,
                      "the root names %u children, expected %u",
                      root_children, fx->root_children + 1u);
                CHECK(root_children != 0u &&
                      kids[root_children - 1u] == grown,
                      "the root's last child is %u, not the new index node %u",
                      root_children ? kids[root_children - 1u] : 0u, grown);
            }
            tr_walk(&vol, &walk, NULL, NULL);
            CHECK(walk.shape_ok && walk.order_ok &&
                  walk.records == vol.leaf_records &&
                  walk.records == (uint32_t)fx->count + 2u,
                  "index-split catalog: shape=%d order=%d records=%u header=%u",
                  walk.shape_ok, walk.order_ok, walk.records,
                  vol.leaf_records);
            CHECK(tr_find(&vol, FX_ROOT, "gamma", &folder) &&
                  folder.type == 1u,
                  "/gamma is missing after the two-level split");
            tr_close(&vol);
        }
    }
    run_release(&run);
    free(fx);
}

/*
 * Where the split stops, by name.  Both of these are cases a half-implemented
 * writer would take a run at; a refused work image is byte-identical to the
 * one the provisioner was handed, which expect_refusal() re-reads and checks.
 */
static void test_split_boundaries_are_refused(void) {
    fixture_t *root_full = fx_create_deep(28u, 0u, 0u, 150u, 2u);
    fixture_t *no_nodes = fx_create_deep(28u, 400u, 0u, 170u, 3u);
    rootfs_work_entry_t entry;
    run_t run;

    entry_directory(&entry, "/gamma", 0u);
    /*
     * A depth-2 tree whose ROOT is the index node, with no room in it.  Growing
     * a new root and incrementing treeDepth is the one rightmost-append shape
     * that is not implemented, and it must say so rather than attempt it.
     */
    if (root_full) {
        CHECK(root_full->tree_depth == 2u && root_full->leaf_free == 28u &&
              root_full->index_free < 14u,
              "root-full fixture is depth %u with %u free leaf and %u free "
              "root bytes", root_full->tree_depth, root_full->leaf_free,
              root_full->index_free);
        if (run_provision(&run, root_full, "rootfull", &entry, 1u, 0u)) {
            expect_refusal(&run, root_full,
                           ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED,
                           "a split that would have to grow a new root");
            run_release(&run);
        }
    } else {
        CHECK(0, "root-full fixture allocation failed");
    }
    /*
     * A structurally fine tree with no spare node.  The split is the right
     * answer and there is nowhere to put it, which is a different refusal from
     * "the shape is unsupported" and gets a different name.
     */
    if (no_nodes) {
        fx_exhaust_node_map(no_nodes);
        if (run_provision(&run, no_nodes, "btreefull", &entry, 1u, 0u)) {
            expect_refusal(&run, no_nodes, ROOTFS_WORK_PROVISION_BTREE_FULL,
                           "a split with no free B-tree node to take");
            run_release(&run);
        }
    } else {
        CHECK(0, "exhausted-map fixture allocation failed");
    }
    /*
     * A full leaf that is NOT the chain's end.  This fixture's first leaf ends
     * at (16,""), so a key of (16,"bbbbbbbbbbbb") sorts after everything in it
     * and before (16,"dup") in the next leaf: position == count in a leaf that
     * is not the last one, which is HALF of what the rightmost append needs.
     *
     * This used to be the PROVISION_NODE_FULL refusal.  It is now served by the
     * general split, which chains a node in and redistributes -- so the
     * assertions below are the ones the refusal never got to make: the record
     * is there, the chain grew by exactly one node in exactly the right place,
     * both link directions agree, and the LAST leaf is untouched, which is what
     * still separates this case from the rightmost append.
     */
    {
        fixture_t *interior = fx_create_deep(400u, 400u, 0u, 100u, 3u);

        if (interior) {
            uint16_t room = fx_node_free(interior, interior->first_leaf);

            CHECK(interior->first_leaf != interior->last_leaf &&
                  interior->leaf_free == 400u,
                  "interior fixture: first leaf %u, last leaf %u with %u free "
                  "bytes", interior->first_leaf, interior->last_leaf,
                  interior->leaf_free);
            /*
             * The name lengths straddle that leaf's free space: a 12-unit name
             * makes the folder record 120 bytes and 122 with its slot, which
             * does not fit; a 1-unit name makes it 98 and 100, which does.
             */
            CHECK(room >= 100u && room < 122u,
                  "the interior leaf has %u free bytes; the test needs it "
                  "between 100 and 121", room);
            entry_directory(&entry, "/alpha/bbbbbbbbbbbb", 0u);
            if (run_provision(&run, interior, "interior", &entry, 1u, 0u)) {
                expect_success(&run, "a full leaf that is not the chain's end");
                CHECK(run.result.provision_leaf_splits == 1u &&
                      run.result.provision_index_splits == 0u,
                      "interior split report: %u leaf, %u index splits",
                      run.result.provision_leaf_splits,
                      run.result.provision_index_splits);
                if (run.output) {
                    tr_volume_t vol;

                    if (tr_open(run.output, run.output_size, &vol)) {
                        uint32_t chain[FX_MAX_RECORDS];
                        int links_ok = 0;
                        uint32_t nodes = tr_level_chain(&vol, vol.first_leaf,
                                                        chain,
                                                        sizeof(chain) /
                                                        sizeof(chain[0]),
                                                        &links_ok);
                        tr_walk_t walk;
                        tr_record_t folder;

                        tr_walk(&vol, &walk, NULL, NULL);
                        CHECK(walk.shape_ok && walk.order_ok,
                              "interior split: shape=%d order=%d",
                              walk.shape_ok, walk.order_ok);
                        CHECK(nodes == interior->leaf_nodes + 1u && links_ok,
                              "interior split: %u leaves (was %u), links_ok=%d",
                              nodes, interior->leaf_nodes, links_ok);
                        /* The new node is chained immediately after the leaf
                         * that split, which is the first leaf here. */
                        CHECK(nodes >= 2u && chain[0] == interior->first_leaf &&
                              tr_blink(&vol, chain[1]) == chain[0],
                              "interior split: chain starts %u then %u",
                              nodes >= 1u ? chain[0] : 0u,
                              nodes >= 2u ? chain[1] : 0u);
                        /* The rightmost append's own bookkeeping stayed put. */
                        CHECK(vol.last_leaf == interior->last_leaf,
                              "interior split moved lastLeafNode to %u, was %u",
                              vol.last_leaf, interior->last_leaf);
                        CHECK(vol.tree_depth == interior->tree_depth &&
                              vol.root_node == interior->root_node &&
                              vol.first_leaf == interior->first_leaf,
                              "interior split moved depth/root/firstLeaf to "
                              "%u/%u/%u", vol.tree_depth, vol.root_node,
                              vol.first_leaf);
                        CHECK(tr_find(&vol, FX_ALPHA, "bbbbbbbbbbbb",
                                      &folder) && folder.type == 1u,
                              "/alpha/bbbbbbbbbbbb is missing after the "
                              "interior split");
                        tr_close(&vol);
                    }
                }
                run_release(&run);
            }
            /*
             * PROVISION_NODE_FULL now means what its name says and nothing
             * else: the node is full and NO split of its records plus the new
             * one leaves two halves that both fit.
             *
             * Reaching it is arithmetic.  A record can only end up alone in one
             * half if it goes at a node's very start or its very end, so an
             * INTERIOR record that cannot share a 512-byte node with even the
             * smallest of its neighbours has no split point at all.  This
             * leaf's smallest record is a 28-byte thread; a 112-unit file name
             * makes the record 480 bytes, which still fits an empty node
             * (14 + 480 + 4 = 498) but leaves 526 bytes of demand against 512
             * once that thread comes with it.  The key (2, "bz...") sorts after
             * (2,"beta") and before (16,""), so it lands inside the leaf rather
             * than at either end.
             */
            {
                static const char body[] = "x";
                char name[128];
                rootfs_work_entry_t wide;

                name[0] = '/';
                memset(name + 1, 'z', 112u);
                name[1] = 'b';
                name[113] = '\0';
                CHECK(strlen(name) == 113u,
                      "the node-full probe name is %u bytes, not 113",
                      (unsigned)strlen(name));
                entry_file(&wide, name, body, 1u, 0u);
                if (run_provision(&run, interior, "nosplitfit", &wide, 1u,
                                  0u)) {
                    expect_refusal(&run, interior,
                                   ROOTFS_WORK_PROVISION_NODE_FULL,
                                   "an interior record with no split point");
                    run_release(&run);
                }
                /* Two units shorter is 476 bytes, and 14 + 476 + 28 + 6 = 524,
                 * still over.  Halving the name gives it a split point and it
                 * succeeds, so the refusal above is about the arithmetic and
                 * not about the shape of the request. */
                name[57] = '\0';
                entry_file(&wide, name, body, 1u, 0u);
                if (run_provision(&run, interior, "splitfits", &wide, 1u, 0u)) {
                    expect_success(&run, "an interior record that can split");
                    CHECK(run.result.provision_leaf_splits == 1u,
                          "the shorter interior record reported %u leaf splits",
                          run.result.provision_leaf_splits);
                    run_release(&run);
                }
            }
            /* The same insert point, two bytes inside the leaf's means: it
             * fits, so nothing splits at all. */
            entry_directory(&entry, "/alpha/b", 0u);
            if (run_provision(&run, interior, "interiorfits", &entry, 1u, 0u)) {
                expect_success(&run, "an interior append that fits");
                CHECK(run.result.provision_leaf_splits == 0u,
                      "an interior append that fitted reported %u leaf splits",
                      run.result.provision_leaf_splits);
                run_release(&run);
            }
        } else {
            CHECK(0, "interior-leaf fixture allocation failed");
        }
        free(interior);
    }
    free(root_full);
    free(no_nodes);
}

/* ------------------------------- symlinks ------------------------------- */

/*
 * PROVENANCE.  Nothing asserted below was recalled: every field was censused
 * out of firmware/rootfs.img itself, by walking its leaf chain and decoding
 * HFSPlusCatalogFile at absolute offsets.  All 409 symlinks on the stock 7E18
 * volume agree, with no exceptions, on
 *
 *   recordType 2, flags 0x0002, reserved1 0
 *   fileMode 0120755, adminFlags 0, ownerFlags 0, linkCount 1
 *   fdType 'slnk', fdCreator 'rhap', the rest of FndrFileInfo zero
 *   FndrOpaqueInfo 16 zero bytes, textEncoding 0, reserved2 0
 *   dataFork logicalSize == strlen(target) with NO NUL inside it,
 *            clumpSize 0, totalBlocks 1, one extent, extents[1..7] zero
 *   resourceFork 80 zero bytes
 *
 * and /etc (CNID 14880, target "private/etc") is the worked example the
 * constants were taken from.  The only field that varies is groupID -- 0 on
 * 384 of them, 80 on the 25 under / and /Library -- which is why it stays the
 * caller's to choose.  A provisioned symlink that fails any line here is
 * unlike anything the stock volume contains.
 *
 * The targets are real ones from the acquired payload, so the shapes exercised
 * are the shapes that have to be written: a trailing-slash target, a bare
 * sibling name, and a relative one with '..'.
 */
static void test_symlink_matches_the_stock_format(void) {
    static const char ETC_TARGET[] = "private/etc/";
    static const char SH_TARGET[] = "bash";
    static const char PREFS_TARGET[] = "../private/var/preferences/";
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entries[3];
    run_t run;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    entry_symlink(&entries[0], "/etc", ETC_TARGET, 0u);
    entry_symlink(&entries[1], "/sh", SH_TARGET, 0u);
    entry_symlink(&entries[2], "/alpha/Preferences", PREFS_TARGET, 0u);
    if (!run_provision(&run, fx, "symlink", entries, 3u, 0u)) {
        CHECK(0, "symlink fixture setup failed");
        free(fx);
        return;
    }
    expect_success(&run, "three symlinks");
    CHECK(run.result.provision_entries == 3u && run.result.provision_blocks == 3u,
          "symlink report: entries=%u blocks=%u", run.result.provision_entries,
          run.result.provision_blocks);
    if (run.output) {
        tr_volume_t vol;

        if (tr_open(run.output, run.output_size, &vol)) {
            static const struct {
                uint32_t parent;
                const char *name;
                const char *target;
            } CASES[] = {
                {FX_ROOT, "etc", ETC_TARGET},
                {FX_ROOT, "sh", SH_TARGET},
                {FX_ALPHA, "Preferences", PREFS_TARGET}
            };
            tr_walk_t walk;
            size_t index;

            tr_walk(&vol, &walk, NULL, NULL);
            CHECK(walk.shape_ok && walk.order_ok &&
                  walk.records == vol.leaf_records &&
                  walk.records == (uint32_t)fx->count + 6u,
                  "symlink catalog: shape=%d order=%d records=%u header=%u",
                  walk.shape_ok, walk.order_ok, walk.records, vol.leaf_records);
            for (index = 0; index < sizeof(CASES) / sizeof(CASES[0]); index++) {
                tr_record_t link;
                tr_record_t thread;
                const uint8_t *data;
                size_t length = strlen(CASES[index].target);
                uint32_t start;
                uint32_t cnid;

                if (!tr_find(&vol, CASES[index].parent, CASES[index].name,
                             &link)) {
                    CHECK(0, "symlink %s is missing", CASES[index].name);
                    continue;
                }
                data = link.data;
                cnid = get_be32(data + 8);
                CHECK(link.type == 2u,
                      "%s is record type %u, not a catalog FILE record",
                      CASES[index].name, link.type);
                CHECK(link.data_length == 248u,
                      "%s data is %u bytes, the stock volume's are 248",
                      CASES[index].name, link.data_length);
                CHECK(get_be16(data + 2) == 0x0002u,
                      "%s flags are 0x%04x, the stock volume's are 0x0002",
                      CASES[index].name, get_be16(data + 2));
                CHECK(get_be32(data + 4) == 0u, "%s reserved1 is not zero",
                      CASES[index].name);
                CHECK(get_be16(data + 42) == 0120755u,
                      "%s fileMode is 0%o, every stock symlink is 0120755",
                      CASES[index].name, get_be16(data + 42));
                CHECK(data[40] == 0u && data[41] == 0u,
                      "%s adminFlags/ownerFlags are 0x%02x/0x%02x, not 0/0",
                      CASES[index].name, data[40], data[41]);
                CHECK(get_be32(data + 32) == 0u && get_be32(data + 36) == 0u,
                      "%s owner/group are %u/%u, not 0/0", CASES[index].name,
                      get_be32(data + 32), get_be32(data + 36));
                CHECK(get_be32(data + 44) == 1u,
                      "%s linkCount is %u, the stock volume's is 1",
                      CASES[index].name, get_be32(data + 44));
                CHECK(memcmp(data + 48, "slnk", 4u) == 0,
                      "%s Finder type is %02x%02x%02x%02x, not 'slnk'",
                      CASES[index].name, data[48], data[49], data[50],
                      data[51]);
                CHECK(memcmp(data + 52, "rhap", 4u) == 0,
                      "%s Finder creator is %02x%02x%02x%02x, not 'rhap'",
                      CASES[index].name, data[52], data[53], data[54],
                      data[55]);
                CHECK(all_zero(data + 56, 8u),
                      "%s fdFlags/fdLocation/fdFldr are not zero",
                      CASES[index].name);
                CHECK(all_zero(data + 64, 16u),
                      "%s FndrOpaqueInfo is not 16 zero bytes",
                      CASES[index].name);
                CHECK(get_be32(data + 80) == 0u && get_be32(data + 84) == 0u,
                      "%s textEncoding/reserved2 are not zero",
                      CASES[index].name);
                CHECK(get_be64(data + 88) == (uint64_t)length,
                      "%s logicalSize is %llu, strlen(target) is %zu",
                      CASES[index].name,
                      (unsigned long long)get_be64(data + 88), length);
                CHECK(get_be32(data + 96) == 0u, "%s clumpSize is %u, not 0",
                      CASES[index].name, get_be32(data + 96));
                CHECK(get_be32(data + 100) == 1u && get_be32(data + 108) == 1u,
                      "%s owns %u blocks in a %u-block extent",
                      CASES[index].name, get_be32(data + 100),
                      get_be32(data + 108));
                CHECK(all_zero(data + 112, 48u),
                      "%s uses more than its first extent descriptor",
                      CASES[index].name);
                CHECK(all_zero(data + 160, 80u),
                      "%s has a non-empty resource fork", CASES[index].name);
                start = get_be32(data + 104);
                CHECK(tr_bitmap(&vol, start),
                      "%s: allocation bit for block %u was not set",
                      CASES[index].name, start);
                CHECK((size_t)start * FX_BLOCK_SIZE + length <=
                          run.output_size &&
                      memcmp(run.output + (size_t)start * FX_BLOCK_SIZE,
                             CASES[index].target, length) == 0,
                      "%s: the target bytes are not in its allocation block",
                      CASES[index].name);
                /*
                 * The NUL is outside logicalSize on the stock volume: the
                 * target ends where the fork ends, and the rest of the block
                 * is zero.  Both halves matter -- a writer that stored
                 * strlen+1 would look identical until something read the link.
                 */
                CHECK(run.output[(size_t)start * FX_BLOCK_SIZE + length] == 0u,
                      "%s: the byte past the target is not zero",
                      CASES[index].name);
                CHECK(all_zero(run.output + (size_t)start * FX_BLOCK_SIZE +
                               length, FX_BLOCK_SIZE - length),
                      "%s: the tail of the target's block is not zeroed",
                      CASES[index].name);
                /* A symlink's thread is an ordinary FILE thread. */
                CHECK(tr_find(&vol, cnid, NULL, &thread) && thread.type == 4u,
                      "%s has no file thread record", CASES[index].name);
                CHECK(get_be32(thread.data + 4) == CASES[index].parent,
                      "%s thread names parent %u, expected %u",
                      CASES[index].name, get_be32(thread.data + 4),
                      CASES[index].parent);
            }
            /* Symlinks are files to the volume header, not folders. */
            CHECK(vol.file_count == 4u && vol.folder_count == 3u,
                  "after three symlinks the volume counts are file=%u "
                  "folder=%u, expected 4/3", vol.file_count, vol.folder_count);
            {
                tr_record_t alpha;

                CHECK(tr_find(&vol, FX_ROOT, "alpha", &alpha) &&
                      get_be32(alpha.data + 84) == 1u,
                      "alpha's folderCount changed for a SYMLINK child");
            }
            tr_close(&vol);
        }
    }
    run_release(&run);

    /*
     * groupID is the caller's, exactly as on the stock volume where 25 of the
     * 409 carry 80/admin, and so are the permission bits -- but the format
     * bits are not, and 0755 stays the default because that is what all 409
     * carry.
     */
    {
        rootfs_work_entry_t owned;

        entry_symlink(&owned, "/etc", "private/etc", 0777u);
        owned.owner_id = 501u;
        owned.group_id = 80u;
        if (run_provision(&run, fx, "symowner", &owned, 1u, 0u)) {
            tr_volume_t vol;
            tr_record_t link;

            expect_success(&run, "symlink with an explicit owner and mode");
            if (run.output && tr_open(run.output, run.output_size, &vol)) {
                CHECK(tr_find(&vol, FX_ROOT, "etc", &link),
                      "the owned symlink is missing");
                CHECK(get_be16(link.data + 42) == (0120000u | 0777u),
                      "an explicit symlink mode came back as 0%o",
                      get_be16(link.data + 42));
                CHECK(get_be32(link.data + 32) == 501u &&
                      get_be32(link.data + 36) == 80u,
                      "symlink owner/group came back as %u/%u",
                      get_be32(link.data + 32), get_be32(link.data + 36));
                CHECK(memcmp(link.data + 48, "slnk", 4u) == 0,
                      "an explicit mode disturbed the Finder type");
                tr_close(&vol);
            }
            run_release(&run);
        }
    }

    /* A symlink with no target is not a symlink, and is refused rather than
     * written as a zero-length one. */
    {
        rootfs_work_entry_t bad;

        entry_symlink(&bad, "/etc", "", 0u);
        if (run_provision(&run, fx, "symempty", &bad, 1u, 0u)) {
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_INVALID,
                           "symlink with an empty target");
            run_release(&run);
        }
        entry_symlink(&bad, "/etc", NULL, 0u);
        if (run_provision(&run, fx, "symnull", &bad, 1u, 0u)) {
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_INVALID,
                           "symlink with a NULL target");
            run_release(&run);
        }
        /* An embedded NUL would silently truncate the link for the guest. */
        entry_symlink(&bad, "/etc", "private/etc", 0u);
        bad.content = (const uint8_t *)"private\0etc";
        bad.content_size = 11u;
        if (run_provision(&run, fx, "symnul", &bad, 1u, 0u)) {
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_INVALID,
                           "symlink target with an embedded NUL");
            run_release(&run);
        }
        entry_symlink(&bad, "/etc", "private/\x80tc", 0u);
        if (run_provision(&run, fx, "symhigh", &bad, 1u, 0u)) {
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_INVALID,
                           "symlink target outside printable ASCII");
            run_release(&run);
        }
    }
    /* Above the target cap, which is reported and not silently truncated. */
    {
        rootfs_work_entry_t big;
        char *target = (char *)malloc(ROOTFS_WORK_MAX_PATH + 2u);

        if (target) {
            memset(target, 'x', ROOTFS_WORK_MAX_PATH + 1u);
            target[ROOTFS_WORK_MAX_PATH + 1u] = '\0';
            entry_symlink(&big, "/etc", target, 0u);
            if (run_provision(&run, fx, "symbig", &big, 1u, 0u)) {
                expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_LIMIT,
                               "symlink target above the path cap");
                run_release(&run);
            }
            free(target);
        }
    }
    /* An entry kind that is none of the three is refused by name. */
    {
        rootfs_work_entry_t odd;

        entry_directory(&odd, "/gamma", 0u);
        odd.kind = (rootfs_work_entry_kind_t)7;
        if (run_provision(&run, fx, "symkind", &odd, 1u, 0u)) {
            expect_refusal(&run, fx, ROOTFS_WORK_PROVISION_INVALID,
                           "an entry kind that is not one of the three");
            run_release(&run);
        }
    }
    free(fx);
}

/*
 * setuid, setgid and sticky must reach the record's BSD fileMode unmasked.
 * These are not hypothetical bits: the acquired payload ships MobileCydia
 * 06755, bin/su 04555, private/var/local 02775 and private/var/lock 01777, and
 * a provisioner that quietly dropped any of them would produce a Cydia that
 * cannot escalate and a lock directory anyone can empty.
 */
static void test_special_mode_bits_survive(void) {
    static const struct {
        int directory;
        const char *path;
        const char *name;
        uint32_t parent;
        uint16_t mode;
        uint32_t group;
        const char *payload_path;
    } CASES[] = {
        {0, "/alpha/MobileCydia", "MobileCydia", FX_ALPHA, 06755u, 0u,
         "Applications/Cydia.app/MobileCydia"},
        {0, "/su", "su", FX_ROOT, 04555u, 0u, "bin/su"},
        {1, "/local", "local", FX_ROOT, 02775u, 50u, "private/var/local"},
        {1, "/lock", "lock", FX_ROOT, 01777u, 0u, "private/var/lock"},
        {0, "/beta/everything", "everything", FX_BETA, 07777u, 0u, "(all 12)"}
    };
    fixture_t *fx = fx_create(FX_DATA_BLOCKS - 1u);
    rootfs_work_entry_t entries[sizeof(CASES) / sizeof(CASES[0])];
    run_t run;
    size_t index;

    if (!fx) {
        CHECK(0, "fixture allocation failed");
        return;
    }
    for (index = 0; index < sizeof(CASES) / sizeof(CASES[0]); index++) {
        if (CASES[index].directory)
            entry_directory(&entries[index], CASES[index].path,
                            CASES[index].mode);
        else
            entry_file(&entries[index], CASES[index].path, "x", 1u,
                       CASES[index].mode);
        entries[index].group_id = CASES[index].group;
    }
    if (!run_provision(&run, fx, "setuid", entries,
                       sizeof(CASES) / sizeof(CASES[0]), 0u)) {
        CHECK(0, "setuid fixture setup failed");
        free(fx);
        return;
    }
    expect_success(&run, "records carrying setuid/setgid/sticky");
    if (run.output) {
        tr_volume_t vol;

        if (tr_open(run.output, run.output_size, &vol)) {
            for (index = 0; index < sizeof(CASES) / sizeof(CASES[0]);
                 index++) {
                tr_record_t record;
                uint16_t want = (uint16_t)((CASES[index].directory ? 0040000u
                                                                   : 0100000u) |
                                           CASES[index].mode);

                if (!tr_find(&vol, CASES[index].parent, CASES[index].name,
                             &record)) {
                    CHECK(0, "%s is missing", CASES[index].path);
                    continue;
                }
                CHECK(get_be16(record.data + 42) == want,
                      "%s (payload %s) came back as mode 0%o, requested 0%o",
                      CASES[index].path, CASES[index].payload_path,
                      get_be16(record.data + 42), want);
                /*
                 * Name the three bits individually, so a mask that ate exactly
                 * one of them says which.
                 */
                CHECK((get_be16(record.data + 42) & 04000u) ==
                          (CASES[index].mode & 04000u),
                      "%s lost or gained setuid", CASES[index].path);
                CHECK((get_be16(record.data + 42) & 02000u) ==
                          (CASES[index].mode & 02000u),
                      "%s lost or gained setgid", CASES[index].path);
                CHECK((get_be16(record.data + 42) & 01000u) ==
                          (CASES[index].mode & 01000u),
                      "%s lost or gained sticky", CASES[index].path);
                CHECK(get_be32(record.data + 36) == CASES[index].group,
                      "%s groupID is %u, requested %u", CASES[index].path,
                      get_be32(record.data + 36), CASES[index].group);
            }
            tr_close(&vol);
        }
    }
    run_release(&run);
    /* A symlink carries them too, since it is the same BSD info word. */
    {
        rootfs_work_entry_t link;

        entry_symlink(&link, "/sticky", "private/var/tmp/", 01777u);
        if (run_provision(&run, fx, "stickylink", &link, 1u, 0u)) {
            tr_volume_t vol;
            tr_record_t record;

            expect_success(&run, "symlink with the sticky bit");
            if (run.output && tr_open(run.output, run.output_size, &vol)) {
                CHECK(tr_find(&vol, FX_ROOT, "sticky", &record) &&
                      get_be16(record.data + 42) == (0120000u | 01777u),
                      "a sticky symlink came back as mode 0%o",
                      tr_find(&vol, FX_ROOT, "sticky", &record) ?
                          get_be16(record.data + 42) : 0u);
                tr_close(&vol);
            }
            run_release(&run);
        }
    }
    free(fx);
}

/* ------------------------------- scale ---------------------------------- */

/*
 * THE PAYLOAD-SIZED RUN.
 *
 * The acquired jailbreak payload is 555 regular files and 89 symlinks, roughly
 * 644 objects, and the header used to cap a request at 64 with a comment saying
 * that raising the constant was the whole change needed.  It was not, and this
 * test is what establishes that: with the cap raised and nothing else changed,
 * this request died after EIGHT of its 678 entries with PROVISION_NODE_FULL.
 *
 * The reason is arithmetic, not a bug in the caller's ordering.  Creating file
 * f under folder P writes two records with two different keys -- the name key
 * (P, f) and the thread key (cnid(f), "") -- and every cnid handed out is
 * larger than P.  So the second file under P must be inserted BETWEEN the first
 * file's name record and the first file's thread record.  Once that stretch of
 * the tree has filled one leaf, the next name record is an interior insert into
 * a full leaf, which the rightmost-append split cannot serve.  The names below
 * are deliberately adjacent (f000.dat, f001.dat, ...) because that is the shape
 * a real payload has and the shape that forces the case.
 *
 * What is asserted, all of it from the PUBLISHED image and through the chain
 * walker rather than the writer's own descent:
 *
 *   - every one of the 678 entries is findable by its (parentCNID, name) key,
 *     carries the CNID the run reported, and has its matching thread record
 *     pointing back at the right parent under the right name
 *   - the leaf chain from firstLeafNode is complete, ordered, bLink-consistent,
 *     ends at lastLeafNode, and holds exactly the records that went in
 *   - every index level's child sequence is exactly the chain of the level
 *     below it, node for node
 *   - the node-allocation map accounts for every node the tree now uses
 *   - the symlinks still match the stock format field for field
 *   - splits actually happened, at the leaf level AND at an index level
 */
#define SC_GROUPS 27u
#define SC_FILES_PER_GROUP 20u
#define SC_LINKS_PER_GROUP 3u
#define SC_DEEP_DIRS 27u
#define SC_ENTRIES (1u + SC_GROUPS * (1u + SC_FILES_PER_GROUP + \
                                      SC_LINKS_PER_GROUP) + \
                    1u + SC_DEEP_DIRS + 1u)
#define SC_PATH 192u
#define SC_BODY 32u
#define SC_NODE_CAP 2048u

typedef struct sc_collect {
    tr_record_t *records;
    uint32_t count;
    uint32_t capacity;
    int overflow;
} sc_collect_t;

static void sc_collect_visit(const tr_record_t *record, void *context) {
    sc_collect_t *collect = (sc_collect_t *)context;

    if (collect->count >= collect->capacity) {
        collect->overflow = 1;
        return;
    }
    collect->records[collect->count++] = *record;
}

static int sc_key_cmp(uint32_t left_parent, const uint16_t *left,
                      uint16_t left_len, uint32_t right_parent,
                      const uint16_t *right, uint16_t right_len) {
    uint16_t limit = left_len < right_len ? left_len : right_len;
    uint16_t index;

    if (left_parent != right_parent)
        return left_parent < right_parent ? -1 : 1;
    for (index = 0; index < limit; index++)
        if (left[index] != right[index])
            return left[index] < right[index] ? -1 : 1;
    if (left_len == right_len)
        return 0;
    return left_len < right_len ? -1 : 1;
}

/* Binary search of the walked records, which are in HFSX binary key order --
 * a linear scan per entry would be 678 walks of a 1366-record tree. */
static const tr_record_t *sc_find(const sc_collect_t *collect, uint32_t parent,
                                  const char *name) {
    uint16_t units[SC_PATH];
    uint16_t length = (uint16_t)(name ? strlen(name) : 0u);
    uint32_t low = 0;
    uint32_t high = collect->count;
    uint16_t index;

    if (length > SC_PATH)
        return NULL;
    for (index = 0; index < length; index++)
        units[index] = (uint16_t)(unsigned char)name[index];
    while (low < high) {
        uint32_t mid = low + (high - low) / 2u;
        const tr_record_t *record = &collect->records[mid];
        int order = sc_key_cmp(record->parent, record->name,
                               record->name_length, parent, units, length);

        if (order == 0)
            return record;
        if (order < 0)
            low = mid + 1u;
        else
            high = mid;
    }
    return NULL;
}

static int sc_thread_name_is(const tr_record_t *thread, const char *name) {
    uint16_t length = get_be16(thread->data + 8);
    uint16_t index;

    if (length != (uint16_t)strlen(name))
        return 0;
    for (index = 0; index < length; index++)
        if (get_be16(thread->data + 10 + index * 2u) !=
            (uint16_t)(unsigned char)name[index])
            return 0;
    return 1;
}

static void test_scale_payload_forces_real_splits(void) {
    static const char *const TARGETS[] = {
        "../../usr/lib/libcydia.dylib", "private/etc/", "bash",
        "../private/var/preferences/"
    };
    scale_fixture_t *sx = sx_create();
    rootfs_work_entry_t *entries = NULL;
    char (*paths)[SC_PATH] = NULL;
    char (*bodies)[SC_BODY] = NULL;
    const char **leaf = NULL;
    int *parent_of = NULL;
    uint32_t *valence = NULL;
    sc_collect_t collect;
    run_t run;
    size_t count = 0;
    size_t index;
    unsigned group;
    unsigned deep;
    int payload_index;
    int deep_index;
    unsigned dirs = 0;
    unsigned files = 0;
    unsigned links = 0;
    unsigned bodied = 0;
    char deep_path[SC_PATH];

    memset(&collect, 0, sizeof(collect));
    entries = (rootfs_work_entry_t *)calloc(SC_ENTRIES, sizeof(*entries));
    paths = (char (*)[SC_PATH])calloc(SC_ENTRIES, SC_PATH);
    bodies = (char (*)[SC_BODY])calloc(SC_ENTRIES, SC_BODY);
    leaf = (const char **)calloc(SC_ENTRIES, sizeof(*leaf));
    parent_of = (int *)calloc(SC_ENTRIES, sizeof(*parent_of));
    valence = (uint32_t *)calloc(SC_ENTRIES, sizeof(*valence));
    if (!sx || !entries || !paths || !bodies || !leaf || !parent_of ||
        !valence) {
        CHECK(0, "scale fixture allocation failed");
        goto done;
    }

    /*
     * /payload/gNN/f000.dat ... adjacent names under a common parent, which is
     * exactly the interleaving that defeats an append-only writer, plus a
     * 30-component chain that runs MAX_PATH_DEPTH (32) close to its cap.
     */
    payload_index = (int)count;
    snprintf(paths[count], SC_PATH, "/payload");
    leaf[count] = paths[count] + 1;
    parent_of[count] = -1;
    entry_directory(&entries[count], paths[count], 0u);
    count++;
    dirs++;
    for (group = 0; group < SC_GROUPS; group++) {
        int group_index = (int)count;
        unsigned which;

        snprintf(paths[count], SC_PATH, "/payload/g%02u", group);
        leaf[count] = paths[count] + strlen("/payload/");
        parent_of[count] = payload_index;
        entry_directory(&entries[count], paths[count], 0u);
        valence[payload_index]++;
        count++;
        dirs++;
        for (which = 0; which < SC_FILES_PER_GROUP; which++) {
            snprintf(paths[count], SC_PATH, "/payload/g%02u/f%03u.dat", group,
                     which);
            leaf[count] = paths[count] + strlen("/payload/gNN/");
            parent_of[count] = group_index;
            /* Every fifth file carries content, so the block allocator is
             * exercised at scale without a 3 MiB image. */
            if (which % 5u == 0u) {
                snprintf(bodies[count], SC_BODY, "payload-%05u\n",
                         (unsigned)count);
                entry_file(&entries[count], paths[count], bodies[count],
                           strlen(bodies[count]), 0u);
                bodied++;
            } else {
                entry_file(&entries[count], paths[count], NULL, 0u, 0u);
            }
            valence[group_index]++;
            count++;
            files++;
        }
        for (which = 0; which < SC_LINKS_PER_GROUP; which++) {
            snprintf(paths[count], SC_PATH, "/payload/g%02u/l%03u", group,
                     which);
            leaf[count] = paths[count] + strlen("/payload/gNN/");
            parent_of[count] = group_index;
            entry_symlink(&entries[count], paths[count],
                          TARGETS[(group + which) % 4u], 0u);
            valence[group_index]++;
            count++;
            links++;
        }
    }
    deep_index = (int)count;
    snprintf(paths[count], SC_PATH, "/payload/deep");
    leaf[count] = paths[count] + strlen("/payload/");
    parent_of[count] = payload_index;
    entry_directory(&entries[count], paths[count], 0u);
    valence[payload_index]++;
    count++;
    dirs++;
    snprintf(deep_path, sizeof(deep_path), "/payload/deep");
    for (deep = 1; deep <= SC_DEEP_DIRS; deep++) {
        size_t at = strlen(deep_path);

        /* Bound `at` so the compiler knows there is room for "/dNN". The chain
         * is 30 components of four bytes, so this is never taken; without it
         * GCC assumes at could reach SC_PATH-1 and reports a truncation, which
         * -Wformat-truncation=2 turns into an error. */
        if (at + 8u >= sizeof deep_path) break;
        snprintf(deep_path + at, sizeof(deep_path) - at, "/d%02u", deep);
        snprintf(paths[count], SC_PATH, "%s", deep_path);
        leaf[count] = paths[count] + at + 1u;
        parent_of[count] = deep_index;
        entry_directory(&entries[count], paths[count], 0u);
        valence[deep_index]++;
        deep_index = (int)count;
        count++;
        dirs++;
    }
    /*
     * The %s is bounded so the compiler can prove the result fits. deep_path is
     * SC_PATH bytes and "/leaf.txt" is nine more, which GCC reports as a
     * possible truncation at -O2 -- and -O2 is -Werror in CI, where this first
     * broke the build. The chain is 30 components of "/dNN", 120 bytes, so the
     * bound never binds in practice; the CHECK is what would catch it if the
     * fixture ever grew deep enough for it to matter.
     */
    CHECK(snprintf(paths[count], SC_PATH, "%.*s/leaf.txt",
                   (int)(SC_PATH - sizeof("/leaf.txt")), deep_path)
              < (int)SC_PATH,
          "the deepest path plus /leaf.txt fits SC_PATH");
    leaf[count] = paths[count] + strlen(deep_path) + 1u;
    parent_of[count] = deep_index;
    snprintf(bodies[count], SC_BODY, "deepest\n");
    entry_file(&entries[count], paths[count], bodies[count],
               strlen(bodies[count]), 0u);
    valence[deep_index]++;
    count++;
    files++;
    bodied++;

    CHECK(count == SC_ENTRIES && count > 650u,
          "the scale fixture built %zu entries, expected %u and over 650",
          count, (unsigned)SC_ENTRIES);
    CHECK(count <= ROOTFS_WORK_MAX_ENTRIES,
          "%zu entries is over the %u cap this test exists to raise", count,
          ROOTFS_WORK_MAX_ENTRIES);
    {
        /* The deepest path must approach MAX_PATH_DEPTH or the depth limit is
         * not being exercised at all. */
        unsigned components = 0;
        const char *cursor = paths[count - 1u];

        while (*cursor != '\0')
            if (*cursor++ == '/')
                components++;
        CHECK(components == 30u && components <= ROOTFS_WORK_MAX_PATH_DEPTH,
              "the deepest provisioned path has %u components against a cap "
              "of %u", components, ROOTFS_WORK_MAX_PATH_DEPTH);
    }

    if (!run_provision_image(&run, sx->image, SX_SIZE, "scale", entries, count,
                             0u)) {
        CHECK(0, "the scale run could not be started");
        goto done;
    }
    expect_success(&run, "a 678-entry payload");
    printf("  scale: %u entries, %u leaf splits, %u index splits, "
           "%u blocks\n", run.result.provision_entries,
           run.result.provision_leaf_splits, run.result.provision_index_splits,
           run.result.provision_blocks);
    if (run.status != ROOTFS_WORK_OK) {
        run_release(&run);
        goto done;
    }
    CHECK(run.result.provision_entries == (uint32_t)count,
          "the run reported %u of %zu entries", run.result.provision_entries,
          count);
    CHECK(run.result.provision_first_cnid == FX_NEXT_CNID &&
          run.result.provision_last_cnid == FX_NEXT_CNID + (uint32_t)count - 1u,
          "CNIDs ran %u..%u, expected %u..%u",
          run.result.provision_first_cnid, run.result.provision_last_cnid,
          FX_NEXT_CNID, FX_NEXT_CNID + (uint32_t)count - 1u);
    /*
     * THE HEADLINE.  If either of these is zero the fixture did not exercise
     * what this test claims and its result means nothing.
     */
    CHECK(run.result.provision_leaf_splits > 0u,
          "the scale run reported ZERO leaf splits, so it never split a node");
    CHECK(run.result.provision_index_splits > 0u,
          "the scale run reported ZERO index splits, so the index level was "
          "never extended");

    if (run.output) {
        tr_volume_t vol;

        if (tr_open(run.output, run.output_size, &vol)) {
            uint32_t *chain = (uint32_t *)calloc(SC_NODE_CAP, sizeof(*chain));
            uint32_t *below = (uint32_t *)calloc(SC_NODE_CAP, sizeof(*below));
            uint32_t *kids = (uint32_t *)calloc(SC_NODE_CAP, sizeof(*kids));
            uint8_t *seen = (uint8_t *)calloc(SX_TOTAL_BLOCKS, 1u);
            tr_walk_t walk;
            int links_ok = 0;
            uint32_t leaf_nodes = 0;
            uint32_t below_count = 0;
            uint32_t used_nodes = 1u;   /* the header node */
            uint16_t level;

            collect.capacity = (uint32_t)(2u * count + 64u);
            collect.records = (tr_record_t *)calloc(collect.capacity,
                                                    sizeof(*collect.records));
            if (!chain || !below || !kids || !seen || !collect.records) {
                CHECK(0, "scale verification allocation failed");
                free(chain);
                free(below);
                free(kids);
                free(seen);
                tr_close(&vol);
                run_release(&run);
                goto done;
            }

            /* ---- the leaf chain, walked from firstLeafNode ---- */
            tr_walk(&vol, &walk, sc_collect_visit, &collect);
            leaf_nodes = tr_level_chain(&vol, vol.first_leaf, chain,
                                        SC_NODE_CAP, &links_ok);
            CHECK(walk.shape_ok && walk.order_ok && !collect.overflow,
                  "scale chain: shape=%d order=%d overflow=%d", walk.shape_ok,
                  walk.order_ok, collect.overflow);
            CHECK(walk.records == vol.leaf_records,
                  "the leaf chain holds %u records, the header says %u",
                  walk.records, vol.leaf_records);
            CHECK(walk.records == 10u + 2u * (uint32_t)count,
                  "the leaf chain holds %u records, expected %u (10 shipped + "
                  "2 per entry)", walk.records, 10u + 2u * (uint32_t)count);
            CHECK(walk.final_node == vol.last_leaf,
                  "the chain ends at node %u, lastLeafNode says %u",
                  walk.final_node, vol.last_leaf);
            CHECK(links_ok && leaf_nodes == walk.nodes,
                  "the fLink/bLink walk found %u leaves, the record walk %u "
                  "(links_ok=%d)", leaf_nodes, walk.nodes, links_ok);
            CHECK(vol.first_leaf == sx->first_leaf,
                  "firstLeafNode moved to %u, was %u", vol.first_leaf,
                  sx->first_leaf);
            CHECK(leaf_nodes > sx->leaf_nodes,
                  "the run ended with %u leaves, started with %u -- nothing "
                  "split", leaf_nodes, sx->leaf_nodes);
            /* The root is the one node this writer will not split. */
            CHECK(vol.tree_depth == sx->tree_depth &&
                  vol.root_node == sx->root_node,
                  "the tree became depth %u rooted at %u, was %u/%u",
                  vol.tree_depth, vol.root_node, sx->tree_depth,
                  sx->root_node);
            used_nodes += leaf_nodes;

            /* ---- every index level against the level below it ---- */
            memcpy(below, chain, (size_t)leaf_nodes * sizeof(*below));
            below_count = leaf_nodes;
            for (level = 2u; level <= vol.tree_depth; level++) {
                uint32_t leftmost = vol.root_node;
                uint32_t nodes;
                uint32_t cursor = 0;
                uint32_t at;
                uint16_t down;
                int level_ok = 1;

                for (down = vol.tree_depth; down > level; down--) {
                    uint32_t first[1];

                    if (tr_children(&vol, leftmost, (uint8_t)down, first,
                                    1u) == 0u) {
                        level_ok = 0;
                        break;
                    }
                    leftmost = first[0];
                }
                nodes = tr_level_chain(&vol, leftmost, chain, SC_NODE_CAP,
                                       &links_ok);
                for (at = 0; at < nodes; at++) {
                    uint16_t children = tr_children(&vol, chain[at],
                                                    (uint8_t)level, kids,
                                                    SC_NODE_CAP);
                    uint16_t child;

                    if (children == 0u)
                        level_ok = 0;
                    for (child = 0; child < children; child++) {
                        if (cursor >= below_count ||
                            kids[child] != below[cursor])
                            level_ok = 0;
                        cursor++;
                    }
                }
                CHECK(level_ok && links_ok && cursor == below_count,
                      "level %u names %u children over %u nodes; the level "
                      "below is a %u-node chain (ok=%d links=%d)", level,
                      cursor, nodes, below_count, level_ok, links_ok);
                memcpy(below, chain, (size_t)nodes * sizeof(*below));
                below_count = nodes;
                used_nodes += nodes;
            }
            CHECK(below_count == 1u && below[0] == vol.root_node,
                  "the top level is %u nodes, and its first is %u, not the "
                  "root %u", below_count, below_count ? below[0] : 0u,
                  vol.root_node);

            /* ---- the node-allocation map accounts for the tree ---- */
            /*
             * The tree's node count is the ceiling on how many nodes one
             * request can have TOUCHED, which is what
             * ROOTFS_WORK_MAX_CATALOG_NODES bounds.  Printed so the constant
             * can be justified from a measurement rather than an estimate.
             */
            printf("  scale: %u leaves, %u nodes used of %u, cap %u\n",
                   leaf_nodes, used_nodes, vol.total_nodes,
                   ROOTFS_WORK_MAX_CATALOG_NODES);
            CHECK(used_nodes <= ROOTFS_WORK_MAX_CATALOG_NODES,
                  "the published tree uses %u nodes, over the %u-node cap on "
                  "what one request may touch", used_nodes,
                  ROOTFS_WORK_MAX_CATALOG_NODES);
            CHECK(vol.free_nodes == vol.total_nodes - used_nodes,
                  "freeNodes is %u for %u total and %u in the tree",
                  vol.free_nodes, vol.total_nodes, used_nodes);
            {
                uint32_t marked = 0;
                uint32_t node;

                for (node = 0; node < vol.total_nodes; node++)
                    if (tr_node_used(&vol, node) == 1)
                        marked++;
                CHECK(marked == used_nodes,
                      "the node map marks %u nodes in use, the tree uses %u",
                      marked, used_nodes);
            }

            /* ---- every entry, by key, from the walked records ---- */
            {
                uint32_t missing_name = 0;
                uint32_t wrong_shape = 0;
                uint32_t missing_thread = 0;
                uint32_t wrong_thread = 0;
                uint32_t bad_symlink = 0;
                uint32_t bad_content = 0;
                uint32_t bad_valence = 0;
                uint32_t first_cnid = run.result.provision_first_cnid;

                for (index = 0; index < count; index++) {
                    uint32_t cnid = first_cnid + (uint32_t)index;
                    uint32_t parent = parent_of[index] < 0 ? FX_ROOT :
                        first_cnid + (uint32_t)parent_of[index];
                    const rootfs_work_entry_t *entry = &entries[index];
                    const tr_record_t *record = sc_find(&collect, parent,
                                                        leaf[index]);
                    const tr_record_t *thread = sc_find(&collect, cnid, NULL);
                    uint16_t want_type = entry->kind ==
                        ROOTFS_WORK_ENTRY_DIRECTORY ? 1u : 2u;

                    if (!record) {
                        missing_name++;
                        continue;
                    }
                    if (record->type != want_type ||
                        get_be32(record->data + 8) != cnid)
                        wrong_shape++;
                    if (!thread) {
                        missing_thread++;
                    } else if (thread->type != (want_type == 1u ? 3u : 4u) ||
                               get_be32(thread->data + 4) != parent ||
                               !sc_thread_name_is(thread, leaf[index])) {
                        wrong_thread++;
                    }
                    if (entry->kind == ROOTFS_WORK_ENTRY_DIRECTORY) {
                        if (get_be16(record->data + 42) != (0040000u | 0755u) ||
                            get_be32(record->data + 4) != valence[index])
                            bad_valence++;
                        continue;
                    }
                    if (get_be64(record->data + 88) !=
                            (uint64_t)entry->content_size ||
                        get_be32(record->data + 100) !=
                            (entry->content_size != 0u ? 1u : 0u))
                        bad_content++;
                    if (entry->kind == ROOTFS_WORK_ENTRY_SYMLINK) {
                        /* The stock format, field for field. */
                        if (get_be16(record->data + 42) != (0120000u | 0755u) ||
                            memcmp(record->data + 48, "slnk", 4u) != 0 ||
                            memcmp(record->data + 52, "rhap", 4u) != 0 ||
                            get_be32(record->data + 44) != 1u ||
                            get_be32(record->data + 100) != 1u ||
                            !all_zero(record->data + 112, 56u) ||
                            !all_zero(record->data + 168, 80u))
                            bad_symlink++;
                    } else if (get_be16(record->data + 42) !=
                               (0100000u | 0644u)) {
                        wrong_shape++;
                    }
                    if (entry->content_size != 0u) {
                        uint32_t start = get_be32(record->data + 104);
                        uint64_t at = (uint64_t)start * vol.block_size;

                        if (start >= SX_TOTAL_BLOCKS || seen[start] ||
                            tr_bitmap(&vol, start) != 1 ||
                            at + entry->content_size > run.output_size ||
                            memcmp(run.output + at, entry->content,
                                   entry->content_size) != 0)
                            bad_content++;
                        else
                            seen[start] = 1u;
                    }
                }
                CHECK(missing_name == 0u,
                      "%u of %zu provisioned objects are not in the published "
                      "leaf chain", missing_name, count);
                CHECK(wrong_shape == 0u,
                      "%u provisioned records have the wrong type, CNID or "
                      "mode", wrong_shape);
                CHECK(missing_thread == 0u && wrong_thread == 0u,
                      "%u thread records are missing and %u name the wrong "
                      "parent or leaf", missing_thread, wrong_thread);
                CHECK(bad_valence == 0u,
                      "%u provisioned directories have the wrong mode or "
                      "valence", bad_valence);
                CHECK(bad_content == 0u,
                      "%u provisioned files disagree with their own content or "
                      "extent", bad_content);
                CHECK(bad_symlink == 0u,
                      "%u of %u symlinks do not match the stock format",
                      bad_symlink, links);
            }

            /* ---- the volume's own counters ---- */
            CHECK(vol.file_count == 1u + files + links,
                  "fileCount is %u, expected %u", vol.file_count,
                  1u + files + links);
            CHECK(vol.folder_count == 3u + dirs,
                  "folderCount is %u, expected %u", vol.folder_count,
                  3u + dirs);
            CHECK(vol.next_cnid == FX_NEXT_CNID + (uint32_t)count,
                  "nextCatalogID is %u, expected %u", vol.next_cnid,
                  FX_NEXT_CNID + (uint32_t)count);
            CHECK(run.result.provision_blocks == bodied + links,
                  "the run claimed %u blocks for %u bodied files and %u "
                  "symlinks", run.result.provision_blocks, bodied, links);
            CHECK(vol.free_blocks ==
                      sx->free_blocks - run.result.provision_blocks,
                  "freeBlocks is %u, expected %u", vol.free_blocks,
                  sx->free_blocks - run.result.provision_blocks);

            free(chain);
            free(below);
            free(kids);
            free(seen);
            tr_close(&vol);
        } else {
            CHECK(0, "the published scale image could not be opened");
        }
    }
    run_release(&run);

done:
    free(collect.records);
    free(valence);
    free(parent_of);
    free((void *)leaf);
    free(bodies);
    free(paths);
    free(entries);
    sx_release(sx);
}

static void test_status_and_stage_names(void) {
    static const rootfs_work_status_t statuses[] = {
        ROOTFS_WORK_PROVISION_INVALID, ROOTFS_WORK_PROVISION_UNSUPPORTED,
        ROOTFS_WORK_PROVISION_CATALOG_CORRUPT,
        ROOTFS_WORK_PROVISION_PARENT_MISSING, ROOTFS_WORK_PROVISION_EXISTS,
        ROOTFS_WORK_PROVISION_NODE_FULL, ROOTFS_WORK_PROVISION_LEAF_HEAD,
        ROOTFS_WORK_PROVISION_SPLIT_UNSUPPORTED,
        ROOTFS_WORK_PROVISION_BTREE_FULL,
        ROOTFS_WORK_PROVISION_NO_SPACE, ROOTFS_WORK_PROVISION_LIMIT
    };
    size_t index;

    for (index = 0; index < sizeof(statuses) / sizeof(statuses[0]); index++)
        CHECK(strcmp(rootfs_work_status_name(statuses[index]), "unknown") != 0,
              "status %d has no name", (int)statuses[index]);
    CHECK(strcmp(rootfs_work_stage_name(ROOTFS_WORK_STAGE_PROVISION_PLAN),
                 "provision-plan") == 0, "plan stage is unnamed");
    CHECK(strcmp(rootfs_work_stage_name(ROOTFS_WORK_STAGE_PROVISION_WRITE),
                 "provision-write") == 0, "write stage is unnamed");
}

int main(void) {
    printf("HFS+ catalog provisioning tests\n");
    test_status_and_stage_names();
    test_fixture_is_a_valid_volume();
    test_create_directory();
    test_create_file();
    test_create_directory_and_file_together();
    test_deep_existing_parents();
    test_missing_parent_is_refused();
    test_duplicate_name_is_refused();
    test_full_leaf_is_refused_not_split();
    test_out_of_space_is_refused();
    test_broken_catalog_is_never_absence();
    test_unsupported_catalogs_are_refused();
    test_index_descent_and_leaf_head();
    test_rightmost_leaf_split();
    test_rightmost_index_split();
    test_split_boundaries_are_refused();
    test_scale_payload_forces_real_splits();
    test_symlink_matches_the_stock_format();
    test_special_mode_bits_survive();
    test_invalid_requests_are_refused();
    test_output_is_deterministic();
    test_activation_entries();
    test_ppp_entries();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
