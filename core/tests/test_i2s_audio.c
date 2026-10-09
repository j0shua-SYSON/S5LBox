#include "soc.h"
#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
typedef struct { unsigned calls[2]; float last[2]; } capture_t;
static void frame(void *p, unsigned d, uint32_t rate, float samples[2]) {
    capture_t *c = p; c->calls[d]++;
    CHECK(rate == 44100u || rate == 48000u);
    if (!d) memcpy(c->last, samples, sizeof c->last);
    else { samples[0] = 0.25f; samples[1] = -0.5f; }
}
static void setup(s5l8900_t *m, capture_t *capture) {
    CHECK(s5l8900_init(m, 0, 65536));
    s5l_wm8991_t *c = &m->codec;
    c->regs[1] = 0x1003; c->regs[2] = 0x8003; c->regs[3] = 3;
    c->regs[6] = 0x1ce; c->regs[7] = 0x5000;
    c->regs[8] = c->regs[9] = 0x8020;
    c->regs[0xb] = c->regs[0xc] = c->regs[0xf] = c->regs[0x10] = 192;
    /* Stock guest leaves speaker PGA unwritten: its reset value is unity. */
    c->regs[0x36] = 3;
    c->regs[0x3c] = 0x87; c->regs[0x3d] = 0x86; c->regs[0x3e] = 0xc2;
    c->reg_writes++;
    s5l_i2s_t *s = &m->i2s[0];
    s->frame = frame; s->frame_ctx = capture;
    s5l_i2s_write_width(s, 0, 1, 4);
    s5l_i2s_write_width(s, 4, 0x01100301, 4);
    s5l_i2s_write_width(s, 8, 6, 4);
    s5l_i2s_write_width(s, 0x34, 6, 4);
    s5l_i2s_audio_tick(s, c, 0, 6000000);
}
static void test_transport(void) {
    s5l8900_t m; capture_t cap = {0}; setup(&m, &cap);
    s5l_i2s_t *s = &m.i2s[0];
    CHECK(s->audio.rate[0] == 44100 && s->audio.rate[1] == 44100);
    CHECK(s5l_i2s_audio_next(s, 6000000) == 137);
    CHECK(!s5l_i2s_dma_ready(s, 2, true));
    s5l_i2s_write_width(s, 0x10, 0x4000, 2);
    s5l_i2s_write_width(s, 0x10, 0xc000, 2);
    s5l_i2s_audio_tick(s, &m.codec, 136, 6000000);
    CHECK(cap.calls[0] == 0 && s->audio.count[0] == 4);
    s5l_i2s_audio_tick(s, &m.codec, 1, 6000000);
    CHECK(cap.calls[0] == 1 && cap.last[0] == 0.5f && cap.last[1] == -0.5f);
    CHECK(s5l_i2s_dma_ready(s, 4, true));
    CHECK(s5l_i2s_read_width(s, 0x38, 4) == 0xc0002000u);
    CHECK(!s5l_i2s_dma_ready(s, 1, true));
    /* Mute and volume updates are not bypassed by host playback. */
    m.codec.regs[0xa] = 4; m.codec.reg_writes++;
    s5l_i2s_write_width(s, 0x10, 0x40004000, 4);
    s5l_i2s_audio_tick(s, &m.codec, 137, 6000000);
    CHECK(cap.last[0] == 0 && cap.last[1] == 0);
    m.codec.regs[0xa] = 0; m.codec.regs[0x26] = 115;
    m.codec.written[0x26] = 1; m.codec.reg_writes++;
    s5l_i2s_write_width(s, 0x10, 0x40004000, 4);
    s5l_i2s_audio_tick(s, &m.codec, 137, 6000000);
    CHECK(fabsf(cap.last[0] - 0.2505936f) < 0.00001f);
    m.codec.regs[0x26] = 0; m.codec.reg_writes++;
    s5l_i2s_write_width(s, 0x10, 0x40004000, 4);
    s5l_i2s_audio_tick(s, &m.codec, 137, 6000000);
    CHECK(cap.last[0] == 0 && cap.last[1] == 0); /* explicit PGA mute */
    /* Full FIFO applies backpressure; an overflow never writes out of bounds. */
    for (unsigned i = 0; i < 64; i++) s5l_i2s_write_width(s, 0x10, i, 4);
    CHECK(!s5l_i2s_dma_ready(s, 2, false));
    s5l_i2s_write_width(s, 0x10, 42, 4);
    CHECK(s->audio.count[0] == S5L_I2S_FIFO_BYTES && s->audio.xruns[0] == 1);
    /* Old audio cannot reappear after stop/restart. */
    s5l_i2s_write_width(s, 8, 0, 4);
    CHECK(s->audio.count[0] == 0 && s->audio.phase[0] == 0);
    unsigned before = cap.calls[0];
    s5l_i2s_write_width(s, 4, 0x01100321, 4);
    s5l_i2s_write_width(s, 8, 6, 4);
    s5l_i2s_audio_tick(s, &m.codec, 137, 6000000);
    CHECK(cap.calls[0] == before); /* unknown controller packing is not guessed */
    s5l8900_free(&m);
}
static void test_clock_and_snapshot(void) {
    s5l8900_t a, b; capture_t ca = {0}, cb = {0}; setup(&a, &ca); setup(&b, &cb);
    s5l_i2s_t *s = &a.i2s[0];
    /* Exact count over a second, independent of fractional TB edges. */
    s->frame = NULL;
    for (unsigned i = 0; i < 1000; i++) s5l_i2s_audio_tick(s, &a.codec, 6000, 6000000);
    CHECK(s->audio.frames[0] == 44100 && s->audio.frames[1] == 44100);
    CHECK(s->audio.phase[0] == 0);
    /* 48 kHz PLL = 8 + 12583/65536 with the same divisors. */
    a.codec.regs[0x3c] = 0x88; a.codec.regs[0x3d] = 0x31;
    a.codec.regs[0x3e] = 0x27; a.codec.reg_writes++;
    s5l_i2s_audio_tick(s, &a.codec, 10, 6000000);
    CHECK(s->audio.rate[0] == 48000);
    s5l_i2s_write_width(s, 0x10, 0x1234, 2);
    uint8_t *buf = NULL; size_t len = 0;
    CHECK(snapshot_save_mem(&a, &buf, &len) == SNAP_OK);
    CHECK(snapshot_load_mem(&b, buf, len) == SNAP_OK);
    CHECK(memcmp(&s->audio, &b.i2s[0].audio, sizeof s->audio) == 0);
    CHECK(b.i2s[0].frame == frame && b.i2s[0].frame_ctx == &cb);
    s5l_i2s_audio_tick(&b.i2s[0], &b.codec, 0, 6000000);
    CHECK(s->audio.phase[0] == b.i2s[0].audio.phase[0]);
    free(buf); buf = NULL;
    s->audio.count[0] = S5L_I2S_FIFO_BYTES + 1;
    CHECK(snapshot_save_mem(&a, &buf, &len) == SNAP_ERR_CORRUPT);
    s5l8900_free(&a); s5l8900_free(&b);
}
static void test_real_dma_and_batched_time(void) {
    s5l8900_t m; capture_t cap = {0}; setup(&m, &cap);
    m.cpu_hz = m.tb_hz = 6000000;
    m.codec.regs[0x3c] = 0x88; m.codec.regs[0x3d] = 0x31;
    m.codec.regs[0x3e] = 0x27; m.codec.reg_writes++;
    for (unsigned i = 0; i < 128; i++) m.bus.write32(m.bus.ctx, 0x1000 + i * 4u, 0xc0004000);
    s5l_pl080_chan_t *tx = &m.dmac[0].ch[0];
    tx->src = 0x1000; tx->dst = 0x3ca00010;
    tx->ctrl = 0x84249000u | 256u; tx->cfg = 0x8801;
    s5l_pl080_chan_t *rx = &m.dmac[1].ch[0];
    rx->src = 0x3ca00038; rx->dst = 0x2000;
    rx->ctrl = 0x88249000u | 64u; rx->cfg = 0x9001;
    m.dmac[0].config = m.dmac[1].config = 1;
    s5l8900_tick(&m, 0);
    CHECK(m.dmac[0].bytes_moved == 256 && m.dmac[0].raw_tc == 0);
    CHECK(m.dmac[1].bytes_moved == 0);
    const s5l_wake_source_t *sources = NULL;
    unsigned n = s5l8900_wake_sources(&sources), seen = 0;
    for (unsigned i = 0; i < n; i++) if (sources[i].line == S5L8900_IRQ_DMAC0) {
        uint32_t edge = 0;
        CHECK(sources[i].next_edge(&m, &edge) == S5L_WAKE_AT && edge == 125);
        seen++;
    }
    CHECK(seen == 1);
    /* A 1 ms host-clock batch must NOT consume 48 frames from a 64-frame
     * FIFO and only then refill it; repeat twice to cross its capacity. */
    s5l8900_tick(&m, 6000);
    s5l8900_tick(&m, 6000);
    CHECK(cap.calls[0] == 96 && cap.last[0] == 0.5f && cap.last[1] == -0.5f);
    CHECK(m.i2s[0].audio.xruns[0] == 0);
    CHECK(m.dmac[0].bytes_moved == 512 && (m.dmac[0].raw_tc & 1u));
    CHECK(m.dmac[1].bytes_moved == 128 && (m.dmac[1].raw_tc & 1u));
    CHECK(m.bus.read32(m.bus.ctx, 0x2000) == 0xc0002000u);
    s5l8900_free(&m);
}
static void test_clock_before_dma(void) {
    s5l8900_t m; capture_t cap = {0}; setup(&m, &cap);
    m.cpu_hz = m.tb_hz = 6000000;
    s5l_i2s_write_width(&m.i2s[0], 8, 0, 4);
    s5l_i2s_write_width(&m.i2s[0], 0x34, 0, 4);
    s5l_gpioic_write(&m.gpioic, GPIOIC_INTEN + 4 * 4, 1u << 6);
    const s5l_wake_source_t *sources = NULL;
    unsigned n = s5l8900_wake_sources(&sources), seen = 0;
    for (unsigned i = 0; i < n; i++) if (!strcmp(sources[i].name, "gpio-group4")) {
        uint32_t edge = 0;
        CHECK(sources[i].next_edge(&m, &edge) == S5L_WAKE_AT && edge == 137);
        seen++;
    }
    CHECK(seen == 1);
    s5l8900_tick(&m, 136);
    CHECK(!(m.gpioic.stat[4] & (1u << 6)));
    s5l8900_tick(&m, 1);
    CHECK(m.gpioic.stat[4] & (1u << 6));
    CHECK(s5l_gpioic_group_irq(&m.gpioic, 4));
    CHECK(cap.calls[0] == 0 && cap.calls[1] == 0);
    CHECK(m.i2s[0].audio.frames[0] == 0 && m.i2s[0].audio.count[1] == 0);
    s5l_gpioic_write(&m.gpioic, GPIOIC_INTSTAT + 4 * 4, 1u << 6);
    s5l8900_tick(&m, 0);
    CHECK(!(m.gpioic.stat[4] & (1u << 6)));
    s5l8900_tick(&m, 136);
    CHECK(m.gpioic.stat[4] & (1u << 6)); /* second edge unblocks stock start */
    s5l_gpioic_write(&m.gpioic, GPIOIC_INTEN + 4 * 4, 0);
    CHECK(!s5l_gpioic_group_irq(&m.gpioic, 4));
    s5l_i2s_write_width(&m.i2s[0], 0, 0, 4);
    CHECK(s5l_i2s_clock_next(&m.i2s[0], m.tb_hz) == 0);
    s5l8900_free(&m);
}
int main(void) {
    test_transport(); test_clock_and_snapshot(); test_real_dma_and_batched_time();
    test_clock_before_dma();
    printf("I2S PCM: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
