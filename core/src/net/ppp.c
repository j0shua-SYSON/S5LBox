/*
 * S5LBox — the host's PPP peer. Milestone N2 of docs/networking.md §10.
 *
 * The guest runs Apple's own /usr/sbin/pppd against /dev/tty.debug, which is
 * uart4 in core/src/soc/uart.c. This file is the other end of that line: it
 * deframes what pppd sends, answers LCP and IPCP, and hands the guest
 * 10.0.2.15. Everything it implements is from a published specification —
 * RFC 1662 (HDLC-async framing), RFC 1661 (LCP and the option automaton),
 * RFC 1332 (IPCP) and RFC 1877 (the DNS options) — which is a large part of
 * why docs/networking.md §12 picked this route at all.
 *
 * WHAT THE EVIDENCE ACTUALLY SAYS. run80 recorded 47 bytes of a real guest's
 * first Configure-Request in work/run80-ppp-tty/uart4-ppp.bin. Decoded, they
 * are one LCP Configure-Request, identifier 1, length 20, carrying FOUR
 * options: ASYNCMAP 0x00000000, Magic-Number 0x7961f51c, Protocol-Field-
 * Compression and Address-and-Control-Field-Compression. There is NO
 * Maximum-Receive-Unit option — pppd omits it when it wants the default 1500 —
 * so anything this file does with MRU is written from RFC 1661 §6.1 and is
 * UNTESTED AGAINST THIS GUEST. That distinction is kept everywhere below.
 *
 * TWO DELIBERATE ASYMMETRIES, both in the direction of "cannot go wrong".
 *
 * 1. WE ALWAYS ESCAPE AS IF THE ASYNC-CONTROL-CHARACTER-MAP WERE 0xFFFFFFFF.
 *    RFC 1662 §7.1 makes 0xFFFFFFFF the default until the peer's map is
 *    negotiated, and §4.2 requires the receiver to un-escape any escaped octet
 *    whether or not the map demanded it. So escaping more than strictly
 *    required is always correct and escaping less can break a link over a
 *    transport that eats control characters. The cost is a few octets per
 *    frame on a line with no real baud rate. tx_accm is a field rather than a
 *    constant so the day someone wants to narrow it, the change is one
 *    assignment and one test.
 *
 * 2. WE NEVER COMPRESS, AND WE NEVER ASK TO. We acknowledge the guest's PCOMP
 *    and ACCOMP — refusing them would be gratuitous, and our deframer accepts
 *    both forms unconditionally — but we do not request them ourselves, so
 *    every frame we send carries FF 03 and a full two-octet protocol field.
 *    That removes an entire class of "which form did we agree to" bug from the
 *    transmit path, at a cost of three octets per frame.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "ppp.h"
#include <string.h>

/* ============================================================== FCS-16 === */

/*
 * RFC 1662 §C.2's FCS-16: the CRC over the polynomial x^16 + x^12 + x^5 + 1,
 * reflected, which is 0x8408 when the register shifts right.
 *
 * Computed a bit at a time rather than through the RFC's 256-entry table. The
 * table would be four times faster and this peer's whole traffic is a few
 * hundred octets a boot, so the trade is: no 512-byte constant to transcribe
 * (and to get one entry wrong in, silently), and nothing copied from anywhere.
 */
uint16_t ppp_fcs16(uint16_t fcs, const uint8_t *data, size_t n) {
    if (!data) return fcs;
    for (size_t i = 0; i < n; i++) {
        fcs ^= data[i];
        for (unsigned b = 0; b < 8u; b++)
            fcs = (uint16_t)((fcs & 1u) ? ((fcs >> 1) ^ 0x8408u) : (fcs >> 1));
    }
    return fcs;
}

/* ============================================================= framing === */

/* RFC 1662 §4.2: 0x7E and 0x7D are always escaped; an octet below 0x20 is
 * escaped when its bit is set in the map. */
static bool needs_escape(uint8_t c, uint32_t accm) {
    if (c == PPP_FLAG || c == PPP_ESCAPE) return true;
    if (c < 0x20u) return (accm & (1u << c)) != 0u;
    return false;
}

static size_t emit(uint8_t *out, size_t cap, size_t at, uint8_t c,
                   uint32_t accm) {
    if (needs_escape(c, accm)) {
        if (at + 2u > cap) return cap + 1u;      /* signals "did not fit" */
        out[at++] = PPP_ESCAPE;
        out[at++] = (uint8_t)(c ^ PPP_TRANS);
    } else {
        if (at + 1u > cap) return cap + 1u;
        out[at++] = c;
    }
    return at;
}

size_t ppp_frame(uint16_t proto, const uint8_t *info, size_t info_len,
                 uint32_t accm, uint8_t *out, size_t cap) {
    if (!out || (!info && info_len)) return 0;
    if (info_len > PPP_MRU_DEFAULT) return 0;

    /* The FCS covers address, control, protocol and information — everything
     * between the flags, BEFORE escaping (RFC 1662 §3.1, §4.3). */
    const uint8_t hdr[4] = {
        PPP_ALLSTATIONS, PPP_UI,
        (uint8_t)(proto >> 8), (uint8_t)(proto & 0xffu)
    };
    uint16_t fcs = ppp_fcs16(PPP_FCS_INIT, hdr, sizeof hdr);
    fcs = ppp_fcs16(fcs, info, info_len);
    fcs = (uint16_t)(fcs ^ 0xffffu);      /* RFC 1662 §3.1: ones complement */

    size_t at = 0;
    if (at + 1u > cap) return 0;
    out[at++] = PPP_FLAG;
    for (size_t i = 0; i < sizeof hdr; i++) {
        at = emit(out, cap, at, hdr[i], accm);
        if (at > cap) return 0;
    }
    for (size_t i = 0; i < info_len; i++) {
        at = emit(out, cap, at, info[i], accm);
        if (at > cap) return 0;
    }
    /* Low-order octet first (RFC 1662 §3.1). */
    at = emit(out, cap, at, (uint8_t)(fcs & 0xffu), accm);
    if (at > cap) return 0;
    at = emit(out, cap, at, (uint8_t)(fcs >> 8), accm);
    if (at > cap) return 0;
    if (at + 1u > cap) return 0;
    out[at++] = PPP_FLAG;
    return at;
}

