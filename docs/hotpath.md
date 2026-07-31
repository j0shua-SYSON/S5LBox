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
| the tick's cost is `ext_inputs()` | refuted | early-out ratio 0.804 with, 0.802 without |
| UTM SE reaches 60 fps without a JIT | **false premise** | it is the SLOW edition; TCTI trades speed for App Store legality |

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

* **The PC profile counts INSTRUCTIONS, not TIME.** `prof_sample` fires on
  `(n & 0x3ff) == 0` where n is the retired-instruction counter, so every share
  in this file is a share of instructions executed and NOT of seconds spent.
  That distinction was got wrong once here, and it cuts both ways: because VFP
  instructions were 5.5x dearer than integer ones before they were fixed, the
  VFP-dense rasteriser was a LARGER share of frame time than its 40.2% of
  instructions, and integer-bignum lockdownd a SMALLER one than its 51.5%.
  A consequence worth knowing before running the experiment: a change that
  makes instructions cheaper cannot show up in this profile at all. r221 ran
  the same window with VFP 4.9x faster and produced a page table byte-for-byte
  identical to r214's, which is the instrument working correctly, not a null
  result. Measuring that kind of win needs a wall clock.

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

## The rasteriser sites are live, and the counts corroborate the profile (r218)

`--hle` now arms the three QuartzCore sites and counts them. r218, whole run,
all in SpringBoard's space with zero wrong-address-space hits:

| site | hits |
|---|---|
| `sw_scanline` | 84,983 |
| `sw_sample_nearest_BGRA8` | 60,125 |
| `CGBlt_fillBytes` | 20,901 |
| `_CGSFillDRAM8by1` | 20,901 |
| `ogl_poly_scan` | 1,571 |

This is the calls-times-cost check the header demands before a site may become
REPLACE, and it is the check `CGBlt_fillBytes` failed: 20,901 calls across a
whole boot, against 43 inside the window that decides frame rate.

TWO INDEPENDENT INSTRUMENTS NOW AGREE. The profile bucketed a swipe by page
and attributed 39.8% of user samples to three pages; the site counter, which
knows nothing about the profile, finds those same three functions entered tens
of thousands of times in the right process. Neither could have produced the
other.

WHAT THESE COUNTS DO NOT SAY. They are whole-run, and `CGBlt_fillBytes` is the
standing proof that a whole-run count and a frame-window count can disagree by
enough to reverse a decision. The window evidence here is the profile's 39.8%,
not these numbers.

THE SHAPE OF THE CALL TREE. `ogl_poly_scan` is entered 1,571 times and
`sw_scanline` 84,983 -- about 54 scanlines per polygon, which is what scanning
a polygon means. But `sw_sample_nearest_BGRA8` is entered FEWER times than
`sw_scanline`, so it is not a per-pixel call; the per-pixel loop lives inside
these functions rather than between them. That matters for REPLACE: the leaf
to replace is a span sampler, not a pixel sampler, and its cost per call is
therefore large and variable rather than small and fixed.

### It is periodic, not a search making progress (r216 timings)

The 801 `_isGiantPrime` calls are not evenly spread. Sorted gaps between
consecutive tests:

| | instructions |
|---|---|
| min | 940,766 |
| median | 1,027,056 |
| max | 479,318,931 |

Tests cluster ~1.0 M apart -- one primality test -- in BURSTS, and the bursts
are separated by a gap that recurs at **~20.9 M instructions with under 1%
variance**, over and over for billions of instructions. That regularity is the
finding. A CPU-bound prime search does not pause on a metronome; a timer does.
At the device's 25 M insn/s, 20.9 M is about 0.84 s -- a once-a-second wake-up.

Every one of the 801 calls is entered with **`sp` = 0x002ff42c**, identical
across 6.2e9 instructions, so it is the same call path at the same depth every
time, and `r1` is 16 on all of them.

WHAT THIS RETIRES. "Key generation that terminates, and should be snapshotted
past rather than fixed" was recorded here earlier as the likely reading of
r216's entry-point counts. The timings refute it. 801 candidate tests is
already more than a 512-bit prime search should need -- the density of primes
near 2^512 gives roughly one in 355 odd candidates -- so a search that has
tested 801 and not finished is not converging. Nothing waits this out, and
there is no key to persist because none is ever produced.

