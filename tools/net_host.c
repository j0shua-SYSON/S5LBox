/*
 * S5LBox — the sockets behind core/src/net/net.c's egress table.
 *
 * Every function here is one member of net_egress_t and nothing else. The
 * policy — which flow, which port, when to give up — lives in core/; this file
 * knows only how to open a socket, move bytes and report what the host said.
 *
 * NON-BLOCKING THROUGHOUT, and that is not a preference. docs/networking.md
 * §8.3 requires the host->guest handoff to happen on the CPU thread between run
 * slices, so a call that blocked here would stall the emulated ARM. Every entry
 * point below either completes at once or reports "not yet" and is asked again
 * on the next net_tick().
 *
 * THE ONE EXCEPTION IS resolve(), and it is stated rather than hidden:
 * getaddrinfo() has no portable non-blocking form, so a DNS lookup for a name
 * that is not cached blocks the CPU thread for as long as the host's resolver
 * takes. Fixing that needs a thread and a completion queue, which is more
 * machinery than N0 justifies; it is recorded here so the first person to see a
 * multi-second hitch on a captive network knows where to look.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "net_host.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define SOCK_INVALID   INVALID_SOCKET
#  define sock_close      closesocket
#  define sock_errno()    WSAGetLastError()
#  define SOCK_EWOULDBLOCK WSAEWOULDBLOCK
#  define SOCK_EINPROGRESS WSAEWOULDBLOCK   /* Winsock reports connect() so   */
#  define SOCK_EALREADY    WSAEALREADY
#  define SOCK_EISCONN     WSAEISCONN
#  define SOCK_ECONNRESET  WSAECONNRESET
#  define SOCK_ECONNREFUSED WSAECONNREFUSED
#  define SOCK_ENETRESET   WSAENETRESET
#  define SOCK_EHOSTUNREACH WSAEHOSTUNREACH
#  define SOCK_ENETUNREACH WSAENETUNREACH
#  define SOCK_EMSGSIZE    WSAEMSGSIZE
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
   typedef int sock_t;
#  define SOCK_INVALID   (-1)
#  define sock_close      close
#  define sock_errno()    errno
#  define SOCK_EWOULDBLOCK EWOULDBLOCK
#  define SOCK_EINPROGRESS EINPROGRESS
#  define SOCK_EALREADY    EALREADY
#  define SOCK_EISCONN     EISCONN
#  define SOCK_ECONNRESET  ECONNRESET
#  define SOCK_ECONNREFUSED ECONNREFUSED
#  define SOCK_ENETRESET   ENETRESET
#  define SOCK_EHOSTUNREACH EHOSTUNREACH
#  define SOCK_ENETUNREACH ENETUNREACH
#  define SOCK_EMSGSIZE    EMSGSIZE
#endif

typedef struct {
    bool     used;
    sock_t   fd;
    unsigned proto;
    bool     connected;      /* TCP: the connect has completed               */
    bool     failed;
} host_sock_t;

struct net_host {
    host_sock_t      s[NET_HOST_MAX_SOCKETS];
    net_host_stats_t stats;
};

/* ------------------------------------------------------------- helpers --- */

/*
 * "That datagram did not get there", as opposed to "this socket is finished".
 *
 * A connected UDP socket surfaces an ICMP error from a PREVIOUS send on the
 * NEXT recv, and which code it picks is the host's business: Winsock uses
 * WSAECONNRESET for port-unreachable and WSAENETRESET for TTL-exceeded, BSD
 * uses ECONNREFUSED, and either may produce EHOSTUNREACH or ENETUNREACH from
 * an intermediate router. The first draft of this file named only one of them
 * and a CI runner promptly returned a different one -- which is the whole
 * argument for listing the family rather than the one code that happened to
 * come up locally.
 *
 * None of them is terminal for UDP, which has no connection to lose. Treating
 * any as fatal would kill a live NAT flow because a single datagram bounced.
 * For TCP every one of these IS terminal and this is not consulted.
 */
static bool bounced(int e) {
    switch (e) {
        case SOCK_ECONNRESET:
        case SOCK_ECONNREFUSED:
        case SOCK_ENETRESET:
        case SOCK_EHOSTUNREACH:
        case SOCK_ENETUNREACH:
        case SOCK_EMSGSIZE:
            return true;
        default:
            return false;
    }
}

static bool would_block(int e) {
    return e == SOCK_EWOULDBLOCK
#if !defined(_WIN32)
           || e == EAGAIN || e == EINTR
#endif
           ;
}

static bool set_nonblocking(sock_t fd) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}

/*
 * A write to a TCP peer that closed can raise SIGPIPE on Darwin and terminate
 * the whole app before send() returns an error. Linux can suppress it per send
 * with MSG_NOSIGNAL; Apple platforms expose the equivalent socket option.
 * Refuse an Apple socket if that guard cannot be installed: a dead guest flow
 * must become a counted network error, never a host-process crash.
 */
static bool suppress_sigpipe(sock_t fd) {
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
    int enabled = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                      &enabled, (socklen_t)sizeof enabled) == 0;
