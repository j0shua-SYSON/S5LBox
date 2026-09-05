/* ARM PrimeCell PL192 register, priority and daisy-chain behavior.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#include "pl192.h"
#include <string.h>

static uint32_t pending(const pl192_t *v) { return v->input | v->soft; }

/* Equal priority never preempts an ISR. Hardware source order breaks ties
 * only among simultaneously eligible requests, with daisy after source31. */
static unsigned candidate(const pl192_t *v, unsigned *level) {
    unsigned ceiling = 16u, source = 33u;
    for (unsigned p = 0; p < 16u; p++) {
        if (v->in_service & (1u << p)) { ceiling = p; break; }
    }
    uint32_t irq = pending(v) & v->enable & ~v->select;
    for (unsigned i = 0; i < 33u; i++) {
        unsigned p = i == 32u ? v->daisy_priority : v->priority[i];
        bool requested = i == 32u ? v->daisy_irq : (irq & (1u << i)) != 0u;
        if (requested && p < ceiling && (v->software_mask & (1u << p))) {
            source = i;
            ceiling = p;
        }
    }
    *level = ceiling;
    return source;
}

/* VICADDRESS retains the last eligible vector even if its source withdraws
 * before the CPU reads it. A vector value of zero is still a valid request. */
static void update_vector(pl192_t *v) {
    unsigned level;
    unsigned source = candidate(v, &level);
    if (source < 33u) v->last_vector = source == 32u ? v->daisy_vector : v->vector[source];
}

void pl192_reset(pl192_t *v) {
    memset(v, 0, sizeof *v);
    memset(v->priority, 15, sizeof v->priority);
    v->daisy_priority = 15u;
    v->software_mask = 0xffffu;
}

bool pl192_set_line(pl192_t *v, unsigned line, bool asserted) {
    if (line >= 32u) return false;
    if (asserted) v->input |= 1u << line;
    else v->input &= ~(1u << line);
    update_vector(v);
    return true;
}

void pl192_set_daisy(pl192_t *v, bool irq, bool fiq, uint32_t vector) {
    v->daisy_irq = irq; v->daisy_fiq = fiq; v->daisy_vector = vector;
    update_vector(v);
}

bool pl192_irq(const pl192_t *v) {
    unsigned level;
    return candidate(v, &level) < 33u;
}

bool pl192_fiq(const pl192_t *v) {
    return (pending(v) & v->enable & v->select) != 0u || v->daisy_fiq;
}

uint32_t pl192_vector(const pl192_t *v) { return v->last_vector; }

static bool accessible(const pl192_t *v, uint32_t offset, bool privileged) {
    return !(offset & 3u) && offset < 0x1000u &&
           (privileged || (!v->protection && offset != PL192_PROTECTION));
}

bool pl192_read(pl192_t *v, uint32_t offset, bool privileged, uint32_t *value) {
    if (!value || !accessible(v, offset, privileged)) return false;
    uint32_t result;
    if (offset >= PL192_VECTADDR0 && offset < PL192_VECTADDR0 + 128u) {
        result = v->vector[(offset - PL192_VECTADDR0) / 4u];
    } else if (offset >= PL192_VECTPRIORITY0 && offset < PL192_VECTPRIORITY0 + 128u) {
        result = v->priority[(offset - PL192_VECTPRIORITY0) / 4u];
    } else if (offset >= 0xfe0u) {
        /* DDI0273A table3-1: revision0, 32 sources, standard component ID. */
        static const uint8_t id[] = {0x92,0x11,0x04,0x00,0x0d,0xf0,0x05,0xb1};
        result = id[(offset - 0xfe0u) / 4u];
    } else {
        switch (offset) {
            case PL192_IRQSTATUS: result = pending(v) & v->enable & ~v->select; break;
            case PL192_FIQSTATUS: result = pending(v) & v->enable & v->select; break;
            case PL192_RAWINTR: result = pending(v); break;
            case PL192_INTSELECT: result = v->select; break;
            case PL192_INTENABLE: result = v->enable; break;
            case PL192_SOFTINT: result = v->soft; break;
            case PL192_PROTECTION: result = v->protection; break;
            case PL192_SWPRIORITYMASK: result = v->software_mask; break;
            case PL192_PRIORITYDAISY: result = v->daisy_priority; break;
            case PL192_ADDRESS: {
                unsigned level;
                unsigned source = candidate(v, &level);
                result = v->last_vector;
                if (source < 33u) v->in_service |= (uint16_t)(1u << level);
                update_vector(v);
                break;
            }
            default: return false;
        }
    }
    *value = result;
    return true;
}

bool pl192_write(pl192_t *v, uint32_t offset, bool privileged, uint32_t value) {
    if (!accessible(v, offset, privileged)) return false;
    if (offset >= PL192_VECTADDR0 && offset < PL192_VECTADDR0 + 128u) {
        v->vector[(offset - PL192_VECTADDR0) / 4u] = value;
    } else if (offset >= PL192_VECTPRIORITY0 && offset < PL192_VECTPRIORITY0 + 128u) {
        if (value & ~15u) return false;
        v->priority[(offset - PL192_VECTPRIORITY0) / 4u] = (uint8_t)value;
    } else {
        switch (offset) {
            case PL192_INTSELECT: v->select = value; break;
            case PL192_INTENABLE: v->enable |= value; break;
            case PL192_INTENCLEAR: v->enable &= ~value; break;
            case PL192_SOFTINT: v->soft |= value; break;
            case PL192_SOFTINTCLEAR: v->soft &= ~value; break;
            case PL192_PROTECTION:
                if (value & ~1u) return false;
                v->protection = (value & 1u) != 0u;
                break;
            case PL192_SWPRIORITYMASK:
                if (value & ~0xffffu) return false;
                v->software_mask = (uint16_t)value;
                break;
            case PL192_PRIORITYDAISY:
                if (value & ~15u) return false;
                v->daisy_priority = (uint8_t)value;
                break;
            case PL192_ADDRESS:
                /* Nested service levels strictly decrease. End the innermost
                 * level, preserving the masks for all suspended outer ISRs. */
                for (unsigned p = 0; p < 16u; p++) {
                    if (v->in_service & (1u << p)) {
                        v->in_service &= (uint16_t)~(1u << p);
                        break;
                    }
                }
                break;
            default: return false;
        }
    }
    update_vector(v);
    return true;
}
