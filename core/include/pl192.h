/* ARM PrimeCell PL192 functional interrupt controller. DDI0273A chapters2/3.
 * Independent of the legacy S5L8900 controller and snapshot state.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_PL192_H
#define S5LBOX_PL192_H

#include <stdbool.h>
#include <stdint.h>

enum {
    PL192_IRQSTATUS = 0x000, PL192_FIQSTATUS = 0x004, PL192_RAWINTR = 0x008,
    PL192_INTSELECT = 0x00c, PL192_INTENABLE = 0x010, PL192_INTENCLEAR = 0x014,
    PL192_SOFTINT = 0x018, PL192_SOFTINTCLEAR = 0x01c, PL192_PROTECTION = 0x020,
    PL192_SWPRIORITYMASK = 0x024, PL192_PRIORITYDAISY = 0x028,
    PL192_VECTADDR0 = 0x100, PL192_VECTPRIORITY0 = 0x200, PL192_ADDRESS = 0xf00
};

typedef struct pl192 {
    uint32_t input, enable, select, soft;
    uint32_t vector[32], last_vector, daisy_vector;
    uint16_t software_mask, in_service;
    uint8_t priority[32], daisy_priority;
    bool protection, daisy_irq, daisy_fiq;
} pl192_t;

/* Initialize/reset registers and deassert inputs; the board must redrive any
 * external levels afterwards. Inputs are stable logical levels, without the
 * AHB/synchronizer clock latency or integration-test circuitry. */
void pl192_reset(pl192_t *v);
bool pl192_set_line(pl192_t *v, unsigned line, bool asserted);
void pl192_set_daisy(pl192_t *v, bool irq, bool fiq, uint32_t vector);
bool pl192_irq(const pl192_t *v);
bool pl192_fiq(const pl192_t *v);
uint32_t pl192_vector(const pl192_t *v);

/* Aligned 32-bit register accesses. False refuses an unsupported selector,
 * reserved-bit violation, or protected User access before mutation. A refused
 * read preserves *value. Protection itself is always privileged. Identity is
 * the documented revision0, 32-source PL192; board-specific revision is separate.
 * Reading ADDRESS acknowledges an eligible IRQ; any ADDRESS write ends the
 * highest-priority active service. The daisy interface uses VIC0 blocking mode
 * (DDI0273A2.3.2): acknowledgement is not forwarded to the next controller. */
bool pl192_read(pl192_t *v, uint32_t offset, bool privileged, uint32_t *value);
bool pl192_write(pl192_t *v, uint32_t offset, bool privileged, uint32_t value);

#endif
