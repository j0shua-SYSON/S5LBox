/*
 * S5LBox — IPv4 demultiplexing, ICMP echo, UDP NAT and the local resolver.
 *
 * The gateway half of docs/networking.md §8.2. TCP lives next door in tcp.c;
 * everything else the guest can put on the wire ends here.
 *
 * THE RULE THIS FILE IS ORGANISED AROUND. A datagram is either well formed or
 * it is counted and dropped, and there is no third case. Every early return
 * below increments exactly one counter, so a run that "did nothing" can always
 * be told apart from a run that silently ate something — which is the failure
 * mode a NAT is worst placed to debug after the fact.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "net_priv.h"
#include <string.h>

/* ============================================================ checksums === */

/*
 * RFC 1071 §4.1's ones-complement sum of 16-bit words, with a final fold and
 * complement. An odd trailing octet is padded with a zero on the right (§1,
 * "the octets are... padded with a zero byte"), and the pad is NOT part of the
 * data — it exists only for the arithmetic.
 */
uint16_t net_checksum(const uint8_t *data, size_t n) {
    uint32_t sum = 0;
    if (!data) return 0xffffu;
    while (n >= 2u) { sum += net_rd16(data); data += 2; n -= 2u; }
    if (n) sum += (uint32_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)(~sum & 0xffffu);
}

/*
 * The TCP/UDP pseudo-header of RFC 793 §3.1: source, destination, a zero
 * octet, the protocol and the segment length, summed ahead of the segment
 * itself. Written as one pass over a 12-octet scratch plus one over the
 * segment rather than as two calls to net_checksum(), because two independent
 * complemented sums do not add up to the sum of the whole.
 */
uint16_t net_l4_checksum(uint32_t src, uint32_t dst, unsigned proto,
                         const uint8_t *seg, size_t n) {
    uint8_t ph[12];
    net_wr32(ph, src);
    net_wr32(ph + 4, dst);
    ph[8]  = 0;
    ph[9]  = (uint8_t)proto;
    net_wr16(ph + 10, (uint16_t)n);

    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof ph; i += 2u) sum += net_rd16(ph + i);
    size_t i = 0;
    for (; i + 1u < n; i += 2u) sum += net_rd16(seg + i);
    if (i < n) sum += (uint32_t)seg[i] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)(~sum & 0xffffu);
}

/* ============================================================== output === */

size_t net_build_ipv4(uint8_t *out, size_t cap, uint32_t src, uint32_t dst,
                      unsigned proto, uint16_t ident, size_t payload_len) {
    if (!out || cap < IPV4_HDR_LEN) return 0;
    if (payload_len + IPV4_HDR_LEN > NET_MTU) return 0;

    memset(out, 0, IPV4_HDR_LEN);
    out[0] = 0x45u;                                  /* version 4, IHL 5     */
    out[1] = 0;                                      /* DSCP/ECN both zero   */
    net_wr16(out + 2, (uint16_t)(IPV4_HDR_LEN + payload_len));
    net_wr16(out + 4, ident);
    /*
     * Don't Fragment. We never fragment and §8.2 refuses reassembly, so
     * advertising DF is the honest flag: a path that needs a smaller MTU
     * should say so with a Fragmentation Needed rather than quietly succeed on
     * a datagram nobody can carry.
     */
    net_wr16(out + 6, 0x4000u);
    out[8] = 64u;                                    /* TTL, RFC 1122 §3.2.1.7 */
    out[9] = (uint8_t)proto;
    net_wr32(out + 12, src);
    net_wr32(out + 16, dst);
    net_wr16(out + 10, net_checksum(out, IPV4_HDR_LEN));
    return IPV4_HDR_LEN;
}

net_dgram_t *net_out_alloc(net_stack_t *ns) {
    if (ns->out_head - ns->out_tail >= NET_OUT_SLOTS) return NULL;
    return &ns->out[ns->out_head % NET_OUT_SLOTS];
}

