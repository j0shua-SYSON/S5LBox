# Where the guest actually spends its instructions

Measured 2026-07-30, **substantially corrected 2026-07-31**. This file exists
because the answer was recorded nowhere and was assumed wrongly for weeks; the
first version of it then got the headline wrong in its own way, which is why the
correction is at the top rather than buried at the end.

## The short version, as it now stands

Everything below the horizontal rule was measured on a WHOLE RUN. A whole run is
dominated by boot, and boot is not what decides frame rate. Windowing the
profile to an actual animation (`-W`, which the tool had all along) changes the
answer completely:

| slice | whole run (r195) | inside a frame window |
|---|---|---|
| `_SHA1Init` — page hashing | 17.8% | **3.1%** (r206) |
| Security giants — RSA modular exponentiation | ~12.5% | **40.6%** (r208) |
| QuartzCore `CA::OGL::sw_scanline`, `sw_sample_nearest_BGRA8` | not visible | **14.4%** (r208) |

So "crypto" was never one answer. **Hashing is a boot cost and irrelevant to
fps.** **RSA is the largest single consumer during a home-screen swipe**, which
is not something a real iPhone does while swiping and is therefore suspected to
be pathological rather than necessary — see the open question at the end.

The graphics path is now identified rather than guessed: `CA::OGL::sw_*` is Core
Animation's software rasteriser, the path taken because this VM un-matches the
MBX and the guest therefore composites every pixel on the CPU.

**The number that governs everything**, measured on the target device rather
than extrapolated: 25 M insn/s at 1–2 fps gives **~16.7 M instructions per
composited frame**, about 109 guest instructions per pixel. 30 fps needs ~500 M
insn/s, a **20× gap**. Concentrated work in the swipe window totals 55%, so
high-level emulation of *both* hot regions caps out near **2.2×**.

A superseded figure that should not be reused: an earlier per-frame cost of
141,596 instructions was wrong by ~100×. It came from the median gap between
consecutive `CABackingStoreUpdate` calls, and that function fires once per
BACKING STORE, not once per frame — a single composited frame updates many
layers, so the gap measured one layer update.

---

## How the userspace bucket was opened

`diagnostic_pc_name()` prints every user-mode PC as the single literal
`[userspace]`, which is why 65.3% arrived under one name. The addresses were
always in the log; only the print depth was short (16 rows, since raised to 64).

All sixteen hottest PCs of **r187, r194 and r195 independently** fall inside one
88-byte span, `0x3145ad4c..0x3145ada4` — about twenty-two instructions, one loop,
each instruction carrying ~0.8% of all samples.

```
python tools/hfsx_extract.py <work.img> dyld_shared_cache_armv6 work/cache/dsc_armv6
python tools/dscmap.py work/cache/dsc_armv6 0x3145ad4c --count 12
```

```
image:  /System/Library/Frameworks/Security.framework/Security
symbol: _mulg_common at 3145ac70
```

Its neighbours are `_SECOID_FindOID`, `_DEREncodeSequence`,
`_modg_via_recipSmall` and `_ginverseMod`: CryptKit giant-number arithmetic
beside ASN.1 DER and OID lookup. That is certificate and signature verification.

The shared cache maps at a fixed address in every iPhone OS 3 process, so a
userspace VA resolves without knowing which process was running.

## How much data SHA-1 is chewing

880 M instructions at roughly 1,200 Thumb instructions per 64-byte block is
~733,000 blocks, about **47 MB hashed in one run** — against a 96 MB shared
cache. Consistent with hashing code pages as they fault in: at boot, again at
each app launch, and again whenever memory pressure evicts a page and it faults
back.

A native SHA-1 costs ~10 host cycles/byte against the interpreter's ~19 guest
instructions/byte at 20-50 host cycles each. The slice does not shrink, it
effectively vanishes. Amdahl on 17.8% alone is **1.22x**.

## The SHA-1 site, bounded

```
./build-opt/core/machoinfo.exe firmware/kernel.macho -r <addr>
```

```
_SHA1Init                      0xc01704a8
_SHA1UpdateUsePhysicalAddress  0xc01718a4    <- next symbol
                               ---------
                               0x13fc = 5116 bytes
```

Only `SHA1Init` is exported; `SHA1Update`, `SHA1Final` and `SHA1Transform` are
local and have no symbols, so all 5 KB is attributed to `_SHA1Init`. The hot
offsets cluster near the end of the region, which is where Transform's 80-round
loop would sit. Sampled PCs there are odd, so the code is **Thumb**.

