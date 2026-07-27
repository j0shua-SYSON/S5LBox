/*
 * S5LBox — SHA-1 and HMAC-SHA1 tests.
 *
 * The digest itself is checked against published vectors and nothing else,
 * because a hash that is wrong is wrong in a way that no amount of internal
 * consistency reveals: an implementation missing FIPS 180-1's extra rotate is
 * SHA-0, and it will still stream, still finalize, still agree with itself
 * across chunk boundaries, and still hand back twenty confident bytes. So the
 * three FIPS 180-1 vectors, the empty message, and all seven of RFC 2202's
 * HMAC cases are here verbatim, including the two whose keys are longer than
 * the block and must therefore be hashed first.
 *
 * The rest of the file tests the contract rather than the arithmetic, and the
 * part worth the most is the refusal path. This runs against an IPSW the user
 * found on the internet, so a caller will occasionally pass a NULL span or a
 * length it did not mean; what matters is that the context is then exactly
 * what it was before the bad call, so retrying gets the right answer instead
 * of a digest of a message length that never existed.
 *
 * Nothing here decides whether an artefact is genuine — see VMFirmwareDigest.c
 * on why SHA-1 must never be asked that — so these are correctness tests for
 * key schedule material, and the last section hashes exactly what the
 * `encrcdsa` block reader will: HMAC-SHA1(key, big-endian block index), cut to
 * the 16 bytes an AES-CBC IV takes.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareTest.h"
#include "VMFirmwareFormats.h"

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Digests are compared as hex text rather than with memcmp so that a failure
 * prints both digests. "20 bytes differ" tells you nothing about which vector
 * is broken; a pair of hex strings usually tells you immediately whether the
 * length coding, the byte order or the rounds are at fault.
 */
static void expect_digest(vmfw_test_t *t,
                          const uint8_t got[VMFW_SHA1_DIGEST_SIZE],
                          const char *want_hex, const char *what) {
    static const char HEX[] = "0123456789abcdef";
    char got_hex[VMFW_SHA1_DIGEST_SIZE * 2u + 1u];
    size_t i;

    for (i = 0u; i < VMFW_SHA1_DIGEST_SIZE; i++) {
        got_hex[i * 2u] = HEX[(got[i] >> 4) & 0x0fu];
        got_hex[i * 2u + 1u] = HEX[got[i] & 0x0fu];
    }
    got_hex[VMFW_SHA1_DIGEST_SIZE * 2u] = '\0';
    VMFW_T_CHECK(t, strcmp(got_hex, want_hex) == 0, "%s: got %s want %s",
                 what, got_hex, want_hex);
}

/* Hash `len` bytes handing them over in the chunk sizes named by `sizes`,
 * cycling through the list. The awkward sizes are the point: 1 exercises the
 * partial-block path on every call, 63/65/127 straddle the block boundary in
 * both directions, and 64 hits it exactly. */
static bool hash_in_chunks(const uint8_t *data, size_t len,
                           const size_t *sizes, size_t sizes_count,
                           uint8_t out[VMFW_SHA1_DIGEST_SIZE]) {
    vmfw_sha1_ctx_t ctx;
    size_t offset = 0u;
    size_t which = 0u;

    if (!vmfw_sha1_init(&ctx))
        return false;
    while (offset < len) {
        size_t take = sizes[which % sizes_count];
        if (take > len - offset)
            take = len - offset;
        if (!vmfw_sha1_update(&ctx, data + offset, take))
            return false;
        offset += take;
        which++;
    }
    return vmfw_sha1_final(&ctx, out);
}

/* A deterministic filler, so a failure is reproducible without a data file.
 * This is a test fixture and not a random number generator; it only has to be
 * the same sequence on every host. */
static void fill_pattern(uint8_t *buf, size_t len, uint32_t seed) {
    size_t i;
    uint32_t state = seed;

    for (i = 0u; i < len; i++) {
        state = state * UINT32_C(1103515245) + UINT32_C(12345);
        buf[i] = (uint8_t)(state >> 16);
    }
}