bool net_emit(net_stack_t *ns, uint32_t src, uint32_t dst, unsigned proto,
              const uint8_t *payload, size_t n) {
    if (n + IPV4_HDR_LEN > NET_MTU) { ns->stats.out_dropped++; return false; }
    net_dgram_t *d = net_out_alloc(ns);
    if (!d) { ns->stats.out_dropped++; return false; }

    /*
     * A fresh Identification per datagram. It is only meaningful to a
     * reassembler and we set DF, but a constant here would make two datagrams
     * from us indistinguishable in a capture, which is exactly the thing a
     * person debugging this will want to do.
     */
    size_t hl = net_build_ipv4(d->data, sizeof d->data, src, dst, proto,
                               (uint16_t)(ns->stats.ip_out + 1u), n);
    if (!hl) { ns->stats.out_dropped++; return false; }
    if (n) memcpy(d->data + hl, payload, n);
    d->len = (uint16_t)(hl + n);
    ns->out_head++;
    ns->stats.ip_out++;
    return true;
}

size_t net_output_peek(const net_stack_t *ns) {
    if (!ns || ns->out_head == ns->out_tail) return 0;
    return ns->out[ns->out_tail % NET_OUT_SLOTS].len;
}

size_t net_output_pending(const net_stack_t *ns) {
    return ns ? (size_t)(ns->out_head - ns->out_tail) : 0u;
}

size_t net_output(net_stack_t *ns, uint8_t *buf, size_t cap) {
    if (!ns || !buf || ns->out_head == ns->out_tail) return 0;
    const net_dgram_t *d = &ns->out[ns->out_tail % NET_OUT_SLOTS];
    if (d->len > cap) return 0;         /* left queued; the caller can retry */
    memcpy(buf, d->data, d->len);
    size_t len = d->len;
    ns->out_tail++;
    return len;
}

/* =============================================================== flows === */

net_flow_t *net_flow_find(net_stack_t *ns, unsigned proto, uint16_t gport,
                          uint32_t dst_ip, uint16_t dport) {
    for (unsigned i = 0; i < NET_MAX_FLOWS; i++) {
        net_flow_t *f = &ns->flow[i];
        if (f->used && f->proto == proto && f->guest_port == gport &&
            f->dst_ip == dst_ip && f->dst_port == dport)
            return f;
    }
    return NULL;
}

net_flow_t *net_flow_alloc(net_stack_t *ns, unsigned proto, uint16_t gport,
                           uint32_t dst_ip, uint16_t dport) {
    for (unsigned i = 0; i < NET_MAX_FLOWS; i++) {
        net_flow_t *f = &ns->flow[i];
        if (f->used) continue;
        memset(f, 0, sizeof *f);
        f->used       = true;
        f->proto      = proto;
        f->guest_port = gport;
        f->dst_ip     = dst_ip;
        f->dst_port   = dport;
        f->handle     = -1;
        f->last_ms    = ns->now_ms;
        f->state      = NET_TCP_CLOSED;
        ns->stats.flows_open++;
        size_t live = net_flows_open(ns);
        if (live > ns->stats.flows_peak) ns->stats.flows_peak = live;
        return f;
    }
    ns->stats.flow_table_full++;
    return NULL;
}

void net_flow_free(net_stack_t *ns, net_flow_t *f) {
    if (!f || !f->used) return;
    if (f->handle >= 0 && ns->eg.close) ns->eg.close(ns->eg.ctx, f->handle);
    memset(f, 0, sizeof *f);
    f->handle = -1;
}

size_t net_flows_open(const net_stack_t *ns) {
    size_t n = 0;
    if (!ns) return 0;
    for (unsigned i = 0; i < NET_MAX_FLOWS; i++) if (ns->flow[i].used) n++;
    return n;
}

/* ================================================================ ICMP === */

/*
 * RFC 792's Echo/Echo Reply. The reply is the request with the type changed
 * from 8 to 0 and the checksum recomputed — identifier, sequence and the whole
 * data field are echoed verbatim, which is what makes `ping`'s round-trip time
 * and payload check work.
 */
