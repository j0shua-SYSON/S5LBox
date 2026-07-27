/*
 * S5LBox — a PPP peer, for the far end of the guest's own /usr/sbin/pppd.
 *
 * Milestone N2 of docs/networking.md §10: HDLC-async framing (RFC 1662), LCP
 * (RFC 1661) and IPCP (RFC 1332), enough that the guest's pppd gets an answer
 * and configures an interface at 10.0.2.15 with the peer at 10.0.2.2.
 *
 * WHAT THIS MODULE IS NOT. It has no I/O, no sockets, no clock, no allocation
 * and no platform header — the whole of it is <stdint.h>, <stdbool.h>,
 * <stddef.h> and <string.h>. Bytes go in through ppp_input(), bytes come out
 * through ppp_output(), and time arrives as a number the caller chooses. That
 * is not minimalism for its own sake: it is what lets the 47 real bytes the
 * real guest really sent (work/run80-ppp-tty/uart4-ppp.bin) be replayed against
 * it in a unit test in microseconds, on a host with no phone attached.
 *
 * IT DOES NOT ROUTE ANY IP. Received IPv4 frames are counted and dropped. NAT,
 * ICMP, TCP and DNS are N0/N4/N5 and are separate work; this module stops at
 * "the link is up and the guest has an address", which is exactly where N2
 * stops.
 *
 * THREADING. None. It is a plain struct with no internal pointers, so a caller
 * that needs it on another thread copies or locks it; docs/networking.md §8.3
 * requires the host->guest handoff to happen on the CPU thread between run
 * slices, and this module is written so that is the only place it ever runs.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_PPP_H
#define S5LBOX_PPP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------ constants --- */

/* Protocol numbers, RFC 1661 §2 and the "PPP DLL Protocol Numbers" registry. */
#define PPP_PROTO_IP     0x0021u
#define PPP_PROTO_IPCP   0x8021u
#define PPP_PROTO_LCP    0xc021u

/* LCP codes, RFC 1661 §5. IPCP reuses 1..7 (RFC 1332 §2). */
#define PPP_CONF_REQ     1u
#define PPP_CONF_ACK     2u
#define PPP_CONF_NAK     3u
#define PPP_CONF_REJ     4u
#define PPP_TERM_REQ     5u
#define PPP_TERM_ACK     6u
#define PPP_CODE_REJ     7u
#define PPP_PROTO_REJ    8u
#define PPP_ECHO_REQ     9u
#define PPP_ECHO_REPLY   10u
#define PPP_DISCARD_REQ  11u

/* LCP configuration options, RFC 1661 §6 and RFC 1662 §7. */
#define LCP_OPT_MRU      1u
#define LCP_OPT_ASYNCMAP 2u
#define LCP_OPT_AUTH     3u
#define LCP_OPT_QUALITY  4u
#define LCP_OPT_MAGIC    5u
#define LCP_OPT_PCOMP    7u
#define LCP_OPT_ACCOMP   8u

/* IPCP configuration options, RFC 1332 §3 and RFC 1877 §1.1/1.2. */
#define IPCP_OPT_ADDRESSES  1u   /* deprecated by RFC 1332 §3.1              */
#define IPCP_OPT_COMPRESS   2u   /* IP-Compression-Protocol (VJ)             */
#define IPCP_OPT_ADDRESS    3u   /* IP-Address                               */
#define IPCP_OPT_DNS1       129u
#define IPCP_OPT_DNS2       131u

/* HDLC-async, RFC 1662 §4. */
#define PPP_FLAG         0x7eu
#define PPP_ESCAPE       0x7du
#define PPP_TRANS        0x20u
#define PPP_ALLSTATIONS  0xffu
#define PPP_UI           0x03u

/*
 * FCS-16, RFC 1662 §C.2. The initial value, and the residue every correct frame
 * leaves when the FCS is fed through the same register.
 */
#define PPP_FCS_INIT     0xffffu
#define PPP_FCS_GOOD     0xf0b8u

/*
 * The default MRU is 1500 (RFC 1661 §6.1) and this peer neither asks for more
 * nor accepts more: the receive assembly buffer is exactly one maximum frame,
 * so an MRU larger than this could not be honoured and is Nak'd rather than
 * accepted and then violated.
 *
 * The reassembly buffer is the information field plus the largest header a
 * frame can carry — address, control and a two-octet protocol — plus the FCS.
 */
#define PPP_MRU_DEFAULT  1500u
#define PPP_MAX_FRAME    (PPP_MRU_DEFAULT + 6u)

/*
 * The transmit ring. Worst case for one frame is every octet escaped: two
 * flags plus twice the frame. One frame's worth would be enough for lockstep
 * request/reply, but a peer that is retransmitting while the guest is also
 * asking questions can have two or three in flight, so the ring is sized for
 * four maximum frames and overflow is counted rather than silently truncating a
 * frame into an FCS error at the far end.
 */