static void store_be32_test(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

/* ------------------------------------------------------------------------ */
/* FIPS 180-1 / RFC 3174 vectors                                             */
/* ------------------------------------------------------------------------ */

static void test_published_sha1_vectors(vmfw_test_t *t) {
    static const char TWO_BLOCK[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    uint8_t digest[VMFW_SHA1_DIGEST_SIZE];
    uint8_t chunk[1000];
    vmfw_sha1_ctx_t ctx;
    size_t i;

    VMFW_T_SECTION(t, "sha1 published vectors");

    /* The empty message is the one case where the padding block is the only
     * block, so it is the check that catches a length field written into the
     * wrong end of it. */
    VMFW_T_CHECK(t, vmfw_sha1("", 0u, digest), "empty message refused");
    expect_digest(t, digest, "da39a3ee5e6b4b0d3255bfef95601890afd80709",
                  "sha1(\"\")");

    /* A NULL pointer describes the empty message too, when the length agrees. */
    VMFW_T_CHECK(t, vmfw_sha1(NULL, 0u, digest), "NULL/0 refused");
    expect_digest(t, digest, "da39a3ee5e6b4b0d3255bfef95601890afd80709",
                  "sha1(NULL, 0)");

    VMFW_T_CHECK(t, vmfw_sha1("abc", 3u, digest), "\"abc\" refused");
    expect_digest(t, digest, "a9993e364706816aba3e25717850c26c9cd0d89d",
                  "sha1(\"abc\")");

    /* 56 characters: one byte of message past the point where the length no
     * longer fits beside it, so finalization must emit a second block. Off by
     * one here and every message of 56..63 bytes is wrong and nothing else is. */
    VMFW_T_EQ_U(t, strlen(TWO_BLOCK), 56u, "two-block vector length");
    VMFW_T_CHECK(t, vmfw_sha1(TWO_BLOCK, strlen(TWO_BLOCK), digest),
                 "two-block vector refused");
    expect_digest(t, digest, "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
                  "sha1(56-char string)");

    /* One million 'a', streamed in 1000-byte pieces. Building a megabyte on
     * the stack would overflow the 512 KB thread this eventually runs on, and
     * the streaming is the interesting half of the vector anyway. */
    memset(chunk, 'a', sizeof(chunk));
    VMFW_T_CHECK(t, vmfw_sha1_init(&ctx), "init refused before 1M vector");
    for (i = 0u; i < 1000u; i++) {
        if (!vmfw_sha1_update(&ctx, chunk, sizeof(chunk))) {
            VMFW_T_CHECK(t, false, "1M vector update refused at chunk %zu", i);
            break;
        }
    }
    VMFW_T_CHECK(t, vmfw_sha1_final(&ctx, digest), "1M vector final refused");
    expect_digest(t, digest, "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
                  "sha1(1000000 * 'a')");

    /* The same million bytes again, but handed over in sizes that never line
     * up with the block, which is how the buffered path gets exercised a
     * million times rather than a thousand. */
    {
        static const size_t SIZES[] = { 1u, 63u, 64u, 65u, 127u };
        size_t offset = 0u;
        size_t which = 0u;
        bool ok = vmfw_sha1_init(&ctx);

        while (ok && offset < 1000000u) {
            size_t take = SIZES[which % 5u];
            if (take > 1000000u - offset)
                take = 1000000u - offset;
            ok = vmfw_sha1_update(&ctx, chunk, take);
            offset += take;
            which++;
        }
        VMFW_T_CHECK(t, ok, "1M vector refused mid-stream in awkward chunks");
        VMFW_T_CHECK(t, vmfw_sha1_final(&ctx, digest),
                     "1M awkward-chunk final refused");
        expect_digest(t, digest, "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
                      "sha1(1000000 * 'a') streamed in 1/63/64/65/127 chunks");
    }
}

/* ------------------------------------------------------------------------ */
/* Chunking must not change the answer                                       */
/* ------------------------------------------------------------------------ */

static void test_incremental_matches_one_shot(vmfw_test_t *t) {
    static const size_t SIZES[5] = { 1u, 63u, 64u, 65u, 127u };
    uint8_t data[701];
    uint8_t one_shot[VMFW_SHA1_DIGEST_SIZE];
    uint8_t streamed[VMFW_SHA1_DIGEST_SIZE];
    vmfw_sha1_ctx_t ctx;
    size_t i;

    VMFW_T_SECTION(t, "sha1 incremental");

    fill_pattern(data, sizeof(data), UINT32_C(0x5f00d));
    VMFW_T_CHECK(t, vmfw_sha1(data, sizeof(data), one_shot),
                 "one-shot over the pattern refused");

    /* Each chunk size on its own... */
    for (i = 0u; i < 5u; i++) {
        VMFW_T_CHECK(t, hash_in_chunks(data, sizeof(data), &SIZES[i], 1u,
                                       streamed),
                     "streaming in %zu-byte chunks refused", SIZES[i]);
        VMFW_T_EQ_MEM(t, streamed, one_shot, VMFW_SHA1_DIGEST_SIZE,
                      "streamed digest differs from one-shot");
    }
    /* ...and all of them cycling, which is closer to what a chunked disk image
     * actually feeds in. */
    VMFW_T_CHECK(t, hash_in_chunks(data, sizeof(data), SIZES, 5u, streamed),
                 "streaming in cycling chunk sizes refused");
    VMFW_T_EQ_MEM(t, streamed, one_shot, VMFW_SHA1_DIGEST_SIZE,
                  "cycling-chunk digest differs from one-shot");

    /* Zero-length updates are accepted and must be invisible: a source that
     * returns 0 bytes without being at end of input is a real occurrence, and
     * it must not disturb the running state. */
    VMFW_T_CHECK(t, vmfw_sha1_init(&ctx), "init refused");
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, NULL, 0u), "leading NULL/0 refused");
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, data, 100u), "update refused");
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, data, 0u), "interior data/0 refused");
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, NULL, 0u), "interior NULL/0 refused");
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, data + 100u, sizeof(data) - 100u),
                 "tail update refused");
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, NULL, 0u), "trailing NULL/0 refused");
    VMFW_T_CHECK(t, vmfw_sha1_final(&ctx, streamed), "final refused");
    VMFW_T_EQ_MEM(t, streamed, one_shot, VMFW_SHA1_DIGEST_SIZE,
                  "zero-length updates changed the digest");
}

