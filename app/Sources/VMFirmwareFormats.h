/*
 * S5LBox — the container formats an IPSW is made of.
 *
 * Everything declared here reads bytes the user found on the internet. Every
 * length, offset and count in an IPSW is attacker-controlled in the only sense
 * that matters: nobody checked it, and a wrong one must produce a named refusal
 * rather than a read past the end of a buffer. So the rules for this file are
 * the same as core/src/firmware/img3.c's, and for the same reason:
 *
 *   - every declared size is validated against the real buffer before any read;
 *   - all size arithmetic is done in 64-bit so overflow cannot wrap;
 *   - an impossible field is refused, never clamped;
 *   - nothing allocates, so nothing can fail to allocate halfway through.
 *
 * Deliberately plain C11: no UIKit, no Foundation, no platform headers beyond
 * the standard ones. The iOS shell is a caller, and a host CI runner can build
 * and test every line below without a phone.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_VM_FIRMWARE_FORMATS_H
#define S5LBOX_VM_FIRMWARE_FORMATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------ */
/* Streaming                                                                 */
/* ------------------------------------------------------------------------ */
/*
 * The three artefacts differ in size by four orders of magnitude: the device
 * tree is 40 KB and the root filesystem is 433 MB. Nothing here may assume the
 * data fits in memory, because on the phone this actually runs on it does not.
 * So bulk data moves through a pull source and a push sink and the decoders
 * hold only their own working state.
 */

/* Fill up to `cap` bytes. Returns the count read; 0 means end of input.
 * Returns VMFW_SOURCE_ERROR to report a read failure, which is distinct from
 * end of input and must not be mistaken for it. */
#define VMFW_SOURCE_ERROR ((size_t)-1)
typedef size_t (*vmfw_source_fn)(void *ctx, uint8_t *buf, size_t cap);

/* Consume `len` bytes. Returns false to abort the operation. */
typedef bool (*vmfw_sink_fn)(void *ctx, const uint8_t *data, size_t len);

/* A source over a fixed buffer, for tests and for small in-memory members. */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} vmfw_mem_source_t;

void vmfw_mem_source_init(vmfw_mem_source_t *s, const uint8_t *data, size_t len);
size_t vmfw_mem_source_read(void *ctx, uint8_t *buf, size_t cap);

/* A sink into a fixed buffer that refuses to overrun it. `overflowed` is set
 * if more bytes arrived than `cap` allowed; the sink returns false at that
 * point, so a caller that ignores the flag still cannot corrupt memory. */
typedef struct {
    uint8_t *data;
    size_t cap;
    size_t len;
    bool overflowed;
} vmfw_mem_sink_t;

void vmfw_mem_sink_init(vmfw_mem_sink_t *s, uint8_t *data, size_t cap);
bool vmfw_mem_sink_write(void *ctx, const uint8_t *data, size_t len);

/* A sink that counts and discards, for measuring without storing. */
typedef struct { uint64_t len; } vmfw_null_sink_t;
bool vmfw_null_sink_write(void *ctx, const uint8_t *data, size_t len);

/* ------------------------------------------------------------------------ */
/* DEFLATE (RFC 1951) and zlib (RFC 1950)                                    */
/* ------------------------------------------------------------------------ */
/*
 * 43 of the 49 members in a 7E18 IPSW are deflated, and 1,622 of the root
 * filesystem's 1,659 UDIF chunks are zlib streams. There is no way to read an
 * IPSW without this, and linking a system zlib would put the one piece of
 * untrusted-input parsing we cannot test outside the suite that tests
 * everything else. So it is implemented here and mutation-tested with the rest.
 */
