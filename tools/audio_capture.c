/*
 * S5LBox -- the guest's I2S transmit stream, on disk.
 *
 * See audio_capture.h for what this refuses to do and why. The short version
 * is that every decision here is arranged so a file that exists cannot make a
 * claim the run did not earn.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "audio_capture.h"

#include "devicetree.h"

#include <string.h>

/* ------------------------------------------------------------- format --- */

uint32_t audio_block_align(const audio_format_t *fmt) {
    if (!fmt) return 0u;
    if (fmt->bits != 8u && fmt->bits != 16u &&
        fmt->bits != 24u && fmt->bits != 32u) return 0u;
    if (fmt->channels < 1u || fmt->channels > 8u) return 0u;
    return (uint32_t)fmt->channels * ((uint32_t)fmt->bits / 8u);
}

bool audio_format_valid(const audio_format_t *fmt) {
    uint32_t align = audio_block_align(fmt);
    if (!align) return false;
    if (fmt->rate_hz == 0u) return false;
    /* The header's byte-rate field is a uint32_t. An overflowing product would
     * write a wrong number into a correct-looking file, which is worse than
     * refusing the format. */
    if (fmt->rate_hz > UINT32_MAX / align) return false;
    /* Whole 32-bit FIFO words in, so a frame must tile a word or a word must
     * tile a frame. See the note in the header on why this is not sufficient
     * on its own and what audio_sink_close() does about the remainder. */
    if ((4u % align) != 0u && (align % 4u) != 0u) return false;
    return true;
}

/* Little-endian stores, by hand. The host's own byte order never enters this
 * file: a capture written on a big-endian runner has to be the same bytes as
 * one written here, or the tests are only checking this machine. */
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

bool wav_header_build(uint8_t out[WAV_HEADER_BYTES],
                      const audio_format_t *fmt, uint32_t data_bytes) {
    if (!out || !audio_format_valid(fmt)) return false;
    /* 36 is the RIFF size field's fixed overhead: the 44-byte header minus the
     * 8 bytes of "RIFF" and the field itself. */
    if (data_bytes > UINT32_MAX - 36u) return false;

    uint32_t align = audio_block_align(fmt);

    memcpy(out + 0,  "RIFF", 4);
    put32(out + 4,   36u + data_bytes);
    memcpy(out + 8,  "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    put32(out + 16,  16u);            /* PCM `fmt ` chunk length            */
    put16(out + 20,  1u);             /* WAVE_FORMAT_PCM                    */
    put16(out + 22,  fmt->channels);
    put32(out + 24,  fmt->rate_hz);
    put32(out + 28,  fmt->rate_hz * align);   /* byte rate                  */
    put16(out + 32,  (uint16_t)align);        /* block align                */
    put16(out + 34,  fmt->bits);
    memcpy(out + 36, "data", 4);
    put32(out + 40,  data_bytes);
    return true;
}

/* ------------------------------------------- what the device establishes --- */

/*
 * `sysclk-<decimal>` -> the decimal, or 0.
 *
 * Strict on purpose: the whole reason the rate is taken from the NAME is that
 * a name is unambiguous, so anything that is not exactly the prefix followed by
 * at least one digit and nothing else is not a rate. A tolerant parser here
 * would quietly turn some future `sysclk-auto` into 0 Hz.
 */
static uint32_t sysclk_rate(const char *name) {
    static const char PREFIX[] = "sysclk-";
    size_t i = 0;
    while (PREFIX[i]) {
        if (name[i] != PREFIX[i]) return 0u;
        i++;
    }
    if (name[i] < '0' || name[i] > '9') return 0u;
    uint32_t v = 0u;
    for (; name[i]; i++) {
        if (name[i] < '0' || name[i] > '9') return 0u;
        if (v > (UINT32_MAX - 9u) / 10u) return 0u;   /* absurd, but bounded */
        v = v * 10u + (uint32_t)(name[i] - '0');
    }
    return v;
}

static uint32_t load32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool audio_dt_read(const uint8_t *blob, size_t len, const char *path,
                   audio_dt_facts_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!blob || !len || !path) return false;

    dt_t dt;
    dt_node_t root, node;
    /* dt_parse validates the WHOLE blob -- every node header, every property
     * length, the nesting depth -- so the hand walk below is walking a tree
     * that has already been proved to fit. The bounds tests are kept anyway;
     * they cost nothing and they are what makes this function safe to read on
     * its own rather than only in the presence of its caller. */
    if (dt_parse(blob, len, &dt, &root) != DT_OK) return false;
    if (dt_path(&dt, &root, path, &node) != DT_OK) return true;  /* no node */
    out->node_found = true;

    size_t off = node.props_off;
    for (uint32_t i = 0; i < node.n_props; i++) {
        if (off + DT_PROP_NAME_LEN + 4u > len) break;
        char name[DT_PROP_NAME_LEN + 1];
        memcpy(name, blob + off, DT_PROP_NAME_LEN);
        name[DT_PROP_NAME_LEN] = '\0';
        off += DT_PROP_NAME_LEN;

        uint32_t vlen = load32le(blob + off) & 0x7fffffffu;  /* bit 31 is a
                                                              * tool flag */
        off += 4u;
        if (vlen > len - off) break;
        const uint8_t *val = blob + off;

        uint32_t rate = sysclk_rate(name);
        if (rate) {
            if (out->rates < AUDIO_DT_RATE_MAX) {
                out->rate[out->rates] = rate;
                out->rate_value[out->rates] =
                    vlen >= 4u ? load32le(val) : 0u;
                out->rates++;
            } else {
                out->rates_dropped++;
            }
        } else if (strcmp(name, "clock-frequency") == 0 && vlen >= 4u) {
            out->clock_frequency = load32le(val);
            out->have_clock_frequency = true;
        } else if (strcmp(name, "reg") == 0 && vlen <= AUDIO_DT_REG_MAX) {
            memcpy(out->reg, val, vlen);
            out->reg_len = (unsigned)vlen;
        }

        off += (vlen + 3u) & ~(size_t)3u;   /* values pad to a 4-byte edge */
    }
    return true;
}

