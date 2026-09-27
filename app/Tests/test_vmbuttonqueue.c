/*
 * S5LBox — VMButtonQueue tests.
 *
 * Two properties, and the first is the one that can be wrong without anything
 * reporting an error:
 *
 *   1. The app's button order and the core's are DIFFERENT and the translation
 *      between them is right. VMButton is the order the control bar draws
 *      (Home first); S5L_BUTTON_* is /device-tree/buttons' own order (hold
 *      first, because that is the interrupt index AppleM68Buttons asks its
 *      provider for). A cast between them compiles, runs, and quietly sends
 *      Power when the user pressed Home.
 *   2. A button transition is never coalesced away. A press and its release
 *      are two edges; dropping either leaves the guest holding a key nobody
 *      is pressing.
 *
 * Both are plain C, so this runs on every host CI runner with no Apple
 * toolchain, no simulator and no device.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMButtonQueue.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, ...) do { \
    if (cond) g_pass++; \
    else { \
        g_fail++; \
        printf("  FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

/*
 * The expected mapping, written out longhand rather than derived, so that a
 * change to the table in VMButtonQueue.c is a test failure and not a silently
 * agreed one. The names are what a person calls the button and what the device
 * tree calls it, which is exactly where the confusion lives.
 */
static const struct {
    unsigned    app;
    unsigned    guest;
    const char *person;
    const char *tree;
} EXPECTED[VM_BUTTON_COUNT] = {
    { VM_BUTTON_HOME,          S5L_BUTTON_MENU,     "Home",   "menu"     },
    { VM_BUTTON_POWER,         S5L_BUTTON_HOLD,     "Power",  "hold"     },
    { VM_BUTTON_VOLUME_UP,     S5L_BUTTON_VOLUP,    "Vol +",  "volup"    },
    { VM_BUTTON_VOLUME_DOWN,   S5L_BUTTON_VOLDOWN,  "Vol -",  "voldown"  },
    { VM_BUTTON_RINGER_SILENT, S5L_BUTTON_RINGERAB, "Silent", "ringerab" },
};

static void test_the_two_orders_are_not_the_same_and_map_correctly(void) {
    /* If these ever became the same order this test would still pass, so say
     * out loud that they are not: Home is first for the UI and second for the
     * tree, which is the whole reason a table exists. */
    CHECK(VM_BUTTON_HOME != S5L_BUTTON_MENU,
          "the app and tree orders have converged; the mapping table is now "
          "untested by construction and this test needs rewriting");

    for (unsigned i = 0; i < VM_BUTTON_COUNT; i++) {
        unsigned which = 0xffffu;
        bool pressed = false;
        CHECK(vm_button_to_guest(EXPECTED[i].app, true, &which, &pressed),
              "%s was refused", EXPECTED[i].person);
        CHECK(which == EXPECTED[i].guest,
              "%s maps to core button %u, expected %u (%s)",
              EXPECTED[i].person, which, EXPECTED[i].guest, EXPECTED[i].tree);
        CHECK(which < S5L_BUTTON_COUNT,
              "%s maps outside the core's five", EXPECTED[i].person);
        CHECK(strcmp(s5l_button_name(which), EXPECTED[i].tree) == 0,
              "%s maps to '%s', expected the tree's '%s'",
              EXPECTED[i].person, s5l_button_name(which), EXPECTED[i].tree);
    }

    /* No two app buttons may land on the same core button, or one control
     * would be silent and another would fire twice. */
    for (unsigned a = 0; a < VM_BUTTON_COUNT; a++)
        for (unsigned b = a + 1u; b < VM_BUTTON_COUNT; b++) {
            unsigned wa = 0, wb = 0;
            (void)vm_button_to_guest(a, true, &wa, NULL);
            (void)vm_button_to_guest(b, true, &wb, NULL);
            CHECK(wa != wb, "%s and %s both map to core button %u",
                  EXPECTED[a].person, EXPECTED[b].person, wa);
        }

    /* And every core button is reachable from the UI. */
    for (unsigned g = 0; g < S5L_BUTTON_COUNT; g++) {
        bool found = false;
        for (unsigned a = 0; a < VM_BUTTON_COUNT; a++) {
            unsigned w = 0;
            (void)vm_button_to_guest(a, true, &w, NULL);
            if (w == g) found = true;
        }
        CHECK(found, "core button %s (%u) cannot be pressed from the UI",
              s5l_button_name(g), g);
    }

    /* Out of range is refused and writes nothing. */
    unsigned which = 0xabcdu;
    bool pressed = true;
    CHECK(!vm_button_to_guest(VM_BUTTON_COUNT, true, &which, &pressed),
          "a sixth app button was accepted");
    CHECK(!vm_button_to_guest(999u, true, &which, &pressed),
          "a wildly out-of-range app button was accepted");
    CHECK(which == 0xabcdu && pressed,
          "a refused translation wrote its outputs anyway");
    /* NULL outputs are allowed; the caller may want only one of them. */
    CHECK(vm_button_to_guest(VM_BUTTON_HOME, true, NULL, NULL),
          "NULL outputs were refused");
}

