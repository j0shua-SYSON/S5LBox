/*
 * S5LBox — DEFLATE (RFC 1951) and zlib (RFC 1950) expansion.
 *
 * This is the widest untrusted-input surface in the import path: 43 of the 49
 * members of a 7E18 IPSW and 1,622 of the root filesystem's UDIF chunks arrive
 * here, and every length, distance and code length in them is a number nobody
 * checked. So the same rules as core/src/firmware/img3.c apply, plus two that
 * are particular to a decompressor:
 *
 *   - nothing allocates, and the whole decoder is one 40 KB struct the caller
 *     owns, so there is no dynamic state to leak and no static state to make
 *     this unsafe to run off the main thread;
 *   - output is bounded twice over. `expect_out` stops production at exactly
 *     the size the container declared, and a back-reference is checked against
 *     the number of bytes actually produced rather than against how full the
 *     32 KiB window looks.
 *
 * The second one is the bug this file exists to not have. After the first
 * 32 KiB the window is always full, so a decoder that validates a distance
 * against the window instead of against the output accepts a reference to
 * bytes that were never produced, and hands the caller whatever the window
 * happened to contain. Here that is VMFW_INFLATE_ERR_BAD_DISTANCE.
 *
 * Neither the input nor the output is ever held whole: bytes are pulled from a
 * source in 4 KiB bites and pushed to a sink 32 KiB at a time, because the
 * largest thing this decodes is a 433 MB filesystem on a phone.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareFormats.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* Sources and sinks                                                         */
/* ------------------------------------------------------------------------ */

void vmfw_mem_source_init(vmfw_mem_source_t *s, const uint8_t *data, size_t len) {
    if (!s) return;
    s->data = data;
    /* A NULL buffer with a nonzero length is a caller bug that would otherwise
     * turn into a memcpy from NULL on the first read; make it an empty source
     * so the decoder reports a truncated stream instead. */
    s->len  = data ? len : 0u;
    s->pos  = 0u;
}

size_t vmfw_mem_source_read(void *ctx, uint8_t *buf, size_t cap) {
    vmfw_mem_source_t *s = (vmfw_mem_source_t *)ctx;
    if (!s || !buf) return VMFW_SOURCE_ERROR;
    if (s->pos > s->len) return VMFW_SOURCE_ERROR;   /* invariant broken */

    size_t left = s->len - s->pos;
    size_t take = left < cap ? left : cap;
    if (take) memcpy(buf, s->data + s->pos, take);
    s->pos += take;
    return take;
}

void vmfw_mem_sink_init(vmfw_mem_sink_t *s, uint8_t *data, size_t cap) {
    if (!s) return;
    s->data       = data;
    s->cap        = data ? cap : 0u;
    s->len        = 0u;
    s->overflowed = false;
}

bool vmfw_mem_sink_write(void *ctx, const uint8_t *data, size_t len) {
    vmfw_mem_sink_t *s = (vmfw_mem_sink_t *)ctx;
    if (!s) return false;
    if (len == 0u) return true;
    if (!data) return false;

    /*
     * Written as a subtraction against the space left rather than as
     * `s->len + len > s->cap`, because `len` comes from a decoder driven by
     * attacker-controlled input: the addition can wrap on a 64-bit host and
     * the comparison would then pass for a length that runs off the end.
     *
     * Nothing is copied on refusal. A partial write followed by false would
     * leave the caller unable to say how much of its buffer is real.
     */
    if (s->len > s->cap || len > s->cap - s->len) {
        s->overflowed = true;
        return false;
    }
    memcpy(s->data + s->len, data, len);
    s->len += len;
    return true;
}