static void icmp_input(net_stack_t *ns, uint32_t src, uint32_t dst,
                       const uint8_t *p, size_t n) {
    ns->stats.icmp_in++;
    if (n < ICMP_HDR_LEN) { ns->stats.ip_bad_header++; return; }
    if (net_checksum(p, n) != 0u) { ns->stats.ip_bad_checksum++; return; }
    if (p[0] != ICMP_ECHO_REQUEST || p[1] != 0u) {
        /* Anything else — unreachables, time exceeded, timestamps — is not
         * something a NAT with no forwarding plane can answer. Counted with
         * the rest of the traffic it cannot carry rather than silently. */
        ns->stats.icmp_unsupported++;
        return;
    }

    if (dst == ns->cfg.local_ip || dst == ns->cfg.dns_ip) {
        /* We ARE the destination, so the reply is ours to make and needs no
         * network at all. This is docs/networking.md §10's N4 proof: the
         * cheapest end-to-end evidence the guest's IP path works. */
        uint8_t r[NET_MTU - IPV4_HDR_LEN];
        if (n > sizeof r) { ns->stats.ip_bad_header++; return; }
        memcpy(r, p, n);
        r[0] = ICMP_ECHO_REPLY;
        net_wr16(r + 2, 0);
        net_wr16(r + 2, net_checksum(r, n));
        if (net_emit(ns, dst, src, NET_PROTO_ICMP, r, n))
            ns->stats.icmp_echo_replies++;
        return;
    }

    /*
     * Aimed past us. §8.2 hoped for an unprivileged SOCK_DGRAM/IPPROTO_ICMP;
     * where the host cannot provide one there is nothing honest to do but
     * count it. Answering locally on behalf of a machine we never contacted
     * would be a fabricated reply, and this project does not fabricate
     * replies.
     */
    if (ns->eg.icmp_echo &&
        ns->eg.icmp_echo(ns->eg.ctx, dst, p, n) >= 0)
        return;
    ns->stats.icmp_unsupported++;
}

/* ================================================================= DNS === */

/*
 * A resolver, not a forwarder. docs/networking.md §8.2: parse the query and
 * call the host's getaddrinfo() through the egress, which inherits the host's
 * own resolver configuration and survives a captive network that blocks
 * port 53.
 *
 * RFC 1035 §4.1. One question only — that is what every stub resolver of this
 * era sends, and QDCOUNT > 1 has no defined per-question RCODE, so more than
 * one is refused with FORMERR rather than half-answered.
 */
#define DNS_QR      0x8000u
#define DNS_AA      0x0400u
#define DNS_RD      0x0100u
#define DNS_RA      0x0080u
#define DNS_RC_OK      0u
#define DNS_RC_FORMERR 1u
#define DNS_RC_SERVFAIL 2u
#define DNS_RC_NXDOMAIN 3u
#define DNS_RC_NOTIMP   4u
#define DNS_T_A     1u
#define DNS_T_AAAA  28u
#define DNS_C_IN    1u

/*
 * Decode a QNAME into dotted form. Returns the number of octets the wire name
 * occupied, or 0 for anything malformed.
 *
 * Compression pointers are REFUSED here (RFC 1035 §4.1.4 allows them anywhere
 * a name appears, but a pointer in a question can only point backwards into
 * the header, which is not a name). Refusing them also removes the pointer
 * loop that every naive parser eventually gets wrong.
 */
static size_t dns_name(const uint8_t *p, size_t n, char *out, size_t cap) {
    size_t at = 0, w = 0;
    for (;;) {
        if (at >= n) return 0;
        uint8_t l = p[at++];
        if (l == 0) break;
        if ((l & 0xc0u) != 0u) return 0;             /* pointer or reserved */
        if (l > 63u) return 0;                       /* RFC 1035 §2.3.4    */
        if (at + l > n) return 0;
        if (w && w + 1u < cap) out[w++] = '.';
        for (unsigned i = 0; i < l; i++) {
            uint8_t c = p[at + i];
            /* A label octet is arbitrary in the protocol but a host name is
             * not, and this string is about to be handed to getaddrinfo().
             * Anything outside the preferred syntax of RFC 1035 §2.3.1 plus
             * the underscore and hyphen real deployments use is refused. */
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok) return 0;
            if (w + 1u < cap) out[w++] = (char)c;
        }
        at += l;
        if (at > 255u) return 0;                     /* RFC 1035 §2.3.4    */
    }
    if (w >= cap) return 0;
    out[w] = '\0';
    return w ? at : 0;                               /* the root alone is not
                                                        a question we answer */
}