audio_fmt_status_t audio_format_derive(const audio_dt_facts_t *dt,
                                       const s5l_i2s_t *i2s,
                                       audio_format_t *out) {
    if (!dt || !dt->node_found) return AUDIO_FMT_NO_NODE;

    /*
     * "The controller was never programmed" outranks everything below it. A
     * reader who is told the rate is ambiguous will go looking for the bit that
     * selects it; a reader who is told nothing was ever written knows to look
     * several layers up instead, at whether a transfer was configured at all.
     *
     * `writes` counts every store to the window and `unknown_writes` the ones
     * that missed the seven, so their difference is what the driver's own
     * writeRegister actually landed.
     */
    if (!i2s || i2s->writes <= i2s->unknown_writes) return AUDIO_FMT_UNCONFIGURED;

    if (dt->rates != 1u) return AUDIO_FMT_RATE_UNDECIDED;

    /*
     * And here is where it stops, on every firmware this project has. There is
     * no established source for the sample width or the channel count -- not in
     * the six codec registers the driver reads back, not in the seven opaque
     * controller words, not in audio0's 32-byte `reg` blob. Returning a guess
     * would produce a file indistinguishable from a correct one.
     *
     * `out` stays untouched. When a width and a channel count are eventually
     * established, they are filled in here and the status becomes AUDIO_FMT_OK;
     * nothing else in this file changes.
     */
    (void)out;
    return AUDIO_FMT_WIDTH_UNESTABLISHED;
}

const char *audio_fmt_reason(audio_fmt_status_t st) {
    switch (st) {
    case AUDIO_FMT_OK:
        return "established from the device";
    case AUDIO_FMT_NO_NODE:
        return "no /arm-io/i2s0/audio0 node was read";
    case AUDIO_FMT_UNCONFIGURED:
        return "the guest never wrote an I2S register";
    case AUDIO_FMT_RATE_UNDECIDED:
        return "the node declares no single rate";
    case AUDIO_FMT_WIDTH_UNESTABLISHED:
        return "nothing establishes sample width or channel count";
    }
    return "unknown";
}

/* ---------------------------------------------------------------- sink --- */

/*
 * The largest payload the header can describe. Both size fields are uint32_t,
 * so beyond this the file stops being a WAV rather than becoming a longer one.
 * Appending is stopped and `capped` is latched; the words keep being counted,
 * because the count is the measurement and the file is only its container.
 */
#define AUDIO_MAX_DATA_BYTES (UINT32_MAX - 64u)