/* ------------------------------------------------------------------------ */
/* Refusals                                                                  */
/* ------------------------------------------------------------------------ */

static void test_refusals(vmfw_test_t *t) {
    uint8_t digest[VMFW_SHA1_DIGEST_SIZE];
    uint8_t again[VMFW_SHA1_DIGEST_SIZE];
    uint8_t saved[sizeof(vmfw_sha1_ctx_t)];
    uint8_t data[200];
    uint8_t reference[VMFW_SHA1_DIGEST_SIZE];
    vmfw_sha1_ctx_t ctx;

    VMFW_T_SECTION(t, "sha1 refusals");

    VMFW_T_CHECK(t, !vmfw_sha1_init(NULL), "init accepted a NULL context");
    VMFW_T_CHECK(t, !vmfw_sha1_update(NULL, "x", 1u),
                 "update accepted a NULL context");
    VMFW_T_CHECK(t, !vmfw_sha1_final(NULL, digest),
                 "final accepted a NULL context");
    VMFW_T_CHECK(t, !vmfw_sha1(NULL, 1u, digest),
                 "one-shot accepted NULL with a non-zero length");
    VMFW_T_CHECK(t, !vmfw_sha1("x", 1u, NULL),
                 "one-shot accepted a NULL output");

    /*
     * The rule that matters. After a refused update the context must be the
     * bytes it was before, so finishing the message gives the published digest
     * and not the digest of 5 phantom bytes. Both halves are checked: the raw
     * struct (init memsets it, so padding compares equal) and the digest,
     * because a context could be restored in the fields the test knows about
     * and still be wrong in one it does not.
     */
    VMFW_T_CHECK(t, vmfw_sha1_init(&ctx), "init refused");
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, "abc", 3u), "update refused");
    memcpy(saved, &ctx, sizeof(saved));
    VMFW_T_CHECK(t, !vmfw_sha1_update(&ctx, NULL, 5u),
                 "update accepted NULL with a non-zero length");
    VMFW_T_EQ_MEM(t, &ctx, saved, sizeof(saved),
                  "a refused update modified the context");
    VMFW_T_CHECK(t, vmfw_sha1_final(&ctx, digest), "final refused");
    expect_digest(t, digest, "a9993e364706816aba3e25717850c26c9cd0d89d",
                  "sha1(\"abc\") after a refused update");

    /* The same, with data on both sides of the refusal and a longer message,
     * compared against the digest the same input gets in one call. */
    fill_pattern(data, sizeof(data), UINT32_C(0xa5a5));
    VMFW_T_CHECK(t, vmfw_sha1(data, sizeof(data), reference),
                 "reference one-shot refused");
    VMFW_T_CHECK(t, vmfw_sha1_init(&ctx), "init refused");
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, data, 130u), "update refused");
    VMFW_T_CHECK(t, !vmfw_sha1_update(&ctx, NULL, 64u),
                 "update accepted NULL with a non-zero length");
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, data + 130u, sizeof(data) - 130u),
                 "update after a refusal was itself refused");
    VMFW_T_CHECK(t, vmfw_sha1_final(&ctx, digest), "final refused");
    VMFW_T_EQ_MEM(t, digest, reference, VMFW_SHA1_DIGEST_SIZE,
                  "a refused mid-stream update changed the result");

    /* A refused final must not consume the one finalization the caller has. */
    VMFW_T_CHECK(t, vmfw_sha1_init(&ctx), "init refused");
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, "abc", 3u), "update refused");
    VMFW_T_CHECK(t, !vmfw_sha1_final(&ctx, NULL),
                 "final accepted a NULL digest buffer");
    VMFW_T_CHECK(t, vmfw_sha1_final(&ctx, digest),
                 "final refused after a NULL-output refusal consumed it");
    expect_digest(t, digest, "a9993e364706816aba3e25717850c26c9cd0d89d",
                  "sha1(\"abc\") after a refused final");

    /* Second final and update-after-final. Hashing the padding a second time
     * would return twenty bytes that are the digest of nothing anybody sent. */
    memset(again, 0xee, sizeof(again));
    VMFW_T_CHECK(t, !vmfw_sha1_final(&ctx, again), "final succeeded twice");
    VMFW_T_CHECK(t, again[0] == 0xeeu,
                 "a refused second final still wrote to the buffer");
    VMFW_T_CHECK(t, !vmfw_sha1_update(&ctx, "x", 1u),
                 "update accepted after final");
    VMFW_T_CHECK(t, !vmfw_sha1_update(&ctx, NULL, 0u),
                 "zero-length update accepted after final");

    /*
     * A context the caller corrupted. Nothing this file produces can have
     * block_used == 64, but the struct is the caller's and a stale or memcpy'd
     * one can say anything; continuing from it would write past `block`.
     */
    VMFW_T_CHECK(t, vmfw_sha1_init(&ctx), "init refused");
    ctx.block_used = VMFW_SHA1_BLOCK_SIZE;
    VMFW_T_CHECK(t, !vmfw_sha1_update(&ctx, "x", 1u),
                 "update accepted a full-block context");
    VMFW_T_CHECK(t, !vmfw_sha1_final(&ctx, digest),
                 "final accepted a full-block context");
}

