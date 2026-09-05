/* See guest_packet_bridge.h. Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "guest_packet_bridge.h"
#include <string.h>

/* Audited XNU32 mbuf layout: m_next=0, m_len=8, m_data=12, flags=18,
 * pkthdr.len=20; external storage begins at 68 and its size is at 76.
 * The receive driver obtains a private 2048-byte cluster with mbuf_getpacket.
 * Refuse other receive layouts instead of guessing their writable capacity. */
#define MBUF_HEADER 80u
#define MAX_CHAIN 64u
#define GRANULE 1024u
typedef struct { uint8_t *data; uint32_t length; uint32_t pa; } span_t;
typedef struct { span_t span[3]; unsigned count; } map_t;

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void st32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8u * i));
}

void guest_packet_token(uint8_t token[GUEST_PACKET_TOKEN_SIZE], uint64_t id) {
    static const uint8_t magic[8] = {'S','5','L','B','P','K','T',1};
    memcpy(token, magic, sizeof magic);
    st32(token + 8, (uint32_t)id);
    st32(token + 12, (uint32_t)(id >> 32));
}

static bool map(guest_packet_bridge_t *b, arm_cpu_t *c, uint32_t va,
                 uint32_t n, bool write, map_t *out) {
    out->count = 0;
    if (!b->ram || !n || va < UINT32_C(0xc0000000) ||
        (uint64_t)va + n > UINT64_C(0x100000000) ||
        !(c->cp15.sctlr & 1u)) return false;
    while (n) {
        uint32_t size = GRANULE - (va & (GRANULE - 1u)), pa = 0;
        if (size > n) size = n;
        if (out->count == 3u ||
            arm_mmu_translate(c, va, write ? ARM_ACCESS_WRITE : ARM_ACCESS_READ,
                              true, &pa) || pa < b->ram_base ||
            (uint64_t)pa - b->ram_base > b->ram_size ||
            size > b->ram_size - ((uint64_t)pa - b->ram_base)) return false;
        out->span[out->count++] = (span_t){
            b->ram + ((uint64_t)pa - b->ram_base), size, pa};
        va += size;
        n -= size;
    }
    return true;
}
static void copy_from(const map_t *m, uint8_t *out) {
    for (unsigned i = 0; i < m->count; i++) {
        memcpy(out, m->span[i].data, m->span[i].length);
        out += m->span[i].length;
    }
}
static void copy_to(const map_t *m, const uint8_t *in) {
    for (unsigned i = 0; i < m->count; i++) {
        memcpy(m->span[i].data, in, m->span[i].length);
        in += m->span[i].length;
    }
}
static bool overlap(const map_t *a, const map_t *b) {
    for (unsigned i = 0; i < a->count; i++)
        for (unsigned j = 0; j < b->count; j++)
            if ((uint64_t)a->span[i].pa <
                    (uint64_t)b->span[j].pa + b->span[j].length &&
                (uint64_t)b->span[j].pa <
                    (uint64_t)a->span[i].pa + a->span[i].length) return true;
    return false;
}
static arm_svc_result_t call(arm_cpu_t *c, uint32_t target, uint32_t ret) {
    c->r[14] = ret;
    if (target & 1u) { c->cpsr |= ARM_CPSR_T; c->r[15] = target & ~1u; }
    else { c->cpsr &= ~ARM_CPSR_T; c->r[15] = target & ~3u; }
    return ARM_SVC_REDIRECTED;
}
static arm_svc_result_t fail(guest_packet_bridge_t *b) {
    b->failures++;
    return ARM_SVC_ERROR;
}

