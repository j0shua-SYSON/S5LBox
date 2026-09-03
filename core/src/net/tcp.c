/*
 * S5LBox — the TCP that faces the guest.
 *
 * docs/networking.md §11 puts "our TCP against the guest's TCP" at the top of
 * the risk list, because a half-correct TCP produces stalls that look like
 * emulator bugs and that is the failure mode this project is worst placed to
 * debug. So this file is the one that gets written before any guest exists and
 * driven from a test with no sockets in it.
 *
 * WHAT THIS IS. One RFC 793 endpoint per guest flow, always the PASSIVE side —
 * the guest is the only thing that opens connections, and docs/networking.md
 * §8.2 excludes inbound connections from this milestone. Behind each endpoint
 * is an ordinary host SOCK_STREAM reached through net_egress_t, so the
 * internet-facing half of the reliability problem is not ours: we are
 * translating a reliable byte stream into a reliable byte stream, and the only
 * lossy link is a function call.
 *
 * FOUR DELIBERATE SIMPLIFICATIONS, each one in the direction of "cannot
 * silently stall".
 *
 * 1. NO REASSEMBLY QUEUE. A segment that is not the next one is dropped and
 *    the current cumulative ACK is repeated. That is legal (RFC 793 §3.9,
 *    "segments not at the left window edge... may be discarded"), it costs a
 *    retransmit on a link where loss is impossible anyway, and it removes an
 *    out-of-order buffer that would be pure untested surface. An overlapping
 *    retransmission IS handled: the already-seen prefix is skipped and the new
 *    tail accepted, because that is what a guest whose segment boundaries move
 *    actually sends.
 *
 * 2. WE ACK IMMEDIATELY, NEVER LATE. RFC 1122 §4.2.3.2 permits a delayed ACK
 *    and every real stack uses one; the saving is bandwidth we do not pay for,
 *    and the risk is a stall that only appears under a specific interleaving.
 *    An immediate ACK is always correct.
 *
 * 3. GO-BACK-N ON TIMEOUT, no fast retransmit and no SACK. Same argument: the
 *    window is 4 KB and the peer is in the same process.
 *
 * 4. TIME-WAIT IS TWO SECONDS, not 2MSL. TIME-WAIT exists to absorb a delayed
 *    duplicate from a previous incarnation, and on a point-to-point link with
 *    no reordering and no queues the only duplicate possible is a
 *    retransmission the guest is still sending. Two seconds of guest time is
 *    ten wall minutes at this emulator's speed; a real 2MSL would pin a flow
 *    slot for the rest of the run.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "net_priv.h"
#include <string.h>

/* ---------------------------------------------------------------- send --- */

/*
 * One segment. `seq` is explicit rather than taken from the flow because a
 * retransmission and a fresh send differ in exactly that and in nothing else,
 * and a function that decided it for itself would need a flag that means the
 * same thing.
 *
 * The source address is the address the GUEST believes it is talking to. This
 * is a NAT: nothing about the translation is visible from the guest's side,
 * which is what lets a stock 2010 CFNetwork work against it unmodified.
 */
