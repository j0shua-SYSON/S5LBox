/* Host-side tests for the iOS PPP/NAT attachment. */
#include "VMNetworkSession.h"

#include "ppp.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void send_initial_guest_request(s5l8900_t *machine) {
    ppp_config_t config;
    ppp_peer_t guest;
    ppp_config_default(&config);
    config.magic ^= UINT32_C(0x01010101);
    ppp_init(&guest, &config);
    ppp_open(&guest);

    int byte;
    while ((byte = ppp_output_byte(&guest)) >= 0)
        machine->bus.write32(machine->bus.ctx,
                             S5L8900_UART4_BASE + UART_UTXH,
                             (uint32_t)(uint8_t)byte);
}

static void test_ppp_attachment_closes_the_uart_loop(void) {
    s5l8900_t machine;
    char detail[192];
    CHECK(s5l8900_init(&machine, 0u, 1u << 20), "machine init failed");

    vm_network_session_t *session = vm_network_session_create(
        &machine, false, detail, sizeof detail);
    CHECK(session != NULL, "PPP-only attachment failed: %s", detail);
    if (!session) {
        s5l8900_free(&machine);
        return;
    }

    vm_network_status_t status;
    vm_network_session_status(session, &status);
    CHECK(status.attached && !status.nat_enabled && !status.peer_opened,
          "fresh status is attached/nat/open=%u/%u/%u",
          status.attached, status.nat_enabled, status.peer_opened);

    send_initial_guest_request(&machine);
    vm_network_session_status(session, &status);
    CHECK(status.peer_opened && status.guest_tx_bytes > 6u,
          "guest request did not open/feed the peer (%llu bytes)",
          (unsigned long long)status.guest_tx_bytes);
    CHECK(machine.uart4.rx_count == 0u,
          "host bytes entered uart4 inside guest MMIO instead of at a boundary");

    /* No guest instruction is required to poll a peer: the public run-call
     * boundary itself is the synchronization point. */
    arm_status_t arm_status = ARM_OK;
    CHECK(s5l8900_run(&machine, 0u, &arm_status) == 0u &&
          arm_status == ARM_OK,
          "zero-retirement service boundary changed CPU status");
    vm_network_session_status(session, &status);
    CHECK(status.service_calls == 1u && status.guest_rx_bytes > 0u &&
          machine.uart4.rx_count > 0u,
          "host reply was not injected at the run boundary "
          "(calls/rx/fifo=%llu/%llu/%u)",
          (unsigned long long)status.service_calls,
          (unsigned long long)status.guest_rx_bytes,
          machine.uart4.rx_count);

    /* The physical FIFO remains sixteen bytes, but a receive DMA command must
     * consume beyond it in ONE tick. Each successful URXH load exposes the
     * next already-framed byte, while source readiness prevents the PL080 from
     * inventing zeroes if that queue ever runs dry. Seventeen is deliberate:
     * it is the smallest transfer that cannot pass by draining only the FIFO
     * initially visible at the run boundary. */
    const uint32_t dma = S5L8900_DMAC0_BASE + PL080_CHAN_BASE;
    machine.bus.write32(machine.bus.ctx, dma + PL080_CH_SRC,
                        S5L8900_UART4_BASE + UART_URXH);
    machine.bus.write32(machine.bus.ctx, dma + PL080_CH_DST, 0x400u);
    machine.bus.write32(machine.bus.ctx, dma + PL080_CH_LLI, 0u);
    machine.bus.write32(machine.bus.ctx, dma + PL080_CH_CTRL,
                        PL080_CTRL_DI | PL080_CTRL_I | 17u);
    machine.bus.write32(machine.bus.ctx, dma + PL080_CH_CFG,
                        (2u << PL080_CFG_FLOW_SHIFT) | PL080_CFG_ITC |
                        PL080_CFG_EN);
    machine.bus.write32(machine.bus.ctx,
                        S5L8900_UART4_BASE + UART_UCON, 0x1880u);
    machine.bus.write32(machine.bus.ctx,
                        S5L8900_UART4_BASE + UART_UFCON, 0x31u);
    machine.bus.write32(machine.bus.ctx,
                        S5L8900_DMAC0_BASE + PL080_CONFIG,
                        PL080_CONFIG_EN);
    s5l8900_tick(&machine, 0u);
    vm_network_session_status(session, &status);
    CHECK(status.refill_calls > 0u && status.guest_rx_bytes > UART_RX_FIFO,
          "DMA demand refill did not stream past one hardware FIFO "
          "(calls/rx=%llu/%llu)",
          (unsigned long long)status.refill_calls,
          (unsigned long long)status.guest_rx_bytes);
    CHECK((machine.dmac[0].ch[0].cfg & PL080_CFG_EN) == 0u &&
          machine.dmac[0].bytes_moved == 17u &&
          machine.uart4.rx_underruns == 0u,
          "DMA moved %llu bytes/enabled=%u/underruns=%llu, expected 17/0/0",
          (unsigned long long)machine.dmac[0].bytes_moved,
          (machine.dmac[0].ch[0].cfg & PL080_CFG_EN) != 0u,
          (unsigned long long)machine.uart4.rx_underruns);
    CHECK(status.dma_guest_rx.found &&
          status.dma_guest_rx.controller == 0u &&
          status.dma_guest_rx.channel == 0u &&
          status.dma_guest_rx.bytes == 17u &&
          status.dmac_bytes[0] == 17u &&
          status.uart4_rx_reads == 17u,
          "DMA receive witness is found/controller/channel/bytes/total/reads="
          "%u/%u/%u/%llu/%llu/%llu, expected 1/0/0/17/17/17",
          status.dma_guest_rx.found,
          status.dma_guest_rx.controller,
          status.dma_guest_rx.channel,
          (unsigned long long)status.dma_guest_rx.bytes,
          (unsigned long long)status.dmac_bytes[0],
          (unsigned long long)status.uart4_rx_reads);
    CHECK(status.uart4_ucon == 0x1880u &&
          status.uart4_ufcon == 0x31u &&
          status.uart4_rx_count <= UART_RX_FIFO,
          "UART receive registers/count are %08x/%08x/%u",
          status.uart4_ucon, status.uart4_ufcon,
          (unsigned)status.uart4_rx_count);

    /* UART4's shipped transmit DMA record crosses to uart0 UTXH. The status
     * witness must identify that physical endpoint while the host callback
     * still receives it as logical UART4 traffic. */
    machine.bus.write8(machine.bus.ctx, 0x500u, 0x7eu);
    const uint32_t txdma = dma + PL080_CHAN_STRIDE;
    machine.bus.write32(machine.bus.ctx, txdma + PL080_CH_SRC, 0x500u);
    machine.bus.write32(machine.bus.ctx, txdma + PL080_CH_DST,
                        S5L8900_UART0_BASE + UART_UTXH);
    machine.bus.write32(machine.bus.ctx, txdma + PL080_CH_LLI, 0u);
    machine.bus.write32(machine.bus.ctx, txdma + PL080_CH_CTRL,
                        PL080_CTRL_SI | PL080_CTRL_I | 1u);
    machine.bus.write32(machine.bus.ctx, txdma + PL080_CH_CFG,
                        (1u << PL080_CFG_FLOW_SHIFT) | PL080_CFG_ITC |
                        PL080_CFG_EN);
    s5l8900_tick(&machine, 0u);
    vm_network_session_status(session, &status);
    CHECK(status.dma_guest_tx.found &&
          status.dma_guest_tx.controller == 0u &&
          status.dma_guest_tx.channel == 1u &&
          status.dma_guest_tx.bytes == 1u &&
          status.guest_tx_bytes > 7u,
          "DMA transmit witness is found/controller/channel/bytes/guest="
          "%u/%u/%u/%llu/%llu, expected 1/0/1/1/>7",
          status.dma_guest_tx.found,
          status.dma_guest_tx.controller,
          status.dma_guest_tx.channel,
          (unsigned long long)status.dma_guest_tx.bytes,
          (unsigned long long)status.guest_tx_bytes);

    vm_network_session_destroy(&session);
    CHECK(session == NULL && machine.uart4_host_tx == NULL &&
          machine.uart4_host_service == NULL &&
          machine.uart4_host_refill == NULL &&
          machine.uart4_host_ctx == NULL,
          "destroy did not detach the uart4 host peer");
    s5l8900_free(&machine);
}

