# Quality and validation

This file records what was actually checked for the post-run18 TV-out,
framebuffer-planning, CLCD-bounds, and boot-diagnostic change at commit
`afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9`, plus run19's real-firmware
verdict, the surgical timing-predicate correction at exact commit
`590d2248af4d7e5e92ec7bbd1be079c3bb415542`, run20's firmware verdict, and the
VFP11 decoder correction and run21 firmware replay at exact commit
`debec04ff9b0faa469d5ad2ee7d75d1bf3b53b1a`. It separates
committed/hosted, real-firmware, visual, and local-only evidence so that neither
a green unit test nor a later lifecycle callback is presented as a rendered
SpringBoard. Later test-only commits are recorded separately and do not inherit
run21's firmware evidence.

## Current verdict

- The post-run19 TV-out correction built successfully in Release mode, and all
  **23/23** then-registered CTest tests passed on that tree.
- The corrected affected binaries reported **5,504 passed, 0 failed** for
  `test_soc` and **469 passed, 0 failed** for `test_snapshot`.
- The pre-run19 `afa650e` targeted CTest gate for `s5l8900_machine`, `snapshot`,
  and `external_md_cli_preflight` had also passed **3/3**.
- `external_md_cli_preflight`, strict compiler checks, diagnostic analyzer
  checks, and zero-step tool smokes passed as recorded below.
- Hosted GitHub Actions passed for the pre-run19 exact commit: the
  [core run](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30088519878)
  was green across Linux, macOS, Windows, warnings-as-errors, ASan+UBSan, and
  JIT jobs; the
  [unsigned iOS run](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30088519892)
  was also green.
- Hosted GitHub Actions also passed for exact correction commit `590d224`: the
  [core run](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30091220128)
  and
  [unsigned iOS run](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30091220122)
  were both green.
- Run19 exposed the over-strict all-three-bank timing predicate. Run20 then
  validated the `590d224` mixer+SDO correction in real firmware: 4 TV-out
  frames, IRQ 30 filter/action, swap-gate wake, and exact `IOServiceClose` user
  return `r0=0` at 1,915,263,517.
- Run20 advanced UIKit's `startWindowServer` return to 1,919,831,289 with a
  decoded 320x480 display and entered SpringBoard
  `applicationDidFinishLaunching:` at 1,923,358,329.
- Run20 exited **9** at 1,937,979,818 on `0xEE274B10` in PID 20's libm
  `_fmod+0x1a8`. The encoding is valid VFP11 `FMDHR` /
  `VMOV.32 d7[1], r4`, not NEON.
- The VFP11 decoder correction at exact commit `debec04` passes the local
  Release suite **23/23** and focused strict builds. Targeted `test_vfp`,
  `test_arm`, and `test_jit` binaries pass **452/0**, **810/0**, and **347/0**
  respectively. Exact-commit hosted
  [core run 30095081111](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30095081111)
  and
  [unsigned iOS run 30095081184](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30095081184)
  are green.
- Run21 firmware-validated that exact correction: it cleared `_fmod+0x1a8` and
  exited **0** at the configured **2,500,000,000-instruction cap**,
  **562,020,182 instructions beyond** run20's stop.
- SpringBoard again entered `applicationDidFinishLaunching:` at
  1,923,358,329. `-[SBTetherController isTethered]` returned false at
  1,924,647,850, and SpringBoard continued through debugging/demo preferences,
  lock-button, and platform-controller setup. It entered
  `+[SBTelephonyManager sharedTelephonyManager]` at 1,965,837,070 and `-init`;
  PID 20's last exact user instruction was 1,966,242,080, then its thread
  switched out at 1,966,246,193 in a shared-cache `mach_msg` before returning
  to the caller. Post-run resolution proves that
  `_CTTelephonyCenterGetDefault` creates a CTServerConnection, successfully
  looks up literal `com.apple.commcenter`, receives port name **0x4f07**, and
  blocks in its initial generated handshake before the `mach_msg` return.
  No reply, deadlock, queue-full state, or baseband cause is inferred.
- **SpringBoard was not rendered.** Run21 recorded zero `UIController` hits,
  zero live-scanout mutations, and zero changed pixels; the final PPM was the
  unchanged seed.
