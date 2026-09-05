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

#define RX_CLUSTERS (GUEST_PACKET_RX_MAX / 2048u + 1u)
typedef struct {
    uint32_t m, next, data_va, room;
    map_t meta, data, fields, next_field;
} rx_cluster_t;
typedef struct {
    rx_cluster_t node[RX_CLUSTERS];
    map_t link, reserve;
    unsigned count;
    uint32_t capacity, mru;
    bool large;
} rx_chain_t;

/* Borrow only native-allocated, private clusters under the driver's lock.
 * For a notification the first mbuf is already detached; unused clusters
 * still belong to ld->inm. A batch starts with the whole chain at ld->inm. */
static int prepare_rx(guest_packet_bridge_t *b, arm_cpu_t *c, uint32_t first,
                       bool detached, rx_chain_t *out) {
    memset(out, 0, sizeof *out);
    out->large = b->peek_large != NULL;
    uint8_t word[4], header[MBUF_HEADER];
    if (!map(b, c, c->r[5] + 0xf8u, 4u, true, &out->link)) return -1;
    copy_from(&out->link, word);
    uint32_t unused = ld32(word), m = first;
    if (out->large) {
        if (!map(b, c, c->r[5] + 0x8cu, 4u, true, &out->reserve)) return -1;
        copy_from(&out->reserve, word);
        out->mru = ld32(word);
    }
    while (m && out->count < (out->large ? RX_CLUSTERS : 1u)) {
        for (unsigned i = 0u; i < out->count; i++)
            if (out->node[i].m == m) return -1;
        rx_cluster_t *v = &out->node[out->count];
        v->m = m;
        if ((m & 3u) || !map(b, c, m, MBUF_HEADER, false, &v->meta)) return -1;
        copy_from(&v->meta, header);
        uint32_t base = ld32(header + 68u);
        bool used_head = detached && out->count == 0u;
        v->data_va = used_head ? ld32(header + 12u) : base;
        v->next = used_head ? unused : ld32(header);
        if ((header[18] & 3u) != 3u || ld32(header + 76u) != 2048u ||
            v->data_va < base || (uint64_t)v->data_va - base >= 2048u ||
            (used_head && ld32(header)) || (!out->large && ld32(header))) return 0;
        v->room = 2048u - (v->data_va - base);
        if (!map(b, c, v->data_va, v->room, true, &v->data) ||
            !map(b, c, m + 8u, 8u, true, &v->fields) ||
            !map(b, c, m, 4u, true, &v->next_field)) return -1;
        out->capacity += v->room;
        out->count++;
        m = v->next;
    }
    for (unsigned i = 0u; m && i < out->count; i++)
        if (out->node[i].m == m) return -1;
    if (!out->count || out->capacity < 24u) return 0;
    /* Preflight physical aliasing, including distinct virtual aliases. No
     * payload may overwrite allocator metadata, links or another cluster. */
    for (unsigned i = 0u; i < out->count; i++) {
        const rx_cluster_t *a = &out->node[i];
        if (overlap(&a->meta, &out->link) || overlap(&a->data, &out->link) ||
            (out->large && (overlap(&a->meta, &out->reserve) ||
                           overlap(&a->data, &out->reserve)))) return -1;
        for (unsigned j = 0u; j < out->count; j++) {
            const rx_cluster_t *d = &out->node[j];
            if (overlap(&a->data, &d->meta) ||
                (i != j && (overlap(&a->data, &d->data) ||
                             overlap(&a->meta, &d->meta)))) return -1;
        }
    }
    out->capacity -= 4u;
    uint32_t limit = out->large ? GUEST_PACKET_RX_MAX : GUEST_PACKET_MTU;
    if (out->capacity > limit) out->capacity = limit;
    return 1;
}

