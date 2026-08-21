/*
 * S5LBox — the on-device demo guest. See VMGuest.h for what this is for.
 *
 * The payload is emitted here by a ~40-line assembler rather than written out
 * as a table of magic words. That is not decoration: every branch target is
 * then computed from the label's actual position, so inserting an instruction
 * cannot silently break a loop, and each encoding appears exactly once next to
 * the mnemonic it implements.
 *
 * The program, in ARM assembly:
 *
 *     ldr   r7, =UART0_BASE
 *     ldr   r8, =FB_BASE
 *     mov   r9, #320                 @ width
 *     mov   r10,#480                 @ height
 *     mov   r4, #0                   @ t: frame counter
 *     ldr   r6, =banner
 * 1:  ldrb  r5, [r6], #1             @ print(banner)
 *     cmp   r5, #0
 *     strne r5, [r7, #UART_UTXH]
 *     bne   1b
 * frame:
 *     mov   r0, r8                   @ dst = framebuffer
 *     mov   r3, #0                   @ y = 0
 * yloop:
 *     add   r11, r3, r4              @ green and alpha are constant per row,
 *     and   r11, r11, #0xff          @ so hoist them out of the inner loop
 *     mov   r11, r11, lsl #8
 *     orr   r11, r11, #0xff000000
 *     mov   r2, #0                   @ x = 0
 * xloop:
 *     eor   r5, r2, r3               @ blue  = (x ^ y) & 0xff
 *     and   r5, r5, #0xff
 *     add   r6, r2, r4               @ red   = (x + t) & 0xff
 *     and   r6, r6, #0xff
 *     orr   r5, r5, r6, lsl #16
 *     orr   r5, r5, r11              @ green = (y + t) & 0xff, alpha = 0xff
 *     str   r5, [r0], #4
 *     add   r2, r2, #1
 *     cmp   r2, r9
 *     bne   xloop
 *     add   r3, r3, #1
 *     cmp   r3, r10
 *     bne   yloop
 *     add   r4, r4, #1
 *     ldr   r6, =msg                 @ print("frame ")
 * 2:  ldrb  r5, [r6], #1
 *     cmp   r5, #0
 *     strne r5, [r7, #UART_UTXH]
 *     bne   2b
 *     mov   r5, r4, lsr #4           @ print(hex(t & 0xff))
 *     and   r5, r5, #0x0f
 *     cmp   r5, #9
 *     add   r5, r5, #'0'
 *     addhi r5, r5, #7
 *     str   r5, [r7, #UART_UTXH]
 *     and   r5, r4, #0x0f
 *     cmp   r5, #9
 *     add   r5, r5, #'0'
 *     addhi r5, r5, #7
 *     str   r5, [r7, #UART_UTXH]
 *     mov   r5, #'\n'
 *     str   r5, [r7, #UART_UTXH]
 *     b     frame
 *
 * Note the loops only ever branch BACKWARDS, and the two string printers exit
 * by falling through a conditional store rather than a forward branch. That is
 * why no relocation pass is needed: every target is already known when the
 * branch is emitted.
 *
 * Cost is 10 instructions per pixel, so a full 320x480 repaint is about 1.54
 * million instructions. On an interpreter retiring a few million instructions a
 * second that is a fraction of a second per frame — which is exactly the point:
 * the UI samples guest DRAM asynchronously, so you watch the raster sweep down
 * the panel. A finished frame appearing atomically would look like a mock.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMGuest.h"
#include "VMFrameTelemetry.h"

#include <string.h>

_Static_assert(S5L_MBX_3D_REJECTION_HISTORY ==
                   VM_MBX_3D_REJECTION_HISTORY,
               "core/app 3D rejection history counts differ");
_Static_assert(S5L_MBX_3D_REJECTION_RECORD_WORDS ==
                   VM_MBX_3D_REJECTION_RECORD_WORDS,
               "core/app 3D rejection record sizes differ");
_Static_assert(S5L_MBX_3D_REJECTION_TA_WORDS ==
                   VM_MBX_3D_REJECTION_TA_WORDS,
               "core/app 3D rejection TA windows differ");
_Static_assert(sizeof(s5l_mbx_3d_rejection_witness_t) ==
                   sizeof(vm_mbx_3d_rejection_witness_t),
               "core/app 3D rejection witness layouts differ");
#define VM_ASSERT_MBX_REJECTION_OFFSET(field_) \
    _Static_assert(offsetof(s5l_mbx_3d_rejection_witness_t, field_) == \
                       offsetof(vm_mbx_3d_rejection_witness_t, field_), \
                   "core/app 3D rejection witness offsets differ")
VM_ASSERT_MBX_REJECTION_OFFSET(sequence);
VM_ASSERT_MBX_REJECTION_OFFSET(tiled_reason_hash);
VM_ASSERT_MBX_REJECTION_OFFSET(status_reason_hash);
VM_ASSERT_MBX_REJECTION_OFFSET(sprite_reason_hash);
VM_ASSERT_MBX_REJECTION_OFFSET(solid_reason_hash);
VM_ASSERT_MBX_REJECTION_OFFSET(ta_reason_hash);
VM_ASSERT_MBX_REJECTION_OFFSET(ta_word_count);
VM_ASSERT_MBX_REJECTION_OFFSET(ta_failure_word);
VM_ASSERT_MBX_REJECTION_OFFSET(ta_window_start_word);
VM_ASSERT_MBX_REJECTION_OFFSET(ta_window_valid_words);
VM_ASSERT_MBX_REJECTION_OFFSET(ta_window_words);
VM_ASSERT_MBX_REJECTION_OFFSET(region);
VM_ASSERT_MBX_REJECTION_OFFSET(object);
VM_ASSERT_MBX_REJECTION_OFFSET(target);
VM_ASSERT_MBX_REJECTION_OFFSET(xclip);
VM_ASSERT_MBX_REJECTION_OFFSET(yclip);
VM_ASSERT_MBX_REJECTION_OFFSET(pixel_sample);
VM_ASSERT_MBX_REJECTION_OFFSET(framebuffer_control);
VM_ASSERT_MBX_REJECTION_OFFSET(framebuffer_stride);
VM_ASSERT_MBX_REJECTION_OFFSET(list_valid_mask);
VM_ASSERT_MBX_REJECTION_OFFSET(list_words);
VM_ASSERT_MBX_REJECTION_OFFSET(record_base);
VM_ASSERT_MBX_REJECTION_OFFSET(record_valid_words);
VM_ASSERT_MBX_REJECTION_OFFSET(record_words);
#undef VM_ASSERT_MBX_REJECTION_OFFSET

_Static_assert(S5L_MBX_3D_ACCEPT_HISTORY == VM_MBX_3D_ACCEPT_HISTORY,
               "core/app 3D acceptance history counts differ");
_Static_assert(S5L_MBX_3D_ACCEPT_RECORD_WORDS ==
                   VM_MBX_3D_ACCEPT_RECORD_WORDS,
               "core/app 3D acceptance record sizes differ");
_Static_assert(sizeof(s5l_mbx_3d_accept_witness_t) ==
                   sizeof(vm_mbx_3d_accept_witness_t),
               "core/app 3D acceptance witness layouts differ");
#define VM_ASSERT_MBX_ACCEPT_OFFSET(field_) \
    _Static_assert(offsetof(s5l_mbx_3d_accept_witness_t, field_) == \
                       offsetof(vm_mbx_3d_accept_witness_t, field_), \
                   "core/app 3D acceptance witness offsets differ")
VM_ASSERT_MBX_ACCEPT_OFFSET(sequence);
VM_ASSERT_MBX_ACCEPT_OFFSET(record_hash);
VM_ASSERT_MBX_ACCEPT_OFFSET(kind);
VM_ASSERT_MBX_ACCEPT_OFFSET(pixels);
VM_ASSERT_MBX_ACCEPT_OFFSET(region);
VM_ASSERT_MBX_ACCEPT_OFFSET(object);
VM_ASSERT_MBX_ACCEPT_OFFSET(target);
VM_ASSERT_MBX_ACCEPT_OFFSET(target_physical);
VM_ASSERT_MBX_ACCEPT_OFFSET(target_mapping_span);
VM_ASSERT_MBX_ACCEPT_OFFSET(xclip);
VM_ASSERT_MBX_ACCEPT_OFFSET(yclip);
VM_ASSERT_MBX_ACCEPT_OFFSET(pixel_sample);
VM_ASSERT_MBX_ACCEPT_OFFSET(framebuffer_control);
VM_ASSERT_MBX_ACCEPT_OFFSET(framebuffer_stride);
VM_ASSERT_MBX_ACCEPT_OFFSET(list_valid_mask);
VM_ASSERT_MBX_ACCEPT_OFFSET(list_words);
VM_ASSERT_MBX_ACCEPT_OFFSET(record_base);
VM_ASSERT_MBX_ACCEPT_OFFSET(record_valid_words);
VM_ASSERT_MBX_ACCEPT_OFFSET(record_words);
#undef VM_ASSERT_MBX_ACCEPT_OFFSET

_Static_assert(S5L_MBX_3D_TARGET_LEDGER == VM_MBX_3D_TARGET_LEDGER,
               "core/app 3D target-ledger counts differ");
_Static_assert(sizeof(s5l_mbx_3d_target_ledger_t) ==
                   sizeof(vm_mbx_3d_target_ledger_t),
               "core/app 3D target-ledger layouts differ");
#define VM_ASSERT_MBX_TARGET_OFFSET(field_) \
    _Static_assert(offsetof(s5l_mbx_3d_target_ledger_t, field_) == \
                       offsetof(vm_mbx_3d_target_ledger_t, field_), \
                   "core/app 3D target-ledger offsets differ")
VM_ASSERT_MBX_TARGET_OFFSET(last_sequence);
VM_ASSERT_MBX_TARGET_OFFSET(completed);
VM_ASSERT_MBX_TARGET_OFFSET(pixels);
VM_ASSERT_MBX_TARGET_OFFSET(target);
VM_ASSERT_MBX_TARGET_OFFSET(target_physical);
VM_ASSERT_MBX_TARGET_OFFSET(target_mapping_span);
VM_ASSERT_MBX_TARGET_OFFSET(last_kind);
#undef VM_ASSERT_MBX_TARGET_OFFSET

_Static_assert(S5L_POWER_TRACE_HISTORY == VM_POWER_TRACE_HISTORY,
               "core/app Power trace history counts differ");
#define VM_ASSERT_POWER_TRACE_EVENT(core_, app_) \
    _Static_assert((unsigned)(core_) == (unsigned)(app_), \
                   "core/app Power trace event values differ")
VM_ASSERT_POWER_TRACE_EVENT(S5L_POWER_TRACE_EVENT_STATE,
                            VM_POWER_TRACE_EVENT_STATE);
VM_ASSERT_POWER_TRACE_EVENT(S5L_POWER_TRACE_EVENT_HOST_PRESS,
                            VM_POWER_TRACE_EVENT_HOST_PRESS);
VM_ASSERT_POWER_TRACE_EVENT(S5L_POWER_TRACE_EVENT_HOST_RELEASE,
                            VM_POWER_TRACE_EVENT_HOST_RELEASE);
VM_ASSERT_POWER_TRACE_EVENT(S5L_POWER_TRACE_EVENT_HOST_REFUSED,
                            VM_POWER_TRACE_EVENT_HOST_REFUSED);
VM_ASSERT_POWER_TRACE_EVENT(S5L_POWER_TRACE_EVENT_RELEASE_WAIT,
                            VM_POWER_TRACE_EVENT_RELEASE_WAIT);
VM_ASSERT_POWER_TRACE_EVENT(S5L_POWER_TRACE_EVENT_WAKE_BEGIN,
                            VM_POWER_TRACE_EVENT_WAKE_BEGIN);
VM_ASSERT_POWER_TRACE_EVENT(S5L_POWER_TRACE_EVENT_WAKE_RESET,
                            VM_POWER_TRACE_EVENT_WAKE_RESET);
VM_ASSERT_POWER_TRACE_EVENT(S5L_POWER_TRACE_EVENT_WAKE_FAILED,
                            VM_POWER_TRACE_EVENT_WAKE_FAILED);
#undef VM_ASSERT_POWER_TRACE_EVENT
_Static_assert(sizeof(s5l_power_trace_entry_t) ==
                   sizeof(vm_power_trace_entry_t),
               "core/app Power trace entry layouts differ");
#define VM_ASSERT_POWER_TRACE_OFFSET(field_) \
    _Static_assert(offsetof(s5l_power_trace_entry_t, field_) == \
                       offsetof(vm_power_trace_entry_t, field_), \
                   "core/app Power trace entry offsets differ")
VM_ASSERT_POWER_TRACE_OFFSET(sequence);
VM_ASSERT_POWER_TRACE_OFFSET(cpu_cycles);
VM_ASSERT_POWER_TRACE_OFFSET(cpu_pc);
VM_ASSERT_POWER_TRACE_OFFSET(changes);
VM_ASSERT_POWER_TRACE_OFFSET(event);
VM_ASSERT_POWER_TRACE_OFFSET(buttons_pressed);
VM_ASSERT_POWER_TRACE_OFFSET(pmu_shutdown);
VM_ASSERT_POWER_TRACE_OFFSET(pmu_int2);
VM_ASSERT_POWER_TRACE_OFFSET(pmu_int2_mask);
VM_ASSERT_POWER_TRACE_OFFSET(power_gpio);
VM_ASSERT_POWER_TRACE_OFFSET(pmu_gpio);
VM_ASSERT_POWER_TRACE_OFFSET(clcd);
VM_ASSERT_POWER_TRACE_OFFSET(cpu_lines);
#undef VM_ASSERT_POWER_TRACE_OFFSET

/* ---------------------------------------------------------- encodings --- */

