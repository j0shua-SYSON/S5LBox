/*
 * S5LBox -- capture the guest's I2S transmit stream to a file.
 *
 * WHAT THIS IS FOR. "Does iPhone OS 3 produce sound" has been an assumption on
 * this project, in both directions, because there was no sink: anything the
 * guest put on the transmit path was discarded by the emulator and no run could
 * be judged afterwards. This is the audio equivalent of firmware/screen.ppm --
 * evidence written to disk, not a speaker. No host audio API, no realtime
 * constraint, no platform dependency; it is host FILESYSTEM work, which is why
 * it lives beside file_block.c and rootfs_work.c rather than in emucore.
 *
 * THE ONE PROPERTY THIS FILE EXISTS TO KEEP. A file that exists must not be
 * able to lie. Three separate rules enforce that and each is tested:
 *
 *   1. NOTHING IS WRITTEN UNTIL A SAMPLE ARRIVES. The stream is opened by the
 *      first word, exactly as tools/bootkernel.c's uart_tee_byte() opens the
 *      console tee, so a run that captured nothing leaves no file to be
 *      mistaken for silence. Absence and silence are different results and the
 *      filesystem is where the difference is easiest to destroy.
 *
 *   2. NO HEADER IS INVENTED. A RIFF/WAVE header states a sample rate, a width
 *      and a channel count. If the device did not establish those, writing
 *      44100/16/2 over the guest's bytes would turn a measurement into a guess
 *      that looks like a measurement -- and it would look exactly the same as a
 *      correct capture. When no format is established the same samples are
 *      written HEADERLESS instead (see audio_sink_arm), so the evidence
 *      survives without a claim attached to it.
 *
 *   3. THE PAYLOAD IS THE GUEST'S BYTES. audio_sink_word() appends the 32-bit
 *      FIFO word little-endian and does not repack, scale, dither or reorder
 *      it. The header, when there is one, describes those bytes; it never
 *      changes them. If a future format declaration turns out to be wrong, the
 *      file is still byte-exact and can be re-headed.
 *
 * WHAT IS NOT CLAIMED, AND IS THE REASON A REAL BOOT CAPTURES NOTHING TODAY.
 * The CPU does not touch the I2S FIFOs at all: /arm-io/i2s0's `dma-channels`
 * hands their physical addresses to the PL080. That controller IS modelled now
 * (core/src/soc/pl080.c), and a boot to userspace still records nothing -- the
 * reason moved upstream, and was measured rather than assumed. With
 * per-register logging on the DMAC window, a full boot writes
 * DMACConfiguration (+0x030) ZERO times, so setEnabled(true) never runs, the
 * command refcount never goes 0->1, and startDMACommand is never called. No
 * channel is ever enabled. A phone sitting at the lock screen is silent.
 *
 * So a zero here is a statement about what the guest was ASKED to do, not
 * about the DMA controller and not about audio. Getting a sample out needs
 * something that plays one -- which is downstream of touch, not of this file.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_AUDIO_CAPTURE_H
#define S5LBOX_AUDIO_CAPTURE_H

#include "soc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------- format --- */

/*
 * What a RIFF/WAVE header has to state. Every field must have come from the
 * device; there is no default and there is deliberately no constructor that
 * supplies one.
 *
 * `bits` is the CONTAINER width as it will be written, not an amplitude
 * resolution: 24-bit samples carried in 32-bit slots are bits = 32, because
 * that is what a reader has to step by. Getting this backwards would silently
 * halve or double the playback rate of an otherwise perfect capture.
 */
typedef struct {
    uint32_t rate_hz;
    uint16_t bits;        /* 8, 16, 24 or 32 */
    uint16_t channels;    /* 1..8            */
} audio_format_t;

#define WAV_HEADER_BYTES 44u

/* Bytes one frame occupies: channels * bits/8. Zero for an invalid format. */
uint32_t audio_block_align(const audio_format_t *fmt);

/*
 * Whether `fmt` is one this writer will emit a header for. Four requirements,
 * three of them about the FILE and one about the CAPTURE:
 *
 *   - bits in {8,16,24,32} and channels in 1..8 -- what canonical PCM RIFF
 *     expresses without a WAVE_FORMAT_EXTENSIBLE chunk this does not write;
 *   - rate_hz non-zero;
 *   - rate_hz * block_align must fit in uint32_t, because it IS a uint32_t
 *     field in the header and an overflowing byte rate is a corrupt file
 *     rather than a rejected one;
 *   - block_align must divide 4 or be divisible by 4. The capture appends
 *     whole 32-bit FIFO words, so any other value guarantees a data chunk that
 *     ends part-way through a frame. It is still not a hard guarantee of whole
 *     frames -- an odd word count with an 8-byte block align leaves half a
 *     frame -- so audio_sink_close() reports the remainder rather than padding
 *     it away. Padding would be fabricating a sample.
 */
