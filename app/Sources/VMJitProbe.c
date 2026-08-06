/*
 * S5LBox — the JIT executability probe. See VMJitProbe.h for why it exists and
 * what it deliberately does not do.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
/* sigaction, sigsetjmp and mmap are POSIX, and the targets build -std=c11
 * without extensions, so they are not declared unless this is asked for
 * first. Must precede every include. */
#if defined(__unix__) || defined(__APPLE__)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#    define _DARWIN_C_SOURCE 1   /* MAP_ANON, MAP_JIT */
#  endif
#endif

#include "VMJitProbe.h"

#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#  define VM_JIT_POSIX 1
#  include <setjmp.h>
#  include <signal.h>
#  include <sys/mman.h>
#  include <unistd.h>
/* BSD and Darwin spell it MAP_ANON; Linux spells it MAP_ANONYMOUS and only
 * defines the short form under _BSD_SOURCE. Accept whichever exists. */
#  if !defined(MAP_ANON) && defined(MAP_ANONYMOUS)
#    define MAP_ANON MAP_ANONYMOUS
#  endif
#else
#  define VM_JIT_POSIX 0
#endif

#if defined(__APPLE__)
#  include <libkern/OSCacheControl.h>
#  include <pthread.h>
#  include <TargetConditionals.h>
#endif

/*
 * MAP_JIT's write-protect toggle is macOS-only, and finding that out is itself
 * a result worth keeping.
 *
 * `pthread_jit_write_protect_np` is declared unavailable on iOS -- the arm64
 * iphoneos build fails outright on the call, which is how this was learned. So
 * the two Apple platforms need different shapes for the same strategy:
 *
 *   macOS  - map MAP_JIT, toggle write protection off, write, toggle it back.
 *   iOS    - there is no toggle. A MAP_JIT mapping is either writable AND
 *            executable for this process or the mapping is refused, and which
 *            of those happens is exactly what the probe is here to find out.
 *
 * So on iOS this strategy differs from plain RWX only by the MAP_JIT flag,
 * which is the honest test: it asks whether the flag changes the verdict.
 */
#if defined(__APPLE__) && defined(MAP_JIT)
#  if defined(TARGET_OS_OSX) && TARGET_OS_OSX
#    define VM_JIT_HAVE_WP_TOGGLE 1
#  else
#    define VM_JIT_HAVE_WP_TOGGLE 0
#  endif
#  define VM_JIT_HAVE_MAP_JIT 1
#else
#  define VM_JIT_HAVE_WP_TOGGLE 0
#  define VM_JIT_HAVE_MAP_JIT 0
#endif

/* --------------------------------------------------------------- the payload --- */

/*
 * A function that returns VM_JIT_PROBE_SENTINEL and nothing else. Emitted for
 * the HOST architecture, not the guest's -- this asks what the operating system
 * permits, so it has to be code the host CPU would really run. Writing ARMv6
 * here would test nothing on either machine.
 *
 * Both encodings are spelled out rather than assembled, so this file has no
 * toolchain dependency and the bytes are reviewable.
 */
#if defined(__aarch64__) || defined(_M_ARM64) || \
    defined(__x86_64__)  || defined(_M_X64)
#  define VM_JIT_HAVE_PAYLOAD 1
#else
#  define VM_JIT_HAVE_PAYLOAD 0
#endif

/* Defined only where it is actually used. A platform without mmap would
 * otherwise carry an unused constant, and these targets build -Werror. */
#if VM_JIT_POSIX && VM_JIT_HAVE_PAYLOAD
#  if defined(__aarch64__) || defined(_M_ARM64)
static const uint32_t PROBE_CODE[] = {
    /* movz x0, #0x5a5a  ->  0xd2800000 | (imm16 << 5) | Rd */
    0xd28b4b40u,
    /* ret (br x30) */
    0xd65f03c0u,
};
#  else
static const uint8_t PROBE_CODE[] = {
    0xb8, 0x5a, 0x5a, 0x00, 0x00,   /* mov eax, 0x00005a5a */
    0xc3,                            /* ret                 */
};
#  endif

typedef uint32_t (*probe_fn_t)(void);
#endif

/* ---------------------------------------------------------------- the names --- */

const char *vm_jit_strategy_name(vm_jit_strategy_t strategy) {
    switch (strategy) {
        case VM_JIT_STRATEGY_RWX:      return "RWX mmap";
        case VM_JIT_STRATEGY_MAP_JIT:  return "MAP_JIT";
        case VM_JIT_STRATEGY_MPROTECT: return "mprotect RW->RX";
        default:                       return "unknown strategy";
    }
}

