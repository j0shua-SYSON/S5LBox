# Ordered APT catalog index experiment

Status: host-tested algorithm prototype; not linked into the host app and not
installed in a guest. No physical Cydia improvement is claimed.

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
string offsets, and recent-cache behavior. The auxiliary index is private to
one generator lifetime; it must be released on every destructor path. No
process-global address cache or host-side instruction skipping is involved.

Index allocation failure, invalid preexisting ordering, and embedded-NUL ranges
select ordinary list traversal. A failed pool string write preserves the old
partially linked node and disables the auxiliary index. The empty-key hash
avoids the historical undefined read past its terminator; valid unique empty
keys retain the same handle. The prototype requires a fixed-map pool adapter,
matching this pinned version, not an arbitrary newer APT ABI.

## Current evidence

- Strict Windows build: 75/75 CTests passed. The algorithm suite passed 44,542
  checks covering insertion orders, duplicates, byte ordering, existing lists,
  allocation failures, retained-cache view relocation, and cleanup.
- A representative `NewVersion` field replay of the retained 8,245,183-byte
  BigBoss Packages file covered 9,971 records and 10,041 unique strings. The
  original-list model made 32,332,661 comparisons; the indexed model made
  307,856. Serialized handles, items, links, and strings matched; 50,055 checks
  passed. This is not a complete APT parser, actual call trace, emulated CPU
  benchmark, or physical wall-clock result.
- An ascending synthetic input already favorable to head insertion took fewer
  comparisons with the original list. The index removes the repeated-search
  worst case; do not claim every input becomes faster.
- Portable LLVM 18.1.8 compiled the source for both ARMv6 ELF and the actual
  `armv6-apple-ios3.0` Mach-O target. Compilation is not execution or ABI proof.

The earlier compact bulk-string experiment did not pass its physical usability
gate and remains disabled; see [bulk execution](bulk-execution.md).

## Next gates

1. Exact-identity guest adapter and offline linking, using the Apple ARMv6 ABI,
   existing pool methods, and non-executable private memory for the index.
2. Differential execution of the original and replacement ARM code, including
   both generator destructor variants, fallback, and object reuse.
3. Same-build physical Cydia refresh from preserved paired checkpoints, with
   catalog/package equivalence, repeated reloads, and lifecycle checks.

Do not change the pinned package version or default installation policy on the
strength of the host comparison counts. Do not modify a mounted guest disk
behind its filesystem cache or overwrite retained paired backups. An eventual
modified-library distribution must comply with APT's corresponding-source
license obligations; no library binary is included here.
