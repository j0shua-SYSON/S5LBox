/* Host-side tests for the iOS PPP/NAT attachment. */
#include "VMNetworkSession.h"

#include "net.h"
#include "ppp.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void write_be16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void send_guest_ppp_bytes(s5l8900_t *machine, ppp_peer_t *guest) {
    int byte;
    while ((byte = ppp_output_byte(guest)) >= 0)
        machine->bus.write32(machine->bus.ctx,
                             S5L8900_UART4_BASE + UART_UTXH,
                             (uint32_t)(uint8_t)byte);
}

static void receive_host_ppp_bytes(s5l8900_t *machine, ppp_peer_t *guest) {
    while (machine->uart4.rx_count != 0u) {
        uint8_t byte = (uint8_t)machine->bus.read32(
            machine->bus.ctx, S5L8900_UART4_BASE + UART_URXH);
        ppp_input_byte(guest, byte);
    }
}

static bool run_service_boundary(s5l8900_t *machine) {
    arm_status_t status = ARM_OK;
    return s5l8900_run(machine, 0u, &status) == 0u && status == ARM_OK;
}

typedef struct {
    uint64_t now_ns;
    bool available;
} fake_host_clock_t;

static bool fake_host_now(void *ctx, uint64_t *nanoseconds) {
    fake_host_clock_t *clock = (fake_host_clock_t *)ctx;
    if (!clock || !nanoseconds || !clock->available) return false;
    *nanoseconds = clock->now_ns;
    return true;
}

static size_t drain_uart4_bytes(s5l8900_t *machine, size_t limit) {
    size_t count = 0u;
    while (count < limit) {
        if (machine->uart4.rx_count == 0u) {
            if (!run_service_boundary(machine) ||
                machine->uart4.rx_count == 0u)
                break;
        }
        (void)machine->bus.read32(machine->bus.ctx,
                                  S5L8900_UART4_BASE + UART_URXH);
        count++;
    }
    return count;
}

static bool settle_test_ppp_link(s5l8900_t *machine,
                                 vm_network_session_t *session,
                                 ppp_peer_t *guest) {
    for (unsigned turn = 0u; turn < 32u; turn++) {
        send_guest_ppp_bytes(machine, guest);
        if (!run_service_boundary(machine)) return false;
        receive_host_ppp_bytes(machine, guest);

        vm_network_status_t status;
        vm_network_session_status(session, &status);
        if (status.ipcp_open && ppp_ipcp_open(guest)) {
            send_guest_ppp_bytes(machine, guest);
            if (!run_service_boundary(machine)) return false;
            receive_host_ppp_bytes(machine, guest);
            return true;
        }
    }
    return false;
}

static bool open_test_ppp_link(s5l8900_t *machine,
                               vm_network_session_t *session,
                               ppp_peer_t *guest) {
    ppp_config_t config;
    ppp_config_default(&config);
    uint32_t address = config.local_ip;
    config.local_ip = config.remote_ip;
    config.remote_ip = address;
    config.magic ^= UINT32_C(0x01010101);
    ppp_init(guest, &config);
    ppp_open(guest);
    return settle_test_ppp_link(machine, session, guest);
}

static size_t build_echo_request(uint8_t *packet, size_t capacity,
                                 uint16_t sequence) {
    uint8_t icmp[12] = {0};
    icmp[0] = 8u;
    write_be16(icmp + 4u, UINT16_C(0x5355));
    write_be16(icmp + 6u, sequence);
    for (unsigned i = 8u; i < sizeof icmp; i++)
        icmp[i] = (uint8_t)(sequence + i);
    write_be16(icmp + 2u, net_checksum(icmp, sizeof icmp));

    size_t header = net_build_ipv4(packet, capacity,
                                   UINT32_C(0x0a00020f),
                                   UINT32_C(0x0a000202),
                                   NET_PROTO_ICMP, sequence, sizeof icmp);
    if (!header || header + sizeof icmp > capacity) return 0u;
    memcpy(packet + header, icmp, sizeof icmp);
    return header + sizeof icmp;
}

typedef struct {
    unsigned packets;
    size_t length;
    uint8_t last[64];
} guest_ip_sink_t;

static void capture_guest_ip(void *ctx, const uint8_t *packet, size_t length) {
    guest_ip_sink_t *sink = (guest_ip_sink_t *)ctx;
    if (!sink || !packet) return;
    sink->packets++;
    sink->length = length;
    if (length <= sizeof sink->last) memcpy(sink->last, packet, length);
}

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

