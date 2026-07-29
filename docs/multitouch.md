# Multi-touch: the chain from a finger to a UIKit event

Everything below is measured — from `firmware/kernel.macho`, from the armv6
dyld shared cache at `work/analysis/dsc_armv6`, and from raw guest RAM in
`work/run96-base/snap-3.5e9.bin`. Where something is inferred it says so.

The short version: **touch does not work because the multitouch controller is
never programmed**, and everything else that looks broken is downstream of that
one fact.

---

## 1. The chain, end to end

```
  MTZ2 controller (spi1 cs0)          core/src/soc/mtz2.c
        |  reports 0xD1 0xD3 0xD9 0xD0 over control reads
  AppleMultitouchZ2SPI                kernel, 0xc0438xxx
        |  setProperty into its IORegistry entry
  MultitouchSupport.framework         userspace, base 0x33cf6000
        |  _mt_CachePropertiesForDevice caches them at device+0x10..0x2c
        |  _mt_DefineSurfaceGrid builds the surface grid at device+0x168
        |  _alg_InitRowColXYConvert derives the four normalisation bounds
        |  _mt_FillMTContactDirectFromBinary normalises each contact
  MultitouchHID.plugin                base 0x007D7000
        |  IOHIDEventSystem -> SpringBoard -> GSSendEvent
  __UIApplicationHandleEvent          0x324f6edc
```

## 2. The device's reports, and what the kernel does with each

| report | kernel publisher | property | payload |
|---|---|---|---|
| `0xD1` | `0xc0438670` | `Family ID` | `[0]` |
| `0xD3` | `0xc04386e0` | `Endianness`, `Sensor Rows`, `Sensor Columns`, `bcdVersion` | `[0]`, `[1]`, `[2]`, `[3..4]` big-endian |
| `0xD9` | `0xc04388dc` | `Sensor Surface Width` / `Height` | two unaligned LE32 |
| `0xD0` | `0xc043880c` | `Sensor Region Descriptor` | opaque blob |
| `0xA1` | `0xc0438874` | `Sensor Region Param` | opaque blob |

All five are vtable slots `+0x3a8`, `+0x3ac`, `+0x3b0`, `+0x3b4`, `+0x3b8` of
the vtable at **`0xc0449f40`**, and all five are called from one gated chain in
`publishProperties` at **`0xc043b11c`** — each call must return non-zero or the
chain bails and *nothing* is published. The chain itself is preceded by two
gates: `+0x3a0` (publishes `Multitouch ID`) and `+0x3a4`
(`GET_DEVICE_INFO`, opcode `0xE2`, "Getting device info (stage 1/2)").

`GET_DEVICE_INFO` validates only two things — `rx[0] == 0xE2` and a correct
sum16 over `rx[0..13]` at `rx[14..15]` — both of which `mtz2.c` already
satisfies. It is not the blocker.

## 3. How userspace turns those bytes into pixel coordinates

`_mt_CachePropertiesForDevice` (`0x33cf7cbc`) caches:

| property | offset | on failure |
|---|---|---|
| `Multitouch ID` | `device+0x10` (64-bit) | zeroed |
| `Family ID` | `device+0x18` | zeroed |
| `bcdVersion` | `device+0x1c` | zeroed |
| **`Sensor Rows`** | `device+0x20` | zeroed |
| **`Sensor Columns`** | `device+0x24` | zeroed |
| `Sensor Surface Width` | `device+0x28` | **5000** (`0x33cf8098`) |
| `Sensor Surface Height` | `device+0x2c` | **7500** (`0x33cf80a0`) |
| `Endianness` | `device+0x30` | 1 |

It is a plain `IORegistryEntryCreateCFProperty` on `device+0x08`, so a NULL
return means the key is genuinely absent.

`_mt_InitializeAlgorithmsForDevice` (`0x33cf80ec`) then passes
`device->0x20` and `device->0x24` as the rows/cols arguments to
`_mt_DefineSurfaceGrid` (`0x33d01d70`), along with the parsed descriptor array
at `device+0x74` (16 records of **7 bytes**, count hardwired at `0x33cf7ef8`).

**The back-fill.** At `0x33d01dd0`, if `blob[7] == 0` — that is, if the second
seven-byte record's first octet is zero — `_mt_DefineSurfaceGrid` writes the
record itself: `desc[0]=1, desc[3]=1, desc[2]=(byte)rows, desc[5]=(byte)cols`.
So a device that supplies no descriptor gets one synthesised from the two
properties.

`_alg_InitRowColXYConvert` (`0x33d01010`) builds two 66-entry tables **indexed
from −1** (a real entry; both loops start `mvn r5,#0`) and derives:

```
Xmax = grid[0x26] + colTable[desc[5] - 1]      0x33d01124
Xmin = colTable[0] - grid[0x24]                0x33d01130
Ymax = grid[0x2a] + rowTable[desc[2] - 1]      0x33d01104
Ymin = rowTable[0] - grid[0x28]                0x33d01110

colTable[i] = (i - grid[0x3c]) * grid[0x34]*100 / grid[0x38]
rowTable[i] =  i               * grid[0x2c]*100 / grid[0x30]
```

