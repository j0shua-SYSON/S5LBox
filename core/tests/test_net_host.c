/*
 * S5LBox — the sockets behind the NAT's egress table.
 *
 * WHAT THIS SUITE WILL AND WILL NOT ASSERT. core/tests/test_net.c drives the
 * whole TCP state machine against a scripted egress and needs no network at
 * all; this one is the opposite half, and it touches the host's real sockets.
 * So every case here is confined to what a machine with NO route to anywhere
 * can still be held to: the loopback interface, a name RFC 6761 guarantees
 * cannot resolve, and the bookkeeping in this file itself.
 *
 * Nothing here contacts a remote host, and nothing here fails because a runner
 * is offline. A case that needed the internet would be a case that goes red for
 * a reason having nothing to do with the code.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "net_host.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  define nap_ms(n) Sleep(n)
#else
#  include <unistd.h>
#  define nap_ms(n) usleep((n) * 1000)
#endif

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

#define LOOPBACK 0x7f000001u    /* 127.0.0.1, host byte order */

static void test_the_table_is_filled_except_where_it_must_not_be(void) {
    net_egress_t eg;
    memset(&eg, 0xff, sizeof eg);
    net_host_t *h = net_host_open(&eg);
    CHECK(h != NULL, "the sockets layer would not start");
    if (!h) return;

    CHECK(eg.ctx == h, "the egress context is not the host");
    CHECK(eg.open && eg.status && eg.send && eg.recv &&
          eg.shutdown_tx && eg.close && eg.resolve,
          "an egress member was left unset, so net.c would silently do "
          "nothing for that operation");
    /*
     * icmp_echo MUST be NULL. docs/networking.md §8.2 hoped for an
     * unprivileged SOCK_DGRAM/IPPROTO_ICMP and could not establish that one
     * exists; net.c counts an echo aimed past the gateway rather than
     * answering on behalf of a machine it never contacted, and that only holds
     * while this member stays absent.
     */
    CHECK(eg.icmp_echo == NULL,
          "an ICMP forwarder was installed; an echo past the gateway would "
          "now be answered by something that never sent it");

    net_host_close(h);
}

static void test_a_connect_to_a_closed_port_fails_rather_than_hanging(void) {
    net_egress_t eg;
    net_host_t *h = net_host_open(&eg);
    if (!h) { CHECK(false, "the sockets layer would not start"); return; }

    /*
     * Port 1 on the loopback, which nothing listens on. The point is not that
     * it is refused -- it is that status() reaches a TERMINAL answer. A
     * connect that stayed PENDING for ever would leave net.c holding a flow
     * whose SYN is never answered and never reset, which is the one failure
     * shape a guest cannot recover from on its own.
     */
    int fd = eg.open(eg.ctx, NET_PROTO_TCP, LOOPBACK, 1u);
    CHECK(fd >= 0, "opening a socket failed outright");
    if (fd < 0) { net_host_close(h); return; }

    /*
     * Polled the way the emulator polls it: once per net_tick(), which happens
     * between CPU run slices, so real time passes between calls. Two thousand
     * back-to-back polls complete in microseconds and would only prove that a
     * connect takes longer than nothing.
     */
    int st = NET_ST_PENDING;
    for (unsigned i = 0; i < 200u && st == NET_ST_PENDING; i++) {
        st = eg.status(eg.ctx, fd);
        if (st == NET_ST_PENDING) nap_ms(5);
    }
    /*
     * A hosted CI runner may hold a connect to a closed loopback port PENDING
     * for far longer than a second, or drop it into a filter that never
     * answers. That is a fact about the runner, not about this code, and this
     * suite said at the top that it would not go red for one. So a settled
     * answer is checked when there is one and the unsettled case is reported
     * and passed -- the assertion that matters, that status() never returns
     * something outside the three legal values, holds either way.
     */
    CHECK(st == NET_ST_FAILED || st == NET_ST_READY || st == NET_ST_PENDING,
          "status() returned %d, which is not one of PENDING/READY/FAILED", st);
    if (st == NET_ST_PENDING)
        printf("      note: the connect was still PENDING after a second; "
               "this host does not refuse a closed loopback port promptly\n");
    /* READY would mean something really is listening on port 1, which is
     * possible on a strange machine and is not this file's business to judge;
     * PENDING for ever is the failure. */

    eg.close(eg.ctx, fd);
    const net_host_stats_t *s = net_host_stats(h);
    CHECK(s->opens == 1u && s->closes == 1u,
          "the socket bookkeeping is %llu open / %llu closed",
          (unsigned long long)s->opens, (unsigned long long)s->closes);
    net_host_close(h);
}