/* Condition codes (bits 31:28). */
#define C_HI 0x8u   /* unsigned higher      */
#define C_NE 0x1u   /* not equal            */
#define C_AL 0xeu   /* always               */
#define COND(c) ((uint32_t)(c) << 28)

/* Data-processing opcodes (bits 24:21). */
#define OP_AND 0x0u
#define OP_EOR 0x1u
#define OP_ADD 0x4u
#define OP_CMP 0xau
#define OP_ORR 0xcu
#define OP_MOV 0xdu

/* Barrel-shift types (bits 6:5). */
#define SH_LSL 0u
#define SH_LSR 1u

/*
 * Data processing with an immediate second operand. The operand is
 * ror(imm8, 2*rot) — ARM has no 32-bit literal field, which is why constants
 * like 320 are encoded as 0x50 rotated rather than written out.
 */
static uint32_t dp_imm(unsigned cond, unsigned op, unsigned s,
                       unsigned rn, unsigned rd, unsigned rot, unsigned imm8) {
    return COND(cond) | 0x02000000u | (op << 21) | (s << 20)
         | (rn << 16) | (rd << 12) | (rot << 8) | imm8;
}

/* Data processing with a register second operand, shifted by a constant. */
static uint32_t dp_reg(unsigned cond, unsigned op, unsigned s,
                       unsigned rn, unsigned rd, unsigned rm,
                       unsigned shtype, unsigned shamt) {
    return COND(cond) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12)
         | ((shamt & 31u) << 7) | (shtype << 5) | rm;
}

