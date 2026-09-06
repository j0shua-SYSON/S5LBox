# Statistical guest cursor attribution

The explicit compact-PC profiling mode now samples the guest cursor along with
the host PC. Both come from the same target-thread register snapshot. Generated
text labels delimit the interval after W26 receives the guest PC and before
the epilogue restores the caller's X26. The labels add no instructions.
Outside callbacks, prologue/epilogue observations, malformed register values,
short thread-state results and profile-generation races are not accepted as
guest cursor observations. No running-thread CPU, context or code pointer is
dereferenced.

The existing two-millisecond sampler and before/after runnable-state checks
remain. A second bounded histogram counts 256-byte guest virtual-address bins
throughout the run; the top 16 are exposed in telemetry. Captured, distinct-bin
overflow and unavailable observations partition accepted host-PC samples.
Counters saturate rather than wrap. Re-enablement resets both histograms, and a
generation guard rejects an in-flight sample from the previous run.

These are CPU-time observations of a live cursor, not instruction frequencies.
The cursor can already point at the next instruction during retirement or be
partway through branch-target calculation. Addresses are not process-qualified:
resolve them against retained guest images and the workload, and do not combine
overlapping process mappings as if they named one known function. A top-16 list
does not contain the entire histogram; ranks can change between snapshots.
Do not subtract different ranks or silently treat an absent bin as zero.

Ordinary machines do not enable the sampler. The extension adds no per-guest-
instruction tracing, memory reads or decode branches, and changes no guest
clocks, budgets, registers, memory or snapshots. Profiling remains diagnostic,
not a claimed execution-speed improvement. Application timings must be repeated
with profiling disabled.

Portable tests cover interval edges, malformed bounds, high register bits,
alignment, the valid zero cursor, refusal output preservation, sustained
histogram counting, collisions, overflow and saturation. Native Mac tests must
capture real nonzero A32 and Thumb loop samples from the expected guest regions,
while preserving registers, flags and exact cycle accounting. CI requires the
native marker and cannot accept a skipped or all-zero run.