/*
 * THE SLIDER. Four of the five translate `pressed` as the identity and the
 * fifth must not: the control bar's "Silent" key being down has to mean the
 * position the guest's own driver dispatches Phone Mute with value 1 for, which
 * core/include/soc.h names S5L_BUTTONS_RINGER_MUTED. This is the second
 * inversion in a chain that already has one, and the failure mode of getting it
 * wrong is a phone that rings when the user asked for silence.
 */
static void test_the_silent_switch_means_muted(void) {
    bool pressed = false;
    CHECK(vm_button_to_guest(VM_BUTTON_RINGER_SILENT, true, NULL, &pressed),
          "Silent was refused");
    CHECK(pressed == S5L_BUTTONS_RINGER_MUTED,
          "Silent held does not select the muted position");
    CHECK(vm_button_to_guest(VM_BUTTON_RINGER_SILENT, false, NULL, &pressed),
          "Silent released was refused");
    CHECK(pressed == !S5L_BUTTONS_RINGER_MUTED,
          "Silent released does not select the unmuted position");

    /* And the other four really are the identity. */
    for (unsigned i = 0; i < VM_BUTTON_COUNT; i++) {
        if (EXPECTED[i].app == VM_BUTTON_RINGER_SILENT) continue;
        bool down = false, up = true;
        (void)vm_button_to_guest(EXPECTED[i].app, true, NULL, &down);
        (void)vm_button_to_guest(EXPECTED[i].app, false, NULL, &up);
        CHECK(down && !up, "%s is inverted; only the ringer may be",
              EXPECTED[i].person);
    }
}

/* Ordinary FIFO behaviour, and the counters. */
static void test_the_queue_is_a_strict_fifo(void) {
    vm_button_queue_t q;
    vm_button_event_t e;
    memset(&q, 0x5a, sizeof q);
    vm_button_queue_reset(&q);
    CHECK(q.count == 0u && q.queued == 0u && q.dropped == 0u,
          "reset did not clear a poisoned queue");
    CHECK(!vm_button_queue_peek(&q, &e), "an empty queue produced a transition");
    vm_button_queue_pop(&q);            /* harmless */

    CHECK(vm_button_queue_push(&q, S5L_BUTTON_MENU, true), "push refused");
    CHECK(vm_button_queue_push(&q, S5L_BUTTON_MENU, false), "push refused");
    CHECK(q.count == 2u && q.queued == 2u, "the queue did not hold both");

    CHECK(vm_button_queue_peek(&q, &e) && e.which == S5L_BUTTON_MENU &&
          e.pressed, "the press did not come out first");
    vm_button_queue_pop(&q);
    CHECK(vm_button_queue_peek(&q, &e) && e.which == S5L_BUTTON_MENU &&
          !e.pressed, "the release did not come out second");
    vm_button_queue_pop(&q);
    CHECK(q.count == 0u, "the queue did not empty");

    /* NULL is safe everywhere. */
    CHECK(!vm_button_queue_push(NULL, S5L_BUTTON_MENU, true), "NULL push");
    CHECK(!vm_button_queue_peek(NULL, &e), "NULL peek");
    CHECK(!vm_button_queue_peek(&q, NULL), "peek into NULL");
    vm_button_queue_pop(NULL);
    vm_button_queue_reset(NULL);

    /* A button the core does not have is refused rather than stored. */
    CHECK(!vm_button_queue_push(&q, S5L_BUTTON_COUNT, true),
          "a sixth core button was queued");
    CHECK(q.count == 0u && q.dropped == 0u,
          "a malformed push was stored or counted as a drop");
}

