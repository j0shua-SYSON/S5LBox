/* Transactional connection of the SPI FIFO engine and serial flash model.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_spi_flash.h"

bool s5l8920_spi_flash_clock(s5l8920_spi_t *s,sst25vf080b_t *flash,
    uint64_t periods,uint8_t bias_value,uint8_t bias_known,size_t *count) {
    if (!s || !flash || !count || !flash->initialized || !flash->image ||
        flash->powerup_ns || !s->pin_programmed ||
        flash->selected!=(s->pin==0u) || ((s->config>>15)&3u)) return false;
    /* First plan the controller's completions on a disposable copy. Placeholder
     * receive words are never committed: actual flash bytes must all validate
     * before replaying and publishing either state. No clock/time/CE side effects
     * occur in the borrowed flash image during this byte-only transaction. */
    enum { WORDS=S5L8920_SPI_FIFO_DEPTH+1u };
    uint32_t rx[WORDS]={0},tx[WORDS];
    s5l8920_spi_t next=*s;
    size_t done=0;
    if (!s5l8920_spi_serial_clock(&next,periods,rx,WORDS,tx,WORDS,&done)) return false;
    sst25vf080b_t peer=*flash;
    for (size_t i=0;i<done;++i) {
        sst25vf080b_output_t out;
        if (!sst25vf080b_transfer(&peer,(uint8_t)tx[i],&out)) return false;
        if (i<s->rx_words && ((uint8_t)~out.driven & (uint8_t)~bias_known)) return false;
        rx[i]=(uint8_t)((out.value&out.driven)|(bias_value&(uint8_t)~out.driven));
    }
    next=*s;
    if (!s5l8920_spi_serial_clock(&next,periods,rx,done,tx,WORDS,&done)) return false;
    *s=next; *flash=peer; *count=done;
    return true;
}
