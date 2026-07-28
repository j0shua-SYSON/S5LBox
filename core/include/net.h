/*
 * S5LBox — the host-side IPv4 stack and NAT that sits above the PPP peer.
 *
 * Milestone N0 of docs/networking.md §10, and the emulator half of N4. The
 * guest's pppd has an interface at 10.0.2.15 and routes everything through
 * 10.0.2.2; this module IS 10.0.2.2. It terminates ICMP echo, answers DNS, and
 * turns guest TCP and UDP flows into ordinary unprivileged host sockets —
 * §8.1's whole argument for NAT over bridging is that it needs no raw socket,
 * no TUN device and no new entitlement.
 *
 * WHAT THIS MODULE IS NOT. Like core/src/net/ppp.c it has no I/O, no sockets,
 * no clock, no allocation and no platform header: <stdint.h>, <stdbool.h>,
 * <stddef.h> and <string.h>, and nothing else. Datagrams go in through
 * net_input(), datagrams come out through net_output(), time arrives as a
 * number the caller chooses, and every byte that must actually leave the
 * machine leaves through the net_egress_t function table below. That is what
 * lets a whole TCP handshake be driven in microseconds by a test with no
 * sockets in it, which is the property docs/networking.md §11 risk 1 asks for
 * by name: our TCP is written and tested before any guest exists, because a
 * half-correct TCP produces stalls that look like emulator bugs.
 *
 * THE EGRESS BOUNDARY IS THE POINT. core/ must stay linkable into the iOS app
 * and testable on a runner with no network, so the socket calls live in
 * tools/net_host.c the way host I/O lives in tools/audio_capture.c and
 * tools/file_block.c. Everything here is arithmetic over octets.
 *
 * SCOPE, stated once so nothing below has to keep apologising. IPv4 only.
 * Fragments are refused rather than half-reassembled (§8.2). No inbound
 * connections. ICMP echo is answered for the addresses this NAT owns; echoes
 * aimed past it need an egress that can send them, and on a host that cannot
 * they are counted, never faked. No IPv6, no TLS (that is N7), no
 * SystemConfiguration (N6).
 *
 * THREADING. None, deliberately. It is a plain struct with no internal
 * pointers other than the egress table the caller installed. docs/networking.md
 * §8.3 requires the host->guest handoff to happen on the CPU thread between run
 * slices; this module is written so that is the only place it ever runs.
 *
 * SIZE. net_stack_t carries every flow's buffers inline so there is no
 * allocator in core/. It is a few hundred kilobytes — ALLOCATE IT, do not put
 * one on the stack.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_NET_H
#define S5LBOX_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------ constants --- */

/* IANA protocol numbers, the three this NAT understands. */
#define NET_PROTO_ICMP   1u
#define NET_PROTO_TCP    6u
#define NET_PROTO_UDP    17u

/*
 * The link MTU. PPP's MRU is 1500 (RFC 1661 §6.1) and core/src/net/ppp.c
 * neither asks for more nor accepts more, so no datagram larger than this can
 * ever reach the guest.
 */
#define NET_MTU          1500u

/*
 * MSS clamped to 1460 = 1500 - 20 (IPv4) - 20 (TCP), per docs/networking.md
 * §8.2. Anything the guest asks for above this is clamped rather than honoured:
 * we would have to fragment, and §8.2 refuses fragmentation on principle.
 */
#define NET_TCP_MSS_MAX  1460u

/*
 * Per-flow buffers. tx is bytes travelling toward the guest that are not yet
 * acknowledged plus bytes not yet sent; rx is bytes the guest sent that the
 * host socket has not yet accepted. Both are the TCP receive window we
 * advertise and the back-pressure we apply, so they are the one tuning knob
 * that changes throughput.
 */
#define NET_TCP_TXBUF    4096u
#define NET_TCP_RXBUF    4096u

/*
 * How many guest flows may exist at once. A browser opens four to six per page
 * and iPhone OS 3's CFNetwork is not more parallel than that; 24 leaves room
 * for DNS and a stray connection without making the struct enormous.
 */
#define NET_MAX_FLOWS    24u

/*
 * Datagrams queued for the guest. Deep enough that a burst of segments from a
 * fast loopback server is not dropped while PPP drains at UART speed, shallow
 * enough that the struct stays a couple of hundred kilobytes.
 */
