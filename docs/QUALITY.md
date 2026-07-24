# Quality and validation

This file records what was actually checked for the post-run18 TV-out,
framebuffer-planning, CLCD-bounds, and boot-diagnostic change at commit
`afa650e284c2b27b6a4a2a2b2d772e0f68e5dac9`, plus run19's real-firmware
verdict and the resulting surgical timing-predicate correction. It separates
baseline engineering evidence, real-firmware evidence, and pending correction
validation so that a green unit test is never presented as a SpringBoard boot.

## Current verdict

- The post-run19 surgical correction built successfully in Release mode, and
  all **23/23** registered CTest tests passed on the final local tree.
- The corrected affected binaries reported **5,504 passed, 0 failed** for
  `test_soc` and **469 passed, 0 failed** for `test_snapshot`.
- The pre-run19 `afa650e` targeted CTest gate for `s5l8900_machine`, `snapshot`,
  and `external_md_cli_preflight` had also passed **3/3**.
- `external_md_cli_preflight`, strict compiler checks, diagnostic analyzer
  checks, and zero-step tool smokes passed as recorded below.
- Hosted GitHub Actions passed for the exact commit: the
  [core run](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30088519878)
  was green across Linux, macOS, Windows, warnings-as-errors, ASan+UBSan, and
  JIT jobs; the
  [unsigned iOS run](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30088519892)
  was also green.
- Run19, the first post-change real-firmware validation, completed cleanly at
  2.5 B instructions with exit code 0 and empty stderr. It validated exact
  source integrity, external-md layout, TV-out MMIO routing, and the active
  primary CLCD window.
- Run19 did **not** produce TV-out VSYNC/IRQ 30, enter the shipped filter/action,
  return from the exact `IOServiceClose`, advance SpringBoard, or mutate live
  scanout. It exposed an over-strict all-three-bank timing predicate in the
  `afa650e` model.
- The surgical correction keeps control's independent ready handshake but
  makes mixer+SDO the timing eligibility pair. Local focused validation is
  green; hosted validation and a fresh firmware run are pending. Nothing here
  claims run20, a SpringBoard render, or iPhone runtime stability.

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
| Run19 integrity/layout | Exit 0, `OK`, stderr 0 B; exact source hashes unchanged; work image 466,825,216 B | The `afa650e` firmware run was clean and confined to its work image; framebuffer/TOKD planning reached real firmware | A rendered frame or correct TV-out timing |
| Run19 TV-out chain | Pages mapped; final control/mixer/SDO `0/5/1`; frames/IRQ/filter/action/close-return all zero | The all-three timing condition was not the shipped active state and suppressed completion | That mixer+SDO alone will clear the wait until rerun |
| Run19 primary CLCD/frame | Correct 320x480, stride-1280 window; 662 CLCD frames; final PPM identical to seed | Corrected layout and controller handoff operated for the full run | Any SpringBoard pixel or live-scanout write |
| Diff hygiene | `git diff --check` passed after the documentation update | No whitespace-error patch was introduced | Markdown rendering on every client |

Run19 is the real-firmware verdict on `afa650e`: routing/layout passed, but the
aggregate timing predicate failed. The new local 23/23 result and focused
5,504/0 plus 469/0 totals validate the surgical correction off-firmware. The
hosted runs still describe `afa650e`; they do not validate the subsequent
source change.

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
- [ ] Hosted core/iOS workflows pass the exact surgical-fix commit.
- [ ] A fresh firmware run shows SDO VSYNC and VIC0 raw IRQ 30 asserting under
  the shipped `0/5/1` state.
- [ ] The shipped IRQ filter/action acknowledges SDO pending, clears active
  swap work, wakes the gate, and lets the exact `IOServiceClose` return.
- [ ] Later SpringBoard control flow and live CLCD scanout mutation are
  observed.
- [ ] An installable build is tested separately on the iPhone 6s Plus.

Until those gates pass, the honest claim is narrow: run19 exposed the exact
timing-predicate error and the correction has strong local edge coverage, but
its hosted, real-firmware, rendering, and on-device results remain unproven.
