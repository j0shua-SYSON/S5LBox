# Persistent User RAM mapping experiment

This is an independent, **default-off** engine experiment. It does not claim a
reload-time improvement. Enable only with a nonempty `engine.compact-ram-map-on`
file in an isolated machine's work directory. It conflicts with the older
eight-window cache, raw-TLB-only refill and window-refill-off controls.

## Hypothesis

A busy-only physical reload profile on the preceding build counted 890.3 million
guest instructions, 67.2 million code-window fast refills and 39.6 million data
refill requests. Guest cursors were distributed across ordinary libraries;
there was no measured majority routine. Earlier raw-TLB native refill and
invocation-local window reuse did not establish a substantial reload-time win.

The new path retains successful plain-RAM grants across bounded native calls
and eviction from the raw TLB or small data caches. Its 4,096-entry table uses
64 KiB on AArch64 and contains host block pointers and complete
translation-generation/1-KiB-VA/access keys, never code. Lookup bypasses repeated
raw-TLB and RAM-range proof for that already-authorized mapping. Cold misses
still require the exact existing raw TLB proof; they never walk page tables or
touch a device from native text.

## Correctness boundaries

- User mode, MMU enabled, little endian only. READ, WRITE and FETCH grants are
  independent. WRITE additionally requires explicit direct-write consent.
- Entry and every callback continuation validate translation controls, pending
  interrupts/aborts and bus identity. Native User instructions cannot change
  these controls without returning through the guarded boundary.
- Architectural invalidation revokes grants. Software-cache eviction alone does
  not revoke a previously granted mapping, just as with existing DREAD entries.
  Map hits therefore are **not** reported as raw TLB hits.
- Generation wrap, discontinuous flush counters, owner/capability changes and
  failed rebinding clear the table. Machine reset, engine switching and snapshot
  application explicitly invalidate it, even if generation and pointers repeat.
- The full capability remains immutable for its owner-declared lifetime. A host
  replacing RAM must invalidate derived engine state before reusing an address.
- Instruction alignment, span, fault/refusal and exact-retirement checks are
  unchanged. Guest code is fetched live; stores to code are not hidden by this
  pointer cache. No snapshot fields or guest files are modified by the mode.

## Evidence and acceptance

`test_arm_ram_window` includes portable permission/lifetime checks and separate
real AArch64 tests. Native tests compare A32/Thumb execution with the interpreter,
including every RAM byte, warm hits after raw-TLB eviction, generation/access
revocation, callback mutation, live guest code stores and same-machine restore
with intentionally colliding generations. CI must actually emit
`NATIVE-PERSISTENT-RAM-MAP A32/Thumb fetch/read/write hits executed` on AArch64;
an x86 unavailable verdict is not native execution evidence.

Telemetry reports `compact_ram_map_fetch`, `compact_ram_map_read` and
`compact_ram_map_write` separately from raw-TLB refills. Host tests and fewer
refill operations do not establish speed. Acceptance requires an unprofiled,
same-binary physical ON/OFF comparison from identical paired disk/RAM/device
state, plus general guest workload checks. If wall time does not improve,
leave the marker absent and reject promotion. The requirement remains reloads
in seconds, not minutes; this experiment alone has not met it.
