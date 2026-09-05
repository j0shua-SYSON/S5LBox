/*
 * S5LBox — the host IPv4 stack, NAT and resolver.
 *
 * WHY THIS SUITE EXISTS AT ALL, and why it is this large:
 * docs/networking.md §11 risk 1 says a half-correct TCP produces stalls that
 * look like emulator bugs. By the time a stall is visible in a guest it has
 * crossed PPP, a UART, an interrupt controller and an 800-line driver, and
 * nothing in that chain will tell you which end was wrong. So the TCP is driven
 * here, with no sockets and no guest, in microseconds.
 *
 * THE TEST OWNS ITS OWN ENCODER AND DECODER. build_tcp()/build_udp()/take()
 * below construct and parse datagrams field by field from RFC 791, 768 and 793,
 * so a round trip really does cross the boundary. The two things they borrow
 * from the module are net_checksum() and net_l4_checksum(), and both are pinned
 * first — against the worked example in RFC 1071 §3 and a header whose
 * checksum is a published constant — so a wrong sum cannot pass by agreeing
 * with itself.
 *
 * THE EGRESS IS A SCRIPT, not a socket. Every case states what the "internet"
 * does — accepts 3 of 10 octets, reports EOF, refuses to connect, stays PENDING
 * — because that is the half of the state machine a real network will only
 * exercise by accident and only on someone else's machine.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "net.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { \
        g_fail++; \
        printf("  FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define GUEST_IP  0x0a00020fu   /* 10.0.2.15 */
#define LOCAL_IP  0x0a000202u   /* 10.0.2.2  */
#define DNS_IP    0x0a000203u   /* 10.0.2.3  */
#define PEER_IP   0x5db8d822u   /* 93.184.216.34, example.com's old address  */

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

/* The struct is deliberately heap-scale in production; keep one static here. */
static net_stack_t g_ns;

/* ==========================================================================
 * A scripted internet.
 * ======================================================================== */

#define MOCK_HANDLES 32
#define MOCK_BUF     8192

typedef struct {
    bool     open_used;
    unsigned proto;
    uint32_t ip;
    uint16_t port;
    int      status;            /* NET_ST_*                                  */
    bool     closed, shutdown;

    /* What the host is willing to accept from the guest this call. 0 means
     * "as much as offered"; a negative value is a fatal error. */
    int      accept_limit;
    uint8_t  sent[MOCK_BUF];
    size_t   sentlen;

    /* What the host hands back. `recv_err` is returned once the queue is dry. */
    uint8_t  give[MOCK_BUF];
    size_t   givelen, givepos;
    size_t   stream_pos, stream_left;
    int      recv_err;          /* NET_EG_WOULDBLOCK / _EOF / _ERROR         */
    size_t   give_chunk;        /* 0 = as much as fits                       */
} mock_flow_t;

typedef struct {
    mock_flow_t h[MOCK_HANDLES];
    int         next;
    /* What a freshly opened handle reports. READY unless a case is exercising
     * a connect that is still in flight — which must be set BEFORE the SYN,
     * because tcp_syn() polls the status immediately so a loopback connect can
     * complete inside the same call. */
    int         open_status;
    bool        refuse_open;
    int         resolve_rc;
    uint32_t    resolve_ip;
    char        resolved[256];
    unsigned    resolve_calls;
    int         icmp_calls;
    bool        icmp_ok;
    unsigned    opens, closes;
} mock_t;

static mock_t g_mock;

static int m_open(void *ctx, unsigned proto, uint32_t ip, uint16_t port) {
    mock_t *m = ctx;
    if (m->refuse_open || m->next >= MOCK_HANDLES) return -1;
    int h = m->next++;
    memset(&m->h[h], 0, sizeof m->h[h]);
    m->h[h].open_used = true;
    m->h[h].proto = proto;
    m->h[h].ip = ip;
    m->h[h].port = port;
    m->h[h].status = m->open_status;
    m->h[h].recv_err = NET_EG_WOULDBLOCK;
    m->opens++;
    return h;
}
static int m_status(void *ctx, int h) {
    mock_t *m = ctx;
    if (h < 0 || h >= MOCK_HANDLES) return NET_ST_FAILED;
    return m->h[h].status;
}
static int m_send(void *ctx, int h, const uint8_t *d, size_t n) {
    mock_t *m = ctx;
    if (h < 0 || h >= MOCK_HANDLES) return -1;
    mock_flow_t *f = &m->h[h];
    if (f->accept_limit < 0) return -1;
    size_t take = f->accept_limit ? (size_t)f->accept_limit : n;
    if (take > n) take = n;
    if (take > MOCK_BUF - f->sentlen) take = MOCK_BUF - f->sentlen;
    memcpy(f->sent + f->sentlen, d, take);
    f->sentlen += take;
    return (int)take;
}
static int m_recv(void *ctx, int h, uint8_t *buf, size_t cap) {
    mock_t *m = ctx;
    if (h < 0 || h >= MOCK_HANDLES) return NET_EG_ERROR;
    mock_flow_t *f = &m->h[h];
    size_t left = f->givelen - f->givepos;
    if (!left && f->stream_left) {
        size_t take = f->stream_left;
        if (f->give_chunk && take > f->give_chunk) take = f->give_chunk;
        if (take > cap) take = cap;
        for (size_t i = 0; i < take; i++) {
            size_t at = f->stream_pos + i;
            buf[i] = (uint8_t)(((at * 131u + 17u) ^ (at >> 7u)) & 0xffu);
        }
        f->stream_pos += take;
        f->stream_left -= take;
        return (int)take;
    }
    if (!left) return f->recv_err;
    size_t take = left;
    if (f->give_chunk && take > f->give_chunk) take = f->give_chunk;
    if (take > cap) take = cap;
    memcpy(buf, f->give + f->givepos, take);
    f->givepos += take;
    return (int)take;
}
static void m_shutdown(void *ctx, int h) {
    mock_t *m = ctx;
    if (h >= 0 && h < MOCK_HANDLES) m->h[h].shutdown = true;
}
static void m_close(void *ctx, int h) {
    mock_t *m = ctx;
    if (h >= 0 && h < MOCK_HANDLES) { m->h[h].closed = true; m->closes++; }
}
static int m_resolve(void *ctx, const char *name, uint32_t *ip) {
    mock_t *m = ctx;
    m->resolve_calls++;
    snprintf(m->resolved, sizeof m->resolved, "%s", name);
    if (m->resolve_rc == NET_RES_OK) *ip = m->resolve_ip;
    return m->resolve_rc;
}
static int m_icmp(void *ctx, uint32_t ip, const uint8_t *p, size_t n) {
    mock_t *m = ctx;
    (void)ip; (void)p; (void)n;
    m->icmp_calls++;
    return m->icmp_ok ? 0 : -1;
}

/* `full` installs every member; the ICMP forwarder is separate because §8.2
 * expects it to be absent on most hosts and the absent case is the one that
 * must not fabricate a reply. */
static void mock_reset(net_egress_t *eg, bool full, bool with_icmp) {
    memset(&g_mock, 0, sizeof g_mock);
    g_mock.open_status = NET_ST_READY;
    g_mock.resolve_rc = NET_RES_OK;
    g_mock.resolve_ip = PEER_IP;
    memset(eg, 0, sizeof *eg);
    eg->ctx = &g_mock;
    if (full) {
        eg->open = m_open;   eg->status = m_status; eg->send = m_send;
        eg->recv = m_recv;   eg->shutdown_tx = m_shutdown;
        eg->close = m_close; eg->resolve = m_resolve;
    }
    if (with_icmp) eg->icmp_echo = m_icmp;
}

static void start(bool full, bool with_icmp) {
    net_egress_t eg;
    mock_reset(&eg, full, with_icmp);
    net_init(&g_ns, NULL, &eg);
}

/* ==========================================================================
 * Datagram construction and inspection, written from the RFCs.
 * ======================================================================== */

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t r16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t r32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

typedef struct {
    uint8_t  raw[NET_MTU];
    size_t   rawlen;
    uint32_t src, dst;
    uint8_t  proto;
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  flags;
    uint16_t wnd;
    const uint8_t *pay;
    size_t   paylen;
} pkt_t;

/* Pull the next datagram queued for the guest and decode it. */
static bool take(pkt_t *o) {
    memset(o, 0, sizeof *o);
    o->rawlen = net_output(&g_ns, o->raw, sizeof o->raw);
    if (!o->rawlen) return false;
    o->src   = r32(o->raw + 12);
    o->dst   = r32(o->raw + 16);
    o->proto = o->raw[9];
    size_t ihl = (size_t)(o->raw[0] & 0x0fu) * 4u;
    const uint8_t *l4 = o->raw + ihl;
    size_t l4len = (size_t)r16(o->raw + 2) - ihl;
    o->sport = r16(l4);
    o->dport = r16(l4 + 2);
    if (o->proto == NET_PROTO_TCP) {
        o->seq = r32(l4 + 4);
        o->ack = r32(l4 + 8);
        o->flags = l4[13];
        o->wnd = r16(l4 + 14);
        size_t doff = (size_t)(l4[12] >> 4) * 4u;
        o->pay = l4 + doff;
        o->paylen = l4len - doff;
    } else if (o->proto == NET_PROTO_UDP) {
        o->pay = l4 + 8;
        o->paylen = (size_t)r16(l4 + 4) - 8u;
    } else {
        o->pay = l4;
        o->paylen = l4len;
    }
    return true;
}

static size_t drain(void) {
    pkt_t p; size_t n = 0;
    while (take(&p)) n++;
    return n;
}

/* Wrap `payload` in an IPv4 header from the guest and feed it in. */
static void feed(uint32_t dst, unsigned proto, const uint8_t *payload, size_t n) {
    uint8_t pkt[NET_MTU];
    size_t hl = net_build_ipv4(pkt, sizeof pkt, GUEST_IP, dst, proto, 0x1234u, n);
    memcpy(pkt + hl, payload, n);
    net_input(&g_ns, pkt, hl + n);
}

static void send_tcp(uint32_t dst, uint16_t sport, uint16_t dport,
                     uint32_t seq, uint32_t ack, uint8_t flags, uint16_t wnd,
                     const uint8_t *data, size_t n,
                     const uint8_t *opt, size_t optn) {
    uint8_t seg[NET_MTU - 20];
    size_t hl = 20u + optn;
    memset(seg, 0, hl);
    w16(seg, sport); w16(seg + 2, dport);
    w32(seg + 4, seq); w32(seg + 8, ack);
    seg[12] = (uint8_t)((hl / 4u) << 4);
    seg[13] = flags;
    w16(seg + 14, wnd);
    if (optn) memcpy(seg + 20, opt, optn);
    if (n) memcpy(seg + hl, data, n);
    w16(seg + 16, 0);
    w16(seg + 16, net_l4_checksum(GUEST_IP, dst, NET_PROTO_TCP, seg, hl + n));
    feed(dst, NET_PROTO_TCP, seg, hl + n);
}

static void send_udp(uint32_t dst, uint16_t sport, uint16_t dport,
                     const uint8_t *data, size_t n) {
    uint8_t u[NET_MTU - 20];
    size_t ulen = 8u + n;
    w16(u, sport); w16(u + 2, dport); w16(u + 4, (uint16_t)ulen); w16(u + 6, 0);
    if (n) memcpy(u + 8, data, n);
    w16(u + 6, net_l4_checksum(GUEST_IP, dst, NET_PROTO_UDP, u, ulen));
    feed(dst, NET_PROTO_UDP, u, ulen);
}

/* ==========================================================================
 * Checksums, pinned before anything relies on them.
 * ======================================================================== */

static void test_checksum_is_rfc1071_and_not_our_opinion_of_it(void) {
    /*
     * The worked IPv4 example everyone checks against: a 20-octet header whose
     * checksum field is zero sums to 0x4E19, so the value that goes on the wire
     * is 0xB1E6. Computed here by hand from RFC 1071 §3 rather than by calling
     * the function twice.
     */
    static const uint8_t hdr[20] = {
        0x45,0x00,0x00,0x3c, 0x1c,0x46,0x40,0x00, 0x40,0x06,0x00,0x00,
        0xac,0x10,0x0a,0x63, 0xac,0x10,0x0a,0x0c
    };
    uint16_t c = net_checksum(hdr, sizeof hdr);
    CHECK(c == 0xb1e6u, "the RFC 1071 worked example summed to 0x%04x, "
                        "expected 0xb1e6", c);

    /* And a header carrying its own checksum sums to zero — the property every
     * receiver actually tests. */
    uint8_t done[20];
    memcpy(done, hdr, sizeof done);
    w16(done + 10, c);
    CHECK(net_checksum(done, sizeof done) == 0u,
          "a header carrying its checksum did not verify to zero");

    /* The degenerate cases, which are where a fold goes wrong. */
    CHECK(net_checksum((const uint8_t *)"", 0) == 0xffffu,
          "the empty sum is not 0xffff");
    static const uint8_t one[2] = { 0x00, 0x01 };
    CHECK(net_checksum(one, 2) == 0xfffeu, "sum of 0x0001 is wrong");
    /* An odd trailing octet pads on the RIGHT: 0xab becomes 0xab00. */
    static const uint8_t odd[1] = { 0xab };
    CHECK(net_checksum(odd, 1) == (uint16_t)~0xab00u,
          "the odd trailing octet was not padded on the right");
    /* The end-around carry: 0xffff + 0x0001 must fold to 0x0001, not 0x0000. */
    static const uint8_t carry[4] = { 0xff, 0xff, 0x00, 0x01 };
    CHECK(net_checksum(carry, 4) == 0xfffeu,
          "the end-around carry folded wrongly: got 0x%04x",
          net_checksum(carry, 4));
}