#define NET_OUT_SLOTS    32u

/*
 * Timers, in the caller's milliseconds.
 *
 * The retransmit timeout starts at one second and doubles (RFC 6298 §5 without
 * the RTT estimator — we have no useful RTT: the "network" between us and the
 * guest is a function call, and the delay that matters is the emulator's, which
 * no measurement here could predict).
 */
#define NET_TCP_RTO_MS      1000u
#define NET_TCP_RTO_MAX_MS  16000u
#define NET_TCP_RTX_MAX     8u
#define NET_TCP_TIME_WAIT_MS 2000u   /* abbreviated 2MSL; see tcp.c            */
#define NET_UDP_IDLE_MS     60000u   /* docs/networking.md §8.2's idle timer   */
#define NET_DNS_TTL         60u      /* what we put in the answers we synth    */

/* ------------------------------------------------------------- egress ----- */

/* net_egress_t::status */
#define NET_ST_PENDING    0
#define NET_ST_READY      1
#define NET_ST_FAILED   (-1)

/* net_egress_t::recv */
#define NET_EG_WOULDBLOCK  0     /* nothing available; not an error           */
#define NET_EG_EOF       (-1)    /* orderly close: the peer sent FIN          */
#define NET_EG_ERROR     (-2)    /* reset, refused, or anything else fatal    */

/* net_egress_t::resolve */
#define NET_RES_OK         0
#define NET_RES_NXDOMAIN (-1)    /* authoritatively no such name              */
#define NET_RES_FAIL     (-2)    /* transient: the guest gets SERVFAIL        */

/*
 * The one interface between this module and anything that can actually reach
 * the internet. Every member may be NULL: a stack with no egress at all still
 * answers ICMP echo to its own address and still refuses malformed input,
 * which is exactly the configuration core/tests/test_net.c runs most of its
 * cases in.
 *
 * Handles are small non-negative integers chosen by the implementation. This
 * module treats them as opaque and never does arithmetic on them.
 */
typedef struct net_egress {
    void *ctx;

    /*
     * Begin a flow toward dst_ip:dst_port. For TCP the connect may still be in
     * progress on return — that is what status() is for, and it is why this
     * module does not answer the guest's SYN until the host socket is really
     * connected. Returns a handle, or negative on failure.
     */
    int  (*open)(void *ctx, unsigned proto, uint32_t dst_ip, uint16_t dst_port);

    /* NET_ST_*. Only meaningful for TCP; a UDP handle is READY at once. */
    int  (*status)(void *ctx, int handle);

    /* Bytes accepted (may be fewer than n — the rest stays buffered here and
     * is offered again), or negative on a fatal error. */
    int  (*send)(void *ctx, int handle, const uint8_t *data, size_t n);

    /* > 0 bytes, or one of NET_EG_*. */
    int  (*recv)(void *ctx, int handle, uint8_t *buf, size_t cap);

    /* The guest sent FIN and we have flushed its data: half-close. */
    void (*shutdown_tx)(void *ctx, int handle);

    void (*close)(void *ctx, int handle);

    /*
     * Resolve one name to one IPv4 address, host byte order. Implemented with
     * the host's own getaddrinfo() rather than by forwarding port 53, because
     * that inherits the host's resolver configuration and survives a captive
     * network that blocks 53 outright (docs/networking.md §8.2).
     */
    int  (*resolve)(void *ctx, const char *name, uint32_t *ip);

    /*
     * Send one ICMP echo request past the gateway and, later, hand the reply
     * back through recv-like polling. OPTIONAL and expected to be absent: it
     * needs SOCK_DGRAM/IPPROTO_ICMP, which §8.2 records as unprivileged on
     * Darwin (INFERRED for iOS) and which does not exist at all on some hosts.
     * When it is NULL, an echo aimed past this NAT is counted in
     * stats.icmp_unsupported and nothing is sent and nothing is faked.
     */
    int  (*icmp_echo)(void *ctx, uint32_t dst_ip,
                      const uint8_t *icmp, size_t n);
} net_egress_t;

/* -------------------------------------------------------------- config --- */

typedef struct {
    uint32_t guest_ip;   /* 10.0.2.15 — the only source address we accept    */
    uint32_t local_ip;   /* 10.0.2.2  — us, the gateway                      */
    uint32_t dns_ip;     /* 10.0.2.3  — us again, wearing a resolver's hat   */
    uint32_t iss;        /* seed for our TCP initial send sequences          */
} net_config_t;

