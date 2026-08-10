/*
 * Host-side tests for the buffer between a finger and the touch controller.
 *
 * The rule under test is one sentence — a MOVED may be replaced by a newer
 * MOVED, an edge may not be dropped — and every way of getting it wrong loses
 * input in a way that is indistinguishable, from the outside, from the emulator
 * being broken. A tap whose MAKE_TOUCH was coalesced away is not a tap. A tap
 * whose BREAK_TOUCH was coalesced away leaves the guest believing a finger is
 * still down. Neither produces an error anywhere.
 *
 * So the expectations below are written out longhand against hand-computed
 * outcomes rather than against the queue's own behaviour, and the phase mapping
 * is checked field by field rather than by round-tripping it through the
 * encoder. A format checked against itself checks nothing.
 */
#include "VMTouchQueue.h"

#include <stdio.h>
#include <string.h>

static unsigned tests;
static unsigned failed;

#define CHECK(expr, ...) do {                                                \
    tests++;                                                                 \
    if (!(expr)) {                                                           \
        failed++;                                                            \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                 \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
    }                                                                        \
} while (0)

/* ------------------------------------------------------ the phase mapping --- */

static void test_phase_mapping(void) {
    s5l_mt_contact_t c;

    CHECK(vm_touch_contact_from_ui(VM_TOUCH_BEGAN, 10, 20, &c),
          "BEGAN at a valid point must translate");
    CHECK(c.phase == MTZ2_PHASE_MAKE_TOUCH,
          "BEGAN -> MAKE_TOUCH, got %u", (unsigned)c.phase);
    CHECK(c.x == 10 && c.y == 20,
          "coordinates pass through unchanged, got %u,%u",
          (unsigned)c.x, (unsigned)c.y);
    CHECK(c.id == VM_TOUCH_CONTACT_ID,
          "path id must be the constant, got %u", (unsigned)c.id);
    CHECK(c.id >= 1u && c.id <= 11u,
          "path id must be inside the parser's 1..11, got %u", (unsigned)c.id);
    CHECK(c.pressure > 0u, "a finger that is down carries amplitude");
    CHECK(c.major == VM_TOUCH_MAJOR && c.minor == VM_TOUCH_MINOR,
          "ellipse axes come from the named constants");

    CHECK(vm_touch_contact_from_ui(VM_TOUCH_MOVED, 0, 0, &c), "MOVED at 0,0");
    CHECK(c.phase == MTZ2_PHASE_TOUCHING,
          "MOVED -> TOUCHING, got %u", (unsigned)c.phase);
    CHECK(c.pressure > 0u, "a moving finger is still down");

    CHECK(vm_touch_contact_from_ui(VM_TOUCH_ENDED, 5, 5, &c), "ENDED");
    CHECK(c.phase == MTZ2_PHASE_BREAK_TOUCH,
          "ENDED -> BREAK_TOUCH, got %u", (unsigned)c.phase);
    CHECK(c.pressure == 0u,
          "a lifted finger carries zero amplitude, got %u",
          (unsigned)c.pressure);

    CHECK(vm_touch_contact_from_ui(VM_TOUCH_CANCELLED, 5, 5, &c), "CANCELLED");
    CHECK(c.phase == MTZ2_PHASE_BREAK_TOUCH,
          "CANCELLED -> BREAK_TOUCH too: the device has no other way to say a "
          "finger is gone, and sending nothing would leave it down");
    CHECK(c.pressure == 0u, "a cancelled finger also carries zero amplitude");
}