static void dns_reply(net_stack_t *ns, uint16_t gport,
                      const uint8_t *q, size_t qlen, size_t qend,
                      unsigned rcode, bool have_a, uint32_t a) {
    uint8_t r[NET_MTU - IPV4_HDR_LEN - UDP_HDR_LEN];
    if (qend + 16u + UDP_HDR_LEN > sizeof r) { ns->stats.dns_malformed++; return; }

    uint8_t *dns = r + UDP_HDR_LEN;
    /* The whole question section is echoed verbatim, header included, then
     * the header's flags are rewritten — RFC 1035 §4.1.1 requires the ID and
     * the question to come back unchanged. */
    memcpy(dns, q, qend);
    uint16_t flags = (uint16_t)(DNS_QR | DNS_AA | DNS_RA | rcode);
    if (net_rd16(q + 2) & DNS_RD) flags |= DNS_RD;
    net_wr16(dns + 2, flags);
    net_wr16(dns + 4, rcode == DNS_RC_FORMERR ? 0u : 1u);   /* QDCOUNT       */
    net_wr16(dns + 6, have_a ? 1u : 0u);                    /* ANCOUNT       */
    net_wr16(dns + 8, 0);
    net_wr16(dns + 10, 0);
    size_t at = qend;

    if (have_a) {
        /* A pointer to the question's name at offset 12, RFC 1035 §4.1.4 —
         * the one place compression is unambiguous, since the question name
         * is the first name in the message. */
        net_wr16(dns + at, 0xc00cu); at += 2u;
        net_wr16(dns + at, DNS_T_A); at += 2u;
        net_wr16(dns + at, DNS_C_IN); at += 2u;
        net_wr32(dns + at, NET_DNS_TTL); at += 4u;
        net_wr16(dns + at, 4u); at += 2u;
        net_wr32(dns + at, a); at += 4u;
    }
    (void)qlen;

    size_t ulen = UDP_HDR_LEN + at;
    net_wr16(r, 53u);
    net_wr16(r + 2, gport);
    net_wr16(r + 4, (uint16_t)ulen);
    net_wr16(r + 6, 0);
    net_wr16(r + 6, net_l4_checksum(ns->cfg.dns_ip, ns->cfg.guest_ip,
                                    NET_PROTO_UDP, r, ulen));
    if (net_emit(ns, ns->cfg.dns_ip, ns->cfg.guest_ip, NET_PROTO_UDP, r, ulen))
        ns->stats.udp_out++;
}

static void dns_finish(net_stack_t *ns, uint16_t gport,
                       const uint8_t *query, size_t query_len,
                       int result, uint32_t address) {
    if (result == NET_RES_OK) {
        dns_reply(ns, gport, query, query_len, query_len,
                  DNS_RC_OK, true, address);
        ns->stats.dns_answered++;
    } else if (result == NET_RES_NXDOMAIN) {
        dns_reply(ns, gport, query, query_len, query_len,
                  DNS_RC_NXDOMAIN, false, 0u);
        ns->stats.dns_nxdomain++;
    } else {
        dns_reply(ns, gport, query, query_len, query_len,
                  DNS_RC_SERVFAIL, false, 0u);
        ns->stats.dns_failed++;
    }
}

/*
 * Keep the question, not a pointer into ppp.c's borrowed receive frame. A
 * retransmission of the same transaction is already represented and needs no
 * second slot; a fifth independent lookup is dropped so the guest's normal
 * DNS retry policy can recover without receiving a fabricated failure.
 */
static bool dns_defer(net_stack_t *ns, uint16_t gport,
                      const uint8_t *query, size_t query_len,
                      const char *name) {
    if (!ns || !query || !name || !query_len ||
        query_len > NET_DNS_QUERY_MAX)
        return false;

    net_dns_pending_t *free_slot = NULL;
    for (unsigned i = 0u; i < NET_DNS_PENDING; i++) {
        net_dns_pending_t *slot = &ns->dns[i];
        if (!slot->used) {
            if (!free_slot) free_slot = slot;
            continue;
        }
        if (slot->guest_port == gport &&
            slot->query_len == (uint16_t)query_len &&
            memcmp(slot->query, query, query_len) == 0)
            return true;
    }
    if (!free_slot) {
        ns->stats.dns_pending_full++;
        return false;
    }

    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = true;
    free_slot->guest_port = gport;
    free_slot->query_len = (uint16_t)query_len;
    free_slot->started_ms = ns->now_ms;
    memcpy(free_slot->query, query, query_len);
    size_t name_len = strlen(name);
    if (name_len >= sizeof free_slot->name) name_len = sizeof free_slot->name - 1u;
    memcpy(free_slot->name, name, name_len);
    free_slot->name[name_len] = '\0';
    ns->stats.dns_deferred++;
    return true;
}

