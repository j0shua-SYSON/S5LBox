# Ordered APT catalog index experiment

Status: the exact v3 guest-library candidate has been installed in the test
guest through a private reversible package. The host app does not contain it.
Real refreshes complete, but still take minutes; the seconds-level usability
target is not met. Catalog equivalence and package-removal rollback are still
acceptance gates, so this is not a default guest-library replacement.

The exact pinned `apt7-lib` 0.7.20.2-1 library implements `WriteUniqString` with
a 26-entry recent-value cache followed by a descending sorted linked-list
search. Every visited node calls `strlen` and a signed-byte range comparator.
Its SHA256 is
`239202453005e090d9176e7d017203adeaa54cd82e0a7a17edd39ef6df9d6075`.
The method starts at image address `0x65f50`. The publisher's extra display-name
fields matter: unlike the unmodified Debian source, the actual `NewVersion`
binary interns `Name`, falls back to `Maemo-Display-Name`, and then interns
`Section` and `Architecture`. This puts thousands of distinct names into the
same linked list, not merely a few architecture and section labels.

`tools/guest_apt_index.c` replaces repeated list traversal with an AVL ordered
index. Equality and predecessor/successor lookup are logarithmic. It retains
the original pool allocation and link-writing sequence, serialized item layout,
string offsets, and recent-cache behavior. Its comparison is deliberately not
ordinary `strcmp`: bytes are signed, but a shorter common prefix sorts higher.
The auxiliary index is private to
one generator lifetime; it must be released on every destructor path. No
process-global address cache or host-side instruction skipping is involved.

Index allocation failure, invalid preexisting ordering, and embedded-NUL ranges
select ordinary list traversal. A failed pool string write preserves the old
partially linked node and disables the auxiliary index. The empty-key hash
avoids the historical undefined read past its terminator; valid unique empty
keys retain the same handle. The prototype requires a fixed-map pool adapter,
matching this pinned version, not an arbitrary newer APT ABI.

## Current evidence

- Strict Windows build: 75/75 CTests passed before the ARM prefix correction.
  The corrected algorithm suite passed 44,550
  checks covering insertion orders, duplicates, byte ordering, existing lists,
  allocation failures, retained-cache view relocation, and cleanup.
- A representative `NewVersion` field replay of the retained 8,245,183-byte
  BigBoss Packages file covered 9,971 records and 10,041 unique strings. The
  corrected original-list model made 32,330,295 comparisons; the indexed model
  made 309,882. Serialized handles, items, links, and strings matched; 50,055 checks
  passed. This is not a complete APT parser, actual call trace, emulated CPU
  benchmark, or physical wall-clock result.
- An ascending synthetic input already favorable to head insertion took fewer
  comparisons with the original list. The index removes the repeated-search
  worst case; do not claim every input becomes faster.
- The actual Apple ARMv6 candidate passed 60,718 checks while executing the
  original and replacement ARM code. These include both stack alignments,
  volatile-register clobbering, both destructor entries, allocation refusal,
  object reuse, all first-byte values, embedded NULs and pool-write failure.
  In the normal synthetic case, lookup code retired 5,253,911 instructions
  versus 41,463,137 for the original. Pool and system-call boundaries are host
  fixtures, so this is not full OS work or a physical timing. Forced index
  allocation failure was slower than the original but preserved results.
- The first ARM edge test caught a wrong common-prefix ordering shared by the
  initial implementation and host reference. The exact binary at `0x305c`
  established the shorter-first rule, now independently tested. Earlier v1/v2
  offline artifacts are rejected and must not be installed.
- The probe initially treated R9 as preserved. Apple's documented iOS 3 ABI
  makes it volatile and uses four-byte stack alignment. The corrected probe
  poisons R9 across fixture calls. See [Apple's ARMv6 ABI](https://developer.apple.com/documentation/xcode/writing-armv6-code-for-ios).

`guest_apt_pinned.c` uses the existing pool methods and anonymous non-executable
memory mappings, with a per-generator sentinel and both destructor detours.
`build_guest_apt_index.py` accepts only the exact publisher library and bounded
Apple ARMv6 Mach-O objects. It inserts read/execute pages before LINKEDIT,
preserves virtual/file segment ordering and original TEXT/DATA addresses,
updates linker-table file offsets, and replaces the ad-hoc page signature.
`verify_guest_apt_index.py` independently checks segment extents, the three
entry edits, unchanged original code/data/linker contents, and all code hashes.
No loader or physical execution is established by that verifier.

The corrected local v3 candidate SHA256 is
`de78e2b761479eeabab8a4abd066b3cf4eb0976b82f40f65747ad08375da6b0c`.
Its generated code is 4,592 bytes in an 8 KiB executable segment; the signed
library is 1,034,523 bytes. Compiler versions can produce different artifacts;
CI rebuilds and executes its own exact candidate instead of trusting this hash.

The earlier compact bulk-string experiment did not pass its physical usability
gate and remains disabled; see [bulk execution](bulk-execution.md).

## Next gates

The corrected v3 ARM probe and exact-commit CI passed. A private package loaded
the exact candidate in Cydia; read-only checks of stopped retained disk images
confirmed its hash, the unchanged pinned package version, and the preserved
original library. Three physical refreshes completed. The two continuously
observed comparisons on the same ready checkpoint completed within
(114.519,132.312] and (117.590,132.317] seconds on the old and expanded-native
emulator engines respectively. Their bounds overlap: the native arithmetic
extension does not establish a speed win. Both used the identical v3 library
and repository contents, with bulk, native TLB and profiling disabled.

Remaining gates:

1. Full catalog/package graph equivalence, not only clean cache headers and
   matching repository hashes/counts.
2. Removal of the private package must safely restore the original library.
3. Substantial physical refresh and general-work acceleration, repeated with
   preserved paired checkpoints and normal lifecycle checks.

Do not change the pinned package version or default installation policy on the
strength of the host comparison counts. Do not modify a mounted guest disk
behind its filesystem cache or overwrite retained paired backups. An eventual
modified-library distribution must comply with APT's corresponding-source
license obligations; no library binary is included here.