static void test_phase_mapping_rejects(void) {
    s5l_mt_contact_t c;
    memset(&c, 0xAA, sizeof c);

    CHECK(!vm_touch_contact_from_ui(VM_TOUCH_BEGAN, -1, 0, &c),
          "a negative x is off the panel");
    CHECK(!vm_touch_contact_from_ui(VM_TOUCH_BEGAN, 0, -1, &c),
          "a negative y is off the panel");
    CHECK(!vm_touch_contact_from_ui(VM_TOUCH_BEGAN,
                                    (int)S5L_MT_PANEL_W, 0, &c),
          "x == width is one past the last column");
    CHECK(!vm_touch_contact_from_ui(VM_TOUCH_BEGAN,
                                    0, (int)S5L_MT_PANEL_H, &c),
          "y == height is one past the last row");
    CHECK(vm_touch_contact_from_ui(VM_TOUCH_BEGAN,
                                   (int)S5L_MT_PANEL_W - 1,
                                   (int)S5L_MT_PANEL_H - 1, &c),
          "the last pixel IS on the panel");
    CHECK(!vm_touch_contact_from_ui((vm_touch_phase_t)99, 0, 0, &c),
          "a phase this app does not produce is refused");
    CHECK(!vm_touch_contact_from_ui(VM_TOUCH_BEGAN, 0, 0, NULL),
          "a null out pointer is refused rather than dereferenced");
}

/* ------------------------------------------------------------ the queue --- */

static s5l_mt_contact_t make(uint8_t phase, uint16_t x) {
    s5l_mt_contact_t c;
    memset(&c, 0, sizeof c);
    c.id    = VM_TOUCH_CONTACT_ID;
    c.phase = phase;
    c.x     = x;
    return c;
}

static void test_fifo_order(void) {
    vm_touch_queue_t q;
    vm_touch_queue_reset(&q);

    s5l_mt_contact_t out;
    CHECK(!vm_touch_queue_peek(&q, &out), "an empty queue has nothing to peek");

    for (uint16_t i = 0; i < 4; i++) {
        s5l_mt_contact_t c = make(MTZ2_PHASE_TOUCHING, i);
        CHECK(vm_touch_queue_push(&q, &c), "push %u below capacity", i);
    }
    CHECK(q.count == 4u, "four held, got %u", q.count);
    CHECK(q.queued == 4u, "four counted, got %llu",
          (unsigned long long)q.queued);

    for (uint16_t i = 0; i < 4; i++) {
        CHECK(vm_touch_queue_peek(&q, &out), "peek %u", i);
        CHECK(out.x == i, "oldest first: expected x=%u got %u",
              i, (unsigned)out.x);
        vm_touch_queue_pop(&q);
    }
    CHECK(q.count == 0u, "drained, got %u", q.count);
    vm_touch_queue_pop(&q);     /* must be harmless */
    CHECK(q.count == 0u, "popping an empty queue changes nothing");
}

static void test_wraparound(void) {
    /* Fill, drain, refill: the ring's head must wrap without reordering. */
    vm_touch_queue_t q;
    vm_touch_queue_reset(&q);
    s5l_mt_contact_t out;

    for (uint16_t i = 0; i < VM_TOUCH_QUEUE_CAP; i++) {
        s5l_mt_contact_t c = make(MTZ2_PHASE_TOUCHING, i);
        CHECK(vm_touch_queue_push(&q, &c), "fill %u", i);
    }
    for (uint16_t i = 0; i < 5; i++) vm_touch_queue_pop(&q);
    for (uint16_t i = 100; i < 105; i++) {
        s5l_mt_contact_t c = make(MTZ2_PHASE_TOUCHING, i);
        CHECK(vm_touch_queue_push(&q, &c), "refill %u", i);
    }
    CHECK(q.count == VM_TOUCH_QUEUE_CAP, "full again, got %u", q.count);

    uint16_t expect[VM_TOUCH_QUEUE_CAP];
    unsigned n = 0;
    for (uint16_t i = 5; i < VM_TOUCH_QUEUE_CAP; i++) expect[n++] = i;
    for (uint16_t i = 100; i < 105; i++)              expect[n++] = i;
    for (unsigned i = 0; i < n; i++) {
        CHECK(vm_touch_queue_peek(&q, &out), "wrapped peek %u", i);
        CHECK(out.x == expect[i], "wrapped order %u: expected %u got %u",
              i, (unsigned)expect[i], (unsigned)out.x);
        vm_touch_queue_pop(&q);
    }
}