/* ======================================================== transmit ring === */

static size_t tx_used(const ppp_peer_t *p) { return p->tx_head - p->tx_tail; }

/*
 * Queue one whole frame or none of it. A frame committed in pieces and then
 * truncated by a full ring is worse than a frame never sent: the far end sees a
 * valid-looking prefix and an FCS error, and blames the line.
 */
static void tx_frame(ppp_peer_t *p, uint16_t proto,
                     const uint8_t *info, size_t info_len) {
    uint8_t enc[2u * PPP_MAX_FRAME + 2u];
    size_t n = ppp_frame(proto, info, info_len, p->tx_accm, enc, sizeof enc);
    if (n == 0u || n > PPP_TX_RING - tx_used(p)) {
        p->stats.tx_overflows++;
        return;
    }
    for (size_t i = 0; i < n; i++)
        p->tx[(p->tx_head + (uint32_t)i) % PPP_TX_RING] = enc[i];
    p->tx_head += (uint32_t)n;
    p->stats.frames_out++;
    p->stats.tx_bytes += n;
}

size_t ppp_output_pending(const ppp_peer_t *p) {
    return p ? tx_used(p) : 0u;
}

int ppp_output_byte(ppp_peer_t *p) {
    if (!p || p->tx_head == p->tx_tail) return -1;
    uint8_t b = p->tx[p->tx_tail % PPP_TX_RING];
    p->tx_tail++;
    return (int)b;
}

size_t ppp_output(ppp_peer_t *p, uint8_t *buf, size_t cap) {
    if (!p || !buf) return 0;
    size_t n = 0;
    while (n < cap) {
        int b = ppp_output_byte(p);
        if (b < 0) break;
        buf[n++] = (uint8_t)b;
    }
    return n;
}

/* ================================================ control-packet output === */

/* RFC 1661 §5: code, identifier, a two-octet length that COUNTS ITSELF and the
 * code and identifier, then the data. */
static void send_cp(ppp_peer_t *p, uint16_t proto, uint8_t code, uint8_t id,
                    const uint8_t *data, size_t n) {
    uint8_t pkt[PPP_MRU_DEFAULT];
    if (n + 4u > sizeof pkt) { p->stats.tx_overflows++; return; }
    pkt[0] = code;
    pkt[1] = id;
    pkt[2] = (uint8_t)((n + 4u) >> 8);
    pkt[3] = (uint8_t)((n + 4u) & 0xffu);
    if (n) memcpy(pkt + 4, data, n);
    tx_frame(p, proto, pkt, n + 4u);
}

static void put_be32(uint8_t *at, uint32_t v) {
    at[0] = (uint8_t)(v >> 24); at[1] = (uint8_t)(v >> 16);
    at[2] = (uint8_t)(v >> 8);  at[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t *at) {
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) |
           ((uint32_t)at[2] << 8)  |  (uint32_t)at[3];
}

/* ================================================= the RFC 1661 automaton == */

static ppp_fsm_t *fsm_for(ppp_peer_t *p, uint16_t proto) {
    return proto == PPP_PROTO_LCP ? &p->lcp : &p->ipcp;
}

/* Build the Configure-Request we are currently willing to make. Rebuilt on
 * every send because a Configure-Reject shrinks it (RFC 1661 §5.4). */
static size_t build_confreq(ppp_peer_t *p, uint16_t proto, uint8_t *out) {
    size_t n = 0;
    if (proto == PPP_PROTO_LCP) {
        if (p->lcp.req_opts & PPP_REQ_LCP_ASYNCMAP) {
            /* RFC 1662 §7.1. Zero means "you need not escape any control
             * character when sending to me" — this deframer un-escapes
             * whatever arrives, so it can honestly ask for the cheapest map. */
            out[n++] = LCP_OPT_ASYNCMAP; out[n++] = 6u;
            put_be32(out + n, 0x00000000u); n += 4u;
        }
        if (p->lcp.req_opts & PPP_REQ_LCP_MAGIC) {
            out[n++] = LCP_OPT_MAGIC; out[n++] = 6u;
            put_be32(out + n, p->our_magic); n += 4u;
        }
        /* No MRU (we accept the RFC 1661 §6.1 default of 1500), no PCOMP and
         * no ACCOMP — see asymmetry 2 in the file header. */
    } else {
        if (p->ipcp.req_opts & PPP_REQ_IPCP_ADDRESS) {
            /* RFC 1332 §3.3. We name our own address; the guest's is assigned
             * by Nak'ing whatever it asks for. */
            out[n++] = IPCP_OPT_ADDRESS; out[n++] = 6u;
            put_be32(out + n, p->cfg.local_ip); n += 4u;
        }
    }
    return n;
}

static void restart_start(ppp_peer_t *p, ppp_fsm_t *f) {
    f->timer_running = true;
    f->restart_at_ms = p->now_ms + PPP_RESTART_MS;
}
static void restart_stop(ppp_fsm_t *f) { f->timer_running = false; }

/* "irc" — Initialize-Restart-Count (RFC 1661 §4.4). */
static void irc(ppp_fsm_t *f, uint8_t count) {
    f->restarts = count;
    f->failures = PPP_MAX_FAILURE;
}

/* "scr" — Send-Configure-Request. `fresh` picks a new identifier; a
 * retransmission must reuse the old one (RFC 1661 §5.1). */
