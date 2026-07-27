/*
 * S5LBox -- allocation-free SHA-1 and HMAC-SHA1 (FIPS 180-1, RFC 2104).
 *
 * This authenticates nothing, and no later change may make it look as though
 * it does. An Apple `encrcdsa` disk image cuts its ciphertext into 4096-byte
 * blocks and derives each block's AES-CBC IV from the block's index:
 *
 *     IV(n) = HMAC-SHA1(hmac_key, big-endian uint32 n) truncated to 16 bytes
 *
 * so every byte produced here is key schedule material feeding a decryption
 * that something else has to judge. SHA-1 is the wrong instrument for judging:
 * it has had practical collisions since 2017, and a digest from this file must
 * never decide that an artefact is the artefact we wanted. tools/sha256.c is
 * the only thing in this project that makes that call. Code that starts
 * comparing a value computed here against a published hash is a bug however
 * reasonable the surrounding lines look.
 *
 * The contract is deliberately tools/sha256.c's, because the two are a pair and
 * a caller should not have to re-learn the rules moving between them:
 *
 *   - nothing allocates, so nothing can fail halfway through;
 *   - no mutable static state, because the firmware import runs off the main
 *     thread and two imports may be in flight at once;
 *   - an update that is refused leaves the entire context unchanged, so a
 *     caller that checks the return value can carry on with the same context
 *     and get the digest it would have got had it never made the bad call;
 *   - finalization happens exactly once;
 *   - the cumulative length is capped so the 64-bit bit count written into the
 *     padding cannot wrap and silently produce the digest of a different
 *     length of message.
 *
 * Plain C11 with no platform headers, so a host CI runner tests every line.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareFormats.h"

#include <string.h>

/*
 * The padding carries the message length in bits as a 64-bit big-endian value,
 * so a message of more than floor(UINT64_MAX / 8) bytes could not be described
 * by it. Refusing at that bound is what keeps the multiplication in
 * vmfw_sha1_final from wrapping; a wrapped count would hash cleanly and give
 * the digest of some other message length. This is the same bound and the same
 * reason as IOS3_SHA256_MAX_INPUT_BYTES.
 */
#define VMFW_SHA1_MAX_INPUT_BYTES (UINT64_MAX / UINT64_C(8))

static uint32_t rotate_left(uint32_t value, unsigned count) {
    /* Every call site below passes 1, 5 or 30. A count of 0 or 32 would shift
     * a 32-bit value by its own width, which is undefined, so this must not be
     * given one -- the same unchecked precondition as tools/sha256.c's
     * rotate_right, kept unchecked for the same reason: a branch here would run
     * eighty times per block to guard against a call that does not exist. */
    return (value << count) | (value >> (32u - count));
}