static void test_moved_coalesces_when_full(void) {
    vm_touch_queue_t q;
    vm_touch_queue_reset(&q);

    for (uint16_t i = 0; i < VM_TOUCH_QUEUE_CAP; i++) {
        s5l_mt_contact_t c = make(MTZ2_PHASE_TOUCHING, i);
        (void)vm_touch_queue_push(&q, &c);
    }
    s5l_mt_contact_t newer = make(MTZ2_PHASE_TOUCHING, 999);
    CHECK(vm_touch_queue_push(&q, &newer),
          "a MOVED onto a full queue whose back is a MOVED is accepted");
    CHECK(q.count == VM_TOUCH_QUEUE_CAP, "still exactly full, got %u", q.count);
    CHECK(q.coalesced == 1u, "one coalesce counted, got %llu",
          (unsigned long long)q.coalesced);
    CHECK(q.dropped == 0u, "nothing was lost, got %llu",
          (unsigned long long)q.dropped);

    /* The newest position must be the one that survived, and it must be at the
     * BACK — coalescing must not reorder the gesture. */
    s5l_mt_contact_t out;
    for (unsigned i = 0; i < VM_TOUCH_QUEUE_CAP - 1u; i++) {
        CHECK(vm_touch_queue_peek(&q, &out), "drain %u", i);
        CHECK(out.x == (uint16_t)i, "front order preserved at %u, got %u",
              i, (unsigned)out.x);
        vm_touch_queue_pop(&q);
    }
    CHECK(vm_touch_queue_peek(&q, &out), "the last one");
    CHECK(out.x == 999, "the newer MOVED replaced the older at the back, "
          "got %u", (unsigned)out.x);
}

static void test_edges_are_never_coalesced(void) {
    vm_touch_queue_t q;
    s5l_mt_contact_t out;

    /* An incoming EDGE displaces the newest MOVED. It stays at the back, so
     * every surviving report remains in chronological order. */
    vm_touch_queue_reset(&q);
    for (uint16_t i = 0; i < VM_TOUCH_QUEUE_CAP; i++) {
        s5l_mt_contact_t c = make(MTZ2_PHASE_TOUCHING, i);
        (void)vm_touch_queue_push(&q, &c);
    }
    s5l_mt_contact_t lift = make(MTZ2_PHASE_BREAK_TOUCH, 500);
    CHECK(vm_touch_queue_push(&q, &lift),
          "a BREAK_TOUCH was dropped although a MOVED could make room");
    CHECK(q.dropped == 0u, "preserving the edge reported a dropped input");
    CHECK(q.coalesced == 1u, "the displaced MOVED was not counted");
    CHECK(vm_touch_queue_peek(&q, &out) && out.x == 0,
          "making room reordered the front of the queue");
    for (unsigned i = 0; i < VM_TOUCH_QUEUE_CAP - 1u; i++)
        vm_touch_queue_pop(&q);
    CHECK(vm_touch_queue_peek(&q, &out) &&
          out.phase == MTZ2_PHASE_BREAK_TOUCH && out.x == 500,
          "the preserved lift is not the last report");

    /* A queued EDGE at the back must not be overwritten by a MOVED. */
    vm_touch_queue_reset(&q);
    for (uint16_t i = 0; i < VM_TOUCH_QUEUE_CAP - 1u; i++) {
        s5l_mt_contact_t c = make(MTZ2_PHASE_TOUCHING, i);
        (void)vm_touch_queue_push(&q, &c);
    }
    s5l_mt_contact_t down = make(MTZ2_PHASE_MAKE_TOUCH, 700);
    CHECK(vm_touch_queue_push(&q, &down), "the edge fills the queue");
    s5l_mt_contact_t move = make(MTZ2_PHASE_TOUCHING, 800);
    CHECK(!vm_touch_queue_push(&q, &move),
          "a MOVED must not overwrite a queued MAKE_TOUCH — that would turn a "
          "tap into no tap at all");
    CHECK(q.dropped == 1u, "counted, got %llu", (unsigned long long)q.dropped);

    /* Confirm the edge really is still there, at the back. */
    for (unsigned i = 0; i < VM_TOUCH_QUEUE_CAP - 1u; i++)
        vm_touch_queue_pop(&q);
    CHECK(vm_touch_queue_peek(&q, &out), "the edge survived");
    CHECK(out.phase == MTZ2_PHASE_MAKE_TOUCH && out.x == 700,
          "and it is the same edge, got phase %u x %u",
          (unsigned)out.phase, (unsigned)out.x);
}