static void scr(ppp_peer_t *p, uint16_t proto, bool fresh) {
    ppp_fsm_t *f = fsm_for(p, proto);
    uint8_t opts[64];
    size_t n = build_confreq(p, proto, opts);
    if (fresh) f->last_req_id = f->next_id++;
    send_cp(p, proto, PPP_CONF_REQ, f->last_req_id, opts, n);
    if (f->restarts) f->restarts--;
    restart_start(p, f);
}

/* "str" — Send-Terminate-Request. */
static void str_(ppp_peer_t *p, uint16_t proto) {
    ppp_fsm_t *f = fsm_for(p, proto);
    f->last_req_id = f->next_id++;
    send_cp(p, proto, PPP_TERM_REQ, f->last_req_id, NULL, 0);
    if (f->restarts) f->restarts--;
    restart_start(p, f);
}

/* "sta" — Send-Terminate-Ack. */
static void sta(ppp_peer_t *p, uint16_t proto, uint8_t id) {
    send_cp(p, proto, PPP_TERM_ACK, id, NULL, 0);
}

static void ipcp_start(ppp_peer_t *p);

/* "tlu" / "tld" — This-Layer-Up / This-Layer-Down. LCP's up event is what
 * starts IPCP; IPCP's is what hands the guest its address. */
static void tlu(ppp_peer_t *p, uint16_t proto) {
    if (proto == PPP_PROTO_LCP) {
        ipcp_start(p);
    } else {
        p->assigned_ip = p->cfg.remote_ip;
        p->assigned = true;
    }
}
static void tld(ppp_peer_t *p, uint16_t proto) {
    if (proto == PPP_PROTO_LCP) {
        p->ipcp.state = PPP_S_INITIAL;
        restart_stop(&p->ipcp);
        p->assigned = false;
    } else {
        p->assigned = false;
    }
}

/* ================================================== option negotiation === */

/*
 * The verdict on one Configuration Option. RFC 1661 §5.3/§5.4 draw the line
 * differently from how it is usually described: a Configure-Nak carries options
 * that are RECOGNIZED but whose VALUE is unacceptable, and a Configure-Reject
 * carries options that are not recognizable OR are not negotiable at all. An
 * option we understand perfectly well and simply will not do — authentication —
 * is therefore a Reject, not a Nak, because there is no value of it we would
 * accept and a Nak invites the peer to try another.
 */
typedef enum { OPT_ACK, OPT_NAK, OPT_REJ } opt_verdict_t;

/*
 * `nak` receives the option as we would like to see it, when the verdict is
 * OPT_NAK. Its length is returned in *nak_len.
 */
static opt_verdict_t lcp_review(ppp_peer_t *p, const uint8_t *o, uint8_t olen,
                                uint8_t *nak, uint8_t *nak_len) {
    uint8_t type = o[0];
    *nak_len = 0;
    switch (type) {
        case LCP_OPT_MRU:
            /* RFC 1661 §6.1. UNTESTED against this guest: run80's pppd did not
             * send this option at all. */
            if (olen != 4u) return OPT_REJ;
            if ((uint32_t)(((uint32_t)o[2] << 8) | o[3]) > PPP_MRU_DEFAULT) {
                nak[0] = LCP_OPT_MRU; nak[1] = 4u;
                nak[2] = (uint8_t)(PPP_MRU_DEFAULT >> 8);
                nak[3] = (uint8_t)(PPP_MRU_DEFAULT & 0xffu);
                *nak_len = 4u;
                return OPT_NAK;
            }
            p->peer_mru = (uint16_t)(((uint16_t)o[2] << 8) | o[3]);
            return OPT_ACK;

        case LCP_OPT_ASYNCMAP:
            /* RFC 1662 §7.1. Any map is acceptable: it constrains what WE
             * escape when transmitting, and we already escape a superset. */
            if (olen != 6u) return OPT_REJ;
            p->peer_asyncmap = get_be32(o + 2);
            return OPT_ACK;

        case LCP_OPT_AUTH:
            /* Recognized, and there is no value we would accept: this peer
             * does not authenticate and the guest's job is started with
             * `noauth`. Reject, not Nak — see the comment on opt_verdict_t. */
            return OPT_REJ;

        case LCP_OPT_QUALITY:
            return OPT_REJ;                  /* no Link Quality Monitoring */

        case LCP_OPT_MAGIC: {
            if (olen != 6u) return OPT_REJ;
            uint32_t m = get_be32(o + 2);
            if (m == p->our_magic) {
                /* RFC 1661 §6.4: equal magic numbers mean the line may be
                 * looped back. Nak with a different one and let the peer pick
                 * again; if it is a genuine loop the numbers keep colliding
                 * and Max-Failure ends it. */
                uint32_t alt = m ^ 0xa5a5a5a5u;
                if (alt == m) alt = m + 1u;
                nak[0] = LCP_OPT_MAGIC; nak[1] = 6u;
                put_be32(nak + 2, alt);
                *nak_len = 6u;
                return OPT_NAK;
            }
            p->peer_magic = m;
            p->peer_magic_valid = true;
            return OPT_ACK;
        }

        case LCP_OPT_PCOMP:
            if (olen != 2u) return OPT_REJ;
            p->peer_pcomp = true;            /* the deframer accepts both */
            return OPT_ACK;

        case LCP_OPT_ACCOMP:
            if (olen != 2u) return OPT_REJ;
            p->peer_accomp = true;
            return OPT_ACK;

        default:
            return OPT_REJ;                  /* not recognized: RFC 1661 §5.4 */
    }
}