static void test_nat_socket_owner_starts_without_a_flow(void) {
    s5l8900_t machine;
    char detail[192];
    CHECK(s5l8900_init(&machine, 0u, 1u << 20), "machine init failed");
    vm_network_session_t *session = vm_network_session_create(
        &machine, true, detail, sizeof detail);
    CHECK(session != NULL, "PPP/NAT attachment failed: %s", detail);
    if (session) {
        vm_network_status_t status;
        vm_network_session_status(session, &status);
        CHECK(status.attached && status.nat_enabled &&
              status.host_errors == 0u && status.tcp_live_flows == 0u,
              "fresh NAT status attached/nat/errors/live=%u/%u/%llu/%u",
              status.attached, status.nat_enabled,
              (unsigned long long)status.host_errors,
              status.tcp_live_flows);
    }
    vm_network_session_destroy(&session);
    s5l8900_free(&machine);
}

static void test_invalid_create_fails_closed(void) {
    char detail[96];
    vm_network_session_t *session = vm_network_session_create(
        NULL, true, detail, sizeof detail);
    CHECK(session == NULL && detail[0] != '\0',
          "NULL-machine create did not fail with a reason");
    vm_network_status_t status;
    memset(&status, 0xa5, sizeof status);
    vm_network_session_status(NULL, &status);
    CHECK(!status.attached && status.guest_tx_bytes == 0u,
          "NULL status did not produce a clean zero report");
    vm_network_session_destroy(NULL);
}

int main(void) {
    printf("S5LBox iOS PPP/NAT attachment tests\n");
    test_ppp_attachment_closes_the_uart_loop();
    test_nat_socket_owner_starts_without_a_flow();
    test_invalid_create_fails_closed();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