- Test-only commit `0670ab8` passed hosted core/iOS runs 30096115501 and
  30096115527 with VFP **469/0**. Latest hosted test-only commit `657e8d8` expands
  helper-sequence coverage to VFP **488/0 locally** and passed hosted core/iOS
  runs 30097023293 and 30097023356.

## Evidence ledger

| Check | Result | What it establishes | What it does not establish |
|---|---|---|---|
| Full CMake Release build | Passed locally on the post-run19 correction | All configured core tests and host tools compiled and linked together | Cross-platform or iPhone compilation |
| Full Release CTest suite | **23/23 passed** on the post-fix tree | The public firmware-free suite, including storage, firmware parsers, CPU, SoC, snapshot, framebuffer bridge, and CLI preflight, is green with the corrected predicate | Private-firmware or device behavior |
| Final focused unit binaries | `test_soc`: **5,504/0**; `test_snapshot`: **469/0** | The real `0/5/1` timing state, independent control transitions, IRQ/WFI, MMIO, CLCD, and snapshot invariants pass locally | Real Apple driver behavior after the correction |
| Baseline targeted CTest | **3/3 passed** at `afa650e`: `s5l8900_machine`, `snapshot`, `external_md_cli_preflight` | The linked baseline targets and startup preflight passed before run19 | The post-run19 correction by itself |
| `external_md_cli_preflight` | Passed | Startup self-checks pin framebuffer PA `0x0885c000` and `topOfKernelData` `0x088f4000`; incompatible external-md/tree/RAM/root/snapshot combinations fail closed before firmware is opened | A rootfs copy, guest boot, or long storage run |
| Strict GCC pass | Passed with `-std=c11 -Wall -Wextra -Werror` on the changed core and focused test sources | The affected portable-C paths are warning-free under the local GCC frontend | Clang, MSVC, Xcode, sanitizers, or runtime behavior |
| Targeted diagnostic warnings | Relevant `-Wformat=2` and `-Wconversion` checks passed | New diagnostic formatting and selected conversion-sensitive paths were checked more strictly than the default build | A whole-repository conversion-clean guarantee |
| GCC static analyzer | Passed for the changed `bootkernel` diagnostic paths | No analyzer finding remained in the new TV-out/framebuffer diagnostic control flow | Proof that the analyzer models every guest/host interaction |
| Zero-step tool smokes | `bootkernel` and `snapboot` passed | Startup invariants, option/report plumbing, and the new non-invasive state diagnostics execute without retiring guest instructions | Any emulated-time progress or firmware stability |
| [Hosted core run 30088519878](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30088519878) | Passed for exact commit `afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9` | Linux, macOS, Windows, warnings-as-errors, ASan+UBSan, and JIT jobs were green in GitHub Actions | Private-firmware execution or an on-device boot |
| [Hosted iOS run 30088519892](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30088519892) | Passed for the same exact commit | The unsigned iOS workflow built successfully on the hosted runner | Installation, signing, JIT entitlement activation, or iPhone runtime stability |
| [Hosted correction core run 30091220128](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30091220128) | Passed for exact commit `590d2248af4d7e5e92ec7bbd1be079c3bb415542` | The corrected core passed the hosted platform, strict-warning, sanitizer, and JIT matrix | Private-firmware execution or an on-device boot |
| [Hosted correction iOS run 30091220122](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30091220122) | Passed for the same exact commit | The corrected revision built successfully in the unsigned iOS workflow | Installation, signing, JIT entitlement activation, or iPhone runtime stability |
| [Hosted VFP core run 30095081111](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30095081111) | Passed for exact commit `debec04ff9b0faa469d5ad2ee7d75d1bf3b53b1a` | The VFP correction passed the hosted platform, strict-warning, sanitizer, and JIT matrix | Private-firmware execution, pixels, or an on-device boot |
| [Hosted VFP iOS run 30095081184](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30095081184) | Passed for the same exact commit | The decoder-fix revision built successfully in the unsigned iOS workflow | Installation, signing, JIT activation, or iPhone runtime stability |
| [Hosted exact-path test core run 30096115501](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30096115501) | Passed for test-only commit `0670ab8cbf6b9febbfe059b17ffdeb755ee0133a`; VFP **469/0** | The exact libm sequence regression passed in the hosted core matrix | A second firmware run or evidence belonging to `debec04` |
| [Hosted exact-path test iOS run 30096115527](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30096115527) | Passed for the same test-only commit | The expanded-test revision still built in the unsigned iOS workflow | Device execution or private-firmware behavior |
| [Latest hosted helper core run 30097023293](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30097023293) | Passed for test-only commit `657e8d8f2f42d09c573a4012a618e0f896307bdf`; VFP **488/0 locally** | Additional post-`_fmod` helper sequences are covered and the hosted core matrix is green | Firmware execution at that test-only commit or rendering |
| [Current helper iOS run 30097023356](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30097023356) | Passed for the same test-only commit | The current expanded-test revision builds in the unsigned iOS workflow | Device execution or private-firmware behavior |
| Run19 integrity/layout | Exit 0, `OK`, stderr 0 B; exact source hashes unchanged; work image 466,825,216 B | The `afa650e` firmware run was clean and persistent filesystem writes were confined to its work image; framebuffer/TOKD planning reached real firmware | A rendered frame or correct TV-out timing |
| Run19 TV-out chain | Pages mapped; final control/mixer/SDO `0/5/1`; frames/IRQ/filter/action/close-return all zero | The all-three timing condition was not the shipped active state and suppressed completion | That mixer+SDO alone will clear the wait until rerun |
| Run19 primary CLCD/frame | Correct 320x480, stride-1280 window; 662 CLCD frames; final PPM identical to seed | Corrected layout and controller handoff operated for the full run | Any SpringBoard pixel or live-scanout write |
| Run20 integrity/resources | Exit **9** at 1,937,979,818; original source hashes unchanged; external-md failures **0**; guest-free low **51.76 MiB**; run directory **447.18 MiB on F:** | The retained `590d224` run stayed inside the expected storage/resource envelope until its explicit CPU stop | A clean cap, boot success, or decoder-fix validation |
| Run20 TV-out chain | **4 frames**; IRQ 30 filter/action hit; gate woke; exact PID 20 close user return `r0=0` at 1,915,263,517 | The mixer+SDO correction fixes the observed run19 TV-out completion wait in real firmware | Framebuffer mutation or a rendered home screen |
| Run20 SpringBoard/display | `startWindowServer` returned at 1,919,831,289; display decoded 320x480; `applicationDidFinishLaunching:` entered at 1,923,358,329; `UIController` unreached; **0 changed pixels** | Control flow advanced past the old close wait and into the application delegate | SpringBoard rendering, UI readiness, touch, or on-device runtime |
| Run20 VFP stop | Exit 9 on `0xEE274B10` in PID 20 libm `_fmod+0x1a8` | The committed decoder's NEON classification is wrong; the word is VFP11 `FMDHR` / `VMOV.32 d7[1], r4` | That the local decoder correction works in firmware |
| `debec04` VFP local gate | Release **23/23**; `test_vfp` **452/0**; `test_arm` **810/0**; `test_jit` **347/0**; focused strict builds passed | All 16 D registers, both halves and directions, decoder boundaries, disabled-VFP behavior, conditions, JIT refusal, system-register privilege, and guest-Undefined disposition are locally covered | Firmware behavior outside the reached path |
| Run21 integrity/resources | Exit **0** at 2,500,000,000; source hashes unchanged; external-md failures **0**; guest-free low **50.63 MiB**; run directory **447.27 MiB on F:** | The `debec04` cold run reached its cap within the expected source/storage boundary | Boot completion, rendering, or on-device runtime |
| Run21 VFP replay | Crossed the run20 stop by **562,020,182 instructions** | The exact `_fmod+0x1a8` VFP correction is firmware-validated | General VFP completeness |
| Run21 SpringBoard/display | `applicationDidFinishLaunching:` at 1,923,358,329; `isTethered` false at 1,924,647,850; telephony singleton entry 1,965,837,070; successful `com.apple.commcenter` lookup returned port `0x4f07`; initial request `0x0054b557` entered `mach_msg` at `0x30a117e0` with send/receive sizes `0x834`/`0x30` and did not reach `0x30a117e4`; `UIController` **0**; live scanout **0**; PPM seed with **0 changed pixels** | SpringBoard progressed beyond the former CPU stop into the initial CTServerConnection handshake; header size 6 is stale state left by the stock generated stub, not emulator corruption | A service reply, deadlock, queue-full condition, baseband causality, UI readiness, or rendered SpringBoard |
| Diff hygiene | `git diff --check` passed after the documentation update | No whitespace-error patch was introduced | Markdown rendering on every client |