typedef enum {
    VMFW_INFLATE_OK = 0,
    VMFW_INFLATE_ERR_INVALID_ARGUMENT,
    VMFW_INFLATE_ERR_TRUNCATED,      /* input ended mid-stream               */
    VMFW_INFLATE_ERR_READ,           /* the source reported a read error     */
    VMFW_INFLATE_ERR_BAD_BLOCK,      /* reserved block type 3                */
    VMFW_INFLATE_ERR_BAD_STORED,     /* stored block LEN/NLEN disagree       */
    VMFW_INFLATE_ERR_BAD_HUFFMAN,    /* over-subscribed or incomplete code   */
    VMFW_INFLATE_ERR_BAD_SYMBOL,     /* symbol outside the defined range     */
    VMFW_INFLATE_ERR_BAD_DISTANCE,   /* back-reference before the output     */
    VMFW_INFLATE_ERR_SIZE_MISMATCH,  /* produced != the caller's expectation */
    VMFW_INFLATE_ERR_SINK,           /* the sink refused                     */
    VMFW_INFLATE_ERR_BAD_ZLIB_HEADER /* zlib CMF/FLG invalid or preset dict  */
} vmfw_inflate_status_t;

/*
 * Expand a raw DEFLATE stream.
 *
 * `expect_out` is the size the container claims the result will be. It is a
 * bound, not a hint: production stops at exactly that many bytes and a stream
 * that wants to write more is refused. Pass VMFW_INFLATE_NO_EXPECTATION only
 * when no container stated a size — every caller in this project has one, and
 * checking it is what turns a corrupted member into an error instead of a
 * silently short file.
 */
#define VMFW_INFLATE_NO_EXPECTATION UINT64_MAX

vmfw_inflate_status_t vmfw_inflate(vmfw_source_fn source, void *source_ctx,
                                   vmfw_sink_fn sink, void *sink_ctx,
                                   uint64_t expect_out, uint64_t *out_produced);

/* The same, wrapped in a zlib header/trailer, and the Adler-32 is checked. */
vmfw_inflate_status_t vmfw_zlib_inflate(vmfw_source_fn source, void *source_ctx,
                                        vmfw_sink_fn sink, void *sink_ctx,
                                        uint64_t expect_out,
                                        uint64_t *out_produced);

/* Convenience for the small, bounded case: whole input and output in memory. */
vmfw_inflate_status_t vmfw_inflate_buffer(const uint8_t *src, size_t srclen,
                                          uint8_t *dst, size_t dstcap,
                                          uint64_t expect_out, size_t *out_len);
vmfw_inflate_status_t vmfw_zlib_inflate_buffer(const uint8_t *src, size_t srclen,
                                               uint8_t *dst, size_t dstcap,
                                               uint64_t expect_out,
                                               size_t *out_len);

const char *vmfw_inflate_strerror(vmfw_inflate_status_t st);

/* ------------------------------------------------------------------------ */
/* SHA-1 and HMAC-SHA1                                                       */
/* ------------------------------------------------------------------------ */
/*
 * Not used to authenticate anything. An `encrcdsa` image derives each block's
 * AES IV as HMAC-SHA1(hmac_key, big-endian block index), so this is key
 * schedule material, and SHA-256 (tools/sha256.c) remains the only thing that
 * decides whether an artefact is the right one.
 */
#define VMFW_SHA1_DIGEST_SIZE 20u
#define VMFW_SHA1_BLOCK_SIZE  64u

typedef struct {
    uint32_t state[5];
    uint64_t total_bytes;
    uint8_t  block[VMFW_SHA1_BLOCK_SIZE];
    size_t   block_used;
    bool     finalized;
} vmfw_sha1_ctx_t;

bool vmfw_sha1_init(vmfw_sha1_ctx_t *ctx);
bool vmfw_sha1_update(vmfw_sha1_ctx_t *ctx, const void *data, size_t len);
bool vmfw_sha1_final(vmfw_sha1_ctx_t *ctx, uint8_t out[VMFW_SHA1_DIGEST_SIZE]);
bool vmfw_sha1(const void *data, size_t len, uint8_t out[VMFW_SHA1_DIGEST_SIZE]);

