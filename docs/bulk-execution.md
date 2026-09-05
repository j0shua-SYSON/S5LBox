# Witnessed bulk execution experiment

The compact engine can execute recognized string scans and signed-byte range
comparisons as bounded native operations. It matches complete live instruction
sequences, not symbols, process names, fixed addresses, or installed versions.
The purpose is to remove repeated instruction dispatch from CPU-heavy guest work.
This is an experiment, not a measured application-speed improvement.

A nonempty `engine.compact-bulk-on` file in a machine's work directory opts that
machine in. The default remains off. The control conflicts with the forced
interpreter and is reapplied after a powered-off checkpoint rebuild. It does not
modify the guest disk, guest code, checkpoint format, or stock-host policy.

## Execution contract

- User-mode ARM1176, A32, little-endian execution only.
- Complete FETCH-window byte match before execution; changed/truncated code
  follows ordinary instruction execution.
- Every load requires an existing current User DREAD RAM witness. Missing,
  stale, wrong-privilege, or device mappings cannot be replaced by a page walk
  or an assumed zero. The flat-memory alternative exists only in core tests.
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
