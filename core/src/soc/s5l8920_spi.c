/* SPI stopped-state programming and event acknowledgement; no transfer defaults.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_spi.h"
#include <string.h>

void s5l8920_spi_reset(s5l8920_spi_t *s) {
    if (s) memset(s,0,sizeof *s);
}

bool s5l8920_spi_read(const s5l8920_spi_t *s, uint32_t offset, uint32_t *value) {
    if (!s || !value) return false;
    if (offset==0u && s->stopped) { *value=0u; return true; }
    if (offset==12u && s->pin_programmed) { *value=s->pin; return true; }
    return false;
}

bool s5l8920_spi_write(s5l8920_spi_t *s, uint32_t offset, uint32_t value) {
    if (!s) return false;
    if (offset==0u) {
        if (value) return false;
        s->stopped=true;
        return true;
    }
    if (!s->stopped) return false;
    if (offset==4u) {
        uint32_t master=value&0x18u;
        if ((value&~S5L8920_SPI_CONFIG_MASK) || (master!=0u && master!=0x18u) ||
            ((value>>5)&3u)==3u || ((value>>15)&3u)==3u) return false;
        s->config=value; s->config_programmed=true;
        return true;
    }
    if (offset==0x30u) {
        s->divider=value; s->divider_programmed=true;
        return true;
    }
    if (offset==0x38u) {
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
    return false;
}
