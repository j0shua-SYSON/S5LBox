/* SPI programming, FIFO preparation and acknowledgement; no transfer defaults.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_spi.h"
#include <string.h>

/* A configured error-free word link supplies clocks/data and explicit FIFO
 * thresholds. Unconfigured controllers retain the programming-only guard. */
#define SPI_MODE_MASK UINT32_C(0x60)
#define SPI_IRQ_MASK UINT32_C(0x200180)
#define SPI_RX_COMPLETE UINT32_C(1)
#define SPI_THRESHOLD UINT32_C(2)
#define SPI_TX_COMPLETE UINT32_C(0x400000)

static bool spi_pio_request(const s5l8920_spi_t *s,uint32_t config) {
    return (config&0x18u)==0x18u && !(config&1u) &&
        (s->link_configured || !(config&SPI_IRQ_MASK)) &&
        (config&SPI_MODE_MASK)<=0x20u;
}

static void spi_fifo_reset(s5l8920_spi_fifo_t *fifo) {
    memset(fifo,0,sizeof *fifo);
    fifo->known=true;
}

static bool spi_link_ready(const s5l8920_spi_t *s) {
    return s && s->link_configured && s->control_programmed && s->config_programmed &&
        s->divider_programmed && s->divider && s->word_delay_programmed && s->pin_programmed &&
        s->tx.known && s->rx.known && s->tx_count_programmed && s->rx_count_programmed &&
        spi_pio_request(s,s->config) && !(s->config&0x2000u);
}

static uint32_t spi_status(const s5l8920_spi_t *s) {
    uint32_t events=s->pending;
    /* Thresholds are explicit link inputs. This cause is a level; completion
     * causes latch independently until W1C. Error frames/overrun are refused. */
    if (!s->stopped && ((s->tx_words && s->tx.count<=s->tx_low) || s->rx.count>=s->rx_high))
        events|=SPI_THRESHOLD;
    return events|(s->tx.count<<6)|(s->rx.count<<11);
}

void s5l8920_spi_reset(s5l8920_spi_t *s) {
    if (s) memset(s,0,sizeof *s);
}

bool s5l8920_spi_read(s5l8920_spi_t *s, uint32_t offset, uint32_t *value) {
    if (!s || !value) return false;
    if (offset==0u && s->control_readable) { *value=0u; return true; }
    if (offset==12u && s->pin_programmed) { *value=s->pin; return true; }
    if (offset==8u && spi_link_ready(s)) { *value=spi_status(s); return true; }
    if (offset==0x20u && s->rx.known && s->rx.count) {
        *value=s->rx.words[s->rx.head];
        s->rx.head=(s->rx.head+1u)%S5L8920_SPI_FIFO_DEPTH;
        s->rx.count--;
        return true;
    }
    return false;
}

bool s5l8920_spi_write(s5l8920_spi_t *s, uint32_t offset, uint32_t value) {
    if (!s) return false;
    if (offset==0u) {
        if (value&~0xdu) return false;
        if ((value&12u) && (s->busy || s->delay_remaining)) return false;
        /* Validate before either reset: refused requests are atomic. RUN is
         * retained as an armed request, not a source of serial clocks/data. */
        if ((value&1u) && (!s->config_programmed || !s->divider_programmed ||
            !s->word_delay_programmed || !s->pin_programmed || !s->divider ||
            !spi_pio_request(s,s->config) || (!s->tx.known && !(value&4u)) ||
            (!s->rx.known && !(value&8u)))) return false;
        if (value&4u) {
            spi_fifo_reset(&s->tx);
            s->tx_count_programmed=false;
            s->pending&=~SPI_TX_COMPLETE;
        }
        if (value&8u) {
            spi_fifo_reset(&s->rx);
            s->rx_count_programmed=false;
            s->pending&=~SPI_RX_COMPLETE;
        }
        s->control_programmed=true;
        s->control_readable=value==0u;
        s->stopped=!(value&1u);
        if (value) s->cleared_events=0u;
        return true;
    }
    if (!s->control_programmed) return false;
    if (offset==4u) {
        uint32_t master=value&0x18u;
        if ((value&~S5L8920_SPI_CONFIG_MASK) || (master!=0u && master!=0x18u) ||
            ((value>>5)&3u)==3u || ((value>>15)&3u)==3u) return false;
        if ((!s->stopped || s->busy) && (!spi_pio_request(s,value) ||
            ((value^s->config)&~(SPI_IRQ_MASK|(s->busy?0u:SPI_MODE_MASK))))) return false;
        if (!s->stopped && (value&SPI_IRQ_MASK) && !spi_link_ready(s)) return false;
        s->config=value; s->config_programmed=true;
        return true;
    }
    if (offset==0x30u) {
        if ((!s->stopped || s->busy) && value!=s->divider) return false;
        s->divider=value; s->divider_programmed=true;
        return true;
    }
    if (offset==0x38u) {
        if ((!s->stopped || s->busy) && value!=s->word_delay) return false;
        s->word_delay=value; s->word_delay_programmed=true;
        return true;
    }
    if (offset==12u) {
        if (value&~2u) return false;
        if (s->busy && value!=s->pin) return false;
        s->pin=value; s->pin_programmed=true;
        return true;
    }
    if (offset==8u) {
        if (value&~(S5L8920_SPI_EVENT_MASK|S5L8920_SPI_FIFO_LEVEL_MASK)) return false;
        s->cleared_events|=value&S5L8920_SPI_EVENT_MASK;
        s->pending&=~(value&S5L8920_SPI_EVENT_MASK);
        return true;
    }
    if (offset==0x4cu && s->tx.known) {
        if (s->busy) return false;
        s->tx_words=value; s->tx_count_programmed=true;
        return true;
    }
    if (offset==0x34u && s->rx.known) {
        if (s->busy) return false;
        s->rx_words=value; s->rx_count_programmed=true;
        return true;
    }
    if (offset==0x10u && s->tx.known && s->config_programmed &&
        (s->config&SPI_MODE_MASK)==0x20u && s->tx.count<S5L8920_SPI_FIFO_DEPTH) {
        unsigned tail=(s->tx.head+s->tx.count)%S5L8920_SPI_FIFO_DEPTH;
        s->tx.words[tail]=value;
        s->tx.count++;
        return true;
    }
    return false;
}