Run19 is the real-firmware verdict on `afa650e`: routing/layout passed, but the
aggregate timing predicate failed. The 23/23 result and focused 5,504/0 plus
469/0 totals validate the surgical correction off-firmware; the `590d224`
hosted runs validate its public build/test/package surface; run20 validates its
TV-out behavior in private firmware. None of those establishes rendering or
on-device behavior. Exact commit `debec04` owns the VFP hosted results and
run21 firmware validation. The later `0670ab8` and `657e8d8` test-only commits
must not inherit run21's firmware evidence.

## Run19 real-firmware evidence

The exact-gated source hashes were reverified after the run:

```text
kernel.macho    0D8CDB339D37CF37A1DB2638FFF79272ECD63A17764BF7666EFA1618725DF70C
devicetree.bin  4867C95FEDF544BDA2ECAA2626AE14C01A60D7771DC53FFE6FD3A6AAC8B8BA57
rootfs.img      C3251E7F092C939D5818E92086CB47680981CFB03731DE7B55D238C942EB5E82
```

The run used a fresh, exact 466,825,216-byte work image. Boot arguments at
`0x087db000`, raw bounce `0x087dc000..0x0885c000`, framebuffer
`0x0885c000..0x088f2000`, and physical
`topOfKernelData 0x088f4000` were accepted. The bridge recorded zero failures;
its native raw path completed two redirects and two completions with zero
pending.