static opt_verdict_t ipcp_review(ppp_peer_t *p, const uint8_t *o, uint8_t olen,
                                 uint8_t *nak, uint8_t *nak_len) {
    uint8_t type = o[0];
    *nak_len = 0;
    switch (type) {
        case IPCP_OPT_ADDRESS: {
            /* RFC 1332 §3.3. This is the whole point of N2: whatever the guest
             * proposes — and pppd with no `ipparam` proposes 0.0.0.0 — we Nak
             * with the address docs/networking.md §8.2 assigns it. Only when it
             * comes back asking for exactly that do we Ack. */
            if (olen != 6u) return OPT_REJ;
            uint32_t want = get_be32(o + 2);
            if (want == p->cfg.remote_ip) return OPT_ACK;
            nak[0] = IPCP_OPT_ADDRESS; nak[1] = 6u;
            put_be32(nak + 2, p->cfg.remote_ip);
            *nak_len = 6u;
            return OPT_NAK;
        }

        case IPCP_OPT_COMPRESS:
            /* Van Jacobson TCP/IP header compression (RFC 1332 §3.2). Not
             * implemented, and a compressed header we could not decode would be
             * a silently corrupted packet — exactly what this core refuses to
             * risk. Reject. */
            return OPT_REJ;

        case IPCP_OPT_ADDRESSES:
            /* Deprecated by RFC 1332 §3.1 itself. */
            return OPT_REJ;

        case IPCP_OPT_DNS1:
        case IPCP_OPT_DNS2: {
            /* RFC 1877 §1.1/1.2, sent by pppd only with `usepeerdns`. Answer
             * with the address docs/networking.md §8.2 names, even though
             * nothing answers DNS there yet: that is N5, and handing out an
             * address for a server that does not exist is better than Rejecting
             * an option we intend to support. */
            if (olen != 6u) return OPT_REJ;
            p->stats.ipcp_dns_requests++;
            uint32_t want = get_be32(o + 2);
            uint32_t have = type == IPCP_OPT_DNS1 ? p->cfg.dns1 : p->cfg.dns2;
            if (want == have) { p->stats.ipcp_dns_acked++; return OPT_ACK; }
            nak[0] = type; nak[1] = 6u;
            put_be32(nak + 2, have);
            *nak_len = 6u;
            p->stats.ipcp_dns_naked++;
            return OPT_NAK;
        }

        default:
            return OPT_REJ;
    }
}

/*
 * RFC 1661 §5.1: walk the options, classify each, and decide on EXACTLY ONE
 * answer — Reject beats Nak beats Ack — building the response's option list as
 * we go. A Reject or Nak carries only the offending options; an Ack carries all
 * of them verbatim.
 *
 * This DECIDES and does not send, because RFC 1661 §4.1's action lists for the
 * Stopped and Opened states put "scr" ahead of "sca"/"scn": the peer must see
 * our own Configure-Request before our answer to its. Sending inside the
 * classifier made that ordering impossible to express.
 */
typedef enum { CR_INVALID, CR_ACK, CR_NAK, CR_REJ } cr_result_t;

static cr_result_t classify_confreq(ppp_peer_t *p, uint16_t proto,
                                    const uint8_t *opts, size_t n,
                                    uint8_t *resp, size_t *resp_len) {
    uint8_t rej[PPP_MRU_DEFAULT], nak[PPP_MRU_DEFAULT];
    uint8_t one[256];              /* one option; its Length field is 8 bits */
    size_t rejn = 0, nakn = 0;
    *resp_len = 0;

    for (size_t i = 0; i < n; ) {
        /* RFC 1661 §5: a Configure-Request whose options do not tile its
         * length exactly is invalid, and an invalid packet is discarded
         * without an answer of any kind. */
        if (n - i < 2u) return CR_INVALID;
        uint8_t olen = opts[i + 1];
        if (olen < 2u || (size_t)olen > n - i) return CR_INVALID;

        uint8_t onelen = 0;
        opt_verdict_t v = proto == PPP_PROTO_LCP
                        ? lcp_review(p, opts + i, olen, one, &onelen)
                        : ipcp_review(p, opts + i, olen, one, &onelen);
        if (v == OPT_REJ) {
            if (rejn + olen <= sizeof rej) {
                memcpy(rej + rejn, opts + i, olen);
                rejn += olen;
            }
        } else if (v == OPT_NAK) {
            if (nakn + onelen <= sizeof nak) {
                memcpy(nak + nakn, one, onelen);
                nakn += onelen;
            }
        }
        i += olen;
    }

    if (rejn) { memcpy(resp, rej, rejn); *resp_len = rejn; return CR_REJ; }
    if (nakn) { memcpy(resp, nak, nakn); *resp_len = nakn; return CR_NAK; }
    if (n) memcpy(resp, opts, n);
    *resp_len = n;
    return CR_ACK;
}

/*
 * A Configure-Nak or Configure-Reject aimed at OUR request. RFC 1661 §5.3/§5.4:
 * a Nak means "not that value", a Reject means "not that option ever". Both
 * change the request we build next.
 */
static void apply_nak(ppp_peer_t *p, uint16_t proto,
                      const uint8_t *opts, size_t n) {
    for (size_t i = 0; i + 2u <= n; ) {
        uint8_t type = opts[i], olen = opts[i + 1];
        if (olen < 2u || (size_t)olen > n - i) return;
        if (proto == PPP_PROTO_LCP) {
            if (type == LCP_OPT_MAGIC && olen == 6u)
                p->our_magic = get_be32(opts + i + 2);
            /* An ASYNCMAP Nak asks us to escape more. We already escape
             * everything (asymmetry 1), so adopting the suggestion changes
             * nothing on the wire — but the request we send next must carry
             * their value or they will Nak it forever. */
        } else {
            if (type == IPCP_OPT_ADDRESS && olen == 6u)
                p->cfg.local_ip = get_be32(opts + i + 2);
        }
        i += olen;
    }
}

static void apply_rej(ppp_peer_t *p, uint16_t proto,
                      const uint8_t *opts, size_t n) {
    for (size_t i = 0; i + 2u <= n; ) {
        uint8_t type = opts[i], olen = opts[i + 1];
        if (olen < 2u || (size_t)olen > n - i) return;
        if (proto == PPP_PROTO_LCP) {
            if (type == LCP_OPT_MAGIC)    p->lcp.req_opts &= (uint8_t)~PPP_REQ_LCP_MAGIC;
            if (type == LCP_OPT_ASYNCMAP) p->lcp.req_opts &= (uint8_t)~PPP_REQ_LCP_ASYNCMAP;
        } else {
            if (type == IPCP_OPT_ADDRESS) p->ipcp.req_opts &= (uint8_t)~PPP_REQ_IPCP_ADDRESS;
        }
        i += olen;
    }
}

