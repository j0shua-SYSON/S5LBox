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
    bool     attached;
    bool     nat_enabled;
    bool     peer_opened;
    bool     lcp_open;
    bool     ipcp_open;
    uint64_t service_calls;
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
    uint64_t tcp_established;
    uint64_t tcp_bytes_to_host;
    uint64_t tcp_bytes_to_guest;
    uint64_t host_bytes_out;
    uint64_t host_bytes_in;
    uint64_t host_errors;
    int      host_last_error;
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