#else
    (void)fd;
    return true;
#endif
}

static host_sock_t *slot(net_host_t *h, int handle) {
    if (!h || handle < 0 || handle >= (int)NET_HOST_MAX_SOCKETS) return NULL;
    return h->s[handle].used ? &h->s[handle] : NULL;
}

static void note_error(net_host_t *h) {
    h->stats.errors++;
    h->stats.last_error = sock_errno();
}

/* net.c hands addresses in host byte order; the sockets API wants network. */
static void fill_sin(struct sockaddr_in *sa, uint32_t ip, uint16_t port) {
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port   = htons(port);
    sa->sin_addr.s_addr = htonl(ip);
}

/* ---------------------------------------------------------- the table ---- */

static int h_open(void *ctx, unsigned proto, uint32_t ip, uint16_t port) {
    net_host_t *h = ctx;
    if (!h) return -1;

    int handle = -1;
    for (unsigned i = 0; i < NET_HOST_MAX_SOCKETS; i++)
        if (!h->s[i].used) { handle = (int)i; break; }
    if (handle < 0) { h->stats.open_failures++; return -1; }

    int type = (proto == NET_PROTO_TCP) ? SOCK_STREAM : SOCK_DGRAM;
    sock_t fd = socket(AF_INET, type, 0);
    if (fd == SOCK_INVALID) { note_error(h); h->stats.open_failures++; return -1; }
    if (!set_nonblocking(fd) || !suppress_sigpipe(fd)) {
        note_error(h); sock_close(fd); h->stats.open_failures++; return -1;
    }

    host_sock_t *s = &h->s[handle];
    memset(s, 0, sizeof *s);
    s->used  = true;
    s->fd    = fd;
    s->proto = proto;

    struct sockaddr_in sa;
    fill_sin(&sa, ip, port);
    /*
     * A connected UDP socket, deliberately. It gives send()/recv() without an
     * address on every call, and it makes the kernel drop datagrams from
     * anywhere other than the peer this flow is for — which is the closest
     * thing to source validation available without a raw socket, and it is
     * free.
     */
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        s->connected = true;
        if (proto == NET_PROTO_TCP) h->stats.connects_ready++;
    } else {
        int e = sock_errno();
        if (e == SOCK_EINPROGRESS || e == SOCK_EALREADY || would_block(e)) {
            /* Still in flight. net.c will not answer the guest's SYN until
             * status() says READY, which is the whole reason status() exists. */
            h->stats.connects_pending++;
        } else {
            note_error(h);
            sock_close(fd);
            memset(s, 0, sizeof *s);
            h->stats.open_failures++;
            h->stats.connects_failed++;
            return -1;
        }
    }
    h->stats.opens++;
    return handle;
}

static int h_status(void *ctx, int handle) {
    net_host_t *h = ctx;
    host_sock_t *s = slot(h, handle);
    if (!s) return NET_ST_FAILED;
    if (s->failed) return NET_ST_FAILED;
    if (s->connected) return NET_ST_READY;

    /*
     * A non-blocking connect reports completion through writability, and the
     * error has to be read out of SO_ERROR rather than errno: the failure
     * happened on the network, not in this call.
     */
    fd_set w, e;
    struct timeval tv;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(s->fd, &w); FD_SET(s->fd, &e);
    tv.tv_sec = 0; tv.tv_usec = 0;
    int n = select((int)s->fd + 1, NULL, &w, &e, &tv);
    if (n <= 0) return NET_ST_PENDING;

    int soerr = 0;
#if defined(_WIN32)
    int len = (int)sizeof soerr;
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len) != 0)
        soerr = sock_errno();
#else
    socklen_t len = (socklen_t)sizeof soerr;
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0)
        soerr = sock_errno();
#endif
    if (soerr != 0) {
        s->failed = true;
        h->stats.connects_failed++;
        h->stats.last_error = soerr;
        h->stats.errors++;
        return NET_ST_FAILED;
    }
    s->connected = true;
    h->stats.connects_ready++;
    return NET_ST_READY;
}

static int h_send(void *ctx, int handle, const uint8_t *data, size_t n) {
    net_host_t *h = ctx;
    host_sock_t *s = slot(h, handle);
    if (!s || s->failed) return -1;
    if (!n) return 0;

    int flags = 0;
#if !defined(_WIN32) && defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
#endif
    int w = (int)send(s->fd, (const char *)data, (int)n, flags);
    if (w >= 0) { h->stats.bytes_out += (uint64_t)w; return w; }

    int e = sock_errno();
    if (would_block(e)) {
        /* Not an error: the socket buffer is full. net.c keeps the bytes and
         * offers them again, which is exactly the back-pressure §8.3 wants. */
        h->stats.send_wouldblock++;
        return 0;
    }
    note_error(h);
    s->failed = true;
    return -1;
}

