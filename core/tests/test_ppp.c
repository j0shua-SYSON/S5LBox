/*
 * S5LBox — the host PPP peer, tested against the bytes a real guest really
 * sent.
 *
 * THE CENTREPIECE IS NOT A FIXTURE. run80 booted iPhone OS 3.1.3, launchd
 * started Apple's own /usr/sbin/pppd against /dev/tty.debug, and the emulator's
 * uart4 tee captured exactly 47 bytes before the guest gave up waiting for an
 * answer. Those 47 bytes are transcribed below as RUN80_CAPTURE, and
 * test_run80_capture_matches_the_recorded_file() checks the transcription
 * against work/run80-ppp-tty/uart4-ppp.bin whenever that file is present —
 * /work/ is git-ignored, so a public clone has the transcription and not the
 * file, and the suite has to be able to run either way.
 *
 * Everything the peer does with that frame is therefore checked against
 * evidence rather than against a frame this test invented. Where a behaviour
 * has NO such evidence — the Maximum-Receive-Unit option, which run80's pppd
 * never sent; the DNS options, which need `usepeerdns`; the whole of IPCP,
 * which the guest never reached — the test says so in the case's own comment
 * and the claim stops at "conforms to the RFC", never at "matches the guest".
 *
 * THE TEST OWNS ITS OWN FRAMER AND DEFRAMER. tframe_encode() and
 * tframe_decode() below are written from RFC 1662 §4.2 independently of
 * core/src/net/ppp.c, so the round-trip cases really do cross the boundary:
 * the module's framer is checked by the test's deframer and vice versa. A test
 * that framed with the code under test would agree with any consistent bug.
 *
 * The one thing they share is ppp_fcs16(), and that is deliberate and safe:
 * the FCS is pinned independently against 0x1951, the value the REAL pppd
 * computed over the REAL frame, so a wrong CRC cannot pass by agreeing with
 * itself.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "ppp.h"
#include <stdio.h>
#include <stdlib.h>
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

/* ==========================================================================
 * The evidence.
 * ======================================================================== */

/*
 * work/run80-ppp-tty/uart4-ppp.bin, all 47 octets, in the order the guest
 * stored them to uart4's UTXH. Decoded:
 *
 *   7E                       flag                          RFC 1662 §4.1
 *   FF                       all-stations address          RFC 1662 §4.2
 *   7D 23  -> 03            UI control, escaped
 *   C0 21                    protocol 0xC021, LCP           RFC 1661 §2
 *   7D 21  -> 01            code 1, Configure-Request      RFC 1661 §5.1
 *   7D 21  -> 01            identifier 1
 *   7D 20 7D 34 -> 00 14    length 20
 *   7D 22 7D 26 7D 20 7D 20 7D 20 7D 20
 *          -> 02 06 00 00 00 00   ASYNCMAP 0x00000000       RFC 1662 §7.1
 *   7D 25 7D 26 79 61 F5 7D 3C
 *          -> 05 06 79 61 F5 1C   Magic-Number 0x7961F51C   RFC 1661 §6.4
 *   7D 27 7D 22 -> 07 02    Protocol-Field-Compression      RFC 1661 §6.5
 *   7D 28 7D 22 -> 08 02    Address-and-Control-Field-Comp  RFC 1661 §6.6
 *   51 7D 39 -> 51 19       FCS 0x1951, low octet first     RFC 1662 §3.1
 *   7E                       flag
 *
 * FOUR options. NOT five: there is no Maximum-Receive-Unit option, because
 * pppd omits an option whose value it is happy to leave at the default. Anyone
 * who writes "MRU 1500, ASYNCMAP, Magic, PCOMP, ACCOMP" from memory is
 * describing pppd's man page rather than this capture.
 */
static const uint8_t RUN80_CAPTURE[47] = {
    0x7e, 0xff, 0x7d, 0x23, 0xc0, 0x21, 0x7d, 0x21,
    0x7d, 0x21, 0x7d, 0x20, 0x7d, 0x34, 0x7d, 0x22,
    0x7d, 0x26, 0x7d, 0x20, 0x7d, 0x20, 0x7d, 0x20,
    0x7d, 0x20, 0x7d, 0x25, 0x7d, 0x26, 0x79, 0x61,
    0xf5, 0x7d, 0x3c, 0x7d, 0x27, 0x7d, 0x22, 0x7d,
    0x28, 0x7d, 0x22, 0x51, 0x7d, 0x39, 0x7e
};

/* The same frame with the escapes removed: what the FCS is computed over, and
 * what the deframer must reconstruct. 24 octets. */
static const uint8_t RUN80_UNESCAPED[24] = {
    0xff, 0x03, 0xc0, 0x21,                     /* address, control, LCP     */
    0x01, 0x01, 0x00, 0x14,                     /* Conf-Req, id 1, length 20 */
    0x02, 0x06, 0x00, 0x00, 0x00, 0x00,         /* ASYNCMAP 0                */
    0x05, 0x06, 0x79, 0x61, 0xf5, 0x1c,         /* Magic 0x7961F51C          */
    0x07, 0x02,                                 /* PCOMP                     */
    0x08, 0x02                                  /* ACCOMP                    */
    /* FCS 0x1951 follows on the wire and is checked separately. */
};

/* The 16 option octets, which a correct Configure-Ack echoes verbatim. */
static const uint8_t RUN80_OPTIONS[16] = {
    0x02, 0x06, 0x00, 0x00, 0x00, 0x00,
    0x05, 0x06, 0x79, 0x61, 0xf5, 0x1c,
    0x07, 0x02,
    0x08, 0x02
};

#define RUN80_MAGIC 0x7961f51cu
#define RUN80_FCS   0x1951u

/* ==========================================================================
 * An independent HDLC-async framer and deframer, RFC 1662 §4.2.
 * ======================================================================== */

typedef struct {
    uint16_t proto;
    uint8_t  info[1600];
    size_t   len;
    bool     fcs_ok;
} tframe_t;

/* Build one frame the way a peer that escapes everything would. Always emits
 * the uncompressed address/control pair and a two-octet protocol, which is what
 * pppd sends before it has negotiated ACCOMP or PCOMP. */
static size_t tframe_encode(uint16_t proto, const uint8_t *info, size_t n,
                            uint8_t *out) {
    uint8_t raw[1700];
    size_t  rn = 0;
    raw[rn++] = 0xff; raw[rn++] = 0x03;
    raw[rn++] = (uint8_t)(proto >> 8); raw[rn++] = (uint8_t)(proto & 0xffu);
    memcpy(raw + rn, info, n); rn += n;

    uint16_t fcs = (uint16_t)(ppp_fcs16(0xffffu, raw, rn) ^ 0xffffu);
    raw[rn++] = (uint8_t)(fcs & 0xffu);
    raw[rn++] = (uint8_t)(fcs >> 8);

    size_t at = 0;
    out[at++] = 0x7e;
    for (size_t i = 0; i < rn; i++) {
        uint8_t c = raw[i];
        if (c == 0x7e || c == 0x7d || c < 0x20u) {
            out[at++] = 0x7d;
            out[at++] = (uint8_t)(c ^ 0x20u);
        } else {
            out[at++] = c;
        }
    }
    out[at++] = 0x7e;
    return at;
}

/*
 * Pull every complete frame out of a byte stream. Returns how many there were,
 * NOT how many fit in `out` — an earlier draft capped the return at `max`, and
 * because "discard whatever is queued" is spelled drain(p, NULL, 0), every
 * assertion of the form "the peer sent nothing" was silently unfalsifiable.
 * Counting first and storing second is what makes those cases mean something.
 */
static size_t tframe_decode(const uint8_t *in, size_t n,
                            tframe_t *out, size_t max) {
    uint8_t  acc[1700];
    size_t   an = 0, got = 0;
    bool     inf = false, esc = false;

    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (c == 0x7e) {
            if (inf && an >= 4u) {
              if (out && got < max) {
                tframe_t *f = &out[got];
                memset(f, 0, sizeof *f);
                f->fcs_ok = ppp_fcs16(0xffffu, acc, an) == 0xf0b8u;
                size_t body = an - 2u;
                size_t at = 0;
                if (body >= 2u && acc[0] == 0xffu && acc[1] == 0x03u) at = 2;
                if (body - at >= 1u) {
                    if (acc[at] & 1u) { f->proto = acc[at]; at += 1; }
                    else if (body - at >= 2u) {
                        f->proto = (uint16_t)((acc[at] << 8) | acc[at + 1]);
                        at += 2;
                    }
                }
                f->len = body - at;
                if (f->len > sizeof f->info) f->len = sizeof f->info;
                memcpy(f->info, acc + at, f->len);
              }
              got++;
            }
            an = 0; esc = false; inf = true;
            continue;
        }
        if (!inf) continue;
        if (c == 0x7d) { esc = true; continue; }
        if (esc) { c = (uint8_t)(c ^ 0x20u); esc = false; }
        if (an < sizeof acc) acc[an++] = c;
    }
    return got;
}

/* Everything the peer has queued, decoded. */
static size_t drain(ppp_peer_t *p, tframe_t *out, size_t max) {
    uint8_t buf[8192];
    size_t  n = ppp_output(p, buf, sizeof buf);
    return tframe_decode(buf, n, out, max);
}

/* Send one frame to the peer as the guest would. */
static void guest_send(ppp_peer_t *p, uint16_t proto,
                       const uint8_t *info, size_t n) {
    uint8_t wire[4096];
    size_t  w = tframe_encode(proto, info, n, wire);
    ppp_input(p, wire, w);
}

/* Build a Control Protocol packet: code, id, length, data. */
static size_t cp(uint8_t *out, uint8_t code, uint8_t id,
                 const uint8_t *data, size_t n) {
    out[0] = code; out[1] = id;
    out[2] = (uint8_t)((n + 4u) >> 8);
    out[3] = (uint8_t)((n + 4u) & 0xffu);
    if (n) memcpy(out + 4, data, n);
    return n + 4u;
}