static void test_pseudo_header_covers_the_addresses(void) {
    /* Same segment, different addresses, must give different sums — that is
     * the entire point of the pseudo-header and the easiest thing to omit. */
    static const uint8_t seg[8] = { 0,53, 0,53, 0,8, 0,0 };
    uint16_t a = net_l4_checksum(GUEST_IP, LOCAL_IP, NET_PROTO_UDP, seg, 8);
    uint16_t b = net_l4_checksum(GUEST_IP, PEER_IP, NET_PROTO_UDP, seg, 8);
    CHECK(a != b, "the pseudo-header ignored the destination address");
    uint16_t c = net_l4_checksum(GUEST_IP, LOCAL_IP, NET_PROTO_TCP, seg, 8);
    CHECK(a != c, "the pseudo-header ignored the protocol");

    /* And a segment carrying its own sum verifies to zero. */
    uint8_t s[8]; memcpy(s, seg, 8);
    w16(s + 6, net_l4_checksum(GUEST_IP, LOCAL_IP, NET_PROTO_UDP, s, 8));
    CHECK(net_l4_checksum(GUEST_IP, LOCAL_IP, NET_PROTO_UDP, s, 8) == 0u,
          "a UDP segment carrying its checksum did not verify");
}

/* ==========================================================================
 * IPv4 admission.
 * ======================================================================== */

static void test_ipv4_refuses_exactly_what_it_says_it_refuses(void) {
    uint8_t pkt[64];
    size_t hl;
    start(true, false);

    /* Too short to hold a header at all. */
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 0x45u;
    net_input(&g_ns, pkt, 19);
    CHECK(g_ns.stats.ip_bad_header == 1u, "a 19-octet datagram was not refused");

    /* Not IPv4. */
    hl = net_build_ipv4(pkt, sizeof pkt, GUEST_IP, LOCAL_IP, NET_PROTO_ICMP, 1, 8);
    pkt[0] = 0x65u;                                   /* version 6, IHL 5     */
    w16(pkt + 10, 0); w16(pkt + 10, net_checksum(pkt, hl));
    net_input(&g_ns, pkt, hl + 8);
    CHECK(g_ns.stats.ip_bad_version == 1u, "a version-6 datagram was accepted");

    /* IHL below 5. */
    hl = net_build_ipv4(pkt, sizeof pkt, GUEST_IP, LOCAL_IP, NET_PROTO_ICMP, 1, 8);
    pkt[0] = 0x44u;
    net_input(&g_ns, pkt, hl + 8);
    CHECK(g_ns.stats.ip_bad_header == 2u, "IHL 4 was accepted");

    /* Total Length claiming more than arrived. */
    hl = net_build_ipv4(pkt, sizeof pkt, GUEST_IP, LOCAL_IP, NET_PROTO_ICMP, 1, 8);
    w16(pkt + 2, 200);
    net_input(&g_ns, pkt, hl + 8);
    CHECK(g_ns.stats.ip_bad_header == 3u, "a truncated datagram was accepted");

    /* A corrupted header checksum. */
    hl = net_build_ipv4(pkt, sizeof pkt, GUEST_IP, LOCAL_IP, NET_PROTO_ICMP, 1, 8);
    pkt[10] ^= 0xffu;
    net_input(&g_ns, pkt, hl + 8);
    CHECK(g_ns.stats.ip_bad_checksum == 1u, "a bad header checksum was accepted");

    /* A fragment, both ways of being one. */
    hl = net_build_ipv4(pkt, sizeof pkt, GUEST_IP, LOCAL_IP, NET_PROTO_ICMP, 1, 8);
    w16(pkt + 6, 0x2000u);                            /* More Fragments       */
    w16(pkt + 10, 0); w16(pkt + 10, net_checksum(pkt, hl));
    net_input(&g_ns, pkt, hl + 8);
    hl = net_build_ipv4(pkt, sizeof pkt, GUEST_IP, LOCAL_IP, NET_PROTO_ICMP, 1, 8);
    w16(pkt + 6, 0x0001u);                            /* a non-zero offset    */
    w16(pkt + 10, 0); w16(pkt + 10, net_checksum(pkt, hl));
    net_input(&g_ns, pkt, hl + 8);
    CHECK(g_ns.stats.ip_frags_refused == 2u,
          "fragments were not refused: %llu",
          (unsigned long long)g_ns.stats.ip_frags_refused);

    /* A source that is not the address IPCP handed out. */
    hl = net_build_ipv4(pkt, sizeof pkt, 0x0a000210u, LOCAL_IP,
                        NET_PROTO_ICMP, 1, 8);
    net_input(&g_ns, pkt, hl + 8);
    CHECK(g_ns.stats.ip_bad_source == 1u, "a forged source address was accepted");

    /* A protocol this NAT does not carry. */
    hl = net_build_ipv4(pkt, sizeof pkt, GUEST_IP, LOCAL_IP, 47u, 1, 8);
    net_input(&g_ns, pkt, hl + 8);
    CHECK(g_ns.stats.proto_unsupported == 1u, "GRE was not counted as unsupported");

    /* Nothing above produced a single datagram for the guest. */
    CHECK(net_output_pending(&g_ns) == 0u,
          "a refused datagram still generated output");
    CHECK(g_ns.stats.ip_in == 9u, "ip_in counted %llu, expected 9",
          (unsigned long long)g_ns.stats.ip_in);
}

static void test_trailing_octets_past_total_length_are_ignored(void) {
    /* A link that pads — and PPP with an odd ACCM will — must not make the
     * datagram look malformed. Total Length is the authority. */
    uint8_t pkt[128], icmp[12];
    start(true, false);
    memset(icmp, 0, sizeof icmp);
    icmp[0] = 8u;                                     /* Echo Request         */
    w16(icmp + 2, 0); w16(icmp + 2, net_checksum(icmp, sizeof icmp));
    size_t hl = net_build_ipv4(pkt, sizeof pkt, GUEST_IP, LOCAL_IP,
                               NET_PROTO_ICMP, 1, sizeof icmp);
    memcpy(pkt + hl, icmp, sizeof icmp);
    memset(pkt + hl + sizeof icmp, 0xee, 16);         /* padding              */
    net_input(&g_ns, pkt, hl + sizeof icmp + 16u);
    CHECK(g_ns.stats.icmp_echo_replies == 1u,
          "padding past Total Length broke an otherwise good datagram");
    pkt_t r;
    CHECK(take(&r) && r.paylen == sizeof icmp,
          "the reply carried the padding: %u octets", (unsigned)r.paylen);
}

/* ==========================================================================
 * ICMP.
 * ======================================================================== */

static void test_icmp_echo_comes_back_from_the_address_it_was_aimed_at(void) {
    uint8_t icmp[16];
    pkt_t r;
    start(true, false);

    memset(icmp, 0, sizeof icmp);
    icmp[0] = 8u; icmp[1] = 0u;
    w16(icmp + 4, 0xbeefu);                           /* identifier           */
    w16(icmp + 6, 7u);                                /* sequence             */
    for (unsigned i = 8; i < sizeof icmp; i++) icmp[i] = (uint8_t)(i * 3u);
    w16(icmp + 2, 0); w16(icmp + 2, net_checksum(icmp, sizeof icmp));

    feed(LOCAL_IP, NET_PROTO_ICMP, icmp, sizeof icmp);
    CHECK(take(&r), "the gateway did not answer an echo request");
    CHECK(r.src == LOCAL_IP && r.dst == GUEST_IP,
          "the reply came from 0x%08x to 0x%08x", r.src, r.dst);
    CHECK(r.proto == NET_PROTO_ICMP, "the reply was protocol %u", r.proto);
    CHECK(r.pay[0] == 0u && r.pay[1] == 0u,
          "the reply is type %u code %u, expected Echo Reply 0/0",
          r.pay[0], r.pay[1]);
    CHECK(net_checksum(r.pay, r.paylen) == 0u,
          "the reply's ICMP checksum does not verify");
    CHECK(r16(r.pay + 4) == 0xbeefu && r16(r.pay + 6) == 7u,
          "identifier/sequence were not echoed: 0x%04x/%u",
          r16(r.pay + 4), r16(r.pay + 6));
    CHECK(memcmp(r.pay + 8, icmp + 8, sizeof icmp - 8u) == 0,
          "the payload was not echoed verbatim — ping's data check would fail");

    /* The resolver address answers too: the guest pings it to test DNS. */
    feed(DNS_IP, NET_PROTO_ICMP, icmp, sizeof icmp);
    CHECK(take(&r) && r.src == DNS_IP,
          "10.0.2.3 did not answer an echo");

    /* A type we cannot answer is counted, not answered. */
    icmp[0] = 13u;                                    /* Timestamp            */
    w16(icmp + 2, 0); w16(icmp + 2, net_checksum(icmp, sizeof icmp));
    feed(LOCAL_IP, NET_PROTO_ICMP, icmp, sizeof icmp);
    CHECK(g_ns.stats.icmp_unsupported == 1u && net_output_pending(&g_ns) == 0u,
          "a Timestamp request was answered as though it were an echo");

    /* A corrupt ICMP checksum is refused before the type is even considered. */
    icmp[0] = 8u;
    w16(icmp + 2, 0); w16(icmp + 2, net_checksum(icmp, sizeof icmp));
    icmp[3] ^= 0x5au;
    feed(LOCAL_IP, NET_PROTO_ICMP, icmp, sizeof icmp);
    CHECK(g_ns.stats.ip_bad_checksum == 1u && net_output_pending(&g_ns) == 0u,
          "an echo with a bad checksum was answered");
}

static void test_an_echo_past_the_gateway_is_never_fabricated(void) {
    uint8_t icmp[12];
    start(true, false);                                /* no icmp_echo member */
    memset(icmp, 0, sizeof icmp);
    icmp[0] = 8u;
    w16(icmp + 2, 0); w16(icmp + 2, net_checksum(icmp, sizeof icmp));

    feed(PEER_IP, NET_PROTO_ICMP, icmp, sizeof icmp);
    CHECK(net_output_pending(&g_ns) == 0u,
          "an echo to 93.184.216.34 was answered by a NAT that never "
          "contacted it — that is a fabricated reply");
    CHECK(g_ns.stats.icmp_unsupported == 1u,
          "the unanswerable echo was not counted");

    /* With a forwarder installed it is handed over and still not answered. */
    start(true, true);
    g_mock.icmp_ok = true;
    feed(PEER_IP, NET_PROTO_ICMP, icmp, sizeof icmp);
    CHECK(g_mock.icmp_calls == 1u, "the echo was not handed to the forwarder");
    CHECK(net_output_pending(&g_ns) == 0u && g_ns.stats.icmp_unsupported == 0u,
          "the forwarded echo was also answered locally");

    /* A forwarder that fails falls back to counting, not to answering. */
    g_mock.icmp_ok = false;
    feed(PEER_IP, NET_PROTO_ICMP, icmp, sizeof icmp);
    CHECK(g_ns.stats.icmp_unsupported == 1u && net_output_pending(&g_ns) == 0u,
          "a failed forward produced a local answer");
}

/* ==========================================================================
 * DNS.
 * ======================================================================== */

/* Build a query for `name` and return its length. */
static size_t dns_query(uint8_t *q, size_t cap, const char *name,
                        uint16_t qtype, uint16_t qclass, uint16_t qdcount) {
    size_t at = 12;
    memset(q, 0, cap);
    w16(q, 0x4242u);
    w16(q + 2, 0x0100u);                              /* RD                   */
    w16(q + 4, qdcount);
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        q[at++] = (uint8_t)l;
        memcpy(q + at, s, l); at += l;
        s = dot ? dot + 1 : s + l;
    }
    q[at++] = 0;
    w16(q + at, qtype); at += 2;
    w16(q + at, qclass); at += 2;
    return at;
}

static void test_dns_answers_an_a_query_from_the_host_resolver(void) {
    uint8_t q[512];
    pkt_t r;
    start(true, false);
    size_t qn = dns_query(q, sizeof q, "www.example.com", 1u, 1u, 1u);
    send_udp(DNS_IP, 40000u, 53u, q, qn);

    CHECK(strcmp(g_mock.resolved, "www.example.com") == 0,
          "the resolver was asked for \"%s\"", g_mock.resolved);
    CHECK(take(&r), "no DNS reply");
    CHECK(r.src == DNS_IP && r.dst == GUEST_IP && r.sport == 53u &&
          r.dport == 40000u, "the reply's addressing is wrong");
    const uint8_t *d = r.pay;
    CHECK(r16(d) == 0x4242u, "the transaction ID was not echoed");
    /*
     * Read ONCE. The first version evaluated r16(d + 2) in the condition and
     * again in the message, and CI printed a flags word that satisfied the
     * condition it had just failed -- so the two reads were not seeing the
     * same thing and the report was describing a different moment than the
     * test. Whatever the underlying cause, a check whose message can disagree
     * with its own condition is not evidence of anything.
     */
    const uint16_t reply_flags = r16(d + 2);
    CHECK((reply_flags & 0x8000u) != 0u,
          "QR was not set: flags 0x%04x, id 0x%04x, %u octets of payload",
          reply_flags, r16(d), (unsigned)r.paylen);
    CHECK((reply_flags & 0x000fu) == 0u, "RCODE is %u, expected NOERROR",
          reply_flags & 0x000fu);
    CHECK((reply_flags & 0x0100u) != 0u,
          "RD was not mirrored back, so a stub resolver sees a server that "
          "ignored its recursion request");
    CHECK(r16(d + 4) == 1u && r16(d + 6) == 1u,
          "QDCOUNT/ANCOUNT are %u/%u, expected 1/1", r16(d + 4), r16(d + 6));
    /* The answer: a pointer to offset 12, type A, class IN, then the address. */
    const uint8_t *a = d + qn;
    CHECK(r16(a) == 0xc00cu, "the answer name is not a pointer to the question");
    CHECK(r16(a + 2) == 1u && r16(a + 4) == 1u, "the answer is not A/IN");
    CHECK(r32(a + 6) == NET_DNS_TTL, "the TTL is %u", r32(a + 6));
    CHECK(r16(a + 10) == 4u, "RDLENGTH is %u, expected 4", r16(a + 10));
    CHECK(r32(a + 12) == PEER_IP, "the address came back as 0x%08x", r32(a + 12));
    CHECK(net_l4_checksum(DNS_IP, GUEST_IP, NET_PROTO_UDP,
                          r.raw + 20, r.rawlen - 20u) == 0u,
          "the reply's UDP checksum does not verify");
    CHECK(g_ns.stats.dns_answered == 1u, "dns_answered did not count");
}