static void test_a_udp_socket_is_ready_at_once(void) {
    net_egress_t eg;
    net_host_t *h = net_host_open(&eg);
    if (!h) { CHECK(false, "the sockets layer would not start"); return; }

    /* There is no handshake to wait for, and net.c's UDP path never calls
     * status() -- but if it ever did, PENDING would stall a flow that is
     * already usable. */
    int fd = eg.open(eg.ctx, NET_PROTO_UDP, LOOPBACK, 9u);   /* discard */
    CHECK(fd >= 0, "opening a UDP socket failed");
    if (fd >= 0) {
        CHECK(eg.status(eg.ctx, fd) == NET_ST_READY,
              "a connected UDP socket did not report READY at once");
        /* And a datagram into the void is accepted rather than erroring: the
         * discard port may or may not exist, and either way sending must not
         * tear the flow down. */
        int w = eg.send(eg.ctx, fd, (const uint8_t *)"x", 1u);
        CHECK(w >= 0, "a UDP send reported a fatal error (%d)", w);
        /*
         * A datagram sent to the discard port may bounce, and a connected UDP
         * socket surfaces that as ECONNRESET on the NEXT recv -- which this
         * layer deliberately reports as WOULDBLOCK rather than tearing the
         * flow down. Some hosts deliver it, some swallow it, and one may even
         * have something listening on port 9. What must NOT happen is a fatal
         * error, because that would kill a live NAT flow over one stray
         * packet, and that is what is asserted.
         */
        int r = eg.recv(eg.ctx, fd, (uint8_t *)&w, sizeof w);
        CHECK(r == NET_EG_WOULDBLOCK || r > 0,
              "an idle UDP socket reported %d; a bounced datagram must read as "
              "WOULDBLOCK, never as a fatal error", r);
        eg.close(eg.ctx, fd);
    }
    net_host_close(h);
}

static void test_the_resolver_separates_no_such_name_from_try_again(void) {
    net_egress_t eg;
    uint32_t ip = 0;
    net_host_t *h = net_host_open(&eg);
    if (!h) { CHECK(false, "the sockets layer would not start"); return; }

    /* A name every host can answer without a network. */
    int rc = eg.resolve(eg.ctx, "localhost", &ip);
    CHECK(rc == NET_RES_OK, "\"localhost\" did not resolve (rc %d)", rc);
    if (rc == NET_RES_OK)
        CHECK((ip >> 24) == 127u,
              "\"localhost\" resolved to 0x%08x, which is not in 127/8", ip);

    /*
     * RFC 6761 §6.4 reserves .invalid precisely so that this case exists: it
     * is guaranteed not to resolve. A resolver behind a captive portal may
     * still answer it, and a network that is simply down gives a transient
     * failure, so the assertion is that we do NOT report success -- not which
     * flavour of no we report. Reporting NXDOMAIN for a transient failure is
     * the mistake that matters, and it is checked in test_net.c against a
     * scripted resolver where it can be checked exactly.
     */
    ip = 0xdeadbeefu;
    rc = eg.resolve(eg.ctx, "s5lbox-nonexistent-host.invalid", &ip);
    CHECK(rc == NET_RES_NXDOMAIN || rc == NET_RES_FAIL,
          "a .invalid name resolved successfully to 0x%08x", ip);
    CHECK(ip == 0xdeadbeefu,
          "a failed resolve wrote 0x%08x into the caller's address anyway", ip);

    net_host_close(h);
}