/* ------------------------------------------------------------------------ */
/* The length cap                                                            */
/* ------------------------------------------------------------------------ */

static void test_length_cap(vmfw_test_t *t) {
    /* The bound VMFirmwareDigest.c enforces, written out here rather than
     * exported, so that changing it in the module without meaning to shows up
     * as a failure. */
    const uint64_t cap = UINT64_MAX / UINT64_C(8);
    uint8_t saved[sizeof(vmfw_sha1_ctx_t)];
    uint8_t digest[VMFW_SHA1_DIGEST_SIZE];
    vmfw_sha1_ctx_t ctx;

    VMFW_T_SECTION(t, "sha1 length cap");

    /*
     * No test can feed 2^61 bytes, so the counter is set directly. That is
     * legitimate here because the struct is public and a caller resuming a
     * long hash owns exactly these fields. What is being checked is that the
     * bit-count multiplication in final can never be reached with a value that
     * would wrap it.
     */
    VMFW_T_CHECK(t, vmfw_sha1_init(&ctx), "init refused");
    ctx.total_bytes = cap;
    memcpy(saved, &ctx, sizeof(saved));
    VMFW_T_CHECK(t, !vmfw_sha1_update(&ctx, "x", 1u),
                 "update accepted a byte past the cap");
    VMFW_T_EQ_MEM(t, &ctx, saved, sizeof(saved),
                  "the refusal at the cap modified the context");
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, NULL, 0u),
                 "a zero-length update at the cap was refused");
    VMFW_T_CHECK(t, vmfw_sha1_final(&ctx, digest),
                 "final refused a context sitting exactly at the cap");

    /* One byte below the cap: exactly one more byte fits, and then no more. */
    VMFW_T_CHECK(t, vmfw_sha1_init(&ctx), "init refused");
    ctx.total_bytes = cap - UINT64_C(1);
    VMFW_T_CHECK(t, vmfw_sha1_update(&ctx, "x", 1u),
                 "the last byte under the cap was refused");
    VMFW_T_EQ_U(t, ctx.total_bytes, cap, "total after the last byte");
    VMFW_T_CHECK(t, !vmfw_sha1_update(&ctx, "y", 1u),
                 "update accepted a byte past the cap");

    /* A context already past the cap is refused outright rather than trusted. */
    VMFW_T_CHECK(t, vmfw_sha1_init(&ctx), "init refused");
    ctx.total_bytes = cap + UINT64_C(1);
    VMFW_T_CHECK(t, !vmfw_sha1_update(&ctx, "x", 1u),
                 "update accepted a context already past the cap");
    VMFW_T_CHECK(t, !vmfw_sha1_final(&ctx, digest),
                 "final accepted a context already past the cap");
}