Run19 repeated run18's boundary. `UIApplicationMain` ran at 1,849,444,535,
`rendersLocally` returned `YES` at 1,869,087,332, the optional finalizer/close
ran at 1,887,341,013/1,887,341,029, and ID-2816 asserted its wait at
1,887,344,201 before the exact thread switched out at 1,887,345,137. From
run18's finalizer through its wait, every matching checkpoint moved by exactly
13,983,022 instructions. No close return or later SpringBoard checkpoint was
gained.

The exact late TV-out state was control/mixer/SDO `0/5/1`, SDO pending/mask
`0/0`, zero model frames, zero raw IRQ 30, zero filter/action hits, and zero
close-return hits. Control ready reads returned `0x2` and removed run18's
shutdown-timeout warnings, so that independent handshake remains valid. Static
driver disassembly shows control `+0` is conditional source state; mixer+SDO
are the persistent timing eligibility pair.

CLCD window 0 was 320x480 at `0x0885c000`, stride 1280, and counted 662 frames.
The 460,815-byte PPM SHA-256 was
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`,
identical to run18's seed: 153,472 black pixels, 128 white seed pixels, no
other colors, zero changed pixels, and zero live-scanout writes.

## Run20 real-firmware evidence

Run20 used exact source commit
`590d2248af4d7e5e92ec7bbd1be079c3bb415542`. Unlike run19, it did not reach the
configured 2.5 B cap: it exited 9 at 1,937,979,818 instructions on an explicit
undefined-instruction stop. Before that stop, the original source inputs
reverified unchanged:

```text
kernel.macho    0D8CDB339D37CF37A1DB2638FFF79272ECD63A17764BF7666EFA1618725DF70C
devicetree.bin  4867C95FEDF544BDA2ECAA2626AE14C01A60D7771DC53FFE6FD3A6AAC8B8BA57
rootfs.img      C3251E7F092C939D5818E92086CB47680981CFB03731DE7B55D238C942EB5E82
```

The external-md bridge reported zero failures. The 466,825,216-byte work image
remained inside a retained run directory measuring 447.18 MiB on F:, and the
guest-free low-water sample was 13,250 pages, 51.76 MiB, at instruction
1,937,571,840.

The corrected completion path and downstream checkpoints were:

```text
TV-out IRQ 30 filter entry                         1,894,168,651
TV-out IRQ action entry                            1,894,171,336
IOMFB close sleep-gate return                      1,894,175,066
IOMFB close epilogue                               1,915,251,328
IOServiceClose PID 20 user return, r0=0            1,915,263,517
UIKit startWindowServer return                     1,919,831,289
SpringBoard applicationDidFinishLaunching: entry  1,923,358,329
VFP decoder stop                                   1,937,979,818
```

The TV-out model counted 4 frames, and the shipped filter/action cleared the
work and woke the gate. The primary display decoded as 320x480 with stride
1280. These are runtime confirmations of the `590d224` correction, not visual
evidence.

The `UIController` call had zero hits. The 460,815-byte PPM retained SHA-256
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`,
the run18/run19 seed: 153,472 black pixels, 128 white seed pixels, no other
colors, and **0 changed pixels**. The live observer likewise recorded zero
overlapping or RGB-visible scanout writes. Therefore SpringBoard is **not
rendered**, even though its application delegate was entered.

