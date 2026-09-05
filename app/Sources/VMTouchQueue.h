/*
 * S5LBox — the buffer between a finger and the emulated touch controller.
 *
 * WHY THIS IS A SEPARATE FILE. The UI thread produces touch reports at whatever
 * rate UIKit delivers them; the emulator thread consumes them between bounded
 * chunks of guest execution. Those two rates have nothing to do with each other,
 * so something has to hold reports in between — and that something has to
 * decide what to do when it fills up. That decision is the whole content of
 * this file, and getting it wrong loses taps in a way that looks like the
 * emulator being broken.
 *
 * The rule, stated once:
 *
 *     A MOVED may be replaced by a newer MOVED. An EDGE may not be dropped
 *     while there is any alternative. A new press must leave space for its
 *     eventual release, either an empty slot or an evictable MOVED.
 *
 * A tap is a MAKE_TOUCH followed by a BREAK_TOUCH. Coalescing either one turns
 * a tap into nothing, or worse, into a finger the guest believes is still down.
 * A MOVED, by contrast, is completely superseded by a later MOVED — the newer
 * position is strictly better information about where the finger is now.
 *
 * That is a rule about a ring buffer, which is testable without a device, a
 * simulator, or an Apple toolchain — so it lives here in plain C11 with no
 * UIKit and no Objective-C, and core/CMakeLists.txt builds
 * app/Tests/test_vmtouchqueue.c against it on every host CI runner. VMEngine.m
 * is then only the mutex and the call into the device.
 *
 * NOTHING HERE TOUCHES THE MACHINE. This is a container. Whether the emulated
 * Z2 accepts a report it is handed is that device's decision, made in
 * core/src/soc/mtz2.c, and it is entitled to say no.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_APP_VMTOUCHQUEUE_H
#define S5LBOX_APP_VMTOUCHQUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "soc.h"          /* s5l_mt_contact_t, MTZ2_PHASE_*, S5L_MT_PANEL_* */
#include "VMTouchMap.h"   /* vm_touch_phase_t */

/*
 * How many reports may wait. Eight is about a seventh of a second of a 60 Hz
 * drag; past that the finger has moved on, and replaying where it used to be
 * would be a worse answer than admitting the report was dropped.
 */
#define VM_TOUCH_QUEUE_CAP 8u

/*
 * The path identifier every report from this app carries. The guest's parser
 * accepts 1..11 and silently aliases anything outside that onto slot 0, so this
 * is not a free choice — see s5l_mt_contact_t in soc.h. This app models a
 * single finger, so it is a constant; multi-touch will need one id per
 * concurrent UITouch, held stable for the life of that finger.
 */
#define VM_TOUCH_CONTACT_ID 1u

/*
 * Contact amplitude and ellipse for a fingertip.
 *
 * Exactly ONE property of these was established from the guest side: the
 * amplitude must be non-zero for a finger that is down and zero for one that
 * has lifted. Nothing was observed to require a particular ellipse, so the axes
 * are a plausible fingertip and NOT a measurement — roughly 9 mm by 7 mm at the
 * panel's 163 dpi, which is 58 by 45 pixels. They are named rather than inlined
 * so that the day something downstream turns out to care, there is one place to
 * correct and one comment to correct with it.
 */
#define VM_TOUCH_PRESSURE 160u
#define VM_TOUCH_MAJOR     58u
#define VM_TOUCH_MINOR     45u

typedef struct {
    s5l_mt_contact_t slot[VM_TOUCH_QUEUE_CAP];
    unsigned         head;      /* index of the oldest report */
    unsigned         count;     /* how many are held */
    /* Bounded accounting, so a caller that ignores every return value still
     * leaves evidence. `coalesced` counts obsolete MOVED reports replaced or
     * displaced by newer information; `dropped` means the incoming report had
     * no safe place to go. */
    uint64_t         queued, coalesced, dropped;
} vm_touch_queue_t;

/*
 * What the emulated controller has ACCEPTED, as opposed to what UIKit most
 * recently queued. The controller forgets a frame after the guest reads it,
 * but the guest still believes that path is down until a BREAK_TOUCH arrives.
 * Keeping that one physical fact lets lifecycle code cancel a contact before
 * it serializes the guest, without reaching into the controller's wire frame.
 */
typedef struct {
    bool             active;
    s5l_mt_contact_t last;
} vm_touch_delivery_state_t;

/* Empty the queue and zero the counters. Safe on a zeroed struct. */
void vm_touch_queue_reset(vm_touch_queue_t *q);

/*
 * Cancel reports not yet accepted by the controller, preserving all lifetime
 * accounting. Returns how many were removed. This is lifecycle cancellation,
 * not queue overflow, so it does not increment `dropped`.
 */
unsigned vm_touch_queue_cancel_pending(vm_touch_queue_t *q);

/*
 * Translate one UIKit-side report into the device's encoding.
 *
 * Returns false — and writes nothing — for a coordinate off the 320x480 panel
 * or a phase this app does not produce. UIKit's four phases map onto the
 * device's eight as:
 *
 *     BEGAN     -> MAKE_TOUCH,  amplitude VM_TOUCH_PRESSURE
 *     MOVED     -> TOUCHING,    amplitude VM_TOUCH_PRESSURE
 *     ENDED     -> BREAK_TOUCH, amplitude 0
 *     CANCELLED -> BREAK_TOUCH, amplitude 0
 *
 * CANCELLED becoming a BREAK_TOUCH is a deliberate choice, not an oversight.
 * The device has no vocabulary for "the system took this finger away", and the
 * alternative — sending nothing — would leave the guest believing a finger is
 * still down for as long as it cared to wait.
 */
bool vm_touch_contact_from_ui(vm_touch_phase_t phase, int x, int y,
                              s5l_mt_contact_t *out);

/* Track only reports the emulated controller accepted. */
void vm_touch_delivery_reset(vm_touch_delivery_state_t *state);
void vm_touch_delivery_note_accepted(vm_touch_delivery_state_t *state,
                                     const s5l_mt_contact_t *contact);

/* Build the BREAK_TOUCH needed to cancel an accepted contact. */
bool vm_touch_delivery_make_break(const vm_touch_delivery_state_t *state,
                                  s5l_mt_contact_t *out);

/*
 * Add a report. Returns whether it is now in the queue.
 *
 * False means it was dropped and `dropped` was incremented: the queue was full
 * without an obsolete MOVED, or a new press would consume the last space for
 * its release. A later edge is more important than an older position, so it
 * may displace the newest queued
 * MOVED while preserving the order of every surviving report. A consumer must
 * keep the caller's lock across peek, device acceptance and pop: displacing a
 * MOVED can change even the head while a producer is making room for an edge.
 * Nothing else can fail — a null argument aside — because validation is
 * vm_touch_contact_from_ui()'s job.
 */
bool vm_touch_queue_push(vm_touch_queue_t *q, const s5l_mt_contact_t *c);

/* Copy the oldest report out without removing it. False if empty. */
bool vm_touch_queue_peek(const vm_touch_queue_t *q, s5l_mt_contact_t *out);

/* Remove the oldest report. Harmless on an empty queue. */
void vm_touch_queue_pop(vm_touch_queue_t *q);

#endif /* S5LBOX_APP_VMTOUCHQUEUE_H */