static void dns_tick(net_stack_t *ns) {
    if (!ns) return;
    for (unsigned i = 0u; i < NET_DNS_PENDING; i++) {
        net_dns_pending_t *slot = &ns->dns[i];
        if (!slot->used) continue;

        uint32_t address = 0u;
        int result = ns->eg.resolve
            ? ns->eg.resolve(ns->eg.ctx, slot->name, &address)
            : NET_RES_FAIL;
        if (result == NET_RES_PENDING) {
            if ((uint32_t)(ns->now_ms - slot->started_ms) <
                NET_DNS_TIMEOUT_MS)
                continue;
            result = NET_RES_FAIL;
            ns->stats.dns_timeouts++;
        }
        dns_finish(ns, slot->guest_port, slot->query, slot->query_len,
                   result, address);
        memset(slot, 0, sizeof *slot);
    }
}

static void dns_input(net_stack_t *ns, uint16_t gport,
                      const uint8_t *p, size_t n) {
    ns->stats.dns_queries++;
    if (n < 12u) { ns->stats.dns_malformed++; return; }
    uint16_t flags = net_rd16(p + 2);
    /* Recorded before anything acts on it; see net.h. */
    ns->stats.dns_last_flags   = flags;
    ns->stats.dns_last_qdcount = net_rd16(p + 4);
    if (flags & DNS_QR) { ns->stats.dns_malformed++; return; } /* a response */
    if (net_rd16(p + 4) != 1u) {
        ns->stats.dns_malformed++;
        dns_reply(ns, gport, p, n, 12u, DNS_RC_FORMERR, false, 0);
        return;
    }

    char name[256];
    size_t nl = dns_name(p + 12, n - 12u, name, sizeof name);
    if (!nl || 12u + nl + 4u > n) {
        ns->stats.dns_malformed++;
        dns_reply(ns, gport, p, n, 12u, DNS_RC_FORMERR, false, 0);
        return;
    }
    size_t qend  = 12u + nl + 4u;
    uint16_t qt  = net_rd16(p + 12u + nl);
    uint16_t qc  = net_rd16(p + 12u + nl + 2u);

    if (qc != DNS_C_IN) {
        dns_reply(ns, gport, p, n, qend, DNS_RC_NOTIMP, false, 0);
        ns->stats.dns_failed++;
        return;
    }
    if (qt == DNS_T_AAAA) {
        /*
         * NOERROR with no answers, not NXDOMAIN. A name that exists with no
         * AAAA is exactly this case (RFC 2308 §2.2's NODATA), and answering
         * NXDOMAIN would tell the guest the name does not exist at all, which
         * a negative cache would then apply to its A query too.
         */
        dns_reply(ns, gport, p, n, qend, DNS_RC_OK, false, 0);
        ns->stats.dns_answered++;
        return;
    }
    if (qt != DNS_T_A) {
        dns_reply(ns, gport, p, n, qend, DNS_RC_NOTIMP, false, 0);
        ns->stats.dns_failed++;
        return;
    }

    uint32_t a = 0;
    int rc = ns->eg.resolve ? ns->eg.resolve(ns->eg.ctx, name, &a)
                            : NET_RES_FAIL;
    if (rc == NET_RES_PENDING) {
        (void)dns_defer(ns, gport, p, qend, name);
        return;
    }
    dns_finish(ns, gport, p, qend, rc, a);
}

/* ================================================================= UDP === */

