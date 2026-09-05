/* Native-buffer transport checks. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "guest_packet_bridge.h"
#include "soc.h"
#include <stdio.h>
#include <string.h>

static s5l8900_t machine;
static guest_packet_bridge_t bridge;
static uint8_t payload[1500], captured[1500], expected_token[16];
static uint8_t large_payload[GUEST_PACKET_RX_MAX];
static size_t large_length, peek_capacity;
static unsigned sent, consumed, finished, passed, failed;
static bool accept_send, have_rx;
#define CHECK(x) do { if (x) passed++; else { failed++; \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)
#define VA UINT32_C(0xc0000000)
#define RX (VA + 0x800u)
#define TX (VA + 0x804u)
#define BATCH (VA + 0x808u)
static void put(uint32_t off, uint32_t v) {
    for (unsigned i = 0; i < 4u; i++) machine.ram[off + i] = (uint8_t)(v >> (8u*i));
}
static uint32_t get(uint32_t off) {
    const uint8_t *p = machine.ram + off;
    return p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static bool send_packet(void *ctx, const uint8_t *p, size_t n) {
    (void)ctx;
    if (!accept_send) return false;
    CHECK(n == sizeof payload);
    memcpy(captured, p, n);
    sent++;
    return true;
}
static size_t peek_packet(void *ctx, const uint8_t *token, const uint8_t **p) {
    (void)ctx;
    if (!have_rx || (token && memcmp(token, expected_token, 16u))) return 0;
    *p = payload;
    return sizeof payload;
}
static void consume_packet(void *ctx) { (void)ctx; consumed++; have_rx = false; }
static void finish_packets(void *ctx) { (void)ctx; finished++; }
static size_t peek_large_packet(void *ctx, const uint8_t *token, size_t capacity,
                                 const uint8_t **p) {
    (void)ctx; (void)token;
    peek_capacity = capacity;
    *p = large_payload;
    return large_length < capacity ? large_length : capacity;
}
static arm_svc_result_t handler(void *ctx, arm_cpu_t *c, uint32_t pc, uint32_t op) {
    return guest_packet_bridge_svc(ctx, c, pc, op);
}
static void reset(void) {
    memset(machine.ram, 0, machine.ram_size);
    arm_reset(&machine.cpu, &machine.bus);
    machine.cpu.cpsr = ARM_MODE_SVC;
    machine.cpu.cp15.ttbr0 = 0x4000u;
    machine.cpu.cp15.dacr = 1u;
    machine.cpu.cp15.sctlr |= ARM_SCTLR_M;
    put(0x4000u + 0xc00u*4u, 0xc02u);
    put(0x800u, GUEST_PACKET_RX_SVC);
    put(0x804u, GUEST_PACKET_TX_SVC);
    put(0x808u, GUEST_PACKET_BATCH_SVC);
    bridge = (guest_packet_bridge_t){
        .sites = {RX, TX, VA+0x900u, VA+0xa00u, VA+0xb00u,
                  BATCH, VA+0xd00u, VA+0xe00u},
        .ram = machine.ram, .ram_base = 0u, .ram_size = machine.ram_size,
        .send = send_packet, .peek = peek_packet, .consume = consume_packet
    };
    arm_bus_set_privileged_svc_handler(&machine.bus, handler, &bridge);
    for (unsigned i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i*17u);
    payload[0] = 0x45u;
    guest_packet_token(expected_token, UINT64_C(0x1122334455667788));
    sent = consumed = finished = 0;
    accept_send = have_rx = true;
}
static void mbuf(uint32_t off, uint32_t data, uint32_t n) {
    put(off+8u, n);
    put(off+12u, VA+data);
    put(off+16u, 0x00030001u); /* M_EXT | M_PKTHDR, MT_DATA */
    put(off+20u, n);
    put(off+68u, VA+data);
    put(off+76u, 2048u);
}
static void setup_rx(void) {
    mbuf(0x1000u, 0x2f00u, 20u); /* payload crosses 1 KiB permission boundaries */
    memcpy(machine.ram+0x2f00u, "\xff\x03\x00\x21", 4u);
    memcpy(machine.ram+0x2f04u, expected_token, 16u);
    machine.cpu.r[0] = machine.cpu.r[4] = VA+0x1000u;
    machine.cpu.r[5] = VA+0x2000u;
    machine.cpu.r[6] = 22u;
    machine.cpu.r[15] = RX;
}
static void setup_tx(void) {
    mbuf(0x1000u, 0x2f00u, 1504u);
    memcpy(machine.ram+0x2f00u, "\xff\x03\x00\x21", 4u);
    memcpy(machine.ram+0x2f04u, payload, sizeof payload);
    machine.cpu.r[0] = machine.cpu.r[5] = VA+0x1000u;
    machine.cpu.r[6] = VA+0xc01u;
    machine.cpu.r[15] = TX;
}
static void test_receive(void) {
    reset(); setup_rx();
    CHECK(arm_step(&machine.cpu) == ARM_OK);
    CHECK(bridge.rx_packets == 1u && bridge.rx_bytes == 1500u && consumed == 1u);
    CHECK(machine.cpu.r[15] == RX+4u && machine.cpu.r[1] == 1504u);
    CHECK(get(0x1008u) == 1504u && get(0x1014u) == 20u);
    CHECK(!memcmp(machine.ram+0x2f04u, payload, sizeof payload));
    CHECK(machine.ram[0x2f03u] == 0x21u && machine.ram[0x34e0u] == 0u);
    reset(); setup_rx(); have_rx = false;
    CHECK(arm_step(&machine.cpu) == ARM_OK);
    CHECK(consumed == 0u && bridge.stale_tokens == 1u && get(0x1008u) == 20u);
    CHECK(machine.cpu.r[15] == VA+0x900u && machine.cpu.r[14] == VA+0xb00u);
    CHECK(machine.cpu.cpsr & ARM_CPSR_T);
    reset(); setup_rx(); bridge.peek = NULL; bridge.consume = NULL;
    CHECK(arm_step(&machine.cpu) == ARM_OK && bridge.stale_tokens == 1u);
    reset(); setup_rx(); machine.ram[0x2f04u] ^= 1u;
    CHECK(arm_step(&machine.cpu) == ARM_OK && bridge.rx_packets == 0u);
    CHECK(machine.cpu.r[1] == 20u && machine.cpu.r[15] == RX+4u);
    reset(); setup_rx(); put(0x104cu, 128u);
    CHECK(arm_step(&machine.cpu) == ARM_HALT);
    CHECK(bridge.failures == 1u && consumed == 0u && get(0x1008u) == 20u);
    CHECK(!memcmp(machine.ram+0x2f04u, expected_token, 16u));
    CHECK(machine.cpu.r[15] == RX);
    reset(); setup_rx(); put(0x1000u, VA+0x1100u);
    CHECK(arm_step(&machine.cpu) == ARM_HALT && consumed == 0u);
    reset(); setup_rx(); put(0x100cu, VA+0xffffcu);
    CHECK(arm_step(&machine.cpu) == ARM_OK && consumed == 0u); /* non-token: native */
}
static void test_transmit(void) {
    reset(); setup_tx();
    CHECK(arm_step(&machine.cpu) == ARM_OK);
    CHECK(sent == 1u && bridge.tx_packets == 1u && bridge.tx_bytes == 1500u);
    CHECK(!memcmp(captured, payload, sizeof payload));
    CHECK(machine.cpu.r[15] == VA+0x900u && machine.cpu.r[14] == VA+0xa00u);
    CHECK(machine.cpu.r[0] == VA+0x1000u && (machine.cpu.cpsr & ARM_CPSR_T));
    reset(); setup_tx();
    mbuf(0x1100u, 0x3800u, 1000u);
    memcpy(machine.ram+0x3800u, payload+500u, 1000u);
    put(0x1000u, VA+0x1100u); put(0x1008u, 504u);
    CHECK(arm_step(&machine.cpu) == ARM_OK && sent == 1u);
    CHECK(!memcmp(captured, payload, sizeof payload));
    reset(); setup_tx(); accept_send = false;
    CHECK(arm_step(&machine.cpu) == ARM_OK && sent == 0u);
    CHECK(machine.cpu.r[15] == VA+0xc00u && machine.cpu.r[14] == TX+4u);
    CHECK(bridge.tx_fallback == 1u);
    reset(); setup_tx(); bridge.send = NULL;
    CHECK(arm_step(&machine.cpu) == ARM_OK && machine.cpu.r[15] == VA+0xc00u);
    reset(); setup_tx(); machine.ram[0x2f02u] = 0xc0u;
    CHECK(arm_step(&machine.cpu) == ARM_OK && sent == 0u && bridge.tx_fallback == 1u);
    reset(); setup_tx(); put(0x1000u, VA+0x1000u);
    CHECK(arm_step(&machine.cpu) == ARM_HALT && sent == 0u);
    reset(); setup_tx(); put(0x100cu, VA+0xfff00u);
    CHECK(arm_step(&machine.cpu) == ARM_HALT && sent == 0u);
}
static void setup_batch(void) {
    reset(); setup_rx();
    bridge.finish = finish_packets;
    CHECK(arm_step(&machine.cpu) == ARM_OK && consumed == 1u);
    CHECK(bridge.batch_link == VA+0x2000u && bridge.rx_batches == 1u);
    /* Simulate the unmodified native enqueue/getm supplying its next mbuf. */
    mbuf(0x1100u, 0x3700u, 0u);
    put(0x20f8u, VA+0x1100u);
    machine.cpu.r[15] = BATCH;
    have_rx = true;
}
static void test_batch(void) {
    setup_batch();
    CHECK(arm_step(&machine.cpu) == ARM_OK);
    CHECK(consumed == 2u && bridge.rx_batched == 1u && bridge.rx_bytes == 3000u);
    CHECK(machine.cpu.r[15] == VA+0xd00u && machine.cpu.r[4] == VA+0x1100u &&
          machine.cpu.r[6] == 1506u && get(0x20f8u) == 0u);
    CHECK(get(0x1108u) == 1504u && get(0x1114u) == 0u);
    CHECK(!memcmp(machine.ram+0x3700u, "\xff\x03\x00\x21", 4u) &&
          !memcmp(machine.ram+0x3704u, payload, sizeof payload));
    machine.cpu.r[15] = BATCH;
    CHECK(arm_step(&machine.cpu) == ARM_OK && finished == 1u);
    CHECK(machine.cpu.r[15] == VA+0xe00u && bridge.batch_link == 0u);

    setup_batch(); put(0x20f8u, 0u); /* native allocation failure */
    CHECK(arm_step(&machine.cpu) == ARM_OK && consumed == 1u && have_rx);
    CHECK(finished == 1u && bridge.failures == 0u);
    setup_batch(); put(0x114cu, 4096u); /* valid but unsupported allocation */
    CHECK(arm_step(&machine.cpu) == ARM_OK && consumed == 1u && finished == 1u);
    CHECK(get(0x20f8u) == VA+0x1100u && get(0x1108u) == 0u);
    setup_batch(); put(0x110cu, VA+0xffff0u); put(0x1144u, VA+0xffff0u);
    CHECK(arm_step(&machine.cpu) == ARM_HALT && consumed == 1u);
    CHECK(get(0x20f8u) == VA+0x1100u && get(0x1108u) == 0u);
    setup_batch(); put(0x110cu, VA+0x1100u); put(0x1144u, VA+0x1100u);
    CHECK(arm_step(&machine.cpu) == ARM_HALT && consumed == 1u); /* metadata alias */
    setup_batch(); machine.cpu.r[7] = 4u; /* unrelated native invocation */
    CHECK(arm_step(&machine.cpu) == ARM_OK && consumed == 1u && finished == 0u);
    CHECK(machine.cpu.r[15] == VA+0xe00u && bridge.batch_link != 0u);
    setup_batch(); bridge.batch_link = 0u; /* unsaved owner after restore */
    CHECK(arm_step(&machine.cpu) == ARM_OK && consumed == 1u && finished == 0u);
    setup_batch();
    bool bounded = true;
    for (unsigned i = 1u; i < GUEST_PACKET_BATCH_MAX; i++) {
        put(0x20f8u, VA+0x1100u); have_rx = true; machine.cpu.r[15] = BATCH;
        if (arm_step(&machine.cpu) != ARM_OK) bounded = false;
    }
    CHECK(bounded && consumed == GUEST_PACKET_BATCH_MAX && bridge.batch_left == 0u);
    put(0x20f8u, VA+0x1100u); have_rx = true; machine.cpu.r[15] = BATCH;
    CHECK(arm_step(&machine.cpu) == ARM_OK && finished == 1u && have_rx);
    CHECK(consumed == GUEST_PACKET_BATCH_MAX && get(0x20f8u) == VA+0x1100u);
}
static void test_user_svc_stays_in_guest(void) {
    reset(); setup_tx(); machine.cpu.cpsr = ARM_MODE_USR;
    CHECK(arm_step(&machine.cpu) == ARM_OK);
    CHECK(sent == 0u && bridge.failures == 0u && machine.cpu.r[15] != VA+0x900u);
    CHECK(guest_packet_bridge_svc(&bridge, &machine.cpu, RX+4u,
                                  GUEST_PACKET_RX_SVC) == ARM_SVC_UNHANDLED);
}
static void setup_large_receive(unsigned extra) {
    reset(); setup_rx();
    bridge.peek_large = peek_large_packet;
    bridge.finish = finish_packets;
    large_length = sizeof large_payload;
    for (unsigned i = 0u; i < sizeof large_payload; i++)
        large_payload[i] = (uint8_t)((i * 31u) ^ (i >> 9u));
    large_payload[0] = 0x45u;
    put(0x208cu, 1500u);
    for (unsigned i = 1u; i <= extra; i++) {
        uint32_t off = 0x1000u + i * 0x100u;
        mbuf(off, 0x5000u + (i - 1u) * 0x800u, 0u);
        put(off, i < extra ? VA + off + 0x100u : 0u);
    }
    put(0x20f8u, extra ? VA+0x1100u : 0u);
}
static void test_large_receive(void) {
    setup_large_receive(9u);
    CHECK(arm_step(&machine.cpu) == ARM_OK && consumed == 1u);
    CHECK(peek_capacity == GUEST_PACKET_RX_MAX && bridge.rx_bytes == large_length);
    CHECK(machine.cpu.r[1] == large_length+4u && get(0x208cu) == GUEST_PACKET_RX_MAX);
    CHECK(get(0x1000u) == VA+0x1100u && get(0x1008u) == 2048u);
    CHECK(get(0x1808u) == 4u && get(0x1800u) == 0u && get(0x20f8u) == VA+0x1900u);
    CHECK(get(0x1908u) == 0u && get(0x1014u) == 20u);
    CHECK(!memcmp(machine.ram+0x2f04u, large_payload, 2044u));
    bool exact = true;
    for (unsigned i = 1u; i < 9u; i++) {
        size_t offset = 2044u + (i-1u)*2048u;
        size_t n = large_length-offset < 2048u ? large_length-offset : 2048u;
        if (memcmp(machine.ram+0x5000u+(i-1u)*2048u, large_payload+offset, n)) exact=false;
    }
    CHECK(exact);
    setup_large_receive(1u); /* getm allocated only part of its requested reserve */
    CHECK(arm_step(&machine.cpu) == ARM_OK && peek_capacity == 4092u);
    CHECK(get(0x1108u) == 2048u && get(0x20f8u) == 0u);
    setup_large_receive(9u); large_length = 1500u;
    CHECK(arm_step(&machine.cpu) == ARM_OK && get(0x1000u) == 0u);
    CHECK(get(0x20f8u) == VA+0x1100u && get(0x1108u) == 0u);
    setup_large_receive(9u); put(0x1800u, VA+0x1000u);
    CHECK(arm_step(&machine.cpu) == ARM_HALT && consumed == 0u);
    CHECK(get(0x1008u) == 20u && get(0x20f8u) == VA+0x1100u);
    setup_large_receive(9u); put(0x1844u, VA+0x2f00u);
    CHECK(arm_step(&machine.cpu) == ARM_HALT && consumed == 0u);
    setup_large_receive(9u); put(0x1844u, VA+0xffff0u);
    CHECK(arm_step(&machine.cpu) == ARM_HALT && consumed == 0u);
    CHECK(get(0x208cu) == 1500u && get(0x1008u) == 20u);
    setup_batch(); bridge.peek_large = peek_large_packet;
    large_length = 3000u;
    mbuf(0x1200u, 0x5000u, 0u); put(0x1100u, VA+0x1200u);
    CHECK(arm_step(&machine.cpu) == ARM_OK && consumed == 2u);
    CHECK(get(0x1108u) == 2048u && get(0x1208u) == 956u && get(0x20f8u) == 0u);
    CHECK(machine.cpu.r[6] == 3006u && get(0x1200u) == 0u);
}
int main(void) {
    if (!s5l8900_init(&machine, 0u, 1u<<20)) return 1;
    test_receive(); test_transmit(); test_batch(); test_user_svc_stays_in_guest();
    test_large_receive();
    s5l8900_free(&machine);
    printf("guest packet bridge: %u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