static void test_lifecycle_cancellation_preserves_accounting(void) {
    vm_button_queue_t q;
    vm_button_queue_reset(&q);
    CHECK(vm_button_queue_push(&q, S5L_BUTTON_MENU, true), "setup press");
    CHECK(vm_button_queue_push(&q, S5L_BUTTON_MENU, false), "setup release");
    uint64_t queued = q.queued;
    uint64_t dropped = q.dropped;
    CHECK(vm_button_queue_cancel_pending(&q) == 2u,
          "cancellation did not report both transitions");
    CHECK(q.count == 0u && q.head == 0u,
          "cancellation did not empty the ring");
    CHECK(q.queued == queued && q.dropped == dropped,
          "cancellation rewrote lifetime accounting");
    CHECK(vm_button_queue_cancel_pending(&q) == 0u,
          "cancelling an empty queue reported work");
    CHECK(vm_button_queue_cancel_pending(NULL) == 0u,
          "cancelling a null queue reported work");
    vm_button_event_t event;
    CHECK(vm_button_queue_push(&q, S5L_BUTTON_VOLUP, false) &&
          vm_button_queue_peek(&q, &event) &&
          event.which == S5L_BUTTON_VOLUP && !event.pressed,
          "the cancelled ring could not be reused from a clean head");
}

/*
 * NOTHING IS EVER COALESCED. This is where the button queue is deliberately
 * stricter than the touch queue, and the test states the difference: a repeated
 * identical transition still costs a slot, because deciding here that it is
 * redundant would mean this file tracking a state it cannot see the guest's
 * half of.
 */
static void test_no_transition_is_ever_coalesced(void) {
    vm_button_queue_t q;
    vm_button_queue_reset(&q);

    for (unsigned i = 0; i < VM_BUTTON_QUEUE_CAP; i++)
        CHECK(vm_button_queue_push(&q, S5L_BUTTON_MENU, (i & 1u) == 0u),
              "push %u refused below the cap", i);
    CHECK(q.count == VM_BUTTON_QUEUE_CAP, "the queue does not hold its cap");

    /* Full. The incoming transition is DROPPED and counted — not merged into
     * the one at the back, whatever it is. */
    CHECK(!vm_button_queue_push(&q, S5L_BUTTON_MENU, true),
          "a full queue accepted another transition");
    CHECK(q.dropped == 1u, "the drop was not counted");
    CHECK(q.count == VM_BUTTON_QUEUE_CAP, "a dropped push changed the count");

    /* And an identical repeat of the back entry is still queued when there is
     * room, rather than being silently swallowed. */
    vm_button_queue_reset(&q);
    CHECK(vm_button_queue_push(&q, S5L_BUTTON_VOLUP, true), "push refused");
    CHECK(vm_button_queue_push(&q, S5L_BUTTON_VOLUP, true), "push refused");
    CHECK(q.count == 2u,
          "an identical repeat was coalesced; this queue must not coalesce");

    /* The ring wraps without reordering: drain a few, push a few, and the
     * order out must still be the order in. */
    vm_button_queue_reset(&q);
    for (unsigned i = 0; i < VM_BUTTON_QUEUE_CAP; i++)
        (void)vm_button_queue_push(&q, i % S5L_BUTTON_COUNT, true);
    for (unsigned i = 0; i < VM_BUTTON_QUEUE_CAP / 2u; i++) {
        vm_button_event_t e;
        CHECK(vm_button_queue_peek(&q, &e) && e.which == i % S5L_BUTTON_COUNT,
              "entry %u came out of order", i);
        vm_button_queue_pop(&q);
    }
    for (unsigned i = 0; i < VM_BUTTON_QUEUE_CAP / 2u; i++)
        CHECK(vm_button_queue_push(&q, S5L_BUTTON_HOLD, false),
              "the ring did not reuse the drained slots");
    for (unsigned i = VM_BUTTON_QUEUE_CAP / 2u; i < VM_BUTTON_QUEUE_CAP; i++) {
        vm_button_event_t e;
        CHECK(vm_button_queue_peek(&q, &e) && e.which == i % S5L_BUTTON_COUNT,
              "the wrap reordered entry %u", i);
        vm_button_queue_pop(&q);
    }
    for (unsigned i = 0; i < VM_BUTTON_QUEUE_CAP / 2u; i++) {
        vm_button_event_t e;
        CHECK(vm_button_queue_peek(&q, &e) && e.which == S5L_BUTTON_HOLD &&
              !e.pressed, "the wrapped entries came out wrong");
        vm_button_queue_pop(&q);
    }
    CHECK(q.count == 0u, "the queue did not empty after a full wrap");
    CHECK(q.dropped == 0u, "a wrap that never overflowed reported a drop");
}