/* --------------------------------------------------------------- state --- */

/*
 * RFC 793 §3.2's state names, minus the ones an endpoint that never initiates
 * a connection cannot reach (LISTEN and SYN-SENT). The guest is always the
 * active opener; docs/networking.md §8.2 excludes inbound connections from
 * this milestone's scope.
 */
typedef enum {
    NET_TCP_CLOSED = 0,
    NET_TCP_SYN_RCVD,
    NET_TCP_ESTABLISHED,
    NET_TCP_FIN_WAIT_1,   /* we sent FIN first                               */
    NET_TCP_FIN_WAIT_2,   /* ...and it was acknowledged                      */
    NET_TCP_CLOSE_WAIT,   /* the guest sent FIN first                        */
    NET_TCP_CLOSING,      /* both sent FIN, ours unacknowledged              */
    NET_TCP_LAST_ACK,
    NET_TCP_TIME_WAIT
} net_tcp_state_t;

typedef struct {
    bool     used;
    unsigned proto;             /* NET_PROTO_TCP or NET_PROTO_UDP            */
    uint16_t guest_port;
    uint32_t dst_ip;
    uint16_t dst_port;
    int      handle;            /* egress handle, or -1                      */
    uint32_t last_ms;           /* last activity, for the idle timer         */

    /* ---- TCP only ---- */
    net_tcp_state_t state;
    uint32_t irs;               /* the guest's initial sequence number       */
    uint32_t rcv_nxt;           /* next sequence we will accept from it      */
    uint32_t snd_una;           /* oldest sequence we sent and it has not ack*/
    uint32_t snd_nxt;           /* highest sequence we have sent, plus one   */
    uint32_t snd_wnd;           /* what the guest last advertised            */
    uint16_t mss;               /* clamped to NET_TCP_MSS_MAX                */
    bool     fin_sent;          /* our FIN occupies snd_una + txlen          */
    bool     host_eof;          /* the egress reported an orderly close      */
    bool     tx_shutdown;       /* we already half-closed the host socket    */
    bool     need_ack;          /* something owes the guest an ACK           */
    bool     rto_on;            /* the retransmit/persist timer is running   */
    uint32_t rto_at;            /* when it expires                           */
    uint32_t rtx;               /* consecutive retransmissions               */
    uint32_t close_at;          /* TIME-WAIT deadline; the connect deadline
                                 * while still in SYN-RCVD, where TIME-WAIT
                                 * cannot yet be running                     */

    /* txbuf[0] is sequence snd_una. Bytes in [snd_una, snd_nxt) are in
     * flight; bytes from snd_nxt to snd_una+txlen have not been sent yet. */
    uint8_t  txbuf[NET_TCP_TXBUF]; uint32_t txlen;
    uint8_t  rxbuf[NET_TCP_RXBUF]; uint32_t rxlen;
} net_flow_t;

typedef struct {
    uint64_t ip_in, ip_out;
    uint64_t ip_bad_version;    /* not IPv4                                  */
    uint64_t ip_bad_header;     /* IHL, total length or truncation           */
    uint64_t ip_bad_checksum;   /* RFC 1071 said no                          */
    uint64_t ip_frags_refused;  /* MF or a non-zero offset — §8.2 refuses    */
    uint64_t ip_bad_source;     /* not the address IPCP assigned             */
    uint64_t proto_unsupported; /* something other than ICMP/TCP/UDP         */

    uint64_t icmp_in, icmp_echo_replies;
    uint64_t icmp_unsupported;  /* aimed past us with no egress that can go  */

    uint64_t udp_in, udp_out;
    uint64_t dns_queries, dns_answered, dns_nxdomain, dns_failed;
    uint64_t dns_malformed;     /* answered FORMERR, or too broken to answer */

    uint64_t tcp_in, tcp_out;
    uint64_t tcp_syns;          /* SYNs that created a flow                  */
    uint64_t tcp_established;   /* flows that reached ESTABLISHED            */
    uint64_t tcp_refused;       /* the host connect failed: we sent RST      */
    uint64_t tcp_resets_out;
    uint64_t tcp_resets_in;
    uint64_t tcp_retransmits;
    uint64_t tcp_out_of_order;  /* dropped and re-ACKed, never reassembled   */
    uint64_t tcp_bytes_to_host, tcp_bytes_to_guest;

    uint64_t flows_open, flows_peak, flow_table_full;
    uint64_t out_dropped;       /* the queue toward the guest was full       */
} net_stats_t;