static uint32_t load_be32(const uint8_t bytes[4]) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void store_be32(uint8_t bytes[4], uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void transform(vmfw_sha1_ctx_t *ctx,
                      const uint8_t block[VMFW_SHA1_BLOCK_SIZE]) {
    uint32_t words[80];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    size_t index;

    /* The block is read big-endian regardless of the host, which is why this
     * cannot just alias the caller's bytes as uint32_t: on the ARM64 phone and
     * the x86 CI runner alike that would hash the bytes in the wrong order. */
    for (index = 0u; index < 16u; index++)
        words[index] = load_be32(block + index * 4u);
    /* FIPS 180-1 differs from SHA-0 in exactly this rotate. Without it the
     * function still runs and still produces 20 plausible bytes -- it is just
     * not SHA-1, and the only thing that catches it is a published vector. */
    for (index = 16u; index < 80u; index++)
        words[index] = rotate_left(words[index - 3u] ^ words[index - 8u] ^
                                   words[index - 14u] ^ words[index - 16u], 1u);

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    for (index = 0u; index < 80u; index++) {
        uint32_t mix;
        uint32_t konst;
        uint32_t temp;

        if (index < 20u) {
            mix = (b & c) | ((~b) & d);
            konst = UINT32_C(0x5a827999);
        } else if (index < 40u) {
            mix = b ^ c ^ d;
            konst = UINT32_C(0x6ed9eba1);
        } else if (index < 60u) {
            mix = (b & c) | (b & d) | (c & d);
            konst = UINT32_C(0x8f1bbcdc);
        } else {
            mix = b ^ c ^ d;
            konst = UINT32_C(0xca62c1d6);
        }
        /* Every term is uint32_t, so the sum wraps modulo 2^32 by definition
         * rather than by luck; nothing here is ever a signed int. */
        temp = rotate_left(a, 5u) + mix + e + konst + words[index];
        e = d;
        d = c;
        c = rotate_left(b, 30u);
        b = a;
        a = temp;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

bool vmfw_sha1_init(vmfw_sha1_ctx_t *ctx) {
    if (!ctx)
        return false;
    /* Zero the whole struct, not just the named fields: the tail of `block`
     * and any padding then hold a defined value, so a test may compare two
     * contexts byte for byte and mean it. */
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = UINT32_C(0x67452301);
    ctx->state[1] = UINT32_C(0xefcdab89);
    ctx->state[2] = UINT32_C(0x98badcfe);
    ctx->state[3] = UINT32_C(0x10325476);
    ctx->state[4] = UINT32_C(0xc3d2e1f0);
    return true;
}

bool vmfw_sha1_update(vmfw_sha1_ctx_t *ctx, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t amount;

    /*
     * Every reason to refuse is decided before a single byte of the context is
     * touched. That is the whole of the "a rejected update changes nothing"
     * rule: a caller that hands over a NULL buffer, notices the false and
     * retries with the right pointer must land on the digest it would have got
     * without the mistake, not on a context that already counted the bytes it
     * never read.
     *
     * `block_used >= VMFW_SHA1_BLOCK_SIZE` and a total already past the cap are
     * impossible for a context this file produced; they are checked because a
     * caller owns the struct and can memcpy anything into it, and continuing
     * from either would write past the end of `block`.
     */
    if (!ctx || (len != 0u && !data) || ctx->finalized ||
        ctx->block_used >= VMFW_SHA1_BLOCK_SIZE ||
        ctx->total_bytes > VMFW_SHA1_MAX_INPUT_BYTES)
        return false;
#if SIZE_MAX > (UINT64_MAX / UINT64_C(8))
    /* Only reachable where size_t is wider than the cap; on a 64-bit host this
     * whole branch would be dead code and a comparison the compiler warns is
     * always false. */
    if (len > (size_t)VMFW_SHA1_MAX_INPUT_BYTES)
        return false;
#endif
    amount = (uint64_t)len;
    /* Written as a subtraction from the cap so the check itself cannot wrap. */
    if (amount > VMFW_SHA1_MAX_INPUT_BYTES - ctx->total_bytes)
        return false;
    if (len == 0u)
        return true;

    ctx->total_bytes += amount;
    if (ctx->block_used != 0u) {
        size_t available = VMFW_SHA1_BLOCK_SIZE - ctx->block_used;
        size_t take = len < available ? len : available;

        memcpy(ctx->block + ctx->block_used, bytes, take);
        ctx->block_used += take;
        bytes += take;
        len -= take;
        if (ctx->block_used == VMFW_SHA1_BLOCK_SIZE) {
            transform(ctx, ctx->block);
            ctx->block_used = 0u;
        }
    }
    /* Whole blocks are hashed straight out of the caller's buffer; the copy
     * into `block` is only ever for the ragged head and tail. */
    while (len >= VMFW_SHA1_BLOCK_SIZE) {
        transform(ctx, bytes);
        bytes += VMFW_SHA1_BLOCK_SIZE;
        len -= VMFW_SHA1_BLOCK_SIZE;
    }
    if (len != 0u) {
        memcpy(ctx->block, bytes, len);
        ctx->block_used = len;
    }
    return true;
}

bool vmfw_sha1_final(vmfw_sha1_ctx_t *ctx, uint8_t out[VMFW_SHA1_DIGEST_SIZE]) {
    uint64_t total_bits;
    size_t index;

    /* `finalized` is what makes a second call fail rather than hash the
     * padding a second time and hand back a digest of something that was never
     * a message. */
    if (!ctx || !out || ctx->finalized ||
        ctx->block_used >= VMFW_SHA1_BLOCK_SIZE ||
        ctx->total_bytes > VMFW_SHA1_MAX_INPUT_BYTES)
        return false;

    /* Safe because update refused anything that would push total_bytes past
     * UINT64_MAX / 8. */
    total_bits = ctx->total_bytes * UINT64_C(8);
    ctx->block[ctx->block_used++] = UINT8_C(0x80);
    /* block_used is at most 64 here, so if the length no longer fits in the
     * last eight bytes the partial block is flushed and the count goes into a
     * block of its own. */
    if (ctx->block_used > 56u) {
        memset(ctx->block + ctx->block_used, 0,
               VMFW_SHA1_BLOCK_SIZE - ctx->block_used);
        transform(ctx, ctx->block);
        ctx->block_used = 0u;
    }
    memset(ctx->block + ctx->block_used, 0, 56u - ctx->block_used);
    for (index = 0u; index < 8u; index++)
        ctx->block[56u + index] =
            (uint8_t)(total_bits >> (56u - (unsigned)index * 8u));
    transform(ctx, ctx->block);
    for (index = 0u; index < 5u; index++)
        store_be32(out + index * 4u, ctx->state[index]);
    /* Clear the residue of the last block. Best effort only -- a compiler is
     * free to elide a memset over storage that is never read again -- and it is
     * hygiene rather than a defence, because nothing this file computes is a
     * secret in the first place. */
    memset(ctx->block, 0, sizeof(ctx->block));
    ctx->block_used = 0u;
    ctx->finalized = true;
    return true;
}

bool vmfw_sha1(const void *data, size_t len, uint8_t out[VMFW_SHA1_DIGEST_SIZE]) {
    vmfw_sha1_ctx_t ctx;

    if (!out || (len != 0u && !data))
        return false;
    return vmfw_sha1_init(&ctx) &&
           vmfw_sha1_update(&ctx, data, len) &&
           vmfw_sha1_final(&ctx, out);
}

bool vmfw_hmac_sha1(const uint8_t *key, size_t key_len,
                    const uint8_t *msg, size_t msg_len,
                    uint8_t out[VMFW_SHA1_DIGEST_SIZE]) {
    vmfw_sha1_ctx_t ctx;
    uint8_t block_key[VMFW_SHA1_BLOCK_SIZE];
    uint8_t pad[VMFW_SHA1_BLOCK_SIZE];
    uint8_t inner[VMFW_SHA1_DIGEST_SIZE];
    size_t index;
    bool ok;

    /* Same NULL rule as the hash: a NULL pointer is a description of an empty
     * string only when the length agrees. An `encrcdsa` image with no HMAC key
     * material is a real case (key_len 0), and RFC 2104's padding defines it
     * exactly: the key is zero-extended to the block size like any other. */
    if (!out || (key_len != 0u && !key) || (msg_len != 0u && !msg))
        return false;

    /*
     * RFC 2104 step 0. A key longer than the block size is replaced by its own
     * digest -- not truncated, and not folded. Getting this wrong costs nothing
     * visible: the code still produces 20 bytes, and only the two RFC 2202
     * vectors with 80-byte keys ever notice.
     */
    memset(block_key, 0, sizeof(block_key));
    if (key_len > VMFW_SHA1_BLOCK_SIZE) {
        if (!vmfw_sha1(key, key_len, block_key))
            return false;
    } else if (key_len != 0u) {
        memcpy(block_key, key, key_len);
    }

    for (index = 0u; index < VMFW_SHA1_BLOCK_SIZE; index++)
        pad[index] = (uint8_t)(block_key[index] ^ UINT8_C(0x36));
    ok = vmfw_sha1_init(&ctx) &&
         vmfw_sha1_update(&ctx, pad, sizeof(pad)) &&
         vmfw_sha1_update(&ctx, msg, msg_len) &&
         vmfw_sha1_final(&ctx, inner);

    if (ok) {
        for (index = 0u; index < VMFW_SHA1_BLOCK_SIZE; index++)
            pad[index] = (uint8_t)(block_key[index] ^ UINT8_C(0x5c));
        ok = vmfw_sha1_init(&ctx) &&
             vmfw_sha1_update(&ctx, pad, sizeof(pad)) &&
             vmfw_sha1_update(&ctx, inner, sizeof(inner)) &&
             vmfw_sha1_final(&ctx, out);
    }
    /* On failure `out` is left holding zeroes rather than whatever the outer
     * pass got as far as writing, so a caller that ignores the return value
     * gets an obviously wrong IV instead of a half-computed one. */
    if (!ok)
        memset(out, 0, VMFW_SHA1_DIGEST_SIZE);

    /* Again best effort: the padded key and the inner digest are derived from
     * the caller's key, and there is no reason to leave them on the stack for
     * the next frame to inherit. */
    memset(block_key, 0, sizeof(block_key));
    memset(pad, 0, sizeof(pad));
    memset(inner, 0, sizeof(inner));
    memset(&ctx, 0, sizeof(ctx));
    return ok;
}
