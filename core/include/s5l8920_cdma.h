/* Partial S5L8920 CDMA AES and backed peripheral receive paths.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed. */
#ifndef S5LBOX_S5L8920_CDMA_H
#define S5LBOX_S5L8920_CDMA_H
#include "aes.h"

#define S5L8920_CDMA_BASE UINT32_C(0x87000000)
#define S5L8920_AES_CONTEXT1 UINT32_C(0x87801000)

/* Return a contiguous RAM view, or NULL without side effects. Never MMIO.
 * Views returned by one owner must refer to the same backing allocation. */
typedef uint8_t *(*s5l8920_cdma_memory_fn)(void *,uint32_t,uint32_t);
typedef struct {
    uint32_t control, remaining, descriptor, address, terminal;
    bool reset, size_programmed, descriptor_programmed, started;
} s5l8920_cdma_channel_t;

#define S5L8920_CDMA_NAND_FIRST 5u
#define S5L8920_CDMA_NAND_COUNT 4u
#define S5L8920_CDMA_NAND_IRQ_BASE 0x2au
#define S5L8920_CDMA_MAX_SEGMENTS 31u
typedef struct {
    uint32_t descriptor,next,flags,address,length;
} s5l8920_cdma_segment_t;
typedef struct {
    s5l8920_cdma_channel_t state;
    uint32_t configuration,peripheral,offset;
    unsigned count,index;
    bool configuration_programmed,peripheral_programmed;
    s5l8920_cdma_segment_t segments[S5L8920_CDMA_MAX_SEGMENTS];
} s5l8920_cdma_nand_t;

/* Host bound for pending kernel data descriptors, with 0x303 command ends.
 * The following flags0 descriptor remains an unsupported engine transition. */
#define S5L8920_CDMA_AMC_CHANNEL 24u
#define S5L8920_CDMA_AMC_MAX_SEGMENTS 127u
typedef struct {
    s5l8920_cdma_channel_t state;
    uint32_t configuration,peripheral,offset;
    unsigned count,index;
    bool configuration_programmed,peripheral_programmed;
    s5l8920_cdma_segment_t segments[S5L8920_CDMA_AMC_MAX_SEGMENTS],boundary;
} s5l8920_cdma_amc_t;

/* A transient peripheral supplies exactly the requested bytes on READY.
 * WAIT preserves its FIFO and all DMA state; UNAVAILABLE refuses the service.
 * WAIT/refusal output bytes are discarded. A callback may consume its endpoint
 * FIFO, but neither it nor the RAM provider may reconfigure/reset the controller
 * or board, reenter service, or invalidate RAM views during the call. */
typedef enum {
    S5L8920_CDMA_UNAVAILABLE=0, S5L8920_CDMA_WAIT=1, S5L8920_CDMA_READY=2
} s5l8920_cdma_input_t;
typedef s5l8920_cdma_input_t (*s5l8920_cdma_receive_fn)(void *owner,uint32_t peripheral,
                                                    uint8_t *bytes,unsigned count);
typedef struct {
    s5l8920_cdma_channel_t channel[2]; /* Channels 1 (feed) and 2 (drain). */
    uint32_t setup;
    uint8_t key[32], iv[16], chain[16], key_programmed, iv_programmed;
    aes_ctx_t cipher;
    bool initialized, setup_programmed, servicing;
    s5l8920_cdma_nand_t nand[S5L8920_CDMA_NAND_COUNT];
    s5l8920_cdma_amc_t amc;
} s5l8920_cdma_t;

/* First use requires a zero-initialized object. No default key, IV, context
 * or channel register values. Init refuses reentry during endpoint service.
 * Functional reset
 * discards an in-flight operation and invalidates all programming. */