static void test_power_release_uses_display_edge_or_bounded_fallback(void) {
    const uint64_t down_ns = UINT64_C(7000000000);
    const uint64_t down_cycles = UINT64_C(9000000000);
    vm_button_event_t power_down = { S5L_BUTTON_HOLD, true };
    vm_button_event_t power_up = { S5L_BUTTON_HOLD, false };
    vm_button_event_t home_up = { S5L_BUTTON_MENU, false };
    vm_button_power_hold_t hold = {
        .active = true,
        .display_running_at_press = false,
        .delivered_ns = down_ns,
        .delivered_cycles = down_cycles,
    };

    CHECK(vm_button_power_release_ready(
              &power_down, &hold, down_ns, down_cycles, false),
          "a Power press was delayed");
    CHECK(vm_button_power_release_ready(
              &home_up, &hold, down_ns, down_cycles, false),
          "a non-Power release was delayed");
    CHECK(!vm_button_power_release_ready(
              &power_up, &hold,
              down_ns + VM_BUTTON_POWER_DARK_MIN_HOLD_NS - 1u,
              down_cycles + VM_BUTTON_POWER_MIN_HOLD_CYCLES, true),
          "a display edge waived the host-time floor");
    CHECK(vm_button_power_release_ready(
              &power_up, &hold,
              down_ns + VM_BUTTON_POWER_DARK_MIN_HOLD_NS,
              down_cycles + 1u, true),
          "a dark-display wake edge did not release Power");
    CHECK(!vm_button_power_release_ready(
              &power_up, &hold,
              down_ns + VM_BUTTON_POWER_DARK_MIN_HOLD_NS,
              down_cycles + VM_BUTTON_POWER_MIN_HOLD_CYCLES - 1u, false),
          "a dark display ignored the bounded retirement fallback");
    CHECK(!vm_button_power_release_ready(
              &power_up, &hold,
              down_ns + VM_BUTTON_POWER_DARK_MAX_HOLD_NS - 1u,
              down_cycles + VM_BUTTON_POWER_MIN_HOLD_CYCLES - 1u, false),
          "Power ignored the host cap's lower boundary");
    CHECK(vm_button_power_release_ready(
              &power_up, &hold,
              down_ns + VM_BUTTON_POWER_DARK_MAX_HOLD_NS,
              down_cycles + 1u, false),
          "Power stayed held at the absolute host-time cap");
    CHECK(vm_button_power_release_ready(
              &power_up, &hold,
              down_ns + VM_BUTTON_POWER_DARK_MIN_HOLD_NS,
              down_cycles + VM_BUTTON_POWER_MIN_HOLD_CYCLES, false),
          "Power stayed held at the exact fallback boundary");

    hold.display_running_at_press = true;
    CHECK(!vm_button_power_release_ready(
              &power_up, &hold,
              down_ns + VM_BUTTON_POWER_DARK_MAX_HOLD_NS,
              down_cycles + VM_BUTTON_POWER_MIN_HOLD_CYCLES, true),
          "the dark-display host cap shortened an awake Power pulse");
    CHECK(!vm_button_power_release_ready(
              &power_up, &hold,
              down_ns + VM_BUTTON_POWER_AWAKE_MIN_HOLD_NS - 1u,
              down_cycles + VM_BUTTON_POWER_MIN_HOLD_CYCLES, true),
          "an awake press ignored the host-time floor");
    CHECK(vm_button_power_release_ready(
              &power_up, &hold,
              down_ns + VM_BUTTON_POWER_AWAKE_MIN_HOLD_NS,
              down_cycles + 1u, false),
          "an awake press was forced through the dark-display fallback");
    hold.display_running_at_press = false;

    /* A missing host clock waives only its own half.  This is the regression:
     * a dark display still needs an observed wake or the bounded fallback. */
    hold.delivered_ns = 0u;
    CHECK(!vm_button_power_release_ready(
              &power_up, &hold, 0u,
              down_cycles + VM_BUTTON_POWER_MIN_HOLD_CYCLES - 1u, false),
          "a missing host clock also waived the dark-display fallback");
    CHECK(vm_button_power_release_ready(
              &power_up, &hold, 0u,
              down_cycles + VM_BUTTON_POWER_MIN_HOLD_CYCLES, false),
          "the fallback did not release Power without a host clock");

    hold.delivered_ns = down_ns;
    CHECK(vm_button_power_release_ready(
              &power_up, NULL, down_ns, down_cycles, false),
          "an unpaired Power release wedged the queue");
    CHECK(vm_button_power_release_ready(
              &power_up, &hold,
              down_ns + VM_BUTTON_POWER_DARK_MIN_HOLD_NS,
              down_cycles - 1u, false),
          "a backwards guest counter wedged Power");
    CHECK(!vm_button_power_release_ready(
              NULL, &hold, down_ns, down_cycles, false),
          "a NULL event was called ready");
}