/* Single data transfer with a 12-bit immediate offset. */
static uint32_t ldst(unsigned cond, bool load, bool byte, bool pre, bool up,
                     bool wb, unsigned rn, unsigned rd, unsigned imm12) {
    return COND(cond) | 0x04000000u
         | (pre  ? 1u << 24 : 0u) | (up   ? 1u << 23 : 0u)
         | (byte ? 1u << 22 : 0u) | (wb   ? 1u << 21 : 0u)
         | (load ? 1u << 20 : 0u)
         | (rn << 16) | (rd << 12) | (imm12 & 0xfffu);
}

/* ------------------------------------------------------------ emitter --- */

/* Word index of the literal pool, and byte offset of the string blob, within
 * VM_GUEST_BLOB_BYTES. Both are far enough past the code that no relocation is
 * needed; vm_guest_install() asserts the code actually fits. */
#define LIT_WORD  0x40u        /* byte offset 0x100 */
#define STR_OFF   0x140u

typedef struct { uint32_t *w; unsigned n; } emitter_t;

static void emit(emitter_t *e, uint32_t insn) { e->w[e->n++] = insn; }

/* Branch to an already-emitted label. The 24-bit field counts words from
 * pc + 8, hence the -2. */
static void emit_b(emitter_t *e, unsigned cond, unsigned target_word) {
    int32_t off = (int32_t)target_word - (int32_t)e->n - 2;
    e->w[e->n++] = COND(cond) | 0x0a000000u | ((uint32_t)off & 0x00ffffffu);
}

/* LDR Rd,[pc,#imm] — pull a 32-bit constant out of the literal pool. */
static void emit_ldr_lit(emitter_t *e, unsigned rd, unsigned lit_word) {
    unsigned imm = (lit_word - e->n) * 4u - 8u;
    e->w[e->n++] = COND(C_AL) | 0x059f0000u | (rd << 12) | (imm & 0xfffu);
}

/* Print a NUL-terminated string whose address is already in r6. Falls out of
 * the loop on the NUL byte, so there is no forward branch to fix up. */