WHAT IS STILL NOT ESTABLISHED. Why the periodic task re-enters. A constant
`r0` is NOT evidence of a restart on its own: a prime search legitimately
reuses one candidate buffer and mutates its contents, so the pointer would
repeat either way. Distinguishing "restarts from scratch each wake-up" from
"resumes and is preempted" needs the candidate VALUE at r0, not its address.
That is the next measurement, and it should come before any fix is attempted.

### The leading hypothesis: the prime search has no entropy

Two facts found while checking why a periodic search never converges:

* **Nothing in the SoC models a randomness source.** `grep -rn "random|entropy|RNG"`
  across `core/src/soc/*.c` and `core/src/*.c` returns three hits, all of them
  unrelated -- a comment about virtual-address entropy in `mmu.c`, and two file
  banners for the block and byte-source backends. There is no RNG device.
* **The guest re-initialises its PRNG constantly.** `_prngInitialize` carries
  17,865 samples, 0.2% of a whole run. A PRNG seeded once does not appear in a
  profile at all.

That fits every observation together: a candidate generator with no entropy
returns the SAME number each time, that number is composite, `_isGiantPrime`
rejects it, and the search re-enters on its timer forever. It explains the
non-convergence after 801 tests, the fixed candidate buffer, the identical
`sp`, and the metronome period, without needing any of them to be coincidence.

WHY THIS IS THE RIGHT KIND OF FIX. `tools/ios3_hle.c` draws the line: "It is
not a way around a bug in a device model. If something is SLOW, it is a
candidate; if something is BROKEN, the model gets fixed." Missing entropy is a
model gap, so the answer is to model the source, not to high-level-emulate the
primality test -- and HLE'ing `_isGiantPrime` would have made a non-terminating
search reject candidates faster, which is the 2026-07-30 lesson again.

IT IS STILL A HYPOTHESIS. Three of its cousins were wrong today. The decisive
measurement is unchanged: read the candidate VALUE at r0 across two separate
bursts. Identical values across bursts confirms it; progressing values refute
it and send the search back to being genuinely slow. That measurement comes
before any entropy source is written.

### The entropy hypothesis is refuted, and so is "non-converging"

Refuted before it was acted on, by the cheapest available test rather than the
one proposed. If the same composite were tested 801 times, the work per test
would be IDENTICAL -- the emulator is deterministic -- so the instruction gaps
between consecutive tests would repeat exactly. They do not: of 644 intra-burst
gaps, **641 are distinct values**, with three coincidental pairs.

So the candidates differ, and the search is doing real work on real numbers.
No entropy source is missing, and `_prngInitialize`'s 17,865 samples are the
cost of a PRNG being drawn from, not re-seeded from nothing.

**"It never converges" was also over-stated.** 801 candidates is only unusual
against a 512-bit prime, where the density near 2^512 predicts ~355 odd
candidates per prime. For 1024-bit primes the expectation is ~710 EACH, so 801
total is an ordinary mid-search position rather than evidence of a loop. And a
~20.9 M period with tiny variance is what a low-priority background thread
looks like when it gets one scheduler slice per timer tick -- which is normal
OS behaviour, not a metronome driving a retry.

WHERE THIS LEAVES IT. The reading that now fits every measurement is the
simplest one: lockdownd is generating an RSA key pair, legitimately, at low
priority, and 25 M insn/s is slow enough that it had not finished within 8e9
instructions. That restores the remedy retired earlier today -- pay it once and
persist or snapshot past it -- and the decisive test is no longer a register
value but simply a longer run: if `_isGiantPrime` stops, it terminates.

FOUR READINGS OF ONE MEASUREMENT, in one day: a spin, a terminating keygen, a
non-converging periodic search, and now a genuine keygen in progress. Each
correction came from a measurement rather than from re-reading the previous
argument, and none of them was acted on in code. That is the only reason the
cost of being wrong four times was four analyses instead of four runs plus a
fix built on the wrong one.

## The instruction RATE is a second lever, and it was never examined (2026-07-31)

