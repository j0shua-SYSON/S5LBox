# Witnessed bulk execution experiment

The compact engine can execute recognized string scans, signed-byte range
comparisons and read-only pointer searches as bounded native operations.
It matches complete live instruction
sequences, not symbols, process names, fixed addresses, or installed versions.
The purpose is to remove repeated instruction dispatch from CPU-heavy guest work.
This is an experiment, not a measured application-speed improvement.

A nonempty `engine.compact-bulk-on` file in a machine's work directory opts that
machine in. The default remains off. The control conflicts with the forced
interpreter and is reapplied after a powered-off checkpoint rebuild. It does not
modify the guest disk, guest code, checkpoint format, or stock-host policy.

## Execution contract

- User-mode ARM1176, A32/Thumb, little-endian execution only.
- Complete FETCH-window byte match before execution; changed/truncated code
  follows ordinary instruction execution.
- Loads require current User READ permission and a plain-RAM grant. The Thumb
  pointer loops may resolve a cold page through the shared MMU decoder when
  both descriptor levels and the complete data block lie inside the captured
  RAM range. Existing cached mappings/faults take precedence. No speculative
  bus read, cache publication, fault or assumed zero is permitted. The flat
  memory alternative exists only in core tests.
- No stores, guest library patching, executable allocations, or generated
  runtime code. All native text is linked before signing.
- Complete architectural register and flag results, not merely ABI return values.
- Exact original retirement counts, including failed conditions. Long loops
  return an exact prefix at a loop header. The existing device/interrupt budget
  is never enlarged; pending unmasked interrupts and aborts reject execution.
- The native caller owns cycle accounting and restores its internal floating-
  point session before calling C. A refused candidate preserves decode operands.

`compact_bulk_calls` counts successful operations/prefixes. `compact_bulk_retired`
counts the original instructions they represent and is a subset of native
retirements, not additional guest time. Neither counter is snapshot state.
`compact_bulk_reuse_current` reports accepted search summaries in the current
cache epoch. It is an absolute gauge, reset on cache invalidation/disable,
not a lifetime delta or an application-speed measurement.

## Validation and rollout gate

`test_arm_bulk` compares complete state against `arm_step`, covering byte values,
alignment, NZCV, interworking, exact budgets, long resumptions, MMU permissions,
non-contiguous mappings, changed code and refusal without side effects. With a
signed A64 engine it also exercises the real generated runner with the switch
both off and on. macOS CI requires actual bulk calls; an execution skip fails.

Keep the control off until same-build physical runs from matched guest
checkpoints demonstrate a substantial wall-clock improvement for real work,
with identical resulting package/catalog contents and no lifecycle regression.
Host correctness tests and successful invocation counters alone are insufficient.

The cold-page experiment removes the earlier requirement that every traversed
page already be resident in the small DREAD/TLB caches. Complete read-only
iterations can now span cold but valid RAM pages without leaving the executor.
The shared permission decoder retains section/page, AP/APX, domain, access-flag
and translation-control semantics. Existing counters classify represented cold
reads as TLB misses, not DREAD/TLB hits, including invariant reads whose proofs
are reused within one read-only interval. Refused iterations commit no counters.
This remains behind the existing bulk option; physical speedup is unverified.

The filtered pointer search also admits its different-depth path and the
empty-payload detour when both child and sibling are nonzero. Unlike untaken
exit branches, every taken internal edge must lead to separately matched live
instructions within the FETCH witness. Depth-only traversal does not read the
unused stack target. Mixed iterations retain their individual 15, 19, 22 or 26
instruction costs and literal logical-load counts. Descent, unlinking, stores,
returns, unproved mappings and incomplete budgets remain ordinary execution.
The differential tests cover mixed paths, all NZCV inputs, cold nonidentity
pages, changed/truncated branch targets and precise refusal after a prefix.
Native integration requires the entire chosen budget in one batch on both
warm and cold pages. This expands execution coverage, not the physical gate.

The same opt-in path retains up to 256 summaries of 2-32 read-only search
iterations, with at most eight 1 KiB RAM pages per summary (about 2 MiB total).
Reuse rechecks the live instruction shape, query bounds, current User READ
mapping of every page, and exact loaded values with byte comparisons. It does
not rely on hashes, page generations, write callbacks or saved host pointers.
This covers direct CPU/graphics/bridge writes and changed mappings without
instrumenting stores. Equal pages pass a single comparison; changed pages are
checked against the exact loaded-word mask so unrelated stores do not discard
useful work. Over-capacity spans and incomplete individual iterations stay
literal. A machine-budget boundary can publish a shorter complete prefix;
loads from a refused next iteration are never included in that prefix.
Original instruction/load counts and final registers/flags remain exact;
the existing device budget is unchanged. Summaries are derived host data,
reset with the engine and freed on disable/disposal, never checkpoint bytes.
The regression requires actual reuse, then changes inputs, data, mappings,
permissions and code and compares against ordinary instruction execution.
No application wall-time improvement has yet been established for this path.

The original fixed-32 summary test used larger standalone budgets and missed
the normal active-clock machine cap of 256 instructions. No 32-iteration search
fit that interval (the shortest requires 320 instructions). The regression now
derives its short budgets from `S5L8900_ACTIVE_CLOCK_BATCH_INSNS` and requires
actual reuse in all seven path shapes, including native warm/cold execution.
Summary length follows available complete work; device timing is not enlarged.

## First physical result

Build `cd6f350` passed all eight core CI jobs (including native ARM64 execution
on both Mac runners) and the iOS build. Same-build, profiler-off real catalog
refreshes from the same paired ready checkpoint did not demonstrate a
substantial improvement. With bulk execution off, completion was observed in
`(178.049, 239.996]` seconds; with it on, `(213.111, 255.215]` seconds. These are
observation bounds, not exact timings. They overlap and initial idle histories
differed, so they do not establish a precise percentage or definite regression.

The enabled run recorded 76,241,464 successful bulk operations representing
887,929,969 original instructions. High invocation counts did not translate
into a usability win. About 1.326 MB of host input had arrived long before the
catalog UI became ready in both runs. The experiment remains off.

The subsequent ordered-list investigation produced the offline APT candidate
described in `guest-apt-index.md`, but it remains uninstalled. The current highest
priority is substantial emulator-wide acceleration across demanding guest work;
application-specific changes are a fallback or supplement, not the main solution.
Removing repeated instruction dispatch or reducing counters is insufficient
without a substantial same-checkpoint physical wall-time improvement.

The existing long-refresh native profile also had an attribution limitation:
outside-runner PCs stopped being captured after 4096 samples, although the broad
region counters kept increasing. The opt-in sampler now counts a bounded streaming
histogram of 4096 distinct 256-byte PC regions throughout the run. Known regions
keep accepting samples when full; unknown-region overflow is explicitly dropped.
This fixes diagnostic coverage, not guest performance. The portable regression
test includes a late hot phase beyond the former cutoff, full-table collisions,
zero-address regions, saturation, and reset. Fresh physical profiles are required
before attributing the old profile's full outside-runner share to specific code.

The app's PC-profile marker now uses sampling-only mode. Previously it also
classified and counted every interpreter fallback on the sampled thread; the
old long-run trace recorded 545 million such events. Those extra operations can
distort CPU attribution. Exact fallback tracing remains an explicit core API
option for instruction-coverage diagnostics, separate from the sampling-only
mode used to select performance work. Neither diagnostic is a timing oracle;
all final performance comparisons must still have profiling disabled.
