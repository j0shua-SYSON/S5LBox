/*
 * S5LBox — high-level emulation of individual iPhone OS 3 userspace routines.
 *
 * WHAT THIS IS FOR. The guest software-composites every pixel, because this VM
 * un-matches the PowerVR MBX and QuartzCore therefore takes its software path.
 * That is work the real device never did on its CPU — the MBX exists precisely
 * to avoid it — and it is interpreted here one guest instruction at a time. No
 * achievable interpreter speed absorbs it; the only way past is to stop
 * executing it. Intercepting a leaf blit and doing the same work natively
 * turns millions of guest instructions per frame into microseconds of host
 * work.
 *
 * WHY IT NEEDS NO JIT, which is the point on a stock phone. The replacement is
 * ordinary compiled C in this binary's own signed __TEXT. The interception is
 * a comparison against the program counter in a loop that already runs. Nothing
 * is generated, nothing is written and then executed, no page is mapped
 * PROT_EXEC. There is nothing here for iOS to refuse.
 *
 * WHAT IT IS NOT. It is not a way around a bug in a device model. If something
 * is SLOW, it is a candidate; if something is BROKEN, the model gets fixed.
 * 2026-07-30 is the argument for that rule: had the Z2 bootload been
 * high-level-emulated, the digitizer would have "worked" hours earlier and the
 * register-write helper's 0x4AD1 acknowledgement would never have been found.
 * A shortcut there would have destroyed real knowledge about the hardware.
 *
 * ============================ THE CONTRACT ================================
 *
 * Every site must satisfy all of it. A site that cannot is not a candidate.
 *
 * 1. IDENTITY. The bytes at the target address are verified against a recorded
 *    prologue before the site is armed, and the site stays disarmed if they
 *    differ. These are absolute addresses in a shared cache belonging to one
 *    build of one OS; pointing them at anything else must fail closed, exactly
 *    as ios3_kernel_patch refuses a kernel whose hash it does not know.
 *
 * 2. ADDRESS SPACE. A shared-cache address exists in EVERY process. A site
 *    must fire only in the intended one, identified by TTBR0, or SpringBoard's
 *    blitter will be "helpfully" executed on behalf of mDNSResponder.
 *
 * 3. ABI. AAPCS: r0-r3 then the stack, results in r0/r1, r4-r11 and the CPSR
 *    flags preserved, return by loading PC from LR. Each site is audited
 *    individually and never by family: "it also takes a length" is not
 *    evidence that two functions share a signature.
 *
 * 4. MEMORY. Pixel buffers are user virtual addresses and are NOT guaranteed
 *    physically contiguous, so every access goes through the guest MMU, page
 *    by page, unprivileged. A host memcpy across a buffer that spans two
 *    non-adjacent frames corrupts whatever follows it.
 *
 * 5. FAULTS. A native replacement that silently succeeds where the guest would
 *    have taken a data abort changes behaviour AND destroys a diagnostic --
 *    run56 was exactly such an abort inside _CGSFillDRAM8by1, at FAR
 *    0x00621000. A handler that cannot reproduce the fault must decline the
 *    call and let the guest run its own code.
 *
 * 6. AN ORACLE. Pixel-exactness is not assumed, it is diffed: the same run
 *    with the site armed and disarmed must produce identical framebuffers.
 *    clcd.c states the doctrine -- "a wrong picture drawn confidently is worse
 *    than no picture."
 *
 * 7. DETERMINISM, and this one is a project-level cost rather than a bug.
 *    Replacing guest code changes the retired-instruction count, so every
 *    recorded instruction index in BOOTLOG.md and every snapshot stops being
 *    comparable across the boundary. Runs must say whether HLE was armed.
 *
 * ORDER OF WORK. Sites arrive as OBSERVE first -- counted and timed, guest
 * code still executed -- because that measures what the site is worth before
 * anything is replaced, and because an interception that returns wrongly is
 * far harder to diagnose than one that returns at all. Only a site whose
 * measured cost justifies it becomes REPLACE.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3_HLE_H
#define IOS3_HLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Guest memory as a handler may touch it: unprivileged, through the MMU, and
 * fallible. Both return false on a translation fault, which a handler must
 * treat as "decline and let the guest do it" rather than as a zero.
 */