static void emit_puts(emitter_t *e) {
    unsigned loop = e->n;
    emit(e, ldst(C_AL, true,  true,  false, true, false, 6, 5, 1));  /* ldrb r5,[r6],#1 */
    emit(e, dp_imm(C_AL, OP_CMP, 1, 5, 0, 0, 0));                    /* cmp  r5,#0      */
    emit(e, ldst(C_NE, false, false, true,  true, false, 7, 5, UART_UTXH));
    emit_b(e, C_NE, loop);
}

/* Print one hex digit from bits [shift+3:shift] of r4. */
static void emit_hex_digit(emitter_t *e, unsigned shift) {
    if (shift) emit(e, dp_reg(C_AL, OP_MOV, 0, 0, 5, 4, SH_LSR, shift));
    else       emit(e, dp_reg(C_AL, OP_MOV, 0, 0, 5, 4, SH_LSL, 0));
    emit(e, dp_imm(C_AL, OP_AND, 0, 5, 5, 0, 0x0fu));   /* and   r5,r5,#0xf  */
    emit(e, dp_imm(C_AL, OP_CMP, 1, 5, 0, 0, 9));       /* cmp   r5,#9       */
    emit(e, dp_imm(C_AL, OP_ADD, 0, 5, 5, 0, '0'));     /* add   r5,r5,#'0'  */
    emit(e, dp_imm(C_HI, OP_ADD, 0, 5, 5, 0, 7));       /* addhi r5,r5,#7    */
    emit(e, ldst(C_AL, false, false, true, true, false, 7, 5, UART_UTXH));
}

/* ------------------------------------------------------------- public --- */

uint32_t vm_guest_fb_pa(uint32_t ram_base, uint32_t ram_size) {
    /* Leave room for the payload itself at the bottom of DRAM, and for a
     * comfortable gap, before claiming the top for the framebuffer. */
    if (ram_size < VM_FB_BYTES + VM_GUEST_BLOB_BYTES + 0x10000u) return 0;
    /* The machine API accepts an arbitrary 32-bit RAM base. Do the end
     * calculation wide: wrapping a bank near 4 GiB would otherwise turn the
     * framebuffer into a small physical address and the host-pointer helpers
     * below into an out-of-bounds read. An end exactly at 2^32 is valid as long
     * as the framebuffer itself still begins in the 32-bit address space. */
    uint64_t end = (uint64_t)ram_base + ram_size;
    if (end > 0x100000000ull) return 0;
    uint64_t fb = (end - VM_FB_BYTES) & ~(uint64_t)0xfffu;
    if (fb > UINT32_MAX || fb < ram_base) return 0;
    return (uint32_t)fb;
}

const uint8_t *vm_guest_framebuffer(const s5l8900_t *m) {
    if (!m || !m->ram) return NULL;
    uint32_t pa = vm_guest_fb_pa(m->ram_base, m->ram_size);
    if (!pa) return NULL;
    return m->ram + (pa - m->ram_base);
}