static void udp_input(net_stack_t *ns, uint32_t src, uint32_t dst,
                      const uint8_t *p, size_t n) {
    ns->stats.udp_in++;
    if (n < UDP_HDR_LEN) { ns->stats.ip_bad_header++; return; }
    uint16_t sport = net_rd16(p);
    uint16_t dport = net_rd16(p + 2);
    uint16_t ulen  = net_rd16(p + 4);
    if (ulen < UDP_HDR_LEN || ulen > n) { ns->stats.ip_bad_header++; return; }

    /* RFC 768: a zero checksum means "not computed". Everything else must
     * verify — accepting a wrong one would let a corrupt DNS answer through
     * to a guest that has no other integrity check. */
    if (net_rd16(p + 6) != 0u &&
        net_l4_checksum(src, dst, NET_PROTO_UDP, p, ulen) != 0u) {
        ns->stats.ip_bad_checksum++;
        return;
    }

    const uint8_t *pay = p + UDP_HDR_LEN;
    size_t paylen = (size_t)ulen - UDP_HDR_LEN;

    if (dst == ns->cfg.dns_ip && dport == 53u) {
        dns_input(ns, sport, pay, paylen);
        return;
    }

    net_flow_t *f = net_flow_find(ns, NET_PROTO_UDP, sport, dst, dport);
    if (!f) {
        f = net_flow_alloc(ns, NET_PROTO_UDP, sport, dst, dport);
        if (!f) return;
        f->handle = ns->eg.open ? ns->eg.open(ns->eg.ctx, NET_PROTO_UDP,
                                              dst, dport)
                                : -1;
        if (f->handle < 0) { net_flow_free(ns, f); return; }
    }
    f->last_ms = ns->now_ms;
    if (ns->eg.send && ns->eg.send(ns->eg.ctx, f->handle, pay, paylen) < 0)
        net_flow_free(ns, f);
}

static void udp_poll(net_stack_t *ns, net_flow_t *f) {
    if (!ns->eg.recv || f->handle < 0) return;
    for (unsigned guard = 0; guard < 8u; guard++) {
        uint8_t buf[NET_MTU - IPV4_HDR_LEN - UDP_HDR_LEN];
        int r = ns->eg.recv(ns->eg.ctx, f->handle, buf, sizeof buf);
        if (r == NET_EG_WOULDBLOCK) return;
        if (r < 0) { net_flow_free(ns, f); return; }

        uint8_t u[NET_MTU - IPV4_HDR_LEN];
        size_t ulen = UDP_HDR_LEN + (size_t)r;
        net_wr16(u, f->dst_port);
        net_wr16(u + 2, f->guest_port);
        net_wr16(u + 4, (uint16_t)ulen);
        net_wr16(u + 6, 0);
        memcpy(u + UDP_HDR_LEN, buf, (size_t)r);
        net_wr16(u + 6, net_l4_checksum(f->dst_ip, ns->cfg.guest_ip,
                                        NET_PROTO_UDP, u, ulen));
        if (!net_emit(ns, f->dst_ip, ns->cfg.guest_ip, NET_PROTO_UDP, u, ulen))
            return;                    /* queue full: try again next tick    */
        ns->stats.udp_out++;
        f->last_ms = ns->now_ms;
    }
}

/* ================================================================ IPv4 === */

void net_input(net_stack_t *ns, const uint8_t *pkt, size_t n) {
    if (!ns || !pkt) return;
    ns->stats.ip_in++;

    if (n < IPV4_HDR_LEN) { ns->stats.ip_bad_header++; return; }
    if ((pkt[0] >> 4) != 4u) { ns->stats.ip_bad_version++; return; }

    size_t ihl = (size_t)(pkt[0] & 0x0fu) * 4u;
    if (ihl < IPV4_HDR_LEN || ihl > n) { ns->stats.ip_bad_header++; return; }

    size_t total = net_rd16(pkt + 2);
    if (total < ihl || total > n) { ns->stats.ip_bad_header++; return; }

    if (net_checksum(pkt, ihl) != 0u) { ns->stats.ip_bad_checksum++; return; }

    /*
     * RFC 791 §3.1: More-Fragments, or a non-zero Fragment Offset, means this
     * is a piece. docs/networking.md §8.2 refuses reassembly rather than
     * half-implementing it — a reassembler is a well-known source of
     * overlapping-fragment bugs and nothing the guest does needs one, since
     * every path out of here is MSS-clamped or a single datagram.
     */
    uint16_t frag = net_rd16(pkt + 6);
    if ((frag & 0x2000u) || (frag & 0x1fffu)) {
        ns->stats.ip_frags_refused++;
        return;
    }

    uint32_t src = net_rd32(pkt + 12);
    uint32_t dst = net_rd32(pkt + 16);

    /*
     * Only the address IPCP handed out. A NAT that forwarded whatever source
     * it was given would be an open relay for anything that ever gets code
     * execution in the guest, and the guest has exactly one address.
     */
    if (src != ns->cfg.guest_ip) { ns->stats.ip_bad_source++; return; }

    const uint8_t *l4 = pkt + ihl;
    size_t l4len = total - ihl;

    switch (pkt[9]) {
        case NET_PROTO_ICMP: icmp_input(ns, src, dst, l4, l4len); return;
        case NET_PROTO_UDP:  udp_input(ns, src, dst, l4, l4len);  return;
        case NET_PROTO_TCP:  net_tcp_input(ns, src, dst, l4, l4len); return;
        default: ns->stats.proto_unsupported++; return;
    }
}

