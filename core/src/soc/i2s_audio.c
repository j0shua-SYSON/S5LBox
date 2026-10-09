/* Timed WM8991 PCM transport. Register facts: S5L8900 7E18 device tree and
 * AppleS5L8900XI2SController; WM8990/91 clock/volume register definitions.
 * This is a digital transport, not a model of analogue noise or telephony.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "soc.h"
#include <math.h>

static bool running(const s5l_i2s_t *s, unsigned d) {
    return (s->regs[0] & 1u) && (s->regs[d ? 4 : 2] & 2u);
}

static unsigned sample_bits(const s5l_i2s_t *s, unsigned d) {
    /* The N82 stock stream and openiBoot establish code 0 = 16 bits. The
     * codec's 16/20/24/32 encodings are NOT evidence for the SoC's other codes.
     * Do not invent packing for an unobserved serial-controller mode. Guest
     * AudioQueue/CoreAudio convert app sample formats to this hardware stream. */
    return ((s->regs[d ? 3 : 1] >> 5) & 3u) == 0u ? 16u : 0u;
}

static uint32_t pop(s5l_i2s_audio_t *a, unsigned d, unsigned n) {
    uint32_t v = 0;
    for (unsigned j = 0; j < n; j++) {
        v |= (uint32_t)a->fifo[d][a->head[d]] << (8u * j);
        a->head[d] = (a->head[d] + 1u) % S5L_I2S_FIFO_BYTES;
    }
    a->count[d] -= n;
    return v;
}

static void push(s5l_i2s_audio_t *a, unsigned d, uint32_t v, unsigned n) {
    for (unsigned j = 0; j < n; j++) {
        unsigned at = (a->head[d] + a->count[d]++) % S5L_I2S_FIFO_BYTES;
        a->fifo[d][at] = (uint8_t)(v >> (8u * j));
    }
}

bool s5l_i2s_dma_ready(const s5l_i2s_t *s, unsigned width, bool source) {
    if (!s || (width != 1u && width != 2u && width != 4u)) return false;
    unsigned d = source ? 1u : 0u;
    /* TX can be primed before startTransfer enables its serial clock. */
    if (!(s->regs[0] & 1u)) return false;
    return source ? s->audio.count[d] >= width :
                    s->audio.count[d] <= S5L_I2S_FIFO_BYTES - width;
}

uint32_t s5l_i2s_read_width(s5l_i2s_t *s, uint32_t off, unsigned width) {
    if (!s) return 0;
    if (off != S5L_I2S_RX_FIFO_OFF) return s5l_i2s_read(s, off);
    s->reads++;
    if (width != 1u && width != 2u && width != 4u) return 0;
    if (s->audio.count[1] < width) { s->audio.xruns[1]++; return 0; }
    return pop(&s->audio, 1, width);
}

void s5l_i2s_write_width(s5l_i2s_t *s, uint32_t off, uint32_t v, unsigned width) {
    if (!s) return;
    if (off != S5L_I2S_TX_FIFO_OFF) {
        s5l_i2s_write(s, off, v);
        if (off == 0u || off == 8u || off == 0x34u) {
            for (unsigned d = 0; d < 2; d++) if (!running(s, d)) {
                s->audio.head[d] = s->audio.count[d] = 0;
                s->audio.phase[d] = 0;
            }
        }
        return;
    }
    s->writes++;
    if (width != 1u && width != 2u && width != 4u) return;
    if (s->audio.count[0] > S5L_I2S_FIFO_BYTES - width) {
        s->audio.xruns[0]++; return;
    }
    push(&s->audio, 0, v, width);
}

static uint32_t codec_rate(const s5l_wm8991_t *c, unsigned d) {
    /* N82's codec reference clock is 12 MHz. PLL = ref * (N + K/65536)/4.
     * Fractional BCLK divisors are represented doubled to retain precision. */
    static const unsigned bdiv2[] = {2,3,4,6,8,11,12,16,22,24,32,44,48,64,88,96};
    uint64_t numerator = UINT64_C(12000000), denominator = 1;
    if (c->regs[7] & 0x4000u) {
        if (!(c->regs[2] & 0x8000u)) return 0;
        uint32_t pll = c->regs[0x3c];
        uint32_t k = (pll & 0x80u) ?
            ((c->regs[0x3d] & 255u) << 8) | (c->regs[0x3e] & 255u) : 0;
        numerator *= ((pll & 15u) << 16) + k;
        denominator *= UINT64_C(262144) * ((pll & 0x40u) ? 2u : 1u);
    }
    unsigned mdiv = (c->regs[7] >> 11) & 3u;
    if (mdiv == 2u) denominator *= 2u;
    else if (mdiv != 0u) return 0; /* reserved clock mode */
    unsigned lrdiv = c->regs[d ? 8 : 9] & 0x7ffu;
    if (!lrdiv) return 0;
    numerator *= 2u;
    denominator *= bdiv2[(c->regs[6] >> 1) & 15u] * lrdiv;
    uint64_t rate = (numerator + denominator / 2u) / denominator;
    return rate >= 8000u && rate <= 96000u ? (uint32_t)rate : 0;
}

static float db_gain(float db) { return powf(10.f, db / 20.f); }
static float out_gain(unsigned v) {
    /* Output PGAs: codes 0..47 mute, 48=-73dB, 121=0dB, 127=+6dB. */
    v &= 127u;
    return v < 48u ? 0.f : db_gain((float)v - 121.f);
}