static void test_host_datagrams_cross_one_per_run_boundary(void) {
    s5l8900_t machine;
    char detail[192];
    CHECK(s5l8900_init(&machine, 0u, 1u << 20), "machine init failed");
    vm_network_session_t *session = vm_network_session_create(
        &machine, true, detail, sizeof detail);
    CHECK(session != NULL, "PPP/NAT attachment failed: %s", detail);
    if (!session) {
        s5l8900_free(&machine);
        return;
    }

    ppp_peer_t guest;
    CHECK(open_test_ppp_link(&machine, session, &guest),
          "the two in-process PPP peers did not reach IPCP Opened");

    vm_network_status_t before;
    vm_network_session_status(session, &before);
    CHECK(before.ipcp_open && before.tcp_output_pending == 0u,
          "the test link opened with IPCP/pending=%u/%u",
          before.ipcp_open, before.tcp_output_pending);

    for (uint16_t sequence = 1u; sequence <= 3u; sequence++) {
        uint8_t packet[64];
        size_t length = build_echo_request(packet, sizeof packet, sequence);
        CHECK(length != 0u && ppp_send_ip(&guest, packet, length),
              "echo request %u could not be framed", sequence);
    }
    send_guest_ppp_bytes(&machine, &guest);

    vm_network_status_t status;
    CHECK(run_service_boundary(&machine), "the first service boundary failed");
    vm_network_session_status(session, &status);
    CHECK(status.net_to_guest == before.net_to_guest + 1u &&
          status.tcp_output_pending == 2u,
          "first boundary sent/pending=%llu/%u, expected %llu/2",
          (unsigned long long)status.net_to_guest,
          status.tcp_output_pending,
          (unsigned long long)(before.net_to_guest + 1u));

    CHECK(run_service_boundary(&machine), "the second service boundary failed");
    vm_network_session_status(session, &status);
    CHECK(status.net_to_guest == before.net_to_guest + 2u &&
          status.tcp_output_pending == 1u,
          "second boundary sent/pending=%llu/%u, expected %llu/1",
          (unsigned long long)status.net_to_guest,
          status.tcp_output_pending,
          (unsigned long long)(before.net_to_guest + 2u));

    CHECK(run_service_boundary(&machine), "the third service boundary failed");
    vm_network_session_status(session, &status);
    CHECK(status.net_to_guest == before.net_to_guest + 3u &&
          status.tcp_output_pending == 0u &&
          status.net_to_guest_lost == 0u && status.guest_ip_dropped == 0u,
          "third boundary sent/pending/lost/dropped=%llu/%u/%llu/%llu",
          (unsigned long long)status.net_to_guest,
          status.tcp_output_pending,
          (unsigned long long)status.net_to_guest_lost,
          (unsigned long long)status.guest_ip_dropped);

    vm_network_session_destroy(&session);
    s5l8900_free(&machine);
}