static void test_home_and_volume_survive_the_guest_debounce(void) {
    const uint64_t down_ns = UINT64_C(11000000000);
    vm_button_event_t home_down = { S5L_BUTTON_MENU, true };
    vm_button_event_t home_up = { S5L_BUTTON_MENU, false };
    vm_button_event_t power_up = { S5L_BUTTON_HOLD, false };
    vm_button_event_t ringer_up = { S5L_BUTTON_RINGERAB, false };
    vm_button_momentary_holds_t holds;
    memset(&holds, 0, sizeof holds);

    CHECK(vm_button_momentary_release_ready(&home_down, &holds, down_ns, true),
          "a Home press was delayed");
    vm_button_momentary_note_accepted(&home_down, &holds, down_ns, false);
    CHECK(!vm_button_momentary_release_ready(
              &home_up, &holds,
              down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS - 1u, true),
          "Home was released before the debounce floor");
    CHECK(vm_button_momentary_release_ready(
              &home_up, &holds,
              down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS, true),
          "Home stayed held at the exact debounce boundary");

    vm_button_momentary_note_accepted(
        &home_up, &holds, down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS, false);
    CHECK(vm_button_momentary_release_ready(&home_up, &holds, down_ns, true),
          "an already-released Home key wedged the queue");

    const unsigned volume[] = { S5L_BUTTON_VOLUP, S5L_BUTTON_VOLDOWN };
    for (unsigned i = 0; i < sizeof volume / sizeof volume[0]; i++) {
        vm_button_event_t down = { (uint8_t)volume[i], true };
        vm_button_event_t up = { (uint8_t)volume[i], false };
        vm_button_momentary_note_accepted(&down, &holds, down_ns, false);
        CHECK(!vm_button_momentary_release_ready(
                  &up, &holds,
                  down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS - 1u, true),
              "volume button %u ignored the debounce floor", volume[i]);
        CHECK(vm_button_momentary_release_ready(
                  &up, &holds,
                  down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS, true),
              "volume button %u stayed held at the boundary", volume[i]);
        vm_button_momentary_note_accepted(
            &up, &holds, down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS, false);
    }

    CHECK(vm_button_momentary_release_ready(&power_up, &holds, down_ns, true),
          "the ordinary floor captured Power's separate policy");
    CHECK(vm_button_momentary_release_ready(&ringer_up, &holds, down_ns, true),
          "the momentary floor captured the two-position ringer");
    vm_button_momentary_note_accepted(&power_up, &holds, down_ns, false);
    vm_button_momentary_note_accepted(&ringer_up, &holds, down_ns, false);
    CHECK(!holds.active[S5L_BUTTON_HOLD] &&
          !holds.active[S5L_BUTTON_RINGERAB],
          "Power or ringer acquired a momentary hold anchor");

    vm_button_momentary_note_accepted(&home_down, &holds, down_ns, false);
    CHECK(vm_button_momentary_release_ready(&home_up, &holds, 0u, true),
          "a missing host clock wedged Home");
    CHECK(vm_button_momentary_release_ready(&home_up, &holds, down_ns - 1u, true),
          "a backwards host clock wedged Home");
    CHECK(vm_button_momentary_release_ready(&home_up, NULL, down_ns, true),
          "an unpaired Home release wedged the queue");
    CHECK(!vm_button_momentary_release_ready(NULL, &holds, down_ns, true),
          "a NULL event was called ready");
    vm_button_momentary_note_accepted(NULL, &holds, down_ns, false);
    vm_button_momentary_note_accepted(&home_down, NULL, down_ns, false);
}