/* ==========================================================================
 * 1. The FCS, pinned against the real frame.
 * ======================================================================== */

static void test_fcs16_matches_what_the_real_pppd_computed(void) {
    /*
     * pppd computed 0x1951 over these exact 24 octets on a real ARM11 in a real
     * boot. That number is the only thing in this file that could not have been
     * derived from our own implementation, so it is the anchor: if ppp_fcs16()
     * is wrong in any way, this fails, and every other FCS assertion in the
     * suite becomes trustworthy because this one holds.
     */
    uint16_t fcs = ppp_fcs16(PPP_FCS_INIT, RUN80_UNESCAPED,
                             sizeof RUN80_UNESCAPED);
    fcs = (uint16_t)(fcs ^ 0xffffu);
    CHECK(fcs == RUN80_FCS, "FCS over run80's frame = 0x%04x, pppd said 0x%04x",
          fcs, RUN80_FCS);

    /* And the receive-side identity: running the FCS over the frame INCLUDING
     * its own FCS leaves RFC 1662 §3.1's magic residue. */
    uint8_t whole[sizeof RUN80_UNESCAPED + 2u];
    memcpy(whole, RUN80_UNESCAPED, sizeof RUN80_UNESCAPED);
    whole[sizeof RUN80_UNESCAPED]      = (uint8_t)(RUN80_FCS & 0xffu);
    whole[sizeof RUN80_UNESCAPED + 1u] = (uint8_t)(RUN80_FCS >> 8);
    CHECK(ppp_fcs16(PPP_FCS_INIT, whole, sizeof whole) == PPP_FCS_GOOD,
          "the good-FCS residue is 0x%04x, not 0xf0b8",
          ppp_fcs16(PPP_FCS_INIT, whole, sizeof whole));

    /* A single flipped bit anywhere must break it. Checked over every bit of
     * the frame rather than one chosen byte, because a CRC that is only right
     * for some positions is the classic transcription failure. */
    unsigned survived = 0;
    for (size_t i = 0; i < sizeof whole; i++)
        for (unsigned b = 0; b < 8u; b++) {
            uint8_t bad[sizeof whole];
            memcpy(bad, whole, sizeof whole);
            bad[i] ^= (uint8_t)(1u << b);
            if (ppp_fcs16(PPP_FCS_INIT, bad, sizeof bad) == PPP_FCS_GOOD)
                survived++;
        }
    CHECK(survived == 0u,
          "%u single-bit corruptions of run80's frame still passed the FCS",
          survived);
}

/* ==========================================================================
 * 2. The transcription, against the file itself.
 * ======================================================================== */

static void test_run80_capture_matches_the_recorded_file(void) {
#ifdef S5LBOX_RUN80_PPP
    FILE *f = fopen(S5LBOX_RUN80_PPP, "rb");
    if (!f) {
        /* /work/ is git-ignored: a public clone legitimately has no capture.
         * The transcription above is then the evidence, and every other case in
         * this file still runs against it. */
        printf("  note: %s not present; replaying the transcription only\n",
               S5LBOX_RUN80_PPP);
        return;
    }
    uint8_t got[128];
    size_t  n = fread(got, 1, sizeof got, f);
    (void)fclose(f);
    CHECK(n == sizeof RUN80_CAPTURE,
          "the recorded capture is %zu bytes, the transcription is %zu",
          n, sizeof RUN80_CAPTURE);
    CHECK(n == sizeof RUN80_CAPTURE &&
          memcmp(got, RUN80_CAPTURE, sizeof RUN80_CAPTURE) == 0,
          "the transcription in this file is not the recorded bytes");
#else
    printf("  note: built without S5LBOX_RUN80_PPP; transcription unchecked\n");
#endif
}

/* ==========================================================================
 * 3. Replaying it.
 * ======================================================================== */

static void test_run80_deframes_to_one_configure_request(void) {
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);
    (void)drain(&p, NULL, 0);        /* discard our own Configure-Request */

    ppp_input(&p, RUN80_CAPTURE, sizeof RUN80_CAPTURE);

    CHECK(p.stats.rx_bytes == sizeof RUN80_CAPTURE,
          "the peer counted %llu received octets, not 47",
          (unsigned long long)p.stats.rx_bytes);
    CHECK(p.stats.frames_in == 1u,
          "47 octets produced %llu frames, expected exactly 1",
          (unsigned long long)p.stats.frames_in);
    CHECK(p.stats.fcs_errors == 0u, "a valid frame was reported as an FCS error");
    CHECK(p.stats.short_frames == 0u, "a 24-octet frame was called short");
    CHECK(p.stats.long_frames == 0u, "a 24-octet frame was called long");
    CHECK(p.stats.aborts == 0u, "a well-formed frame was reported as aborted");
    CHECK(p.stats.unknown_protos == 0u, "0xC021 was not recognised as LCP");
    CHECK(p.stats.unknown_codes == 0u, "code 1 was not recognised");

    /* Every option, individually, because "it parsed" is not the claim. */
    CHECK(p.peer_asyncmap == 0x00000000u,
          "ASYNCMAP parsed as 0x%08x, the capture says 0x00000000",
          p.peer_asyncmap);
    CHECK(p.peer_magic_valid, "the Magic-Number option was not recorded");
    CHECK(p.peer_magic == RUN80_MAGIC,
          "Magic-Number parsed as 0x%08x, the capture says 0x%08x",
          p.peer_magic, RUN80_MAGIC);
    CHECK(p.peer_pcomp, "Protocol-Field-Compression was not recorded");
    CHECK(p.peer_accomp,
          "Address-and-Control-Field-Compression was not recorded");
    /* And the one that is NOT there: pppd sent no MRU option, so the peer must
     * still be sitting on RFC 1661 §6.1's default rather than on something it
     * imagined it read. */
    CHECK(p.peer_mru == PPP_MRU_DEFAULT,
          "MRU is %u after a request that carried no MRU option", p.peer_mru);
}

static void test_run80_is_answered_with_a_correct_configure_ack(void) {
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);
    (void)drain(&p, NULL, 0);

    ppp_input(&p, RUN80_CAPTURE, sizeof RUN80_CAPTURE);

    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    CHECK(n == 1u, "the peer answered run80's request with %zu frames, not 1", n);
    if (n != 1u) return;

    CHECK(f[0].fcs_ok, "the peer's answer failed its own FCS");
    CHECK(f[0].proto == PPP_PROTO_LCP,
          "the answer's protocol is 0x%04x, not 0xC021", f[0].proto);
    CHECK(f[0].len == 20u, "the answer is %zu octets, not 20", f[0].len);
    if (f[0].len < 4u) return;

    CHECK(f[0].info[0] == PPP_CONF_ACK,
          "the answer's code is %u, not 2 (Configure-Ack)", f[0].info[0]);
    /* RFC 1661 §5.2: the identifier is COPIED from the request. An answer with
     * a fresh identifier is a new request, and pppd would ignore it. */
    CHECK(f[0].info[1] == 0x01u,
          "the answer's identifier is %u, not the request's 1", f[0].info[1]);
    CHECK(f[0].info[2] == 0x00u && f[0].info[3] == 0x14u,
          "the answer's Length is 0x%02x%02x, not 0x0014",
          f[0].info[2], f[0].info[3]);
    /*
     * RFC 1661 §5.2: "The options field ... is unchanged". Not a subset, not
     * reordered, not re-encoded — the same 16 octets. This is the assertion
     * that says the peer really understood the request rather than merely
     * matching its shape.
     */
    CHECK(f[0].len == 20u &&
          memcmp(f[0].info + 4, RUN80_OPTIONS, sizeof RUN80_OPTIONS) == 0,
          "the Configure-Ack did not echo the request's options verbatim");

    /* And the negotiation moved: a Configure-Ack sent from Req-Sent is
     * Ack-Sent (RFC 1661 §4.1), not Opened. The link is NOT up yet — the guest
     * has still not acknowledged our own request — and a peer that claimed
     * otherwise would be fabricating the milestone. */
    CHECK(p.lcp.state == PPP_S_ACKSENT,
          "LCP is %s after sending a Configure-Ack, expected Ack-Sent",
          ppp_state_name(p.lcp.state));
    CHECK(!ppp_lcp_open(&p), "the peer claimed LCP was open after one Ack");
    CHECK(ppp_phase(&p) == PPP_PHASE_ESTABLISH,
          "the phase is %s, expected establish", ppp_phase_name(ppp_phase(&p)));
}

static void test_run80_with_a_broken_fcs_is_refused(void) {
    /*
     * Corrupt one octet of the information field and leave everything else —
     * length, framing, escaping — exactly as the guest sent it. The frame is
     * structurally perfect and only the FCS says otherwise, which is the case a
     * peer that skipped the check would sail straight through.
     */
    for (size_t i = 6; i < sizeof RUN80_CAPTURE - 1u; i++) {
        if (RUN80_CAPTURE[i] == 0x7du) continue;   /* keep the escaping valid */
        uint8_t bad[sizeof RUN80_CAPTURE];
        memcpy(bad, RUN80_CAPTURE, sizeof bad);
        bad[i] ^= 0x40u;                            /* stays out of 0x7D/0x7E */

        ppp_peer_t p;
        ppp_init(&p, NULL);
        ppp_open(&p);
        (void)drain(&p, NULL, 0);
        ppp_input(&p, bad, sizeof bad);

        tframe_t f[4];
        size_t   n = drain(&p, f, 4);
        CHECK(p.stats.fcs_errors == 1u && p.stats.frames_in == 0u,
              "corrupting octet %zu gave fcs_errors=%llu frames_in=%llu",
              i, (unsigned long long)p.stats.fcs_errors,
              (unsigned long long)p.stats.frames_in);
        CHECK(n == 0u,
              "the peer answered a frame with a bad FCS (%zu frames)", n);
    }
}