**Next step, and the reason no site is armed yet:** disassemble
`0xc01704a8..0xc01718a4` as Thumb with Capstone, find the `push` prologues, and
identify the `SHA1Update` and `SHA1Transform` entries. A prologue invented from
what a compiler "would" emit is not an identity check -- it is a guess that fails
open the first time it is wrong (see `tools/ios3_hle.c`). Both neighbouring
CoreGraphics sites needed four prologue words rather than two for exactly this
reason.

## What was ruled out, so it is not re-attempted

| hypothesis | verdict | evidence |
|---|---|---|
| ARM decode chain is the cost | **refuted** | 10-position mixed loop 14.45 M insn/s vs 1-position 14.18, same sweep |
| MMU walk is the cost | refuted | off 14.18 / sections-1M 14.04 / pages-4K 14.10 |
| data-read caching helps | refuted | 95.15% hit rate, 1.004x wall clock |
| Thumb decoder is slower | refuted | Thumb is 1.46x *faster* on the same loop |
| CoreGraphics leaf HLE is the fps lever | refuted | run168: `CGBlt_fillBytes` 43 times across 75 composites |
| device tick is free | **false, still standing** | 16.8% (14.18 -> 11.80 with tick on) |

## The framing error underneath all of it

`docs/ROADMAP.md` row D justified the dynarec with "15-30 M insn/s against
250-370 needed for realtime". That comparison is wrong.

```
fps = host_instruction_rate / instructions_per_frame
```

Guest idle is free because WFI fast-forwards: `_machine_idle` is entered 23,799
times in r195 and does not appear in the profile at all. Executing a guest second
per host second was never the requirement; only per-frame work is. The
load-bearing unknown is **instructions per composited frame**, which r187 put a
floor under at 111,337 (its minimum gap between composites).

## The SHA-1 site, disassembled

Capstone lives at `work/tools/capstone-python` (see `tools/dscmap.py` for the
`sys.path` line). `__TEXT` maps `vm 0xc0008000 -> file 0x0`, so a kernel VA in
that segment is at file offset `VA - 0xc0008000`.

**It is Thumb, and that was measured rather than assumed:** over the 0x13fc-byte
region, Thumb decodes 2,541 instructions with 1 undecodable byte and ARM decodes
1,279 with 154. Not a close call.

Candidate function entries in the region, by prologue:

| VA | offset | prologue |
|---|---|---|
| `0xc01704a8` | +0x0 | (none -- leaf) `SHA1Init`, zeroes a context at +0x5c |
| `0xc01704dc` | +0x34 | `push {r4, r5, r6, r7, lr}` |
| `0xc0170534` | +0x8c | `push {r4, r5, r6, lr}` |
| `0xc017053c` | +0x94 | `push {r4, r5, r6}` |
| `0xc01716e0` | +0x1238 | `push {r4, r5, r6, r7, lr}` |
| `0xc01717c4` | +0x131c | `push {r4, r5, r6, r7, lr}` |
| `0xc0171840` | +0x1398 | `push {r4, r5, r6, r7, lr}` |
| `0xc0171894` | +0x13ec | `push {r7, lr}` |

The profile's hot window (+0x10e8..+0x1120) falls between +0x94 and +0x1238, so
the function containing it spans roughly `0xc017053c..0xc01716e0` -- about 4,516
bytes with no backward branch. That is **the SHA-1 compression function, fully
unrolled**: 80 rounds at ~56 bytes each.

The round structure is visible in the window and confirms the identification
without needing the whole listing:

* `rors` by `#2`   -- ROTL30, applied to `b`
* `rors` by `#0x1b` -- ROTL5, applied to `a`
* `rors` by `#0x1f` -- ROTL1, over a four-way `eors` chain: the message
  schedule `W[t] = ROTL1(W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16])`

**Still to do before arming.** The exact entry of the compression function is not
yet pinned -- `0xc0170534` and `0xc017053c` are 8 bytes apart and only one is the
true entry; the other is reached by a branch. Its ABI (whether it takes
`(state, block)` or a whole `SHA1_CTX`) has to be read off the call sites, not
guessed. Intercepting the compression function is preferable to intercepting
`SHA1Update`, because it is pure: 5 words of state in, 64 bytes of block in, 5
words out, no buffering and no length bookkeeping to reproduce.

An HLE that computes a real SHA-1 over the real input is not fabrication -- it is
the same answer by a faster route -- but it must be proved so: run both paths on
the first N blocks and compare digests before the native path is trusted.

