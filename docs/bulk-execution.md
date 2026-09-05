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
