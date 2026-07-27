/*
 * S5LBox -- the drag gesture the harness schedules, as pure arithmetic.
 *
 * WHAT A DRAG IS, AND WHY --touch COULD NOT BE ONE.
 *
 * tools/bootkernel.c's --touch emits exactly two reports for a contact:
 * MTZ2_PHASE_MAKE_TOUCH, then MTZ2_PHASE_BREAK_TOUCH `hold` instructions
 * later. MTZ2_PHASE_TOUCHING -- value 4, "still down, possibly moving", which
 * s5l_mtz2_set_contacts() has always accepted and stored into the frame -- was
 * emitted by nothing in this tree.
 *
 * run90 measured what that costs. Eight --touch points were scheduled along
 * the 3.1.3 lock screen's unlock slider to fake a drag, and the funnel it
 * measured reads 16 / 17 / 9 / 2:
 *
 *   16  reports enqueued by the kernel -- every one of the 8 downs and 8 ups.
 *       Probe 0xc043d6b8 captured exactly 16, each ~43,000 instructions after
 *       its scheduled moment, none refused. The device and the driver did
 *       their whole job.
 *   17  frames parsed in userspace at _mt_HandleMultitouchFrame (0x33cfb3ec).
 *   9   reached the MultitouchHID plugin (0x33cfdee0, the blx inside
 *       _mt_ForwardBinaryContacts) -- nearly half were already gone HERE,
 *       before UIKit was involved at all.
 *   2   reached __UIApplicationHandleEvent (0x324f6edc): the first
 *       finger-down and the first finger-up.
 *
 * Nothing downstream was broken. UIKit was simply never shown one contact
 * moving; it was shown eight independent fingers landing on the same path
 * identifier, and everything between the plugin and the application coalesced
 * that into a tap. The screen did not unlock.
 *
 * So a drag is ONE contact under ONE identifier, reported as:
 *
 *     MakeTouch  at (x0,y0)                 pressure MT_DRAG_PRESSURE
 *     Touching   at each interpolated point pressure MT_DRAG_PRESSURE
 *     BreakTouch at (x1,y1)                 pressure 0
 *
 * WHY THIS IS ITS OWN FILE. The scheduling half of a drag -- when each report
 * is due, and what to do when the device refuses one -- needs a running
 * machine and stays in bootkernel.c beside --touch's. The half here is the one
 * that can be wrong without booting anything: the phase sequence, the
 * identifier, and the interpolated coordinates. Keeping it separable is what
 * lets core/tests/test_mt_drag.c pin it in milliseconds rather than in a
 * 27-minute run, and run90 is a receipt for how expensive the other way is.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef S5LBOX_MT_DRAG_H
#define S5LBOX_MT_DRAG_H

#include "soc.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * INTERMEDIATE REPORTS.
 *
 * Eight is the default because eight is what run90's operator asked for with
 * eight --touch points: that was the gesture actually wanted, and at the pacing
 * below it is nine scan frames, ~144 ms of guest time -- a deliberate slider
 * drag rather than a flick.
 *
 * Zero is not a shorter drag. It is a tap, and the parser refuses it rather
 * than emitting a two-report gesture under a name that promises movement --
 * that is exactly the sequence run90 already proved UIKit discards. The
 * ceiling is a readability bound and not a device one: 64 intermediate reports
 * is over a second of continuous contact at the pacing below, well past
 * anything UIKit distinguishes, and it keeps one drag to one line of the run
 * report.
 */
#define MT_DRAG_STEPS_DEFAULT 8u
#define MT_DRAG_STEPS_MIN     1u
#define MT_DRAG_STEPS_MAX    64u

/*
 * ONE SCAN OF THE MODELLED PART, IN RETIRED INSTRUCTIONS.
 *
 * The device advances its frame timestamp by MTZ2_FRAME_PERIOD_MS per report
 * because that is one frame at the ~60 Hz a part like this scans at, and this
 * machine retires one instruction per CPU tick at S5L8900_CPU_HZ. A report
 * every MT_DRAG_FRAME_INSTRS instructions is therefore a drag paced exactly as
 * the real digitiser would report it: 0.016 * 412,000,000 = 6,592,000.
 *
 * This is the same arithmetic BUTTON_DEBOUNCE_INSTRS is derived from, and it
 * is arithmetic rather than a safety margin. Pacing faster is legal and is not
 * refused -- the device holds one report at a time, so it would simply refuse
 * the next one until the guest drained the last, and the run report counts
 * every refusal -- but a drag paced faster than the part can scan describes a
 * device that does not exist.
 */