static bool tcp_out(net_stack_t *ns, net_flow_t *f, uint8_t flags,
                    uint32_t seq, const uint8_t *data, size_t n) {
    uint8_t seg[TCP_HDR_LEN + 4u + NET_TCP_MSS_MAX];
    size_t  hl = TCP_HDR_LEN;

    memset(seg, 0, TCP_HDR_LEN);
    net_wr16(seg, f->dst_port);
    net_wr16(seg + 2, f->guest_port);
    net_wr32(seg + 4, seq);
    net_wr32(seg + 8, (flags & TCP_ACK) ? f->rcv_nxt : 0u);

    if (flags & TCP_SYN) {
        /* RFC 793 §3.1's Maximum Segment Size option, and the only option we
         * ever send. Clamped per docs/networking.md §8.2 so nothing we emit
         * can need fragmenting on a 1500-octet link. */
        seg[hl++] = 2u; seg[hl++] = 4u;
        net_wr16(seg + hl, (uint16_t)NET_TCP_MSS_MAX); hl += 2u;
    }
    seg[12] = (uint8_t)((hl / 4u) << 4);
    seg[13] = flags;

    /* The window is the room left in the buffer the host socket drains. It
     * cannot retract: accepting k octets advances rcv_nxt by k and fills k
     * octets of the buffer, so the right edge stands still, and draining the
     * buffer only moves it right. */
    uint32_t wnd = NET_TCP_RXBUF - f->rxlen;
    if (wnd > 0xffffu) wnd = 0xffffu;
    net_wr16(seg + 14, (uint16_t)wnd);

    if (n) memcpy(seg + hl, data, n);
    size_t total = hl + n;
    net_wr16(seg + 16, 0);
    net_wr16(seg + 16, net_l4_checksum(f->dst_ip, ns->cfg.guest_ip,
                                       NET_PROTO_TCP, seg, total));

    if (net_emit(ns, f->dst_ip, ns->cfg.guest_ip, NET_PROTO_TCP, seg, total)) {
        ns->stats.tcp_out++;
        if (n) ns->stats.tcp_bytes_to_guest += n;
        if (flags & TCP_ACK) f->need_ack = false;
        return true;
    }
    return false;
}

/*
 * RFC 793 §3.4's reset rules for a segment that matches no connection. There
 * are exactly two forms and getting them the wrong way round produces a reset
 * the peer ignores, so both are spelled out rather than merged.
 */
static void tcp_reject(net_stack_t *ns, uint32_t dst_ip,
                       const uint8_t *seg, size_t n, size_t paylen) {
    uint8_t r[TCP_HDR_LEN];
    uint8_t flags = seg[13];
    if (flags & TCP_RST) return;                 /* never reset a reset      */

    memset(r, 0, sizeof r);
    net_wr16(r, net_rd16(seg + 2));              /* our port is its dest     */
    net_wr16(r + 2, net_rd16(seg));
    r[12] = (uint8_t)((TCP_HDR_LEN / 4u) << 4);

    if (flags & TCP_ACK) {
        /* "the RST takes its sequence number from the ACK field" */
        net_wr32(r + 4, net_rd32(seg + 8));
        r[13] = TCP_RST;
    } else {
        /* "...and acknowledges everything the segment occupied" */
        uint32_t end = net_rd32(seg + 4) + (uint32_t)paylen;
        if (flags & TCP_SYN) end++;
        if (flags & TCP_FIN) end++;
        net_wr32(r + 8, end);
        r[13] = TCP_RST | TCP_ACK;
    }
    net_wr16(r + 16, net_l4_checksum(dst_ip, ns->cfg.guest_ip,
                                     NET_PROTO_TCP, r, sizeof r));
    if (net_emit(ns, dst_ip, ns->cfg.guest_ip, NET_PROTO_TCP, r, sizeof r))
        ns->stats.tcp_resets_out++;
    (void)n;
}

/* Reset an established flow and drop it. Record WHY before net_flow_free()
 * erases the only state that can distinguish a transport defect from an
 * unreachable host. */
static void tcp_abort(net_stack_t *ns, net_flow_t *f, uint32_t reason) {
    switch (reason) {
        case NET_TCP_ABORT_UNEXPECTED_SYN:
            ns->stats.tcp_aborts_unexpected_syn++;
            break;
        case NET_TCP_ABORT_CONNECT:
            ns->stats.tcp_aborts_connect++;
            break;
        case NET_TCP_ABORT_HOST_SEND:
            ns->stats.tcp_aborts_host_send++;
            break;
        case NET_TCP_ABORT_HOST_RECV:
            ns->stats.tcp_aborts_host_recv++;
            break;
        case NET_TCP_ABORT_RETRANSMIT:
            ns->stats.tcp_aborts_retransmit++;
            break;
        default:
            reason = NET_TCP_ABORT_NONE;
            break;
    }
    ns->stats.tcp_last_abort_reason   = reason;
    ns->stats.tcp_last_abort_state    = (uint32_t)f->state;
    ns->stats.tcp_last_abort_window   = f->snd_wnd;
    ns->stats.tcp_last_abort_inflight = f->snd_nxt - f->snd_una;
    ns->stats.tcp_last_abort_buffered = f->txlen;
    ns->stats.tcp_last_abort_retries  = f->rtx;
    tcp_out(ns, f, TCP_RST | TCP_ACK, f->snd_nxt, NULL, 0);
    ns->stats.tcp_resets_out++;
    net_flow_free(ns, f);
}

