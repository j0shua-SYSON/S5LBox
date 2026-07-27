/*
 * S5LBox — guest audio capture tests.
 *
 * The properties defended here are the ones that decide whether a captured
 * file can be trusted, and the first three matter more than the format work:
 *
 *   - a run that captured nothing leaves NO file, so absence can never be read
 *     as silence;
 *   - a run that captured only zeroes DOES leave a file, so silence can never
 *     be read as absence;
 *   - no header is written unless a format was established, and the derivation
 *     that establishes one reports which field was missing when it cannot.
 *
 * Then the writer itself: byte-exact header fields, little-endian on any host,
 * size fields that match the payload, and a known pattern that survives the
 * round trip unchanged.
 *
 * The feed is synthetic on purpose. A real boot costs ~25 minutes and would
 * capture nothing anyway -- the CPU never stores to the I2S FIFOs, because the
 * device tree hands their addresses to the PL080 and the PL080 is not modelled
 * -- so a machine driven through its own bus is both cheaper and a strictly
 * stronger test of the tap than a boot would be.
 */
#include "audio_capture.h"
#include "soc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { \
        g_fail++; \
        printf("  FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

/* ------------------------------------------------------------- helpers --- */

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t ld16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* Whole-file read. Returns NULL when the file does not exist, which several
 * cases below depend on being distinguishable from an empty file. */
static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0L, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0L, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)n + 1u);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    *len = got;
    return b;
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static const audio_format_t FMT_CD = { 44100u, 16u, 2u };

/* ---------------------------------------------------------- the header --- */

static void test_wav_header_is_byte_exact(void) {
    uint8_t h[WAV_HEADER_BYTES];
    CHECK(wav_header_build(h, &FMT_CD, 8u), "the canonical format was refused");

    /* Written out longhand rather than computed, so a mistake in the builder
     * cannot be mirrored by the same mistake in the expectation. 44100 Hz
     * 16-bit stereo: block align 4, byte rate 176400 = 0x0002b110. */
    static const uint8_t want[WAV_HEADER_BYTES] = {
        'R','I','F','F', 0x2c,0x00,0x00,0x00,   /* 36 + 8            */
        'W','A','V','E',
        'f','m','t',' ', 0x10,0x00,0x00,0x00,   /* 16                */
        0x01,0x00,                              /* WAVE_FORMAT_PCM   */
        0x02,0x00,                              /* channels          */
        0x44,0xac,0x00,0x00,                    /* 44100             */
        0x10,0xb1,0x02,0x00,                    /* 176400            */
        0x04,0x00,                              /* block align       */
        0x10,0x00,                              /* bits              */
        'd','a','t','a', 0x08,0x00,0x00,0x00    /* 8                 */
    };
    CHECK(memcmp(h, want, sizeof want) == 0,
          "the 44-byte header does not match the literal");

    /* A second, deliberately different shape: 8-bit mono, block align 1. */
    const audio_format_t mono8 = { 8000u, 8u, 1u };
    CHECK(wav_header_build(h, &mono8, 3u), "8-bit mono was refused");
    CHECK(ld16(h + 22) == 1u && ld32(h + 24) == 8000u &&
          ld32(h + 28) == 8000u && ld16(h + 32) == 1u && ld16(h + 34) == 8u,
          "8-bit mono fields are wrong");
    CHECK(ld32(h + 40) == 3u && ld32(h + 4) == 39u,
          "8-bit mono sizes are wrong (%u/%u)", ld32(h + 40), ld32(h + 4));
}

