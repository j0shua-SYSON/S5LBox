# Current audio implementation (2026-10-10 candidate)

The `feature/audio-disk-presets` branch adds timed codec-side I²S DMA playback
and capture, plus an iOS 13+ AVAudioEngine backend. Physical-device microphone
recording and decoded Voice Memos replay through host PCM are verified;
audible playback and routing remain candidate gates.
The older investigation below is preserved
as historical evidence; its performance numbers and missing-PL080 statements
do not describe the current emulator.

- `core/src/soc/i2s_audio.c`: bounded serial FIFOs, PLL/divider-derived sample
  rates, playback/capture PCM, DAC mute, digital volume and output-PGA volume.
  Guest time advances the serial port; host audio callbacks never touch the board.
- `machine.c`: PL080 request backpressure and sample-edge servicing within
  batched clock advances. WFI names the next audio edge.
- `VMAudioBuffer.c`: two bounded single-producer/single-consumer queues and
  rate conversion to/from a 48 kHz host stream. No callback allocation or locks.
- `VMAudioOutput.m`: route/interruption handling, foreground-only host audio,
  and explicit microphone permission. Machine Settings has a session-only
  microphone switch, initially off. Denial feeds silence, not fabricated input.
- Snapshot v34 stores unread guest FIFO bytes and clock phases. Tested v32/v33
  migration starts empty FIFOs, since those versions had no PCM transport.
  Host microphone buffers, permission and callback pointers are never saved.

Host tests cover digital samples in both directions, clock ratios, muted/attenuated
output, full/empty queues, real PL080 transfers, batched-time underrun prevention,
snapshot migration, rate conversion and permission-off silence. They do **not**
prove audible system/media playback. Physical Voice Memos recording evidence
is below; headphones/route changes and guest volume/ringer behavior remain
release gates. Baseband telephone audio and analogue effects are not modelled.
The serial path currently accepts the stock 16-bit stereo controller mode;
other controller word-length encodings remain unimplemented, rather than being
inferred from the different WM8991 register encoding. Application media formats
are converted to the hardware stream by the guest audio stack.