/* ==================================================== the state machine === */

/*
 * RFC 1661 §4.1's table, one row per event, transcribed for the states this
 * peer can reach. Every transition below can be checked against the RFC by
 * name: the comment on each case is the RFC's own action list.
 */

static void ev_rcr(ppp_peer_t *p, uint16_t proto, uint8_t id,
                   const uint8_t *opts, size_t n) {
    ppp_fsm_t *f = fsm_for(p, proto);

    if (f->state == PPP_S_INITIAL || f->state == PPP_S_CLOSED) {
        /* Closed + RCR: "sta" and stay Closed. Nobody has issued the Open
         * event, so this peer has no business negotiating. */
        sta(p, proto, id);
        return;
    }

    /* Decide before sending anything: two of the five states below owe the peer
     * a Configure-Request of our own FIRST (RFC 1661 §4.1 lists "scr" ahead of
     * "sca"/"scn"), and a classifier that sent as it decided could not express
     * that order. An unparseable option list is discarded without an answer of
     * any kind, so nothing at all has been transmitted at this point. */
    uint8_t     resp[PPP_MRU_DEFAULT];
    size_t      respn = 0;
    cr_result_t r = classify_confreq(p, proto, opts, n, resp, &respn);
    if (r == CR_INVALID) return;
    bool good = (r == CR_ACK);

    ppp_state_t entry = f->state;
    if (entry == PPP_S_STOPPED) {
        /* irc, scr, sca/scn. The counters are refreshed BEFORE the request is
         * judged: a peer that once exhausted Max-Failure must still be able to
         * answer a guest that has just restarted its pppd. */
        irc(f, PPP_MAX_CONFIGURE);
        scr(p, proto, true);
    } else if (entry == PPP_S_OPENED) {
        /* tld, scr, sca/scn — the peer is renegotiating an open link. */
        tld(p, proto);
        irc(f, PPP_MAX_CONFIGURE);
        scr(p, proto, true);
    }

    send_cp(p, proto,
            r == CR_ACK ? PPP_CONF_ACK : (r == CR_NAK ? PPP_CONF_NAK
                                                      : PPP_CONF_REJ),
            id, resp, respn);

    if (!good) {
        if (!f->failures) {                  /* Max-Failure, RFC 1661 §4.6 */
            f->state = PPP_S_STOPPED;
            restart_stop(f);
            return;
        }
        f->failures--;
    }

    switch (entry) {
        case PPP_S_STOPPED:
        case PPP_S_OPENED:
        case PPP_S_REQSENT:
        case PPP_S_ACKSENT:
            f->state = good ? PPP_S_ACKSENT : PPP_S_REQSENT;
            break;
        case PPP_S_ACKRCVD:
            /* "sca,tlu / 9" on RCR+; RCR- leaves Ack-Rcvd unchanged. */
            if (good) { f->state = PPP_S_OPENED; restart_stop(f); tlu(p, proto); }
            break;
        default:
            break;
    }
}

static void ev_rca(ppp_peer_t *p, uint16_t proto, uint8_t id) {
    ppp_fsm_t *f = fsm_for(p, proto);
    /* RFC 1661 §5.2: an Ack whose identifier does not match the outstanding
     * request is invalid and MUST be silently discarded. */
    if (id != f->last_req_id) return;

    switch (f->state) {
        case PPP_S_CLOSED:
        case PPP_S_STOPPED:
            sta(p, proto, id);
            break;
        case PPP_S_REQSENT:
            /*
             * "irc / 7". The Restart timer keeps running, and that is not an
             * oversight: RFC 1661 §4.1 lists a TO+ event for Ack-Rcvd, whose
             * action is scr back to Req-Sent. A peer that acknowledged our
             * request and then never sent one of its own would otherwise leave
             * this side waiting forever with no timer to notice.
             */
            irc(f, PPP_MAX_CONFIGURE);
            f->state = PPP_S_ACKRCVD;
            break;
        case PPP_S_ACKRCVD:
            /* Crossed connection: scr, back to Req-Sent. */
            scr(p, proto, true);
            f->state = PPP_S_REQSENT;
            break;
        case PPP_S_ACKSENT:
            irc(f, PPP_MAX_CONFIGURE);
            restart_stop(f);
            f->state = PPP_S_OPENED;
            tlu(p, proto);
            break;
        case PPP_S_OPENED:
            tld(p, proto);
            scr(p, proto, true);
            f->state = PPP_S_REQSENT;
            break;
        default:
            break;
    }
}

static void ev_rcn(ppp_peer_t *p, uint16_t proto, uint8_t id) {
    ppp_fsm_t *f = fsm_for(p, proto);
    if (id != f->last_req_id) return;

    switch (f->state) {
        case PPP_S_CLOSED:
        case PPP_S_STOPPED:
            sta(p, proto, id);
            break;
        case PPP_S_REQSENT:
        case PPP_S_ACKSENT:
            irc(f, PPP_MAX_CONFIGURE);
            scr(p, proto, true);
            break;               /* state unchanged */
        case PPP_S_ACKRCVD:
            scr(p, proto, true);
            f->state = PPP_S_REQSENT;
            break;
        case PPP_S_OPENED:
            tld(p, proto);
            scr(p, proto, true);
            f->state = PPP_S_REQSENT;
            break;
        default:
            break;
    }
}