typedef struct {
    void *ctx;
    bool (*read)(void *ctx, uint32_t va, void *dst, uint32_t len);
    bool (*write)(void *ctx, uint32_t va, const void *src, uint32_t len);
    /*
     * A PRIVILEGED read, for KERNEL-SIDE DESCRIPTORS ONLY. May be NULL.
     *
     * `read` and `write` translate unprivileged on purpose -- contract item 4
     * says a pixel is a user virtual address and must go through the guest's
     * own mapping, so a handler that touched pixels any other way could write
     * where the process cannot. That rule is not relaxed here and this must
     * never be used for pixels.
     *
     * It exists because r253 found something the unprivileged path cannot
     * reach at all: MBX2D's context holds its source and destination surfaces
     * as pointers into KERNEL space (0xc54bca00, 0xc54bc980), and every word
     * read back as unreadable because the compositing process genuinely cannot
     * dereference them. The pixel base a native blit needs lives inside those
     * objects. The hardware being modelled reads them too -- an IOSurface is
     * shared with the GPU -- so reading them is emulating the device rather
     * than granting the guest a new power: nothing the guest can observe
     * changes, and no guest-visible mapping is bypassed for anything the guest
     * itself would write.
     */
    bool (*read_priv)(void *ctx, uint32_t va, void *dst, uint32_t len);
} ios3_hle_mem_t;

typedef enum {
    /* Count and measure; the guest still runs its own code. */
    IOS3_HLE_OBSERVE = 0,
    /* Do the work natively and return to LR without executing the body. */
    IOS3_HLE_REPLACE,
    /*
     * Run the handler for its SIDE EFFECTS ONLY and then let the guest execute
     * anyway. The handler's return value is discarded and the site never
     * reports "handled", so the guest's behaviour is bit-for-bit what it would
     * be with the site absent.
     *
     * This exists because OBSERVE cannot answer the question that actually
     * gates a replacement. A hit count says a site is on the path; it does not
     * say what the arguments MEAN, and the order-of-work rule requires "an
     * argument shape taken at the site itself" before anything is replaced.
     * OBSERVE never calls the handler, so there was nowhere to read r0-r3 and
     * the incoming stack words from.
     *
     * Appended rather than inserted, so IOS3_HLE_REPLACE keeps its value and no
     * existing comparison changes meaning.
     */
    IOS3_HLE_TRACE
} ios3_hle_mode_t;

/*
 * Returns true only if it did the whole job AND set the CPU up to return.
 * False means "not handled" and the guest's own code runs -- the correct
 * answer for an argument shape not covered, or a guest pointer that would
 * fault. Declining is always safe; guessing is not.
 */
typedef bool (*ios3_hle_fn_t)(arm_cpu_t *cpu, const ios3_hle_mem_t *mem);

typedef struct {
    const char   *name;
    uint32_t      va;          /* absolute shared-cache address, thumb bit 0 */
    const uint32_t *prologue;  /* words that must be present to arm          */
    unsigned      prologue_n;
    ios3_hle_fn_t handler;     /* NULL for a pure observation site           */
    ios3_hle_mode_t mode;
    /* Filled in by the library. */
    bool          armed;
    bool          identity_failed;
    uint64_t      hits;        /* times the site was reached                 */
    uint64_t      handled;     /* times the handler took the call            */
    uint64_t      declined;    /* reached, handler said no -- the guest ran  */
    uint64_t      wrong_space; /* reached in the wrong address space         */
} ios3_hle_site_t;

/* How many sites this build knows about, and read-only access to them. */
unsigned          ios3_hle_site_count(void);
ios3_hle_site_t  *ios3_hle_site_at(unsigned i);

/*
 * Verify identities and arm. `ttbr0` is the address space sites are restricted
 * to; 0 disables that check and must only be used by tests. Returns the number
 * armed, and leaves identity_failed set on any site whose bytes did not match.
 */
unsigned ios3_hle_arm(const ios3_hle_mem_t *mem, uint32_t ttbr0);

/* Disarm everything, keeping the counters for the report. */
void ios3_hle_disarm(void);

/*
 * The hot path. Returns true only if a handler took the call, in which case
 * the CPU's PC has already been set and the caller must NOT execute this
 * instruction. Cheap when nothing is armed.
 */
bool ios3_hle_step(arm_cpu_t *cpu, const ios3_hle_mem_t *mem, uint32_t pc,
                   uint32_t ttbr0);

#ifdef __cplusplus
}
#endif

#endif /* IOS3_HLE_H */