static void commit_rx(const rx_chain_t *chain, const uint8_t *packet, size_t n) {
    uint8_t framed[GUEST_PACKET_RX_MAX + 4u], fields[8];
    memcpy(framed, "\xff\x03\x00\x21", 4u);
    memcpy(framed + 4u, packet, n);
    uint32_t left = (uint32_t)n + 4u, offset = 0u;
    for (unsigned i = 0u; left; i++) {
        const rx_cluster_t *v = &chain->node[i];
        uint32_t size = left < v->room ? left : v->room, copied = 0u;
        for (unsigned s = 0u; copied < size; s++) {
            uint32_t part = v->data.span[s].length;
            if (part > size - copied) part = size - copied;
            memcpy(v->data.span[s].data, framed + offset + copied, part);
            copied += part;
        }
        st32(fields, size); st32(fields + 4u, v->data_va);
        copy_to(&v->fields, fields);
        left -= size;
        offset += size;
        st32(fields, left ? v->next : 0u);
        copy_to(&v->next_field, fields);
        if (!left) {
            st32(fields, v->next);
            copy_to(&chain->link, fields);
        }
    }
    /* This is the private serial driver's allocation reserve, not ifnet MTU
     * or the negotiated wire MRU. Native getm owns replenishment, including
     * partial allocation failure; peek_large never exceeds actual capacity. */
    if (chain->large && chain->mru < GUEST_PACKET_RX_MAX) {
        st32(fields, GUEST_PACKET_RX_MAX);
        copy_to(&chain->reserve, fields);
    }
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
    map_t meta, data;
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
    rx_chain_t chain;
    if (prepare_rx(b, c, m, true, &chain) != 1) return fail(b);
    if (b->peek_large) n = b->peek_large(b->ctx, token + 4u, chain.capacity, &packet);
    if (!packet || n < 20u || n > chain.capacity) return fail(b);
    commit_rx(&chain, packet, n);
    c->r[1] = (uint32_t)n + 4u; /* native mbuf_pkthdr_setlen follows the SVC */
    b->consume(b->ctx);
    b->rx_packets++;
    b->rx_bytes += n;
    if (b->finish && b->sites.batch_pc) {
        b->batch_link = c->r[5];
        b->batch_frame = c->r[7];
        b->batch_left = GUEST_PACKET_BATCH_MAX - 1u;
        b->rx_batches++;
    }
    return ARM_SVC_HANDLED;
}

static arm_svc_result_t finish_batch(guest_packet_bridge_t *b, arm_cpu_t *c) {
    b->batch_link = b->batch_frame = b->batch_left = 0u;
    if (b->finish) b->finish(b->ctx);
    c->r[15] = b->sites.rx_unlock_pc;
    return ARM_SVC_REDIRECTED;
}

static arm_svc_result_t receive_batch(guest_packet_bridge_t *b, arm_cpu_t *c) {
    /* This site is reached with the native PPP mutex still held, after the
     * previous packet has been enqueued and getm has replenished ld->inm.
     * A restored checkpoint has no host batch owner: just unlock normally. */
    if (!b->batch_link || b->batch_link != c->r[5] ||
        b->batch_frame != c->r[7]) {
        c->r[15] = b->sites.rx_unlock_pc;
        return ARM_SVC_REDIRECTED;
    }
    if (!b->batch_left || !b->peek || !b->consume || !b->finish)
        return finish_batch(b, c);
    const uint8_t *packet = NULL;
    size_t n = b->peek(b->ctx, NULL, &packet);
    if (!n) return finish_batch(b, c);

    map_t link;
    uint8_t word[4];
    if (!map(b, c, b->batch_link + 0xf8u, 4u, true, &link)) return fail(b);
    copy_from(&link, word);
    uint32_t m = ld32(word);
    /* MBUF_DONTWAIT allocation failure is normal backpressure. Leave the
     * queued packet owned by the host and request a later notification. */
    if (!m) return finish_batch(b, c);
    rx_chain_t chain;
    int ready = prepare_rx(b, c, m, false, &chain);
    if (ready < 0) return fail(b);
    if (!ready) return finish_batch(b, c);
    if (b->peek_large) n = b->peek_large(b->ctx, NULL, chain.capacity, &packet);
    if (!packet || n < 20u || n > chain.capacity) return fail(b);
    commit_rx(&chain, packet, n);
    b->consume(b->ctx);
    b->batch_left--;
    b->rx_packets++;
    b->rx_bytes += n;
    b->rx_batched++;
    c->r[4] = m;
    c->r[6] = (uint32_t)n + 6u; /* native length includes removed FCS */
    c->r[15] = b->sites.rx_enqueue_pc;
    return ARM_SVC_REDIRECTED;
}

arm_svc_result_t guest_packet_bridge_svc(guest_packet_bridge_t *b,
                                        arm_cpu_t *c, uint32_t pc,
                                        uint32_t encoding) {
    if (!b || !c) return ARM_SVC_UNHANDLED;
    bool rx = pc == b->sites.rx_pc && encoding == GUEST_PACKET_RX_SVC;
    bool tx = pc == b->sites.tx_pc && encoding == GUEST_PACKET_TX_SVC;
    bool batch = pc == b->sites.batch_pc && encoding == GUEST_PACKET_BATCH_SVC;
    if (!rx && !tx && !batch) return ARM_SVC_UNHANDLED;
    if ((c->cpsr & (ARM_CPSR_T | 0x1fu)) != ARM_MODE_SVC || !c->bus)
        return fail(b);
    return rx ? receive(b, c) : batch ? receive_batch(b, c) : transmit(b, c, pc);
}