static void test_wav_header_fields_are_little_endian(void) {
    /* A rate whose four bytes are all different, so a byte-order mistake in
     * any position is visible rather than symmetric. 0x04030201 Hz is absurd
     * and that is the point: this checks the encoding, not a real device. */
    const audio_format_t odd = { 0x04030201u, 16u, 2u };
    uint8_t h[WAV_HEADER_BYTES];
    CHECK(wav_header_build(h, &odd, 0x0a0b0c00u), "the odd format was refused");
    CHECK(h[24] == 0x01u && h[25] == 0x02u && h[26] == 0x03u && h[27] == 0x04u,
          "sample rate is not little-endian: %02x %02x %02x %02x",
          h[24], h[25], h[26], h[27]);
    /* byte rate = rate * block align = 0x04030201 * 4 = 0x100c0804 */
    CHECK(h[28] == 0x04u && h[29] == 0x08u && h[30] == 0x0cu && h[31] == 0x10u,
          "byte rate is not little-endian: %02x %02x %02x %02x",
          h[28], h[29], h[30], h[31]);
    CHECK(h[40] == 0x00u && h[41] == 0x0cu && h[42] == 0x0bu && h[43] == 0x0au,
          "data size is not little-endian");
    /* RIFF size = data + 36 = 0x0a0b0c24 */
    CHECK(h[4] == 0x24u && h[5] == 0x0cu && h[6] == 0x0bu && h[7] == 0x0au,
          "RIFF size is not little-endian");
    /* The four ASCII tags are positional and a swap between them is silent. */
    CHECK(memcmp(h + 0, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0 &&
          memcmp(h + 12, "fmt ", 4) == 0 && memcmp(h + 36, "data", 4) == 0,
          "a chunk tag is in the wrong place");
}

static void test_header_refuses_what_it_cannot_describe(void) {
    uint8_t h[WAV_HEADER_BYTES];
    memset(h, 0xa5, sizeof h);

    const audio_format_t bad[] = {
        { 44100u,  12u, 2u },        /* not an expressible width          */
        { 44100u,  16u, 0u },        /* no channels                       */
        { 44100u,  16u, 9u },        /* past the canonical ceiling        */
        {     0u,  16u, 2u },        /* no rate                           */
        { UINT32_MAX, 16u, 2u },     /* byte rate would overflow uint32   */
        { 44100u,  24u, 1u },        /* align 3: tiles no 32-bit word     */
        { 44100u,  24u, 2u },        /* align 6: same                     */
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!audio_format_valid(&bad[i]),
              "format %u (%u/%u/%u) was accepted", i,
              bad[i].rate_hz, bad[i].bits, bad[i].channels);
        CHECK(!wav_header_build(h, &bad[i], 0u),
              "a header was built for format %u", i);
    }
    /* Refusal must not have touched the buffer. */
    for (unsigned i = 0; i < WAV_HEADER_BYTES; i++)
        CHECK(h[i] == 0xa5u, "refusal wrote byte %u", i);

    /* And the shapes that must stay accepted, including the two whose block
     * align is a MULTIPLE of the FIFO word rather than a divisor of it. */
    const audio_format_t good[] = {
        { 44100u, 16u, 2u }, { 48000u, 16u, 1u }, {  8000u,  8u, 1u },
        { 44100u,  8u, 2u }, { 96000u, 32u, 1u }, { 48000u, 32u, 2u },
        { 22050u, 24u, 4u },                      /* align 12, 12 % 4 == 0 */
    };
    for (unsigned i = 0; i < sizeof good / sizeof good[0]; i++)
        CHECK(audio_format_valid(&good[i]),
              "format %u (%u/%u/%u) was refused", i,
              good[i].rate_hz, good[i].bits, good[i].channels);

    CHECK(audio_block_align(&FMT_CD) == 4u, "CD block align is not 4");
    CHECK(audio_block_align(NULL) == 0u, "a NULL format has an alignment");
    CHECK(!audio_format_valid(NULL), "NULL was a valid format");
    /* The 36-byte RIFF overhead must not be allowed to wrap the size field. */
    CHECK(!wav_header_build(h, &FMT_CD, UINT32_MAX - 4u),
          "a payload that overflows the RIFF size field was accepted");
    CHECK(!wav_header_build(NULL, &FMT_CD, 0u), "a NULL buffer was written");
}

/* ------------------------------------------------------------ the sink --- */

/* A fixed, non-repeating pattern with values in every byte lane, a zero, and
 * a full-scale word. Fixed rather than random so a failure is reproducible. */
static uint32_t pattern(unsigned i) {
    static const uint32_t seed[8] = {
        0x00000000u, 0x0001fffeu, 0x7fff8000u, 0xffffffffu,
        0x12345678u, 0xdeadbeefu, 0x0000ffffu, 0xa5a55a5au
    };
    return seed[i & 7u] ^ ((uint32_t)i << 3);
}

static void test_size_fields_match_the_payload(void) {
    static const char *PATH = "audio-capture.wav";
    remove(PATH);

    audio_sink_t s;
    audio_sink_arm(&s, NULL, &FMT_CD);
    CHECK(strcmp(s.path, PATH) == 0, "fallback path is %s", s.path);

    const unsigned N = 1500u;          /* 6000 bytes: past one 4 KiB patch */
    for (unsigned i = 0; i < N; i++) audio_sink_word(&s, pattern(i));
    CHECK(audio_sink_close(&s), "close reported failure");
    CHECK(s.words == N && audio_sink_data_bytes(&s) == 4u * N,
          "counts are %llu words / %llu bytes",
          (unsigned long long)s.words,
          (unsigned long long)audio_sink_data_bytes(&s));

    size_t len = 0;
    uint8_t *f = slurp(PATH, &len);
    CHECK(f != NULL, "no file was written");
    if (f) {
        CHECK(len == WAV_HEADER_BYTES + 4u * N,
              "file is %llu bytes, expected %llu",
              (unsigned long long)len,
              (unsigned long long)(WAV_HEADER_BYTES + 4u * N));
        CHECK(ld32(f + 40) == 4u * N,
              "data chunk says %u, payload is %u", ld32(f + 40), 4u * N);
        CHECK(ld32(f + 4) == 36u + 4u * N,
              "RIFF size says %u, expected %u", ld32(f + 4), 36u + 4u * N);
        /* The two size fields are related, and a writer that patched only one
         * of them would still satisfy each check above on its own. */
        CHECK(ld32(f + 4) == ld32(f + 40) + 36u,
              "the two size fields disagree");
        CHECK(ld32(f + 4) + 8u == (uint32_t)len,
              "RIFF size does not describe the file it is in");
        free(f);
    }
    CHECK(audio_sink_partial_frame(&s) == 0u,
          "a whole number of frames reported a remainder");
    remove(PATH);

    /*
     * A block align that a 32-bit FIFO word does NOT tile: 32-bit stereo is 8
     * bytes per frame, so an odd word count ends the data chunk half-way
     * through one. That remainder is reported, never padded -- padding would
     * be inventing a sample, which is the one thing this whole module exists
     * to refuse.
     */
    const audio_format_t wide = { 48000u, 32u, 2u };
    audio_sink_t odd;
    audio_sink_arm(&odd, NULL, &wide);
    for (unsigned i = 0; i < 3u; i++) audio_sink_word(&odd, pattern(i));
    CHECK(audio_sink_close(&odd), "close failed on a partial frame");
    CHECK(audio_sink_partial_frame(&odd) == 4u,
          "partial frame is %u bytes, expected 4",
          audio_sink_partial_frame(&odd));
    len = 0;
    f = slurp(PATH, &len);
    CHECK(f && len == WAV_HEADER_BYTES + 12u,
          "the partial-frame file was padded or truncated (%llu bytes)",
          (unsigned long long)len);
    if (f) {
        CHECK(ld32(f + 40) == 12u, "data chunk says %u, expected the 12 bytes"
              " actually written", ld32(f + 40));
        free(f);
    }
    remove(PATH);
}

