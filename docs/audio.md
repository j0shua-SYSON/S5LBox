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