with `0x33d01154` setting 56/11, 36/7 and all four margins to 75, and leaving
`grid[0x3c]` zero. So the tables are `i*5600/11` and `i*3600/7`, and
**`desc[2]` is the row count and `desc[5]` the column count**.

`grid = device + 0x168`. The four bounds live at `grid+0x148` (Xmax),
`+0x14a` (Xmin), `+0x14c` (Ymax), `+0x14e` (Ymin).

## 4. What a live guest actually holds

`work/run96-base/snap-3.5e9.bin`, at the lock screen. The grid was located by
searching for the two constant tables — they appear exactly once, 0x84 bytes
apart, which is exactly `0xc4 - 0x40`. That fixes `device` at VA **`0x007e9000`**.

```
device+0x18 Family ID             0        (model publishes 1)
device+0x1c bcdVersion            0        (model publishes 0x0100)
device+0x20 Sensor Rows           0        (model publishes 15)
device+0x24 Sensor Columns        0        (model publishes 10)
device+0x28 Sensor Surface Width  5000     (model publishes 4800 — this is the FALLBACK)
device+0x2c Sensor Surface Height 7500     (model publishes 7200 — the FALLBACK)

desc (= blob+7)   01 00 00 01 00 00 00     <- the back-fill's fingerprint,
                                              run with rows=0 and cols=0

grid+0x148 Xmax  -434     grid+0x14a Xmin  -75
grid+0x14c Ymax  -439     grid+0x14e Ymin  -75
grid+0x150 -509 = colTable[-1]   grid+0x154 -514 = rowTable[-1]
```

Both spans are **negative**, so `_alg_ClipPosToScreenEdge` (`0x33d00f2c`)
returns the maximum for every coordinate on every frame. That is the pinned
normalised 1.0 run98 measured at `plugin+0x2114`, and it is why no step size
ever helped: the contact never moves in pixel space at all.

5000 and 7500 are not a rounding of 4800 and 7200 — they are
MultitouchSupport's own literals for a property it could not read. **Every
int32 property is missing.**

## 5. Why they are missing: the HBPP gate

`isInHBPP()` is `0xc0441008`, reached only through vtable slot `0x4d0`. Two
callers want opposite answers:

* `finishStarting()` `0xc0442670` — **DETACHES** on FALSE.
* `attemptToBootloadDevice()` `0xc04414c4` — on TRUE logs "attempting to
  bootload device" and pushes ~54 KB of firmware; on FALSE logs "not in HBPP,
  so skipping bootload" and **returns 0**, which the retry loop counts as a
  failed attempt.

`core/src/soc/mtz2.c` resolves this with one monotonic bit: TRUE once (so the
driver stays attached), FALSE thereafter (so the bootload is skipped). The file
calls this "a bounded, named, single-bit lie that costs three cosmetic log
lines."

**run100 measured what it actually costs.** Eight kernel probes, three of them
positive controls:

| probe | captures |
|---|---|
| `0xc0442670` `finishStarting` | 1 (control) |
| `0xc0441008` `isInHBPP` | 4 (control) |
| `0xc04414c4` `attemptToBootloadDevice` | 3 (control) |
| `0xc043b11c` `publishProperties` | **0** |
| `0xc04385a8` `getReport` | **0** |
| the three publishers | **0** |

In two billion instructions the driver never issues a single control read.

A Z2 has no flash: iOS downloads its firmware on **every boot**, which is why
`finishStarting` insists on HBPP — at that moment the part really is an
unprogrammed bootloader. Our device claims to be one and then refuses to be
programmed, so it never runs application firmware, and the driver correctly
declines to interrogate a part that has none.

## 6. What has to be built

`attemptToBootloadDevice` returns 1 only after `bootloadDevice()` (vtable
`+0x74` of the bootloader vtable at **`0xc044a494`**, implementation
`0xc0445860`) returns non-zero. That function is five virtual calls in
sequence, each of which must return non-zero:

| slot | address | what its own log calls it |
|---|---|---|
| `+0x8c` | `0xc0444370` | reads `fll-mval`, `cal-dl-addr`, `Firmware`, `Constructed Firmware`, `PreconstructedBootloadPacketType` |
| `+0x9c` | `0xc0444a98` | prox calibration download ("No prox calibration data present") |
| `+0x98` | `0xc0444dec` | calibration download ("Invalid calibration data or version.") |
| `+0x88` | `0xc04455d0` | `MTSPIBootloader_Z2::performCalibSeq()` |
| `+0x60` | `0xc044490c` | "about to execute" |

The firmware itself comes from the kext's own property table
(`Firmware` / `Constructed Firmware`), not from a file — which is why run65 saw
54,156 preconstructed bytes pushed. The transfers go through the ordinary SPI
entry `v[0x368]` (`0xc0444ff8`, `0xc0445224`, `0xc04454d0`), **not** through
DMA: the controller only arms DMA when `this+0xf4` is non-zero
(`0xc05a6c24`), and our 16-byte transfers run in PIO today.

The driver's own narration of the sequence, in address order:

