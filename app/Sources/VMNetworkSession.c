/* See VMNetworkSession.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "VMNetworkSession.h"

#include "net.h"
#include "net_host.h"
#include "ppp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint16_t len;
    uint8_t  bytes[NET_MTU];
} vm_network_dgram_t;

struct vm_network_session {
    s5l8900_t    *machine;          /* borrowed for the attachment lifetime */
    ppp_peer_t   *peer;
    net_stack_t  *net;
    net_host_t   *host;
    bool          nat_enabled;
    bool          peer_opened;
    bool          lcp_reported;
    bool          ipcp_reported;
    uint64_t      service_calls;
    uint64_t      retired_since_open;
    uint64_t      guest_tx_bytes;
    uint64_t      guest_rx_bytes;
    uint64_t      guest_ip_queued;
    uint64_t      guest_ip_dropped;
    uint64_t      net_to_guest;
    uint64_t      net_to_guest_lost;
    vm_network_dgram_t inbound[NET_OUT_SLOTS];
    unsigned      inbound_head;
    unsigned      inbound_tail;
};

static void set_detail(char *out, size_t capacity, const char *text) {
    if (!out || !capacity) return;
    (void)snprintf(out, capacity, "%s", text ? text : "");
    out[capacity - 1u] = '\0';
}

static unsigned inbound_next(unsigned index) {
    return (index + 1u) % NET_OUT_SLOTS;
}

/* ppp.c's receive buffer is borrowed, so the handoff owns a bounded copy. */
static void queue_guest_ip(void *ctx, const uint8_t *packet, size_t length) {
    vm_network_session_t *session = (vm_network_session_t *)ctx;
    if (!session || !packet || !length || length > NET_MTU) return;

    unsigned next = inbound_next(session->inbound_tail);
    if (next == session->inbound_head) {
        session->guest_ip_dropped++;
        return;
    }
    vm_network_dgram_t *slot =
        &session->inbound[session->inbound_tail];
    slot->len = (uint16_t)length;
    memcpy(slot->bytes, packet, length);
    session->inbound_tail = next;
    session->guest_ip_queued++;
}

static void uart4_guest_tx(void *ctx, uint8_t byte) {
    vm_network_session_t *session = (vm_network_session_t *)ctx;
    if (!session || !session->peer) return;

    session->guest_tx_bytes++;
    if (!session->peer_opened) {
        session->peer_opened = true;
        session->retired_since_open = 0u;
        ppp_open(session->peer);
        (void)fprintf(stderr,
                      "[network] guest opened uart4; PPP negotiation started\n");
    }
    ppp_input_byte(session->peer, byte);
}