static void test_run80_truncated_is_refused(void) {
    /* Cut the closing flag off: the frame never completes, so nothing is
     * delivered and nothing is an error yet — it is still arriving. */
    {
        ppp_peer_t p;
        ppp_init(&p, NULL);
        ppp_open(&p);
        (void)drain(&p, NULL, 0);
        ppp_input(&p, RUN80_CAPTURE, sizeof RUN80_CAPTURE - 1u);
        CHECK(p.stats.frames_in == 0u,
              "a frame with no closing flag was delivered");
        CHECK(p.stats.fcs_errors == 0u,
              "an incomplete frame was blamed on the FCS");
        CHECK(drain(&p, NULL, 0) == 0u,
              "the peer answered a frame it had not finished receiving");
    }
    /* Cut octets out of the middle and close it properly: now it IS complete,
     * and it must fail on the FCS rather than be parsed short. */
    for (size_t cut = 1; cut <= 20u; cut++) {
        uint8_t bad[sizeof RUN80_CAPTURE];
        size_t  n = sizeof RUN80_CAPTURE - 1u - cut;
        memcpy(bad, RUN80_CAPTURE, n);
        bad[n++] = 0x7e;
        /* An odd truncation can strip a 0x7D and leave a dangling escape; the
         * peer must call that an abort, not a frame. */
        ppp_peer_t p;
        ppp_init(&p, NULL);
        ppp_open(&p);
        (void)drain(&p, NULL, 0);
        ppp_input(&p, bad, n);
        CHECK(p.stats.frames_in == 0u,
              "truncating %zu octets still produced a delivered frame", cut);
        CHECK(p.stats.fcs_errors + p.stats.aborts + p.stats.short_frames == 1u,
              "truncating %zu octets was not reported exactly once "
              "(fcs=%llu abort=%llu short=%llu)", cut,
              (unsigned long long)p.stats.fcs_errors,
              (unsigned long long)p.stats.aborts,
              (unsigned long long)p.stats.short_frames);
        CHECK(drain(&p, NULL, 0) == 0u,
              "the peer answered a truncated frame");
    }
}

static void test_a_closed_peer_answers_with_a_terminate_ack(void) {
    /*
     * RFC 1661 §4.1, Closed + RCR: "sta". Nobody issued the Open event, so this
     * peer must NOT negotiate — and it must not stay silent either, because a
     * silent line leaves pppd retrying for its full Max-Configure. This is the
     * behaviour that makes ppp_open() meaningful rather than decorative.
     */
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_input(&p, RUN80_CAPTURE, sizeof RUN80_CAPTURE);

    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    CHECK(n == 1u, "a closed peer sent %zu frames, expected 1", n);
    if (n != 1u) return;
    CHECK(f[0].proto == PPP_PROTO_LCP && f[0].info[0] == PPP_TERM_ACK,
          "a closed peer answered with code %u, expected 6 (Terminate-Ack)",
          f[0].info[0]);
    CHECK(f[0].info[1] == 0x01u,
          "the Terminate-Ack did not copy the request's identifier");
    CHECK(!p.peer_magic_valid,
          "a closed peer recorded the request's options anyway");
    CHECK(ppp_phase(&p) == PPP_PHASE_DEAD,
          "a closed peer reports phase %s", ppp_phase_name(ppp_phase(&p)));
}

/* ==========================================================================
 * 4. Our own request, and a full negotiation.
 * ======================================================================== */

static void test_our_configure_request_is_well_formed(void) {
    ppp_peer_t p;
    ppp_init(&p, NULL);
    CHECK(ppp_output_pending(&p) == 0u,
          "a peer sent something before the Open event");

    ppp_open(&p);
    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    CHECK(n == 1u, "ppp_open() queued %zu frames, expected 1", n);
    if (n != 1u) return;

    CHECK(f[0].fcs_ok, "our Configure-Request failed an independent FCS check");
    CHECK(f[0].proto == PPP_PROTO_LCP,
          "our request went out as protocol 0x%04x", f[0].proto);
    CHECK(f[0].len >= 4u && f[0].info[0] == PPP_CONF_REQ,
          "our first frame is code %u, not 1", f[0].info[0]);
    CHECK(f[0].info[1] != 0u,
          "the identifier is 0; RFC 1661 §5.1 wants it changed per request");
    size_t plen = ((size_t)f[0].info[2] << 8) | f[0].info[3];
    CHECK(plen == f[0].len,
          "Length says %zu but the frame carries %zu octets", plen, f[0].len);

    /* Walk the options and require exactly the two the peer documents: an
     * ASYNCMAP of 0 and a Magic-Number. Not MRU (we take the default), and
     * emphatically not PCOMP or ACCOMP — requesting those would commit our own
     * transmit path to a compression we do not implement. */
    bool saw_asyncmap = false, saw_magic = false, saw_other = false;
    for (size_t i = 4; i + 2u <= f[0].len; ) {
        uint8_t t = f[0].info[i], l = f[0].info[i + 1];
        if (l < 2u || i + l > f[0].len) { saw_other = true; break; }
        if (t == LCP_OPT_ASYNCMAP && l == 6u) {
            saw_asyncmap = true;
            CHECK(f[0].info[i+2] == 0 && f[0].info[i+3] == 0 &&
                  f[0].info[i+4] == 0 && f[0].info[i+5] == 0,
                  "we asked for a non-zero ASYNCMAP");
        } else if (t == LCP_OPT_MAGIC && l == 6u) {
            saw_magic = true;
        } else {
            saw_other = true;
        }
        i += l;
    }
    CHECK(saw_asyncmap, "our Configure-Request carries no ASYNCMAP option");
    CHECK(saw_magic, "our Configure-Request carries no Magic-Number option");
    CHECK(!saw_other,
          "our Configure-Request carries an option this peer does not document");

    /* A retransmission must reuse the identifier (RFC 1661 §5.1); a new one
     * would make the guest's Ack unmatchable and the link would never come up. */
    uint8_t first_id = f[0].info[1];
    ppp_tick(&p, PPP_RESTART_MS);
    n = drain(&p, f, 4);
    CHECK(n == 1u, "the restart timer produced %zu frames, expected 1", n);
    if (n == 1u)
        CHECK(f[0].info[1] == first_id,
              "the retransmission used identifier %u, not the original %u",
              f[0].info[1], first_id);
}

/* Drive LCP all the way to Opened using run80's real request as the guest's
 * half. Returns the peer left in that state. */
static void bring_lcp_up(ppp_peer_t *p) {
    tframe_t f[4];
    ppp_init(p, NULL);
    ppp_open(p);

    size_t n = drain(p, f, 4);           /* our Configure-Request        */
    uint8_t our_id = (n == 1u) ? f[0].info[1] : 0u;

    ppp_input(p, RUN80_CAPTURE, sizeof RUN80_CAPTURE);   /* theirs        */
    (void)drain(p, f, 4);                                /* our Ack       */

    uint8_t pkt[64];
    size_t  m = cp(pkt, PPP_CONF_ACK, our_id, NULL, 0);
    /* Echo our own options back, which is what a real Ack carries; the peer
     * must accept it on the identifier alone. */
    guest_send(p, PPP_PROTO_LCP, pkt, m);
}

static void test_a_full_lcp_negotiation_converges(void) {
    ppp_peer_t p;
    bring_lcp_up(&p);

    CHECK(ppp_lcp_open(&p), "LCP did not reach Opened: state is %s",
          ppp_state_name(p.lcp.state));
    CHECK(p.lcp.state == PPP_S_OPENED, "LCP state is %s",
          ppp_state_name(p.lcp.state));
    CHECK(!p.lcp.timer_running,
          "the restart timer is still running in Opened; RFC 1661 §4.1 has no "
          "TO event there and it would retransmit an acknowledged request");
    CHECK(ppp_phase(&p) == PPP_PHASE_NETWORK,
          "phase is %s, expected network", ppp_phase_name(ppp_phase(&p)));
    /* LCP up is NOT an address. The guest has no IP until IPCP finishes, and
     * conflating the two is the exact fabrication this suite exists to stop. */
    CHECK(!ppp_ipcp_open(&p), "IPCP claimed to be open the moment LCP came up");
    CHECK(!p.assigned, "an address was recorded before IPCP ran");

    /* RFC 1661 §3.2: LCP Opened enters the Network-Layer Protocol phase, and
     * this peer's whole reason for existing is to start IPCP there. */
    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    CHECK(n == 1u, "LCP came up and the peer sent %zu frames, expected an "
                   "IPCP Configure-Request", n);
    if (n != 1u) return;
    CHECK(f[0].proto == PPP_PROTO_IPCP,
          "the frame after LCP came up is protocol 0x%04x, not 0x8021",
          f[0].proto);
    CHECK(f[0].info[0] == PPP_CONF_REQ,
          "the first IPCP frame is code %u, not a Configure-Request",
          f[0].info[0]);
    /* RFC 1332 §3.3: we name our own address, 10.0.2.2. */
    CHECK(f[0].len == 10u && f[0].info[4] == IPCP_OPT_ADDRESS &&
          f[0].info[5] == 6u &&
          f[0].info[6] == 10u && f[0].info[7] == 0u &&
          f[0].info[8] == 2u  && f[0].info[9] == 2u,
          "our IPCP request does not carry IP-Address 10.0.2.2");
}

/* ==========================================================================
 * 5. IPCP: the milestone.
 * ======================================================================== */

