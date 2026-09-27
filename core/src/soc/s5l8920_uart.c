/* N88 polled/TX-interrupt Apple UART, independent of the legacy UART.
 * Register fields: Apple's apple_uart_regs.h; actual N88 early console uses
 * UTRSTAT bit2/bit0, word UTXH/URXH, and the encoded 16-sample divisor field.
 * Functional timing and host-frame boundaries are explicit in the header.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "s5l8920_uart.h"
#include <string.h>

#define ALL_PROGRAMMED 31u
#define RX_EVENT 0x10u
#define TX_EVENT 0x20u
static bool configured(const s5l8920_uart_t *u) {
    return u->programmed==ALL_PROGRAMMED && u->ulcon==3u && u->ufcon==1u;
}
static uint32_t frame_cycles(const s5l8920_uart_t *u) {
    return 10u*((u->ubrdiv&0xffffu)+1u)*(16u-((u->ubrdiv>>16)&15u));
}
static void start_next(s5l8920_uart_t *u) {
    if (u->tx_busy || !u->tx_count) return;
    u->tx_shift=u->tx[u->tx_head];
    u->tx_head=(u->tx_head+1u)%S5L8920_UART_FIFO_SIZE;
    u->tx_count--; u->tx_busy=true; u->tx_remaining=frame_cycles(u);
    if (!u->tx_count) u->pending|=TX_EVENT;
}
void s5l8920_uart_reset(s5l8920_uart_t *u) {
    if (u) memset(u,0,sizeof *u);
}
bool s5l8920_uart_irq(const s5l8920_uart_t *u) {
    return u && (u->ucon&0x2000u)!=0u && (u->pending&TX_EVENT)!=0u;
}
bool s5l8920_uart_read(s5l8920_uart_t *u,uint32_t offset,uint32_t *value) {
    if (!u || !value) return false;
    uint32_t result, bit=0u;
    switch (offset) {
    case S5L8920_UART_ULCON: result=u->ulcon; bit=1u; break;
    case S5L8920_UART_UCON: result=u->ucon; bit=2u; break;
    case S5L8920_UART_UFCON: result=u->ufcon; bit=4u; break;
    case S5L8920_UART_UMCON: result=u->umcon; bit=8u; break;
    case S5L8920_UART_UBRDIV: result=u->ubrdiv; bit=16u; break;
    case S5L8920_UART_UTRSTAT:
        if (!configured(u)) return false;
        result=u->pending | (u->rx_count?1u:0u) | (u->tx_count?0u:2u) |
               (u->tx_busy || u->tx_count?0u:4u);
        break;
    case S5L8920_UART_UFSTAT:
        if (!configured(u)) return false;
        result=(u->rx_count&15u) | ((u->tx_count&15u)<<4) |
               (u->rx_count==16u?0x100u:0u) | (u->tx_count==16u?0x200u:0u);
        break;
    case S5L8920_UART_URXH:
        if (!configured(u) || (u->ucon&3u)!=1u || !u->rx_count) return false;
        result=u->rx[u->rx_head];
        u->rx_head=(u->rx_head+1u)%S5L8920_UART_FIFO_SIZE; u->rx_count--;
        break;
    default: return false;
    }
    if (bit && !(u->programmed&bit)) return false;
    *value=result;
    return true;
}
bool s5l8920_uart_write(s5l8920_uart_t *u,uint32_t offset,uint32_t value) {
    if (!u) return false;
    uint32_t *target, bit;
    switch (offset) {
    case S5L8920_UART_ULCON:
        if (value!=3u) return false;
        target=&u->ulcon; bit=1u; break;
    case S5L8920_UART_UCON:
        if ((value&~0x2405u)!=0u) return false;
        target=&u->ucon; bit=2u; break;
    case S5L8920_UART_UMCON:
        if (value>1u) return false;
        target=&u->umcon; bit=8u; break;
    case S5L8920_UART_UBRDIV:
        if ((value&~0xfffffu)!=0u || ((value>>16)&15u)>8u) return false;
        target=&u->ubrdiv; bit=16u; break;
    case S5L8920_UART_UFCON:
        if ((value&~7u)!=0u || (u->tx_busy && (value&1u)!=u->ufcon)) return false;
        if (value&2u) { u->rx_head=0u; u->rx_count=0u; }
        if (value&4u) {
            if (u->tx_count) u->pending|=TX_EVENT;
            u->tx_head=0u; u->tx_count=0u;
        }
        u->ufcon=value&1u; u->programmed|=4u;
        return true;
    case S5L8920_UART_UTRSTAT:
        if (!configured(u) || (value&~(RX_EVENT|TX_EVENT))!=0u) return false;
        u->pending&=~value;
        return true;
    case S5L8920_UART_UTXH:
        if (!configured(u) || (u->ucon&12u)!=4u || value>255u || u->tx_count==16u) return false;
        u->tx[(u->tx_head+u->tx_count)%S5L8920_UART_FIFO_SIZE]=(uint8_t)value;
        u->tx_count++; start_next(u);
        return true;
    default: return false;
    }
    uint32_t active_mutable=offset==S5L8920_UART_UCON?0x2000u:0u;
    if (u->tx_busy && ((value^*target)&~active_mutable)!=0u) return false;
    *target=value; u->programmed|=bit;
    return true;
}
bool s5l8920_uart_clock(s5l8920_uart_t *u,bool nclk,uint64_t ticks,
                       uint8_t *output,size_t capacity,size_t *count) {
    if (!u || !count || (!output && capacity)) return false;
    s5l8920_uart_t next=*u;
    uint8_t completed[S5L8920_UART_FIFO_SIZE+1u];
    size_t n=0u;
    if (((u->ucon&0x400u)!=0u)==nclk) {
        while (next.tx_busy && ticks>=next.tx_remaining) {
            ticks-=next.tx_remaining;
            completed[n++]=next.tx_shift;
            next.tx_busy=false; next.tx_remaining=0u;
            start_next(&next);
        }
        if (next.tx_busy) next.tx_remaining-=(uint32_t)ticks;
    }
    if (n>capacity) return false;
    if (n) memcpy(output,completed,n);
    *u=next; *count=n;
    return true;
}
bool s5l8920_uart_receive(s5l8920_uart_t *u,uint8_t byte) {
    if (!u || !configured(u) || (u->ucon&3u)!=1u || u->rx_count==16u) return false;
    u->rx[(u->rx_head+u->rx_count)%S5L8920_UART_FIFO_SIZE]=byte;
    u->rx_count++; u->pending|=RX_EVENT;
    return true;
}