```
  "sending MT_SPI_Z2_WAKE_CMD"                          0xc0445fac
  "Detected Z2 Version: 0x%08X. Writing to registers"   0xc0445654
  "constructing HBPP DATA packet with %d bytes payload" 0xc0444a4c
  "sending preconstructed firmware bytes"               0xc0445164
  "sending unconstructed firmware bytes"                0xc0445330
  "sending calibration bytes"                           0xc0444ea8
  "sending prox calibration bytes"                      0xc0444b4c
  "%s: status: 0x%08X sending DATA packet."             (x4)
  "requesting calibration" / "Waiting for calibration to end"
  "about to execute"                                    0xc0444988
```

### 6.1 The HBPP DATA packet, exactly

Built by `0xc0445dcc` (the function at `0xc0444a20` is only the wrapper that
logs "constructing HBPP DATA packet with %d bytes payload"). Arguments are
`(out, address, words, nbytes)`, and the return is `nbytes`.

```
  [0]           0x30                       the DATA marker
  [1]           0x01
  [2]           nbytes >> 10               [2..3] is the WORD count,
  [3]           nbytes >> 2                big-endian 16-bit
  [4]           address >> 8
  [5]           address >> 0
  [6]           address >> 24
  [7]           address >> 16
  [8]           hdrsum >> 8                hdrsum = sum16(pkt[2..7])
  [9]           hdrsum >> 0
  [10 + 4i + 0] w_i >> 8                   for i in 0 .. (nbytes/4 - 1)
  [10 + 4i + 1] w_i >> 0
  [10 + 4i + 2] w_i >> 24
  [10 + 4i + 3] w_i >> 16
  [10 + 4c + 0] paysum >> 8                paysum = sum32(pkt[10 .. 10+4c))
  [10 + 4c + 1] paysum >> 0
  [10 + 4c + 2] paysum >> 24
  [10 + 4c + 3] paysum >> 16

  total length  = 14 + nbytes
```

**Every 32-bit quantity is middle-endian**: big-endian 16-bit halves with the
LOW half first, `[v>>8, v>>0, v>>24, v>>16]`. That is the byte order of a part
with a 16-bit bus, and it applies to the address, to each payload word and to
the trailing checksum alike.

Both checksums are plain byte sums with no seed and no complement —
`0xc0445d74` truncates to 16 bits (it is byte-for-byte `s5l_mtz2_sum16`) and
`0xc0445da4` keeps all 32.

### 6.2 The acknowledgement, and the number it has to be

After each packet the sender (`0xc0445144` for preconstructed firmware; the
calibration senders are the same shape) calls vtable `+0x4d4` —
`0xc0440c98`, which logs "performing HBPP ATN_ACK". That is a **two-byte SPI
transfer**:

```
   host sends   1A A1
   device must  4B C1        status = (rx[0] << 8) | rx[1] == 0x4BC1
```

`1A A1` is the first halfword of the HBPP probe pattern the model already
answers. The comparison is at `0xc0445284` against the literal `0x00004bc1`;
anything else is retried, **five times**, and then the whole send fails.

So the minimum a device must do to be programmed is: accept a `0x30`-marked
DATA packet of `14 + nbytes` octets, and answer `4B C1` to the `1A A1` that
follows it.

### 6.3 The wake, and the execute

**`MT_SPI_Z2_WAKE_CMD` is opcode `0xEE`**, sent by `0xc0445f2c` as an ordinary
sixteen-octet command frame — the same shape `mtz2.c` already frames for every
other opcode:

```
   EE 00 00 00 00 00 00 00 00 00 00 00 00 00 EE 00
   ^opcode                                   ^ LE16 sum16(tx[0..13]) = 0x00EE
```

**"About to execute" is a twelve-octet packet** built by `0xc044490c`, and it
is the one that ends the bootload:

```
   1D 53 18 00 10 00 00 01 00 00 00 29
   ^^^^^ marker        checksum over [2..9] ^^^^^  sum16 = 0x0029
```

Its answer is **never examined** — `0xc0444a00` returns 1 unconditionally. So
the packet's arrival, not its reply, is the event.

### 6.4 What the model has to become

The current design is a probe counter: TRUE once, FALSE forever. The correct
design is a **state machine keyed on the bootload**, which is what removes the
lie rather than relocating it:

```
   reset            -> in HBPP.  The probe answers TRUE for as long as this
                       lasts, so BOTH finishStarting() and
                       attemptToBootloadDevice() get the TRUE they need.
   0xEE             -> wake, answered like any other command frame.
   0x30 packet      -> consume 14 + nbytes octets.
   1A A1            -> answer 4B C1.
   1D 53 packet     -> leave HBPP. The part is now running the firmware it was
                       handed, so every later probe answers FALSE, which is
                       what a real programmed Z2 does and what lets the driver
                       go on to interrogate it.
```