/* ------------------------------------------------------------------------ */
/* RFC 2202                                                                  */
/* ------------------------------------------------------------------------ */

static void test_rfc2202_hmac(vmfw_test_t *t) {
    uint8_t key[80];
    uint8_t data[73];
    uint8_t mac[VMFW_SHA1_DIGEST_SIZE];
    size_t i;

    VMFW_T_SECTION(t, "hmac rfc 2202");

    /* Case 1: 20-byte key, short data. */
    memset(key, 0x0b, 20u);
    VMFW_T_CHECK(t, vmfw_hmac_sha1(key, 20u, (const uint8_t *)"Hi There", 8u,
                                   mac), "case 1 refused");
    expect_digest(t, mac, "b617318655057264e28bc0b6fb378c8ef146be00",
                  "RFC 2202 case 1");

    /* Case 2: a 4-byte key, far shorter than the block, so the zero padding
     * has to be right. */
    VMFW_T_CHECK(t, vmfw_hmac_sha1((const uint8_t *)"Jefe", 4u,
                                   (const uint8_t *)
                                   "what do ya want for nothing?", 28u, mac),
                 "case 2 refused");
    expect_digest(t, mac, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79",
                  "RFC 2202 case 2");

    /* Case 3: 50 bytes of data, so the inner hash spans two blocks. */
    memset(key, 0xaa, 20u);
    memset(data, 0xdd, 50u);
    VMFW_T_CHECK(t, vmfw_hmac_sha1(key, 20u, data, 50u, mac),
                 "case 3 refused");
    expect_digest(t, mac, "125d7342b9ac11cd91a39af48aa17b4f63f175d3",
                  "RFC 2202 case 3");

    /* Case 4: a 25-byte key of ascending bytes, which catches a padding loop
     * that stopped at the key length instead of the block length. */
    for (i = 0u; i < 25u; i++)
        key[i] = (uint8_t)(i + 1u);
    memset(data, 0xcd, 50u);
    VMFW_T_CHECK(t, vmfw_hmac_sha1(key, 25u, data, 50u, mac),
                 "case 4 refused");
    expect_digest(t, mac, "4c9007f4026250c6bc8414f9bf50c86c2d7235da",
                  "RFC 2202 case 4");

    /* Case 5, and the 96-bit truncation the RFC also publishes. The encrcdsa
     * reader truncates to 128 bits, so it is worth stating once that a
     * truncated HMAC is a prefix of the full one and not a separate value. */
    memset(key, 0x0c, 20u);
    VMFW_T_CHECK(t, vmfw_hmac_sha1(key, 20u,
                                   (const uint8_t *)"Test With Truncation", 20u,
                                   mac), "case 5 refused");
    expect_digest(t, mac, "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04",
                  "RFC 2202 case 5");
    {
        static const uint8_t WANT96[12] = {
            0x4c, 0x1a, 0x03, 0x42, 0x4b, 0x55,
            0xe0, 0x7f, 0xe7, 0xf2, 0x7b, 0xe1
        };
        VMFW_T_EQ_MEM(t, mac, WANT96, sizeof(WANT96),
                      "RFC 2202 case 5 truncated to 96 bits");
    }

    /*
     * Cases 6 and 7: 80-byte keys, longer than the 64-byte block, so RFC 2104
     * replaces the key with its own SHA-1 before padding. An implementation
     * that truncated to 64 bytes instead passes every other case in this file.
     */
    memset(key, 0xaa, 80u);
    VMFW_T_CHECK(t, vmfw_hmac_sha1(key, 80u, (const uint8_t *)
                                   "Test Using Larger Than Block-Size Key - "
                                   "Hash Key First", 54u, mac),
                 "case 6 refused");
    expect_digest(t, mac, "aa4ae5e15272d00e95705637ce8a3b55ed402112",
                  "RFC 2202 case 6 (80-byte key)");

    memcpy(data, "Test Using Larger Than Block-Size Key and Larger Than One "
                 "Block-Size Data", 73u);
    VMFW_T_CHECK(t, vmfw_hmac_sha1(key, 80u, data, 73u, mac),
                 "case 7 refused");
    expect_digest(t, mac, "e8e99d0f45237d786d6bbaa7965c7808bbff1a91",
                  "RFC 2202 case 7 (80-byte key, 73-byte data)");

    /* A key of exactly one block is neither padded nor re-hashed; 65 bytes is
     * the first length that is re-hashed. Nothing published covers the
     * boundary, so it is pinned against the equivalent constructions instead:
     * a 64-byte key is itself, and a 65-byte key is SHA-1 of itself. */
    {
        uint8_t boundary[65];
        uint8_t hashed_key[VMFW_SHA1_DIGEST_SIZE];
        uint8_t direct[VMFW_SHA1_DIGEST_SIZE];
        uint8_t via_digest[VMFW_SHA1_DIGEST_SIZE];

        fill_pattern(boundary, sizeof(boundary), UINT32_C(0x0ff1ce));
        VMFW_T_CHECK(t, vmfw_hmac_sha1(boundary, 65u, (const uint8_t *)"m", 1u,
                                       direct), "65-byte key refused");
        VMFW_T_CHECK(t, vmfw_sha1(boundary, 65u, hashed_key),
                     "hashing the long key refused");
        VMFW_T_CHECK(t, vmfw_hmac_sha1(hashed_key, VMFW_SHA1_DIGEST_SIZE,
                                       (const uint8_t *)"m", 1u, via_digest),
                     "pre-hashed key refused");
        VMFW_T_EQ_MEM(t, direct, via_digest, VMFW_SHA1_DIGEST_SIZE,
                      "a 65-byte key was not replaced by its own digest");

        /* And a 64-byte key must NOT be re-hashed: if it were, this would
         * accidentally agree with the pre-hashed form above. */
        VMFW_T_CHECK(t, vmfw_hmac_sha1(boundary, 64u, (const uint8_t *)"m", 1u,
                                       direct), "64-byte key refused");
        VMFW_T_CHECK(t, vmfw_sha1(boundary, 64u, hashed_key),
                     "hashing the block-size key refused");
        VMFW_T_CHECK(t, vmfw_hmac_sha1(hashed_key, VMFW_SHA1_DIGEST_SIZE,
                                       (const uint8_t *)"m", 1u, via_digest),
                     "pre-hashed key refused");
        VMFW_T_CHECK(t, memcmp(direct, via_digest,
                               VMFW_SHA1_DIGEST_SIZE) != 0,
                     "a key of exactly one block was re-hashed");
    }
}

