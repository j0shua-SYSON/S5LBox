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

## Complete histogram export

With that same explicit profiling marker enabled, the app saves a complete
guest histogram once at each pause edge and on stopping. A stable heap copy is
taken under the sampler lock; callbacks run after unlocking. There is no disk
I/O in the sampler or the guest execution path. Ordinary machines export nothing.

Each unique `engine.guest-pc-<uuid>.csv` stays in that machine's directory and
refuses to overwrite an existing file. It begins with `s5lbox-guest-pc-v1`, then
`bin,representative_pc,samples`, every occupied bin, and a final
`complete,<captured>,<dropped>` row. A missing footer means an incomplete export;
the sum of row counts must equal captured unless counters saturated. A stopped
or paused guest is not executing new work during export. Raw host-PC sampling
can still have unavailable polls; those remain in ordinary telemetry.

Compare complete exports by bin, not row order. Zero-bin samples are valid.
The bin base is not an exact function entry, the representative PC is not every
PC that was sampled, and neither qualifies the virtual address by process.
Pausing for exports changes the experimental protocol: these runs are for
attribution, not substitutes for marker-free uninterrupted wall-time validation.
Portable tests visit every bin of a full table, exercise refusal and zero bins;
native tests check complete nonzero A32/Thumb exports and callback reentrancy.