#define MT_DRAG_FRAME_INSTRS \
    (((uint64_t)S5L8900_CPU_HZ * (uint64_t)MTZ2_FRAME_PERIOD_MS) / 1000u)

/*
 * THE PATH IDENTIFIER, and the whole point of the exercise.
 *
 * Every report of a drag carries this same value, because a contact that
 * changes identity is a new finger -- which is precisely what run90's eight
 * taps looked like from _mt_getPathLifeCycle onwards. It is --touch's id too,
 * deliberately: this harness reports one finger at a time. That does mean a
 * --touch scheduled inside a --drag's window describes two fingers sharing one
 * identifier, which is nonsense the guest cannot untangle, so the drag section
 * of the run report says so out loud whenever both are armed.
 */
#define MT_DRAG_CONTACT_ID 1u

/*
 * Amplitude and contact ellipse while the finger is down -- --touch's own
 * values, so a drag and a tap present the same finger. The lift carries
 * pressure 0 because the parsed Z amplitude is what the HID plugin tests for
 * "is this finger down", and it must agree with the phase byte rather than
 * contradict it.
 */
#define MT_DRAG_PRESSURE 160u
#define MT_DRAG_MAJOR     24u
#define MT_DRAG_MINOR     20u

/*
 * The gesture's SHAPE. Not its schedule: `at`, the absolute instruction the
 * finger lands on, is bootkernel.c's, for the same reason the retry bookkeeping
 * is. `span` is here because the pacing is a property of the gesture -- it is
 * divided by the report count, which only this file knows.
 */
typedef struct {
    uint16_t x0, y0;   /* where the finger lands, panel pixels             */
    uint16_t x1, y1;   /* where it lifts                                   */
    unsigned steps;    /* intermediate TOUCHING reports, MIN..MAX          */
    uint64_t span;     /* NOMINAL instructions, accepted landing to lift   */
} mt_drag_t;

/*
 * Reports in the whole gesture: the landing, `steps` intermediate ones, and
 * the lift. Returns 0 for a drag mt_drag_valid() rejects, so a caller that
 * loops over the count of an invalid drag emits nothing at all rather than
 * something arbitrary.
 */
unsigned mt_drag_reports(const mt_drag_t *d);

/*
 * The default `span` for a given step count: one MT_DRAG_FRAME_INSTRS scan
 * period per gap, i.e. reports paced exactly as the modelled part scans. It
 * depends on `steps` because "one report per frame" is the property worth
 * holding fixed, not any particular total duration.
 */
uint64_t mt_drag_span_default(unsigned steps);

/*
 * Whole-gesture validation, so a malformed drag is refused on the command line
 * rather than by the device on every instruction from `at` to the end of the
 * run -- which reads exactly like "the guest never drained a report", the
 * wrong answer to the only question the run was asked. Same reasoning as
 * --touch's parse-time coordinate check.
 */
bool mt_drag_valid(const mt_drag_t *d);

/*
 * Instructions between consecutive reports: span / (steps + 1), which is at
 * least 1 for any drag mt_drag_valid() accepts. Returns 0 for an invalid one.
 */
uint64_t mt_drag_gap(const mt_drag_t *d);

/* A drag whose endpoints coincide is a HELD CONTACT, not a drag: legal, useful
 * (it is a long press with the stationary updates --touch cannot make), and
 * called out in the run report so it can never be mistaken for movement. */
bool mt_drag_stationary(const mt_drag_t *d);

/*
 * Fill `out` with report `k` of the gesture, k in [0, mt_drag_reports(d)).
 * False -- and `out` untouched -- for an invalid drag or an out-of-range k, so
 * a caller cannot inject a contact this file never described.
 *
 * The coordinates are a straight line, rounded to nearest with halves UP so
 * that a drag and its reverse visit the same pixels: report 0 is exactly
 * (x0,y0), the last is exactly (x1,y1), and the sequence between them is
 * monotonic in both axes. Every point is on the panel, because both endpoints
 * are and a point between them cannot leave the box they span.
 */
bool mt_drag_contact(const mt_drag_t *d, unsigned k, s5l_mt_contact_t *out);

/* The phase names, for the run report. Never NULL. */
const char *mt_drag_phase_name(uint8_t phase);

#endif /* S5LBOX_MT_DRAG_H */