static void ev_rtr(ppp_peer_t *p, uint16_t proto, uint8_t id) {
    ppp_fsm_t *f = fsm_for(p, proto);
    switch (f->state) {
        case PPP_S_OPENED:
            tld(p, proto);
            f->restarts = 0;                 /* zrc */
            sta(p, proto, id);
            f->state = PPP_S_STOPPING;
            restart_start(p, f);
            break;
        case PPP_S_REQSENT:
        case PPP_S_ACKRCVD:
        case PPP_S_ACKSENT:
            sta(p, proto, id);
            f->state = PPP_S_REQSENT;
            break;
        default:
            sta(p, proto, id);
            break;
    }
}

static void ev_rta(ppp_peer_t *p, uint16_t proto) {
    ppp_fsm_t *f = fsm_for(p, proto);
    switch (f->state) {
        case PPP_S_CLOSING:
            restart_stop(f);
            f->state = PPP_S_CLOSED;
            break;
        case PPP_S_STOPPING:
            restart_stop(f);
            f->state = PPP_S_STOPPED;
            break;
        case PPP_S_ACKRCVD:
            f->state = PPP_S_REQSENT;
            break;
        case PPP_S_OPENED:
            tld(p, proto);
            scr(p, proto, true);
            f->state = PPP_S_REQSENT;
            break;
        default:
            break;
    }
}

static void ipcp_start(ppp_peer_t *p) {
    /* LCP came up: RFC 1661 §3.2's Network-Layer Protocol phase. */
    p->ipcp.state = PPP_S_REQSENT;
    p->ipcp.req_opts = PPP_REQ_IPCP_ADDRESS;
    irc(&p->ipcp, PPP_MAX_CONFIGURE);
    scr(p, PPP_PROTO_IPCP, true);
}

/* ==================================================== packet dispatch ==== */

static void cp_input(ppp_peer_t *p, uint16_t proto,
                     const uint8_t *pkt, size_t len) {
    /* RFC 1661 §5: fewer than four octets, or a Length that does not fit the
     * frame, is an invalid packet and is silently discarded. Octets past
     * Length are padding and are ignored. */
    if (len < 4u) return;
    uint8_t  code = pkt[0];
    uint8_t  id   = pkt[1];
    size_t   plen = ((size_t)pkt[2] << 8) | pkt[3];
    if (plen < 4u || plen > len) return;

    const uint8_t *data = pkt + 4;
    size_t         dlen = plen - 4u;
    ppp_fsm_t     *f    = fsm_for(p, proto);

    switch (code) {
        case PPP_CONF_REQ: ev_rcr(p, proto, id, data, dlen); return;
        case PPP_CONF_ACK: ev_rca(p, proto, id); return;
        case PPP_CONF_NAK:
            if (id == f->last_req_id) apply_nak(p, proto, data, dlen);
            ev_rcn(p, proto, id);
            return;
        case PPP_CONF_REJ:
            if (id == f->last_req_id) apply_rej(p, proto, data, dlen);
            ev_rcn(p, proto, id);
            return;
        case PPP_TERM_REQ: ev_rtr(p, proto, id); return;
        case PPP_TERM_ACK: ev_rta(p, proto); return;

        case PPP_CODE_REJ:
        case PPP_PROTO_REJ:
            /*
             * RFC 1661 §5.5/§5.7. A rejection of a code or protocol this peer
             * cannot do without is event RXJ- and takes the layer down. We do
             * not try to distinguish "catastrophic" from "acceptable" here:
             * everything we send is required, so any rejection of it is fatal
             * and saying so loudly beats limping.
             */
            f->state = PPP_S_STOPPED;
            restart_stop(f);
            if (proto == PPP_PROTO_LCP) tld(p, PPP_PROTO_LCP);
            return;

        case PPP_ECHO_REQ:
            /* RFC 1661 §5.8: valid only in Opened; the reply carries OUR magic
             * number in place of theirs, followed by their data verbatim. */
            if (proto != PPP_PROTO_LCP || f->state != PPP_S_OPENED) return;
            if (dlen < 4u) return;
            {
                uint8_t reply[PPP_MRU_DEFAULT];
                size_t  n = dlen;
                if (n > sizeof reply) n = sizeof reply;
                memcpy(reply, data, n);
                put_be32(reply, p->our_magic);
                send_cp(p, proto, PPP_ECHO_REPLY, id, reply, n);
                p->stats.echo_replies++;
            }
            return;

        case PPP_ECHO_REPLY:
        case PPP_DISCARD_REQ:
            return;                          /* RFC 1661 §5.8/§5.9: discard */

        default:
            /*
             * RFC 1661 §5.6: an unknown Code gets a Code-Reject whose data is
             * the offending packet, truncated to fit the MRU.
             */
            p->stats.unknown_codes++;
            send_cp(p, proto, PPP_CODE_REJ, f->next_id++, pkt,
                    plen > PPP_MRU_DEFAULT - 4u ? PPP_MRU_DEFAULT - 4u : plen);
            return;
    }
}