#define PPP_TX_RING      ((2u * PPP_MAX_FRAME + 2u) * 4u)

/*
 * Restart timer and counters, RFC 1661 §4.6: Restart timer 3 seconds,
 * Max-Configure 10, Max-Terminate 2, Max-Failure 5.
 */
#define PPP_RESTART_MS   3000u
#define PPP_MAX_CONFIGURE 10u
#define PPP_MAX_TERMINATE 2u
#define PPP_MAX_FAILURE   5u

/* ---------------------------------------------------------------- state --- */

/*
 * The RFC 1661 §4.1 automaton, one per Control Protocol. The numbering is the
 * RFC's own so a reader can put the state table beside this file and check it.
 * Only the states this peer can reach are listed; the two that only exist for
 * an implementation with a lower layer that can go down independently
 * (Starting, Stopping) are present because the table names them and leaving a
 * hole in the numbering would make the comparison harder, not easier.
 */
typedef enum {
    PPP_S_INITIAL  = 0,
    PPP_S_STARTING = 1,
    PPP_S_CLOSED   = 2,
    PPP_S_STOPPED  = 3,
    PPP_S_CLOSING  = 4,
    PPP_S_STOPPING = 5,
    PPP_S_REQSENT  = 6,
    PPP_S_ACKRCVD  = 7,
    PPP_S_ACKSENT  = 8,
    PPP_S_OPENED   = 9
} ppp_state_t;

/*
 * The link as a whole, which is what a harness wants to print. This is
 * DELIBERATELY separate from the two automata: "our peer answered" and "the
 * guest accepted the answer" are different claims and a report that conflates
 * them would be fabricating a negotiated link. PPP_PHASE_NETWORK means LCP is
 * Opened — the guest acked our Configure-Request AND we acked its — and only
 * PPP_PHASE_OPEN means the guest has an address.
 */
typedef enum {
    PPP_PHASE_DEAD = 0,      /* nothing negotiated                           */
    PPP_PHASE_ESTABLISH,     /* LCP is negotiating                           */
    PPP_PHASE_NETWORK,       /* LCP Opened; IPCP is negotiating              */
    PPP_PHASE_OPEN,          /* IPCP Opened: the guest has 10.0.2.15         */
    PPP_PHASE_TERMINATE      /* a Terminate-Request has been seen or sent    */
} ppp_phase_t;

typedef struct {
    /* Addresses, host byte order. docs/networking.md §8.2's own numbers. */
    uint32_t local_ip;       /* ours, the gateway the guest routes through   */
    uint32_t remote_ip;      /* what IPCP assigns the guest                  */
    uint32_t dns1, dns2;     /* offered only if the guest asks (RFC 1877)    */
    uint32_t magic;          /* our LCP magic number; 0 disables the option  */
} ppp_config_t;

typedef struct {
    uint64_t rx_bytes, tx_bytes;
    uint64_t frames_in, frames_out;
    uint64_t fcs_errors;        /* frame arrived, FCS did not match          */
    uint64_t short_frames;      /* fewer octets than a protocol plus an FCS  */
    uint64_t long_frames;       /* over PPP_MAX_FRAME: dropped, not truncated*/
    uint64_t aborts;            /* 0x7D immediately followed by 0x7E         */
    uint64_t unknown_protos;    /* answered with a Protocol-Reject           */
    uint64_t unknown_codes;     /* answered with a Code-Reject               */
    uint64_t ip_frames_in;      /* counted and DROPPED — see the header note */
    uint64_t tx_overflows;      /* frames the transmit ring could not hold   */
    uint64_t echo_replies;      /* Echo-Requests we answered                 */
} ppp_stats_t;

/*
 * Which of OUR OWN Configuration Options are still in the request we build.
 * A Configure-Reject removes one permanently (RFC 1661 §5.4: "the option is
 * not included in further Configure-Requests"), so this shrinks and never
 * grows, and a peer that rejects everything ends up sending an empty
 * Configure-Request — which is legal and is the correct outcome.
 */
#define PPP_REQ_LCP_MAGIC     (1u << 0)
#define PPP_REQ_LCP_ASYNCMAP  (1u << 1)
#define PPP_REQ_IPCP_ADDRESS  (1u << 0)

typedef struct {
    ppp_state_t state;
    uint8_t     next_id;        /* identifier for the next request we send   */
    uint8_t     last_req_id;    /* identifier of the request outstanding     */
    uint8_t     restarts;       /* Restart counter, RFC 1661 §4.6            */
    uint8_t     failures;       /* Max-Failure counter                       */
    uint8_t     req_opts;       /* PPP_REQ_* still present in our request    */
    uint32_t    restart_at_ms;  /* when the restart timer next expires       */
    bool        timer_running;
} ppp_fsm_t;