bool vmfw_hmac_sha1(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[VMFW_SHA1_DIGEST_SIZE]);

/* ------------------------------------------------------------------------ */
/* Apple XML property lists                                                  */
/* ------------------------------------------------------------------------ */
/*
 * Two plists decide what an IPSW contains, and reading them is the difference
 * between identifying members and guessing at their names. Restore.plist names
 * the kernelcache, the board and the system restore image; a UDIF trailer's
 * resource fork names the partitions inside the root filesystem.
 *
 * This is a read-only scanner over the caller's bytes: it copies out scalars
 * and hands back spans for bulk data. It does not build a tree and it does not
 * allocate, so a plist with a million nested dictionaries costs nothing but the
 * time to walk past it.
 */
#define VMFW_PLIST_MAX_DEPTH 32u

typedef struct {
    const uint8_t *xml;
    size_t len;
} vmfw_plist_t;

typedef enum {
    VMFW_PLIST_OK = 0,
    VMFW_PLIST_ERR_INVALID_ARGUMENT,
    VMFW_PLIST_ERR_NOT_XML,        /* no <plist> root                       */
    VMFW_PLIST_ERR_MALFORMED,      /* unterminated tag, bad nesting         */
    VMFW_PLIST_ERR_TOO_DEEP,       /* nesting beyond VMFW_PLIST_MAX_DEPTH   */
    VMFW_PLIST_ERR_NOT_FOUND,      /* the path names nothing                */
    VMFW_PLIST_ERR_WRONG_TYPE,     /* found, but not the type asked for     */
    VMFW_PLIST_ERR_TOO_LONG,       /* value does not fit the caller's buffer*/
    VMFW_PLIST_ERR_BAD_BASE64
} vmfw_plist_status_t;

/* Validate enough of the document to walk it. */
vmfw_plist_status_t vmfw_plist_init(vmfw_plist_t *pl,
                                    const uint8_t *xml, size_t len);

/*
 * Paths are slash-separated. A component is a dictionary key, or a decimal
 * index into an array: "ProductType", "SystemRestoreImages/User",
 * "DeviceMap/0/BoardConfig", "resource-fork/blkx/5/Name".
 *
 * A key containing a slash is not addressable. No key in either plist this
 * project reads contains one, and inventing an escape syntax for a case that
 * does not occur would be untested code on the untrusted-input path.
 */
vmfw_plist_status_t vmfw_plist_get_string(const vmfw_plist_t *pl,
                                          const char *path,
                                          char *out, size_t cap);

/* Number of elements in the <array> at `path`. */
vmfw_plist_status_t vmfw_plist_array_count(const vmfw_plist_t *pl,
                                           const char *path, size_t *out);

/* The still-encoded base64 span of the <data> at `path`. Points into the
 * caller's bytes; nothing is copied. */
vmfw_plist_status_t vmfw_plist_get_data_span(const vmfw_plist_t *pl,
                                             const char *path,
                                             const uint8_t **out_b64,
                                             size_t *out_b64_len);

/* Decode base64, skipping the whitespace Apple's writer inserts every 64
 * characters. Refuses any other character rather than ignoring it. */
vmfw_plist_status_t vmfw_base64_decode(const uint8_t *in, size_t in_len,
                                       uint8_t *out, size_t cap,
                                       size_t *out_len);

/* The decoded length base64 of `in_len` characters can produce, for sizing a
 * buffer before decoding. Whitespace is not counted. */
size_t vmfw_base64_decoded_size(const uint8_t *in, size_t in_len);

const char *vmfw_plist_strerror(vmfw_plist_status_t st);

/* ------------------------------------------------------------------------ */
/* ZIP (an IPSW is one)                                                      */
/* ------------------------------------------------------------------------ */
/*
 * Read through the central directory, never the local headers. In a real IPSW
 * every member sets the streaming flag (bit 3), which means the sizes in the
 * local header are zero and the true ones are in the central directory. A
 * reader that trusted the local header would extract nothing at all, and one
 * that trusted a data descriptor would be trusting a length that appears after
 * the data it describes.
 */
