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
    uint64_t      refill_calls;
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

/* Move only bytes already framed by PPP. This function deliberately performs
 * no socket or protocol work: it is also called from the guest's URXH read
 * path, where re-entering the network stack once per byte would merely replace
 * the old throughput bug with an avoidable CPU cost. */
static void refill_uart4(vm_network_session_t *session) {
    if (!session || !session->machine || !session->peer) return;
    unsigned room = s5l_uart_rx_space(&session->machine->uart4);
    while (room--) {
        int byte = ppp_output_byte(session->peer);
        if (byte < 0) break;
        if (!s5l_uart_rx_push(&session->machine->uart4, (uint8_t)byte)) break;
        session->guest_rx_bytes++;
    }
}

static void uart4_host_refill(void *ctx) {
    vm_network_session_t *session = (vm_network_session_t *)ctx;
    if (!session) return;
    session->refill_calls++;
    refill_uart4(session);
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
            /* net_output() consumes a datagram. Reserve enough encoded space
             * before consuming it so a full PPP ring is ordinary
             * backpressure, never a packet silently removed between TCP and
             * the guest. The exact frame is usually smaller; this worst-case
             * bound includes both flags and every frame octet escaped. */
            const size_t worst_frame = 2u * PPP_MAX_FRAME + 2u;
            if (!ppp_ipcp_open(session->peer) ||
                ppp_output_capacity(session->peer) < worst_frame)
                break;
            uint8_t packet[NET_MTU];
            size_t length = net_output(session->net, packet, sizeof packet);
            if (!length) break;
            if (ppp_send_ip(session->peer, packet, length))
                session->net_to_guest++;
            else {
                /* With the same-thread capacity reservation above this is an
                 * invariant failure (for example, a peer MRU contradiction),
                 * not congestion. Keep it counted and make it diagnosable. */
                session->net_to_guest_lost++;
                (void)fprintf(stderr,
                              "[network] guest datagram handoff failed "
                              "after PPP capacity reservation\n");
                break;
            }
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

    uint64_t before = session->guest_rx_bytes;
    refill_uart4(session);
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
                                uart4_host_service, uart4_host_refill,
                                session)) {
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
    out->refill_calls = session->refill_calls;
    out->retired_since_open = session->retired_since_open;
    out->guest_tx_bytes = session->guest_tx_bytes;
    out->guest_rx_bytes = session->guest_rx_bytes;
    out->guest_ip_queued = session->guest_ip_queued;
    out->guest_ip_dropped = session->guest_ip_dropped;
    out->net_to_guest = session->net_to_guest;
    out->net_to_guest_lost = session->net_to_guest_lost;
    if (session->machine) {
        const s5l8900_t *machine = session->machine;
        out->uart4_rx_pushed = machine->uart4.rx_pushed;
        out->uart4_rx_reads = machine->uart4.rx_reads;
        out->uart4_rx_dropped = machine->uart4.rx_dropped;
        out->uart4_rx_underruns = machine->uart4.rx_underruns;
        out->uart4_ucon = machine->uart4.ucon;
        out->uart4_ufcon = machine->uart4.ufcon;
        out->uart4_utrstat_pending = machine->uart4.utrstat_pending;
        out->uart4_rx_count = machine->uart4.rx_count;
        out->uart4_rx_timeout_state = machine->uart4.rx_timeout_state;
        for (unsigned d = 0u; d < S5L8900_DMAC_COUNT; d++) {
            const s5l_pl080_t *controller = &machine->dmac[d];
            out->dmac_reads[d] = controller->reads;
            out->dmac_writes[d] = controller->writes;
            out->dmac_bytes[d] = controller->bytes_moved;
            out->dmac_items[d] = controller->items;
            out->dmac_completions[d] = controller->completions;
            out->dmac_refused_flow[d] = controller->refused_flow;
            out->dmac_refused_width[d] = controller->refused_width;
            out->dmac_refused_chain[d] = controller->refused_chain;
            for (unsigned c = 0u; c < S5L_PL080_CHANNELS; c++) {
                const s5l_pl080_chan_t *channel = &controller->ch[c];
                vm_network_dma_endpoint_status_t *endpoint = NULL;
                if (channel->dst == S5L8900_UART0_BASE + UART_UTXH)
                    endpoint = &out->dma_guest_tx;
                else if (channel->src == S5L8900_UART4_BASE + UART_URXH)
                    endpoint = &out->dma_guest_rx;
                if (!endpoint || (endpoint->found &&
                    endpoint->bytes >= channel->bytes))
                    continue;
                endpoint->found = true;
                endpoint->controller = d;
                endpoint->channel = c;
                endpoint->src = channel->src;
                endpoint->dst = channel->dst;
                endpoint->lli = channel->lli;
                endpoint->ctrl = channel->ctrl;
                endpoint->cfg = channel->cfg;
                endpoint->runs = channel->runs;
                endpoint->bytes = channel->bytes;
            }
        }
    }
    if (session->peer) {
        out->lcp_open = ppp_lcp_open(session->peer);
        out->ipcp_open = ppp_ipcp_open(session->peer);
        out->ppp_fcs_errors = session->peer->stats.fcs_errors;
        out->ppp_tx_overflows = session->peer->stats.tx_overflows;
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
        out->tcp_resets_out = session->net->stats.tcp_resets_out;
        out->tcp_resets_in = session->net->stats.tcp_resets_in;
        out->tcp_retransmits = session->net->stats.tcp_retransmits;
        out->tcp_out_of_order = session->net->stats.tcp_out_of_order;
        out->tcp_output_dropped = session->net->stats.out_dropped;
        out->tcp_aborts_unexpected_syn =
            session->net->stats.tcp_aborts_unexpected_syn;
        out->tcp_aborts_connect = session->net->stats.tcp_aborts_connect;
        out->tcp_aborts_host_send = session->net->stats.tcp_aborts_host_send;
        out->tcp_aborts_host_recv = session->net->stats.tcp_aborts_host_recv;
        out->tcp_aborts_retransmit =
            session->net->stats.tcp_aborts_retransmit;
        out->tcp_last_abort_reason = session->net->stats.tcp_last_abort_reason;
        out->tcp_last_abort_state = session->net->stats.tcp_last_abort_state;
        out->tcp_last_abort_window = session->net->stats.tcp_last_abort_window;
        out->tcp_last_abort_inflight =
            session->net->stats.tcp_last_abort_inflight;
        out->tcp_last_abort_buffered =
            session->net->stats.tcp_last_abort_buffered;
        out->tcp_last_abort_retries =
            session->net->stats.tcp_last_abort_retries;
        out->tcp_last_peer_reset_state =
            session->net->stats.tcp_last_peer_reset_state;
        out->tcp_last_peer_reset_window =
            session->net->stats.tcp_last_peer_reset_window;
        out->tcp_last_peer_reset_inflight =
            session->net->stats.tcp_last_peer_reset_inflight;
        out->tcp_last_peer_reset_buffered =
            session->net->stats.tcp_last_peer_reset_buffered;
        out->tcp_last_peer_reset_retries =
            session->net->stats.tcp_last_peer_reset_retries;
        out->tcp_output_pending = (uint32_t)net_output_pending(session->net);
        net_tcp_live_status_t live;
        if (net_get_tcp_live_status(session->net, &live)) {
            out->tcp_live_flows = live.flows;
            out->tcp_live_state = (uint32_t)live.state;
            out->tcp_live_guest_port = live.guest_port;
            out->tcp_live_dst_port = live.dst_port;
            out->tcp_live_window = live.window;
            out->tcp_live_inflight = live.inflight;
            out->tcp_live_tx_buffered = live.tx_buffered;
            out->tcp_live_rx_buffered = live.rx_buffered;
            out->tcp_live_retries = live.retries;
            out->tcp_live_rto_remaining_ms = live.rto_remaining_ms;
            out->tcp_live_flags = live.flags;
        }
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
        session->machine->uart4_host_refill == uart4_host_refill &&
        session->machine->uart4_host_ctx == session)
        (void)s5l8900_set_uart4_host(session->machine, NULL, NULL, NULL, NULL);
    if (session->net) net_reset(session->net);
    net_host_close(session->host);
    free(session->net);
    free(session->peer);
    free(session);
    *slot = NULL;
}