static void test_hmac_edges(vmfw_test_t *t) {
    uint8_t mac[VMFW_SHA1_DIGEST_SIZE];
    uint8_t mac_zero_key[VMFW_SHA1_DIGEST_SIZE];
    uint8_t zeros[VMFW_SHA1_BLOCK_SIZE];

    VMFW_T_SECTION(t, "hmac edges");

    /* An empty key is not an error: RFC 2104's padding zero-extends whatever
     * it is given, so a zero-length key must equal a key of 64 zero bytes.
     * Refusing it here would turn an image with no HMAC material into an
     * import failure instead of an import. */
    memset(zeros, 0, sizeof(zeros));
    VMFW_T_CHECK(t, vmfw_hmac_sha1(NULL, 0u,
                                   (const uint8_t *)"zero key message", 16u,
                                   mac), "empty key refused");
    expect_digest(t, mac, "8ebed575981ca5e6dd352e583acdc453188822a5",
                  "hmac with a zero-length key");
    VMFW_T_CHECK(t, vmfw_hmac_sha1(zeros, sizeof(zeros),
                                   (const uint8_t *)"zero key message", 16u,
                                   mac_zero_key), "64 zero-byte key refused");
    VMFW_T_EQ_MEM(t, mac, mac_zero_key, VMFW_SHA1_DIGEST_SIZE,
                  "an empty key differs from 64 zero bytes");

    /* An empty message is likewise legal and distinct from an empty key. */
    VMFW_T_CHECK(t, vmfw_hmac_sha1((const uint8_t *)"k", 1u, NULL, 0u, mac),
                 "empty message refused");

    /* The refusals. */
    VMFW_T_CHECK(t, !vmfw_hmac_sha1(NULL, 1u, (const uint8_t *)"m", 1u, mac),
                 "accepted a NULL key with a non-zero key length");
    VMFW_T_CHECK(t, !vmfw_hmac_sha1((const uint8_t *)"k", 1u, NULL, 1u, mac),
                 "accepted a NULL message with a non-zero length");
    VMFW_T_CHECK(t, !vmfw_hmac_sha1((const uint8_t *)"k", 1u,
                                    (const uint8_t *)"m", 1u, NULL),
                 "accepted a NULL output buffer");
}