#define VMFW_ZIP_MAX_NAME 256u

typedef enum {
    VMFW_ZIP_OK = 0,
    VMFW_ZIP_ERR_INVALID_ARGUMENT,
    VMFW_ZIP_ERR_READ,             /* the file could not be read            */
    VMFW_ZIP_ERR_NOT_ZIP,          /* no end-of-central-directory record    */
    VMFW_ZIP_ERR_MULTI_DISK,       /* a split archive                       */
    VMFW_ZIP_ERR_ZIP64,            /* 64-bit archive; see the note below    */
    VMFW_ZIP_ERR_BAD_DIRECTORY,    /* a central directory entry is impossible*/
    VMFW_ZIP_ERR_TOO_MANY,         /* more members than we will enumerate   */
    VMFW_ZIP_ERR_NAME_TOO_LONG,
    VMFW_ZIP_ERR_ENCRYPTED,        /* ZIP-level encryption; IPSWs use none  */
    VMFW_ZIP_ERR_BAD_METHOD,       /* not stored and not deflated           */
    VMFW_ZIP_ERR_BAD_LOCAL_HEADER,
    VMFW_ZIP_ERR_MEMBER_TRUNCATED, /* the member runs past end of file      */
    VMFW_ZIP_ERR_INFLATE,          /* see the inflate status for detail     */
    VMFW_ZIP_ERR_CRC_MISMATCH,     /* extracted bytes are not the member    */
    VMFW_ZIP_ERR_SINK
} vmfw_zip_status_t;

#define VMFW_ZIP_METHOD_STORE   0u
#define VMFW_ZIP_METHOD_DEFLATE 8u

typedef struct {
    char     name[VMFW_ZIP_MAX_NAME];
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint64_t local_header_offset;
    uint32_t crc32;
    uint16_t method;
    bool     is_directory;
} vmfw_zip_entry_t;

/*
 * The archive is read through a caller-supplied random-access reader rather
 * than a FILE*, so the core stays free of any file API and the tests can serve
 * an archive out of memory. Returns the number of bytes read, which must be
 * `len` for success; anything else is treated as a read error.
 */
typedef size_t (*vmfw_pread_fn)(void *ctx, uint64_t offset,
                                uint8_t *buf, size_t len);

typedef struct {
    vmfw_pread_fn pread;
    void         *ctx;
    uint64_t      size;          /* total archive size in bytes             */
    uint64_t      cd_offset;     /* central directory start                 */
    uint64_t      cd_size;
    uint32_t      entry_count;
} vmfw_zip_t;

/* Locate and validate the end-of-central-directory record. */
vmfw_zip_status_t vmfw_zip_open(vmfw_zip_t *zip, vmfw_pread_fn pread, void *ctx,
                                uint64_t size);

/*
 * Walk the central directory, calling `visit` once per member. Returning false
 * from `visit` stops the walk successfully — that is how a caller finds one
 * member without enumerating the rest.
 */
typedef bool (*vmfw_zip_visit_fn)(void *ctx, const vmfw_zip_entry_t *entry,
                                  uint32_t index);
vmfw_zip_status_t vmfw_zip_iterate(const vmfw_zip_t *zip,
                                   vmfw_zip_visit_fn visit, void *visit_ctx);

/* Find one member by exact name. */
vmfw_zip_status_t vmfw_zip_find(const vmfw_zip_t *zip, const char *name,
                                vmfw_zip_entry_t *out);

/*
 * Extract a member into `sink`. The CRC-32 in the central directory is
 * recomputed over what was produced and a mismatch is an error: it is the only
 * end-to-end check that the bytes handed onward are the bytes Apple shipped,
 * and it costs one pass we are making anyway.
 */