`fps = rate / instructions_per_frame`. Everything above attacks the
denominator. The numerator has its own headroom, and insnbench measures it:

| configuration | M insn/s |
|---|---|
| ARM alu/branch, mmu off, no tick | 16.92 |
| **Thumb** alu/branch, mmu off, no tick | **27.54** |
| load/store, tick OFF | 17.34 |
| load/store, tick ON | 13.95 |
| load/store, mmu off -> pages-4K | 17.34 -> 16.90 |

Three readings, in order of size:

**ARM interpretation is 1.63x slower than Thumb** on comparable loops. Thumb
decodes through `switch (insn >> 12)`; ARM walks a linear masked-`if` chain in
which data processing -- the most common class there is -- is the LAST arm,
about 25 comparisons deep, while LDR/STR exits at 6. Note this does NOT
contradict "decode scatter is free": scatter-freedom says WHICH instruction
you decode does not matter, and this says the ARM path as a whole is dearer.
ARM also pays for conditional execution and barrel-shifted operands on every
instruction, so the gap is not all decode.

**The device tick costs 20%**, and its early-out is already good -- it
short-circuits 68 of every 69 calls. What remains is the cost of reaching it.

**The MMU costs 2.5%**, so the TLB is working. The insnbench legend claiming
"there is no TLB" is stale: a full ARMv6 walk per fetch and per access could
not possibly cost 2.5%.

### What the tick's 20% is NOT

`ext_inputs()` -- three loads, two shifts and a compare per instruction --
looked like the obvious suspect, and removing it from the early-out is safe in
principle: the full path already runs whenever `tb_accum >= cpu_hz`, i.e. every
69 instructions, so dropping the term can only DELAY host input by <69
instructions (~2.8 us) and can never lose it.

It buys nothing. Measured by the ratio, which is robust to the machine noise
that moved both rows: 13.95/17.34 = 0.804 before, 13.42/16.73 = 0.802 after.
The change was reverted rather than kept, because a behaviour change that
cannot be measured is a liability with no asset against it. The remaining
suspect is the 64-bit multiply-accumulate `tb_accum += ticks * tb_hz`, which
runs per instruction and cannot be constant-folded because `ticks` is a
parameter.

### On UTM SE, since it comes up

UTM SE does not reach 60 fps and is not evidence that a fast no-JIT emulator is
easy. SE is the SLOW edition: it uses QEMU's TCTI threaded interpreter
specifically to be legal on the App Store, and UTM's own framing is that this
"results in a rather slow experience even by the standards of the emulated
hardware". Fast UTM on iOS needs JIT; fast UTM on Apple Silicon uses
Virtualization.framework, which is same-architecture hardware virtualisation
and not emulation at all.

The TECHNIQUE, though, is the right one and is not yet used here. TCG
translates a basic block ONCE into an IR and caches it, then interprets the
cached ops. No code is generated, nothing is mapped PROT_EXEC, and there is
nothing for iOS to refuse -- the same property that makes `ios3_hle.c` legal.
Against a decoder that re-decodes every instruction every time, that is
precisely the ARM-vs-Thumb gap above, and it is the most promising route to
the ~1.7x still missing after the two frame-cost fixes.

## Buttons were never broken; they were slow (2026-07-31)

Reported from the device as "the buttons don't seem to work", then resolved by
the reporter's own next observation: pressing Power turned the guest's screen
black while the app kept running. That is not a failure, it is iPhone OS 3
going to sleep, which is precisely what Power does -- so the whole chain works
end to end:

    tap -> VMEngine queue -> emulator thread -> s5l_buttons_set -> GPIO pin
        -> interrupt controller -> AppleM68Buttons -> SpringBoard -> sleep

Nothing was wrong with the app path, which matches a static review that failed
to find a defect in it: the bar is enabled, the transition is queued, the
app-to-core enum translation is correct, the drain runs between chunks, and
buttons.c's own header already recorded run86 measuring the guest ARMING all
five lines at instruction 238,689,154 (INTEN group 1 = 0x00002f00).