static void test_ipcp_assigns_10_0_2_15(void) {
    /*
     * UNTESTED AGAINST THE GUEST. run80's pppd never got an answer, so it never
     * reached IPCP and there is no capture of this exchange. Everything here is
     * RFC 1332 §3.2/§3.3 plus what pppd's documented defaults imply, and the
     * claim it supports is "this peer conforms", not "this is what 3.1.3 does".
     */
    ppp_peer_t p;
    bring_lcp_up(&p);

    tframe_t f[4];
    size_t   n = drain(&p, f, 4);              /* our IPCP request */
    uint8_t our_ipcp_id = (n == 1u) ? f[0].info[1] : 0u;

    /* The guest asks for Van Jacobson compression and an unspecified address,
     * which is what pppd sends by default. */
    static const uint8_t req1[] = {
        IPCP_OPT_COMPRESS, 0x06, 0x00, 0x2d, 0x0f, 0x01,
        IPCP_OPT_ADDRESS,  0x06, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t pkt[64];
    guest_send(&p, PPP_PROTO_IPCP, pkt, cp(pkt, PPP_CONF_REQ, 1, req1,
                                           sizeof req1));
    n = drain(&p, f, 4);
    CHECK(n == 1u, "the peer answered the first IPCP request with %zu frames", n);
    if (n != 1u) return;
    /*
     * RFC 1661 §5.4 and RFC 1332 §3.2: a Reject beats a Nak in the same packet,
     * and it carries ONLY the rejected option. Van Jacobson compression is
     * rejected rather than Nak'd because there is no value of it we would
     * accept — a compressed header we could not decode is a silently corrupted
     * packet.
     */
    CHECK(f[0].info[0] == PPP_CONF_REJ,
          "VJ compression got code %u, expected 4 (Configure-Reject)",
          f[0].info[0]);
    CHECK(f[0].len == 10u && f[0].info[4] == IPCP_OPT_COMPRESS,
          "the Reject does not carry exactly the compression option "
          "(%zu octets, first type %u)", f[0].len, f[0].info[4]);

    /* The guest drops it and asks again for an address only. */
    static const uint8_t req2[] = {
        IPCP_OPT_ADDRESS, 0x06, 0x00, 0x00, 0x00, 0x00
    };
    guest_send(&p, PPP_PROTO_IPCP, pkt, cp(pkt, PPP_CONF_REQ, 2, req2,
                                           sizeof req2));
    n = drain(&p, f, 4);
    CHECK(n == 1u, "the peer answered the second IPCP request with %zu frames", n);
    if (n != 1u) return;
    CHECK(f[0].info[0] == PPP_CONF_NAK,
          "0.0.0.0 got code %u, expected 3 (Configure-Nak)", f[0].info[0]);
    CHECK(f[0].info[1] == 2u, "the Nak did not copy identifier 2");
    /* THE MILESTONE, in four octets. */
    CHECK(f[0].len == 10u && f[0].info[4] == IPCP_OPT_ADDRESS &&
          f[0].info[5] == 6u &&
          f[0].info[6] == 10u && f[0].info[7] == 0u &&
          f[0].info[8] == 2u  && f[0].info[9] == 15u,
          "the Nak proposed %u.%u.%u.%u, not 10.0.2.15",
          f[0].info[6], f[0].info[7], f[0].info[8], f[0].info[9]);

    /* The guest takes the suggestion. */
    static const uint8_t req3[] = {
        IPCP_OPT_ADDRESS, 0x06, 10, 0, 2, 15
    };
    guest_send(&p, PPP_PROTO_IPCP, pkt, cp(pkt, PPP_CONF_REQ, 3, req3,
                                           sizeof req3));
    n = drain(&p, f, 4);
    CHECK(n == 1u, "the peer answered the third IPCP request with %zu frames", n);
    if (n != 1u) return;
    CHECK(f[0].info[0] == PPP_CONF_ACK,
          "10.0.2.15 got code %u, expected 2 (Configure-Ack)", f[0].info[0]);
    CHECK(f[0].len == 10u &&
          memcmp(f[0].info + 4, req3, sizeof req3) == 0,
          "the IPCP Ack did not echo the option verbatim");

    /* Still not open: our own request has not been acknowledged. */
    CHECK(!ppp_ipcp_open(&p),
          "IPCP claimed open before the guest acknowledged our request");

    guest_send(&p, PPP_PROTO_IPCP, pkt,
               cp(pkt, PPP_CONF_ACK, our_ipcp_id, NULL, 0));

    CHECK(ppp_ipcp_open(&p), "IPCP did not reach Opened: state is %s",
          ppp_state_name(p.ipcp.state));
    CHECK(p.assigned, "IPCP opened without recording an assignment");
    CHECK(p.assigned_ip == 0x0a00020fu,
          "the assigned address is 0x%08x, not 10.0.2.15 (0x0a00020f)",
          p.assigned_ip);
    CHECK(ppp_phase(&p) == PPP_PHASE_OPEN,
          "phase is %s, expected open", ppp_phase_name(ppp_phase(&p)));
    CHECK(!p.ipcp.timer_running, "the IPCP restart timer runs in Opened");
    CHECK(p.stats.tx_overflows == 0u,
          "%llu frames were dropped by the transmit ring during a negotiation "
          "that should fit several times over",
          (unsigned long long)p.stats.tx_overflows);
}

static void test_ipcp_dns_options_are_offered_when_asked(void) {
    /* RFC 1877 §1.1/§1.2, and pppd only sends these with `usepeerdns`. UNTESTED
     * against the guest for the same reason as everything else in IPCP. */
    ppp_peer_t p;
    bring_lcp_up(&p);
    (void)drain(&p, NULL, 0);

    static const uint8_t req[] = {
        IPCP_OPT_ADDRESS, 0x06, 10, 0, 2, 15,
        IPCP_OPT_DNS1,    0x06, 0,  0, 0, 0,
        IPCP_OPT_DNS2,    0x06, 0,  0, 0, 0
    };
    uint8_t pkt[64];
    guest_send(&p, PPP_PROTO_IPCP, pkt, cp(pkt, PPP_CONF_REQ, 7, req,
                                           sizeof req));
    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    CHECK(n == 1u, "the DNS request got %zu answers", n);
    if (n != 1u) return;
    CHECK(f[0].info[0] == PPP_CONF_NAK,
          "unset DNS servers got code %u, expected a Nak", f[0].info[0]);
    /* The acceptable option (the address) must NOT appear in the Nak: RFC 1661
     * §5.3 says a Nak carries only what is being corrected. */
    CHECK(f[0].len == 16u,
          "the Nak is %zu octets; it should carry exactly the two DNS options",
          f[0].len);
    CHECK(f[0].info[4] == IPCP_OPT_DNS1 && f[0].info[10] == IPCP_OPT_DNS2,
          "the Nak's options are %u and %u, not 129 and 131",
          f[0].info[4], f[0].info[10]);
    CHECK(f[0].info[6] == 10u && f[0].info[7] == 0u &&
          f[0].info[8] == 2u && f[0].info[9] == 3u,
          "the primary DNS offered is %u.%u.%u.%u, not 10.0.2.3",
          f[0].info[6], f[0].info[7], f[0].info[8], f[0].info[9]);
}

/* ==========================================================================
 * 6. Option negotiation, the awkward cases.
 * ======================================================================== */

static void test_an_unknown_option_is_rejected_not_acked(void) {
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);
    (void)drain(&p, NULL, 0);

    /* A recognised option (ASYNCMAP), an option nobody has heard of, and
     * authentication — which IS recognised and is still a Reject, because there
     * is no value of it this peer would accept. RFC 1661 §5.4. */
    static const uint8_t opts[] = {
        LCP_OPT_ASYNCMAP, 0x06, 0x00, 0x00, 0x00, 0x00,
        200,              0x04, 0xde, 0xad,
        LCP_OPT_AUTH,     0x04, 0xc0, 0x23           /* PAP */
    };
    uint8_t pkt[64];
    guest_send(&p, PPP_PROTO_LCP, pkt, cp(pkt, PPP_CONF_REQ, 9, opts,
                                          sizeof opts));

    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    CHECK(n == 1u, "an unknown option produced %zu answers", n);
    if (n != 1u) return;
    CHECK(f[0].info[0] == PPP_CONF_REJ,
          "an unknown option got code %u, expected 4 (Configure-Reject)",
          f[0].info[0]);
    CHECK(f[0].info[1] == 9u, "the Reject did not copy identifier 9");
    /* Exactly the two unacceptable options, and NOT the ASYNCMAP: a Reject that
     * echoed the whole request would tell pppd to stop asking for something it
     * is perfectly entitled to. */
    CHECK(f[0].len == 12u,
          "the Reject is %zu octets; expected 4 + the two bad options (8)",
          f[0].len);
    CHECK(f[0].len == 12u && f[0].info[4] == 200u && f[0].info[8] == LCP_OPT_AUTH,
          "the Reject does not carry exactly options 200 and 3");
}

static void test_a_magic_number_collision_is_naked(void) {
    /* RFC 1661 §6.4: identical magic numbers mean the line may be looped back,
     * and the correct answer is a Nak proposing a different one — never an Ack,
     * which would leave both ends unable to detect the loop at all. */
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);
    (void)drain(&p, NULL, 0);

    uint8_t opts[6] = { LCP_OPT_MAGIC, 0x06, 0, 0, 0, 0 };
    opts[2] = (uint8_t)(p.our_magic >> 24); opts[3] = (uint8_t)(p.our_magic >> 16);
    opts[4] = (uint8_t)(p.our_magic >> 8);  opts[5] = (uint8_t)(p.our_magic);

    uint8_t pkt[64];
    guest_send(&p, PPP_PROTO_LCP, pkt, cp(pkt, PPP_CONF_REQ, 3, opts,
                                          sizeof opts));
    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    CHECK(n == 1u, "a magic collision produced %zu answers", n);
    if (n != 1u) return;
    CHECK(f[0].info[0] == PPP_CONF_NAK,
          "a magic-number collision got code %u, expected 3 (Configure-Nak)",
          f[0].info[0]);
    uint32_t proposed = ((uint32_t)f[0].info[6] << 24) |
                        ((uint32_t)f[0].info[7] << 16) |
                        ((uint32_t)f[0].info[8] << 8)  | f[0].info[9];
    CHECK(f[0].len == 10u && f[0].info[4] == LCP_OPT_MAGIC &&
          proposed != p.our_magic,
          "the Nak proposed 0x%08x, which is still ours", proposed);
    CHECK(!p.peer_magic_valid,
          "a colliding magic number was recorded as the peer's");
}

