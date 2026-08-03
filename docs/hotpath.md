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

r468 then counted every instruction in a 10 M-instruction restored steady-state
interval rather than carrying forward the early-kernel mix from `docs/dynarec.md`:

| state / mode | instructions | share |
|---|---:|---:|
| ARM state | 8,644,952 | **86.44952%** |
| Thumb state | 1,355,048 | 13.55048% |
| User | 6,713,186 | 67.13186% |
| SVC | 2,898,732 | 28.98732% |
| IRQ | 384,873 | 3.84873% |
| FIQ / ABT / UND combined | 3,209 | 0.03209% |

The counts sum to exactly 10,000,000. They describe this post-keygen
SpringBoard/MBX interval, not the whole boot, but they settle the next architecture for
the target workload: a portable block interpreter must prioritise ARM first. Building
Thumb first from the old 68.95% early-kernel sample would optimise the wrong phase.

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