static const uint8_t *vm_guest_record_display(const s5l8900_t *m,
                                              const uint8_t *pixels,
                                              size_t bytes,
                                              const vm_frame_scanout_observation_t *observation) {
    uint64_t timer_ticks = 0;
    uint64_t clcd_frames = 0;
    uint32_t timebase_hz = 0;
    uint32_t cpu_hz = 0;
    if (m) {
        timer_ticks = m->timer.ticks;
        timebase_hz = m->tb_hz;
        cpu_hz = m->cpu_hz;
#ifdef S5L8900_CLCD_BASE
        clcd_frames = m->clcd.frames;
#endif
        if (vm_frame_telemetry_is_enabled()) {
            s5l_static_a64_compact_raw_refusals_t refusals;
            vm_execution_telemetry_observation_t execution;
            memset(&execution, 0, sizeof execution);
            s5l8900_static_a64_compact_raw_refusals(m, &refusals);
            execution.cpu_pc = m->cpu.r[15];
            execution.cpu_cpsr = m->cpu.cpsr;
            execution.cpu_irq_line = m->cpu.irq_line ? 1u : 0u;
            execution.cpu_fiq_line = m->cpu.fiq_line ? 1u : 0u;
            execution.wfi_host_pacing_enabled =
                m->wfi_host_sleep ? 1u : 0u;
            execution.active_host_clock_enabled =
                m->active_host_now ? 1u : 0u;
            execution.mtz2_attention_pending = m->mtz2.atn ? 1u : 0u;
            execution.mtz2_pending_frame_bytes = m->mtz2.frame_len;
            execution.cpu_retired = m->cpu.cycles;
            execution.interpreter_tick_batches =
                s5l8900_interpreter_tick_batches(m);
            execution.interpreter_tick_batched_retired =
                s5l8900_interpreter_tick_batched_retired(m);
            execution.static_native_retired =
                s5l8900_static_a64_retired(m);
            execution.compact_attempts =
                s5l8900_static_a64_compact_raw_attempts(m);
            execution.compact_calls =
                s5l8900_static_a64_compact_raw_calls(m);
            execution.compact_native_retired =
                s5l8900_static_a64_compact_raw_retired(m);
            execution.compact_fallback_retired =
                s5l8900_static_a64_compact_raw_fallback_retired(m);
            execution.compact_privileged_attempts =
                s5l8900_static_a64_compact_raw_privileged_attempts(m);
            execution.compact_privileged_calls =
                s5l8900_static_a64_compact_raw_privileged_calls(m);
            execution.compact_privileged_retired =
                s5l8900_static_a64_compact_raw_privileged_retired(m);
            execution.compact_privileged_window_refills =
                s5l8900_static_a64_compact_raw_privileged_window_refills(m);
            execution.compact_privileged_boundary_retired =
                s5l8900_static_a64_compact_raw_privileged_boundary_retired(m);
            execution.compact_window_crossings =
                s5l8900_static_a64_compact_raw_window_crossings(m);
            execution.compact_window_reloads =
                s5l8900_static_a64_compact_raw_window_reloads(m);
            execution.compact_window_fast_refills =
                s5l8900_static_a64_compact_raw_window_fast_refills(m);
            execution.compact_window_cache_hits =
                s5l8900_static_a64_compact_raw_window_cache_hits(m);
            execution.compact_window_stops =
                s5l8900_static_a64_compact_raw_window_stops(m);
            execution.compact_refused_guard = refusals.guard;
            execution.compact_refused_privileged = refusals.privileged;
            execution.compact_refused_alignment = refusals.alignment;
            execution.compact_refused_fetch_witness =
                refusals.fetch_witness;
            execution.compact_refused_runner = refusals.runner;
            execution.compact_zero_retired = refusals.zero_retired;
            execution.fetch_refill_attempts =
                s5l8900_static_a64_fetch_refill_attempts(m);
            execution.fetch_refill_hits =
                s5l8900_static_a64_fetch_refill_hits(m);
            execution.fetch_refill_skips =
                s5l8900_static_a64_fetch_refill_skips(m);
            execution.known_negative_bypasses =
                s5l8900_static_a64_known_negative_bypasses(m);
            execution.mbx_2d_candidates = m->mbx_telemetry.candidates_2d;
            execution.mbx_2d_completed = m->mbx_telemetry.completed_2d;
            execution.mbx_2d_rejected = m->mbx_telemetry.rejected_2d;
            execution.mbx_2d_degraded = m->mbx_telemetry.degraded_2d;
            execution.mbx_2d_bytes = m->mbx_telemetry.bytes_2d;
            execution.mbx_2d_last_rejected_ring_offset =
                m->mbx_telemetry.last_rejected_2d_ring_offset;
            execution.mbx_2d_last_rejected_count =
                m->mbx_telemetry.last_rejected_2d_count;
            execution.mbx_2d_last_rejected_reason_hash =
                m->mbx_telemetry.last_rejected_2d_reason_hash;
            execution.mbx_3d_candidates = m->mbx_telemetry.candidates_3d;
            execution.mbx_3d_completed = m->mbx_telemetry.completed_3d;
            execution.mbx_3d_rejected = m->mbx_telemetry.rejected_3d;
            execution.mbx_3d_degraded = m->mbx_telemetry.degraded_3d;
            execution.mbx_3d_pixels = m->mbx_telemetry.pixels_3d;
            memcpy(execution.mbx_3d_rejection_history,
                   m->mbx_telemetry.rejected_3d_history,
                   sizeof execution.mbx_3d_rejection_history);
            memcpy(execution.mbx_3d_accept_history,
                   m->mbx_telemetry.accepted_3d_history,
                   sizeof execution.mbx_3d_accept_history);
            memcpy(execution.mbx_3d_target_ledger,
                   m->mbx_telemetry.target_3d_ledger,
                   sizeof execution.mbx_3d_target_ledger);
            execution.active_clock_updates = m->active_clock_updates;
            execution.active_clock_added_ticks =
                m->active_clock_added_ticks;
            execution.active_clock_clamps = m->active_clock_clamps;
            execution.active_clock_failures = m->active_clock_failures;
            execution.active_clock_input_guards =
                m->active_clock_input_guards;
            execution.active_clock_input_guard_quiesces =
                m->active_clock_input_guard_quiesces;
            execution.active_clock_deadline_shields =
                m->active_clock_deadline_shields;
            execution.mtz2_frames_queued = m->mtz2.frames_queued;
            execution.mtz2_frames_read = m->mtz2.frames_read;
            execution.mtz2_length_reads = m->mtz2.length_reads;
            execution.mtz2_data_reads = m->mtz2.data_reads;
            execution.mtz2_injects_refused = m->mtz2.injects_refused;
            execution.active_clock_input_guard_active =
                m->active_clock_input_guard ? 1u : 0u;
            execution.active_clock_deadline_shield_active =
                m->active_clock_deadline_shield ? 1u : 0u;
            execution.wfi_paced_waits = m->wfi_paced_waits;
            execution.wfi_paced_wait_ns = m->wfi_paced_wait_ns;
            execution.wfi_paced_partial_advances =
                m->wfi_paced_partial_advances;
            execution.wfi_paced_failures = m->wfi_paced_failures;
            execution.power_trace_sequence = m->power_trace_sequence;
            execution.power_trace_ticks_left = m->power_trace_ticks_left;
            memcpy(execution.power_trace, m->power_trace,
                   sizeof execution.power_trace);
            s5l_static_a64_compact_pc_profile_t pc_profile;
            s5l8900_static_a64_compact_raw_pc_profile(m, &pc_profile);
            execution.compact_pc_profile_polls = pc_profile.polls;
            execution.compact_pc_profile_not_running =
                pc_profile.not_running;
            execution.compact_pc_profile_state_failures =
                pc_profile.state_failures;
            execution.compact_pc_profile_target_races =
                pc_profile.target_races;
            execution.compact_pc_profile_samples = pc_profile.samples;
            execution.compact_pc_profile_outside = pc_profile.outside;
            execution.compact_pc_profile_entry = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_ENTRY];
            execution.compact_pc_profile_dp = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_DP];
            execution.compact_pc_profile_memory = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_MEMORY];
            execution.compact_pc_profile_block_control = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_BLOCK_CONTROL];
            execution.compact_pc_profile_system = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_SYSTEM];
            execution.compact_pc_profile_vfp = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_VFP];
            execution.compact_pc_profile_thumb_decode = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_THUMB_DECODE];
            execution.compact_pc_profile_thumb_low_alu = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_THUMB_LOW_ALU];
            execution.compact_pc_profile_thumb_alu_high = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_THUMB_ALU_HIGH];
            execution.compact_pc_profile_thumb_memory_form = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_THUMB_MEMORY_FORM];
            execution.compact_pc_profile_thumb_misc = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_THUMB_MISC];
            execution.compact_pc_profile_thumb_branch = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_THUMB_BRANCH];
            execution.compact_pc_profile_thumb_memory_access =
                pc_profile.region[
                    S5L_STATIC_A64_COMPACT_PC_THUMB_MEMORY_ACCESS];
            execution.compact_pc_profile_thumb_condition = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_THUMB_CONDITION];
            execution.compact_pc_profile_a32_condition = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_A32_CONDITION];
            execution.compact_pc_profile_thumb =
                execution.compact_pc_profile_thumb_decode +
                execution.compact_pc_profile_thumb_low_alu +
                execution.compact_pc_profile_thumb_alu_high +
                execution.compact_pc_profile_thumb_memory_form +
                execution.compact_pc_profile_thumb_misc +
                execution.compact_pc_profile_thumb_branch +
                execution.compact_pc_profile_thumb_memory_access +
                execution.compact_pc_profile_thumb_condition +
                execution.compact_pc_profile_a32_condition;
            execution.compact_pc_profile_retire = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_RETIRE];
            execution.compact_pc_profile_fallback = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_FALLBACK];
            execution.compact_pc_profile_exit = pc_profile.region[
                S5L_STATIC_A64_COMPACT_PC_EXIT];
            _Static_assert(VM_COMPACT_PC_PROFILE_HOT_COUNT ==
                               S5L_STATIC_A64_COMPACT_PC_HOT_COUNT,
                           "compact PC-profile hot count drifted");
            execution.compact_pc_profile_reference_pc =
                pc_profile.reference_pc;
            execution.compact_pc_profile_outside_pc_captured =
                pc_profile.outside_pc_captured;
            execution.compact_pc_profile_outside_pc_dropped =
                pc_profile.outside_pc_dropped;
            for (unsigned i = 0u;
                 i < VM_COMPACT_PC_PROFILE_HOT_COUNT; i++) {
                execution.compact_pc_profile_outside_hot_pc[i] =
                    pc_profile.outside_hot[i].pc;
                execution.compact_pc_profile_outside_hot_samples[i] =
                    pc_profile.outside_hot[i].samples;
            }
            _Static_assert(VM_COMPACT_FALLBACK_PROFILE_OUTCOME_COUNT ==
                               S5L_STATIC_A64_COMPACT_FALLBACK_OUTCOME_COUNT,
                           "compact fallback outcome count drifted");
            _Static_assert(VM_COMPACT_FALLBACK_PROFILE_HOT_COUNT ==
                               S5L_STATIC_A64_COMPACT_FALLBACK_HOT_COUNT,
                           "compact fallback hot count drifted");
            execution.compact_fallback_profile_events =
                pc_profile.fallback_events;
            execution.compact_fallback_profile_witness_misses =
                pc_profile.fallback_witness_misses;
            memcpy(execution.compact_fallback_profile_outcome,
                   pc_profile.fallback_outcome,
                   sizeof execution.compact_fallback_profile_outcome);
            execution.compact_fallback_profile_dread_hits =
                pc_profile.fallback_dread_hits;
            execution.compact_fallback_profile_dread_misses =
                pc_profile.fallback_dread_misses;
            execution.compact_fallback_profile_dwrite_hits =
                pc_profile.fallback_dwrite_hits;
            execution.compact_fallback_profile_dwrite_misses =
                pc_profile.fallback_dwrite_misses;
            execution.compact_fallback_profile_dread_events =
                pc_profile.fallback_dread_events;
            execution.compact_fallback_profile_dwrite_events =
                pc_profile.fallback_dwrite_events;
            execution.compact_fallback_profile_mixed_data_events =
                pc_profile.fallback_mixed_data_events;
            execution.compact_fallback_profile_no_data_events =
                pc_profile.fallback_no_data_events;
            for (unsigned i = 0u;
                 i < VM_COMPACT_FALLBACK_PROFILE_HOT_COUNT; i++) {
                execution.compact_fallback_profile_hot_pc[i] =
                    pc_profile.fallback_hot[i].pc;
                execution.compact_fallback_profile_hot_insn[i] =
                    pc_profile.fallback_hot[i].insn;
                execution.compact_fallback_profile_hot_events[i] =
                    pc_profile.fallback_hot[i].events;
                execution.compact_fallback_profile_hot_error[i] =
                    pc_profile.fallback_hot[i].error;
                execution.compact_fallback_profile_hot_thumb[i] =
                    pc_profile.fallback_hot[i].thumb ? 1u : 0u;
                execution.compact_fallback_profile_hot_privileged[i] =
                    pc_profile.fallback_hot[i].privileged ? 1u : 0u;
            }
            vm_frame_telemetry_note_execution(&execution);
        }
    }
    vm_frame_telemetry_note_scanout(
        pixels, bytes, timer_ticks, clcd_frames, timebase_hz, cpu_hz,
        observation);
    return pixels;
}