WHY IT LOOKED BROKEN, and why this belongs in the frame-rate file rather than
an input one. At 1-2 fps a frame is 0.5-1 SECOND. SpringBoard answers Home
with an animation, which is many frames, so a press produces no visible change
for ten seconds or more -- indistinguishable from a dead button. Power looked
different only because its response is a single state change with no animation
to sit through: one frame, and the screen is off.

So "input is broken" was a frame-rate symptom, and there is no separate input
bug to fix. The lesson for the next such report is that at these frame rates
the UI cannot be judged by whether it responds, only by whether it responds
EVENTUALLY, and the status line now prints the board's own delivered/refused
counters so that question can be answered without waiting for an animation.

## The key generation TERMINATES, at ~6.4e9 instructions (r219, decisive)

r219 ran to a 30,000,000,000-instruction cap -- "stopped after 30000000000
instructions: OK" -- probing `_isGiantPrime` the whole way. 598 captures, and
the last of them at instruction **6,368,479,883**.

**23.6 billion instructions with not one primality test after it.** The search
completes and never runs again. Every earlier reading of this is now settled:

| reading | verdict |
|---|---|
| a spin | wrong -- the code is correct Barrett reduction |
| a terminating keygen | **right, and now measured** |
| non-converging periodic search | wrong -- 8e9 was simply too short a horizon |
| no entropy, same composite forever | wrong -- refuted by cost variance |

WHAT THIS MEANS FOR FRAME RATE, and it is the largest single result in this
file. lockdownd's ~51% of a frame is a ONE-TIME first-boot cost that pays off
at about 6.4e9 instructions. At the device's measured 25 M insn/s that is
roughly **4.2 minutes** of running. After it, that half of the frame budget is
free permanently, and the machine should be about twice as fast without a
single line of code changing.

It also makes persistence load-bearing rather than a convenience. lockdownd
writes its key under /var/root/Library/Lockdown, so a work image that survives
across launches keeps it and later boots skip the generation entirely. A work
image thrown away each launch pays the 4.2 minutes every time.

THE REMEDY IS THEREFORE NOT A FIX. Nothing here is a bug: it is real work that
a real iPhone also does once, on a CPU perhaps forty times faster. The correct
responses are to let it finish once and keep the result, and -- if a faster
first launch is wanted -- to provision a key the way data_ark.plist is already
provisioned, which is a choice about setup rather than a repair.

## Buttons, confirmed at the board (r220)

```
press 0  menu  down @4200000000  up @4224000000  refused 0
press 1  menu  down @4600000000  up @4624000000  refused 0
board:  sets 4  refused 0  edges driven 4
gpioic group 1: en 0x00003f00  level 0x00003900  type 0x00003f00
```

Zero refusals, four edges driven, and INTEN carrying every button line. This
is the emulator's own account agreeing with the device report that Power put
the guest to sleep. There is no input bug; see the section above on why 1-2
fps makes a working button look dead.

## The keygen is paid ONCE, and the proof is on the disk

r219 established that lockdownd's prime search ends at instruction
6,368,479,883 and never returns. The remaining question was whether that cost
recurs on every launch, because a one-time cost that is re-paid every boot is
not one-time in any way a user experiences.

It does not recur. Comparing the source rootfs against r219's work image,
which ran to 3e10 and so is long past the end of the search:

| file | source | after keygen |
|---|---|---|
| `device_private_key.pem` | absent | **present** |
| `device_public_key.pem` | absent | **present** |

Those are lockdownd's device key pair, and they are what 6.4e9 instructions of
primality testing produced. They are written into the work image, which the
app deliberately preserves -- VMFirmwareBoot.c removes an INCOMPLETE work
image and refuses to touch a finished one, on the grounds that it "is the
user's machine ... deleting it because provisioning happened to be asked again
would be data loss dressed up as repair."

SO THE FRAME BUDGET REPAIRS ITSELF, PERMANENTLY, WITH NO CODE. A first launch
pays about 4.2 minutes at the device's measured 25 M insn/s; every launch
after that finds the keys and skips the search entirely. The ~51% of frame
instructions attributed to lockdownd in r213 is therefore a FIRST-BOOT cost
and not a steady-state one.