/* Rewrite the size fields every 4 KiB rather than only at close, for the same
 * reason uart_tee_byte() flushes at every newline: this file has to survive a
 * run that ends badly, and a header claiming zero bytes over a megabyte of
 * samples is a file no reader will play. */
#define AUDIO_PATCH_EVERY 4096u

static void path_for(char *out, size_t cap, const char *work,
                     const char *suffix, const char *fallback) {
    int n = -1;
    if (work && *work) n = snprintf(out, cap, "%s%s", work, suffix);
    if (n <= 0 || (size_t)n >= cap) snprintf(out, cap, "%s", fallback);
}

void audio_sink_arm(audio_sink_t *s, const char *work_image_path,
                    const audio_format_t *fmt) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->and_all = UINT32_MAX;
    s->armed = true;
    /* A format that would not survive wav_header_build() is treated as no
     * format at all. The alternative -- refusing to arm -- would throw the
     * guest's samples away over a mistake in a declaration. */
    s->have_fmt = fmt && audio_format_valid(fmt);
    if (s->have_fmt) s->fmt = *fmt;
    path_for(s->path, sizeof s->path, work_image_path,
             s->have_fmt ? ".audio.wav" : ".audio.raw",
             s->have_fmt ? AUDIO_WAV_PATH : AUDIO_RAW_PATH);
}

static bool patch_sizes(audio_sink_t *s) {
    uint8_t hdr[WAV_HEADER_BYTES];
    if (!wav_header_build(hdr, &s->fmt, (uint32_t)s->payload_bytes))
        return false;
    /* Seek back to the two fields and then to the end again. SEEK_END rather
     * than an absolute return offset on purpose: `long` is 32-bit on this
     * project's own MinGW host, and a payload near the cap would overflow the
     * absolute form silently. */
    if (fseek(s->f, 4L, SEEK_SET) != 0) return false;
    if (fwrite(hdr + 4, 1, 4, s->f) != 4) return false;
    if (fseek(s->f, 40L, SEEK_SET) != 0) return false;
    if (fwrite(hdr + 40, 1, 4, s->f) != 4) return false;
    if (fseek(s->f, 0L, SEEK_END) != 0) return false;
    return true;
}

void audio_sink_word(audio_sink_t *s, uint32_t word) {
    if (!s) return;

    /* Counted first and unconditionally. Every later step can fail; the answer
     * to "did the guest transmit" must not depend on any of them. */
    s->words++;
    if (word) s->nonzero_words++;
    s->or_all  |= word;
    s->and_all &= word;

    if (!s->armed || s->failed || s->closed || s->capped) return;

    if (!s->f) {
        /* Opened by the first word, never at arm time -- see the header. */
        s->f = fopen(s->path, "wb");
        if (!s->f) { s->failed = true; return; }
        if (s->have_fmt) {
            uint8_t hdr[WAV_HEADER_BYTES];
            /* data_bytes 0 for now; patch_sizes() corrects it as we go. */
            if (!wav_header_build(hdr, &s->fmt, 0u) ||
                fwrite(hdr, 1, WAV_HEADER_BYTES, s->f) != WAV_HEADER_BYTES) {
                s->failed = true;
                return;
            }
        }
    }

    if (s->payload_bytes + 4u > AUDIO_MAX_DATA_BYTES) {
        s->capped = true;
        return;
    }

    uint8_t le[4];
    put32(le, word);
    if (fwrite(le, 1, 4, s->f) != 4) { s->failed = true; return; }
    s->payload_bytes += 4u;

    if (s->have_fmt && (s->payload_bytes % AUDIO_PATCH_EVERY) == 0u &&
        !patch_sizes(s))
        s->failed = true;
}

bool audio_sink_close(audio_sink_t *s) {
    if (!s || s->closed) return false;
    s->closed = true;
    if (!s->f) return false;               /* nothing was ever captured */
    if (!s->failed && s->have_fmt && !patch_sizes(s)) s->failed = true;
    if (fclose(s->f) != 0) s->failed = true;
    s->f = NULL;
    return !s->failed;
}

uint64_t audio_sink_data_bytes(const audio_sink_t *s) {
    return s ? s->payload_bytes : 0u;
}

uint32_t audio_sink_partial_frame(const audio_sink_t *s) {
    if (!s || !s->have_fmt) return 0u;
    uint32_t align = audio_block_align(&s->fmt);
    if (!align) return 0u;
    return (uint32_t)(s->payload_bytes % align);
}