static void test_home_wake_waits_for_display_with_a_host_cap(void) {
    const uint64_t down_ns = UINT64_C(11000000000);
    vm_button_event_t down = { S5L_BUTTON_MENU, true };
    vm_button_event_t up = { S5L_BUTTON_MENU, false };
    vm_button_event_t volume_down = { S5L_BUTTON_VOLUP, true };
    vm_button_event_t volume_up = { S5L_BUTTON_VOLUP, false };
    vm_button_momentary_holds_t holds = {0};

    vm_button_momentary_note_accepted(&down, &holds, down_ns, true);
    CHECK(holds.home_wake && holds.active[S5L_BUTTON_MENU],
          "accepted Home wake lost its resume policy");
    CHECK(!vm_button_momentary_release_ready(
              &up, &holds, down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS - 1u, true),
          "CLCD waived the ordinary electrical floor");
    CHECK(!vm_button_momentary_release_ready(
              &up, &holds, down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS, false),
          "a 50 ms Home wake pulse ended before resume debounce");
    CHECK(vm_button_momentary_release_ready(
              &up, &holds, down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS, true),
          "the CLCD wake edge did not release Home");
    CHECK(!vm_button_momentary_release_ready(
              &up, &holds, down_ns + VM_BUTTON_HOME_WAKE_MAX_HOLD_NS - 1u, false),
          "Home ignored the wake cap's lower boundary");
    CHECK(vm_button_momentary_release_ready(
              &up, &holds, down_ns + VM_BUTTON_HOME_WAKE_MAX_HOLD_NS, false),
          "a dark display kept Home held beyond its host cap");

    vm_button_momentary_note_accepted(&down, &holds, down_ns + 1u, false);
    CHECK(holds.home_wake && holds.delivered_ns[S5L_BUTTON_MENU] == down_ns,
          "a duplicate held Home replaced its wake anchor");
    vm_button_momentary_note_accepted(&volume_down, &holds, down_ns, true);
    CHECK(vm_button_momentary_release_ready(
              &volume_up, &holds, down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS, false),
          "Home wake policy leaked to a volume button");
    vm_button_momentary_note_accepted(&volume_up, &holds, down_ns + 1u, false);
    CHECK(holds.home_wake, "a volume release cleared Home's pending wake");
    CHECK(vm_button_momentary_release_ready(&up, &holds, 0u, false) &&
          vm_button_momentary_release_ready(&up, &holds, down_ns - 1u, false),
          "a missing or backwards clock wedged Home wake");

    vm_button_momentary_note_accepted(&up, &holds, down_ns + 1u, false);
    CHECK(!holds.home_wake && !holds.active[S5L_BUTTON_MENU] &&
          holds.delivered_ns[S5L_BUTTON_MENU] == 0u,
          "Home release retained a wake anchor");
    vm_button_momentary_note_accepted(&down, &holds, down_ns, false);
    CHECK(vm_button_momentary_release_ready(
              &up, &holds, down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS, false),
          "an ordinary Home press inherited the wake delay");
    vm_button_momentary_note_accepted(&up, &holds, down_ns + 1u, false);
    vm_button_momentary_note_accepted(&down, &holds, 0u, true);
    CHECK(vm_button_momentary_release_ready(&up, &holds, down_ns, false),
          "an unavailable press clock wedged Home wake");

    /* Lifecycle cancellation synthesizes the same release; it must obey the
     * wake boundary without keeping a cancelled queue alive forever. */
    memset(&holds, 0, sizeof holds);
    vm_button_momentary_note_accepted(&down, &holds, down_ns, true);
    vm_button_queue_t q = {0};
    CHECK(vm_button_queue_push(&q, S5L_BUTTON_MENU, false) &&
          vm_button_queue_cancel_pending(&q) == 1u && q.count == 0u,
          "could not cancel a queued wake release");
    CHECK(!vm_button_momentary_release_ready(
              &up, &holds, down_ns + VM_BUTTON_MOMENTARY_MIN_HOLD_NS, false) &&
          vm_button_momentary_release_ready(
              &up, &holds, down_ns + VM_BUTTON_HOME_WAKE_MAX_HOLD_NS, false),
          "checkpoint release lost the bounded Home wake policy");
}