static void test_a_nak_of_our_magic_is_adopted(void) {
    /* RFC 1661 §5.3: a Configure-Nak carries the value the peer WOULD accept,
     * and the next request must carry it. A peer that ignored the Nak would
     * loop until Max-Failure and the link would never come up. */
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);

    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    if (n != 1u) { CHECK(false, "no Configure-Request to Nak"); return; }
    uint8_t our_id = f[0].info[1];
    uint32_t before = p.our_magic;

    static const uint8_t sug[] = { LCP_OPT_MAGIC, 0x06, 0x12, 0x34, 0x56, 0x78 };
    uint8_t pkt[64];
    guest_send(&p, PPP_PROTO_LCP, pkt, cp(pkt, PPP_CONF_NAK, our_id, sug,
                                          sizeof sug));

    CHECK(p.our_magic == 0x12345678u,
          "our magic is 0x%08x after a Nak proposing 0x12345678 (was 0x%08x)",
          p.our_magic, before);
    n = drain(&p, f, 4);
    CHECK(n == 1u, "a Nak produced %zu new requests, expected 1", n);
    if (n != 1u) return;
    CHECK(f[0].info[0] == PPP_CONF_REQ, "the answer to a Nak is code %u, not 1",
          f[0].info[0]);
    CHECK(f[0].info[1] != our_id,
          "the new request reused identifier %u; RFC 1661 §5.1 wants a change",
          our_id);
    bool carries = false;
    for (size_t i = 4; i + 6u <= f[0].len; i += f[0].info[i + 1])
        if (f[0].info[i] == LCP_OPT_MAGIC)
            carries = f[0].info[i+2] == 0x12 && f[0].info[i+3] == 0x34 &&
                      f[0].info[i+4] == 0x56 && f[0].info[i+5] == 0x78;
    CHECK(carries, "the new request does not carry the Nak'd magic number");
}

static void test_a_reject_of_our_option_removes_it_permanently(void) {
    /* RFC 1661 §5.4: a rejected option "is not included in further
     * Configure-Requests". Not "is included with a different value" — gone. */
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);

    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    if (n != 1u) { CHECK(false, "no Configure-Request to Reject"); return; }
    uint8_t our_id = f[0].info[1];

    static const uint8_t rej[] = { LCP_OPT_ASYNCMAP, 0x06, 0, 0, 0, 0 };
    uint8_t pkt[64];
    guest_send(&p, PPP_PROTO_LCP, pkt, cp(pkt, PPP_CONF_REJ, our_id, rej,
                                          sizeof rej));

    n = drain(&p, f, 4);
    CHECK(n == 1u, "a Reject produced %zu new requests", n);
    if (n != 1u) return;
    bool still = false, magic = false;
    for (size_t i = 4; i + 2u <= f[0].len; i += f[0].info[i + 1]) {
        if (f[0].info[i] == LCP_OPT_ASYNCMAP) still = true;
        if (f[0].info[i] == LCP_OPT_MAGIC)    magic = true;
    }
    CHECK(!still, "the rejected ASYNCMAP option is still in our request");
    CHECK(magic, "rejecting ASYNCMAP also dropped the Magic-Number option");

    /* And it stays gone across a retransmission. */
    ppp_tick(&p, p.now_ms + PPP_RESTART_MS);
    n = drain(&p, f, 4);
    if (n == 1u) {
        still = false;
        for (size_t i = 4; i + 2u <= f[0].len; i += f[0].info[i + 1])
            if (f[0].info[i] == LCP_OPT_ASYNCMAP) still = true;
        CHECK(!still, "the rejected option came back on retransmission");
    }
}

static void test_an_ack_with_the_wrong_identifier_is_ignored(void) {
    /* RFC 1661 §5.2: an Ack whose identifier does not match the outstanding
     * request is invalid and MUST be silently discarded. A peer that accepted
     * it would bring the link up on the strength of a stale packet. */
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);
    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    if (n != 1u) { CHECK(false, "no request outstanding"); return; }
    uint8_t our_id = f[0].info[1];

    uint8_t pkt[64];
    guest_send(&p, PPP_PROTO_LCP, pkt,
               cp(pkt, PPP_CONF_ACK, (uint8_t)(our_id + 7u), NULL, 0));
    CHECK(p.lcp.state == PPP_S_REQSENT,
          "a mismatched Configure-Ack moved LCP to %s",
          ppp_state_name(p.lcp.state));
    CHECK(drain(&p, NULL, 0) == 0u, "a mismatched Ack provoked a reply");

    /* The right one still works, which is what makes the check above a
     * discrimination rather than a peer that ignores every Ack. */
    guest_send(&p, PPP_PROTO_LCP, pkt, cp(pkt, PPP_CONF_ACK, our_id, NULL, 0));
    CHECK(p.lcp.state == PPP_S_ACKRCVD,
          "the matching Configure-Ack left LCP in %s, expected Ack-Rcvd",
          ppp_state_name(p.lcp.state));
}

/* ==========================================================================
 * 7. Codes, protocols, echo, terminate.
 * ======================================================================== */

static void test_an_unknown_code_gets_a_code_reject(void) {
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);
    (void)drain(&p, NULL, 0);

    uint8_t pkt[64];
    size_t  m = cp(pkt, 99u, 5u, NULL, 0);      /* RFC 1661 §5.6 */
    guest_send(&p, PPP_PROTO_LCP, pkt, m);

    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    CHECK(n == 1u, "an unknown code produced %zu answers", n);
    if (n != 1u) return;
    CHECK(f[0].info[0] == PPP_CODE_REJ,
          "an unknown code got %u, expected 7 (Code-Reject)", f[0].info[0]);
    CHECK(p.stats.unknown_codes == 1u,
          "unknown_codes is %llu", (unsigned long long)p.stats.unknown_codes);
    /* The Code-Reject's data is the offending packet, so the far end can see
     * what we could not read. */
    CHECK(f[0].len >= 8u && f[0].info[4] == 99u && f[0].info[5] == 5u,
          "the Code-Reject does not carry the rejected packet");
}

static void test_an_unknown_protocol_is_rejected_only_when_lcp_is_open(void) {
    /* RFC 1661 §5.7: "Protocol-Reject packets can only be sent in the LCP
     * Opened state". Before that, silence — because an unknown protocol during
     * negotiation is far more likely to be line noise than a real request. */
    {
        ppp_peer_t p;
        ppp_init(&p, NULL);
        ppp_open(&p);
        (void)drain(&p, NULL, 0);
        uint8_t junk[4] = { 1, 2, 3, 4 };
        guest_send(&p, 0x4021u, junk, sizeof junk);   /* Cisco... something */
        CHECK(p.stats.unknown_protos == 1u,
              "an unknown protocol was not counted");
        CHECK(drain(&p, NULL, 0) == 0u,
              "a Protocol-Reject was sent before LCP was Opened");
    }
    {
        ppp_peer_t p;
        bring_lcp_up(&p);
        (void)drain(&p, NULL, 0);
        uint8_t junk[4] = { 1, 2, 3, 4 };
        guest_send(&p, 0x4021u, junk, sizeof junk);
        tframe_t f[4];
        size_t   n = drain(&p, f, 4);
        CHECK(n == 1u, "an unknown protocol in Opened produced %zu answers", n);
        if (n != 1u) return;
        CHECK(f[0].proto == PPP_PROTO_LCP && f[0].info[0] == PPP_PROTO_REJ,
              "expected an LCP Protocol-Reject, got protocol 0x%04x code %u",
              f[0].proto, f[0].info[0]);
        CHECK(f[0].info[4] == 0x40u && f[0].info[5] == 0x21u,
              "the Protocol-Reject names 0x%02x%02x, not 0x4021",
              f[0].info[4], f[0].info[5]);
    }
}

static void test_an_echo_request_is_answered_only_when_open(void) {
    {
        ppp_peer_t p;
        ppp_init(&p, NULL);
        ppp_open(&p);
        (void)drain(&p, NULL, 0);
        uint8_t data[8] = { 0, 0, 0, 0, 'p', 'i', 'n', 'g' };
        uint8_t pkt[64];
        guest_send(&p, PPP_PROTO_LCP, pkt,
                   cp(pkt, PPP_ECHO_REQ, 4, data, sizeof data));
        CHECK(drain(&p, NULL, 0) == 0u,
              "an Echo-Request was answered before LCP was Opened");
        CHECK(p.stats.echo_replies == 0u, "echo_replies moved anyway");
    }
    {
        ppp_peer_t p;
        bring_lcp_up(&p);
        (void)drain(&p, NULL, 0);
        uint8_t data[8] = { 0, 0, 0, 0, 'p', 'i', 'n', 'g' };
        uint8_t pkt[64];
        guest_send(&p, PPP_PROTO_LCP, pkt,
                   cp(pkt, PPP_ECHO_REQ, 4, data, sizeof data));
        tframe_t f[4];
        size_t   n = drain(&p, f, 4);
        CHECK(n == 1u, "an Echo-Request in Opened got %zu answers", n);
        if (n != 1u) return;
        CHECK(f[0].info[0] == PPP_ECHO_REPLY,
              "code %u, expected 10 (Echo-Reply)", f[0].info[0]);
        CHECK(f[0].info[1] == 4u, "the Echo-Reply did not copy identifier 4");
        /* RFC 1661 §5.8: the reply carries OUR magic number, then the request's
         * data verbatim. */
        uint32_t magic = ((uint32_t)f[0].info[4] << 24) |
                         ((uint32_t)f[0].info[5] << 16) |
                         ((uint32_t)f[0].info[6] << 8)  | f[0].info[7];
        CHECK(magic == p.our_magic,
              "the Echo-Reply carries 0x%08x, not our magic 0x%08x",
              magic, p.our_magic);
        CHECK(f[0].len == 12u && memcmp(f[0].info + 8, "ping", 4) == 0,
              "the Echo-Reply did not echo the request's data");
        CHECK(p.stats.echo_replies == 1u, "echo_replies is %llu",
              (unsigned long long)p.stats.echo_replies);
        CHECK(ppp_lcp_open(&p), "answering an echo took the link down");
    }
}