static void test_restored_guest_reopens_replaced_host_peer(void) {
    s5l8900_t machine;
    char detail[192];
    CHECK(s5l8900_init(&machine, 0u, 1u << 20), "machine init failed");
    vm_network_session_t *session = vm_network_session_create(
        &machine, true, detail, sizeof detail);
    CHECK(session != NULL, "initial PPP/NAT attachment failed: %s", detail);
    if (!session) {
        s5l8900_free(&machine);
        return;
    }

    ppp_peer_t guest;
    CHECK(open_test_ppp_link(&machine, session, &guest),
          "the initial link did not reach IPCP Opened");
    vm_network_session_destroy(&session);

    /* A checkpoint can contain bytes already committed to the UART FIFO while
     * the rest of that old host frame lived in the unsaved peer output ring.
     * Preserve such a fragment: the replacement peer's leading Flag must end
     * it and let the already-open guest re-synchronize. */
    static const uint8_t STALE_FRAME[] = {0xffu, 0x21u, 0x45u, 0xaau, 0xbbu};
    for (size_t i = 0u; i < sizeof STALE_FRAME; i++)
        CHECK(s5l_uart_rx_push(&machine.uart4, STALE_FRAME[i]),
              "could not stage stale UART byte %zu", i);
    uint64_t fcs_before = guest.stats.fcs_errors;

    session = vm_network_session_create(&machine, true, detail, sizeof detail);
    CHECK(session != NULL, "replacement PPP/NAT attachment failed: %s", detail);
    if (!session) {
        s5l8900_free(&machine);
        return;
    }
    vm_network_status_t status;
    vm_network_session_status(session, &status);
    CHECK(!status.peer_opened && machine.uart4.rx_count == sizeof STALE_FRAME,
          "replacement peer opened early or discarded saved UART bytes");
    CHECK(vm_network_session_reopen_after_restore(session),
          "restore-specific peer reopen was refused");
    vm_network_session_status(session, &status);
    CHECK(status.peer_opened && !status.lcp_open && !status.ipcp_open,
          "fresh host peer reported an inherited open state");
    CHECK(settle_test_ppp_link(&machine, session, &guest),
          "the already-open guest did not renegotiate with its new host peer");
    CHECK(guest.stats.fcs_errors == fcs_before + 1u,
          "fresh Flag did not isolate the stale frame (%llu -> %llu errors)",
          (unsigned long long)fcs_before,
          (unsigned long long)guest.stats.fcs_errors);

    guest_ip_sink_t sink;
    memset(&sink, 0, sizeof sink);
    ppp_set_ip_sink(&guest, capture_guest_ip, &sink);
    uint8_t packet[64];
    size_t length = build_echo_request(packet, sizeof packet, 0x33u);
    CHECK(length != 0u && ppp_send_ip(&guest, packet, length),
          "post-restore echo request could not be framed");
    send_guest_ppp_bytes(&machine, &guest);
    bool serviced = true;
    for (unsigned turn = 0u; turn < 8u && sink.packets == 0u; turn++) {
        if (!run_service_boundary(&machine)) {
            serviced = false;
            break;
        }
        receive_host_ppp_bytes(&machine, &guest);
        send_guest_ppp_bytes(&machine, &guest);
    }
    CHECK(serviced, "post-restore echo service boundary failed");
    vm_network_session_status(session, &status);
    CHECK(sink.packets == 1u && sink.length >= 28u &&
          sink.last[9] == NET_PROTO_ICMP && sink.last[20] == 0u,
          "post-restore gateway echo reply packets/length/out/pending="
          "%u/%zu/%llu/%u",
          sink.packets, sink.length,
          (unsigned long long)status.net_to_guest,
          status.tcp_output_pending);

    vm_network_session_destroy(&session);
    s5l8900_free(&machine);
}