/* -------------------------------------------------------------- timers --- */

static uint32_t tcp_backoff(uint32_t rtx) {
    uint32_t ms = NET_TCP_RTO_MS;
    for (uint32_t i = 0; i < rtx && ms < NET_TCP_RTO_MAX_MS; i++) ms *= 2u;
    return ms > NET_TCP_RTO_MAX_MS ? NET_TCP_RTO_MAX_MS : ms;
}

/*
 * The timer serves two jobs that share one deadline because they can never be
 * pending at once: retransmission when something is unacknowledged, and the
 * RFC 1122 §4.2.2.17 persist probe when the guest has closed its window and
 * therefore nothing CAN be unacknowledged. Without the second, a zero window
 * would stop the flow with no timer running and nothing to restart it — the
 * classic silent stall.
 */
static void tcp_timers(net_stack_t *ns, net_flow_t *f) {
    bool unacked = f->snd_nxt != f->snd_una;
    bool persist = !unacked && f->txlen != 0u && f->snd_wnd == 0u;
    if (unacked || persist) {
        if (!f->rto_on) {
            f->rto_on = true;
            f->rto_at = ns->now_ms + tcp_backoff(f->rtx);
        }
    } else {
        f->rto_on = false;
        f->rtx    = 0;
    }
}

/* ---------------------------------------------------------------- pump --- */

/* Bytes of our send buffer already in flight. */
static uint32_t tcp_inflight(const net_flow_t *f) {
    uint32_t n = f->snd_nxt - f->snd_una;
    /* The FIN occupies one sequence past the data; it is not buffer content. */
    if (f->fin_sent && n > f->txlen) n = f->txlen;
    return n;
}

/* RFC 3390: min(4*SMSS, max(2*SMSS, 4380)).  The peer's MSS is already
 * clamped, so every intermediate fits comfortably in uint32_t. */
static uint32_t tcp_initial_cwnd(uint16_t mss) {
    uint32_t four = 4u * (uint32_t)mss;
    uint32_t floor = 2u * (uint32_t)mss;
    if (floor < 4380u) floor = 4380u;
    return four < floor ? four : floor;
}

static uint32_t tcp_cwnd_ceiling(const net_flow_t *f) {
    uint32_t ceiling = NET_TCP_CWND_MAX_SEGMENTS * (uint32_t)f->mss;
    return ceiling < NET_TCP_TXBUF ? ceiling : NET_TCP_TXBUF;
}

static void tcp_cwnd_ack(net_flow_t *f, uint32_t acknowledged) {
    if (!acknowledged) return;
    uint32_t increase;
    if (f->cwnd < f->ssthresh) {
        /* Slow start grows by at most one SMSS for each cumulative ACK. */
        increase = acknowledged < f->mss ? acknowledged : f->mss;
    } else {
        /* Congestion avoidance: approximately one SMSS per window. */
        increase = ((uint32_t)f->mss * (uint32_t)f->mss) / f->cwnd;
        if (!increase) increase = 1u;
    }
    uint32_t ceiling = tcp_cwnd_ceiling(f);
    if (f->cwnd >= ceiling || increase > ceiling - f->cwnd) f->cwnd = ceiling;
    else f->cwnd += increase;
}

static void tcp_cwnd_timeout(net_flow_t *f) {
    uint32_t floor = 2u * (uint32_t)f->mss;
    uint32_t threshold = tcp_inflight(f) / 2u;
    if (threshold < floor) threshold = floor;
    uint32_t ceiling = tcp_cwnd_ceiling(f);
    f->ssthresh = threshold < ceiling ? threshold : ceiling;
    f->cwnd = f->mss;
}

