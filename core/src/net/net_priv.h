/*
 * S5LBox — internals shared between core/src/net/net.c and core/src/net/tcp.c.
 *
 * Not installed and not part of the module's contract; net.h is. The split
 * exists because the TCP automaton is the only part of this stack with real
 * state, and keeping it in its own file makes it possible to read RFC 793
 * beside it without a thousand lines of ICMP and DNS in between.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_NET_PRIV_H
#define S5LBOX_NET_PRIV_H

#include "net.h"

/* Fixed offsets, RFC 791 §3.1 / RFC 793 §3.1 / RFC 768 / RFC 792 §Echo. */
#define IPV4_HDR_LEN     20u
#define TCP_HDR_LEN      20u
#define UDP_HDR_LEN      8u
#define ICMP_HDR_LEN     8u

/* TCP flags, RFC 793 §3.1. */
#define TCP_FIN  0x01u
#define TCP_SYN  0x02u
#define TCP_RST  0x04u
#define TCP_PSH  0x08u
#define TCP_ACK  0x10u
#define TCP_URG  0x20u

/* ICMP types, RFC 792. */
#define ICMP_ECHO_REPLY   0u
#define ICMP_ECHO_REQUEST 8u

/* ------------------------------------------------------- octet plumbing --- */

static inline uint16_t net_rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint32_t net_rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline void net_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void net_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/*
 * RFC 793 §3.3's sequence comparison. Sequence numbers wrap, so "later" is a
 * signed difference and never a magnitude test — the bug this inline exists to
 * make impossible to write by accident.
 */
static inline bool net_seq_lt(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}
static inline bool net_seq_le(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) <= 0;
}

/* ------------------------------------------------ shared implementation --- */

/*
 * Reserve the next output slot, or NULL when the queue toward the guest is
 * full (counted in stats.out_dropped by the caller's helper below).
 */
net_dgram_t *net_out_alloc(net_stack_t *ns);

/*
 * Wrap `payload` in an IPv4 header and queue it. `payload` may already sit in
 * a scratch buffer; this copies. Returns false if the queue was full or the
 * datagram would exceed the MTU, and counts either way.
 */
bool net_emit(net_stack_t *ns, uint32_t src, uint32_t dst, unsigned proto,
              const uint8_t *payload, size_t n);

/* Find the flow matching this 4-tuple, or NULL. */
net_flow_t *net_flow_find(net_stack_t *ns, unsigned proto, uint16_t gport,
                          uint32_t dst_ip, uint16_t dport);

/* Take an unused slot and fill in the tuple, or NULL when the table is full. */
net_flow_t *net_flow_alloc(net_stack_t *ns, unsigned proto, uint16_t gport,
                           uint32_t dst_ip, uint16_t dport);

/* Close the egress handle if there is one and return the slot to the pool. */
void net_flow_free(net_stack_t *ns, net_flow_t *f);

/* net.c hands TCP segments here; tcp.c owns everything after that. */
void net_tcp_input(net_stack_t *ns, uint32_t src, uint32_t dst,
                   const uint8_t *seg, size_t n);
void net_tcp_tick(net_stack_t *ns, net_flow_t *f);

#endif /* S5LBOX_NET_PRIV_H */