static void test_restore_retry_uses_monotonic_host_time(void) {
    s5l8900_t machine;
    char detail[192];
    CHECK(s5l8900_init(&machine, 0u, 1u << 20), "machine init failed");
    vm_network_session_t *session = vm_network_session_create(
        &machine, false, detail, sizeof detail);
    CHECK(session != NULL, "PPP attachment failed: %s", detail);
    if (!session) {
        s5l8900_free(&machine);
        return;
    }

    fake_host_clock_t clock = {
        UINT64_C(900000000000),
        true
    };
    CHECK(vm_network_session_set_host_clock(session, fake_host_now, &clock),
          "host protocol clock was refused");
    CHECK(vm_network_session_reopen_after_restore(session),
          "restore-specific peer reopen was refused");
    CHECK(run_service_boundary(&machine), "initial service boundary failed");
    CHECK(drain_uart4_bytes(&machine, 37u) == 37u,
          "initial LCP request did not contain 37 wire bytes");

    vm_network_status_t status;
    vm_network_session_status(session, &status);
    CHECK(status.host_clock_enabled && status.host_clock_anchored &&
          status.network_now_ms == 0u && status.retired_since_open == 0u &&
          status.guest_rx_bytes == 37u,
          "initial host clock/open status was %u/%u/%u/%llu/%llu",
          status.host_clock_enabled, status.host_clock_anchored,
          status.network_now_ms,
          (unsigned long long)status.retired_since_open,
          (unsigned long long)status.guest_rx_bytes);

    clock.now_ns += UINT64_C(2999000000);
    CHECK(run_service_boundary(&machine), "pre-deadline boundary failed");
    vm_network_session_status(session, &status);
    CHECK(status.network_now_ms == 2999u && status.guest_rx_bytes == 37u &&
          drain_uart4_bytes(&machine, 1u) == 0u,
          "LCP retried before 3 seconds (now/rx=%u/%llu)",
          status.network_now_ms,
          (unsigned long long)status.guest_rx_bytes);

    clock.now_ns += UINT64_C(1000000);
    CHECK(run_service_boundary(&machine), "deadline boundary failed");
    CHECK(drain_uart4_bytes(&machine, 37u) == 37u,
          "three-second LCP retry did not contain 37 wire bytes");
    vm_network_session_status(session, &status);
    CHECK(status.network_now_ms == 3000u &&
          status.retired_since_open == 0u &&
          status.guest_rx_bytes == 74u &&
          status.host_clock_failures == 0u,
          "wall-clock retry status now/retired/rx/fail=%u/%llu/%llu/%llu",
          status.network_now_ms,
          (unsigned long long)status.retired_since_open,
          (unsigned long long)status.guest_rx_bytes,
          (unsigned long long)status.host_clock_failures);

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

static void test_bulk_packets_use_tokens_and_recover_lost_notifications(bool batch) {
    s5l8900_t machine;
    char detail[192];
    CHECK(s5l8900_init(&machine, 0u, 1u << 20), "machine init failed");
    vm_network_session_t *session = vm_network_session_create(
        &machine, true, detail, sizeof detail);
    if (!session) { CHECK(false, "create failed: %s", detail); s5l8900_free(&machine); return; }
    fake_host_clock_t clock = {UINT64_C(1000000000), true};
    CHECK(vm_network_session_set_host_clock(session, fake_host_now, &clock),
          "clock attach failed");
    guest_packet_bridge_t bridge = {
        .sites = {0xc0000800u, 0xc0000804u, 0xc0000900u, 0xc0000a00u, 0xc0000b00u,
                  0xc0000808u, 0xc0000d00u, 0xc0000e00u},
        .ram = machine.ram, .ram_base = 0u, .ram_size = machine.ram_size
    };
    CHECK(!vm_network_session_attach_packet_bridge(session, &bridge),
          "an old unpatched checkpoint enabled bulk tokens");
    machine.bus.write32(machine.bus.ctx, 0x800u, GUEST_PACKET_RX_SVC);
    CHECK(!vm_network_session_attach_packet_bridge(session, &bridge),
          "a partially patched checkpoint enabled bulk tokens");
    machine.bus.write32(machine.bus.ctx, 0x804u, GUEST_PACKET_TX_SVC);
    if (batch) machine.bus.write32(machine.bus.ctx, 0x808u, GUEST_PACKET_BATCH_SVC);
    CHECK(vm_network_session_attach_packet_bridge(session, &bridge), "bulk attach failed");
    CHECK((bridge.finish != NULL) == batch, "old checkpoint enabled batch delivery");
    CHECK((bridge.peek_large != NULL) == batch, "old checkpoint enabled coalescing");
    CHECK(!vm_network_session_attach_packet_bridge(session, &bridge), "double attach accepted");
    ppp_peer_t guest;
    CHECK(open_test_ppp_link(&machine, session, &guest), "bulk setup lost stock IPCP");
    guest_ip_sink_t sink = {0};
    ppp_set_ip_sink(&guest, capture_guest_ip, &sink);
    uint8_t packet[64];
    for (unsigned i = 0u; i < 3u; i++) {
        size_t n = build_echo_request(packet, sizeof packet, (uint16_t)(i+1u));
        CHECK(bridge.send(bridge.ctx, packet, n), "bulk guest output failed");
    }
    vm_network_status_t before, status;
    vm_network_session_status(session, &before);
    CHECK(run_service_boundary(&machine), "bulk service failed");
    receive_host_ppp_bytes(&machine, &guest);
    vm_network_session_status(session, &status);
    CHECK(status.packet_offload && status.net_to_guest == before.net_to_guest+3u,
          "bulk transport retained one-packet scheduling");
    /* This fixture uses PIO, not the real driver's demand-refilled DMA. Its
     * 16-byte FIFO needs further service boundaries to carry the tokens. */
    for (unsigned turn = 0u; turn < 16u; turn++) {
        CHECK(run_service_boundary(&machine), "token refill failed");
        receive_host_ppp_bytes(&machine, &guest);
    }
    CHECK(sink.packets == (batch ? 1u : 3u) && sink.length == GUEST_PACKET_TOKEN_SIZE,
          "UART notification packets/length=%u/%zu ip=%llu bad=%llu pending=%u",
          sink.packets, sink.length, (unsigned long long)guest.stats.ip_frames_in,
          (unsigned long long)guest.stats.fcs_errors, machine.uart4.rx_count);
    const uint8_t *reply = NULL;
    size_t n = bridge.peek(bridge.ctx, sink.last, &reply);
    if (batch) {
        CHECK(bridge.peek_large(bridge.ctx, sink.last, GUEST_PACKET_RX_MAX, &reply) == n,
              "coalesced non-TCP control datagrams");
    }
    CHECK(n == 32u && reply && reply[20] == 0u && net_checksum(reply, 20u) == 0u,
          "guest packet queue did not retain the actual ICMP reply");
    bridge.consume(bridge.ctx);
    if (batch) {
        CHECK(bridge.peek(bridge.ctx, NULL, &reply) == 32u,
              "batch next packet missing after token consumption");
        bridge.consume(bridge.ctx);
        /* End early as if native allocation failed: the third packet is
         * retained and must get a fresh notification at the next boundary. */
        bridge.finish(bridge.ctx);
        for (unsigned turn = 0u; turn < 16u; turn++) {
            CHECK(run_service_boundary(&machine), "partial batch rearm failed");
            receive_host_ppp_bytes(&machine, &guest);
        }
        CHECK(sink.packets == 2u && bridge.peek(bridge.ctx, sink.last, &reply) == 32u,
              "early batch finish lost or duplicated the remaining packet");
        bridge.consume(bridge.ctx);
        CHECK(bridge.peek(bridge.ctx, NULL, &reply) == 0u, "batch left a packet queued");
        bridge.finish(bridge.ctx);
    } /* Without batching the first two notifications were deliberately lost. */
    vm_network_session_status(session, &status);
    CHECK(status.net_to_guest_lost == (batch ? 0u : 2u),
          "lost earlier notifications wedged or silently disappeared");
    CHECK(bridge.peek(bridge.ctx, sink.last, &reply) == 0u,
          "duplicate notification reused a consumed packet");
    n = build_echo_request(packet, sizeof packet, 4u);
    CHECK(bridge.send(bridge.ctx, packet, n), "send after notification loss failed");
    CHECK(run_service_boundary(&machine), "second bulk service failed");
    receive_host_ppp_bytes(&machine, &guest);
    for (unsigned turn = 0u; turn < 16u; turn++) {
        CHECK(run_service_boundary(&machine), "second token refill failed");
        receive_host_ppp_bytes(&machine, &guest);
    }
    CHECK(sink.packets == (batch ? 3u : 4u), "notification after lost tokens did not arrive");
    clock.now_ns += UINT64_C(5000000000);
    CHECK(run_service_boundary(&machine), "expiration service failed");
    CHECK(bridge.peek(bridge.ctx, sink.last, &reply) == 0u,
          "fully lost notifications retained their slots forever");
    vm_network_session_status(session, &status);
    CHECK(status.net_to_guest_lost == (batch ? 1u : 3u), "expired notification loss unreported");
    if (batch) {
        n = build_echo_request(packet, sizeof packet, 5u);
        CHECK(bridge.send(bridge.ctx, packet, n), "send after lost batch failed");
        for (unsigned turn = 0u; turn < 16u; turn++) {
            CHECK(run_service_boundary(&machine), "lost batch rearm failed");
            receive_host_ppp_bytes(&machine, &guest);
        }
        CHECK(sink.packets == 4u && bridge.peek(bridge.ctx, sink.last, &reply) == 32u,
              "lost whole-batch notification wedged the link");
    }
    vm_network_session_destroy(&session);
    CHECK(!bridge.ctx && !bridge.send && !bridge.peek && !bridge.consume && !bridge.finish &&
          !bridge.peek_large,
          "destroy left dangling host packet callbacks");
    s5l8900_free(&machine);
}

int main(void) {
    printf("S5LBox iOS PPP/NAT attachment tests\n");
    test_ppp_attachment_closes_the_uart_loop();
    test_nat_socket_owner_starts_without_a_flow();
    test_host_datagrams_cross_one_per_run_boundary();
    test_restored_guest_reopens_replaced_host_peer();
    test_restore_retry_uses_monotonic_host_time();
    test_invalid_create_fails_closed();
    test_bulk_packets_use_tokens_and_recover_lost_notifications(false);
    test_bulk_packets_use_tokens_and_recover_lost_notifications(true);
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