That ordering is the whole point: `finishStarting` runs **before**
`attemptToBootloadDevice` (run96's console proves it — "detected HBPP. driver
will be kept alive" precedes the retries by thousands of lines), so a single
TRUE-then-FALSE bit satisfies the first and starves the second. Only the
bootload itself can separate them honestly.

### 6.5 The register primitives

`performCalibSeq` drives the part through two more packet types, both built
with the same middle-endian convention as the DATA packet.

**Read** — vtable `+0x4e4`, `0xc0440a74`, eight octets:

```
   1C 73 <addr:4 middle-endian> <sum16(pkt[2..5]):2 big-endian>
```

The value does not come back in that transfer. A second call, vtable `+0x4e0`
at `0xc0440b80` — "performing long HBPP ATN_ACK for a MemRead" — sends

```
   1A A1 18 E1 18 E1 18 E1
```

which is **the HBPP probe pattern the model already answers**, extended to
eight octets, and the reply carries the 32-bit register value.

**Write** — vtable `+0x4e8`, `0xc0440e4c`, sixteen octets:

```
   1E 33 <addr:4> <mask:4> <value:4> <sum16(pkt[2..13]):2>
```

so it is an ordinary sixteen-octet frame like every other command.

### 6.6 The version trap

`performCalibSeq` reads register `0x10008ffc` and compares it against
**`0x5A020028`** at `0xc044563c`:

* **equal** → the four register writes are skipped and `bootloader->0x7e4` is
  set to **1**;
* **not equal** → it logs "Detected Z2 Version: …", performs four writes
  (`0x10001c04` mask `0x1fff`, `0x10001c08` value `0x840000` mask `0xff0000`,
  `0x1000300c` value 5 mask `0x85`, `0x1000304c` value `0x20` mask all), each
  of which must return non-zero, and leaves `0x7e4` at **0**.

And `attemptToBootloadDevice` tests exactly that byte at `0xc0441568`: **zero
returns success**, non-zero falls into the `WANTS_FRAMES_IGNORED` path that
ends in "*** ERROR: Disabling touch ***".

So the model must report a version that is **not** `0x5A020028` and must
answer the four register writes. Reporting the "convenient" value that skips
the writes would take the branch that disables touch — which is the sort of
thing that only shows up as a working bootload and a dead digitizer.

### 6.7 The calibration request, and what "waiting" actually is

`0xc0445740` onward: a **two-octet** transfer of

```
   1F 01
```

then — and this is much less than the log line suggests — a single
`IOSleep(0x41)` (65 ms) at `0xc04457e0`, then **one short ATN_ACK**. It is not
a poll loop. And `0xc0445804` returns success on the ATN_ACK's *transfer*
status alone: the halfword it returns is **not compared to anything**. So the
part only has to not fail the transfer.

The long MemRead ATN_ACK's reply carries the value at `rx[2..5]` in the same
middle-endian order as everything else — `0xc0440c5c`-`0xc0440c74` assemble it
as `(rx[2]<<8 | rx[3]) | (rx[4]<<8 | rx[5]) << 16`.

### 6.9 The idle attention word, and the two octets that broke everything

**Measured 2026-07-29, run144.** §6.8 above frames the three `1A A1` forms by
context, and that reasoning is sound. It is also not the whole framing problem,
because a FOURTH thing appears on this wire and it does not begin with `1A A1`.

The device's own stream, dumped after the probes:

```
  18 e1 | 30 01 34 df 00 00 00 00 01 13 | f0 18 e5 9f ...
  ^^^^^   ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^   ^^^^^^^^^^^
  idle    a DATA header, exactly as 6.1    the payload
          specifies                        (ARM code)
```

Read the header against §6.1: `0x30`, `0x01`, word count `0x34df` big-endian =
**13,535**, address zero in the middle-endian order, and `hdrsum = 0x0113`.
Check it: `sum16(0x34, 0xdf, 0, 0, 0, 0) = 0x113`. **It verifies exactly.**
13,535 words is 54,140 octets, plus 14 of framing is 54,154 — the whole 54,156
octet image **in one DATA packet**.

So §6.1 was right in every detail and the bootload was well-formed the entire
time. What went wrong is two octets in front of it.

`0x18` is not an opcode this model knew. Between packets the framer therefore
consumed the `18` as an unknown byte and read the following `E1` as
`GET_CMD_STATUS` — a sixteen-octet frame, which swallowed the DATA header
whole. From there it was parsing ARM instructions as commands, and because
ARM's `AL` condition nibble IS `0xe` while the command opcodes are `0xe1`..
`0xee`, it found "commands" constantly: `e5` (LDR/STR) as WRITE_LONG, `ea` (B)
as FRAME_Z1, `e2` (data-processing) as DEVICE_INFO.

`0x18E1` is one of the seven words `isInHBPP`'s accept-set holds
(`0xc0440658`), so a device seeing it between packets is seeing an attention
word rather than a command. It is two octets and it loops back, like the probe.

**What this retracts.** Three days of framing theories were aimed at the wrong
question. All of these were measured and all of them were beside the point:

  - opcode framing cannot survive an ARM image (run138) — true, and irrelevant,
    because the image is not framed by opcode; it is one packet with a length
  - the chip select brackets the 16-octet probes and not the image (run142)
  - `SPI_CNT` is written zero by the DMA path and `SPI_PIN` sees only a
    power-on zero, so neither delimits anything

None of that was wrong. It was all an answer to "where does the image end?",
and the image carried its own length in a header the framer stepped over.

### 6.8 The framing problem, and how to resolve it

`isInHBPP` (`0xc0441008`) builds its probe as `0x1A 0xA1` followed by seven
`18 E1` pairs — **sixteen octets**. So three different transactions begin with
the same two bytes:

| length | bytes | when |
|---|---|---|
| 2 | `1A A1` | short ATN_ACK, after a DATA packet or a calibration request |
| 8 | `1A A1 18 E1 18 E1 18 E1` | long ATN_ACK, after a register read |
| 16 | `1A A1` + `18 E1` ×7 | the HBPP probe |

The 8- and 16-octet forms are **prefixes of one another**, so no amount of
looking at the bytes can separate them. On real silicon the chip select does
it; `soc.h` records that this controller cannot observe a select edge, because
the select lines are GPIO platform functions and neither controller sets
`internal-cs`.

The model must therefore frame these by **context** — which is available,
because it sees the whole stream in order:

```
   after a 0x30 DATA packet      -> the next 1A A1 is 2 octets
   after a 1C 73 register read   -> the next 1A A1 is 8 octets
   after a 1F 01 calib request   -> the next 1A A1 is 2 octets
   otherwise                     -> 16 octets, the probe
```

That is a fact about this bus rather than a convenience, and it is the one
piece of the design that is not a direct transcription of the driver.

### 6.9 Measured against a guest, once

run101 booted with all of this implemented. **The probes are fixed** — seven,
all answered yes, and run96's six refusals are gone, so `finishStarting()` and
`attemptToBootloadDevice()` now get the same honest answer.

**The bootload still sent nothing**: `data 0, acks 0, rd 0, wr 0, exec 0`, and
the part ended the run still a bootloader. So every packet layout in this
section is transcribed from the builders but **none of it has yet been
exercised by a real driver**, and where `bootloadDevice()` gives up is the open
question. One hypothesis is already eliminated — the device tree carries
neither `fll-mval` nor `cal-dl-addr`, but `0xc04443a4` defaults them to
`0x16e4` and `0x400200`, so a missing property is not the blocker. See
`docs/BOOTLOG.md` run101.

### 6.11 Where the firmware is -- RETRACTED, and what run102 measured instead

**The claim previously in this section was wrong and is withdrawn.** It said the
Z2 firmware is not in the kernelcache, on the grounds that `__PRELINK_INFO`
holds no `Firmware`, `Constructed Firmware`, `Calibration Data` or
`PreconstructedBootloadPacketType` key and no `<data>` blob larger than 1,756
characters. Those searches were accurate; **the conclusion drawn from them was
not**. The section noted that it contradicted run65 and said a measurement
would decide it. It did, and run65 was right.

**run102**, eight kernel probes:

| probe | captures |
|---|---|
| `0xc0442670` `finishStarting` | 1 (control) |
| `0xc0441008` `isInHBPP` | 4 (control) |
| `0xc04414c4` `attemptToBootloadDevice` | **3** |
| `0xc044153c` the object-creation result | **3** |
| `0xc0445860` `bootloadDevice` | **3** |
| `0xc0444370` the `+0x8c` dispatcher | **3** |
| `0xc0445144` **the preconstructed sender** | **3** |
| `0xc04452c8` the unconstructed sender | 0 |

So `attemptToBootloadDevice` now gets its TRUE three times where it used to get
FALSE, the bootloader object is created rather than returning NULL,
`bootloadDevice()` is entered, the dispatcher picks the **preconstructed** arm,
and the sender that logs "sending preconstructed firmware bytes" runs. The
register captures confirm the chain: the sender is entered with
`lr = 0xc0444390`, which is the dispatcher's own `ldr pc,[r3,#0x90]`.

**The firmware therefore reaches the driver by some route this project has not
located.** A plugin `__DATA` section rather than a plist property is the
obvious candidate; the search that failed only covered `__PRELINK_INFO`. Where
it lives is still unknown and is no longer on the critical path, because the
driver clearly has it.

### 6.12 What is actually blocking, as of run102

The sender is entered and the device receives nothing, so it stops between the
two. There is exactly one branch there that can do that (`0xc04451b0`):

```
    ldrb r3, [r5, #0x2c]      ; the bootloader's use-DMA flag
    cmp  r3, #0
    beq  0xc04451f4           ; PIO   -> v[0x368] at 0xc0445224
    cmp  r6, #0xff            ; r6 = the firmware length
    bls  0xc04451f4           ; 255 or fewer octets -> PIO anyway
    ...                       ; DMA   -> v[0x360] at 0xc04451ec
```

and the guest's own console says **`AppleMultitouchZ2SPI: using DMA for
bootloading`**, which is `this->0x2c` being non-zero.

run103 answered which arm is taken, and it is the DMA one: `0xc04451c4` 15
captures, `v[0x360]` 15, the PIO path 0. The guest was never the problem.

**RESOLVED 2026-07-28, and the cause was in this emulator, not in the guest.**
Everything below this line about *why* the bootload failed is superseded; the
protocol table that follows it is unaffected and still correct.

The firmware is delivered now — `ch5 runs 14 bytes 54156`, which is the Z2
image exactly, with `spi1 words 54236` and `tx-drops 0` (run120). Getting there
meant three defects, each visible only once the one above it was fixed:

1. **The bus decode dropped every narrow DMA store.** `machine.c` decoded the
   SPI and I2S windows with `mmio_word()`, which requires `bytes == 4`. But
   `/arm-io/spi1`'s own `dma-channels` template is `0x00089000` — four-byte
   source, ONE-BYTE destination — so the controller issued `write8` and every
   one fell past every device into `unmapped_writes`. run114 measured 812,340
   bytes leaving the DMAC against 176 words shifted by the port, with SPI1's
   page named in the outside-the-map list.
2. **The controller burst the whole image into an eight-deep FIFO** inside one
   tick, before the driver had armed the port: `tx-drops 54140` against an image
   of 54,156 (run116). A PL080 paces peripheral destinations with request lines
   this model did not have; it has them now.
3. **The shifter only ran on register access**, so a controller waiting for FIFO
   space deadlocked against a port waiting to be written — `ch5 runs 0 bytes 16
   ENABLED` with 1020 transfers left (run119). A shifter with data and a clock
   shifts, so it runs from the tick.

The reason this took so long is worth recording next to the answer. A whole
chain of guest-side explanations was written and believed — that
`AppleARMPL080DMAC` never registered as an `IODMAController`, that
`IODMAController::getController` returned NULL, that `v[0x360]` refused with
`kIOReturnDMAError`, that `DMACConfiguration` was never written, that
`SPI_SETUP_DMA` was never raised. **Every one of those was measured false on
2026-07-28**, and several came from diagnostics that printed a register's
resting VALUE while reading as though they reported whether an event had ever
happened. Those diagnostics now count events.

What is delivered is measured. What the device does with the image — whether
the ATN_ACK answers `4B C1`, application firmware runs, and `Sensor Rows` /
`Sensor Columns` reach userspace — is a separate question and is not claimed
here.

### 6.10 The complete HBPP command set

Everything the model has to answer, in one place:

| bytes | length | meaning | the device must |
|---|---|---|---|
| `EE …` | 16 | `MT_SPI_Z2_WAKE_CMD` | answer like any command frame |
| `30 01 …` | 14 + 4·W | DATA packet | consume it |
| `1A A1` | 2 | short ATN_ACK | answer **`4B C1`** |
| `1C 73 …` | 8 | register read request | consume it |
| `1A A1 18 E1 ×3` | 8 | long ATN_ACK (MemRead) | return the value at `rx[2..5]` |
| `1E 33 …` | 16 | register write | succeed |
| `1F 01` | 2 | request calibration | succeed |
| `1D 53 …` | 12 | execute | **leave HBPP** |
| `1A A1 18 E1 ×7` | 16 | `isInHBPP` probe | loopback while in HBPP |

with register `0x10008ffc` reporting anything **except** `0x5A020028`.

### 6.13 The select edge that cut the header in half (run148)

§6.9 fixed the idle attention word, and it worked: `18 e1` stopped desynchronising
the framer. It was not enough, and run148 says exactly why.

The transaction ledger — octets between chip-select edges — came back:

```
transactions: 0 0 16 0 16 0 16 0 16 0 16 8 54148 0
                                          ─┬─ ──┬──
```

Six 16-octet brackets are the probes. The last two are **one HBPP DATA packet cut
in half**:

```
    8 octets:  18 e1 │ 30 01 34 df 00 00
               ^idle   ^six of the ten header octets
54148 octets:  00 00 01 13 │ <54140 payload> │ <4-octet sum>
               ^rest of addr, then hdrsum
```

The arithmetic closes with nothing left over, which had not happened before:

| quantity | value |
|---|---|
| header states | `0x34df` = 13,535 words |
| packet length | `14 + 4×13535` = **54,154** |
| octets delivered | `6 + 54,148` = **54,154** ✓ |
| plus the idle word | `2 + 54,154` = **54,156** = the firmware image ✓ |

`s5l_mtz2_select_pin()` discarded any part-received packet at every edge, on the
stated reasoning that *"a chip-select edge means no transfer is in flight, so
discarding a part-received packet at one is always safe."* That assumption is
false for this driver. Throwing the header away left the framer re-syncing onto
ARM payload words, whose `0xe` condition nibble reads as this protocol's own
command opcodes (§6.9) — producing run148's

```
packets: 1a:16 1a:16 18:2 ×6 e1:16 ea:16 ea:16 e2:16 e2:16 e2:16 30:17538 …
hbpp:    probes 6  acks 0  data 2 (18408 bytes)  rd 0  wr 0  calib 0  exec 0
```

18,408 booked against a 54,156-byte image, and `exec 0`.

**Fix (commit `65e704e`):** an HBPP DATA packet whose length is known and not yet
complete survives the edge. Deliberately narrower than "any in-flight packet" —
an ordinary command frame cut by a select edge is still discarded, because
`test_framing_survives_without_a_chip_select` asserts it and no evidence shows
the real driver ever does that. `test_an_hbpp_data_packet_survives_a_select_edge`
reproduces the split in miniature.

**Not claimed:** that this makes touch work or restores the display. run151 is
the test. run148's black screen is *not* evidence either way — it ran a 3.0 G
budget against run140's 3.55 G, the same confound that made run143 look like a
counterexample to the display finding.

**Superseded:** run142's reading that "chip-select brackets only the probes" was
drawn from a ledger that had recorded only its first eight transactions. The
transfer is bracketed; the ledger just had not reached it.

### 6.14 The sender's own retry loop says where the driver is stuck (run151)

§6.2 described the acknowledgement from the driver's log strings. Here is the
sender disassembled, `0xc0445144` onward, ARM (prelinked kexts are ARM; only the
XNU core is Thumb):

```
c04451b0  ldrb r3, [r5, #0x2c]     ; <- the retry target
c04451bc  cmp  r6, #0xff           ; payload > 255 octets?
c04451c0  bls  #0xc04451f4         ;   no  -> the byte-at-a-time path, vtable+0x7c
c04451ec  ldr  pc, [ip, #0x360]    ;   yes -> THE DMA SEND. 54,156 octets go here
c04451f0  b    #0xc0445228         ; <- where it returns to

c0445228  cmp  r0, #0              ; the DMA send's result
c0445230  beq  #0xc044525c         ;   0 -> acknowledge
c0445254  blx  r3                  ; non-0 -> log the error, then fall through
c0445258  b    #0xc0445290         ;          to the retry counter

c044525c  add  r1, sp, #0x10
c0445260  strh r0, [r1, #-2]!      ; park a zero halfword to receive into
c0445270  ldr  pc, [r3, #0x4d4]    ; THE ATN_ACK -- exactly the vtable slot 6.2 names
c0445274  cmp  r0, #0
c0445278  bne  #0xc0445290         ; transfer failed -> retry
c044527c  ldrh r2, [sp, #0xe]      ; what came back
c0445284  cmp  r2, r3              ; against the 0x4BC1 literal
c045288   addeq r0, r0, #1
c044528c  beq  #0xc04452a0         ; match -> return 1, the packet is delivered

c0445290  add  r8, r8, #1          ; the retry counter
c0445294  cmp  r8, #5
c0445298  bne  #0xc04451b0         ; ...five times, re-sending the whole packet
c044529c  mov  r0, #0              ; then the send fails
```

**What this rules out.** Every path out of the DMA send passes through
`c0445290`, and every one of them re-sends the packet up to five times. A run
where the device answered the ATN_ACK wrongly, or where the DMA send reported an
error, would therefore show **five DATA packets** and at least one ATN_ACK.

run151 shows `data 1` -- one packet -- with `acks 0`, `probes 2` and no retry.
So the driver never reached `c0445290` at all. It is not looping, not failing,
and not giving up.

**What that leaves — and it was WRONG.** The reasoning above concluded that the
call at `c04451ec` had not returned and the driver was blocked inside the DMA
send. Every step of it was sound and the conclusion was false. run155 probed all
eight addresses and measured the opposite:

```
c04451ec  the DMA send call        entered  @1,111,780,316
c04451f0  its return point         REACHED  @1,111,844,336   r0 = 0 (success)
c0445270  the ATN_ACK call         entered  @1,111,844,345
c0445274  after the ATN_ACK        NEVER REACHED
c0445290  the retry counter        never reached
```

The DMA send returned, and returned *successfully*. The driver then called the
ATN_ACK nine instructions later, and **that** is what never came back. The
elimination argument was correct that the retry loop was never reached; it just
picked the wrong one of the two calls that could explain it.

The lesson is cheap to state and was expensive to learn: an elimination argument
over a control-flow graph narrows the candidates, it does not choose between
them. Probing all eight addresses cost one run and settled in one line what an
hour of reading could not.

**The real cause, and it was in the SPI report all along: `tx/rx level 2/8`.**
See §6.15.

The model moves every octet (`dmac1 ch5 ... runs 14 bytes 54156`, and the device
frames all 54,154 of them as one packet), so what is missing is not the data but
the completion the driver is waiting on. run154 probes all eight addresses above
to confirm the call is entered and never returns, rather than leaving that as
the last inference standing.

**Which completion: the SPI interrupt, not the DMA controller's.** run151's VIC
enable masks are `VIC0 en=c004269f`, `VIC1 en=000404c3`, which decode to lines
`0 1 2 3 4 7 9 10 13 18 30 31 32 33 38 39 42 50`. **SPI0 is IRQ 9 and SPI1 is
IRQ 10 and both are enabled; DMAC0 and DMAC1 are IRQ 16 and 17 and neither is.**
So the driver arms the DMA, sleeps, and expects to be woken through spi1's line
-- which matches `s5l_spi_irq()`'s own note in `core/src/soc/spi.c`: the stock
filter's action is what runs `finishTransfer`, and `finishTransfer` is what calls
`commandWakeup`. A bootload that never wakes is a `commandSleep` that was never
answered.

And spi1's line is not asserted. run151 ends with `VIC0 raw=00000000` -- no line
raised at all -- while `s5l_spi_irq()` requires three terms together: the SETUP
interrupt bits, a latched STATUS event, and a non-empty receive FIFO. The FIFO
term is satisfied and then some (`tx/rx level 2/8`, full, with 54,148 overruns),
and `setup 000011be` carries the 0x180 the driver sets, so the term that is
failing is the latched STATUS event.

**Still not claimed:** why it is not latched. The end-state registers cannot say
whether the event was never set during the DMA transfer or was set and then
cleared by the filter's own acknowledge, and those need different fixes. The
overruns are a symptom to explain, not yet a cause: silicon does not stall a
shifter on a full receive FIFO, so overrunning is correct behaviour for a
write-only transfer and `spi.c` says so at line 86.

### 6.15 The receive FIFO the bootload left full (run155)

The ATN_ACK is a two-octet PIO transfer. `spi_shift()` will only move an octet in
PIO mode when `rx_level < S5L_SPI_FIFO_DEPTH`, and that stall is load-bearing:
it is what lets a sixteen-octet command work against an eight-deep FIFO, because
each `RXDATA` read makes room for one more.

The Z2 bootload runs immediately before it, and it is **memory-to-peripheral on
dmac1 ch5 alone** — `src 0bfdd38c dst 3ce00010`, nothing moving the other way.
Nothing is configured to drain a receive FIFO. `spi_shift()` filled one anyway:
the first eight answers stored, the other 54,148 counted as overruns, leaving
`rx_level` at 8 when the burst ended.

So the ATN_ACK could never shift. Its two octets sat in the transmit FIFO for
**2.44 G instructions** — that is the `tx/rx level 2/8` the report had been
printing since the transfer started working — the device never saw `1A A1`
(`acks 0`), and the driver slept inside a call with no way to complete.

Nothing was ever going to drain those eight: **91 register reads against 54,351
writes** on spi1 across the entire run.

**Fix (`a6f3520`):** in DMA mode the answer is dropped rather than queued, and
still counted. The existing principle that no discarded octet may be invisible is
kept — the unit test's 200-octet burst now counts 200 overruns instead of 192.

**INFERRED:** that a TX-only DMA transfer has no receive consumer and therefore
queues nothing. **MEASURED:** that queueing it deadlocks the bus permanently, and
that nothing drains it. Whatever silicon does with those bytes it cannot be
"wedge the bus until reset", because the shipped driver runs this exact sequence
on real hardware and goes on to finish the bootload.

`test_spi` now asserts `rx_level == 0` after a DMA burst and, more to the point,
that a PIO transfer *following* a DMA burst still shifts. The PIO stall itself is
untouched and still asserted.

**Not claimed:** that this completes the bootload. run156 is the test.

### 6.16 It was never the DMA's eight octets (run156, run157)

run156 shipped that fix and deadlocked identically -- `tx/rx level 2/8`,
`rx-overruns 54148`, `acks 0`. The fix is real (`test_spi` proves the DMA path
queues nothing now) and it changed nothing, because **those eight octets were
never the DMA's.**

The ledger had already said so: `... 16 0 16 8 54148 0`. The **8-octet PIO
transaction fills the FIFO before the burst starts**. With the fix the burst
drops all 54,148; without it the FIFO was already full so they all overran
anyway. Identical counters either way, which is why the numbers could not
distinguish a landed fix from an absent one.

run157 then settled who was at fault, with two counters added for the purpose:

```
rxdata-reads 80    irq rising-edges 10
```

Both of the standing hypotheses died at once. **The guest does read the receive
FIFO, and this model does raise the line.** And the arithmetic names the
exception: the ledger holds five 16-octet transactions, and 5 x 16 = 80. Every
command the driver cares about is drained exactly -- its flow control was never
the problem. The one transaction it never reads is the 8-octet header opening
the Z2 download, and it has no reason to: a firmware download is write-only and
its answers are meaningless.

**Why silicon cannot leave them there.** `0xc0445284` compares the ATN_ACK's
REPLY against `0x4BC1`, read back at `ldrh r2, [sp, #0xe]`. A receive FIFO still
holding the header's answers would hand it stale octets and fail that compare
*even on hardware that shifted perfectly*. So the controller must present a
clean receive path at the start of a transfer, and the driver demonstrably does
not do it by hand.

**Fix:** writing `SPI_CNT` clears the receive FIFO. It is the per-transfer
register -- `max(txLen, rxLen)`, written at `0xc05a6b3c` before the data moves
-- so it is the one store that happens exactly once per transfer and always
before it. INFERRED that this marks the boundary; MEASURED that without it the
bus deadlocks permanently, that the guest never drains the stale octets, and
that every reply it does want it already reads. Not a transmit-side flush:
nothing is measured about pending output there.

**Not claimed:** that this completes the bootload. run158 is the test, and the
three predictions of that kind made before it were all wrong.

## 7. Downstream, once this is unblocked

* `mtz2.c` already answers 0xD1/0xD3/0xD9 with correct non-zero values, and as
  of commit `1c9c831` answers 0xD0 with a real descriptor whose `desc[0]` is
  non-zero — which suppresses the back-fill and makes the geometry independent
  of the property path.
* `to_surface()` maps the panel onto `[-75, 4656]` and `[-75, 7275]`, the
  bounds the guest derives from 10 columns and 15 rows.
* **Sound is downstream of touch.** `core/src/soc/pl080.c` is modelled and
  correct but idle: nothing asks the audio stack to play at a lock screen.