static void test_round_trips_a_known_pattern(void) {
    static const char *PATH = "audio-capture.wav";
    remove(PATH);

    audio_sink_t s;
    audio_sink_arm(&s, NULL, &FMT_CD);
    const unsigned N = 64u;
    for (unsigned i = 0; i < N; i++) audio_sink_word(&s, pattern(i));
    CHECK(audio_sink_close(&s), "close reported failure");

    size_t len = 0;
    uint8_t *f = slurp(PATH, &len);
    CHECK(f && len == WAV_HEADER_BYTES + 4u * N, "wrong file length");
    if (f && len == WAV_HEADER_BYTES + 4u * N) {
        unsigned bad = 0;
        for (unsigned i = 0; i < N; i++)
            if (ld32(f + WAV_HEADER_BYTES + 4u * i) != pattern(i)) bad++;
        CHECK(bad == 0u, "%u of %u words did not survive the round trip",
              bad, N);
        /* Byte order of the payload is not the header's byte order and could
         * be got wrong independently: word 4 is 0x12345678 ^ 0x20. */
        CHECK(f[WAV_HEADER_BYTES + 16u] == 0x58u &&
              f[WAV_HEADER_BYTES + 19u] == 0x12u,
              "payload words are not little-endian");
        free(f);
    }
    /* Nothing was silent, and the bit evidence agrees. */
    CHECK(s.nonzero_words == N - 1u,
          "nonzero words = %llu, expected %u",
          (unsigned long long)s.nonzero_words, N - 1u);
    CHECK(s.and_all == 0u && s.or_all != 0u,
          "or/and evidence is %08x/%08x", s.or_all, s.and_all);
    remove(PATH);
}

static void test_absence_and_silence_are_different_files(void) {
    static const char *WAV = "audio-capture.wav";
    static const char *RAW = "audio-capture.raw";
    remove(WAV);
    remove(RAW);

    /* ABSENCE: armed, never fed. This is the case a real boot produces today,
     * and a file here would be read by the next person as "the guest emitted
     * silence" -- a claim nothing in the run supports. */
    audio_sink_t nothing;
    audio_sink_arm(&nothing, NULL, &FMT_CD);
    CHECK(!audio_sink_close(&nothing), "close claimed success with no samples");
    CHECK(!file_exists(WAV), "a file was written for a run that captured none");
    CHECK(nothing.words == 0u && audio_sink_data_bytes(&nothing) == 0u,
          "an unfed sink counted something");

    /* The same with no format, so neither output name can appear. */
    audio_sink_t nothing_raw;
    audio_sink_arm(&nothing_raw, NULL, NULL);
    CHECK(!audio_sink_close(&nothing_raw), "raw close claimed success");
    CHECK(!file_exists(RAW), "a raw file was written for an empty run");

    /* SILENCE: fed nothing but zeroes. A real, reportable result -- the file
     * must exist, be the right length, and say so. */
    audio_sink_t quiet;
    audio_sink_arm(&quiet, NULL, &FMT_CD);
    const unsigned N = 256u;
    for (unsigned i = 0; i < N; i++) audio_sink_word(&quiet, 0u);
    CHECK(audio_sink_close(&quiet), "close failed on a silent capture");
    CHECK(quiet.words == N && quiet.nonzero_words == 0u,
          "silent capture counted %llu/%llu",
          (unsigned long long)quiet.words,
          (unsigned long long)quiet.nonzero_words);
    CHECK(quiet.or_all == 0u && quiet.and_all == 0u,
          "silence left bits set: or=%08x and=%08x", quiet.or_all,
          quiet.and_all);
    size_t len = 0;
    uint8_t *f = slurp(WAV, &len);
    CHECK(f != NULL, "silence produced no file");
    if (f) {
        CHECK(len == WAV_HEADER_BYTES + 4u * N, "silent file is %llu bytes",
              (unsigned long long)len);
        CHECK(ld32(f + 40) == 4u * N, "silent data chunk is %u", ld32(f + 40));
        unsigned nonzero = 0;
        for (size_t i = WAV_HEADER_BYTES; i < len; i++) if (f[i]) nonzero++;
        CHECK(nonzero == 0u, "%u payload bytes were not zero", nonzero);
        free(f);
    }
    remove(WAV);
}