The terminal word was `0xEE274B10` at userspace PC `0x33acca88`, resolved under
PID 20 to libm `_fmod+0x1a8`. VFP11 names it `FMDHR d7, r4`; modern tools may
print `VMOV.32 d7[1], r4`. It is a VFPv2 32-bit transfer into the high word of
`d7`, not an Advanced SIMD/NEON lane operation.

## Run21 real-firmware evidence

Run21 used exact source commit
`debec04ff9b0faa469d5ad2ee7d75d1bf3b53b1a`. It exited **0** at the configured
**2,500,000,000-instruction cap**, rather than stopping at run20's
1,937,979,818 boundary. The replay therefore advanced
**562,020,182 instructions beyond** the former stop and firmware-validates the
VFP11 high-word transfer correction in libm `_fmod+0x1a8`.

The immutable source inputs reverified unchanged:

```text
kernel.macho    0D8CDB339D37CF37A1DB2638FFF79272ECD63A17764BF7666EFA1618725DF70C
devicetree.bin  4867C95FEDF544BDA2ECAA2626AE14C01A60D7771DC53FFE6FD3A6AAC8B8BA57
rootfs.img      C3251E7F092C939D5818E92086CB47680981CFB03731DE7B55D238C942EB5E82
```

The external-md bridge reported **0 failures**. Guest free memory reached a
low of **50.63 MiB**, and the retained run directory occupies **447.27 MiB on
F:**.

SpringBoard entered `applicationDidFinishLaunching:` at the same exact
1,923,358,329 checkpoint. `-[SBTetherController isTethered]` returned from
`0x967ba` to `0xa72c` at **1,924,647,850**, then took the false branch
`0xa730 -> 0xa74c`. SpringBoard continued through
`loadDebuggingAndDemoPrefs`, `_initLockButtonBearTrap`, and
`SBPlatformController`, disproving `isTethered` as the later frontier.

It then entered `+[SBTelephonyManager sharedTelephonyManager]` at
**1,965,837,070** and `-init` at `0x28240`. PID 20's last exact-attributed user
instruction was **1,966,242,080**. Its thread switched out at
**1,966,246,193** inside a shared-cache `mach_msg`, before returning to
`0xa77d`, while other guest work continued to the cap.

Post-run shared-cache resolution and message-episode decoding remove the
service ambiguity. `_CTTelephonyCenterGetDefault` creates a
CTServerConnection. The bootstrap lookup for the literal
`com.apple.commcenter` succeeds and returns port name **0x4f07**. The initial
generated handshake enters at **0x30a1177c** and calls `mach_msg` at
**0x30a117e0** with request ID **0x0054b557**, send size **0x834**, and receive
size **0x30**. SpringBoard blocks before **0x30a117e4**. The generated stub does
not initialize `msgh_size` or `reserved` before the call, so the observed
header size **6** is stale stock stack state rather than emulator corruption.

These facts prove the service identity and initial handshake boundary. They do
not prove a reply, deadlock, queue-full condition, or causal link to the absent
baseband. The next diagnostic gate is to trace the `ipc_mqueue_send` receiver
and queue state, then correlate CommCenter PID 24's wait state with the observed
baseband/SPI behavior. Since this synchronous stock path gates UI startup, the
minimum graceful no-modem behavior needed to continue boot is part of M5; full
telephony, SIM, and cellular operation remains outside that boot gate.