static void test_a_terminate_request_is_acknowledged(void) {
    ppp_peer_t p;
    bring_lcp_up(&p);
    (void)drain(&p, NULL, 0);
    CHECK(ppp_lcp_open(&p), "the link was not up to begin with");

    uint8_t pkt[64];
    guest_send(&p, PPP_PROTO_LCP, pkt, cp(pkt, PPP_TERM_REQ, 12, NULL, 0));

    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    CHECK(n == 1u, "a Terminate-Request got %zu answers", n);
    if (n == 1u) {
        CHECK(f[0].info[0] == PPP_TERM_ACK,
              "code %u, expected 6 (Terminate-Ack)", f[0].info[0]);
        CHECK(f[0].info[1] == 12u, "the Terminate-Ack did not copy identifier 12");
    }
    CHECK(!ppp_lcp_open(&p), "LCP is still Opened after a Terminate-Request");
    CHECK(!ppp_ipcp_open(&p), "IPCP survived LCP going down");
    CHECK(!p.assigned, "an address survived LCP going down");
    CHECK(ppp_phase(&p) == PPP_PHASE_TERMINATE,
          "phase is %s, expected terminating", ppp_phase_name(ppp_phase(&p)));
}

static void test_ip_frames_are_counted_and_dropped(void) {
    /*
     * N2 stops at "the link is up and the guest has an address". Routing this
     * packet is N4. What must NOT happen is the packet vanishing without trace:
     * the counter is what tells N4's first run that the guest really did try.
     */
    ppp_peer_t p;
    bring_lcp_up(&p);
    (void)drain(&p, NULL, 0);

    /* A minimal IPv4 header, enough to be recognisable and no more. */
    uint8_t ip[20] = { 0x45, 0x00, 0x00, 0x14 };
    guest_send(&p, PPP_PROTO_IP, ip, sizeof ip);

    CHECK(p.stats.ip_frames_in == 1u,
          "an IP frame was not counted (%llu)",
          (unsigned long long)p.stats.ip_frames_in);
    CHECK(p.stats.unknown_protos == 0u, "0x0021 was treated as unknown");
    CHECK(drain(&p, NULL, 0) == 0u,
          "the peer answered an IP packet; N2 does not route IP");
    CHECK(ppp_lcp_open(&p), "an IP packet took the link down");
}

/* ==========================================================================
 * 8. Framing, both directions.
 * ======================================================================== */

static void test_framing_round_trips_including_7e_and_7d(void) {
    /*
     * The two octets that cannot appear raw inside a frame, adjacent, repeated,
     * and at both ends — plus every control character, which the default
     * async-control-character map also requires escaped.
     */
    static const uint8_t nasty[] = {
        0x7e, 0x7d, 0x7d, 0x7e, 0x00, 0x01, 0x1f, 0x20,
        0x7e, 0x7e, 0x7d, 0x5e, 0x5d, 0xff, 0x03, 0x7d
    };
    uint8_t wire[256];
    size_t  w = ppp_frame(PPP_PROTO_IP, nasty, sizeof nasty, 0xffffffffu,
                          wire, sizeof wire);
    CHECK(w > 0u, "ppp_frame refused a 16-octet payload");

    /* Nothing between the flags may be a raw 0x7E. That is the property the
     * whole escape mechanism exists for, and it is checked directly rather than
     * inferred from the decode succeeding. */
    unsigned raw_flags = 0;
    for (size_t i = 1; i + 1u < w; i++) if (wire[i] == 0x7e) raw_flags++;
    CHECK(raw_flags == 0u,
          "%u raw flag octets appear inside the frame body", raw_flags);
    CHECK(wire[0] == 0x7e && wire[w - 1u] == 0x7e,
          "the frame is not delimited by flags");

    tframe_t f[2];
    size_t   n = tframe_decode(wire, w, f, 2);
    CHECK(n == 1u, "an independently written deframer found %zu frames", n);
    if (n != 1u) return;
    CHECK(f[0].fcs_ok, "the framer's FCS did not verify");
    CHECK(f[0].proto == PPP_PROTO_IP, "protocol came back as 0x%04x", f[0].proto);
    CHECK(f[0].len == sizeof nasty &&
          memcmp(f[0].info, nasty, sizeof nasty) == 0,
          "the payload did not survive the round trip");

    /* Now the other direction: every one of the 256 octet values, framed by the
     * test and deframed by the module. */
    uint8_t all[256];
    for (unsigned i = 0; i < 256u; i++) all[i] = (uint8_t)i;
    ppp_peer_t p;
    ppp_init(&p, NULL);
    bring_lcp_up(&p);
    (void)drain(&p, NULL, 0);
    guest_send(&p, PPP_PROTO_IP, all, sizeof all);
    CHECK(p.stats.ip_frames_in == 1u,
          "a 256-octet payload containing every byte value did not arrive "
          "intact (fcs errors %llu)", (unsigned long long)p.stats.fcs_errors);
    CHECK(p.stats.fcs_errors == 0u,
          "%llu FCS errors on a frame the test framed correctly",
          (unsigned long long)p.stats.fcs_errors);
}

static void test_the_transmit_escape_map_is_honoured(void) {
    /*
     * ADDED BECAUSE A MUTATION SURVIVED. Deleting the async-control-character
     * map from needs_escape() — so no octet below 0x20 was ever escaped —
     * passed every other case in this file, because both deframers un-escape
     * correctly whether or not the escape was required, and run80's pppd
     * happens to ask for a map of zero. The bug is only visible against a
     * receiver that really does eat control characters, i.e. exactly the case
     * the map exists for and the case a unit test cannot reach by round-trip.
     *
     * So this checks the WIRE FORM directly, in both directions of the map.
     */
    uint8_t payload[0x22];
    for (unsigned i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)i;

    /* RFC 1662 §7.1's default map: every octet below 0x20 escaped. */
    uint8_t wire[512];
    size_t  w = ppp_frame(PPP_PROTO_IP, payload, sizeof payload, 0xffffffffu,
                          wire, sizeof wire);
    CHECK(w > 0u, "ppp_frame refused the escape-map payload");
    unsigned raw_control = 0, escapes = 0;
    for (size_t i = 1; i + 1u < w; i++) {
        if (wire[i] == PPP_ESCAPE) { escapes++; i++; continue; }
        if (wire[i] < 0x20u) raw_control++;
    }
    CHECK(raw_control == 0u,
          "%u control octets went out unescaped under a map of 0xffffffff",
          raw_control);
    /* 0x00..0x1F in the payload, the protocol's 0x00 high octet, the 0x03
     * control field, and whatever the FCS needs — so at least 33. Counting
     * them at all is what distinguishes "escaped" from "dropped". */
    CHECK(escapes >= 33u, "only %u escape sequences for 33 control octets",
          escapes);

    /* And the other direction: a map of zero means the sender need not escape
     * them, and this one does not. A peer that always escaped would also be
     * correct on the wire, but it would not be what the code says it does, and
     * a field nobody consults is a field that will be wrong when it matters. */
    w = ppp_frame(PPP_PROTO_IP, payload, sizeof payload, 0x00000000u,
                  wire, sizeof wire);
    raw_control = 0;
    for (size_t i = 1; i + 1u < w; i++) {
        if (wire[i] == PPP_ESCAPE) { i++; continue; }
        if (wire[i] < 0x20u) raw_control++;
    }
    CHECK(raw_control > 0u,
          "a map of 0x00000000 still escaped every control octet: "
          "the map is not consulted");
    /* 0x7E and 0x7D are escaped under EVERY map (RFC 1662 §4.2) — they are not
     * negotiable, and a map of zero must not let a flag through. */
    unsigned raw_flags = 0;
    for (size_t i = 1; i + 1u < w; i++) {
        if (wire[i] == PPP_ESCAPE) { i++; continue; }
        if (wire[i] == PPP_FLAG) raw_flags++;
    }
    CHECK(raw_flags == 0u,
          "a map of 0x00000000 let %u raw flag octets into the frame",
          raw_flags);

    /*
     * Now the peer's own output, which is the case that actually reaches the
     * guest. Its Configure-Request carries a code of 1, a length of 0x0010 and
     * four zero octets of ASYNCMAP — all below 0x20 — and pppd's own capture
     * (run80) shows it escaping exactly those, so this peer must too.
     */
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);
    uint8_t out[512];
    size_t  n = ppp_output(&p, out, sizeof out);
    CHECK(n > 0u, "the peer queued nothing to inspect");
    raw_control = 0;
    for (size_t i = 1; i + 1u < n; i++) {
        if (out[i] == PPP_ESCAPE) { i++; continue; }
        if (out[i] < 0x20u) raw_control++;
    }
    CHECK(raw_control == 0u,
          "the peer sent %u unescaped control octets; run80 shows pppd "
          "escaping the same ones", raw_control);
}

