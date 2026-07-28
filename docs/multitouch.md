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

## 7. Downstream, once this is unblocked

* `mtz2.c` already answers 0xD1/0xD3/0xD9 with correct non-zero values, and as
  of commit `1c9c831` answers 0xD0 with a real descriptor whose `desc[0]` is
  non-zero — which suppresses the back-fill and makes the geometry independent
  of the property path.
* `to_surface()` maps the panel onto `[-75, 4656]` and `[-75, 7275]`, the
  bounds the guest derives from 10 columns and 15 rows.
* **Sound is downstream of touch.** `core/src/soc/pl080.c` is modelled and
  correct but idle: nothing asks the audio stack to play at a lock screen.
