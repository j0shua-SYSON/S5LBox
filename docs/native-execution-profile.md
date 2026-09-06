# Complete native execution profile

The opt-in compact-PC sampler already distinguishes ordered runner regions
and records the hottest outside-runner addresses. That summary is not a full
host profile: its top eight addresses omit most outside observations, and a
large region can contain several different costs.

The same sampler now also counts every accepted host PC in a bounded
256-byte-region histogram. No second sampler, signal handler, per-instruction
counter, executable-memory write or guest-state change is introduced.
Marker-free runs never initialize or update this diagnostic storage.

At a pause or stop boundary the application writes an independent, uniquely
named `engine.native-pc-<UUID>.csv` in the machine directory. It does not
overwrite previous evidence. The existing guest-PC export remains separate.
Symbol lookup and export happen after timing guest execution has stopped.

The file format is:

```text
s5lbox-native-pc-v1
reference_pc,<hex address of the linked compact entry>
bin,representative_pc,samples,image_base,symbol_address,image,symbol
<hex bin>,<hex PC>,<decimal count>,<hex image base>,<hex symbol address>,"image","symbol"
complete,<captured>,<dropped>
```

Image and symbol names use CSV quoting. Empty names and zero addresses mean
the dynamic loader could not resolve that representative address. Retain the
exact application binary and build identity; the reference PC and image base
allow matching its linked addresses despite ASLR. Do not resolve these host
addresses against guest firmware.

The owner-thread visitor copies the histogram under the sampler lock, releases
that lock before invoking callbacks, and zeros result totals on any failure.
The application refuses to publish a complete export unless captured plus
dropped equals the paused host-sample count. A callback refusal must never
produce a misleading completion footer. Unsupported hosts and machines that
did not explicitly enable sampling cannot expose the histogram.

For analysis, verify the schema, unique bins, PC membership in each bin,
positive row counts and exact sum of row counts against the completion footer.
Report dropped samples rather than treating them as measured zero cost. Use
two complete exports from the same run for interval attribution; subtract all
bins, not only two changing top-eight lists. Reject incompatible image bases
or reference addresses when comparing cumulative snapshots.

These are statistical observations of a running host thread, not retired
guest-instruction frequencies, exact function shares, CPU accounting or FPS.
A 256-byte bin can cross a symbol boundary. Its representative PC and nearest
loader symbol do not establish that every observation in that bin belongs to
that function. Sampling perturbs execution; use marker-free matched runs to
accept or reject performance changes.

Validation covers disabled/null exports on all hosts, shared histogram
overflow and stable-copy traversal, and actual Apple AArch64 A32/Thumb loops.
The native tests require nonzero observations, exact host sample totals,
callback reentry without holding the sampler lock, refusal handling, and
unchanged guest registers, flags and retirement counts. Passing tests alone
does not establish a real-device performance improvement.