typedef struct {
    ppp_config_t cfg;
    ppp_stats_t  stats;

    ppp_fsm_t    lcp;
    ppp_fsm_t    ipcp;
    bool         opened;        /* the administrative Open event has arrived */
    uint32_t     now_ms;

    /* Our live magic number. Starts at cfg.magic and moves if the guest Naks
     * it or if it collides with the guest's (RFC 1661 §6.4 loopback
     * detection); cfg stays exactly what the caller passed in. */
    uint32_t     our_magic;

    /* What the guest asked for and we acknowledged. */
    uint32_t     peer_magic;
    bool         peer_magic_valid;
    uint32_t     peer_asyncmap;
    uint16_t     peer_mru;
    bool         peer_pcomp, peer_accomp;
    uint32_t     assigned_ip;   /* what IPCP actually gave the guest         */
    bool         assigned;

    /*
     * The map that governs OUR transmit escaping. It is 0xffffffff and stays
     * there; see the escaping note in core/src/net/ppp.c for why escaping more
     * than strictly required is the only choice with no failure mode.
     */
    uint32_t     tx_accm;

    /* Deframer. */
    uint8_t      rxbuf[PPP_MAX_FRAME];
    uint16_t     rxlen;
    bool         in_frame;
    bool         escaped;
    bool         overlong;      /* this frame already exceeded the MRU       */

    /* Transmit ring, bytes already escaped and framed. */
    uint8_t      tx[PPP_TX_RING];
    uint32_t     tx_head, tx_tail;
} ppp_peer_t;

/* ------------------------------------------------------------------ API --- */

/*
 * Default configuration: guest 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3 —
 * docs/networking.md §8.2's own addresses, and the reason they are a function
 * rather than three defines is that a caller must be able to see the whole set
 * it is accepting.
 */
void ppp_config_default(ppp_config_t *cfg);

/*
 * Reset to Initial. `cfg` may be NULL, which selects the defaults. Safe to call
 * on a peer in any state; it forgets everything, including anything queued for
 * transmit, which is what a line that has just been re-opened should look like.
 */
void ppp_init(ppp_peer_t *p, const ppp_config_t *cfg);

/*
 * The RFC 1661 §4.2 Open event. Moves LCP from Initial to Req-Sent and queues
 * our Configure-Request. Nothing is sent before this: a peer in Closed answers
 * a Configure-Request with a Terminate-Ack (§4.1's state table), which is a
 * real behaviour rather than an omission, and it is what stops this module
 * talking to a line nobody asked it to talk to.
 */
void ppp_open(ppp_peer_t *p);

/* The Close event. Queues a Terminate-Request and moves toward Closing. */
void ppp_close(ppp_peer_t *p);

/*
 * Feed bytes the guest transmitted. Any number, at any boundary — the deframer
 * is a byte-at-a-time state machine, so a frame split across ten calls is the
 * same frame.
 */
void ppp_input(ppp_peer_t *p, const uint8_t *bytes, size_t n);
void ppp_input_byte(ppp_peer_t *p, uint8_t byte);

/*
 * Take bytes to hand to the guest. ppp_output() fills up to `cap` and returns
 * how many; ppp_output_byte() returns one byte or -1 when the ring is empty,
 * which is the shape a 16-entry UART receive FIFO wants.
 */
size_t ppp_output(ppp_peer_t *p, uint8_t *buf, size_t cap);
int    ppp_output_byte(ppp_peer_t *p);
size_t ppp_output_pending(const ppp_peer_t *p);

/*
 * Advance the peer's notion of time to `now_ms` and run whatever the restart
 * timer owes. Monotonic; going backwards is ignored rather than rescheduling
 * everything into the past. The caller chooses the clock — a harness may derive
 * it from retired instructions — because this module has none.
 */
void ppp_tick(ppp_peer_t *p, uint32_t now_ms);

/* What a report may claim, and nothing more. */
ppp_phase_t ppp_phase(const ppp_peer_t *p);
bool        ppp_lcp_open(const ppp_peer_t *p);
bool        ppp_ipcp_open(const ppp_peer_t *p);
const char *ppp_state_name(ppp_state_t s);
const char *ppp_phase_name(ppp_phase_t ph);

/*
 * FCS-16 over `n` octets, RFC 1662 §C.2, seeded with `fcs` so it can be run
 * incrementally. Exposed because a test that recomputes the FCS of the recorded
 * capture with the same function the peer validates it with proves nothing;
 * the test uses this to BUILD frames, and checks the peer's verdict on frames
 * whose FCS it has deliberately corrupted.
 */
uint16_t ppp_fcs16(uint16_t fcs, const uint8_t *data, size_t n);

/*
 * Frame one PPP payload (protocol plus information) into HDLC-async octets.
 * Returns the number written, or 0 if `cap` is too small. Exposed so a test can
 * round-trip framing against deframing without reaching into the peer.
 */
size_t ppp_frame(uint16_t proto, const uint8_t *info, size_t info_len,
                 uint32_t accm, uint8_t *out, size_t cap);

#endif /* S5LBOX_PPP_H */