static void test_dns_waits_for_an_async_host_resolver(void) {
    uint8_t q[512];
    pkt_t r;
    start(true, false);
    g_mock.resolve_rc = NET_RES_PENDING;
    size_t qn = dns_query(q, sizeof q, "slow.example.com", 1u, 1u, 1u);
    send_udp(DNS_IP, 40010u, 53u, q, qn);

    CHECK(g_mock.resolve_calls == 1u &&
          strcmp(g_mock.resolved, "slow.example.com") == 0,
          "the first asynchronous resolve was not started exactly once");
    CHECK(!take(&r),
          "a pending host lookup produced an immediate DNS answer");
    CHECK(g_ns.stats.dns_deferred == 1u &&
          g_ns.stats.dns_answered == 0u && g_ns.stats.dns_failed == 0u,
          "pending DNS was not recorded as deferred only");

    g_mock.resolve_rc = NET_RES_OK;
    net_tick(&g_ns, 1u);
    CHECK(g_mock.resolve_calls == 2u,
          "net_tick did not poll the pending resolver");
    CHECK(take(&r) && r.dport == 40010u &&
          (r16(r.pay + 2) & 0xfu) == 0u &&
          r16(r.pay + 6) == 1u && r32(r.pay + qn + 12u) == PEER_IP,
          "the completed asynchronous lookup did not become its DNS answer");
    CHECK(g_ns.stats.dns_answered == 1u && g_ns.stats.dns_failed == 0u,
          "the asynchronous success counters are wrong");
}

static void test_pending_dns_is_bounded_and_times_out(void) {
    uint8_t q[512];
    pkt_t r;
    start(true, false);
    g_mock.resolve_rc = NET_RES_PENDING;

    for (unsigned i = 0u; i < NET_DNS_PENDING + 1u; i++) {
        char name[64];
        snprintf(name, sizeof name, "pending-%u.example.com", i);
        size_t qn = dns_query(q, sizeof q, name, 1u, 1u, 1u);
        q[0] = (uint8_t)(i >> 8u);
        q[1] = (uint8_t)i;
        send_udp(DNS_IP, (uint16_t)(40100u + i), 53u, q, qn);
    }
    CHECK(g_ns.stats.dns_deferred == NET_DNS_PENDING &&
          g_ns.stats.dns_pending_full == 1u,
          "the bounded DNS queue recorded deferred/full as %llu/%llu",
          (unsigned long long)g_ns.stats.dns_deferred,
          (unsigned long long)g_ns.stats.dns_pending_full);
    CHECK(net_output_pending(&g_ns) == 0u,
          "a full pending queue fabricated a DNS failure");

    net_tick(&g_ns, NET_DNS_TIMEOUT_MS - 1u);
    CHECK(net_output_pending(&g_ns) == 0u,
          "a pending lookup timed out early");
    net_tick(&g_ns, NET_DNS_TIMEOUT_MS);
    CHECK(g_ns.stats.dns_timeouts == NET_DNS_PENDING &&
          g_ns.stats.dns_failed == NET_DNS_PENDING,
          "DNS timeout/failure counters are %llu/%llu, expected %u/%u",
          (unsigned long long)g_ns.stats.dns_timeouts,
          (unsigned long long)g_ns.stats.dns_failed,
          NET_DNS_PENDING, NET_DNS_PENDING);
    for (unsigned i = 0u; i < NET_DNS_PENDING; i++) {
        CHECK(take(&r) && (r16(r.pay + 2) & 0xfu) == 2u,
              "timed-out DNS slot %u did not produce SERVFAIL", i);
    }
}

static void test_dns_says_the_right_no(void) {
    uint8_t q[512];
    pkt_t r;
    size_t qn;

    /* NXDOMAIN when the host says the name does not exist. */
    start(true, false);
    g_mock.resolve_rc = NET_RES_NXDOMAIN;
    qn = dns_query(q, sizeof q, "nope.invalid", 1u, 1u, 1u);
    send_udp(DNS_IP, 40001u, 53u, q, qn);
    CHECK(take(&r) && (r16(r.pay + 2) & 0xfu) == 3u,
          "a non-existent name did not get NXDOMAIN");
    CHECK(g_ns.stats.dns_nxdomain == 1u, "dns_nxdomain did not count");

    /* SERVFAIL when it is a transient failure — a different answer, because a
     * guest that caches NXDOMAIN would stop retrying a name that exists. */
    start(true, false);
    g_mock.resolve_rc = NET_RES_FAIL;
    send_udp(DNS_IP, 40002u, 53u, q, qn);
    CHECK(take(&r) && (r16(r.pay + 2) & 0xfu) == 2u,
          "a transient failure did not get SERVFAIL");

    /* AAAA gets NOERROR with no answers, NOT NXDOMAIN (RFC 2308 §2.2). */
    start(true, false);
    qn = dns_query(q, sizeof q, "www.example.com", 28u, 1u, 1u);
    send_udp(DNS_IP, 40003u, 53u, q, qn);
    CHECK(take(&r), "an AAAA query got no reply at all");
    CHECK((r16(r.pay + 2) & 0xfu) == 0u && r16(r.pay + 6) == 0u,
          "AAAA answered rcode %u ancount %u; NODATA is rcode 0 with no "
          "answers, and NXDOMAIN here would poison the A lookup too",
          r16(r.pay + 2) & 0xfu, r16(r.pay + 6));

    /* A class we do not implement. */
    start(true, false);
    qn = dns_query(q, sizeof q, "www.example.com", 1u, 3u /* CHAOS */, 1u);
    send_udp(DNS_IP, 40004u, 53u, q, qn);
    CHECK(take(&r) && (r16(r.pay + 2) & 0xfu) == 4u,
          "class CHAOS did not get NOTIMP");

    /* More than one question has no per-question RCODE, so it is FORMERR and
     * the echoed QDCOUNT is zeroed rather than left claiming two. */
    start(true, false);
    qn = dns_query(q, sizeof q, "www.example.com", 1u, 1u, 2u);
    send_udp(DNS_IP, 40005u, 53u, q, qn);
    CHECK(take(&r) && (r16(r.pay + 2) & 0xfu) == 1u,
          "QDCOUNT 2 did not get FORMERR");
    CHECK(r16(r.pay + 4) == 0u,
          "the FORMERR still claimed %u questions", r16(r.pay + 4));
}

static void test_dns_refuses_names_it_should_not_hand_to_getaddrinfo(void) {
    uint8_t q[512];
    pkt_t r;
    start(true, false);

    /* A compression pointer in a question. RFC 1035 allows pointers where a
     * name appears, but one here can only point back into the header. */
    memset(q, 0, sizeof q);
    w16(q, 1u); w16(q + 2, 0x0100u); w16(q + 4, 1u);
    q[12] = 0xc0u; q[13] = 0x0cu;                     /* pointer to offset 12 */
    w16(q + 14, 1u); w16(q + 16, 1u);
    send_udp(DNS_IP, 40006u, 53u, q, 18);
    CHECK(g_mock.resolved[0] == '\0',
          "a compression pointer reached the resolver as \"%s\"", g_mock.resolved);
    CHECK(take(&r) && (r16(r.pay + 2) & 0xfu) == 1u,
          "a pointer in the question did not get FORMERR");

    /* A label octet outside the preferred syntax — this string was about to be
     * handed to getaddrinfo(). */
    start(true, false);
    size_t qn = dns_query(q, sizeof q, "ok.example", 1u, 1u, 1u);
    q[12 + 1] = ';';
    send_udp(DNS_IP, 40007u, 53u, q, qn);
    CHECK(g_mock.resolved[0] == '\0',
          "a name containing ';' reached the resolver as \"%s\"",
          g_mock.resolved);

    /* A label length that runs past the end of the message. */
    start(true, false);
    memset(q, 0, sizeof q);
    w16(q, 1u); w16(q + 2, 0x0100u); w16(q + 4, 1u);
    q[12] = 40u;                                      /* claims 40 octets     */
    send_udp(DNS_IP, 40008u, 53u, q, 20);
    CHECK(g_mock.resolved[0] == '\0', "an overlong label reached the resolver");
    CHECK(g_ns.stats.dns_malformed >= 1u, "the malformed query was not counted");

    /* A response, not a query: QR set. Answered by nobody. */
    start(true, false);
    qn = dns_query(q, sizeof q, "www.example.com", 1u, 1u, 1u);
    w16(q + 2, 0x8000u);
    send_udp(DNS_IP, 40009u, 53u, q, qn);
    /*
     * Two different faults produce one symptom here, so they are separated.
     * dns_malformed counts the QR guard firing. If it went up, the guard saw
     * the bit and something else queued a datagram; if it did not, the guard
     * never ran and the bit did not survive the trip through udp_input.
     */
    /*
     * The two ends, side by side. dns_last_flags is what dns_input read out of
     * the datagram it was handed; r16(q + 2) is what this test put in. If they
     * differ, the bytes changed somewhere between send_udp() and dns_input()
     * and the fault is in the plumbing; if they agree and the guard still did
     * not fire, the fault is in the comparison.
     */
    CHECK(g_ns.stats.dns_last_flags == r16(q + 2),
          "dns_input saw flags 0x%04x where this test sent 0x%04x "
          "(qdcount seen %u), so the datagram changed in transit",
          g_ns.stats.dns_last_flags, r16(q + 2),
          g_ns.stats.dns_last_qdcount);
    CHECK(g_ns.stats.dns_malformed == 1u,
          "the QR guard did not fire: dns_malformed %llu, dns_queries %llu, "
          "ip_bad_checksum %llu -- flags seen 0x%04x, sent 0x%04x",
          (unsigned long long)g_ns.stats.dns_malformed,
          (unsigned long long)g_ns.stats.dns_queries,
          (unsigned long long)g_ns.stats.ip_bad_checksum,
          g_ns.stats.dns_last_flags, r16(q + 2));
    CHECK(net_output_pending(&g_ns) == 0u,
          "a DNS *response* aimed at us produced a reply, which is how a "
          "reflection loop starts (queued %u; the query we sent had flags "
          "0x%04x at offset 2, and dns_input tests that against 0x8000)",
          (unsigned)net_output_pending(&g_ns), r16(q + 2));
}

/* ==========================================================================
 * UDP.
 * ======================================================================== */

static void test_udp_goes_out_through_a_socket_and_comes_back(void) {
    static const uint8_t msg[5] = { 'h','e','l','l','o' };
    pkt_t r;
    start(true, false);

    send_udp(PEER_IP, 33333u, 5353u, msg, sizeof msg);
    CHECK(g_mock.opens == 1u, "no socket was opened");
    CHECK(g_mock.h[0].proto == NET_PROTO_UDP && g_mock.h[0].ip == PEER_IP &&
          g_mock.h[0].port == 5353u, "the socket went to the wrong place");
    CHECK(g_mock.h[0].sentlen == sizeof msg &&
          memcmp(g_mock.h[0].sent, msg, sizeof msg) == 0,
          "the payload did not reach the socket");
    CHECK(net_flows_open(&g_ns) == 1u, "no flow was created");

    /* A second datagram on the same 4-tuple reuses the flow. */
    send_udp(PEER_IP, 33333u, 5353u, msg, sizeof msg);
    CHECK(g_mock.opens == 1u, "the second datagram opened a second socket");
    CHECK(g_mock.h[0].sentlen == 2u * sizeof msg, "the second datagram was lost");

    /* The host answers. Nothing arrives until net_tick(), which is the only
     * place the egress is polled. */
    static const uint8_t back[3] = { 1, 2, 3 };
    memcpy(g_mock.h[0].give, back, sizeof back);
    g_mock.h[0].givelen = sizeof back;
    CHECK(net_output_pending(&g_ns) == 0u,
          "a datagram appeared without a tick");
    net_tick(&g_ns, 10u);
    CHECK(take(&r), "the host's reply never reached the guest");
    CHECK(r.src == PEER_IP && r.dst == GUEST_IP,
          "the reply is not addressed as though it came from the peer");
    CHECK(r.sport == 5353u && r.dport == 33333u, "the ports were not swapped");
    CHECK(r.paylen == sizeof back && memcmp(r.pay, back, sizeof back) == 0,
          "the reply payload is wrong");
    CHECK(net_l4_checksum(PEER_IP, GUEST_IP, NET_PROTO_UDP,
                          r.raw + 20, r.rawlen - 20u) == 0u,
          "the reply's UDP checksum does not verify");

    /* A different destination port is a different flow. */
    send_udp(PEER_IP, 33333u, 5354u, msg, sizeof msg);
    CHECK(net_flows_open(&g_ns) == 2u, "the second destination shared a flow");
}

static void test_udp_flows_expire_when_idle(void) {
    static const uint8_t msg[2] = { 'a','b' };
    start(true, false);
    send_udp(PEER_IP, 33333u, 5353u, msg, sizeof msg);
    CHECK(net_flows_open(&g_ns) == 1u, "no flow");

    net_tick(&g_ns, NET_UDP_IDLE_MS);
    CHECK(net_flows_open(&g_ns) == 1u,
          "the flow expired exactly at the idle timeout rather than past it");
    net_tick(&g_ns, NET_UDP_IDLE_MS + 1u);
    CHECK(net_flows_open(&g_ns) == 0u, "the idle flow was never expired");
    CHECK(g_mock.h[0].closed, "the expired flow did not close its socket");
}