bool vmfw_null_sink_write(void *ctx, const uint8_t *data, size_t len) {
    vmfw_null_sink_t *s = (vmfw_null_sink_t *)ctx;
    (void)data;
    if (!s) return false;
    s->len += (uint64_t)len;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Decoder state                                                             */
/* ------------------------------------------------------------------------ */

#define VMFW_WINDOW_BITS 15u
#define VMFW_WINDOW_SIZE (1u << VMFW_WINDOW_BITS)   /* RFC 1951's 32 KiB */
#define VMFW_WINDOW_MASK (VMFW_WINDOW_SIZE - 1u)
#define VMFW_IN_BUF      4096u
#define VMFW_MAX_BITS    15u    /* longest legal Huffman code            */
#define VMFW_MAX_LIT     288u   /* 0..285 defined, 286 and 287 reserved  */
#define VMFW_MAX_DIST    32u    /* 0..29 defined, 30 and 31 reserved     */
#define VMFW_MAX_CLEN    19u    /* code-length alphabet                  */

typedef struct {
    uint16_t count[VMFW_MAX_BITS + 1u];   /* codes of each length     */
    uint16_t symbol[VMFW_MAX_LIT];        /* symbols in code order    */
    uint16_t coded;                       /* symbols with a code word */
} vmfw_huff_t;

typedef struct {
    vmfw_source_fn source;
    void          *source_ctx;
    vmfw_sink_fn   sink;
    void          *sink_ctx;

    uint64_t       limit;      /* expect_out; production stops here        */
    uint64_t       produced;   /* bytes handed to emit, flushed or not     */

    uint32_t       adler_a;
    uint32_t       adler_b;

    uint32_t       bitbuf;     /* holds at most 23 bits; see need_bits     */
    unsigned       bitcnt;

    size_t         in_len;
    size_t         in_pos;
    bool           in_eof;

    uint32_t       wpos;       /* next write index in `window`             */

    vmfw_inflate_status_t status;

    uint8_t        in[VMFW_IN_BUF];
    uint8_t        window[VMFW_WINDOW_SIZE];
    uint8_t        lengths[VMFW_MAX_LIT + VMFW_MAX_DIST];

    vmfw_huff_t    lencode;
    vmfw_huff_t    distcode;
    vmfw_huff_t    clcode;
} vmfw_state_t;

/* RFC 1951 section 3.2.5, verbatim. Kept as tables rather than derived,
 * because the two irregularities (symbol 284 stops one short of 258 and
 * symbol 285 codes 258 with no extra bits) are exactly what a derivation
 * gets wrong. */
static const uint16_t VMFW_LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t VMFW_LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t VMFW_DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t VMFW_DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
/* The order the code-length code's own lengths appear in, which puts the
 * lengths most likely to be zero last so HCLEN can cut them off. */
static const uint8_t VMFW_CLEN_ORDER[VMFW_MAX_CLEN] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ------------------------------------------------------------------------ */
/* Output                                                                    */
/* ------------------------------------------------------------------------ */

static void adler_update(vmfw_state_t *st, const uint8_t *p, size_t n) {
    uint32_t a = st->adler_a, b = st->adler_b;
    while (n) {
        /* 5552 is the longest run of 0xff bytes that cannot push `b` past
         * 2^32; taking the modulus every byte instead would cost a division
         * per byte of a 433 MB image. */
        size_t k = n < 5552u ? n : 5552u;
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521u;
        b %= 65521u;
    }
    st->adler_a = a;
    st->adler_b = b;
}

/*
 * Hand the window's unflushed prefix to the sink and start it again at zero.
 *
 * The window is both the history a back-reference reads and the staging buffer
 * output leaves through, so this is only ever called with the window exactly
 * full (where resetting wpos to 0 lands back on the ring's own wrap point and
 * the history stays addressable) or once at the very end, after which nothing
 * reads the history again.
 */
static bool window_flush(vmfw_state_t *st) {
    if (st->wpos == 0u) return true;
    adler_update(st, st->window, st->wpos);
    if (!st->sink(st->sink_ctx, st->window, st->wpos)) {
        st->status = VMFW_INFLATE_ERR_SINK;
        return false;
    }
    st->wpos = 0u;
    return true;
}

static bool emit_byte(vmfw_state_t *st, uint8_t b) {
    if (st->produced >= st->limit) {
        /* The container said how big this is. A stream that wants to write
         * past that is not the member it claims to be, and truncating it
         * silently would hand the caller a file that looks whole. */
        st->status = VMFW_INFLATE_ERR_SIZE_MISMATCH;
        return false;
    }
    st->window[st->wpos++] = b;
    st->produced++;
    if (st->wpos == VMFW_WINDOW_SIZE) return window_flush(st);
    return true;
}

static bool emit_run(vmfw_state_t *st, const uint8_t *p, size_t n) {
    while (n) {
        if (st->produced >= st->limit) {
            st->status = VMFW_INFLATE_ERR_SIZE_MISMATCH;
            return false;
        }
        uint64_t room  = st->limit - st->produced;
        size_t   chunk = (size_t)(VMFW_WINDOW_SIZE - st->wpos);
        if (chunk > n) chunk = n;
        if ((uint64_t)chunk > room) chunk = (size_t)room;

        memcpy(st->window + st->wpos, p, chunk);
        st->wpos     += (uint32_t)chunk;
        st->produced += (uint64_t)chunk;
        p            += chunk;
        n            -= chunk;

        if (st->wpos == VMFW_WINDOW_SIZE && !window_flush(st)) return false;
    }
    return true;
}

static bool emit_match(vmfw_state_t *st, unsigned length, unsigned distance) {
    /*
     * Against `produced`, not against the window. The window holds the last
     * 32768 bytes whatever happens, so from the second window onward it is
     * always "full" and a fill-based check is no check at all; only the count
     * of bytes actually produced says whether these bytes ever existed.
     */
    if ((uint64_t)distance > st->produced) {
        st->status = VMFW_INFLATE_ERR_BAD_DISTANCE;
        return false;
    }

    uint32_t from = (st->wpos - distance) & VMFW_WINDOW_MASK;
    while (length--) {
        /* Copied a byte at a time on purpose: DEFLATE's run-length idiom is a
         * match that overlaps its own output (distance 1, length 258), so the
         * source must see bytes this loop is still writing. */
        if (!emit_byte(st, st->window[from])) return false;
        from = (from + 1u) & VMFW_WINDOW_MASK;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Input                                                                     */
/* ------------------------------------------------------------------------ */

static int next_byte(vmfw_state_t *st) {
    if (st->in_pos >= st->in_len) {
        if (st->in_eof) {
            st->status = VMFW_INFLATE_ERR_TRUNCATED;
            return -1;
        }
        size_t n = st->source(st->source_ctx, st->in, (size_t)VMFW_IN_BUF);
        if (n == VMFW_SOURCE_ERROR) {
            /* Distinct from end of input on purpose: a read that failed and a
             * stream that ended are the same length of nothing, and only one
             * of them means the file is wrong. */
            st->status = VMFW_INFLATE_ERR_READ;
            return -1;
        }
        if (n > (size_t)VMFW_IN_BUF) {
            /* A source claiming to have written more than the cap it was
             * given; believing it would read uninitialised buffer. */
            st->status = VMFW_INFLATE_ERR_READ;
            return -1;
        }
        if (n == 0u) {
            st->in_eof = true;
            st->status = VMFW_INFLATE_ERR_TRUNCATED;
            return -1;
        }
        st->in_len = n;
        st->in_pos = 0u;
    }
    return (int)st->in[st->in_pos++];
}

/* Never called with more than 16, so bitcnt stays below 24 and bitbuf cannot
 * overflow 32 bits. */
static bool need_bits(vmfw_state_t *st, unsigned n) {
    while (st->bitcnt < n) {
        int c = next_byte(st);
        if (c < 0) return false;
        st->bitbuf |= (uint32_t)c << st->bitcnt;
        st->bitcnt += 8u;
    }
    return true;
}

static uint32_t take_bits(vmfw_state_t *st, unsigned n) {
    uint32_t v = st->bitbuf & ((1u << n) - 1u);
    st->bitbuf >>= n;
    st->bitcnt  -= n;
    return v;
}

static bool get_bits(vmfw_state_t *st, unsigned n, uint32_t *out) {
    if (!need_bits(st, n)) return false;
    *out = take_bits(st, n);
    return true;
}

static void align_to_byte(vmfw_state_t *st) {
    (void)take_bits(st, st->bitcnt & 7u);
}

/*
 * One byte of the stream, valid only once align_to_byte has been called: the
 * whole bytes still sitting in the bit buffer were already taken out of the
 * input buffer, so they have to come back out first or the caller would skip
 * up to three bytes of the stream.
 */
static int aligned_byte(vmfw_state_t *st) {
    if (st->bitcnt >= 8u) return (int)take_bits(st, 8u);
    return next_byte(st);
}

/* ------------------------------------------------------------------------ */
/* Huffman codes                                                             */
/* ------------------------------------------------------------------------ */

/*
 * Build the canonical code described by `lengths`.
 *
 * `allow_incomplete` is true only for a distance code. RFC 1951 does not say
 * a code must use all of its code space, and zlib accepts two incomplete
 * distance codes that real streams produce: none at all (a block with no
 * matches) and a single one-bit code (a block that uses exactly one distance,
 * where spending a whole bit on the alternative would be waste). Any other
 * incomplete code, and any incomplete literal/length code, is refused: the
 * undefined half of the code space has no meaning to guess at, and an
 * incomplete literal/length code cannot even encode end-of-block.
 */
static vmfw_inflate_status_t huff_build(vmfw_huff_t *h, const uint8_t *lengths,
                                        unsigned n, bool allow_incomplete) {
    memset(h->count, 0, sizeof h->count);
    h->coded = 0u;

    unsigned maxlen = 0u;
    for (unsigned s = 0; s < n; s++) {
        if (lengths[s] > VMFW_MAX_BITS) return VMFW_INFLATE_ERR_BAD_HUFFMAN;
        h->count[lengths[s]]++;
        if (lengths[s] > maxlen) maxlen = lengths[s];
    }
    unsigned coded = n - (unsigned)h->count[0];

    /*
     * Kraft's inequality, walked one length at a time. `left` is how much of
     * the code space is still unclaimed after the codes of each length; going
     * negative means two symbols were given the same code word, which is the
     * over-subscription that makes a decoder's tables ambiguous.
     */
    int left = 1;
    for (unsigned len = 1u; len <= VMFW_MAX_BITS; len++) {
        left <<= 1;
        left  -= (int)h->count[len];
        if (left < 0) return VMFW_INFLATE_ERR_BAD_HUFFMAN;
    }
    if (left > 0 && !(allow_incomplete && maxlen <= 1u))
        return VMFW_INFLATE_ERR_BAD_HUFFMAN;

    /* Symbols sorted by (code length, symbol), which is what makes the code
     * canonical and lets huff_decode find a symbol by counting. */
    uint16_t offs[VMFW_MAX_BITS + 2u];
    memset(offs, 0, sizeof offs);
    for (unsigned len = 1u; len <= VMFW_MAX_BITS; len++)
        offs[len + 1u] = (uint16_t)(offs[len] + h->count[len]);
    for (unsigned s = 0; s < n; s++)
        if (lengths[s]) h->symbol[offs[lengths[s]]++] = (uint16_t)s;

    h->coded = (uint16_t)coded;
    return VMFW_INFLATE_OK;
}

/*
 * Read one code word and return its symbol, or -1 with st->status set.
 *
 * Bit at a time up the code lengths, comparing against the first code of each
 * length. `code >= first` holds throughout: failing to match at a length means
 * code >= first + count, and both sides then double.
 */
static int huff_decode(vmfw_state_t *st, const vmfw_huff_t *h) {
    if (h->coded == 0u) {
        /* A code with no symbols defines no code words. Reaching it means the
         * stream used a distance in a block whose header said it would use
         * none. */
        st->status = VMFW_INFLATE_ERR_BAD_HUFFMAN;
        return -1;
    }

    unsigned code = 0u, first = 0u, idx = 0u;
    for (unsigned len = 1u; len <= VMFW_MAX_BITS; len++) {
        if (!need_bits(st, 1u)) return -1;
        code |= take_bits(st, 1u);
        unsigned count = h->count[len];
        if (code - first < count) return (int)h->symbol[idx + (code - first)];
        idx   += count;
        first  = (first + count) << 1;
        code <<= 1;
    }

    /* Fifteen bits and no match: the stream used a code word this code does
     * not define. The single-symbol distance code allowed above has exactly
     * one such half, and this is where using it lands. */
    st->status = VMFW_INFLATE_ERR_BAD_HUFFMAN;
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Blocks                                                                    */
/* ------------------------------------------------------------------------ */

static vmfw_inflate_status_t build_fixed(vmfw_state_t *st) {
    /* RFC 1951 section 3.2.6. The distance code has 32 five-bit codes even
     * though only 30 mean anything, so 30 and 31 are decodable and have to be
     * refused as symbols rather than as code words. */
    uint8_t *l = st->lengths;
    unsigned s = 0u;
    for (; s < 144u; s++) l[s] = 8u;
    for (; s < 256u; s++) l[s] = 9u;
    for (; s < 280u; s++) l[s] = 7u;
    for (; s < 288u; s++) l[s] = 8u;

    vmfw_inflate_status_t rc = huff_build(&st->lencode, l, 288u, false);
    if (rc != VMFW_INFLATE_OK) return rc;

    for (s = 0u; s < 32u; s++) l[s] = 5u;
    return huff_build(&st->distcode, l, 32u, false);
}

static vmfw_inflate_status_t read_dynamic(vmfw_state_t *st) {
    uint32_t v;
    if (!get_bits(st, 5u, &v)) return st->status;
    unsigned nlen = (unsigned)v + 257u;
    if (!get_bits(st, 5u, &v)) return st->status;
    unsigned ndist = (unsigned)v + 1u;
    if (!get_bits(st, 4u, &v)) return st->status;
    unsigned ncode = (unsigned)v + 4u;

    /* HLIT and HDIST can name 288 length and 32 distance symbols, but RFC 1951
     * defines only 286 and 30. Refusing the header rather than waiting to see
     * whether the extra symbols get used is what zlib does, and it means the
     * decode loop never has to consider a length symbol it has no base for. */
    if (nlen > 286u || ndist > 30u) return VMFW_INFLATE_ERR_BAD_SYMBOL;

    uint8_t *l = st->lengths;
    memset(l, 0, (size_t)VMFW_MAX_CLEN);
    for (unsigned i = 0; i < ncode; i++) {
        if (!get_bits(st, 3u, &v)) return st->status;
        l[VMFW_CLEN_ORDER[i]] = (uint8_t)v;
    }
    /* The code-length code gets no incomplete-code licence: it is not a
     * distance code, and the whole header is unreadable without it. */
    vmfw_inflate_status_t rc = huff_build(&st->clcode, l, VMFW_MAX_CLEN, false);
    if (rc != VMFW_INFLATE_OK) return rc;

    /* Safe to reuse `l`: huff_build has already copied everything it needs. */
    unsigned total = nlen + ndist;
    unsigned i = 0u;
    while (i < total) {
        int sym = huff_decode(st, &st->clcode);
        if (sym < 0) return st->status;

        if (sym < 16) { l[i++] = (uint8_t)sym; continue; }

        unsigned repeat;
        uint8_t  value = 0u;
        if (sym == 16) {
            /* Repeat the previous length. With nothing before it there is no
             * previous length to repeat, and reading l[-1] is the bug. */
            if (i == 0u) return VMFW_INFLATE_ERR_BAD_HUFFMAN;
            value = l[i - 1u];
            if (!get_bits(st, 2u, &v)) return st->status;
            repeat = 3u + (unsigned)v;
        } else if (sym == 17) {
            if (!get_bits(st, 3u, &v)) return st->status;
            repeat = 3u + (unsigned)v;
        } else {
            if (!get_bits(st, 7u, &v)) return st->status;
            repeat = 11u + (unsigned)v;
        }
        /* A repeat that runs past the declared count would write lengths for
         * symbols the header never claimed, off the end of `lengths`. */
        if (repeat > total - i) return VMFW_INFLATE_ERR_BAD_HUFFMAN;
        while (repeat--) l[i++] = value;
    }

    rc = huff_build(&st->lencode, l, nlen, false);
    if (rc != VMFW_INFLATE_OK) return rc;
    return huff_build(&st->distcode, l + nlen, ndist, true);
}

static vmfw_inflate_status_t stored_block(vmfw_state_t *st) {
    align_to_byte(st);

    uint32_t len, nlen;
    if (!get_bits(st, 16u, &len))  return st->status;
    if (!get_bits(st, 16u, &nlen)) return st->status;
    /* NLEN is LEN's ones' complement. It is the only integrity check a stored
     * block has, so a disagreement means the bit position is wrong and every
     * byte after it would be garbage presented as data. */
    if (len != ((~nlen) & 0xffffu)) return VMFW_INFLATE_ERR_BAD_STORED;

    size_t remaining = (size_t)len;

    /* Bytes the bit buffer swallowed while reading LEN and NLEN come first. */
    while (remaining && st->bitcnt >= 8u) {
        if (!emit_byte(st, (uint8_t)take_bits(st, 8u))) return st->status;
        remaining--;
    }
    while (remaining) {
        if (st->in_pos >= st->in_len) {
            int c = next_byte(st);      /* refills, or says why it cannot */
            if (c < 0) return st->status;
            if (!emit_byte(st, (uint8_t)c)) return st->status;
            remaining--;
            continue;
        }
        size_t chunk = st->in_len - st->in_pos;
        if (chunk > remaining) chunk = remaining;
        if (!emit_run(st, st->in + st->in_pos, chunk)) return st->status;
        st->in_pos += chunk;
        remaining  -= chunk;
    }
    return VMFW_INFLATE_OK;
}

static vmfw_inflate_status_t coded_block(vmfw_state_t *st) {
    for (;;) {
        int sym = huff_decode(st, &st->lencode);
        if (sym < 0) return st->status;

        if (sym < 256) {
            if (!emit_byte(st, (uint8_t)sym)) return st->status;
            continue;
        }
        if (sym == 256) return VMFW_INFLATE_OK;

        sym -= 257;
        /* Symbols 286 and 287 have code words in the fixed literal/length code
         * but no length assigned to them anywhere in RFC 1951. */
        if (sym >= 29) return VMFW_INFLATE_ERR_BAD_SYMBOL;

        uint32_t v;
        if (!get_bits(st, VMFW_LEN_EXTRA[sym], &v)) return st->status;
        unsigned length = (unsigned)VMFW_LEN_BASE[sym] + (unsigned)v;

        int dsym = huff_decode(st, &st->distcode);
        if (dsym < 0) return st->status;
        /* Likewise 30 and 31 in the fixed distance code. */
        if (dsym >= 30) return VMFW_INFLATE_ERR_BAD_SYMBOL;

        if (!get_bits(st, VMFW_DIST_EXTRA[dsym], &v)) return st->status;
        unsigned distance = (unsigned)VMFW_DIST_BASE[dsym] + (unsigned)v;

        if (!emit_match(st, length, distance)) return st->status;
    }
}

static vmfw_inflate_status_t inflate_blocks(vmfw_state_t *st) {
    for (;;) {
        uint32_t v;
        if (!get_bits(st, 1u, &v)) return st->status;
        bool final_block = (v != 0u);
        if (!get_bits(st, 2u, &v)) return st->status;

        vmfw_inflate_status_t rc;
        switch (v) {
            case 0u:
                rc = stored_block(st);
                break;
            case 1u:
                rc = build_fixed(st);
                if (rc == VMFW_INFLATE_OK) rc = coded_block(st);
                break;
            case 2u:
                rc = read_dynamic(st);
                if (rc == VMFW_INFLATE_OK) rc = coded_block(st);
                break;
            default:
                /* Type 3 is reserved. There is no reading of it that is not a
                 * guess, and a guess here writes bytes into a kernel image. */
                return VMFW_INFLATE_ERR_BAD_BLOCK;
        }
        if (rc != VMFW_INFLATE_OK) return rc;
        if (final_block) return VMFW_INFLATE_OK;
    }
}

/* ------------------------------------------------------------------------ */
/* Entry points                                                              */
/* ------------------------------------------------------------------------ */

static vmfw_inflate_status_t zlib_header(vmfw_state_t *st) {
    int cmf = next_byte(st);
    if (cmf < 0) return st->status;
    int flg = next_byte(st);
    if (flg < 0) return st->status;

    /*
     * RFC 1950 section 2.2. CM 8 is DEFLATE and the only method ever defined;
     * CINFO above 7 asks for a window larger than the 32 KiB this decoder (and
     * the format) has; the two bytes together must be a multiple of 31; and
     * FDICT means the stream was compressed against a dictionary that is not
     * in the file, so nothing here could expand it correctly.
     */
    unsigned cm    = (unsigned)cmf & 0x0fu;
    unsigned cinfo = ((unsigned)cmf >> 4) & 0x0fu;
    unsigned fdict = ((unsigned)flg >> 5) & 1u;
    if (cm != 8u || cinfo > 7u || fdict != 0u ||
        ((unsigned)cmf * 256u + (unsigned)flg) % 31u != 0u)
        return VMFW_INFLATE_ERR_BAD_ZLIB_HEADER;

    return VMFW_INFLATE_OK;
}

static vmfw_inflate_status_t zlib_trailer(vmfw_state_t *st) {
    align_to_byte(st);

    uint32_t want = 0u;
    for (unsigned i = 0; i < 4u; i++) {
        int c = aligned_byte(st);
        if (c < 0) return st->status;
        want = (want << 8) | (uint32_t)c;    /* big-endian, unlike everything
                                              * else in the format */
    }

    /*
     * An Adler-32 that does not match reports VMFW_INFLATE_ERR_BAD_ZLIB_HEADER
     * because the enum has no separate status for it and this file does not
     * get to add one. The two mean the same thing to a caller: the bytes
     * claimed to be a zlib stream and the claim did not hold, so whatever came
     * out of the sink must not be used. Distinguishing which end of the
     * wrapper failed would only matter to someone debugging the compressor.
     */
    if (want != ((st->adler_b << 16) | st->adler_a))
        return VMFW_INFLATE_ERR_BAD_ZLIB_HEADER;
    return VMFW_INFLATE_OK;
}

static vmfw_inflate_status_t inflate_run(vmfw_state_t *st, bool zlib_wrapped) {
    if (zlib_wrapped) {
        vmfw_inflate_status_t rc = zlib_header(st);
        if (rc != VMFW_INFLATE_OK) return rc;
    }

    vmfw_inflate_status_t rc = inflate_blocks(st);
    if (rc != VMFW_INFLATE_OK) return rc;
    if (!window_flush(st)) return st->status;

    if (zlib_wrapped) {
        rc = zlib_trailer(st);
        if (rc != VMFW_INFLATE_OK) return rc;
    }

    /* The stream ended cleanly but short of what the container promised. A
     * caller that took the sink's bytes on an OK would be taking a file with a
     * hole in it. */
    if (st->limit != VMFW_INFLATE_NO_EXPECTATION && st->produced != st->limit)
        return VMFW_INFLATE_ERR_SIZE_MISMATCH;

    return VMFW_INFLATE_OK;
}

static vmfw_inflate_status_t inflate_entry(vmfw_source_fn source,
                                           void *source_ctx,
                                           vmfw_sink_fn sink, void *sink_ctx,
                                           uint64_t expect_out,
                                           uint64_t *out_produced,
                                           bool zlib_wrapped) {
    if (out_produced) *out_produced = 0u;
    if (!source || !sink) return VMFW_INFLATE_ERR_INVALID_ARGUMENT;

    /*
     * Forty kilobytes of automatic storage, and deliberately so: it is the
     * only way to have a 32 KiB history window without allocating and without
     * a static the app could re-enter from a second thread. Only the scalars
     * are initialised. `window` is never read at an index the decoder has not
     * written (that is what the distance check guarantees), so zeroing 32 KiB
     * per call would buy nothing.
     */
    vmfw_state_t st;
    st.source     = source;
    st.source_ctx = source_ctx;
    st.sink       = sink;
    st.sink_ctx   = sink_ctx;
    st.limit      = expect_out;
    st.produced   = 0u;
    st.adler_a    = 1u;          /* RFC 1950: the Adler-32 of nothing is 1 */
    st.adler_b    = 0u;
    st.bitbuf     = 0u;
    st.bitcnt     = 0u;
    st.in_len     = 0u;
    st.in_pos     = 0u;
    st.in_eof     = false;
    st.wpos       = 0u;
    st.status     = VMFW_INFLATE_OK;

    vmfw_inflate_status_t rc = inflate_run(&st, zlib_wrapped);

    /* Reported even on failure. A streaming decoder has already pushed bytes
     * to the sink by the time it refuses, and the caller needs to know how
     * many in order to throw them away. */
    if (out_produced) *out_produced = st.produced;
    return rc;
}

vmfw_inflate_status_t vmfw_inflate(vmfw_source_fn source, void *source_ctx,
                                   vmfw_sink_fn sink, void *sink_ctx,
                                   uint64_t expect_out, uint64_t *out_produced) {
    return inflate_entry(source, source_ctx, sink, sink_ctx,
                         expect_out, out_produced, false);
}

vmfw_inflate_status_t vmfw_zlib_inflate(vmfw_source_fn source, void *source_ctx,
                                        vmfw_sink_fn sink, void *sink_ctx,
                                        uint64_t expect_out,
                                        uint64_t *out_produced) {
    return inflate_entry(source, source_ctx, sink, sink_ctx,
                         expect_out, out_produced, true);
}

static vmfw_inflate_status_t inflate_buffer_entry(const uint8_t *src,
                                                  size_t srclen,
                                                  uint8_t *dst, size_t dstcap,
                                                  uint64_t expect_out,
                                                  size_t *out_len,
                                                  bool zlib_wrapped) {
    if (out_len) *out_len = 0u;
    if (!src || !dst) return VMFW_INFLATE_ERR_INVALID_ARGUMENT;

    vmfw_mem_source_t in;
    vmfw_mem_sink_t   out;
    vmfw_mem_source_init(&in, src, srclen);
    vmfw_mem_sink_init(&out, dst, dstcap);

    vmfw_inflate_status_t rc =
        inflate_entry(vmfw_mem_source_read, &in, vmfw_mem_sink_write, &out,
                      expect_out, NULL, zlib_wrapped);

    /* Set whatever happened, so a caller looking at a failure can see how far
     * it got. `out.overflowed` is the reason behind a VMFW_INFLATE_ERR_SINK
     * from this entry point: the expansion did not fit `dstcap`. */
    if (out_len) *out_len = out.len;
    return rc;
}

vmfw_inflate_status_t vmfw_inflate_buffer(const uint8_t *src, size_t srclen,
                                          uint8_t *dst, size_t dstcap,
                                          uint64_t expect_out, size_t *out_len) {
    return inflate_buffer_entry(src, srclen, dst, dstcap, expect_out, out_len,
                                false);
}

vmfw_inflate_status_t vmfw_zlib_inflate_buffer(const uint8_t *src, size_t srclen,
                                               uint8_t *dst, size_t dstcap,
                                               uint64_t expect_out,
                                               size_t *out_len) {
    return inflate_buffer_entry(src, srclen, dst, dstcap, expect_out, out_len,
                                true);
}

const char *vmfw_inflate_strerror(vmfw_inflate_status_t st) {
    switch (st) {
        case VMFW_INFLATE_OK:
            return "ok";
        case VMFW_INFLATE_ERR_INVALID_ARGUMENT:
            return "invalid inflate argument";
        case VMFW_INFLATE_ERR_TRUNCATED:
            return "compressed input ended mid-stream";
        case VMFW_INFLATE_ERR_READ:
            return "the source reported a read error";
        case VMFW_INFLATE_ERR_BAD_BLOCK:
            return "reserved DEFLATE block type";
        case VMFW_INFLATE_ERR_BAD_STORED:
            return "stored block LEN and NLEN disagree";
        case VMFW_INFLATE_ERR_BAD_HUFFMAN:
            return "over-subscribed or incomplete Huffman code";
        case VMFW_INFLATE_ERR_BAD_SYMBOL:
            return "symbol outside the range RFC 1951 defines";
        case VMFW_INFLATE_ERR_BAD_DISTANCE:
            return "back-reference before the start of the output";
        case VMFW_INFLATE_ERR_SIZE_MISMATCH:
            return "expanded size is not the size the container declared";
        case VMFW_INFLATE_ERR_SINK:
            return "the sink refused the expanded bytes";
        case VMFW_INFLATE_ERR_BAD_ZLIB_HEADER:
            return "not the zlib stream it claims to be";
        default:
            return "unknown error";
    }
}