bool s5l8920_spi_configure_link(s5l8920_spi_t *s,unsigned tx_low,unsigned rx_high) {
    if (!s || tx_low>=S5L8920_SPI_FIFO_DEPTH || !rx_high || rx_high>S5L8920_SPI_FIFO_DEPTH) return false;
    if (s->link_configured) return s->tx_low==tx_low && s->rx_high==rx_high;
    if (s->control_programmed) return false;
    s->tx_low=tx_low;s->rx_high=rx_high;s->link_configured=true;
    return true;
}

bool s5l8920_spi_irq(const s5l8920_spi_t *s) {
    if (!spi_link_ready(s)) return false;
    uint32_t events=spi_status(s);
    return ((s->config&0x80u) && (events&SPI_RX_COMPLETE)) ||
        ((s->config&0x100u) && (events&SPI_THRESHOLD)) ||
        ((s->config&0x200000u) && (events&SPI_TX_COMPLETE));
}

bool s5l8920_spi_serial_clock(s5l8920_spi_t *s,uint64_t cycles,
    const uint32_t *received,size_t received_count,uint32_t *transmitted,size_t capacity,size_t *count) {
    if (!spi_link_ready(s) || !count || (!received && received_count) || (!transmitted && capacity)) return false;
    s5l8920_spi_t next=*s;
    uint32_t output[S5L8920_SPI_FIFO_DEPTH+1u];
    size_t done=0u,used=0u;
    unsigned bits=8u<<((s->config>>15)&3u);
    uint32_t mask=bits==32u?UINT32_MAX:((1u<<bits)-1u);
    while (cycles && !next.stopped && !next.delay_remaining) {
        if (!next.busy) {
            if (!next.tx_words || !next.tx.count) break;
            if (next.pin!=0u) return false;
            next.shift_word=next.tx.words[next.tx.head]&mask;
            next.tx.head=(next.tx.head+1u)%S5L8920_SPI_FIFO_DEPTH;
            next.tx.count--;
            next.bits_remaining=bits;next.busy=true;
        }
        if (cycles<next.bits_remaining) {
            next.bits_remaining-=(unsigned)cycles;
            break;
        }
        cycles-=next.bits_remaining;
        if (done>=capacity) return false;
        if (next.rx_words) {
            if (used>=received_count || next.rx.count>=S5L8920_SPI_FIFO_DEPTH) return false;
            unsigned tail=(next.rx.head+next.rx.count)%S5L8920_SPI_FIFO_DEPTH;
            next.rx.words[tail]=received[used++]&mask;next.rx.count++;
            if (!--next.rx_words) next.pending|=SPI_RX_COMPLETE;
        }
        output[done++]=next.shift_word;
        next.busy=false;next.bits_remaining=0u;
        if (!--next.tx_words) next.pending|=SPI_TX_COMPLETE;
        if (next.tx_words) next.delay_remaining=next.word_delay;
    }
    if (done) memcpy(transmitted,output,done*sizeof *output);
    *s=next;*count=done;
    return true;
}

bool s5l8920_spi_delay_clock(s5l8920_spi_t *s,uint64_t cycles) {
    if (!spi_link_ready(s)) return false;
    if (!s->stopped) s->delay_remaining=cycles>=s->delay_remaining?0u:s->delay_remaining-(uint32_t)cycles;
    return true;
}