static void test_udp_with_a_zero_checksum_is_accepted_and_a_wrong_one_is_not(void) {
    uint8_t u[16];
    start(true, false);

    /* RFC 768: zero means "not computed". */
    memset(u, 0, sizeof u);
    w16(u, 33333u); w16(u + 2, 5353u); w16(u + 4, 10u); w16(u + 6, 0);
    feed(PEER_IP, NET_PROTO_UDP, u, 10);
    CHECK(g_mock.opens == 1u, "a zero-checksum datagram was refused");

    /* Anything else must verify. */
    start(true, false);
    memset(u, 0, sizeof u);
    w16(u, 33333u); w16(u + 2, 5353u); w16(u + 4, 10u);
    w16(u + 6, 0x1234u);                              /* not the right sum    */
    feed(PEER_IP, NET_PROTO_UDP, u, 10);
    CHECK(g_ns.stats.ip_bad_checksum == 1u && g_mock.opens == 0u,
          "a UDP datagram with a wrong checksum was forwarded");

    /* A UDP Length that disagrees with the datagram. */
    start(true, false);
    memset(u, 0, sizeof u);
    w16(u, 33333u); w16(u + 2, 5353u); w16(u + 4, 200u); w16(u + 6, 0);
    feed(PEER_IP, NET_PROTO_UDP, u, 10);
    CHECK(g_ns.stats.ip_bad_header == 1u, "an impossible UDP Length was accepted");
}

/* ==========================================================================
 * TCP.
 * ======================================================================== */

/* Drive the guest's half of a handshake and leave the flow ESTABLISHED.
 * Returns our initial sequence (the SYN-ACK's), so the caller can talk. */
static uint32_t handshake(uint16_t gport, uint32_t *guest_seq) {
    static const uint8_t mss_opt[4] = { 2u, 4u, 0x05, 0xb4 };  /* 1460        */
    pkt_t r;
    uint32_t gs = 0x11110000u + gport;
    send_tcp(PEER_IP, gport, 80u, gs, 0, TCP_SYN, 8192u, NULL, 0,
             mss_opt, sizeof mss_opt);
    if (!take(&r)) return 0;
    if ((r.flags & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK)) return 0;
    uint32_t iss = r.seq;
    send_tcp(PEER_IP, gport, 80u, gs + 1u, iss + 1u, TCP_ACK, 8192u,
             NULL, 0, NULL, 0);
    if (guest_seq) *guest_seq = gs + 1u;
    return iss + 1u;                    /* our next sequence to send from     */
}

static void test_the_handshake_waits_for_the_host_connect(void) {
    pkt_t r;
    start(true, false);

    /* Hold the connect PENDING and the guest gets silence, not a SYN-ACK to a
     * socket that does not exist yet. Armed before the SYN: tcp_syn() polls
     * the status inside the same call so a loopback connect can finish there. */
    g_mock.open_status = NET_ST_PENDING;
    send_tcp(PEER_IP, 50000u, 80u, 1000u, 0, TCP_SYN, 8192u, NULL, 0, NULL, 0);
    net_tick(&g_ns, 1u);
    CHECK(net_output_pending(&g_ns) == 0u,
          "the SYN was acknowledged while the host connect was still pending");
    CHECK(g_ns.stats.tcp_syns == 1u, "the SYN did not create a flow");

    /* A retransmitted SYN in that window is also answered with silence. */
    send_tcp(PEER_IP, 50000u, 80u, 1000u, 0, TCP_SYN, 8192u, NULL, 0, NULL, 0);
    CHECK(net_output_pending(&g_ns) == 0u,
          "a retransmitted SYN produced a SYN-ACK before the connect landed");

    /* Now it connects. */
    g_mock.h[0].status = NET_ST_READY;
    net_tick(&g_ns, 2u);
    CHECK(take(&r), "the connect completed but no SYN-ACK was sent");
    CHECK((r.flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK),
          "flags are 0x%02x, expected SYN|ACK", r.flags);
    CHECK(r.ack == 1001u, "the SYN-ACK acknowledges %u, expected 1001", r.ack);
    CHECK(r.src == PEER_IP && r.sport == 80u,
          "the SYN-ACK is not addressed as though it came from the peer");

    /* The MSS option is present and clamped. */
    const uint8_t *l4 = r.raw + 20;
    size_t doff = (size_t)(l4[12] >> 4) * 4u;
    CHECK(doff == 24u, "the SYN-ACK data offset is %u, expected 24 with an "
                       "MSS option", (unsigned)doff);
    CHECK(l4[20] == 2u && l4[21] == 4u && r16(l4 + 22) == NET_TCP_MSS_MAX,
          "the advertised MSS is %u, expected %u", r16(l4 + 22),
          (unsigned)NET_TCP_MSS_MAX);

    /* The guest's ACK completes it. */
    send_tcp(PEER_IP, 50000u, 80u, 1001u, r.seq + 1u, TCP_ACK, 8192u,
             NULL, 0, NULL, 0);
    CHECK(g_ns.stats.tcp_established == 1u, "the flow never reached ESTABLISHED");
}

static void test_a_refused_connect_resets_rather_than_hanging(void) {
    pkt_t r;
    start(true, false);
    g_mock.refuse_open = true;
    send_tcp(PEER_IP, 50001u, 80u, 2000u, 0, TCP_SYN, 8192u, NULL, 0, NULL, 0);
    CHECK(take(&r), "a refused connect produced no answer at all — the guest "
                    "would retransmit its SYN for a minute");
    CHECK((r.flags & TCP_RST) != 0u, "flags 0x%02x, expected RST", r.flags);
    CHECK(g_ns.stats.tcp_refused == 1u, "tcp_refused did not count");
    CHECK(net_flows_open(&g_ns) == 0u, "the refused flow was left allocated");

    /* And a connect that fails later, after the socket existed. */
    start(true, false);
    g_mock.open_status = NET_ST_PENDING;
    send_tcp(PEER_IP, 50002u, 80u, 2000u, 0, TCP_SYN, 8192u, NULL, 0, NULL, 0);
    net_tick(&g_ns, 1u);
    drain();
    g_mock.h[0].status = NET_ST_FAILED;
    net_tick(&g_ns, 2u);
    CHECK(take(&r) && (r.flags & TCP_RST), "a failed connect did not reset");
    CHECK(net_flows_open(&g_ns) == 0u, "the failed flow was left allocated");
}

static void test_data_crosses_in_both_directions(void) {
    static const uint8_t req[] = "GET / HTTP/1.0\r\n\r\n";
    static const uint8_t rsp[] = "HTTP/1.0 200 OK\r\n\r\nhi";
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50010u, &gseq);
    CHECK(ours != 0u, "the handshake did not complete");
    drain();

    /* Guest -> host. It is buffered on input and pushed on the next tick. */
    send_tcp(PEER_IP, 50010u, 80u, gseq, ours, TCP_ACK | TCP_PSH, 8192u,
             req, sizeof req - 1u, NULL, 0);
    CHECK(g_mock.h[0].sentlen == sizeof req - 1u &&
          memcmp(g_mock.h[0].sent, req, sizeof req - 1u) == 0,
          "the request did not reach the socket (%u octets)",
          (unsigned)g_mock.h[0].sentlen);
    CHECK(take(&r) && (r.flags & TCP_ACK) &&
          r.ack == gseq + (uint32_t)(sizeof req - 1u),
          "the data was not acknowledged");
    CHECK(g_ns.stats.tcp_bytes_to_host == sizeof req - 1u,
          "tcp_bytes_to_host is %llu",
          (unsigned long long)g_ns.stats.tcp_bytes_to_host);

    /* Host -> guest. */
    memcpy(g_mock.h[0].give, rsp, sizeof rsp - 1u);
    g_mock.h[0].givelen = sizeof rsp - 1u;
    net_tick(&g_ns, 10u);
    CHECK(take(&r), "the response never reached the guest");
    CHECK(r.paylen == sizeof rsp - 1u &&
          memcmp(r.pay, rsp, sizeof rsp - 1u) == 0,
          "the response payload is wrong (%u octets)", (unsigned)r.paylen);
    CHECK(r.seq == ours, "the response starts at sequence %u, expected %u",
          r.seq, ours);
    CHECK(net_l4_checksum(PEER_IP, GUEST_IP, NET_PROTO_TCP,
                          r.raw + 20, r.rawlen - 20u) == 0u,
          "the segment's checksum does not verify");
}

/*
 * BigBoss's current compressed package index is about 1.3 MB. Before using a
 * repository rewrite to hide its failure, prove that the byte-stream bridge
 * can carry a response of that size across thousands of bounded refills and
 * acknowledgements. The mock generates bytes instead of storing the body, so
 * this remains a fast deterministic unit test rather than a network fixture.
 */
static void test_a_bigboss_sized_stream_arrives_byte_exact(void) {
    const size_t total = 1317743u;
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50015u, &gseq);
    CHECK(ours != 0u, "the handshake did not complete");
    drain();

    g_mock.h[0].stream_left = total;
    g_mock.h[0].give_chunk = 997u;  /* deliberately unrelated to MSS/buffer */
    g_mock.h[0].recv_err = NET_EG_EOF;

    size_t received = 0u;
    uint32_t expected_seq = ours;
    bool sequence_ok = true;
    bool bytes_ok = true;
    bool saw_fin = false;
    bool saw_rst = false;

    for (uint32_t now = 1u; now < 100000u && !saw_fin; now++) {
        net_tick(&g_ns, now);
        while (take(&r)) {
            if (r.flags & TCP_RST) saw_rst = true;
            if (r.paylen) {
                if (r.seq != expected_seq) sequence_ok = false;
                for (size_t i = 0; i < r.paylen; i++) {
                    size_t at = received + i;
                    uint8_t want =
                        (uint8_t)(((at * 131u + 17u) ^ (at >> 7u)) & 0xffu);
                    if (r.pay[i] != want) bytes_ok = false;
                }
                received += r.paylen;
                expected_seq += (uint32_t)r.paylen;
            }
            if (r.flags & TCP_FIN) {
                if (r.seq != expected_seq) sequence_ok = false;
                saw_fin = true;
            }
            if (r.paylen || (r.flags & TCP_FIN)) {
                uint32_t ack = expected_seq + (saw_fin ? 1u : 0u);
                send_tcp(PEER_IP, 50015u, 80u, gseq, ack, TCP_ACK,
                         8192u, NULL, 0, NULL, 0);
            }
        }
    }

    CHECK(received == total, "received %zu of %zu stream octets", received,
          total);
    CHECK(sequence_ok, "the long stream had a gap, overlap, or misplaced FIN");
    CHECK(bytes_ok, "the long stream changed at least one octet");
    CHECK(saw_fin && !saw_rst, "the long stream ended with fin/reset=%u/%u",
          saw_fin, saw_rst);
    CHECK(g_ns.stats.tcp_bytes_to_guest == total,
          "the TCP byte counter is %llu, expected %zu",
          (unsigned long long)g_ns.stats.tcp_bytes_to_guest, total);
    CHECK(g_ns.stats.tcp_retransmits == 0u && g_ns.stats.out_dropped == 0u,
          "an ideal long stream retransmitted/dropped %llu/%llu times",
          (unsigned long long)g_ns.stats.tcp_retransmits,
          (unsigned long long)g_ns.stats.out_dropped);
}

/* A 64 KiB advertised window is capacity, not a safe initial burst.  Pace a
 * new bulk flow with an acknowledgement clock, then grow it only after the
 * guest proves that it drained the first flight. */
static void test_bulk_data_starts_with_a_bounded_ack_clocked_flight(void) {
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50017u, &gseq);
    CHECK(ours != 0u, "the bulk-window handshake did not complete");
    drain();

    /* Window updates carry no payload but immediately replace snd_wnd. */
    send_tcp(PEER_IP, 50017u, 80u, gseq, ours, TCP_ACK, 65535u,
             NULL, 0u, NULL, 0u);
    drain();

    g_mock.h[0].stream_left = 64u * 1024u;
    g_mock.h[0].recv_err = NET_EG_WOULDBLOCK;
    net_tick(&g_ns, 10u);

    size_t queued = net_output_pending(&g_ns);
    size_t payload = 0u;
    while (take(&r)) payload += r.paylen;
    const size_t initial = 3u * NET_TCP_MSS_MAX; /* RFC 3390 at MSS 1460 */
    CHECK(queued == 3u && payload == initial,
          "the initial bulk flight is datagrams/octet=%zu/%zu, expected 3/%zu",
          queued, payload, initial);
    CHECK(g_ns.stats.tcp_bytes_to_guest == payload,
          "the initial byte counter/report disagree: %llu/%zu",
          (unsigned long long)g_ns.stats.tcp_bytes_to_guest, payload);

    send_tcp(PEER_IP, 50017u, 80u, gseq, ours + (uint32_t)payload,
             TCP_ACK, 65535u, NULL, 0u, NULL, 0u);
    queued = net_output_pending(&g_ns);
    size_t second = 0u;
    while (take(&r)) second += r.paylen;
    CHECK(queued == NET_TCP_CWND_MAX_SEGMENTS &&
          second == NET_TCP_CWND_MAX_SEGMENTS * NET_TCP_MSS_MAX,
          "the ACK-clocked second flight is datagrams/octet=%zu/%zu",
          queued, second);

    send_tcp(PEER_IP, 50017u, 80u, gseq,
             ours + (uint32_t)payload + (uint32_t)second,
             TCP_ACK, 65535u, NULL, 0u, NULL, 0u);
    queued = net_output_pending(&g_ns);
    size_t third = 0u;
    while (take(&r)) third += r.paylen;
    CHECK(queued == NET_TCP_CWND_MAX_SEGMENTS &&
          third == NET_TCP_CWND_MAX_SEGMENTS * NET_TCP_MSS_MAX,
          "the bounded steady flight is datagrams/octet=%zu/%zu",
          queued, third);
    CHECK(g_ns.stats.out_dropped == 0u,
          "paced bulk output still overfilled its bounded queue");
}