void s5l8920_cdma_init(s5l8920_cdma_t *c);
bool s5l8920_cdma_read(const s5l8920_cdma_t *c,bool aes,uint32_t offset,uint32_t *value);
bool s5l8920_cdma_write(s5l8920_cdma_t *c,bool aes,uint32_t offset,uint32_t value,
                      s5l8920_cdma_memory_fn memory,void *owner);
/* One explicit work quantum processes at most one real AES block. This is
 * functional scheduling, not a measured hardware clock or transfer latency.
 * Reads and CPU execution never service DMA. Waiting/idle is successful;
 * unavailable RAM refuses without consuming a block or declaring completion.
 * Only context1, custom-key CBC, channels1/2, one whole-block data descriptor
 * per channel followed by a terminal descriptor are supported. In-place is
 * allowed; this AES service refuses partial overlap, hardware keys, peripherals, scatter/gather,
 * live reprogramming, hold/abort, interrupt mode and other channel/context
 * modes refuse. This is the bootloader's polled memory transfer path only. */
bool s5l8920_cdma_service(s5l8920_cdma_t *c,s5l8920_cdma_memory_fn memory,void *owner);
/* NAND receive channels5/7 use FMI+14, configuration58;6/8 use FMI+18,
 * configuration0. Explicit reset/programming and a validated RAM descriptor
 * chain are required. Up to31 nonempty segments (flags3, final103) followed by
 * a flags0 terminal are supported. Live descriptor edits and descriptor/output
 * overlap refuse. GO1C0009 enables completion events; W1C acknowledges them.
 * One call moves at most16 supplied bytes, never crosses a descriptor, and
 * never advances CPU/time. No peripheral, FIFO data or readiness is implicit.
 * Other modes, encryption, outbound DMA, hold/abort and error IRQs refuse.
 * These entry points do not implement the FMI data producer. */
bool s5l8920_cdma_nand_service(s5l8920_cdma_t *c,unsigned channel,
                              s5l8920_cdma_memory_fn memory,void *memory_owner,
                              s5l8920_cdma_receive_fn receive,void *receive_owner);
bool s5l8920_cdma_nand_irq(const s5l8920_cdma_t *c,unsigned channel);
/* Standalone channel24 receive commands: configuration300D8, endpoint8480002C,
 * GO19. Explicit reset and all programming are required; initial current
 * address is unavailable until GO. Preflight latches 1..127 data descriptors:
 * ordinary flags3 and command ends303, followed by a backed flags0 boundary.
 * Active GO19 appends data at the previous zero tail with its link unchanged.
 * Pending descriptors must remain identical; retired entries can be reused.
 * Preflight compacts only retired entries, preserves the partial offset,
 * progress registers and pending CIR, and refuses GO without new data. Only this
 * combined303 form is interpreted; standalone100/200 modes remain refused.
 * Service moves at most min(budget,16,segment remainder) supplied bytes, with
 * no crossing of a descriptor. Closed clock/zero budget/WAIT do no work.
 * CIR100000 is latched only after every byte of the final303 descriptor is
 * stored. Control bit10 enables its IRQ; W1C acknowledges it. This is a
 * software-derived functional command boundary, not a measured DMA latency.
 * Remaining count must exceed all pending bytes: no size-exhaustion/DONE
 * transition is inferred. At flags0 the next non-paused service refuses,
 * preserving actual progress and active state. Descriptor changes before GO
 * still refuse service. Active register reprogramming, hold/abort, board
 * mapping, clock source, producer and IRQ routing remain unsupported.
 * bytes_moved is required and set to zero even on refusal. Callback rules are
 * the same as NAND receive; RAM views must remain stable during each call. */
bool s5l8920_cdma_amc_service(s5l8920_cdma_t *c,bool clock_open,unsigned budget,
                             s5l8920_cdma_memory_fn memory,void *memory_owner,
                             s5l8920_cdma_receive_fn receive,void *receive_owner,
                             unsigned *bytes_moved);
bool s5l8920_cdma_amc_irq(const s5l8920_cdma_t *c);
#endif