static void test_no_format_means_no_header(void) {
    static const char *RAW = "audio-capture.raw";
    static const char *WAV = "audio-capture.wav";
    remove(RAW);
    remove(WAV);

    audio_sink_t s;
    audio_sink_arm(&s, NULL, NULL);
    CHECK(!s.have_fmt, "a NULL format produced a format");
    CHECK(strcmp(s.path, RAW) == 0, "raw path is %s", s.path);

    const unsigned N = 10u;
    for (unsigned i = 0; i < N; i++) audio_sink_word(&s, pattern(i));
    CHECK(audio_sink_close(&s), "raw close failed");
    CHECK(!file_exists(WAV), "a WAV appeared with no format");

    size_t len = 0;
    uint8_t *f = slurp(RAW, &len);
    CHECK(f != NULL, "no raw file was written");
    if (f) {
        CHECK(len == 4u * N, "raw file is %llu bytes, expected %u",
              (unsigned long long)len, 4u * N);
        CHECK(memcmp(f, "RIFF", 4) != 0, "the raw dump carries a RIFF header");
        CHECK(ld32(f) == pattern(0) && ld32(f + 4u * (N - 1u)) == pattern(N - 1u),
              "raw payload is not the words that went in");
        free(f);
    }
    CHECK(audio_sink_partial_frame(&s) == 0u,
          "a headerless dump reported a partial frame");
    remove(RAW);

    /* An invalid format is treated as no format rather than as an error: the
     * samples are worth more than the declaration. */
    const audio_format_t bogus = { 44100u, 24u, 2u };
    audio_sink_t b;
    audio_sink_arm(&b, NULL, &bogus);
    CHECK(!b.have_fmt && strcmp(b.path, RAW) == 0,
          "an invalid format was accepted (path %s)", b.path);
    audio_sink_word(&b, 0x11223344u);
    CHECK(audio_sink_close(&b), "close failed after an invalid format");
    CHECK(file_exists(RAW), "the samples were discarded with the declaration");
    remove(RAW);
}

static void test_the_path_is_per_run_when_a_work_image_names_it(void) {
    audio_sink_t s;

    audio_sink_arm(&s, "work/run99/rootfs.img", &FMT_CD);
    CHECK(strcmp(s.path, "work/run99/rootfs.img.audio.wav") == 0,
          "work-image WAV path is %s", s.path);

    audio_sink_arm(&s, "work/run99/rootfs.img", NULL);
    CHECK(strcmp(s.path, "work/run99/rootfs.img.audio.raw") == 0,
          "work-image raw path is %s", s.path);

    /* Empty and NULL both mean "no work image", exactly as uart_tee_byte()
     * treats them, and both must fall back rather than produce ".audio.wav". */
    audio_sink_arm(&s, "", &FMT_CD);
    CHECK(strcmp(s.path, AUDIO_WAV_PATH) == 0, "empty work path gave %s",
          s.path);
    audio_sink_arm(&s, NULL, &FMT_CD);
    CHECK(strcmp(s.path, AUDIO_WAV_PATH) == 0, "NULL work path gave %s",
          s.path);

    /* A path too long to extend must fall back to the fixed name rather than
     * write a truncated one -- a truncated path is a different file, and it
     * could be a real one. */
    char huge[AUDIO_PATH_MAX + 64u];
    memset(huge, 'w', sizeof huge - 1u);
    huge[sizeof huge - 1u] = '\0';
    audio_sink_arm(&s, huge, &FMT_CD);
    CHECK(strcmp(s.path, AUDIO_WAV_PATH) == 0,
          "an over-long work path did not fall back");

    /* Arming is total: a second arm must not inherit the first one's counts.
     * The stream is closed first because arm does not close it -- see the
     * header; bootkernel's tap arms exactly once, behind an `armed` test. */
    audio_sink_arm(&s, NULL, &FMT_CD);
    audio_sink_word(&s, 1u);
    CHECK(audio_sink_close(&s), "close before re-arming failed");
    audio_sink_arm(&s, NULL, &FMT_CD);
    CHECK(s.words == 0u && s.nonzero_words == 0u && s.f == NULL &&
          !s.failed && !s.closed && s.payload_bytes == 0u,
          "re-arming left state behind (words=%llu)",
          (unsigned long long)s.words);
    CHECK(remove(AUDIO_WAV_PATH) == 0,
          "the test left %s behind", AUDIO_WAV_PATH);
}