bool audio_format_valid(const audio_format_t *fmt);

/*
 * The canonical 44-byte header, built into a caller's buffer. Pure: no I/O, no
 * allocation, no host byte order dependency -- every multi-byte field is
 * assembled a byte at a time, little-endian, so a big-endian host produces the
 * same file. `data_bytes` is the payload length, i.e. what goes in the `data`
 * chunk size; the RIFF size field is that plus 36.
 *
 * Returns false and touches nothing unless audio_format_valid(fmt) and
 * data_bytes <= UINT32_MAX - 36.
 */
bool wav_header_build(uint8_t out[WAV_HEADER_BYTES],
                      const audio_format_t *fmt, uint32_t data_bytes);

/* ------------------------------------------- what the device establishes --- */

#define AUDIO_DT_RATE_MAX 8u
#define AUDIO_DT_REG_MAX  64u

/*
 * The facts /arm-io/i2s0/audio0 states about itself, read out of the tree the
 * guest is actually given. Nothing here is interpreted -- the rates come from
 * PROPERTY NAMES (`sysclk-44100`, `sysclk-48000`), which is the one place in
 * this node where a number means what it says. The property VALUES are PLL or
 * divider words (0x00ac325e, 0x00bb6ccf against a 12,000,000 `clock-frequency`)
 * and are carried only so a reader can see them.
 */
typedef struct {
    bool     node_found;
    uint32_t rate[AUDIO_DT_RATE_MAX];   /* from `sysclk-<decimal>` names */
    uint32_t rate_value[AUDIO_DT_RATE_MAX];
    unsigned rates;
    unsigned rates_dropped;             /* past AUDIO_DT_RATE_MAX */
    uint32_t clock_frequency;           /* 0 when the property is absent */
    bool     have_clock_frequency;
    uint8_t  reg[AUDIO_DT_REG_MAX];     /* the node's `reg`, verbatim */
    unsigned reg_len;                   /* 0 when absent or over the cap */
} audio_dt_facts_t;

/*
 * Read those facts from a flattened Apple device tree. `path` is relative to
 * the root, e.g. "arm-io/i2s0/audio0". Returns false only when the blob does
 * not parse; a missing node is a successful read with node_found == false,
 * because "this firmware has no audio-data node" is itself a result.
 */
bool audio_dt_read(const uint8_t *blob, size_t len, const char *path,
                   audio_dt_facts_t *out);

typedef enum {
    AUDIO_FMT_OK = 0,             /* every field established; write a WAV   */
    AUDIO_FMT_NO_NODE,            /* no tree, or no audio-data node in it   */
    AUDIO_FMT_UNCONFIGURED,       /* the guest never wrote the controller   */
    AUDIO_FMT_RATE_UNDECIDED,     /* the node declares 0 or 2+ rates        */
    AUDIO_FMT_WIDTH_UNESTABLISHED /* nothing states width or channel count  */
} audio_fmt_status_t;

/*
 * Decide the capture format from the device, or say why it cannot be decided.
 *
 * The rules, in the order they are applied, so that the reported reason is the
 * FIRST thing that is missing rather than the last:
 *
 *   NO_NODE            the tree has no /arm-io/i2s0/audio0 to ask.
 *   UNCONFIGURED       the guest never wrote one of the seven I2S registers.
 *                      Whatever else is or is not derivable, a controller that
 *                      was never programmed has no format to report, and this
 *                      is the reason a reader most needs to see first.
 *   RATE_UNDECIDED     the node declares a number of rates other than one.
 *                      On 7E18 it declares TWO -- `sysclk-44100` and
 *                      `sysclk-48000` -- and nothing in this model says which
 *                      one a given transfer selected. That choice is in the
 *                      AppleARMIISCommand words the driver writes to +0x00,
 *                      +0x04, +0x30 and +0x40, and no field layout for those
 *                      is established (soc.h, "NO SEMANTICS ARE CLAIMED").
 *   WIDTH_UNESTABLISHED  no source at all for sample width or channel count.
 *                      The codec's map is not known past the six registers its
 *                      own driver reads, the seven controller words are opaque,
 *                      and audio0's `reg` blob is 32 undocumented bytes. This
 *                      is the honest terminal state, and it is why nothing but
 *                      a caller-supplied format reaches AUDIO_FMT_OK today.
 *
 * `out` is written only on AUDIO_FMT_OK.
 */