static void test_button_pulses_exclude_host_pause(void) {
    const uint64_t ms = UINT64_C(1000000);
    const uint64_t down_ns = 7000u * ms;
    const uint64_t pause_ns = down_ns + 20u * ms;
    const uint64_t resume_ns = pause_ns + 3000u * ms;
    vm_button_event_t home_down = { S5L_BUTTON_MENU, true };
    vm_button_event_t home_up = { S5L_BUTTON_MENU, false };
    vm_button_event_t power_up = { S5L_BUTTON_HOLD, false };
    vm_button_power_hold_t power = {
        .active = true, .display_running_at_press = true,
        .delivered_ns = down_ns, .delivered_cycles = 123u,
    };
    vm_button_momentary_holds_t holds = {0};
    vm_button_momentary_note_accepted(&home_down, &holds, down_ns, true);
    const unsigned volume[] = { S5L_BUTTON_VOLUP, S5L_BUTTON_VOLDOWN };
    for (unsigned i = 0; i < 2u; i++) {
        vm_button_event_t down = { (uint8_t)volume[i], true };
        vm_button_momentary_note_accepted(&down, &holds, down_ns, false);
    }
    vm_button_holds_resume(&power, &holds, pause_ns, resume_ns);
    CHECK(power.delivered_ns == down_ns + 3000u * ms &&
          holds.delivered_ns[S5L_BUTTON_MENU] == power.delivered_ns,
          "pause did not preserve the accepted pulse's 20 ms age");
    CHECK(power.active && power.display_running_at_press &&
          power.delivered_cycles == 123u && holds.home_wake &&
          holds.active[S5L_BUTTON_MENU],
          "pause changed held state, wake policy or guest-cycle witness");
    CHECK(!vm_button_momentary_release_ready(&home_up, &holds, resume_ns, true),
          "a stopped guest spent Home's debounce floor");
    CHECK(vm_button_momentary_release_ready(
              &home_up, &holds, resume_ns + 30u * ms, true),
          "Home restarted its floor instead of retaining pre-pause age");
    CHECK(!vm_button_momentary_release_ready(
              &home_up, &holds, resume_ns + 480u * ms - 1u, false) &&
          vm_button_momentary_release_ready(
              &home_up, &holds, resume_ns + 480u * ms, false),
          "Home's wake cap included stopped host time");
    CHECK(!vm_button_power_release_ready(
              &power_up, &power, resume_ns + 480u * ms - 1u, 124u, true) &&
          vm_button_power_release_ready(
              &power_up, &power, resume_ns + 480u * ms, 124u, true),
          "awake Power's floor included stopped host time");
    power.display_running_at_press = false;
    CHECK(!vm_button_power_release_ready(
              &power_up, &power, resume_ns + 230u * ms - 1u, 124u, false) &&
          vm_button_power_release_ready(
              &power_up, &power, resume_ns + 230u * ms, 124u, false),
          "dark Power's cap included stopped host time");
    for (unsigned i = 0; i < 2u; i++) {
        vm_button_event_t up = { (uint8_t)volume[i], false };
        CHECK(!vm_button_momentary_release_ready(
                  &up, &holds, resume_ns + 30u * ms - 1u, false) &&
              vm_button_momentary_release_ready(
                  &up, &holds, resume_ns + 30u * ms, false),
              "volume %u lost its active-time floor", volume[i]);
    }

    /* Repeated pauses and checkpoint cancellation retain the same accepted
     * press. Only the pending queue is cancelled, not the held-pin witness. */
    vm_button_queue_t q = {0};
    CHECK(vm_button_queue_push(&q, S5L_BUTTON_MENU, false) &&
          vm_button_queue_cancel_pending(&q) == 1u, "cancel release setup");
    vm_button_holds_resume(&power, &holds, resume_ns + 10u * ms,
                           resume_ns + 1010u * ms);
    CHECK(holds.delivered_ns[S5L_BUTTON_MENU] == down_ns + 4000u * ms &&
          !vm_button_momentary_release_ready(
              &home_up, &holds, resume_ns + 1010u * ms, true),
          "a second pause or checkpoint cancellation expired the pulse");
    vm_button_momentary_note_accepted(&home_up, &holds, resume_ns, false);
    power.active = false;
    const uint64_t stale_power = power.delivered_ns;
    vm_button_holds_resume(&power, &holds, pause_ns, resume_ns);
    CHECK(!holds.active[S5L_BUTTON_MENU] && !holds.home_wake &&
          holds.delivered_ns[S5L_BUTTON_MENU] == 0u &&
          power.delivered_ns == stale_power,
          "inactive button acquired a pause-adjusted anchor");
    CHECK(vm_button_queue_push(&q, S5L_BUTTON_MENU, true), "queue press setup");
    vm_button_holds_resume(NULL, &holds, pause_ns, resume_ns);
    vm_button_momentary_note_accepted(&home_down, &holds, resume_ns, true);
    CHECK(holds.delivered_ns[S5L_BUTTON_MENU] == resume_ns,
          "a press queued during pause inherited time before acceptance");

    const uint64_t invalid[][3] = {
        {0u, pause_ns, resume_ns}, {down_ns, 0u, resume_ns},
        {down_ns, pause_ns, 0u}, {down_ns, pause_ns, pause_ns - 1u},
        {pause_ns + 1u, pause_ns, resume_ns},
    };
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        power.active = true;
        power.delivered_ns = invalid[i][0];
        holds.delivered_ns[S5L_BUTTON_MENU] = invalid[i][0];
        vm_button_holds_resume(&power, &holds, invalid[i][1], invalid[i][2]);
        CHECK(power.delivered_ns == 0u && power.delivered_cycles == 123u &&
              power.active && holds.delivered_ns[S5L_BUTTON_MENU] == 0u &&
              holds.active[S5L_BUTTON_MENU] && holds.home_wake,
              "invalid pause clock %u changed more than host anchors", i);
        CHECK(vm_button_momentary_release_ready(&home_up, &holds, resume_ns, false),
              "invalid pause clock %u wedged Home", i);
    }
    power.delivered_ns = UINT64_MAX - 100u;
    vm_button_holds_resume(&power, NULL, UINT64_MAX - 80u, UINT64_MAX);
    CHECK(power.delivered_ns == UINT64_MAX - 20u,
          "pause anchor overflowed near the host clock limit");
    vm_button_holds_resume(&power, NULL, UINT64_MAX, UINT64_MAX);
    CHECK(power.delivered_ns == UINT64_MAX - 20u,
          "zero-duration pause changed the pulse age");
    vm_button_holds_resume(NULL, NULL, 0u, 0u);
}

int main(void) {
    printf("S5LBox app button queue tests\n");
    test_the_two_orders_are_not_the_same_and_map_correctly();
    test_the_silent_switch_means_muted();
    test_the_queue_is_a_strict_fifo();
    test_lifecycle_cancellation_preserves_accounting();
    test_no_transition_is_ever_coalesced();
    test_power_release_uses_display_edge_or_bounded_fallback();
    test_home_and_volume_survive_the_guest_debounce();
    test_home_wake_waits_for_display_with_a_host_cap();
    test_button_pulses_exclude_host_pause();
    printf("  %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