static void test_counts_survive_a_sink_that_cannot_write(void) {
    /* "The guest transmitted" and "the file was written" are different claims,
     * and the first must not depend on the second. A directory separator that
     * names no directory is the cheapest portable way to fail an fopen. */
    audio_sink_t s;
    audio_sink_arm(&s, "no-such-directory-9c3f/img", &FMT_CD);
    for (unsigned i = 0; i < 5u; i++) audio_sink_word(&s, pattern(i));
    CHECK(s.words == 5u, "a failed sink lost the count (%llu)",
          (unsigned long long)s.words);
    CHECK(s.failed, "a sink that could not open its file reported success");
    CHECK(!audio_sink_close(&s), "close claimed success after a failure");
    CHECK(audio_sink_data_bytes(&s) == 0u,
          "a failed sink claimed %llu bytes on disk",
          (unsigned long long)audio_sink_data_bytes(&s));

    /* And an unarmed sink still counts, so a tap wired before the sink is
     * armed cannot silently lose the only evidence there is. */
    audio_sink_t bare;
    memset(&bare, 0, sizeof bare);
    audio_sink_word(&bare, 0x1234u);
    audio_sink_word(&bare, 0u);
    CHECK(bare.words == 2u && bare.nonzero_words == 1u,
          "an unarmed sink did not count");
    audio_sink_word(NULL, 0u);
    CHECK(!audio_sink_close(NULL), "closing NULL claimed success");
}

/* ------------------------------------------------- the format decision --- */

/* A minimal Apple flattened device tree, built by hand. Apple's format, not
 * FDT: u32 nprops, u32 nchildren, then char name[32] + u32 len + padded value
 * per property, then the children. */
typedef struct { uint8_t b[2048]; size_t n; } dtb_t;

static void dtb_u32(dtb_t *d, uint32_t v) {
    d->b[d->n++] = (uint8_t)(v & 0xffu);
    d->b[d->n++] = (uint8_t)((v >> 8) & 0xffu);
    d->b[d->n++] = (uint8_t)((v >> 16) & 0xffu);
    d->b[d->n++] = (uint8_t)((v >> 24) & 0xffu);
}

static void dtb_head(dtb_t *d, uint32_t nprops, uint32_t nkids) {
    dtb_u32(d, nprops);
    dtb_u32(d, nkids);
}

static void dtb_prop(dtb_t *d, const char *name, const void *val, size_t len) {
    memset(d->b + d->n, 0, 32);
    memcpy(d->b + d->n, name, strlen(name));
    d->n += 32;
    dtb_u32(d, (uint32_t)len);
    if (len) memcpy(d->b + d->n, val, len);
    d->n += (len + 3u) & ~(size_t)3u;
}

static void dtb_prop_u32(dtb_t *d, const char *name, uint32_t v) {
    uint8_t le[4] = { (uint8_t)(v & 0xffu), (uint8_t)((v >> 8) & 0xffu),
                      (uint8_t)((v >> 16) & 0xffu), (uint8_t)((v >> 24) & 0xffu) };
    dtb_prop(d, name, le, 4);
}

static void dtb_name(dtb_t *d, const char *n) {
    dtb_prop(d, "name", n, strlen(n) + 1u);
}

/* device-tree / arm-io / i2s0 / audio0, with `nrates` sysclk properties taken
 * from `rates`. Everything else is the shape the shipped tree has. */
static void build_tree(dtb_t *d, const uint32_t *rates, unsigned nrates,
                       bool with_audio0) {
    static const uint8_t REG[32] = {
        0,0,0,0, 0,0,0,0, 0x00,0x03,0x10,0x01, 0x04,0,0,0,
        0,0,0,0, 0x04,0,0,0, 0,0,0,0, 0,0,0,0
    };
    memset(d, 0, sizeof *d);
    dtb_head(d, 1u, 1u); dtb_name(d, "device-tree");
    dtb_head(d, 1u, 1u); dtb_name(d, "arm-io");
    dtb_head(d, 1u, with_audio0 ? 1u : 0u); dtb_name(d, "i2s0");
    if (with_audio0) {
        dtb_head(d, 3u + nrates, 0u);
        dtb_name(d, "audio0");
        for (unsigned i = 0; i < nrates; i++) {
            char nm[32];
            snprintf(nm, sizeof nm, "sysclk-%u", (unsigned)rates[i]);
            /* The VALUE is a divider word and is deliberately not the rate:
             * a reader that took the value would get 12,283,087 Hz. */
            dtb_prop_u32(d, nm, 0x00bb6ccfu + i);
        }
        dtb_prop_u32(d, "clock-frequency", 12000000u);
        dtb_prop(d, "reg", REG, sizeof REG);
    }
}