static void tcp_pump(net_stack_t *ns, net_flow_t *f) {
    if (f->state == NET_TCP_SYN_RCVD || f->state == NET_TCP_TIME_WAIT) return;

    uint32_t sent = tcp_inflight(f);
    while (f->txlen > sent) {
        uint32_t limit = f->snd_wnd < f->cwnd ? f->snd_wnd : f->cwnd;
        uint32_t win = limit > sent ? limit - sent : 0u;
        uint32_t seg = f->txlen - sent;
        if (win == 0u) break;
        if (seg > win) seg = win;
        if (seg > f->mss) seg = f->mss;
        if (!tcp_out(ns, f, TCP_ACK | TCP_PSH, f->snd_nxt,
                     f->txbuf + sent, seg))
            break;
        f->snd_nxt += seg;
        sent       += seg;
    }

    /*
     * The FIN is committed to the sequence space as soon as the host stream
     * ends, but it sits at snd_una + txlen, so it cannot overtake data that
     * has not been sent yet — the ordering falls out of the arithmetic rather
     * than out of a flag.
     */
    if (f->host_eof && !f->fin_sent &&
        (f->state == NET_TCP_ESTABLISHED || f->state == NET_TCP_CLOSE_WAIT)) {
        f->fin_sent = true;
        f->state = (f->state == NET_TCP_CLOSE_WAIT) ? NET_TCP_LAST_ACK
                                                    : NET_TCP_FIN_WAIT_1;
    }
    /*
     * ...but "the FIN sits at snd_una + txlen" stops being true the moment it
     * is ACKNOWLEDGED, and that is a trap this originally fell into. Once the
     * guest acks the FIN, snd_una advances past it, txlen is zero, and
     * snd_nxt == snd_una + txlen becomes true all over again — so every
     * subsequent tick sent another FIN, snd_nxt ran away from snd_una with
     * nothing left to acknowledge it, the retransmit timer fired R2 times and
     * a cleanly closed connection ended in a RST.
     *
     * The three states below are exactly RFC 793's "our FIN is outstanding".
     * FIN-WAIT-2, TIME-WAIT and CLOSED are the states that mean it has already
     * been acked, and none of them belongs here. Using the state rather than
     * another flag keeps the one fact in one place.
     */
    if (f->fin_sent && f->snd_nxt == f->snd_una + f->txlen &&
        (f->state == NET_TCP_FIN_WAIT_1 || f->state == NET_TCP_CLOSING ||
         f->state == NET_TCP_LAST_ACK)) {
        if (tcp_out(ns, f, TCP_ACK | TCP_FIN, f->snd_nxt, NULL, 0))
            f->snd_nxt++;
    }

    if (f->need_ack) tcp_out(ns, f, TCP_ACK, f->snd_nxt, NULL, 0);
    tcp_timers(ns, f);
}

/* --------------------------------------------------------------- input --- */

/* Does this state mean the guest's FIN has already been accepted? */
static bool tcp_guest_finished(net_tcp_state_t s) {
    return s == NET_TCP_CLOSE_WAIT || s == NET_TCP_CLOSING ||
           s == NET_TCP_LAST_ACK   || s == NET_TCP_TIME_WAIT;
}

