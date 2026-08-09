/* Internal bridge between the SoC run loop and the optional signed engine. */
#ifndef S5LBOX_A64_STATIC_ENGINE_H
#define S5LBOX_A64_STATIC_ENGINE_H

#include "soc.h"

/* The generic engine initializes at sixteen. The iOS product explicitly uses
 * this larger ceiling after the exact Apple-host gate. Deterministic execution
 * clamps each invocation to the first timebase edge; optional active host
 * timing may use the whole ceiling because elapsed edges advance together at
 * the next bounded host sample. */
#define S5LBOX_STATIC_A64_PRODUCT_CHAIN_INSNS 256u

/* Cheap execution-policy gate. In the LTO iOS product this folds to the
 * opaque-state pointer and its first enabled byte, allowing a compiled but
 * disabled engine to avoid the much larger timebase/input eligibility path. */
bool s5l8900_static_a64_is_enabled(const s5l8900_t *m);

/* Try an already-translated, time-bounded chain. Each decoded head remains at
 * most sixteen guest instructions, stays inside the current proven fetch block
 * and repeats the cache/raw-byte witness. The configured total cannot exceed
 * the product ceiling above. Zero means fall back to the architectural
 * interpreter without changing guest state. When a positive exact prefix ends
 * at an unchanged decoded negative, or a privileged compact interval leaves
 * its fallback instruction untouched, `known_negative` reports that the next
 * signed probe is redundant. */
unsigned s5l8900_static_a64_try(s5l8900_t *m, unsigned max_insns,
                                bool *known_negative,
                                arm_status_t *status);

/* One exact architectural fallback for the resident compact loop. Implemented
 * beside the SoC run loop because only machine.c owns the complete
 * level/input boundary. Return 0 for no retirement, 1 to continue the bounded
 * resident interval, or 2 after one retirement that must return to the device
 * tick immediately. */
unsigned s5l8900_static_a64_fallback_step(s5l8900_t *m,
                                          arm_status_t *status);

/* Revalidate and account a known-negative probe bypass after the device tick.
 * A compact pending witness is single-use; decoded negatives remain governed
 * by their complete cache witness. False leaves the ordinary next-loop
 * decision untouched. */
bool s5l8900_static_a64_commit_known_negative_bypass(s5l8900_t *m);

/* Release the per-machine decode cache. Safe on an uninitialised NULL field. */
void s5l8900_static_a64_dispose(s5l8900_t *m);

#endif /* S5LBOX_A64_STATIC_ENGINE_H */
