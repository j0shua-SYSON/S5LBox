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

## RETRACTED: r231 did not execute the claimed rendering window

**Correction (2026-08-02): the 2.56x result in this section is invalid.** The
raw logs were re-audited before reusing the checkpoint. All three `before`
runs end with `snapshot format version mismatch`. All three `after` runs load
the snapshot header and then end with `restore: -F was requested but the
snapshot has no active CLCD window`. None prints a normal `stopped after ...:
OK`, a profile, a touch report, or an HLE report. The shell harness printed
`done` and recorded elapsed time without checking the emulator's exit status.

The six numbers below therefore measure image provisioning, snapshot parsing,
and two different restore failures. They measure **zero guest instructions in
the requested 7.0--7.3 B window**. They are retained as failed evidence so the
same mistake is not repeated; they must not be cited as a rendering speedup.

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

The medians differ by 2.56x and the ranges do not overlap, but that comparison
is meaningless because the two executables failed at different restore gates.

The FPS arithmetic originally derived from this failed comparison is also
retracted:

| stage | fps |
|---|---|
| as measured on the device, during keygen | 1.5 |
| keygen paid once (no code; see the key-pair proof above) | ~3.1 |
| + today's interpreter fixes | **unknown; r231 did not run** |

THE LESSON is stricter than the one this section originally claimed: a timed
wrapper is not a benchmark until the child exit code and the emulator's normal
terminal report are checked. r223 answered the wrong workload question; r231
answered no workload question at all. The steady-state effect of the interpreter
fixes remains unmeasured.

CAVEATS. The r219 facts still stand: key generation ends at instruction
6,368,479,883 and its keys persist in the work image. The r213/r214 instruction
shares also remain useful observations of the mixed keygen/render window. What
does not stand is any r231 wall-clock speedup, the derived ~7.9 fps baseline, or
an absolute FPS projection built on either. A new framebuffer-active,
post-keygen checkpoint and an exit-checked A/B are required.

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

THE HONEST CONSEQUENCE FOR 30 FPS. r214's mixed-window instruction census gives
the raster pages 39.8% of all user samples and SpringBoard 48.1%; treating that
ratio as SpringBoard's rendering share gives a conditional Amdahl bound near
5.8x. It does **not** establish post-keygen FPS, and r231 cannot supply the
missing baseline. The alternative route to the same raster subtree is the
rasteriser HLE, whose sites are already armed and counted (sw_scanline 84,983
hits), and which needs pixel-exact native replacements rather than a command
processor. Neither route is cheap, and neither has proved 30 fps.


## The memoised decode cache, measured and rejected

The hoisted data-processing arm in `arm_interp.c` carries a note asking for a
follow-up: "the rest of the chain is still linear, and the principled fix is to
dispatch on bits 27-25 the way the Thumb decoder dispatches on insn>>12". That
follow-up was built and measured, and it is SLOWER. It is written down here so
it is not built a second time.

WHAT WAS BUILT. Which arm of the decode chain an instruction leaves by is a pure
function of the instruction word: it reads no register, no mode and no memory,
and `arm_cond_passed()` has already run before the chain starts. So the answer
can be memoised. An 8192-entry direct-mapped table keyed on the full instruction
word held the arm that ran, each arm recording itself so the cached answer could
not drift from a second copy of the decode logic. Entries were a single 64-bit
word -- tag in the low half, arm in the high half -- so a racing writer could
never pair one instruction's tag with another's arm. On a hit, a switch
dispatched straight to the handler; on a miss, the chain ran unchanged. 54/54
tests passed, so this is a performance result and not a correctness one.

WHAT IT MEASURED, base and cached binaries built from the same tree and run
INTERLEAVED so host drift could not be read as a difference:

    loop           base            cached     
    mixed x10      13.70 / 14.29   11.09       -20%
    load/store     13.38 / 14.49   10.31       -26%
    ldm/stm x4      6.61 /  6.52    6.15        -6%
    vfp mul/add    10.53 /  9.89   10.67        noise
    alu/branch     17.24 / 17.12   17.00        noise

WHY IT LOSES, which is the part worth keeping. The DP hoist already paid down
the depth that made depth expensive. What remains for the hot classes is one to
six comparisons of DIRECT, correctly-predicted branches -- the branch predictor
learns a loop's decode path exactly. The cache replaces that with a multiply, a
load from a 64 KB table, a compare and an INDIRECT switch. The indirect jump
mispredicts where the chain did not, and the table evicts live data from L1. So
the trade is a handful of free branches for one expensive one plus cache
pressure, and it loses on every loop that leaves the chain early.

The earlier result in this file still stands and is not in tension with this:
"scatter is free, depth is not". Both remain true. What this adds is the third
term -- depth is only worth removing while it is DEEP, and after the hoist it no
longer is. There is no further win available from restructuring the ARM decode
dispatch, and the residual 1.7x will have to come from somewhere else.


## Correction: r240 was not waiting for a command processor

The MBX2D section above concludes that the driver "is cycling reset assert and
deassert without end", that this "is what a GPU driver does when work it
submitted never completes", and that the remaining work is therefore to
"implement the 2D command stream". The first clause is close, the second and
third are wrong, and r242 measured why.

A per-offset access histogram (S5LBOX_MBX_TRACE=1) over a full MBX boot:

    off 0x1020  reads 328,111,110  writes 3   last-write 0x00010000
    everything else in the block   46 writes total, single-digit reads

The driver is not cycling. It is stuck in ONE reset, in the deassert half, and
it never submitted anything for a command processor to complete -- 0x6d8 took
two writes in the entire run. The three adjacent kernel PCs that held 44% of
samples are that single spin, not a loop over repeated resets.

The cause was in this tree, not in the guest. The driver deasserts by
read-modify-write: it reads 0x1020 while bit 16 is set, clears bit 0, and writes
the rest back, which puts 0x00010000 into the model's register file. The read
path ORed the completion bit onto the stored value without masking it first, so
the guest's own write held bit 16 set permanently and

    ands r0, r0, #0x10000
    bne  loop                  ; spin while bit 16 is SET

had no exit. Masking before the overlay fixes it: bit 0 is the request the guest
writes, bit 16 is the answer the model owns, and neither can be forged by the
other.

WHAT THIS CHANGES ABOUT THE PLAN. "Needs a 2D command processor" was an estimate
built on a misread, and it is withdrawn. Whether MBX2D works now is a question
for the next run rather than a project, and the honest position until that run
reports is that one register bug is fixed and nothing further is proven.

The general lesson is the one this file keeps re-learning: a profile says WHERE
the guest is, and it is easy to read a story into that. 44% on three PCs was
read as "resetting repeatedly because work never finishes" when it meant "stuck
in one wait". The instrument that could tell those apart was a counter, and it
cost about forty lines.


## Where MBX2D stands after the reset fix (r246)

The reset fix works, and the size of the change is the evidence:

    r242 (before)   328,111,117 reads,  46 writes   -- 328,111,110 of them on 0x1020
    r246 (after)             37 reads,  99 writes

The spin is gone and the driver goes much further. It now programs eight buffer
addresses rather than one repeated value (0x0d981000, 0x0d7c0000, 0x0d8ff000 ...
where before every slot held 0x09676000), submits work three times, and reaches
registers it had never touched: 0x830, 0x834, 0x1024, 0x1028. It attaches, powers
up, adds AppleH1CLCD and AppleH1TVOut as swap devices, and the boot proceeds to
launching applications.

IT STILL DOES NOT WORK, and it now says why in its own words:

    AppleMBXDevice: Graphics Recovery Event
    Free slots=100, IntStatus=00000000
    2DIdle=0, 3DIdle=1, 3dblit=0, TAStatus=0
    CompletedIntStatus=00000000

ONE recovery event, not a loop, and no SpringBoard after it. 3D reports idle and
2D does not, so the driver concludes the 2D core is wedged. This is the "work
submitted never completes" state that r240 was wrongly said to be in -- it is
real now, and it was not real then.

WHAT THE NEXT ANSWER IS CONSTRAINED BY, which is the useful part. The histogram
says only THREE offsets are ever read in the entire run: 0x012c (13), 0x1020
(23), 0xf00 (1). Whatever supplies 2DIdle is a bit in one of those, and nowhere
else. Two further facts are read out of the driver rather than guessed:

  * 0xc078306c polls 0x1020 and does `tst r0, #0x1000000` -- BIT 24 is a busy
    flag, spun on while SET with a 50-try budget. The model returns it clear, so
    that particular wait already passes.
  * 0xc077f388..0xc077f428 WRITES single bits to 0x012c (0x400, 0x10, 0x8, 0x4,
    0x40) through a helper that is a plain store (`str r2,[r1,r3]`), not a
    read-modify-write. The model currently serves reads of 0x012c out of its own
    status word and drops those writes entirely, so whatever they mean is not
    modelled.

That last point is the most likely place for the next gate and it is deliberately
NOT acted on here. Setting a bit because it would make a driver proceed is the
one move this file's header forbids, and the reset bug is the argument for the
rule rather than against it: the fix landed because a counter said which register
and the driver's own code said which bit, and the previous guess -- a 2D command
processor -- would have been weeks of work aimed at the wrong thing.


## Two blockers found while trying to validate a replacement

### The MBX2D surface words are KERNEL pointers

r253 followed the surface descriptors the context points at and every word read
back as unreadable:

    hle-trace   src@c54bca00 +00=?? +04=?? ... +1c=??
    hle-trace   dst@c54bc980 +00=?? +04=?? ... +1c=??

That is not a bug in the tracer. hle_mem_read goes through
guest_read_user_bytes, which translates UNPRIVILEGED and through the MMU on
purpose (contract item 4), and 0xc54bca00 is above 0xc0000000 -- kernel space.
So the first word of each surface is a kernel object address that the compositing
process cannot itself dereference, which also rules out the reading that it is a
pixel base the library walks.

The consequence for a native blit is concrete: the pixel base is not reachable
from the arguments plus the context alone. Either the descriptor has to be read
with kernel privilege -- a deliberate widening of what an HLE site may touch, and
not one to make casually -- or the base has to come from somewhere else entirely,
such as the IOSurface the kext already knows about. Until that is settled,
mbx2DCtxBlitCopy stays TRACE.

### A restored window does not exercise the rasteriser

Four framebuffer-diff attempts over restored windows (r249, r254, r255, r256)
all reported

    armed to: NOTHING -- no registered site was ever reached in user mode

and the report is right to phrase it that way, because the naive reading of the
accompanying "RESULT: IDENTICAL" is that the replacement is pixel-exact. It is
not: with nothing armed, both arms of the diff run identical code, so identical
output is guaranteed and means nothing. The same trap sits one level down --
r240's screen dump was blank (384 of 460,800 bytes non-zero), and a blank
compares equal to a blank.

What the windows actually contained, measured rather than assumed:

  * 5.8e9..6.0e9 and 5.8e9..6.4e9 -- keygen. The report says "no SWI ever
    executed", which is exactly what a bignum loop looks like.
  * 5.8e9..6.75e9 with drags at 6.45e9 and 6.6e9 -- 893 SWIs, so user processes
    ARE running, and still no site reached. Nothing composites in that window.

So a rasteriser diff needs a window in which SpringBoard demonstrably redraws,
and "there is a picture on screen" does not establish that -- a restored
framebuffer shows the snapshot's pixels whether or not anything has rendered
since. The snapshot at 5.8e9 is good (312,474 of 460,800 bytes non-zero, the
same figure r187 produced), and the harness works; what is missing is a window
with rendering in it, and that has to be found by instrumenting when the sites
are hit rather than by choosing a plausible-looking range.


## MBX2D: what edram fixed, and what it did not

The 16 MB aperture is now modelled (S5L_MBX_APERTURE), and r266 measures the
effect against r263 on an otherwise identical run:

    unmapped pages   r263: ... 0x3c400000  0x3ba00000
                     r266: ... 0x3c400000              <- gone
    unmapped writes  r263: 812        r266: 795

So the guest's accesses to MBX-local memory are real, they are 0x3b-based, and
they were previously falling through to "unmapped, returns zero". That is fixed
and it is a genuine model gap closed.

IT DID NOT CLEAR THE 2DIdle GATE. r266 still reaches exactly one Graphics
Recovery Event with `2DIdle=0, 3DIdle=1` and never reaches SpringBoard -- the
output is byte-identical to r246 and r259. So edram was necessary and is not
sufficient, and the same is true of the interrupt line: wiring IRQ 12, which the
device tree declares and nothing was driving, ALSO changed nothing observable.
Both are correct changes. Neither is the cause.

TWO NEGATIVES WORTH KEEPING, because each looked like the answer beforehand:

  * The completion interrupt cannot fire as modelled. s5l_mbx_irq() reports the
    status word, and the driver's submit path polls 0x12c for bit 6 and
    acknowledges through 0x134 SYNCHRONOUSLY, inside the same call. By the time
    a tick samples the status it is already zero, so the line never asserts. An
    interrupt that is always low is indistinguishable from no interrupt, which
    is exactly what the measurement showed.
  * The string that would name the answer cannot be found statically. The
    driver's own "2DIdle=%d, 3DIdle=%d..." lives at 0xc0788918 and NOTHING in
    the kernel references it by absolute address -- a whole-file scan for the
    4-byte value returns nothing, and the pc-relative forms (ldr+add, ADR) do
    not resolve to it either. So where 2DIdle comes from is still open, and the
    histogram's constraint stands: only 0x012c, 0x1020 and 0xf00 are ever READ,
    37 reads in the whole run, which is far too few for a polled status.

The live hypothesis, unproven and NOT acted on: 0x012c is written single bits by
the driver (0x400, 0x10, 0x8, 0x4, 0x40) through a plain store, and the model
serves its reads out of an unrelated status word and drops those writes. If a
2D-idle bit lives there, it reads as zero forever. Acting on that means choosing
a bit, and choosing a bit because it would make a driver proceed is the one move
this file's header forbids.

### The address-space question the blit still has to answer

r265's descriptors give the destination as {0x0885c000, 0x960000} -- a raw CPU
physical in DRAM, cross-checked against the /vram pool -- and the source as
{0x03a8a000, 0x97000}, which is NOT a CPU physical: nothing is mapped there. The
device tree declares the aperture as child address 0x03000000 while this machine
puts its registers at 0x3b000000, so the consistent reading is that surface
descriptors carry CHILD-RELATIVE addresses and a blit must rebase them. That
rebase is a rule, and it has not been verified yet -- writing a blit on an
unverified base is how pixels come from a plausible wrong place.


## 2DIdle, decoded completely

It is not a register, which is why three runs of register work moved it by
nothing. AppleMBX+0xf274 formats the recovery line out of OBJECT FIELDS:

    ldrb r1, [r4, #0x143]    ; TAStatus
    ldr  r0, [r4, #0x124]    ; 3dblit
    ldrb r3, [r4, #0x121]    ; 3DIdle
    ldrb r2, [r4, #0x120]    ; 2DIdle

That reconciles the two things that looked contradictory: the histogram showing
only 0x012c, 0x1020 and 0xf00 ever READ -- 37 reads in a whole run -- while the
driver insisted the 2D core was wedged. It was reading its own memory.

The ISR at AppleMBX+0x804d4 is where the field comes from:

    ldr  r3, [r0, #0x264]
    cmp  r3, #2
    bne  <return 0, doing nothing at all>
    ldr  r3, [r2, #0x12c]     ; status
    ldr  r2, [r0, #0x148]     ; the driver's shadow of the enable mask
    and  r5, r3, r2           ; pending = status & mask
    bl   write(this, 0x134, pending)
    ands r2, r5, #0x400
    ...  strb r6, [r4, #0x120]         ; 2DIdle = 1

So bit 10 of the status word is 2D completion, and the model raised only bit 6 --
which the submit path at AppleMBX+0xe854 polls and acknowledges SYNCHRONOUSLY,
so nothing ever reached the handler. Raising bit 10 as well, and gating the
interrupt line on `status & the enable at 0x130` the way the ISR gates its own
`pending`, is what the driver is written to expect.

WHAT THAT CHANGED, measured: CompletedIntStatus went 0x00000000 -> 0x00000400.
That field is a live read of 0x12c, so the completion bit now genuinely reaches
the driver, where before there was nothing to see.

WHAT IT DID NOT CHANGE: 2DIdle is still 0 and the recovery event still fires.
And the value being STILL SET at recovery time is itself the evidence for why --
the ISR acknowledges through 0x134 as its first act, so an unacknowledged 0x400
means the handler did not run its body. The `[0x264] == 2` gate is not
satisfied, and the other branch returns immediately without acknowledging or
touching 2DIdle.

[0x264] is a driver STATE, not a constant: it has nine writers, and
AppleMBX+0xff58 shows the transition -- state 1 is incremented to 2, and another
path stores 2 directly, both around virtual calls at vtable+0x350/+0x354. So
what remains is a state machine that has not reached "running", and the next
question is which of those two paths the driver is failing to take. That is a
question about what the device has not done yet, and it is the same shape as the
reset bug: read the code, model what it waited for, do not choose a value
because it would make the driver proceed.

### The state chain, and where static reading stops paying

Traced from the ISR's gate downward:

    [0x260] != 0  -->  [0x264] = 1  -->  [0x264] = 2  -->  ISR services
                                                            -->  2DIdle = 1

  * AppleMBX+0xd3cc gates on `[0x260] != 0`, then `mov r6,#1` / `str r6,[r4,#0x264]`
    -- the 0 -> 1 transition.
  * AppleMBX+0xd16c requires `[0x264] == 1`, calls vtable+0x354, then stores 2.
    AppleMBX+0xff58 is the same step written as an increment.
  * The ISR at +0x804d4 requires 2 and otherwise returns without acknowledging,
    which is why CompletedIntStatus stays 0x400.

[0x260] is written 0x3c on one path (AppleMBX+0xe534) and
`min(vtable[0x64](), const)` on another (+0x80026c), so it is probably already
non-zero and the gate that fails is further along. That is where reading stops
paying: four levels of static descent have each cost a disassembly round and the
last one did not narrow anything.

THE NEXT STEP IS A MEASUREMENT, not a fifth level. --call-probe on the recovery
site captures registers, and r4 there is the AppleMBXDevice, so the actual values
of [0x260] and [0x264] at the moment the watchdog fires answer in one run which
transition never happened. That is the same move that ended the reset bug: stop
arguing about which register, count what the driver actually did.

## 2026-08-01 corrections: the state gate and the rasteriser root

The state-gate inference immediately above did not survive its measurement.
r269 hit the ISR entry and its `[0x264] == 2` body twice, hit the store that sets
`2DIdle = 1` twice, and captured state 2 at both points. The interrupt is
delivered, the state gate passes, and the field is set. The later unacknowledged
0x400 was a new submission after an earlier completion, not proof that the ISR
body never ran. Keep the derivation above because it explains why the probe was
necessary; do not keep its conclusion. The remaining MBX fact is narrower:
12 observed copy blits produced only two completions, so submission coverage is
incomplete and the reason is not yet known.

### `ogl_poly_scan` is the rasteriser root, statically proved

The call-count shape previously suggested this tree:

    ogl_poly_scan -> sw_scanline -> sw_sample_*

It is no longer only a count-based hypothesis. A read-only walk of the retained
`work/cache/dsc_armv6` established all of the following:

* `_ogl_poly_scan` is exactly `0x311e2100..0x311e2d04` (3,076 bytes).
  QuartzCore contains exactly one direct caller, at `0x3122cdbc` inside
  `CA::OGL::SWContext::draw_elements`.
* Immediately before that call, the caller materialises `pc + 0x3d8` and stores
  it at incoming `sp+8`. The ARM PC value at that add is `0x3122cda8`, so the
  result is `0x3122d180`, the exact symbol start of `sw_scanline`.
* `_ogl_poly_scan` loads that seventh argument and invokes it with the sole
  indirect `blx` in the function, at `0x311e2c04`. Its other calls resolve to
  clipping/interpolation helpers, `memcpy`, `ceilf`, and `floorf`.
* `sw_scanline` spans `0x3122d180..0x3122e2f0` (4,464 bytes). Its reachable
  control flow calls `sw_sample_texture` and `sw_sample_color`;
  `sw_sample_texture` selects and tail-calls the concrete sampler through the
  SWTexture function pointer.

WHAT THIS PROVES: a complete native replacement at `ogl_poly_scan` can own the
scanline and sampler work beneath it, so it reaches the whole-rasteriser lever
rather than only the root's 6.5% self time. This is the shortest currently known
non-JIT route that is arithmetically capable of crossing 30 fps.

WHAT IT DOES NOT PROVE: it does not supply a correct native transcription, show
that every argument shape is understood, establish pixel equality, or raise the
measured frame rate by one frame. Returning early at the root would merely omit
drawing. The site is therefore TRACE, not REPLACE. It records the eight incoming
arguments plus a bounded polygon dump while Apple's code still performs every
draw.

### r270: the live ABI agrees, and still no speed claim

r270 was a fresh 6.0 B-instruction cold run of commit `efa85aa`, using the r257
software-render/touch schedule. It exited 0 after 2,903.8 host seconds with a
normal cap stop at 5,999,943,780 retired instructions, zero external-md bridge
failures, and a nonblank frame (453,081 of 460,800 RGB bytes nonzero).

The HLE counts reproduced r257 exactly where the replacement matters:

    ogl_poly_scan                  2,095 hits
    sw_scanline                   95,758 hits
    sw_sample_nearest_BGRA8       56,325 hits / 56,220 handled / 105 declined

All 13 retained root records carried `sp+8=0x3122d180`, the exact `sw_scanline`
entry. `sp+12` took four live context values. The first polygons were four-vertex
quads with flags 0x030b, `w=1`, and bounds such as (0,0)..(320,480). That is
runtime confirmation of the static ownership chain, not a transcription of it.

The final PPM SHA-256 was
`F327259A64EAA6471046EC4E5477822ACD6D6DA82151C9FB5CDB549346926DFC`, byte-for-byte
the same as both r257's armed and disarmed PPMs. That equality proves the TRACE
checkpoint did not perturb this run's final framebuffer. It does not validate a
root replacement, because no root call was handled and the guest executed the
whole rasteriser as before.

One diagnostic defect is retained rather than hidden: the tracer claimed a
12-call budget and printed 13. `ios3_hle_arm()` is deliberately retried when a
shared-cache page becomes resident; `efa85aa` reset every trace budget on each
retry. The follow-up keeps a budget across re-arms in the same address space.

### Snapshot speed without snapshot theatre

r270 itself was mistakenly launched without `--snapshot-at`, so it cannot be
retroactively turned into a valid emulator checkpoint. A Windows process dump
would lack the external-md image/state sidecars and is not a substitute.

The two available shortcuts were checked rather than trusted. The useful old
3.5 B pre-drag checkpoint fails current restore with `snapshot format version
mismatch`. The current 5.8 B checkpoint restores, but r254-r256 already ran it
as far as 6.75 B with scheduled drags and reached zero HLE sites; its inherited
nonblank framebuffer is not evidence of a redraw. Both are faster and invalid
for this question.

The next necessary cold run must print each root hit's `cpu.cycles` and save a
current-version checkpoint before a measured redraw. A restored development
window is accepted only with nonzero `ogl_poly_scan` hits, a live nonblank CLCD
frame, and a clean stop. Final replacement acceptance remains a cold armed/
disarmed pair, because that is the test that has already caught zero-work diffs.

### r271/r273: a current redraw checkpoint is now validated

r271 paid the cold-boot cost at the then-current snapshot format. It wrote
checkpoints at 3.5 B and 3.9 B retired instructions, scheduled the same 26-report
drag at 4.0 B, and stopped normally at 4,399,991,513. The 3.9--4.4 B region was
not guessed to contain rendering: the whole cold run counted 324
`ogl_poly_scan` calls and 19,124 `sw_scanline` calls, the drag was accepted and
read 26/26, the external-media bridge reported zero failures, and the final
per-run PPM was nonblack with SHA-256
`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`.

r273 then restored the 3.9 B checkpoint with exact host commit `1749c77` and a
fresh external-media work image. It stopped normally at 4,399,995,184 with:

* 183 `ogl_poly_scan` and 13,812 `sw_scanline` hits after restore;
* 4,818 nearest-BGRA hits, of which 4,816 were handled and two safely declined;
* live CLCD window 0, scanning/running 1/1, and 3,825 of 460,800 RGB bytes
  nonzero;
* all 26 drag reports accepted, queued, and read, with none refused;
* zero external-media failures, two raw redirects/completions, and zero pending;
* a per-run PPM byte-identical to r271, including the SHA-256 above.

That accepts the checkpoint as the fast functional iteration path. The cold run
took 1,967.2 seconds; r273 took 256.260 seconds by a host stopwatch, even though
both cover the same post-checkpoint guest work. The shortcut is therefore roughly
7.7x faster here. It is not a substitute for the final cold armed versus disarmed
validation: a checkpoint inherits guest state, and neither run has a native root
replacement or a measured 30 fps result yet.

Two earlier restore attempts are not part of that comparison. r272 failed before
guest execution because its isolated run
directory lacked the required `firmware/` framebuffer-output directory. r272b
accidentally ran two CPU-bound restores concurrently; stopping the duplicate
also stopped the retained process around 4.181 B, so its partial log and elapsed
time prove nothing. r273 first established a single emulator process and then
completed normally.

The first restored callback records also settle the live `sw_scanline` ABI:

    r0=x, r1=y, r2=count, r3=start
    [sp+0]=dx, [sp+4]=dy0, [sp+8]=dy1,
    [sp+12]=active-field mask, [sp+16]=scan context

The first observed span was x=114, y=417..428, count=161, mask `0x0308`, with
`dy0=dy1=NULL`. Its context points to the render object and render state that the
static decode reads. The live contexts select one texture unit and use both
nearest-BGRX (`0x3122b698`) and nearest-BGRA (`0x3122b8bc`) samplers; claiming
only the already replaced BGRA path would therefore omit a measured case. Those
two are the bounded common path worth implementing first. All other texture
counts, combine modes, blend modes, formats, or unreadable operands must decline
to Apple's original code until separately decoded and pixel-diffed.

The polygon mask is `0x030b`; the callback mask drops bits 0 and 1 because x and
y are passed as integers. The enabled interpolants are therefore w/u0/v0. The
other printed float slots are disabled scratch data, not malformed colours.
The enabled sequence is coherent: u0/v0 starts at `(114.5,417.5)`, `dx` advances
u0 by one per pixel, and successive scanlines advance v0 by one.

### r274--r279: two scanline arms are native, not the rasteriser

The first implementation does not pretend to cover the 1,116-instruction
`sw_scanline`. It transcribes two exact paths and refuses the rest:

* the early no-blend tail call at `0x3122d2fc..0x3122d394`, for one texture,
  mask `0x0308`, a direct four-byte destination, the sentinel colour, equal
  min/mag sampler pointers, and nearest BGRX or BGRA;
* the observed BGRA compositing case with state byte `0x12`, mode byte `1`, and
  jump-table selector 2 at `0x3122dcf4`. Its result is the literal packed ARM
  arithmetic `src + dst * (256 - src_alpha) / 256`, including 32-bit wrap and
  the guest's lane placement.

Both samplers compute the complete source span before one guest-MMU write. The
blended path also reads the complete destination before publishing anything.
Unreadable operands, an alias that would change buffering semantics, a source
or destination fault, a different mask/format/sampler/state, and an auxiliary
surface all decline. Textured and blended spans beyond the guest's 256-pixel
temporary chunk also decline; the direct solid fill has no temporary and may
cover the measured 320-pixel display width. BGRX's one distinguishing
instruction forces alpha to `0xff`; BGRA preserves it.

r274 was the direct-only 3.9--3.95 B checkpoint smoke. Of 180 `sw_scanline`
calls, it handled 60 and declined the 120 live BGRA-blended calls; those fell
through to the already native BGRA leaf. r275 added only the decoded selector-2
arm. It handled 180/180 scanlines, reached neither leaf, stopped cleanly at the
same cap, and produced the exact r274 framebuffer SHA-256
`5B6B71F632896AF923BF7D0CEC74AB57CBB34DD4C0A3493E4B6F04CDEA568900`.
That is useful whole-frame evidence, but a final hash can miss a transient
wrong pixel that is later overwritten.

`--hle-verify` closes that gap without replacing guest code. At a proved
REPLACE entry it runs the native handler against write-capturing shadow memory,
executes Apple's routine unchanged, and compares the exact destination span at
the matching user-mode return PC, stack pointer, and TTBR0. A bounded LIFO
tracks nested scanline/sampler calls. It exits nonzero on a mismatch or an
unreadable result and reports attempted, prepared, passed, and declined shapes
separately. It is deliberately omitted from the app: this is a terminal
validation mode, not a phone setting.

r279 exercised the hardened verifier over the same short restored window:

    nearest BGRX leaf       60 prepared / 60 exact passes
    nearest BGRA leaf      120 prepared / 120 exact passes
    enclosing scanline     180 prepared / 180 exact passes
    total                  360 prepared / 360 passes / 0 failures

Each outer comparison covered 644 bytes, exactly 161 BGRA pixels. The guest
executed every original routine, the final PPM retained the SHA-256 above, the
external-media bridge had zero failures, and the stop was normal. This is the
strongest pixel evidence for these two arms; it is still evidence only for the
shapes the handler accepted.

r276 restored 3.9 B, scheduled the established 26-report drag at 4.0 B, and ran
the replacement to 4.4 B. It stopped normally at 4,399,988,869 with live CLCD
window 0, scanning/running 1/1, all 26 reports accepted and read, zero
external-media failures, and a final PPM byte-identical to both r271's cold
baseline and r273:

`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`.

The later-window coverage is the limiting result, not a footnote:

    sw_scanline             15,727 hits / 3,615 handled / 12,112 declined
    nearest BGRX leaf        4,660 hits / 4,660 handled
    nearest BGRA leaf        2,858 hits / 2,856 handled / 2 declined
    ogl_poly_scan              186 hits / 0 handled

Only 23.0% of `sw_scanline` calls were native; 77.0% safely ran Apple's body.
The leaf replacements recovered another 7,516 calls beneath those declines,
but the generic scanline and the root remain guest code. r276 took 229.545 host
seconds versus r273's 256.260, a 10.4% lower wall time. That is directional
evidence, not a controlled FPS result: replacement changes the retired-
instruction schedule, the two host commits do not execute an identical stream,
and neither run measures displayed frames per second.

The native fixtures now pass 1,417 checks and the complete core suite passes
54/54. Those tests pin ABI refusal, exact BGRX/BGRA pixels, packed selector-2
blend boundaries, atomic publication, alias/fault behaviour, and a non-mutating
oracle. They do not establish 30 fps. No native `ogl_poly_scan` exists, no cold
armed/disarmed replacement pair has been run, and `--hle` remains an opt-in host
experiment rather than an app default. The next performance lever is still the
root or the dominant declined scanline shape, not optimistic wording around a
10% checkpoint-time reduction.

### r280--r288: the full oracle caught a wrong live-mask assumption

The first full verifier replay exposed a diagnostic limit before it exposed a
pixel bug. r276 had more than 22,000 replaceable nested scanline/sampler entries,
while the verifier silently stopped preparing after 4,096. Commit `d63712e`
raises that ceiling to 65,536. r280 then replayed the accepted 3.9--4.4 B window
with Apple's routines still executing and reported:

    nearest BGRX leaf        4,830 attempted / 4,830 exact passes
    nearest BGRA leaf        4,818 attempted / 4,816 exact passes
    sw_scanline             13,333 attempted / 3,546 exact passes
    total prepared          13,192 / 65,536; 0 failures

The drag was accepted and read 26/26, external media had zero failures, and the
final framebuffer retained the cold-baseline SHA-256
`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`.
That proved the previously shipped texture paths across the full window. It
also showed that 9,787 scanline shapes still had no native candidate.

The first zero-texture implementation was wrong despite passing its synthetic
fixtures. Static decoding correctly identified the direct `_CGBlt_fillBytes`
arm and the selector-2 solid blend, but the fixture inherited textured mask
`0x0308`. r282 ran that code through the same full oracle and reproduced r280's
coverage exactly: 3,546/13,333 scanlines prepared. In other words, the new code
handled **zero measured solid calls**. Passing 1,950 local checks did not make
that a live implementation.

r285 added a bounded refusal probe rather than broadening the guard. The first
live solid call arrived at instruction 4,201,269,152 with x=0, y=20, count=320,
state `0x02`, colour `0xff000000`, and active mask **`0x0008`**. The next 15
rows had the same shape. That is the missing fact: a solid polygon carries only
w after x/y are removed; it does not carry unused u/v interpolants. The final
handler accepts only this measured solid mask. It repeats `ctx+0x64` directly
for state `0x02`, or applies the already decoded packed selector-2 blend for
state `0x12`; other masks and states still fall through to Apple.

r286 moved the identical 26-report drag to the 3.9 B restore boundary as a
targeted development shortcut. All reports were accepted and read and the
oracle had zero failures. This shortened branch discovery, but it is not the
accepted baseline because the touch schedule differs. r287 therefore restored
the original 4.0 B drag and ran to 4.4 B:

    nearest BGRX leaf        4,830 attempted / 4,830 exact passes
    nearest BGRA leaf        4,818 attempted / 4,816 exact passes
    sw_scanline             13,333 attempted / 7,119 exact passes
    total prepared          16,765 / 65,536; 0 failures

The extra **3,573** scanline results were compared at the real Apple return.
The verifier logged the first four full-width spans explicitly: each was 1,280
bytes and matched exactly. The run stopped normally, touch was accepted and
read 26/26, external media reported zero failures with 2/2 raw redirects and
completions, CLCD was scanning/running, and the final framebuffer was still the
baseline hash above. Coverage improved to 53.4%; 6,214 attempted scanlines
(46.6%) still declined.

r288 exercised replacement rather than shadow comparison. It stopped normally
at 4,399,982,889 instructions with 8,179/16,870 scanlines handled, 26/26 touch
reports consumed, zero external-media failures, and the same baseline hash.
Its run-directory timestamps span about 269.1 seconds. That is slower than
r276's 229.545 seconds and r273's 256.260 seconds, not a speed victory. The
host workload was not controlled and native replacement changed how much guest
work fit before the absolute instruction cap, so none of these times is FPS.

The fixtures now pass 1,959 checks and the complete suite remains 54/54. The
solid arms are pixel-proved and functionally stable, but this milestone still
does **not** establish 30 fps: replacement coverage is 48.5% in r288, the root
is still TRACE, and no cold armed/disarmed performance pair exists. The next
lever remains a bounded `ogl_poly_scan` replacement that subsumes the callback
tree; polishing the scanline percentage cannot satisfy the arithmetic by
itself.

### r289: atomic multi-row groundwork is not a root replacement

`ogl_poly_scan` cannot safely publish one row at a time. If row 200 faults
after rows 0--199 were written, returning false would run Apple's routine over
an already half-mutated destination. The HLE memory contract now has an
optional transactional scatter write: it translates and validates every byte
of every span before publishing the first. TRACE receives a write-denied
version of both the scalar and scatter interfaces.

The differential oracle now captures up to 480 disjoint spans and 614,400
bytes, enough for a 320x480x4 framebuffer. Expected bytes live in caller-owned
storage rather than a 600 KiB automatic object. `bootkernel` reserves eight
depth slots (4,915,200 bytes of static storage) for nested verification and
compares returned guest memory in 4 KiB chunks. Overlapping spans, insufficient
storage, range wrap, or any empty span decline before capture.

The fixtures exercise two-span capture, overlap/capacity refusal, TRACE writev
denial, and non-mutation, bringing the focused total to 1,972 checks. All 54
tests and the strict-warning build pass. r289 then replayed the 3.9--3.95 B
checkpoint oracle and reproduced all 360 nested exact passes with zero failures
and the accepted SHA-256
`5B6B71F632896AF923BF7D0CEC74AB57CBB34DD4C0A3493E4B6F04CDEA568900`.

This milestone changes verifier/publication plumbing only. `ogl_poly_scan`
remains TRACE, no multi-row replacement calls writev yet, and there is no new
performance result. Its value is narrower: the first root arm can now fail
atomically and be compared row for row instead of relying on a final screenshot.

### r290--r292: the first root arm is exact, and it is not fast enough

The first `ogl_poly_scan` replacement accepts one deliberately narrow shape:
four vertices in the measured TL/TR/BR/BL order, integer on-screen x/y, w=1,
the exact 320x480 bounds and `sw_scanline` callback, and either mask `0x030b`
with axis-aligned one-texel-per-pixel u0/v0 or mask `0x000b`. It renders every
row through the already proved scanline arms into host scratch, rejects
overlapping destination rows, and publishes all rows with one transactional
`writev`. A later source read that aliases an earlier destination row sees the
scratch result, preserving Apple's top-to-bottom dependency without exposing a
half-written guest buffer. Everything outside that shape runs Apple.

The strict fixture now passes 3,981 checks. In addition to the existing leaf
and scanline cases it pins the 320-pixel direct bound, textured/direct-solid/
blended-solid two-row roots, second-row source and publication faults, oracle
capacity failure, same-row alias refusal, and prior-row alias visibility. The
complete suite remains 54/54.

r290 first replayed only 3.9--3.91 B. All three eligible root calls prepared and
matched Apple's returned rows exactly. That was a smoke test, not sufficient
coverage. r291 therefore replayed the accepted 3.9--4.4 B drag window with the
guest routine still executing:

    nearest BGRX leaf        4,830 attempted / 4,830 exact passes
    nearest BGRA leaf        4,818 attempted / 4,816 exact passes
    sw_scanline             13,333 attempted / 7,179 exact passes
    ogl_poly_scan              167 attempted /    92 exact passes
    total prepared          16,917 / 65,536; 0 failures

All 92 prepared roots matched every captured row byte. The other 75 calls were
not errors: they failed the narrow guard or a proved child shape and executed
Apple. The run stopped normally at 4.4 B, touch was accepted and read 26/26,
external media had zero failures with 2/2 raw redirects/completions, and the
framebuffer retained the accepted SHA-256
`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`.

r292 then enabled replacement over the same checkpoint and touch schedule. It
stopped normally at 4,399,990,620 with:

    ogl_poly_scan              187 hits /   94 handled /   93 declined
    sw_scanline              9,096 hits /  414 handled / 8,682 declined
    nearest BGRX leaf        5,520 hits / 5,520 handled
    nearest BGRA leaf        3,354 hits / 3,352 handled / 2 declined

The root therefore removed roughly 7,800 nested scanline entries from this
fixed-instruction window. The final framebuffer was again byte-identical to the
baseline, all 26 touch reports were consumed, and storage remained clean.

The performance result is negative. r292 took 270.864 host seconds, versus
about 269.1 seconds for r288: approximately **0.7% slower**, which is noise-level
but certainly not a speed victory. Worse for reproducibility, the r290--r292
executable also contained a separate 1 KiB HLE MMU batching experiment in
`bootkernel.c` that was uncommitted during those runs and later landed as
`aa67a45`; the time cannot be attributed to this root commit alone. Even with
that batching present, no wall-time gain appeared. A fixed
retired-instruction run is not displayed FPS, and replacement changes how much
guest work fits inside the cap, so this does not prove zero frame-rate benefit
either. It proves only that **30 fps has not been demonstrated**.

The next lever is the 93 declined roots. The retained refusal traces already
show at least three distinct causes: normalized texture extents, nonzero r2/r3
clip origins, and a full-width `0x000b` solid rectangle whose child state still
declines. Each needs an exact reason and its own live oracle; broadening the
guard from geometry alone would repeat the r282 mistake. Final acceptance still
requires a cold armed/disarmed pair plus an actual frame-cadence measurement.

### r293--r297: four full-screen solid roots are exact, still not 30 fps

The first refusal trace above was not specific enough: it showed geometry but
not which scanline guard rejected it. A read-only diagnostic now names the root
stage and, for child refusal, records the render/context state of the failed
row. r293b identified two distinct cases instead of treating all rectangles as
interchangeable:

* The repeated 161x30 textured fade uses state `0x12`, mode 2, nearest BGRA,
  and a changing non-sentinel value at `ctx+0x64`. That is an unimplemented
  modulation/combine path. It remains declined; mode 1 selector-2 source-over
  is not evidence for mode 2.
* The 320x460 `0x000b` solid root uses state `0x12`, source `0xfd000000`, no
  auxiliary surface, and the already decoded selector-2 blend. Its first row
  was rejected only because the guest's internal temporary is chunked at 256
  while the callback receives 320 pixels. The native arrays already cover 320,
  and each solid output pixel is independent.

The solid candidate therefore changes only that branch's bound from 256 to the
measured 320-pixel display width. A 320-pixel fixture pins every result to
`0xff020202` over `0xdeadbeef`; 321 still refuses. The focused suite now passes
5,062 checks.

r294 used the explicitly development-only early-drag schedule to reach the
branch sooner and found zero verifier failures. r295 then repeated the original
4.0 B drag to the same 4.21 B stop as pre-change r293b. This made the delta
unambiguous:

    pre-change r293b  root 76/83 exact; scanline 3,730/4,514 exact
    candidate r295    root 77/83 exact; scanline 4,190/4,514 exact

Exactly one additional root and exactly 460 additional rows prepared and
passed, with no mismatch or unreadable span. r296 paid for the full accepted
3.9--4.4 B oracle rather than extrapolating from that one call:

    nearest BGRX leaf        4,830 attempted / 4,830 exact passes
    nearest BGRA leaf        4,818 attempted / 4,816 exact passes
    sw_scanline             13,333 attempted / 9,019 exact passes
    ogl_poly_scan              167 attempted /    96 exact passes
    total prepared          18,761 / 65,536; 0 failures

Relative to r291, this is exactly four more roots and 1,840 more exact rows.
The drag was accepted/read 26/26, external media again reported zero failures
and 2/2 raw completions, and the framebuffer remained the accepted
`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`.

r297 enabled replacement. Because native roots let more guest work fit inside
the same retired-instruction cap, its counts are not a one-for-one replay of
r292, but the later-window coverage moved in the intended direction:

    ogl_poly_scan              213 hits /  106 handled /  107 declined
    sw_scanline              7,447 hits /  483 handled / 6,964 declined
    nearest BGRX leaf        6,440 hits / 6,440 handled
    nearest BGRA leaf        3,850 hits / 3,848 handled / 2 declined

The screen hash, 26/26 touch consumption, and storage result were unchanged.
The stopwatch result was not: 271.237 seconds, versus 270.864 for r292 and
about 269.1 for r288. That is again effectively flat and **not a 30 fps result**.
The root is doing materially more correct work, but a fixed instruction cap is
not frame cadence and the current replacement overhead/remaining mode-2 work
still dominates any wall-time saving.

A new late diagnostic checkpoint was considered to shorten these iterations.
The host correctly refused `--restore` plus `--snapshot-at` with external media;
composing those states is not implemented safely and requires a cold boot. The
clean 3.9 B checkpoint was retained untouched. Paying a new 45-minute cold boot
solely for this branch diagnosis was unnecessary because the restored oracle
is already validated; final acceptance remains cold.

The highest-value next case is now the measured mode-2 textured modulation,
not the ABI/normalized-UV declines. It must be decoded from the guest's actual
selector and proved with the same oracle before it may replace anything.

### r298--r300: mode 2 is exact; the first clear timing gain is combined

Mode 2 was decoded from the retained armv6 shared cache rather than inferred
from the fade on screen. `sw_scanline` loads `state+0x08` at `0x3122d648`; value
2 dispatches through `0x3122d700` to the loop at
`0x3122d744..0x3122d7c0`. For every BGRA lane, that loop computes the literal
integer expression

    context_colour * (texture + 1) >> 8

and then state `0x12` selects the already decoded source-over arm 2 at
`0x3122dcf4`. The `+context_colour` term in Apple's `mla` matters: replacing it
with the prettier `colour * texture / 255` is not equivalent. With the measured
mask `0x0308`, `mask & 0xf0` is zero, so the colour pointer is `ctx+0x64` with
zero stride. No interpolated colour is involved. The retained calls use both
nearest BGRA and alpha-forced BGRX.

The native arm accepts only that shape: one texture, equal min/mag sampler,
mask `0x0308`, state `0x12`, mode 2, no auxiliary surface, and at most the
guest's real 256-pixel temporary. It still refuses mode 3, interpolated-colour
masks, unknown samplers, auxiliary side effects, and 320-wide mode-2 rows.
Hard-coded BGRA and BGRX fixtures pin the modulation rounding before the blend;
a two-row root fixture pins transactional publication. These are independent
expected constants, not results generated by the new helper.

r298 was the bounded 3.9--4.06 B oracle. That stop includes all five initially
traced 161x30 fades and no later full-width case. It passed 41/41 roots,
1,587/1,587 scanlines, 479/479 BGRX leaves and 1,108/1,108 BGRA leaves with zero
failures. It intentionally stopped after touch report 10/26, so its
`9E4D...D7A` framebuffer is only a point-in-time hash and is not acceptance
evidence.

The first full attempt, r299, is also not evidence. PowerShell promoted the
first expected `root.decline` diagnostic on stderr to a terminating wrapper
error and tore the process down before a report. r299b used raw process log
redirection and completed the original 3.9--4.4 B schedule:

    nearest BGRX leaf        4,830 attempted / 4,830 exact passes
    nearest BGRA leaf        4,818 attempted / 4,816 exact passes
    sw_scanline             13,333 attempted / 9,277 exact passes
    ogl_poly_scan              167 attempted /   107 exact passes
    total prepared          19,030 / 65,536; 0 failures

Relative to r296, mode 2 adds exactly 258 scanlines and 11 roots. Touch was
accepted/read 26/26, external media reported zero failures and 2/2 raw
completions, and the final framebuffer remained
`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`.

r300 enabled replacement and completed normally at 4,399,989,370:

    ogl_poly_scan              213 hits / 117 handled /   96 declined
    sw_scanline              7,189 hits / 483 handled / 6,706 declined
    nearest BGRX leaf        6,440 hits / 6,440 handled
    nearest BGRA leaf        3,592 hits / 3,590 handled /    2 declined

The extra 11 roots remove exactly 258 nested scanline and BGRA-leaf calls.
Touch, storage and the accepted final hash were unchanged. The stopwatch moved
from r297's 271.237 seconds to 253.476 seconds, 17.761 seconds or about 6.5%
lower. That is the first clear movement off the roughly 270-second plateau.

It is **not attributable to mode 2 alone**. The shared executable also contained
a then-uncommitted contiguous-read optimization for proved 1:1 texture rows.
Its r294 smoke compared 183 prepared spans exactly, and r299b validates the
combined implementation over all 19,030 prepared spans; its earlier r295 full
replacement attempt ended before guest execution and proves nothing. r300 is
therefore a combined timing result. It is also still a fixed-retired-instruction
stopwatch, not displayed cadence. **30 fps has not been demonstrated.** Final
acceptance still needs a cold armed/disarmed pair and an actual frame-cadence
measurement.

The contiguous-read change is now a separate checkpoint. It does not broaden
the set of guest spans that HLE will replace and it does not change the pixel
algorithm. It recognizes only a nearest-neighbour 1:1 row with `w == 1`,
`dw == 0`, `du == 1 texel`, constant `v`, exactly representable integer or
half-integer 16.16 coordinates, no clamp, no 32-bit address overflow, and no
source/destination overlap. That row is read into the existing private pixel
buffer with one host callback instead of up to 320 four-byte callbacks. Every
other row still uses the instruction-for-instruction sampler. The boot host's
guest-memory callback retains the existing 1 KiB translated chunks, so a
single HLE callback does not assume physical or page-table contiguity.

The focused fixtures prove one 12-byte read for both BGRA and alpha-forced
BGRX leaves, one 1,280-byte read for a full-width row, and two 12-byte reads
for a transactional two-row root. They also retain the alias, source-fault and
destination-fault refusals. The strict local binary passes 5,350 checks and the
complete strict tree passes 58/58 tests. The live evidence is still the r294
183-span smoke and the r299b combined 19,030-span oracle with zero failures.
There is deliberately no stand-alone timing claim: no identity-read-only full
replacement run was completed. The only measured gain remains r300's combined
6.5%, and **neither that number nor this callback reduction proves 30 fps**.

The next bounded rasterizer case is visible in r299b's refusal log: 320-wide
mode-2 BGRA and BGRX roots. `sw_scanline` chunks those rows around its
256-pixel temporary. The native root must reproduce that chunking and prove
the combined row transaction; simply deleting the bound would be an unmeasured
semantic change.

### r301--r306b: measured 320-wide chunking is exact, but slower in this run

The retained armv6 cache corrected an important wording error above. The root
does not call `sw_scanline` twice. It calls it once with 320 pixels at
`0x311e2c04`; `sw_scanline` itself selects `min(remaining, 256)` at
`0x3122d4a8..0x3122d4d4`, publishes that chunk, advances the selected start
attributes through `_ogl_poly_scan_increment_n`, and loops at
`0x3122e25c..0x3122e2d8`. For the measured `0x0308` mask those attributes are
w/u/v and the root delta is exactly 0/1/0. The native root now reproduces only
the retained 320-wide, one-texture, no-auxiliary, mode-2 `0x12` BGRA/BGRX
shape as 256 + 64 pixels. All other over-256 scanlines keep their old path.
Both chunks remain private until the entire rectangle can be published by one
transactional `writev`.

The implementation also preserves Apple's within-row visibility. A later
chunk can read bytes written by an earlier chunk, including a read that
straddles old guest bytes and captured bytes. A conservatively refused bulk
texture read falls back to the exact four-byte texel loop before the root gives
up; no destination byte has been published at that point. Fixtures cover BGRA
and BGRX 256/64 reads, a mixed old/prior-chunk alias, second-chunk rollback, the
bulk-to-scalar fallback, and refusal of an unmeasured 320-wide mode-1 row.

The path to accepted evidence was not clean:

* r301 is invalid. It ran from the result directory, so the relative
  `firmware/screen.ppm` invalidation failed before guest execution.
* r301b and r302 had zero verifier mismatches, but an initial implementation
  incorrectly split every over-256 root row. Their clean-looking summaries do
  not validate the final code. The target BGRA root still declined on row 15.
* r303's bounded diagnostic exposed both facts: ordinary full-width rows were
  being split despite expecting one write, and the target's second chunk could
  not read `0x04572000`.
* r304 temporarily instrumented that exact translation. Both the 256-byte bulk
  access and a four-byte scalar access returned ARMv6 FSR 7: the page is
  unmapped, not merely non-contiguous. The instrumentation was then removed.
  Apple's guest execution can fault that page in; HLE cannot skip that side
  effect, so this first BGRA occurrence must continue through Apple. r304 is
  diagnosis, not acceptance.

r305 is the final non-instrumented, full 3.9--4.4 B oracle from the retained
clean pre-drag checkpoint and original 26-report drag:

    nearest BGRX leaf        4,830 attempted / 4,830 exact passes
    nearest BGRA leaf        4,818 attempted / 4,816 exact passes
    sw_scanline             13,333 attempted / 9,277 exact passes
    ogl_poly_scan              167 attempted /   113 exact passes
    total prepared          19,036 / 65,536; 0 failures

That is six more exact roots than r299b, with no change to the leaf or guest
scanline counts. The demand-paged BGRA row still declined with the bounded
`failure=3@04572000+4` breadcrumb. The run exited normally, accepted and read
all 26 touch reports, recorded zero external-media failures with 2/2 raw
redirects/completions, and retained the accepted final SHA-256
`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`.

The first replacement wrapper, r306, is also invalid. PowerShell promoted the
expected decline breadcrumb on stderr to a terminating error and killed the
emulator near 4.18 B; it produced neither a final report nor a framebuffer.
r306b used process-level redirection and completed normally at 4,399,991,337:

    ogl_poly_scan              295 hits /  150 handled /  145 declined
    sw_scanline              7,863 hits / 1,035 handled / 6,828 declined
    nearest BGRX leaf            0 hits /     0 handled
    nearest BGRA leaf        7,480 hits / 7,478 handled /     2 declined

Touch, storage, the demand-page refusal and final framebuffer hash all matched
r305. The performance result is negative: r306b took 278.072 seconds, versus
r300's 253.476 seconds, **24.596 seconds or about 9.7% slower**. This is one
full run rather than a variance study, and replacement changes how much guest
work fits under the retired-instruction cap, so it does not isolate the cost of
six newly eligible roots. It absolutely does not justify claiming a speedup.
The chunk path is correct and broadens proved coverage, but **30 fps remains
unproven**. Final performance acceptance still requires an actual frame-cadence
measurement and a cold armed/disarmed pair; the clean snapshot is an iteration
tool, not cold-boot evidence.

### r307--r309: clip bounds remove more subtrees; timing is confounded

The next apparent ABI refusal was decoded before its guard was widened. At
entry `ogl_poly_scan` converts r2/r3 with signed `vcvt` and does the same to
incoming stack words 0/4. The scan loop clamps x to `[r2, stack0)` at
`0x311e2900..0x311e2928` and y to `[r3, stack4)` at
`0x311e281c`/`0x311e2cb4`. These are clip minima and maxima, not a requirement
that every rectangle begin at zero. The distinction affects pixels: one live
polygon spans y=16..97 but has a y clip of 20, so accepting r3 without advancing
v would draw four extra rows from the wrong texels.

The native root now intersects only its already-proved integer, axis-aligned,
one-to-one rectangle with an in-panel integer clip. Removed left/top pixels
advance u/v by the exact integer distance. Empty clips and the still-unknown r1
path decline. A fixture clips all four sides of a 3x2 texture to one pixel,
checks the source texel and destination address independently, and proves every
other destination pixel stayed untouched.

r307 was the fast 3.9--4.23 B differential replay. At the identical cap, r302
had 83/83 completed root comparisons; r307 completed 89/89, so six newly
clipped roots were byte-exact. It prepared a seventh root at the cap, however,
and stopped with verifier depth 2: root 96 attempted / 90 prepared / 89 passed,
with its enclosing scanline also one short. The zero failure counter is not a
complete pass when work is unresolved. r307 is useful bounded evidence, not
acceptance.

That run also showed why several clipped calls still refused. Their mode-1
widths were 300, 304 and 320, over the child handler's 256-pixel temporary.
Static code makes this generic rather than another guessed shape:
`sw_scanline` chooses `min(remaining, 256)` before it dispatches mode 1 or mode
2, then uses the same `increment_n` and destination advance for both. The root
therefore applies 256 + remainder to the already-proved `0x0308`, state `0x12`,
single-texture BGRA/BGRX mode-1 and mode-2 paths up to the 320-pixel display
bound. A 257-pixel 256+1 fixture pins the boundary; mode 3 remains refused. The
focused binary passes 14,441 checks.

r308 then completed the full 3.9--4.4 B oracle:

    nearest BGRX leaf        4,830 attempted / 4,830 exact passes
    nearest BGRA leaf        4,818 attempted / 4,816 exact passes
    sw_scanline             13,333 attempted / 9,277 exact passes
    ogl_poly_scan              167 attempted /   137 exact passes
    total prepared          19,060 / 65,536; 0 failures

All comparisons resolved, all 26 touch reports were accepted/read, storage had
zero failures with 2/2 raw redirects/completions, and the final hash was the
accepted `E0CE...CEC8`. Relative to r305, clipping plus proved mode-1 chunking
adds 24 exact whole roots. The remaining early declines are materially
different: normalized texture extents, fractional animated geometry, and the
known demand-paged mode-2 row.

r309 replacement completed normally with the same accepted hash and device
results. It handled 245/313 roots versus r306b's 150/295, while nested handled
scanlines fell from 1,035 to zero and handled BGRA leaves from 7,478 to 38. The
IOMFB swap handler still fired 69 times. Wall time moved from r306b's 278.072
seconds to 264.020 seconds, 14.052 seconds or about 5.1% lower. It still did not
beat r300's 253.476 seconds; it was 10.544 seconds or about 4.2% slower.

There is a serious attribution limit. Between r307 and the r308 build, another
concurrent process modified the app, button/GPIO/PMU model, related tests, and
`bootkernel.c`. Ninja rebuilt those dirty sources into r308/r309. The r308
same-binary differential result still directly compares this HLE output with
Apple's output, but the full executable is not an isolated checkout and the
r309 timing cannot be attributed to this patch. A clean detached build of the
exact commit must repeat the strict suite and full oracle/replacement pair.
Until then, this is expanded exact coverage plus a confounded timing signal,
**not a 30 fps result** and not final acceptance.

### r310--r311b: the clip result survives clean isolation; 30 fps still is not measured

The required isolation repeat used a detached worktree at exact commit
`65d405607cf78b9f6b95595621ab4c0ab4027046`. Its tracked tree was clean, its
fresh RelWithDebInfo build passed all 54 standard tests, and the resulting
`bootkernel.exe` SHA-256 was
`7928A6301B85554257BB877ACA780625DBF9ABD3FA07A8EA4C2074B56D681E82`.
GitHub Actions independently passed core-tests run `30702303205` and iOS build
run `30702303219` for that exact commit.

r310 repeated the complete 3.9--4.4 B live differential oracle with that clean
executable:

    nearest BGRX leaf        4,830 attempted / 4,830 exact passes
    nearest BGRA leaf        4,818 attempted / 4,816 exact passes
    sw_scanline             13,333 attempted / 9,277 exact passes
    ogl_poly_scan              167 attempted /   137 exact passes
    total prepared          19,060 / 65,536; 0 failures

Every prepared comparison resolved. The run exited zero after exactly 4.4 B
instructions in 355.553 host seconds, consumed the drag 26/26, reported zero
storage failures and 2/2 raw redirects/completions, and produced the accepted
`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`
framebuffer hash. These counters exactly match r308. That removes the dirty
executable ambiguity from the correctness result: clip handling and mode-1
chunking are byte-exact on the isolated source commit.

The first directory named r311 is **not a benchmark result**. Its PowerShell
wrapper promoted an intentional HLE diagnostic on native stderr to a terminating
`NativeCommandError` around 4.18 B instructions. It has no completion record and
must not be timed or compared. r311b used process-level raw stdout/stderr capture
and is the valid clean replacement run. It exited zero in **255.972 seconds**,
stopping normally at 4,399,999,717 with:

    ogl_poly_scan              313 hits / 245 handled /   68 declined
    sw_scanline              3,271 hits /   0 handled / 3,271 declined
    nearest BGRA leaf           40 hits /  38 handled /    2 declined
    nearest BGRX leaf             0 hits

It also retained 61 H1 window updates, 69 swap-handler calls, 26/26 accepted and
read touch reports, zero storage failures, 2/2 raw completions, and the same
accepted framebuffer hash. Against the immediate r306b result of 278.072
seconds, the clean run is 22.100 seconds or about **7.9% lower**. Against the
best historical r300 result of 253.476 seconds, it is 2.496 seconds or about
**1.0% higher**. The clean evidence therefore supports a real reduction from
the immediate pre-clip configuration, while also showing no new best time.
Single-run host variance and the different amount of guest work admitted under
a fixed retired-instruction cap prevent stronger attribution.

Most importantly, neither number is displayed frame cadence. The desktop H1
swap-handler count is a non-invasive activity proxy, not the iOS app's
changed-published-frame FPS counter. This checkpoint still does **not** prove
30 fps. Final performance acceptance remains an actual app cadence measurement
on target hardware plus a cold armed/disarmed pair; the 3.9 B snapshot remains
an iteration accelerator, not cold-boot evidence.

### r315--r320: the 320-to-1 row is bilinear, and four roots are now exact

The first implementation of the next refusal was wrong in a useful but
important way. The polygon geometry really is a 320-pixel rectangle with a
texture extent of only `[0,1)`, and retained disassembly does show binary64 to
binary32 root deltas, pixel-centre starts, and non-fused VFPv2 chunk advances.
But the initial fixture supplied nearest BGRA. The live context instead points
at `0x3122bad8`, a different sampler. r315's bounded run showed zero failures,
but its apparent counter increase was not a clean like-for-like proof. r316's
full isolated run was decisive: it ended with the same 167 attempted / 137
prepared / 137 passed roots as r310, while the refusal moved only from
`texture-map` to `scanline`. The patch had **not handled the live row**. That
local commit was never pushed and was amended rather than preserving an
overclaim in public history.

r317 added trace-only descriptor output and stopped just after the first live
call. It is diagnosis, not acceptance. The captured context was:

    sampler       0x3122bad8 / 0x3122bad8
    texture base  0x031b3000
    pitch         32 bytes
    max x/y       0x0000ffff / 0x005fffff

Disassembly of `0x3122bad8..0x3122bce0` identifies a bilinear BGRA routine. It
subtracts half a pixel in 16.16 space, clamps a 2x2 neighbourhood, loads all
four taps, and interpolates packed byte lanes. For this exact descriptor there
is only one texel column, so both horizontal taps always clamp to column zero.
The retained rectangle reaches every row at an exact vertical pixel centre;
the two vertical taps are either distinct with interpolation weight zero or
the last row clamps both taps together. Its result is therefore exactly the
upper texel, although the lower address still has to be readable to preserve
the native fault boundary.

The replacement is deliberately scoped to that proof. It accepts the exact
bilinear function only through the transactional rectangle-root memory facade,
requires `max_x == 0x0000ffff`, `w == 1`, `dw == 0`, non-wrapping positive u
progression and constant v across the row, reads both vertical tap addresses,
and refuses overlap with the current destination chunk. Ordinary leaf and
stand-alone scanline calls still decline. A two-column descriptor and
same-chunk alias also decline. The focused fixture models two rows and both
256+64 chunks, including the next-row probe and last-row clamp; it passes
21,520 checks.

r319 was the shortest exact-commit live gate, stopping at 4.218 B. It changed
the first scaled root from declined to prepared, but stopped before Apple's
routine returned: 84 attempted / 84 prepared / 83 passed, verifier depth one.
It had zero failures, but it is **not a correctness pass**. Extending that
bounded run would duplicate most of the required full replay, so r320 went
straight to the complete 3.9--4.4 B oracle from the clean checkpoint.

r320 used detached exact commit
`ee89b20700ef64ec9a0029dcfbde80953c67972c`; its fresh RelWithDebInfo emulator
SHA-256 was
`0C644BA14C15841E9E098820B06C0BC55D19504F350F87A979EAD666A6872835`, and all
54 standard tests passed. The full oracle completed in 356.107 seconds with:

    nearest BGRX leaf        4,830 attempted / 4,830 exact passes
    nearest BGRA leaf        4,818 attempted / 4,816 exact passes
    sw_scanline             13,333 attempted / 9,277 exact passes
    ogl_poly_scan              167 attempted /   141 exact passes
    total prepared          19,064 / 65,536; 0 failures

All comparisons resolved. Relative to r310, exactly four additional roots are
prepared and byte-exact while every leaf and scanline count is unchanged. The
run consumed all 26 touch reports, reported zero external-media failures and
2/2 raw redirects/completions, retained 69 H1 swap-handler calls, and produced
the accepted framebuffer SHA-256
`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`.
GitHub Actions also passed iOS build `30707858469` and all eight core-test jobs
in `30707858484` for that exact commit.

There is no replacement timing result in this checkpoint yet. r320 is a
differential correctness run, not a benchmark, and its 356-second wall time is
not guest FPS. **30 fps remains unproven.** A clean replacement run is still
needed for a local performance signal; final acceptance still requires actual
published-frame cadence in the iOS app and a cold armed/disarmed comparison.

r321 supplies that missing local replacement signal, and it is negative. It
used the same detached `ee89b20700ef64ec9a0029dcfbde80953c67972c`
RelWithDebInfo executable and clean pre-drag checkpoint as r320, but selected
`--hle` rather than `--hle-verify`. The executable hash remained
`0C644BA14C15841E9E098820B06C0BC55D19504F350F87A979EAD666A6872835`.
The emulator produced its complete report and self-reported a normal
`stopped after 4399999702 instructions: OK`; the PowerShell wrapper did not
retain a numeric child `ExitCode`, so that field is unavailable rather than
silently asserted. Wall time was **271.818 seconds**, with:

    ogl_poly_scan              313 hits / 260 handled /   53 declined
    sw_scanline              1,831 hits /   0 handled / 1,831 declined
    nearest BGRA leaf           40 hits /  38 handled /    2 declined
    nearest BGRX leaf             0 hits

The final framebuffer still matched
`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`.
All 26 touch reports were accepted and read, external storage reported zero
failures with 2/2 raw redirects/completions, and the desktop proxies remained
61 H1 window updates and 69 swap-handler calls. Compared with r311b, r321
handled 15 more whole roots and left 1,440 fewer guest `sw_scanline` entries,
but took **15.846 seconds or about 6.2% longer**. This single run cannot separate
host variance from the changed guest work admitted by replacement under the
fixed retired-instruction cap. It nevertheless provides no evidence that this
new shortcut made the measured workload faster. It is still not an FPS result,
and **30 fps remains unproven**.

### r323: fractional one-column rows are exact; most remaining rows are wider

The next refusal was fractional vertical geometry, but accepting `float` y
coordinates by rounding them on the host would not have been a valid fix.
Retained `_ogl_poly_scan` code shows two distinct edge rules. Its first active
edge computes `ceilf(y - 0.5f)` and advances an exact half tie at
`0x311e24d0..0x311e2510`; later edge bounds use `floorf(y + 0.5f)` at
`0x311e2638..0x311e264c`. Initial attributes are computed through binary64 and
rounded to binary32, while skipped and completed rows advance with repeated
binary32 adds at `0x311e2c3c..0x311e2c98`. Recomputing a clipped row directly
would be algebraically similar but could have different low bits.

The bounded root now accepts only textured, axis-aligned rectangles whose x,
u and v coordinates remain integer and whose fractional y span is still an
exact integer height. It reproduces those decoded scan bounds and accumulated
row advances; fractional solid geometry remains refused. The one-column BGRA
sampler was also extended from zero-weight rows to the exact packed vertical
interpolation at `0x3122bc08..0x3122bcb4`. Its wrapping 32-bit lane
subtractions, multiplies, logical shifts and byte extraction are transcribed,
not replaced with an approximate per-channel float blend. The existing
one-column descriptor, root-only memory facade, two-tap fault and alias guards
remain mandatory.

The focused fixture translates a 320-to-1 two-row texture by one quarter pixel
and pins the native 192/256 packed blend. It also pins fractional-solid refusal;
the focused binary passes 24,287 checks. Detached exact commit
`11d253464e4ad4acddca379b5142a084294a3473` then passed all 54 standard tests.
Its fresh RelWithDebInfo emulator SHA-256 was
`DAB92254645B63862FC954643A0E4BEB8B881329E83FE9848B9960716156686D`.

r323 was the complete 3.9--4.4 B native-vs-HLE oracle from the retained clean
checkpoint. It exited zero and stopped exactly at 4,400,000,000 instructions in
388.602 host seconds with:

    nearest BGRX leaf        4,830 attempted / 4,830 exact passes
    nearest BGRA leaf        4,818 attempted / 4,816 exact passes
    sw_scanline             13,333 attempted / 9,277 exact passes
    ogl_poly_scan              167 attempted /   144 exact passes
    total prepared          19,067 / 65,536; 0 failures

Every prepared comparison resolved with zero mismatch or unreadable result.
The run consumed all 26 touch reports, reported zero external-media failures
and 2/2 raw redirects/completions, retained 69 H1 swap-handler calls, and
produced the accepted framebuffer SHA-256
`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`.
GitHub Actions passed iOS build `30709876859` and all eight core-test jobs in
`30709876824` for the exact source commit.

The gain is **three** exact roots over r320's 141, not every fractional root.
The refusal trace explains the gap: the remaining translated 71/203/300/304/320
pixel rows still select `0x3122bad8`, but their descriptors expose multiple
texture columns. The implementation's proved one-column shortcut correctly
declines them at the child scanline. A general multi-column bilinear
transcription is therefore the next renderer case. r323's 388.602 seconds is
verifier overhead, not a replacement timing result or displayed cadence.
**30 fps remains unproven.**

### r324--r325: pixel-aligned wider bilinear rows are exact; timing is mixed

The next implementation is deliberately **not** a general bilinear sampler.
The retained wider r323 rows start u at a texel centre and advance by exactly
one texel per destination pixel. After `0x3122bad8` subtracts half a pixel,
their eight-bit horizontal weight is zero. The replacement accepts only that
case (or two fixed coordinates made identical by clamping); any genuine
horizontal blend still runs Apple.

The wider path preserves the sampler's native top-left, bottom-left,
top-right, bottom-right load order and requires all four four-byte reads to
succeed, even though the zero-weight right pair cannot affect the result. It
retains the root-only facade, non-wrapping fixed-coordinate bounds, current-
chunk source/destination alias refusal, and the exact packed vertical blend.
The old one-column path remains a separate two-read constant-row shortcut.

The focused alternating-texel fixture covers two 320-pixel rows, the 256+64
chunk boundary, four native taps per pixel, and the quarter-pixel 192/256
vertical blend. It records 2,560 scalar texture reads totalling 10,240 bytes
and passes with the complete **27,605 checks / 0 failures**. The strict target
passes too. Detached exact commit
`047094f637f73c83f510ae98ab942cf1d1194b40` passed all 54 tests from a fresh
RelWithDebInfo build; its emulator SHA-256 is
`5558F8FC6038F42B4117A90C4909A27D47975FCEC64CA2DD1050DED313A36336`.
GitHub Actions passed iOS build `30710588193` and all eight core-test jobs in
`30710588199` for that exact source commit.

r324 was the complete 3.9--4.4 B live differential oracle from the retained
clean checkpoint. It exited zero in 348.239 seconds and stopped exactly at
4,400,000,000 instructions with:

    nearest BGRX leaf        4,830 attempted / 4,830 exact passes
    nearest BGRA leaf        4,818 attempted / 4,816 exact passes
    sw_scanline             13,333 attempted / 9,277 exact passes
    ogl_poly_scan              167 attempted /   163 exact passes
    total prepared          19,086 / 65,536; 0 failures

Every prepared result resolved: zero mismatches and zero unreadable cases.
This adds **19** exact roots over r323 without changing any leaf or scanline
count. Of the four remaining root attempts, one declined on a real texture
read failure at `0x04572000`, two were outside the bounded rectangle geometry,
and one clipped to empty. The run accepted and consumed all 26 touch reports,
reported zero external-media failures and 2/2 raw redirects/completions,
retained 45 H1 window updates and 69 swap-handler calls, and produced the same
nonblack framebuffer SHA-256 as r323:
`E0CE0EB1C117527ECDFF2C2C4A4549FCF48AB0F9E151AB7CBE13965849F7CEC8`.

r325 then used the same executable, checkpoint, drag and instruction cap with
`--hle` instead of `--hle-verify`. It exited zero after 4,399,999,637 retired
instructions in **267.587 seconds**:

    ogl_poly_scan              336 hits / 325 handled /  11 declined
    sw_scanline                270 hits /   0 handled / 270 declined
    nearest BGRA leaf           40 hits /  38 handled /   2 declined
    nearest BGRX leaf            0 hits

The coverage signal is strong: relative to r321, 65 more roots were handled,
42 fewer roots declined, and 1,561 fewer child scanlines entered Apple. H1
window-update calls rose from 61 to 65 at the same instruction budget, while
swap-handler calls stayed at 69. Touch remained 26/26 accepted and consumed;
storage again had zero failures and 2/2 raw redirects/completions.

The timing signal is weak and conflicting. r325 was 4.231 seconds (about 1.6%)
faster than r321's 271.818 seconds, but remained 11.615 seconds (about 4.5%)
slower than r311b's 255.972 seconds. Its final framebuffer also changed to
`07C2CD8201AF1B2384775F16227DCA4BB06B4031EFDB399F742FF6F3A0A6D97A`.
Visual inspection shows a complete `Searching...` / time / battery status bar
where r321 captured only the beginning of `Sear`, which is consistent with
additional composite progress, but that interpretation is an inference. A
different endpoint is not itself a speed proof.

Therefore this checkpoint proves substantially broader byte-exact renderer
replacement, **not 30 fps**. The 3.9 B snapshot remains a fast, faithful
iteration oracle; it is not a substitute for final cold-boot acceptance. The
next performance decision needs actual published-frame cadence in the target
iOS app (and ultimately a cold armed/disarmed pair), not another claim based
only on fixed-instruction stopwatch time.

### r338--r356: LCD wake is fixed; the first post-keygen HLE A/B is negative

The apparent power-management deadlock was narrower than the PMU hypotheses
that preceded it. The guest entered the framebuffer driver's power-state
method and waited for AppleMerlotLCD's SPI transaction to finish. SPI0 had no
panel endpoint, so the driver's register `0x15` readiness read never returned
the value that lets the enable/disable sequence complete. Commit `823d4f3`
models that exact two-byte read phase on SPI0 CS1 and preserves an in-flight
phase across snapshots. It does not invent a general LCD register file.

The result is measured in both directions. r339 reached the end of the LCD
disable path and stopped CLCD. r343 reached the enable completion; r344 then
ran without injected input from 7.00 to 7.15 B and ended with CLCD window 0
active at 320x480. r347 continued the same state to 9.35 B and observed the
guest's next ordinary sleep around 9.01 B, ending with CLCD off. The 9.35 B
snapshot is therefore structurally valid but useless as a rendering
checkpoint. A sleeping guest comparing equal to another sleeping guest would
repeat the blank-frame mistake this document already retracts.

Detached exact `823d4f3204be416e99c3bf57a4f3b5bf50659cac` passed all 54
tests. Its RelWithDebInfo `bootkernel.exe` SHA-256 is
`5580A222089DA198B2A6BAC6E57D9C492009A90CFBD17B989E76FDA6D4033562`.
GitHub Actions passed iOS build `30726680450` and all core-test jobs in
`30726680469` for that exact SHA.

The preferred fast post-keygen checkpoint is now r350:

    work/r350-settled-cancel-7360m/post-7360m.bin
      SHA-256 6F793352665908C336572F0303A1A66CF890F8959B951B7645636078041E4458
    work/r350-settled-cancel-7360m/post-7360m.bin.mdimage
      SHA-256 94D0E05B2DEF54AE5C26F6DA4CF8C6FAB68AF3ED4BE8E9C41C5F29596E46B8EF
    work/r350-settled-cancel-7360m/post-7360m.bin.mdstate
      SHA-256 A0BA9C6BB61D6C6BBA9B800337F4606E7528B5BAE8946FE48375342027945BA8

It was created with HLE off from an exact-build restore, exited zero, retained
an active 320x480 CLCD window, captured 283,872/460,800 non-zero RGB bytes, and
reported zero storage failures with 2/2 raw redirects/completions. The actual
per-run image is the normal lock screen. That makes it a faithful and useful
iteration checkpoint, **not a target-app acceptance checkpoint**. Cold boot is
still required for final acceptance.

Two failed harness runs are retained because omitting them would make the next
numbers look cleaner than they are. r351 stopped after the display powered off;
PowerShell converted the final `-F` stderr diagnostic into a wrapper error
before preserving the numeric child exit code, so r351 has no trustworthy exit
status. r353 requested an exact HLE snapshot at 7.64 B, but an HLE return skipped
over that exact retired-instruction value; the harness correctly exited 5 for
an unreached snapshot. Neither run is part of the comparison below.

r354 is a native, HLE-off seed taken at 7.54 B just after the scheduled finger
lift, while the lock-screen slider animation was still publishing. r355 and
r356 restored that same seed and ran the same 100 M-instruction, no-input
window with the same executable, screen capture, call probe, and external-media
setup. They differed behaviorally only by `--hle`; neither wrote an output
snapshot. Both child exit codes were retained and were zero.

| arm | host wall | CABackingStoreUpdate | H1 window-update | H1 swap-handler | end state |
|---|---:|---:|---:|---:|---|
| r355 native | 38.494 s | 59 | 174 | 234 | CLCD active; 273,158/460,800 RGB bytes non-zero |
| r356 HLE | 41.323 s | 63 | 187 | 248 | CLCD off; stale capture correctly refused |

r356 handled all 184 `ogl_poly_scan` entries it saw. It was nevertheless
**2.829 seconds, or about 7.35%, slower** than native and reached the display-off
transition before the same instruction cap. The layer-update gaps also changed:
native recorded 1.522--2.382 M instructions (1.703 M mean), while HLE recorded
0.828--3.365 M (1.056 M mean). That is different guest scheduling/progress, not
an equivalent endpoint that can be divided into a speedup.

These counts are deliberately not labelled frames. `CABackingStoreUpdate` is
per layer; the H1 handler runs at display boundaries; and fixed-instruction
desktop wall time is throughput evidence only. The current replacement has no
post-keygen speed win in this valid pair and does not preserve a comparable end
state under a fixed cap. **30 fps remains unproven.** The next measurement tool
must count changed scanout publications (and timestamp them on host wall time)
before another renderer optimization can be judged as FPS work.

### r360--r361: changed publications are finally counted; this slider is not 30 fps

Commit `f6ef5c70b7e4d4887ab161cae21e23cc6d171e99` adds the missing
`--frame-meter` observer. It follows the app's implemented publication path,
not the tempting desktop proxies: schedule a host check after each 100,000
guest instructions, regularly publish at no more than 30 Hz plus the app's
terminal publication, copy all 614,400 raw scanout bytes, and feed one
little-endian word every 397 bytes into the same sampled signature. That is
1,548 sampled words / 6,192 touched bytes per live publication. Changed
signatures are accumulated in the same at-least-0.5-second windows as the app.

One source comment beside the app counter is wrong and should not be used for
arithmetic: it says 460,800 bytes at up to 60 Hz and roughly 1,160 sampled
words. The executed definitions say 614,400 bytes, 30 Hz, and 1,548 words. The
desktop meter follows the executed code. The signature's error direction is
also pinned by a startup self-check: an unsampled tiny change can be missed,
but the observer cannot invent a changed frame.

This is still a desktop proxy. `bootkernel` has host diagnostic overhead the
app does not, the CPUs differ, and it does not reproduce UIKit's concurrent
CGImage work. Its report states all three limits before printing a number. The
100,000-instruction schedule is checked inside the harness's existing 1/1,024
gate, so an observation can be up to 1,023 guest instructions late; the report
states that quantisation too. A host clock failure or backwards step makes the
run exit nonzero instead of returning a flattering rate.

The clean detached RelWithDebInfo binary for that exact commit has SHA-256
`67F8AC16F9309AF11D686DC66A3740306D76167808C55F642F7F86DFBA4C7074`.
The local suite passed 54/54. GitHub Actions independently passed every core
matrix in run `30729123340` (including sanitizers and warnings-as-errors) and
the ad-hoc iOS IPA build in `30729123349`, both at that exact SHA.

r360 restored the native, HLE-off r354 animation seed at 7.54 B, used that
exact binary, injected no input, and stopped at exactly 7.55 B with exit 0.
The meter covered 10 M guest instructions in 2.330352 host seconds, or 4.291 M
guest instructions/s in this diagnostic harness. All 50 publications found a
live CLCD surface. Six had a changed sampled signature, at deterministic guest
positions from 7,540,200,448 through 7,548,800,000:

| measurement | r360 |
|---|---:|
| changed-publication instruction gap, min / mean / max | 1,200,128 / 1,719,910.4 / 1,999,872 |
| host-time gap, min / mean / max | 0.270964 / 0.401318 / 0.458028 s |
| completed 0.5 s windows | 4 |
| app-style FPS, last / min / mean / max | 1.990 / 1.958 / 2.916 / 3.995 |
| windows at 30 fps or more | 0 |

The window mean includes the app's deliberate first-live-signature count and
is therefore not the reciprocal of the five inter-change gaps. The gap mean
is the cleaner steady cadence for this short sample: at a hypothetical fixed
30 changes/s it corresponds to about **51.6 M guest instructions/s**. Applying
the older target-device report of 25 M/s linearly would project about 14.5
changes/s, but that is an inference from an older key-generation run, not a
current target measurement. Neither number may be labelled iPhone FPS.

r361 is the exact same executable, snapshot, media setup, cap and no-input
window with only `--frame-meter` omitted. Both arms ended at the same PC/CPSR,
the same active CLCD state and frame counter, zero storage failures, and 2/2 raw
redirects/completions. Their per-run framebuffer PPMs are byte-identical at
SHA-256
`0B24F372055080E78C7770A90B45CA8328B275212765B7FB05E465B61B545A51`;
their final 466,825,216-byte work images are byte-identical at
`94D0E05B2DEF54AE5C26F6DA4CF8C6FAB68AF3ED4BE8E9C41C5F29596E46B8EF`.
That proves the observer did not change those checked guest outputs. It does
not prove every byte of machine RAM was equal because this pair did not write
terminal machine snapshots.

The brutal conclusion is narrower and more useful than either earlier story.
This post-keygen lock-slider return really is a changed-frame workload, and on
this desktop harness it is a roughly 2--4 fps workload, not 30. Its observed
instruction cost is about ten times smaller than the old 16.7 M/frame
home-screen-swipe figure, so it does **not** validate or invalidate that other
workload. Final acceptance still requires the counter in the real iOS app on
the target device and a cold armed/disarmed pair. Snapshots remain the right
fast iteration tool; they are not cold-boot acceptance evidence.

### r362--r364: HLE changes guest cadence, but it is still nowhere near 30 fps

r362 and r363 repeated r360's exact r354 restore, no-input window, media setup,
frame meter and 7.55 B cap with the exact `f6ef5c7` binary. The only behavioural
change was `--hle`. Both runs exited zero, handled all 24 `ogl_poly_scan` roots,
kept the CLCD active, ended at the same PC/CPSR and guest frame counter, and
produced the same terminal PPM. More importantly, their changed-publication
positions were guest-deterministic: both recorded nine changes at exactly the
same instruction counts from 7,540,200,448 through 7,549,200,360.

| measurement | r360 native | r362 HLE | r363 HLE repeat |
|---|---:|---:|---:|
| host span | 2.330352 s | 2.774689 s | 2.744383 s |
| guest retired/s | 4.291 M | 3.604 M | 3.644 M |
| changed signatures | 6 | 9 | 9 |
| mean instruction gap | 1,719,910.4 | 1,124,989.0 | 1,124,989.0 |
| mean host-time gap | 0.401318 s | 0.312201 s | 0.308342 s |
| app-style window mean / max | 2.916 / 3.995 | 3.385 / 5.627 | 3.454 / 5.664 |
| windows at least 30 fps | 0 | 0 | 0 |

This is a real guest-progress improvement and a host-throughput regression at
the same time: the HLE arm needs about 22.2% fewer guest instructions between
changed publications, but executes guest instructions about 15% more slowly.
It is not a 30 fps result. The repeat proves the instruction positions, not the
host timings, are deterministic.

r364 then probed the four hottest CoreGraphics leaves in the same HLE window.
`_argb32_sample_ARGB32` was live 267 times; `_argb32_image_mark` and
`_argb32_image_mark_RGB32` were each live nine times, and
`_CGSBlendRGBA8888toRGBA8888` was live eight times. The probe overhead makes
r364's wall time unsuitable for a speed comparison. It does establish that the
sampler is the next live leaf; it does not establish that transcribing its large
multi-path body will be a net win.

One product gap remains separate from those numbers: `tools/ios3_hle.c` is
linked into `bootkernel`, but the iOS app does not compile or drive it. These are
desktop experiments until that integration exists and the target app measures
them. A desktop HLE result must not be reported as an app improvement.

### r365: the MBX submit story was wrong, and the hardware path is still black

r365 paid for the required exact cold run because every retained fast graphics
checkpoint was created on the software-renderer path. It used `--mbx`, kept HLE
off, probed the three addresses previously called kernel submit sites plus
`mbx2DCtxBlitCopy`, enabled the MBX register histogram, and stopped exactly at
5.6 B. The child exited zero after 1,896.241 seconds. Storage reported zero
failures and 2/2 raw redirects/completions.

The result corrects the inherited diagnosis:

| event | count |
|---|---:|
| userspace `mbx2DCtxBlitCopy` | 12 |
| kernel `0xc077eb2c` | 0 |
| kernel `0xc077f374` | 1 |
| kernel `0xc077ffe8` | 2 |
| writes to `0x6d8` | 4, not 3 |
| writes to `0x12c` | 1, value `0x00000400` |

The three kernel addresses are calls to the same `AppleMBX+0xe854` synchronous
startup-transfer helper. That helper programs `0x824/0x828/0x82c/0x838/0x83c`,
writes `0x09000000` to `0x6d8`, polls status bit 6, and acknowledges it. They
are not one call site per userspace blit.

The real userspace path is named and much less speculative, but submission is
buffered rather than one direct call chain:

    mbx2DCtxBlitCopy -> copyDispatchEVT2 -> mbx3DCtxBlitCopy
      -> pack2DCtxBlitCopy -> mbxGetCommandSpace -> mbxSubmitCommand
    shared-buffer flush -> mbxSubmitBuffer_no_lock
      -> IOConnectMethodScalarIScalarO(selector 3)

The kernel command-copy helper is at `0xc077a188` (`+0x2188` from the prelink
executable address, or `+0x1188` from its `__TEXT` address). A byte-level read
corrects the first interpretation of it: **it does not write `0x12c`**. It
reads bits 16..23 of `0x12c`, computes `0x7b - consumer` as the available
space, waits until the packet fits, then copies `length / 4` words into the
mapped EDRAM command ring at `mapped_base + 0xa00000 + software_cursor`. It
advances the software cursor stored at `0xc078b1bc`, wrapping it at 64 KiB.

The one observed write of `0x400` to `0x12c` is instead explicit recovery code
at `0xc077f394..0xc077f3a0`: after testing the driver's `2DIdle` byte at object
offset `0x120`, it injects bit 10 only when that byte is zero. It is not a
producer value and `0x400` is not a byte count. Therefore the claim that twelve
blits were proved to be one 1 KiB producer update is withdrawn. The userspace
shared command buffer can batch work, but this run did not measure how those
twelve API calls were grouped at selector 3, so a submission/completion ratio
cannot honestly be inferred from these counts.

The snapshot does contain one real 64-byte packet at aperture offset
`0xa00000`, with only 29 nonzero bytes in the first MiB of that ring:

    f0000000 00897000 94060500 00800080
    30000000 60800200 8000cccc ffffffff
    00000000 014001e0 70000000 70000000
    70000000 70000000 70000000 70000000

The `_pack2DCtxBlitCopy` disassembly accounts for those fields: `0x94060500`
is the source-format/stride word, `0x30000000` is the zero source coordinate,
`0x60800200` is unity scale, and `0x014001e0` is the 320x480 destination
rectangle. The packet's `0x00800080` source and `0x00897000` destination are
GPU virtual addresses. Treating either as a direct EDRAM aperture offset reads
all zero bytes, as does the previously suspected `0x00a8a000` surface.

The real model gap is now narrower and harder: S5LBox has no MBX command
consumer and no GPU-MMU translation for those addresses. The current model
also couples both bit-6 startup completion and bit-10 2D completion to every
`0x6d8` startup kick. Raising completion on the recovery write or on an
unexecuted packet would let the driver proceed while moving zero pixels; that
would be a fake graphics fix.

The screen is not merely slow. It is black: 384 of 460,800 RGB bytes are
nonzero, destination vram slot 3 contains zero nonzero bytes, and visual
inspection confirms a black frame. Raising a completion on the newly identified
write would make the driver proceed while moving zero pixels. That is not a
graphics fix and must not be presented as one. The next honest implementation
must either execute the submitted command data before completing it, or replace
the named MBX2D blit with a validated native operation and compare its pixels
against the software-renderer result.

r365 also created a separate diagnostic checkpoint; it does not replace the
normal r350/r354 checkpoints and is not acceptance evidence:

    work/r365-cold-mbx-submit-paths-5600m/post-5600m.bin
      SHA-256 E3ACDD25A1A72EB210CBE6CEEADDCE1774A4462A3E444BABFAD0C47F33B6EA28
    work/r365-cold-mbx-submit-paths-5600m/post-5600m.bin.mdimage
      SHA-256 24A24F0A440A7F8AD3F56CED76ED935CA19165E459B7A1194D4D54E286413B40
    work/r365-cold-mbx-submit-paths-5600m/post-5600m.bin.mdstate
      SHA-256 172A1C351583EA6D93C19D57A771670E10047CB79AEFA3C54FD7AC586076C82F

### r366: the first packet has a real GART-backed consumer, but cold proof is pending

The GPU addresses are no longer opaque. `AppleMBXMMU::map` at
`0xc0783570..0xc0783650` writes each physical page base into a table selected
by the top of the GPU address. Its index arithmetic is:

    chunk    = gpu_va >> 22
    table_pa = MBX[0x1000 + chunk*4]
    pte_pa   = table_pa + (((gpu_va >> 12) & 0x3ff) * 4)
    phys     = PTE[pte_pa] + (gpu_va & 0xfff)

That is eight 4 KiB roots, one per 4 MiB GPU-VA chunk, with 1024 raw physical
page bases in each root. It is not an inferred page-table shape: the kext's
store at `0xc0783650` writes the physical address at exactly that computed
index.

Applying it to r365 resolves the packet rather than merely renaming it. GPU
source `0x00800080` uses root register `0x1008 = 0x0d9ca000`, PTE 0
`0x0e283000`, and begins at physical `0x0e283080`. GPU destination
`0x00897000` uses PTE 0x97 `0x08a1e000`. Across the packet's 0x96000-byte
320x480 surface, the source has **311,918 nonzero bytes** and all 256 byte
values; the destination has **zero**. The source is real image data and the
destination is exactly vram slot 3, not an EDRAM-offset coincidence.

Commit `39b96ae` implements only the format that evidence supports:

- header `0xf0000000`;
- BGRA8 source descriptor `0x9406xxxx` with measured 0x500-byte stride;
- source coordinates packed as `_pack2DCtxBlitCopy` does;
- exact unity scale `0x60800200` and simple-copy mode `0x8000cccc`;
- a destination rectangle within the measured 320x480 surfaces;
- all six `0x70000000` terminators.

AppleMBX's helper stores words sequentially, so the sixth terminator is the
first point at which all sixteen words are known complete. At that store the
model validates every source and destination PTE and every translated target
as plain DRAM, stages the whole rectangle, commits destination words through
the observer-aware machine bus, and only then raises status bit 10. A missing
late PTE therefore changes zero destination bytes. Unknown formats, strides,
scales, blends, geometry, unaligned entries and non-DRAM targets receive no
pixels and no completion. Register `0x6d8` now raises only its measured
synchronous startup bit 6; it no longer fabricates a 2D completion.

Fast validation is strong but not acceptance:

| check | result |
|---|---|
| focused machine/MBX tests | 2/2 pass |
| full local suite | 55/55 pass |
| strict MinGW compile | `-Werror -pedantic -Wshadow`, pass |
| r365 snapshot replay | executed 1; destination nonzero 0 -> 311,918; byte mismatches 0; status 0x400 |
| exact-SHA hosted core | run 30733981870 green, including strict + ASan/UBSan + Linux/macOS/Windows |
| exact-SHA iOS build | run 30733981862 green |

The replay uses the real cold snapshot's packet, registers, PTEs and pixels,
so it proves the copy engine against real machine state. It does **not** prove
live submission timing, interrupt servicing, later packet formats, a visible
screen, animation correctness, or frame rate. A fresh cold run is required
before calling the black-screen defect fixed; that r366 run is intentionally
being captured separately instead of laundering a post-recovery snapshot into
acceptance evidence.

### r367-r370: live 2D completion is proved; the display is still black

The last paragraph above is now superseded by a cold run and two checkpoint
continuations. The conclusion is narrower than hoped: the one decoded 2D copy
works live, but it copies only the base wallpaper. A separate tiled 3D render
never executes, so the black-screen bug is **not fixed**.

r366 cold-booted the `39b96ae` consumer to 3.3 B instructions. The complete
packet was present in the terminal snapshot, yet the model counted zero
candidates and destination slot 3 stayed zero. r367 continued that state to
5.6 B, but the ring was not written again. The failed trigger hypothesis was
the sixth terminator: it is sufficient for a synthetic sequential unit test,
not the boundary used by the live submitter.

Commit `9f0b42a` traced the ring writes, and r368 measured the exact order:

    ring+0x00 = a0060500       relocation token
    ring+0x04..0x3c           the remaining fifteen packet words
    ring+0x00 = f0000000       final header rewrite

Commit `4a4cabc` therefore executes only the measured
`0xa0060500 -> 0xf0000000` word-zero transition after validating the complete
body. r369 resumed the r368 pre-submit checkpoint and produced:

- one candidate, one completion, zero rejections, 614,400 bytes copied;
- destination slot 3 changed from zero to 311,918 nonzero bytes;
- the destination exactly matched the source across all 614,400 bytes;
- status bit `0x400` became visible, AppleMBX entered its ISR, stored
  `2DIdle=1`, and acknowledged the bit.

That is live proof for this one packet format, not a generic PowerVR claim.
The CLCD output remained visually black: only 384 of 460,800 RGB bytes were
nonzero. Direct inspection of the copied BGRA surface shows the black lock
screen wallpaper and Earth image, but no status bar, clock, date, or slider.
Copying it correctly is necessary and visibly insufficient.

The same r369 state contains a second, real render submission. AppleMBX writes
`1` to `0x680` (`STARTRENDER`) after programming:

    RGNBASE=00001000  OBJBASE=00014000
    FBSTART=00897000  FBLINESTRIDE=00000140
    FBXCLIP=01400000  FBYCLIP=00800010

Both region and object bases are GART virtual addresses, not EDRAM-relative
offsets. The region stream has 40 columns by 7 rows of tiles covering
`x=0..320, y=16..128`; every tile points at object list `0x14068`, whose words
are `60200020 6020002d 61a0007c f0000000`. The referenced object records
contain coherent screen-space rectangles and texture/colour data. This is not
an idle register sequence or another plain copy. It is the next renderer that
must be decoded and executed.

r370 continued the post-copy state to 5.6 B. No second 2D packet appeared and
the copied destination stayed intact. AppleMBX instead reported:

    2DIdle=1, 3DIdle=0, 3dblit=1
    CompletedIntStatus=00000000

That moves the measured stall from 2D to 3D; it does not move the screen.

Two public PowerVR MBX register headers remove ambiguity from the interrupt
and register names. `mbx1defs.h` names bit `0x04` RENDER_COMPLETE, `0x08` ISP,
`0x10` TA_COMPLETE, `0x40` EVM_DALLOC, and `0x400` 2DSYNC. The companion
register map names `0x680` STARTRENDER and `0x6d8` BACKGROUND_TAG. These names
match AppleMBX's code: its submit path clears `3DIdle` then writes STARTRENDER;
its ISR sets `3DIdle` only after ISP, RENDER_COMPLETE, and EVM_DALLOC have all
been observed. Sources:

- [PowerVR MBX interrupt definitions](https://github.com/fergy/iPhone_kernel_26/blob/3841858a6bafa3f6a6cc41fd7c114864b31b64a8/drivers/gpu/mbx/mbx1defs.h)
- [PowerVR MBX register offsets](https://github.com/R0-Developers/YP-R0_Kernel/blob/5beb98d00ae08e758a382b39e31ef3cabb463d26/linux-2.6.24-2.4.2-base/drivers/tinywmr/tinywhimory/mx37_registers.h)

r370 also exposed an independent register-model defect. AppleMBX recovery
writes missing `0x08`, `0x04`, and `0x40` events directly to status `0x12c`,
then reads the accumulated word and acknowledges it through `0x134`. The old
model stored those writes in `reg[]` while status reads came from a separate
field, so the guest read zero. This source checkpoint models `0x12c` as
write-one-to-set and `0x134` as write-one-to-clear, with mask-gated IRQ tests.
That is accurate recovery behaviour; it is **not** a substitute for executing
the render and cannot be counted as a graphics fix.

The r368 pre-submit trio is the fastest accurate checkpoint for iterating on
the first live STARTRENDER without paying for another cold boot:

    work/r368-cold-mbx-ring-order-3300m/pre-2900m.bin
      SHA-256 9AC91432DB59E911E5885B65574AE76A596B2D9402C43AB91A4DE0F42F8D9EFC
    work/r368-cold-mbx-ring-order-3300m/pre-2900m.bin.mdimage
      SHA-256 EC8675922B60D36C38FA2DF6FA56096477B8EB53A5AE309C04BECD3C5D2AD9DE
    work/r368-cold-mbx-ring-order-3300m/pre-2900m.bin.mdstate
      SHA-256 CFD31092C2954C4BD230E2A7D4FA7EBAE3A74EF433254B5B056D5828E2DB22D2

The r369 post-copy trio is useful for recovery/late-stall inspection:

    work/r369-resume-reloc-trigger-3300m/post-3300m.bin
      SHA-256 41E8484F6201F27C3B7218809FCAC91E16377E65C31C27DB758A5616BDCF53D0
    work/r369-resume-reloc-trigger-3300m/post-3300m.bin.mdimage
      SHA-256 5895ADD3ECFD61F594DB396A7734D2637894C00C1C9E111F92270051698400F2
    work/r369-resume-reloc-trigger-3300m/post-3300m.bin.mdstate
      SHA-256 9E04A3550B0EE0A0E7972D01842599FC5506A3D9E90A61E25031F64AEC08F611

Both are diagnostic checkpoints. They make iteration faster without weakening
the evidence, because the relevant pre-submit/post-copy machine and external
media state is serialized. They do not replace the final cold boot, real iOS
app validation, or a measured 30 FPS acceptance run.

Local verification for the status-register checkpoint is 40/40 focused MBX
assertions, 55/55 full tests, and a clean warnings-as-errors build. Hosted
exact-SHA results must still be checked after the commit is pushed; local green
is not being substituted for CI.

### r371-r374: two tiled renders work, but the hardware display is still black

The earlier r368 interpretation of the 2D submit boundary was wrong. It looked as if
AppleMBX rewrote each packet head from `0xa0060500` to `0xf0000000` because the first
packet happened to begin at ring offset zero. r372 observed a second command beginning
at `+0x40` followed by the same `0xf0000000` store to **ring offset zero**. Static reading
of AppleMBX's helper at `+0x1f58` agrees: this is a fixed submit/doorbell write, not a
relocation of the current packet head. The old per-packet-rewrite claim is withdrawn.

The corrected consumer remembers the one measured `0xa0060500` command head while its
body is copied and executes it only when the fixed ring-zero submit arrives. More than
one pending head fails closed because no live batch has established that case. It also
implements the measured 18-word premultiplied 2D copy used by the clock, date, slider,
and related lock-screen layers. Source and destination mappings are prevalidated, source
pixels must satisfy premultiplied BGRA8, pixels are staged, and completion is raised only
after commit. r373 then measured nine candidates, nine completions, zero rejections, and
1,007,428 committed bytes: the base simple copy plus eight blended packets.

The first tiled 3D consumer validates the complete captured 40-by-7 region stream, its
three-object list, all object words, the two GART resources, framebuffer registers, and
the 320-by-96 geometry before applying the recovered fixed-point source-over equation.
r371 and later replays completed 30,720 pixels live. This is an exact captured form, not
a general PowerVR implementation.

The next `_mbx3DCtxQuadCopyPerspective` record and its final live object stream identify
a 10-by-20 premultiplied padlock at destination `x=155..165, y=0..20`. The model likewise
checks all four tiles, all three objects, address-control bits, 0x40-byte source stride,
coordinates, and target before blending. Offline r373 replay changed 108 visible pixels
(324 bytes), changed nothing outside the rectangle, and changed the target FNV-1a hash
from `56c627a0e4adae82` to `e912fb3ab60f85a5`. r374 completed the same 200-pixel render
live.

r374 is progress, not a screen fix:

| measured result | r374 |
|---|---:|
| 2D candidates / completed / rejected | 9 / 9 / 0 |
| 2D bytes committed | 1,007,428 |
| 3D candidates / completed / rejected | 4 / 2 / 2 |
| 3D pixels blended | 30,920 |
| process exit | 0 |
| AppleMBX recovery | one event; `2DIdle=1`, `3DIdle=0` |
| recovery `CompletedIntStatus` | `0x0000000c` |

Directly dumping GPU target `0x00897000` now produces a coherent lock screen with its
padlock. The actual CLCD scanout is still black. A briefly generated "cursed" diagnostic
PNG was the source texture interpreted with an exploratory tiled layout; the captured
status textures are linear and that PNG was never evidence of guest-screen corruption.

The two rejected renders are no longer opaque. Their retained command records and GART
resources decode linearly as:

- a 76-by-16 `Searching...` sprite, source `0x00995080`, 0x140-byte stride, destination
  `x=4..80, y=1..17`;
- a 21-by-20 battery-outline sprite, source `0x00997000`, 0x60-byte stride, destination
  `x=296..317, y=0..20`.

Those labels are visual observations of the extracted source pixels. Their object streams
have not both been captured and validated yet, so completing them speculatively would be
the same fake-completion mistake this work is intended to remove. The next task is to
capture/validate both exact object forms and rerun from the trusted r368 pre-submit
checkpoint. Final graphics and FPS acceptance still require a cold boot; no FPS result is
claimed from r374.

The r374 diagnostic snapshot is reproducible but is not a final-acceptance checkpoint:

    work/r374-resume-second-mbx3d-3500m/post-3500m.bin
      SHA-256 A0E3E4D33093B4FD9879AF6A2C9202E65D62E42CB32A7E5339F3ECFD8E2C41BF
    work/r374-resume-second-mbx3d-3500m/post-3500m.bin.mdimage
      SHA-256 E059FA6A616CC536FB9F9BDF52FE37712CCDDD09455DEFFAD643969BDA67AD4B
    work/r374-resume-second-mbx3d-3500m/post-3500m.bin.mdstate
      SHA-256 9E04A3550B0EE0A0E7972D01842599FC5506A3D9E90A61E25031F64AEC08F611

Local verification for this checkpoint is 63/63 focused MBX assertions, 55/55 full
tests, a clean strict `-Werror -pedantic -Wshadow` build, the exact offline padlock
replay above, and the live r374 run. Exact source SHA
`259cf7f8534915b79984bcad29f20ccf93dfb421` is also green in hosted core Actions run
`30741441156` (all matrices, including strict and sanitizers) and iOS Actions run
`30741441167` (ad-hoc-signed IPA).

### r375-r376: the first real hardware-path lock-screen frame is visible

r375 used a temporary trace, removed immediately after the run, to preserve the two
object streams that r374's following render overwrote. It proved the exact `Searching...`
tile grid is `x=0..9, y=0..1` with rectangle `x=4..80, y=1..17`, and independently
confirmed the battery grid is `x=37..39, y=0..1` with rectangle
`x=296..317, y=0..20`. Every object word through `+0x29c` was captured for both.

The renderer now recognizes exactly three status-sprite descriptors: padlock,
`Searching...`, and battery. Selection requires an exact framebuffer clip; each form
then requires its exact tile grid, common three-object list, boundary object, 44-word
textured quad, source-control bits, target address, dimensions, and stride. GART spans
are validated and source pixels must be premultiplied BGRA8 before staged source-over
pixels commit. This is still a three-form decoder, not a general MBX renderer.

Offline replay of r375's retained final battery object executed successfully, raised
status `0x4c`, changed 138 pixels / 414 bytes inside `x=296..317, y=0..20`, changed zero
pixels outside, and changed the full-target FNV-1a hash
`e912fb3ab60f85a5 -> f6ccaf1fec9d17f5`.

r376 resumed the trusted cold-derived r368 pre-2.9 B checkpoint and reached 3.5 B in
185.342 seconds, exit zero. The first lock-screen sequence completed all nine 2D packets
and all four known 3D renders. The four 3D pixel counts were exactly 30,720, 200, 1,216,
and 420. That allowed the guest to program CLCD with a real, nonblack frame for the first
time on this hardware path. Visual inspection shows the Earth wallpaper and clean status
bar with `Searching...`, padlock, and battery. The PPM contains 69,894 nonblack pixels
(206,453 nonzero RGB bytes of 460,800):

    work/r376-resume-all-status-3500m/w.img.screen.ppm
      SHA-256 7D810155C44E303932FF5672FB67B0EE57C2345E1480B5EDE2AFBDB72ED02CF7
    work/r376-resume-all-status-3500m/scanout.png
      SHA-256 868C24F0A38242F94325F62D3115F56694C458C87C27F396B25510DD64FEEE00

That is a visible-screen breakthrough, not a completed graphics fix. The frame has no
clock, date, or slider. Once the first sequence completed, r376 exposed later work that
the deliberately narrow model rejects:

- eight contiguous 18-word blended commands at ring `+0x02c0..+0x04ff` arrived under
  one fixed ring-zero submit;
- four contiguous 16-word simple-copy commands at `+0x0500..+0x05ff` likewise arrived
  under one submit;
- a ninth STARTRENDER did not match any of the four decoded forms.

The previous single-pending-head model correctly failed closed on both batches instead
of executing only one command. Its terminal `12/10/2` 2D counters count fixed-submit
candidates, not the 22 copied command heads; that metric must be corrected with the batch
model. 3D ended `9/8/1`, 65,112 pixels blended. Two guest recovery events followed, both
with `2DIdle=0`, `3DIdle=1`, and `CompletedIntStatus=0x00000400`. The immediate next task
is an ordered, rejection-atomic multi-command 2D submit; only then should the ninth 3D
object be captured. No FPS result is claimed, and final acceptance still requires a cold
boot plus measured publication cadence.

r376 is a fast diagnostic checkpoint, not cold acceptance:

    work/r376-resume-all-status-3500m/post-3500m.bin
      SHA-256 CEAC45BA47CCC68BAF180BF02F9B44CE88222395CFFB8348B1A7BDAC4AA96E8E
    work/r376-resume-all-status-3500m/post-3500m.bin.mdimage
      SHA-256 364D2C8BF2BBA996D7E6C09D087DFF76E7EACDEBE59F857E2905235C5B0FC6C6
    work/r376-resume-all-status-3500m/post-3500m.bin.mdstate
      SHA-256 9E04A3550B0EE0A0E7972D01842599FC5506A3D9E90A61E25031F64AEC08F611

Local verification is 75/75 focused MBX assertions, 55/55 full tests, a clean strict
warnings-as-errors build, exact battery replay, and the live r376 evidence above. Exact
source SHA `1b42a7b02f31dd7013055a65448c0c0365aa12f4` is green in hosted core run
`30742470038` (all matrices, including strict and sanitizers) and iOS run `30742469992`
(ad-hoc-signed IPA).

### r377-r378: a complete lock-screen frame, with two 3D rejects still live

The 2D consumer now executes each measured multi-command submit as one ordered,
rejection-atomic transaction. It first parses and validates every contiguous command,
evaluates overlapping copies and blends against a staged destination shadow in packet
order, and commits no guest bytes until the entire batch is known. Source aliases of the
destination remain rejected because no live submission has established their semantics.
The pending snapshot marker retains the first ring word plus the exact command count;
the previous v32 single-head marker is decoded compatibly, while its countless
multiple-head state still fails closed. Metrics now count command packets rather than
fixed ring-zero doorbells.

r377 resumed the cold-derived r368 pre-2.9 B checkpoint to 3.5 B in 174.664 seconds and
exited zero. Both batches completed: eight blended commands committed 393,028 bytes and
four simple copies committed 142,152 bytes. Terminal 2D counters were `22/22/0` and
2,157,008 committed bytes. The guest consequently published a coherent 320-by-480 lock
screen containing the status bar, `4:00`, `Wednesday, December 31`, Earth wallpaper,
padlock, and `slide to unlock`. The real CLCD output contains 92,145 nonblack pixels and
273,206 nonzero RGB bytes:

    work/r377-resume-atomic-batches-3500m/w.img.screen.ppm
      SHA-256 5B5946B85048FB7D1ECC63C238BB26C8D65B07989942C8ABB4C9EFE14F4F3A34
    work/r377-resume-atomic-batches-3500m/scanout.png
      SHA-256 C3BB8CD5CA8A78D0D23A990AC8047CFBBB0A3167F58E079066F556755EDA9966

This is a complete visible frame, not a completed graphics implementation. `Searching...`
is the guest's current cellular state; the displayed date/time is the guest's current
clock state. More importantly, r377 still measured 3D `10/8/2`, 65,112 blended pixels,
and one Graphics Recovery Event with `2DIdle=1`, `3DIdle=0`, `3dblit=1`, and
`CompletedIntStatus=0x0000000c`. No publication cadence or FPS was measured.

The next retained submission reused the first background quad and texture but narrowed
the dirty region to tiles `x=1..38, y=6`, boundary `x=8..312, y=97..109`, and source rows
77..88. The renderer now recognizes that literal second form and blends only its 304-by-12
rectangle. Focused tests prove its source-row offset, exact boundary, outside preservation,
and rejection of a one-word boundary mutation. This is another exact captured form, not
a generic tiled rasterizer.

r378 replayed the same trusted checkpoint to 3.5 B in 176.908 seconds. The clipped form
completed 3,648 pixels live and allowed a newly exposed eight-command blended 2D batch at
ring `+0x0600..+0x083f` to complete 260,200 bytes. Terminal counters became 2D `30/30/0`,
2,417,208 bytes, and 3D `11/9/2`, 68,760 pixels. Its PPM is byte-identical to r377, which
shows that the odd-looking state is repeatable guest output rather than random host image
corruption. It does not show that the remaining GPU work is irrelevant: one unknown 3D
form precedes the clipped background, a second follows the new 2D batch, and the same
single recovery event remains.

The final r378 snapshot retains the later rejection. Its registers and object stream show
an exact clipped padlock update: clip `x=152..168, y=16..32`, two tiles `x=19..20, y=1`,
and geometry `x=155..165, y=16..20`. That measured description is not yet an implemented
claim. The earlier rejected form was overwritten by the following background command and
must be captured during a temporary trace replay. A snapshot replay is appropriate for
that decoding work; final graphics/FPS acceptance still requires a cold boot.

r377 diagnostic checkpoint:

    post-3500m.bin
      SHA-256 1FF401399BB024EE41904A4D6E7C6BCE3F93C8759E33E28A81D95AAF91DD64FE
    post-3500m.bin.mdimage
      SHA-256 812577CF60542D49476D82FEEF351C9BC16599D75212C7F8D4782C432D95F173
    post-3500m.bin.mdstate
      SHA-256 9E04A3550B0EE0A0E7972D01842599FC5506A3D9E90A61E25031F64AEC08F611

r378 diagnostic checkpoint:

    post-3500m.bin
      SHA-256 BB24F64166ED9EC53BF6A3F373924E26E6AE32CA5CE81B6C0D96DCD76FCFF812
    post-3500m.bin.mdimage
      SHA-256 812577CF60542D49476D82FEEF351C9BC16599D75212C7F8D4782C432D95F173
    post-3500m.bin.mdstate
      SHA-256 9E04A3550B0EE0A0E7972D01842599FC5506A3D9E90A61E25031F64AEC08F611

The atomic-batch source checkpoint is
`75f2b0d28ea6da4eb4972cff54031a2049aed8ad`; exact-SHA core run `30743393592`
and iOS run `30743393574` are green. The clipped-background changes described above
remain a separate checkpoint until their own local and hosted verification completes.

### r379-r380: both retained rejects clear; one newly exposed form remains

r379 repeated the trusted r368-to-3.5 B replay with a temporary read-only rejection dump.
The dump read registers and GART-backed command memory without changing guest state and
was removed before the capture was interpreted. The run reproduced r378's counters and
recovery exactly, so the instrument did not alter the observed submission sequence.

The previously overwritten render is another exact dirty rectangle over the recovered
background quad and source: clip `x=0..320, y=16..112`, tiles `x=0..39, y=1..6`,
boundary/write rectangle `x=0..320, y=20..97`, and source rows 0..76. The retained final
r378 render is a clipped padlock tail: clip `x=152..168, y=16..32`, tiles
`x=19..20, y=1`, write rectangle `x=155..165, y=16..20`, and source rows 16..19. Both
implementations require their literal clips, tile streams, boundaries, complete object
words, addresses, and source offsets. They do not broaden the renderer beyond the two
captured forms.

Focused tests now contain 96 assertions. They prove the first form writes exactly 24,640
pixels and the second exactly 40 pixels, use the measured source rows, preserve adjacent
pixels, raise completion only after commit, and reject mutated object words. All 96 pass;
all 55 repository tests pass; the `-Wall -Wextra -Werror` build passes; and direct
`-pedantic -Wformat=2 -Wshadow` compilation of both implementation and test passes.

r380 then resumed r368's cold-derived pre-2.9 B checkpoint to 3.5 B in 167.375 seconds,
exit zero. Both former rejections completed live at exactly 24,640 and 40 pixels. The
guest advanced through three more 1,216-pixel `Searching...` redraws and additional 2D
work before stopping at one newly exposed unknown 3D form:

| measured result | r380 |
|---|---:|
| 2D packets completed / rejected | 42 / 0 |
| 2D bytes committed | 2,553,192 |
| 3D renders completed / rejected | 14 / 1 |
| 3D pixels blended | 97,088 |
| process exit | 0 |
| AppleMBX recovery | one event; `2DIdle=1`, `3DIdle=0`, `3dblit=1` |
| recovery `CompletedIntStatus` | `0x0000000c` |

The CLCD frame remains visually coherent and has the same 92,145 nonblack pixels and
273,206 nonzero RGB bytes as r377. It is not byte-identical: 347 pixels changed, all in
the slider-label bounding box `x=114..272, y=422..440`. That localized guest update is
not random full-screen PNG corruption. It also is not an FPS result.

    work/r380-resume-all-current-3d-3500m/w.img.screen.ppm
      SHA-256 F61FA4ABD7C246B10D7A04B42167DFA0DD73E333B00227B7B2A01EFCC80766DB
    work/r380-resume-all-current-3d-3500m/scanout.png
      SHA-256 C72433BB87A2D17C045C8005BFA19A3556CDE2C2E09D6FCBFB79DED0D2C80E2B

r380's final snapshot is a valid next diagnostic checkpoint:

    post-3500m.bin
      SHA-256 385E1F1B50405AC8FE487489448F0C505E1EC0C3B03DD84B16F42AF57A6CF568
    post-3500m.bin.mdimage
      SHA-256 AD846CB58DFF21718B7CABDBB94788C82936CC38C05496C5D69E8E569FD08515
    post-3500m.bin.mdstate
      SHA-256 9E04A3550B0EE0A0E7972D01842599FC5506A3D9E90A61E25031F64AEC08F611

The single rejection happened before two later known `Searching...` submissions, so its
object stream was overwritten by the time the 3.5 B snapshot was saved. The final
snapshot therefore cannot honestly identify it. The next step is one more temporary
rejection-dump replay, followed by removal of that instrumentation. Graphics are still
not fixed; cold-boot and publication-cadence acceptance remain pending.

### r381-r382: the 3.5 B checkpoint path is rejection- and recovery-free

r381 used the same temporary read-only dump procedure for r380's single overwritten
rejection, then removed it and verified tracked MBX source was clean. The captured form
is the battery analogue of the padlock tail: framebuffer clip
`x=296..320, y=16..32`, tiles `x=37..39, y=1`, destination rectangle
`x=296..317, y=16..20`, and source rows 16..19 from the existing battery surface at
GPU `0x00997000`. Its complete object differs from the full 21-by-20 battery form in the
clip, one tile row, lower boundary/geometry, and vertical source coordinates.

The renderer now recognizes only that literal 21-by-4 descriptor. The shared status-form
path validates its clip, three tiles, boundary, all non-address object words, source and
target controls, GART spans, premultiplied pixels, and source-row offset before staging
and committing 84 pixels. The focused suite is 102/102; all 55 repository tests pass;
the warnings-as-errors build passes; and direct `-pedantic -Wformat=2 -Wshadow`
compilation of implementation and tests passes.

r382 resumed the trusted cold-derived r368 pre-2.9 B checkpoint to 3.5 B in 190.642
seconds and exited zero. This time every observed hardware-path operation completed:

| measured result | r382 |
|---|---:|
| 2D packets completed / rejected | 84 / 0 |
| 2D bytes committed | 3,364,632 |
| 3D renders completed / rejected | 15 / 0 |
| 3D pixels blended | 97,172 |
| `Graphics Recovery Event` lines | 0 |
| process exit | 0 |

The formerly rejected battery tail completed exactly 84 pixels. There is no recovery
record to reinterpret: `Graphics Recovery Event`, `2DIdle`, and recovery
`CompletedIntStatus` are absent from the r382 guest log. This clears the measured
checkpoint recovery boundary through 3.5 B retired instructions.

The visible CLCD frame is byte-identical to r380, remains coherent, and still contains
92,145 nonblack pixels / 273,206 nonzero RGB bytes:

    work/r382-resume-clipped-battery-3500m/w.img.screen.ppm
      SHA-256 F61FA4ABD7C246B10D7A04B42167DFA0DD73E333B00227B7B2A01EFCC80766DB
    work/r382-resume-clipped-battery-3500m/scanout.png
      SHA-256 C72433BB87A2D17C045C8005BFA19A3556CDE2C2E09D6FCBFB79DED0D2C80E2B

r382's post-run checkpoint preserves the new no-recovery guest path:

    post-3500m.bin
      SHA-256 95DBA9359A8CEE989981CAF0763257D6C4F3F95BDF1824E629B557972A405209
    post-3500m.bin.mdimage
      SHA-256 B22766BD9F123AB41C8FEDADACF1F79CCF977FB7F71FD90384A2C085ED896AC4
    post-3500m.bin.mdstate
      SHA-256 9E04A3550B0EE0A0E7972D01842599FC5506A3D9E90A61E25031F64AEC08F611

This is the strongest graphics result so far, but it is still not the project's final
graphics or performance acceptance. r382 restored a cold-derived checkpoint, so a new
instruction-zero cold boot must reproduce the rejection-free path. It also measured
completed packets/renders, not guest frame-publication cadence; no FPS number, especially
no 30 FPS claim, follows from these counters. The next phase is cold acceptance plus an
explicit cadence measurement, after this source checkpoint is pushed and exact-SHA CI is
green.

### r383-r397: the unlock transition advances, but it is not finished

The rejection-free statement above was true only through r382's 3.5 B frontier. Extending
the exact full drag (`57,431 -> 305,431`, 24 steps) past that point exposed more command
forms. Fast iterations below restore a checkpoint derived from the instruction-zero cold
run, preserve its exact external-media sidecars, and then execute only a retained command
whose complete stream was already in the snapshot. That is valid diagnostic replay; it is
not a substitute for the still-pending final cold acceptance.

r392 established a second complete 44-word stream for the clipped transition quad. The
old and new streams differ in two setup words and all four vertex-alpha words. They are
accepted as two whole alternatives; a mixture is rejected. The `0xbf000000` words are
per-vertex alpha, not Z values -- the first interpretation was wrong and was corrected
before implementation. r393 then retained a full-width 320-by-20 status-time sprite on
GPU target `0x00897000`; r394 retained the clipped 21-by-4 transparent battery transfer
onto new target `0x00a41000`. Each form requires its literal target, clip, tiles, list,
boundary, quad, source control, and GART spans. r393 accidentally ran without
`S5LBOX_MBX_TRACE=1`, so it has no live counter claim; its retained command completed in
offline replay and the following traced run consumed the resulting checkpoint.

The screen during this work was sometimes visually incomplete, but not random host PNG
corruption. r392 was a coherent lock screen. r394 deliberately cleared the Earth layer
and left the lower surface black after the next command was rejected. Calling that frame
"fixed" or "home" would be false.

The later traced runs form a strict expose-decode-replay chain:

| run | absolute stop | wall time | 2D candidate/completed/rejected | 3D candidate/completed/rejected | result at the stop |
|---|---:|---:|---:|---:|---|
| r394 | 4.8 B | 64.273 s | 1 / 1 / 0 | 3 / 2 / 1 | coherent partial transition; one retained battery transfer |
| r395 | 5.0 B | 64.542 s | 1 / 0 / 1 | 0 / 0 / 0 | new opaque global-alpha 2D copy rejected |
| r396 | 5.2 B | 61.292 s | 7 / 7 / 0 | 2 / 1 / 1 | global-alpha form is live; new status-time target rejected |
| r397 | 5.4 B | 61.279 s | 2 / 0 / 2 | 2 / 2 / 0 | status target is live; two-command split clear rejected atomically |

r395's 18-word packet uses equation `0x0d5f8000`, global alpha 248, source
`0x00800080`, and target `0x00a41000`. This was not generalized from its visual result.
`CA::RenderMBX2D::set_tex_blend_mode` at `0x3123a968` has a literal branch that passes
factors `0x00500000` and `0x0d000000` plus a variable byte to
`_mbx2DSetBlendEquation`; `_mbx2DCtxSetBlendEquation` stores those factors with the byte
shifted by 12. The retained 320-by-480 source independently contains zero non-opaque
pixels and zero premultiplication violations. The implementation therefore accepts only
those factor bits, requires every source pixel to be opaque BGRA8, modulates every channel
with the already oracle-checked `(component + 1) * alpha >> 8` rule, and then performs
premultiplied source-over. A non-opaque source or altered factor remains a rejection.

Offline replay of the untouched r395 packet completed one command, committed 588,800
bytes, raised `0x400`, and produced a clean Earth-on-black destination. r396 then proved
that result live: all seven subsequent 2D commands completed. Its final 3D rejection was
not new geometry at all; it was the already captured full-width status-time object on the
third exact target, `0x00a41000`. The source at `0x00a3a080` contains 378 nonzero,
premultiplied pixels spelling `4:00 PM`. Offline replay completed 6,400 pixels and changed
exactly those 378 RGB pixels, all inside rows 0..19. The resulting off-screen surface is a
coherent complete lock screen -- status bar, clock, date, Earth, and slider.

r397 consumed that completion and rendered both following 3D objects without rejection.
Its visible CLCD frame is likewise a coherent lock screen, not the home screen:

    work/r397-derived-resume-5400m/w.img.screen.ppm
      SHA-256 A0C40DBF678F3EDA5F8FBFAC39E79B42E8A31E4EAB5C8139655C37E95365D158
    work/r397-derived-resume-5400m/screen.png
      SHA-256 04CCF7ED9A4A2850AEB3C21B21AF6C7E517CDED9673E088163694F150B99230B

The next submit divided the known opaque-black clear into two literal rectangles on
target `0x00998000`: rows 20..388 and 389..479. Both arrived under one doorbell. The
model now accepts those two rectangles plus the previously captured unsplit 20..479
rectangle, while preserving ordered all-or-nothing batch staging. Offline replay completed
both packets, committed 588,800 bytes, left rows 0..19 intact, and made every RGB byte in
rows 20..479 zero. Its latest derived diagnostic checkpoint is:

    work/r397-derived-complete-split-fill-5400m/post-fill-5400m.bin
      SHA-256 7A8AE5B2884FD9BAC3676869721DFB897BAC94517C1B333C69E20C77FB64B23C
    post-fill-5400m.bin.mdimage
      SHA-256 F26E566043AB20E1EB6BFF2673B319147E4556C0F9F7E88E6CCAD7698768C3EE
    post-fill-5400m.bin.mdstate
      SHA-256 C2BD6D07AA799BF6B3B3E2E82BA5630B884667086E0839E40F4D79BCCD7AD669

That final split-clear implementation has offline exact-command evidence but has not yet
been consumed by a resumed guest at this checkpoint. The current local verification is
251/251 focused MBX assertions, 55/55 CTest targets, and 251/251 again in the strict
warnings-as-errors build. Graphics are still not complete: r397 contains one recovery
caused by the now-decoded split clear, no run in this chain has displayed the home screen,
no rejection-free instruction-zero cold boot has reproduced the extended path, and no
frame-publication cadence or 30 FPS result has been measured.

### r398-r407: the guest reaches a tutorial-free home screen; acceptance is still pending

r398 consumed the split clear live and advanced from 5.4 B to 5.6 B. Its two-dimensional
work was clean (`52 / 52 / 0`, 2,641,728 bytes), but its final three-dimensional battery
tail was rejected (`4 / 3 / 1`). The visible frame at that stop contained only the status
bar: 955 nonblack pixels, PPM SHA-256
`07C2CD8201AF1B2384775F16227DCA4BB06B4031EFDB399F742FF6F3A0A6D97A`. That frame was a
coherent intermediate surface after the guest cleared the lower screen, not random PNG
corruption and not a home-screen result.

The retained object exposed a second list word for an otherwise familiar top-band
battery tail. Form dispatch now includes the measured third list word, then validates the
whole list again. Three top-band variants were subsequently observed on literal targets
`0x00a41000`, `0x00998000`, and `0x00897000`. Each uses the same 21-by-4 source rows
16..19 at `0x00986000`; all 84 source pixels were independently measured as exactly zero
BGRA, and the implementation rejects a nonzero source. Each retained replay completed
84 pixels and raised `0x4c`.

r399 resumed the first completion to 5.8 B and displayed the real iPhone OS 3 home screen
with the `Edit Home Screen` tutorial. It exited zero, had no media failures, and produced:

    work/r399-derived-resume-5800m/w.img.screen.ppm
      SHA-256 BCF55ECBB9FC83074D1F230BD8E1F83C9D84C7FFA755C21C0CA02CDF6B24ED3D
    work/r399-derived-resume-5800m/screen.png
      SHA-256 0846D85F283D04E79B2E0F7C031E5581BE90EC957331B9D86ED2CA26DAAD4A6E

This is visual progress, not a performance result. r399 still logged a Graphics Recovery
Event. r400 then injected one tap at `(160,335)` over `Dismiss`: down and up were accepted
at 5,810,000,000 and 5,834,000,000 instructions, zero attempts were refused, and all 28
device reports were read. The guest reacted by submitting new render work. That proves
this specific touch reached SpringBoard; it does not prove every coordinate, gesture, or
the iOS host-app input path.

The tutorial dismissal exposed literal three-dimensional layers rather than a generic
unknown raster operation:

| retained run | exact layer | destination | source and measured constraint | replayed pixels |
|---|---|---:|---|---:|
| r402 | lower-screen dim mask | `0,20 320x460` | `0x00b12080`, rows 20..479, stride `0x500`, alpha-only premultiplied BGRA, vertex alpha exactly `0xb7` | 147,200 |
| r403 | popup panel | `18,130 284x241` | `0x00ba9080`, stride `0x480`, zero premultiplication violations, vertex alpha exactly `0x05` | 68,444 |
| r404 | `Edit Home Screen` title | `30,145 260x23` | `0x00bed080`, stride `0x420`, coherent text texture, vertex alpha exactly `0x05` | 5,980 |
| r405 | instructional body | `30,175 260x121` | `0x00bf3080`, stride `0x420`, coherent six-line text texture, vertex alpha exactly `0x05` | 31,460 |
| r406 | `Dismiss` button | `29,312 262x43` | `0x00c2b080`, stride `0x420`, coherent button texture, vertex alpha exactly `0x05` | 11,266 |

The source widths and row counts above are not guesses from the visible frame. Reading
the popup beyond row 240 reaches unrelated data and premultiplication failures; the exact
284-by-241 crop does not. For the title, stride `0x420` yields coherent text and zero
premultiplication violations, while the plausible `0x440` and `0x480` interpretations do
not. The focused fixture also had to model GART root 3 because the body texture crosses
GPU VA `0x00c00000`; the first root-2-only fixture failed and was corrected before replay.
Only the captured `0xb7` and `0x05` vertex words are accepted. Unlike the multiply observed
slider opacity, these single captures are not generalized to arbitrary alpha.

r407 consumed the final completion and ran 10 M more instructions with no decoder
rejection:

| measured r407 result | value |
|---|---:|
| 2D candidates / completed / rejected | 136 / 136 / 0 |
| 2D bytes committed | 3,012,184 |
| 3D candidates / completed / rejected | 15 / 15 / 0 |
| 3D pixels blended | 32,396 |
| external-media failures | 0 |
| process exit | 0 |

Its final CLCD frame is a coherent tutorial-free home screen with 80,312 nonblack pixels
and 238,252 nonzero RGB bytes:

    work/r407-derived-dismiss-next-6100m/w.img.screen.ppm
      SHA-256 A667640D78E19A8CB1DDBB20155EB4C6697C837B29B2CC894E06749ADCE4355E
    work/r407-derived-dismiss-next-6100m/screen.png
      SHA-256 C8EDB47A32AD8AD70118412D8E8599C7F8503BCFDCEEA239CDF8BFBD9EFA0986

Local verification at this checkpoint is 367/367 focused MBX assertions, 55/55 CTest
targets, and 367/367 again in the independent strict warnings-as-errors build.

The uncomfortable part matters: r407 still contains one Graphics Recovery Event with
`CompletedIntStatus=0x00000400`. That is compatible with delayed watchdog state carried
from the preceding rejected-command checkpoint, but r407 alone cannot prove that cause.
Therefore the result is **not** described as recovery-free. A clean resumed interaction
must show that the event does not recur, and an instruction-zero cold boot must reproduce
the entire path without any rejection or recovery before graphics can be called fixed.

The r400/r401 frame-meter probes each saw only their initial signature because rendering
stalled at the then-unknown form; their means (`0.042` and `0.039` fps) are stall evidence,
not steady-state home-screen cadence. They do not establish the emulator's current FPS.
No measured 30 FPS result exists yet. Network, sound, and iOS-app completion remain behind
that graphics acceptance gate.

### r408-r410: Settings launch advances through bounded fills and two exact home-screen layers

r408 started from the tutorial-free r407 checkpoint and injected the measured Settings
tap. The guest accepted the touch and reached a 50-command 2D submission, but the decoder
rejected its first previously unseen rectangle. Therefore r408 did **not** prove that
Settings opened. The useful result was a complete retained batch: six black fills, 42
premultiplied blends, and two plain copies, ending at `ring+0xbc60`.

The earlier fill implementation admitted only the full-width `(0,20)-(320,480)` clear and
its one measured split at row 389. Static disassembly closed the geometry ambiguity:
`_pack2DCtxBlitColor` at `0x30e1b080..0x30e1b0b4` packs `(x,y)` and
`(x+width,y+height)` into words 8 and 9, using literal masks `0x1fff` and
`0x1fff0000`. The decoder now accepts non-empty rectangles within 320x480, but remains
restricted to the captured command headers, surface form, mode `0x8000f0f0`, and black
color `0xff000000`. It validates every destination row before staging any pixel. The six
r408 rectangles were:

| left | top | right | bottom |
|---:|---:|---:|---:|
| 3 | 32 | 317 | 107 |
| 79 | 107 | 165 | 108 |
| 3 | 120 | 317 | 195 |
| 3 | 208 | 317 | 283 |
| 3 | 296 | 241 | 371 |
| 136 | 375 | 178 | 385 |

The focused test checks all six interiors, four adjacent outside sentinels, exact completion,
and a second batch whose last rectangle ends at x=321. That late-invalid batch produces no
completion and commits none of its earlier valid jobs. This is evidence for bounds and
atomicity, not support for arbitrary MBX2D fills.

Exact offline replay of the retained r408 batch completed all 50 commands and committed
879,232 bytes. It changed 4,146 pixels / 12,438 bytes on target `0x00897000`, with target
hash `9a728040559961b7 -> dbc63ef0fa1cd707` and status `0 -> 0x400`:

    work/r408-derived-complete-settings-batch-6200m/post-settings-batch-6200m.bin
      SHA-256 7A7484A991B387F3BFED67D06A5FD385B06A60BE1F5E8DF433D16AA809AA1CDC
    work/r408-derived-complete-settings-batch-6200m/screen.ppm
      SHA-256 C03EC02E1B99DA083F37A9E8B1056A1AE7E0277531050E6E2B046894CE131A2E

r409 resumed that completed batch from 6.2 to 6.3 billion retired instructions. It
completed 12/12 2D commands (389,500 bytes) and 3/4 3D renders (8,036 pixels), then stopped
at a new exact form. The retained source at GPU VA `0x00931080`, stride `0x160`, is a
coherent 86x13 `Messages` label: 333 nonzero-RGB pixels, 1,075 nonopaque pixels, and zero
premultiplication violations. The captured destination is `(3,94)` with size 86x13 on
target `0x00a41000`. Exact replay blended 1,118 destination pixels, changed 333 pixels /
999 bytes, changed nothing outside the rectangle, and moved the target hash
`6ec3a306a886257f -> 6e88cb664d1f8e5d`:

    work/r409-derived-complete-messages-6300m/post-messages-6300m.bin
      SHA-256 D98182022706F4993374CDF05C8ADA72408F8A9F120B858352F94F374E9D2F4E

r410 resumed that completed form from 6.3 to 6.32 billion instructions and stopped at the
next exact 3D form. Its source at GPU VA `0x00933000`, stride `0x100`, is a coherent 59x62
Messages icon: 3,191 nonzero pixels, 535 nonopaque pixels, and zero premultiplication
violations. Its destination is `(16,32)` with size 59x62 on the same transition target.
An independently retained r408 2D packet names the same source, stride, and destination,
which supports the interpretation without broadening it to other icons. Exact replay
blended 3,658 destination pixels, changed 3,191 pixels / 9,257 bytes, changed nothing
outside the rectangle, and moved the target hash
`6e88cb664d1f8e5d -> 8ab6affc47bc7754`:

    work/r410-derived-complete-messages-icon-6320m/post-messages-icon-6320m.bin
      SHA-256 51EC8AA004B4962C4A7F664821C9222E440DDF453EF487202D2833383FD6404C
    work/r410-derived-complete-messages-icon-6320m/screen.ppm
      SHA-256 014EFE85944CD00D48CE8DBAD35C3B8FA318B57F9D2E990473804A020C547DDB

The r409 and r410 live runs each still logged one Graphics Recovery Event with completed
status `0x400`. Each final CLCD image remained the coherent home screen; neither showed an
opened Settings application. The r409 frame meter saw 500 publications but only its first
signature changed (`0.044` mean / `1.896` maximum fps), while r410 saw 102 publications and
the same single-change pattern (`0.182` mean / `1.820` maximum fps). Those numbers measure
decoder stalls, not useful steady-state animation, and make no 30 FPS claim.

At this checkpoint, the previously rejected 2D batch and two immediately subsequent 3D
forms are implemented and exactly replayed. The Settings launch as a whole is **not fixed**:
the guest is still exposing one unsupported command at a time, recovery still recurs, and
the screen has not transitioned into Settings. A clean resumed interaction and a final
instruction-zero cold boot remain mandatory. Network, sound, and iOS-app completion have
not started because the graphics acceptance gate is still open.

Local verification after both forms is 393/393 focused MBX assertions, 55/55 CTest
targets, and 393/393 again in the independent strict warnings-as-errors build.

### r411-r412: Calendar label and icon base are exact; the transition is still incomplete

r411 restored the completed Messages-icon checkpoint at 6.32 B and ran to 6.34 B retired
instructions. It submitted no 2D work and stopped at one new rejected 3D form. The exact
register tuple was `xclip=0x00a80048`, `yclip=0x00700050`, target `0x00a41000`; its region
list covered tiles x=9..20, y=5..6. Decoding source word `0x8e1526e1` gives GPU VA
`0x00937080`, and the captured control retains the already measured `0x160` stride. The
86x13 source is visibly the `Calendar` label. It contains 276 nonzero-RGB pixels, 1,079
nonopaque pixels, and zero premultiplication violations:

    work/r411-derived-messages-next-6340m/source-00937080-86x13.ppm
      SHA-256 839528E9A333AD8FCD1FFF1CEFD1873B6FC05C58E55AE98D2B2F35820C4D53D5

The complete retained boundary and 44-word quad resolve to destination `(79,94)` at
86x13. Exact replay blended 1,118 destination pixels, changed 276 pixels / 828 bytes,
changed zero pixels outside `(79,94)-(165,107)`, raised status `0 -> 0x4c`, and moved the
target hash `8ab6affc47bc7754 -> 9c022d948d4114cb`:

    work/r411-derived-complete-calendar-label-6340m/post-calendar-label-6340m.bin
      SHA-256 053E011E639E05A54454204A7C332622710E70B14EA4489ABCDE27778FD509EF

r412 restored that completed form at 6.34 B and ran to 6.36 B. Again it submitted no 2D
work and stopped at one new rejected 3D form. The registers were
`xclip=0x00980058`, `yclip=0x00600020`, target `0x00a41000`; the region list covered tiles
x=11..18, y=2..5. Source word `0x8e112720` decodes to GPU VA `0x00939000` with captured
stride `0x100`. The 59x62 source is a coherent red-header/white-page Calendar icon base,
not the complete dated icon: its date text is absent and must be a later layer. It has
3,191 nonzero-RGB pixels, 535 nonopaque pixels, and zero premultiplication violations:

    work/r412-derived-calendar-next-6360m/source-00939000-59x62.ppm
      SHA-256 A87D0A32EC7E0F7624A93D62283A841EE665CDCC4C2011356C2ADA0806CDB5D0

The complete retained form resolves to destination `(92,32)` at 59x62. Exact replay
blended 3,658 destination pixels, changed 3,191 pixels / 9,573 bytes, changed zero pixels
outside `(92,32)-(151,94)`, raised status `0 -> 0x4c`, and moved the target hash
`9c022d948d4114cb -> 69d963d33ba27dbc`. The resulting transition surface is visually
coherent but deliberately partial: it contains the Messages icon/label, Calendar icon
base/label, page dots, and dock over black, not a completed screen:

    work/r412-derived-complete-calendar-icon-base-6360m/post-calendar-icon-base-6360m.bin
      SHA-256 C30E16B8434196B4C5B2F28A9EB5865F9D930818CF8342FDD24E7A346C5CE0D9
    work/r412-derived-complete-calendar-icon-base-6360m/post-calendar-icon-base-6360m.bin.mdimage
      SHA-256 0DD614CBEFD79920B120BD2A15AEE2A5CE9B93391FBC65BA10EA5E3FF93214D4
    work/r412-derived-complete-calendar-icon-base-6360m/post-calendar-icon-base-6360m.bin.mdstate
      SHA-256 A4BB46262AF2CB8B592B2DAF4C721593717788C52B3FBAA0868F6B9E4AB627AE
    work/r412-derived-complete-calendar-icon-base-6360m/screen.ppm
      SHA-256 6F3CAC312AE6554F1B30C1F4EB62A92953F1B83E98381ADDB78484E2DF98460F

Both live runs exited zero and reported zero external-media failures, but each still
logged one Graphics Recovery Event with completed status `0x400`. Their final live CLCD
frames were byte-identical coherent home screens (`C3A60DB2...`), not the partial target
above and not an opened Settings application. Each frame-meter run published 101 scans
but changed only its first sampled signature. r411 reported `0.206` mean / `1.851`
maximum fps and r412 `0.221` mean / `1.992` maximum fps. These are decoder-stall results,
not steady-state performance measurements.

Local verification is now 413/413 focused MBX assertions, 55/55 CTest targets, and
413/413 in the independent strict warnings-as-errors build. This advances the retained
transition by two literal forms; it does **not** fix the transition, recovery, Settings
launch, or 30 FPS acceptance. More layers are still expected, and a clean interaction
plus instruction-zero cold boot remain mandatory after the command frontier is cleared.

### r413-r414: replace the app-by-app literals with one producer-derived sprite decoder

The r409-r412 implementation was useful as an exact oracle, but it was also an obvious
scaling failure: one runtime table entry per icon or label is controlled whack-a-mole.
Those four app-specific entries have therefore been removed from the decoder. Their raw
commands remain in the tests and retained checkpoints, where they are evidence rather
than an implementation strategy.

Static analysis of the shipped MBX2D producer closes enough fields to decode this family
semantically. `_mbx3DCtxQuadCopyPerspective` is at `0x30e1cb68`; its exported wrapper is
at `0x30e1d6e8`. The producer finds the four transformed extrema, adds exact float
literals `0x3eefe000` / `0x3f081000`, converts them to integer bounds, then aligns x to
8-pixel tiles and y to 16-pixel tiles. Texture-header nibbles encode
`log2(power-of-two dimension)-3`. Linear pitch is split across two words because the GPU
address occupies bits 0..17: the high bits of `pitch_bytes/16` remain in source-control
bits 18..23, while bit 1 is relocated to texture-header bit 0. That equation independently
reconstructs every measured stride: `0x40`, `0x140`, `0x160`, `0x2a0`, `0x420`, `0x480`,
and `0x500`.

The new bounded app-grid decoder requires all of those encodings to agree. It accepts only
the measured transition target `0x00a41000`, BGRA8 source-over setup, axis-aligned 1:1
geometry, origin-zero `(width-0.5)/pow2` UVs, normalized destination coordinates, the
producer-derived clip, a matching row-major tile list, an eight-pixel padded source pitch,
uniform vertex alpha, premultiplied source pixels, and a fully mapped 320x480 destination.
All source and destination rows are validated and staged before any write. Perspective,
scaling, coloured vertices, other targets, inconsistent redundant fields, and partial
GART mappings fail without completion or writes. The second measured sampler uses the
uniform-alpha equation already pinned against QuartzCore's software compositor; the
producer itself writes that one context byte to all four vertices.

The four retained r409-r412 commands now replay through this decoder with byte-identical
results, not through literal fallback:

| form | target hash after replay | changed pixels / bytes | outside |
|---|---|---:|---:|
| Messages label | `6e88cb664d1f8e5d` | 333 / 999 | 0 |
| Messages icon | `8ab6affc47bc7754` | 3,191 / 9,257 | 0 |
| Calendar label | `9c022d948d4114cb` | 276 / 828 | 0 |
| Calendar icon base | `69d963d33ba27dbc` | 3,191 / 9,573 | 0 |

A fifth test is intentionally synthetic and labelled as such: it constructs a
producer-consistent 24x8 sprite at a third position and pitch. It exists to prevent the
four removed literals from merely reappearing as conditionals. Adversarial tests mutate
normalized positions, UV extent, split pitch, vertex alpha, clip registers, region tiles,
header controls, and the final source texel. Every case rejects atomically. Focused and
strict builds now pass 517/517 assertions; the full suite passes 55/55 targets.

r413 resumed the completed Calendar checkpoint at 6.36 B and stopped at 6.40 B. It saw
eight 3D submissions and completed seven previously unseen ones, blending 18,694 pixels,
before rejecting the second sampler form. The resulting target contained Messages,
Calendar including its date, Photos, Camera, YouTube, and the dock. It was not yet the
complete grid. The next source was independently dumped as the 86x13 `Stocks` label at GPU
VA `0x00954080`, stride `0x160`; it has zero premultiplication violations:

    work/r413-semantic-app-grid-6400m/source-00954080-86x13.ppm
      SHA-256 97260CD89E9575291CD0984EAFE292C05A2B483E93F64F256281160A94AAF04E

Its packet uses the same semantic geometry and pitch fields, sampler word `0xcd206c40`,
and uniform vertex alpha `0xfe`. Exact replay through the generalized modulation path
blended 1,118 pixels, changed 219 pixels / 657 bytes, changed nothing outside
`(79,182)-(165,195)`, and moved the target hash
`075cde4b1a5a5d74 -> ea7aa2c98c3cc544`:

    work/r413-derived-complete-stocks-label-6400m/post-stocks-label-6400m.bin
      SHA-256 EC6565D6D66345F78B73A76B2475E78CAD6BF50202A2F4B04BC543F7797A14FC

r414 is the falsification test for the semantic claim. It resumed that completed Stocks
checkpoint from 6.40 B to 6.42 B and completed **19/20 previously unseen 3D submissions**
(46,642 blended pixels) plus 10/10 2D commands (309,376 committed bytes) before the next
rejection. The target is now a visually coherent complete stock home-screen grid and dock:

    work/r414-semantic-modulation-6420m/transition.ppm
      SHA-256 99A9E5CBEF89A9973D7081E1EB1F75A8A22150E9B031FEF0C90FFB9D91EF6AE9

That is substantial evidence that this decoder represents a command family rather than
an app-name whitelist. It is still not end-to-end success. The rejected command changes
the third object-list word from `0x61a0007c` to `0x612000a8`. Its clip and first boundary
object describe only `(159,239)-(161,241)`, while its textured quad lies at roughly
`(168,296)-(227,358)`. Treating the list word as interchangeable and blending the whole
quad would therefore be fabricated behaviour. The next work is to decode the list/object
relationship from the producer and retained stream, not add an App Store-specific form.

Both r413 and r414 still logged one Graphics Recovery Event after their final rejection,
and both live CLCD frames remained the old coherent home surface with SHA-256
`C3A60DB2...`. r414's frame meter saw 103 publications but only its initial sampled
signature changed; its mean/max were `0.184` / `1.836` fps. Those are rejection-stall
measurements, not useful animation cadence. Settings has not been shown open, no 30 FPS
result exists, and the final clean interaction plus instruction-zero cold boot remain
mandatory. Network, sound, and iOS-app completion remain behind that graphics gate.

### r415-r417: object pointers, screen clipping, and guard bounds are now semantic

The r414 rejection did not describe another app sprite. The low twenty bits of the third
object-list word are a word offset from `OBJBASE`: `0x61a0007c` selects the texture at
`+0x1f0`, while `0x612000a8` selects a different object at `+0x2a0`. The latter is five
33-word records emitted by `_mbx3DCtxQuadColor`; the visually suggestive texture still at
`+0x1f0` is stale and must not execute. Five older `0x612` status forms had made exactly
that mistake and were removed rather than preserved as compatibility cases. The bounded
solid decoder now follows the pointer and cross-checks the five records against geometry,
normalized coordinates, boundary, clip, tiles, target mapping, and one uniform colour.

**Correction found after r419:** the first implementation misidentified the quad colour.
It treated fixed `0xffffffff` words in the four trailing parameter records as colour and
required the main per-vertex word to be zero. That made r414 paint four white pixels and
was pushed in `307c017`. Disassembly of the live `_mbx3DQuadColor` binding proves this was
wrong: SpringBoard's lazy pointer `0x38189904` resolves to wrapper `0x30e1ba94`, which
passes its second exported argument in `r2` to producer `0x30e1b468`; the producer stores
that value at staging offset `+0x48`, and the serializer repeats it before all four
normalized vertex pairs. r414's actual argument is `0x00000000`, so it is a transparent
no-op. Corrected exact replay raises `0 -> 0x4c` while leaving the target hash unchanged
at `f8c6aee0461c0993`, with zero changed bytes or pixels:

    work/r414-derived-complete-transparent-center-6420m/post-transparent-center-6420m.bin
      SHA-256 8322C3BC41FE76904AE0B8B81D34B07C5664E8AF63284D3C2A02071C1B998015
    work/r414-derived-complete-transparent-center-6420m/screen.ppm
      SHA-256 99A9E5CBEF89A9973D7081E1EB1F75A8A22150E9B031FEF0C90FFB9D91EF6AE9

The earlier artifact below is retained only as failed evidence. It is pixel-incorrect and
must not be used as a resume point:

    work/r414-derived-complete-solid-center-6420m/post-solid-center-6420m.bin
      SHA-256 E63FED314F1CA7FEBAD9601C2D81D0B5F793D9FE7BAA8186C3CAC5DBA3AD756D

Consequently, the r415-r419 chain originally resumed from that invalid checkpoint. Its
command order, completion counts, and producer-format discoveries remain diagnostic
evidence because the error changed only four target pixels, not device state or guest
control flow. Its pixel hashes and visual checkpoints are contaminated and are not final
evidence; the chain must be regenerated from the corrected r414 checkpoint.

r415 resumed that checkpoint from 6.42 B to 6.44 B. It completed 4/4 2D submissions
(523,552 bytes) and 3/4 3D submissions (8,036 pixels) before a page-indicator sprite on
target `0x00998000`. This capture established a second producer layout: source control
`0x0e...` uses the alternate vertex order and full `width/pow2`, `height/pow2` UV extents;
the earlier `0x8e...` order uses half-texel extents. The sprite decoder no longer
whitelists one transition address. Instead the background object, blend object, and
`FBSTART` must independently resolve to the same fully mapped target. Exact replay of the
10x10 page indicator blended 100 pixels, changed its 36 nontransparent pixels / 108
bytes, changed nothing outside `(136,375)-(146,385)`, and saved:

    work/r415-derived-complete-page-indicator-6440m/post-page-indicator-6440m.bin
      SHA-256 E0DFABDBAABAE16D16D6DACECE7E1BE8AAA5EEBB08C20DB0C87A1102684ABBEA

r416 restored that completion from 6.44 B to 6.46 B. MBX tracing was accidentally omitted
from this one run, so there is no honest per-command completion count. The final snapshot
does prove that the guest consumed the page-indicator completion and submitted the next
command: status returned to zero and the object changed to a partly off-left Messages
label. Its quad is `(-16.3478,73.0469)-(69.6522,86.0469)`, while its source is the full
86x13 label at GPU VA `0x00931080`, stride `0x160`. Disassembly of the shipped
`_mbx3DCtxQuadCopyPerspective` at `0x30e1cb68` shows that it intersects float extrema with
the context bounds before adding constants `0x3eefe000` / `0x3f081000`, converting with
`VCVT.U32.F32`, and aligning x/y to 8x16 tiles. The decoder now models only a one-sided,
unity-scale edge crop; general filtering, scaling, and sprites crossing both screen edges
remain unsupported. Exact replay selected source columns 16..85, blended 910 pixels,
changed 333 pixels / 999 bytes, touched nothing outside `(0,73)-(70,86)`, and saved a
reload-verified checkpoint whose external-media sidecars are hard links on F::

    work/r416-derived-complete-clipped-messages-6460m/post-clipped-messages-6460m.bin
      SHA-256 EF137DC1F18018691C6725AE6105164EC27508647620EA233CC25BBD1E373A59
    work/r416-derived-complete-clipped-messages-6460m/screen.ppm
      SHA-256 1CD46F842EBA9AA6C502B298E424D120A7D79C5D138DD3CA21822E62BA8EEFCA

r417 restored that checkpoint from 6.46 B to 6.48 B with tracing enabled. It submitted 11
new 3D renders and completed 10 of them (25,616 blended pixels) before reaching the next
Stocks-label packet. That is real command-family progress, but it did not produce moving
scanout: 100 publications changed only at the first publication; 8/9 completed windows
were zero, with `0.213` mean / `1.914` maximum changed-publication fps. The run exited
zero, had zero external-media failures, and still logged one Graphics Recovery Event.
Its clean live home-screen image is byte-identical to r416
(`FE689B05888C85C7A2DCD8E158BE1370EDE2A4353A4961A728854807E2929383`); Settings
still has not opened.

The r417 packet exposed a latent geometry error rather than a new format. Its 13-pixel
quad spans y=`162.5213..175.5213`, but the producer's asymmetric biases conservatively
encode guard bounds y=`162..176` (14 rows). Fragment coverage is instead the pixel-centre
interval `[ceil(min-0.5), ceil(max-0.5))`, y=`163..176` (13 rows). The decoder now keeps
guard/tile bounds and raster coverage separate. Exact replay blended 1,118 pixels, changed
the 219 nontransparent Stocks pixels / 657 bytes, and changed zero pixels in the extra
guard-only row or anywhere outside `(58,163)-(144,176)`:

    work/r417-derived-complete-stocks-label-6480m/post-stocks-label-6480m.bin
      SHA-256 F745B43369DFE68095104F66025C48A2F640A7ACEE7883B98433F6D171849AE3
    work/r417-derived-complete-stocks-label-6480m/screen.ppm
      SHA-256 1406CA871236E0B2384B37E23EBDA1F61805F17CDAAC22319C0BA3580A14D87E

r418 completed 13/14 further sprites (29,606 pixels) before a partly off-left Settings
label. Its geometry extends to y=`393.0765`, but the boundary object stops at integer
y=`389`, the top of the dock. This is the context intersection visible in the shipped
producer, not another texture layout. The sprite decoder now accepts an integer context
scissor only when it is a nonempty subset of the independently encoded quad and surface;
it intersects pixel-centre coverage with that boundary, derives the same contiguous
source crop, and still requires guard-rounded clip registers and tiles to agree. Exact
replay covered 71x9 pixels, changed 216 pixels / 648 bytes, and touched nothing outside
`(0,380)-(71,389)`:

    work/r418-derived-complete-clipped-settings-label-6500m/post-settings-label-6500m.bin
      SHA-256 37E4F47AA1A68D0D72B2116B5762FC13AD85D54C591B27E356871A2E4432E7DA

r419 then completed 13/14 3D submissions (30,556 pixels) and 2/2 2D commands
(232,960 bytes) before another `0x612` object. The same producer mapping above proves its
uniform main colour is premultiplied translucent black `0x17000000`; the trailing
`0xffffffff` words remain fixed parameters. A direct replay completes 1,320 covered
pixels. Its current target rectangle is already opaque black, so the correct blend changes
zero bytes; that no-change result is not independent proof of the blend equation, which
is instead covered by synthetic nonblack-destination tests. This r419 run still descends
from the invalid four-pixel checkpoint and must be repeated before it becomes final pixel
evidence.

The current strict focused result is 560/560 assertions and the full strict suite is
59/59 targets. This checkpoint replaces several literal mistakes with producer and
raster invariants, but the end-to-end verdict remains **not fixed**: recovery recurs at
the next unsupported command, the measured scanout is nowhere near 30 FPS, Settings has
not opened, and no final cold boot has run. Network, sound, and iOS-app completion remain
behind the still-open graphics gate.

### r420-r423: `0x8e` is filtered, and the first uniform minification family clears

There is another material correction to the pixel evidence above. The `0x8e` texture
order is not a contiguous row copy. QuartzCore's
`CA::RenderMBX2D::transform_filter_bits` at `0x31239dc0` returns fractional-transform
bits 0/3. `emit_pattern` calls it at `0x3123b744`, reduces `result & 9` to one boolean in
`Data+0x76` bit 0, and `CA::RenderMBX2D::blit_persp` passes that boolean to
`_mbx3DQuadCopyPerspective`. The MBX producer turns it into the `0x8e...` source-control
layout. This is direct shipped-code evidence that the earlier fractional icons and labels
were filtered subpixel draws. Their command completion and control-flow progress remain
real, but their direct-copy pixel hashes are approximations. In particular, the r416 and
r418 claims about selecting one exact contiguous source crop are superseded.

The co-shipped software reference is
`CA::OGL::sw_sample_linear_BGRA8` at `0x3122bad8..0x3122bce0`. It converts interpolated
coordinates to 16.16, subtracts half a texel, clamps a 2x2 tap set, takes eight-bit
fractions, then interpolates top-left/bottom-left and top-right/bottom-right vertically
before the final horizontal interpolation. Its packed `0x00ff00ff` unsigned
multiply/add sequence is now transcribed in the MBX path. The decoder stages the complete
sample window and destination before validating premultiplication and committing any
write. Missing late PTEs and a non-premultiplied final sampled tap therefore reject
atomically.

This is stronger than the old row copy, but it is not honestly hardware-bit-exact. No
PowerVR MBX hardware oracle is available for the interpolator's undocumented sub-LSB
precision. The implementation uses binary32 producer coordinates and Apple's own
software filter kernel. That is the best local reference available; the limitation is
explicit rather than hidden behind an exact-looking hash.

The first r420 run, from corrected r414 at 6.42 B to 6.44 B, cleared 46/47 3D submissions
and 6/6 2D commands. Its control-flow result remains useful, but its target pixels used
the now-disproved row-copy approximation and are not a resume base for acceptance. The
new exact packet at that frontier samples a coherent opaque 320x460 pinstripe/home
surface from GPU VA `0x00bad080`, stride `0x500`, into a
`(145.5945,220.1924)-(174.4055,261.6083)` quad with uniform alpha `0x17`. Its scale is
uniform to about `4.2e-8` absolute between axes. Direct filtered replay covers 28x42 =
1,176 pixels, changes all 1,176 on the retained target, touches zero outside
`(146,220)-(174,262)`, and raises status `0 -> 0x4c`.

r421 therefore regenerated the chain from the corrected transparent r414 checkpoint,
not from r420's approximate output. It reached 6.44 B with 51/52 3D completions (107,213
pixels) and 7/7 2D completions (782,112 bytes), then stopped at another uniformly scaled
packet. This one is a coherent 320x20 transparent status strip containing `4:00 PM`.
It uses the already measured `0xcd206c40` / `0xae504ea0` modulated source-over state, so
scale is a transform/filter semantic rather than a third-state-only property. Exact
replay covers 28 pixels; only three destination pixels change because almost all sampled
source pixels are transparent, and none changes outside `(146,219)-(174,220)`:

    work/r421-filtered-chain-6440m/post-resume-6440m.bin
      SHA-256 C1BFE9409D1FE65FDE79D3F7B3C98879A75C86AB2EDDAD586C4EA2371DDE3DCF
    work/r421-derived-complete-status-strip-6440m/post-status-strip-6440m.bin
      SHA-256 D6E5B5BB0486046EEB550D95D023888298377493988CAE96FBB988053A1C6DC6

r422 exposed one more incorrect assumption in dimension recovery. Its 76x16 source has
encoded texel maxima `(75.5,16.0)`: one axis ends on a texel centre and the other on the
texture edge. Unconditionally adding 0.5 was wrong. The semantic rule is now to retain
the exact encoded texel extent and recover the addressable source rectangle with
`ceil(extent)` on each axis. Header powers, split pitch, UV record order, source bounds,
geometry, scale, boundary, clip, and tiles must still agree independently. This is not a
literal exception for 76x16. Exact replay covers seven pixels, changes five, touches zero
outside `(146,219)-(153,220)`, and yields the verified derived checkpoint:

    work/r422-derived-complete-mixed-edge-6460m/post-mixed-edge-6460m.bin
      SHA-256 39FAA477B3353E689D8329489ABA02D09194ACBE8CCECB87DCD686DEEF973C5D

The family then cleared rather than producing another literal chase. r423 resumed that
checkpoint from 6.46 B to 6.48 B and completed **13/13 3D submissions** (317,734 pixels)
plus **15/15 2D commands** (1,875,472 bytes), with zero decoder rejections and zero
external-media failures. The live scanout rose from 238,243 to 460,266 nonzero RGB bytes
and is visually coherent: `Searching...`, `4:00 PM`, a battery icon, and the native
pinstripe backing surface. It is not random/cursed memory, but it is also not proof that
Settings finished opening; app content is still absent. One Graphics Recovery Event
still occurs, so graphics is not fixed.

    work/r423-filtered-chain-6480m/post-resume-6480m.bin
      SHA-256 3E32204D7307BDE2BDEEEFDA9904C8FE8946DA488818C0E2CB5C70F5C1E9C36C
    work/r423-filtered-chain-6480m/w.img.screen.ppm
      SHA-256 B140DB30F707921DE0E884500A9368D3BCDD0D080D68546414C0DB228DB1A558

The performance verdict remains bad. r423's desktop app-equivalent meter saw four changed
sampled signatures in 100 publications: nine completed windows had mean `0.862` and
maximum `3.995` changed-publication fps, six windows were zero, and none reached 30 FPS.
This is neither an iOS-device FPS measurement nor a compositor count, but it is enough to
reject any 30 FPS success claim. Strict focused verification is now 699/699 assertions;
the warnings-as-errors build and all 59/59 CTest targets pass. Direct-state scaling,
magnification, nonuniform scale, perspective, coloured vertices, final cold boot, stable
app presentation, network, sound, and iOS-app completion all remain open.

### r424-r429: Settings presents, but recovery and performance remain open

r424 extended r423 from 6.48 B to 6.50 B without a new MBX submission and with a
byte-identical screen. Its terminal profile made Preferences look stuck in libobjc:
PID 38 owned 76.6% of user samples, concentrated around `_method_list_nth` and
`_getMethodNoSuper_nolock`. That interpretation did not survive a progress probe.
r425 continued the same state to 6.52 B and recorded 111
`_getMethodNoSuper_nolock` calls, three `_argb32_mark_pixelshape` calls, and 14
`_CABackingStoreUpdate` calls. The selectors included `drawLayer:inContext:`,
`drawRect:`, text, separator, highlight, and colour work; the profile moved into
zlib/CoreGraphics and Preferences fell to 57.5%. This was finite Settings view
construction, not one libobjc loop:

    work/r425-preferences-progress-6520m/post-resume-6520m.bin
      SHA-256 99C52588194E8605CD2EE2F523A90CED2E34AA5DCAE1005E8E69B18B2AC21556

r426 then submitted five valid 2D commands and rejected one 3D sprite. The retained
packet draws a 5x27 UV rectangle from a 16x32 allocation with a 64-byte row pitch.
The old decoder incorrectly required the allocation to be the smallest power of two
around the UV extent. Static producer evidence contradicts that rule:
`_mbx3DCtxQuadCopyPerspective` at `0x30e1cb68` derives the texture header from its
allocation arguments while the row pitch arrives independently through context
offset `+0x14`. The decoder now reconstructs the allocation width from the split
pitch/header encoding and requires the complete UV rectangle to stay inside it.

This correction also invalidated an old negative test. Flipping one pitch bit can
describe another valid padded allocation; it is not necessarily malformed. The test
now breaks the redundant header-width/pitch relationship instead of assuming a
minimal stride. That failed test was corrected rather than weakened or hidden.

r427 proved the padded allocation live: the formerly rejected packet completed 135
pixels. The immediately following packet exposed the actual remaining transform: a
nonzero UV interval `u=5..6`, `v=0..27` magnified into a 63x27 destination. The
renderer now decodes all four redundant corners of an axis-aligned UV rectangle,
samples from that origin with the already transcribed Apple software bilinear kernel,
and clamps taps to the encoded allocation. Direct filtered magnification is accepted
only when both axes magnify; direct minification, a mixed minify/magnify transform,
perspective, and unfiltered nonzero-origin copies still reject. Alternate/modulated
filtered state remains limited to the previously measured uniform minification.

This is a semantic generalisation rather than a literal two-packet whitelist. r428
replayed the same r425 input and, after the two captured forms, completed five more 3D
renders and 41 more 2D commands without another decoder change:

| measured r428 result | value |
|---|---:|
| 2D commands completed / rejected | 46 / 0 |
| 2D bytes committed | 758,720 |
| 3D renders completed / rejected | 7 / 0 |
| 3D pixels blended | 3,379 |
| external-media failures | 0 |
| process exit | 0 |

The display changed for the first time in this chain. Against r423-r427, exactly
112,573 of 153,600 pixels changed (73.290%), inside `(9,32)-(317,479)`. Visual
inspection shows a coherent, populated native Settings screen: title, Airplane Mode,
Wi-Fi, Sounds, Brightness, Wallpaper, General, Mail/Contacts/Calendars, and Phone.
It is not the earlier blank pinstripe surface and not random/cursed memory:

    work/r428-uv-subrect-6540m/post-resume-6540m.bin
      SHA-256 2E5A7923C02B790BCFF2B50D597081BC8835E9E105528989ED314DBB5102AA03
    work/r428-uv-subrect-6540m/w.img.screen.ppm
      SHA-256 4C7634FDF7543B975C131F036AA3D1F3684897B5AC4AA3920CF57BC45837382E

That is a visible Settings-presentation fix at this checkpoint, not final graphics
acceptance. r428's desktop meter saw 121 publications and only two changed sampled
signatures. Its 12 completed windows had mean `0.307`, maximum `1.851`, ten zero
windows, and none at or above 30 fps.

r429 extended the exact r428 snapshot by another 20 M instructions. It exited zero,
had zero media failures, submitted no 2D or 3D command, and changed zero framebuffer
pixels; the PPM stayed byte-identical. Its 101 publications again contained only the
initial signature (`0.189` mean / `1.888` maximum, nine of ten windows zero, none at
30 fps). Stability therefore does not become an FPS claim.

More importantly, recovery is still unresolved. r429's fresh MBX trace contains one
interrupt-mask write, `0x130 <- 0`, and its guest log reports a Graphics Recovery Event
with `2DIdle=0`, `3DIdle=1`, and `CompletedIntStatus=0x400`, despite no new command in
that window. The state descends from a chain that previously rejected commands, so this
run cannot distinguish inherited poisoned driver state from a current completion/
lifecycle defect. It does prove that clearing the sprite decoder is not sufficient to
call recovery fixed. A clean pre-recovery continuation and ultimately an
instruction-zero cold boot remain mandatory:

    work/r429-settings-settle-6560m-retry/post-resume-6560m.bin
      SHA-256 FC5CB82DB0AD931E23A901DCC35B4C7DE4F1DBF6793BCFADB6B10F57D8F265FD

Current local verification is 753/753 focused MBX assertions, the independent strict
warnings-as-errors build is green, and all 55/55 tests in the currently configured
exact suite pass. The PowerVR interpolator still has no hardware bit-exact oracle;
binary32 producer coordinates plus Apple's co-shipped software kernel remain the stated
reference. Graphics recovery, 30 fps, a clean cold reproduction, network, sound, and
iOS-app completion all remain open.

### r430-r431: clean replay clears the unfiltered integer-crop family

r429 could not distinguish a current lifecycle fault from recovery state inherited from
earlier rejected commands. r430 therefore restarted from r384's instruction-zero,
cold-derived pre-drag checkpoint at 3.90 B, before that lineage had recorded a Graphics
Recovery Event. Replaying the same unlock drag through 4.60 B completed 259/259 2D
commands (28,658,364 bytes) and 114/115 3D renders (3,517,009 pixels) without a recovery
event. The one retained rejection was real: an unfiltered 1:1 sprite selected source
rows 20..479 and destination rows 20..479 from a 320x480 surface stored inside a 512x512
allocation. The old decoder incorrectly required every unfiltered crop to begin at
texture origin.

The decoder now accepts an unfiltered nonzero-origin rectangle only when all four UV
edges are exact integers, the full raster is 1:1 with that rectangle, and the clipped
copy remains inside the independently recovered source bounds. Fractional unfiltered
coordinates still reject atomically; there is no guessed nearest-neighbour rule. The
retained source had alpha 0..127 and uniform vertex alpha `0x01`, so Apple's measured
eight-bit modulation makes the command a pixel no-op. Exact offline replay left all
153,600 target pixels byte-identical while changing completion status from `0` to
`0x4c`. That unchanged image is expected arithmetic, not proof obtained by ignoring the
command:

    work/r430-clean-unlock-replay-4600m/post-replay-4600m.bin
      SHA-256 696D31F4488F671A695CDE42B54082AD169AD6765DFBFF9B9C214E6110160202
    work/r430-derived-complete-lower-crop-4600m/post-crop-4600m.bin
      SHA-256 60246426A34D7E9B5B52524A0259DE4ACF7F4015A956CA3D60D1DCBCF3D97710

r431 resumed that derived completion with the corrected live binary through 4.80 B. It
did not stop at the next literal packet: it completed **40/40 2D commands**
(16,716,880 bytes) and **384/384 3D renders** (2,862,526 pixels), with zero decoder
rejections, zero Graphics Recovery Events, zero raw external-media guest errors, and a
zero process exit. Against r430's status-bar-only frame, 110,826 pixels changed inside
`(0,3)-(320,480)` and nonblack pixels rose from 955 to 110,823. Visual inspection shows
a coherent native Home screen with icons, dock, status bar, and the `Edit Home Screen`
alert. This is broad live-family progress and clean-state evidence against r429's
inherited-recovery interpretation; it is not yet a final cold-boot proof that recovery
can never recur:

    work/r431-clean-after-lower-crop-4800m/post-resume-4800m.bin
      SHA-256 41946051A1EA97B28874935D4DE2E4F83E1B555B9BF18E074B3E9FAD45D52D81
    work/r431-clean-after-lower-crop-4800m/w.img.screen.ppm
      SHA-256 F4D5F756A02A66C134A165548095440B87C666653EAF2D17D71D57C35C67AC84

The performance result is still unacceptable. Over 200 M guest instructions, the
desktop app-equivalent meter observed 1,006 publications and 15 changed sampled
signatures. Its 91 completed windows had mean `0.311`, maximum `3.711`, 77 zero windows,
and none at or above 30 changed-publication fps. The guest retired 3.559 M instructions/s
over the measured 56.189 host seconds. This is not iOS-device FPS, but it decisively does
not support a 30 FPS claim. Focused verification is 782/782 assertions; full exact and
strict verification pass 55/55 and 59/59 tests respectively, including the independent
`-Wall -Wextra -Werror` build. Final cold boot, 30 fps, network, sound, and iOS-app
completion remain open.

### r432-r433: the 2D producer crosses 64 KiB before wrapping its cursor

r432 injected a real Z2 tap at `(160,334)` to dismiss the first-run Home-screen tip.
The device queued, length-read, data-read, and completed all 28 reports with zero
refusals. UIKit began the dismissal, 58/58 3D renders completed (1,687,152 pixels), and
320 2D commands completed (8,026,096 bytes) before one 44-command 2D submit rejected.
There was no Graphics Recovery Event. The translucent terminal frame is the alert's
partly rendered dismissal, not a completed UI or an acceptable visual checkpoint.

The rejection exposed a ring-lifecycle assumption rather than a new pixel operation.
The batch begins at ring `+0xff70`. After valid commands at `+0xff70` and `+0xffb0`,
another begins at `+0xfff8`: its header and target are the last two words inside the
nominal 64 KiB ring, while its remaining words continue contiguously in the following
EDRAM bytes. The next complete command begins at `+0x0000`.

Static AppleMBX code independently specifies that behaviour. The command-copy helper at
`0xc077a188` loads the cursor at `0xc078b1bc`, copies all `length / 4` words with a
post-incrementing store, and only after the copy adds the length, compares the updated
cursor with `0x10000`, and resets the next cursor to zero when it is greater or equal.
It neither splits the crossing command modulo the ring nor discards it. The decoder now
allows a command body to continue contiguously beyond the nominal ring inside the MBX
aperture, then resets only the next command head to ring zero. Heads must still begin
inside the measured 64 KiB range and remain contiguous in the producer's exact order.

The focused boundary test places a simple copy at `+0xfff8` and another at `+0x0000`.
Both destinations must remain unchanged before the fixed doorbell store and both must
change afterward; splitting, skipping, or carrying the overrun modulo the ring fails the
test. Exact replay of r432's retained batch then completed all 44 commands, committed
969,928 bytes, and changed status from `0` to `0x400`. Its opaque destination surface
changed 109,919 pixels / 309,867 RGB bytes, all inside `(0,33)-(320,480)`:

    work/r432-derived-complete-ring-wrap-5000m/post-wrap-5000m.bin
      SHA-256 02D31D57622F32509937FAC99305F94FB2B3329A993981CA7610BDFEA30F8E6A
    work/r432-derived-complete-ring-wrap-5000m/target-before.ppm
      SHA-256 CD48E247062675FE25D50DE4F4E354E74BA068AF0D20C4D1B533CC0EC0BE998B
    work/r432-derived-complete-ring-wrap-5000m/target-after.ppm
      SHA-256 ECDA2D7412E0C498BAA3E51EC93AEE86E114A2B491DE95DF8A2E9C21BA0FA559

r433 supplied that completion to the live guest and continued from 5.00 B to 5.20 B. It
cleared 11 further 2D submits containing **230/230 commands** (5,105,640 bytes) and
**49/49 3D renders** (666,660 pixels), with zero decoder rejections, zero recovery
events, zero raw external-media guest errors, and exit zero. The alert is fully gone;
the terminal frame is the coherent tutorial-free Home screen and is byte-identical to
the independently reached r407 reference:

    work/r433-live-after-ring-wrap-5200m/post-resume-5200m.bin
      SHA-256 EEE6F21EF507EEF2D021D83F5DF489B49FB6D5486F0E1410E8BA539343CD74D9
    work/r433-live-after-ring-wrap-5200m/w.img.screen.ppm
      SHA-256 A667640D78E19A8CB1DDBB20155EB4C6697C837B29B2CC894E06749ADCE4355E

This still is not a performance win. r433's mostly static post-dismiss window produced
1,006 publications but only nine changed sampled signatures. Its 90 completed windows
had mean `0.191`, maximum `1.990`, 81 zero windows, and none at or above 30 fps. That
workload cannot expose a renderer's frame ceiling because SpringBoard has no reason to
redraw a static Home screen. A sustained stationary contact will next enter icon-wiggle
mode and provide an honest animated workload. Focused exact and strict verification are
785/785 assertions; the full exact and strict suites pass 55/55 and 59/59 tests,
including the independent `-Wall -Wextra -Werror` build. Final cold boot, 30 fps,
network, sound, and iOS-app completion remain open.

### r434-r439: measured affine sprite families clear; 30 fps still fails

r434 replayed r433 from 5.20 B to 5.80 B with one stationary contact at `(45,62)`.
All 66 scheduled reports were accepted and the device completed 94/94 queued reports,
but the old decoder rejected the first non-axis-aligned filtered sprite. There was no
Graphics Recovery Event; the guest simply waited on the missing 3D completion. Its
frame-meter result is therefore invalid as an animation measurement: 3/4 3D renders
completed and only two sampled signatures changed.

The retained object is not an arbitrary four-point warp. Its corners close to a
parallelogram within `0.000008` pixels, its perspective terms are zero, and its edge
vectors are orthogonal, positive-orientation, rigid 1:1 transforms of an 86x13 source.
The decoder now inverse-maps covered destination pixel centres into the existing clamped
16.16 bilinear kernel. It accepts only the direct sampler's rigid transform, rejects
shear/nonuniform scale/reversed orientation atomically, and leaves boundary-box pixels
outside the parallelogram untouched. Exact offline replay changed completion status from
zero to `0x4c`; only 288 visible pixels changed, all inside `(28,466)-(62,476)`, and the
resulting `Phone` label is coherent:

    work/r434-stationary-longpress-5800m/post-longpress-5800m.bin
      SHA-256 F08A241ECB9CF59BE262A13DED60ADABBC11E5393EF41E82DB32925C5A15E50E
    work/r434-derived-complete-affine-5800m/post-affine-5800m.bin
      SHA-256 DACC89F991EE3B6254ED32F9720669F1935335E601476B09B783DE791603E59F

A fresh full gesture in r437 then completed 119/120 3D renders and 28/28 2D commands
before exposing the same filtered label producer at uniform axis-aligned scale
`1.0066147904 x 1.0066146851`, with uniform vertex alpha `0xfc`. This measured form
justifies positive uniform scale for the modulated sampler; the alternate sampler remains
minification-only. Its exact replay raised `0x4c` and changed 423 visible pixels only
inside `(19,96)-(72,106)`:

    work/r437-affine-longpress-5800m/post-longpress-5800m.bin
      SHA-256 ED081E568793773E157BE3C4D7210AE6AA752F94FB7B4A066B8D49F5B64D9DB7
    work/r437-derived-complete-scale-5800m/post-scale-5800m.bin
      SHA-256 42A93B2BA8657657A9D1C70D39FD877DA97A14C5A5D876A34641C77A57A1D837

r438 continued from that derived completion and cleared 77/78 further renders before a
rotated, uniformly magnified instance of the same modulated producer. Its two edge scales
are `1.2500000003 x 1.2499997437`, its dot product is `0.0000162`, and its closure error
is below `0.000008` pixels. The implemented similarity path still rejects shear,
nonuniform scale, perspective and four-point warps. Offline replay raised `0x4c`, changed
671 visible pixels only inside `(14,102)-(80,114)`, and produced a coherent rotated
`Messages` label:

    work/r438-scale-resume-6000m/post-resume-6000m.bin
      SHA-256 263EAE5FB1AFB3D79F3D99F5622AD7F4C74E64B1D3FD3D9464F9065E253B9E42
    work/r438-derived-complete-similarity-6000m/post-similarity-6000m.bin
      SHA-256 62DCAD1ED31616A2ACEA486D3972476866E9DD06FF677FE44FAE2E19B96D25C7

r439 is the broad live check rather than another single-packet replay. From 6.00 B to
6.20 B it completed **78/78 2D commands** (7,499,000 bytes) and **508/508 3D renders**
(1,243,477 blended pixels), with zero decoder rejection, zero Graphics Recovery Event,
zero external-media failure and process exit zero. The terminal Home screen is coherent:

    work/r439-similarity-resume-6200m/post-resume-6200m.bin
      SHA-256 BCF33A2756C0B28A3DC4FFA47180AEFB6012E88E788BFE7664DBB826AFDE8C38
    work/r439-similarity-resume-6200m/w.img.screen.ppm
      SHA-256 C17DAC8E3F223AC2CD3B78AE5DE12BAC74E7EC3295A1AB971FC78FBA4BD69368

This is a substantial correctness boundary, not a performance success. r439 retired
3.485 M guest instructions/s over the meter's 57.396 host seconds. Of 1,006 live
publications, only 13 sampled signatures changed. Its 94 completed windows had mean
`0.260`, maximum `3.733`, 82 zero windows and none at or above 30 changed-publication
fps. The sparse signature can miss a small change but cannot invent one; these data do
not support a 30 fps claim. Focused exact and strict verification are 938/938 assertions;
the full exact and warnings-as-errors suites pass 55/55 and 59/59 tests. A final cold
boot remains mandatory after performance work. Network, sound and iOS-app completion
remain downstream of the still-open 30 fps priority.

### r440-r447: first-boot RSA separated from steady state; run-loop tick tax reduced

r439's rate and sparse updates were not a steady-state result after all. Sampling-only
`gprof` runs (link-time `-pg`, but no per-function entry instrumentation) showed
lockdownd spending 96% of its user samples in Security.framework giant-number
arithmetic. The first fully instrumented profile was discarded: `_mcount_private` and
`__fentry__` consumed 60.79% of its samples, so it measured the profiler. Disabling
ASLR and sampling an otherwise ordinary `-O2` build produced usable addresses.

The checkpoint names below preserve the run record, not the conclusions guessed while
they were made. r443's `keygen-complete` label was wrong, and r444 seeing no new entry to
`_isGiantPrime` did not prove the already-entered final operation had returned:

    work/r443-keygen-complete-6500m/post-keygen-6500m.bin
      SHA-256 8DF692FE21BA264C52A7EBFDF9A6C1D36745AA8E1F001142E651A3B3C40FE19B
    work/r444-keygen-probe-6800m/post-probe-6800m.bin
      SHA-256 5EE1ABF1ADB871498D6F4255F84E496910250EDD8D1D544B914397852955CD95

r445 is the first defensible post-keygen checkpoint. Its final 100 M-instruction sample
was 98.2% SpringBoard, with lockdownd absent:

    work/r445-keygen-finish-check-7100m/post-check-7100m.bin
      SHA-256 3E8FABD1DB75D8E843B68BE5A30846DA1B58C3791E097D0BBD5A8BBE4F36F2F5

r446 restored that checkpoint and applied one controlled 26-report swipe over 220 M
instructions. All 26 reports were accepted; the device completed 120/120 length/data
reads. Graphics completed **1,388/1,388 2D candidates** (130,348,168 bytes) and
**8,888/8,888 3D renders** (22,126,457 pixels), with zero decoder rejection, zero
Graphics Recovery Event, and zero storage failure. This is the evidence that ends the
packet whack-a-mole phase: the live family is broad and clean enough that inventing
another packet decoder without a rejection would be unjustified.

The valid steady-state frame result still fails the target. r446 published 1,531 sampled
scanouts and changed 228 signatures over 220 M instructions. Its 141 completed windows
had mean `3.088`, maximum `5.984`, and none at or above 30 changes/s. Changed signatures
were separated by 399,360 / 967,838 / 3,099,648 guest instructions (minimum / mean /
maximum). At a fixed cadence, the mean implies about **29.0 M guest instructions/s** for
30 changed scanouts/s. That is an inference from sampled scanout changes, not measured
iOS-device FPS. The desktop harness itself retired only 2.689 M instructions/s because
it includes tracing and host-side observation; it is not the app's execution rate.

The guest profile explains why another renderer packet is not the next lever. Its user
samples were SpringBoard QuartzCore render-tree, geometry, and command-building work
(`CA::Render::Updater::prepare_layer`, `prepare_layer0`, path conversion, transforms,
and `CA::RenderMBX2D::render_layer_bg`). Every observed MBX command completed. The r446
PPM was later losslessly converted for inspection. The apparent grid wobble is real and
is the guest's edit-mode animation: r433's non-edit icons occupy identical 57x57 boxes
at exact grid origins, while r446's independently shifted/rotated boxes extend by one or
two pixels and Calculator has moved into the dragged slot. This does not license wobble
outside edit mode. The fixed Settings crop is not darker numerically: its mean RGB is
`(136.10,136.90,138.02)` in r433 and `(136.32,137.10,138.20)` in r446. The crop is not
byte-identical because rotation changes its antialiased boundary, so those means are a
brightness check, not a claim that the two rendered icons are geometrically identical.

The remaining generic cost was measurable in `s5l8900_run()`. At commit `370b252`, its
assembly called the large public `s5l8900_tick(m, 1)` once per instruction. The accepted
change moves the identical observable device-refresh body into one private
`s5l8900_refresh()` function. This leaves the public converter small enough for GCC to
inline it into the run loop and constant-fold `ticks * tb_hz` into one add on the common
path, while the full refresh remains out of line. No device or external-input check is
deleted. A hand-written ratio-specialized alternative was tested first and rejected:
in its first controlled five-repetition build it ran at 24.50 M/s while the same
binary's literal public-tick row reached 25.88 M/s. Extra code and a second loop were a
loss, not an optimization.

Whole-suite timing then became thermally unstable, so it was not used to price the
accepted change. `insnbench --filter tick=run` now selects only the app-facing row. An
old `370b252` worktree and the candidate were built with the same GCC 15.2.0,
RelWithDebInfo flags, and identical benchmark source, then run in both orders at nine
repetitions of 20 M instructions:

| order | old median | refresh-split median | change |
|---|---:|---:|---:|
| old then new | 16.89 M/s | 18.26 M/s | +8.1% |
| new then old | 17.67 M/s | 18.92 M/s | +7.1% |
| mean of medians | 17.28 M/s | 18.59 M/s | **+7.6%** |

All 720 M benchmark instructions retired and passed their architectural end-state
checks. The differential machine test also compares `s5l8900_run()` with literal
`arm_step()+s5l8900_tick(1)` across the real ratio, fractional phase, 1:1 clocks, dirty
guest MMIO, a host-side button change, inverted and zero clocks, and an invalid phase;
the focused machine binary passes 5,832/5,832 assertions.

r448 is the real-guest semantics check. Both the old `370b252` bootkernel and the
refresh-split build restored r445, retired the same 2 M instructions, and stopped at
7.102 B with exit zero, 15,626 external-media reads, 469 writes, and zero failures. All
five retained outputs are byte-identical old versus new:

| artifact | bytes | SHA-256 (both builds) |
|---|---:|---|
| machine snapshot | 122,015,066 | `1E15E32EE78C2E5667B1A35B30328E05EF35CD46B2FA4609AFDF7C6E32625E67` |
| snapshot media image | 466,825,216 | `8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` |
| snapshot bridge state | 131,248 | `A3D8E6DC89261FD57A257FF098301331C4C6D447CCBF79B93DF74AE3A358B133` |
| live work image | 466,825,216 | `8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` |
| terminal framebuffer | 460,815 | `988BD8DC0625EA85A90A7A587C66F0C7418A8BEC2FD54E5EB66576AAB4406E72` |

The retained new checkpoint is
`work/r448-tick-refresh-ab/new/post-7102m.bin`. This proves the refactor did not
perturb this live guest trajectory. It does not measure the optimization: bootkernel's
debug loop intentionally uses literal `arm_step()+s5l8900_tick(1)` so it can inject and
observe at exact instruction boundaries.

This is a real generic interpreter-loop improvement, not 30 fps. Applying 7.6% to the
older reported 25 M/s would yield only about 26.9 M/s, and that extrapolation crosses
hosts, compilers, and workloads. An exact iOS build and the eventual final cold boot are
still required. Network, sound, and app completion remain behind the unpassed graphics
gate.

### r449-r454: packed NZCV wins the microbenchmarks and loses the real-guest gate

r450's sampling-only Thumb benchmark put `alu_add` at 9.44% of samples. The old
helper updates N, Z, C, and V through four `set_flag()` read/modify/write decisions,
so replacing those with one masked CPSR write was a plausible generic interpreter
optimization. A new boundary suite checked ADDS, ADCS, SUBS, SBCS, MOVS, and
ANDS results and exact flags while seeding Q, GE[3:0], A, E, I, F, V, and the mode;
both exact and warnings-as-errors builds passed 1,010/1,010 assertions.

The first packed implementation looked excellent in the workload that suggested it.
Twenty order-balanced pairs of 20 M instructions raised the flag-heavy Thumb median
from 41.915 to 45.335 M/s (+8.2%; median paired delta +8.8%). It also made GCC inline
the flag calculation everywhere, however, growing host text by 1,116 bytes and slowing
the decoder-scattered, no-MMU mixed row. r452 therefore kept the packed helper inline
only where Thumb flags are unconditional and left ARM's dynamic-S wrapper out of line.
That reduced the text growth to 636 bytes and made `exec_data_processing()` 16 bytes
smaller than baseline. Twelve order-balanced 20 M-instruction pairs per row gave:

| benchmark row | pushed `9242a16` | r452 | independent-median change | paired-median change |
|---|---:|---:|---:|---:|
| Thumb ALU, MMU off | 47.635 M/s | 49.675 M/s | +4.3% | +4.1% |
| Thumb ALU, 4 KiB MMU + tick | 32.785 M/s | 36.520 M/s | +11.4% | +10.5% |
| mixed ARM, MMU off | 30.635 M/s | 29.995 M/s | **-2.1%** | **-1.1%** |
| mixed ARM, 4 KiB MMU + tick | 23.770 M/s | 24.270 M/s | +2.1% | +2.4% |
| load/store through `s5l8900_run()` | 24.910 M/s | 25.980 M/s | +4.3% | +5.3% |

Those are real synthetic gains, including the regression; they are not evidence that
the live guest becomes faster. r453 tested that distinction. The pushed baseline and
r452 each restored the same r448 checkpoint and retired the same 100 M instructions
twice, in old-new then new-old order. Every run exited zero with 15,626 external-media
reads, 469 writes, and zero failures. All four copies of every retained artifact are
byte-identical:

| artifact | bytes | SHA-256 (all four runs) |
|---|---:|---|
| machine snapshot | 122,015,066 | `7DC65F9C627EAF4EAA687794A2474FD74ED3D03A469A5DCFB2EEB54C658CE79E` |
| snapshot media image | 466,825,216 | `8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` |
| snapshot bridge state | 131,248 | `A3D8E6DC89261FD57A257FF098301331C4C6D447CCBF79B93DF74AE3A358B133` |
| terminal framebuffer | 460,815 | `1C8A5E2BA060F35424B643D4F93AA6CBE55572E8BEEA78AB2A3696198B35F581` |

The raw r453 times appeared to favour r452: old was 77.897 and 58.323 seconds while
new was 65.042 and 60.239 seconds. That apparent roughly 8% median improvement is
**discarded**. The monotonic host/cache warm-up is larger than the proposed effect;
the first old run occupied the uniquely cold position and the final old run beat both
new runs.

r454 is the performance gate. After one discarded warm-up per binary, it ran eight
independent 50 M-instruction restores in the symmetric order
`old,new,new,old,new,old,old,new`. Old therefore occupied positions 1/4/6/7 and new
2/3/5/8; both have the same mean order position, cancelling a linear host drift.

| build | elapsed seconds | mean |
|---|---|---:|
| pushed `9242a16` | 20.969, 19.342, 20.295, 19.389 | **19.999** |
| packed NZCV r452 | 20.232, 19.463, 20.364, 19.896 | **19.989** |

The time difference is +0.05%, effectively zero; averaging the individually derived
rates instead gives old 2.5029 versus new 2.5022 M/s, fractionally in the opposite
direction. Setup, tracing, device work, and reporting are included identically in both,
so neither calculation supports a live-guest speed claim. The packed implementation
was therefore rejected and removed. Its independent CPSR preservation tests remain as
correctness coverage, but production flag handling is unchanged from `9242a16`.

This failed hypothesis is useful: a hot function in a flag-saturated synthetic Thumb
loop can show a reproducible 4-11% local win and still contribute nothing measurable to
this SpringBoard interval. The retained semantics checkpoint is
`work/r453-real-guest-flags-ab/pair2-new/post-7202m.bin`. It is a checkpoint, not a
30 fps result. The 30 fps target, current-device measurement, and final cold boot all
remain open.

### r455-r457: measure the app loop, not bootkernel's observers

r455's first real-guest sampling profile exposed a flaw in the performance gate above.
Even with `--fast`, 48.51% of samples landed in `bootkernel`'s `main`, including
unconditional PC naming and diagnostic bookkeeping around every instruction. That tool
loop is valuable when the requested evidence needs instruction boundaries, but its
elapsed time is not the app's interpreter time. Consequently r453/r454 remain valid
semantic comparisons and a valid rejection of packed NZCV in that harness; their raw
2.5 M-instruction/s rates do **not** describe `VMEngine`.

`bootkernel --run-api` closes that measurement error. It calls the public
`s5l8900_run()` entry point in at most 100,000-instruction chunks, exactly matching the
iOS app's current chunk size, and times only those calls. It retains CPU, MMU, device
tick, framebuffer, MBX, and external-media execution. A pending snapshot shortens the
next chunk, so an absolute `--snapshot-at` boundary stays exact and checkpoint I/O is
outside the timed calls. At this checkpoint, features that required an instruction
callback were refused rather than silently lost: scheduled input, HLE, frame metering,
call probes, PPP, stop-on-abort, and the per-instruction diagnostic windows. Default NAT
remained accepted because it is inert while PPP is off; the first live preflight
incorrectly rejected that ordinary configuration, and its regression test now fixes
the distinction. r461 later moved frame metering onto exact app-shaped chunk
boundaries; the other refusals remain.

r456 restored r448's 7.102 B checkpoint and retired 100 M instructions through this
path. It made 1,000 calls in 6.371434 timed seconds, or **15.695053 M instructions/s**,
and stopped exactly at 7.202 B with exit zero, 15,626 external-media reads, 469 writes,
and zero failures. The media image, bridge sidecar, live work image, and framebuffer
are byte-identical to r453's literal-step reference:

| artifact | SHA-256 |
|---|---|
| snapshot media / live work image | `8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` |
| snapshot bridge state | `A3D8E6DC89261FD57A257FF098301331C4C6D447CCBF79B93DF74AE3A358B133` |
| terminal framebuffer | `1C8A5E2BA060F35424B643D4F93AA6CBE55572E8BEEA78AB2A3696198B35F581` |

The machine snapshot is deliberately not called byte-identical. A byte comparison
found differences only in serialized TLB counters and the final snapshot checksum:
the diagnostic reference has 838,477,965 hits / 18,170,600 misses / 138,584 flushes,
while `--run-api` has 838,433,853 / 18,170,596 / 138,584. The extra 44,112 hits and
four misses came from diagnostic observer translations; they are measurements, not
architectural guest state. The `--run-api` snapshot itself is retained at
`work/r456-run-api-equivalence/post-7202m.bin` with SHA-256
`0AF1F236B5F817C81ABE10664B545F1B6EB2DAA0144D2FC1BF29BFA3DCB875E4`.

r457 then restored the same 7.102 B state and sampled 500 M instructions through the
new path. Its 5,000 calls took 34.314912 timed seconds, or **14.570925 M
instructions/s**. The lean flat profile is no longer dominated by the tool:

| function | sampled time |
|---|---:|
| `arm_step` | 34.39% |
| `exec_data_processing` | 7.16% |
| `vfp_execute_inner` | 5.81% |
| `arm_cond_passed` | 5.58% |
| `mbx_execute_textured_sprite` | 5.34% |
| `s5l8900_run` | 5.25% |
| `thumb_step` | 5.07% |
| `arm_mmu_translate` | 4.09% |
| `alu_add` | 1.49% |
| `s5l8900_refresh` | 0.84% |

This profile explains the packed-NZCV null result: even eliminating `alu_add` entirely
could recover only 1.49% of this interval. It also selects a more plausible generic
next test: the current ARM loop calls `arm_cond_passed()` for every ordinary ARM
instruction, including unconditional `AL` instructions, and the helper extracts all
four flags before dispatching its condition.

No FPS was measured in r456 or r457 because `--run-api` intentionally has no scanout
observer. Dividing 14.570925 M instructions/s by r446's 0.967838 M-instruction mean
changed-scanout gap projects roughly **15.1 changes/s**, but that crosses runs and is
only a throughput-derived estimate. The latest direct r446 measurement remains 3.088
mean / 5.984 maximum changes/s in the instrumented desktop harness. Neither number is
iOS-device FPS, neither proves the current app cadence, and neither meets 30. A direct
app-equivalent publication measurement and the final cold boot remain required.

### r458: do not decode CPSR flags for unconditional ARM instructions

r457 selected this test from the live profile rather than from a synthetic hot loop.
`arm_step()` used to call `arm_cond_passed()` for every ordinary ARM instruction. For
the common `AL` condition, that helper still extracted N, Z, C and V and entered a
16-way switch only to return true. r458 bypasses the helper when `cond == 0xe`; the
`cond == 0xf` unconditional instruction space is still intercepted earlier, and every
conditional instruction follows the old path unchanged.

Both local configurations rebuilt cleanly. The exact configuration passed 55/55 tests
and the stricter JIT-enabled configuration passed 59/59. That is necessary semantic
coverage, not a speed result. The live gate restored r448's same 7.102 B SpringBoard
checkpoint four times, retired 200 M instructions per restore through `--run-api`, and
used the symmetric order `old,new,new,old` to expose host drift:

| order | build | timed seconds | rate |
|---:|---|---:|---:|
| 1 | pushed baseline | 13.002677 | 15.381448 Minsn/s |
| 2 | r458 | 12.447926 | 16.066934 Minsn/s |
| 3 | r458 | 13.052484 | 15.322754 Minsn/s |
| 4 | pushed baseline | 13.960006 | 14.326641 Minsn/s |

Absolute host speed visibly fell across the sequence, so the fastest single number is
not the claim. r458 won both neighbouring comparisons: +4.4566% in the first pair and
+6.9529% in the reverse-order pair. Mean rates were 14.854045 Minsn/s old versus
15.694844 Minsn/s new, a **+5.6604%** gain. The timer covers only the 2,000 public
`s5l8900_run()` calls in each run; setup, reporting and checkpoint I/O are excluded.
Every run stopped exactly at 7.302 B, exited zero, and reported zero external-media
failures.

The first old/new pair also wrote complete end checkpoints. All retained artifacts are
byte-identical across the code change:

| artifact | SHA-256 |
|---|---|
| machine snapshot | `9321F91D5F40CA0380484763E91CB4318E7A6188B6F4170432AE08B2D06FA9E2` |
| snapshot media / live work image | `06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D` |
| snapshot bridge state | `C152E63315BE0F62ECB81E55C2A1B9CE7394708360AB00069210A7DCD7969CB8` |
| terminal framebuffer | `E7A1487C8546EED5CA61542DB473DDDA67EA3CC56715E83DE6C09D5C044A1F36` |

The retained candidate checkpoint is
`work/r458-cond-fast-ab/new1/post-7302m.bin`. This is a repeatable generic interpreter
win on a live guest, not a frame-rate result. In particular, desktop instruction
throughput does not include the iOS app's framebuffer conversion, main-thread
publication, or device-specific host speed. A user-observed 0–4 fps phone result must
not be replaced by a desktop-derived estimate; the app needs its own low-overhead
execution/publication measurements. The 30 fps target and final cold boot remain open.

### r459-r460: a smaller opcode-class dispatcher is substantially slower

r459 repeated the sampling-only live profile after r458. It restored the same 7.102 B
checkpoint, retired 300 M instructions through the app-equivalent loop, stopped exactly
at 7.402 B with zero failures, and measured 13.626505 Minsn/s in the profiling build.
The important result is the new distribution, not that build-specific rate:

| function | sampled time after r458 |
|---|---:|
| `arm_step` | 32.96% |
| `exec_data_processing` | 8.43% |
| `vfp_execute_inner` | 5.67% |
| `s5l8900_run` | 5.24% |
| `arm_mmu_translate` | 5.17% |
| `mbx_execute_textured_sprite` | 4.75% |
| `thumb_step` | 3.76% |
| `arm_cond_passed` | 2.27% |

The AL bypass therefore did what its source change says: condition checking fell from
5.58% to 2.27%, while the body of `arm_step` remained the largest target. r460 tested
the principled-looking next rewrite already named in this file: dispatch first on ARM
instruction bits 27--25, sending the seven non-overlapping classes directly to their
existing executors and leaving only class 000 in the old overlap chain.

The rewrite was semantically clean. Exact and strict builds passed 55/55 and 59/59
tests. GNU `nm` measured `arm_step` shrinking from 11,568 to 10,416 host-code bytes. A
200 M-instruction candidate run also produced byte-identical machine, media, bridge,
work-image and framebuffer artifacts at 7.302 B; their hashes are the r458 hashes above.

The short performance gate was inconclusive. In `old,new,new,old` order its two paired
deltas were -0.1514% and +4.2687%; the symmetric means suggested +2.1213%, but a result
that loses one pair is not repeatable evidence. A fixed-affinity follow-up was discarded
after the first baseline collapsed to 7.563682 Minsn/s and the following candidate ran
12.810375 Minsn/s. Pinning one logical processor exposed host scheduling/frequency noise;
it did not control it.

The longer gate retired 500 M instructions per restore over the same 7.102--7.602 B
SpringBoard interval. This was decisive:

| order | build | timed seconds | rate |
|---:|---|---:|---:|
| 1 | r458 baseline | 40.411359 | 12.372759 Minsn/s |
| 2 | opcode-class switch | 56.259667 | 8.887361 Minsn/s |
| 3 | opcode-class switch | 46.878429 | 10.665886 Minsn/s |
| 4 | r458 baseline | 38.525074 | 12.978560 Minsn/s |

The candidate lost both long pairs, by 28.1699% and 17.8192%. Its mean was 9.776624
Minsn/s against 12.675660 for the baseline, a **22.8709% regression**. The shorter
200 M interval simply did not include the full instruction mix that exposed it. Smaller
host code is not faster code here; the indirect class dispatch is worse than the host
branch predictor's handling of the deliberately ordered comparisons. That explanation
is an inference, while the regression is measured.

The class switch was removed in full. No production decoder change remains from r460.
This closes the top-level-switch hypothesis rather than leaving another marginal layer
in `arm_step`. Reaching the phone's 30 fps target now requires a structural change that
also removes repeated work, such as a semantics-checked decoded-basic-block interpreter;
rearranging the same per-instruction comparisons is not enough.

### r461-r464: measure publication on the app-shaped fast loop

The earlier throughput-to-FPS estimate crossed two runs with different observers. r461
removes that avoidable uncertainty. `--run-api --frame-meter` now keeps the app's exact
100,000-instruction `s5l8900_run()` chunks, shortens a chunk when a meter boundary is
next, and polls only after that exact boundary. It still refuses every feature that
really needs per-instruction observation. The option contract has a regression test,
and the report distinguishes exact app-shaped boundaries from the older diagnostic
loop's at-most-1,023-instruction quantisation.

All four measurements restored
`work/r445-keygen-finish-check-7100m/post-check-7100m.bin`, ran the experimental MBX
machine from 7.100 B through 7.320 B retired instructions, and exited zero with no
external-media failure and 2/2 raw-media redirects/completions. The order bracketed
each metered run with a nearby no-meter run, but it was not a fully symmetric benchmark:

| run | frame observer | timed `s5l8900_run()` calls | full meter span | publications / changed | changed-FPS min / mean / max |
|---|---|---:|---:|---:|---:|
| r461 | on | 16.874757 s / 13.037225 Minsn/s | 17.020960 s / 12.925 Minsn/s | 446 / 193 | 9.435 / 11.449 / 13.572 |
| r462 | off | 16.170016 s / 13.605429 Minsn/s | not measured | not applicable | not applicable |
| r463 | on | 17.792820 s / 12.364538 Minsn/s | 17.950490 s / 12.256 Minsn/s | 470 / 193 | 5.729 / 10.816 / 13.954 |
| r464 | off | 16.503285 s / 13.330679 Minsn/s | not measured | not applicable | not applicable |

Neither metered run had a window at or above 30. Both nevertheless counted exactly 193
changed signatures. Their mean instruction gaps were 1,143,750.0 and 1,143,229.2, a
stable mean of **1,143,489.6 retired instructions per detectable change** despite host
wall-time drift. If that guest-instruction cadence remains fixed, 30 changes/s projects
to **34.304688 Minsn/s**. Against the two full metered rates' 12.5905 Minsn/s mean, the
remaining desktop factor is about **2.725x**. This is a linear capacity projection, not
a promise: a structural optimisation can also change guest timing, and the sampled
signature can miss a small unsampled update.

The observer is not free. The adjacent metered/control core-rate gaps were 5.00% and
8.06%; host drift and cache state were not controlled well enough to call all of that a
causal meter penalty. Within the metered runs, time outside the timed core calls was
0.146203 s (0.859% of the full span) and 0.157670 s (0.878%). Those two facts bound the
measurement distortion honestly: it is measurable and noisy, but it cannot explain a
2.725x gap.

The checked guest outputs rule out a functional divergence in this interval. All four
live work images have SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D`; all four final
PPMs have SHA-256
`E357B88F7DCB1533B88A62DADF85D97FB8BCCF1EA54520B5AECE048C18B5D6D9`. No end-machine
snapshots were written, so this is **not** a claim that every diagnostic counter or
serialized byte of machine state was compared.

Finally, these are desktop bootkernel changed-scanout measurements. They exclude UIKit
conversion/drawing and are not measurements from the user's iPhone. The reported
roughly 0--4 fps phone result remains the only real-device result and therefore remains
the product truth until an exact IPA reports both Minsn/s and changed-frame FPS on a
fresh software-render machine and a fresh MBX machine. The final graphics acceptance
still requires a cold boot.

### r465-r469: build flags do not close the gap; steady state is ARM; runner batching loses

The app was already ahead of the ordinary desktop comparison in one mundane way:
`app/project.yml` compiles the core at `-O3` with LLVM LTO. A separate GCC Release +
LTO build therefore tested whether r461/r463's `-O2` desktop meter understated the
available interpreter speed. All 55 exact tests passed. In the first adjacent restored
pair, the same 100 M-instruction interval improved from 13.866741 to 15.148466 Minsn/s
(+9.24%). The reverse pair cannot supply a symmetric multiplier: the LTO repeat stayed
at 15.150482 Minsn/s while the following `-O2` process collapsed to 10.813955 Minsn/s.
That is host drift, not a 22.8% optimisation claim.

The direct LTO frame run makes the practical conclusion unambiguous. Over the same
7.100--7.320 B MBX interval it retired 11.368339 Minsn/s inside the core calls and saw
501 publications, 193 changed signatures, mean 9.932 changed fps, maximum 13.996, and
zero windows at 30. Its immediate no-meter control was 12.499616 Minsn/s; both were in
a slow host period. LTO is already in the iOS app and ordinary build optimisation does
not plausibly supply the remaining roughly 2.725x desktop factor.

r468 then counted every literal-runner entry in a 10 M-entry restored steady-state
interval rather than carrying forward the early-kernel mix from `docs/dynarec.md`:

| state / mode at runner entry | entries | share |
|---|---:|---:|
| ARM state | 8,644,952 | **86.44952%** |
| Thumb state | 1,355,048 | 13.55048% |
| User | 6,713,186 | 67.13186% |
| SVC | 2,898,732 | 28.98732% |
| IRQ | 384,873 | 3.84873% |
| FIQ / ABT / UND combined | 3,209 | 0.03209% |

The counts sum to exactly 10,000,000. r472 later found that 511 of those entries took a
pending interrupt before fetching an instruction; one of the 511 arrived while CPSR.T
was set. The fetched mix is therefore 8,644,442 ARM and 1,355,047 Thumb instructions,
86.44884% and 13.55116% of 9,999,489 fetches. That correction is tiny but real. The
interval describes post-keygen SpringBoard/MBX rather than the whole boot, and its
architecture decision remains unchanged: a portable block interpreter must prioritise
ARM first. Building Thumb first from the old 68.95% early-kernel sample would optimise
the wrong phase.

r469 tested the prerequisite that looked cheapest: batching the public runner's device
ticks only to the next mathematically exact timebase edge. Guest MMIO and host input
ended a batch at the exact instruction; inverted, zero and invalid clock shapes retained
the one-step path; WFI first received every preceding retirement before asking devices
for a wake edge. The expanded differential suite passed 5,844 assertions. Correctness
was not the rejection reason.

Performance was. Separate-process `old,new,new,old` medians moved with the host
(14.56, 14.47, 14.73, 13.43 Minsn/s), so they do not support a causal delta. The useful
control is inside `insnbench`, which interleaves the app-facing `tick=run` row with the
literal `arm_step` + `tick(1)` row in the same process:

| binary | literal tick row | app-facing runner | runner versus literal |
|---|---:|---:|---:|
| pushed `9f81e86` | 10.61 Minsn/s | 11.49 Minsn/s | **+8.3%** |
| r469 batch prototype | 13.90 Minsn/s | 13.40 Minsn/s | **-3.6%** |

The absolute columns cannot be compared across sessions because the no-tick controls
also moved from 13.49 to 16.92 Minsn/s. The within-session reversal is enough: the
prototype gave back roughly eleven percentage points relative to its own control.
Disassembly showed a much larger runner, a stack-resident pending count updated every
instruction, thread-local WFI coordination, and a variable-count tick call. That is a
plausible mechanism; the regression itself is measured. The implementation and its test
additions were removed completely.

This closes outer-loop tick batching as an independent speed fix. The current live
profile gives `s5l8900_run` 5.24% and the refresh 0.84%, so even deleting both cannot
bridge 2.725x. The next implementation must remove repeated work inside `arm_step`
across multiple instructions: an ARM-first, semantics-checked decoded-basic-block
interpreter whose blocks end before WFI, device-observable memory, state changes and
the exact tick/interrupt boundary. That is structural work, not another decode-chain
reordering, and it still earns its place only by passing differential, restored-real-
guest, and eventual cold-boot gates.

### r470-r471: the first portable decoded-block interpreter is rejected

The next prototype implemented the structural idea above rather than another decoder
reordering. It was compile-time experimental and default-off, emitted no host code, and
kept the literal runner in the same binary. Blocks were ARM-only and contained ordinary
data processing plus an optional terminal B/BL. Thumb, memory, WFI, CP15, state changes,
exceptions and host callbacks stayed on `arm_step`. The machine capped every block at
the next exact timebase edge and forced one-step execution for dirty device state,
changed external input and unusual clocks. Entries were tied to resolved host RAM and
validated against the live raw words, so self-modifying code could not execute a stale
decode.

The correctness work was real but did not rescue the performance result. The expanded
SoC differential exercised mid-block tick cuts, flags and conditions, a masked pending
IRQ and a modified cached instruction; it and the snapshot suite passed. Both 100 M-
instruction restored A/B series exited zero. In r471 all four writable media images were
byte-identical at SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`, and all four
final PPMs were byte-identical at
`2EAE64986CFA1A34E25FC0D67DACC9E220E2262E9214BE2DAD775FD4518B266A`. No end-machine
snapshots were written, so this is not a claim that derived counters matched.

The first in-process synthetic control already showed the wrong shape:

| app-shaped row | blocks | literal control | delta |
|---|---:|---:|---:|
| five-instruction ALU/branch | 25.65 Minsn/s | 21.26 Minsn/s | **+20.6%** |
| matched load/store | 12.88 Minsn/s | 16.83 Minsn/s | **-23.5%** |

Caching proven one-word/negative starts removed repeated full block probes. In the next
30 M x 9 interleaved session the ALU pair was effectively tied (17.95 versus 17.93) and
the load/store loss shrank but remained (10.78 versus 11.34, **-4.9%**). Absolute rates
moved badly with host load; only the paired direction is used here.

The restored SpringBoard interval was decisive. Every arm restored
`post-check-7100m.bin`, used the app's 100,000-instruction `s5l8900_run()` chunks, and
stopped at 7.200 B:

| series | block arm | adjacent literal arm | delta |
|---|---:|---:|---:|
| r470 first bracket | 6.400148 | 11.404148 | **-43.9%** |
| r470 reverse bracket | 10.771980 | 13.371798 | **-19.4%** |
| r471 host-key first bracket | 10.843156 | 13.978594 | **-22.4%** |
| r471 host-key reverse bracket | 9.541273 | 11.333318 | **-15.8%** |

The unit is Minsn/s. Host drift explains the absolute spread, not four same-direction
losses. r471 attempted 40,811,597 block lookups, hit only 6,770,588 (**16.59%**), built
34,041,009 entries and retired just 31,832,490 instructions in blocks (**31.83%**) at
2.68 instructions per successful call. Removing the global TLB generation from the
decoded key was correct--translation still resolved the host block and every hit still
validated raw words--but cut builds by only 51,215 (**0.15%**). The hypothesis that
normal address-space switches were destroying most reuse was therefore refuted.

The remaining miss mechanism was not isolated, and guessing a larger cache would be
cache-size whack-a-mole. What is established is enough to reject this shape: it pays
lookup/build/validation costs across the whole runner, accelerates less than one third
of the real interval, and calls existing per-instruction semantic helpers inside very
short blocks. The implementation, runtime switch, counters and differential additions
were removed completely. The matched ALU `tick=run` benchmark stays because future
runner work must report both it and load/store instead of choosing whichever flatters
the change.

### r472-r473: full dynamic sequences explain why the narrow block shape lost

The first block prototype established its own low coverage, but not the instruction
stream around the misses. r472 adds an explicit `bootkernel --sequence-profile` mode
and reruns exactly 10 M literal steps from
`work/r445-keygen-finish-check-7100m/post-check-7100m.bin`. The observer resolves the
live fetch mapping before `arm_step()`, distinguishes pending-interrupt entry from an
actual fetch, and aggregates physical sites, raw encodings, raw pairs, classes and run
lengths in host memory. It implies `--fast` and refuses `--run-api` and
`--frame-meter`: its extra translation perturbs the software TLB and host time, so its
throughput is deliberately not evidence.

The accounting is exact: 10,000,000 entries were 9,999,489 fetched instructions plus
511 pending-interrupt entries, with zero fetch failures. The fetched instruction mix
was:

| class | fetched instructions | share |
|---|---:|---:|
| ordinary ARM data processing, not writing PC | 3,013,148 | **30.133%** |
| ARM single load/store | 1,921,650 | **19.217%** |
| ARM VFP | 1,709,378 | **17.095%** |
| Thumb | 1,355,047 | **13.551%** |
| ARM B/BL | 1,201,691 | **12.018%** |
| ARM block load/store | 397,373 | 3.974% |
| ARM other | 204,915 | 2.049% |
| coprocessor / extra-sync / multiply / media / SVC / unconditional / DP-to-PC | 196,287 | 1.963% |

r473 then split the mixed classes at the exact boundaries a conservative second
prototype can use without a code-page write-generation scheme. Single transfers were
1,369,860 loads and 551,790 stores, with 44,249 loads writing PC. Block transfers were
212,290 loads and 185,083 stores, with 111,453 loads including PC. VFP was 796,804
compute, 248,014 register-transfer, 491,878 memory-load and 172,682 memory-store
instructions.

Allowing safe DP, multiply/media, VFP compute/register/load, and non-PC single/block
loads produces 6,025,193 body instructions (**60.255%**) in 1,999,450 runs averaging
3.013 and reaching 68. A branch followed 1,057,409 of those runs and a store followed
635,262. If each is executed as a terminal instruction, the static upper bound becomes
7,717,864 fetched instructions (**77.183%**) at 3.860 instructions per candidate call.
Runtime MMIO loads, exceptions, state changes, timebase edges and cache misses can only
lower those numbers. This is materially broader than r471; it is not a prediction that
the broader engine will win.

This is the direct explanation for r470-r471. Its safe class covered only 30.13% of
fetches. Those data-processing instructions formed 1,842,492 runs averaging just 1.635
instructions; 87.60% of them were in runs of four or fewer. A terminal B/BL followed
857,973 runs. That shape is consistent with the rejected engine retiring only 31.83%
in blocks at 2.68 instructions per successful call. It was paying a whole-run lookup
tax to accelerate short islands occupying less than one third of the stream.

The broader stream has a very different shape. There were 8,717,390 sequential virtual
edges (87.178% of fetched edges), and 8,710,945 were also physically contiguous.
Sequential-PC runs averaged 7.799 instructions, reached 260, and 82.64% of fetched
instructions belonged to runs of at least five. The leading sequential class pairs
were VFP-to-VFP (15.948% of all sequential edges), DP-to-DP (13.429%), Thumb-to-Thumb
(12.945%), load/store-to-DP (9.857%), DP-to-B/BL (9.842%), load/store-to-load/store
(8.531%) and DP-to-load/store (6.559%). A second portable engine cannot remain DP-only:
it must span the exact VFP and RAM load/store semantics that separate those islands
while retaining an interrupt/tick boundary at every retirement and stopping safely for
control/state changes, MMIO, callbacks and self-modifying-code hazards.

The working set also prevents a different bad conclusion. The profile saw only 29,000
exact physical instruction sites, 11,444 raw encodings and 20,671 sequential raw pairs;
none of the aggregate tables dropped an entry. For the 3,013,148 safe-DP accesses there
were 6,690 distinct physical sites. A deliberately simpler direct-mapped *site*
simulation hit 69.85% at 1,024 entries, 88.75% at 4,096 and 96.55% at 16,384. Those are
not the rejected engine's block-call hit rates and do not justify resizing its cache.
They do establish that its measured 16.59% hit rate cannot be attributed to the guest's
instruction-site working set alone; any future cache must instrument its actual miss
causes before its size is changed.

Nor does the profile justify narrow site-specific fusion. The hottest exact site was
0.984% of fetches and the hottest 24 sites together were only 5.687%. The hottest raw
pair was 1.129% of sequential edges. There is no handful of PCs or pairs here with a
credible 2.7x desktop ceiling. The next justified prototype is broad, long-lived
predecode across the dominant mixed ARM sequences, not another special case.

The observer did not change the checked guest outputs. r468, r472 and r473 all stopped at
7.110 B with final pre/post PCs `0x312092b8`/`0x312092bc`, zero external-media failures,
byte-identical 466,825,216-byte work images at SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`, and byte-identical
460,815-byte final PPMs at
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`. No end snapshots
were written, and the profiler intentionally changes derived TLB counters, so this is
not a serialized-machine-state equality claim.

### r474-r479: the broad mixed ARM block engine is correct and still slower

r473 justified exactly one broader portable prototype. It was built behind a default-off
compile gate with a same-binary runtime switch, not slipped into the shipping interpreter.
The cache was 4-way/4,096-entry, capped blocks at 16 A32 words, validated every live raw
word on a hit, stopped at the 1 KiB fetch-permission boundary, treated every branch and
store as terminal, exited immediately after an MMIO access raised `level_dirty`, and let
the machine cap every call at the next exact timebase edge. Its uops called the existing
DP, multiply, media, load/store, VFP and branch semantic helpers. A dedicated same-binary
differential suite passed 74/74 checks, the complete gated build passed 56/56 tests, and
the unchanged default-off strict/JIT build passed 59/59.

The first timing pair, r474-r475, is **invalid as an engine comparison** and is retained
to prevent its attractive false result being reused. The default `bootkernel` diagnostic
loop calls `arm_step()` directly so it can observe every boundary; both runs reported
zero block calls. Their 71.36 s versus 12.81 s wall times were cold/warm host setup and
file-cache noise, not a 5.57x emulator gain. `--run-api` is the app-facing path that
actually calls `s5l8900_run()` and times only its 100,000-instruction chunks.

The valid restored results, all from the exact r445 7.100 B checkpoint through 7.110 B,
were:

| run | path | rate | change versus r476 |
|---|---|---:|---:|
| r476 | same binary, engine disabled | 14.607785 M/s | control |
| r477 | broad mixed engine | 13.501772 M/s | **-7.57%** |
| r478 | skip Thumb dispatch; execute classified one-uop/terminal entries | 13.559337 M/s | **-7.18%** |
| r479 | same binary, engine disabled, repeated last | 15.535040 M/s | +6.35% control drift |

The two controls moved with host frequency/load, but both were faster than both engine
runs; the engine runs themselves differed by only 0.43%. r478 made 3,058,496 engine
calls and retired 8,136,439 of the 10 M instructions through cached uops (**81.364%**),
including 2,111,246 terminal instructions. It recorded 2,852,633 cache hits, 205,863
misses/builds, 482,821 negative hits, 201,767 replacements, 28,137 MMIO exits, 2,096,905
control exits and 508,003 literal fallbacks. This is not another low-coverage failure:
predecode covered most of the interval and still lost. Cache lookup, raw validation,
uop dispatch and calls back into the same per-instruction semantic helpers cost more than
the removed fetch/top-level-decode/runner work.

Correctness checks passed at the available boundary. Every valid A/B retired exactly
10 M instructions with status OK, final PC/CPSR `0x312092bc`/`0x20000010`, zero external
media failures, byte-identical 466,825,216-byte work images at SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`, and byte-identical
460,815-byte PPMs at
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`. No end snapshot was
written, so this is not a whole-serialized-machine equality claim.

The broad engine, gate, cache, counters and tests were removed in full. r472-r473's
profiler remains because it answers a real workload question. The rejected conclusion
is now stronger: a third portable block cache that merely predecodes more instructions
and calls the same interpreter helpers is not a credible route to 30 FPS. Another
attempt requires a materially different way to remove semantic-helper and dispatch work,
plus the same restored and eventual on-device/cold-boot gates. Current desktop cadence
and the phone's observed 0-4 FPS are unchanged by this rejected experiment.

### r480-r481: the iOS binary is optimized, but the measured renderer was not named

r480 audited the exact hosted iOS artifact at commit
`3a8f11b292d3a647172b40785d314ef562069c1e`, rather than inferring its build mode from
project files. The archive used the Release configuration, the device `iphoneos` SDK and
the arm64 architecture; the compiler response and scan commands contained `-O3` and
`-flto`. The 0-4 FPS seen on the phone therefore cannot honestly be blamed on an
accidental Debug or stale `-Os` build.

The app already keeps raw interpreter throughput and visual cadence separate. It calls
`s5l8900_run()` in 100,000-instruction chunks, publishes at most 30 status updates per
second, reports raw Minsn/s, and counts a frame only when its sampled framebuffer
signature changes. That change test happens on the emulator thread before the UIKit
presentation copies. The presentation path is not free, but it cannot explain away a
counter that already observed only 0-4 changed guest frames per second.

There is a more important comparability problem: a new app machine defaults to the CPU
software renderer (`mbx=false`, `ca-software-render=true`). The desktop restored cadence
used MBX. Selecting **Experimental MBX** in Settings affects machines created afterward;
the choice is written into the new work image, so changing the setting does not convert
an existing machine. The phone's 0-4 FPS remains the real user-visible result, but it is
not a valid measurement of the MBX path until a genuinely new Experimental-MBX machine
reports both Minsn/s and FPS. MBX is not made the default on this evidence alone: it still
owes a real-device run and the mandatory final cold-boot acceptance.

r481 tested one profile-backed but deliberately small core change while waiting for that
device split. All valid VFP groups, 17.095% of the measured restored stream, were checked
before the long block/extra/exclusive/SWP decode tail while retaining the same
`vfp_execute()` semantics. The full default suite passed 55/55. Four isolated 100 M
restored arms from 7.100 B to 7.200 B produced:

| bracket | original order | VFP-first order | VFP-first delta |
|---|---:|---:|---:|
| first | 17.328997 M/s | 17.494201 M/s | +0.95% |
| reverse | 16.549839 M/s | 17.050470 M/s | +3.02% |
| arithmetic mean | 16.939418 M/s | 17.272336 M/s | **+1.97%** |

Every valid arm retired exactly 100 M instructions with status OK and zero external-media
failures. All work images matched SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`; all final PPMs
matched `2EAE64986CFA1A34E25FC0D67DACC9E220E2262E9214BE2DAD775FD4518B266A`.
One earlier invocation never ran because the harness correctly refused to overwrite an
existing work image, and is excluded rather than presented as a failed candidate run.

The dispatch reorder was reverted completely. Roughly 2% is correct but not substantial,
does not move either measured platform close to 30 FPS, and does not justify another
decode-order whack-a-mole series. No r481 candidate source remains. The next core
prototype must remove work across the dominant mixed stream--for example by fusing or
specializing semantics rather than putting another cache in front of the existing
per-instruction helpers--and must earn its keep in restored A/B measurements before it
can reach the app.

### r482: a cacheless mixed literal loop is correct, broad, and still only marginal

r482 tested a materially different shape from r470-r479. It cached neither decoded
instructions nor host code. While the PC remained inside the already validated 1 KiB
fetch mapping, it reread the live raw instruction and executed common ARM, Thumb, VFP,
load/store and branch semantics in one C loop. The machine capped each call at the next
exact timebase edge and stopped after device-visible work. This removed cache lookup,
build, validation and uop-dispatch costs while retaining self-modifying-code visibility.
It was compile-time experimental and emitted no JIT code.

The public run/tick differential passed, followed by the full **55/55** default suite.
Synthetic app-facing rows initially looked substantial in non-LTO `-O3` builds:

| row | baseline mean | cacheless mean | delta |
|---|---:|---:|---:|
| ALU/branch + 4 KiB MMU + `s5l8900_run` | 32.815 M/s | 42.990 M/s | **+31.0%** |
| load/store + 4 KiB MMU + `s5l8900_run` | 24.105 M/s | 27.200 M/s | **+12.8%** |

That was not the guest result. A non-LTO restored `old,new,new,old` bracket moved badly
with the host: the first candidate lost 35.8%, the reverse candidate won 13.2%, and the
symmetric means were 12.983856 M/s baseline versus 11.005270 M/s candidate (**-15.2%**).
The opposing pairs make the short non-LTO series noisy, but they also disprove using the
synthetic gain as a product claim.

The shipping iOS core uses `-O3` and LTO, so a second pair used CMake's proven
whole-program-optimization configuration. LTO reduced the synthetic mean deltas to
+17.4% for ALU/branch and +4.3% for load/store. The decisive restored bracket used the
same r445 7.100 B MBX checkpoint and retired 100 M instructions per arm:

| order | build | rate | adjacent delta |
|---:|---|---:|---:|
| 1 | LTO baseline | 16.924587 M/s | control |
| 2 | LTO cacheless loop | 17.331551 M/s | +2.40% |
| 3 | LTO cacheless loop | 17.506433 M/s | +2.03% vs following control |
| 4 | LTO baseline | 17.157879 M/s | control |
| mean | LTO baseline / cacheless | 17.041233 / 17.418992 M/s | **+2.22%** |

The exact counters explain why broad coverage did not become a broad speedup. Each
candidate arm retired 89,308,320 instructions in the loop and 10,691,680 literally:
**89.308% coverage**, but only 10.995 instructions per successful batch. There were
8,122,359 batches and 10,439,220 zero-prefix attempts. In the non-LTO binaries, adding a
second caller also changed compiler inlining: `arm_step` shrank from 11,872 to 6,176 host
bytes while several helpers became out-of-line. That is a plausible mechanism for the
non-LTO loss, not a measured attribution; LTO is the relevant final comparison.

Every restored arm stopped at exactly 7.200 B with status OK and empty stderr. All eight
non-LTO/LTO work images matched SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`; all eight final
PPMs matched
`2EAE64986CFA1A34E25FC0D67DACC9E220E2262E9214BE2DAD775FD4518B266A`. No end-machine
snapshot was written, so this is not a serialized-counter equality claim.

The roughly 2% shipping-shaped gain is real but not substantial. It does not approach
the projected 34.304688 M/s needed for 30 changed frames/s, and keeping 338 lines of
experimental surface for it would repeat r481's marginal-optimisation mistake. The loop,
stats and tool reporting were therefore removed in full. No r482 performance code ships,
and neither the verified desktop cadence nor the phone's reported 0--4 FPS changes.

### r483-r484: physical predecode has high reuse and is still slower

r483 measured the proposed cache before implementing it. Over the exact r445 7.100--7.110
B restored interval, a simulated direct-mapped cache of 1,024 physical 1 KiB blocks saw
621,224/638,696 block hits (**97.264%**) and 9,356,267/9,999,489 decoded-site hits
(**93.567%**), with 17,472 block fills, 643,222 uop builds and zero observed raw-word
changes on hits. That was useful capacity evidence, not a speed result and not a complete
self-modifying-code proof. The read-only observer remains; it changes no guest state.

r484 then tested exactly one implementation and one profile-directed refinement. The
default-off experiment used 1,024 direct-mapped physical blocks, 256 lazy A32 uops per
block, one architectural retirement per call, and the existing semantic helpers. It was
about 2 MiB plus a 512 KiB exact physical-block presence bitmap. Thumb stayed literal.
Every direct RAM publication path was audited: CPU and DMA stores, host load, snapshot
restore, IMG3 load, both md bridges, boot HLE writes and the bring-up kernel callback.
Stale-code tests covered those paths. A deterministic oracle executed 8,192 generated
instructions twice with different state and compared status, CPU, RAM, counters and the
final serialized machine. After the bitmap refinement, the unchanged build passed 59/59
tests and the gated build passed 55/55.

The first LTO restored `off,on,on,off` bracket exposed the original implementation's
failure:

| arm | literal control | physical predecode |
|---:|---:|---:|
| first | 17.432912 M/s | 16.370694 M/s |
| reverse | 17.504613 M/s | 16.370307 M/s |
| mean | 17.468763 M/s | 16.370501 M/s (**-6.29%**) |

It had entered the invalidator for every ordinary RAM write: 38,577,243 calls and zero
populated blocks actually invalidated. The exact presence bitmap reduced that tax to
nine calls in the same 100 M-instruction interval. That fixed the diagnosed problem, but
did not rescue the design:

| order | runtime path | rate |
|---:|---|---:|
| 1 | literal control | 16.289164 M/s |
| 2 | physical predecode | 15.863551 M/s |
| 3 | physical predecode | 15.755698 M/s |
| 4 | literal control | 16.763929 M/s |
| mean | literal / predecode | 16.526547 / 15.809625 M/s (**-4.34%**) |

Both enabled arms recorded the same 4,946,909/5,000,546 block hits and
75,482,698/77,786,877 uop hits, so this was not a low-coverage result. The final synthetic
rows were mixed in the same direction as the underlying implementation: ALU/branch rose
from 43.28 to 48.16 M/s (**+11.3%**), while load/store fell from 27.81 to 22.22 M/s
(**-20.1%**). Removing almost all invalidation traffic still left cache lookup, uop
dispatch and calls into per-instruction helpers more expensive than literal decode on the
real mixed guest.

Every restored arm stopped at exactly 7.200 B with status OK and empty stderr. All work
images matched SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`; all final PPMs
matched `2EAE64986CFA1A34E25FC0D67DACC9E220E2262E9214BE2DAD775FD4518B266A`.
No end snapshot was written, so this is not a complete serialized-state comparison.

The implementation, coherency hooks, counters and tests were removed in full. No r484
performance code ships. High cache reuse is therefore not enough: another portable
predecode cache in front of the same helpers is now a repeatedly measured dead end, not
an open optimisation. A credible next experiment must remove semantic-helper and
per-instruction dispatch work across long mixed sequences, remain interrupt/timer exact,
and clear a substantial restored A/B gate before reaching the iOS app.

### r485: the current Release/LTO profile leaves no small 30 FPS lever

r485 refreshed the host-cost profile after all rejected r482/r484 code had been removed.
It used the shipping-shaped GCC Release configuration (`-O3`, LTO), added only link-time
`-pg` sampling with ASLR disabled, restored the exact r445 7.100 B checkpoint and retired
500 M instructions through `--run-api`. The 5,000 timed core calls took 31.781004 seconds,
or **15.732668 M/s**, and stopped exactly at 7.600 B with empty stderr and zero external-
media failures. This run had no frame observer, so it measured no FPS.

The flat profile's leading self samples were:

| function | sampled executable time |
|---|---:|
| `arm_step` | 32.84% |
| `exec_data_processing` | 10.01% |
| `mbx_execute_textured_sprite` | 6.46% |
| `main` | 6.07% |
| `vfp_execute_inner` | 5.77% |
| `mem_r32_as` | 4.68% |
| `thumb_step` | 4.34% |
| `arm_mmu_translate` | 4.09% |
| `bus_write` | 3.35% |
| `sw32` | 2.86% |
| `s5l8900_refresh` | 2.71% |
| `arm_cond_passed` | 2.61% |

This is a sampling profile, not exact wall-time attribution. It covers the whole process,
so `main` includes harness work outside the timed calls; Windows/system time can also be
absent from the named executable samples. Memory/bus routines can serve devices as well
as CPU instructions. The percentages therefore must not be inserted blindly into an
Amdahl calculation. They do establish the architectural shape: decode/semantics plus
translation and memory dominate, while no remaining individual helper has anything like
the required ceiling.

Applying the stable r461/r463 cadence of 1,143,489.6 retired instructions per detectable
change still projects 34.304688 M/s for 30 changes/s. Against this current unmetered LTO
rate, that is a **2.18x** throughput gap; the real iPhone result remains the reported
0--4 FPS and can be worse. Eliminating `arm_cond_passed`, the refresh, or any other small
row cannot close it. Nor can another cache in front of the same calls, as r474-r479 and
r484 already measured.

The only remaining credible no-JIT core experiment is materially larger: a static,
multi-instruction semantic engine whose compact decoded operations execute dominant ARM
classes without returning to `arm_step`, retain hot guest state across a sequence, and
exit at the exact timer, interrupt, MMIO, fault, state-change and self-modifying-code
boundaries. Static threaded handlers are signed app code; only decoded data is built at
runtime, so that architecture does not require executable writable memory. This profile
justifies investigating that architecture. It does not prove that it will be fast enough,
and it does not justify shipping a partial or marginal implementation.

### r486: the first static threaded semantic engine is correct and slower

r486 implemented that larger experiment behind a compile-time gate and a default-off
runtime switch. It emitted no host code: 1,024 direct-mapped physical 1 KiB blocks held
only decoded data, every cached word was compared with live RAM, and GNU computed-goto
handlers remained inside one signed C function across mixed ARM instructions. Dominant
data-processing, branch and single-transfer forms ran directly; multiply, media, block
and extra transfers, and VFP stayed inside the sequence through existing semantic
helpers. CP15, unconditional space, Thumb and residual encodings returned to the literal
decoder. The machine capped each sequence at the exact next timebase refresh and exited
after an MMIO-dirtying retirement, pending interrupt, fault or state change.

The correctness gate was useful before it was green. A deterministic oracle rewrote one
physical code site with 8,192 encodings and ran every encoding under two independently
generated register/flag states. It found two real prototype defects: a generic fallback
charged `cycles` before `arm_step` charged it again, and an over-broad residual decode
turned reserved word `0xE047BC96` into an ALU operation. Both were fixed without weakening
the comparison. The final oracle completed all 16,384 executions, compared CPU state,
RAM, unmapped counters and the final serialized machine, and the gated strict build then
passed 55/55 tests with `-Wall -Wextra -Werror`.

The matched Release/LTO synthetic bracket looked promising and was still not product
evidence:

| loop | literal symmetric mean | threaded symmetric mean | change |
|---|---:|---:|---:|
| ALU/branch, 4 KiB MMU, `tick=run` | 31.525 M/s | 43.515 M/s | **+38.0%** |
| load/store, 4 KiB MMU, `tick=run` | 24.720 M/s | 30.270 M/s | **+22.5%** |

The controls themselves drifted heavily, from 27.21 to 35.84 M/s on ALU/branch and from
21.59 to 27.85 M/s on load/store. The symmetric means reduce order bias; they do not make
the synthetic loops representative of iOS.

The decisive same-binary restored bracket used the exact r445 7.100 B checkpoint and
retired 100 M instructions per arm through `--run-api`:

| order | runtime path | rate |
|---:|---|---:|
| 1 | literal control | 17.152419 M/s |
| 2 | static threaded | 16.649385 M/s |
| 3 | static threaded | 16.956785 M/s |
| 4 | literal control | 17.418559 M/s |
| mean | literal / threaded | 17.285489 / 16.803085 M/s (**-2.79%**) |

This was not a coverage miss. Each enabled arm retired 80,427,723 of the 100 M
instructions inside the threaded engine. It recorded 88,312,843/90,831,112 decoded-word
hits, 15,223,571/15,277,208 block hits, 2,518,269 uop builds, zero raw-word changes and
25,905,282 engine entries. That last number is the practical failure: only 3.10 threaded
retirements occurred per entry on average, while 24,448,994 exits still returned to the
literal path. Cache lookup, live-word validation, architectural-state traffic and short
mixed sequences consumed more than the avoided decode/dispatch work saved.

Every restored arm stopped at exactly 7.200 B with status OK and empty stderr. All work
images matched SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`; every per-run and
tool-relative PPM matched
`2EAE64986CFA1A34E25FC0D67DACC9E220E2262E9214BE2DAD775FD4518B266A`. No real-workload
end snapshot was written, so those hashes are not a complete serialized-state claim.

The engine, cache, runtime hooks, counters and oracle were removed in full. No r486
performance code reaches the desktop or iOS product, and the reported phone result
remains 0--4 FPS. The honest conclusion is narrower than "threaded interpretation cannot
work": this shallow per-instruction-uop shape, with live raw validation and frequent
literal exits, cannot. A successor would need genuinely longer fused operations or
traces, hot guest registers held outside `arm_cpu_t`, direct memory fast paths and far
fewer semantic/literal boundaries. Merely tuning this cache would be another marginal
whack-a-mole pass, not a plausible route across the remaining 2.18x gap.

### r487: whole-trace reuse has headroom; dispatch reduction alone does not close 2.18x

r487 extended the existing read-only `--sequence-profile` observer instead of building
another engine. For trace caps 4, 8, 16, 32 and 64 it splits the actual dynamic stream at
physical discontinuities and the selected cap, counts the resulting calls, and simulates
direct-mapped trace-head caches with 1,024, 4,096 and 16,384 entries. It changes no guest
state. The model deliberately assumes every ARM and Thumb semantic is available, spans
stores and not-taken branches, ignores extra timer/MMIO exits, and validates only the
head word. It is therefore an optimistic reuse and dispatch ceiling, not a speed result
and not a coherency proof.

The exact r445 7.100--7.110 B restored interval produced:

| max trace instructions | modeled calls | mean instructions/call | entry reduction | 1K / 4K / 16K head hit rate |
|---:|---:|---:|---:|---:|
| 4 | 2,983,252 | 3.352 | 70.17% | 64.78% / 86.02% / 95.07% |
| 8 | 1,923,837 | 5.198 | 80.76% | 74.87% / 90.96% / 96.98% |
| 16 | 1,480,884 | **6.752** | **85.19%** | 79.80% / **93.08%** / 97.46% |
| 32 | 1,330,784 | 7.514 | 86.69% | 81.36% / 93.59% / 97.57% |
| 64 | 1,299,329 | 7.696 | 87.01% | 82.17% / 93.78% / 97.56% |

The accounting remained exact at 10,000,000 observations: 9,999,489 fetched
instructions, 511 pending-interrupt entries and zero fetch failures. Execution stopped
at exactly 7.110 B with status OK, empty stderr and zero external-media failures. The
466,825,216-byte work image matched SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`; both final PPMs
matched `1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.

This changes the next experiment, but not the FPS status. Cap 16 captures most of the
available dynamic length; doubling to 32 buys only 0.762 more instructions per modeled
entry. A 4K head cache already models 93.08% reuse, so enlarging a shallow cache is not
the missing breakthrough. Compared with r486's 3.10 internal retirements per entry, a
whole trace can plausibly remove far more lookup and re-entry work. It still cannot make
the other sampled costs disappear: instruction semantics, VFP, translation, memory and
devices remain. A justified prototype must therefore cache complete cap-16 traces,
validate them with code-page generations instead of a live comparison per word, span
both ARM and Thumb, keep common semantics inside the trace, and clear a double-digit
restored A/B gate. A trace cache that still calls `arm_step` or validates every word is
already contradicted by r474--r486 and should not be built.

### r488: the cap-16 generation-validated trace is correct and 20.73% slower

r488 implemented the exact experiment r487 justified, behind a default-off compile and
runtime gate. A direct-mapped 4,096-entry cache held up to sixteen decoded operations per
physical trace head. Each traced 1 KiB RAM block had a 64-bit write generation and an
active byte; ordinary RAM writes advanced only active generations, so a trace performed
one coherency comparison at entry rather than one live raw comparison per instruction.
The runner used direct computed-goto handlers for common ARM data-processing, branch and
single-transfer forms, retained multiply/media/block/extra/VFP helpers inside the trace,
and executed Thumb through its decoded helper without returning to `arm_step`. Timer,
interrupt, MMIO, fault, mapping/state and self-modifying-code boundaries remained exact.
Because the external-media bridges can copy directly into guest RAM inside a privileged
SVC callback, every host SVC conservatively flushed decoded data before the callback.

The correctness work again found no excuse for the speed result. The strict gated build
was warning-clean and passed 55/55 tests. Its differential oracle completed 8,192 A32
encodings under two generated CPU states plus 4,096 Thumb encodings, repeatedly rewrote
the same physical code block, compared CPU/RAM/unmapped/dirty state after every step,
compared the final serialized machines, and executed a real sixteen-instruction batch.
Focused coherency cases proved both difficult paths: a guest store rewrote the next
already-decoded instruction, and a privileged SVC directly bypassed the bus to rewrite
the next instruction. Both executed the new word, not stale metadata. The final machine
test reported 5,848 assertions and zero failures.

The same-binary Release/LTO restored bracket used the exact r445 7.100 B checkpoint and
retired 100 M instructions per arm:

| order | runtime path | rate |
|---:|---|---:|
| 1 | literal control | 17.465669 M/s |
| 2 | cap-16 trace | 13.605587 M/s |
| 3 | cap-16 trace | 13.544812 M/s |
| 4 | literal control | 16.784660 M/s |
| mean | literal / trace | 17.125165 / 13.575200 M/s (**-20.73%**) |

The counters explain why the optimistic model did not become an implementation result.
Each enabled arm retired 94,580,945 instructions inside the runner, including 13,951,813
Thumb instructions, but required 23,562,386 entries: only **4.014 retirements per entry**,
not r487's modeled 6.752. The cache hit 19,540,250/23,557,347 lookups (**82.95%**), built
4,017,097 traces, took 9,284,886 literal exits and flushed 2,098 times. It observed
38,577,243 RAM-write notifications and exactly zero active-code-page invalidations. Thus
real self-modification was not destroying reuse; short semantic boundaries, head
diversity/collisions, trace construction, generation bookkeeping, computed dispatch and
the still-per-instruction semantic helpers cost far more than fetch/decode elimination
saved.

Every arm stopped at exactly 7.200 B with status OK and empty stderr. All work images
matched SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`; every PPM matched
`2EAE64986CFA1A34E25FC0D67DACC9E220E2262E9214BE2DAD775FD4518B266A`. No real-workload
end snapshot was written, so the hashes are not a complete serialized-state claim.

The trace engine, generation arrays, machine hooks, counters and tests were removed in
full. No r488 performance code ships, and phone FPS remains the reported 0--4. This is
stronger negative evidence than r486: even broad ARM/Thumb coverage and page-generation
coherency make this portable C trace-over-existing-helpers shape dramatically worse.
Tweaking associativity, cap length or the inactive-write check would be whack-a-mole
against a 20.73% loss and the still-unclosed 2.18x target gap. A credible no-JIT successor
must replace, not wrap, the per-instruction semantic/memory machinery--most plausibly a
host-specific static AArch64 interpreter with guest registers and fast memory state held
in host registers. That is a substantially larger and device-dependent project. No
current measurement proves it can reach 30 FPS, and optimism should now be lower, not
higher.

### r489: native AArch64 semantics have ceiling; static no-JIT retention is unproved

r489 first made the repository's existing register-pinned AArch64 translator answer a
narrow feasibility question on the two hosted Apple-Silicon runners. `jitbench` translates
one synthetic sixteen-instruction block once, then interleaves repeated `arm_step()` and
native-block execution. It compares all sixteen registers, CPSR, retired cycles, the full
1 MiB RAM hash and the block exit after every arm. The mixed blocks use four memory
operations out of sixteen (25%, near the historical 22.6% guest share). The tool is not
linked into the app, does not change the default core, and prints its limitations before
its numbers.

The first 20 M-instruction invocation was directionally large but too short: native arms
finished in tens of milliseconds and macOS-15's interpreter samples ranged from 30.001 to
58.804 M/s in one row. Those ratios were rejected as decision-quality evidence rather
than promoted. The exact same executable shape was rerun at 500 M guest instructions per
arm, three reversed-order repetitions. The medians were:

| runner / synthetic block | interpreter M/s | native block M/s | ratio |
|---|---:|---:|---:|
| macOS-14 arm64 / A32 ALU | 79.671 | 2,223.487 | 27.908x |
| macOS-14 arm64 / A32 25%-memory mixed | 74.743 | 722.426 | 9.666x |
| macOS-14 arm64 / Thumb ALU | 141.183 | 2,151.583 | 15.240x |
| macOS-14 arm64 / Thumb 25%-memory mixed | 128.051 | 711.520 | **5.557x** |
| macOS-15 arm64 / A32 ALU | 69.013 | 2,150.686 | 31.163x |
| macOS-15 arm64 / A32 25%-memory mixed | 64.101 | 586.127 | 9.144x |
| macOS-15 arm64 / Thumb ALU | 121.027 | 1,775.991 | 14.674x |
| macOS-15 arm64 / Thumb 25%-memory mixed | 108.943 | 565.045 | **5.187x** |

All twenty-four paired repetitions (forty-eight timed arms) passed the state comparison.
GitHub Actions core run
`30829906322` and manually dispatched iOS build `30829914814` both passed at exact commit
`b3f93607d3450e9e7c8a7281e14899319c0dfe24`.

This is real AArch64 execution and substantial ceiling evidence. It is still **not a
speedup in the emulator**. The block specializes guest register numbers, immediates and
control flow; it performs no runtime translation/cache lookup, timer tick, interrupt
sample, MMIO, device work, framebuffer publication or UIKit presentation. A static
no-JIT handler must dynamically select registers and operations from decoded data, while
the measured native block has already compiled those choices away. Therefore 5.187x is
not an expected static-engine multiplier and none of these rates is phone FPS. Current
shipping FPS remains 0--4.

The result changes one decision: native semantics are not ruled out by their raw cost,
so a host-specific design is worth investigating. It does not justify writing a generic
assembly interpreter blind. The next read-only gate is concentration of the real cap-16
trace heads and raw sequences. A small dominant set could support signed, precompiled
superblocks for the exact supported firmware; a diffuse set would rule that out before
another large implementation.

### r490: exact trace heads are moderately reusable, not a shippable AOT catalog

r490 extended only the read-only sequence observer. For each completed cap-16 dynamic
slice it attributes the call and retired instructions to the exact physical head tuple
`(pa, first raw instruction, ARM/Thumb)`. A slice closes at a physical discontinuity,
interrupt/fetch exit or the cap. Variable lengths at one head remain visible; the counter
does not pretend that a head proves one immutable full block. It is not in the CPU or app
path and its extra host allocation and lookups invalidate wall-clock performance.

The exact r445 7.100--7.110 B restored interval accounted for all **1,480,884** modeled
cap-16 calls and all **9,999,489** fetched instructions across **4,438** distinct heads,
with zero dropped calls or instructions:

| hottest exact heads | calls covered | fetched instructions covered |
|---:|---:|---:|
| 1 | 6.639% | 1.969% |
| 10 | 12.526% | 9.115% |
| 100 | 25.677% | 31.438% |
| 1,000 | 75.106% | **85.451%** |
| all 4,438 | 100.000% | 100.000% |

The largest head is the two-instruction `_cpu_idle` loop and covers only 1.969% of fetched
instructions. The next entries mix AppleMBX, other kernel routines and userspace; the top
sixteen together cover 11.378%. This is meaningful reuse, but it is not a tiny set that
can be hand-specialized. One thousand generated blocks might have acceptable code size,
yet that observation does not make them a product architecture. The repository ships no
Apple firmware, the app imports the user's firmware after its executable has already been
signed, and physical placements plus interactive workloads are not established invariant
by one restored window. A firmware-derived catalog therefore cannot be generated on the
phone and is incompatible with the current distributable app unless Apple-derived
semantics are baked into the signed binary. That boundary is not being crossed.

Correctness matched r487 exactly: the run stopped at 7.110 B with status OK, empty stderr
and zero external-media failures. The work image SHA-256 was
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`; the per-run PPM was
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.

This rejects firmware-specific precompiled superblocks as the default app route; it does
not reject static multi-instruction execution. A universal decoded cache can easily hold
4,438 heads, but r488 already proved that cache plus portable per-instruction helpers is
20.73% slower. The remaining credible experiment is therefore narrower and harder: a
signed, host-specific AArch64 semantic loop that consumes runtime-decoded data while
keeping hot guest registers and fast-memory state in host registers. It must first clear
a semantics-exact synthetic gate by substantially more than the 2.18x desktop capacity
gap before any real-emulator integration is justified. Product code and phone FPS remain
unchanged at the reported 0--4.

### r491: firmware-independent signed AArch64 semantics clear the synthetic gate

r491 implements the narrow gate r490 required. A deterministic build-time generator
enumerates **2,577 generic ISA/register handler variants**; it reads no firmware, profile
or guest opcode stream. The resulting AArch64 assembly is ordinary signed executable text.
Runtime decoding creates only eight-byte data records containing a handler index and an
immediate, so this path never generates code and never asks for writable executable
memory. Guest r0--r7 and SP remain in AArch64 callee-saved registers across the entire
run, and direct threading stays inside the signed function between guest instructions.

This is deliberately a benchmark subset, not an emulator engine. It implements exactly
the A32 ALU/load/store and Thumb ALU/SP-relative load/store forms used by the four r489
synthetic blocks, with the final unconditional branch returning to address zero. The
decoder refuses every unsupported bit. Apple-arm64 ctest executes 1.6 M instructions per
case and compares the same state as the long benchmark; x86 builds validate all sixteen
uops per case and report an execution skip.

The long measurement used 500 M guest instructions per arm and three rotated orderings.
Every one of twenty-four static arms matched the interpreter's sixteen registers, CPSR,
cycles, full 1 MiB RAM hash, status and exit state:

| runner / synthetic block | interpreter M/s | static signed M/s | ratio |
|---|---:|---:|---:|
| macOS-14 arm64 / A32 ALU | 80.315 | 2,484.929 | 30.940x |
| macOS-14 arm64 / A32 25%-memory mixed | 71.923 | 2,915.129 | 40.531x |
| macOS-14 arm64 / Thumb ALU | 132.253 | 2,493.107 | 18.851x |
| macOS-14 arm64 / Thumb 25%-memory mixed | 121.478 | 2,156.213 | **17.750x** |
| macOS-15 arm64 / A32 ALU | 61.387 | 2,029.147 | 33.055x |
| macOS-15 arm64 / A32 25%-memory mixed | 60.077 | 2,683.469 | 44.667x |
| macOS-15 arm64 / Thumb ALU | 112.619 | 2,186.758 | 19.417x |
| macOS-15 arm64 / Thumb 25%-memory mixed | 109.236 | 1,865.101 | **17.074x** |

Exact commit `b038b7311b2abb460ee41decf3e6f9333f3092c9` passed core-tests run
`30832792761`, including both Apple execution gates, and iOS build `30832792876`.

The large ratios are real for the stated loop and extremely optimistic for the product.
Each block is a fixed sixteen instructions rather than r487's real 6.752 mean; data memory
is a masked flat host pointer rather than the guest MMU/TLB/MMIO path; one native call
repeats all blocks without cache lookup, raw/generation validation, timer budgeting,
interrupt sampling, faults or device exits. This also explains why the static mixed rows
beat the JIT rows: the JIT benchmark re-enters a generated function per block and its
memory path retains helper/policy costs, while the static proof amortises its prologue and
uses direct RAM. It would be false to project 17.074x onto the emulator or call any value
above phone FPS.

The decision is nevertheless positive. The weakest exact static ratio exceeds the 2.18x
desktop capacity gap by enough margin that a real, default-off integration is now
justified. Its next gate must pay the costs this proof avoided: real decoded-block lookup
and coherency, variable/short exits, conditions, r8--r12 and PC semantics, MMU/TLB-backed
loads/stores, timer/interrupt boundaries, faults and fallback. Only a same-binary restored
firmware A/B can promote it. Shipping phone FPS remains the measured 0--4 until that gate
passes and the iOS app is rebuilt and measured.

### r492: variable signed blocks preserve exact guest addresses

r492 removed a benchmark-only assumption before touching the SoC. The signed decoder now
accepts any block length from one through sixteen instructions, records its real guest
start and exit addresses, and emits an explicit fallthrough exit when the last instruction
is not a branch. A terminal ARM or Thumb unconditional branch may target any aligned guest
address; a branch before the end of a proposed block is rejected. The machine-facing entry
point reads an unaligned little-endian guest byte stream rather than casting translated RAM
to a host-native instruction array. The original host-native array entry point remains for
the cross-endian benchmark contract.

The exact oracle covers every accepted length and both fallthrough and branch exits, then
compares signed execution with the literal interpreter on Apple arm64. Decoder negatives
include zero and over-cap lengths, misaligned PCs, a mid-block branch, and an unaligned
guest-byte buffer. The static handler set and its synthetic ceiling are otherwise unchanged:
this milestone adds address correctness, not ISA coverage or performance. Exact commit
`0bad476e1ceeff9738bce5cccdc936af402d7a3d` is included in the later fully green r493
matrices.

This was necessary integration work, but it did not put the engine in the product and did
not produce an FPS result. Calling it progress toward 30 FPS is fair only in the structural
sense: it removed a false fixed-loop model that could not execute real guest control flow.
It did not make the emulator faster.

### r493: the first signed path reaches the SoC loop, default off

r493 integrates the signed AArch64 contract into the real `s5l8900_run()` loop behind
`S5LBOX_STATIC_A64_ENGINE`, which defaults to OFF, and a separate per-machine runtime opt-in.
The path still creates no executable memory. A 1,024-entry direct-mapped cache stores only
decoded data records. Its identity includes guest PC/state, translated fetch pointer,
translation generation and privilege; every hit also compares the exact guest bytes before
reuse. That redundant-looking byte witness is intentional: direct RAM writes and host SVC
bridges can change code without going through a normal guest-store invalidation hook.

The integration fails closed. It runs only when the existing fetch translation is already
valid, the CPU mode is valid, no abort or unmasked interrupt is pending, and the full block
fits before the next exact device-time boundary. Unsupported instructions return to
`arm_step()` without changing state. Most importantly, the first version refuses **every
load and store**. The benchmark's masked flat-RAM operations do not implement the guest
MMU, permission faults, unaligned access, MMIO or write observers, so reusing them here
would be fast and wrong. Snapshot data and version remain pointer-free; only the opaque
per-machine cache pointer changes the in-memory machine layout.

The Apple execution oracle uses two complete S5L8900 machines at the board's real
412 MHz:6 MHz clock ratio. One uses signed handlers and one remains literal. They run across
hundreds of device boundaries, rewrite a cached ADD into SUB through direct RAM, and compare
their final serialized snapshots byte for byte. Both hosted Apple runners reported
`STATIC-A64-SOC-ORACLE exact=yes retired=21520 smc=yes` and 5,843 assertions with zero
failures. The JIT-on matrix ran 60/60 tests on each Apple runner, and the independent
JIT-off build ran 55/55. Exact commit
`6b85b4a775c65ee3f3edb647a61a9b915eaef600` passed core run `30866085797` and iOS build
`30866085815`.

That evidence is a correctness milestone, **not a speed milestone**. The oracle is a tiny
unconditional ADD/SUB loop chosen specifically to enter the current subset. There is no
same-binary restored-firmware A/B, no cold boot, no frame measurement and no phone result.
The iOS target still builds with the engine off; the green IPA job proves that the ordinary
app was not broken, not that the new path is active on an iPhone. Measured product FPS is
therefore unchanged at the reported 0--4.

Running the current subset against the restored workload would mostly measure fallback
overhead and could make a misleadingly narrow implementation look decisive. The r473
profile instead dictates the next work: broaden A32 data processing around MOV, CMP,
conditions, barrel shifts and r8--r14, then add exact MMU/TLB read hits and later Thumb/VFP.
Only after enough of the measured stream stays inside signed handlers is a restored
same-binary A/B useful. Cache-size tuning or claiming the synthetic 18x--45x ratios as
emulator speed would be another whack-a-mole detour.

### r494: exact immediate A32 semantics cover a measured 18.569% ceiling

r494 expands the signed path along the largest already-measured tractable slice rather
than tuning its cache. The r473 restored profile observed 1,856,853 A32 immediate
data-processing instructions: **18.569% of all fetched instructions** and 61.625% of the
data-processing class. The decoder now accepts all sixteen immediate data-processing
opcodes, conditions 0x0--0xd plus AL, r0--r14 destinations and r0--r15 sources. PC reads
use the exact per-instruction `pc + 8` value placed in a data record at decode time. PC
writes, cond=0xf, BL and conditional branches remain rejected.

Condition execution is a signed guard record. A failed guard skips the following semantic
record without changing flags or registers; the guest instruction still counts once in
the block's timer budget. Arithmetic operations use AArch64's matching 32-bit NZCV
semantics. Logical operations explicitly reconstruct ARM's N/Z, shifter-C and preserved-V
rules, including the difference between an unrotated immediate that preserves C and a
rotated immediate that replaces it. Guest r0--r7 and SP remain pinned. r8--r12 and LR are
loaded/stored through the live CPU register array, so writes from an earlier signed
instruction are visible to the next one. Runtime records grew from eight to sixteen bytes
to carry the immediate, exact PC operand and flag metadata; build-time handlers grew from
2,577 to 10,271. They are still ordinary signed text, not generated executable memory.

The independent Apple oracle now contains one sixteen-instruction case spanning every
opcode, r8--r14, PC input, rotated/unrotated immediates and flag writes, plus a second case
that holds N=0/Z=0/C=1/V=0 while exercising all fourteen real condition codes: seven pass
and seven fail. Both macOS runners reported exact final CPU/RAM state at the correct exit
PC and cycle count. The full S5L8900 oracle was also upgraded from repeated ADDs to a loop
using conditions, r8--r14, carry consumers and logical flags. Both runners reported
`STATIC-A64-SOC-ORACLE exact=yes retired=21520 smc=yes`; changing its cached MOV changed
the EQ/NE path and still produced a byte-identical final machine snapshot.

The larger record and handler table did not erase the synthetic ceiling. Across the same
500 M-instruction timed rows, static signed ratios ranged from 17.340x to 41.240x on
macOS-15 and 18.738x to 40.380x on macOS-14. Those rows still repeat four old synthetic
blocks with flat RAM and no real lookup, so they are a regression guard, not a projection
of the newly supported workload or a phone speed result. Exact commit
`cf36c139b533d6fa85b63f0c0802faf1f9070dda` passed core run `30867587456`, including
60/60 JIT-on and 55/55 JIT-off tests, and iOS build `30867587465`.

Brutal status: this is the first expansion with a quantified real-workload ceiling, but
there is still **zero measured emulator or iPhone FPS gain**. The decoder can cover at
most the immediate 18.569% slice plus its older tiny register subset, and unsupported
instructions fragment that coverage into short runs. The iOS target still builds the
engine out. A restored A/B now would mainly report fallback/entry overhead and could not
prove the 2.18x end-to-end requirement even under infinite immediate-ALU speed. The next
non-whack-a-mole step is the other 1,158,810 profiled register-form data-processing
instructions with the exact barrel shifter and full register set, followed by MMU/TLB RAM
read hits. Product FPS remains the reported 0--4.

### r495: exact register Operand2 completes the measured safe-DP class

r495 implements the remaining A32 register-form data-processing shape rather than
adjusting the cache around the immediate subset. The r473 profile counted 1,158,810
register-form observations, **11.589% of fetched instructions**. That is a gross form
count: r473 recorded 2,515 data-processing writes to PC across both immediate and
register forms without splitting those 2,515 by Operand2 kind. The defensible combined
ceiling is therefore the separately classified 3,013,148 ordinary non-PC data-processing
instructions, **30.133% of fetched instructions**, not 18.569% + 11.589% presented as
an exact reached-path sum.

Each supported register instruction is represented by an exact barrel-shifter record
followed by an ALU record. Immediate shifts implement LSL #0 carry preservation,
LSR/ASR #0 as shift-by-32, and ROR #0 as RRX through the incoming C flag.
Register-specified shifts use Rs[7:0] and distinguish 0, 1--31, 32 and greater than 32
instead of inheriting AArch64's modulo-32 variable-shift behaviour. The shifter changes
no host flags before ADC/SBC/RSC consumes the incoming C. Logical flag writes still use
the shifter carry and preserve V. All sixteen opcodes, r0--r14 destinations, valid
sources, every condition and the interpreter's register-shift R15 refusal are covered;
PC writes remain outside the contract.

The signed table now contains 23,847 firmware-independent handlers. Runtime records
remain sixteen-byte data, and there is still no runtime code generation or writable
executable mapping. The larger text also forced a real range correction: table address
formation now uses page-relative relocations, and the distant exit uses a local CBNZ
plus an ordinary branch, rather than relying on ADR/CBZ's +/-1 MiB reach. Generated
assembly was 6,625,182 bytes in this revision, but it is build output rather than a
checked-in or firmware-derived artifact.

The Apple oracle adds three independent blocks: all opcodes with immediate shifts,
all opcodes with register-specified shifts at amounts 0/1/31/32/33/255, and all fourteen
condition guards skipping two records. macOS-14 and macOS-15 reported every block exact
at its expected PC and sixteen cycles. The complete SoC loop also executes signed EOR
and BIC register forms and still reported
`STATIC-A64-SOC-ORACLE exact=yes retired=21520 smc=yes` with byte-identical final
machine snapshots. Local checks included a 476,928-case shifter-model comparison and
5,832/0 SoC assertions; hosted Apple runs reported 5,843/0, 60/60 JIT-on tests and
55/55 JIT-off tests.

The old four-block 500 M-instruction synthetic ceiling remains positive but is still
not a product measurement. Static ratios ranged from 17.527x to 38.552x on macOS-14
and 15.024x to 40.269x on macOS-15. Those timed blocks mostly use the old small handler
set, flat RAM and one already-decoded loop, so neither the ratios nor their runner-to-runner
movement measure the new real-workload coverage. Exact commit
`9cfce454edda8bd27b619db63f5ca0834acdfd61` passed core run `30869029321` and iOS build
`30869029353`; the latter still builds the signed engine out and therefore proves only
that the ordinary IPA was not broken.

Brutal status: **measured emulator and phone FPS improvement is still zero**. This is
substantial semantics and integration progress, but the measured safe-DP islands average
only 1.635 instructions and every load/store, VFP instruction, Thumb instruction and
control transfer still returns to the literal path. There has been no restored
same-binary performance bracket, no phone execution of this engine, and no cold-boot
acceptance. The next evidence-backed step is an encoding census followed by exact
MMU/TLB-backed RAM read hits; cache-size tuning or an FPS claim here would be dishonest.

### r496: exact DREAD RAM hits broaden the signed path without hiding misses

Before implementing a load fast path, r496 extended the read-only restored-workload
observer and replayed exactly 10,000,000 entries from the r445 snapshot at 7.100 billion
retired instructions. The interval contained 9,999,489 fetched instructions and 511
interrupt entries. It observed 1,325,611 non-PC A32 single loads: 60,331 failed their
condition, none were invalid if executed, and none took an alignment abort. Among passed,
valid loads, the interpreter's pre-step 1 KiB DREAD cache would hit 1,152,488 times and
miss 112,792 times, a 91.086% hit rate. The narrower pre-index/no-writeback family now
implemented by the signed decoder accounted for 1,306,528 observations: 57,303 failed
conditions, while 1,136,467 of the passed loads had a DREAD hit. That profile was a
read-only classification run; it changed no guest behaviour and measured no speed.

The product decoder now accepts that exact A32 family for word and byte loads, immediate
or register offsets, add or subtract addressing, every base register, and every non-PC
destination. Register offsets use the exact ARM immediate barrel-shifter rules. Stores,
loads to PC, post-indexing, writeback, invalid register-offset encodings and Rm=PC remain
literal. Generated ordinary AArch64 text grew from 23,847 to 23,941 firmware-independent
handlers; decoded blocks remain data records and still require no JIT, writable executable
memory or firmware-derived code.

A direct read is allowed only when the live DREAD slot has the exact virtual tag,
translation generation and privilege, and word alignment is valid. A hit reads through
the cached host pointer and increments the existing hit counter exactly. A miss does not
pretend that RAM is safe: the signed runner refunds the unretired suffix of its prepaid
cycle budget, publishes the unexecuted load PC and completed prefix state, and returns to
`arm_step()`. The literal path then owns the MMU walk, permission fault, alignment policy,
MMIO exit, miss accounting and cache fill. Native Apple oracles cover eight exact hits, a
zero-prefix miss and a miss after one completed data-processing instruction, all against
the interpreter.

The first integration also exposed a separate structural loss at device-time boundaries.
The cache retained the longest decodable block, but a caller budget shorter than that
block returned directly to the interpreter. r496 now decodes a temporary longest prefix
bounded by the remaining budget instead. The cached block stays unchanged, and at most
one shortened prefix is needed at an edge. The complete SoC oracle explicitly reaches a
warm sixteen-instruction block with a one-instruction budget and requires that instruction
to retire through the signed path; the final machine snapshot remains byte-exact.

At exact commit `74e488011012d4a4bce9d93bacd6cf9decd29a06`, core-tests run
`30871962759` and iOS-build run `30871962760` both passed. macOS-14 and macOS-15 each
reported `STATIC-READ-SHAPE exact=yes insns=9 uops=23 handlers=23941`,
`STATIC-READ-ORACLE exact=yes hits=8 zero-prefix=yes partial-prefix=yes`,
`STATIC-A64-SOC-ORACLE exact=yes retired=24095 smc=yes`, 5,846/0 SoC assertions,
60/60 engine-on tests and 55/55 independent engine-off tests. Linux, Windows,
sanitizers and warnings-as-errors were also green. The ad-hoc-signed IPA built and
packaged successfully, but its normal project configuration still builds this engine out.

The largest honest single-instruction eligibility ceiling visible in that profile is
3,013,148 safe non-PC data-processing observations plus 1,193,770 supported load guards
or hits: 4,206,918 of 9,999,489 fetched instructions, or **42.071%**. This is not measured
signed retirement, is not a speedup, and cannot be inserted into Amdahl's law as an
independent additive gain. Unsupported instructions, cache entry costs, short fragments,
misses, interrupts and timer boundaries can make realised coverage much smaller.

Brutal status: **measured emulator and iPhone FPS improvement is still zero**, and the
only current phone report remains 0--4 FPS. There has been no restored same-binary A/B,
no iPhone execution of this engine, and no cold-boot acceptance. The work is substantial
correctness and coverage infrastructure, but calling it an FPS improvement would be
false. The next gate is an exact restored-trace model of the product decoder's contiguous
signed runs, including live DREAD misses and the sixteen-instruction cap. Only if that
shows useful residency should the engine enter an app build and face same-binary restored
and phone measurements; final acceptance still requires a cold boot.

### r497: exact residency proves the current signed subset cannot reach 30 FPS

r497 answers the gate at the end of r496 without executing a native handler or changing
the guest. The read-only sequence observer now calls the exact shipping product decoder
for every live instruction, classifies the literal pre-step condition and DREAD state,
and reconstructs the actual signed-call boundaries. The model includes the product's
1 KiB fetch-block rule, sixteen-instruction cap, exact device-timer edges, interrupts,
the app's 100,000-instruction `run()` boundary, current fetch mapping/generation and every
read-only entry gate visible to `bootkernel`. Runtime opt-in and an arm64 target are
deliberately assumed. Decode/cache lookup time is omitted, and this Windows replay cannot
execute the AArch64 handlers, so the result is residency rather than speed.

The exact restored 7.100--7.110 B interval again contained 10,000,000 observations,
9,999,489 fetched instructions and 511 interrupt entries, with no fetch failure. Every
accounting identity closed:

| exact current product model | instructions | fetched share |
|---|---:|---:|
| decoder-supported | 4,474,493 | 44.747% |
| decoder-rejected | 5,524,996 | 55.253% |
| retirement-eligible after condition/DREAD checks | 4,361,735 | 43.619% |
| modeled signed retirement | **4,062,729** | **40.629%** |
| entry-gate refusal | 299,006 | 2.990% |

The model formed 1,860,925 calls with a mean of only 2.183 instructions and a maximum of
sixteen. It attributed one stop to every call. Unsupported or dynamically ineligible
instructions caused 1,684,361 stops; timer, fetch-block and cap stops were 58,499, 8,398
and 2,868 respectively. All 299,006 entry refusals were the existing fetch-cache gate;
machine, eager-tick, dirty-level, external-input and timebase refusals were zero. This is
an exact description of the observed literal stream under the stated model, not a claim
that 40.629% will run for free or that native lookup overhead is zero.

The r485 optimized baseline is 15.732668 M guest instructions/s and the 30 FPS capacity
target is 34.304688 M/s, a required **2.180475x** speedup. If the modeled 40.629% became
infinitely fast, Amdahl's law caps the whole program at **1.684334x**. The absolute
infinite-speed coverage minimum is 54.138%. Therefore the current signed engine is not
merely unmeasured or unlikely to reach 30 FPS: **this measured subset cannot reach it**.
Enabling it in the app now would be a misleading device experiment.

The observer was then extended to retain each exact product-decoder rejection and group
it by the same broad branches as the real VFP and Thumb decoders. No raw encoding was
dropped. All 1,709,378 VFP observations were currently rejected:

| VFP decoder family | observations | fetched share |
|---|---:|---:|
| VLDR | 466,108 | 4.661% |
| VCVT single/double | 223,959 | 2.240% |
| VMRS/VMSR | 181,590 | 1.816% |
| VCMP/VCMPE | 180,986 | 1.810% |
| VSTR | 148,246 | 1.483% |
| VMLA/VMLS/VNMLA/VNMLS | 121,266 | 1.213% |
| VADD/VSUB | 84,887 | 0.849% |
| VMOV/VABS/VNEG register | 75,991 | 0.760% |
| VMUL/VNMUL | 68,646 | 0.686% |
| remaining transfers, conversions, loads/stores and divide | 157,699 | 1.577% |

Thumb contributed 1,355,047 observations. The product decoder supported 46,428 and
rejected 1,308,619. The largest rejected families were immediate loads (178,060),
conditional branches (147,493), immediate MOV/CMP/ADD/SUB (138,699), small ADD/SUB
(134,717), high-register operations (98,780), register ALU (81,841), BL/BLX pair halves
(66,755), BX/BLX-register (48,502), PUSH/POP (96,233 combined), and shifts (39,332).
The complete family rows accounted exactly for both the observed and rejected totals.

This is diffuse enough to reject an opcode-at-a-time strategy. The hottest VFP encoding,
`eef1fa10` (`VMRS APSR_nzcv,FPSCR`), is 1.810% of the whole trace; the hottest rejected
Thumb halfword is only 0.311%. The hottest 32 encodings cover 37.870% of rejected VFP and
26.598% of rejected Thumb. Special-casing those lists would still leave most of each
class literal and would bake one restored interval into product semantics.

The coverage arithmetic also rejects two superficially attractive shortcuts. Adding
every rejected Thumb instruction to current modeled retirement reaches only **53.716%**,
still below the infinite-speed minimum. Adding every VFP instruction except stores reaches
**55.997%**, whose impossible zero-cost ceiling is only 2.273x and leaves no allowance for
native entry, FP-mode, DREAD or device overhead. A credible route must therefore combine
broad Thumb and VFP execution and increase contiguous call length; neither family alone
has honest margin.

The optimized full observer run exited OK in 30.607 seconds with empty stderr and zero
external-media failures. That instrumented wall time is not a performance result. Its
work-image SHA-256 was
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`, and the captured
PPM was `1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`, both identical
to r496. Guest counters and status, including 1,590 CLCD frames, also matched the prior
observer run; this is checked-output equality, not a serialized whole-machine comparison.
Local strict engine-on/off builds passed, as did all 60 engine-on tests. Exact commit
`319e01a97b199ff49c75093a67861ce27e2041c5` passed core-tests run `30874343573` and the
manually dispatched iOS build `30874363387`.

Brutal status: **we are not close to 30 FPS yet**. The exact model converts previous
optimism into a falsifiable plan, but it produces zero measured emulator or iPhone FPS
gain, the app still builds the engine out, and the phone remains at the reported 0--4 FPS.
The next retained implementation must cover broad Thumb integer/control/read families
and then broad exact VFP transfer/compute/read families, preserving literal fallback for
faults and exceptional FP modes. Firmware-specific AOT remains possible only in a bespoke
pre-sign build for one imported image; it is not compatible with the current distributable
app, which imports user firmware after its executable is signed. A generic signed engine
remains the product route. It does not enter the app until a same-binary restored A/B
shows a structural gain large enough to justify physical-iPhone and final cold-boot tests.

### r498-r500: broad Thumb integer and DREAD-load coverage is substantial, not yet fast

r498 implemented Thumb families rather than individual hot halfwords. Immediate shifts,
small register/immediate ADD/SUB, all four imm8 ALU forms, all sixteen register-ALU
operations, non-PC high-register operations, PC/SP address formation and SP adjustment
reuse the signed A32 shifter and ALU semantics. Dynamic PC writes, BX/BLX, conditional
branches, BL, stack operations and product memory operations remained literal in that
tranche. A 16-instruction real-SoC oracle crosses timer boundaries and compares complete
serialized machines with the interpreter on Apple arm64.

The identical 7.100--7.110 B observer replay measured a real coverage change. Decoder
support rose from 44.747% to 50.302%, and modeled signed retirement rose from 4,062,729
to 4,570,357 instructions: **40.629% to 45.706%**, or +5.076 percentage points. That was
not automatically a performance win. Modeled calls increased by 305,444 and their mean
length fell from 2.183 to 2.110 instructions. The new arithmetic semantics were exact and
broad, but unsupported loads and control flow still split them into more short entries.
Calling that an FPS improvement would have been false.

r500 then adds every ordinary Thumb single-load shape to the product DREAD contract:
PC-relative, register-offset, immediate word/byte/halfword and SP-relative forms, including
signed byte and signed halfword results. The generated table grows from 24,005 to 24,050
firmware-independent handlers. Stores remain literal. An aligned cache hit reads through
the interpreter's already-proved 1 KiB host pointer and updates the existing hit counter.
An alignment guard or cache miss changes no guest state, refunds the unretired suffix and
returns at the exact load PC so `arm_step()` still owns the MMU walk, permission/alignment
fault, MMIO path and cache fill. A cold-cache SoC oracle deliberately exercises that
miss-to-interpreter-to-signed-hit lifecycle rather than prewarming every access.

The first r500 replay correctly refused to count the new instructions: its observer still
had an explicit A32-only address proof and classified 277,405 recognized Thumb loads as
`read-guard`. That run kept the old 45.706% modeled result and was diagnostic, not evidence
of failure or speed. The observer was then extended with an independent Thumb address and
alignment model and the same 10 M-entry replay was repeated. All identities closed:

| exact current product model | instructions | fetched share |
|---|---:|---:|
| decoder-supported | 5,307,377 | 53.076% |
| decoder-rejected | 4,692,112 | 46.924% |
| retirement-eligible after condition/DREAD checks | 5,156,763 | 51.570% |
| modeled signed retirement | **4,784,184** | **47.844%** |
| entry-gate refusal | 372,579 | 3.726% |

Across all A32 and Thumb reads the exact outcomes were 57,303 condition skips,
1,376,016 DREAD hits, 150,571 misses and 43 conservative alignment guards. Within Thumb,
879,312 instructions decoded, 841,456 were dynamically eligible and 766,070 were modeled
as retired. Relative to r498, the load tranche adds 213,827 modeled instructions while
reducing calls by 15,618; mean call length improves from 2.110 to 2.224. That is the first
new tranche here to improve both coverage and continuity in the exact model. It is still
residency, not execution time.

Both full observer runs exited OK with empty stderr, exact family/raw accounting and no
external-media failure. The final work-image SHA-256 remained
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`; the final PPM
remained `1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`, with 1,590
CLCD frames. These are output-equality checks, not a saved whole-machine comparison and
not a throughput or FPS result. The broad integer commit
`df69e572ab044b50516046a95df72a7a8671d5aa` passed core run `30875079567` and iOS run
`30875079573`. The load commit
`f7f69190aa0f7216980a8304370c922703cd54d2` passed core run `30876286838` and iOS run
`30876286829`. macOS-14 and macOS-15 each reported
`STATIC-A64-THUMB-READ-ORACLE exact=yes retired=23997`, 5,864/0 SoC assertions and
`STATIC-READ-ORACLE exact=yes hits=18 thumb=yes zero-prefix=yes partial-prefix=yes`.

Brutal status: **there is still zero measured emulator or iPhone FPS gain**. At 47.844%
coverage, even infinitely fast signed instructions cap the whole workload at **1.917332x**,
below the required 2.180475x. The engine remains compiled out of the normal iOS app, so
the phone correctly remains at the reported 0--4 FPS. Another 6.294 percentage points are
required merely to cross the impossible zero-cost coverage minimum, before allowing for
lookup, entry, cache misses, UIKit or device work. Remaining Thumb control alone cannot
provide that margin. The next evidence-backed tranche is therefore broad exact VFP
register/bitwise transfers and cache-hit VLDR, with arithmetic and exceptional modes
continuing to fall back until separately proved. Only after coverage clears the
mathematical gate does a same-binary restored A/B and then a physical-iPhone build become
honest.

### r501-r503: guarded VFP transfers and VLDR cross only the absolute coverage floor

r501-r502 add exact VFPv2 register and system-state families to the signed product path:
VMOV between core and single/double words, two-core-register transfers, VMRS/VMSR,
`VMRS APSR_nzcv,FPSCR`, and raw same-width VMOV/VABS/VNEG. These handlers do not assume
that decode-time VFP state remains live. Each execution rechecks CPACR, FPEXC and, where
required, FPSCR.Len before changing guest state; a failed guard returns the exact
unretired prefix to `arm_step()`. The generated, firmware-independent table grew from
24,050 to 24,612 handlers.

r503 adds pre-indexed, no-writeback single-register VLDR in both widths and offset
directions. It reuses the already-proved DREAD tag, generation and privilege checks.
Misalignment, a cache miss, or a double crossing the 1 KiB cache block changes no VFP
state and falls back at the exact instruction. A successful single load accounts for one
interpreter-equivalent read32 hit; a successful double accounts for two. Stores,
load-multiple and every arithmetic operation remained literal. The table is now 24,614
handlers.

The local 60-test Release suite and a strict `-Wall -Wextra -Werror` build passed. On both
macOS-14 and macOS-15, core run `30878278847` reported exact whole-SoC cold-fill and
signed-engine oracles for the VFP register and read paths, including disabled/Len guards,
partial prefixes, alignment and cache-block boundaries. iOS run `30878278855` built the
ad-hoc-signed app at the same exact commit
`965231b1eef6ae2014a502afb944013f572bc089`. Those jobs prove semantics and compilation;
they do not install the app or measure an iPhone.

The identical restored 7.100--7.110 B observer replay then closed every class, outcome,
VFP-family and stop identity:

| exact current product model | instructions | fetched share |
|---|---:|---:|
| decoder-supported | 6,097,490 | 60.978% |
| decoder-rejected | 3,901,999 | 39.022% |
| retirement-eligible after condition/DREAD/VFP checks | 5,933,923 | 59.342% |
| modeled signed retirement | **5,528,961** | **55.292%** |
| entry-gate refusal | 404,962 | 4.050% |

Relative to r500, the VFP tranches add 790,113 decoded and 744,777 modeled instructions.
That is a real structural coverage gain, not an FPS result. Calls rise from 2,150,751 to
2,632,258 and their mean falls from 2.224 to 2.100 instructions, because newly admitted
VFP entries are still split by unsupported arithmetic, compare, conversion and store
families. The run exited OK with empty stderr and 1,590 CLCD frames. Its work-image and
PPM SHA-256 values remained respectively
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.
Those output hashes are strong replay checks, but they are not a serialized whole-machine
A/B and they do not validate native execution on this x86 host.

Brutal status: this is the first revision to cross the absolute 54.138% zero-cost floor,
but it crosses by only **1.154 percentage points**. Infinite-speed signed instructions
would cap the workload at **2.236758x**; reaching the required 2.180475x at the current
coverage would require the covered region itself to run about **47.914x** faster. That is
not a credible practical margin. There is still **zero measured emulator or iPhone FPS
gain**, the normal app still builds this engine out, and the phone remains at the reported
0--4 FPS. The next trace-selected tranche is exact VCMP/VCMPE (1.810% of all fetched
instructions), whose comparison, NaN, signed-zero, FZ and IOC/IDC behavior can be expressed
with integer bit logic. Arithmetic and narrowing conversion stay literal until their
rounding and cumulative-exception behavior is separately proved.

### r504: exact VCMP improves both coverage and continuity, not measured FPS

r504 admits every scalar VFPv2 VCMP/VCMPE register and `#0` form in single and
double precision. The signed handlers do not use the host floating-point unit.
They classify and order IEEE-754 encodings with integer operations, including quiet and
signalling NaNs, infinities, signed zero and FZ input denormals; they overwrite FPSCR.NZCV
and preserve or accumulate IOC/IDC exactly. CPACR, FPEXC, FPSCR.Len and all trap-enable
bits remain live runtime guards, so a disabled or exceptional mode returns the exact
unretired prefix without changing guest state. The generated table grows by two generic
handlers, from 24,614 to 24,616.

The local 60-test Release suite and strict warning build passed. Core run `30879381816`
and iOS run `30879381822` are fully green at exact commit
`c2b4f99abd841a5b17e34f4c1d3f6cfc2693c635`. Both Apple runners executed 15 independent
single/double, NaN, FZ, signed-zero and fallback-prefix cases and a complete serialized
23,999-instruction SoC comparison. This is substantially stronger than accepting the
decoder shape; it still is not a physical-iPhone run.

The identical 10 M-entry restored observer replay exited OK with empty stderr, 1,590 CLCD
frames, exact accounting, and unchanged work-image/PPM hashes. Its current product model is:

| r504 exact product model | instructions | fetched share |
|---|---:|---:|
| decoder-supported | 6,278,476 | 62.788% |
| retirement-eligible | 6,114,909 | 61.152% |
| modeled signed retirement | **5,706,014** | **57.063%** |
| entry-gate refusal | 408,895 | 4.089% |

All 180,986 observed comparisons now decode; 177,053 more instructions are modeled as
retired. Unlike the earlier register tranche, continuity also improves: modeled calls
fall by 98,613 to 2,533,645 and mean call length rises from 2.100 to 2.252 instructions.
That is a substantial structural result. It is not throughput. The infinite-speed ceiling
rises to **2.328997x**, but hitting 2.180475x at this coverage still requires the covered
region to be about **19.511x** faster. There remains zero measured emulator or iPhone FPS
gain. The next broad safe candidate is exact single-to-double VCVT widening, which dominates
the hottest remaining conversion encodings; narrowing and arithmetic still require their
own rounding/exception proof.

### r505: exact VCVT widening adds 1.598 points, not measured FPS

r505 admits the complete scalar VFPv2 `VCVT.F64.F32` family whose destination is one of
the implemented `d0`--`d15` registers. It still rejects double-to-single narrowing and
high-D-register destinations. The generated AArch64 handler never borrows host floating
state: integer operations widen normals exactly, normalize binary32 subnormals, flush FZ
input denormals to signed zero with IDC, preserve infinities and quiet-NaN payloads, quiet
signalling NaNs with IOC, and implement DN default-NaN selection. Live CPACR, FPEXC,
FPSCR.Len and trap-enable guards fail before changing guest state. The generic handler
table grows from 24,616 to 24,617 entries.

The portable VFP edge suite reports 552 assertions passed, the full local Release suite
reports 60/60 tests passed, and the strict warning build passes. Exact commit
`8f0509808fcd20af972582bb30628ca147bef037` is fully green in core run `30880172743`
and iOS run `30880172791`. On both macOS-14 and macOS-15, the native arm64 jobs report
`STATIC-VFP-WIDEN-ORACLE exact=yes ops=15`, covering finite, subnormal, NaN, FZ and
fallback-prefix cases, plus a serialized 23,999-instruction SoC comparison with
`widen=yes`. That proves the generated handler on Apple arm64; it does not measure the
normal app or a phone.

The unchanged 7.100--7.110 B restored replay completed in 32.015 host seconds with empty
stderr and exact accounting. Splitting the formerly combined conversion census shows
161,207 single-to-double operations and 62,752 double-to-single operations. Every widening
operation now decodes; every narrowing operation remains rejected. The product model is:

| r505 exact product model | instructions | fetched share |
|---|---:|---:|
| decoder-supported | 6,439,683 | 64.400% |
| retirement-eligible | 6,276,116 | 62.764% |
| modeled signed retirement | **5,865,830** | **58.661%** |
| entry-gate refusal | 410,286 | 4.103% |

Relative to r504, modeled retirement increases by 159,816 instructions, or **1.598
percentage points**. Continuity again improves: modeled calls fall by 53,072 to 2,480,573
and mean run length rises from 2.252 to 2.365 instructions. The work-image SHA-256 remains
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE`; the PPM remains
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`, with 1,590 CLCD
frames. These hashes prove deterministic replay output, not native execution timing.

Brutal status: this raises the infinite-speed ceiling from 2.328997x to **2.419041x** and
widens the margin over the impossible zero-cost coverage floor to 4.523 percentage points.
It also lowers the covered-region speedup required for the 30-FPS capacity target from
19.511x to **12.970x**. That is real progress, but 12.970x is still a severe requirement.
There is still zero measured emulator or iPhone FPS gain, the signed engine remains built
out of the normal iOS app, and the phone therefore remains at the reported 0--4 FPS. The
next tranche must address a broad remaining family and demonstrate both exact Apple-arm64
semantics and actual native throughput; merely adding another hot encoding is no longer
enough.

### r506-r508: product-entry timing rejects coverage-only and removes repeated validation

r506 times the public product runner once per already-decoded block instead of repeating a
single native call internally. That distinction is load-bearing: the real engine enters the
runner once for every modeled signed call, and r505 averages only 2.365 guest instructions per
call. The benchmark includes native entry/exit, context construction and the same public block
validation the product used. It deliberately excludes SoC cache lookup, decode misses, timer and
IRQ gates, device ticks, framebuffer publication and UIKit, so it remains a ceiling rather than
phone or emulator FPS.

The fully validated wrapper does not approach the 12.970x covered-region speedup required by the
r505 residency model. Across the two long Apple-arm64 measurements its speedup is below 1x for a
one-instruction block, about 1.07x for length two, 1.43--1.71x for length three, 1.71--1.77x for
length four, 2.01--2.11x for length eight and 1.98--2.12x for length sixteen. Even the most
favourable synthetic row reaches only **2.121x**. Coverage alone therefore cannot rescue the
existing entry architecture; that is a measured rejection of the previous plan, not a tuning
opinion.

r507 isolates the cost by adding a cache-owned decoded-block contract. It shares the identical
native execution and context path, but does not rescan every uop on every hot entry. The contract
is deliberately narrow: only an owned, unmodified result of the exact decoder may use it, and it
still checks dynamic CPU state, PC/T state, RAM shape, block bounds and the final exit record.
The benchmark compares interpreter, fully validated and decoded-contract architectural state on
every repetition. Long-run medians were:

| block length | macOS 14 validated | macOS 14 decoded | macOS 15 validated | macOS 15 decoded |
|---:|---:|---:|---:|---:|
| 1 | 0.635x | 0.878x | 0.564x | 0.831x |
| 2 | 1.077x | 1.981x | 1.073x | 2.025x |
| 3 | 1.432x | 3.085x | 1.708x | 3.656x |
| 4 | 1.706x | 4.345x | 1.766x | 4.801x |
| 8 | 2.009x | 8.958x | 2.114x | 8.636x |
| 16 | 1.980x | 15.361x | 2.121x | 18.265x |

This is a substantial and internally consistent result: repeated validation was a real
bottleneck, and eliminating it increasingly matters as blocks grow. It also makes the remaining
problem more explicit. One-instruction entries are still slower than interpretation; two are
only about 2x; and the real modeled stream is dominated by short calls. It would be dishonest to
apply the length-sixteen number to a 2.365-instruction mean or to claim that this curve predicts
an FPS value.

r508 switches the default-off whole-SoC engine to that decoded contract. The cache still compares
the live fetch host, PC, translation generation, privilege, instruction state and raw bytes before
calling it, preserving self-modifying-code invalidation. Exact commit
`a959e2ba5b17741ebfc684bf0e32b2a7fa2da15a` passes core run `30881750130` and iOS run
`30881750150`. Both Apple runners report
`STATIC-A64-SOC-ORACLE exact=yes retired=24095 smc=yes decoded=yes`; the signed and interpreter
machines serialize byte-identically after timer crossings, a bounded one-instruction prefix and
a live code mutation. Strict warnings, sanitizers and every platform matrix job are green. The
preceding measurement commits `c53fbdeabdfba3022546ee6263d9c06e0337263e` and
`3f1bd9699da793f65a68fdc9e00080f3cf5e5c46` also passed their exact-SHA core and iOS runs.

Brutal status: **this is a proven engine optimization, but still not a measured emulator or
iPhone FPS improvement**. The normal iOS app still builds the engine out, the only physical-phone
report remains 0--4 FPS, and there has been no same-binary restored A/B or cold boot. The decoded
ceiling is nowhere near 12.970x at the short lengths the trace currently forms. The next test must
measure the complete `s5l8900_run()` path at lengths 1, 2, 3, 4, 8 and 16, including the real
cache/raw-byte witness, entry gates and device-time boundaries, then combine that curve with an
exact per-length replay histogram. That evidence will decide whether cache/gate work and block
chaining can close the gap or whether broader Thumb/VFP coverage must first create longer runs.

### r509-r510: the real SoC curve predicts a modest gain, not 30 FPS

r509 moves timing through the complete app-facing `s5l8900_run()` path. For each synthetic
length it initializes independent signed and interpreter machines, warms two loop iterations
outside the clock, and then times 20 million guest instructions. The signed side includes the
real 1,024-entry cache index, live raw-byte self-modification witness, CPU/fetch/interrupt gates,
timebase-edge splitting and device ticks. The interpreter side uses the same machine API. Every
repetition ends in a complete serialized-machine comparison, and every timed signed instruction
must appear in the engine's retirement counter. The loops are still synthetic, MMU-off and free
of firmware, framebuffer publication and UIKit, so the result is not phone or emulator FPS.

r510 extends that curve to every possible product call length and prints the observer's exact
one-through-sixteen histogram. Exact commit
`860d17335e505d02f57a735cf492b4ae336ad4cd` is fully green in core run `30883223864` and
iOS run `30883236735`; both Apple runners passed all sixteen byte-identical machine comparisons.
The unchanged 7.100--7.110 B replay produced:

| length | modeled calls | modeled instructions | modeled share | macOS 14 SoC speedup | macOS 15 SoC speedup |
|---:|---:|---:|---:|---:|---:|
| 1 | 1,018,251 | 1,018,251 | 17.359% | 0.499x | 0.535x |
| 2 | 697,579 | 1,395,158 | 23.784% | 1.250x | 1.209x |
| 3 | 305,484 | 916,452 | 15.624% | 1.763x | 1.779x |
| 4 | 227,229 | 908,916 | 15.495% | 2.524x | 2.293x |
| 5 | 84,414 | 422,070 | 7.195% | 2.459x | 2.661x |
| 6 | 58,105 | 348,630 | 5.943% | 2.857x | 2.904x |
| 7 | 31,420 | 219,940 | 3.750% | 3.188x | 2.919x |
| 8 | 17,432 | 139,456 | 2.377% | 3.606x | 3.811x |
| 9 | 6,070 | 54,630 | 0.931% | 3.790x | 4.010x |
| 10 | 5,854 | 58,540 | 0.998% | 3.938x | 4.439x |
| 11 | 9,107 | 100,177 | 1.708% | 4.115x | 4.241x |
| 12 | 2,849 | 34,188 | 0.583% | 4.133x | 4.522x |
| 13 | 4,826 | 62,738 | 1.070% | 4.273x | 4.877x |
| 14 | 851 | 11,914 | 0.203% | 4.255x | 4.609x |
| 15 | 2,862 | 42,930 | 0.732% | 4.353x | 4.587x |
| 16 | 8,240 | 131,840 | 2.248% | 4.440x | 4.674x |

All 2,480,573 calls and 5,865,830 modeled instructions land in exactly one row. The table
exposes the current failure mode: **41.143% of modeled signed instructions are in length-one or
length-two calls**, while only 8.473% reach lengths nine through sixteen. Length one is about half
the interpreter's speed. The attractive 4.4--4.7x length-sixteen result therefore describes only
2.248% of the modeled signed work.

There is no unique honest way to turn synthetic ALU/branch loops into a firmware prediction.
Two explicit weightings bound how much optimism is hidden. Treating every covered guest
instruction as equal-cost and applying each length's relative curve gives a covered-region
speedup of 1.287x on macOS 14 and 1.312x on macOS 15; Amdahl's law at 58.661% coverage gives only
**1.150x and 1.162x whole-workload speedup**. Retaining each synthetic reference loop's measured
seconds gives the more favourable covered values 1.474x and 1.437x, or **1.233x and 1.217x
overall**. These are alternative synthetic interpretations, not a confidence interval and not a
same-binary A/B. Applied only as scale to the 15.732668 M/s r485 baseline, they span roughly
18.10--19.39 M/s, still far below the 34.304688 M/s capacity target.

The replay exited OK in 32.9 host seconds with empty stderr. The histogram, outcome, class,
stop and gate identities are exact; CLCD remains at 1,590 frames. Its work image and PPM hashes
remain respectively
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.

Brutal status: **there is still no measured emulator or phone FPS improvement, and this model is
nowhere near 30 FPS**. The normal app still builds the engine out and the physical phone remains
at the reported 0--4 FPS. The result does establish that longer signed calls are valuable and
that the current short-call distribution is the primary obstacle. Of 2,480,573 calls, 2,268,911
stop on an ineligible instruction; only 102,431 stop on a currently signed branch, so branch
chaining alone would be premature. The largest broad rejected boundary family is A32 immediate
B/BL: 1,093,302 of 1,201,691 observations are rejected, **10.933% of the full fetched trace**.
Exact conditional and link semantics are therefore the next coverage tranche. If admitted, they
can convert a large literal boundary into the tail of an existing signed call; only after that
replay will branch-to-target chaining have enough volume to justify its added dispatcher risk.

### r511-r512: exact A32 B/BL improves coverage, still not 30 FPS

r511 admits the complete terminal A32 immediate branch family without runtime-generated code.
Fourteen signed handlers select conditional B exits from live NZCV; fifteen conditional/AL BL
handlers additionally write LR only on a taken link. The decoder record carries both the taken
target and natural fallthrough, full validation proves the dynamic exit is terminal and
word-aligned, and the cache-owned fast contract checks that the decoder's dynamic-exit bit still
matches the penultimate handler. Unconditional AL B retains the old compact fixed END record.
Nothing chains to another block yet.

Exact commit `42e6247a737b833306ae7fa452806a663181a946` is fully green in core run
`30885206122` and iOS run `30885206182`. Both Apple runners passed a 58-case native matrix over
every condition, taken and not-taken outcomes, positive and negative targets, taken-only LR
writes and a corrupted decoded-contract refusal. The real SoC oracle retired 19,999 of 20,000
control-flow-heavy loop instructions through signed handlers and serialized byte-identically to
the interpreter. Strict warnings, ASan/UBSan, all 60 local tests, every hosted platform job and
the ad-hoc-signed IPA build are green.

The first relinked replay counted instruction coverage correctly, but its call histogram still
recognized only the old unconditional B as terminal. It therefore merged a failed conditional
B/BL's sequential fallthrough into the same modeled call even though the current product
returns from every dynamic branch handler. This was caught while auditing the proposed chaining
contract, before using the optimistic histogram to implement it. Commit
`75b3bfde7121e76d5a3c21a9d0e390c30ed9f396` makes every admitted A32 immediate B/BL terminal in
the current-engine observer. The corrected identical 7.100--7.110 B replay reports:

- decoder support rises from 64.400% to 75.334% of fetched instructions;
- modeled signed retirement rises from 5,865,830 (58.661%) to 6,956,087 (69.564%);
- all 1,201,691 observed A32 B/BL instructions decode, and 1,179,537 remain retirement-modeled;
- calls rise from 2,480,573 to 2,628,811 because newly supported branches can form their own
  signed calls, while mean length still rises from 2.365 to 2.646;
- length-one/two modeled work falls from 41.143% to 32.441%; length nine-through-sixteen remains
  nearly flat at 8.534%, rather than the erroneous first replay's 21.674%.

| length | modeled calls | modeled instructions | modeled share |
|---:|---:|---:|---:|
| 1 | 838,292 | 838,292 | 12.051% |
| 2 | 709,180 | 1,418,360 | 20.390% |
| 3 | 491,070 | 1,473,210 | 21.179% |
| 4 | 274,165 | 1,096,660 | 15.765% |
| 5 | 140,650 | 703,250 | 10.110% |
| 6 | 69,598 | 417,588 | 6.003% |
| 7 | 34,625 | 242,375 | 3.484% |
| 8 | 21,590 | 172,720 | 2.483% |
| 9 | 12,718 | 114,462 | 1.645% |
| 10 | 4,536 | 45,360 | 0.652% |
| 11 | 6,129 | 67,419 | 0.969% |
| 12 | 8,717 | 104,604 | 1.504% |
| 13 | 4,639 | 60,307 | 0.867% |
| 14 | 1,571 | 21,994 | 0.316% |
| 15 | 1,810 | 27,150 | 0.390% |
| 16 | 9,521 | 152,336 | 2.190% |

Reweighting the unchanged Apple SoC-entry medians gives an equal-instruction covered speedup of
1.454x/1.475x on macOS 14/15, or **1.277x/1.289x whole-workload** at 69.564% coverage. Retaining
the synthetic reference loops' measured seconds gives 1.622x/1.596x covered and
**1.364x/1.351x overall**. Used only as scale against r485, those alternatives span about
20.10--21.45 M/s. They are still synthetic interpretations, not confidence bounds, measured
firmware timing, emulator FPS or phone FPS; the 30 FPS capacity target remains 34.304688 M/s.

The corrected replay exited OK in about 33 host seconds with empty stderr and 1,590 CLCD frames. Its work
image and PPM hashes remain byte-identical to r510:
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.

Brutal status: **this is a real structural gain, but it still does not prove any emulator or
physical-phone FPS gain and the corrected estimate remains far below 30 FPS**. The normal iOS
app still compiles the default-off engine out, and the only phone observation remains 0--4 FPS.
The current model stops 1,192,688 calls on signed branches; flow stops remain only 4,345. That is
finally enough branch volume to justify bounded branch-to-target chaining, but the erroneous
fallthrough merge shows why its accounting and tests must be explicit. The next tranche must
preserve the caller/timer budget, target fetch translation, cache/raw-byte witness, privilege and
generation keys, interrupt gates and exact interpreter state; a faster chain that skips any of
those boundaries is not an optimization this project will accept.

### r513: bounded branch chaining removes outer calls but exposes the native-entry wall

r513 implements the justified branch-to-target chain in the real product engine. One
`s5l8900_static_a64_try()` invocation may now execute several decoded blocks, but its total remains
bounded to sixteen guest instructions and to the nearer caller or device-time edge. Every new head
rechecks CPU/interrupt state, privilege, fetch translation generation, the 1 KiB fetch block, the
direct-mapped cache key and the complete raw-byte witness. A target outside the currently proved
fetch block returns to `arm_step()`, which still owns translation, MMU faults and MMIO. No cached
host pointer is followed speculatively and no runtime code is generated.

The first pushed workflow for exact implementation commit
`694d88aaedb0bfff9a979b681962b5eb62b739e9` failed both Apple native jobs even though all 60 tests,
5,892 SoC assertions and the new branch snapshot comparison passed. The failed check expected a
synthetic length-sixteen loop to report zero chains. That expectation was wrong: once a timer edge
ends a call partway through the loop, the next sixteen-instruction call can legitimately execute
the tail, branch, and chain into the head while still retiring exactly sixteen total instructions.
This was a test-design error, not an engine-semantic failure, and it was not hidden by rerunning.

Commit `6e390f090a74d7c94142288630dbc6b0c2bdc19e` replaces that proxy with a direct native caller-bound
oracle. After one literal fetch warm-up, a one-step public call must retire one signed self-branch
and chain zero blocks; a sixteen-step call must retire sixteen and chain exactly fifteen. Each end
state must serialize byte-identically to a literal machine. Both macOS-14 and macOS-15 report:

```
STATIC-A64-BRANCH-SOC-ORACLE exact=yes retired=19999 chains=11801 conditional=yes link=yes taken=yes fallthrough=yes
STATIC-A64-CHAIN-BOUND-ORACLE exact=yes one-retired=1 one-chains=0 sixteen-retired=16 sixteen-chains=15
```

Exact-SHA core run `30887778520` is fully green across both Apple native jobs, Linux, Windows,
ASan/UBSan, warnings-as-errors and the default-off rebuild. Exact-SHA iOS run `30887778528` built
and packaged the ad-hoc-signed app. The earlier implementation SHA's iOS run `30887357924` was also
green; its core run `30887357906` remains correctly red because of the invalid zero-chain workflow
assertion above.

The independently relinked `work/r513-branch-chain-10m` replay restored the identical r445 7.100 B
checkpoint and stopped at exactly 7.110 B with status OK in 32.7 host seconds. Stderr was empty,
external-media failures were zero and CLCD remained at 1,590 frames. The 466,825,216-byte work
image and 460,815-byte PPM retained the exact SHA-256 values
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.

Coverage correctly does not change: 7,532,985/9,999,489 fetched instructions decode and 6,956,087
(69.564%) are modeled as signed retirement. What changes is call shape:

| length | modeled calls | modeled instructions | modeled share |
|---:|---:|---:|---:|
| 1 | 574,613 | 574,613 | 8.261% |
| 2 | 388,832 | 777,664 | 11.180% |
| 3 | 241,130 | 723,390 | 10.399% |
| 4 | 158,206 | 632,824 | 9.097% |
| 5 | 95,554 | 477,770 | 6.868% |
| 6 | 82,579 | 495,474 | 7.123% |
| 7 | 53,335 | 373,345 | 5.367% |
| 8 | 42,485 | 339,880 | 4.886% |
| 9 | 28,197 | 253,773 | 3.648% |
| 10 | 24,186 | 241,860 | 3.477% |
| 11 | 18,108 | 199,188 | 2.864% |
| 12 | 14,156 | 169,872 | 2.442% |
| 13 | 16,755 | 217,815 | 3.131% |
| 14 | 12,654 | 177,156 | 2.547% |
| 15 | 8,857 | 132,855 | 1.910% |
| 16 | 73,038 | 1,168,608 | 16.800% |

All 1,832,685 calls and all 6,956,087 modeled instructions close exactly. Relative to r512,
outer calls fall by 796,126 (**30.285%**) and mean length rises from 2.646 to **3.796**. Modeled
work in length-one/two calls falls from 32.441% to **19.441%**; length nine-through-sixteen rises
from 8.534% to **36.819%**. The model observed 841,386 successfully chained heads. Consequently,
the native runner would still be entered 2,674,071 times: **1.459 blocks per outer call** and
2.601 instructions per native block. That is 45,260 (**1.722%**) more native block entries than
r512 even while outer machine calls fall sharply. Stop accounting remains exact at
73,038/101,007/73/0/4,345/201,722/1,452,500/0 for
cap/timer/caller/branch/flow/fetch-block/ineligible/observer.

The long three-repetition Apple SoC curves explain why the native-block distinction is
load-bearing. Selected pre-chain (`30885962821`) versus r513 speedups are:

| synthetic block length | macOS 14 pre / chained | macOS 15 pre / chained |
|---:|---:|---:|
| 1 | 0.492x / **0.608x** | 0.637x / **0.705x** |
| 2 | 1.104x / **1.270x** | **1.404x** / 1.328x |
| 3 | **1.576x** / 1.536x | **1.691x** / 1.413x |
| 4 | **2.066x** / 1.890x | 2.124x / **2.226x** |
| 8 | **3.508x** / 2.234x | **3.346x** / 2.693x |
| 16 | **4.218x** / 2.080x | **4.484x** / 2.264x |

The one-instruction case improves on both hosts because one outer call can amortize device and
machine-loop work across many blocks. Long synthetic loops regress because a timer-created
mid-loop phase persists: repeated tail-plus-head chains pay the full generated native
prologue/epilogue twice per outer call instead of returning at the branch and realigning. Host
rates also drift between workflow runs, so these are architecture diagnostics, not a clean A/B.

Brutal status: **r513 is exact and materially reduces modeled outer dispatch, but it still does
not prove a firmware, emulator or iPhone FPS gain**. Applying the old call-length curve to the new
histogram would be false because equal-length calls can contain different numbers of native
blocks. The normal iOS app still compiles the engine out and the only phone observation remains
0--4 FPS. The next architectural measurement must retain both instructions and internal block
count, and the next serious prototype must keep guest registers/native context live across
validated block heads. More isolated opcode additions or a flattering length-only weighting
would be whack-a-mole, not a credible path to 30 FPS.

### r514: joint block shape quantifies the persistent-context opportunity

r514 adds a read-only observer to `bootkernel --sequence-profile`; it does not change engine
dispatch or guest state. For every modeled outer call it records both total guest instructions
and successful native blocks, then independently closes three identities: calls, instructions and
blocks. The last quantity must equal outer calls plus chained heads. The identical restored replay
reports

```
exact shape calls/instructions/blocks=1832685/6956087/2674071 model=1832685/6956087/2674071  EXACT
```

The successful-block distribution is:

| native blocks in outer call | calls | call share | guest instructions | instruction share | native block entries |
|---:|---:|---:|---:|---:|---:|
| 1 | 1,370,748 | 74.795% | 3,182,960 | 45.758% | 1,370,748 |
| 2 | 276,965 | 15.113% | 1,612,741 | 23.185% | 553,930 |
| 3 | 87,115 | 4.753% | 799,566 | 11.494% | 261,345 |
| 4 | 52,303 | 2.854% | 670,595 | 9.640% | 209,212 |
| 5 | 25,851 | 1.411% | 379,598 | 5.457% | 129,255 |
| 6 | 6,371 | 0.348% | 98,388 | 1.414% | 38,226 |
| 7 | 1,305 | 0.071% | 19,824 | 0.285% | 9,135 |
| 8 | 6,023 | 0.329% | 96,351 | 1.385% | 48,184 |
| 9 | 6,004 | 0.328% | 96,064 | 1.381% | 54,036 |

Only 25.205% of outer calls cross multiple native blocks, but those calls carry **54.242%** of
modeled guest instructions. The 841,386 chained heads are **31.465%** of all 2,674,071 successful
native-block entries. Those are the entries at which the current implementation repeats the full
generated-function boundary: save/restore AAPCS64 callee-saved registers, reload/write back pinned
guest registers and reconstruct dispatcher state. A persistent context cannot remove the required
per-head raw-byte/cache/fetch/interrupt checks, but it can potentially avoid that repeated native
register round trip at exactly those 841,386 transitions. This is a measured hot-path population,
not an assumed one.

`work/r514-chain-shape-10m` was independently relinked, restored the same r445 checkpoint and
stopped at exactly 7.110 B retired instructions with status OK in 31.3 host seconds. Stderr was
empty, external-media failures were zero and CLCD again reached 1,590 frames. The work image and
screen PPM remain byte-identical to r513 at SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`. Both complete local suites
pass 60/60. Observer commit `1a9833e337c0e7b2d08f2289dcf84e45ffb14fb7` is on `main`; exact-SHA
core run `30888920549` is green in all eight jobs, including macOS 14/15 native execution,
ASan/UBSan, warnings-as-errors and the default-off build.

Brutal status: **r514 proves that persistent native context addresses substantial real signed
work, but it proves zero FPS improvement by itself**. The table counts successful native blocks;
it does not count refused/zero-retire native attempts, measure prologue cycles or provide a
same-binary firmware A/B. The normal iOS app still compiles this engine out, and the only physical
iPhone observation remains 0--4 FPS. The justified next experiment is one bounded persistent
native invocation across these already-validated heads, preserving the total sixteen-instruction
caller/timer limit, dynamic branch PC, NZCV, exact read-miss prefix semantics and final serialized
CPU state. It must beat the current signed path on native Apple hosts before it can be considered
for the app; if the C validation callback costs as much as the removed register round trip, the
prototype should be rejected rather than narrated as progress.

### r515: persistent native context is exact, but the C callback loses

Implementation commit `f72e67a184bedf5addb28d840b1dae1bd1b6a02d` adds an experimental
same-binary control without changing the legacy signed path or the normal app default. One signed
invocation may keep guest r0--r7 and SP in AAPCS64 callee-saved registers across block heads. It
saves NZCV around a C head-selection callback, repeats the interrupt/fetch/cache/raw-byte contract,
retains the total sixteen-instruction caller/timer bound and writes architectural state back once
at the final exit or exact read/VFP miss prefix. Persistent mode must be explicitly enabled after
the already optional signed engine.

This difficult part is correct on both Apple hosts. Exact-SHA core run `30890861908` is green in
all eight jobs and exact-SHA iOS run `30890862137` builds the ad-hoc-signed IPA. macOS 14 and 15
both pass the required native markers; the completed macOS 14 log is representative:

```
STATIC-A64-SOC-ORACLE exact=yes retired=24095 smc=yes decoded=yes persistent=yes
STATIC-A64-BRANCH-SOC-ORACLE exact=yes retired=19999 chains=11801 conditional=yes link=yes taken=yes fallthrough=yes persistent=yes
STATIC-A64-CHAIN-BOUND-ORACLE exact=yes one-retired=1 one-chains=0 sixteen-retired=16 sixteen-chains=15 persistent-chains=15
```

Correctness is not speed. Benchmark commit `d427dbea0470dbc9179327f31c3568a09964d2ec`
rotates three separately initialised machines in one binary: literal interpreter, legacy signed
chaining and persistent signed chaining. Warm-up stays outside timing; legacy and persistent chain
counts must match; both complete snapshots must match the reference before a row is printed.
Exact-SHA core run `30891229180` is green in all eight jobs. Its three-repetition
`persistent-over-signed` medians are:

| synthetic block length | macOS 14 | macOS 15 |
|---:|---:|---:|
| 1 | **1.144x** | **1.049x** |
| 2 | 0.932x | 0.874x |
| 3 | 0.923x | 0.906x |
| 4 | 0.821x | 0.869x |
| 5 | 0.937x | 0.818x |
| 6 | 0.815x | 0.863x |
| 7 | 0.806x | 0.853x |
| 8 | **1.001x** | 0.877x |
| 9 | 0.846x | 0.907x |
| 10 | 0.834x | 0.889x |
| 11 | 0.926x | 0.875x |
| 12 | 0.829x | 0.905x |
| 13 | 0.748x | 0.919x |
| 14 | 0.845x | 0.905x |
| 15 | 0.715x | 0.901x |
| 16 | 0.847x | 0.893x |

The one-instruction case removes enough native ABI traffic to win by 14.4%/4.9%; length eight
only ties on macOS 14. Every other row loses, commonly by 7--18% and by as much as 28.5%. Absolute
host rates drifted while three paths shared the hosted machines, so the conclusion uses only the
same-row, same-run persistent/legacy ratio. Exact snapshots and identical transition counts rule
out the tempting explanation that the persistent arm did less work.

Brutal status: **the callback-based persistent prototype is rejected as a performance path**. It
proves that pinned state can cross dynamic branches and exact miss exits without semantic drift,
but its nested C/indirect callback, NZCV crossing and dispatcher reconstruction cost more than the
register round trip they replace for the workload shapes that matter. It remains opt-in as a
correctness scaffold and must not be enabled in the app. This is negative performance evidence,
not an FPS gain; the app remains compiled without the engine and the only physical-phone report
remains 0--4 FPS.

The next justified architecture is a callback-free signed graph dispatcher. A hot fetch-block
cache must expose predecoded head descriptors to ordinary build-time-signed assembly; the native
boundary must look up the dynamic exit PC, enforce the remaining budget and compare that head's
raw-byte witness without returning through C. Stable MMU/privilege/interrupt facts may be proved
once per bounded invocation only because the admitted subset cannot mutate them and devices do not
tick inside the batch. A missing, colliding, stale, cross-fetch-block or oversized descriptor must
return to the legacy path. No runtime code generation, writable executable memory or relaxed guest
contract is justified by these results.

### 2026-08-04: callback-free graph dispatch wins its synthetic gate and enters the app

Commit `7afdddce4c30eaa9e05938f605e6c76b37295378` implements that architecture. Each
1 KiB fetch block has a fixed 512-slot, 128-byte data-only descriptor table indexed by
instruction offset. After a C-validated first head, ordinary build-time-signed AArch64 assembly
selects dynamic branch targets without a callback. Every accepted head still checks the complete
PC, host fetch pointer, translation generation, privilege, ARM/Thumb state, remaining caller/timer
budget and raw instruction witness. A missing, colliding, stale, cross-block or oversized node
returns the exact completed prefix. Runtime read or VFP misses retain the existing exact partial
accounting. There is no runtime code generation and no writable executable page.

The graph oracle deliberately makes self-branches at `0x000` and `0x400` collide in the same
descriptor slot, then requires the original cached head to republish and chain fifteen times. The
complete Apple-host markers include byte-identical serialized machines, live SMC, dynamic
conditional/link branches, a one-instruction caller bound, the sixteen-instruction total bound,
raw-witness rejection and collision recovery. Two benchmark-contract corrections are recorded in
`a35410637cc6c2fad50aa93f926741bc783becf5` and
`736f540d3d3a1b6869eb16abc5743b7ccdacba3b`: a fixed graph node may correctly stop when it is
longer than the timer remainder, and a length-sixteen node correctly has zero internal graph
transitions because it consumes the whole bound. Neither correction changes engine semantics.

Commit `f11296fb8a9ea875fa11d9d06d8847c04f5bf2eb` removes an unconditional 128-byte
descriptor republish on every outer entry. Exact-SHA core run `30894417886` is green in all eight
jobs. In its long three-repetition Apple measurements the graph beats the legacy signed path at
all sixteen synthetic lengths: by 4.9--48.9% on macOS 14 and 18.4--66.2% on macOS 15. Those are
same-row graph/legacy ratios, not firmware or phone FPS. They prove that removing the callback and
repeated native register boundary is worthwhile; they do not establish how the restored firmware's
mixed graph shapes weight those rows.

The product integration is no longer hypothetical. Commit
`87b40ba69c99b387220d9b0a31acb9426d63e08c` makes the iOS target generate and link the
signed handler assembly, compile the generic engine, and fail initialization rather than silently
fall back if the signed graph is unavailable. It does not touch or enable the JIT. Commit
`db94bb5ad8e0f7cc6ff8a838b6a10567024882e9` fixes only the linked-symbol CI check. Core run
`30894814704` is green in all eight jobs and iOS run `30894954810` builds, fake-signs and uploads
the graph-enabled IPA. That proves the actual app binary contains the engine. It does not prove the
binary was installed or made a physical iPhone faster.

Brutal status: **the callback-free graph is substantial architecture progress, but it is not a
30 FPS result**. The only physical-phone observation remains the older, uncontrolled 0--4 FPS
report. No connected iPhone was available for this build, and a restored Windows firmware replay
cannot execute AArch64 signed handlers. A cold desktop boot would therefore test guest correctness
but say nothing about this optimization's native path. The first authority for speed is an
exact-build physical-device run, with execution and publication measured separately; final
acceptance still requires a cold boot after the performance path is selected.

### 2026-08-04: block-sized SMC witnesses improve the short cases, still not 30 FPS

The first graph audit found that every cache hit compared the entire decode candidate: as many as
64 bytes even when the cached executable block was one instruction. That work is not required for
correctness. A supported cache entry depends on the bytes of the instructions it can execute. If a
later formerly unsupported instruction changes, retaining the shorter prefix is still exact:
the signed block returns at its old end and `arm_step()` or a separately decoded next head owns the
following PC. An unsupported entry needs only its first instruction, because changing later bytes
cannot make instruction zero supported while its own encoding is unchanged. Live PC-relative and
VFP loads remain guarded data reads; their loaded bytes were never instruction-witness bytes.

Commit `fa2f68a155abe2d543044073162f8faa3732773e` implements that narrower contract without
a dirty bitmap or write-notification dependency. This distinction matters: the snapshot code
already documents why a forgotten RAM writer makes dirty tracking silently wrong. Immediate SMC
inside an executable prefix still invalidates by byte comparison. The native graph-bound oracle
now pins a one-instruction self-branch to an exact four-byte witness and still proves fifteen
callback-free transitions, collision republish and byte-identical final machine state.

Both complete local suites pass 60/60; the focused machine test passes 5,832 assertions. Exact-SHA
core run `30896405607` is green in all eight jobs, including both Apple-arm64 native runs,
warnings-as-errors and ASan/UBSan. Exact-SHA iOS run `30896409779` builds and packages the real app.
Against the preceding exact graph curve, normalized graph/interpreter speedups change as follows:

| synthetic block length | macOS 14 | macOS 15 |
|---:|---:|---:|
| 1 | **+18.1%** | **+20.9%** |
| 2 | **+19.1%** | **+19.8%** |
| 3 | **+12.3%** | **+26.3%** |
| 4 | +3.4% | +13.5% |

The direction and size at lengths one through three agree on both hosts and match the removed
work. Longer rows, where the witness was already nearer its executed span, fluctuate in both
directions by ordinary hosted-run noise; length sixteen changes by -3.9%/-3.1% even though its
64-byte witness is unchanged, which is an explicit control against over-reading the table.

For continuity with r512 only, applying the new per-length graph/interpreter curve to r512's old
call-length histogram changes the deliberately naive whole-workload calculation from
1.442x/1.525x to **1.530x/1.642x** on macOS 14/15. Scaling r485's 15.732668 Minsn/s gives
24.06--25.83 Minsn/s; scaling the old 11.143 changed-scanout/s mean gives **17.0--18.3
changes/s**. This weighting is not a forecast: graph chaining changes call shapes, synthetic loops
omit firmware/MMU/framebuffer/UIKit work, and the physical host is different. Its legitimate use
is negative: even the improved shortcut remains far below the 34.304688 Minsn/s capacity target
and 30 changes/s.

Brutal status: **this is a measured, exact short-block improvement and still not evidence that the
phone exceeds 0--4 FPS, much less reaches 30 FPS**. It stays because both Apple hosts show the
intended gain where the removed comparisons existed, all exactness gates remain green, and the
actual iOS target packages it. The next highest-value measurement is the exact IPA on a physical
iPhone. Without that, another result must either model the restored graph's real joint shapes or
remove another structural boundary; isolated opcode additions would return to whack-a-mole.

### 2026-08-04: exact graph-head reuse rejects invocation tokens

An invocation token initially looked like a safe way to skip repeated native raw-witness checks.
Once a node has been validated inside one bounded signed invocation, the admitted subset cannot
store to guest memory and therefore cannot change that node's instruction bytes before returning.
That observation is exact, but it does not establish that repeated nodes are common enough to pay
for token state.

Observer commit `979c4a35b1576cdf5c8728f8d7bf95538a4ecf92` therefore adds no product
dispatch. It extends the existing joint call-shape model with the unique `(PC, ARM/Thumb)` graph
heads visited by each call and closes the new totals against the already exact call/block counts.
The unchanged 7.100--7.110 B restored replay reports:

```
exact graph validation calls/heads/unique=1832685/2674071/2582592 current-native/token-native/reusable=841386/749907/91479  EXACT
```

Of 1,832,685 modeled calls, 1,370,748 (**74.795%**) still visit only one node and offer no
next-head work to remove. The current callback-free graph performs 841,386 native next-head
validations. A perfect per-invocation token would still have to validate 749,907 first visits and
could skip only 91,479 revisits: **10.872% of next-head validations**, or 0.915 skipped checks per
100 fetched guest instructions. Most of the reuse is concentrated in 5,474 eight-block calls with
one unique node and 6,004 nine-block calls with two; it is not a broad property of the workload.

`work/r516-graph-reuse-10m` stopped at exactly 7.110 B instructions with status `OK`, empty
stderr and 1,590 CLCD frames. Its work image and PPM remain byte-identical to the prior replay at
SHA-256 `8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`. The complete local
suite passes 60/60 and the independent strict build passes. Exact-SHA core runs `30898201583` and
`30898213574` are green in all eight jobs; exact-SHA iOS run `30898215966` is green as well.

Brutal status: **an invocation token is rejected as the next production optimization**. It could
make synthetic self-loops faster, but the exact restored workload says it can remove only a small
minority of already narrowed graph-head checks while adding epoch state, wrap handling and another
native conditional. This observer proves no FPS gain. The larger structural cost remains handler
dispatch inside every covered signed instruction, not graph-node revisits.

### 2026-08-04: pre-resolved signed dispatch is exact, performance-neutral and reverted

Commits `68611e7ce4c28814e89d09252cbd9b750be097e6` and
`f4f2f06df659e78947e96b763904fdbf0263bae3` tested a narrower handler-dispatch shape. The
decoder replaced each handler ID with that handler's signed-text-relative offset while retaining
the existing sixteen-byte record. Native dispatch therefore removed one dependent table load per
reached record: `ldrsw/add/br` instead of `ldr/ldrsw/add/br`. It generated no runtime code, stored
no process pointer and added no per-record or per-block sidecar.

The first exact Apple run exposed a real fail-closed regression before performance was accepted.
Fast cache-owned validation trusted the decoded block's `dynamic_exit` flag, so the existing oracle
could mutate that flag and incorrectly pass validation. The fix retained the terminal handler ID in
the block and derived the dynamic-branch contract independently on the hot validation path. This
failure is part of the result, not erased by the later green run. Corrected exact-SHA core run
`30899399339` passed all eight jobs, including the mutation oracle, warnings-as-errors and
ASan/UBSan; iOS run `30899399431` built and packaged the actual arm64 app.

Correctness was green, but the three-repetition 20 M-instruction Apple curves did not establish a
useful speedup. The table compares each of the sixteen normalized ratios with the preceding exact
`fa2f68a` run, then takes the geometric mean of those per-length changes:

| runner | signed-path change | lengths better/worse | graph-path change | lengths better/worse | graph range |
|---|---:|---:|---:|---:|---:|
| macOS 14 arm64 | **-1.32%** | 5 / 11 | +0.50% | 10 / 6 | -8.41% to +17.84% |
| macOS 15 arm64 | **-0.65%** | 8 / 8 | +2.17% | 9 / 7 | -19.92% to +23.91% |

The signed and graph engines execute the same changed handler dispatch, yet their directions differ
and individual rows swing by double digits. That is mixed hosted-run noise, not a broad structural
gain. A sub-three-percent geometric change cannot justify keeping extra offset finalization,
reverse validation and terminal-contract state when the project still needs roughly a twofold core
improvement.

Commit `973159db66c0a4b5ff04dd6cf8c0f4004ba5cb07` reverts the experiment without rewriting
history. After the revert the complete local suite passes 60/60 and the independent strict build is
clean. Exact-SHA core run `30900231098` is green in all eight jobs and iOS run `30900231131` is
green. The retained graph engine is therefore back to the previously proved handler-ID contract.

Brutal status: **pre-resolved dispatch is rejected, and it produced no phone FPS result**. The only
physical-phone observation remains the older 0--4 FPS report. The next architectural gate must
measure how many handler dispatches the restored workload actually performs and how many a
universal, build-time-signed fusion could remove before another product implementation is attempted.

### 2026-08-04: immediate-read fusion wins in isolation and loses at the SoC boundary

Observer commit `a2db8938ab5134b99c4e7e6cad33d6190e8bb133` first measured the proposal
against the unchanged 7.100--7.110 B restored replay instead of guessing from an opcode list. The
model closed exactly over 6,956,087 retirement-eligible signed guest instructions and 11,600,622
handler records. Even a perfect adjacent-record fusion could remove only 3,163,152 records
(27.267%). The practical immediate scalar and VFP families covered 1,407,449 removable records
(12.132% of the current record stream); this was a real but bounded target, not a path to 30 FPS by
arithmetic alone.

Commit `2314c0c5a7ea4fa78366855bcb1525a8bd540198` implemented the deliberately broad
same-binary gate with 2,464 additional build-time-signed handlers. Its exact 20 M-instruction
Apple-host direct-wrapper curve improved by 1.452x on macOS 14 and 1.580x on macOS 15, but the
actual iOS executable grew from 4,403,312 to 5,130,128 bytes: +726,816 bytes or 16.506%. Core run
`30902899037` and iOS run `30902899021` were green. Correct and fast in a maximally favourable
wrapper is not enough to justify that product cost.

Commit `a14502d639435be701534677d89b042a149f8d18` reduced the family to 548 handlers while
retaining every immediate read the product decoder could emit: dedicated pinned low-register/SP/PC
cases and packed shared high-register cases for scalar and VFP loads. The exact oracles included
those shared cases. The iOS executable delta fell to 163,568 bytes (+3.715%), while the direct
20 M-instruction curve still measured 1.246x on macOS 14 and 1.454x on macOS 15. Core run
`30904186455` and iOS run `30904186355` were green. That justified one stricter experiment, not
shipping it.

Commit `f38a75960c87f8e5d199ccbf7acb55eb991dc8bb` then measured baseline and compact fusion
through `s5l8900_run()` in the same executable. Setup and cache warming remained outside timing;
the measured path included the app-facing run API, signed cache lookup, complete executable-byte
witness, dynamic entry gates, timebase-boundary splitting and device ticks. Three separately
initialized machines ran in rotated order, and the interpreter, baseline graph and fused graph had
to serialize byte-identically on every repetition. Exact-SHA core run `30905164727` passed all
eight jobs and iOS run `30905164538` built and packaged the app.

The end-to-end result rejected the optimization:

| Apple host | direct decoded wrapper | complete SoC path |
|---|---:|---:|
| macOS 14 arm64 | 1.553x | **1.003x** |
| macOS 15 arm64 | 1.439x | **0.937x** |

The direct row proves that halving the read-loop records really removed dispatch work. The SoC row
proves that the saved work was masked by the remaining outer-entry, validation and device-time
costs; on macOS 15 the larger handler family was measurably worse. The test used fifteen warm
plain-RAM loads in every sixteen instructions (93.75%), which is substantially friendlier than the
firmware. A neutral-to-negative result there cannot honestly be promoted into a guest-speed claim.

The rejection accompanying this record removes the 548 handlers, the fused decoder and its
benchmark-only SoC switch, restoring the 24,646-handler product. The shipping app default was never
turned on. **There is no new phone FPS measurement**: no physical iPhone was connected, and the
only device observation remains roughly 0--4 FPS. The useful conclusion is structural: further
record-level fusion is not the next lever. The next gate must amortize or remove a larger boundary,
such as the current total signed-invocation bound, while preserving the exact first timebase edge;
otherwise it is another dispatch micro-optimization hidden under the same dominant costs.

### 2026-08-04: timebase-bounded graph invocations win broadly and enter the app

The next audit found a structural mismatch rather than another missing opcode. A decoded signed
head could contain at most sixteen instructions, which is a useful cache and witness bound. The
*entire native invocation* was also capped at sixteen, however. That second limit prevented graph
chaining exactly when a full first head retired: the length-sixteen benchmark reported zero graph
transitions even though, with the current invented 412 MHz : 6 MHz instruction-to-tick ratio, the
first timebase edge can be about 69 instructions away. The graph was paying its outer entry,
validation and device-time costs again before it could amortize them.

Commit `3b301f9f3efc0a8afab8ef4f67649adffdc256be` separates those contracts. Each decoded
head remains at most sixteen instructions and retains the same raw-byte witness, fetch-block,
interrupt, MMU and dynamic-exit gates. The generic engine still defaults to a sixteen-instruction
total, while a same-binary experimental setting permits up to 256. `s5l8900_run()` remains the
ultimate bound: it supplies only the instructions before the first exact modeled timebase edge, so
the larger ceiling does not skip a device tick. This is still ordinary build-time-signed code: no
JIT, executable allocation or writable-executable page is involved.

The Apple-only oracle starts two otherwise identical machines at the same zero timebase phase and
runs a 64-instruction self-branch interval before that first edge. The legacy setting retires only
sixteen instructions; the extended setting retires all 64 with exactly 63 graph transitions. Both
machines then serialize byte-identically to the interpreter reference. The focused machine test
passes 5,832 assertions, both complete local suites pass 60/60, and exact-SHA core run
`30906989815` is green in all eight jobs. Exact-SHA iOS run `30906989506` builds and packages the
app.

Commit `de173905da3f10a209b243b15146c6186acb1e16` then makes the exact-gated iOS product
explicitly select the extended ceiling while leaving the generic engine default unchanged. Product
initialization fails closed if signed execution, graph execution or the extended bound cannot be
enabled. Exact-SHA core run `30907692589` is green in all eight jobs and exact-SHA iOS run
`30907692726` is green and packages the real app. Repeating the 20 M-instruction, three-repetition,
same-binary comparison at that product SHA gives the following extended/current-graph ratios
through the complete `s5l8900_run()` path:

| synthetic block length | macOS 14 arm64 | macOS 15 arm64 |
|---:|---:|---:|
| 1 | 1.353x | 1.410x |
| 2 | 1.389x | 1.418x |
| 3 | 1.468x | 1.460x |
| 4 | 1.486x | 1.377x |
| 5 | 1.501x | 1.526x |
| 6 | 1.600x | 1.417x |
| 7 | 1.494x | 1.427x |
| 8 | 1.441x | 1.372x |
| 9 | 1.719x | 1.653x |
| 10 | 1.551x | 1.422x |
| 11 | 1.570x | 1.584x |
| 12 | 1.479x | 1.483x |
| 13 | 1.501x | 1.432x |
| 14 | 1.442x | 1.441x |
| 15 | 1.410x | 1.369x |
| 16 | 1.360x | 1.291x |
| **geometric mean** | **1.483x** | **1.440x** |

All sixteen lengths improve on both hosts; the full observed range is 1.291x--1.719x. Unlike the
discarded read-fusion result, this gain survives the app-facing SoC boundary and is broad rather
than tied to one favourable handler sequence. It is the strongest structural no-JIT core result in
this phase and is therefore enabled in the app.

Brutal status: **this is still not a phone FPS result and it does not prove 30 FPS**. The synthetic
machines use warm plain RAM with the MMU disabled and omit restored-firmware instruction mix,
device traffic, framebuffer publication and UIKit. The first timebase edge is exact relative to the
emulator's current clock model, but that 412:6 ratio is invented rather than measured silicon
timing. The exact IPA has not yet been installed on a physical iPhone, and the only device
observation remains roughly 0--4 FPS. The next decisive measurement is that exact IPA; absent a
device, the restored replay must first measure how often real graph calls can use the newly removed
boundary before another product optimization is justified.

### 2026-08-04: restored firmware bounds the extended-graph win

Commit `2cb6cb8d4e56a979d6598b4b60b995f6e2a18480` answers that gate without
pretending that an x86-64 Windows host can time AArch64 signed handlers. The existing exact
`--sequence-profile` replay now keeps the old sixteen-total-instruction accounting side by side
with a second model. The second model leaves each decoded head capped at sixteen instructions but
continues across eligible sequential and branch heads until the nearer of the caller limit or the
first exact modeled timebase edge, with a hard product ceiling of 256. It changes no CPU, memory,
device or framebuffer state and assumes a graph node is warm whenever the corresponding supported
head is available. It is therefore an exact call-shape model and an optimistic availability bound,
not a wall-time benchmark.

The independently relinked `work/r521-extended-boundary-10m` replay restored the trusted r445
7.100 B MBX checkpoint and stopped at exactly 7.110 B with status `OK`, empty stderr and zero
external-media failures. Both models account for the same 6,956,087 signed-modeled guest
instructions:

| restored accounting | current total cap 16 | timebase-bounded total cap 256 | change |
|---|---:|---:|---:|
| outer signed calls | 1,832,685 | 1,767,198 | **-65,487 (-3.573%)** |
| decoded heads entered | 2,674,071 | 2,628,811 | **-45,260 (-1.693%)** |
| graph chains | 841,386 | 861,613 | +20,227 |
| mean instructions per outer call | 3.796 | 3.936 | +3.69% |
| longest outer call | 16 | 69 | +53 |

The longer model contains 46,229 calls above sixteen instructions and those calls carry
1,283,272 instructions, or 18.448% of the signed-modeled population. That is reach, not speed:
most instructions in those calls already ran in the old graph through additional outer entries.
The only boundary work the extension can remove in this interval is the 65,487 outer entries and
45,260 now-unnecessary short heads above. The model's exact eligibility closure is
7,369,418 = 6,956,087 modeled retirements + 413,331 fetch-block refusals. Its extended stop totals
also close: 101,937 timer edges, 73 caller bounds, 4,345 control-flow exits, 203,195 fetch-block
exits and 1,457,648 ineligible heads; there are zero cap, branch, observer or timebase-gate errors.

The replay preserves the prior 466,825,216-byte work-image SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and the prior
460,815-byte screen PPM SHA-256
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`. The complete
local suite passes 60/60, and an independent `-Wall -Wextra -Werror` bootkernel build is clean.
Exact-SHA core run `30910744224` is green in all eight jobs, including both Apple-arm64 native
runs, warnings-as-errors and ASan/UBSan. Exact-SHA iOS run `30910758465` is green and packages
the app. Neither workflow result is a physical-device measurement.

Brutal status: **the new boundary reaches the real workload, but its incremental firmware
opportunity is modest and it does not put the project close to 30 FPS**. The broad 1.440x--1.483x
synthetic result remains valid for those deliberately repetitive machines; projecting it wholesale
onto SpringBoard would now contradict the exact restored call shape. The only phone observation
remains roughly 0--4 FPS. A fresh instruction-zero MBX boot is still required for final graphics
correctness, while speed requires an exact-build physical-iPhone run or a larger architectural
lever than another graph-boundary or record-dispatch micro-optimization.

### 2026-08-05: signed plain-RAM stores remove a real interpreter boundary

The extended graph still had one categorical break: every store returned to `arm_step()`. Commit
`d98b03d07c6304d5b172f549d69a1bcc24852087` first built the permission boundary rather than
putting stores directly into signed text. A separate `host_ram_write` callback is the frontend's
consent to bypass RAM-write observers; ordinary readable host RAM is not consent. The canonical
iOS machine bus opts in. `bootkernel` explicitly opts out before installing its live framebuffer
observer. Any interposed bus fails closed, and changing consent clears every derived pointer.

The interpreter then owns a 1 KiB virtual write-block cache with the same privilege, MMU-generation
and range witness as the read cache. A miss performs the complete architectural path first--MMU
write permission, alignment, fault and bus callback--and only then fills a direct pointer for later
plain-RAM hits. The cache is cleared on reset, restore, MMU invalidation and consent changes and is
never serialized. The implementation therefore does not infer that MMIO is RAM, does not bypass a
frontend observer, and does not create executable memory. Exact-SHA core run `30920076009` and iOS
run `30920076338` are green.

Observer commit `eb8b789009db2ba5ffa26719a875d834cfbc9ad7` tested the proposed store
families against the unchanged 7.100--7.110 B restored interval before adding signed handlers. Its
independent literal oracle reconstructed every expected physical write and compared it with the
real interpreter callback stream: 1,032,111/1,032,111 candidate matches, zero mismatches, zero
non-literal steps, zero abandoned validations, and 1,736,595/1,736,595 expected/observed write
events. The modeled DWRITE cache made 988,657 block lookups, with 955,195 hits (96.615%). Exact-SHA
core run `30917374174` and iOS run `30917408775` are green. This broad model was a ceiling, not a
claim that all five families had shipped:

| store family | candidates | condition skips | direct hits | miss/fill | fault or non-RAM |
|---|---:|---:|---:|---:|---:|
| A32 single | 551,790 | 38,713 | 485,501 | 13,235 | 14,341 |
| A32 block | 185,083 | 3,320 | 181,040 | 723 | 0 |
| A32 VFP | 172,682 | 3,126 | 169,503 | 27 | 0 |
| Thumb single | 73,506 | 0 | 68,741 | 4,765 | 0 |
| Thumb multi | 49,050 | 0 | 48,679 | 371 | 0 |

Commit `4084b570686c019fc15a7a66b27deace033da85a` implements only the two
single-register rows: A32 `STR`/`STRB` across addressing mode 2 and Thumb register, immediate and
SP-relative `STR`/`STRB`/`STRH`. A separate memory decoder preserves the old store-rejecting read
API. A store is always the last semantic instruction in a decoded head. A failed condition can
retire without memory; a live aligned cache hit can write and retire; a miss, fault-sensitive
alignment, MMIO target or unavailable consent returns before the store so literal `arm_step()` owns
the complete operation. A32 writeback occurs only after a successful direct write, translation
forms preserve forced-user privilege, and a word store from PC uses the interpreter's PC+12 value.
The next graph head still verifies raw instruction bytes, so a store into code cannot run a stale
successor.

The generated build-time-signed family grows from 24,646 to 26,198 handlers (+1,552, or 6.297%).
It remains ordinary signed executable text: no JIT, runtime code generation, executable allocation
or writable-executable page. The exact Apple oracle covers fifteen store cases, conditional and
partial prefixes, PC source, writeback, unprivileged transfers and Thumb forms. A second SoC oracle
proves one direct-write hit, a terminal store, byte-identical state and a self-modifying-code witness
with zero stale graph edges. Exact-SHA core run `30923998672` is green in all eight jobs; iOS run
`30924000123` builds and packages the arm64 app.

Commit `2522e0723faa6b2f78a7a31fdfbb91f46f66a3a5` then measures the boundary in one
binary instead of comparing unrelated hosted runs. Three separately initialized machines execute
the same sixteen-instruction A32 loop through `s5l8900_run()`: interpreter reference, extended graph
with direct-write consent off, and the same graph with consent on. The loop deliberately contains
four stores and two loads, uses warm flat RAM with the MMU off, and rotates execution order over
three repetitions. Setup and warming stay outside timing. Cache lookup, raw-byte witness, entry
gates, timebase splitting and device ticks stay inside. Every arm must serialize to the same complete
machine snapshot.

| Apple arm64 runner | reference median | graph/store off | graph/store on | on/reference | on/off |
|---|---:|---:|---:|---:|---:|
| macOS 14 | 35.579 Minsn/s | 21.332 Minsn/s | 121.321 Minsn/s | **3.410x** | **5.687x** |
| macOS 15 | 30.030 Minsn/s | 21.695 Minsn/s | 103.643 Minsn/s | **3.451x** | **4.777x** |

Both long runs retire 20,000,000/20,000,000 instructions in signed text with consent on, record
5,000,000 direct-write hits and zero timed misses, and remain byte-identical to the interpreter.
With consent off, the graph retires 15,000,000/20,000,000 instructions in signed text and returns
for each store. Exact-SHA core run `30925475192` is green in all eight jobs. Unlike the earlier ALU
layout noise, the direction is large and consistent on both hosts and survives the complete SoC
entry path. It justifies retaining this architectural boundary removal.

The synthetic ratio is not the firmware ratio. Commit
`7cbc280325cf69604244ef7b1dc2491c3e45f1a7` makes the restored observer report the shipped
single-store subset separately from the broad ceiling. `work/r528-implemented-store-10m` restores
the trusted 7.100 B checkpoint, stops at exactly 7.110 B with status `OK`, empty stderr and 1,590
frames, and preserves both reference hashes: work image
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and screen
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.
Both local suites pass 60/60, the strict build is clean, and exact-SHA core run `30927010290` is
green in all eight jobs. That observer-only commit does not change the iOS target, so the packaged
app evidence remains implementation run `30924000123` rather than a redundant new IPA.

The implemented rows contain 625,296 candidates and 592,955 retirement-eligible stores, or 5.930%
of the fetched interval. The exact continuity model changes 4,810,600 current runner entries to
3,892,377, removing **918,223 (19.087%)**. Both the implemented and broad histograms close exactly.
All three remaining families together can lower the modeled entry count only another 359,312, or
7.469% of the current count; the old 26.557% figure remains that broader ceiling, not current app
coverage.

Brutal status: **this is substantial no-JIT core evidence, but it is still not a physical-phone FPS
result and it does not prove that 30 FPS is close**. The 3.410x--3.451x synthetic speedup comes from
a loop with stores in 25% of its instructions; stores are retirement-eligible in only 5.930% of the
measured firmware interval. The 19.087% figure counts modeled runner entries removed, not elapsed
time, frames or scanout publications, and cannot be multiplied into the older 0--4 FPS phone report.
The exact IPA containing these handlers has not been installed on a physical iPhone. That install
and an execution/publication breakdown are now more informative than reflexively implementing the
remaining store families, whose combined entry-removal ceiling is only another 7.469% before code
size and end-to-end timing are considered.

### 2026-08-05: signed register-indirect exits remove a second real boundary

The store model still stopped at every register-indirect control transfer. Observer commit
`e0ce013` therefore extended only the read-only restored sequence census before adding native
code. `work/r529-indirect-continuity-final-10m` restored the trusted 7.100 B checkpoint and stopped
at exactly 7.110 B with status `OK`, empty stderr and 1,590 CLCD frames. The work-image and screen
hashes remained the established
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.

The observer read every BX/BLX target from the literal pre-step register file and required the
next fetched PC and ARM/Thumb state to match before extending a chain. All 233,187 candidates were
retirement-eligible, with 65,538 link operations, 60,278 state switches, zero invalid register
forms and zero invalid targets. The exact family census was 156,764 A32 BX, 27,921 A32 BLX,
10,147 Thumb BX and 38,355 Thumb BLX. On top of the shipped single-store model, the added control
flow changes the modeled runner entries from 3,892,377 to 3,717,526: another 174,851 entries, or
**4.492%**. Total modeled removal relative to the earlier read-only decoder is 1,093,074 of
4,810,600 entries, or **22.722%**. This was coverage and continuity, not elapsed time or FPS.

Commit `34e780b6d65f65fb1f5b12997f58652ff3097b0d` implements that exact family as 62
build-time-signed terminal handlers: A32 BX 16, A32 BLX 15, Thumb BX 16 and Thumb BLX 15. The
generated family grows from 26,198 to 26,260 handlers (+0.237%). Live targets are guarded before
LR, CPSR.T or persistent graph context changes; A32 conditions, architectural PC source values,
link values, ARM/Thumb switching and unrepresentable halfword targets retain interpreter
semantics. A refusal returns before architectural mutation so literal `arm_step()` owns it.

The exhaustive native shape/oracle covers all 62 unique handlers and 240 execution cases across
all source registers and all A32 condition outcomes. The mixed-state SoC oracle traverses all four
families and both state directions through distinct graph slots. On both Apple runners it retired
19,999 instructions with 8,540 graph chains and serialized exactly like the interpreter; the
focused SoC suite passed 5,988 assertions. Exact-SHA core run `30961542308` is green in all eight
jobs and iOS run `30961542292` builds and packages the app.

Commit `9fc5a02f9424a76f676ef87f4041884722a1dca9` then measures the isolated boundary in
the same executable. Four two-instruction heads cross A32/Thumb state through BX and BLX. The off
arm retains the older signed engine, extended graph and every store handler but returns to the
interpreter for each indirect branch; the on arm changes only admission of the 62 new handlers.
Interpreter, off and on run through `s5l8900_run()` on separately initialized machines in rotated
order. Setup and warming remain outside timing; cache lookup, raw-byte witness, entry gates,
timebase splitting and device ticks remain inside. Every repetition must serialize to the same
complete machine snapshot.

| Apple arm64 runner | interpreter | indirect off | indirect on | on/interpreter | on/off |
|---|---:|---:|---:|---:|---:|
| macOS 14 | 58.241 Minsn/s | 19.536 Minsn/s | 173.515 Minsn/s | **2.979x** | **8.882x** |
| macOS 15 | 52.475 Minsn/s | 21.518 Minsn/s | 160.690 Minsn/s | **3.062x** | **7.468x** |

Each long off run retires exactly 10,000,000 of 20,000,000 instructions in signed text and has
zero graph chains; each on run retires all 20,000,000 and records 9,708,737 graph chains. All
snapshots are exact. Exact-SHA core run `30962409297` is green in all eight jobs, including both
Apple-native gates, warnings-as-errors and ASan/UBSan. Exact-SHA iOS run `30962409295` is green and
packages the real app.

Brutal status: **the boundary removal is correct and very large when half of a synthetic loop is
BX/BLX, but that is not the firmware mix and it does not put the project close to 30 FPS**. The
restored interval says indirect branches are only 2.332% of fetched instructions and can remove at
most another 4.492% of runner entries beyond stores. Neither 7.468x nor 8.882x may be multiplied
into the old roughly 0--4 FPS phone report. This exact IPA has still not been installed on a
physical iPhone, so there is no new phone FPS result. The next no-JIT tranche must be selected from
measured remaining families and code-size cost; a cold boot is still required for final graphics
acceptance, but it would not make this isolated speed comparison more accurate than the native
oracle and restored checkpoint used here.

### 2026-08-05: single-register VFP stores preserve the graph boundary

The next store was selected by measurement rather than opcode order. Observer commit
`a4714c50f138ad69af6487ced913e8ecb2c1f2d6` split the unchanged 7.100--7.110 B
interval into single-register VSTR and multi-register VSTM/FSTMX. VSTR S/D supplied 148,219
retirement-eligible encounters and predicted 175,502 fewer runner entries; VSTM/FSTMX supplied
24,410 eligible encounters and predicted 38,270. VSTR therefore covered 82.098% of the measured
A32 VFP-store continuity benefit with much narrower semantics. This selected the implementation;
it did not predict FPS.

Commit `ebe975c347b5c6b7b13e9983cda903b1566b19c2` implements terminal VSTR S and D
through the existing consent-gated DWRITE contract. The signed handler checks the live VFP enable
state, condition, alignment, privilege, translation generation and one/two-word plain-RAM witness
before mutation. A failed condition skips the complete memory/VFP semantic group; a refusal returns
before the store so literal `arm_step()` remains authoritative. Single precision preserves the
selected half of the D register and double precision writes both words. PC-relative bases retain
the interpreter's PC+8 address. No runtime code generation or executable allocation is added.

The Apple oracle covers S/D values, PC-relative addressing, every condition outcome, VFP-disabled
refusal, alignment and DWRITE miss rollback. Exact-SHA core run `30964845382` is green in all
eight jobs and iOS run `30964845373` packages the app. Product-continuity commits
`3cc754a060602ceb01ded0e9aaf0f5d369983852` and
`641220d5bedc8384ca4926bff555d13d8709a42f` then move VSTR into the shipped observer
baseline and save the exact project-local 7.110 B checkpoint used by later fast restores.

Benchmark commit `48b7bebbd4e1f42c141a111c80455415846aed81`, with reference-accounting
correction `2953d0749fe3344aa5a6b2174773639b55b03d03`, isolates VSTR in one executable.
The sixteen-instruction loop contains four VSTR operations and six written words. Reference, VSTR
disabled and VSTR enabled run through `s5l8900_run()` on separately initialized machines in rotated
order. Warm-up stays outside timing; cache lookup, raw witness, graph entry gates, timebase edges and
device ticks stay inside; complete snapshots must match before output.

| Apple arm64 runner | interpreter | VSTR off | VSTR on | on/interpreter | on/off |
|---|---:|---:|---:|---:|---:|
| macOS 14 | 39.959 Minsn/s | 31.185 Minsn/s | 146.958 Minsn/s | **3.678x** | **4.712x** |
| macOS 15 | 45.423 Minsn/s | 42.196 Minsn/s | 168.899 Minsn/s | **3.718x** | **4.003x** |

Each long disabled run retires 15,000,000 of 20,000,000 instructions in signed text; each enabled
run retires all 20,000,000, records 7,500,000 direct-write word hits and zero timed misses, and
serializes byte-identically to the interpreter. Exact-SHA core run `30966856232` is green in all
eight jobs and iOS run `30966877869` is green.

The restored authority is smaller. `work/r533-vstr-current-checkpoint-7110m` reproduces the
predicted baseline exactly: 3,717,526 runner entries become 3,542,024, a reduction of 175,502
(4.721%). The process exits 0 with empty stderr, 1,590 live CLCD frames, zero media failures, and
the established work-image/screen hashes. A separate restore smoke starts the new checkpoint at
exactly 7,110,000,000 and advances it without state drift.

Brutal status: **VSTR is a real, exact no-JIT boundary removal and still not a phone-FPS result**.
The 4.003x--4.712x on/off ratio belongs to a synthetic loop where VSTR is 25% of instructions.
Only 1.482% of fetched instructions in the restored interval are retirement-eligible VSTR, and
4.721% counts removed runner entries rather than elapsed time or frames. The physical iPhone has
not run this exact IPA; its only reported result remains roughly 0--4 FPS.

### 2026-08-05: transactional ordinary STM removes the measured block-store boundary

The VSTR baseline made A32 block stores the largest remaining store frontier. Read-only observer
commit `fc5c934196f64590604b1d9d18b4dae1840b8e95` split 185,083 live candidates by
semantics and DWRITE span. Ordinary transfers whose live addresses fit one 1 KiB block account for
182,512 retirement-eligible encounters and predict 124,114 fewer runner entries. That is
124,114/125,236 = **99.104%** of the entire A32 block-store continuity opportunity. Supporting
ordinary two-block cases would add only 580 entries and user-bank forms another 542. The measured
contract therefore selected all IA/IB/DA/DB modes and optional writeback, with a transactional
one-block preflight and fail-closed fallback for the rest.

Implementation commit `f2e6323d86ad79da237f5e2e55f1036130ad4e79` adds ordinary terminal A32
STM records. The decoder rejects user-bank, load, empty-list, PC-base and writeback/base-alias
forms. Runtime preflight checks the condition, exact live address span, alignment, privilege,
translation generation and DWRITE witness before the first write. Only after every guard succeeds
do ascending register commits run; a stored PC uses PC+12 and optional base writeback occurs in the
finish record. Any cold, crossing, fault-sensitive or non-consented case returns before mutation.
This is ordinary build-time-signed AArch64, not JIT code.

`d697a87131087bacd110100aa914a230f5836328` keeps large conditional semantic groups within
AArch64 branch range. `97f808e5865dad83d9f562719cc31c29b7ed239f` corrects an oracle whose DA
fixture accidentally crossed a 1 KiB boundary; the implementation's refusal was correct and the
test assumption was not. `cbb5f3c5bc603cc6707fc93a4316abc3316beeae` adds a same-binary rollout
gate that defaults on and invalidates only derived graph/cache state when toggled. Native tests
cover IA/IB/DA/DB, lists through all sixteen sources, PC+12, writeback, every condition outcome,
full sixteen-word transfer, alignment/cold-cache refusal and cross-block rollback. Exact-SHA core
run `30969064608` and iOS run `30969064552` are green.

Benchmark commit `687dd921af01f4e8d0373029f54bd81e9bba57d3` uses four STM operations and
twelve written words in each sixteen-instruction loop. Its off arm retains identical direct-write
consent and every older signed feature but disables only STM admission. The on and off binaries are
therefore literally the same executable. All timed arms cross the complete SoC run boundary and
must serialize exactly.

| Apple arm64 runner | interpreter | STM off | STM on | on/interpreter | on/off |
|---|---:|---:|---:|---:|---:|
| macOS 14 | 37.213 Minsn/s | 26.382 Minsn/s | 151.879 Minsn/s | **4.081x** | **5.757x** |
| macOS 15 | 36.694 Minsn/s | 35.822 Minsn/s | 152.523 Minsn/s | **4.157x** | **4.258x** |

Each 20 M-instruction disabled run retires exactly 15,000,000 instructions in signed text; each
enabled run retires all 20,000,000, records 15,000,000 DWRITE word hits and zero timed misses, and
matches the interpreter snapshot byte for byte. Exact-SHA core run `30969381030` is green in all
eight jobs. The wide host-to-host off-rate difference is a warning against comparing absolute CI
rates; the conclusion uses only rotated same-run ratios and the exact state gate.

Observer commit `08010c223498a06bded2889bc4ae5f7c887ee691` advances the firmware baseline.
`work/r536-stm-current-10m` restores the same trusted 7.100 B checkpoint, reaches exactly 7.110 B
in 59.2 host seconds, exits 0, leaves stderr empty, keeps 1,590 live nonblack CLCD frames, completes
15,626 media reads and 469 writes with zero failures, and preserves the established work-image and
screen hashes. The literal write oracle remains exact at 1,032,111/1,032,111 candidates and
1,736,595/1,736,595 events.

The shipped store baseline now has 923,686 retirement-eligible instructions (9.237% fetched) and
3,417,910 runner entries. The preceding VSTR baseline was 3,542,024: the measured reduction is
**124,114 exactly**, matching the pre-implementation prediction. Re-adding shipped one-block STM
changes zero entries, and all partitions/histograms report `EXACT`. The new remaining frontiers are:

| remaining store family | eligible | entries removed from 3,417,910 | baseline reduction |
|---|---:|---:|---:|
| A32 VSTM/FSTMX | 24,410 | 45,370 | **1.327%** |
| Thumb multi-store | 48,679 | 28,733 | 0.841% |
| A32 block remainder | 1,848 | 1,122 | 0.033% |

Exact-SHA core run `30970050048` is green in all eight jobs. The accurate r533 project-local
checkpoint remains the fast 7.110 B restore point; this read-only run does not waste storage on a
duplicate snapshot.

Brutal status: **this is substantial architectural progress, but measured emulator and phone FPS
improvement is still zero**. The 4.258x--5.757x on/off ratio comes from a deliberately dense 25%
STM loop, while the restored benefit is 124,114 fewer modeled runner entries. Neither number is a
frame-rate multiplier. No physical iPhone install was possible in this environment, and the only
device observation remains roughly 0--4 FPS. The next measured store frontier is VSTM/FSTMX, but
even its exact continuity ceiling is only 1.327% of the new baseline. A real exact-build device
measurement remains the decisive authority; implementing VSTM is justified as bounded continuity
work, not as a promise that 30 FPS is close.

### 2026-08-05: transactional one-block VSTM closes the measured VFP-store boundary

Read-only observer commit `6227c2a91fdc86b302fce33ad39df9ad76504f4e` split the VFP
multi-store frontier before any implementation. The unchanged 7.100--7.110 B instruction stream
contained 24,436 architectural VSTM/VPUSH candidates and 24,410 retirement-eligible encounters.
Exactly 24,275 eligible encounters fit the existing one-block DWRITE contract; the remaining 135
cross a 1 KiB boundary. The model predicted that the one-block family would remove 45,370 runner
entries, while admitting all 135 cross-block cases would remove **zero additional entries**. That
measured result selected a transactional one-block implementation and rules out cross-block VSTM
as useful continuity work on this authority.

Implementation commit `56db1b3d770869f3383bdb794344750f07ce5d27` adds 45 terminal
build-time-signed handlers: IA without writeback, IA with writeback, and DB with writeback/VPUSH,
across all non-PC base registers. Single-precision lists may contain 1--32 words, including odd
counts; double-precision lists use even encoded word counts and D0--D15. Deprecated FSTMX, empty
lists, invalid register ranges and PC bases remain in the interpreter. Runtime admission checks the
A32 condition, live VFP enable state, complete aligned address span, privilege, translation
generation, consent and one 1 KiB DWRITE witness before the first write. Only then are all words
committed, followed by optional base writeback. Any refusal happens before architectural mutation.
No executable page is created or changed at runtime.

The native oracle covers IA/DB, writeback, odd S lists, double registers, every A32 condition,
VFP-disabled refusal, alignment, cold-cache fallback, consent denial and cross-block rollback. It
reports six exact write cases and 70 exact word hits. An initial Apple CI failure exposed a validator
ordering bug, not a VSTM semantic or RAM-corruption bug: the generic semantic-span scanner mistook a
terminal one-record VSTM for half of a direct-write pair. Commit
`832a2e8f959e02a8f841b8ce6cbe016a9501ca99` recognizes the complete VFP record first.
Exact-SHA core run `30973237408` is green in all eight jobs, and exact-SHA iOS run
`30973237428` builds and packages the real app.

Benchmarking uses the same executable for reference, VSTM disabled and VSTM enabled. Each
sixteen-instruction loop contains four VSTM operations and writes twenty words. Setup and warm-up
remain outside timing; graph lookup, byte witness, entry gates, timebase splitting and device ticks
remain inside. Every arm must serialize to the same complete machine snapshot.

| Apple arm64 runner | interpreter | VSTM off | VSTM on | on/interpreter | on/off |
|---|---:|---:|---:|---:|---:|
| macOS 14 | 32.627 Minsn/s | 27.013 Minsn/s | 142.253 Minsn/s | **4.360x** | **5.266x** |
| macOS 15 | 29.195 Minsn/s | 31.124 Minsn/s | 133.300 Minsn/s | **4.566x** | **4.283x** |

Each 20 M-instruction disabled run retires exactly 15,000,000 instructions in signed text; each
enabled run retires all 20,000,000, records 20,000,000 DWRITE word hits and zero timed misses, and
matches the interpreter snapshot byte for byte. The different absolute host rates are not compared
across CI runners; only rotated same-run ratios and the exact-state gate support the result.

Baseline commit `e667fa697ce07afd77eb8e49934217d116db9207` advances the exact
firmware authority without creating a duplicate snapshot. `work/r542-vstm-baseline-10m` restores
the trusted project-local 7.100 B checkpoint, reaches exactly 7.110 B, exits 0 in 55.677 host
seconds, leaves stderr empty, produces 1,590 live nonblack CLCD frames, and completes 15,626 media
reads plus 469 writes with zero failures. The literal-write oracle remains exact at
1,032,111/1,032,111 candidates and 1,736,595/1,736,595 events. Work-image, screen and output hashes
remain stable. Exact-SHA core run `30973710318` is green in all eight jobs.

The shipped store baseline now contains 947,961 retirement-eligible instructions (9.480% fetched)
and 3,372,540 runner entries. The preceding STM baseline was 3,417,910: the measured reduction is
**45,370 exactly**, matching the pre-implementation prediction. Relative to the current read-only
decoder, the implemented store graph removes 1,283,112 entries, or 27.560%. Re-adding shipped
one-block VSTM changes zero entries, and all partitions and histograms report `EXACT`.

| remaining store family | eligible | entries removed from 3,372,540 | baseline reduction |
|---|---:|---:|---:|
| Thumb multi-store | 48,679 | 28,733 | **0.852%** |
| A32 block remainder | 1,848 | 1,122 | 0.033% |
| A32 VSTM cross-block/FSTMX | 135 | 0 | **0.000%** |

The accurate r533 project-local checkpoint remains the fast restore point; this run deliberately
does not consume storage for a byte-identical duplicate checkpoint.

Brutal status: **the implementation is exact and the architectural progress is real, but measured
phone-FPS improvement remains zero**. The 4.283x--5.266x on/off result belongs to a synthetic loop
where VSTM writes twenty words per sixteen instructions. Real restored benefit is 45,370 fewer
modeled runner entries, not elapsed time or frames. The exact IPA has not been installed on a
physical iPhone in this environment, and the only device observation remains roughly 0--4 FPS.
The next remaining store family can remove only 0.852% of this baseline, so another store opcode is
not automatically the next best long-term FPS investment. The next tranche must be chosen from a
broader measured bottleneck, and 30 FPS is not close or established.

### 2026-08-05: all Thumb conditional exits move into signed text

The next tranche was not chosen by continuing down an opcode list. Read-only observer commit
`f3522d00d31e91c1d428a4e19a075049e57f2b08` compared six remaining families side by side on the
unchanged 7.100--7.110 B literal stream. The exact bounded results were:

| post-store family | eligible | entries removed from 3,372,540 | old-baseline reduction |
|---|---:|---:|---:|
| Thumb conditional B | 147,493 | **263,208** | **7.804%** |
| A32 LDM, no PC, one DREAD block | 95,956 | 130,759 | 3.877% |
| VLDM/VPOP, one DREAD block | 25,626 | 48,757 | 1.446% |
| Thumb POP/LDM, no PC, one DREAD block | 9,142 | 14,203 | 0.421% |
| A32 multiply, perfect one-record ceiling | 38,997 | 57,262 | 1.698% |
| remaining VFP compute, perfect one-record ceiling | 378,620 | 584,961 | 17.345% |

The final two rows are deliberately labelled ceilings. They assume a perfect semantic handler and
do not prove that host integer multiply or floating point is exact for ARM1176/VFPv2. The first row
was both the largest fully bounded contract and simpler than either memory family. Its 73,132 taken
and 74,361 fallthrough observations covered both paths. That evidence selected the implementation.

Implementation commit `eb31c1fa17fa04ef0800ddd2b3c41475d97db5ba` admits every valid 16-bit
Thumb conditional branch (`0xD000`--`0xDDFF`) only when it terminates a decoded head. It reuses the
fourteen existing build-time-signed A32 condition handlers, so it adds no generated executable
handler text and no runtime code generation. The record carries the signed halfword target and the
natural two-byte fallthrough separately; live NZCV chooses between them while CPSR.T and LR remain
unchanged. Conditions `0xE`/`0xF`, mid-block branches and mis-shaped metadata refuse cleanly.

The validator independently reconstructs a dedicated Thumb-conditional-exit bit from the terminal
handler, alignment and natural fallthrough. A mutation oracle clears that bit and proves the decoded
runner returns false without changing CPU state or the completed count. The semantic matrix crosses
all fourteen conditions with taken and fallthrough outcomes, alternating forward and backward
targets: 28 interpreter-versus-signed cases, exact registers/CPSR/cycles, Thumb state preserved and
LR untouched. A separate app-facing SoC oracle must exceed the old subset's exact 75% retirement
ceiling, publish graph chains and serialize byte-identically to the interpreter. The rollout switch
invalidates only derived decode/graph state, allowing the benchmark to disable exactly this family
inside one binary.

The rotated 20 M-instruction same-binary benchmark keeps the complete SoC run API, raw-byte witness,
entry gates, graph lookup, timebase splitting and device ticks inside timing. The branch-disabled arm
still signs the other twelve instructions per loop and retires exactly 15,000,000 signed
instructions; the enabled arm retires all 20,000,000. Every arm serializes to the same complete
machine snapshot.

| Apple arm64 runner | interpreter | branch off | branch on | on/interpreter | on/off |
|---|---:|---:|---:|---:|---:|
| macOS 14 | 66.811 Minsn/s | 30.905 Minsn/s | 171.680 Minsn/s | **2.570x** | **5.555x** |
| macOS 15 | 83.672 Minsn/s | 35.809 Minsn/s | 203.849 Minsn/s | **2.436x** | **5.693x** |

Those ratios belong to a deliberately branch-dense 25% synthetic loop whose taken destination is
the natural fallthrough so every arm executes an identical fixed path. They isolate product dispatch
cost; they are not the restored firmware mix, elapsed firmware speed or phone FPS. Exact-SHA core
run `30975501851` is green in all eight jobs, including macOS 14/15 native execution. Exact-SHA iOS
run `30975501834` builds and packages the app.

`work/r545-thumb-cond-10m` is the decisive firmware replay. It restores the trusted 7.100 B
checkpoint, stops at exactly 7.110 B in 56.8 host seconds, exits 0, leaves stderr empty and reports
57 `EXACT` verdicts with no case-sensitive `MISMATCH`. The literal write oracle remains exact at
1,032,111/1,032,111 candidates and 1,736,595/1,736,595 events. CLCD remains live and nonblack at
frame 1,590; 15,626 media reads and 469 writes finish with zero failures. Work-image and screen
SHA-256 values are byte-identical to r544, so the product/profile change altered no guest output.

The measured before/after is exact:

| restored product metric | before | after | change |
|---|---:|---:|---:|
| decoder-supported fetched instructions | 7,766,172 | 7,913,665 | +147,493 |
| retirement-eligible instructions | 7,602,605 | 7,750,098 | +147,493 |
| modeled signed instructions | 7,186,816 | 7,334,306 | +147,490 |
| modeled signed calls | 1,910,113 | 1,804,655 | -105,458 |
| post-store runner entries | 3,372,540 | 3,109,332 | **-263,208** |

Three of the newly supported observations remain behind the unchanged fetch entry gate, explaining
the 147,493-versus-147,490 difference. After stores, the exact product shape moves from
`calls/instructions/heads/chains=1405523/8032472/3069366/1663843` to
`1289805/8179962/3084029/1794224`. Re-adding Thumb conditional B now changes zero candidates,
eligible instructions or entries. The pre-implementation prediction and post-implementation audit
therefore agree exactly.

The new baseline also changes the honest ranking. One-block no-PC A32 LDM is now the largest bounded
unimplemented family at 130,759 entries, or 4.205% of 3,109,332. The VFP-compute ceiling is larger at
584,961 entries (18.813%), but remains unsafe to implement until exact ARM VFPv2 exception, NaN,
rounding, denormal and cumulative-status behavior is proved; calling that a ready-made 18.813% win
would be fiction.

Brutal status: **this is the largest exact continuity reduction implemented in the recent isolated
tranches, but measured phone-FPS improvement is still zero**. Runner entries are dispatch
opportunities, not time or frames. The exact IPA has not been installed on a physical phone in this
environment; the only device observation remains roughly 0--4 FPS. No duplicate checkpoint was
created because the accurate r533 7.110 B checkpoint already represents the identical guest state.
Thirty FPS is still neither close nor established by this result.

### 2026-08-05: transactional one-block A32 LDM removes the next measured boundary

The Thumb-condition replay made ordinary A32 block loads the largest bounded remaining family.
The earlier read-only ranking predicted that no-PC LDM transfers whose complete live address span
fit one 1 KiB DREAD block would add 95,956 retirement-eligible instructions and remove 130,759
runner entries, or 4.205% of the 3,109,332-entry product baseline. This was an exact live-state
contract, unlike the larger multiply and VFP-compute rows, which still assume a perfect semantic
handler. That distinction selected LDM; it was not selected merely because it was the next opcode.

Implementation commit `da415e09627aa7455a9e4eb72b63e8ab831b1952` adds 91
build-time-signed handlers for transactional A32 LDM: four IA/IB/DA/DB preflight modes over r0--r14
bases, fifteen destination commits, and finish records with or without safe writeback. The decoder
admits r0--r14 destinations and bases, every ordinary condition, lists through fifteen words and
optional writeback when the base is not in the list. PC loads, PC bases, user-bank transfers,
empty lists and writeback/base aliases remain literal. Unlike STM, LDM is nonterminal, so later
instructions may remain in the same signed head.

The runtime contract is all-or-nothing. Before changing a destination or base register, the
preflight reconstructs the complete ascending transfer span and requires aligned addresses, one
live plain-RAM DREAD block, matching privilege and translation generation, and enough record
metadata for every destination. A cold or stale cache, misalignment, cross-block span, invalid
metadata or other mismatch returns an exact zero prefix; `arm_step()` remains the only owner of
translation, faults and MMIO. Only a successful preflight permits the ordered destination commits,
optional writeback and aggregate DREAD hit accounting. The same-binary `set_ldm` switch defaults on
and clears only derived decode/graph state when changed.

The flat native oracle covers all four modes, writeback, a legal base-in-list/no-writeback case,
condition skip, a fifteen-word list and nonterminal continuation: six interpreter-versus-signed
cases and thirty exact hit words. Separate gates require cross-block rollback, cold/misaligned/stale
cache refusal, a metadata mutation that fails closed, every generated shape, and the app-facing SoC
path. The dense benchmark uses the identical executable for interpreter, LDM disabled and LDM
enabled. Four LDM instructions load twelve words in each sixteen-instruction loop. Setup and warmup
stay outside timing; product cache lookup, raw-byte witness, graph entry gates, the 256-instruction
product ceiling, timebase boundaries and device ticks remain inside. Every repetition serializes to
the same complete machine snapshot.

| Apple arm64 runner | interpreter | LDM off | LDM on | on/interpreter | on/off |
|---|---:|---:|---:|---:|---:|
| macOS 14 | 48.169 Minsn/s | 33.757 Minsn/s | 140.995 Minsn/s | **2.927x** | **4.177x** |
| macOS 15 | 28.743 Minsn/s | 23.025 Minsn/s | 99.306 Minsn/s | **3.455x** | **4.313x** |

Each long disabled run retires 15,000,000 of 20,000,000 instructions in signed text; each enabled
run retires all 20,000,000, records 15,000,000 DREAD word hits and zero timed misses, and remains
snapshot-exact. Absolute rates differ sharply across hosted machines, so only rotated same-run
ratios and the exact-state gate support the isolated result.

The proof did expose test and observer mistakes, and they are not being erased from the account.
The first exact replay, r546, printed seventeen `MISMATCH` verdicts because the read-only observer's
per-instruction reached-record histogram stopped at four records while a maximal LDM semantic group
can reach eighteen. The guest disk and screen remained deterministic, but that run was not accepted
as exact evidence. Commit `c4a5d4743d72b212188139cf0d61817ab7e116ee` raises the
observer limit to eighteen. The native SoC oracle also remained red through three core CI attempts:
one fixture used a decrementing transfer at a DREAD boundary, and the revised sixteen-instruction
loop still used the generic sixteen-instruction invocation ceiling, leaving no budget to prove a
successor edge. The implementation correctly refused the crossing case, and the graph correctly
stopped at its configured ceiling. Commits `3ce91e5e0f2e7d961fd47bd9add94ee89b7bae91` and
`6f87756d6e5d4adf12d7b31017428f8a4a07e3fe` put all four transfers inside one
block and select the actual 256-instruction iOS product ceiling. The graph assertion was retained,
not weakened. It then reports 15,998 signed retirements, 11,999 DREAD hits, one cold miss, all four
modes, an exact snapshot and a real graph edge on both Apple hosts.

`work/r547-ldm-record-cap-10m` is the accepted restored authority. It starts from the trusted
7.100 B checkpoint, stops at exactly 7.110 B, exits 0, leaves stderr empty and reports 57
case-sensitive `EXACT` verdicts with zero case-sensitive `MISMATCH`. The final two source commits
change only the test fixture, so they do not alter the replayed `bootkernel` implementation. The
literal-write oracle remains exact at 1,032,111/1,032,111 candidates and 1,736,595/1,736,595
events. CLCD remains live and nonblack at frame 1,590; 15,626 media reads and 469 writes complete
with zero failures. Work-image and screen SHA-256 values remain byte-identical to r545:
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.

The measured product change is exact:

| restored product metric | before | after | change |
|---|---:|---:|---:|
| decoder-supported fetched instructions | 7,913,665 | 8,014,303 | +100,638 |
| retirement-eligible instructions | 7,750,098 | 7,846,054 | +95,956 |
| modeled signed instructions | 7,334,306 | 7,426,504 | +92,198 |
| product runner entries | 3,109,332 | 2,978,573 | **-130,759** |

The full product shape moves from
`calls/instructions/heads/chains=1289805/8179962/3084029/1794224` to
`1251244/8272160/3066547/1815303`. Re-adding one-block no-PC A32 LDM now changes zero entries.
The remaining LDM census contains 116,334 candidates but zero eligible encounters: 4,643 are live
DREAD misses, 39 cross a block and 111,652 use states outside this exact contract. The predicted
and measured runner-entry reductions therefore agree exactly. No duplicate 7.110 B checkpoint was
created; the existing r533 checkpoint already provides the fast, byte-identical restore state.

Final exact-SHA core run `30978271615` is green in all eight jobs, including both Apple-native
oracles, warnings-as-errors, ASan/UBSan and JIT-off builds. Exact-SHA iOS run `30978271669` builds,
fake-signs and uploads the app. The local strict suite is 60/60 green. Commits contain no co-author
trailers.

Brutal status: **this is another real structural reduction, and it still does not show that 30 FPS
is close**. The 4.177x--4.313x on/off result belongs to a deliberately dense loop where one quarter
of instructions are LDM. The restored result is 130,759 fewer modeled runner entries, not elapsed
firmware time, scanouts or physical-device frames. This exact IPA has not been installed here, and
the only phone observation remains roughly 0--4 FPS. The next bounded family, one-block VLDM/VPOP,
can remove 48,757 entries or 1.637% of the current baseline. The larger multiply and VFP-compute
rows can remove 60,146 (2.019%) and 585,011 (19.641%) only under perfect-handler assumptions. The
next tranche must weigh exact semantic cost and broad coverage rather than blindly walking that
list; none of these counts is a promise of 30 FPS.

### 2026-08-05: guarded scalar VFP arithmetic closes the large boundary, but is not yet a speed win

The post-LDM ranking did not justify another small memory opcode. Read-only commit
`00beb3a1a65c830f85cf6487a9985a139ea91a45` and its preceding observer work instead measured
the complete VFPv2 three-operand arithmetic family over the unchanged 7.100--7.110 B literal
stream. There were 289,127 VMLA/VMLS/VNMLA/VNMLS, VMUL/VNMUL, VADD/VSUB and VDIV candidates:
4,753 condition skips and 284,374 executed instructions. Every executed observation used
round-to-nearest with FZ and DN set, vector length zero, all exception enables clear, and IXC
already sticky. Every operand and result was a signed zero or finite normal; no newly visible
exception flag appeared. Under that measured contract, admitting the whole family predicted
417,915 fewer runner entries, or **14.031%** of the 2,978,573-entry LDM baseline. This was a broad
measured frontier, not an opcode-by-opcode choice.

Implementation commit `6969ebda3f8d943435acf49e3af57a926f49ee3b` adds all nine operations in
both single and double precision as eighteen ordinary build-time-signed AArch64 handlers. It does
not allocate or modify executable memory. Runtime admission checks live CPACR/FPEXC access, the
exact RN+FZ+DN/Len-zero/enables-zero/IXC-sticky guest mode and zero-or-normal operand classes before
touching guest state. Host FPCR/FPSR are saved, normalized and restored. Results must remain signed
zero or normal and may expose only no exception or IXC; the FZ smallest-normal ambiguity refuses.
VMLA-family operations use separately rounded multiply and add stages, sample the intermediate
result and flags, and never contract into host FMA. Any failed check returns the exact completed
prefix before mutating the rejected guest instruction.

The Apple-native oracle covers all eighteen operations, signed zero, inexact results, conditional
skip, partial-prefix return and 25 fail-closed cases across access, mode, sticky flags, abnormal
inputs/results, overflow, underflow, divide-by-zero and VMLA intermediate underflow. It also
perturbs host FPCR/FPSR and requires exact restoration on success and rejection. Rollout commit
`b558ce69c880e977d926ee819417a5ec7f04f3b3` then proves the feature through the actual SoC path:
the same machine retires 1,000 signed instructions with arithmetic disabled and all 16,000 with it
enabled, publishes 766 graph chains and remains snapshot-identical to the literal reference.

The long same-binary benchmark is deliberately hostile to self-deception: fifteen of every sixteen
instructions are VFP arithmetic, with the loop branch as the sixteenth. Setup and warm-up remain
outside timing; cache lookup, raw witness, entry gates, graph lookup, timer boundaries and device
ticks remain inside. All three arms serialize byte-identically.

| Apple arm64 runner | interpreter | arithmetic off | arithmetic on | on/interpreter | on/off |
|---|---:|---:|---:|---:|---:|
| macOS 14 | 34.883 Minsn/s | 15.172 Minsn/s | 28.740 Minsn/s | **0.824x** | **1.894x** |
| macOS 15 | 30.223 Minsn/s | 15.781 Minsn/s | 29.225 Minsn/s | **0.967x** | **1.852x** |

That result is useful precisely because it is not flattering. Native arithmetic removes the signed
fallback boundary and is 1.852x--1.894x faster than leaving that boundary in the signed engine, but
the guarded handler itself does **not** beat the literal interpreter in this arithmetic-dense loop.
Per-instruction host FPCR/FPSR save, normalization, sampling and restoration are now the named cost
to attack. The next performance tranche is batching or otherwise amortizing that exact FP-state
contract across a signed invocation, with the same failure atomicity and host-state oracle. Adding
another opcode before addressing this measured cost would be the wrong optimization order.

`work/r550-vfp-arith-current-10m` is the exact product-baseline replay. It restores the trusted r445
7.100 B checkpoint, stops at 7.110 B and PC `0x312092bc`, exits 0 in 58.736 host seconds and leaves
stderr empty. All 59 case-sensitive accounting verdicts are `EXACT`; none is `MISMATCH`. The 284,374
executed arithmetic observations again have only zero/normal operands and results, RN+FZ+DN, and
pre-existing IXC. Re-adding the family is now a zero-increment audit, proving the current product
decoder owns the measured candidates.

| restored product metric | before | after | change |
|---|---:|---:|---:|
| decoder-supported fetched instructions | 8,014,303 | 8,303,430 | +289,127 |
| retirement-eligible instructions | 7,846,054 | 8,135,181 | +289,127 |
| modeled signed instructions | 7,426,504 | 7,715,606 | +289,102 |
| modeled signed calls | 1,251,244 | 1,122,431 | -128,813 |
| modeled signed heads | 3,066,547 | 2,960,614 | -105,933 |
| modeled chain transitions | 1,815,303 | 1,838,183 | +22,880 |
| product runner entries | 2,978,573 | 2,560,658 | **-417,915 (-14.031%)** |

The pre-implementation prediction and shipped audit therefore agree exactly. The final work image
and screen remain byte-identical to r549 and the retained r533 checkpoint:
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`. CLCD remains live and
nonblack at 1,590 frames; 15,626 media reads and 469 writes complete with zero failures. No duplicate
snapshot was created because r533 already holds the identical 7.110 B state.

Exact-SHA core run `30982162147` is green in all eight jobs, including both Apple-native semantic
and SoC rollout oracles, warnings-as-errors, ASan/UBSan and JIT-off builds. Exact-SHA iOS run
`30982162237` builds, fake-signs and uploads the app. Commits contain no co-author trailers.

Brutal status: **this is the largest recent exact structural reduction, but it still does not show
that 30 FPS is close**. The dense A64 result says the current guarded arithmetic path is at best near
interpreter speed and sometimes slower. The restored 14.031% result counts avoided runner entries,
not elapsed firmware speed or frames. This exact IPA has not been installed on a physical phone in
this environment, so measured phone-FPS improvement remains zero and the only device observation
remains roughly 0--4 FPS. The work is substantial, but the evidence says FP-state amortization and a
new exact A/B are required before this architecture can honestly be called a path to 30 FPS.

### 2026-08-05: FP-state batching is a real arithmetic speed win, not a phone-FPS result

Commit `9bdbf76207cb76400cc812cd5be93f2d4e11e95b` replaces per-operation host
FPCR/FPSR preservation with one lazy session per generated signed invocation. The first guarded
arithmetic handler saves caller FPCR/FPSR and normalizes FPCR; subsequent arithmetic handlers reuse
that state. FPSR is still cleared and sampled for every guest operation and between the separately
rounded multiply/add stages of VMLA-family operations. Every generated exit restores the caller
environment. A callback-backed chain also restores before crossing into C, while a validated graph
chain can retain the session across decoded heads.

That implementation initially had an encouraging cross-run benchmark, but comparing it with an
older executable could not isolate batching from runner variation. Commit
`25e067c899c3c55b4a5bc243ac9a81e9854f48c9` therefore adds an execution-policy switch in the
existing four-byte context padding. Its control arm uses the former inline save/normalize/restore
sequence on every operation; its product arm uses the lazy session. Both arms execute the same
generated binary, decode the same records, traverse the same graph and rotate order with the
interpreter and arithmetic-disabled controls. The native oracle perturbs host FPCR/FPSR and proves
success plus second-operation rejection for both policies before accepting the benchmark.

The 20-million-instruction Apple-arm64 result is unambiguous for this deliberately dense loop:

| Apple arm64 runner | interpreter | arithmetic off | unbatched | batched | batched/interpreter | batched/unbatched |
|---|---:|---:|---:|---:|---:|---:|
| macOS 14 | 32.819 Minsn/s | 14.602 Minsn/s | 26.737 Minsn/s | 51.816 Minsn/s | **1.579x** | **1.938x** |
| macOS 15 | 20.706 Minsn/s | 10.838 Minsn/s | 22.042 Minsn/s | 41.230 Minsn/s | **1.991x** | **1.871x** |

Every unbatched and batched repetition retires all 20,000,000 guest instructions, publishes exactly
958,737 graph transitions and serializes byte-identically to the interpreter. Absolute hosted rates,
especially the macOS 15 interpreter samples, still move with runner load. The paired 1.871x--1.938x
session ratio is the defensible result because it compares adjacent policy arms with identical work;
the absolute rates are not a device forecast. Exact-SHA core run `30984819392` is green in all eight
jobs, and exact-SHA iOS run `30984819388` builds, fake-signs and uploads the app.

The read-only `r551-vfp-fp-session-observer-10m` replay then asks whether real firmware presents
enough adjacent arithmetic to use that win. It restores the trusted r445 checkpoint at 7.100 B,
stops at 7.110 B, exits zero in 68.727 seconds and leaves stderr empty. All 60 case-sensitive
accounting verdicts are `EXACT`; none is `MISMATCH`. Of 284,374 executed arithmetic observations,
284,349 actually enter the modeled signed runner; 25 land behind exact product entry-gate refusals
and remain literal. The warm-graph model needs 107,490 host-FP sessions, or 2.645 arithmetic
operations per session. Resetting conservatively at every decoded-head boundary needs 126,531, or
2.247 operations per session. Relative to one preservation pair per operation, those bounds remove
176,859 (**62.198%**) or 157,818 (**55.502%**) FPCR/FPSR save/restore pairs. The longest observed
call and head contain twelve and eight arithmetic operations respectively.

The replay changes no guest state and creates no duplicate snapshot. Its work-image and screen
SHA-256 remain the established
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`. The final CLCD image is
nonblack; 15,626 media reads and 469 writes complete with zero failures.

Brutal status: **FP-state batching is the first demonstrated large speed improvement for the new
arithmetic handlers, and it still does not prove meaningful emulator FPS improvement**. VFP
arithmetic accounts for only about 2.84% of fetched instructions in this restored window, although
its interpreter cost is not known to equal an average instruction. The 14.031% runner-entry removal,
1.871x--1.938x dense-loop session gain and 55.502%--62.198% real-stream preservation reduction are
different measurements and cannot honestly be multiplied into the old roughly 0--4 FPS phone
report. This exact IPA has not run on the iPhone here. The optimization is worth retaining, but an
exact-build device measurement or a trustworthy end-to-end signed-firmware timing path remains the
authority for whether the project has moved toward 30 FPS.

### 2026-08-05: 98.421% of the remaining fetch-entry refusals already have an exact TLB witness

The current product still refuses 521,880 otherwise eligible signed-runner entries in the exact
7.100--7.110 B restored interval because its 1 KiB fetch-host cache does not cover the current PC.
Returning to `arm_step()` for the first instruction of every such block is deliberately safe: that
path owns page-table walks, prefetch faults and MMIO. It is also potentially unnecessary when the
software TLB already contains the exact successful FETCH translation and the bus can prove the
translated 1 KiB span is ordinary RAM.

The `r553-fetch-preflight-10m` observer measures that distinction without changing the product or
guest. It samples the fetch cache and direct-mapped TLB **before** `--sequence-profile` performs its
own diagnostic translation. Sampling afterward would make the profiler manufacture the answer it
wanted by warming the very TLB entry under test. The probe performs no walk, fault, bus read, cache
fill or counter update; it only validates the live MMU-register stamp, exact generation/tag/fault
record and `host_ram` pointer contract.

The exact current-product result is:

| pre-profile state at a current-product fetch refusal | observations | share |
|---|---:|---:|
| exact successful TLB translation to a full plain-RAM block | **513,642** | **98.421%** |
| no matching live TLB entry | 8,238 | 1.579% |
| stale MMU context, cached fault, non-RAM, MMU-off or misalignment | 0 | 0.000% |
| total | 521,880 | 100.000% |

The overlapping gate reasons are 52 null host pointers, 521,828 block mismatches, 52 generation
mismatches, 78 privilege mismatches and zero alignment failures. Thus the broad loss is almost
entirely a change of 1 KiB virtual fetch block, not a fault or unsafe mapping. A lookup-only refill
could make up to 513,642 more observations enter signed text in this interval, moving the raw
modeled-instruction bound from 8,561,262 (85.613% fetched) to 9,074,904 (90.749%). That arithmetic
is a coverage bound, not yet a measured runner-entry reduction or speedup; the next implementation
must still prove exact call continuity and native execution.

The run stops exactly at 7.110 B with exit zero and empty stderr. All 61 case-sensitive accounting
verdicts are `EXACT`; none is `MISMATCH`. The external disk completes 15,626 reads and 469 writes
with zero failures. The work image and nonblack CLCD screen remain byte-identical to r551 at
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`. No new snapshot was
created; the run reused the retained r445 7.100 B checkpoint. All 60 strict local tests pass.

Brutal status: **this is a large systemic coverage opportunity, not evidence that the emulator is
near 30 FPS**. It can remove a literal warm-up from about 5.14% of all fetched instructions, but the
lookup, decode and signed handler still cost host time, and no physical iPhone has run this change.
The evidence is strong enough to implement a fail-closed lookup-only refill; it is not strong enough
to predict even one additional frame per second.

### 2026-08-05: adaptive lookup-only fetch refill retains the coverage and only barely clears the Apple-host cost gate

Commit `715ed21dd6bc13bfdfebbad22c1dc23c08c8869e` implements the preflight above as
`arm_fetch_cache_try_refill()`. It accepts only the exact current FETCH software-TLB generation,
virtual tag, privilege and successful translation, then requires the complete 1 KiB translated span
to resolve through `host_ram` as ordinary RAM. It performs no page-table walk, fault, MMIO read or
guest-visible access. Every refusal returns zero before guest state changes, leaving `arm_step()` as
the sole architectural fetch owner. The refill cache remains a host reconstruction and is absent
from snapshots.

Enabling that helper on every eligible miss was safe and structurally useful, but not fast enough.
The deliberately pathological all-single-call Apple test fell to 0.609x refill-off on macOS 14 and
0.560x on macOS 15. The first long representative confirmation was split: macOS 14 had a 1.059x
paired median with 9/9 wins, while macOS 15 had a 0.996x paired median, 3/9 wins and a 0.999x ratio
of medians. **That unconditional design was rejected.** A coverage improvement which loses host
time on one Apple runner is not a product speed optimization.

Commit `f4bb68451b37b354964740fb3a139ac327210c9d` adds a 1,024-entry host-only admission
predictor keyed by PC, fetch generation, instruction set and privilege. A naturally
single-instruction refill call skips fifteen repeats before it is probed again. A caller/timebase
budget of one is not treated as evidence about natural call length, and any call which proves more
than one instruction becomes admitted. Periodic probing permits changed paths to recover; direct-map
collisions fail toward another measurement rather than permanent exclusion. The predictor still
cannot observe or learn from a skipped call.

The read-only `r560-fetch-refill-length-10m` replay applies the product admission decisions to the
same unconditional call stream without changing the guest. It restores r445 at exactly 7.100 B and
stops at 7.110 B after 10,000,000 observations: 9,999,489 fetched instructions, 511 interrupt
entries and zero fetch failures. Unconditional refill finds 513,642 recoveries and removes 434,840
of 2,560,658 modeled outer runner entries (**16.982%**). Adaptive admission makes 457,256 attempts
and 56,386 skips. It attempts 24,493 single and 432,763 multi-instruction calls, skips 54,309 single
and 2,077 multi-instruction calls, and therefore removes 432,763 entries (**16.900%**) while
retaining **99.522%** of the unconditional reduction.

The recovered call-length distribution also rules out tuning a flattering high threshold:

| Oracle minimum recovered length | removals retained | share of unconditional removal |
|---:|---:|---:|
| 2 | 434,840 | 100.000% |
| 3 | 368,457 | 84.734% |
| 4 | 314,846 | 72.405% |
| 10 | 145,472 | 33.454% |
| 16 | 71,160 | 16.365% |

Those rows are offline upper bounds, not implementable foreknowledge. They show why raising the
threshold until one synthetic sample looks good would discard most real continuity. The adaptive
length-two policy instead keeps the firmware benefit and amortizes the known single-call loss.

The replay reports 60 lines ending in `EXACT` (including three `ACCOUNTING-EXACT` lines) and zero
ending in `MISMATCH`. stderr is empty. The external disk completes 15,626 reads and 469 writes with
zero failures. No new checkpoint was created. The work image and nonblack CLCD screen remain
byte-identical at
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.

Hosted timing required a second correction in methodology. The three-arm benchmark put an
interpreter run between off and adaptive in one third of repetitions, weakening its claim to be
paired. Commit `f2c8bc3ad01fc549957707af79e25df3e45410fb` adds a focused workflow which builds
only `jitbench`, alternates adjacent `off -> adaptive` and `adaptive -> off` pairs, and still requires
exact complete-machine snapshots and exact refill accounting. This makes the confirmation both
faster and less exposed to runner drift. Two independent 9 x 200,000,110-instruction runs give:

| Apple arm64 runner | adjacent pairs | paired median | geometric mean | min--max | wins |
|---|---:|---:|---:|---:|---:|
| macOS 14 | 18 | **1.091x** | **1.0861x** | 0.975x--1.281x | 17/18 |
| macOS 15 | 18 | **1.026x** | **1.0014x** | 0.870x--1.060x | 12/18 |

The macOS 14 result is a defensible hosted speed win. The macOS 15 result is not: its geometric
mean is only 0.14% above refill-off and its spread includes a 13% loss. The honest reading is
**neutral to slightly positive under runner noise**, not "2.6% faster." Because the central result
does not regress on either Apple host and the exact firmware observer retains 99.522% of the entry
reduction, adaptive refill remains enabled. It should be disabled again if a physical-device or
stable-machine measurement shows a repeatable loss.

Focused break-even run `30994602023` is green on macOS 14/15. The two adjacent confirmations,
`30994873385` and `30995187485`, are also green on both hosts. Exact implementation SHA
`f2c8bc3ad01fc549957707af79e25df3e45410fb` is green in all eight core jobs as run
`30994857220`; iOS run `30994870032` builds, fake-signs and packages the app. The strict local suite
is 60/60 green, and the commits contain no co-author trailers.

Brutal status: **this is substantial and carefully measured hot-path progress, but it does not show
that 30 FPS is close**. The strongest real-stream statement is 432,763 fewer modeled outer entries
in one 10-million-instruction window. The strongest host-timing statement is an 8.61% geometric-mean
synthetic gain on macOS 14 and effectively zero on macOS 15. Neither is end-to-end firmware time,
scanout cadence, UIKit presentation time or physical-iPhone FPS. This exact IPA has not run on the
phone here; the only trustworthy device observation remains roughly 0--4 FPS. No honest multiplier
turns these measurements into 30 FPS. The next authority is an exact-build sustained device trace;
until transport is available, broader end-to-end bottleneck work remains necessary.

### 2026-08-05: an exact negative witness removes redundant native re-entry, but not enough for 30 FPS

Commit `9bb21f12a5a536b0a249671b0b09bfbe85205038` removes one systemic losing path without
adding guest semantics. When a positive graph invocation ends before its budget at an unsupported
instruction, the signed engine returns a host-only hint only if its direct-mapped decoder entry is
still a valid negative for the exact fetch pointer, PC, generation, privilege, instruction state and
one-instruction raw witness. The machine ticks the completed batch first, repeats its device/timebase
gate, then revalidates the complete negative witness before falling directly into the one literal
`arm_step()`. An interrupt, input edge, dirty device level, fetch change or changed instruction byte
cancels the shortcut. The policy and counter are absent from snapshots.

The same executable can disable the policy, so the Apple test alternates adjacent off/on arms with
no interpreter run between them. Its ten-instruction loop contains eight signed ALU instructions,
one cached interpreter-only MUL and one signed branch. Each 200-million-instruction arm requires an
exact complete-machine snapshot, retires 180 million instructions in signed text and encounters 20
million unsupported boundaries. The product arm validates 19,611,650 bypasses; this deliberately
98.058% warm-cache workload is a cost isolation, not a firmware mix.

| Apple arm64 runner | repetitions | ratio of medians | paired median | min--max | wins |
|---|---:|---:|---:|---:|---:|
| macOS 14 | 9 | **1.048x** | **1.061x** | 0.928x--1.178x | 7/9 |
| macOS 15 | 9 | **1.107x** | **1.128x** | 0.981x--1.212x | 7/9 |

Those results prove that the redundant probe has meaningful Apple-host cost. They do **not** say
that firmware or an iPhone becomes 4.8%--12.8% faster: the synthetic loop makes the boundary almost
three times denser than the exact firmware replay and omits real framebuffer/UIKit work.

The first firmware accounting treated every ineligible call end as a candidate. That was wrong.
`r564-known-negative-adaptive-lower-bound-10m` corrects it by splitting the exact implemented/refill
model's 651,725 ineligible stops by decoder result, then replaying the product's 1,024-slot cache key
and complete one-instruction negative identity. Every modeled positive head overwrites its slot; a
refill head whose exact host pointer is unavailable clobbers the slot instead of being ignored. It
also discounts every rejected boundary reached by a refill call which the shipped adaptive predictor
would skip, including ambiguous multi-instruction continuations. Those choices charge uncertainty
against the optimization. The read-only model still assumes the existing warm-graph continuity
ceiling, so this is a conservative applicability lower bound rather than an executed native counter.

| exact 7.100--7.110 B firmware boundary | observations | share |
|---|---:|---:|
| all ineligible modeled call ends | 651,725 | 6.517% of observations |
| decoder-rejected boundaries | 530,817 | 81.450% of ineligible ends |
| supported read miss / read guard | 120,827 / 81 | cannot be negative bypasses |
| adaptive-skipped rejected boundaries | 42,472 | conservatively excluded |
| adaptive-admitted rejected boundaries | 488,345 | 92.000% of rejected boundaries |
| exact warm negative hits | **288,090** | **58.993% of admitted boundaries** |
| cold or displaced negatives | 200,255 | 41.007% of admitted boundaries |
| warm hits versus all observations | **288,090 / 10,000,000** | **2.881%** |
| warm hits versus adaptive modeled runner entries | **288,090 / 2,127,895** | **13.539%** |

The replay restores the retained r445 checkpoint at exactly 7.100 B, stops at 7.110 B and creates no
new snapshot. It exits zero with empty stderr. All 62 case-sensitive accounting verdicts end in
`EXACT` (including the accounting-exact rows), none ends in `MISMATCH`, and the 9,999,489 fetched
instructions plus 511 interrupt entries account for all 10 million observations. Its work image and
nonblack CLCD screen remain byte-identical at
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.

Focused Apple run `30997713569` succeeds on macOS 14 and 15. Exact implementation core run
`30997692642` is green in all eight jobs, and iOS run `30997692975` builds, fake-signs and packages
the app. The strict local suite is 60/60 green. No commit contains a co-author trailer.

Brutal status: **this is a real redundant-host-work reduction, not a route from 0--4 FPS to 30
FPS**. Its conservative firmware hit density is only 29.38% of the synthetic benchmark's density.
Even the non-authoritative linear sanity check scales the Apple ratios to roughly 1.014x--1.038x, and workload
costs, Apple A9 behavior, framebuffer publication and UIKit make that unsuitable as a device
forecast. The only trustworthy phone result therefore remains roughly 0--4 FPS. The fresh IPA is
worth testing for regression and a small sustained gain; expecting 30 FPS from this change would be
dishonest. A physical exact-build trace remains the authority, and a much larger end-to-end lever is
still required.

### 2026-08-05: standalone warm negatives are too sparse to justify another runtime path

The post-prefix bypass above raised a broader systemic question before any more opcode work: how
often does the machine enter the signed engine at an instruction whose exact decoder-cache slot is
already a warm negative, without first retiring a positive signed prefix? A fast rejection there
could shorten a failed graph selection, but it could not retire the instruction; `arm_step()` would
still own its architectural fetch and semantics. The question therefore needed a real-stream
population gate before another policy switch or Apple benchmark.

The read-only cache replay now counts that population separately. It uses the same 1,024-slot key,
fetch pointer, PC, generation, privilege, instruction state and one-instruction raw witness as the
product. Positive heads still overwrite their slots and ambiguous refill heads still clobber them.
The replay also stops republishing an identical warm negative: the current product's cache hit
selects the existing unsupported entry and returns without decoding or publishing it again. This
changes observer accounting only; it executes no signed code and changes no guest state.

`work/r565-standalone-negative-observer-10m` restores the retained r445 checkpoint at exactly
7.100 B, stops at 7.110 B and creates no snapshot. It exits zero in 62.914 host seconds with empty
stderr. All 63 case-sensitive accounting verdicts end in `EXACT`, none ends in `MISMATCH`, and the
work image and nonblack CLCD screen remain byte-identical at
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`.

| standalone rejected signed-engine entry | observations | share |
|---|---:|---:|
| all standalone decoder rejects | 217,281 | 2.173% of observations |
| exact fetch identity ready / unready | 119,982 / 97,299 | 55.220% / 44.780% |
| exact warm negative | **55,850** | **46.549% of ready rejects** |
| cold or displaced negative | 64,132 | 53.451% of ready rejects |
| warm negative versus all observations | **55,850 / 10,000,000** | **0.559%** |

Brutal status: **this candidate is rejected before implementation**. Even a zero-cost perfect fast
rejection could touch only 0.559% of the measured instruction observations, and every hit would
still execute the same ordinary `arm_step()`. That is smaller than the already-small post-prefix
population and cannot plausibly close a multi-fold phone-FPS gap. No runtime switch, generated
handler, app change or synthetic speed claim is added. The device result remains roughly 0--4 FPS;
the next work must seek a materially larger end-to-end lever or obtain the exact-build phone
throughput/publication split, not shave another decoder boundary.

### 2026-08-05: native raster HLE reaches the app, but remains an explicit phone experiment

The next candidate is deliberately larger than another decoder shortcut. The repository already
had four native replacements for hot iPhone OS 3.1.3 raster routines, but the iOS app neither
compiled nor drove them. The app can now opt into those replacements through the same public
`s5l8900_run()` path it ships. This is still not permission to call HLE fast: it is a controlled way
to find out whether avoiding guest raster work pays for the replacement C on an Apple A9.

The machine exposes one bounded, fail-closed pre-step hook with at most sixteen exact even-PC
targets. A 64-bit rejection filter makes almost every ordinary PC miss without scanning the target
array. A match calls the replacement; refusal executes the original guest instruction, while
success advances guest state, supplies one device tick and does not invent retired instructions for
the skipped function. Installing or clearing targets invalidates signed-A64 derived blocks, and the
signed graph builder treats every target as a hard boundary even in the middle of a cached linear
block. The callback, target table, filter and counters are live host policy, not guest snapshot
bytes; existing snapshots remain compatible.

`VMFirmwareHLE.c` retains the earlier verifier's safety rules in a portable app adapter. It requires
the exact function prologue, binds only the first matching userspace translation context, refuses a
different address space, retries only when the site is resident, preflights every 1 KiB MMU span
before writing anything, permits unprivileged virtual writes only to RAM, and permits
descriptor-supplied physical reads only from RAM or MBX EDRAM. Only the four
`IOS3_HLE_REPLACE` sites become machine boundaries. The normal IPA does not install the adapter.
A manual `experimental_hle=true` workflow build defines
`S5LBOX_IOS3_HLE_EXPERIMENT=1`, fails firmware start rather than silently becoming a false control,
labels the firmware summary as experimental, and uploads a separately named IPA.

`work/r566-app-hle-bracket` measures this exact app-shaped runner from the retained r354 checkpoint
at 7.540 B through 7.550 B retired guest instructions. Each arm restores the same checkpoint and
work image, uses 100,000-instruction `s5l8900_run()` chunks plus the app-equivalent changed-scanout
meter, and creates no snapshot. The order brackets drift: normal, HLE, HLE, normal.

| arm | core rate | changed FPS mean / max | changed signatures | mean guest gap |
|---|---:|---:|---:|---:|
| normal A | 6.977664 Minsn/s | 4.964 / 5.969 | 6 | 1.700 M instructions |
| HLE B | 4.160006 Minsn/s | 3.936 / 5.902 | 9 | 1.1375 M instructions |
| HLE C | 4.617447 Minsn/s | 4.280 / 5.624 | 9 | 1.1375 M instructions |
| normal D | 5.174374 Minsn/s | 3.268 / 3.939 | 6 | 1.720 M instructions |

Both HLE arms reach 24 exact targets and handle all 24, all at `ogl_poly_scan`. Their final screen is
byte-identical to the earlier retained HLE evidence at
`B257D0FB90F4DD510CD3147BF31E1A49FF89DB3AFBBDB062F2921E72E943729E`; both normal arms match the
earlier normal screen at
`0B24F372055080E78C7770A90B45CA8328B275212765B7FB05E465B61B545A51`. Every work image remains
byte-identical at
`94D0E05B2DEF54AE5C26F6DA4CF8C6FAB68AF3ED4BE8E9C41C5F29596E46B8EF`. stderr is empty and all
external-media operations succeed.

The symmetric desktop result is not a speed win. The two normal FPS means average 4.116; the two
HLE means average 4.108. HLE cuts the mean changed-frame guest gap by about one third, but its native
raster cost reduces guest-retired throughput from a bracket-average 6.076019 to 4.388727 Minsn/s,
about 27.8%. A current-tree r567 smoke after adding the rejection filter repeats the exact HLE
behavior: 24/24 targets handled, nine changed signatures, a 1.1375 M mean gap, 4.312 desktop FPS,
the same work-image/screen hashes, empty stderr and no new snapshot. The strict local suite is
61/61 green; the signed-A64 mid-block boundary case necessarily awaits Apple-arm64 CI on this x86
host.

Brutal status: **desktop HLE is cadence-neutral and the phone result is unknown**. Keeping it on in
the normal app would be unjustified. ARM64 can still differ materially because the control uses the
signed native graph while the replacement runs as ordinary compiled C, but that is a reason to run
the labelled A/B experiment, not a forecast. The only trustworthy existing phone observation
remains roughly 0--4 FPS, and nothing here proves that 30 FPS is close. Promotion requires the same
visible phone animation in normal and experimental IPAs, simultaneous sustained Minsn/s and changed
FPS, correct pixels, and a worthwhile repeatable gain. A regression means removing or redesigning
the path, not rationalizing it.

### 2026-08-05: sampling rejects the HLE-cost story; isolate the larger MBX phone test

The interpretation immediately above assigned the bracket's lower fixed-retired throughput to
"native raster cost." That attribution was not measured. A fixed retired-instruction window with
HLE reaches a different guest state and instruction mix, and the four arms also show large host
drift. r568 first tried to settle the question with compile-time `-pg`; it was invalid for the exact
reason already recorded at r440: `_mcount_private` plus `__fentry__` consumed about 60% of the
samples. No conclusion from r568 is retained.

r569 uses the established sampling-only build instead: ordinary `-O2`, `-pg` only at link time,
and a non-relocating executable. In the short 7.540--7.550 B pair, HLE retired 11.700728 Minsn/s
against normal's 12.065438 Minsn/s, only 3.0% lower rather than the earlier inferred 27.8%. More
importantly, its mean host gap between changed scanouts was 0.095445 seconds against 0.144593,
although the sub-second arms completed only one FPS window and are not a phone forecast.

The longer HLE sample retired 100 M instructions in 8.375276 seconds and handled 184/184
`ogl_poly_scan` roots. Across 5.76 sampled CPU seconds, each of the root handler, scanline handler,
app MMU read adapter, and shadow-row reader received exactly one 10 ms sample: 40 ms combined, or
about 0.75% after excluding the 0.44-second snapshot-validation sample. The suspected quadratic
row bookkeeping is real source structure but is not the throughput bottleneck in this workload.
Optimizing it now would be polishing a sub-1% population, so no such patch is made.

The structurally larger candidate remains the already decoded MBX path. r570 restored r446's
post-keygen hardware-renderer animation at 7.320 B and ran the current app-shaped
`s5l8900_run()`/publication loop for 100 M instructions with no new snapshot. It exited zero with
all 710 publications reading a live scanout:

| measurement | r570 MBX app-shaped run |
|---|---:|
| core rate | 2.600708 Minsn/s |
| changed signatures | 87 |
| changed FPS mean / max | 2.242 / 3.976 |
| mean guest gap | 1.153488 M instructions |
| mean host gap | 0.445842 seconds |
| windows at or above 30 FPS | 0 |

That result is negative: MBX is not fast on this x86 run. It does preserve the much smaller guest
work per changed scanout that made the path interesting. r571's clean sampling-only 20 M window
measured 7.670556 Minsn/s and 7.072 mean changed FPS. Textured-sprite execution, coordinate
generation, linear sampling, premultiplied blending, GART reads and 2D staging together account for
0.17 of 1.65 non-snapshot sampled CPU seconds, about 10%. Even deleting all of that cannot explain
the phone's reported 0--4 FPS or produce an eightfold gain.

The next phone artifact is therefore deliberately isolated rather than silently promoted. A manual
MBX workflow build uses a separate bundle identifier and the visible name `S5LBox MBX`, defaults
MBX on and the CPU renderer off, and consequently owns a fresh app container and fresh work image.
The normal app remains CPU-renderer-default. This removes the persistent-image ambiguity from the
phone A/B; it does not make MBX faster and it is not a 30 FPS claim. The decisive report remains
simultaneous sustained Minsn/s and changed FPS on the same visible animation.

### 2026-08-05: the isolated MBX artifact is exact; caching affine taps is neutral

Commit `5107c802311a8a72c820ea379b2c48dee3c6ecc5` makes the experiment
unambiguous on a phone. The normal IPA remains `com.j0shua.S5LBox` / `S5LBox`; the manual MBX IPA
is `com.j0shua.S5LBox.MBXExperiment` / `S5LBox MBX`. Its independent container forces a fresh work
image with MBX enabled and the QuartzCore CPU renderer disabled. Exact-SHA core run `31011319261`,
normal iOS run `31011319491`, and MBX iOS run `31011339013` are green. The downloaded IPAs are:

| artifact | bytes | SHA-256 |
|---|---:|---|
| normal `work/artifacts/5107c80-normal/S5LBox.ipa` | 1,242,096 | `97C315C7E2F94A40A1D0610B22B0789E1967DF8CB02E16962D800BA3DD283D25` |
| MBX `work/artifacts/5107c80-mbx/S5LBox.ipa` | 1,242,097 | `8D2CA568101355AB5507F3B674766FE76E0B124B55D815EA4647A6F4D66F4091` |

r572 then tested the largest obvious redundancy inside the sampled MBX renderer instead of assuming
it mattered. Affine textured sprites traverse every destination pixel twice: the first pass finds
the exact source-tap window and the second recomputes the same affine transform and bilinear taps
while blending. A temporary bounded cache preserved the first pass's already-quantized taps, so it
changed neither floating-point sampling nor pixels. Both cache-off and cache-on focused binaries
passed 938/938 assertions. Four alternating Release replay arms restored r446 at 7.320 B, ran the
same 100 M instructions through the app-shaped meter, and created no snapshot:

| arm | affine tap cache | core rate | mean changed FPS | changes |
|---|---|---:|---:|---:|
| A | off | 8.014172 Minsn/s | 7.043 | 87 |
| B | on | 8.169055 Minsn/s | 7.055 | 87 |
| C | on | 8.073924 Minsn/s | 7.073 | 87 |
| D | off | 8.170579 Minsn/s | 7.083 | 87 |

The cache averages 8.121490 Minsn/s against 8.092376 for the controls, only **+0.36%**, while
mean changed FPS is 7.064 versus 7.063. Every arm exits zero with empty stderr, no 30-FPS window,
the exact work-image SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D`, and the exact screen
SHA-256 `96EC1953CD74B76E979F08EB933684FE2AC4182C64BF6129143DEE7E040CE63E`.
The temporary code is removed rather than converting noise into a product feature.

Brutal status: **the isolated IPA is ready for a trustworthy phone A/B, but no measured speedup was
added here and 30 FPS is not close or established**. The x86 Release replay itself averages about
7.06 changed scanouts/s; eliminating the duplicated affine math did essentially nothing end to end.
This closes another renderer micro-optimization path. The next local work must profile a larger core
execution or device boundary, while the decisive MBX-versus-normal result remains simultaneous
sustained Minsn/s and changed FPS from these exact, visibly distinct IPAs on the same phone scene.

### 2026-08-06: a fresh current-tree MBX rebuild reaches about 14 changed scanouts/s on x86, not 30

r570--r572 remain exact measurements of the executables named in their logs, but they are not a
current-tree throughput baseline. After rebuilding `build-mbx` and the sampling-only
`build-gprof` from HEAD `b6d79e94073b987c21889d420a5140e83f4374a6`, with `core/` and `tools/`
clean, the same retained r446 state and the same 7.320--7.420 B app-shaped MBX window run much
faster. No source optimization was made in this experiment. The reason the older binaries were
slower is **not established**; build directories needed to identify every old flag no longer
exist, so attributing the difference to a particular compiler option would be invention.

r574 and r575 use the freshly rebuilt ordinary-`-O2`, link-only-`-pg` sampling binary. Both exit
zero, retire the requested 100 M instructions, observe 87 changed signatures, and finish with
byte-identical guest state:

| run | core rate | mean / max changed FPS | live publications | >=30-FPS windows |
|---|---:|---:|---:|---:|
| r574 | 14.918200 Minsn/s | 13.111 / 15.506 | 181 | 0 |
| r575 | 14.917172 Minsn/s | 12.942 / 15.109 | 182 | 0 |

The longer sample moves the broad attribution away from another MBX micro-optimization. After
excluding 0.20 seconds in one-time snapshot validation, `arm_step` owns about 38% of sampled CPU,
`exec_data_processing` about 6.7%, `s5l8900_tick` about 5.5%, and the next individual interpreter,
MMU and bus functions are each about 2.6--4.3%. `mbx_execute_textured_sprite` itself is about 2.9%.
This is x86 interpreter attribution: the iOS product additionally uses its build-time-signed
AArch64 graph, so these percentages must not be projected onto the A9.

r576 and r577 then run the freshly rebuilt non-profiled `build-mbx` binary, SHA-256
`EF15340C930F708863CEC436F7D7F30DFA59039E24BD69F0D047D9165BDECB56`, through the identical
window:

| run | core rate | mean / max changed FPS | live publications | >=30-FPS windows |
|---|---:|---:|---:|---:|
| r576 | 16.448664 Minsn/s | 14.342 / 15.854 | 167 | 0 |
| r577 | 15.895194 Minsn/s | 13.809 / 17.393 | 173 | 0 |

Their averages are 16.171929 Minsn/s and 14.076 changed scanouts/s. All four successful runs have
empty stderr, the exact work-image SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D`, the exact screen SHA-256
`96EC1953CD74B76E979F08EB933684FE2AC4182C64BF6129143DEE7E040CE63E`, and a nonblack final frame
with 250,188 of 460,800 RGB bytes non-zero. No snapshot was created: every run restored the
canonical r446 checkpoint. r573 is only a harness-location failure before guest execution; its
reconstructible work image was deleted and it contributes no performance result.

Brutal status: **the current x86 MBX baseline is substantially better than the stale executable,
but it is still about 14 changed scanouts/s, contains no 30-FPS window, and says nothing direct
about the installed iPhone build**. The physical device remains the authority. Optimizing x86
`arm_step` from this profile before measuring the iOS signed graph would risk solving the wrong
bottleneck; the next decisive action remains the unlocked-phone MBX run with simultaneous app
changed-frame status, sustained process CPU and CoreAnimation telemetry.

### 2026-08-06: the signed-static graph is a 6.17x A9 regression under the app bus

The synthetic signed-engine benchmark above was not enough evidence for product policy. Commit
`fe1b45c03516e629f58b322cd3715d08706ca404` added a runtime interpreter control and native-engine
retirement/refill counters to the **same** iPhoneOS full-guest executable. Exact workflow run
`31067846341` is green. Its hosted-signed artifact is 3,546,816 bytes with SHA-256
`8D17C3A294E0EE07D5EF8BB4F78749884DAA2D1EC6799AEE6111CF573ADAC602`; the exact phone CDHash is
`1417760a73a63df2d40ad7450404113bb1b588d3`. It has only the three previously proven Dopamine CLI
entitlements and requests no JIT or writable-executable memory.

The run reused r446's retained 7.320 B checkpoint instead of spending another cold boot. The
snapshot, external work image and sidecar remained exact:

| retained input | bytes | SHA-256 |
|---|---:|---|
| `post-swipe-7320m.bin` | 122,092,890 | `6A1F8ECF15F71AE4AC020C26E162C72C723BDEB0B04AB7BCE7BC317FC7311C61` |
| `.mdimage` | 466,825,216 | `06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D` |
| `.mdstate` | 131,248 | `C152E63315BE0F62ECB81E55C2A1B9CE7394708360AB00069210A7DCD7969CB8` |

Each arm restored that state, ran the same app-shaped 100,000-instruction schedule for exactly
100 M guest instructions, and created a separate disposable work image. The A/B/A result on the
iPhone 6s Plus A9 is decisive:

| arm | execution policy | core rate | mean / max changed cadence | changed signatures | windows zero / >=30 |
|---|---|---:|---:|---:|---:|
| A | interpreter control | 6.524984 Minsn/s | 5.664 / 7.997 | 87 | 0 / 0 |
| B | signed-static graph | 0.963142 Minsn/s | 0.838 / 1.998 | 87 | 99 / 0 |
| A | interpreter control | 6.529058 Minsn/s | 5.708 / 7.777 | 87 | 0 / 0 |

The interpreter controls average 6.527021 Minsn/s, **6.78 times the graph's rate**. This is not a
minor regression or thermal drift between separate builds: the slower arm sits between two nearly
identical controls in one exact executable. The graph claims 80,909,582 signed-native retirements
out of 100 M (80.910%), 4,311,170 chained graph entries, 3,849,314 refill attempts, 3,706,664 hits,
2,531,948 skips, and 2,654,726 known-negative bypasses. High native coverage therefore did not
translate into useful throughput; graph lookup/refill and handler-boundary costs overwhelm the
saved interpreter dispatch on this workload and device.

All three arms exit zero with empty stderr and the same terminal PC/CPSR, thread, task, process,
translation tables, fault registers, device counters and CLCD state. Every disposable work image
ends at SHA-256 `06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D`, and product versus
interpreter screen output is byte-identical on the phone at SHA-256
`F23E8D07E9C863755DBB1BFDE4A1892F17110FE529A56F676C0EBFFB02EEB7C7`. The capture-log SHA-256
values, in A/B/A order, are
`08651EB8FC657B16A3A514745A30DA3DB9D62AECF0B99619A5C8DE8F9F7CA8BB`,
`FB65D459DE8D88E88CC0BBC26AC98B5DC75C6572E664E6EF0A1DB9A2A36FADEF`, and
`BE53E9CCA0C0E8B602B6DB8E0CD4FDF68991D5D8A14466993DBC1F41FCC2D47F`.

There were two important limits. First, bootkernel's diagnostic bus observes RAM writes and therefore
revoked the engine's separate direct-RAM-write consent; the app's canonical bus does not have that
observer. Commit `94ed9dfb75dd32e20c3d7be51ba442555df2b7f4` closed that gap with a same-binary
`--canonical-bus` mode plus a separate `--no-direct-ram-writes` control. Its exact hosted artifact
came from green workflow run `31069383068`; the phone copy was 3,546,864 bytes with SHA-256
`A25766071A5E700041EAD029DEBFE0D1418AA207254F2E8AFD116650FD41B713`, and its exact admitted
CDHash was `fabd3fb733d2c88daac7ad4ba9afdf9ebdca1c65`. These are standalone lab-harness properties,
not dependencies or entitlements of the stock-compatible app.

The canonical-bus sustained controls restored the same checkpoint, ran exactly 100 M instructions,
disabled per-instruction host observers during timing, and produced the same final work-image SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D` and screen SHA-256
`F23E8D07E9C863755DBB1BFDE4A1892F17110FE529A56F676C0EBFFB02EEB7C7`:

| arm | execution policy | core rate | mean / max changed cadence | changed signatures | windows zero / >=30 |
|---|---|---:|---:|---:|---:|
| A | graph + direct writes | 1.125446 Minsn/s | 0.985 / 1.991 | 87 | 77 / 0 |
| B1 | interpreter + direct writes | 6.951659 Minsn/s | 6.062 / 7.921 | 87 | 0 / 0 |
| C | interpreter, direct writes disabled | 6.668303 Minsn/s | 5.825 / 7.641 | 87 | 0 / 0 |
| B2 | interpreter + direct writes | 6.937394 Minsn/s | 6.046 / 7.928 | 87 | 0 / 0 |

The two direct interpreter controls average 6.944527 Minsn/s: **6.17 times the graph rate** and
**4.14% faster** than the no-direct control. Their log SHA-256 values in A/B1/C/B2 order are
`C1171DAFC3A10F5B4515A4AAC558737727382B39B630653BEA343C3E64B660ED`,
`10E49597676B779D79EDD20F347A955A299EF8C9C8D695965CF1C09EB3997CD2`,
`7EED5D47AD170A8F75B246EB82D9911181C130F9AF0E0CF1C5C35E4CC453D1FF`, and
`24DA057A7C94BC769DA0253DD697B3D96BDD6B0BEDD38DDC43C199484676F600`.
This closes the app-bus caveat: product policy must disable the graph but retain the independently
useful direct-write contract.

The second limit remains: a Windows interpreter replay reached the same guest machine state at
5.982866 Minsn/s and 5.199 mean changed cadence, but its final PPM differs from the phone at three
pixels: seven of 460,800 RGB bytes, each by one least-significant bit. Cross-host raster output is
therefore not universally byte exact. The same-phone product/interpreter comparison is byte exact,
which is the comparison used for the engine decision.

This change keeps the signed engine and generated native handlers compiled for repair and controlled
experiments, removes only the graph default from the shipping iOS target, and keeps direct writes.
A CI policy test pins both decisions. The synthetic device workflow continues to build the signed
candidate deliberately, but is now labelled as a diagnostic rather than falsely described as
app-matched.

Brutal status: **this is a real, large shipping regression removed, not a 30 FPS result**. The
canonical-bus interpreter controls average only about 6.05 changed scanouts/s and contain no 30-FPS
window. More importantly, the project owner reports older app runs near 20 Minsn/s on this A9 and
40 Minsn/s on an iPhone 16 Pro Max while visible presentation still stayed around 0--2 FPS. Those
historical figures are not yet reproduced under this exact harness, but they are strong contrary
evidence against equating instruction throughput with UIKit FPS. The next foreground build must
count and time every boundary from guest scanout change through VM publication, copy/conversion,
main-thread delivery and actual presentation. MBX remains functionally useful but has not yet proved
a phone-speed win. Further structural work must target whichever measured boundary drops the frames.

### 2026-08-06: guest time is not the missing 5x; instrument the real app pipeline

Commit `55379b6517c12452eb14c8dd16292a39c5be4fc0` adds the clock domain that the earlier
frame meter omitted. Exact full-guest workflow run `31071907256` is green. Its hosted-signed arm64
artifact is 3,546,864 bytes, SHA-256
`CAC31EF1D760FE4D25F86DDDFDC0672B776598E21766E6760412995B25149FAF`, and has phone CDHash
`a3f7c61fb5a36fd90c05c992fc44a53c88c4e4c9`. Those are standalone jailbreak-lab harness
properties, not app dependencies. The stock-compatible app still requests no private entitlement.

The exact artifact restored the same authenticated r446 7.320 B state and ran the canonical app bus,
interpreter control, direct RAM writes and 100,000-instruction app schedule for exactly 100 M retired
instructions on the A9. It stopped normally with empty stderr:

| quantity | exact A9 result |
|---|---:|
| timed core rate | 6.961979 Minsn/s |
| frame-meter host span | 14.407973 s |
| guest timer advance | 26,002,769 ticks at 6 MHz = 4.333795 guest s |
| modeled active / non-retiring guest time | 0.242718 / 4.091076 s |
| CLCD VBlank advance | 260 = 59.994/guest s = 18.046/host s |
| post-baseline sampled changes | 86 = 19.844/guest s = 5.969/host s |
| publications / valid / unavailable | 366 / 366 / 0 |
| changed-window mean / max / >=30 windows | 6.040 / 7.991 / 0 |

The work image remains SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D`; the final screen remains
`F23E8D07E9C863755DBB1BFDE4A1892F17110FE529A56F676C0EBFFB02EEB7C7`. The complete stdout is
SHA-256 `DFFA93078889F767F03BA92ABB85C26C62871F1A4789E75667E232227D467F33`.

This corrects the tempting but wrong theory that one retired instruction always advances only one
modeled CPU cycle and therefore prevents guest realtime. In this interval, WFI/event fast-forward
supplies about 4.09 of 4.33 guest seconds. The CLCD itself advances at essentially 60 VBlanks per
guest second. Changing the 412 MHz CPU constant cannot provide the missing fivefold host cadence in
this checkpoint. The sampled content changes only about 19.8 times per guest second here, although
the 397-byte sampled signature can miss a small update and therefore remains an undercounting meter.

The remaining ambiguity is now narrower and measurable. Opt-in device automation counts validated
scanouts where they enter `VMEngine` and changed immutable-image submissions where
`VMFramebufferView` assigns `CALayer.contents`, including main-thread image-build time. The result is
exposed in the guest screen's accessibility value so the existing before/after physical profile can
bracket it without per-frame file I/O. A layer submission is explicitly **not** labelled a displayed
frame. The simultaneous DVT graphics service has no PID selector, so its
`CoreAnimationFramesPerSecond` sample is only a **device-wide correlating gauge**, not authority that
this app displayed a frame. Final visible-FPS proof still requires the foreground app and screen.
Normal launches pay only a disabled atomic gate. No protected controller or engine file is modified.

Brutal status: **this adds a decisive diagnostic and retracts a bad time-model lead; it adds zero
measured FPS by itself**. The next exact foreground MBX run must show which boundary loses cadence.
Only then is it rational to optimize guest rendering, publication/copy, main-thread image creation,
or the compositor instead of continuing to whack whichever code path happens to look expensive.

### 2026-08-07: two interpreter fixes are real; moving hot state ahead of the TLB is not

Commits `2f2912ec8a659bb9d1497d91b24b4d40290f57d8` and
`895f4183f5245ad2f500c8a65b224caa5b77451c` remove two costs that the exact Apple binary proved
were paid by every interpreted instruction. The first bypasses signed-static eligibility work when
that engine is disabled. The second keeps `privileged_svc_result()` out of line. That rare rollback
helper copies the approximately 68 KiB `arm_cpu_t`; Apple LTO had inlined it into `arm_step()`, so
the ordinary instruction path called `___chkstk_darwin` with a 68,160-byte frame. The fixed
`arm_step()` frame is 144 bytes; only the rare SVC helper retains the large rollback frame.

These are not assembly-only claims. Release tests pass 60/60 and strict `-Wall -Wextra -Werror`
tests pass 65/65. Exact-SHA GitHub runs `31087095281` (stock-compatible iOS), `31087095411`
(core matrix), and `31087708044` (MBX artifact) are green. The exact installed MBX executable is
SHA-256 `53C8A234372BDE3DB71A8643C92CB4616CE3C9010C5285763A36639C87E5B286`.
A matched physical-device A/B/A/B against exact pre-fix executable `5f84a20` measured pair means
of 22.559143 Minsn/s before and 27.972571 Minsn/s after, about **+24.0%**. That is substantial
instruction-throughput progress. It is still not a 30-FPS result: the scene was static, so its
displayed `0 fps` cannot establish dynamic presentation cadence.

The next candidate tested whether structure layout hid another broad AArch64 tax. In branch commit
`e2e2aa6750716833724d55539576033043e38a05`, the 64 KiB TLB backing array moved behind the fetch,
data-read and data-write host caches. Exact Apple code generation did improve as intended:

| function | `895f418` bytes | candidate bytes | change |
|---|---:|---:|---:|
| `arm_step` | `0x2928` | `0x2920` | -8 |
| `mem_r32_as` | `0x1f0` | `0x1dc` | -20 |
| `mem_w32_as` | `0x1e4` | `0x1d8` | -12 |
| `s5l8900_run` | `0xf64` | `0xf14` | -80 |

The repeated AArch64 `cpu + 0x10000` base synthesis disappeared from the fetch/read/write paths.
Exact candidate runs `31090617832` (core), `31090617732` (stock iOS), and `31090942204` (MBX)
are green. Its MBX IPA is 1,223,871 bytes, SHA-256
`364B116073AB6396A0BBA050E13A1603204539BBCBDAF68965AB9CCD028F8BCA`; the executable is
SHA-256 `949C551E28A5346BD21B15C789B167D5E86FC3424C7C3A7406290A0BCA2BFBC4`.

Cleaner code did **not** become a measured product win. The iPhone 6s Plus crossover used iOS MCP
for every device action, killed the app between arms, reinstalled exact IPAs, and restored the same
466,825,216-byte rootfs checkpoint (SHA-256
`4A5B1A2739A57CF52FA843CEF612CC285C6DC6242DA8BF7AE1832D38BCCBD6BB`) before every cold app
run. Each arm used a 20-second warmup followed by seven three-second-spaced accessibility samples:

| arm | executable | sample mean | sample median | range | layer mean |
|---|---|---:|---:|---:|---:|
| A1 | `895f418` | 28.021 | 27.910 | 26.94--28.95 | 0.330 ms |
| B1 | `e2e2aa6` | 28.634 | 28.380 | 26.75--31.25 | 0.328 ms |
| A2 | `895f418` | 27.124 | 26.830 | 24.88--31.21 | 0.325 ms |
| B2 | `e2e2aa6` | 26.744 | 26.320 | 25.89--28.16 | 0.326 ms |

Across all fourteen samples per build, the old and candidate means are 27.572857 and 27.689286
Minsn/s: only **+0.42%**. Their aggregate medians are 27.445 and 27.065 Minsn/s, **-1.39%**.
Every arm stayed at two scanout signatures because the home screen was static, so none supplies a
dynamic-FPS result. The distributions overlap completely. The layout candidate is therefore
rejected, not promoted to `main`, and the phone was restored to exact `895f418` plus the authenticated
pre-test rootfs.

Brutal status: **removing disabled-engine work and the accidental 68 KiB instruction frame was real;
moving hot fields merely made the disassembly prettier**. The shipping A9 app now sustains roughly
27--28 Minsn/s in this static scene, but historical evidence already shows that instruction rate and
visible FPS can decouple. Another structure-layout or dispatch micro-edit is not justified. The next
implementation must attack a larger boundary selected by exact dynamic-scene evidence or change the
execution strategy substantially; 30 FPS remains unproved and not close on current evidence.

### 2026-08-07: timebase-bounded User-mode tick batching is exact and buys about 4.6% on A9

The next implementation was selected by a physical A9 profile, not by another opcode guess. A
five-second root `spindump` of the ordinary cold-boot app sampled the emulator thread 502 times:
`arm_step` held 203 exclusive samples (40.438%), `s5l8900_tick` held 87 (17.331%), and
`exec_data_processing` held 80 (15.936%). The profile is a cold-boot workload rather than the retained
SpringBoard scene, but it established that the per-retirement machine tick was a real A9 cost. The
main thread used only 0.228 CPU seconds during the same five seconds while the emulator thread used
4.777, so UIKit was not the CPU bottleneck in that cold-boot interval. The retained profile SHA-256 is
`E08F8A76FD2BB9A19D2B83C85F9DEE9E63EF3104AD38B9E1FF83E278C1B9008E`.

Commit `3f7a47ed0f5798d511be64a64be565b7e91f4570` therefore collapses only redundant calls to
`s5l8900_tick`; it does not batch or translate ARM instructions. The interpreter may do this only in
User mode, with no pre-step/HLE hook, with the signed engine disabled, clean device levels, unchanged
host inputs, valid clock state, and only up to the first exact timebase edge. It still checks every
retirement for guest MMIO, host input, or exception/mode escape and stops the interval immediately.
WFI and privileged host SVC can advance device time from inside `arm_step`, which is why privileged
execution is deliberately excluded. Two host-only counters record attempted intervals and their
retirements; they are not guest state and did not move the snapshot format.

The contract is tested against the literal `arm_step()` plus `s5l8900_tick(1)` oracle across User and
privileged execution, fractional and one-to-one clocks, guest MMIO, external input, User SVC escape,
pre-step policy, invalid clocks and an initially invalid accumulator. CPU, clock phase, interrupt
state and every advancing device remain equal. Release tests pass 60/60 and strict/static-engine tests
pass 65/65. Exact-SHA GitHub runs `31145472828` (core matrix), `31145472809` (stock-compatible iOS app)
and `31145496571` (no-JIT iPhone replay) are green. The replay artifact is 3,546,864 bytes, source-exact
to `3f7a47e`, with SHA-256
`B2B5448BA15C531531E40F8147978DE98D43596C7C63539D6050ED63DAD44B21`.

The matched Windows Release benchmark is intentionally reported even though it is unimpressive. Its
new User-mode rows measured 38.66 -> 38.72 Minsn/s for ALU/branch (+0.2%) and 27.52 -> 27.39 Minsn/s
for load/store (-0.5%). That is neutral. GCC already inlines the cheap early-out on this host; the A9
profile showed an out-of-line tick, so physical-device evidence remained the acceptance gate.

The iPhone 6s Plus test reused, read-only, the authenticated r446 checkpoint at 7.320 B retired
instructions and ran to exactly 7.420 B. Before the series, the snapshot, `.mdimage`, and `.mdstate`
still had SHA-256 values
`6A1F8ECF15F71AE4AC020C26E162C72C723BDEB0B04AB7BCE7BC317FC7311C61`,
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D`, and
`C152E63315BE0F62ECB81E55C2A1B9CE7394708360AB00069210A7DCD7969CB8`; the kernel, device tree and
rootfs still had
`0D8CDB339D37CF37A1DB2638FFF79272ECD63A17764BF7666EFA1618725DF70C`,
`4867C95FEDF544BDA2ECAA2626AE14C01A60D7771DC53FFE6FD3A6AAC8B8BA57`, and
`C3251E7F092C939D5818E92086CB47680981CFB03731DE7B55D238C942EB5E82`. Every arm used canonical app bus callbacks, direct plain-RAM writes,
interpreter control, 100,000-instruction app chunks, a fresh disposable work image, and the same
frame-meter contract. The sequence was deliberately balanced B/A/B/A/B/A because the plugged-in
phone sped up during the series:

| order | build | core rate | mean / max changed cadence | stderr | zero / >=30 windows |
|---|---|---:|---:|---:|---:|
| B0 | `3f7a47e` candidate | 10.729454 Minsn/s | 9.358 / 11.636 | 0 bytes | 0 / 0 |
| A1 | `9ad43b6` baseline | 10.625308 Minsn/s | 9.231 / 9.962 | 0 bytes | 0 / 0 |
| B1 | `3f7a47e` candidate | 11.235212 Minsn/s | 9.796 / 11.975 | 0 bytes | 0 / 0 |
| A2 | `9ad43b6` baseline | 10.741804 Minsn/s | 9.370 / 11.351 | 0 bytes | 0 / 0 |
| B2 | `3f7a47e` candidate | 11.682340 Minsn/s | 10.168 / 11.586 | 0 bytes | 0 / 0 |
| A3 | `9ad43b6` baseline | 11.431719 Minsn/s | 9.965 / 11.911 | 0 bytes | 0 / 0 |

The baseline/candidate medians are 10.741804/11.235212 Minsn/s (**+4.59%**) and 9.370/9.796 mean
changed cadence (**+4.55%**). A simple linear-trend fit over the alternating order estimates +4.64%
core throughput and +4.72% cadence, so the conclusion does not depend on pretending the upward drift
was absent. The candidate deterministically covered 66,544,318 of 100 M retirements (66.544%) in
971,832 intervals with a mean of 68.473 retirements. Every arm advanced exactly 260 CLCD VBlanks and
86 post-baseline changed signatures, ended with work-image SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D`, and produced byte-identical
screen SHA-256 `F23E8D07E9C863755DBB1BFDE4A1892F17110FE529A56F676C0EBFFB02EEB7C7`.

Brutal status: **this is a repeatable, exact A9 improvement and still not close to 30 FPS**. The best
candidate window in the balanced series was 11.975 FPS, the candidate median mean was 9.796, and not
one window reached 30. Tick batching removes a measured tax; it does not supply the roughly threefold
gain still missing from this scene, and it cannot explain historical 20--40 Minsn/s foreground runs
that remained visually slow. Keep this layer, but the next step still has to be a larger no-JIT
execution/presentation change selected by dynamic evidence rather than another small layout edit.

### 2026-08-07: a live-byte compact AArch64 loop clears the architecture gate

The prior signed graph result ruled out another handler-table rearrangement: 80.91% native coverage
still ran 6.17 times slower than the interpreter on the A9. Commits
`c38c3b8db75b6c5ba10e004898e06f3cbcf08987` and
`e4f8acd3da6959ab0096c685cc7900bdfc5acc3f` therefore test a materially different boundary. One
ordinary build-time-linked AArch64 function reads live A32 instruction words, keeps the guest PC,
register file, code base and flat-RAM base resident, decodes and executes several instructions in one
loop, and returns the exact retired prefix. It allocates no executable memory, emits no runtime code,
uses no decoded-record cache, and never enters the 26,508-handler graph.

The first gate is intentionally narrow: AL data-processing result opcodes without flag writes,
immediate rotation or register LSL #0, aligned immediate word LDR/STR, and immediate B/BL. Conditions,
flag-setting/comparison forms, Thumb, MMU translation, interrupts, MMIO, unaligned policy and device
time are refused before the current instruction mutates state. The Apple-host oracle covers all twelve
result opcodes in immediate and register form, both address directions, load/store, link branches,
live-byte fetch, a branch outside the code window, an unsupported instruction after an exact prefix,
an unsupported first instruction, and pre-entry contract rollback. Registers, CPSR, cycles and every
RAM byte must equal the literal interpreter.

Exact-SHA core run `31150592987` is green across all eight jobs. Both Apple Silicon runners pass the
oracle, the 1.6 M-instruction CTest and the explicit 10 k verification. Their longer rotated
three-repetition medians use 500 M guest instructions per arm:

| runner | synthetic case | interpreter | compact live-byte loop | speedup |
|---|---|---:|---:|---:|
| macOS 14 | A32 ALU/branch | 83.167 Minsn/s | 339.661 Minsn/s | **4.084x** |
| macOS 14 | A32 25% load/store | 75.061 Minsn/s | 360.908 Minsn/s | **4.808x** |
| macOS 15 | A32 ALU/branch | 84.047 Minsn/s | 351.315 Minsn/s | **4.180x** |
| macOS 15 | A32 25% load/store | 79.348 Minsn/s | 358.923 Minsn/s | **4.523x** |

The predecoded static block reports much larger 28.56--36.82x synthetic ceilings in the same run,
but that number has already proved non-predictive at the product boundary: it repeats a tiny decoded
block and omits the lookup/refill/handler costs that destroyed the A9 graph. The compact row is the
relevant architecture gate because it includes live instruction fetch and decode inside its persistent
loop. Even it still omits real firmware and every machine/UI cost named above.

Brutal status: **4.084x at the weakest long row is enough headroom to continue this architecture; it
is not evidence of 30 FPS and it changes the shipping app by zero FPS today.** The next implementation
must extend exact coverage in census order, then enter the real SoC only behind a timebase-bounded,
interrupt-safe, live-fetch contract with serialized-machine equality. Only after restored replay and
physical A9 A/B evidence may it become an app candidate. A marginal product result still rejects it,
regardless of this attractive synthetic ceiling.

### 2026-08-07: the compact loop survives the MMU-on SoC boundary; firmware and A9 still decide

Commits `49982515dc92128c23c439d10ba76db94936c67f` and
`d172e32dcd3a44e0a7a122d629eaed0b76de9ef5` close the first obvious semantic gaps instead of
optimizing the original favourable loop. The compact decoder now implements all fourteen ordinary
A32 conditions, all sixteen data-processing flag/comparison forms, immediate rotation, and every
immediate LSL/LSR/ASR/ROR/RRX register shift. PC operands/destinations, register-specified shifts,
Thumb and most instruction classes still stop before mutation. Exact-SHA core runs `31151397911`
and `31151785140`, and stock-compatible iOS runs `31151397916` and `31151785167`, are green.

Commit `91be5c119173460db44833e56e5e85453a8593a5` then models that exact admission rule against the
unchanged restored 7.100--7.110 B interval. The replay exits zero, writes no stderr or external-media
failure, and retains work-image SHA-256
`8A59C388C481165F460984926AA5FFB1B72A0E9030216CD0038DE9B3264B79FE` and screen SHA-256
`1EF63FFE3EEFD976416E17120A36BA074BF295EA0955D716E2D345FCC5EA0A9E`. Its accounting closes at
9,999,489/9,999,489 fetched instructions:

| exact restored-stream outcome | instructions | fetched share |
|---|---:|---:|
| condition-passed compact execution | 4,977,851 | 49.781% |
| failed-condition architectural no-op | 811,454 | 8.115% |
| **total semantically admitted** | **5,789,305** | **57.896%** |
| unsupported instruction class | 2,139,357 | 21.395% |
| Thumb | 1,355,047 | 13.551% |
| every other rejection combined | 715,780 | 7.158% |

The admitted instructions form 1,103,946 runs with mean length 5.244 and maximum 2,415. Requiring
at least four/eight/sixteen consecutive admissions would retain only 79.736%/61.880%/40.295% of the
admitted population. More importantly, the guest MMU is enabled at every one of the 9,999,489
observations. The earlier flat-RAM wrapper therefore has **zero real-firmware entries**, regardless
of its synthetic speed. This is a categorical blocker, not a small coverage caveat. Exact-SHA core
run `31153034889` and iOS run `31153034865` are green.

Commit `c078a4b142a8d51deab8ed9a4e10111e3ee6cdf8` removes only that blocker. The caller must supply
the CPU's current proven 1 KiB virtual fetch window; the path rechecks its virtual block,
translation generation and privilege and refuses Thumb, big endian, abort, an unmasked interrupt,
an invalid mode, or any pre-step hook. It performs no page-table walk and claims no data-translation
authority. A condition-passed data access therefore stops before the instruction mutates state;
the interpreter still owns translation, faults, MMIO and the access itself. The mode is default off,
mutually exclusive with the decoded graph/persistent paths, bounded by `s5l8900_run()` to the first
device-time edge, and uses ordinary build-time-linked text rather than runtime code generation.

The Apple gate runs an identity-mapped MMU-on User-mode loop through the complete machine API. The
reference uses the exact interpreter tick batcher; the compact arm uses the signed engine's
equivalent first-timebase-edge bound. Setup and warm-up stay outside timing. Every repetition must
retire the full requested count through the compact counters and then produce a byte-identical
complete serialized machine. Exact-SHA core run `31154140877` is green across all eight jobs and
stock-compatible iOS run `31154140615` is green. The sustained 200 M-instruction result is:

| Apple arm64 runner | interpreter | compact MMU-on SoC path | speedup | exact bounded calls |
|---|---:|---:|---:|---:|
| macOS 14 | 71.821 Minsn/s | 185.636 Minsn/s | **2.585x** | 2,912,622 |
| macOS 15 | 64.960 Minsn/s | 171.043 Minsn/s | **2.633x** | 2,912,622 |

All three repetitions on both hosts retire exactly 200,000,000 compact instructions and pass the
serialized-machine comparison. The simultaneous direct semantic-loop medians remain 3.203x/3.763x
for ALU/mixed on macOS 14 and 3.272x/4.027x on macOS 15. The smaller but still large SoC ratios are
the honest number: roughly one third of the attractive flat-loop gain is spent on real machine
entry, live translation witnesses, timebase splitting and device ticks.

Brutal status: **the new architecture has survived a materially harder boundary at 2.585x, but it
has not yet accelerated one real firmware instruction on Apple hardware.** The synthetic SoC loop
is deliberately compute-only and achieves 100% compact retirement. Real restored firmware admits
57.896% in much shorter runs, stops at every condition-passed data access, and may lose the entire
gain to repeated entry/refusal overhead. No shipping app policy changed, no physical A9 result
exists for this path, and these Minsn/s numbers say nothing about displayed iPhone FPS. The next
valid decision is a same-binary interpreter/compact replay of the authenticated restored interval,
followed by a balanced A9 A/B only if that replay is exact and positive. App integration before
those gates would be optimism replacing evidence.

Commit `9a8f5558f010f4f69b44adbf88fbea03a3c2ff09` adds the missing same-binary
restored-replay control. `--interpreter-control` and `--compact-raw-control` now select the two arms
without changing the executable or guest inputs; the compact arm requires `--run-api`, is mutually
exclusive with the interpreter arm, refuses HLE because its exact-PC pre-step hook invalidates the
in-loop branch contract, and reports attempts, successful calls and retired instructions separately.
It changes diagnostic tooling only: the shipping app policy remains untouched. The post-change local
suites pass 65/65 in the strict signed-engine build and 60/60 in the release build. Exact-SHA core
run `31154771824` is green across all eight jobs and stock-compatible iOS run `31154771769` is green.
Those results prove that the A/B instrument is buildable and regression-clean; they still do not
prove that compact execution wins on restored firmware or improves one displayed frame.

### 2026-08-07: restored firmware rejects the fragmented compact wrapper on A9

Manual no-JIT device workflow run `31155444985` built commit `9a8f5558` successfully for iPhoneOS
arm64. The hosted artifact is 3,566,272 bytes with SHA-256
`42E3430F6D45F4FEDA9C49E1CE9D16915F44400715298D4971DC1DA6AD060007`; the byte-identical phone
copy executed without phone-side re-signing. Immediately before the test, iOS MCP re-hashed the
retained r446 snapshot, media image and media state plus the kernel, device tree and root file
system. All six matched the canonical hashes recorded above.

The physical iPhone 6s Plus (`iPhone8,2`, A9, iOS 15.8.5) then ran a reversed
interpreter/compact/compact/interpreter bracket over the same 7.320--7.330 B restored interval.
Every arm used the same executable, canonical app bus/direct writes, `--run-api`, frame meter and
fresh disposable work image:

| arm | core rate | complete host-span rate | compact retired / calls / attempts |
|---|---:|---:|---:|
| interpreter A | 17.107820 Minsn/s | 16.629 Minsn/s | 0 / 0 / 0 |
| compact B | 11.887016 Minsn/s | 11.847 Minsn/s | 4,298,791 / 1,688,084 / 6,974,776 |
| compact C | 11.975101 Minsn/s | 11.936 Minsn/s | 4,298,791 / 1,688,084 / 6,974,776 |
| interpreter D | 17.108728 Minsn/s | 17.050 Minsn/s | 0 / 0 / 0 |

All four arms exit zero, write empty stderr, retire exactly 10,000,000 instructions and finish
with work-image SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D` and screen SHA-256
`0CF5CB094E56D5FF444DB2F7DE5B838357D83295BC3607C59B669F0AAE815335`. Correctness therefore
passes. Performance does not: the medians are 17.108274 versus 11.931059 Minsn/s, so the compact
arm is **30.26% slower**. Reversing the order changes neither counter set and barely changes either
rate, which rejects a warm-up or order explanation without wasting time on a ceremonial 100 M run.

The failure mechanism is structural. Each successful native call retires only 2.5465 instructions
on average, and 5,286,692 of 6,974,776 attempted entries retire nothing. The real stream therefore
pays the AArch64 wrapper and machine-entry cost millions of times, while enabling the engine also
forfeits the accepted interpreter tick batch on fallback instructions. This is the opposite of the
synthetic SoC loop's roughly 68.7 instructions per call. The compact mode remains default off and
must not enter the app.

Brutal status: **the first physical-firmware gate killed this implementation, while preserving the
underlying live-byte idea.** A credible successor must stay resident across interpreter fallbacks,
reuse proven MMU read/write cache witnesses for RAM accesses, and amortize entry to one bounded
machine interval instead of one tiny semantic run. Filtering or micro-tuning the current 1.69 M
calls cannot plausibly reach 30 FPS. Evidence is retained under
`work/artifacts/9a8f555-a9-compact-raw-gate-20260807/`. The reported scanout/publication cadence is
still emulator-thread telemetry, not displayed UIKit/Core Animation FPS.

### 2026-08-07: proven MMU data-cache hits help substantially, but the A9 still rejects the path

Commits `146d56b12f011c4ff94e429610f371a9cee380c4` and
`637e3acc5b037f8f0b92b85125da15b36556b5a5` attack the two measured structural costs rather than
adding a catalogue of isolated handlers. The first keeps one build-time-linked AArch64 invocation
resident while `arm_step()` executes unsupported instructions exactly. The second executes only
aligned, immediate, pre-indexed, word, no-writeback LDR/STR operations through the interpreter's
already-proven 1 KiB `dread`/`dwrite` witnesses. Host pointer, virtual tag, privilege and translation
generation must all match; DWRITE additionally requires the frontend's live direct-write consent.
Every miss reaches the literal fallback before mutation, so the interpreter still owns walks,
faults, MMIO, cache fill and observer policy.

The Apple exact gate at `637e3ac` moves its controlled MMU-on loop from 5/3 native/fallback
retirements to 7/1 while preserving the complete serialized machine. Exact core run `31159903991`
and stock-compatible iOS run `31159903993` are green. Diagnostic-only commit
`d2b6c9a321cb9e05637e1eb0d3f1be01fd4f6dea` adds interval data-cache deltas; all eight jobs in core
run `31160539347` and device replay build `31160623542` are green.

The physical iPhone 6s Plus then ran the same authenticated 7.320--7.330 B interval in balanced
interpreter/candidate/candidate/interpreter order. iOS MCP re-hashed the snapshot, media triplet,
kernel, device tree and root file system first; all six remained canonical. Every arm used the same
3,566,736-byte no-JIT binary, canonical app bus/direct writes, fresh disposable work image,
`--run-api` and frame meter:

| arm | core rate | complete span | signed retired | resident fallback | calls / attempts |
|---|---:|---:|---:|---:|---:|
| interpreter A | 16.513995 Minsn/s | 16.204 Minsn/s | 0 | 0 | 0 / 0 |
| cache resident B | 15.363272 Minsn/s | 15.309 Minsn/s | 4,208,810 | 2,483,250 | 451,794 / 3,539,283 |
| cache resident C | 15.743370 Minsn/s | 15.686 Minsn/s | 4,208,810 | 2,483,250 | 451,794 / 3,539,283 |
| interpreter D | 17.143719 Minsn/s | 17.075 Minsn/s | 0 | 0 | 0 / 0 |

All four arms exit zero, write empty stderr, retire exactly 10,000,000 instructions and end with
byte-identical work-image SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D` and screen SHA-256
`0CF5CB094E56D5FF444DB2F7DE5B838357D83295BC3607C59B669F0AAE815335`. Every arm also reports the
same 3,003,411/182,581 dread hits/misses and 1,725,312/41,799 dwrite hits/misses. The compact path
increments those same architectural counters when it consumes a witness; it does not invent a
separate favourable metric.

Relative to the preceding resident gate, exactly 1,080,656 instructions move from fallback to
signed retirement. Native coverage rises 34.55% to 42.0881%; in-loop fallback falls 30.32%; the new
resident median improves 6.78% from 14.565440 to 15.553321 Minsn/s. The acceptance comparison is the
contemporaneous interpreter median of 16.828857, however, so the candidate remains **7.58% slower**.
It stays disabled and changes the shipping app by zero FPS.

The next bottleneck is now narrower and directly counted. Interpreter control batches 6,755,697
retirements into 98,664 user-mode intervals (mean 68.472). The candidate disables that batcher but
fragments 6,692,060 resident retirements into 451,794 positive calls and performs 3,539,283 probes,
3,087,489 of which retire nothing. The fixed proven 1 KiB fetch window is the main artificial return
boundary. The next implementation must reload the post-fallback proven fetch window and remain in
the same AArch64 invocation until the actual timebase/device edge. Its exact oracle must cross a
window both sequentially and by branch, refuse an unavailable witness without mutation, and retain
serialized-machine equality before another A9 gate.

Brutal status: **this is real structural progress from -30.26% to -13.99% to -7.58%, not a win and
not evidence that 30 displayed FPS is close.** More opcode whack-a-mole is no longer justified.
Evidence is retained under `work/artifacts/d2b6c9a-a9-cache-witness-gate-20260807/`. The reported
scanout/publication cadence remains emulator-thread telemetry, not UIKit/Core Animation FPS.

### 2026-08-07: cross-window residency removes the fragments and still loses on A9

Commit `7a768a2775b1df89b3e108a74bae93c4a159d31d` tests the previous section's
specific hypothesis rather than broadening the opcode catalogue. After one exact fallback, the
callback may publish the proven fetch window containing the next PC. The same build-time-linked
AArch64 invocation clears the previous descriptor, validates the new pointer/base/size/PC contract,
reloads it, and continues until its real machine budget ends. A missing or malformed publication
stops before another native fetch. No runtime-generated code is involved.

Exact core run `31163204610`, stock-iOS run `31163204579`, and full-guest replay build
`31163729731` are green. Both Apple Silicon jobs pass sequential and branch crossings, witness
refusal, stale-publication rejection, and a serialized SoC oracle with 62 crossings/reloads and zero
stops. The physical iPhone 6s Plus then ran the same authenticated 7.320--7.330 B
interpreter/candidate/candidate/interpreter bracket:

| arm | core rate | complete span | signed / fallback | calls / attempts | crossings / reloads / stops |
|---|---:|---:|---:|---:|---:|
| interpreter A | 16.688640 Minsn/s | 16.485 Minsn/s | 0 / 0 | 0 / 0 | 0 / 0 / 0 |
| window resident B | 15.443658 Minsn/s | 15.389 Minsn/s | 4,152,321 / 2,552,885 | 101,933 / 3,187,346 | 359,577 / 357,239 / 2,155 |
| window resident C | 15.101452 Minsn/s | 15.045 Minsn/s | 4,152,321 / 2,552,885 | 101,933 / 3,187,346 | 359,577 / 357,239 / 2,155 |
| interpreter D | 16.951451 Minsn/s | 16.891 Minsn/s | 0 / 0 | 0 / 0 | 0 / 0 / 0 |

All four runs exit zero with empty stderr, exactly 10 M retirements, identical cache accounting,
canonical derived work-image SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D`, and screen SHA-256
`0CF5CB094E56D5FF444DB2F7DE5B838357D83295BC3607C59B669F0AAE815335`.

The structural mechanism works: positive resident calls fall 77.44%, from 451,794 to 101,933,
almost matching the interpreter's 98,664 bounded batches. Average resident-call length rises from
14.81 to 65.78 retirements. Performance nevertheless regresses from a bracketed interpreter median
of 16.820046 to 15.272555 Minsn/s (**-9.20%**), and is 1.81% below the previous cache-resident
candidate. The remaining 2,552,885 exact `arm_step()` callbacks, including roughly 360,000 window
transitions, cost more than the removed outer returns save.

Brutal status: **the experiment falsifies resident-call fragmentation as the primary remaining
bottleneck.** It achieved the intended interval length and remained slower. Do not keep stretching
this callback architecture and do not resume small opcode whack-a-mole. A credible next engine must
remove most per-instruction C fallbacks with a broad callback-free live decoder/semantic tier; until
that exists and wins physically, the accepted interpreter stays active. The candidate remains
diagnostic-only, changes stock-app policy by zero, and provides no 30-FPS evidence. Full evidence is
under `work/artifacts/7a768a2-cross-window-gate-20260807/`; its cadence remains emulator-thread
telemetry, not displayed UIKit/Core Animation FPS.

### 2026-08-07: the exact 7.320 B census selects mixed Thumb and VFP, not another callback change

The older 7.100--7.110 B sequence census was a different scene, so it was not sufficient authority
for choosing the next engine boundary. A current-tree `build-strict` replay at documentation commit
`a66377f638b1da63f27430dd9c443dccef12d132` restored the exact r446 7.320 B checkpoint and observed
the next 10,000,000 retirements with `--sequence-profile`. It exited zero with empty stderr,
9,999,510 fetched instructions plus 490 interrupt entries, canonical derived work-image SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D`, and per-run screen SHA-256
`45E3B4807D81A42EB14251246CA94F3149647462A0BC79C4C865C9F2418B6B57`. The profiler deliberately
performs an extra translation per fetched instruction, so none of its elapsed time or ordinary TLB
counters is performance evidence.

The generated compact loop's exact instruction-semantic classifier admits 5,804,750 instructions,
or **58.050%** of the fetched stream: 4,992,391 condition-passed executions and 812,359 failed-
condition architectural no-ops. Its 1,104,522 admitted runs average 5.255 instructions. The missing
coverage is broad and stable relative to the older scene:

| current live-stream outcome | instructions | fetched share |
|---|---:|---:|
| Thumb | 1,343,144 | 13.432% |
| unsupported instruction class | 2,140,887 | 21.410% |
| broader load/store form | 212,296 | 2.123% |
| load/store PC operand | 166,872 | 1.669% |
| DP PC operand/destination | 194,647 | 1.947% |
| register-specified shift | 99,508 | 0.995% |
| DP `Rm=PC` | 18,971 | 0.190% |
| unconditional/NV space | 18,435 | 0.184% |

The class census identifies what the coarse unsupported row contains. VFP contributes 1,705,451
observations and only 32,097 condition skips, leaving **1,673,354** unexecuted by the compact tier.
Thumb contributes 1,343,144, all outside it. Block transfer leaves 380,633 outside; extra/sync,
multiply, media and coprocessor classes add another 175,606. In contrast, A32 data processing is
98.314% admitted and immediate B/BL is 100% admitted. Extending those already-dominant A32 families
again would therefore be the wrong scale of work.

This coverage model does not reinterpret the physical result: the actual A9 candidate retired
4,152,321 instructions natively and sent 2,552,885 through the resident C fallback. It only selects
the next architecture. The live signed loop must become mixed A32/Thumb and implement whole proven
Thumb and VFP families internally, then add broad witnessed memory/control families. It must not
route through the rejected decoded graph and must not call C once per common instruction. A design
target is fewer than roughly 300,000 exact fallbacks per 10 M in this interval before another phone
gate; that is a coverage gate, not a promised speedup or FPS value.

Brutal status: **the new census explains the repeated losses but adds zero accepted speed today.**
It supports a plausible Amdahl path only if the broad native semantics retain their measured inner-
loop advantage after real VFP, Thumb and memory work. The interpreter remains the stock policy.
Evidence is retained under `work/artifacts/compact-raw-admission-7320-a66377f/`.

### 2026-08-07: the callback-free mixed-Thumb tier raises exact admission to 69.430%

Commit `288b616856afa508a616ee364a83904b24882307` extends the build-time-linked live
AArch64 loop into a mixed A32/Thumb engine. It decodes and executes, inside the
signed loop, the broad Thumb families proven by the existing interpreter: immediate
and register shifts, all 16 ALU operations, high-register operations, PC/SP address
forms, conditional and unconditional control flow, A32/Thumb state exchange, and
witnessed word/byte/halfword/signed memory operations. Unsupported Thumb encodings
still fall back exactly. This is a stock-compatible ahead-of-time engine; it adds no
JIT entitlement, jailbreak dependency, decoded graph, or common-instruction C
callback.

Apple arm64 differential validation covers 99 isolated Thumb cases, including shift
counts 0, 1, 31, 32 and 33, all ALU operations and condition codes, high-register and
state-switch paths, and all admitted memory kinds. Seven resident-loop cases also
prove mixed A32/Thumb continuation, unsupported-Thumb fallback, live read witnessing,
and consented writes. Those tests compare the compact engine with the interpreter;
they are semantic evidence, not a throughput or displayed-frame-rate benchmark.

The exact r446 7.320--7.330 B census was then repeated with the new classifier at
workflow commit `0a1d1ff0aabf86fd053b30fd68070fa54760c8db`. It again accounts for
9,999,510 fetched instructions plus 490 interrupt entries, exits zero with empty
stderr, and preserves canonical derived work-image SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D` and screen
SHA-256 `45E3B4807D81A42EB14251246CA94F3149647462A0BC79C4C865C9F2418B6B57`.

| exact 10 M classifier outcome | A32-only tier | mixed-Thumb tier | delta |
|---|---:|---:|---:|
| admitted | 5,804,750 (58.050%) | 6,942,629 (69.430%) | +1,137,879 (+11.380 pp) |
| condition-passed execution | 4,992,391 | 6,056,466 | +1,064,075 |
| failed-condition no-op | 812,359 | 886,163 | +73,804 |
| rejected | 4,194,760 | 3,056,881 | -1,137,879 |

Of 1,343,144 observed Thumb instructions, 1,137,879 are now admitted and 205,265
remain outside the tier, for **84.718% Thumb admission**. The result is substantial
coverage progress, but it is not yet accepted speed: no physical-phone A/B was run,
the interpreter remains stock policy, and there is still no new UIKit/Core Animation
FPS measurement. The dominant next gap is unchanged VFP: 1,705,451 observations,
32,097 condition skips, and **1,673,354 condition-passed instructions** not yet
executed by the compact tier. Another phone gate before broad VFP coverage would
mostly remeasure known fallback overhead. Complete evidence is under
`work/artifacts/compact-raw-thumb-admission-7320-0a1d1ff-r1/`.

### 2026-08-07: broad VFP and memory/control families raise admission to 93.155%

Commits `8510c162` through `6e2e4cc3` add callback-free VFP state, memory and
arithmetic families behind exact runtime-state guards. They raise the unchanged
7.320--7.330 B classifier result from 6,942,629 to 8,501,791 admitted instructions:
**+1,559,162 instructions and +15.592 percentage points**. Commits `b4169dc8`,
`68312348` and `4cd03ecd` then add A32 `BX`/`BLX(register)`, ordinary block
transfers and broad addressing-mode-2 single transfers. This is build-time-linked
AArch64 code, not runtime code generation, and adds no JIT entitlement or jailbreak
dependency.

The single-transfer tranche covers immediate and shifted-register offsets, every
immediate shift kind including RRX, byte/word accesses, pre/post indexing,
writeback, unprivileged translation tags, PC-relative bases, PC stores and
LDR-to-PC interworking. Its guards reject unsafe aliases, unaligned words, stale or
wrong-privilege cache witnesses, revoked write consent and invalid targets before
architectural mutation. The block path handles IA/IB/DA/DB and up to sixteen words
through one fully preflighted 1 KiB DREAD/DWRITE span. Apple-arm64 differential
oracles cover the flat and resident paths, cache telemetry, fallback convergence
and transactional refusal. Exact-SHA core run `31181608245` and stock-iPhone
build/IPA run `31181607347` are entirely green at `4cd03ec`.

The current-tree restored census exits zero with empty stderr and again accounts
for 9,999,510 fetched instructions plus 490 interrupt entries. Its derived work
image and per-run screen remain canonical at SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D` and
`45E3B4807D81A42EB14251246CA94F3149647462A0BC79C4C865C9F2418B6B57`.

| exact 10 M classifier outcome | broad VFP | + indirect/block/single | delta |
|---|---:|---:|---:|
| admitted | 8,501,791 (85.022%) | 9,315,056 (93.155%) | +813,265 (+8.133 pp) |
| condition-passed execution | 7,615,628 | 8,428,893 | +813,265 |
| rejected | 1,497,719 | 684,454 | -813,265 |
| admitted runs | 1,049,175 | 545,072 | -504,103 |
| mean admitted run | 8.103 | 17.090 | +8.987 |

A32 single transfers now have **100.000% semantic admission** in this scene.
Ordinary block support raises that class from 5.140% to 75.098%, while the indirect
branch work raises `ARM other` to 90.208%. This is substantial architectural
progress, not measured phone speed. The sequence profiler performs an extra
translation for every fetched instruction, so its elapsed time and ordinary TLB
counters are invalid as performance evidence. No physical-phone A/B or displayed
UIKit/Core Animation FPS measurement has been run for this build, and the shipping
interpreter policy has not changed.

Brutal status: **93.155% is still short of the fewer-than-roughly-300k fallback
design gate.** Exactly 684,454 fetched instructions remain outside the semantic
tier. The largest coherent residuals are the remaining Thumb stack/long-branch
forms (205,265 total Thumb rejects), VFP guarded/form rejects (114,192), LDM-to-PC
and other block forms (99,921), coprocessor instructions (86,683), and the smaller
DP/extra/multiply/media families. The next implementation combines Thumb
PUSH/POP/STM/LDM and long BL/BLX with transactional LDM-to-PC; another phone run
before those whole families land would mostly remeasure known fallback cost.
Evidence is retained under
`work/artifacts/compact-raw-block-single-admission-7320-4cd03ec-r1/`.

### 2026-08-07: Thumb stack/return and LDM-to-PC raise exact admission to 96.170%

Commit `622d2c490b46f86735fb6f79492b7197c500c609` adds native Thumb
`SXTH`/`SXTB`/`UXTH`/`UXTB`, `PUSH`/`POP`, `STMIA`/`LDMIA`, and the two-halfword
BL/BLX forms to the same build-time-linked compact loop. It also extends the
transactional A32 block path through plain LDM-to-PC interworking. Invalid
`0b10` targets, unaligned or cross-1-KiB spans, empty lists, unsafe base aliases,
stale read witnesses, and revoked write consent are rejected before register,
writeback, cache-telemetry, or memory commit. A follow-up at
`c24ad58bc351809e4d3fbc704f8e401488b4b22c` fixes one AArch64 immediate encoding;
it changes no guest semantics.

The generated assembly has 27,282 labels with zero duplicates. Local strict
CTest passes 65/65. Exact-SHA core run `31184673044` is green in every job,
including the semantic loop on macOS 14 and 15 Apple arm64 and the JIT-off
rebuild. Stock-iPhone build/IPA run `31184673177` is also green. Its policy
checks confirm that this remains ahead-of-time signed code with no JIT
entitlement or jailbreak dependency.

The unchanged r446 checkpoint was restored at 7,320,000,000 and stopped at
7,330,000,000 with `--sequence-profile`. The run exits zero with empty stderr,
accounts for 9,999,510 fetched instructions plus 490 interrupt entries, and
preserves derived work-image SHA-256
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D` and screen
SHA-256 `45E3B4807D81A42EB14251246CA94F3149647462A0BC79C4C865C9F2418B6B57`.

| exact 10 M classifier outcome | prior broad tier | + Thumb stack/return + LDM-PC | delta |
|---|---:|---:|---:|
| admitted | 9,315,056 (93.155%) | 9,616,559 (96.170%) | +301,503 (+3.015 pp) |
| condition-passed execution | 8,428,893 | 8,730,396 | +301,503 |
| rejected | 684,454 | 382,951 | -301,503 |
| admitted runs | 545,072 | 351,203 | -193,869 |
| mean admitted run | 17.090 | 27.382 | +10.292 |

Thumb semantic admission is now 1,342,988/1,343,144 (**99.988%**), leaving 156
per-class rejects instead of 205,265. Block-transfer admission is
397,731/401,258 (**99.121%**), leaving 3,527 instead of 99,921. Those two exact
reductions account for the complete 301,503-instruction gain. This is broad,
reproducible architectural progress, not measured phone speed: no physical A9
A/B or displayed UIKit/Core Animation FPS measurement has run for this build,
and the accepted app policy remains the interpreter.

Brutal status: **96.170% still misses the deliberately chosen phone-test gate.**
There are 382,951 exact rejects, 82,951 above the fewer-than-roughly-300k design
target. The largest coherent outcome gaps are 114,058 guarded/form VFP rejects,
99,508 A32 register-specified shifts, and 86,900 unsupported instruction-class
outcomes. The register-shift family alone is large enough to cross the gate and
shares the already-proven data-processing commit path; implement that whole
family and rerun this identical census before spending another physical-phone
bracket. Evidence is under
`work/artifacts/compact-raw-thumb-stack-admission-7320-c24ad58-r1/`.

### 2026-08-07: true A32 register shifts expose an overlapping residual

Commit `360f6fdf3fc8cf0ce5cccdc015bb498ed9ced989` adds the complete A32
data-processing register-specified-shift shape to the build-time-linked compact
loop. The shift amount is the low byte of `Rs`; LSL/LSR/ASR/ROR cover amounts
0, 1, 7, 31, 32, 33, 64 and 255, both incoming carry states, every one of the
16 data-processing opcodes, flag-setting and non-flag-setting result forms, and
the five architecturally awkward destination/source aliases. PC operands and
the overlapping bit-7 multiply/extra/synchronization space still refuse before
guest state changes. The Apple-arm64 oracle covers 1,797 exact cases plus four
PC-refusal/fallback convergence cases.

Local strict CTest passes 65/65. Exact-SHA core run `31186445394` is green in
every job, including the compact semantic loop on macOS 14 and 15, sanitizers,
strict warnings, and the JIT-off rebuild. Stock-iPhone build/IPA run
`31186446894` is also green. The generated assembly contains 27,292 unique
labels and no runtime code generation.

The unchanged r446 checkpoint was again restored at 7,320,000,000 and stopped
at 7,330,000,000 with `--sequence-profile`. It exits zero with empty stderr,
accounts for 9,999,510 fetched instructions plus 490 interrupt entries, and
preserves the same derived work-image and screen hashes as the preceding run.

| exact 10 M classifier outcome | prior Thumb/LDM-PC tier | + true A32 register shifts | delta |
|---|---:|---:|---:|
| admitted | 9,616,559 (96.170%) | 9,639,738 (96.402%) | +23,179 (+0.232 pp) |
| rejected | 382,951 | 359,772 | -23,179 |
| admitted runs | 351,203 | 333,297 | -17,906 |
| mean admitted run | 27.382 | 28.922 | +1.540 |

Brutal correction: the previous section's projection was wrong. The old
99,508-count `register shift` outcome was not one homogeneous data-processing
family. Only 23,179 dynamic instructions were true register-specified shifts;
the other 76,329 have both bits 7 and 4 set and belong to multiply,
extra-load/store, or synchronization encodings. Treating the outcome label as
an implementation-size estimate hid that overlap.

This is still substantial architectural coverage, but it does **not** cross
the deliberately chosen fewer-than-roughly-300k phone-test gate. There are
359,772 exact rejects, still 59,772 above it, and no physical-phone FPS result
for this build. The next measured coherent candidate is `VCVT.F32.F64`: the
decoder census sees 62,586 narrowing conversions, but admission will remain
literal until their live FPSCR modes, operands, results and sticky-flag effects
are audited. Evidence is retained under
`work/artifacts/compact-raw-register-shift-admission-7320-360f6fd-r1/`.

### 2026-08-07: audited VFP narrowing crosses the physical-test gate at 97.028%

Commit `7cbab223b386b62b1dfdb25bf3c882c459914918` admits only the exact
`VCVT.F32.F64` state observed in the unchanged 7.320--7.330 B interval. The
decision came from a read-only live audit, not from treating a decoder count as
proof. All 62,586 candidates used round-to-nearest with FZ and DN set, had no
vector length or exception enables, consumed only signed-zero or finite-normal
double inputs, and produced only signed-zero or finite-normal single outputs.
The incoming sticky state was IXC-only, no conversion made a new exception bit
visible, every literal interpreter result was `ARM_OK`, and all 62,586 passed
the proposed guard both before and after conversion. The audit accounts for
every candidate exactly and is retained under
`work/artifacts/compact-raw-vfp-narrow-audit-7320-wip-r1/`.

The signed-static and compact live-byte handlers still validate the contract at
runtime. They enter a lazy host-FP session, save the caller's complete FPCR/FPSR,
run the conversion under a controlled internal mode, stage the result, reject
unexpected result classes or exception flags before guest commit, and restore
the caller state on both success and fallback. The compact handler also rejects
the smallest-normal plus IXC flush boundary. There is no executable allocation,
runtime-generated code, JIT entitlement, jailbreak dependency, or stock-app
policy change.

Local strict CTest passes 65/65. The generated assembly contains 27,307 unique
labels and no duplicate labels. Exact-SHA core run `31189124207` is green in
all eight jobs, including the Apple-arm64 semantic and serialized-SoC oracles
on macOS 14 and 15 plus the JIT-off rebuild. Stock-iPhone build/IPA run
`31189124817` is also green.

The post-change restored census again exits zero with empty stderr and accounts
for 9,999,510 fetched instructions plus 490 interrupt entries. Its derived
work-image SHA-256 is
`06AAAA84FB4BFEAE5A647290C9B50BEBE7640F420457089F40BDBED961D6992D` and its
screen SHA-256 is
`45E3B4807D81A42EB14251246CA94F3149647462A0BC79C4C865C9F2418B6B57`.

| exact 10 M classifier outcome | register-shift tier | + guarded VFP narrowing | delta |
|---|---:|---:|---:|
| admitted | 9,639,738 (96.402%) | 9,702,324 (97.028%) | +62,586 (+0.626 pp) |
| condition-passed execution | 8,753,575 | 8,816,161 | +62,586 |
| rejected | 359,772 | 297,186 | -62,586 |
| admitted runs | 333,297 | 271,231 | -62,066 |
| mean admitted run | 28.922 | 35.771 | +6.849 |

This crosses the predeclared fewer-than-roughly-300k physical-test threshold by
only 2,814 instructions. That threshold was an engineering gate, not a speed or
FPS prediction. The sequence profiler performs an extra translation per fetched
instruction, so its elapsed time is invalid as performance evidence. **There is
still no physical-phone speedup and no basis for claiming that 30 displayed FPS
is close.** The accepted app remains on the interpreter until a controlled A9
interpreter/candidate bracket proves the candidate faster with identical guest
state. Exact admission evidence is retained under
`work/artifacts/compact-raw-vfp-narrow-admission-7320-7cbab22-r1/`.

### 2026-08-09: safe system coprocessors cut native entries, not visible FPS

Commit `522c7b274e74208edd8cceea659be07e37902925` keeps three coherent,
callback-free system-coprocessor families in the build-time-linked compact
loop: privileged CP14 probes of the deliberately absent debug unit; CP15 c7
cache/barrier operations except WFI; and CP15 c13 software thread-ID reads and
writes with the interpreter's exact privilege rules. WFI still falls back for
its synchronous platform callback. MMU, TLB and control mutations remain
interpreter-owned. There is no runtime code generation or JIT entitlement.

The Apple-arm64 differential oracle covers privileged and User CP14/c7/c13,
PC-source writes, WFI prefix stopping, failed-condition WFI, and privilege/CRm
refusals. Exact-tip core run `31262652956` passes all eight jobs; stock-iPhone
build run `31263032113` also passes. The unchanged 7.320--7.330 B replay exits
zero with empty stderr and canonical media/raw-screen hashes. Its exact
semantic model moves from 9,702,324 to 9,788,838 admitted instructions
(97.028% to 97.893%). It admits 86,514 of 86,683 observed coprocessor
instructions, cuts modeled admitted-run entries from 271,231 to 190,557, and
raises mean modeled run length from 35.771 to 51.370.

The physical iPhone 6s Plus then ran a balanced candidate/baseline/baseline/
candidate bracket. Every arm restored the same authenticated 7.320 B snapshot
and work image, reached valid live scanout, accepted all 156 generated touch
samples, and ran the same six alternating 450 ms home-screen swipes.

| balanced metric | `429f5c1` baseline | system-coprocessor candidate | delta |
|---|---:|---:|---:|
| CPU throughput | 21.5158 Minsn/s | 22.0986 Minsn/s | +2.709% |
| compact calls / native retired | 0.076511 | 0.066957 | -12.487% |
| changed scanout signatures/s | 13.7285 | 14.6243 | +6.525% |
| changed layer signatures/s | 13.6124 | 14.5767 | +7.084% |
| pooled visible FPS median | 16.0 | 16.0 | no change |
| pooled visible FPS mean | 14.5 | 13.5 | -1.0 FPS |
| pooled samples at or below 4 FPS | 3/16 | 1/16 | two fewer |

The candidate's native-call density and core-throughput improvements are real.
The frame result is not: the pooled median is unchanged, the mean is lower,
and individual samples span 0--24 FPS. The changed-signature rows are pipeline
telemetry, not proof that iOS displayed those frames. Two fewer hard stalls are
directionally useful but far too small a sample to claim a stability fix.

Brutal status: **this is a correct structural gain and a failed visible-FPS
gate.** Keep the broad semantics, but do not call 16 FPS close to 30 and do not
spend the next iteration on another residual coprocessor opcode. The remaining
work is to remove short-run, zero-retirement and fetch-window/entry overhead as
a system, then explain why higher instruction throughput still does not map
monotonically to displayed cadence.

### 2026-08-09: exact privileged witnesses remove wasted probes, not the FPS ceiling

Commit `3bbf24d80beff1f95ab2922fb05900f2624f6f9e` removes a specific
double-dispatch path without adding a sticky opcode cache. After a productive
privileged compact prefix stops on an instruction that the architectural
fallback will not execute, the engine records one exact pending witness: fetch
host, PC, fetch generation, privilege, ARM/Thumb state, and the literal two or
four instruction bytes. The next matching entry consumes that witness and goes
directly to one interpreter step. Any mismatch discards it. The ordinary hot
path does not perform a persistent negative-cache lookup.

The Apple-arm64 differential oracle verifies both sides of the switch. With
the bypass disabled, its exact boundary case makes two attempts, one productive
call, and one zero-retirement probe. With the bypass enabled, it makes one
attempt, one productive call, zero zero-retirement probes, and one bypass; the
serialized machine states remain identical. Core run `31264800171` and stock-
iPhone build run `31264800095` are green.

The iPhone 6s Plus then ran candidate/baseline/baseline/candidate from the same
authenticated 7.320 B snapshot and writable image. Each arm performed the same
six alternating 450 ms home-screen swipes, accepted all 156 generated touch
samples, and refused none. Throughput and pipeline rows below pool cumulative
counters by their corresponding host time; visible FPS gives each arm the same
eight samples.

| balanced metric | `e92c6c0` baseline | exact-witness candidate | delta |
|---|---:|---:|---:|
| CPU throughput | 22.0238 Minsn/s | 22.4510 Minsn/s | +1.940% |
| compact attempts / native retired | 0.078140 | 0.063846 | -18.293% |
| compact calls / native retired | 0.058404 | 0.057432 | -1.664% |
| zero-retirement share of attempts | 25.1899% | 9.9612% | -60.456% relative |
| changed scanout signatures/s | 18.6921 | 19.2514 | +2.992% |
| changed layer signatures/s | 18.6855 | 19.2434 | +2.986% |
| pooled visible FPS median | 18.5 | 18.0 | -0.5 FPS |
| pooled visible FPS mean | 17.2500 | 17.3125 | +0.0625 FPS |

The candidate consumed 27,707,419 exact witnesses across its two arms. The
privileged-attempt/call difference still equals the remaining zero-retirement
count exactly, so the counter identifies the intended pathology rather than an
unrelated workload shift. Candidate FPS samples span 6--23; baseline samples
span 9--23. The half-FPS median loss and 0.0625-FPS mean gain are noise, not a
visible speedup.

Brutal status: **ship this as a bounded efficiency improvement, not as progress
to 30 displayed FPS.** It removes real redundant work, improves physical CPU
and pipeline rates, preserves exact semantics, and is cheap enough to retain.
It also proves that these failed privileged re-entries were not the dominant
frame limiter. The next iteration must target the remaining fetch-window and
entry costs or the guest-to-display cadence directly.

### 2026-08-10: privileged window continuation cuts entries but stays off by default

Commit `88c7ed2f16a907c0321b8a0f539179c7e11f94b2` extends the resident
compact interval across a 1 KiB fetch-window boundary in privileged mode. The
callback first accounts the completed prefix through the ordinary machine and
device boundary, then rechecks interrupts and translation state before
publishing another exact FETCH witness. Walks, faults, MMIO, control-state
changes and the unchanged instruction remain outside the callback. Follow-up
oracle fixes through `ceda36b4a2c612a34a7384a6e91a814e2b262e47` prove the
serialized-machine contract and require fewer outer entries without runtime
code generation.

The first physical test used the existing all-window-refill switch. Across
three exact Settings pairs, enabling both User and privileged continuation cut
median compact calls by 42.61%, fallback retirements by 43.97%, privileged
calls by 49.81% and fetch-refill attempts by 81.03%. That was substantial
engine efficiency. It was also a bad product result: median endpoint FPS fell
from 15 to 10, changed scanout signatures/s fell 42.0%, and the measured
scanout host interval did not get shorter. The result could not identify the
privileged path alone because its OFF arm also disabled the older User-mode
continuation.

Commit `dafe857cfce7a8e7132a8895a4bd3e4c280fd6e3` therefore keeps the
implementation but separates its rollout. User-mode continuation remains on
in both arms and in the stock app. Privileged continuation has its own
nonempty `engine.compact-privileged-window-refill-on` experiment marker and
defaults off. Conflicting User-only or all-window-refill controls fail loudly.
Local strict CTest passes 65/65. Exact-SHA core run `31321723729` and stock-
iPhone build run `31321723747` are green.

Both Apple-arm64 jobs report the same byte-exact privileged-window oracle:
8,288 guest instructions, 63 refills, 1,087 boundary retirements, 186 compact
calls OFF versus 153 ON, and 62 versus 32 outer fallbacks. The output records
`user-window-refill=on-both`, `privileged-control=isolated`, serialized-machine
equality, real device ticks and no runtime code generation. The downloaded IPA
SHA-256 is
`869108E6BB2A0510ACE117B6A580175D02019B3567223B5C8E42358CF454ACD0`.
Its extracted and installed executable both hash to
`BFCEF4295DC1E760BD2E24AC462D2556AAAD9A7C46C9A91C16B0CD6A9090A7C6`.

The iPhone 6s Plus then ran OFF/ON, ON/OFF, OFF/ON from the same authenticated
7,212 M snapshot, writable image, active host clock, Settings touch and 160 M
instruction cap. Every arm consumed the restore and touch markers, accepted
both touch transitions, reached the populated Settings screen, and preserved
work-image SHA-256
`3DFEB6129FDE451B7DC4BBF66E67082407D313ABF486E3275BA04F8FBFB138FD`.
Snapshot and external-media state hashes were also identical in all arms.

| isolated physical-A9 metric | privileged OFF samples | privileged ON samples | median ON vs OFF |
|---|---:|---:|---:|
| endpoint FPS | 10, 17, 15 | 15, 8, 17 | 15 vs 15; no gain |
| changed scanout signatures | 13, 14, 14 | 14, 10, 12 | 12 vs 14; -14.29% |
| changed scanout signatures/s | 2.845, 2.861, 2.844 | 2.991, 1.959, 2.369 | 2.369 vs 2.845; -16.73% |
| scanout host interval, s | 4.218199, 4.544131, 4.571654 | 4.345907, 4.593015, 4.643237 | 4.593015 vs 4.544131; +1.08% |
| maximum scanout-attempt gap, ms | 38.163, 40.683, 42.970 | 39.104, 40.077, 37.752 | 39.104 vs 40.683; -3.88% |
| attempt gaps over 100 / 500 ms | 0 / 0 in every arm | 0 / 0 in every arm | no stall difference |
| maximum changed-scanout gap, ms | 3067.215, 3388.880, 3352.018 | 3206.162, 3505.590, 3416.189 | 3416.189 vs 3352.018; +1.91% |
| compact calls | 4,230,621; 4,255,180; 4,308,932 | 2,452,022; 2,387,666; 2,395,521 | -43.70% |
| privileged compact calls | 3,723,527; 3,748,350; 3,803,113 | 1,945,372; 1,881,680; 1,887,752 | -49.64% |
| compact native retirements | 151,762,675; 151,948,168; 151,933,239 | 152,228,241; 152,097,501; 152,081,092 | +0.11% |
| compact fallback retirements | 4,916,986; 4,917,549; 4,903,656 | 4,912,786; 4,915,199; 4,922,069 | -0.04% |
| fetch-refill attempts | 2,304,318; 2,318,188; 2,353,328 | 421,208; 414,983; 416,587 | -82.03% |
| privileged window refills | 0, 0, 0 | 2,127,877; 2,107,278; 2,108,528 | exact control separation |

Brutal status: **retain the code, reject it as the stock default, and do not
call it an FPS improvement.** The entry and fetch-work reductions are large,
stable and worth keeping for later composition with a real cadence fix. The
isolated median FPS is unchanged, mean endpoint FPS falls from 14.0 to 13.3,
and changed-signature cadence is worse. This neither reaches 30 FPS nor fixes
the reported 2 FPS navigation experience. Raw evidence is retained under
`work/artifacts/dafe857-a9-privileged-window-isolated/`; the phone was returned
to the marker-free stock policy after the comparison.

### 2026-08-10: compact PC sampling finds transition work, not presentation

Commit `2fe8094a5fdf1d6018d404cc6beb27da05311c92` adds an explicit
diagnostic marker that samples process CPU time only while `s5l8900_run()` is
active. Build-time text aliases divide the generated compact runner into ten
regions without adding an executed instruction. Marker-free machines install
no signal handler or timer and pay only one disabled gate per public run
slice, never per guest instruction. Local strict CTest passes 65/65. Exact-SHA
core run `31324423103` and iPhone build run `31324423077` are green.

The downloaded IPA SHA-256 is
`0F98AB98677F4817347A253C125C2D8E066E7D8926456E5AF6664F5AA51E44D4`.
Its extracted and installed executable both hash to
`6D54B9458438E54CC6E97A5E4290207EC6EEDA5A23048E23CA5A21F3591EC23C`.
The iPhone 6s Plus ran three repetitions from the authenticated 7,212 M
Settings snapshot, matching writable image, active host clock, identical touch
and 160 M instruction cap. Every repetition accepted both touch transitions,
reached populated Settings and consumed its one-shot restore and touch files.

| physical-A9 profile metric | run 1 | run 2 | run 3 | median |
|---|---:|---:|---:|---:|
| endpoint FPS | 9 | 11 | 14 | 11 |
| changed scanout signatures/s | 2.620 | 2.615 | 2.580 | 2.615 |
| scanout host interval, s | 4.198850 | 4.206586 | 4.651963 | 4.206586 |
| maximum changed-scanout gap, ms | 3081.515 | 3123.450 | 3435.226 | 3123.450 |
| compact calls | 4,223,322 | 4,237,032 | 4,293,026 | 4,237,032 |
| User fast window refills | 4,192,056 | 4,189,756 | 4,165,795 | 4,189,756 |
| profile samples | 1,463 | 1,479 | 2,599 | 1,479 |
| samples outside generated runner | 46.89% | 50.17% | 58.37% | 50.17% |
| Thumb share of runner-only samples | 34.62% | 32.29% | 31.61% | 32.29% |

The sampler is statistical, its absolute delivery count varies, and the
outside bucket does not by itself identify a C function. It therefore does
not prove that all outside time belongs to refills. The stable combination is
still decisive enough to choose the next experiment: roughly half the samples
are outside generated text, the run crosses User fetch windows about 4.19
million times through a C callback, layer work remains only 0.34 ms on average,
and changed pixels still disappear for more than three seconds.

Brutal status: **the app is CPU-heavy and still not usable at the requested
cadence.** This profile is diagnosis, not an FPS improvement. It rejects layer
presentation as the dominant cost and makes a resident multi-window witness
cache the next structural target: repeated, already-proved User windows should
switch inside signed AArch64 text without re-entering C. Exact translation
generation, privilege, fallback and serialized-machine oracles remain required
before that path can be enabled. Raw measurements are retained under
`work/artifacts/2fe8094-a9-compact-pc-profile/`; the diagnostic marker was
removed after the third run.