/* Acknowledging bytes merely buffered for transmission must not remove them
 * or admit any other state changes carried by that invalid segment. */
static void test_future_ack_cannot_discard_unsent_data_or_admit_its_payload(void) {
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50110u, &gseq);
    drain();
    send_tcp(PEER_IP, 50110u, 80u, gseq, ours, TCP_ACK, 8u,
             NULL, 0u, NULL, 0u);
    for (unsigned i = 0u; i < 16u; i++) g_mock.h[0].give[i] = (uint8_t)i;
    g_mock.h[0].givelen = 16u;
    net_tick(&g_ns, 10u);
    CHECK(take(&r) && r.seq == ours && r.paylen == 8u && !take(&r),
          "small window did not leave eight bytes unsent");
    uint32_t deadline = g_ns.flow[0].rto_at;

    /* The second half is buffered but has NEVER been emitted. An ACK for it
     * must not discard that half, update the window, deliver data, or close
     * the guest's side merely because this invalid segment also carries FIN. */
    static const uint8_t bad[] = { 'b', 'a', 'd' };
    send_tcp(PEER_IP, 50110u, 80u, gseq, ours + 16u,
             TCP_ACK | TCP_FIN, 0u, bad, sizeof bad, NULL, 0u);
    net_flow_t *f = &g_ns.flow[0];
    CHECK(f->used && f->state == NET_TCP_ESTABLISHED &&
          f->snd_una == ours && f->snd_nxt == ours + 8u && f->txlen == 16u,
          "future ACK discarded unsent data or changed connection state");
    CHECK(f->snd_wnd == 8u && f->rcv_nxt == gseq &&
          f->rto_on && f->rto_at == deadline &&
          g_mock.h[0].sentlen == 0u && !g_mock.h[0].shutdown,
          "invalid ACK changed window/timer or admitted its data/FIN");
    CHECK(take(&r) && r.flags == TCP_ACK && r.seq == ours + 8u &&
          r.ack == gseq && r.paylen == 0u && !take(&r),
          "future ACK did not receive only the current empty acknowledgement");

    send_tcp(PEER_IP, 50110u, 80u, gseq, ours + 8u, TCP_ACK, 8u,
             NULL, 0u, NULL, 0u);
    CHECK(take(&r) && r.seq == ours + 8u && r.paylen == 8u &&
          memcmp(r.pay, g_mock.h[0].give + 8u, 8u) == 0,
          "the valid ACK could not release the preserved unsent bytes");
}

static void test_packet_transport_grows_after_real_acks(void) {
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    g_ns.cfg.tcp_cwnd_segments = NET_OUT_SLOTS;
    uint32_t ours = handshake(50017u, &gseq);
    CHECK(ours != 0u, "packet transport handshake failed");
    drain();
    send_tcp(PEER_IP, 50017u, 80u, gseq, ours, TCP_ACK, 65535u,
             NULL, 0u, NULL, 0u);
    drain();
    g_mock.h[0].stream_left = 512u * 1024u;
    g_mock.h[0].recv_err = NET_EG_WOULDBLOCK;
    net_tick(&g_ns, 10u);
    CHECK(net_output_pending(&g_ns) == 3u,
          "packet offload bypassed the initial congestion window");
    size_t max_flight = 0u;
    for (unsigned round = 0; round < 20u; round++) {
        size_t bytes = 0u;
        while (take(&r)) bytes += r.paylen;
        if (bytes > max_flight) max_flight = bytes;
        ours += (uint32_t)bytes;
        send_tcp(PEER_IP, 50017u, 80u, gseq, ours, TCP_ACK, 65535u,
                 NULL, 0u, NULL, 0u);
        net_tick(&g_ns, 11u + round);
    }
    CHECK(max_flight > 3u * NET_TCP_MSS_MAX,
          "new packet transport still has the serial three-packet ceiling");
    CHECK(max_flight <= NET_OUT_SLOTS * NET_TCP_MSS_MAX,
          "packet transport exceeded its bounded flight capacity");
    CHECK(g_ns.stats.tcp_retransmits == 0u,
          "ideal packet transport unexpectedly retransmitted");
}

static void test_ack_cannot_establish_before_syn_ack_was_sent(void) {
    pkt_t r;
    start(true, false);
    g_mock.open_status = NET_ST_PENDING;
    send_tcp(PEER_IP, 50111u, 80u, 1000u, 0u, TCP_SYN, 8192u,
             NULL, 0u, NULL, 0u);
    CHECK(!take(&r), "pending host connect unexpectedly sent a SYN-ACK");
    net_flow_t *f = &g_ns.flow[0];
    uint32_t iss = f->snd_una;
    static const uint8_t payload = 0x5au;
    send_tcp(PEER_IP, 50111u, 80u, 1001u, iss, TCP_ACK, 8192u,
             &payload, 1u, NULL, 0u);
    CHECK(f->used && f->state == NET_TCP_SYN_RCVD &&
          g_ns.stats.tcp_established == 0u && g_mock.h[0].sentlen == 0u,
          "ACK established or sent data through a still-pending connection");
    CHECK(take(&r) && r.flags == TCP_RST && r.seq == iss && !take(&r),
          "unacceptable handshake ACK was not rejected with a reset");
    g_mock.h[0].status = NET_ST_READY;
    net_tick(&g_ns, 10u);
    CHECK(take(&r) && r.flags == (TCP_SYN | TCP_ACK) && r.seq == iss,
          "rejecting an invalid ACK broke the eventual real handshake");
}

static void test_ack_of_original_flight_survives_retransmit_rewind(void) {
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50112u, &gseq);
    drain();
    CHECK(g_ns.stats.tcp_bytes_acked_by_guest == 0u,
          "SYN was counted as acknowledged payload");
    g_mock.h[0].stream_left = 6u * NET_TCP_MSS_MAX;
    net_tick(&g_ns, 10u);
    CHECK(drain() == 3u, "initial flight was not three segments");
    net_tick(&g_ns, 10u + NET_TCP_RTO_MS);
    CHECK(take(&r) && r.seq == ours && r.paylen == NET_TCP_MSS_MAX &&
          !take(&r), "timeout did not rewind to one retransmitted segment");
    CHECK(g_ns.stats.tcp_bytes_acked_by_guest == 0u,
          "emitting/retransmitting data was counted as guest receipt");

    uint32_t original_end = ours + 3u * NET_TCP_MSS_MAX;
    send_tcp(PEER_IP, 50112u, 80u, gseq, original_end, TCP_ACK, 0u,
             NULL, 0u, NULL, 0u);
    net_flow_t *f = &g_ns.flow[0];
    CHECK(f->snd_una == original_end && f->snd_nxt == original_end &&
          f->txlen == 3u * NET_TCP_MSS_MAX && f->rtx == 0u,
          "a real ACK beyond the rewind point was incorrectly refused");
    CHECK(g_ns.stats.tcp_bytes_acked_by_guest == 3u * NET_TCP_MSS_MAX,
          "cumulative payload ACK was not counted exactly once");
    drain();
    send_tcp(PEER_IP, 50112u, 80u, gseq, original_end + 1u, TCP_ACK, 0u,
             NULL, 0u, NULL, 0u);
    CHECK(f->snd_una == original_end && f->txlen == 3u * NET_TCP_MSS_MAX,
          "the rewind exception admitted a byte never in the original flight");
    send_tcp(PEER_IP, 50112u, 80u, gseq, original_end, TCP_ACK, 0u,
             NULL, 0u, NULL, 0u);
    CHECK(g_ns.stats.tcp_bytes_acked_by_guest == 3u * NET_TCP_MSS_MAX,
          "invalid or duplicate ACK inflated guest receipt");
}

/* A full bounded queue is backpressure, not evidence that bytes reached the
 * guest. Once room returns, TCP must send immediately rather than waiting an
 * emulated-second RTO for data it only imagined it had emitted. */
static void test_a_full_output_queue_does_not_strand_tcp_data(void) {
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50016u, &gseq);
    CHECK(ours != 0u, "the handshake did not complete");
    drain();

    uint8_t icmp[12];
    memset(icmp, 0, sizeof icmp);
    icmp[0] = 8u;
    w16(icmp + 2, net_checksum(icmp, sizeof icmp));
    for (unsigned i = 0; i < NET_OUT_SLOTS; i++)
        feed(LOCAL_IP, NET_PROTO_ICMP, icmp, sizeof icmp);
    CHECK(net_output_pending(&g_ns) == NET_OUT_SLOTS,
          "only %zu of %u queue slots filled", net_output_pending(&g_ns),
          (unsigned)NET_OUT_SLOTS);

    for (unsigned i = 0; i < 100u; i++) g_mock.h[0].give[i] = (uint8_t)i;
    g_mock.h[0].givelen = 100u;
    net_tick(&g_ns, 10u);       /* the first send attempt has nowhere to go */
    CHECK(net_output_pending(&g_ns) == NET_OUT_SLOTS,
          "the rejected TCP segment overwrote queued output");

    drain();                    /* the attachment made room for the packet  */
    net_tick(&g_ns, 11u);       /* well before NET_TCP_RTO_MS               */
    CHECK(take(&r) && r.paylen == 100u && r.seq == ours,
          "TCP data was stranded until an RTO after output backpressure");
}

static void test_the_guests_window_bounds_what_we_send(void) {
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50020u, &gseq);
    drain();

    /* 3000 octets waiting, and a window of 100. */
    for (unsigned i = 0; i < 3000u; i++) g_mock.h[0].give[i] = (uint8_t)i;
    g_mock.h[0].givelen = 3000u;
    send_tcp(PEER_IP, 50020u, 80u, gseq, ours, TCP_ACK, 100u, NULL, 0, NULL, 0);
    net_tick(&g_ns, 10u);

    size_t got = 0;
    while (take(&r)) got += r.paylen;
    CHECK(got == 100u, "with a 100-octet window we sent %u octets",
          (unsigned)got);

    /* Open the window and the rest follows, in MSS-sized segments. */
    send_tcp(PEER_IP, 50020u, 80u, gseq, ours + 100u, TCP_ACK, 8192u,
             NULL, 0, NULL, 0);
    net_tick(&g_ns, 20u);
    size_t biggest = 0; got = 0;
    while (take(&r)) { got += r.paylen; if (r.paylen > biggest) biggest = r.paylen; }
    CHECK(got > 0u, "opening the window sent nothing");
    CHECK(biggest <= NET_TCP_MSS_MAX,
          "a %u-octet segment exceeded the MSS", (unsigned)biggest);
}

static void test_live_tcp_status_names_the_buffered_flow(void) {
    pkt_t packet;
    uint32_t guest_seq;
    start(true, false);

    net_tcp_live_status_t live;
    memset(&live, 0xa5, sizeof live);
    CHECK(!net_get_tcp_live_status(NULL, &live) && live.flows == 0u,
          "NULL stack produced a live TCP flow");
    CHECK(!net_get_tcp_live_status(&g_ns, &live) && live.flows == 0u,
          "an empty stack produced a live TCP flow");
    CHECK(!net_get_tcp_live_status(&g_ns, NULL),
          "a NULL live-status destination succeeded");

    uint32_t ours = handshake(50021u, &guest_seq);
    CHECK(ours != 0u, "the live-status handshake did not complete");
    drain();

    send_tcp(PEER_IP, 50021u, 80u, guest_seq, ours, TCP_ACK, 100u,
             NULL, 0u, NULL, 0u);
    drain();
    g_mock.h[0].stream_left = 3000u;
    g_mock.h[0].recv_err = NET_EG_WOULDBLOCK;
    net_tick(&g_ns, 10u);

    CHECK(net_get_tcp_live_status(&g_ns, &live),
          "the established flow was not reported");
    CHECK(live.flows == 1u && live.state == NET_TCP_ESTABLISHED &&
          live.guest_port == 50021u && live.dst_port == 80u,
          "live identity is flows/state/ports=%u/%u/%u/%u",
          live.flows, (unsigned)live.state, (unsigned)live.guest_port,
          (unsigned)live.dst_port);
    CHECK(live.window == 100u && live.mss == NET_TCP_MSS_MAX &&
          live.congestion_window == 3u * NET_TCP_MSS_MAX &&
          live.slow_start_threshold ==
              NET_TCP_CWND_MAX_SEGMENTS * NET_TCP_MSS_MAX &&
          live.inflight == 100u &&
          live.tx_buffered == 3000u && live.rx_buffered == 0u,
          "live bottleneck is wnd/mss/cwnd/ss/fly/tx/rx="
          "%u/%u/%u/%u/%u/%u/%u",
          live.window, (unsigned)live.mss, live.congestion_window,
          live.slow_start_threshold, live.inflight, live.tx_buffered,
          live.rx_buffered);
    CHECK(live.retries == 0u && live.rto_remaining_ms == NET_TCP_RTO_MS &&
          live.flags == NET_TCP_LIVE_RTO_ON,
          "live timer is retries/remaining/flags=%u/%u/%x",
          live.retries, live.rto_remaining_ms, live.flags);
    while (take(&packet)) { }
}