## The frame window, and what it actually contains (2026-07-31)

`-W <lo>[:<hi>]` confines the sampler to an instruction range. It existed the
whole time this file's first version was being written from whole-run data.

**r206**, windowed over the unlock drag (4.0–4.2 G): 92.0% userspace, `_SHA1Init`
down to 3.1%, nothing else in the kernel above 1%. Every one of the top sixty-
four PCs fell inside a 2 KB span that `dscmap.py` resolves to `_gshiftright`,
`_grammarSquare_common`, `_mulg_common` and `_normal_subg` — shift, square,
multiply, subtract, which together are modular exponentiation.

**r208**, windowed over a home-screen swipe (5.3–5.5 G), with per-address-space
attribution added first so the result could be interpreted:

```
187624 user samples over 2 address space(s)
51.9%  ttbr0 0x0b441000
48.1%  ttbr0 0x0bf1b000
```

and by region, over the top sixty-four PCs (55% of the window in total):

| region | share |
|---|---|
| Security giants — RSA | 40.6% |
| QuartzCore software rasteriser | 14.4% |
| everything else, over 11,625 further PCs | ~45%, diffuse |

The QuartzCore entries resolve to
`CA::OGL::sw_sample_nearest_BGRA8` (texture sampling) and `CA::OGL::sw_scanline`
(scanline rasterising). The hot PC inside `sw_scanline` sits ~2,956 bytes into
the function, so its samples spread across many PCs and the 14.4% figure is a
FLOOR for QuartzCore's real share, not an estimate of it.

## Method notes that cost real runs to learn

* Window the profile before drawing an fps conclusion. A whole-run profile
  answers a different question and reads plausibly while doing so.
* Attribute samples to an address space before deciding a hot function is on the
  frame's critical path. Two processes at ~50/50 are indistinguishable from one
  process doing everything, in a profile that only reports PCs.
* `CABackingStoreUpdate` is per layer, not per frame. Anything derived from the
  gap between its calls is a per-layer number.
* Per-frame cost in INSTRUCTIONS is host-independent, so it may be combined with
  a rate measured under different conditions. Wall-clock rates may not: r201 ran
  at 1.21 M insn/s because it ran without `--fast` and alongside another run,
  and dividing by that would have understated frame rate by ~3.5×.

## The open question this file exists to hand over

**Why is 40.6% of a home-screen swipe spent in RSA?**

A real device does none. The leading hypothesis is a retry loop, and the most
likely thing being retried is a network operation, because nothing in this guest
can resolve a hostname (see the networking notes: the PPP link opens, IPCP
negotiates DNS successfully, and zero packets are ever sent). If that is right,
the broken networking and a large share of the frame cost are the same bug, and
fixing it is worth more than any HLE site.

That is a hypothesis. It has not been measured, and the measurement is to name
the two address spaces above — which is what the pid column in the address-space
table is for.

## The largest consumer of a frame is a daemon doing endless RSA (2026-07-31)

`lockdownd`.

```
51.9%  ttbr0 0x0b441000  pid 12  "lockdownd"    (proc+0x14c)
48.1%  ttbr0 0x0bf1b000  pid 20  "SpringBoard"  (proc+0x14c)
```

Measured in r213 (address-space split) and named in r214. Within its 51.9%,
`lockdownd` spends **99.2%** inside Security's giant-number arithmetic --
`_mulg_common`, `_grammarSquare_common`, `_gshiftright`, `_normal_subg` --
which is modular exponentiation, which is RSA. It renders nothing and serves
nothing; no host is attached over USB.

So the single largest consumer of the frame budget renders nothing, and
removing it would be worth ~2x on frame rate for no rendering effort at all:

| step | per frame | fps |
|---|---|---|
| today | 16.7 M | 1.5 |
| stop the RSA | 8.0 M | 3.1 |
| + rasteriser HLE | ~1.1 M | ~22 |
| + MBX2D geometry | | 30 plausible |

### "Spinning" was the wrong word, and the call chain says why (r215)

An earlier revision of this file said "a daemon doing only RSA, forever, is
spinning". Half of that survived contact with the call chain and half did not.

**Survived: it does not stop.** `--call-probe` on `_mulg_common` captured
**141,377** entries. The last 4096 of them span instruction 5,550,086,584 to
5,599,995,809 -- the final instruction of a run capped at 5.6e9. It is 82
captures per million instructions over that closing window against 25 per
million averaged across the whole run, so the rate at the end is ~3x the
run-long average and still climbing. Nothing about it converges.