The presentation result did not improve. `UIController` had **0 hits**, the
live-scanout observer recorded **0 mutations**, and the PPM remained the exact
seed with SHA-256
`CBAD1C110E67CAD553A2B4EEBBF46E7BF09255389851902B24816249294AF2AB`
and **0 changed pixels**. SpringBoard is therefore **not rendered**.

Run21 belongs only to `debec04`. Exact-commit hosted
[core run 30095081111](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30095081111)
and
[unsigned iOS run 30095081184](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30095081184)
also passed. Test-only commit `0670ab8` later passed
[core run 30096115501](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30096115501)
and
[unsigned iOS run 30096115527](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30096115527),
with VFP **469/0**. Latest hosted test-only commit `657e8d8` expands the local VFP
result to **488/0** and passed hosted
[core run 30097023293](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30097023293)
and
[unsigned iOS run 30097023356](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30097023356).

## Local edge coverage of the surgical correction

### TV-out MMIO and interrupt behavior

- All three independent 4 KiB banks are present at `0x39100000`,
  `0x39200000`, and `0x39300000`, with exact window names and sizes.
- Reset exposes hardware-derived ready bit 1 while stopped, ignores guest
  attempts to store that bit, and preserves unrelated control bits such as the
  mixer's bit 2.
- Byte, halfword, and word accesses use little-endian lanes. An unaligned
  halfword crossing two backing words round-trips, while an access crossing a
  4 KiB bank boundary is rejected and counted as unmapped.
- The real run19 state control/mixer/SDO `0/5/1` is timing-active. Control may
  independently transition between run and ready without resetting phase;
  stopping mixer or SDO resets phase.
- SDO pending bit 0 latches, its mask gates the level without destroying
  pending state, and a byte write-one-to-clear acknowledgement deasserts it.
  Timing-gate transitions do not silently consume a latched completion, and
  restart does not manufacture an immediate stale VBlank.
- Large tick intervals preserve the number of elapsed boundaries and residual
  phase. VIC0 line 30 reaches the CPU; mixer acknowledgement does not fabricate
  IRQ 38.
- WFI advances to the next deliverable TV-out boundary without adding retired
  instructions, and existing earliest-edge behavior remains covered against
  timer and CLCD activity.

### Snapshot format and malformed state

- Snapshot format v4 serializes all three TV-out register banks, frame period,
  residual phase, and frame counter; the live `0/5/1` state with nonzero phase
  must round-trip.
- Invalid `frame_accum >= frame_ticks`, residual phase while mixer+SDO timing
  is stopped, a stored hardware-owned ready bit, unsupported SDO status bits,
  and nonzero nonasserting mixer status are rejected. Control-ready with live
  mixer+SDO phase is valid.
- Every malformed save case starts with non-null output sentinels and proves
  that failure returns no allocation and a zero length.
- A legitimate pending VSYNC may remain latched while stopped until explicit
  W1C and is accepted. Version mismatch, malformed structure, checksum, and
  transactional-load behavior remain covered by the broader snapshot suite.

### Framebuffer planning and CLCD bounds

- The external-md startup invariant requires the 320x480x4 framebuffer range
  `0x0885c000..0x088f2000`, advances physical `topOfKernelData` to the next
  16 KiB boundary at `0x088f4000`, and preserves at least `0x11000` bytes of
  bootstrap headroom.
- CLCD seeding validates `stride * height` in 64-bit arithmetic, its 4 KiB
  round-up, the driver's 32-bit allocation-size limit, and the complete
  physical end address.
- Tests cover multiplication overflow, page-rounding overflow, padded final
  stride, a physical span crossing 4 GiB, and the valid boundary case ending
  exactly at 4 GiB. Rejection is atomic: no controller field changes.

### Diagnostic integrity

- Firmware-free startup self-checks exercise the framebuffer/headroom constants
  and TV-out completion-chain classifier tables.
- The TV-out report reads machine state directly rather than generating device
  reads, so enabling diagnostics cannot acknowledge or manufacture an
  interrupt.
- Format-heavy diagnostic paths received targeted compiler and analyzer
  checks, followed by zero-step `bootkernel` and `snapboot` executions.