Register references: the authenticated 7E18 controller and N82 device tree,
[openiBoot hardware definitions](https://github.com/iDroid-Project/openiBoot/blob/master/plat-s5l8900/includes/hardware/i2s.h),
and [Wolfson's Linux codec register definitions](https://github.com/torvalds/linux/blob/master/sound/soc/codecs/wm8990.h).
Host API references: [Apple audio input](https://developer.apple.com/documentation/avfaudio/avaudioengine/inputnode)
and [microphone privacy](https://developer.apple.com/library/archive/documentation/Audio/Conceptual/AudioSessionProgrammingGuide/RequestingPermission/RequestingPermission.html).
No reference implementation source is vendored into this feature.

### Candidate checkpoint

Code `2279d75` on `feature/audio-disk-presets`: local Windows **81/81 tests
passed**. [iOS build and iOS 13 API check](https://github.com/j0shua-SYSON/S5LBox/actions/runs/37954602556)
passed. Transport IPA SHA-256:
`fee25b33c6a041d0d3e4158bf22e959c39b2c40b357b08f9cac4ef92e39efc42`.
The artifact is an ad-hoc-signed lab transport, not a stock-install signing claim.

The candidate was installed after the phone was unlocked. A new disposable
4 GiB machine (`audio-disk-4g-test`) booted to SpringBoard and saved/reopened.
The host engine starts on iOS 15.8.5, and opt-in microphone permission enables
the input route. Voice Memos, however, stalled before starting recording:
the captured state had GPIO4 enable `08000040`, zero pending bits, I2S0
TXCON `01100301`, both transfer commands stopped, and zero PCM frames.
The exact driver (`c05a3948..c05a3a04`, handler `c05a3c2c`) waits for two
clock interrupts on DT GPIO line 134 before starting DMA. The original candidate
never supplied these edges. The follow-up clocks LRCLK independently of serial
transfer enable, routes its edge through GPIO4/VIC2 and declares the WFI deadline.
Regression coverage includes both pre-DMA edges, acknowledgement, masking and
no invented PCM while serial transfer is stopped.

`e1604ac` passed 81/81 local tests and the iOS build. Installed on the phone,
a cold-booted disposable 8 GiB guest now records in Voice Memos. Its saved clip
decodes successfully as 44.1 kHz mono ALAC, 25.727710 seconds / 1,134,002 samples,
with nonzero signal (range -286..341, RMS -55.17 dBFS). This proves microphone
data reached an actual guest recording, not that every sample is nonzero or
that playback was heard. Guest replay progress advances. A post-replay snapshot
contains 3,860,108 TX and 2,286,186 RX frames with zero core FIFO xruns;
live speaker gain/routing and host audible playback remain under validation.

A snapshot paused during replay exposed a second independent bug: TXCOM `6`,
4,008,435 TX frames and an enabled/unmuted speaker route, but both output gains
were zero. R38 (`0x26`) had never been written. The initial register-only model
returned zero for all unwritten storage, while both 7E18's uint16 defaults table
at `c0691030 + 0x4c` and [WM8991 Rev 4.0, Table 37](https://www.mouser.com/datasheet/2/76/WM8991-473496.pdf)
specify `0x79` (0 dB). The follow-up supplies that default through the codec's
effective read path, including old snapshots. Explicit writes of zero still
mute. Bus reads, PCM output, attenuation and saved-state regression tests cover
the distinction; no guessed volume boost or application-specific patch is used.

`6eb0652` passed 81/81 local tests (250 I2S, 542 codec and 64,016 host-handoff
checks in the focused executables) and the [iOS build / iOS 13 API check](https://github.com/j0shua-SYSON/S5LBox/actions/runs/37959714684).
It was installed in-place on the unlocked iPhone. IPA SHA-256:
`6bf567d1a52ea6a529dd56c6416d7ec774f651df4134fad878495b15bf97c63f`.
Loading the actual replay snapshot with this core changes the effective speaker
gain from zero to unity. The original Voice Memos replay capture still contained
zero-valued queued PCM; that observation was not an audible-playback pass.

A subsequent physical-device isolation test on the same build installed an
authored ARMv6 AudioQueue app in `audio-disk-8g-test` over virtual USB (AppSync
for OS 3.1 was installed in that disposable guest). A continuous 44.1 kHz,
16-bit stereo triangle wave exercised the real guest audio stack. The guest
reported successful queue operations, repeated buffer callbacks and hardware
volume 0.50. A **live** read of the host playback queue found 8,192/8,192 nonzero
float samples, peak 0.09822945, 3,425,280 rendered frames and zero queue drops.
The matching guest snapshot had 1,369,113 TX frames, zero core FIFO xruns,
unity codec gains and nonzero samples throughout the DMA ring. Save/reopen
resumed the fixture and its callback count continued. This establishes nonzero
guest PCM through DMA, codec gain and host handoff, not an independently heard
speaker result, uninterrupted timing or general media compatibility.

Voice Memos was a separate failing case at this checkpoint: replay reached 0:08 while
the live host queue still held zeros. Switching its speaker route did not
establish nonzero output. The fixture then compared that same recording with a
host-decoded PCM WAV through the guest's `AVAudioPlayer`. WAV preparation and
playback succeeded, the position advanced and guest metering was nonzero. The
live host queue contained 8,186/8,192 nonzero float samples, peak 0.0019651793.
Opening the original ALAC succeeded, but `prepareToPlay` returned false. The
fixture's displayed -3 is its own failure sentinel, **not** an OSStatus.
An explicit AudioQueue comparison then decoded the original ALAC using
`UseSoftwareOnly`: 8,186/8,192 live host samples were nonzero, peak 0.0026305886,
with zero host queue drops. A fresh-launch default-policy test started without
an API error but stalled at six buffer callbacks and produced all-zero host
samples. Its snapshot still showed the supported 16-bit serial mode, unity
gain, zero core FIFO xruns and an already-silent DMA ring. This puts the
difference upstream of PCM transport; successful queue startup alone was not
a decoder pass.

The private 7E18 fixture obtained the underlying `prepareToPlayQueue` status
instead of the public BOOL wrapper: the original memo, a host-generated ALAC
control and a host-generated AAC control all returned `0x6e6f7065` (`nope`).
WAV still played. This is broader than one recording's container metadata.
The exact firmware contains both software and transformer-backed decoders,
and its AMC driver binds `arm-io/amc`, an unimplemented accelerator. A
temporary **RAM-only** device-tree unmatch was verified before a test cold
boot. That experiment did **not** pass: preparation returned -308 and later
buffer allocation returned 22 even for WAV. It is not a shipping fix or proof
that hiding AMC alone enables fallback. Repeated reset boots also logged
filesystem I/O errors. Normal shutdown of that experimental boot completed;
a subsequent clean, unmodified boot restored working WAV playback, failing
default ALAC preparation (`nope`), and working software-policy ALAC queues.
This control does not establish lasting filesystem damage or make the AMC-hide
experiment a usable fallback. Firmware files and the emulator's shipping policy
were not changed.

The next private fixture selected `UseSoftwareOnly` **before** AVAudioPlayer's
first preparation: it invoked the exact 7E18 queue-allocation helper, set the
public AudioQueue codec policy, then called the original preparation helper.
The original memo, generated ALAC and generated AAC all prepared successfully
and their playback positions advanced. The original memo produced 8,186/8,192
nonzero live host samples (peak 0.0019074708); AAC produced 8,192/8,192 (peak
0.12641458). Both captures reported zero host queue drops. This isolates a
working early software-decoder route, but **only in the private fixture**:
ordinary Voice Memos and other apps had not acquired that policy automatically.
No framework patch or emulator-wide fallback was shipped by this test.

A narrower RAM-only experiment made the exact AudioCodecs
`ACTransformerManager::HardwareIsAvailable` leaf return false. Its 48-byte
identity was checked and the two changed instructions survived save/reopen,
which also discarded host instruction caches. This did not establish working
Voice Memos replay: the position stayed at zero and the host queue remained
silent. Merely denying that availability query is not a demonstrated general
fallback. Normal shutdown completed and the next launch used a fresh boot;
the temporary alteration was never applied to a firmware or rootfs file.
Do not describe the entire transport as silent or full media playback as fixed.
Private fixtures and captures live under
`work/audio-validation`, not in the shipping app.

The next RAM-only experiment changed the default selector in the authenticated
7E18 `AudioQueueObject::ChooseCodec`: the branch at `3354ced8` goes to its existing
software-only path (`3354cf34`), leaving explicit policy switch entries intact.
The 1 KiB window at `3354cc00` had the same SHA-256 in the cache file and captured
guest RAM. After save/reopen discarded host instruction caches, the fixture's
**default** AudioQueue progressed beyond the old six-callback stall and produced
8,178/8,192 nonzero host samples (peak 0.0022842598). More importantly, unmodified
**Voice Memos** replay advanced from 1 to 11 seconds and produced 8,184/8,192
nonzero host samples (peak 0.0022399065, 4,033,536 rendered frames, zero queue
drops). Host volume was zero: this is decoded-PCM handoff proof, not an
independently heard speaker or uninterrupted-playback claim.

The candidate implementation is `VMFirmwareAudio.c`, enabled only after the
app's exact 7E18 boot/restore gate. On a successful user-mode instruction-fetch
page walk, it authenticates the complete 1 KiB code window and publishes just
that branch through the normal RAM-write path. Unknown code, other addresses,
ARMv7, privileged fetches and out-of-RAM mappings are refused. No framework,
firmware or rootfs **file** is patched. This is an explicit software-codec
compatibility policy, **not AMC hardware emulation**; decoding still runs inside
the guest. Explicit hardware-only requests are not made to succeed.

The optional bus callback runs before a FETCH TLB entry is published, not on
instruction-cache/TLB hits or every instruction. It does not install the
pre-step hook that disables native fast paths. Its context belongs to the live
machine, not the snapshot stream; the saved-state format is unchanged. Tests
cover fault/data/MMU-off refusal, normal and native fetch visibility, snapshot
wiring, full-digest refusal and optional private-cache execution of all five
policy switch targets. Local Windows validation passed **82/82 CTest cases**
and all **20 private-cache checks**. The signed ARM64 refill's direct structure
offsets were updated with the new bus layout; portable layout guards and
capability revocation tests cover that integration too.

`a8112a1` passed the [iPhone build and iOS 13 API check](https://github.com/j0shua-SYSON/S5LBox/actions/runs/38045419012)
and was installed on the same physical iPhone 6s Plus / iOS 15.8.5. IPA SHA-256:
`b25685f557aa79fdf6b1fac6ae11ec4d3e745a1b3ef5a2214a62526acc0bfd5c`.
The disposable 8 GiB machine had shut down normally; the new app explicitly
reported a **fresh boot after powered-off checkpoint**, not snapshot restore.
No debugger writes were performed on this boot. Before starting Voice Memos,
the fresh host queue held zero samples. Ordinary Voice Memos then replayed the
retained 26-second recording: the displayed position reached 0:05, and the
live queue capture held 8,186/8,192 nonzero samples, peak 0.0014645124,
6,032,896 rendered frames and zero queue drops. Microphone input was off.
This passes the integrated cold-boot decoded-playback gate, not speaker
audibility, timing continuity or route-change behavior. Read-only debugger
captures briefly stop execution and cannot certify uninterrupted playback.

A live-installed private fixture then used **only public AVAudioPlayer APIs**,
with no private helper or codec-policy override. The original ALAC memo,
generated ALAC control, generated AAC control and decoded WAV all prepared,
played and advanced their positions. Host captures respectively contained
8,186 / 8,192 / 8,192 / 8,180 nonzero float samples, with peaks 0.0030347290 /
0.12243547 / 0.12703037 / 0.0023194123. All reported zero playback queue drops.
The same AAC playback was paused/resumed and backgrounded/foregrounded through
the app UI: both stop cases disabled guest/host audio and drained pending output,
and both return cases resumed fresh nonzero samples with advancing write counts.
Enabling microphone switched to the duplex host route without losing playback;
input samples arrived. This playback-only guest did not consume microphone data,
so its bounded input ring filled and correctly counted discarded input. Turning
microphone off drained that ring and cleared the permission-to-feed flag.
These are bounded live-path checks, not an underrun-free or acoustic-quality
claim, nor a physical headphones/Bluetooth test.

Save/close/reopen during a default-policy ALAC AudioQueue also passed on this
build. The restored guest's callback count continued to 23; the new host buffer
contained 8,180 nonzero samples (peak 0.0019496002, zero playback drops), while
its microphone ring was empty and zeroed. In a separate constant ALAC tone
test, guest Volume Down produced all-zero queued PCM and Volume Up restored
8,192 nonzero samples (peak 0.12243582). Host volume stayed at zero for these
checks; this verifies guest digital-volume behavior, not ringer-switch policy.

A short acoustic loop check temporarily set host volume to 20%, enabled the
host microphone during the 441 Hz ALAC tone, captured the bounded input ring,
then repeated at zero volume. The output ring's dominant band was correctly
near 440 Hz, but the 85 ms microphone windows were dominated by lower-frequency
transients. This is **inconclusive** for speaker audibility/quality, not a new
playback failure or a listening pass. Host volume was restored to zero and
microphone access switched off. No full-room recording is retained.

Latest code checkpoint `e6398ce` passed the complete
[cross-platform core/JIT/static-ARM64/sanitizer suite](https://github.com/j0shua-SYSON/S5LBox/actions/runs/38045620385)
and [iPhone build / iOS 13 API check](https://github.com/j0shua-SYSON/S5LBox/actions/runs/38045620397).
Its post-`a8112a1` changes only repair portable test declarations and reserve
8 MiB for the MSVC stress-test executable's nested full CPU copies; app/runtime
source is identical to the physically tested build. Local CTest passed 82/82
and the exact private audio-cache test passed all 20 checks. These validations
do not certify runtime behavior on every supported host iOS version.

Earlier, a disposable desktop probe restored the older USB checkpoint and
executed another 150 million instructions with three board-level power presses.
It produced **zero PCM**, and its final display capture failed (no active RGB
window). That attempt is not an audio pass and does not identify an audio cause.
Logs are under project-local `work/audio-validation/lock-sound-02.*.log`.

Next device checks: listening/continuity, system click/lock sounds and ringer
switch; denied microphone permission; external headphone/Bluetooth routes.
Investigate the intermittent guest wake delay after live IPA installation if
it reproduces: a later short Home tap woke it and it then unlocked, but the
earlier long-press/black-screen sequence has no established cause or fix.
Keep main unchanged until
those tests and the large-disk gates in `DISK-SIZE-PRESETS.md` pass.

## Historical register bring-up

<!--
  Extracted from the retired docs/AGENT_HANDOFF.md, section 23.9, on
  2026-07-31, unchanged
  apart from heading depth. It was the newest material in that file and had
  never been committed: 224 of the 235 uncommitted lines in the handoff were
  this section. It is the only record of the audio work anywhere in the tree,
  which is why it was lifted out before the handoff was retired rather than
  after.
-->

# Audio: the codec answers and the I²S windows are decoded

**Status: landed.** `AppleWM8991Audio: I2C register read failed (0): device
error` is gone. The codec is modelled in `core/src/soc/wm8991.c` as an I²C slave
at `0x1B` on i2c0, both I²S windows are decoded device models in
`core/src/soc/i2s.c`, and the two landed together for the reason the warning
below gives. `SNAPSHOT_VERSION` went 10 -> 11.

### The proof, from run87

Absence of an error line is not evidence that a probe succeeded — it is equally
consistent with the driver never running. So run87 armed
`--call-probe-kernel` on the four branches of `AppleWM8991Audio::probe`
(`0xc068b078`, codec vtable slot `+0x178`) and read the registers directly:

```
=== CALL PROBE: CONFIGURED PCs (4) ===
    pc 0xc068b0bc  kernel  captured 1             wrong-mode 0
    pc 0xc068b0f8  kernel  captured 0             wrong-mode 0
    pc 0xc068b118  kernel  captured 1             wrong-mode 0
    pc 0xc068b0ec  kernel  captured 1             wrong-mode 0

    @180963570    pc c068b0bc lr c068b218 ...  r0 00008990 ... r3 00008990
    @180974355    pc c068b0ec lr c068b218 ...  r0 00000000 ...
    @180974358    pc c068b118 lr c068b218 ...  r0 00000000 ...
```

Read that against the disassembly. `0xc068b0bc` is the instruction after
`bne 0xc068b118`, so it executes **only when `readCodecRegister(0)` equalled the
literal**: the capture shows `r0 = 0x00008990` against `r3 = 0x00008990`, which
is the emulated codec's answer arriving over the emulated bus and matching.
`0xc068b0ec` is `ands r0, r0, #0x20` on the value read back from register 1;
`r0 = 0` means bit 5 did **not** stick. `0xc068b0f8` is the first instruction of
the WM1817 branch and captured **zero**. Both runs report the failure line zero
times where run62 reported it once:

```
run62: 1     run86: 0     run87: 0
```

The console around the old failure now reads straight through:

```
Jettisoning kext bootstrap segment.
AppleS5L8900XSDIO: registers @ vaddr 0xe9205000, paddr 0x38d00000
* memMapEntries 6
+ AppleMPVDDriver[0xdc877800]::setPowerStateGated()
ApplePCF50635PMU::start: reading DOWN converter voltages
```

Both runs are 300e6 cold boots with the stock arguments
(`-d devicetree.bin -r rootfs.img -R 512`); the probe fires at instruction
180,963,570.

### The correction this section owed you: R1 bit 5 is inverted

The previous text said "**R1 bit 5 must be writable and read back**". That is
wrong, and building to it would have produced a part the driver calls by the
wrong name. The bit-5 sequence is **not a second gate** — it is a variant
discriminator, and both of its branches return the same success value:

```
c068b0ec  ands   r0, r0, #0x20        ; bit 5 of register 1
c068b0f0  strbeq r0, [r4, #0xc0]      ; clear  -> this->flag = 0
c068b0f4  beq    #0xc068b118          ; clear  -> return, still successful
c068b0f8  mov    r1, #1               ; set    -> this->flag = 1
```

and the getter at `0xc068b044` turns that flag into a name — `moveq` selects
`0xc0690158` `"WM8991"` when the flag is **0**, leaving `0xc0690150` `"WM1817"`
when it is 1. **A part whose R1 bit 5 sticks is reported as a WM1817.** The
shipped device tree calls both of its audio nodes `wm8991`
(`audio-control,wm8991` on i2c0, `audio-data,wm8991` on i2s0) and contains no
occurrence of `1817` anywhere in its 40,544 bytes, so on this board the bit must
read back **clear**. The model leaves it unimplemented: writes are discarded,
reads return zero. This is not cosmetic — the flag is loaded from `this+0xc0` at
ten further sites in the kext.

Register 0 is the **only** hard gate, and it was verified three ways: the word
at `0xc068b124` is `0x00008990` read at the byte level; `0xc068b0ac` is the sole
instruction in the entire kernelcache that loads that literal; and entry 0 of
the driver's own default table at `0xc0691030` is the same `0x8990`.

### The wire protocol, from the helpers rather than a datasheet

`AppleWM8991Audio` inherits its transfer helpers from `AppleEmbeddedAudio`
(`0xc053e000..0xc054c000`, ARM) and reaches them through its vtable
(`V = 0xc0690b20`, base `V = 0xc054867c`; the two tables are structurally
aligned and three separate slots — `+0x178`, `+0x39c`, `+0x3a4` — are
superclass calls at the same offset, which is what pins the bases).

- **read**, `0xc053ff94`: one index byte out, then **two bytes in, MSB first**.
  The assembling instruction is the claim: `ldrb r3,[sp,#0xe]; ldrb r0,[sp,#0xf];
  orr r0,r0,r3,lsl #8` at `0xc0540030..38`. On failure this is the helper that
  prints `%s: I2C register read failed (%#x): %s` (`0xc0548048`) — the exact
  format behind the old `(0): device error`, `%#x` of zero rendering bare.
- **write**, `0xc0540050`: index byte, then the value MSB-first — three bytes.
  This is the form the codec uses (`writeCodecRegister` `0xc068b168` dispatches
  to it).
- A second, **packed** form exists at `0xc0540108` — `(reg << 1) | value[8]`,
  then `value[7:0]`, two bytes — but it belongs to the WM8758 sibling driver.

The model implements both and lets the **byte count** select: one byte is a
pointer-only write (the stock controller's read setup, which must not be
committed as a store), two is packed, three is wide, anything else is NAKed and
counted. The counts are distinct, so nothing guesses.

**Only six registers ever reach the bus.** `readCodecRegister` (`0xc068b1b4`)
gates every read on the bitmap `0x0084000f` at `0xc068b21c` via
`ands r3, r2, r3, lsl r1` over `1 << reg`; set bits {0,1,2,3,18,23} mean
registers **`0x00, 0x01, 0x02, 0x03, 0x12, 0x17`** and nothing else. Every other
read is served from a RAM shadow the driver seeds from `0xc0691030`.

### The one thing that is inferred rather than observed

The codec's GPIO configure path holds a poll with **no timeout, no iteration cap
and no delay in its body**, at `0xc068d4ac..0xc068d514`:

```
rmw(0x17, 0x1000, level << 12);           // c068d44c, before the loop
do { write(0x17, computed);               // c068d4cc
     write(0x12, last & 0x0000efff);      // c068d4e8 — clears bit 12
     v = read(0x12); }                    // c068d4fc
while ((v & ~arg & 0x1000) != (level << 12));   // c068d50c
```

It forces bit 12 of `0x12` clear on every write and then waits for that same bit
to read back as `level`. So it cannot be waiting for storage: on a part where
`0x12` bit 12 held what was last written, `level == 1` never terminates. The
model therefore makes **`0x12` bit 12 a read-only mirror of `0x17` bit 12** and
nothing more. That is an inference from the poll's own structure, deliberately
the narrowest one that lets a loop with no exit but the device finish; it is
counted (`status_mirror_reads`) so a boot can say how often it mattered.

### The I²S side: both prior claims verified, independently

`readRegister` (`0xc05a3c84`) really **has no caller anywhere in the
kernelcache**. Four checks agree: its address occurs as an aligned word exactly
once in the file (its own vtable slot `0xc05ad888`); no ARM or Thumb BL/BLX
targets it; no `ldr pc,[Rn,#0x3c0]` dispatch exists with any base but PC; and
slot `+0x3c0` is a virtual the class *introduces* — its parent
`AppleARMIISController`'s vtable ends at `+0x3ac` — so no other kext can reach
it through a base pointer even in principle. The class contains exactly **one**
MMIO load instruction in the whole image and it is dead code.

`writeRegister` writes exactly the seven claimed offsets, enumerated from every
dispatch site: `configure()` `0xc05a3820` writes `+0x00` (cfg|1), `+0x40`,
`+0x04` (cfg|1), `+0x30`, `+0x08` (0), `+0x34` (0), `+0x3c` (1, if TX);
`startTransfer()` `0xc05a3928` writes `+0x08` and `+0x34` with **6**; `stop()`
`0xc05a3ad0` writes both with **0**. Distinct set = `{0x00, 0x04, 0x08, 0x30,
0x34, 0x3c, 0x40}`.

Two further facts worth carrying. **`start()` cannot spin**: `0xc05a3d24..
0xc05a3f58` performs zero MMIO and contains zero backward branches. And the
class is **ARM** while its parent `AppleARMPlatform` is Thumb — its vtable's own
class entries are all even where the inherited ones are odd. The transfer wait
at `0xc05a39a0` is `commandSleep(..., 1)`, i.e. `THREAD_ABORTSAFE` **and
timer-bounded**, and depends on the GPIO-IC interrupt (`0x86` for i2s0, `0xaa`
for i2s1) or the timeout — never on a register value. The five unbounded waits
§23.9 warned about are one layer up, in `AppleARMIISAudio`/`AppleEmbeddedAudio`
(`IOLockSleep` at `0xc053a5f8`, `0xc053a644`, `0xc05429d4`, `0xc0542a74`).

The windows are `0x3CA00000` and `0x3CD00000`, both 0x1000. i2s0 carries the
codec (`/arm-io/i2s0/audio0`, `audio-data,wm8991`); i2s1 carries the baseband
voice path (`audio-data,baseband`). They are still recording **zero** MMIO
traffic after this change — they do not appear in run86's 15-page census — which
is exactly what the dead-`readRegister` analysis predicts, since nothing writes
them until a transfer is actually configured.

### What was deliberately NOT modelled

- **The 63-entry default register table** at `0xc0691030`. It is the driver's
  own belief about an untouched part and the driver never needs the device to
  supply it; copying it in would be the full register map this model refuses to
  invent. Its entry 0 agreeing with the identity literal is recorded above as
  corroboration, not as a reason to ship the other 62.
- **Any semantics for the seven I²S offsets.** The `+0x08`/`+0x34` pattern
  (0 configured, 6 running, 0 stopped) is consistent with a per-direction
  enable and bit 0 is forced set at `+0x00`/`+0x04`, but "consistent with" is
  not "established". The model stores; it does not interpret.
- **Any interrupt line.** There is no `S5L8900_IRQ_I2S0/1` constant, for the
  same reason there is no `S5L8900_IRQ_UART4`: the `interrupts` properties say
  `0x86`/`0xaa` but both nodes name `interrupt-parent = /arm-io/gpio`, so they
  are GPIO-IC lines, and nothing in this model raises either.
- **The other i2c0 nodes** — accelerometer `0x1d`, ALS `0x44`, tethered `0x29`.
  Nothing establishes what they must answer, and an address that NAKs is a
  driver that fails cleanly, which is what all three do today.
- **The PL080, and any host audio sink.** Samples still need the DMAC; host
  playback needs a portable sink that must never block the CPU thread, and
  `core/` has no threading vocabulary. Both are separate work.

### Tests and mutants

`core/tests/test_wm8991.c` — 11 cases, 493 checks, registered as
`wm8991_codec_and_i2s`. Suite 34 -> 35 (36 with the buttons work that landed
alongside). The codec is driven through the **real** I²C controller using the
stock register sequence, so a change to `i2c.c` that broke it fails here rather
than only in a six-minute boot.

**14 mutants, 14 killed, 0 survived.** Two of them are worth carrying:

- *the mutant that survived first.* Removing the seven-bit wrap from the read
  auto-increment changed nothing, because a second mask at the point of use made
  it redundant — and that redundancy was hiding a real bug: a pointer-only write
  of index byte `0xff` stored `ptr = 0xff`, which the snapshot invariant
  (`ptr < 0x80`) then rejected. A guest-reachable byte made the machine unable to
  checkpoint. Narrowing at the point of *store* fixed the bug and made the
  mutant killable.
- *the mutant the harness refused to score.* Changing `WM8991_I2C_ADDR` from
  `0x1b` to `0x1a` survived, because every test used the symbol. Constants need
  a test that spells out the literal and cites where in the firmware it comes
  from; `test_constants_match_the_shipped_firmware` is that test.

The harness (a) rebuilds and requires the mutant to **compile** before believing
any result, and (b) diffs the file and reports `INVALID` rather than "survived"
when an edit does not apply. It caught one genuine no-op edit this way.

### Tooling note

`tools/kdisasm.py`, `dtwalk.py`, `findstr.py`, `vtscan.py` and `kcensus.py` all
hardcode `REPO = F:\JOSHUA_1st_2021\projects\S5LBox`, which does not exist on
this machine — they fail immediately. `machosyms.py` and `findcalls.py` take a
path argument and work. `kdisasm.py`'s `SEGS` table itself is correct for this
image, verified against the file's own `LC_SEGMENT` commands.

**run62 settled the question that branched the plan.** `--call-probe-kernel` on
`AppleS5L8900XI2SController::start`'s tail: `0xc05a3f40` (publishChildren) and
`0xc05a3f44` (return true) each **captured 2**, `0xc05a3f4c` (return false)
captured **0**. Both controllers publish, at instructions 235,126,205 and
236,694,444. So the `AppleARMIISDevice` nub named `audio0` **exists today**, the
two `IODMAEventSource`s are created successfully, and the three NULL-timeout
`waitForService` calls ahead of it already resolve.

Consequence: **the PL080 can be deferred.** Steps 1-2 — the codec slave plus the
I²S window — are 2-3 days and produce a fully attached codec with a live
`IOAudio2Family`. Samples need the PL080 later, and there is **no boot-argument
escape** for it: all 40 `PE_parse_boot_argn` sites were enumerated and the
`<node>_dma_enable` pattern exists only in `AppleS5L8900XSerial`.

The I²S window itself is nearly free. The driver funnels access through two
accessors and **`readRegister` (`c05a3c84`) has no caller anywhere in the
kernelcache** — it writes seven offsets (`0x00, 0x04, 0x08, 0x30, 0x34, 0x3C,
0x40`) and never reads. The FIFOs at `+0x10`/`+0x38` are touched only by the
PL080 as physical addresses in an LLI. That independently explains the zero
MMIO traffic the census shows on `0x3CA00000`/`0x3CD00000`, and makes the window
80-120 lines rather than 250-350.

> **Do not land the codec alone.** Today's failure is loud, correct and
> harmless, and the boot continues past it. There are **five unbounded waits**
> on the audio path — three NULL-timeout `waitForService` in series plus two
> uninterruptible `IOLockSleep`s in the transfer path — so a codec that answers
> `0x8990` with no I²S window behind it converts a good failure into a **silent,
> uninterruptible hang**. That is a regression, not progress. Steps 1 and 2 are
> one unit.

Two things to keep honest about scope. **Nothing makes a sound today** and
nothing would even with a perfect stack: the activation screen is silent and
iPhone OS 3 has no boot chime, so audio output is downstream of touch exactly as
Wi-Fi is. And **you cannot hear it live** — at 200-294× slower than the 412 MHz
part, the smallest UI sound costs about 6 seconds of wall clock and one guest
second costs about 200. The right design is to clock the model off the guest
timebase, capture PCM to a WAV, and let the host play it back afterwards.

One method note worth carrying. Before run62, the evidence for the nub was that
the device tree declares 14 DMA channels while only 10 initialise, a shortfall
of exactly 4 matching i2s0's 2 plus i2s1's 2. That fit was suggestive and
**wrong** — other partitions exist, the analysis labelled it cannot-determine
rather than concluding from the coincidence, and a four-minute read-only run
settled it properly. The tidy arithmetic would have sent the plan down the
expensive branch.

# 23.8 The trap that has now cost three retractions

A per-process trace block that reports one generation does not describe the
whole run, and **host-side counters are not machine state**: `core/include/snapshot.h`
says a restored process starts them fresh, so a zero in a restored run means
"not seen since the restore point", never "never happened". On 2026-07-26 that
misreading produced a committed claim that `AppleH1CLCD::createSurface` never
ran, when it runs and succeeds at instruction ~238,400,000.

**Compare the heartbeat pc stream before reading divergence into differing
counters.** Restore is bit-exact — 20/20 heartbeats identical between cold run52
and restored run56 across 3.0e9-4.9e9, agreeing 862 million instructions past
the restore point, across three different binaries.
