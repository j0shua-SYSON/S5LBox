/* SPI programming, FIFO preparation and acknowledgement; no transfer defaults.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_spi.h"
#include <string.h>

/* Only these configuration fields may change while a request is armed.
 * Interrupt service and automatic or DMA transfers need a clocked model. */
#define SPI_MODE_MASK UINT32_C(0x60)
#define SPI_UNMODELED_ACTIVE UINT32_C(0x200181)

static bool spi_pio_request(uint32_t config) {
    return (config&0x18u)==0x18u && !(config&SPI_UNMODELED_ACTIVE) &&
        (config&SPI_MODE_MASK)<=0x20u;
}

static void spi_fifo_reset(s5l8920_spi_fifo_t *fifo) {
    memset(fifo,0,sizeof *fifo);
    fifo->known=true;
}

void s5l8920_spi_reset(s5l8920_spi_t *s) {
    if (s) memset(s,0,sizeof *s);
}

bool s5l8920_spi_read(s5l8920_spi_t *s, uint32_t offset, uint32_t *value) {
    if (!s || !value) return false;
    if (offset==0u && s->control_readable) { *value=0u; return true; }
    if (offset==12u && s->pin_programmed) { *value=s->pin; return true; }
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
        /* Validate before either reset: refused requests are atomic. RUN is
         * retained as an armed request, not a source of serial clocks/data. */
        if ((value&1u) && (!s->config_programmed || !s->divider_programmed ||
            !s->word_delay_programmed || !s->pin_programmed || !s->divider ||
            !spi_pio_request(s->config) || (!s->tx.known && !(value&4u)) ||
            (!s->rx.known && !(value&8u)))) return false;
        if (value&4u) {
            spi_fifo_reset(&s->tx);
            s->tx_count_programmed=false;
        }
        if (value&8u) {
            spi_fifo_reset(&s->rx);
            s->rx_count_programmed=false;
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
        if (!s->stopped && (!spi_pio_request(value) ||
            ((value^s->config)&~SPI_MODE_MASK))) return false;
        s->config=value; s->config_programmed=true;
        return true;
    }
    if (offset==0x30u) {
        if (!s->stopped && value!=s->divider) return false;
        s->divider=value; s->divider_programmed=true;
        return true;
    }
    if (offset==0x38u) {
        if (!s->stopped && value!=s->word_delay) return false;
        s->word_delay=value; s->word_delay_programmed=true;
        return true;
    }
    if (offset==12u) {
        if (value&~2u) return false;
        s->pin=value; s->pin_programmed=true;
        return true;
    }
    if (offset==8u) {
        if (value&~(S5L8920_SPI_EVENT_MASK|S5L8920_SPI_FIFO_LEVEL_MASK)) return false;
        s->cleared_events|=value&S5L8920_SPI_EVENT_MASK;
        return true;
    }
    if (offset==0x4cu && s->tx.known) {
        s->tx_words=value; s->tx_count_programmed=true;
        return true;
    }
    if (offset==0x34u && s->rx.known) {
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