static void frame_input(ppp_peer_t *p, const uint8_t *f, size_t n) {
    /*
     * The smallest frame that can mean anything: a one-octet compressed
     * protocol, one octet of information and a two-octet FCS.
     */
    if (n < 4u) { p->stats.short_frames++; return; }

    /* RFC 1662 §3.1: the receiver runs the FCS over the frame INCLUDING the
     * received FCS, and a good frame leaves exactly 0xF0B8 in the register. */
    if (ppp_fcs16(PPP_FCS_INIT, f, n) != PPP_FCS_GOOD) {
        p->stats.fcs_errors++;
        return;
    }
    n -= 2u;                                  /* the FCS is not payload */

    /* Address and control, present unless the peer negotiated ACCOMP — and
     * accepted in either form regardless of what was negotiated, because RFC
     * 1662 §3.1 requires a receiver to handle both. */
    if (n >= 2u && f[0] == PPP_ALLSTATIONS && f[1] == PPP_UI) { f += 2; n -= 2; }
    if (n < 1u) { p->stats.short_frames++; return; }

    /*
     * RFC 1661 §2: protocol field values have an odd least-significant octet
     * and an even most-significant one, which is exactly what makes
     * Protocol-Field-Compression unambiguous — an odd first octet IS the whole
     * (compressed) protocol.
     */
    uint16_t proto;
    if (f[0] & 1u) { proto = f[0]; f += 1; n -= 1; }
    else {
        if (n < 2u) { p->stats.short_frames++; return; }
        proto = (uint16_t)(((uint16_t)f[0] << 8) | f[1]);
        f += 2; n -= 2;
    }

    p->stats.frames_in++;

    switch (proto) {
        case PPP_PROTO_LCP:  cp_input(p, PPP_PROTO_LCP,  f, n); return;
        case PPP_PROTO_IPCP: cp_input(p, PPP_PROTO_IPCP, f, n); return;
        case PPP_PROTO_IP:
            p->stats.ip_frames_in++;
            p->stats.ip_bytes_in += n;
            /*
             * RFC 1332 §2: Network-Layer packets belong to the NCP's Opened
             * state and nowhere else. Before that — and with no sink installed
             * at all, which is the N2 configuration this file shipped with —
             * the datagram is counted separately and dropped, so a report can
             * always tell "nobody was listening" from "we routed it".
             */
            if (p->ip_sink && p->ipcp.state == PPP_S_OPENED)
                p->ip_sink(p->ip_ctx, f, n);
            else
                p->stats.ip_frames_dropped++;
            return;
        default:
            p->stats.unknown_protos++;
            /* RFC 1661 §5.7: a Protocol-Reject is only valid in the LCP Opened
             * state; before that the frame is silently discarded. Its data is
             * the rejected protocol followed by the frame's information. */
            if (p->lcp.state == PPP_S_OPENED) {
                uint8_t rej[PPP_MRU_DEFAULT];
                size_t  m = n;
                if (m > sizeof rej - 2u) m = sizeof rej - 2u;
                rej[0] = (uint8_t)(proto >> 8);
                rej[1] = (uint8_t)(proto & 0xffu);
                memcpy(rej + 2, f, m);
                send_cp(p, PPP_PROTO_LCP, PPP_PROTO_REJ, p->lcp.next_id++,
                        rej, m + 2u);
            }
            return;
    }
}

/* ======================================================== the deframer === */

void ppp_input_byte(ppp_peer_t *p, uint8_t byte) {
    if (!p) return;
    p->stats.rx_bytes++;

    if (byte == PPP_FLAG) {
        if (p->escaped) {
            /* RFC 1662 §4.2: a Control Escape immediately followed by a Flag
             * aborts the frame. */
            p->stats.aborts++;
        } else if (p->in_frame && p->rxlen) {
            if (p->overlong) p->stats.long_frames++;
            else             frame_input(p, p->rxbuf, p->rxlen);
        }
        /* A flag both ends a frame and starts the next, and a run of flags is
         * legal inter-frame fill (RFC 1662 §4.1) — hence no "empty frame"
         * error above, only silence. */
        p->rxlen = 0; p->escaped = false; p->overlong = false;
        p->in_frame = true;
        return;
    }

    if (!p->in_frame) return;      /* octets before the first flag are noise */

    if (byte == PPP_ESCAPE) { p->escaped = true; return; }
    if (p->escaped) { byte = (uint8_t)(byte ^ PPP_TRANS); p->escaped = false; }

    if (p->rxlen >= PPP_MAX_FRAME) {
        /* Over the MRU. Keep consuming to the closing flag rather than
         * resynchronising mid-frame — a truncated frame handed upward would
         * fail its FCS and be reported as a line error, which is a different
         * and misleading diagnosis. */
        p->overlong = true;
        return;
    }
    p->rxbuf[p->rxlen++] = byte;
}

void ppp_input(ppp_peer_t *p, const uint8_t *bytes, size_t n) {
    if (!p || !bytes) return;
    for (size_t i = 0; i < n; i++) ppp_input_byte(p, bytes[i]);
}

/* ============================================================== timers === */

static void restart_timeout(ppp_peer_t *p, uint16_t proto) {
    ppp_fsm_t *f = fsm_for(p, proto);

    if (f->restarts == 0u) {
        /* TO-: the restart counter is exhausted. "tlf" — This-Layer-Finished. */
        restart_stop(f);
        switch (f->state) {
            case PPP_S_CLOSING:  f->state = PPP_S_CLOSED;  break;
            case PPP_S_STOPPING:
            case PPP_S_REQSENT:
            case PPP_S_ACKRCVD:
            case PPP_S_ACKSENT:  f->state = PPP_S_STOPPED; break;
            default: break;
        }
        return;
    }

    /* TO+: retransmit, keeping the identifier (RFC 1661 §5.1). */
    switch (f->state) {
        case PPP_S_CLOSING:
        case PPP_S_STOPPING:
            send_cp(p, proto, PPP_TERM_REQ, f->last_req_id, NULL, 0);
            f->restarts--;
            restart_start(p, f);
            break;
        case PPP_S_REQSENT:
        case PPP_S_ACKRCVD:
        case PPP_S_ACKSENT:
            scr(p, proto, false);
            break;
        default:
            restart_stop(f);
            break;
    }
}

void ppp_tick(ppp_peer_t *p, uint32_t now_ms) {
    if (!p) return;
    /* Monotonic. A caller that hands back an earlier time is confused, and
     * rescheduling every timer into the past would turn that into a storm. */
    if (now_ms < p->now_ms) return;
    p->now_ms = now_ms;

    if (p->lcp.timer_running && now_ms >= p->lcp.restart_at_ms)
        restart_timeout(p, PPP_PROTO_LCP);
    if (p->ipcp.timer_running && now_ms >= p->ipcp.restart_at_ms)
        restart_timeout(p, PPP_PROTO_IPCP);
}

/* ================================================================= API === */

