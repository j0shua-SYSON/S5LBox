/*
 * S5LBox — the sockets behind core/src/net/net.c's egress table.
 *
 * core/ must stay linkable into the iOS app and testable on a runner with no
 * network, so it contains no socket call at all: every byte that must really
 * leave the machine leaves through net_egress_t, and this is the only
 * implementation of it. The same split as tools/audio_capture.c and
 * tools/file_block.c.
 *
 * WHAT IT NEEDS FROM THE HOST, and the whole of it: an unprivileged
 * SOCK_STREAM, an unprivileged SOCK_DGRAM, and getaddrinfo(). No raw socket,
 * no TUN device, no new entitlement — which is docs/networking.md §8.1's whole
 * argument for NAT over bridging, and the reason this can ship inside a
 * sandboxed app.
 *
 * NOT PROVIDED: icmp_echo. §8.2 hoped for an unprivileged
 * SOCK_DGRAM/IPPROTO_ICMP; it is INFERRED to exist on Darwin and does not exist
 * at all on some hosts, so the member is left NULL and net.c counts an echo
 * aimed past the gateway rather than fabricating a reply for a machine it never
 * contacted.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_NET_HOST_H
#define S5LBOX_NET_HOST_H

#include "net.h"

/*
 * How many host sockets may exist at once. One per guest flow and no more, so
 * this matches NET_MAX_FLOWS: net.c will never ask for a handle it has no flow
 * for, and a larger table here could only hide a leak in that accounting.
 */
#define NET_HOST_MAX_SOCKETS NET_MAX_FLOWS

typedef struct net_host net_host_t;

/*
 * Start up (on Windows this is what calls WSAStartup) and fill in `eg` so it
 * can be handed to net_init(). Returns NULL if the sockets layer itself is
 * unavailable, which is a real answer on a locked-down host and not a fatal
 * one: a stack with a NULL egress still answers ICMP echo to its own address
 * and still refuses malformed input.
 */
net_host_t *net_host_open(net_egress_t *eg);

/* Close every socket still open and shut the sockets layer down. */
void net_host_close(net_host_t *h);

/* Bounded diagnostics, for the same reason every other device model has them:
 * a run that "did nothing" must be distinguishable from a run that silently
 * ate something. */
typedef struct {
    uint64_t opens, open_failures, closes;
    uint64_t connects_pending, connects_ready, connects_failed;
    uint64_t bytes_out, bytes_in;
    uint64_t send_wouldblock, recv_wouldblock;
    uint64_t resolves, resolve_failures, resolve_nxdomain;
    uint64_t errors;
    /* The most recent failing errno/WSAGetLastError, so a report can name what
     * went wrong rather than only that something did. */
    int      last_error;
} net_host_stats_t;

const net_host_stats_t *net_host_stats(const net_host_t *h);

#endif /* S5LBOX_NET_HOST_H */