static void test_a_small_mss_from_the_guest_is_honoured(void) {
    static const uint8_t mss_opt[4] = { 2u, 4u, 0x00, 0x80 };   /* 128        */
    pkt_t r;
    start(true, false);
    send_tcp(PEER_IP, 50030u, 80u, 3000u, 0, TCP_SYN, 8192u, NULL, 0,
             mss_opt, sizeof mss_opt);
    CHECK(take(&r), "no SYN-ACK");
    uint32_t ours = r.seq + 1u;
    send_tcp(PEER_IP, 50030u, 80u, 3001u, ours, TCP_ACK, 8192u, NULL, 0, NULL, 0);
    drain();

    for (unsigned i = 0; i < 1000u; i++) g_mock.h[0].give[i] = (uint8_t)i;
    g_mock.h[0].givelen = 1000u;
    net_tick(&g_ns, 10u);
    size_t biggest = 0;
    while (take(&r)) if (r.paylen > biggest) biggest = r.paylen;
    CHECK(biggest == 128u,
          "the guest asked for a 128-octet MSS and we sent %u",
          (unsigned)biggest);
}

static void test_a_peer_without_an_mss_option_gets_a_bounded_initial_flight(void) {
    pkt_t r;
    net_tcp_live_status_t live;
    start(true, false);
    send_tcp(PEER_IP, 50031u, 80u, 4000u, 0u, TCP_SYN, 65535u,
             NULL, 0u, NULL, 0u);
    CHECK(take(&r), "no SYN-ACK for a peer without an MSS option");
    uint32_t ours = r.seq + 1u;
    send_tcp(PEER_IP, 50031u, 80u, 4001u, ours, TCP_ACK, 65535u,
             NULL, 0u, NULL, 0u);
    drain();

    g_mock.h[0].stream_left = 6000u;
    net_tick(&g_ns, 10u);
    size_t queued = net_output_pending(&g_ns);
    size_t payload = 0u;
    while (take(&r)) payload += r.paylen;
    CHECK(queued == 4u && payload == 4u * 536u,
          "default-MSS initial flight is datagrams/octet=%zu/%zu",
          queued, payload);
    CHECK(net_get_tcp_live_status(&g_ns, &live) && live.mss == 536u &&
          live.congestion_window == 4u * 536u,
          "default-MSS telemetry is mss/cwnd=%u/%u",
          (unsigned)live.mss, live.congestion_window);
}

static void test_out_of_order_is_reacked_and_never_reassembled(void) {
    static const uint8_t a[4] = { 'a','a','a','a' };
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50040u, &gseq);
    drain();

    /* A segment from the future. We keep no reassembly queue, so it is dropped
     * and the cumulative ACK repeated. */
    send_tcp(PEER_IP, 50040u, 80u, gseq + 100u, ours, TCP_ACK, 8192u,
             a, sizeof a, NULL, 0);
    CHECK(g_mock.h[0].sentlen == 0u,
          "an out-of-order segment was delivered to the socket");
    CHECK(take(&r) && r.ack == gseq,
          "the duplicate ACK did not point at the hole (ack %u, expected %u)",
          r.ack, gseq);
    CHECK(g_ns.stats.tcp_out_of_order == 1u, "the gap was not counted");

    /* The missing segment arrives; then the retransmission of it overlaps and
     * only the new tail is taken. */
    send_tcp(PEER_IP, 50040u, 80u, gseq, ours, TCP_ACK, 8192u, a, sizeof a,
             NULL, 0);
    CHECK(g_mock.h[0].sentlen == 4u, "the in-order segment was not delivered");
    drain();
    send_tcp(PEER_IP, 50040u, 80u, gseq, ours, TCP_ACK, 8192u, a, sizeof a,
             NULL, 0);
    CHECK(g_mock.h[0].sentlen == 4u,
          "a fully duplicate segment was delivered twice (%u octets)",
          (unsigned)g_mock.h[0].sentlen);
    CHECK(take(&r) && r.ack == gseq + 4u,
          "the duplicate was not re-acknowledged at the right place");
}

static void test_backpressure_shrinks_the_window_and_never_drops(void) {
    uint8_t big[2000];
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50050u, &gseq);
    drain();
    for (unsigned i = 0; i < sizeof big; i++) big[i] = (uint8_t)i;

    /* The socket accepts 10 octets per call and no more. */
    g_mock.h[0].accept_limit = 10;
    send_tcp(PEER_IP, 50050u, 80u, gseq, ours, TCP_ACK, 8192u, big, 1000u,
             NULL, 0);
    CHECK(take(&r), "no ACK for the first burst");
    uint16_t w1 = r.wnd;
    CHECK(w1 < NET_TCP_RXBUF,
          "the window stayed at %u with 990 octets stuck in the buffer", w1);
    CHECK(g_mock.h[0].sentlen == 10u, "the socket took %u octets, expected 10",
          (unsigned)g_mock.h[0].sentlen);

    /* Nothing was dropped: rcv_nxt advanced by the whole 1000. */
    CHECK(r.ack == gseq + 1000u,
          "we acknowledged %u of 1000 octets — back-pressure must buffer, "
          "not discard", r.ack - gseq);

    /* Ticks drain it 10 at a time and the window reopens. */
    for (unsigned i = 0; i < 200u; i++) net_tick(&g_ns, 100u + i);
    CHECK(g_mock.h[0].sentlen == 1000u,
          "only %u octets ever reached the socket",
          (unsigned)g_mock.h[0].sentlen);
    drain();
    send_tcp(PEER_IP, 50050u, 80u, gseq + 1000u, ours, TCP_ACK, 8192u,
             big, 1u, NULL, 0);
    /* Fully open, not RXBUF-1: the octet that arrived was handed to the socket
     * inside the same call, so by the time the ACK is composed the buffer is
     * empty again. A window that reopened only to RXBUF-1 would mean the
     * drain had not happened yet. */
    CHECK(take(&r) && r.wnd == NET_TCP_RXBUF,
          "the window did not reopen: %u", r.wnd);
}

static void test_a_reset_from_the_guest_frees_the_flow(void) {
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50060u, &gseq);
    drain();
    CHECK(net_flows_open(&g_ns) == 1u, "no flow to reset");
    send_tcp(PEER_IP, 50060u, 80u, gseq, ours, TCP_RST, 0u, NULL, 0, NULL, 0);
    CHECK(net_flows_open(&g_ns) == 0u, "the flow survived a RST");
    CHECK(g_mock.h[0].closed, "the socket was not closed");
    CHECK(g_ns.stats.tcp_resets_in == 1u, "the reset was not counted");
    CHECK(g_ns.stats.tcp_last_peer_reset_state == NET_TCP_ESTABLISHED,
          "the reset snapshot lost the live state: %u",
          g_ns.stats.tcp_last_peer_reset_state);
    CHECK(g_ns.stats.tcp_last_peer_reset_window == 8192u &&
          g_ns.stats.tcp_last_peer_reset_inflight == 0u,
          "the reset snapshot has window/inflight %u/%u",
          g_ns.stats.tcp_last_peer_reset_window,
          g_ns.stats.tcp_last_peer_reset_inflight);
    CHECK(net_output_pending(&g_ns) == 0u, "we answered a RST with a segment");
}

static void test_a_segment_for_no_connection_is_reset_both_ways(void) {
    pkt_t r;
    start(true, false);

    /* With ACK: the RST takes its sequence from the ACK field (RFC 793 §3.4). */
    send_tcp(PEER_IP, 50070u, 80u, 5000u, 9999u, TCP_ACK, 8192u, NULL, 0, NULL, 0);
    CHECK(take(&r), "a stray ACK was ignored");
    CHECK(r.flags == TCP_RST, "flags 0x%02x, expected a bare RST", r.flags);
    CHECK(r.seq == 9999u, "the RST's sequence is %u, expected the ACK's 9999",
          r.seq);

    /* Without ACK: RST|ACK acknowledging everything the segment occupied. */
    static const uint8_t d[4] = { 1,2,3,4 };
    send_tcp(PEER_IP, 50071u, 80u, 5000u, 0, TCP_PSH, 8192u, d, sizeof d,
             NULL, 0);
    CHECK(take(&r), "a stray data segment was ignored");
    CHECK(r.flags == (TCP_RST | TCP_ACK), "flags 0x%02x, expected RST|ACK",
          r.flags);
    CHECK(r.ack == 5004u, "the RST acknowledges %u, expected 5004", r.ack);

    /* A stray RST is never answered — that is how reset storms start. */
    send_tcp(PEER_IP, 50072u, 80u, 5000u, 1u, TCP_RST, 0u, NULL, 0, NULL, 0);
    CHECK(net_output_pending(&g_ns) == 0u, "a RST was answered with a RST");
}

static void test_the_host_closing_first_ends_the_connection_cleanly(void) {
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50080u, &gseq);
    drain();

    /* The host stream ends with three octets still to deliver. */
    static const uint8_t tail[3] = { 'b','y','e' };
    memcpy(g_mock.h[0].give, tail, sizeof tail);
    g_mock.h[0].givelen = sizeof tail;
    g_mock.h[0].recv_err = NET_EG_EOF;
    net_tick(&g_ns, 10u);

    /* The data comes first, then the FIN — the ordering that a flag-driven
     * implementation gets wrong under load. */
    CHECK(take(&r) && r.paylen == 3u && !(r.flags & TCP_FIN),
          "the last data segment carried a FIN or never arrived");
    CHECK(take(&r) && (r.flags & TCP_FIN) && r.seq == ours + 3u,
          "the FIN did not follow the data at sequence %u", ours + 3u);
    uint32_t fin_seq = ours + 3u;

    /* The guest acknowledges our FIN. FIN-WAIT-2. */
    send_tcp(PEER_IP, 50080u, 80u, gseq, fin_seq + 1u, TCP_ACK, 8192u,
             NULL, 0, NULL, 0);

    /*
     * AND NOW THE THING THIS CASE EXISTS FOR. Our FIN has been acknowledged.
     * Ticking must not send it again: a second FIN would carry a sequence the
     * guest has already passed, our snd_nxt would run ahead of snd_una with
     * nothing to acknowledge it, the retransmit timer would fire eight times
     * and a clean close would end in a reset.
     */
    for (unsigned i = 0; i < 4u; i++) net_tick(&g_ns, 20u + i * 10u);
    unsigned extra_fins = 0, resets = 0;
    while (take(&r)) {
        if (r.flags & TCP_FIN) extra_fins++;
        if (r.flags & TCP_RST) resets++;
    }
    CHECK(extra_fins == 0u,
          "the FIN was retransmitted %u time(s) after it was acknowledged",
          extra_fins);
    CHECK(resets == 0u, "a cleanly closed connection emitted %u reset(s)",
          resets);

    /* The guest closes its half; the flow reaches TIME-WAIT and then goes. */
    send_tcp(PEER_IP, 50080u, 80u, gseq, fin_seq + 1u, TCP_ACK | TCP_FIN,
             8192u, NULL, 0, NULL, 0);
    CHECK(take(&r) && (r.flags & TCP_ACK) && r.ack == gseq + 1u,
          "the guest's FIN was not acknowledged");

    /* Its ACK is lost and the guest repeats the FIN. TIME-WAIT exists to
     * answer exactly this; silence here turns one lost ACK into a guest that
     * retransmits until its own R2 expires. */
    send_tcp(PEER_IP, 50080u, 80u, gseq, fin_seq + 1u, TCP_ACK | TCP_FIN,
             8192u, NULL, 0, NULL, 0);
    CHECK(take(&r) && (r.flags & TCP_ACK) && r.ack == gseq + 1u,
          "a retransmitted FIN in TIME-WAIT was not re-acknowledged");

    net_tick(&g_ns, 10000u);
    CHECK(net_flows_open(&g_ns) == 0u,
          "the flow never left TIME-WAIT");
    CHECK(g_mock.h[0].closed, "the socket was never closed");
}

static void test_the_guest_closing_first_half_closes_the_socket(void) {
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50090u, &gseq);
    drain();

    /* The guest sends FIN with a last payload. */
    static const uint8_t last[2] = { 'o','k' };
    send_tcp(PEER_IP, 50090u, 80u, gseq, ours, TCP_ACK | TCP_FIN, 8192u,
             last, sizeof last, NULL, 0);
    CHECK(g_mock.h[0].sentlen == 2u, "the final payload was lost");
    CHECK(take(&r) && r.ack == gseq + 3u,
          "the FIN and its data were not acknowledged together (ack %u, "
          "expected %u)", r.ack, gseq + 3u);
    CHECK(g_mock.h[0].shutdown,
          "the host socket was not half-closed, so the far end never learns "
          "the request is finished and an HTTP/1.0 server waits forever");
    CHECK(net_flows_open(&g_ns) == 1u,
          "the flow was dropped while the host still had data to send");

    /* The host answers after the half-close, which is the entire point of a
     * half-close, then ends. */
    static const uint8_t body[4] = { 'd','o','n','e' };
    memcpy(g_mock.h[0].give, body, sizeof body);
    g_mock.h[0].givelen = sizeof body;
    g_mock.h[0].recv_err = NET_EG_EOF;
    net_tick(&g_ns, 10u);
    CHECK(take(&r) && r.paylen == 4u, "the response after the half-close was lost");
    CHECK(take(&r) && (r.flags & TCP_FIN), "the host's FIN never followed");
    uint32_t fin_seq = ours + 4u;

    /* LAST-ACK: the guest acknowledges and the flow goes away. */
    send_tcp(PEER_IP, 50090u, 80u, gseq + 3u, fin_seq + 1u, TCP_ACK, 8192u,
             NULL, 0, NULL, 0);
    net_tick(&g_ns, 20u);
    CHECK(net_flows_open(&g_ns) == 0u, "the flow survived LAST-ACK");
    CHECK(drain() == 0u, "a closed connection kept emitting segments");
}