static void configure(s5l_i2s_t *s, const s5l_wm8991_t *c) {
    if (s->codec_generation == c->reg_writes) return;
    s->codec_generation = c->reg_writes;
    for (unsigned d = 0; d < 2; d++) {
        uint32_t rate = codec_rate(c, d);
        if (s->audio.rate[d] != rate) s->audio.phase[d] = 0;
        s->audio.rate[d] = rate;
        for (unsigned ch = 0; ch < 2; ch++) {
            unsigned vol = c->regs[(d ? 0x0f : 0x0b) + ch] & 255u;
            float gain = vol ? db_gain(((float)vol - 192.f) * 0.375f) : 0.f;
            if (d) {
                if (!(c->regs[2] & (2u >> ch))) gain = 0;
            } else {
                if (!(c->regs[3] & (2u >> ch)) || (c->regs[0xa] & 4u)) gain = 0;
                float analogue = 0;
                if (c->regs[1] & (0x200u >> ch))
                    analogue = out_gain(c->regs[0x1c + ch]);
                if ((c->regs[1] & 0x1000u) && (c->regs[0x36] & (2u >> ch))) {
                    unsigned attn = c->regs[0x22] & 3u;
                    float speaker = attn == 3u ? 0 :
                        out_gain(c->regs[0x26]) * db_gain(-6.f * (float)attn);
                    if (speaker > analogue) analogue = speaker;
                }
                if (c->regs[1] & (0x800u >> ch)) {
                    float receiver = out_gain(c->regs[0x20 + ch]);
                    if (receiver > analogue) analogue = receiver;
                }
                gain *= analogue;
            }
            s->gain[d][ch] = gain;
        }
    }
}

static float decode(uint32_t v, unsigned bits) {
    /* Right-justified little-endian DMA words; no implementation-defined
     * signed shift or float-to-int overflow at negative full scale. */
    uint64_t mask = (UINT64_C(1) << bits) - 1u;
    int64_t value = (int64_t)(v & mask);
    if (value & (INT64_C(1) << (bits - 1u))) value -= INT64_C(1) << bits;
    return (float)((double)value / (double)(UINT64_C(1) << (bits - 1u)));
}

static uint32_t encode(float v, unsigned bits) {
    if (!isfinite(v)) v = 0;
    double scale = (double)(UINT64_C(1) << (bits - 1u));
    double n = (double)v * scale;
    if (n < -scale) n = -scale;
    if (n > scale - 1) n = scale - 1;
    return (uint32_t)(int64_t)n;
}

bool s5l_i2s_audio_tick(s5l_i2s_t *s, const s5l_wm8991_t *c,
                       uint32_t ticks, uint32_t tick_hz) {
    if (!s || !c || !tick_hz) return false;
    configure(s, c);
    bool clock_edge = false;
    for (unsigned d = 0; d < 2; d++) {
        /* LRCLK exists before TXCOM/RXCOM start. The stock slave-mode driver
         * waits for two GPIO clock edges BEFORE issuing its first DMA request
         * (7E18 c05a3948..c05a3a04). Gating the clock on DMA deadlocks it. */
        bool active = running(s, d) && sample_bits(s, d);
        if (!(s->regs[0] & 1u) || !s->audio.rate[d] || (d && !active)) continue;
        uint64_t phase = s->audio.phase[d] + (uint64_t)ticks * s->audio.rate[d];
        uint64_t frames = phase / tick_hz;
        s->audio.phase[d] = phase % tick_hz;
        if (!d && frames && (s->regs[1] & (1u << 20))) clock_edge = true;
        if (!active) continue;
        /* Normal machine stepping stops at each sample edge. Bound externally
         * supplied huge tick jumps as well: count lost time, never allocate or
         * loop for millions of frames on a corrupt/hostile guest clock jump. */
        if (frames > 4096u) { s->audio.xruns[d] += frames - 4096u; frames = 4096u; }
        unsigned bits = sample_bits(s, d), width = bits == 16u ? 2u : 4u;
        while (frames--) {
            float samples[2] = {0, 0};
            if (!d) {
                if (s->audio.count[0] >= width * 2u)
                    for (unsigned ch = 0; ch < 2; ch++)
                        samples[ch] = decode(pop(&s->audio, 0, width), bits) * s->gain[0][ch];
                else s->audio.xruns[0]++;
            }
            if (s->frame) s->frame(s->frame_ctx, d, s->audio.rate[d], samples);
            if (d) {
                if (s->audio.count[1] <= S5L_I2S_FIFO_BYTES - width * 2u)
                    for (unsigned ch = 0; ch < 2; ch++)
                        push(&s->audio, 1, encode(samples[ch] * s->gain[1][ch], bits), width);
                else s->audio.xruns[1]++;
            }
            s->audio.frames[d]++;
        }
    }
    return clock_edge;
}

uint32_t s5l_i2s_clock_next(const s5l_i2s_t *s, uint32_t tick_hz) {
    if (!s || !tick_hz || !(s->regs[0] & 1u) ||
        !(s->regs[1] & (1u << 20)) || !s->audio.rate[0]) return 0;
    uint32_t rate = s->audio.rate[0];
    return (uint32_t)((tick_hz - s->audio.phase[0] + rate - 1u) / rate);
}

uint32_t s5l_i2s_audio_next(const s5l_i2s_t *s, uint32_t tick_hz) {
    uint32_t next = 0;
    if (!s || !tick_hz) return 0;
    for (unsigned d = 0; d < 2; d++) {
        uint32_t rate = s->audio.rate[d];
        if (!running(s, d) || !rate || !sample_bits(s, d)) continue;
        uint64_t remaining = tick_hz - s->audio.phase[d];
        uint32_t ticks = (uint32_t)((remaining + rate - 1u) / rate);
        if (!ticks) ticks = 1;
        if (!next || ticks < next) next = ticks;
    }
    return next;
}
