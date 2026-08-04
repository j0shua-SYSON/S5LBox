/* Internal bridge between the SoC run loop and the optional signed engine. */
#ifndef S5LBOX_A64_STATIC_ENGINE_H
#define S5LBOX_A64_STATIC_ENGINE_H

#include "soc.h"

/* The generic engine initializes at sixteen. The iOS product explicitly uses
 * this larger ceiling after the exact Apple-host gate; the SoC loop still
 * clamps every invocation to the first timebase edge. */
#define S5LBOX_STATIC_A64_PRODUCT_CHAIN_INSNS 256u

/* Try an already-translated, time-bounded chain. Each decoded head remains at
 * most sixteen guest instructions, stays inside the current proven fetch block
 * and repeats the cache/raw-byte witness. The configured total cannot exceed
 * the product ceiling above. Zero means fall back to the architectural
 * interpreter without changing guest state. */
unsigned s5l8900_static_a64_try(s5l8900_t *m, unsigned max_insns);

/* Release the per-machine decode cache. Safe on an uninitialised NULL field. */
void s5l8900_static_a64_dispose(s5l8900_t *m);

#endif /* S5LBOX_A64_STATIC_ENGINE_H */
