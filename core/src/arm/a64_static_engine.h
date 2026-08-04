/* Internal bridge between the SoC run loop and the optional signed engine. */
#ifndef S5LBOX_A64_STATIC_ENGINE_H
#define S5LBOX_A64_STATIC_ENGINE_H

#include "soc.h"

/* Try one already-translated, time-bounded block. Zero means fall back to the
 * architectural interpreter without changing guest state. */
unsigned s5l8900_static_a64_try(s5l8900_t *m, unsigned max_insns);

/* Release the per-machine decode cache. Safe on an uninitialised NULL field. */
void s5l8900_static_a64_dispose(s5l8900_t *m);

#endif /* S5LBOX_A64_STATIC_ENGINE_H */