typedef struct {
    uint16_t len;
    uint8_t  data[NET_MTU];
} net_dgram_t;

typedef struct {
    net_config_t cfg;
    net_egress_t eg;
    net_stats_t  stats;
    uint32_t     now_ms;
    uint32_t     iss_walk;      /* moves per connection, RFC 793 §3.3        */
    net_flow_t   flow[NET_MAX_FLOWS];
    net_dgram_t  out[NET_OUT_SLOTS];
    uint32_t     out_head, out_tail;
} net_stack_t;

/* ------------------------------------------------------------------ API --- */

/*
 * docs/networking.md §8.2's own numbers: guest 10.0.2.15, gateway 10.0.2.2,
 * DNS 10.0.2.3. A function rather than three defines for the same reason
 * ppp_config_default() is one — a caller must be able to see the whole set it
 * is accepting.
 */
void net_config_default(net_config_t *cfg);

/*
 * Reset to nothing in flight. `cfg` may be NULL, which selects the defaults;
 * `eg` may be NULL, which selects a stack that can answer only for itself.
 * Does NOT call eg->close on flows from a previous life — it cannot know the
 * egress is still the same one — so close the old stack before reinitialising
 * a live one. net_reset() is the call that does release them.
 */
void net_init(net_stack_t *ns, const net_config_t *cfg, const net_egress_t *eg);

/* Close every flow through the egress and return to the post-init state. */
void net_reset(net_stack_t *ns);

/*
 * One IPv4 datagram from the guest, exactly as it came off PPP: no framing, no
 * padding, `n` is the whole datagram. Anything malformed is counted and
 * dropped; nothing here can fail in a way the caller must handle.
 */
void net_input(net_stack_t *ns, const uint8_t *pkt, size_t n);

/*
 * Take the next datagram queued for the guest. Returns its length, or 0 when
 * there is none or `cap` is too small — check net_output_peek() first if the
 * difference matters, which for a caller with a bounded PPP ring it does.
 */
size_t net_output(net_stack_t *ns, uint8_t *buf, size_t cap);

/* Length of the next queued datagram, 0 if the queue is empty. */
size_t net_output_peek(const net_stack_t *ns);

/* How many datagrams are queued. */
size_t net_output_pending(const net_stack_t *ns);

/*
 * Advance time and service every flow: poll the egress, push buffered guest
 * data at it, send whatever the guest's window allows, retransmit what the
 * timer owes and expire what is idle. Monotonic; going backwards is ignored.
 *
 * This is the ONLY place the egress is polled, so a caller that never ticks
 * gets a stack that answers pings and nothing else.
 */
void net_tick(net_stack_t *ns, uint32_t now_ms);

/* ------------------------------------------------------------ reporting --- */

const char *net_tcp_state_name(net_tcp_state_t s);
size_t      net_flows_open(const net_stack_t *ns);

/* -------------------------------------------------- exposed for testing --- */

/*
 * RFC 1071's ones-complement sum, folded and complemented — the value that
 * goes in the header. Exposed because a test that checks our checksum with our
 * checksum proves nothing: core/tests/test_net.c pins it against sums computed
 * by hand from RFC 1071 §3 and against real captured headers.
 */
uint16_t net_checksum(const uint8_t *data, size_t n);

/*
 * The same sum over a TCP/UDP pseudo-header (RFC 793 §3.1) followed by the
 * segment. Exposed for the same reason, and because a harness that builds
 * packets by hand needs it to build a VALID one.
 */
uint16_t net_l4_checksum(uint32_t src, uint32_t dst, unsigned proto,
                         const uint8_t *seg, size_t n);

/*
 * Build an IPv4 header for `payload_len` octets of `proto` and return the
 * header length (always 20 — this module emits no options). Exposed so
 * tools/nettest can hand-build the packets N0 asks it to feed in without
 * duplicating the checksum.
 */
size_t net_build_ipv4(uint8_t *out, size_t cap, uint32_t src, uint32_t dst,
                      unsigned proto, uint16_t ident, size_t payload_len);

#endif /* S5LBOX_NET_H */