static void test_dt_rates_come_from_property_names(void) {
    dtb_t d;
    audio_dt_facts_t f;
    const uint32_t two[2] = { 44100u, 48000u };

    build_tree(&d, two, 2u, true);
    CHECK(audio_dt_read(d.b, d.n, "arm-io/i2s0/audio0", &f),
          "a well-formed tree did not parse");
    CHECK(f.node_found, "audio0 was not found");
    CHECK(f.rates == 2u && f.rate[0] == 44100u && f.rate[1] == 48000u,
          "rates are %u: %u, %u", f.rates, f.rate[0], f.rate[1]);
    CHECK(f.rate_value[0] == 0x00bb6ccfu,
          "the divider word was not carried (%08x)", f.rate_value[0]);
    CHECK(f.have_clock_frequency && f.clock_frequency == 12000000u,
          "clock-frequency is %u", f.clock_frequency);
    CHECK(f.reg_len == 32u && f.reg[8] == 0x00u && f.reg[10] == 0x10u,
          "reg blob is %u bytes", f.reg_len);
    CHECK(f.rates_dropped == 0u, "rates were dropped");

    /* A node with no sysclk properties is found but declares nothing. */
    build_tree(&d, NULL, 0u, true);
    CHECK(audio_dt_read(d.b, d.n, "arm-io/i2s0/audio0", &f) &&
          f.node_found && f.rates == 0u,
          "an empty node reported %u rates", f.rates);

    /* A tree without the node at all. */
    build_tree(&d, two, 2u, false);
    CHECK(audio_dt_read(d.b, d.n, "arm-io/i2s0/audio0", &f),
          "a tree missing the node failed to parse");
    CHECK(!f.node_found, "audio0 was found in a tree that has none");

    /* Garbage in: a false return, and a zeroed result rather than a stale one. */
    memset(&f, 0xff, sizeof f);
    CHECK(!audio_dt_read((const uint8_t *)"\xff\xff\xff\xff", 4u,
                         "arm-io/i2s0/audio0", &f),
          "a truncated blob parsed");
    CHECK(!f.node_found && f.rates == 0u, "a failed read left state behind");
    CHECK(!audio_dt_read(NULL, 0u, "x", &f), "a NULL blob parsed");
}

static void test_derivation_reports_the_first_missing_field(void) {
    dtb_t d;
    audio_dt_facts_t f;
    audio_format_t fmt;
    s5l_i2s_t i2s;
    const uint32_t two[2] = { 44100u, 48000u };
    const uint32_t one[1] = { 48000u };

    memset(&fmt, 0xa5, sizeof fmt);
    s5l_i2s_reset(&i2s);

    /* No node: nothing else can even be asked. */
    build_tree(&d, two, 2u, false);
    CHECK(audio_dt_read(d.b, d.n, "arm-io/i2s0/audio0", &f), "read failed");
    CHECK(audio_format_derive(&f, &i2s, &fmt) == AUDIO_FMT_NO_NODE,
          "a missing node did not report NO_NODE");
    CHECK(audio_format_derive(NULL, &i2s, &fmt) == AUDIO_FMT_NO_NODE,
          "a NULL facts pointer did not report NO_NODE");

    /* Node present, controller untouched. This outranks the rate question:
     * a controller nobody programmed has no format at all. */
    build_tree(&d, two, 2u, true);
    CHECK(audio_dt_read(d.b, d.n, "arm-io/i2s0/audio0", &f), "read failed");
    CHECK(audio_format_derive(&f, &i2s, &fmt) == AUDIO_FMT_UNCONFIGURED,
          "an unwritten controller did not report UNCONFIGURED");

    /* Writes that MISSED the seven do not count as configuration. */
    s5l_i2s_write(&i2s, 0x10u, 0x1234u);
    CHECK(audio_format_derive(&f, &i2s, &fmt) == AUDIO_FMT_UNCONFIGURED,
          "a FIFO store was mistaken for configuration");

    /* Now the driver's own sequence: configure() plus startTransfer(). */
    s5l_i2s_write(&i2s, 0x00u, 1u);
    s5l_i2s_write(&i2s, 0x3cu, 1u);
    s5l_i2s_write(&i2s, 0x08u, 6u);
    CHECK(audio_format_derive(&f, &i2s, &fmt) == AUDIO_FMT_RATE_UNDECIDED,
          "two declared rates did not report RATE_UNDECIDED");

    /* One declared rate settles the rate and exposes the next gap. */
    build_tree(&d, one, 1u, true);
    CHECK(audio_dt_read(d.b, d.n, "arm-io/i2s0/audio0", &f), "read failed");
    CHECK(audio_format_derive(&f, &i2s, &fmt) == AUDIO_FMT_WIDTH_UNESTABLISHED,
          "a single rate did not reach the width question");

    /* No path through this function writes `fmt`, because no path returns OK
     * today. If one ever does, this check is what forces somebody to decide
     * what it means rather than letting a partially-filled struct escape. */
    CHECK(fmt.rate_hz == 0xa5a5a5a5u,
          "the output format was written without an OK status");

    /* Every status has a reason, and no two share one. */
    const audio_fmt_status_t all[] = {
        AUDIO_FMT_OK, AUDIO_FMT_NO_NODE, AUDIO_FMT_UNCONFIGURED,
        AUDIO_FMT_RATE_UNDECIDED, AUDIO_FMT_WIDTH_UNESTABLISHED
    };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
        CHECK(audio_fmt_reason(all[i]) && *audio_fmt_reason(all[i]),
              "status %u has no reason", (unsigned)all[i]);
        for (unsigned j = i + 1u; j < sizeof all / sizeof all[0]; j++)
            CHECK(strcmp(audio_fmt_reason(all[i]),
                         audio_fmt_reason(all[j])) != 0,
                  "statuses %u and %u share a reason",
                  (unsigned)all[i], (unsigned)all[j]);
    }
}