static void test_the_deframer_survives_a_hostile_stream(void) {
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);
    (void)drain(&p, NULL, 0);

    /* Octets before the first flag are noise on a line that was already live;
     * a peer that treated them as the head of a frame would resynchronise on
     * garbage forever. */
    static const uint8_t noise[] = { 0x00, 0xa5, 0xff, 0x55 };
    ppp_input(&p, noise, sizeof noise);
    CHECK(p.stats.frames_in == 0u && p.stats.fcs_errors == 0u,
          "pre-flag noise was interpreted as a frame");

    /* A run of flags is legal inter-frame fill (RFC 1662 §4.1) and must produce
     * neither a frame nor an error. Then the real frame, immediately after. */
    static const uint8_t fill[] = { 0x7e, 0x7e, 0x7e, 0x7e, 0x7e };
    ppp_input(&p, fill, sizeof fill);
    CHECK(p.stats.frames_in == 0u && p.stats.short_frames == 0u,
          "inter-frame fill was reported as a frame or an error");
    ppp_input(&p, RUN80_CAPTURE, sizeof RUN80_CAPTURE);
    CHECK(p.stats.frames_in == 1u,
          "a frame following inter-frame fill was lost");

    /* An escape immediately followed by a flag aborts the frame (RFC 1662
     * §4.2). Not "is an FCS error" — abort is its own diagnosis. */
    ppp_init(&p, NULL);
    ppp_open(&p);
    static const uint8_t abort_seq[] = { 0x7e, 0xff, 0x03, 0xc0, 0x7d, 0x7e };
    ppp_input(&p, abort_seq, sizeof abort_seq);
    CHECK(p.stats.aborts == 1u, "a dangling escape was not counted as an abort");
    CHECK(p.stats.frames_in == 0u, "an aborted frame was delivered");

    /* Over the MRU: dropped and counted as long, never truncated into an FCS
     * error, which would blame the line for our own buffer. */
    ppp_init(&p, NULL);
    ppp_open(&p);
    ppp_input_byte(&p, 0x7e);
    for (unsigned i = 0; i < PPP_MAX_FRAME + 64u; i++)
        ppp_input_byte(&p, 0x41u);
    ppp_input_byte(&p, 0x7e);
    CHECK(p.stats.long_frames == 1u,
          "an over-length frame was not counted (long=%llu fcs=%llu)",
          (unsigned long long)p.stats.long_frames,
          (unsigned long long)p.stats.fcs_errors);
    CHECK(p.stats.fcs_errors == 0u,
          "an over-length frame was reported as an FCS error");
    CHECK(p.stats.frames_in == 0u, "an over-length frame was delivered");

    /* And the peer is still usable afterwards. */
    ppp_input(&p, RUN80_CAPTURE, sizeof RUN80_CAPTURE);
    CHECK(p.stats.frames_in == 1u,
          "the deframer never resynchronised after an over-length frame");
}

static void test_the_capture_split_across_every_boundary(void) {
    /*
     * A UART hands over one byte at a time and a host queue hands over whatever
     * happens to have arrived, so the deframer must not depend on a frame
     * arriving in one call. Feed run80's 47 octets split at every possible
     * point — 46 different splits, plus byte-at-a-time — and require the same
     * answer every time.
     */
    unsigned wrong = 0;
    for (size_t cut = 1; cut < sizeof RUN80_CAPTURE; cut++) {
        ppp_peer_t p;
        ppp_init(&p, NULL);
        ppp_open(&p);
        (void)drain(&p, NULL, 0);
        ppp_input(&p, RUN80_CAPTURE, cut);
        ppp_input(&p, RUN80_CAPTURE + cut, sizeof RUN80_CAPTURE - cut);
        if (p.stats.frames_in != 1u || p.peer_magic != RUN80_MAGIC ||
            drain(&p, NULL, 0) != 1u)
            wrong++;
    }
    CHECK(wrong == 0u, "%u of 46 split points changed the outcome", wrong);

    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);
    (void)drain(&p, NULL, 0);
    for (size_t i = 0; i < sizeof RUN80_CAPTURE; i++)
        ppp_input_byte(&p, RUN80_CAPTURE[i]);
    tframe_t f[2];
    CHECK(p.stats.frames_in == 1u && drain(&p, f, 2) == 1u &&
          f[0].info[0] == PPP_CONF_ACK,
          "byte-at-a-time delivery did not produce one Configure-Ack");
}

/* ==========================================================================
 * 9. Timers.
 * ======================================================================== */

static void test_the_restart_timer_retransmits_then_gives_up(void) {
    /*
     * RFC 1661 §4.6: Restart timer 3 seconds, Max-Configure 10. A peer with no
     * timer would send one request into a line where pppd had not started yet
     * and wait forever — which, given that the guest's launchd job starts
     * minutes into a boot, is exactly the situation this peer is in.
     */
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);
    size_t sent = drain(&p, NULL, 0);
    CHECK(sent == 1u, "ppp_open sent %zu requests", sent);

    unsigned retransmissions = 0;
    for (unsigned i = 1; i <= 20u; i++) {
        ppp_tick(&p, i * PPP_RESTART_MS);
        retransmissions += (unsigned)drain(&p, NULL, 0);
    }
    /* Max-Configure is 10 and the initial request consumed one, so nine more
     * go out and then the automaton stops. Pinning the exact number matters:
     * a peer that never stopped would keep a dead line busy for the whole boot,
     * and one that stopped early would give up before pppd started. */
    CHECK(retransmissions == 9u,
          "%u retransmissions, expected 9 (Max-Configure 10 less the first)",
          retransmissions);
    CHECK(p.lcp.state == PPP_S_STOPPED,
          "after Max-Configure the automaton is in %s, expected Stopped",
          ppp_state_name(p.lcp.state));
    CHECK(!p.lcp.timer_running, "the restart timer is still running in Stopped");

    /* Stopped is not dead: a Configure-Request from a guest that finally
     * started must restart the whole negotiation (RFC 1661 §4.1, Stopped+RCR
     * begins with irc). This is the case a naive "give up" would break. */
    ppp_input(&p, RUN80_CAPTURE, sizeof RUN80_CAPTURE);
    tframe_t f[4];
    size_t   n = drain(&p, f, 4);
    CHECK(n == 2u,
          "a Stopped peer answered a late Configure-Request with %zu frames, "
          "expected our own request and an Ack", n);
    if (n != 2u) return;
    CHECK(f[0].info[0] == PPP_CONF_REQ && f[1].info[0] == PPP_CONF_ACK,
          "the Stopped peer sent codes %u and %u, expected 1 then 2",
          f[0].info[0], f[1].info[0]);
    CHECK(p.lcp.state == PPP_S_ACKSENT,
          "after restarting, LCP is in %s", ppp_state_name(p.lcp.state));
}

static void test_time_only_moves_forward(void) {
    ppp_peer_t p;
    ppp_init(&p, NULL);
    ppp_open(&p);
    (void)drain(&p, NULL, 0);
    ppp_tick(&p, 1000u);
    ppp_tick(&p, 500u);            /* a confused caller */
    CHECK(p.now_ms == 1000u, "time went backwards to %u", p.now_ms);
    CHECK(drain(&p, NULL, 0) == 0u,
          "a backwards clock fired the restart timer");
}

/* ==========================================================================
 * 10. Defaults.
 * ======================================================================== */

static void test_the_default_addresses_are_the_documented_ones(void) {
    ppp_config_t c;
    memset(&c, 0xa5, sizeof c);
    ppp_config_default(&c);
    /* docs/networking.md §8.2, stated as four numbers so a change to any of
     * them is a test failure rather than a surprise in a boot log. */
    CHECK(c.local_ip  == 0x0a000202u, "gateway is 0x%08x, not 10.0.2.2",
          c.local_ip);
    CHECK(c.remote_ip == 0x0a00020fu, "guest is 0x%08x, not 10.0.2.15",
          c.remote_ip);
    CHECK(c.dns1 == 0x0a000203u, "DNS is 0x%08x, not 10.0.2.3", c.dns1);
    CHECK(c.magic != 0u, "the default magic number is zero");

    ppp_peer_t p;
    ppp_init(&p, NULL);
    CHECK(p.cfg.remote_ip == 0x0a00020fu,
          "ppp_init(NULL) did not take the documented defaults");
    CHECK(p.tx_accm == 0xffffffffu,
          "the transmit ACCM is 0x%08x; RFC 1662 §7.1's default is 0xffffffff",
          p.tx_accm);
    CHECK(p.lcp.state == PPP_S_INITIAL && p.ipcp.state == PPP_S_INITIAL,
          "a fresh peer is not in Initial");
    CHECK(ppp_phase(&p) == PPP_PHASE_DEAD, "a fresh peer is not dead");
    CHECK(ppp_output_pending(&p) == 0u, "a fresh peer has bytes to send");

    /* A caller-supplied configuration must be used verbatim: the app will want
     * a different subnet one day and finding out by boot log is not the plan. */
    ppp_config_t mine;
    ppp_config_default(&mine);
    mine.remote_ip = 0xc0a80105u;      /* 192.168.1.5 */
    ppp_init(&p, &mine);
    CHECK(p.cfg.remote_ip == 0xc0a80105u,
          "a caller-supplied address was ignored (0x%08x)", p.cfg.remote_ip);
}

/* ======================================================================== */

/* ==========================================================================
 * The seam to the NAT.
 *
 * ppp_set_ip_sink()/ppp_send_ip() are the only two calls that carry a
 * Network-Layer packet in either direction, and they are where core/src/net/
 * net.c gets bolted on. Everything below is about the ONE rule RFC 1332 §2
 * states and that is easy to lose: Network-Layer packets belong to the NCP's
 * Opened state and to nothing else.
 * ======================================================================== */

typedef struct {
    unsigned calls;
    size_t   len;
    uint8_t  last[1600];
} sink_t;

static void sink_fn(void *ctx, const uint8_t *pkt, size_t n) {
    sink_t *s = ctx;
    s->calls++;
    s->len = n;
    if (n <= sizeof s->last) memcpy(s->last, pkt, n);
}

/* LCP up, then IPCP all the way to Opened, using the same exchange
 * test_ipcp_assigns_the_documented_address() walks step by step. */
static void bring_ipcp_up(ppp_peer_t *p) {
    tframe_t f[4];
    uint8_t  pkt[64];
    bring_lcp_up(p);

    size_t n = drain(p, f, 4);                 /* our IPCP Configure-Request */
    uint8_t our_id = 0u;
    for (size_t i = 0; i < n; i++)
        if (f[i].proto == PPP_PROTO_IPCP && f[i].info[0] == PPP_CONF_REQ)
            our_id = f[i].info[1];

    static const uint8_t req[] = { IPCP_OPT_ADDRESS, 0x06, 10, 0, 2, 15 };
    guest_send(p, PPP_PROTO_IPCP, pkt,
               cp(pkt, PPP_CONF_REQ, 3, req, sizeof req));
    (void)drain(p, f, 4);                      /* our Ack of its address     */
    guest_send(p, PPP_PROTO_IPCP, pkt,
               cp(pkt, PPP_CONF_ACK, our_id, NULL, 0));
}