static uint32_t guest_milliseconds(uint64_t retired) {
    uint64_t whole = retired / (uint64_t)S5L8900_CPU_HZ;
    uint64_t rem = retired % (uint64_t)S5L8900_CPU_HZ;
    uint64_t ms = whole * UINT64_C(1000) +
                  (rem * UINT64_C(1000)) / (uint64_t)S5L8900_CPU_HZ;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static void drain_guest_ip(vm_network_session_t *session) {
    if (!session || !session->net) return;
    for (unsigned guard = 0u;
         guard < NET_OUT_SLOTS &&
         session->inbound_head != session->inbound_tail;
         guard++) {
        vm_network_dgram_t *slot =
            &session->inbound[session->inbound_head];
        net_input(session->net, slot->bytes, slot->len);
        slot->len = 0u;
        session->inbound_head = inbound_next(session->inbound_head);
    }
}

static void uart4_host_service(void *ctx, unsigned retired) {
    vm_network_session_t *session = (vm_network_session_t *)ctx;
    if (!session || !session->machine || !session->peer) return;
    session->service_calls++;
    if (!session->peer_opened) return;

    if (UINT64_MAX - session->retired_since_open < (uint64_t)retired)
        session->retired_since_open = UINT64_MAX;
    else
        session->retired_since_open += (uint64_t)retired;
    uint32_t now_ms = guest_milliseconds(session->retired_since_open);
    ppp_tick(session->peer, now_ms);

    if (session->net) {
        /* Socket work is confined to this between-slices boundary. */
        drain_guest_ip(session);
        net_tick(session->net, now_ms);
        for (unsigned guard = 0u; guard < NET_OUT_SLOTS; guard++) {
            uint8_t packet[NET_MTU];
            size_t length = net_output(session->net, packet, sizeof packet);
            if (!length) break;
            if (ppp_send_ip(session->peer, packet, length))
                session->net_to_guest++;
            else
                session->net_to_guest_lost++;
        }
    }

    if (!session->lcp_reported && ppp_lcp_open(session->peer)) {
        session->lcp_reported = true;
        (void)fprintf(stderr, "[network] PPP LCP opened\n");
    }
    if (!session->ipcp_reported && ppp_ipcp_open(session->peer)) {
        session->ipcp_reported = true;
        (void)fprintf(stderr,
                      "[network] PPP IPCP opened: guest 10.0.2.15, "
                      "gateway 10.0.2.2, DNS 10.0.2.3\n");
    }

    unsigned room = s5l_uart_rx_space(&session->machine->uart4);
    uint64_t before = session->guest_rx_bytes;
    while (room--) {
        int byte = ppp_output_byte(session->peer);
        if (byte < 0) break;
        if (!s5l_uart_rx_push(&session->machine->uart4, (uint8_t)byte)) break;
        session->guest_rx_bytes++;
    }
    if (session->guest_rx_bytes != before)
        s5l8900_tick(session->machine, 0u);
}

vm_network_session_t *vm_network_session_create(
        s5l8900_t *machine, bool nat_enabled,
        char *detail, size_t detail_capacity) {
    set_detail(detail, detail_capacity, "");
    if (!machine) {
        set_detail(detail, detail_capacity,
                   "The network peer has no machine to attach to.");
        return NULL;
    }

    vm_network_session_t *session =
        (vm_network_session_t *)calloc(1u, sizeof *session);
    if (!session) {
        set_detail(detail, detail_capacity,
                   "There is not enough memory for the network peer.");
        return NULL;
    }
    session->peer = (ppp_peer_t *)calloc(1u, sizeof *session->peer);
    if (!session->peer) {
        set_detail(detail, detail_capacity,
                   "There is not enough memory for the PPP peer.");
        vm_network_session_destroy(&session);
        return NULL;
    }
    session->machine = machine;
    session->nat_enabled = nat_enabled;
    ppp_init(session->peer, NULL);

    if (nat_enabled) {
        session->net = (net_stack_t *)calloc(1u, sizeof *session->net);
        net_egress_t egress;
        memset(&egress, 0, sizeof egress);
        session->host = session->net ? net_host_open(&egress) : NULL;
        if (!session->net || !session->host) {
            set_detail(detail, detail_capacity,
                       !session->net
                           ? "There is not enough memory for guest NAT."
                           : "The host socket layer could not start guest NAT.");
            vm_network_session_destroy(&session);
            return NULL;
        }
        net_init(session->net, NULL, &egress);
        ppp_set_ip_sink(session->peer, queue_guest_ip, session);
    }

    if (!s5l8900_set_uart4_host(machine, uart4_guest_tx,
                                uart4_host_service, session)) {
        set_detail(detail, detail_capacity,
                   "The uart4 host peer could not be attached.");
        vm_network_session_destroy(&session);
        return NULL;
    }
    /* Byte delivery remains identical; this controls only VIC line 28. */
    s5l_uart_set_rx_irq(&machine->uart4, true);
    (void)fprintf(stderr, "[network] uart4 PPP peer armed%s\n",
                  nat_enabled ? " with IPv4 NAT" : " without NAT");
    return session;
}

void vm_network_session_status(const vm_network_session_t *session,
                               vm_network_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!session) return;
    out->attached = session->machine &&
                    session->machine->uart4_host_ctx == session;
    out->nat_enabled = session->nat_enabled;
    out->peer_opened = session->peer_opened;
    out->service_calls = session->service_calls;
    out->retired_since_open = session->retired_since_open;
    out->guest_tx_bytes = session->guest_tx_bytes;
    out->guest_rx_bytes = session->guest_rx_bytes;
    out->guest_ip_queued = session->guest_ip_queued;
    out->guest_ip_dropped = session->guest_ip_dropped;
    out->net_to_guest = session->net_to_guest;
    out->net_to_guest_lost = session->net_to_guest_lost;
    if (session->peer) {
        out->lcp_open = ppp_lcp_open(session->peer);
        out->ipcp_open = ppp_ipcp_open(session->peer);
    }
    if (session->net) {
        out->ip_in = session->net->stats.ip_in;
        out->ip_out = session->net->stats.ip_out;
        out->dns_queries = session->net->stats.dns_queries;
        out->dns_answered = session->net->stats.dns_answered;
        out->dns_deferred = session->net->stats.dns_deferred;
        out->dns_pending_full = session->net->stats.dns_pending_full;
        out->dns_timeouts = session->net->stats.dns_timeouts;
        out->tcp_established = session->net->stats.tcp_established;
        out->tcp_bytes_to_host = session->net->stats.tcp_bytes_to_host;
        out->tcp_bytes_to_guest = session->net->stats.tcp_bytes_to_guest;
    }
    const net_host_stats_t *host = net_host_stats(session->host);
    if (host) {
        out->host_bytes_out = host->bytes_out;
        out->host_bytes_in = host->bytes_in;
        out->host_resolves = host->resolves;
        out->host_resolve_failures = host->resolve_failures;
        out->host_resolve_nxdomain = host->resolve_nxdomain;
        out->host_errors = host->errors;
        out->host_last_error = host->last_error;
    }
}

void vm_network_session_destroy(vm_network_session_t **slot) {
    if (!slot || !*slot) return;
    vm_network_session_t *session = *slot;
    if (session->machine &&
        session->machine->uart4_host_tx == uart4_guest_tx &&
        session->machine->uart4_host_service == uart4_host_service &&
        session->machine->uart4_host_ctx == session)
        (void)s5l8900_set_uart4_host(session->machine, NULL, NULL, NULL);
    if (session->net) net_reset(session->net);
    net_host_close(session->host);
    free(session->net);
    free(session->peer);
    free(session);
    *slot = NULL;
}