static void tcp_consume_ack(net_stack_t *ns, net_flow_t *f, uint32_t ack) {
    /*
     * RFC 793 §3.9's acceptance test is SND.UNA < SEG.ACK =< SND.NXT, and the
     * ceiling has to be BOTH of the things below because neither alone is it.
     *
     * The buffer end — snd_una + txlen, plus one if our FIN is committed —
     * does not count the SYN. A SYN occupies a sequence number of its own and
     * is not buffer content, so in SYN-RCVD that expression evaluates to
     * snd_una and the handshake's own ACK, which is snd_una + 1, lands one past
     * it and is rejected. Every connection then sat in SYN-RCVD forever.
     * core/tests/test_net.c's handshake case is that bug, and it is the reason
     * this suite exists at all: nothing above a NAT that cannot open a
     * connection produces a diagnosable symptom.
     *
     * snd_nxt alone is not it either, because a retransmission rewinds snd_nxt
     * to snd_una (the go-back-N in net_tcp_tick), and an ACK for data we really
     * did send before the rewind would then read as acking something unsent.
     *
     * Whichever is higher covers both and invents nothing: it is exactly "the
     * furthest sequence this connection has either sent or committed to send".
     */
    uint32_t end = f->snd_una + f->txlen + (f->fin_sent ? 1u : 0u);
    if (net_seq_lt(end, f->snd_nxt)) end = f->snd_nxt;
    if (!net_seq_lt(f->snd_una, ack) || !net_seq_le(ack, end)) return;

    uint32_t dat = ack - f->snd_una;
    bool fin_acked = f->fin_sent && dat > f->txlen;
    if (dat > f->txlen) dat = f->txlen;
    if (dat) {
        tcp_cwnd_ack(f, dat);
        memmove(f->txbuf, f->txbuf + dat, f->txlen - dat);
        f->txlen -= dat;
    }
    f->snd_una = ack;
    if (net_seq_lt(f->snd_nxt, f->snd_una)) f->snd_nxt = f->snd_una;

    /* A new cumulative ACK ends the current retransmission episode. */
    f->rtx    = 0;
    f->rto_on = false;

    if (fin_acked) {
        switch (f->state) {
            case NET_TCP_FIN_WAIT_1: f->state = NET_TCP_FIN_WAIT_2; break;
            case NET_TCP_CLOSING:
                f->state    = NET_TCP_TIME_WAIT;
                f->close_at = ns->now_ms + NET_TCP_TIME_WAIT_MS;
                break;
            case NET_TCP_LAST_ACK:   f->state = NET_TCP_CLOSED; break;
            default: break;
        }
    }
}

static void tcp_syn(net_stack_t *ns, uint32_t dst_ip,
                    const uint8_t *seg, size_t n, size_t doff) {
    uint16_t gport = net_rd16(seg);
    uint16_t dport = net_rd16(seg + 2);

    net_flow_t *f = net_flow_alloc(ns, NET_PROTO_TCP, gport, dst_ip, dport);
    if (!f) {
        /* No slot. A reset is the honest answer: the alternative is a SYN the
         * guest retransmits for a minute before its own timer gives up. */
        tcp_reject(ns, dst_ip, seg, n, 0);
        return;
    }
    ns->stats.tcp_syns++;

    f->irs     = net_rd32(seg + 4);
    f->rcv_nxt = f->irs + 1u;                      /* the SYN's own sequence */
    f->snd_wnd = net_rd16(seg + 14);
    f->mss     = 536u;                             /* RFC 879's default      */
    f->state   = NET_TCP_SYN_RCVD;

    for (size_t at = TCP_HDR_LEN; at + 1u < doff; ) {
        uint8_t kind = seg[at];
        if (kind == 0u) break;                     /* End of Option List     */
        if (kind == 1u) { at++; continue; }        /* No-Operation           */
        uint8_t len = seg[at + 1u];
        if (len < 2u || at + len > doff) break;
        if (kind == 2u && len == 4u) f->mss = net_rd16(seg + at + 2u);
        at += len;
    }
    if (f->mss > NET_TCP_MSS_MAX) f->mss = NET_TCP_MSS_MAX;
    if (f->mss < 64u) f->mss = 64u;                /* a hostile tiny MSS     */
    f->cwnd = tcp_initial_cwnd(f->mss);
    f->ssthresh = tcp_cwnd_ceiling(f);

    /*
     * RFC 793 §3.3 wants the initial send sequence to advance between
     * incarnations of the same connection. Ours walks by a fixed odd stride
     * from a fixed seed: reproducible, which is what a test needs, and not a
     * security ISN, which nothing on this link needs — the only peer that can
     * reach it is the guest at the other end of a UART.
     */
    ns->iss_walk += 0x9e3779b9u;
    f->snd_una = f->snd_nxt = ns->iss_walk;

    f->handle = ns->eg.open ? ns->eg.open(ns->eg.ctx, NET_PROTO_TCP,
                                          dst_ip, dport)
                            : -1;
    if (f->handle < 0) {
        /* Nothing to connect to, or no egress at all. Reset now rather than
         * completing a handshake to a connection that does not exist. */
        ns->stats.tcp_refused++;
        tcp_out(ns, f, TCP_RST | TCP_ACK, f->snd_nxt, NULL, 0);
        ns->stats.tcp_resets_out++;
        net_flow_free(ns, f);
        return;
    }
    /* While the connect is in flight, close_at is the deadline for it; see the
     * field comment in net.h for why the two uses cannot overlap. */
    f->close_at = ns->now_ms + 30000u;
    net_tcp_tick(ns, f);            /* a loopback connect is READY at once   */
}

