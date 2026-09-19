/*
 * S5LBox — S5L8900 timer block.
 *
 * Two independent things live here, and conflating them is the mistake that
 * kept this kernel silent:
 *
 *   1. A free-running 64-bit counter at 0x080/0x084. This is the backing store
 *      for mach_absolute_time(). It counts unconditionally. If it reads zero
 *      forever, every delay loop and every timeout in the kernel waits forever,
 *      and the boot dies quietly in a spin lock rather than with a panic.
 *
 *   2. Timer 4, an up-counter the kernel uses as its decrementer. XNU selects
 *      PWM mode: DATA0 raises match0 WITHOUT resetting the counter; DATA1
 *      resets it. DATA1=UINT32_MAX makes the short deadline effectively a
 *      single compare until set_decrementer clears/rearms the counter.
 *
 * The kernel's register sequence and get_decrementer subtraction establish the
 * up-counter contract. PWM compare semantics are corroborated by the related
 * Samsung timer documentation; see docs/BOOTLOG.md. This is not a complete
 * timer peripheral: the legacy non-PWM paths and unobserved buffering/clock
 * modes are not expanded here.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <string.h>

void s5l_timer_reset(s5l_timer_t *t) { memset(t, 0, sizeof *t); }

static bool timer4_pwm(const s5l_timer_t *t) {
    return (t->t4_config & TIMER4_MODE_MASK) == TIMER4_MODE_PWM;
}

static uint32_t timer4_elapsed(const s5l_timer_t *t) {
    return t->t4_count - t->t4_value;
}

/* Unsigned counter distance; equality means the next wrap, not an immediate
 * repeated compare. A register reprogrammed behind the counter also waits
 * for wrap. Use 64 bits to distinguish a full 2^32 from zero. */
static uint64_t timer4_distance(uint32_t phase, uint32_t target) {
    uint32_t distance = target - phase;
    return distance ? distance : (UINT64_C(1) << 32);
}

uint32_t s5l_timer_ticks_to_irq(const s5l_timer_t *t) {
    if (!(t->t4_state & TIMER4_STATE_START)) return 0;
    if (!timer4_pwm(t))
        return t->t4_value ? t->t4_value : t->t4_count;

    uint32_t phase = timer4_elapsed(t);
    uint32_t period = t->t4_count2 ? t->t4_count2 : 1u;
    uint64_t end = timer4_distance(phase, period);
    uint64_t next = UINT64_MAX;
    if (t->t4_config & TIMER4_INT1_EN) next = end;
    if (t->t4_config & TIMER4_INT0_EN) {
        uint64_t first = timer4_distance(phase, t->t4_count);
        if (first <= end && first < next) next = first;
        if (t->t4_count <= period) {
            uint64_t after_reset = end + t->t4_count;
            if (after_reset < next) next = after_reset;
        }
    }
    if (next == UINT64_MAX) return 0;
    return next > UINT32_MAX ? UINT32_MAX : (uint32_t)next;
}

static void timer4_tick_pwm(s5l_timer_t *t, uint32_t ticks) {
    uint32_t phase = timer4_elapsed(t);
    uint32_t period = t->t4_count2 ? t->t4_count2 : 1u;
    uint64_t end = timer4_distance(phase, period);
    uint64_t first = timer4_distance(phase, t->t4_count);
    if (first <= ticks && first <= end) t->irqlatch |= TIMER4_INT0;
    if (ticks >= end) {
        uint64_t rest = ticks - end;
        t->irqlatch |= TIMER4_INT1;
        if (t->t4_count <= period && rest >= t->t4_count)
            t->irqlatch |= TIMER4_INT0;
        phase = (uint32_t)(rest % period);
    } else {
        phase += ticks;
    }
    t->t4_value = t->t4_count - phase;
}

uint32_t s5l_timer_read(s5l_timer_t *t, uint32_t off) {
    switch (off) {
        case TIMER_TICKSLOW:   return (uint32_t)t->ticks;
        case TIMER_TICKSHIGH:  return (uint32_t)(t->ticks >> 32);
        case TIMER_CONFIG:     return t->config;
        case TIMER4_CONFIG:    return t->t4_config;
        case TIMER4_STATE:     return t->t4_state & ~TIMER4_STATE_UPDATE;
        case TIMER4_COUNTBUF:  return t->t4_count;
        case TIMER4_COUNTBUF2: return t->t4_count2;
        case TIMER4_VALUE:     return timer4_elapsed(t);
        case TIMER_IRQLATCH:   return t->irqlatch;
        case TIMER_IRQSTATUS:  return t->irqlatch;
        default:               return 0;
    }
}

void s5l_timer_write(s5l_timer_t *t, uint32_t off, uint32_t val) {
    switch (off) {
        case TIMER_CONFIG:     t->config = val; break;
        case TIMER4_CONFIG:    t->t4_config = val; break;

        case TIMER4_STATE:
            t->t4_state = val;
            /* Loading the saved remaining-count encoding makes the exposed
             * elapsed counter zero. The clear command is not a status bit. */
            if (val & TIMER4_STATE_UPDATE) t->t4_value = t->t4_count;
            break;

        case TIMER4_COUNTBUF:
            /* Keep the elapsed counter stable across the kernel's DATA0
             * write. Its following STATE=3 performs the actual clear. The
             * legacy interval path retains its immediate-load behavior. */
            if (timer4_pwm(t)) t->t4_value += val - t->t4_count;
            else t->t4_value = val;
            t->t4_count = val;
            break;

        case TIMER4_COUNTBUF2: t->t4_count2 = val; break;

        /* Write-1-to-clear. The FIQ handler acknowledges with TIMER4_IRQ_BITS;
         * failing to drop the latch here leaves the line asserted so the
         * handler re-enters immediately, which presents as a hang. */
        case TIMER_IRQACK:
        case TIMER_IRQLATCH:   t->irqlatch &= ~val; break;
        case TIMER_IRQSTATUS:  t->irqlatch = 0; break;

        default: break;
    }
}

bool s5l_timer_tick(s5l_timer_t *t, uint32_t ticks) {
    t->ticks += ticks;                        /* unconditional: this is time */

    if ((t->t4_state & TIMER4_STATE_START) && ticks) {
        if (timer4_pwm(t)) {
            timer4_tick_pwm(t, ticks);
        } else {
            /* Preserve the existing interval path algebraically. WFI can
             * legitimately cross complete periods in one call. */
            uint32_t value = t->t4_value;
            uint32_t period = t->t4_count;

            if (value == 0) value = period;
            if (value != 0) {
                if (ticks < value) {
                    t->t4_value = value - ticks;
                } else {
                    uint32_t after_first = ticks - value;
                    t->irqlatch |= TIMER4_IRQ_BITS;
                    if (period == 0) {
                        /* A non-zero live value with a zero reload expires
                         * once, then remains stopped at zero. */
                        t->t4_value = 0;
                    } else {
                        uint32_t phase = after_first % period;
                        t->t4_value = phase ? period - phase : period;
                    }
                }
            }
        }
    }
    /* PWM match flags latch even when masked, but only enabled matches drive
     * the line. XNU enables match0, not match1. Keep the legacy non-PWM path
     * unchanged; this is not a claim to implement capture/one-shot modes. */
    if (timer4_pwm(t))
        return (t->irqlatch & (t->t4_config << 4) & TIMER4_IRQ_BITS) != 0;
    return t->irqlatch != 0;
}