static void test_ip_is_refused_until_ipcp_is_opened(void) {
    ppp_peer_t p;
    sink_t s;
    uint8_t ip[40];
    memset(&s, 0, sizeof s);
    memset(ip, 0, sizeof ip);
    ip[0] = 0x45u;

    /* Before anything: not even LCP. */
    ppp_init(&p, NULL);
    ppp_set_ip_sink(&p, sink_fn, &s);
    CHECK(!ppp_send_ip(&p, ip, sizeof ip),
          "a datagram was framed before the link existed");

    /* LCP up but IPCP not: still refused. This is the case that matters,
     * because the link LOOKS up and a caller could reasonably think it is. */
    bring_lcp_up(&p);
    ppp_set_ip_sink(&p, sink_fn, &s);
    CHECK(!ppp_ipcp_open(&p), "IPCP was open when only LCP had come up");
    CHECK(!ppp_send_ip(&p, ip, sizeof ip),
          "a datagram was framed with LCP up but IPCP still negotiating");

    /* And inbound: counted as dropped, never handed to the sink. */
    guest_send(&p, PPP_PROTO_IP, ip, sizeof ip);
    CHECK(s.calls == 0u,
          "an IPv4 packet reached the NAT %u time(s) before IPCP opened",
          s.calls);
    CHECK(p.stats.ip_frames_in == 1u && p.stats.ip_frames_dropped == 1u,
          "the early datagram was not counted as received-and-dropped "
          "(in %llu, dropped %llu)",
          (unsigned long long)p.stats.ip_frames_in,
          (unsigned long long)p.stats.ip_frames_dropped);
}

static void test_ip_crosses_both_ways_once_ipcp_is_open(void) {
    ppp_peer_t p;
    sink_t s;
    tframe_t f[4];
    uint8_t ip[60];
    memset(&s, 0, sizeof s);
    bring_ipcp_up(&p);
    ppp_set_ip_sink(&p, sink_fn, &s);
    CHECK(ppp_ipcp_open(&p), "IPCP did not open: state is %s",
          ppp_state_name(p.ipcp.state));
    (void)drain(&p, f, 4);

    /* Guest -> NAT. */
    for (size_t i = 0; i < sizeof ip; i++) ip[i] = (uint8_t)(i * 7u + 1u);
    ip[0] = 0x45u;
    guest_send(&p, PPP_PROTO_IP, ip, sizeof ip);
    CHECK(s.calls == 1u, "the sink was called %u times, expected once", s.calls);
    CHECK(s.len == sizeof ip && memcmp(s.last, ip, sizeof ip) == 0,
          "the datagram handed to the NAT is not the one the guest sent "
          "(%zu octets)", s.len);
    CHECK(p.stats.ip_frames_dropped == 0u,
          "a delivered datagram was also counted as dropped");
    CHECK(p.stats.ip_bytes_in == sizeof ip, "ip_bytes_in is %llu, expected %zu",
          (unsigned long long)p.stats.ip_bytes_in, sizeof ip);

    /* NAT -> guest, and it must come back out as protocol 0x0021 with an
     * intact FCS through the test's own deframer. */
    CHECK(ppp_send_ip(&p, ip, sizeof ip), "ppp_send_ip refused an open link");
    size_t n = drain(&p, f, 4);
    CHECK(n == 1u, "sending one datagram produced %zu frames", n);
    if (n == 1u) {
        CHECK(f[0].proto == PPP_PROTO_IP,
              "the frame went out as protocol 0x%04x, expected 0x0021",
              f[0].proto);
        CHECK(f[0].fcs_ok, "the outbound datagram's FCS is wrong");
        CHECK(f[0].len == sizeof ip && memcmp(f[0].info, ip, sizeof ip) == 0,
              "the framed payload is not the datagram (%zu octets)", f[0].len);
    }
    CHECK(p.stats.ip_frames_out == 1u && p.stats.ip_bytes_out == sizeof ip,
          "the outbound datagram was not counted");
}

static void test_output_capacity_prevents_destructive_backpressure(void) {
    ppp_peer_t p;
    uint8_t ip[PPP_MRU_DEFAULT];
    uint8_t encoded[PPP_TX_RING];
    bring_ipcp_up(&p);
    (void)ppp_output(&p, encoded, sizeof encoded);
    memset(ip, PPP_FLAG, sizeof ip); /* close to the maximum escaped size */
    ip[0] = 0x45u;

    const size_t worst_frame = 2u * PPP_MAX_FRAME + 2u;
    CHECK(ppp_output_capacity(&p) == PPP_TX_RING,
          "an empty ring reports %zu of %u bytes free",
          ppp_output_capacity(&p), (unsigned)PPP_TX_RING);

    unsigned queued = 0u;
    while (ppp_output_capacity(&p) >= worst_frame) {
        CHECK(ppp_send_ip(&p, ip, sizeof ip),
              "a frame failed after worst-case capacity was reserved");
        queued++;
    }
    CHECK(queued >= 3u, "only %u maximum datagrams fit in the four-frame ring",
          queued);
    CHECK(ppp_output_capacity(&p) < worst_frame,
          "the capacity guard did not stop at a bounded backpressure point");
    CHECK(p.stats.tx_overflows == 0u,
          "capacity-controlled queuing counted %llu overflow(s)",
          (unsigned long long)p.stats.tx_overflows);

    (void)ppp_output(&p, encoded, sizeof encoded);
    CHECK(ppp_output_capacity(&p) == PPP_TX_RING,
          "draining the ring restored only %zu bytes of capacity",
          ppp_output_capacity(&p));
    CHECK(ppp_output_capacity(NULL) == 0u,
          "a NULL peer reported output capacity");
}

static void test_a_datagram_past_the_peers_mru_is_refused(void) {
    ppp_peer_t p;
    uint8_t big[2048];
    bring_ipcp_up(&p);
    memset(big, 0x41, sizeof big);
    big[0] = 0x45u;

    /*
     * RFC 1661 §6.1 makes the MRU a promise about what the peer can receive,
     * not a suggestion. Framing something larger would hand the guest a
     * datagram its own driver has no buffer for, and the failure would surface
     * as a corrupt read a long way from here.
     */
    CHECK(p.peer_mru <= 1500u, "the peer MRU is %u", p.peer_mru);
    CHECK(!ppp_send_ip(&p, big, (size_t)p.peer_mru + 1u),
          "a datagram one octet past the peer's %u-octet MRU was framed",
          p.peer_mru);
    CHECK(ppp_send_ip(&p, big, p.peer_mru),
          "a datagram of exactly the peer's MRU was refused");
}

static void test_no_sink_is_a_drop_and_not_a_crash(void) {
    ppp_peer_t p;
    uint8_t ip[40];
    memset(ip, 0, sizeof ip);
    ip[0] = 0x45u;
    bring_ipcp_up(&p);
    ppp_set_ip_sink(&p, NULL, NULL);       /* the N2 configuration           */

    guest_send(&p, PPP_PROTO_IP, ip, sizeof ip);
    CHECK(p.stats.ip_frames_in == 1u && p.stats.ip_frames_dropped == 1u,
          "with no NAT installed the datagram was not counted as dropped");

    /* And the null-argument forms. */
    ppp_set_ip_sink(NULL, sink_fn, NULL);
    CHECK(!ppp_send_ip(NULL, ip, sizeof ip), "ppp_send_ip(NULL) claimed success");
    CHECK(!ppp_send_ip(&p, NULL, 4), "ppp_send_ip with no packet claimed success");
    CHECK(!ppp_send_ip(&p, ip, 0), "ppp_send_ip of nothing claimed success");
}

int main(void) {
    printf("S5LBox PPP peer tests (RFC 1661 / 1662 / 1332)\n");
    test_fcs16_matches_what_the_real_pppd_computed();
    test_run80_capture_matches_the_recorded_file();
    test_run80_deframes_to_one_configure_request();
    test_run80_is_answered_with_a_correct_configure_ack();
    test_run80_with_a_broken_fcs_is_refused();
    test_run80_truncated_is_refused();
    test_a_closed_peer_answers_with_a_terminate_ack();
    test_our_configure_request_is_well_formed();
    test_a_full_lcp_negotiation_converges();
    test_ipcp_assigns_10_0_2_15();
    test_ipcp_dns_options_are_offered_when_asked();
    test_an_unknown_option_is_rejected_not_acked();
    test_a_magic_number_collision_is_naked();
    test_a_nak_of_our_magic_is_adopted();
    test_a_reject_of_our_option_removes_it_permanently();
    test_an_ack_with_the_wrong_identifier_is_ignored();
    test_an_unknown_code_gets_a_code_reject();
    test_an_unknown_protocol_is_rejected_only_when_lcp_is_open();
    test_an_echo_request_is_answered_only_when_open();
    test_a_terminate_request_is_acknowledged();
    test_ip_frames_are_counted_and_dropped();
    test_framing_round_trips_including_7e_and_7d();
    test_the_transmit_escape_map_is_honoured();
    test_the_deframer_survives_a_hostile_stream();
    test_the_capture_split_across_every_boundary();
    test_the_restart_timer_retransmits_then_gives_up();
    test_time_only_moves_forward();
    test_the_default_addresses_are_the_documented_ones();
    test_ip_is_refused_until_ipcp_is_opened();
    test_ip_crosses_both_ways_once_ipcp_is_open();
    test_output_capacity_prevents_destructive_backpressure();
    test_a_datagram_past_the_peers_mru_is_refused();
    test_no_sink_is_a_drop_and_not_a_crash();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
