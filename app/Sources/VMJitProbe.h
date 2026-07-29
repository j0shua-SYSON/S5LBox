/*
 * S5LBox — can this device execute code it wrote itself?
 *
 * WHY THIS EXISTS. docs/dynarec.md §10.3 projects a mature block JIT at
 * 0.15–0.45x of the guest's nominal 412 MHz, and every number in that section
 * is conditional on one unmeasured fact: whether a stock iPhone will branch
 * into a page this process just wrote. Nobody has tested it.
 *
 * A user's own run on a stock iOS 26.1 iPhone17,2 came back `RWX mmap: YES`,
 * which proves only that the MAPPING was granted. iOS can still enforce W^X at
 * fault time, so a successful mmap is not permission to execute. The answer
 * decides whether stage J3 is worth building at all or needs a different
 * strategy entirely, and it cannot be inferred from the mapping call.
 *
 * WHAT IT IS NOT. This is not a jailbreak, a codesign bypass, or an attempt to
 * defeat a policy. It writes a two-instruction function that returns a
 * constant, calls it, and reports what the operating system did. If the OS
 * refuses, the refusal IS the result and is recorded as such.
 *
 * SAFETY, and it is the whole design constraint. EmulatorViewController's
 * existing note is explicit: "Never execute probe code automatically during
 * viewDidLoad: a jailbreak policy mismatch would turn every app launch into the
 * same crash loop." So:
 *
 *   - Nothing here runs unless a caller asks for one specific strategy.
 *   - Each strategy installs handlers for the four signals a refused execution
 *     can raise and returns VM_JIT_RESULT_FAULTED instead of dying.
 *   - A signal handler cannot save a process the kernel decides to KILL, so the
 *     caller is expected to persist a breadcrumb BEFORE calling and clear it
 *     after. A breadcrumb still present at the next launch means that strategy
 *     took the process down, and it must not be offered again automatically.
 *     vm_jit_probe_run() cannot do that itself: it does not outlive the crash.
 */
#ifndef VM_JIT_PROBE_H
#define VM_JIT_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The three ways a process gets executable memory on Apple platforms. They are
 * separate results rather than a fallback chain, because "which one works"
 * is the actual question -- a device where only MAP_JIT works and one where
 * only plain RWX works need different code in jit_mem.c.
 */
typedef enum {
    VM_JIT_STRATEGY_RWX = 0,     /* mmap PROT_READ|PROT_WRITE|PROT_EXEC       */
    VM_JIT_STRATEGY_MAP_JIT,     /* ...|MAP_JIT + pthread_jit_write_protect_np */
    VM_JIT_STRATEGY_MPROTECT,    /* mmap RW, write, then mprotect to R|X      */
    VM_JIT_STRATEGY_COUNT
} vm_jit_strategy_t;

/*
 * Ordered from "the platform said no early" to "it worked". Every value except
 * OK is a real answer about this device, not a bug in the probe -- which is why
 * MAP_REFUSED and FAULTED are distinct: the first is a policy that denies the
 * memory, the second is a policy that grants it and then denies the branch.
 * That distinction is exactly what `RWX mmap: YES` could not express.
 */
typedef enum {
    VM_JIT_RESULT_UNTESTED = 0,
    VM_JIT_RESULT_UNSUPPORTED,      /* no native probe for this architecture  */
    VM_JIT_RESULT_MAP_REFUSED,      /* the mapping itself failed              */
    VM_JIT_RESULT_PROTECT_REFUSED,  /* mprotect / write-protect toggle failed */
    VM_JIT_RESULT_FAULTED,          /* branching into it raised a signal      */
    VM_JIT_RESULT_WRONG_VALUE,      /* it ran and returned the wrong constant */
    VM_JIT_RESULT_OK                /* it ran and returned the sentinel       */
} vm_jit_result_t;

/* The value the emitted function returns. Arbitrary, non-zero, and not a
 * plausible value for uninitialised memory or a truncated register. */
#define VM_JIT_PROBE_SENTINEL 0x5a5au

/* Stable, human-readable names. Never NULL, including for out-of-range input,
 * because these reach a log line the user is asked to read back. */
const char *vm_jit_strategy_name(vm_jit_strategy_t strategy);
const char *vm_jit_result_text(vm_jit_result_t result);

/* Whether this build knows how to emit a probe function for the host
 * architecture. False means every strategy returns VM_JIT_RESULT_UNSUPPORTED
 * without touching mmap, so a caller can hide the control entirely. */
bool vm_jit_probe_supported(void);

/*
 * Run ONE strategy. `observed` receives the value the emitted function
 * returned, when it returned at all, and is otherwise left alone -- so a caller
 * can tell WRONG_VALUE's payload from an untouched variable.
 *
 * Not thread-safe, and deliberately so: it installs process-wide signal
 * handlers for the duration. Call it from one place.
 */
vm_jit_result_t vm_jit_probe_run(vm_jit_strategy_t strategy,
                                 uint32_t *observed);

#ifdef __cplusplus
}
#endif

#endif /* VM_JIT_PROBE_H */