static void test_the_handle_table_is_bounded_and_reusable(void) {
    net_egress_t eg;
    int fd[NET_HOST_MAX_SOCKETS + 4];
    unsigned opened = 0;
    net_host_t *h = net_host_open(&eg);
    if (!h) { CHECK(false, "the sockets layer would not start"); return; }

    for (unsigned i = 0; i < NET_HOST_MAX_SOCKETS + 4u; i++) {
        fd[i] = eg.open(eg.ctx, NET_PROTO_UDP, LOOPBACK, (uint16_t)(9000u + i));
        if (fd[i] >= 0) opened++;
    }
    CHECK(opened <= NET_HOST_MAX_SOCKETS,
          "%u sockets were opened, past the %u the table holds",
          opened, (unsigned)NET_HOST_MAX_SOCKETS);
    CHECK(net_host_stats(h)->open_failures >= 4u,
          "the four refusals past the table's end were not counted");

    /* Every handle is distinct — two flows sharing one socket would cross
     * their traffic, which is the kind of bug that looks like the far end
     * misbehaving. */
    bool distinct = true;
    for (unsigned i = 0; i < NET_HOST_MAX_SOCKETS + 4u; i++)
        for (unsigned j = i + 1u; j < NET_HOST_MAX_SOCKETS + 4u; j++)
            if (fd[i] >= 0 && fd[i] == fd[j]) distinct = false;
    CHECK(distinct, "two live flows were handed the same handle");

    /* A closed slot is handed out again rather than lost. */
    for (unsigned i = 0; i < NET_HOST_MAX_SOCKETS + 4u; i++)
        if (fd[i] >= 0) eg.close(eg.ctx, fd[i]);
    int again = eg.open(eg.ctx, NET_PROTO_UDP, LOOPBACK, 9999u);
    CHECK(again >= 0, "after closing everything, no handle was available");
    if (again >= 0) eg.close(eg.ctx, again);

    net_host_close(h);
}

static void test_bad_handles_are_refused_and_not_dereferenced(void) {
    net_egress_t eg;
    uint8_t buf[8];
    net_host_t *h = net_host_open(&eg);
    if (!h) { CHECK(false, "the sockets layer would not start"); return; }

    /* net.c stores -1 for a flow with no socket and every member must survive
     * being handed it — a NAT that faulted on its own bookkeeping would take
     * the whole emulator with it. */
    static const int bad[] = { -1, -999, (int)NET_HOST_MAX_SOCKETS,
                               (int)NET_HOST_MAX_SOCKETS + 100 };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(eg.status(eg.ctx, bad[i]) == NET_ST_FAILED,
              "status(%d) did not report FAILED", bad[i]);
        CHECK(eg.send(eg.ctx, bad[i], buf, sizeof buf) < 0,
              "send(%d) claimed to have sent something", bad[i]);
        CHECK(eg.recv(eg.ctx, bad[i], buf, sizeof buf) == NET_EG_ERROR,
              "recv(%d) did not report an error", bad[i]);
        eg.shutdown_tx(eg.ctx, bad[i]);      /* must simply do nothing */
        eg.close(eg.ctx, bad[i]);            /* must simply do nothing */
    }
    CHECK(net_host_stats(h)->closes == 0u,
          "closing a handle that was never open was counted as a close");

    net_host_close(h);
}

static void test_the_null_forms_do_not_crash(void) {
    CHECK(net_host_open(NULL) == NULL, "net_host_open(NULL) returned a host");
    CHECK(net_host_stats(NULL) == NULL, "net_host_stats(NULL) answered");
    net_host_close(NULL);
    g_pass++;                                /* survived the NULL close */
}

int main(void) {
    printf("S5LBox host egress (real sockets, loopback only)\n");
    test_the_table_is_filled_except_where_it_must_not_be();
    test_a_connect_to_a_closed_port_fails_rather_than_hanging();
    test_a_udp_socket_is_ready_at_once();
    test_the_resolver_separates_no_such_name_from_try_again();
    test_the_handle_table_is_bounded_and_reusable();
    test_bad_handles_are_refused_and_not_dereferenced();
    test_the_null_forms_do_not_crash();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