audio_fmt_status_t audio_format_derive(const audio_dt_facts_t *dt,
                                       const s5l_i2s_t *i2s,
                                       audio_format_t *out);

/* One line, for the run report. Never NULL. */
const char *audio_fmt_reason(audio_fmt_status_t st);

/* ---------------------------------------------------------------- sink --- */

#define AUDIO_PATH_MAX 1024u

/*
 * The default names, used when no work image names the run. Two of this
 * project's outputs have already been destroyed by a fixed path -- run86 over
 * run85's console, and firmware/screen.ppm every time -- so these are the
 * FALLBACK, never the first choice. See audio_sink_arm().
 */
#define AUDIO_WAV_PATH "audio-capture.wav"
#define AUDIO_RAW_PATH "audio-capture.raw"

typedef struct {
    /* Set by audio_sink_arm() and then never changed. */
    bool           armed;
    bool           have_fmt;      /* false => headerless raw dump */
    audio_format_t fmt;
    char           path[AUDIO_PATH_MAX];

    /* The stream. Opened by the first word, never at arm time. */
    FILE    *f;
    bool     failed;              /* open or write error; reported once */
    bool     closed;
    /* Both RIFF size fields are uint32_t. Past that the file stops being a WAV
     * rather than becoming a longer one, so appending stops and this latches.
     * `words` keeps counting: the count is the measurement, the file is only
     * its container, and a capped run must still report what it saw. */
    bool     capped;
    uint64_t payload_bytes;       /* bytes actually in the file */

    /*
     * The two counts that must never be collapsed into one. `words` answers
     * "did the guest transmit"; `nonzero_words` answers "was any of it audio".
     * A run with words > 0 and nonzero_words == 0 captured real, digital
     * silence, which is a genuine result and not a failure.
     *
     * `or_all`/`and_all` are the cheapest available evidence about the bits
     * themselves: a stuck channel, a sign-extension mistake or a byte-lane swap
     * shows up in them immediately and costs two instructions per word.
     */
    uint64_t words;
    uint64_t nonzero_words;
    uint32_t or_all;
    uint32_t and_all;
} audio_sink_t;

/*
 * Arm the sink. Resolves the output path and records the format decision; does
 * NOT create a file.
 *
 * PATH RULE, copied deliberately from uart_tee_byte() rather than reinvented.
 * When `work_image_path` is non-empty -- which is what --external-md gives, and
 * the harness refuses to reuse a work image -- the capture is written beside it
 * as `<work>.audio.wav` (or `.audio.raw`), a name unique per run by
 * construction. Otherwise the fixed name above is used and two runs from one
 * directory will overwrite each other, which is the existing behaviour of every
 * other fixed-name output here and is kept only so existing recipes still find
 * their file.
 *
 * `fmt` may be NULL, and that is the normal case today: it selects the
 * headerless dump. Passing a format that fails audio_format_valid() is treated
 * as NULL rather than as an error, because a bad declaration must not be able
 * to produce a header.
 *
 * This OVERWRITES the whole sink and does NOT close an open stream, because it
 * is also the initialiser and cannot read fields that may be uninitialised.
 * Arm once; a caller that might arm twice must audio_sink_close() in between.
 */
void audio_sink_arm(audio_sink_t *s, const char *work_image_path,
                    const audio_format_t *fmt);

/*
 * Append one 32-bit FIFO word. Safe to call on an unarmed or failed sink -- the
 * counts still advance, so "the guest transmitted but the file could not be
 * written" stays distinguishable from "the guest transmitted nothing".
 */
void audio_sink_word(audio_sink_t *s, uint32_t word);

/*
 * Finish the file: patch the two size fields and close. Returns true when a
 * complete file is on disk, false when nothing was captured or something
 * failed. Idempotent.
 */
bool audio_sink_close(audio_sink_t *s);

/* Payload bytes actually in the file: 4 per word accepted. Below 4 * words
 * when the sink was never armed, failed, or hit the cap. */
uint64_t audio_sink_data_bytes(const audio_sink_t *s);

/*
 * Bytes of a trailing partial frame, or 0. Non-zero only when block_align does
 * not divide the payload; reported rather than padded away.
 */
uint32_t audio_sink_partial_frame(const audio_sink_t *s);

#endif /* S5LBOX_AUDIO_CAPTURE_H */