/* ------------------------------------------------------------------------ */
/* What this is actually for                                                 */
/* ------------------------------------------------------------------------ */

static void test_encrcdsa_iv_derivation(vmfw_test_t *t) {
    /*
     * The only consumer. An `encrcdsa` image derives block n's AES-CBC IV as
     * HMAC-SHA1(hmac_key, big-endian uint32 n), keeping the first 16 bytes and
     * discarding the last 4.
     *
     * The key below is twenty ASCII characters invented for this test. It is
     * not a firmware key, no firmware key appears in this repository, and the
     * digests underneath are only interesting as a record that the byte order
     * of the index and the length of the truncation have not drifted. Feeding
     * the index little-endian produces perfectly good IVs that decrypt
     * nothing, and the failure looks like a corrupt disk image rather than a
     * wrong IV.
     */
    static const uint8_t TEST_KEY[] = {
        'S', '5', 'L', 'B', 'o', 'x', ' ', 't', 'e', 's',
        't', ' ', 'v', 'e', 'c', 't', 'o', 'r', '!', '!'
    };
    static const struct {
        uint32_t index;
        const char *iv_hex;   /* the 16 bytes actually used            */
        const char *full_hex; /* the whole 20, to pin the truncation   */
    } CASES[] = {
        { 0u,          "c78b135db7c8750eef67705d8e366e33",
                       "c78b135db7c8750eef67705d8e366e33f70de070" },
        { 1u,          "f94e77dbe0bc084ab3e2c30dd6606a63",
                       "f94e77dbe0bc084ab3e2c30dd6606a63cc7da724" },
        { 1000u,       "4b656497cf11cdb38f34969c2facaac3",
                       "4b656497cf11cdb38f34969c2facaac32bc3d983" },
        /* An index whose four bytes are all different, so a byte order slip
         * cannot look like a match. */
        { 0x00010203u, "a904fbeb6bda8adf1279fd669b8f96f0",
                       "a904fbeb6bda8adf1279fd669b8f96f0ae58463f" }
    };
    uint8_t counter[4];
    uint8_t mac[VMFW_SHA1_DIGEST_SIZE];
    uint8_t iv[16];
    size_t i;

    VMFW_T_SECTION(t, "encrcdsa iv derivation");

    /* The key is 20 bytes because that is the size of the HMAC half of an
     * encrcdsa key blob; if that ever stops being true the reader changes too. */
    VMFW_T_EQ_U(t, sizeof(TEST_KEY), VMFW_SHA1_DIGEST_SIZE,
                "test key length matches the encrcdsa hmac key length");

    for (i = 0u; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        char label[64];

        store_be32_test(counter, CASES[i].index);
        VMFW_T_CHECK(t, vmfw_hmac_sha1(TEST_KEY, sizeof(TEST_KEY), counter,
                                       sizeof(counter), mac),
                     "iv derivation refused for index %lu",
                     (unsigned long)CASES[i].index);
        snprintf(label, sizeof(label), "iv for block %lu",
                 (unsigned long)CASES[i].index);
        expect_digest(t, mac, CASES[i].full_hex, label);

        memcpy(iv, mac, sizeof(iv));
        {
            static const char HEX[] = "0123456789abcdef";
            char iv_hex[sizeof(iv) * 2u + 1u];
            size_t j;

            for (j = 0u; j < sizeof(iv); j++) {
                iv_hex[j * 2u] = HEX[(iv[j] >> 4) & 0x0fu];
                iv_hex[j * 2u + 1u] = HEX[iv[j] & 0x0fu];
            }
            iv_hex[sizeof(iv) * 2u] = '\0';
            VMFW_T_EQ_STR(t, iv_hex, CASES[i].iv_hex, "truncated iv");
        }
    }

    /* Consecutive blocks must not share an IV — the whole point of deriving
     * one per block — and the check costs nothing. */
    {
        uint8_t iv0[VMFW_SHA1_DIGEST_SIZE];
        uint8_t iv1[VMFW_SHA1_DIGEST_SIZE];

        store_be32_test(counter, 0u);
        VMFW_T_CHECK(t, vmfw_hmac_sha1(TEST_KEY, sizeof(TEST_KEY), counter, 4u,
                                       iv0), "iv 0 refused");
        store_be32_test(counter, 1u);
        VMFW_T_CHECK(t, vmfw_hmac_sha1(TEST_KEY, sizeof(TEST_KEY), counter, 4u,
                                       iv1), "iv 1 refused");
        VMFW_T_CHECK(t, memcmp(iv0, iv1, 16u) != 0,
                     "two consecutive blocks derived the same iv");
    }
}

/* ------------------------------------------------------------------------ */

void vmfw_test_digest(vmfw_test_t *t) {
    test_published_sha1_vectors(t);
    test_incremental_matches_one_shot(t);
    test_refusals(t);
    test_length_cap(t);
    test_rfc2202_hmac(t);
    test_hmac_edges(t);
    test_encrcdsa_iv_derivation(t);
}