const uint8_t *vm_guest_display(const s5l8900_t *m,
                                uint32_t *width, uint32_t *height,
                                uint32_t *stride, vm_pixel_order_t *order) {
    /* Never leave geometry from an earlier frame looking valid after an
     * error. Callers can treat a NULL return as a complete no-scanout state. */
    if (width)  *width = 0;
    if (height) *height = 0;
    if (stride) *stride = 0;
    if (order)  *order = VM_ORDER_BGRA;
    vm_frame_scanout_observation_t observation;
    memset(&observation, 0, sizeof observation);
    observation.reason = VM_FRAME_SCANOUT_REASON_UNKNOWN;
    observation.active_window = UINT32_MAX;
    if (!m) {
        observation.reason = VM_FRAME_SCANOUT_REASON_NO_MACHINE;
        return vm_guest_record_display(m, NULL, 0, &observation);
    }
    if (!m->ram) {
        observation.reason = VM_FRAME_SCANOUT_REASON_NO_RAM;
        return vm_guest_record_display(m, NULL, 0, &observation);
    }

#ifdef S5L8900_CLCD_BASE
    /*
     * Ask the display controller which window Apple's driver would adopt.
     * IOMobileFramebuffer tests windows 0, 1, 2, then 3 and uses the first
     * enabled one; hard-coding window 0 can display stale memory once the guest
     * switches scanout. Everything is validated before it is trusted. An
     * enabled but malformed window is an error, never a reason to silently
     * resurrect the demo framebuffer and hide the controller bug.
     */
    uint32_t fb_phys = 0, w = 0, h = 0, st = 0, fmt = 0;
    uint32_t active = s5l_clcd_active_window(&m->clcd);
    observation.scanning = m->clcd.scanning ? 1u : 0u;
    observation.ctrl = m->clcd.ctrl;
    observation.gate = m->clcd.gate;
    observation.active_window = active;

    if (!m->clcd.scanning) {
        observation.reason = VM_FRAME_SCANOUT_REASON_STOPPED;
    } else if ((m->clcd.ctrl & CLCD_CTRL_ENABLE) == 0u) {
        observation.reason = VM_FRAME_SCANOUT_REASON_GLOBAL_DISABLED;
    } else if ((m->clcd.gate & 1u) == 0u) {
        observation.reason = VM_FRAME_SCANOUT_REASON_CLOCK_GATED;
    } else if (active == CLCD_WIN_NONE) {
        observation.reason = VM_FRAME_SCANOUT_REASON_NO_ACTIVE_WINDOW;
    } else if (!s5l_clcd_window(&m->clcd, active,
                                &fb_phys, &w, &h, &st, &fmt, NULL)) {
        observation.reason = VM_FRAME_SCANOUT_REASON_WINDOW_UNAVAILABLE;
    } else {
        observation.framebuffer_phys = fb_phys;
        observation.width = w;
        observation.height = h;
        observation.stride = st;
        observation.format = fmt;
        if (!CLCD_FMT_IS_32BPP(fmt)) {
            observation.reason = VM_FRAME_SCANOUT_REASON_UNSUPPORTED_FORMAT;
        } else if (w != VM_FB_WIDTH || h != VM_FB_HEIGHT) {
            observation.reason = VM_FRAME_SCANOUT_REASON_UNSUPPORTED_GEOMETRY;
        } else if (st < w * VM_FB_BPP) {
            observation.reason = VM_FRAME_SCANOUT_REASON_STRIDE_TOO_SMALL;
        } else if (fb_phys < m->ram_base) {
            observation.reason = VM_FRAME_SCANOUT_REASON_FRAMEBUFFER_BELOW_RAM;
        } else if ((uint64_t)(fb_phys - m->ram_base) +
                       (uint64_t)st * h > m->ram_size) {
            observation.reason = VM_FRAME_SCANOUT_REASON_FRAMEBUFFER_OUTSIDE_RAM;
        } else if ((uint64_t)st * h > VM_FB_BYTES) {
            observation.reason = VM_FRAME_SCANOUT_REASON_PUBLICATION_TOO_LARGE;
        } else {
            observation.reason = VM_FRAME_SCANOUT_REASON_VALID;
            if (width)  *width  = w;
            if (height) *height = h;
            if (stride) *stride = st;
            /* AppleH1CLCD publishes the same 'ARGB' IOSurface for every value
             * of control bits[17:16], and the core deliberately has no evidence
             * for a hardware swizzle. Match s5l_clcd_scanout(): report the
             * observed little-endian AARRGGBB memory layout as BGRA. */
            if (order) *order = VM_ORDER_BGRA;
            return vm_guest_record_display(
                m, m->ram + (fb_phys - m->ram_base), (size_t)st * h,
                &observation);
        }
    }

    /* The core always has a CLCD model. No enabled, usable window means there
     * is no trustworthy scanout to publish; returning the fixed demo address
     * here would turn a guest display failure into a convincing stale frame. */
    return vm_guest_record_display(m, NULL, 0, &observation);
#endif

    /* Build-time fallback for a core configuration without the CLCD model. */
    uint32_t pa = vm_guest_fb_pa(m->ram_base, m->ram_size);
    if (!pa) {
        observation.reason = VM_FRAME_SCANOUT_REASON_FRAMEBUFFER_OUTSIDE_RAM;
        return vm_guest_record_display(m, NULL, 0, &observation);
    }
    observation.reason = VM_FRAME_SCANOUT_REASON_VALID;
    observation.framebuffer_phys = pa;
    observation.width = VM_FB_WIDTH;
    observation.height = VM_FB_HEIGHT;
    observation.stride = VM_FB_WIDTH * VM_FB_BPP;
    if (width)  *width  = VM_FB_WIDTH;
    if (height) *height = VM_FB_HEIGHT;
    if (stride) *stride = VM_FB_WIDTH * VM_FB_BPP;
    if (order)  *order  = VM_ORDER_BGRA;
    return vm_guest_record_display(
        m, m->ram + (pa - m->ram_base), VM_FB_BYTES, &observation);
}