## VFP11 decoder correction and later test-only coverage

The local decoder now distinguishes the VFP11 cp11 word-transfer encodings from
the genuinely NEON-only scalar forms. It accepts the low/high 32-bit halves of
`d0..d15` (`FMDLR`/`FMDHR` and `FMRDL`/`FMRDH`), including the exact run20
`0xEE274B10`, while retaining fail-closed behavior for 8/16-bit lanes,
`d16..d31`, and malformed/reserved forms. The focused local binaries report:

```text
test_vfp  452 passed, 0 failed
test_arm  810 passed, 0 failed
test_jit  347 passed, 0 failed
```

The exact `debec04` source tree passes the complete local Release suite 23/23 and focused
`-Wall -Wextra -Werror` builds. The privilege regressions also prove that
user-mode FPEXC access enters the guest Undefined vector with state unchanged;
it cannot escape as a fatal emulator status. Hosted core/iOS workflows passed
for that exact commit, and run21 cleared the exact firmware stop to the 2.5 B
cap. The later test-only `0670ab8` exact-path regression passed hosted CI with
VFP 469/0. The latest hosted `657e8d8` helper-sequence expansion passes VFP 488/0
locally and passed hosted core/iOS runs 30097023293/30097023356.

## Reproducing the public subset

The public suite needs no Apple firmware:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The two focused device/state tests can be reproduced with:

```sh
ctest --test-dir build -C Release \
  -R "^(s5l8900_machine|snapshot)$" \
  --output-on-failure
```

The separately recorded preflight can be run with:

```sh
ctest --test-dir build -C Release \
  -R "^external_md_cli_preflight$" \
  --output-on-failure
```

Assertion counts are useful evidence for one revision, not a permanent API.
When tests change, the current executable output and hosted workflow logs are
authoritative.

## Promotion gates

- [x] Hosted core tests, strict warnings, sanitizers, JIT jobs, and the unsigned
  iOS build passed for the pre-run19 baseline
  `afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9`.
- [x] Run19 showed all three TV-out pages reaching the model, validated the
  corrected framebuffer/TOKD layout, and captured the exact `0/5/1` state.
- [x] The surgical mixer+SDO predicate correction passes the local full suite
  **23/23**, `test_soc` **5,504/0**, and `test_snapshot` **469/0**.
- [x] Hosted
  [core run 30091220128](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30091220128)
  and
  [iOS run 30091220122](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30091220122)
  passed exact surgical-fix commit `590d224`.
- [x] Run20 produced 4 TV-out frames, asserted IRQ 30, entered the shipped
  filter/action, cleared the swap work, woke the gate, and returned the exact
  `IOServiceClose` to PID 20 with `r0=0`.
- [x] Run20 returned from `startWindowServer` with a decoded 320x480 display and
  entered SpringBoard `applicationDidFinishLaunching:`.
- [ ] SpringBoard reaches the `UIController` call and produces recognizable
  live CLCD scanout. Run21 still had zero hits and zero changed pixels.
- [x] The local VFP11 correction passes the targeted VFP/ARM/JIT binaries.
- [x] The current VFP11 correction passes the full local Release suite and
  focused strict builds.
- [x] Hosted
  [core run 30095081111](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30095081111)
  and
  [iOS run 30095081184](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30095081184)
  pass exact VFP11-correction commit `debec04`.
- [x] Run21 clears `0xEE274B10` in libm `_fmod+0x1a8` and reaches the clean
  2.5 B cap, 562,020,182 instructions beyond run20.
- [x] Test-only `0670ab8` passes hosted core/iOS CI with VFP 469/0.
- [x] Latest hosted test-only `657e8d8` helper coverage, VFP 488/0 locally, passes
  hosted
  [core run 30097023293](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30097023293)
  and
  [iOS run 30097023356](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30097023356).
- [ ] An installable build is tested separately on the iPhone 6s Plus.

The honest claim remains narrow: run20 proves the TV-out correction, and run21
firmware-validates the VFP correction through a clean 2.5 B cap. Neither run
renders SpringBoard. The service is now identified as `com.apple.commcenter`,
but the initial CTServerConnection reply, queue state, CommCenter PID 24 wait,
baseband correlation, UI readiness, and on-device behavior remain open.