const char *vm_jit_result_text(vm_jit_result_t result) {
    switch (result) {
        case VM_JIT_RESULT_UNTESTED:  return "not tested";
        case VM_JIT_RESULT_UNSUPPORTED:
            return "no probe for this architecture";
        case VM_JIT_RESULT_MAP_REFUSED:
            return "REFUSED: the mapping was denied";
        case VM_JIT_RESULT_PROTECT_REFUSED:
            return "REFUSED: the page could not be made executable";
        case VM_JIT_RESULT_FAULTED:
            return "REFUSED: mapped, but branching into it faulted";
        case VM_JIT_RESULT_WRONG_VALUE:
            return "RAN, but returned the wrong value -- do not trust it";
        case VM_JIT_RESULT_OK:
            return "OK: wrote and executed its own code";
        default: return "unknown result";
    }
}

bool vm_jit_probe_supported(void) {
#if VM_JIT_POSIX && VM_JIT_HAVE_PAYLOAD
    return true;
#else
    return false;
#endif
}

/* ------------------------------------------------------------- the mechanism --- */

#if VM_JIT_POSIX && VM_JIT_HAVE_PAYLOAD

/*
 * Catching the refusal instead of dying from it.
 *
 * A denied branch surfaces as one of these four, depending on whether the fault
 * is a permission failure, an alignment/bus condition, a decoded-as-garbage
 * instruction, or a guard trap. All four are handled the same way: unwind to
 * the setjmp and report FAULTED.
 *
 * This CANNOT save the process from a kernel that chooses to kill it outright
 * for a code-signing violation -- no handler runs for SIGKILL. That case is why
 * the header requires the caller to persist a breadcrumb first.
 */
static const int PROBE_SIGNALS[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP };
#define PROBE_NSIGNALS ((int)(sizeof PROBE_SIGNALS / sizeof PROBE_SIGNALS[0]))

static sigjmp_buf         g_probe_jmp;
static volatile sig_atomic_t g_probe_armed;

static void probe_handler(int sig) {
    if (g_probe_armed) {
        g_probe_armed = 0;
        siglongjmp(g_probe_jmp, 1);
    }
    /* Not ours: restore the default disposition and re-raise, so a genuine
     * fault elsewhere still produces a genuine crash report rather than being
     * silently swallowed by a handler that had no business seeing it. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static bool install_handlers(struct sigaction *saved) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_NODEFER so a fault raised from inside the handler's own signal is not
     * blocked; siglongjmp leaves the handler without the normal return path. */
    sa.sa_flags = SA_NODEFER;
    for (int i = 0; i < PROBE_NSIGNALS; i++) {
        if (sigaction(PROBE_SIGNALS[i], &sa, &saved[i]) != 0) {
            /* Roll back whatever was installed, so a partial failure does not
             * leave this file's handler owning a signal it will not remove. */
            for (int j = 0; j < i; j++)
                (void)sigaction(PROBE_SIGNALS[j], &saved[j], NULL);
            return false;
        }
    }
    return true;
}

static void restore_handlers(struct sigaction *saved) {
    for (int i = 0; i < PROBE_NSIGNALS; i++)
        (void)sigaction(PROBE_SIGNALS[i], &saved[i], NULL);
}

/*
 * Make written bytes fetchable as instructions.
 *
 * Skipping this is the classic way to get a misleading FAULTED: the data cache
 * holds the payload, the instruction cache does not, and the core executes
 * whatever was there before. The refusal would then be the probe's own bug
 * reported as the platform's policy.
 */
static void sync_icache(void *p, size_t n) {
#if defined(__APPLE__)
    sys_icache_invalidate(p, n);
#elif defined(__GNUC__)
    __builtin___clear_cache((char *)p, (char *)p + n);
#else
    (void)p; (void)n;
#endif
}