/* --------------------------------------------------- the device and tap --- */

static void test_i2s_names_both_fifos_without_forgiving_them(void) {
    CHECK(s5l_i2s_fifo_role(S5L_I2S_TX_FIFO_OFF) == S5L_I2S_FIFO_TX &&
          s5l_i2s_fifo_role(S5L_I2S_RX_FIFO_OFF) == S5L_I2S_FIFO_RX,
          "the two FIFO offsets are not classified");
    CHECK(S5L_I2S_TX_FIFO_OFF == 0x10u && S5L_I2S_RX_FIFO_OFF == 0x38u,
          "the FIFO offsets moved; /arm-io/i2s0 dma-channels says 0x10/0x38");
    /* None of the seven registers may be mistaken for a FIFO. */
    for (unsigned i = 0; i < S5L_I2S_REGS; i++)
        CHECK(s5l_i2s_fifo_role(s5l_i2s_offset(i)) == S5L_I2S_FIFO_NONE,
              "register offset 0x%02x was classified as a FIFO",
              s5l_i2s_offset(i));
    CHECK(s5l_i2s_fifo_role(0x14u) == S5L_I2S_FIFO_NONE &&
          s5l_i2s_fifo_role(0x0cu) == S5L_I2S_FIFO_NONE &&
          s5l_i2s_fifo_role(0u) == S5L_I2S_FIFO_NONE,
          "a neighbouring offset was classified as a FIFO");

    /* The absolute addresses the `dma-channels` blob hands the PL080. */
    CHECK(s5l_i2s_fifo_pa(0u, S5L_I2S_FIFO_TX) == 0x3ca00010u &&
          s5l_i2s_fifo_pa(0u, S5L_I2S_FIFO_RX) == 0x3ca00038u &&
          s5l_i2s_fifo_pa(1u, S5L_I2S_FIFO_TX) == 0x3cd00010u &&
          s5l_i2s_fifo_pa(1u, S5L_I2S_FIFO_RX) == 0x3cd00038u,
          "the FIFO physical addresses do not match the device tree");
    CHECK(s5l_i2s_fifo_pa(2u, S5L_I2S_FIFO_TX) == UINT32_MAX &&
          s5l_i2s_fifo_pa(0u, S5L_I2S_FIFO_NONE) == UINT32_MAX,
          "a bad FIFO request produced an address");

    /* Naming them must not have started excusing them: a CPU store to a DMA
     * FIFO is still the off-map access it always was. */
    s5l_i2s_t i2s;
    s5l_i2s_reset(&i2s);
    s5l_i2s_write(&i2s, S5L_I2S_TX_FIFO_OFF, 0xcafebabeu);
    s5l_i2s_write(&i2s, S5L_I2S_RX_FIFO_OFF, 0u);
    CHECK(i2s.unknown_writes == 2u && i2s.unknown_off_count == 2u,
          "FIFO stores stopped being counted (writes=%llu off=%u)",
          (unsigned long long)i2s.unknown_writes, i2s.unknown_off_count);
    CHECK(s5l_i2s_read(&i2s, S5L_I2S_TX_FIFO_OFF) == 0u,
          "a FIFO offset became storage");
}