vmfw_zip_status_t vmfw_zip_extract(const vmfw_zip_t *zip,
                                   const vmfw_zip_entry_t *entry,
                                   vmfw_sink_fn sink, void *sink_ctx);

uint32_t vmfw_crc32(uint32_t crc, const uint8_t *data, size_t len);

const char *vmfw_zip_strerror(vmfw_zip_status_t st);

/* ------------------------------------------------------------------------ */
/* Apple disk images: `encrcdsa` and UDIF                                    */
/* ------------------------------------------------------------------------ */
/*
 * The root filesystem member is an AES-128 encrypted wrapper around a UDIF
 * disk image which is itself a table of compressed chunks. Two layers, and the
 * emulator wants one partition out of the middle of the result.
 *
 * The wrapper is decrypted lazily rather than in a pass of its own. Every
 * 4096-byte block carries its own IV derived from its index, so the plaintext
 * is randomly addressable, and the chunk table wants random access anyway.
 * Fusing them saves writing a 208 MB intermediate on a phone.
 */
typedef enum {
    VMFW_DMG_OK = 0,
    VMFW_DMG_ERR_INVALID_ARGUMENT,
    VMFW_DMG_ERR_READ,
    VMFW_DMG_ERR_NOT_ENCRCDSA,
    VMFW_DMG_ERR_ENCRCDSA_VERSION,   /* not version 2                       */
    VMFW_DMG_ERR_BAD_BLOCK_SIZE,
    VMFW_DMG_ERR_BAD_GEOMETRY,       /* declared extent does not fit        */
    VMFW_DMG_ERR_KEY_REQUIRED,       /* encrypted, and no key was supplied  */
    VMFW_DMG_ERR_BAD_KEY_LENGTH,     /* not the 36-byte AES+HMAC blob       */
    VMFW_DMG_ERR_WRONG_KEY,          /* decrypted, and it is not a UDIF     */
    VMFW_DMG_ERR_NO_KOLY,            /* no trailer at end-of-image          */
    VMFW_DMG_ERR_BAD_KOLY,
    VMFW_DMG_ERR_XML,                /* the resource fork will not parse    */
    VMFW_DMG_ERR_NO_PARTITION,       /* the requested blkx is not present   */
    VMFW_DMG_ERR_BAD_BLKX,
    VMFW_DMG_ERR_BAD_CHUNK,          /* a chunk's extent is impossible      */
    VMFW_DMG_ERR_CHUNK_TYPE,         /* a compression we do not implement   */
    VMFW_DMG_ERR_CHUNK_SIZE,         /* a chunk produced the wrong length   */
    VMFW_DMG_ERR_INFLATE,
    VMFW_DMG_ERR_SINK,
    VMFW_DMG_ERR_TOO_MANY_CHUNKS,
    VMFW_DMG_ERR_SCRATCH_TOO_SMALL,
    VMFW_DMG_ERR_NOT_CONTIGUOUS   /* chunks do not tile the partition in order */
} vmfw_dmg_status_t;

#define VMFW_DMG_KEY_BLOB_SIZE 36u   /* 16-byte AES-128 key || 20-byte HMAC */
#define VMFW_DMG_SECTOR_SIZE  512u

/*
 * A reader over one disk image, transparently decrypting if the image is
 * wrapped. `key` may be NULL for an already-plain UDIF; an `encrcdsa` image
 * with a NULL key is refused with VMFW_DMG_ERR_KEY_REQUIRED, which is the
 * message the user needs rather than a spinner that never finishes.
 */
typedef struct {
    vmfw_pread_fn pread;
    void         *ctx;
    uint64_t      file_size;
    bool          encrypted;
    uint64_t      data_offset;    /* where ciphertext begins                */
    uint64_t      plain_size;     /* logical size of the UDIF inside        */
    uint32_t      block_size;
    uint8_t       aes_key[16];
    uint8_t       hmac_key[20];
} vmfw_dmg_reader_t;

