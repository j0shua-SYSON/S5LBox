/* S5L8920 SWI programming and an explicit foreground transmit boundary.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_SWI_H
#define S5LBOX_S5L8920_SWI_H
#include <stdbool.h>
#include <stdint.h>

#define S5L8920_SWI_BASE UINT32_C(0x89100000)
typedef struct {
    uint32_t transfer_cycles; /* Explicit interval in divided source cycles. */
    uint32_t idle_primary, idle_secondary; /* Supplied nonbusy readbacks. */
} s5l8920_swi_input_t;
typedef struct {
    uint64_t sequence;
    uint32_t control, str_delay, word, command;
} s5l8920_swi_request_t;
typedef struct {
    s5l8920_swi_input_t initial;
    s5l8920_swi_request_t request;
    uint64_t sequence;
    uint32_t control, str_delay, primary_data, secondary_data, primary_status;
    uint32_t remaining, phase;
    uint8_t known;
    bool configured, pending;
} s5l8920_swi_t;

/* Caller declares a powered, idle controller and nonzero functional transmit
 * interval. Idle readbacks may contain bit1 only; no silicon reset or command
 * bit1 readback is inferred. The supplied primary idle value is also used
 * after the external receiver accepts a completed transfer. Configure a zero
 * object once; identical input preserves progress, conflicting input refuses.
 * Host reset clears programming/requests, retains inputs and the sequence
 * counter so a stale receiver cannot consume a later request after reset. */
bool s5l8920_swi_configure(s5l8920_swi_t *s, const s5l8920_swi_input_t *input);
void s5l8920_swi_reset(s5l8920_swi_t *s);

/* Aligned words: control00, primary command/status14, primary data18,
 * secondary status1c/data20, STR-delay24. Configuration/data reads require
 * prior programming. Control supports the observed low-bit pair0/3 and an
 * eight-bit bounded divider encoding in15:8 (divisor=encoding+1). Intermediate
 * low-bit combinations, unknown fields and active reconfiguration refuse.
 * Data accepts the bounded15-bit words emitted by the matching driver.
 * Primary command1/3 latches a request, requiring enabled control, data and
 * STR-delay programming. Polling cannot clear busy or advance clocks.
 * Secondary activation/arbitration, IRQ registers and all other offsets
 * remain unavailable; only rewriting its supplied idle value is accepted. */
bool s5l8920_swi_read(const s5l8920_swi_t *s, uint32_t offset, uint32_t *value);
bool s5l8920_swi_write(s5l8920_swi_t *s, uint32_t offset, uint32_t value);

/* Supply independent NCLK cycles. The programmed divider and supplied interval
 * determine earliest receiver eligibility. Busy remains asserted until take.
 * peek is passive; take requires the exact sequence and elapsed interval.
 * The qualified external receiver must account for command mode and STR-delay
 * semantics, serialization and any regulator effects before accepting. This
 * boundary does not infer a wire waveform, voltage, backlight, ACK or IRQ.
 * Missing clocks or receiver therefore cannot produce transfer success. */
bool s5l8920_swi_source_clock(s5l8920_swi_t *s, uint64_t cycles);
bool s5l8920_swi_peek(const s5l8920_swi_t *s, s5l8920_swi_request_t *request, bool *ready);
bool s5l8920_swi_take(s5l8920_swi_t *s, uint64_t sequence, s5l8920_swi_request_t *request);
#endif