/* ================================================================ tick === */

void net_tick(net_stack_t *ns, uint32_t now_ms) {
    if (!ns) return;
    if ((int32_t)(now_ms - ns->now_ms) < 0) return;   /* monotonic only      */
    ns->now_ms = now_ms;

    dns_tick(ns);

    for (unsigned i = 0; i < NET_MAX_FLOWS; i++) {
        net_flow_t *f = &ns->flow[i];
        if (!f->used) continue;
        if (f->proto == NET_PROTO_UDP) {
            udp_poll(ns, f);
            if (f->used && (uint32_t)(now_ms - f->last_ms) > NET_UDP_IDLE_MS)
                net_flow_free(ns, f);
        } else {
            net_tcp_tick(ns, f);
        }
    }
}

/* ============================================================== set-up === */

void net_config_default(net_config_t *cfg) {
    if (!cfg) return;
    cfg->guest_ip = 0x0a00020fu;   /* 10.0.2.15 */
    cfg->local_ip = 0x0a000202u;   /* 10.0.2.2  */
    cfg->dns_ip   = 0x0a000203u;   /* 10.0.2.3  */
    /*
     * RFC 793 §3.3 wants the initial send sequence to advance with time so a
     * segment from a previous incarnation of a connection cannot be mistaken
     * for a current one. We have no clock here, so this is a fixed seed that
     * net_init() walks per connection: reproducible, which a test needs, and
     * not zero, which would make an off-by-one in the sequence arithmetic
     * invisible.
     */
    cfg->iss = 0x5335384cu;        /* "S58L" */
}

void net_init(net_stack_t *ns, const net_config_t *cfg,
              const net_egress_t *eg) {
    if (!ns) return;
    memset(ns, 0, sizeof *ns);
    if (cfg) ns->cfg = *cfg;
    else     net_config_default(&ns->cfg);
    if (eg)  ns->eg = *eg;
    ns->iss_walk = ns->cfg.iss;
    for (unsigned i = 0; i < NET_MAX_FLOWS; i++) ns->flow[i].handle = -1;
}

void net_reset(net_stack_t *ns) {
    if (!ns) return;
    for (unsigned i = 0; i < NET_MAX_FLOWS; i++)
        if (ns->flow[i].used) net_flow_free(ns, &ns->flow[i]);
    net_config_t cfg = ns->cfg;
    net_egress_t eg  = ns->eg;
    net_init(ns, &cfg, &eg);
}

const char *net_tcp_state_name(net_tcp_state_t s) {
    switch (s) {
        case NET_TCP_CLOSED:      return "CLOSED";
        case NET_TCP_SYN_RCVD:    return "SYN-RCVD";
        case NET_TCP_ESTABLISHED: return "ESTABLISHED";
        case NET_TCP_FIN_WAIT_1:  return "FIN-WAIT-1";
        case NET_TCP_FIN_WAIT_2:  return "FIN-WAIT-2";
        case NET_TCP_CLOSE_WAIT:  return "CLOSE-WAIT";
        case NET_TCP_CLOSING:     return "CLOSING";
        case NET_TCP_LAST_ACK:    return "LAST-ACK";
        case NET_TCP_TIME_WAIT:   return "TIME-WAIT";
    }
    return "?";
}