bool vm_guest_install(s5l8900_t *m) {
    if (!m || !m->ram) return false;
    uint32_t fb_pa = vm_guest_fb_pa(m->ram_base, m->ram_size);
    if (!fb_pa) return false;

    uint32_t blob[VM_GUEST_BLOB_BYTES / 4];
    memset(blob, 0, sizeof blob);
    emitter_t e = { blob, 0 };

    static const char banner[] =
        "S5LBox: S5L8900 running on this device.\r\n"
        "ARMv6 interpreter -> system bus -> guest DRAM.\r\n"
        "Painting a 320x480 32bpp framebuffer.\r\n";
    static const char msg[] = "guest: frame ";

    const uint32_t str_pa  = m->ram_base + STR_OFF;
    const uint32_t msg_pa  = str_pa + (uint32_t)sizeof banner;   /* incl. NUL */

    /* --- setup ------------------------------------------------------- */
    emit_ldr_lit(&e, 7, LIT_WORD + 0);                    /* ldr r7,=UART0  */
    emit_ldr_lit(&e, 8, LIT_WORD + 1);                    /* ldr r8,=FB     */
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0,  9, 15, 0x50));   /* mov r9,#320    */
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0, 10, 15, 0x78));   /* mov r10,#480   */
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0,  4,  0, 0));      /* mov r4,#0      */
    emit_ldr_lit(&e, 6, LIT_WORD + 2);                    /* ldr r6,=banner */
    emit_puts(&e);

    /* --- one frame --------------------------------------------------- */
    unsigned l_frame = e.n;
    emit(&e, dp_reg(C_AL, OP_MOV, 0, 0, 0, 8, SH_LSL, 0));      /* mov r0,r8     */
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0, 3, 0, 0));              /* mov r3,#0     */

    unsigned l_y = e.n;
    emit(&e, dp_reg(C_AL, OP_ADD, 0, 3, 11, 4, SH_LSL, 0));     /* add r11,r3,r4 */
    emit(&e, dp_imm(C_AL, OP_AND, 0, 11, 11, 0, 0xffu));        /* and r11,#0xff */
    emit(&e, dp_reg(C_AL, OP_MOV, 0, 0, 11, 11, SH_LSL, 8));    /* lsl r11,#8    */
    emit(&e, dp_imm(C_AL, OP_ORR, 0, 11, 11, 4, 0xffu));        /* orr #ff000000 */
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0, 2, 0, 0));              /* mov r2,#0     */

    unsigned l_x = e.n;
    emit(&e, dp_reg(C_AL, OP_EOR, 0, 2, 5, 3, SH_LSL, 0));      /* eor r5,r2,r3  */
    emit(&e, dp_imm(C_AL, OP_AND, 0, 5, 5, 0, 0xffu));          /* and r5,#0xff  */
    emit(&e, dp_reg(C_AL, OP_ADD, 0, 2, 6, 4, SH_LSL, 0));      /* add r6,r2,r4  */
    emit(&e, dp_imm(C_AL, OP_AND, 0, 6, 6, 0, 0xffu));          /* and r6,#0xff  */
    emit(&e, dp_reg(C_AL, OP_ORR, 0, 5, 5, 6, SH_LSL, 16));     /* orr r5,r6<<16 */
    emit(&e, dp_reg(C_AL, OP_ORR, 0, 5, 5, 11, SH_LSL, 0));     /* orr r5,r11    */
    emit(&e, ldst(C_AL, false, false, false, true, false, 0, 5, 4)); /* str r5,[r0],#4 */
    emit(&e, dp_imm(C_AL, OP_ADD, 0, 2, 2, 0, 1));              /* add r2,#1     */
    emit(&e, dp_reg(C_AL, OP_CMP, 1, 2, 0, 9, SH_LSL, 0));      /* cmp r2,r9     */
    emit_b(&e, C_NE, l_x);

    emit(&e, dp_imm(C_AL, OP_ADD, 0, 3, 3, 0, 1));              /* add r3,#1     */
    emit(&e, dp_reg(C_AL, OP_CMP, 1, 3, 0, 10, SH_LSL, 0));     /* cmp r3,r10    */
    emit_b(&e, C_NE, l_y);

    /* --- end of frame: announce it on the UART ----------------------- */
    emit(&e, dp_imm(C_AL, OP_ADD, 0, 4, 4, 0, 1));              /* add r4,#1     */
    emit_ldr_lit(&e, 6, LIT_WORD + 3);                          /* ldr r6,=msg   */
    emit_puts(&e);
    emit_hex_digit(&e, 4);
    emit_hex_digit(&e, 0);
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0, 5, 0, '\n'));
    emit(&e, ldst(C_AL, false, false, true, true, false, 7, 5, UART_UTXH));
    emit_b(&e, C_AL, l_frame);

    /* The literal pool must not have been overwritten by the code. */
    if (e.n > LIT_WORD) return false;

    blob[LIT_WORD + 0] = S5L8900_UART0_BASE;
    blob[LIT_WORD + 1] = fb_pa;
    blob[LIT_WORD + 2] = str_pa;
    blob[LIT_WORD + 3] = msg_pa;

    /* Strings live past the pool, addressed absolutely through it. */
    if (STR_OFF + sizeof banner + sizeof msg > VM_GUEST_BLOB_BYTES) return false;
    memcpy((uint8_t *)blob + STR_OFF, banner, sizeof banner);
    memcpy((uint8_t *)blob + STR_OFF + sizeof banner, msg, sizeof msg);

    s5l8900_load(m, m->ram_base, blob, sizeof blob);

#ifdef S5L8900_CLCD_BASE
    /*
     * Stand in for iBoot: program display window 0 to point at the framebuffer
     * this payload paints, in the format it paints (32bpp, BGRA). The emulated
     * display controller then reports a real, guest-owned framebuffer that the
     * app scans out through vm_guest_display() — the same handoff a real iBoot
     * leaves for IOMobileFramebuffer to adopt. The interrupt mask is untouched,
     * so no frame IRQ can reach this vector-less payload.
     */
    if (!s5l_clcd_seed_window0(&m->clcd, fb_pa, VM_FB_WIDTH, VM_FB_HEIGHT,
                               VM_FB_WIDTH * VM_FB_BPP, CLCD_FMT_32BPP,
                               CLCD_ORDER_BGRA))
        return false;
#endif

    /* Start in a privileged mode with interrupts masked: this payload has no
     * vector table, so an interrupt would branch it into unwritten memory. */
    arm_reset(&m->cpu, &m->bus);
    m->cpu.r[15] = m->ram_base;
    return true;
}