void ppp_config_default(ppp_config_t *cfg) {
    if (!cfg) return;
    /* docs/networking.md §8.2: guest 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3.
     * §9.1 restates the guest's address for the Route B/C bootstrap, which is
     * why it is one number in one place rather than three. */
    cfg->local_ip  = 0x0a000202u;   /* 10.0.2.2  — us                       */
    cfg->remote_ip = 0x0a00020fu;   /* 10.0.2.15 — the guest                */
    cfg->dns1      = 0x0a000203u;   /* 10.0.2.3                             */
    cfg->dns2      = 0x0a000203u;
    /*
     * A fixed magic number, not a random one. This peer has no entropy source
     * (it has no platform at all), and a magic number's only job is loopback
     * detection: RFC 1661 §6.4's rule is that the two ends must DIFFER, and
     * pppd picks its own randomly — run80's was 0x7961f51c. A constant here is
     * detectable-as-a-loop for exactly one guest magic number out of 2^32, and
     * the collision path is implemented and tested anyway.
     */
    cfg->magic     = 0x5335384cu;   /* "S58L" */
}

void ppp_init(ppp_peer_t *p, const ppp_config_t *cfg) {
    if (!p) return;
    memset(p, 0, sizeof *p);
    if (cfg) p->cfg = *cfg;
    else     ppp_config_default(&p->cfg);

    p->our_magic = p->cfg.magic;
    p->tx_accm   = 0xffffffffu;      /* see asymmetry 1 in the file header */
    p->peer_mru  = PPP_MRU_DEFAULT;  /* RFC 1661 §6.1's default            */
    p->peer_asyncmap = 0xffffffffu;  /* RFC 1662 §7.1's default            */
    p->lcp.state  = PPP_S_INITIAL;
    p->ipcp.state = PPP_S_INITIAL;
    p->lcp.next_id = 1u;
    p->ipcp.next_id = 1u;
}

void ppp_set_ip_sink(ppp_peer_t *p, ppp_ip_sink_fn fn, void *ctx) {
    if (!p) return;
    p->ip_sink = fn;
    p->ip_ctx  = ctx;
}

bool ppp_send_ip(ppp_peer_t *p, const uint8_t *pkt, size_t n) {
    if (!p || !pkt || !n) return false;
    if (p->ipcp.state != PPP_S_OPENED) { p->stats.tx_overflows++; return false; }
    /* The guest told us its MRU (or accepted the 1500 default), and RFC 1661
     * §6.1 makes that a promise about what it can receive, not a suggestion. */
    if (n > p->peer_mru) { p->stats.tx_overflows++; return false; }

    uint64_t before = p->stats.frames_out;
    tx_frame(p, PPP_PROTO_IP, pkt, n);
    if (p->stats.frames_out == before) return false;   /* ring was full */
    p->stats.ip_frames_out++;
    p->stats.ip_bytes_out += n;
    return true;
}

void ppp_open(ppp_peer_t *p) {
    if (!p || p->opened) return;
    p->opened = true;
    p->lcp.req_opts = PPP_REQ_LCP_MAGIC | PPP_REQ_LCP_ASYNCMAP;
    irc(&p->lcp, PPP_MAX_CONFIGURE);
    p->lcp.state = PPP_S_REQSENT;
    scr(p, PPP_PROTO_LCP, true);
}

void ppp_close(ppp_peer_t *p) {
    if (!p) return;
    p->opened = false;
    if (p->lcp.state == PPP_S_OPENED) tld(p, PPP_PROTO_LCP);
    if (p->lcp.state == PPP_S_INITIAL || p->lcp.state == PPP_S_CLOSED) {
        p->lcp.state = PPP_S_CLOSED;
        return;
    }
    irc(&p->lcp, PPP_MAX_TERMINATE);
    p->lcp.state = PPP_S_CLOSING;
    str_(p, PPP_PROTO_LCP);
}

bool ppp_lcp_open(const ppp_peer_t *p) {
    return p && p->lcp.state == PPP_S_OPENED;
}
bool ppp_ipcp_open(const ppp_peer_t *p) {
    return p && p->ipcp.state == PPP_S_OPENED;
}

ppp_phase_t ppp_phase(const ppp_peer_t *p) {
    if (!p) return PPP_PHASE_DEAD;
    if (p->lcp.state == PPP_S_CLOSING || p->lcp.state == PPP_S_STOPPING)
        return PPP_PHASE_TERMINATE;
    if (p->ipcp.state == PPP_S_OPENED) return PPP_PHASE_OPEN;
    if (p->lcp.state  == PPP_S_OPENED) return PPP_PHASE_NETWORK;
    if (p->lcp.state  == PPP_S_INITIAL || p->lcp.state == PPP_S_CLOSED)
        return PPP_PHASE_DEAD;
    return PPP_PHASE_ESTABLISH;
}

const char *ppp_state_name(ppp_state_t s) {
    switch (s) {
        case PPP_S_INITIAL:  return "Initial";
        case PPP_S_STARTING: return "Starting";
        case PPP_S_CLOSED:   return "Closed";
        case PPP_S_STOPPED:  return "Stopped";
        case PPP_S_CLOSING:  return "Closing";
        case PPP_S_STOPPING: return "Stopping";
        case PPP_S_REQSENT:  return "Req-Sent";
        case PPP_S_ACKRCVD:  return "Ack-Rcvd";
        case PPP_S_ACKSENT:  return "Ack-Sent";
        case PPP_S_OPENED:   return "Opened";
        default:             return "?";
    }
}

const char *ppp_phase_name(ppp_phase_t ph) {
    switch (ph) {
        case PPP_PHASE_DEAD:      return "dead";
        case PPP_PHASE_ESTABLISH: return "establish (LCP negotiating)";
        case PPP_PHASE_NETWORK:   return "network (LCP up, IPCP negotiating)";
        case PPP_PHASE_OPEN:      return "open (the guest has an address)";
        case PPP_PHASE_TERMINATE: return "terminating";
        default:                  return "?";
    }
}
