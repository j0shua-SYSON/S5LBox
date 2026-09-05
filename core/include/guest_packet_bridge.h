/* S5LBox -- bounded XNU32 packet offload. Copyright (c) 2026 j0shua-SYSON.
 * MIT licensed. Firmware addresses belong to the frontend's exact-build gate.
 * The guest still owns allocation, queues, routing, TCP and socket semantics.
 * Only payload transport crosses this bridge; no host I/O runs inside an SVC. */
#ifndef S5LBOX_GUEST_PACKET_BRIDGE_H
#define S5LBOX_GUEST_PACKET_BRIDGE_H
#include "arm.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define GUEST_PACKET_MTU 1500u
#define GUEST_PACKET_TOKEN_SIZE 16u
#define GUEST_PACKET_RX_SVC UINT32_C(0xef0000f0)
#define GUEST_PACKET_TX_SVC UINT32_C(0xef0000f1)

typedef struct {
    uint32_t rx_pc;             /* A32 sub r1,r6,#2, after native FCS removal */
    uint32_t tx_pc;             /* A32 blx r6, before serial output enqueue */
    uint32_t free_thumb_pc;     /* native mbuf_freem, even fetch address */
    uint32_t tx_done_pc;        /* native return-success epilogue, A32 */
    uint32_t rx_drop_pc;        /* native receive-buffer replenishment, A32 */
} guest_packet_sites_t;

typedef struct {
    guest_packet_sites_t sites;
    uint8_t *ram;
    uint64_t ram_base, ram_size;
    /* Queue-only callbacks. send=false retains the original serial fallback.
     * peek borrows a packet until consume; both must be non-reentrant. */
    bool (*send)(void *ctx, const uint8_t *packet, size_t length);
    size_t (*peek)(void *ctx, const uint8_t token[GUEST_PACKET_TOKEN_SIZE],
                   const uint8_t **packet);
    void (*consume)(void *ctx);
    void *ctx;
    uint64_t tx_packets, tx_bytes, rx_packets, rx_bytes;
    uint64_t tx_fallback, stale_tokens, failures;
} guest_packet_bridge_t;

void guest_packet_token(uint8_t token[GUEST_PACKET_TOKEN_SIZE], uint64_t id);
arm_svc_result_t guest_packet_bridge_svc(guest_packet_bridge_t *bridge,
                                        arm_cpu_t *cpu, uint32_t pc,
                                        uint32_t encoding);
#endif