vm_jit_result_t vm_jit_probe_run(vm_jit_strategy_t strategy,
                                 uint32_t *observed) {
    const size_t   code_len = sizeof PROBE_CODE;
    const size_t   len      = 4096u;
    /*
     * volatile, and not as a formality.
     *
     * A local that is modified between sigsetjmp() and the siglongjmp() has an
     * INDETERMINATE value afterwards unless it is volatile -- the compiler is
     * entitled to keep it in a call-saved register the unwind restores. `page`
     * is read after the block to unmap it, so on the fault path -- the exact
     * path a device that refuses execution takes -- the munmap() could be
     * handed a garbage address. `result` is assigned in both branches and read
     * after, and has the same problem.
     *
     * GCC caught this as -Werror=clobbered. It is a real defect, not a
     * diagnostic to appease, and it would only ever have misfired on the
     * devices this probe exists to identify.
     */
    void * volatile page    = MAP_FAILED;
    volatile vm_jit_result_t result = VM_JIT_RESULT_UNTESTED;
    int            prot     = PROT_READ | PROT_WRITE;
    int            flags    = MAP_PRIVATE | MAP_ANON;
#if VM_JIT_HAVE_WP_TOGGLE
    /* Declared only where it is read. On Linux the MAP_JIT blocks compile out
     * entirely and an always-false flag is an -Werror=unused-variable. */
    bool           jit_wp   = false;
#endif

    if (strategy < 0 || strategy >= VM_JIT_STRATEGY_COUNT)
        return VM_JIT_RESULT_UNSUPPORTED;

    switch (strategy) {
        case VM_JIT_STRATEGY_RWX:
            prot |= PROT_EXEC;
            break;
        case VM_JIT_STRATEGY_MAP_JIT:
#if VM_JIT_HAVE_MAP_JIT
            prot  |= PROT_EXEC;
            flags |= MAP_JIT;
#  if VM_JIT_HAVE_WP_TOGGLE
            jit_wp = true;
#  endif
#else
            return VM_JIT_RESULT_UNSUPPORTED;
#endif
            break;
        case VM_JIT_STRATEGY_MPROTECT:
            /* Deliberately W^X: never writable and executable at once. This is
             * the strategy a hardened platform is most likely to allow, so it
             * is worth knowing about separately even if RWX also works. */
            break;
        default:
            return VM_JIT_RESULT_UNSUPPORTED;
    }

    page = mmap(NULL, len, prot, flags, -1, 0);
    if (page == MAP_FAILED)
        return VM_JIT_RESULT_MAP_REFUSED;

#if VM_JIT_HAVE_WP_TOGGLE
    /* macOS only. MAP_JIT pages start execute-only for this thread; the toggle
     * is what makes them writable. Without platform JIT authorization the call
     * is documented to abort rather than fail, which is precisely why this
     * strategy is opt-in and breadcrumbed like the others. The stock iOS app
     * requests no such authorization and never calls this macOS-only branch. */
    if (jit_wp) pthread_jit_write_protect_np(0);
#endif

    memcpy(page, PROBE_CODE, code_len);

#if VM_JIT_HAVE_WP_TOGGLE
    if (jit_wp) pthread_jit_write_protect_np(1);
#endif

    if (strategy == VM_JIT_STRATEGY_MPROTECT) {
        if (mprotect(page, len, PROT_READ | PROT_EXEC) != 0) {
            munmap(page, len);
            return VM_JIT_RESULT_PROTECT_REFUSED;
        }
    }

    sync_icache(page, code_len);

    {
        struct sigaction saved[PROBE_NSIGNALS];
        if (!install_handlers(saved)) {
            munmap(page, len);
            return VM_JIT_RESULT_PROTECT_REFUSED;
        }

        g_probe_armed = 1;
        if (sigsetjmp(g_probe_jmp, 1) == 0) {
            /*
             * ISO C forbids casting an object pointer to a function pointer,
             * and these targets build -pedantic -Werror. POSIX requires the
             * conversion to work anyway -- it is what dlsym()'s return value
             * is for -- so it goes through a union, which is the documented
             * way to say "I mean this" without the diagnostic.
             */
            union { void *obj; probe_fn_t fn; } launder;
            launder.obj = page;
            uint32_t got = launder.fn();
            g_probe_armed = 0;
            if (observed) *observed = got;
            result = (got == VM_JIT_PROBE_SENTINEL) ? VM_JIT_RESULT_OK
                                                    : VM_JIT_RESULT_WRONG_VALUE;
        } else {
            /* siglongjmp landed here from probe_handler. */
            result = VM_JIT_RESULT_FAULTED;
        }
        g_probe_armed = 0;
        restore_handlers(saved);
    }

    munmap(page, len);
    return result;
}

#else  /* no POSIX mmap, or no payload for this architecture */

vm_jit_result_t vm_jit_probe_run(vm_jit_strategy_t strategy,
                                 uint32_t *observed) {
    (void)strategy; (void)observed;
    return VM_JIT_RESULT_UNSUPPORTED;
}

#endif