vmfw_dmg_status_t vmfw_dmg_reader_open(vmfw_dmg_reader_t *r,
                                       vmfw_pread_fn pread, void *ctx,
                                       uint64_t file_size,
                                       const uint8_t *key, size_t key_len);

/* Read plaintext bytes [offset, offset+len) of the image inside the wrapper. */
vmfw_dmg_status_t vmfw_dmg_reader_pread(const vmfw_dmg_reader_t *r,
                                        uint64_t offset, uint8_t *buf,
                                        size_t len);

/* What a UDIF says about itself, once the trailer and resource fork parse. */
#define VMFW_DMG_MAX_PARTITIONS 32u
#define VMFW_DMG_MAX_NAME 128u

typedef struct {
    char     name[VMFW_DMG_MAX_NAME];
    uint64_t first_sector;
    uint64_t sector_count;
    uint64_t blkx_data_offset;  /* the mish header's own dataOffset field    */
    /* Where this partition's still-encoded base64 lives inside the XML the
     * caller passed to vmfw_dmg_probe. Kept as a span rather than decoded
     * bytes because the largest table is 66 KB and only one is ever needed. */
    size_t   b64_offset;
    size_t   b64_length;
    uint32_t chunk_count;
} vmfw_dmg_partition_t;

typedef struct {
    uint64_t data_fork_offset;
    uint64_t data_fork_length;
    uint64_t xml_offset;
    uint64_t xml_length;
    uint64_t sector_count;
    uint32_t partition_count;
    vmfw_dmg_partition_t partitions[VMFW_DMG_MAX_PARTITIONS];
} vmfw_dmg_info_t;

/*
 * Parse the trailer and the resource fork. `scratch` holds the decoded XML and
 * must be at least `xml_length` bytes; the required size is reported through
 * VMFW_DMG_ERR_INVALID_ARGUMENT with *needed set, so a caller can size it in
 * one round trip rather than guessing.
 */
vmfw_dmg_status_t vmfw_dmg_probe(const vmfw_dmg_reader_t *r,
                                 uint8_t *scratch, size_t scratch_cap,
                                 size_t *needed, vmfw_dmg_info_t *out);

/*
 * Expand one partition to a raw image. `partition_index` selects a blkx entry;
 * vmfw_dmg_find_partition resolves a substring like "Apple_HFSX" to one.
 *
 * `chunk_scratch` holds the DECODED chunk table, which needs
 * 0xCC + 40 * chunk_count bytes -- 66,564 for a 7E18 root filesystem. It does
 * not have to hold a chunk's data: compressed chunks are streamed straight out
 * of the image, so a 433 MB partition costs no more memory than a 4 KB one.
 * Too small a buffer is VMFW_DMG_ERR_SCRATCH_TOO_SMALL, never a short read.
 *
 * Output is produced strictly in sector order, which is only possible because
 * the chunks tile the partition without gaps or overlap. That is a property of
 * every image Apple's own tools write, but it is a property of the FILE, so it
 * is checked: an image whose chunks disagree with their partition is refused
 * with VMFW_DMG_ERR_NOT_CONTIGUOUS rather than silently producing a raw image
 * with a hole in the middle of a filesystem.
 */
vmfw_dmg_status_t vmfw_dmg_find_partition(const vmfw_dmg_info_t *info,
                                          const char *needle,
                                          uint32_t *out_index);

vmfw_dmg_status_t vmfw_dmg_extract_partition(const vmfw_dmg_reader_t *r,
                                             const vmfw_dmg_info_t *info,
                                             const uint8_t *xml,
                                             size_t xml_len,
                                             uint32_t partition_index,
                                             uint8_t *chunk_scratch,
                                             size_t chunk_scratch_cap,
                                             vmfw_sink_fn sink, void *sink_ctx,
                                             uint64_t *out_bytes);

const char *vmfw_dmg_strerror(vmfw_dmg_status_t st);

#endif /* S5LBOX_VM_FIRMWARE_FORMATS_H */