static void test_a_lost_segment_is_retransmitted_and_then_given_up_on(void) {
    pkt_t r;
    net_tcp_live_status_t live;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50100u, &gseq);
    drain();

    static const uint8_t d[8] = { 1,2,3,4,5,6,7,8 };
    memcpy(g_mock.h[0].give, d, sizeof d);
    g_mock.h[0].givelen = sizeof d;
    net_tick(&g_ns, 10u);
    CHECK(take(&r) && r.paylen == 8u && r.seq == ours, "the data was not sent");

    /* Nothing acknowledges it. The first retransmission is one RTO later. */
    net_tick(&g_ns, 10u + NET_TCP_RTO_MS - 1u);
    CHECK(net_output_pending(&g_ns) == 0u, "we retransmitted before the RTO");
    net_tick(&g_ns, 10u + NET_TCP_RTO_MS);
    CHECK(take(&r) && r.paylen == 8u && r.seq == ours,
          "the retransmission did not repeat the segment from snd_una");
    CHECK(g_ns.stats.tcp_retransmits == 1u, "the retransmission was not counted");
    CHECK(net_get_tcp_live_status(&g_ns, &live) &&
          live.congestion_window == NET_TCP_MSS_MAX &&
          live.slow_start_threshold == 2u * NET_TCP_MSS_MAX,
          "timeout congestion state is cwnd/ss=%u/%u",
          live.congestion_window, live.slow_start_threshold);

    /* Keep not acknowledging. After R2 the flow is reset rather than left to
     * sit forever. */
    uint32_t t = 10u + NET_TCP_RTO_MS;
    for (unsigned i = 0; i < NET_TCP_RTX_MAX + 2u; i++) {
        t += NET_TCP_RTO_MAX_MS + 1u;
        net_tick(&g_ns, t);
    }
    bool saw_rst = false;
    while (take(&r)) if (r.flags & TCP_RST) saw_rst = true;
    CHECK(saw_rst, "a flow that was never acknowledged was not reset");
    CHECK(g_ns.stats.tcp_aborts_retransmit == 1u &&
          g_ns.stats.tcp_last_abort_reason == NET_TCP_ABORT_RETRANSMIT,
          "the R2 abort was not attributed to retransmission exhaustion");
    CHECK(g_ns.stats.tcp_last_abort_state == NET_TCP_ESTABLISHED &&
          g_ns.stats.tcp_last_abort_inflight == sizeof d &&
          g_ns.stats.tcp_last_abort_buffered == sizeof d,
          "the R2 snapshot has state/fly/buf %u/%u/%u",
          g_ns.stats.tcp_last_abort_state,
          g_ns.stats.tcp_last_abort_inflight,
          g_ns.stats.tcp_last_abort_buffered);
    CHECK(net_flows_open(&g_ns) == 0u, "the dead flow was left allocated");
}

static void send_tcp_control_at(uint16_t sport, uint32_t seq, uint32_t ack,
                                uint8_t flags, uint32_t now_ms) {
    uint8_t packet[40];
    size_t header = net_build_ipv4(packet, sizeof packet, GUEST_IP, PEER_IP,
                                   NET_PROTO_TCP, 0x1234u, 20u);
    CHECK(header == 20u, "control packet header did not fit");
    if (header != 20u) return;
    uint8_t *seg = packet + header;
    memset(seg, 0, 20u);
    w16(seg, sport);
    w16(seg + 2u, 80u);
    w32(seg + 4u, seq);
    w32(seg + 8u, ack);
    seg[12] = 5u << 4u;
    seg[13] = flags;
    w16(seg + 14u, 8192u);
    w16(seg + 16u, net_l4_checksum(GUEST_IP, PEER_IP, NET_PROTO_TCP,
                                    seg, 20u));
    net_input_at(&g_ns, packet, sizeof packet, now_ms);
}

static void test_queued_ack_restarts_rto_at_its_service_time(void) {
    pkt_t packet;
    net_tcp_live_status_t live;
    uint32_t guest_seq;
    start(true, false);
    uint32_t ours = handshake(50101u, &guest_seq);
    drain();
    g_mock.h[0].stream_left = 8u;
    net_tick(&g_ns, 10u);
    CHECK(take(&packet) && packet.seq == ours && packet.paylen == 8u,
          "initial response did not reach the guest");
    drain();

    /* The app drains queued guest input BEFORE ticking timers. After a long
     * run/pause gap, that ACK must use this boundary's time, not the previous
     * net_tick's 10 ms. Otherwise new data is queued with an already-expired
     * RTO and the following tick retransmits it immediately. */
    const uint32_t serviced_at = 10u + 2u * NET_TCP_RTO_MS;
    g_mock.h[0].stream_left = 8u;
    send_tcp_control_at(50101u, guest_seq, ours + 8u, TCP_ACK, serviced_at);
    net_tick(&g_ns, serviced_at);
    CHECK(take(&packet) && packet.seq == ours + 8u && packet.paylen == 8u,
          "the ACK did not release the next response bytes");
    CHECK(!take(&packet) && g_ns.stats.tcp_retransmits == 0u,
          "fresh response was retransmitted at its own service boundary");
    CHECK(net_get_tcp_live_status(&g_ns, &live) &&
          live.rto_remaining_ms == NET_TCP_RTO_MS && live.retries == 0u,
          "fresh response has remaining/retries %u/%u, expected %u/0",
          live.rto_remaining_ms, live.retries, NET_TCP_RTO_MS);
    net_tick(&g_ns, serviced_at + NET_TCP_RTO_MS - 1u);
    CHECK(!take(&packet), "fresh response did not receive one full RTO");
    net_tick(&g_ns, serviced_at + NET_TCP_RTO_MS);
    CHECK(take(&packet) && packet.seq == ours + 8u && packet.paylen == 8u &&
          g_ns.stats.tcp_retransmits == 1u,
          "a genuinely unacknowledged response did not retry at the deadline");
}

static void test_new_syn_does_not_inherit_an_expired_clock(void) {
    pkt_t packet;
    net_tcp_live_status_t live;
    start(true, false);
    const uint32_t serviced_at = 5u * NET_TCP_RTO_MS;
    send_tcp_control_at(50102u, 123u, 0u, TCP_SYN, serviced_at);
    net_tick(&g_ns, serviced_at);
    CHECK(take(&packet) && packet.flags == (TCP_SYN | TCP_ACK),
          "new SYN did not receive a SYN-ACK");
    CHECK(!take(&packet) && g_ns.stats.tcp_retransmits == 0u,
          "new SYN-ACK was retransmitted immediately after a service gap");
    CHECK(net_get_tcp_live_status(&g_ns, &live) &&
          live.rto_remaining_ms == NET_TCP_RTO_MS && live.retries == 0u,
          "new SYN inherited remaining/retries %u/%u",
          live.rto_remaining_ms, live.retries);
}

static void test_timestamped_input_does_not_tick_other_flows_first(void) {
    pkt_t packet;
    uint32_t seq_a, seq_b;
    start(true, false);
    uint32_t ours_a = handshake(50103u, &seq_a);
    uint32_t ours_b = handshake(50104u, &seq_b);
    drain();
    g_mock.h[0].stream_left = 8u;
    g_mock.h[1].stream_left = 8u;
    net_tick(&g_ns, 10u);
    CHECK(drain() == 2u, "two flows did not emit their initial responses");

    const uint32_t serviced_at = 10u + 2u * NET_TCP_RTO_MS;
    send_tcp_control_at(50103u, seq_a, ours_a + 8u, TCP_ACK, serviced_at);
    CHECK(g_ns.stats.tcp_retransmits == 0u && net_output_pending(&g_ns) == 0u,
          "input evaluated expiration before the queued ACK or on another flow");
    net_tick(&g_ns, serviced_at);
    CHECK(take(&packet) && packet.dport == 50104u && packet.seq == ours_b &&
          packet.paylen == 8u && !take(&packet) &&
          g_ns.stats.tcp_retransmits == 1u,
          "the following tick did not retry only the still-unacknowledged flow");
}

static void test_timestamped_input_clamps_stale_time_and_handles_wrap(void) {
    static const uint8_t malformed = 0u;
    start(true, false);
    net_tick(&g_ns, 1000u);
    net_input_at(NULL, &malformed, 1u, 2000u);
    net_input_at(&g_ns, NULL, 0u, 2000u);
    CHECK(g_ns.now_ms == 1000u && g_ns.stats.ip_in == 0u,
          "NULL input changed the clock or packet count");
    net_input_at(&g_ns, &malformed, 1u, 500u);
    CHECK(g_ns.now_ms == 1000u && g_ns.stats.ip_bad_header == 1u,
          "stale time moved backward or discarded the packet before admission");
    net_input_at(&g_ns, &malformed, 1u, INT32_MAX);
    net_input_at(&g_ns, &malformed, 1u, UINT32_MAX - 10u);
    net_input_at(&g_ns, &malformed, 1u, 20u);
    CHECK(g_ns.now_ms == 20u && g_ns.stats.ip_bad_header == 4u,
          "monotonic timestamp did not advance across 32-bit wrap");
    net_input_at(&g_ns, &malformed, 1u, UINT32_MAX - 5u);
    CHECK(g_ns.now_ms == 20u && g_ns.stats.ip_bad_header == 5u,
          "pre-wrap stale timestamp was treated as a future instant");
}

static void test_a_zero_window_is_probed_and_not_left_to_stall(void) {
    pkt_t r;
    uint32_t gseq;
    start(true, false);
    uint32_t ours = handshake(50110u, &gseq);
    drain();

    /* The guest closes its window with data waiting for it. */
    for (unsigned i = 0; i < 100u; i++) g_mock.h[0].give[i] = (uint8_t)i;
    g_mock.h[0].givelen = 100u;
    send_tcp(PEER_IP, 50110u, 80u, gseq, ours, TCP_ACK, 0u, NULL, 0, NULL, 0);
    net_tick(&g_ns, 10u);
    size_t sent = 0;
    while (take(&r)) sent += r.paylen;
    CHECK(sent == 0u, "we sent %u octets into a zero window", (unsigned)sent);

    /* The persist timer must probe. Without it the flow stops with nothing
     * running to restart it. */
    net_tick(&g_ns, 10u + NET_TCP_RTO_MS);
    CHECK(take(&r), "a zero window produced no persist probe — this is the "
                    "classic silent stall");
    CHECK(r.paylen == 1u,
          "the persist probe carried %u octets, expected the one RFC 1122 "
          "§4.2.2.17 allows past the window", (unsigned)r.paylen);

    /* The guest reopens and everything flows. */
    send_tcp(PEER_IP, 50110u, 80u, gseq, ours, TCP_ACK, 8192u, NULL, 0, NULL, 0);
    net_tick(&g_ns, 10u + 2u * NET_TCP_RTO_MS);
    sent = 0;
    while (take(&r)) sent += r.paylen;
    CHECK(sent >= 99u, "reopening the window delivered %u of 100 octets",
          (unsigned)sent);
}

static void test_the_flow_table_fills_and_says_so(void) {
    pkt_t r;
    start(true, false);
    for (unsigned i = 0; i < NET_MAX_FLOWS; i++) {
        send_tcp(PEER_IP, (uint16_t)(51000u + i), 80u, 7000u, 0, TCP_SYN,
                 8192u, NULL, 0, NULL, 0);
    }
    CHECK(net_flows_open(&g_ns) == NET_MAX_FLOWS,
          "only %u of %u flows were created",
          (unsigned)net_flows_open(&g_ns), (unsigned)NET_MAX_FLOWS);
    CHECK(g_ns.stats.flows_peak == NET_MAX_FLOWS, "flows_peak is %llu",
          (unsigned long long)g_ns.stats.flows_peak);
    drain();

    send_tcp(PEER_IP, 60000u, 80u, 7000u, 0, TCP_SYN, 8192u, NULL, 0, NULL, 0);
    CHECK(g_ns.stats.flow_table_full == 1u, "the full table was not counted");
    CHECK(take(&r) && (r.flags & TCP_RST),
          "a SYN with no slot got silence instead of a reset");
}

static void test_the_output_queue_backs_up_rather_than_overwriting(void) {
    uint8_t icmp[12];
    pkt_t p;
    start(true, false);
    memset(icmp, 0, sizeof icmp);
    icmp[0] = 8u;

    /*
     * Echo is the cleanest way to overrun the queue on purpose: one reply per
     * request, no window and no timer in the way. A TCP flow cannot do it —
     * with nothing acknowledging, the send buffer bounds what can be in flight
     * long before 32 datagrams exist.
     *
     * Each request carries its own sequence number so the survivors can be
     * identified individually.
     */
    const unsigned N = NET_OUT_SLOTS + 8u;
    for (unsigned i = 0; i < N; i++) {
        w16(icmp + 6, (uint16_t)i);
        w16(icmp + 2, 0);
        w16(icmp + 2, net_checksum(icmp, sizeof icmp));
        feed(LOCAL_IP, NET_PROTO_ICMP, icmp, sizeof icmp);
    }
    CHECK(net_output_pending(&g_ns) == NET_OUT_SLOTS,
          "the queue holds %u datagrams, expected exactly its %u slots",
          (unsigned)net_output_pending(&g_ns), (unsigned)NET_OUT_SLOTS);
    CHECK(g_ns.stats.out_dropped == 8u,
          "eight datagrams were refused and %llu drops were counted; an "
          "uncounted overrun is invisible after the fact",
          (unsigned long long)g_ns.stats.out_dropped);

    /* The survivors are the FIRST 32, in order. A ring that overwrote its tail
     * would hand back the last 32 instead, which is the failure that looks
     * like packet loss on a link that never lost anything. */
    bool ordered = true;
    for (unsigned i = 0; i < NET_OUT_SLOTS; i++) {
        if (!take(&p)) { ordered = false; break; }
        if (r16(p.pay + 6) != (uint16_t)i) ordered = false;
    }
    CHECK(ordered,
          "the queue did not return the first %u replies in order — it "
          "overwrote datagrams it had not delivered",
          (unsigned)NET_OUT_SLOTS);
    CHECK(!take(&p), "the queue held more than its %u slots",
          (unsigned)NET_OUT_SLOTS);
}

/* ==========================================================================
 * Lifecycle.
 * ======================================================================== */