static void test_a_synthetic_transfer_reaches_the_file(void) {
    static const char *PATH = "audio-capture.wav";
    remove(PATH);

    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0u, 1u << 20), "machine init failed");

    audio_sink_t s;
    audio_sink_arm(&s, NULL, &FMT_CD);
    uint64_t rx_stores = 0, other_controller = 0;

    /* Exactly the tap tools/bootkernel.c installs, over the real bus: the
     * machine's own dispatch decides these are I2S accesses, not the test. */
    const unsigned N = 32u;
    for (unsigned i = 0; i < N; i++) {
        uint32_t addr = S5L8900_I2S0_BASE + S5L_I2S_TX_FIFO_OFF;
        m.bus.write32(m.bus.ctx, addr, pattern(i));
        switch (s5l_i2s_fifo_role(addr & 0xfffu)) {
        case S5L_I2S_FIFO_TX: audio_sink_word(&s, pattern(i)); break;
        case S5L_I2S_FIFO_RX: rx_stores++; break;
        default: break;
        }
    }
    /* i2s1 is the baseband voice path, not the codec's. Its samples must be
     * counted somewhere else or they would be spliced into the codec's file. */
    m.bus.write32(m.bus.ctx, S5L8900_I2S1_BASE + S5L_I2S_TX_FIFO_OFF, 0x99u);
    other_controller++;

    CHECK(m.i2s[0].unknown_writes == N && m.i2s[1].unknown_writes == 1u,
          "the machine routed %llu/%llu FIFO stores",
          (unsigned long long)m.i2s[0].unknown_writes,
          (unsigned long long)m.i2s[1].unknown_writes);
    CHECK(m.unmapped_writes == 0u, "an I2S FIFO store was classified unmapped");
    CHECK(rx_stores == 0u && other_controller == 1u, "tap bookkeeping is wrong");

    CHECK(audio_sink_close(&s), "close failed");
    CHECK(s.words == N, "the tap captured %llu of %u words",
          (unsigned long long)s.words, N);

    size_t len = 0;
    uint8_t *f = slurp(PATH, &len);
    CHECK(f && len == WAV_HEADER_BYTES + 4u * N,
          "the synthetic transfer produced %llu bytes",
          (unsigned long long)len);
    if (f) {
        unsigned bad = 0;
        for (unsigned i = 0; i < N; i++)
            if (ld32(f + WAV_HEADER_BYTES + 4u * i) != pattern(i)) bad++;
        CHECK(bad == 0u, "%u words differ between the bus and the file", bad);
        free(f);
    }
    remove(PATH);
    s5l8900_free(&m);
}

/* ------------------------------------------------- the shipped firmware --- */

/*
 * The same derivation against the real 7E18 tree, when it is present. Public
 * CI has no Apple firmware, so this skips there; on this machine it is the
 * only case that can catch the tree saying something other than what the
 * comments in soc.h and audio_capture.h claim it says.
 */
static void test_the_shipped_tree_declares_two_rates(void) {
#ifdef S5LBOX_FIRMWARE_DIR
    char path[1024];
    snprintf(path, sizeof path, "%s/devicetree.bin", S5LBOX_FIRMWARE_DIR);
    size_t len = 0;
    uint8_t *blob = slurp(path, &len);
    if (!blob) {
        printf("  SKIP  %s absent\n", path);
        return;
    }
    audio_dt_facts_t f;
    CHECK(audio_dt_read(blob, len, "arm-io/i2s0/audio0", &f),
          "the shipped device tree did not parse");
    CHECK(f.node_found, "/arm-io/i2s0/audio0 is not in the shipped tree");
    CHECK(f.rates == 2u, "the shipped node declares %u rates, expected 2",
          f.rates);
    if (f.rates == 2u) {
        bool has44 = f.rate[0] == 44100u || f.rate[1] == 44100u;
        bool has48 = f.rate[0] == 48000u || f.rate[1] == 48000u;
        CHECK(has44 && has48, "the declared rates are %u and %u",
              f.rate[0], f.rate[1]);
    }
    CHECK(f.have_clock_frequency && f.clock_frequency == 12000000u,
          "the node's clock-frequency is %u, expected 12000000",
          f.clock_frequency);
    CHECK(f.reg_len == 32u, "the node's reg blob is %u bytes, expected 32",
          f.reg_len);

    /* And therefore, on the firmware this project actually boots, no format
     * is derivable and no WAV may be written. This is the result. */
    s5l_i2s_t i2s;
    s5l_i2s_reset(&i2s);
    s5l_i2s_write(&i2s, 0x00u, 1u);          /* pretend configure() ran */
    audio_format_t fmt;
    CHECK(audio_format_derive(&f, &i2s, &fmt) == AUDIO_FMT_RATE_UNDECIDED,
          "the shipped tree unexpectedly settled the sample rate");

    /* The baseband controller's node declares the same two rates, which is
     * why the capture is restricted to i2s0 by address rather than by
     * whichever window happened to be written first. */
    audio_dt_facts_t f1;
    CHECK(audio_dt_read(blob, len, "arm-io/i2s1/audio1", &f1) &&
          f1.node_found && f1.rates == 2u,
          "the baseband audio node is not the shape i2s0's is");
    free(blob);
#else
    printf("  SKIP  built without S5LBOX_FIRMWARE_DIR\n");
#endif
}

int main(void) {
    printf("S5LBox guest audio capture tests\n");
    test_wav_header_is_byte_exact();
    test_wav_header_fields_are_little_endian();
    test_header_refuses_what_it_cannot_describe();
    test_size_fields_match_the_payload();
    test_round_trips_a_known_pattern();
    test_absence_and_silence_are_different_files();
    test_no_format_means_no_header();
    test_the_path_is_per_run_when_a_work_image_names_it();
    test_counts_survive_a_sink_that_cannot_write();
    test_dt_rates_come_from_property_names();
    test_derivation_reports_the_first_missing_field();
    test_i2s_names_both_fifos_without_forgiving_them();
    test_a_synthetic_transfer_reaches_the_file();
    test_the_shipped_tree_declares_two_rates();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
