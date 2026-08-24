/*
 * S5LBox - the iOS runtime's host peer for guest PPP/NAT.
 *
 * The protocol engines remain portable core code and the sockets remain in
 * tools/net_host.c. This owner is the missing join: uart4 TX enters PPP during
 * guest MMIO, while every host socket poll and every host-to-guest byte transfer
 * happens on the machine thread after a bounded s5l8900_run() slice.
 *
 * No private API, entitlement, tunnel device, raw socket, background service,
 * or executable memory is involved. The same app binary therefore remains a
 * stock-iPhone application; this is ordinary user-mode IPv4 NAT.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMNETWORKSESSION_H
#define S5LBOX_APP_VMNETWORKSESSION_H

#include "soc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vm_network_session vm_network_session_t;

typedef struct {
    bool     found;
    unsigned controller;
    unsigned channel;
    uint32_t src;
    uint32_t dst;
    uint32_t lli;
    uint32_t ctrl;
    uint32_t cfg;
    uint64_t runs;
    uint64_t bytes;
} vm_network_dma_endpoint_status_t;

typedef struct {
    bool     attached;
    bool     nat_enabled;
    bool     peer_opened;
    bool     lcp_open;
    bool     ipcp_open;
    uint64_t service_calls;
    uint64_t refill_calls;
    uint64_t retired_since_open;
    uint64_t guest_tx_bytes;
    uint64_t guest_rx_bytes;
    uint64_t guest_ip_queued;
    uint64_t guest_ip_dropped;
    uint64_t net_to_guest;
    uint64_t net_to_guest_lost;
    uint64_t ip_in;
    uint64_t ip_out;
    uint64_t dns_queries;
    uint64_t dns_answered;
    uint64_t dns_deferred;
    uint64_t dns_pending_full;
    uint64_t dns_timeouts;
    uint64_t tcp_established;
    uint64_t tcp_bytes_to_host;
    uint64_t tcp_bytes_to_guest;
    uint64_t tcp_resets_out;
    uint64_t tcp_resets_in;
    uint64_t tcp_retransmits;
    uint64_t tcp_out_of_order;
    uint64_t tcp_output_dropped;
    uint64_t tcp_aborts_unexpected_syn;
    uint64_t tcp_aborts_connect;
    uint64_t tcp_aborts_host_send;
    uint64_t tcp_aborts_host_recv;
    uint64_t tcp_aborts_retransmit;
    uint32_t tcp_last_abort_reason;
    uint32_t tcp_last_abort_state;
    uint32_t tcp_last_abort_window;
    uint32_t tcp_last_abort_inflight;
    uint32_t tcp_last_abort_buffered;
    uint32_t tcp_last_abort_retries;
    uint32_t tcp_last_peer_reset_state;
    uint32_t tcp_last_peer_reset_window;
    uint32_t tcp_last_peer_reset_inflight;
    uint32_t tcp_last_peer_reset_buffered;
    uint32_t tcp_last_peer_reset_retries;
    uint32_t tcp_output_pending;
    uint64_t ppp_fcs_errors;
    uint64_t ppp_tx_overflows;
    uint64_t host_bytes_out;
    uint64_t host_bytes_in;
    uint64_t host_resolves;
    uint64_t host_resolve_failures;
    uint64_t host_resolve_nxdomain;
    uint64_t host_errors;
    int      host_last_error;
    /* Existing PL080 state, copied at the same machine-thread boundary as the
     * network counters.  These are diagnostic witnesses for the stock UART4
     * DMA route: they distinguish an unarmed channel from bytes moved through
     * the wrong endpoint without changing any guest-visible device behavior. */
    uint64_t dmac_reads[S5L8900_DMAC_COUNT];
    uint64_t dmac_writes[S5L8900_DMAC_COUNT];
    uint64_t dmac_bytes[S5L8900_DMAC_COUNT];
    uint64_t dmac_items[S5L8900_DMAC_COUNT];
    uint64_t dmac_completions[S5L8900_DMAC_COUNT];
    uint64_t dmac_refused_flow[S5L8900_DMAC_COUNT];
    uint64_t dmac_refused_width[S5L8900_DMAC_COUNT];
    uint64_t dmac_refused_chain[S5L8900_DMAC_COUNT];
    uint64_t uart4_rx_pushed;
    uint64_t uart4_rx_reads;
    uint64_t uart4_rx_dropped;
    uint64_t uart4_rx_underruns;
    uint32_t uart4_ucon;
    uint32_t uart4_ufcon;
    uint32_t uart4_utrstat_pending;
    uint8_t  uart4_rx_count;
    bool     uart4_rx_timeout_armed;
    vm_network_dma_endpoint_status_t dma_guest_tx;
    vm_network_dma_endpoint_status_t dma_guest_rx;
} vm_network_status_t;

/*
 * Allocate and attach a PPP peer to `machine`. `nat_enabled` additionally
 * opens the ordinary nonblocking socket adapter. Failure leaves the machine
 * unattached and puts an actionable, terminated reason in `detail`.
 */
vm_network_session_t *vm_network_session_create(
    s5l8900_t *machine, bool nat_enabled,
    char *detail, size_t detail_capacity);

/* A lock-free copy: call only on the same owner thread as the machine. */
void vm_network_session_status(const vm_network_session_t *session,
                               vm_network_status_t *out);

/* Detach, close every host flow, and clear `*session`. Safe on NULL. */
void vm_network_session_destroy(vm_network_session_t **session);

#ifdef __cplusplus
}
#endif

#endif /* S5LBOX_APP_VMNETWORKSESSION_H */