static void test_a_stack_with_no_egress_still_answers_for_itself(void) {
    uint8_t icmp[12];
    pkt_t r;
    start(false, false);                              /* every member NULL    */

    memset(icmp, 0, sizeof icmp);
    icmp[0] = 8u;
    w16(icmp + 2, 0); w16(icmp + 2, net_checksum(icmp, sizeof icmp));
    feed(LOCAL_IP, NET_PROTO_ICMP, icmp, sizeof icmp);
    CHECK(take(&r) && r.pay[0] == 0u,
          "a stack with no egress could not answer its own address");

    /* A TCP connection has nowhere to go and is reset, not hung. */
    send_tcp(PEER_IP, 50200u, 80u, 8000u, 0, TCP_SYN, 8192u, NULL, 0, NULL, 0);
    CHECK(take(&r) && (r.flags & TCP_RST), "a SYN with no egress was not reset");

    /* DNS with no resolver is SERVFAIL, which is a transient answer the guest
     * will retry, rather than NXDOMAIN which it would cache. */
    uint8_t q[512];
    size_t qn = dns_query(q, sizeof q, "www.example.com", 1u, 1u, 1u);
    send_udp(DNS_IP, 40100u, 53u, q, qn);
    CHECK(take(&r) && (r16(r.pay + 2) & 0xfu) == 2u,
          "no resolver did not produce SERVFAIL");
}

static void test_reset_releases_every_socket(void) {
    start(true, false);
    handshake(50300u, NULL);
    send_udp(PEER_IP, 33333u, 5353u, (const uint8_t *)"x", 1u);
    CHECK(net_flows_open(&g_ns) == 2u, "expected two flows");
    unsigned before = g_mock.closes;
    net_reset(&g_ns);
    CHECK(net_flows_open(&g_ns) == 0u, "flows survived net_reset()");
    CHECK(g_mock.closes == before + 2u,
          "net_reset() closed %u sockets, expected 2", g_mock.closes - before);
    CHECK(net_output_pending(&g_ns) == 0u, "net_reset() left output queued");
    CHECK(g_ns.stats.ip_in == 0u, "net_reset() did not clear the counters");
}

static void test_time_only_moves_forward(void) {
    start(true, false);
    net_tick(&g_ns, 1000u);
    send_udp(PEER_IP, 33333u, 5353u, (const uint8_t *)"x", 1u);
    net_tick(&g_ns, 500u);                            /* backwards            */
    CHECK(g_ns.now_ms == 1000u, "time went backwards to %u", g_ns.now_ms);
    net_tick(&g_ns, 1000u + NET_UDP_IDLE_MS + 1u);
    CHECK(net_flows_open(&g_ns) == 0u,
          "the idle timer did not fire after a rejected backwards step");
}

static void test_the_null_arguments_do_not_crash(void) {
    uint8_t buf[64];
    net_init(NULL, NULL, NULL);
    net_input(NULL, buf, sizeof buf);
    net_input(&g_ns, NULL, 4);
    net_tick(NULL, 0);
    net_reset(NULL);
    CHECK(net_output(NULL, buf, sizeof buf) == 0u, "net_output(NULL) answered");
    CHECK(net_output_peek(NULL) == 0u, "net_output_peek(NULL) answered");
    CHECK(net_output_pending(NULL) == 0u, "net_output_pending(NULL) answered");
    CHECK(net_flows_open(NULL) == 0u, "net_flows_open(NULL) answered");
    CHECK(net_checksum(NULL, 4) == 0xffffu, "net_checksum(NULL) is not 0xffff");
    CHECK(net_build_ipv4(NULL, 40, 0, 0, 1, 0, 0) == 0u,
          "net_build_ipv4(NULL) claimed to build a header");
    CHECK(net_build_ipv4(buf, 8, 0, 0, 1, 0, 0) == 0u,
          "net_build_ipv4 wrote a header into an 8-octet buffer");
    CHECK(net_build_ipv4(buf, sizeof buf, 0, 0, 1, 0, NET_MTU) == 0u,
          "net_build_ipv4 built a datagram past the MTU");
    CHECK(strcmp(net_tcp_state_name(NET_TCP_ESTABLISHED), "ESTABLISHED") == 0,
          "the state names are wrong");
}

static void test_output_peek_and_short_buffers(void) {
    uint8_t small[8];
    pkt_t r;
    uint8_t icmp[12];
    start(true, false);
    memset(icmp, 0, sizeof icmp);
    icmp[0] = 8u;
    w16(icmp + 2, 0); w16(icmp + 2, net_checksum(icmp, sizeof icmp));
    feed(LOCAL_IP, NET_PROTO_ICMP, icmp, sizeof icmp);

    size_t want = net_output_peek(&g_ns);
    CHECK(want == 20u + sizeof icmp, "peek says %u octets", (unsigned)want);
    CHECK(net_output(&g_ns, small, sizeof small) == 0u,
          "a datagram was copied into a buffer too small for it");
    CHECK(net_output_pending(&g_ns) == 1u,
          "the datagram was consumed by a failed read — a caller with a "
          "bounded PPP ring would lose it");
    CHECK(take(&r) && r.rawlen == want, "the retry did not return the same "
                                        "datagram");
}


static void receive_checksums(uint8_t *p, size_t n) {
    w16(p + 10u, 0u); w16(p + 10u, net_checksum(p, 20u));
    w16(p + 36u, 0u);
    w16(p + 36u, net_l4_checksum(r32(p + 12u), r32(p + 16u),
                                  NET_PROTO_TCP, p + 20u, n - 20u));
}
static void receive_data(uint8_t *p, size_t n, uint32_t seq, uint8_t marker) {
    memset(p, 0, n);
    (void)net_build_ipv4(p, n, PEER_IP, GUEST_IP, NET_PROTO_TCP, 12u, n - 20u);
    w16(p + 20u, 8000u); w16(p + 22u, 49152u);
    w32(p + 24u, seq); w32(p + 28u, 456u);
    p[32] = 0x50u; p[33] = TCP_ACK | TCP_PSH; w16(p + 34u, 65535u);
    for (size_t i = 40u; i < n; i++) p[i] = (uint8_t)(marker + i);
    receive_checksums(p, n);
}
static void test_receive_coalescing_preserves_stream_and_boundaries(void) {
    uint8_t a[1500], b[1500], dst[16384], before[16384];
    receive_data(a, sizeof a, 0xfffffff0u, 11u);
    receive_data(b, 1291u, 0xfffffff0u + 1460u, 23u); /* wrap + odd data length */
    memset(dst, 0xa5, sizeof dst); memcpy(dst, a, sizeof a);
    size_t n = net_tcp_receive_coalesce(dst, sizeof a, sizeof dst, b, 1291u);
    CHECK(n == 2751u && r16(dst+2u) == n && r32(dst+24u) == 0xfffffff0u,
          "coalesced length or sequence incorrect");
    CHECK(!memcmp(dst+40u, a+40u, 1460u) && !memcmp(dst+1500u, b+40u, 1251u) &&
          dst[2751u] == 0xa5u, "coalesced stream bytes or boundary changed");
    CHECK(net_checksum(dst, 20u) == 0u &&
          net_l4_checksum(PEER_IP, GUEST_IP, NET_PROTO_TCP, dst+20u, n-20u) == 0u,
          "coalesced checksums invalid");
    CHECK(dst[33] == (TCP_ACK|TCP_PSH) && r16(dst+34u) == 65535u &&
          r32(dst+28u) == 456u, "coalescing altered TCP control state");
    receive_data(b, sizeof b, 0xfffffff0u + 1460u, 23u);
    static const unsigned fields[] = {1u, 8u, 12u, 16u, 20u, 22u, 24u, 28u, 34u,
                                     6u, 32u, 33u, 38u};
    for (unsigned i = 0u; i < sizeof fields/sizeof fields[0]; i++) {
        uint8_t bad[1500]; memcpy(bad, b, sizeof bad);
        bad[fields[i]] ^= 1u;
        receive_checksums(bad, sizeof bad);
        memset(dst, 0xa5, sizeof dst); memcpy(dst, a, sizeof a);
        memcpy(before, dst, sizeof dst);
        CHECK(net_tcp_receive_coalesce(dst, sizeof a, sizeof dst, bad, sizeof bad) == 0u &&
              !memcmp(dst, before, sizeof dst), "crossed boundary at header byte %u", fields[i]);
    }
    for (unsigned i = 0u; i < 2u; i++) {
        uint8_t bad[1500]; memcpy(bad, b, sizeof bad);
        bad[i ? 36u : 10u] ^= 1u;
        CHECK(net_tcp_receive_coalesce(dst, sizeof a, sizeof dst, bad, sizeof bad) == 0u &&
              !memcmp(dst, before, sizeof dst), "accepted corrupt checksum %u", i);
    }
    CHECK(net_tcp_receive_coalesce(dst, sizeof a, 1500u, b, sizeof b) == 0u &&
          !memcmp(dst, before, sizeof dst), "capacity refusal changed bytes");
    CHECK(net_tcp_receive_coalesce(dst, sizeof dst+1u, sizeof dst, b, sizeof b) == 0u,
          "oversized destination length accepted");
    CHECK(net_tcp_receive_coalesce(NULL, 1500u, sizeof dst, b, sizeof b) == 0u &&
          net_tcp_receive_coalesce(dst, 1500u, sizeof dst, NULL, 1500u) == 0u,
          "NULL coalescing arguments accepted");
    memcpy(dst, a, sizeof a); n = sizeof a;
    for (unsigned i = 1u; i < 11u; i++) {
        receive_data(b, sizeof b, 0xfffffff0u + i * 1460u, (uint8_t)i);
        n = net_tcp_receive_coalesce(dst, n, sizeof dst, b, sizeof b);
    }
    CHECK(n == 16100u && r16(dst+2u) == n, "large aggregate failed at %zu", n);
    receive_data(b, sizeof b, 0xfffffff0u + 11u * 1460u, 12u);
    CHECK(net_tcp_receive_coalesce(dst, n, sizeof dst, b, sizeof b) == 0u,
          "aggregate exceeded native receive capacity");
}

/*
 * Cases run in order and each announces itself first. This suite drives a
 * state machine that can fault, and a fault with buffered output loses the one
 * line that says which case was running — so stdout is unbuffered and the name
 * is printed before the call, not after it.
 */
#define RUN(fn) do { printf("  .. %s\n", #fn); fn(); } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("S5LBox host IPv4 / NAT / resolver tests\n");
    RUN(test_checksum_is_rfc1071_and_not_our_opinion_of_it);
    RUN(test_pseudo_header_covers_the_addresses);
    RUN(test_ipv4_refuses_exactly_what_it_says_it_refuses);
    RUN(test_trailing_octets_past_total_length_are_ignored);
    RUN(test_icmp_echo_comes_back_from_the_address_it_was_aimed_at);
    RUN(test_an_echo_past_the_gateway_is_never_fabricated);
    RUN(test_dns_answers_an_a_query_from_the_host_resolver);
    RUN(test_dns_waits_for_an_async_host_resolver);
    RUN(test_pending_dns_is_bounded_and_times_out);
    RUN(test_dns_says_the_right_no);
    RUN(test_dns_refuses_names_it_should_not_hand_to_getaddrinfo);
    RUN(test_udp_goes_out_through_a_socket_and_comes_back);
    RUN(test_udp_flows_expire_when_idle);
    RUN(test_udp_with_a_zero_checksum_is_accepted_and_a_wrong_one_is_not);
    RUN(test_the_handshake_waits_for_the_host_connect);
    RUN(test_a_refused_connect_resets_rather_than_hanging);
    RUN(test_data_crosses_in_both_directions);
    RUN(test_a_bigboss_sized_stream_arrives_byte_exact);
    RUN(test_bulk_data_starts_with_a_bounded_ack_clocked_flight);
    RUN(test_future_ack_cannot_discard_unsent_data_or_admit_its_payload);
    RUN(test_packet_transport_grows_after_real_acks);
    RUN(test_ack_cannot_establish_before_syn_ack_was_sent);
    RUN(test_ack_of_original_flight_survives_retransmit_rewind);
    RUN(test_a_full_output_queue_does_not_strand_tcp_data);
    RUN(test_the_guests_window_bounds_what_we_send);
    RUN(test_live_tcp_status_names_the_buffered_flow);
    RUN(test_a_small_mss_from_the_guest_is_honoured);
    RUN(test_a_peer_without_an_mss_option_gets_a_bounded_initial_flight);
    RUN(test_out_of_order_is_reacked_and_never_reassembled);
    RUN(test_backpressure_shrinks_the_window_and_never_drops);
    RUN(test_a_reset_from_the_guest_frees_the_flow);
    RUN(test_a_segment_for_no_connection_is_reset_both_ways);
    RUN(test_the_host_closing_first_ends_the_connection_cleanly);
    RUN(test_the_guest_closing_first_half_closes_the_socket);
    RUN(test_a_lost_segment_is_retransmitted_and_then_given_up_on);
    RUN(test_queued_ack_restarts_rto_at_its_service_time);
    RUN(test_new_syn_does_not_inherit_an_expired_clock);
    RUN(test_timestamped_input_does_not_tick_other_flows_first);
    RUN(test_timestamped_input_clamps_stale_time_and_handles_wrap);
    RUN(test_a_zero_window_is_probed_and_not_left_to_stall);
    RUN(test_the_flow_table_fills_and_says_so);
    RUN(test_the_output_queue_backs_up_rather_than_overwriting);
    RUN(test_a_stack_with_no_egress_still_answers_for_itself);
    RUN(test_reset_releases_every_socket);
    RUN(test_time_only_moves_forward);
    RUN(test_the_null_arguments_do_not_crash);
    RUN(test_output_peek_and_short_buffers);
    RUN(test_receive_coalescing_preserves_stream_and_boundaries);
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