WHAT THIS MEANS FOR THE 1-2 FPS FIGURE. That measurement was taken during the
search, so it is the worst case rather than the typical one. Steady state
should be roughly twice it before anything else is changed, which is a
prediction this file is making and which a five-minute run on the device can
confirm or refute.

## Today's fixes are worth 2.56x on RENDERING (r231)

r223 measured the VFP and decode work against a window at 3.9e9 and found
nothing. That window was wrong: r219 later showed lockdownd's keygen runs
until 6.37e9, so 3.9e9-4.2e9 is dominated by integer bignum arithmetic --
which contains no VFP at all and little that the decode hoists reach. The
fixes were being scored against a workload they were never aimed at.

Restoring at 7.0e9 instead, PAST the end of keygen, puts SpringBoard's
rendering in the window. Six runs, alternating, same 300 M instructions:

| rep | before | after |
|---|---|---|
| 1 | 24,509 ms | 9,789 ms |
| 2 | 22,122 ms | 9,565 ms |
| 3 | 27,059 ms | 9,082 ms |

**2.56x on the medians, and the ranges do not overlap** -- before spans
22.1-27.1 s, after spans 9.1-9.8 s. That is not a marginal effect needing
statistics; every run of one beats every run of the other.

WHAT IT MEANS TOGETHER WITH THE KEYGEN RESULT. The two compound, because they
address different halves of the frame:

| stage | fps |
|---|---|
| as measured on the device, during keygen | 1.5 |
| keygen paid once (no code; see the key-pair proof above) | ~3.1 |
| + today's interpreter fixes at 2.56x | **~7.9** |

THE LESSON, and it is the same one this file already records in another form:
window the measurement before drawing a conclusion. "The interpreter fixes do
not help" was stated here earlier on r223's evidence and was WRONG -- not
because the measurement was faulty but because the window answered a different
question. A whole-run profile misleads, and so does a window over the wrong
phase of the boot.

CAVEATS. These are dev-box wall-clock numbers, not A9 numbers. The VFP fix
should transfer at least as well on arm64, where the replacement is a single
mrs of FPSR against a libc call, but that is an expectation and not a
measurement. And 2.56x is the ratio over this window, which contains some
work that is not rendering.

## MBX2D attaches but does not work, and that is a real result (r240)

Four gates were modelled from the driver's own code -- reset assert (bit 16 of
0x1020), completion (bit 6 of 0x12c), core revision (0x0102xxxx at 0xf00) and
reset deassert. AppleMBX now starts, powers up and binds the display:

    AppleMBXDevice::setPowerState(1)
    AppleMBX: Added swap device: AppleH1CLCD
    AppleMBX: Using AppleH1CLCD as legacy swap device

That is the kext ATTACHED. It is not the kext WORKING, and r240 is the
difference. Booting with MBX2D enabled and no --ca-software-render:

* the boot never reaches SpringBoard -- it stalls during the multitouch
  firmware download;
* the framebuffer is blank, 384 of 460,800 bytes non-zero;
* the profile reports ZERO user-mode samples, with 44% of all samples on
  three adjacent kernel PCs (AppleMBX+0xb0dc/0xb0e0/0xb0e4) plus 12.8% on the
  register write helper at +0x1fb4.

The driver is cycling reset assert and deassert without end. That is what a
GPU driver does when work it submitted never completes: it assumes a hung
engine and resets it. Nothing here executes a command, so nothing ever
completes, so it resets forever.

WHAT THIS COSTS AND WHAT IT BUYS. The software path still works and is the
one to ship -- --ca-software-render remains the default for good reason. What
the four gates buy is that the remaining work is now NAMED: implement the 2D
command stream, so submitted work completes. That is native C against a
framebuffer rather than reverse engineering, but it is a project rather than
a gate, and it cannot be estimated from here.

THE HONEST CONSEQUENCE FOR 30 FPS. The arithmetic that made MBX2D attractive
still holds -- the rasteriser is 83% of the post-keygen frame, so removing it
is worth ~5.8x -- but the route to removing it is longer than four registers.
The alternative route to the same 83% is the rasteriser HLE, whose sites are
already armed and counted (sw_scanline 84,983 hits), and which needs
pixel-exact native replacements rather than a command processor. Neither is
cheap. Both are bounded, and this file now says which is which.