void net_tcp_input(net_stack_t *ns, uint32_t src, uint32_t dst,
                   const uint8_t *seg, size_t n) {
    ns->stats.tcp_in++;
    if (n < TCP_HDR_LEN) { ns->stats.ip_bad_header++; return; }
    if (net_l4_checksum(src, dst, NET_PROTO_TCP, seg, n) != 0u) {
        ns->stats.ip_bad_checksum++;
        return;
    }
    size_t doff = (size_t)(seg[12] >> 4) * 4u;
    if (doff < TCP_HDR_LEN || doff > n) { ns->stats.ip_bad_header++; return; }

    uint16_t gport  = net_rd16(seg);
    uint16_t dport  = net_rd16(seg + 2);
    uint32_t sseq   = net_rd32(seg + 4);
    uint32_t sack   = net_rd32(seg + 8);
    uint8_t  flags  = seg[13];
    const uint8_t *pay = seg + doff;
    size_t   paylen = n - doff;

    net_flow_t *f = net_flow_find(ns, NET_PROTO_TCP, gport, dst, dport);
    if (!f) {
        if ((flags & TCP_SYN) && !(flags & TCP_ACK) && !(flags & TCP_RST))
            tcp_syn(ns, dst, seg, n, doff);
        else
            tcp_reject(ns, dst, seg, n, paylen);
        return;
    }
    f->last_ms = ns->now_ms;

    if (flags & TCP_RST) {
        ns->stats.tcp_resets_in++;
        ns->stats.tcp_last_peer_reset_state    = (uint32_t)f->state;
        ns->stats.tcp_last_peer_reset_window   = f->snd_wnd;
        ns->stats.tcp_last_peer_reset_inflight = f->snd_nxt - f->snd_una;
        ns->stats.tcp_last_peer_reset_buffered = f->txlen;
        ns->stats.tcp_last_peer_reset_retries  = f->rtx;
        net_flow_free(ns, f);
        return;
    }

    if (flags & TCP_SYN) {
        if (f->state == NET_TCP_SYN_RCVD) {
            /* A retransmitted SYN. Repeat the SYN-ACK if we have one to
             * repeat; if the host connect is still pending there is nothing
             * to say yet and silence is the correct answer. */
            if (f->snd_nxt != f->snd_una)
                tcp_out(ns, f, TCP_SYN | TCP_ACK, f->snd_una, NULL, 0);
            return;
        }
        /* A SYN inside an open connection is either a peer that lost its mind
         * or a stale duplicate. RFC 793 §3.9 says reset. */
        tcp_abort(ns, f, NET_TCP_ABORT_UNEXPECTED_SYN);
        return;
    }

    if (!(flags & TCP_ACK)) return;      /* everything past the SYN carries it */

    f->snd_wnd = net_rd16(seg + 14);
    tcp_consume_ack(ns, f, sack);
    if (!f->used) return;

    if (f->state == NET_TCP_SYN_RCVD) {
        if (net_seq_lt(f->snd_una, f->snd_nxt)) return;  /* SYN not acked yet */
        f->state = NET_TCP_ESTABLISHED;
        ns->stats.tcp_established++;
    }

    /*
     * Data. An exactly-in-order segment and an overlapping retransmission are
     * the same case: skip whatever we already have and take the tail.
     */
    uint32_t seg_end = sseq + (uint32_t)paylen;
    if (paylen && !tcp_guest_finished(f->state)) {
        if (net_seq_le(sseq, f->rcv_nxt) && net_seq_lt(f->rcv_nxt, seg_end)) {
            uint32_t skip = f->rcv_nxt - sseq;
            const uint8_t *d = pay + skip;
            uint32_t len = (uint32_t)paylen - skip;
            uint32_t space = NET_TCP_RXBUF - f->rxlen;
            if (len > space) len = space;     /* it exceeded the window we
                                                 advertised; take what fits */
            if (len) {
                memcpy(f->rxbuf + f->rxlen, d, len);
                f->rxlen   += len;
                f->rcv_nxt += len;
                ns->stats.tcp_bytes_to_host += len;
            }
            f->need_ack = true;
        } else {
            /* Out of order, or entirely old. Repeat the cumulative ACK: it
             * tells the guest exactly what we are missing, and it is the only
             * thing that restarts a flow after a lost segment given that we
             * keep no reassembly queue. */
            ns->stats.tcp_out_of_order++;
            f->need_ack = true;
        }
    }

    /*
     * A FIN that arrives once the guest's close has already been accepted is a
     * RETRANSMISSION — its own ACK was lost. RFC 793 §3.5 says acknowledge it
     * again and restart the TIME-WAIT clock; doing nothing is what turns one
     * lost ACK into a guest that retransmits until it gives up.
     */
    if ((flags & TCP_FIN) && tcp_guest_finished(f->state)) {
        f->need_ack = true;
        if (f->state == NET_TCP_TIME_WAIT)
            f->close_at = ns->now_ms + NET_TCP_TIME_WAIT_MS;
    }

    if ((flags & TCP_FIN) && seg_end == f->rcv_nxt &&
        !tcp_guest_finished(f->state)) {
        f->rcv_nxt++;                       /* the FIN occupies a sequence   */
        f->need_ack = true;
        switch (f->state) {
            case NET_TCP_ESTABLISHED: f->state = NET_TCP_CLOSE_WAIT; break;
            case NET_TCP_FIN_WAIT_1:  f->state = NET_TCP_CLOSING;    break;
            case NET_TCP_FIN_WAIT_2:
                f->state    = NET_TCP_TIME_WAIT;
                f->close_at = ns->now_ms + NET_TCP_TIME_WAIT_MS;
                break;
            default: break;
        }
    }

    net_tcp_tick(ns, f);
}