static void test_a_full_tap_survives_a_flood(void) {
    /*
     * The end-to-end property, stated as one scenario: a finger goes down,
     * drags far longer than the queue can hold, and lifts. However many MOVEDs
     * are lost to coalescing, BOTH edges must come out, in order.
     */
    vm_touch_queue_t q;
    vm_touch_queue_reset(&q);

    s5l_mt_contact_t down = make(MTZ2_PHASE_MAKE_TOUCH, 1);
    CHECK(vm_touch_queue_push(&q, &down), "finger down");
    for (uint16_t i = 0; i < 200; i++) {
        s5l_mt_contact_t c = make(MTZ2_PHASE_TOUCHING, (uint16_t)(10 + i));
        (void)vm_touch_queue_push(&q, &c);
    }
    s5l_mt_contact_t up = make(MTZ2_PHASE_BREAK_TOUCH, 2);
    /* The lift arrives at a full queue whose back is a MOVED. The newest old
     * position is the safe report to sacrifice; losing the lift would leave a
     * permanent finger in the guest. */
    bool liftQueued = vm_touch_queue_push(&q, &up);
    CHECK(liftQueued, "the lift did not displace an obsolete MOVED");
    CHECK(q.dropped == 0u, "the drag lost an incoming report");

    s5l_mt_contact_t out;
    CHECK(vm_touch_queue_peek(&q, &out), "the queue still has the press");
    CHECK(out.phase == MTZ2_PHASE_MAKE_TOUCH,
          "the finger-down edge is still at the front after 200 moves, got %u",
          (unsigned)out.phase);
    CHECK(q.count == VM_TOUCH_QUEUE_CAP, "and the queue is exactly full");
    for (unsigned i = 0; i < VM_TOUCH_QUEUE_CAP - 1u; i++)
        vm_touch_queue_pop(&q);
    CHECK(vm_touch_queue_peek(&q, &out) &&
          out.phase == MTZ2_PHASE_BREAK_TOUCH,
          "the finger-up edge did not survive at the back");
}

static void test_null_arguments(void) {
    vm_touch_queue_t q;
    vm_touch_queue_reset(&q);
    s5l_mt_contact_t c = make(MTZ2_PHASE_TOUCHING, 0);
    s5l_mt_contact_t out;

    CHECK(!vm_touch_queue_push(NULL, &c),  "push onto null");
    CHECK(!vm_touch_queue_push(&q, NULL),  "push a null report");
    CHECK(!vm_touch_queue_peek(NULL, &out), "peek a null queue");
    CHECK(!vm_touch_queue_peek(&q, NULL),   "peek into null");
    vm_touch_queue_pop(NULL);               /* must not crash */
    vm_touch_queue_reset(NULL);             /* must not crash */
    CHECK(q.count == 0u, "none of that changed the queue");
}

int main(void) {
    test_phase_mapping();
    test_phase_mapping_rejects();
    test_fifo_order();
    test_wraparound();
    test_moved_coalesces_when_full();
    test_edges_are_never_coalesced();
    test_a_full_tap_survives_a_flood();
    test_null_arguments();

    printf("test_vmtouchqueue: %u checks, %u failed\n", tests, failed);
    return failed ? 1 : 0;
}