static int h_recv(void *ctx, int handle, uint8_t *buf, size_t cap) {
    net_host_t *h = ctx;
    host_sock_t *s = slot(h, handle);
    if (!s || s->failed) return NET_EG_ERROR;
    if (!cap) return NET_EG_WOULDBLOCK;

    int r = (int)recv(s->fd, (char *)buf, (int)cap, 0);
    if (r > 0) { h->stats.bytes_in += (uint64_t)r; return r; }
    if (r == 0) {
        /*
         * TCP: the peer sent FIN. UDP: a genuinely empty datagram, which is
         * legal and is NOT an end of stream — reporting EOF for one would tear
         * down a flow that is perfectly alive.
         */
        return s->proto == NET_PROTO_TCP ? NET_EG_EOF : NET_EG_WOULDBLOCK;
    }

    int e = sock_errno();
    if (would_block(e)) { h->stats.recv_wouldblock++; return NET_EG_WOULDBLOCK; }
    /*
     * A CONNECTED UDP SOCKET REPORTS A BOUNCED DATAGRAM AS ECONNRESET, and it
     * is not a dead socket. Sending to a port nothing listens on gets an ICMP
     * Port Unreachable back, which the kernel surfaces on the NEXT recv —
     * Winsock does this reliably and BSD does it for connected sockets too.
     * Treating it as fatal would tear down a NAT flow because one datagram
     * bounced, and the guest would see a working conversation stop dead after
     * a single stray packet. UDP has no delivery guarantee; that is the whole
     * of what this means.
     *
     * For TCP the same code IS terminal: a reset is the end of that connection.
     */
    if (s->proto != NET_PROTO_TCP && bounced(e)) {
        h->stats.recv_wouldblock++;
        h->stats.last_error = e;      /* kept, so a report can name it */
        return NET_EG_WOULDBLOCK;
    }
    note_error(h);
    s->failed = true;
    return NET_EG_ERROR;
}

static void h_shutdown_tx(void *ctx, int handle) {
    net_host_t *h = ctx;
    host_sock_t *s = slot(h, handle);
    if (!s || s->proto != NET_PROTO_TCP) return;
#if defined(_WIN32)
    (void)shutdown(s->fd, SD_SEND);
#else
    (void)shutdown(s->fd, SHUT_WR);
#endif
}

static void h_close(void *ctx, int handle) {
    net_host_t *h = ctx;
    host_sock_t *s = slot(h, handle);
    if (!s) return;
    sock_close(s->fd);
    memset(s, 0, sizeof *s);
    h->stats.closes++;
}

static int h_resolve(void *ctx, const char *name, uint32_t *ip) {
    net_host_t *h = ctx;
    struct addrinfo hints, *res = NULL;
    if (!h || !name || !ip) return NET_RES_FAIL;
    h->stats.resolves++;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;         /* A records only; N0 is IPv4       */
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(name, NULL, &hints, &res);
    if (rc != 0 || !res) {
        if (res) freeaddrinfo(res);
        /*
         * NXDOMAIN and SERVFAIL are DIFFERENT answers to the guest and the
         * difference matters: a stub resolver caches "no such name" and stops
         * retrying, so reporting it for a transient failure would take a name
         * that exists out of service for the whole boot.
         */
#if defined(EAI_NONAME)
        if (rc == EAI_NONAME) { h->stats.resolve_nxdomain++; return NET_RES_NXDOMAIN; }
#endif
#if defined(EAI_NODATA) && (!defined(EAI_NONAME) || EAI_NODATA != EAI_NONAME)
        if (rc == EAI_NODATA) { h->stats.resolve_nxdomain++; return NET_RES_NXDOMAIN; }
#endif
        h->stats.resolve_failures++;
        return NET_RES_FAIL;
    }

    const struct sockaddr_in *sa = (const struct sockaddr_in *)(void *)res->ai_addr;
    *ip = ntohl(sa->sin_addr.s_addr);
    freeaddrinfo(res);
    return NET_RES_OK;
}

/* ------------------------------------------------------------ lifetime --- */

net_host_t *net_host_open(net_egress_t *eg) {
    if (!eg) return NULL;
#if defined(_WIN32)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
    }
#endif
    net_host_t *h = calloc(1, sizeof *h);
    if (!h) {
#if defined(_WIN32)
        WSACleanup();
#endif
        return NULL;
    }

    memset(eg, 0, sizeof *eg);
    eg->ctx         = h;
    eg->open        = h_open;
    eg->status      = h_status;
    eg->send        = h_send;
    eg->recv        = h_recv;
    eg->shutdown_tx = h_shutdown_tx;
    eg->close       = h_close;
    eg->resolve     = h_resolve;
    /* eg->icmp_echo stays NULL. See the header: an echo aimed past the gateway
     * is counted, never fabricated. */
    return h;
}

void net_host_close(net_host_t *h) {
    if (!h) return;
    for (unsigned i = 0; i < NET_HOST_MAX_SOCKETS; i++)
        if (h->s[i].used) { sock_close(h->s[i].fd); h->s[i].used = false; }
    free(h);
#if defined(_WIN32)
    WSACleanup();
#endif
}

const net_host_stats_t *net_host_stats(const net_host_t *h) {
    return h ? &h->stats : NULL;
}