/* ---------------------------------------------------------------- tick --- */

void net_tcp_tick(net_stack_t *ns, net_flow_t *f) {
    if (!f->used) return;

    if (f->state == NET_TCP_TIME_WAIT) {
        /*
         * The last acknowledgement of the connection, and it has to be sent
         * from here because this return is above tcp_pump(). Acknowledging the
         * peer's FIN — and re-acknowledging a retransmitted one — is the entire
         * job of TIME-WAIT; skipping it leaves the guest retransmitting its FIN
         * until its own R2 expires, which looks like a hung socket rather than
         * like a missing ACK.
         */
        if (f->need_ack) tcp_out(ns, f, TCP_ACK, f->snd_nxt, NULL, 0);
        if ((int32_t)(ns->now_ms - f->close_at) >= 0) net_flow_free(ns, f);
        return;
    }
    if (f->state == NET_TCP_CLOSED) { net_flow_free(ns, f); return; }

    /* The host connect, which is the one thing we wait for before answering
     * the guest's SYN. Acknowledging a connection that does not exist yet
     * would hand the guest an ESTABLISHED socket that then resets. */
    if (f->state == NET_TCP_SYN_RCVD && f->snd_nxt == f->snd_una) {
        int st = ns->eg.status ? ns->eg.status(ns->eg.ctx, f->handle)
                               : NET_ST_READY;
        if (st == NET_ST_PENDING) {
            if ((int32_t)(ns->now_ms - f->close_at) >= 0) {
                ns->stats.tcp_refused++;
                tcp_abort(ns, f, NET_TCP_ABORT_CONNECT);
            }
            return;
        }
        if (st == NET_ST_FAILED) {
            ns->stats.tcp_refused++;
            tcp_abort(ns, f, NET_TCP_ABORT_CONNECT);
            return;
        }
        if (!tcp_out(ns, f, TCP_SYN | TCP_ACK, f->snd_nxt, NULL, 0))
            return;
        f->snd_nxt++;
        tcp_timers(ns, f);
        return;
    }

    /* Guest -> host. Whatever the socket refuses stays buffered and shrinks
     * the window we advertise, which is the back-pressure docs/networking.md
     * §8.3 asks for: a full queue never blocks and never silently drops. */
    if (f->rxlen && ns->eg.send) {
        int w = ns->eg.send(ns->eg.ctx, f->handle, f->rxbuf, f->rxlen);
        if (w < 0) {
            tcp_abort(ns, f, NET_TCP_ABORT_HOST_SEND);
            return;
        }
        if (w > 0) {
            memmove(f->rxbuf, f->rxbuf + w, f->rxlen - (uint32_t)w);
            f->rxlen -= (uint32_t)w;
            f->need_ack = true;      /* the window just re-opened; say so    */
        }
    }
    if (tcp_guest_finished(f->state) && f->rxlen == 0u && !f->tx_shutdown) {
        f->tx_shutdown = true;
        if (ns->eg.shutdown_tx) ns->eg.shutdown_tx(ns->eg.ctx, f->handle);
    }

    /* Host -> guest, into the space the guest's window and our buffer allow. */
    if (!f->host_eof && ns->eg.recv && f->txlen < NET_TCP_TXBUF) {
        for (unsigned guard = 0; guard < 8u && f->txlen < NET_TCP_TXBUF;
             guard++) {
            int r = ns->eg.recv(ns->eg.ctx, f->handle, f->txbuf + f->txlen,
                                NET_TCP_TXBUF - f->txlen);
            if (r == NET_EG_WOULDBLOCK) break;
            if (r == NET_EG_EOF) { f->host_eof = true; break; }
            if (r < 0) {
                tcp_abort(ns, f, NET_TCP_ABORT_HOST_RECV);
                return;
            }
            f->txlen += (uint32_t)r;
        }
    }

    if (f->rto_on && (int32_t)(ns->now_ms - f->rto_at) >= 0) {
        f->rto_on = false;
        if (++f->rtx > NET_TCP_RTX_MAX) {
            /* RFC 1122 §4.2.3.5's R2: give up and tell the guest, rather than
             * leaving a flow that will never move and never be reported. */
            tcp_abort(ns, f, NET_TCP_ABORT_RETRANSMIT);
            return;
        }
        ns->stats.tcp_retransmits++;
        if (f->snd_nxt == f->snd_una && f->txlen && f->snd_wnd == 0u) {
            /* Persist. RFC 1122 §4.2.2.17 explicitly allows the probe to
             * carry one octet beyond the offered window. */
            if (tcp_out(ns, f, TCP_ACK, f->snd_nxt, f->txbuf, 1u)) {
                f->snd_nxt++;
                tcp_timers(ns, f);
            }
            return;
        }
        /* Go back N. tcp_pump() rebuilds the whole window from snd_una, and
         * the FIN follows the data again because it lives at snd_una + txlen
         * rather than in a flag that could be re-sent out of order. */
        tcp_cwnd_timeout(f);
        f->snd_nxt = f->snd_una;
    }

    tcp_pump(ns, f);

    /*
     * The last acknowledgement of a connection that ended cleanly. FIN-WAIT-2
     * with the host stream done and nothing left to send means we are waiting
     * only on the guest, and CLOSED means both sides finished — either way the
     * slot goes back.
     */
    if (f->state == NET_TCP_CLOSED) net_flow_free(ns, f);
}