static arm_svc_result_t transmit(guest_packet_bridge_t *b, arm_cpu_t *c,
                                  uint32_t pc) {
    if (!b->send) return call(c, c->r[6], pc + 4u);
    uint8_t packet[GUEST_PACKET_MTU + 4u], header[MBUF_HEADER];
    uint32_t m = c->r[0], total = 0u, declared = 0u;
    uint32_t seen[MAX_CHAIN];
    unsigned count = 0u;
    while (m) {
        map_t meta, data;
        if (count == MAX_CHAIN || (m & 3u)) return fail(b);
        for (unsigned i = 0; i < count; i++) if (seen[i] == m) return fail(b);
        seen[count++] = m;
        if (!map(b, c, m, MBUF_HEADER, false, &meta)) return fail(b);
        copy_from(&meta, header);
        uint32_t n = ld32(header + 8u);
        if (count == 1u) declared = ld32(header + 20u);
        if (n > sizeof packet - total) goto fallback;
        if (n) {
            if (!map(b, c, ld32(header + 12u), n, false, &data)) return fail(b);
            copy_from(&data, packet + total);
        }
        total += n;
        m = ld32(header);
    }
    /* ppp_link_send may compress address/control and protocol fields. Accept
     * both negotiated forms, but never offload LCP/IPCP or compressed IP. */
    uint32_t offset = 0u;
    if (total >= 2u && packet[0] == 0xffu && packet[1] == 3u) offset = 2u;
    if (total > offset && packet[offset] == 0u) offset++;
    if (total <= offset || packet[offset++] != 0x21u ||
        total - offset < 20u || total - offset > GUEST_PACKET_MTU ||
        total != declared || (packet[offset] >> 4) != 4u) goto fallback;
    if (!b->send(b->ctx, packet + offset, total - offset)) goto fallback;
    b->tx_packets++;
    b->tx_bytes += total - offset;
    /* Native mbuf_freem releases the original chain; its A32 continuation
     * returns success using the original function's saved stack/registers. */
    c->r[0] = c->r[5];
    return call(c, b->sites.free_thumb_pc | 1u, b->sites.tx_done_pc);
fallback:
    b->tx_fallback++;
    return call(c, c->r[6], pc + 4u);
}

static arm_svc_result_t receive(guest_packet_bridge_t *b, arm_cpu_t *c) {
    /* Emulate the replaced instruction exactly for every ordinary PPP frame. */
    c->r[1] = c->r[6] - 2u;
    if (c->r[1] != 4u + GUEST_PACKET_TOKEN_SIZE) return ARM_SVC_HANDLED;
    uint8_t header[MBUF_HEADER], token[4u + GUEST_PACKET_TOKEN_SIZE], magic[16];
    map_t meta, data, length;
    uint32_t m = c->r[4];
    if ((m & 3u) || !map(b, c, m, MBUF_HEADER, false, &meta)) return fail(b);
    copy_from(&meta, header);
    if (ld32(header + 8u) != sizeof token ||
        !map(b, c, ld32(header + 12u), sizeof token, false, &data))
        return ARM_SVC_HANDLED;
    copy_from(&data, token);
    guest_packet_token(magic, 0u);
    if (memcmp(token, "\xff\x03\x00\x21", 4u) ||
        memcmp(token + 4u, magic, 8u)) return ARM_SVC_HANDLED;
    const uint8_t *packet = NULL;
    size_t n = b->peek && b->consume ? b->peek(b->ctx, token + 4u, &packet) : 0u;
    if (!n) {
        /* A checkpoint contains guest UART bytes, never live host sockets or
         * this queue. Dispose of stale notifications, not a fabricated IP. */
        b->stale_tokens++;
        c->r[0] = m;
        return call(c, b->sites.free_thumb_pc | 1u, b->sites.rx_drop_pc);
    }
    uint32_t data_va = ld32(header + 12u), base = ld32(header + 68u);
    uint32_t capacity = ld32(header + 76u);
    if (!packet || n < 20u || n > GUEST_PACKET_MTU || ld32(header) != 0u ||
        (header[18] & 3u) != 3u || data_va < base ||
        (uint64_t)data_va - base + n + 4u > capacity || capacity != 2048u ||
        !map(b, c, data_va + 4u, (uint32_t)n, true, &data) ||
        !map(b, c, m + 8u, 4u, true, &length) || overlap(&data, &meta))
        return fail(b);
    /* Preflight every destination before the first write or queue consume. */
    copy_to(&data, packet);
    uint8_t size[4];
    st32(size, (uint32_t)n + 4u);
    copy_to(&length, size);
    c->r[1] = (uint32_t)n + 4u; /* native mbuf_pkthdr_setlen follows the SVC */
    b->consume(b->ctx);
    b->rx_packets++;
    b->rx_bytes += n;
    return ARM_SVC_HANDLED;
}

arm_svc_result_t guest_packet_bridge_svc(guest_packet_bridge_t *b,
                                        arm_cpu_t *c, uint32_t pc,
                                        uint32_t encoding) {
    if (!b || !c) return ARM_SVC_UNHANDLED;
    bool rx = pc == b->sites.rx_pc && encoding == GUEST_PACKET_RX_SVC;
    bool tx = pc == b->sites.tx_pc && encoding == GUEST_PACKET_TX_SVC;
    if (!rx && !tx) return ARM_SVC_UNHANDLED;
    if ((c->cpsr & (ARM_CPSR_T | 0x1fu)) != ARM_MODE_SVC || !c->bus)
        return fail(b);
    return rx ? receive(b, c) : transmit(b, c, pc);
}
