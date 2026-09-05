# Native second-level RAM refill experiment

This is an emulator-wide, off-by-default experiment, not a change to APT or
any guest executable. It targets repeated exits from the signed compact
runner when a FETCH window or DREAD/DWRITE entry misses but the existing
4096-entry software TLB already has the exact successful translation.

## Contract

`arm_ram_window_capture` obtains an explicit whole-range plain-RAM pointer
from the bus. Read and write grants are separate. Write callbacks, pointer
callbacks, bus identity and bus context are part of the capability. A RAM
address alone is never authority to bypass a write observer.

The native path is User-only and MMU-on. It requires an exact access-kind,
privilege, 1 KiB VA tag and live generation, a zero fault result, and a complete
physical block inside the granted RAM range. It neither walks page tables nor
touches MMIO, raises faults, generates code, or changes retirement budgets.
Unsupported instructions and all refusals retain the architectural fallback.

The C entry proves the translation stamp. Every interpreter continuation is
guarded again against changes to CP15, the translation stamp, generation,
bus/observer contract, mode, endianness and pending interrupts/aborts. A changed
context ends the interval while preserving the callback's exact retirement.
No admitted native User instruction can change translation control or install
a host observer between those mutation boundaries.

Successful FETCH refills publish the runner window, CPU FETCH witness and
engine callback bookkeeping together. Data refills fill the existing cache
and finish the already-decoded access. Live host bytes preserve code-store
coherence. Each successful refill counts one TLB hit; the data instruction
counts its normal DREAD/DWRITE hit once.

Capabilities and counters are derived host state, never snapshot data.
The frontend reacquires the capability after restore/reset.

## Control and evidence

A nonempty `engine.compact-tlb-refill-on` file in a machine's working directory
opts that machine in. It conflicts with interpreter-only, window-refill-off
and the older eight-window-cache experiment. Other machines remain unchanged.
`compact_tlb_fetch`, `compact_tlb_read` and `compact_tlb_write` expose actual
native refills, independently of PC sampling.

`test_arm_ram_window` covers full-range permission, access/privilege tags,
cached faults, physical bounds, stale generations, translation stamps, and
observer revocation without memory access. On real AArch64 it additionally
compares ARM/Thumb loops and memory families against the interpreter, checks
exact retirement budgets and mutation-boundary stops. CI explicitly rejects
skipped native execution on the Apple runners.

No physical performance gain is established by the implementation or by these
tests. Promotion requires matched, profiler-off phone runs on general guest
workloads as well as Cydia, restoring the same paired disk/checkpoint baseline
and verifying nonzero native-refill counters. Previous failed window-cache
and bulk-loop experiments remain disabled.