**Did not survive: that the code is a busy-wait.** The three call sites that
actually executed resolve to a chain of correct, efficient arithmetic:

```
_mulg_common  <- _modg_via_recip (0x3145b54c, 0x3145b568)   Barrett reduction
              <- _rmulg          (0x3145b618)               multiply-then-reduce
_rmulg        <- _powermodg      (0x3145be6c)               modular exponentiation
```

The two equal-count sites inside `_modg_via_recip` are the two multiplies of
one Barrett reduction -- `q = floor(x*recip)`, then `q*n`. That is textbook
bignum code doing real work. The waste is not inside the arithmetic; it is
that something above keeps asking for more of it.

`_powermodg` has exactly six static callers:

| caller | what a call means |
|---|---|
| `_isGiantPrime` | primality testing, i.e. **key generation** |
| `_DH_GenParameters`, `_DH_GenKeyPair`, `_DH_ComputeKey` | Diffie-Hellman |
| `_RSA_Encrypt`, `_RSA_SigVerify` | public-key ops, small exponent |

`_RSA_Decrypt` and `_RSA_Sign` are **not** among them, so whatever this is, it
is not a private-key operation. That matters for the remedy: sustained
~1024-bit exponentiation reaching `_isGiantPrime` would be key generation,
which is one-time and **terminates**, and the fix is to let it finish once and
persist the result. Reaching `_RSA_SigVerify` repeatedly would be a rejected
credential being retried, and the fix is upstream in activation. The two have
nothing in common except the arithmetic they burn.

Which one it is, and whether it ever ends, is r216: all six entry points probed
in one run, extended to 8e9 so termination is observable rather than assumed.

### How the upward walk was done without six more runs

`tools/dscxref.py`. Walking a call chain with `--call-probe` costs one ~35-minute
run per level and only ever sees the callers a particular boot exercised. A BL
is in the image whether or not it ran, so the static answer is cheaper and more
complete -- at the price of not knowing which sites are live.

It was validated against ground truth before being trusted: r215 measured
`_mulg_common`'s callers as returning to `0x3145b550`, `0x3145b56c` and
`0x3145b61c`, and the static scan independently found call sites at
`0x3145b54c`, `0x3145b568` and `0x3145b618` -- each exactly 4 bytes earlier,
which is a BL and its return address. It also found 11 sites this boot never
took, which is the expected difference and not a discrepancy.

### How the process names were trusted

`p_comm`'s offset is not derivable from an accessor this build byte-matches,
the way thread/task/proc/pid were. So it was not asserted: each proc is scanned
for a short NUL-terminated printable name and **the offset is printed beside
it**. Both processes yielded a name at the same `proc+0x14c`, which is the
corroboration. Independently, the profile already showed pid 20 doing QuartzCore
rasterising and the scan returned "SpringBoard" -- the name matches behaviour
that was measured before the name was known.

### What is NOT established

Why it does it. `lockdownd` owns activation, pairing and device services, and
RSA is what it does for activation records and pairing handshakes. This project
provisions activation OFFLINE -- `ActivationState = FactoryActivated` and
`BrickState = false` written straight into `data_ark.plist`, with no Apple
record applied and none verified -- so "it rejects that record and retries
forever" is the obvious guess.

It is still a guess, and the call chain has since made it a less likely one:
the six functions that can reach `_powermodg` do not include a private-key
operation, and half of them are key or parameter GENERATION, which terminates
on its own. Three cousins of this guess were wrong today (`defaultroute`,
`usepeerdns`, `resolv.conf`), each costing a ~35-minute run, and each time the
cheap diagnostic that would have settled it came second. So it is not being
acted on: r216 probes all six entry points at once and runs long enough for
termination to be observed rather than inferred.

Whether it terminates decides which fix is even the right shape. A generation
that completes is not a bug and must not be "fixed" -- it is a one-time cost to
be paid once and snapshotted past, which is what the snapshot work is for.

### Corrections this supersedes

"Crypto is an HLE target" was wrong twice. Making a loop faster is not the same
as stopping a loop that should not be running at all; had the HLE been built
first it would have shipped a ~1.5x that masked a 2x bug. Hashing and RSA were also never one answer: `_SHA1Init` is
17.8% of a whole run and 3.1% inside a frame, so page hashing is a boot cost,
while the giants go the other way.
