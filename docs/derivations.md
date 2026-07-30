<!--
  Extracted from docs/AGENT_HANDOFF.md on 2026-07-31, when that file was
  retired as outdated. These are the sections that CODE COMMENTS cite by number
  as the provenance for a decision -- roughly twenty references across
  core/include/soc.h, core/src/soc/machine.c, core/tests/test_uart4.c,
  tools/bootkernel.c, tools/rootfs_work.h, docs/BOOTLOG.md, docs/QUALITY.md and
  docs/ROADMAP.md. Deleting the handoff without them would have left every one
  of those comments pointing at nothing.

  Kept verbatim apart from heading depth. Section numbers are preserved exactly
  BECAUSE the citations use them: renumbering would break the references this
  file exists to keep working.

  This is a provenance archive, not a guide. It records how particular facts
  were established and is not maintained as a description of the current tree.
-->

# Derivations cited by the source

Where a comment in the tree says "AGENT_HANDOFF section N", N is here.


## 13.0a The exact baseband topology, resolved read-only after Run23

Question 2 above is now partly answered, and the hardware map is exact. All of
this comes from the shipped 7E18 device tree and Capstone disassembly of the
prelinked image; none of it is inferred from behaviour.

**The reset event source is an interrupt event source.** `AppleBaseband`'s
setup path at `0xc0558c98` calls
`IOInterruptEventSource::interruptEventSource(OSObject *owner, Action, IOService *provider, int index)`
(`__ZN22IOInterruptEventSource20interruptEventSourceEP8OSObjectPFvS1_PS_iEP9IOServicei`,
`0xc0189e94`) with `r0` = the AppleBaseband object, `r2` = its provider, and
`r3 = 0` — **interrupt index 0**. It then adds it to the workloop returned by
vtable slot `0x1dc` and calls
`IOInterruptEventSource::enable()` (`0xc0189d58`) through event-source vtable
slot `0x68`. The retained object's vtable is `__ZTV22IOInterruptEventSource`.

So the callback at `0xc0558358` can only run when that hardware interrupt is
delivered. Its body is short and worth knowing exactly: it calls reset-state
read through AppleBaseband vtable slot `0x35c` into a two-word stack buffer,
returns immediately if the read fails, compares the 64-bit result against the
remembered state at object `+0x70`, returns if unchanged, and otherwise calls
`IOService::messageClients` through vtable slot `0x238` with `0xe3ff8000` for a
non-zero (high) state or `0xe3ff8001` for zero (low) — the low path first
checking a suppression byte at object `+0x69`.

**Which interrupt.** `/device-tree/baseband` carries:

```text
name                'baseband'          compatible 'baseband,n82'
interrupts          {0x0000004b, 0x00000005}
interrupt-parent    {0x00b05320}
function-reset_det  {0x00b05320, 'GPIO', 0x00001203, 0x00000100}
function-bb_rst     {0x00b05320, 'GPIO', 0x00000700, 0x00000101}
function-bb_on      {0x00b14140, 'GPIO', 0x00000003, 0x00070001}
function-radio_on   {0x00b05320, 'GPIO', 0x00001507, 0x00010101}
```

`0x00b05320` is the `AAPL,phandle` of `/device-tree/arm-io/gpio`
(`compatible 'gpio,s5l8900x'`, `device_type 'interrupt-controller'`,
`reg {0x06400000,0x1000, 0x01a00000,0x1000}` → PA `0x3e400000` and
`0x39a00000`, `#interrupt-cells 2`, `#interrupt-groups 7`,
`fsel-offset 0x320`).

Therefore **index 0 is GPIO interrupt `0x4b` (75) on the GPIO interrupt
controller**, and the reset-detect signal itself is GPIO `0x1203`. The
emulator declares `gpio` and `gpioic` as storage-only stub windows with no
interrupt generation, so GPIO interrupt 75 can never be delivered, which is
exactly why the callback has zero hits.

**The transport.** `/device-tree/arm-io/spi2` is the baseband SPI:
`compatible 'spi,s5l8900x,baseband'`, `reg {0x05200000,0x1000}` → PA
`0x3d200000`, `interrupts {0x07, 0x02}`, with `function-srdy` GPIO `0x1804`,
`function-mrdy` GPIO `0x1702`, `function-mosi` GPIO `0x1806`, `function-sclk`
GPIO `0x1805`, `function-fail_gpio` GPIO `0x0c03`, and DMA channel descriptors
pointing at `0x3d200010`/`0x3d200020`. Its siblings are `/arm-io/spi0`
(`0x04300000` → `0x3c300000`, interrupt 9) and `/arm-io/spi1`
(`0x04e00000` → `0x3ce00000`, interrupt 10).

**What was changed as a result.** All three SPI windows are now declared named
stubs. Exact disassembly of `BasebandSPI+0x1d42` shows the driver reading
offsets `0x00/0x04/0x08/0x34` into a heap transfer descriptor without testing
or polling them, and `BasebandSPI+0x1eca` shows the configuration burst that
wrote them, so honest storage is the faithful answer and nothing autonomous is
fabricated. This is a window, not a controller, and it is **not** claimed to
unblock the boot.

**How the reset state is actually read.** Vtable slot `0x35c` resolves to
`AppleBaseband+0x11bc` (`0xc05581bc`), and it is short enough to state exactly.
It loads the reset platform-function object from AppleBaseband `+0x6c` — the
one the observer reports as `value=c0b6b020` — returns a failure literal if it
is null, zeroes a one-word stack slot, calls the object's vtable slot `0x50`
with that slot as the out parameter, returns on a non-zero result, and
otherwise widens the returned word to the 64-bit `{value, 0}` the callback
compares against `+0x70`. So "read reset state" means "invoke the
`function-reset_det` GPIO platform function", nothing more.

**The platform-function descriptors, and what their last word appears to mean.**
Across the shipped tree the fourth word is consistent with a direction/operation
code, and the pattern is worth writing down because it bears directly on
whether an edge is faithful:

```text
baseband function-reset_det  {gpio, 'GPIO', 0x1203, 0x00000100}
spi2     function-srdy       {gpio, 'GPIO', 0x1804, 0x00000100}
baseband function-bb_rst     {gpio, 'GPIO', 0x0700, 0x00000101}
spi2     function-mrdy       {gpio, 'GPIO', 0x1702, 0x00000101}
baseband function-radio_on   {gpio, 'GPIO', 0x1507, 0x00010101}
spi2     function-fail_gpio  {gpio, 'GPIO', 0x0c03, 0x00000102}
spi2     function-mosi       {gpio, 'GPIO', 0x1806, 0x00000002}
```

`0x100` lands on exactly the two signals the application processor must
*sense* — `reset_det` and `srdy`, the modem's ready line — while `0x101` lands
on the two it must *drive*, `bb_rst` and `mrdy`. Treat that as a strong reading
of the encoding, not a decoded specification: it has not been confirmed against
the GPIO platform-function implementation in `AppleS5L8900X`.

**Why this argues against fabricating the edge.** If `reset_det` is an input
sensing a line the modem drives, then hardware with no modem fitted would not
produce a `reset_det` transition either, and GPIO interrupt 75 would not fire
on a real device in the same condition. The emulator's zero callbacks would
then be *faithful*, and the missing notification would not be what gates
CommCenter — because a real iPhone whose modem is dead still reaches
SpringBoard. Under that reading the blocker is elsewhere in CommCenter's
startup, and asserting interrupt 75 would be inventing hardware behaviour to
paper over a different bug. Do **not** assert it on the current evidence.

Note also that `AppleBaseband: Could not find mux function` is **stock
behaviour, not an emulator gap**: `/device-tree/baseband` genuinely has no
`function-mux` property, and that line appears in the earliest recorded
framebuffer consoles too.

**Therefore the deciding evidence is §13.0 question 1**, and it needs runtime
state this run did not capture: whether CommCenter's blocked receive port
`c0dd99d8` (mqueue `c0dd99f0`, task-local name `0x10004001`) is an
`ipc_pset` whose member set contains the AppleBaseband interest port
`c3c59ab0`. If it is not a port set containing that port, the baseband lead is
dead and the next frontier must be found by following the earliest of the five
queued senders instead. The ownership probe already validates the
`mqueue == port + 0x18` relationship and the active port-type check that
distinguishes `IOT_PORT` from a port set, so extending it to walk port-set
membership is a small, read-only addition — and it is cheap enough to answer
with a short bounded run rather than a full 24-minute replay.

Do not, on the current evidence, force a queue dequeue, retarget ownership away
from launchd, synthesize a baseband reset edge, inject a CommCenter reply, or
patch SpringBoard/CommCenter.

## 13.0d GPIO pin encoding, and why touch and baseband need the same two blocks

Touch and the baseband transport converge on the same unmodelled hardware, so
work on either pays for both.

`/device-tree/arm-io/spi1/multi-touch` is `compatible "multi-touch,n82"` with
`interrupts {0x9b, 0x00}` and `interrupt-parent 0x00b05320` — that is **GPIO
interrupt 155 on the GPIO interrupt controller**, with `function-reset` GPIO
`0x0606` and `function-power_ldo` GPIO `0x0701`, hanging off **spi1**
(`0x3ce00000`). The baseband path needs the same two blocks: spi2
(`0x3d200000`) plus GPIO `reset_det 0x1203`, `srdy 0x1804`, `mrdy 0x1702`.

So the two remaining goal items — a rendered SpringBoard and a guest touch —
both reduce to **a real S5L8900 SPI controller model and a real GPIO interrupt
controller model**. Neither exists; all five windows are declared stubs with no
transfer, interrupt, or autonomous behaviour.

**The GPIO pin encoding is now decoded from the driver itself.**
`AppleS5L8900X`'s pin accessor at `0xc05a4494` reads:

```text
lsr r1, r5, #8        ; group = pin >> 8
lsl r1, r1, #5        ; group * 32
add r1, r1, #4        ; + 4
ldr pc, [r3, #0x384]  ; <accessor>(group * 32 + 4)
and r1, r5, #0xff     ; bit = pin & 0xff
lsr r0, r0, r1
and r0, r0, #1        ; return (state >> bit) & 1
```

so a platform-function pin id splits as **group = pin >> 8, bit = pin & 0xff**,
and a pin's level lives at **`<base> + group * 32 + 4`**. `#gpio-ports` is 25
and `#interrupt-groups` is 7, consistent with groups 0..24. Concretely:

```text
lcd0 reset        0x0001 -> group  0 bit 1   +0x004
lcd0 ctrl_enable  0x0304 -> group  3 bit 4   +0x064
mt-reset          0x0606 -> group  6 bit 6   +0x0c4
mt-power_ldo      0x0701 -> group  7 bit 1   +0x0e4
bb_rst            0x0700 -> group  7 bit 0   +0x0e4
mrdy              0x1702 -> group 23 bit 2   +0x2e4
reset_det         0x1203 -> group 18 bit 3   +0x244
srdy              0x1804 -> group 24 bit 4   +0x304
```

The two thin accessors are also decoded: object `+0x68` is the **gpio** base
(`0x3e400000`, first touched at `0xc05a44d0`) and object `+0x6c` is the
**gpioic** base (`0x39a00000`, first touched at `0xc05a44e8`).

**A suspected power/gpioic overlap was investigated and does NOT exist.**
The device tree gives `/arm-io/gpio` reg `{0x06400000,0x1000, 0x01a00000,0x1000}`
and `/arm-io/power` reg `{0x01a00000,0x1000}` — both naming the same physical
page `0x39a00000`, which the guest maps twice at different VAs
(`gpioicBaseAddress 0xe3949000`, `_pcBaseAddress 0xe394a000`). Our model splits
that page as power `0x00..0x7f` and gpioic `0x80..0xfff`, so if the pin-level
register lived at `gpioic + group*32 + 4`, groups 0..3 would land inside
power.c's claim — and lcd0's `reset`, `mpl_rx_enable`, `power_enable`,
`pixel_clock_enable` and `control_enable` are all group 0 or 3 pins.

It does not. The four accessors sit consecutively at `c05add68` (`+0x68`
read), `c05add6c` (`+0x68` write), `c05add70` (`+0x6c` read) and `c05add74`
(`+0x6c` write). Scanning the kernelcache for the implied vtable bases shows
`c05ad9e4` referenced as a literal at seven constructor sites and `c05ad9ec` at
none, and `c05ad9e4` is preceded by three zero words — the vtable start
pattern. So the vptr is **`0xc05ad9e4`**, and:

```text
slot 0x384 = c05ad9e4 + 0x384 = c05add68 = the +0x68 accessor = GPIO base
```

**Pin level is therefore at `0x3e400000 + group*32 + 4`**, in the gpio block,
never in the gpioic/power page. The existing power/gpioic split is unaffected
and must not be "fixed" on the strength of the earlier suspicion.

That also fixes the register map a GPIO model has to provide: pin state for
groups 0..24 at gpio `+0x004..+0x304`, `fsel-offset 0x320` from the device
tree, and the separate gpioic page carrying the 7 interrupt groups.

## 17. Documentation and evidence-maintenance rules

The user asked for regular README/docs updates and GitHub pushes. Keep them
truthful and commit-bound.

### Where each update belongs

- `README.md`: concise current headline, capability table, M5 line, latest run,
  strongest evidence, explicit “not rendered” boundary.
- `docs/QUALITY.md`: exact commit/run ledger, local/hosted/firmware validation,
  what each check establishes and does not establish.
- `docs/BOOTLOG.md`: chronological firmware-run narrative and exact coordinates.
- `docs/ROADMAP.md`: milestone status, current frontier, next work item.
- `docs/ARCHITECTURE.md`: only when a lasting ownership, interface, or
  host/core contract changes.
- `docs/debugging.md`: reusable diagnostic procedure, not every transient
  number.
- `docs/networking.md` and `docs/activation.md`: currently protected dirty
  paths; do not edit until ownership is resolved.
- `docs/AGENT_HANDOFF.md`: update at major handoff boundaries, not after every
  minor probe.

### Required language discipline

Use:

- “entered” for an instruction-entry probe;
- “returned with `r0=...`” only at an exact return;
- “candidate” when decisive register/object binding is absent;
- “bound” only when same-object/kmsg criteria pass;
- “saturated at entry” for `msgcount == qlimit`;
- “linked messages” only after a complete consistent queue walk;
- “did not resume before cap” rather than “permanent deadlock”;
- “source hashes unchanged” rather than “nothing was patched”;
- “rendered” only after guest-driven live pixels are captured and inspected.

Every report must state what it does **not** prove.

### Common overclaims to reject

- Exit 0 at an instruction cap means the configured cap was reached, not that
  the OS booted fully.
- A method-entry PC does not prove retirement or return.
- `applicationDidFinishLaunching:` does not prove UI readiness.
- `startWindowServer` returning does not prove pixels.
- CLCD `running=1` does not prove guest writes.
- A changed PPM is not necessarily SpringBoard.
- A seed-only PPM is not a black SpringBoard screen.
- A green public suite cannot execute private firmware.
- A green iOS build does not prove device launch.
- `msgcount=5` does not prove five linked messages.
- An address in a receiver union is not an owner until its discriminator and
  object graph pass.
- A nearby baseband log does not prove queue causality.
- A repeated ID 1000 does not prove `clock_get_time` without destination and
  SEND correlation.
- A static disassembly path does not prove it ran.
- Capstone's mnemonic does not replace architecture-version checks.
- A per-process trace block that reports one generation does not describe the
  whole run. If the process respawns, a longer run prints a **later**
  generation, and the difference between the two is not divergence.
- A checkpoint-restored run that reports different coordinates from its cold
  parent is not evidence of lost restore fidelity until the heartbeat PC stream
  itself is compared.
- A `hits=0` checkpoint proves nothing while the process is dying upstream of
  it; neither does a framebuffer hash that repeats across runs.
- A `hits=0` on a selector is not evidence the work did not happen until that
  selector's call sites are counted in the binary. `-[UIWindow
  makeKeyAndVisible]` has exactly one, on a restore path never taken at boot.
- Un-matching a child device-tree node is not un-matching the device; IOKit
  matching keys off the parent.
- A device-tree patch cannot affect a restored run: the snapshot already holds
  the matched driver in guest RAM.
- JIT tests do not mean boot uses JIT.
- Running on a jailbroken iPhone does not justify embedding a native tweak
  dependency in the core.

## 23.3 Activation: settled, and `docs/activation.md` is wrong

`docs/activation.md` is a protected file and could not be corrected in place, so
the corrections live here. All of the following is byte-verified against
`/usr/libexec/lockdownd` (VA = file offset + 0x1000, ARM not Thumb).

**SpringBoard does not gate the home screen on `ActivationState`.** It reads
`BrickState`. lockdownd derives `BrickState` *from* `ActivationState` at boot.
Two hops the document never modelled:

```
ActivationState --(lockdownd determine_activation_state 0xd340)--> BrickState
BrickState      --(SB _setupActivationState, key _kLockdownBrickStateKey)--> brickedDevice
brickedDevice   --(SBAwayView updateInterface)--> lockout screen
```

**The value the era's tools used does not work here.** Seeding
`ActivationState = Activated` is overwritten to `Unactivated` every boot
(`0xd4f8` CFEqual against `FactoryActivated` fails → `0xd50c` forces
`Unactivated` → tail `0xdba8`/`0xdd00` rewrites). Seeding **`FactoryActivated`**
survives (`0xd508` → `bne 0xda2c`; tail `0xdbb8` not taken → write skipped).

`ActivationState` is normally **non-persistent** (attribute 2, set at `0xdd14`),
which is why the runtime-created `data_ark.plist` has no such key at all.

**No Apple signature is needed to read state.** `verify_activation_record`
(`0xe218`) is called only from `dealwith_activation` and `handle_activate` — the
apply paths — never on boot-default read.

The file to provision, at `/private/var/root/Library/Lockdown/data_ark.plist`:

```xml
<key>-ActivationState</key>  <string>FactoryActivated</string>
<key>-BrickState</key>       <false/>
```

The leading `-` is the global-domain composed form (key builder `0x7b08`). The
second key is not redundant: it makes the fix hold even if `is_phone` is false,
since `determine_activation_state` then skips brick management entirely.

Note the pristine rootfs has `/private/var/root/Library` (CNID 4541) but **no
`Lockdown` subdirectory**, so provisioning must create a directory as well as a
file.

## 23.4a Corrected, twice more, by run65 and run67. Read this before §23.4

§23.4 is right about *what* the HBPP answer must be at each site and wrong about
every mechanism it suggests for deciding it. So was the first revision of this
subsection. Four designs have now been tried; two of them shipped and were
caught by a boot. What follows is what the firmware actually does.

**FIRST: HOW TO SEE ANY OF THIS.** Three things that were got wrong repeatedly:

- **LAUNCH FROM POWERSHELL, NOT FROM GIT BASH.** MSYS path conversion rewrites
  any argument that looks like a Unix path, and the one that matters is
  `--fstab "/dev/md0 / hfs rw,update 0 1"`, which arrives as
  `C:/PortableGit/dev/md0 / hfs rw,update 0 1`. The guest's fstab then names a
  Windows path, `fsck_hfs` fails with `CAN'T CHECK FILE SYSTEM`, launchd halts,
  and the console ends at `pmu go stdby` — about 1.1 billion instructions of
  boot that all look fine and prove nothing. Run69 lost a whole 27-minute run
  to exactly this. The recipe in §11 is PowerShell for a reason. If you must
  use `Start-Process -ArgumentList`, note that it joins the array with spaces
  and does NOT quote: `-c` and `--fstab` need their own embedded `"` or the
  boot dies on `unknown option: serial=1`.
- **`spy_install()` memsets the whole of `G`.** Any option parsed straight into
  `G` is silently discarded, and `--print-config` CANNOT see it, because
  `--print-config` exits before `spy_install` runs. `-H` and `--call-probe`
  already parse into locals and copy afterwards; `--touch` did not, and the
  result was a 27-minute run that accepted two taps, injected neither, and
  printed no touch section at all. Every such option now prints an "armed on"
  line in the run header for exactly this reason: a header that does not carry
  it is a run that did nothing.

- **`debug=0x8` does NOT turn on the multitouch log.** `PE_i_can_has_debugger()`
  reads `/chosen/debug-enabled`, so the boot needs
  **`-D chosen:debug-enabled=1`**. With it, `mt-strings=1` produces the whole
  `mtlog:` stream and the bring-up becomes readable instead of inferred.
- **The bootload timer does not fire until instruction 1,112,618,577.** Every
  cap used before run65 — 400e6, 700e6 — stopped short of the entire bootload
  path, so no observation about it made before then means anything. Use a cap of
  at least **1.5e9**.

**The anchor for all slot arithmetic is vtable base `0xc0449f40`**:
`base + 0x4d0 = 0xc044a410`, which holds `0xc0441008` = `isInHBPP`. A base of
`0xc0449f3c` is off by one slot and makes `0x4d0` resolve to `getCMDStatus`.

**1. `resetDevice` DOES run, at every probe site, and it clocks a decoy.** The
first revision of this subsection claimed resetDevice was unreachable on this
path. Wrong — run65's mtlog shows it at both sites, and each site is:

```
mtlog: AppleMultitouchSPI::setPowerEnabled[false]
mtlog: Asserting reset line
mtlog: disabling power
mtlog: AppleMultitouchSPI::setPowerEnabled[true]
mtlog: enabling power
mtlog: ensuring S_CLK is high
mtlog: initiating dummy transfer        <- 16 bytes, reset still ASSERTED
mtlog: Deasserting reset line
mtlog: checking if in HBPP              <- 16 bytes, reset RELEASED
```

The dummy transfer at `0xc0440d7c` builds **the same sixteen bytes as the
probe** — `mov r3,#0x1a / strb / sub r3,r3,#0x79 / strb` then `0x18,0xE1`
seven times — sends them through the same `v[0x368]`, and discards the answer.
So each site makes **two byte-identical exchanges** and only the second one's
answer is read. Any scheme that counts probes spends itself on the decoy.

**2. The reset line is ACTIVE LOW, and its level is the only discriminator.**
Measured to the instruction from the GPIO trace with `-H 0x3e400000`:

| instruction | fsel write | meaning |
|---|---|---|
| 220,635,069 | `0x0006060e` | assert (group 6, bit 6, level 0) |
| ~309,530,000 | — | the dummy transfer runs |
| 309,541,162 | `0x0006060f` | release |
| 316,898,121 | — | `isInHBPP` entered, `lr = 0xc04426cc` |
| ~316,910,000 | — | the probe runs |
| 316,965,809 | `0x0006060e` | assert again |

The dummy is entirely inside the asserted window and the probe entirely outside
it. A part held in its reset pin drives nothing, so modelling that one physical
fact separates the decoy from the probe with no counting at all. Note also that
the guest's **first** store to this pin writes level 0 — no change from the
pin block's all-zero power-on state, so **no watch callback fires**; a model
must start out believing the part is held down.

**3. A reset must NOT re-arm the claim.** `098ce49` made a reset restore the
in-HBPP state, on the reasoning that a part with no firmware resets into its
bootloader. That reasoning is correct about silicon and fatal here: the guest
resets before *every* site, so it makes the bootload site identical to the
first, and run65 watched the driver answer affirmatively and begin
`MTSPIBootloader_Z2::bootloadDevice()` / "sending preconstructed firmware
bytes". A reset restores the part's **state**; it does not un-ask a question the
host has already had answered. The working model is one monotonic bit spent by
the first probe answered **out of reset**.

**4. Declining at `attemptToBootloadDevice` DOES produce the three
`Bootload attempt` lines.** The first revision of this subsection said the
opposite, from a mis-derived call graph. The literal
`"not in HBPP, so skipping bootload"` (`0xc04486f0`) is referenced from exactly
one place, `0xc04415e4`, which is `attemptToBootloadDevice`'s own literal pool:
the FALSE path logs it and returns 0, and the retry loop at `0xc043a980`
(`subs r2,r0,#0 / beq 0xc043a9e8`) counts that as one failed attempt and prints
`Bootload attempt %d of %d failed`. Three of those, then `isBootloaded()`.

**4a. "Device has firmware?!" DOES NOT PRINT on the cycle that succeeds.**
Expecting it as the success marker will read a working boot as a failure. After
the three attempts the routine calls `isBootloaded()` and branches
`bne 0xc043aa80` (`0xc043aaac`), which lands PAST the log at `0xc043aa64`. The
string only prints on a LATER bootload cycle, when the cached byte at
`this+0x69` is already 1 and `0xc043a950` branches to it. run68 confirms the
shape with `--call-probe-kernel` on all three targets: `0xc043ac58`
(`isBootloaded`) captured 1, `0xc043aa80` (the true branch) captured 1,
`0xc043aa64` (the log) captured 0, and `0xc043aa48`
("No firmware running, and couldn't load any") captured **0**. The correct
success criterion is the ABSENCE of `0xc043aa48`, plus three
`Bootload attempt N of 3 failed` lines.

**5. "Device has no firmware - will attempt to bootload" is a cache read, not a
failed report exchange.** The string at `0xc04472b4` is referenced from
`0xc043aac8`, inside the timer-fired routine, and the top of that routine tests
the cached byte at `this+0x69` (`ldrb r6,[r4,#0x69]` at `0xc043a948`) which is
only written after the three attempts (`strb r0,[r4,#0x69]` at `0xc043aaa8`).
So seeing that line does not mean `getReportInfo(0xD3)` failed — on the first
pass it has not been asked yet.

**6. THE SINK IS NOT BLOCKED BY DMA.** §23.4 says skipping the download "drops
the entire `AppleARMPL080DMAC` model out of scope", which is true, but the
implication that implementing it would *require* that model is not.
`MTSPIBootloader_Z2` pushes through the ordinary SPI entry `v[0x368]` at
`0xc0444ff8`, `0xc0445224` and `0xc04454d0`, and the controller only arms DMA
when `this+0xf4` is non-zero — `ldr r5,[r4,#0xf4] / cmp r5,#0 /
beq 0xc05a6cb4` at `0xc05a6c24`, the branch that skips `orr r2,r2,#0x40`. Our
16-byte transfers run in PIO today, which proves that field is zero, and it is
not per-transfer. So a firmware sink would receive the 54,156 bytes through the
same slave callback everything else uses. What blocks it is that the
bootloader's own multi-stage protocol is unread, its failure mode is a hang
inside `commandSleep` rather than an error, and the feedback loop is an
18-minute boot. It is a fidelity upgrade, not a prerequisite.

Confirmed unchanged and now derived rather than assumed: the probe pattern
(`1A A1` then `18 E1` seven times, loop `0xc0441030`-`0xc0441048`); that **both**
`BE16(rx[0..1])` and `BE16(rx[2..3])` must pass (`0xc04410fc`, `0xc044110c`)
while `rx[4..15]` is only printed; and the accepted set, whose literal pool
gives `0xc04406b4 = 0x18E1`, `0xc04406b8 = 0x1AA1`, `0xc04406bc = 0x4879` with
`+0x620 -> 0x1F01`, `+0x2CC0 -> 0x4BC1`, `+0xF0 -> 0x4969` and
`-0xF0 -> 0x4AD1`. The compared value is `uxth`-truncated. The failure `printf`
has a **double space** after the colon, so grep for `HBPP`.

Also confirmed: `/arm-io/spi1`'s `function-spi_cs0` (GPIO `0x1800`, group 24
bit 0) is really driven, `0x0018000e` at 316,902,467 and `0x0018000f` at
316,918,158, bracketing the probe transfer — so a slave that wants packet
framing from the chip select can have it.

One thing flagged and unexplained: slot `0x458` holds `0xc044061c`, a bare
`bx lr` that leaves `r0 = this`, which makes the following `IOSleep(r0)`
nonsensical. The base is anchored twice, so it is probably not an indexing
error. Do not reason from that line. **RESOLVED — and it was simply misread.
See §23.4b item 1.**

## 23.4d The GPIO interrupt controller's pending latch is EDGE-triggered

This is the ninth correction, and unlike the other eight it is a correction to
*this project's own model* rather than to a reading of Apple's. It is also the
one that decided whether touch works at all, so it is worth the space.

`core/src/soc/gpioic.c` shipped with a LEVEL-sensitive pending latch: the
guest's write-one-to-clear cleared the bit and then immediately re-asserted it
from whatever the board was still driving. The file argued the case in three
points, and the third — "for a device that does deassert it is
indistinguishable from an edge latch, so nothing that behaves correctly can
tell the difference" — is simply false. run71 measured how false:

```
HOT PAGE 0x39a00000, offset 0x0b0 (INTSTAT group 4):
    reads 1,193,122   writes 1,193,123   lastval 0x08000000
@1799986776 R 0x39a000b0 val 0x08000000  pc 0xc05a44dc lr 0xc05a4310
@1799986797 W 0x39a000b0 val 0x08000000  pc 0xc05a44e8 lr 0xc05a4358
@1799987195 R 0x39a000b0 val 0x08000000  pc 0xc05a44dc      <- 419 later
@1799987216 W 0x39a000b0 val 0x08000000  pc 0xc05a44e8
```

One touch report was queued at instruction 1,300,000,000 and the attention line
came up. The GPIO interrupt controller's filter read the pending word, wrote it
back to acknowledge, and the re-latch undid the acknowledge inside the same
store — so it re-entered every ~419 instructions and did so **1,193,122 times**,
for the whole remaining half-billion instructions of the run.
`IOWorkLoop::signalWorkAvailable` ran every time; the work loop's thread never
got scheduled; `AppleMultitouchZ2SPI`'s handler never ran; the frame read never
happened. The device's own counters said so from the other side:
`queued 1, length-reads 0, data-reads 0, read 0` with `in-reset 0` and
`hbpp-answered 1`. A LIVELOCK, not a lost interrupt, and it looks exactly like
"touch is broken" from every direction except this register.

The driver settles it independently: it writes this group's `INTEN` **twice in
the whole boot** (offset 0x0d0, writes 2), so it does not mask the line while
servicing it. A controller whose pending bit survived its own acknowledge would
be unusable by this driver, which means the real part's does not.

So the write-one-to-clear CLEARS, and only a RISING edge on the incoming line
sets a pending bit again. Deasserting still does not clear the latch, so a
pulse shorter than the guest's polling interval is still delivered — the one
property the level design was actually right to want. Four mutations pin it.

**A third downstream kill switch, alongside the two §23.4 already lists.**
MultitouchHID drops every frame while its `UILocked` flag is set, and that flag
is **initialised to 1**. Neither of the two probes §23.4 names as the step-5
milestone can fire while it is.

## 23.5.1 DMA is optional. The answer is a boot argument, not a DMA model.

The open question was whether `AppleS5L8900XSerial` insists on PL080 DMA, since
every boot log brackets it with `AppleARMPL080DMAC::_initDMAChannel` and the
PL080 is unmodelled. **Verified by disassembly: it does programmed I/O, and DMA
setup is gated three ways at `0xc065e410`**, all three skips landing on the same
non-error continuation at `0xc065e6f4` — build the interrupt event source, call
the superclass, no log, no failure:

1. no `dma-types` property on the nub → skip
2. `dma-disable` property present → skip
3. boot argument **`<node>_dma_enable=0`** → skip

The base class agrees: `AppleOnboardSerial`'s vtable supplies a **default DMA
capability of zero** (`movs r0,#0 / bx lr` at `0xc046f154`), queried once at
start and cached; all three consumers treat it as a guard. The PIO receive loop
is real and reads `URXH` in a counted loop at `0xc047212a`.

All four UARTs *do* carry `dma-types`, so the driver will attempt DMA unless
told otherwise — which makes **`uart4_dma_enable=0` in the `-c` string the
entire fix, zero code.** That mechanism is already proven here: `nand-enable-adm=0`
works the same way.

**Use uart4, not uart3** — this contradicts `docs/networking.md` §6, on two
grounds that section did not have. uart3 is the only UART **without**
`no-flow-control`, so the driver enables hardware flow control and reads UMSTAT
for CTS, whereas uart1/uart4 short-circuit `getFlowStatus` to asserted without
touching the register (`0xc065e0bc`). And uart3's child is `bluetooth`, with
`BTServer` (1.1 MB) and its launchd plist both shipping — a live contender for
the port. Nothing owns uart4's `debug` child, and `/etc/ttys`'s getty lines are
inert because getty does not ship.

| node | phys | VIC | child |
|---|---|---|---|
| uart0 | 0x3CC00000 | 24 | `iap` — taken, `boot-console` |
| uart1 | 0x3CC04000 | 25 | `umts` |
| uart3 | 0x3CC0C000 | 27 | `bluetooth` — contended |
| **uart4** | **0x3CC10000** | **28** | `debug` — free |

Register semantics read out of Apple's binary rather than guessed: **UFSTAT
(+0x18)** bits[3:0] RX count, bit8 RX full, bits[7:4] TX count, bit9 TX full,
**FIFO depth 16**; **+0x10 is not a read-only status register** — the interrupt
filter at `0xc065eed8` reads it, masks, and **writes the result back, so it is
write-1-to-clear**.

**Threading caveat that must not be got wrong.** Existing wake sources answer
"how many ticks until my next edge", which a host-delivered byte cannot. The
honest shape is `S5L_WAKE_NEVER` when the RX FIFO is empty and an immediate edge
when it is not — safe because the timer always bounds the sleep. And the
host→guest handoff must happen **on the CPU thread between run slices**, never
from a socket callback: `core/` has no threading vocabulary and a data race
there would be the worst bug this project could acquire.

**Named risk.** `setBaud` (`0xc065ea4c`) divides by a 64-bit `nclk` rate read
from the platform at `0xc065e44c`. `pppd` calls `cfsetspeed`, exercising that
path for the first time — today's boots only *identify* the ports. If our
unmodelled clock tree returns 0 there, expect a divide-by-zero rather than a
graceful message.

Requirements discovered from the acquired payload, each of which is a silent
breakage if missed: **setuid/setgid bits must survive** (`MobileCydia` 6755,
`bin/su` 4555, `var/local` 2775 gid=50); **89 symlinks** including `/etc`,
`/var`, `/tmp` into `private/`; and the volume ships `freeBlocks = 0`, so
`grow_volume` must run first.

## 23.10 Networking step S0: uart4, the plist hijack, and what a run must carry

The first half of `docs/networking.md`'s Route D is implemented. This section
records what was built, the three things it corrected in §23.5/§23.5.1, and the
exact command that reproduces it — because two of the corrections would
otherwise send the next reader down a path that does not exist.

**This is a temporary workaround and the code says so in three places** (the
`--ppp` help text, `rootfs_work.h`'s option comment, and `PPP_PLIST_STOCK`'s
provenance block). PPP over an emulated UART is what is cheap today because
both halves already ship. Real drivers and controllers replace it.

### What was built

- **`uart4` is a decoded device window**, 0x3cc10000, transmit-only, a second
  `s5l_uart_t` next to `uart0`. `SNAPSHOT_VERSION` 9 -> 10.
- **`--ppp`** rewrites `com.apple.chud.pilotfish.plist` in place, at exactly
  its own 530 bytes, into a `RunAtLoad` job running
  `/usr/sbin/pppd /dev/uart.debug local nocrtscts nodetach` with
  `StandardErrorPath = /dev/console`, and appends `uart4_dma_enable=0` to the
  boot arguments.
- **`uart4-ppp.bin`**, a per-run binary tee, plus an automatic scan for
  `7E FF 7D 23 C0 21` reported in a `=== UART4 / PPP ===` section on every
  armed run.

### Three corrections to §23.5 and §23.5.1

**1. The `setBaud` "named risk" is not a boot-killer, and the reasoning behind
it was inverted.** §23.5.1 warned that `pppd`'s `cfsetspeed` would exercise
`setBaud` (`0xc065ea4c`) "for the first time" and that a zero `nclk` would give
"a divide-by-zero rather than a graceful message". Both halves are wrong.

- `setBaud` is **vtable slot +0x374** and has **zero direct `bl` call sites**.
  `AppleOnboardSerial` reaches it from `start()` through `programHardware`,
  after storing a default of **19200 8N1** at `0xc047244a` — the only baud
  constant in either kext, and there is no device-tree baud property anywhere.
  So it runs **unconditionally at start, before any tty is opened**, and
  `pppd`'s speed argument adds nothing that was not already going to happen.
- The divide is `__udivdi3(nclk << 17, baud)` then `__udivsi3(q, 2*framebits)`,
  using helpers **compiled into the kext**, and the kext's own `__aeabi_idiv0`
  at `0xc065f55c` is a bare **`bx lr`**. A zero divisor returns garbage and
  **does not trap**. The consequence is a nonsense `UBRDIV`, not a panic — and
  this VM derives no baud rate from `UBRDIV`, so it is not even observable
  here.

  `nclk` itself is **not** a device-tree property and **not** an MMIO read by
  this driver: `0xc065e44c` loads the C string `"nclk"` at `0xc065fb40` and
  makes a virtual call on the provider nub (slot **+0x358**), which forwards up
  the provider chain (parent slot **+0x354**) into AppleARMPlatform's clock
  layer. The string `nclk` occurs **zero times** in `devicetree.bin`. Nobody
  has walked that chain to a register; do not claim one.

**2. `AppleS5L8900XSerial` is ARM, not Thumb.** `tools/kdisasm.py` defaults to
Thumb, so every address in §23.5.1 disassembles as garbage without `--arm`.
`AppleOnboardSerial` *is* Thumb-1. Exact ranges from `__PRELINK_INFO`:
`AppleOnboardSerial` `0xc046e000..0xc0479000`, `AppleS5L8900XSerial`
`0xc065d000..0xc0662000`.

**3. The window was undeclared, not merely unmodelled.** run59's census
recorded `0x3cc10000 r=8 w=15` falling through to the **unmapped** path, so
every `UTRSTAT` read answered 0 — "transmitter busy". A driver that waits for
room before storing would have waited forever. Decoding the window is what
makes a transmit path terminate at all; it is not a tidiness change.

Confirmed unchanged from §23.5.1, now read out of the tree rather than
inferred: uart4 carries `dma-types {3}`, a zero-length `no-flow-control`,
`interrupts {0x1c}`, `reg {0x04c10000, 0x1000}`, and — unlike uart0 — **no
`boot-console`**, so nothing contends for it. Its `debug` child has exactly two
properties, `name` and `AAPL,phandle`, and nothing else.

### The plist, byte-exactly

Stock `com.apple.chud.pilotfish.plist` lives at **offset 0xf5f000** in
`firmware/rootfs.img`, is **530 bytes**, and the full pattern occurs **exactly
once** in the whole 413 MiB image (verified by a full scan, not assumed).
SHA-256 of those 530 bytes:
`882ebd0b14088120b03750090ef9b6885a7b3bfbbe286df9ac23ecb431f55312`.

Its DOCTYPE says **"Apple Inc."** where the SpringBoard plist's says **"Apple"**
— two different Apple toolchains, and normalising either breaks the match.

The budget arithmetic in §23.5 is confirmed and its candidate ranking stands:
a fully-argumented job is **515 bytes**, so pilotfish's 530 is the only one of
the four inert candidates that fits. `chud.chum` at 515 would fit with **zero**
bytes of slack, which is not a margin.

### What a run must carry, and one trap that costs the whole run

**`nand-enable-adm=0` is mandatory.** Without it the boot panics at
**instruction 218,615,894** in `AppleS5L8900XADMFMC::start` —
`"ADM startup failed"`, called from `0xc04d679c`. This is not new and is not
related to PPP: **run72 died there too**, and so does a clean build of
`55ebb98` with no PPP code in it at all. It is reproducible to the exact
instruction across three different binaries. run71 has it in `-c` and never
reaches the ADM path.

Also note `-F` invalidates **`firmware/screen.ppm` relative to the process
working directory**, so a run launched with its own working directory needs a
`firmware/` subdirectory containing that file or it exits 1 before booting.

Reproduction, from PowerShell (**not** Git Bash — §23.4a explains why
`--fstab` dies there); written as one line per argument so it can be pasted
without continuation characters:

    $r = "F:\JOSHUA_1st_2021\projects\S5LBox"
    $d = "$r\work\run74-ppp"
    New-Item -ItemType Directory -Force "$d\firmware" | Out-Null
    Copy-Item "$r\firmware\screen.ppm" "$d\firmware\screen.ppm" -Force
    Push-Location $d
    & "$r\work\build-ppp\core\bootkernel.exe" "$r\firmware\kernel.macho" -d "$r\firmware\devicetree.bin" -F --usb-otg --ca-software-render --ppp -R 128 --grow 32 --external-md "$r\firmware\rootfs.img" "$d\rootfs-run74.img" --fstab "/dev/md0 / hfs rw,update 0 1" -c "debug=0x8 serial=1 nand-enable-adm=0" -n 850000000 1>"$d\run74.stdout.log" 2>"$d\run74.stderr.log"
    Pop-Location

**A cap of 850e6 is enough and 1.2e9 is waste.** `pppd` is spawned at
557,124,470 and, as run74 measured, is dead by 739,184,188. Everything S0 can
observe has happened by then; 850e6 is about twelve minutes of wall clock
against roughly eighteen for 1.2e9. Only raise it once `pppd` stops exiting.

### Measured, run73 (700e6 cap)

The pipeline is proven as far as the exec. In order:

| what | evidence |
|---|---|
| plist rewritten in the work image | `ppp : com.apple.chud.pilotfish.plist @ image+0x00f5f000` |
| boot argument appended | `cmdline "debug=0x8 serial=1 nand-enable-adm=0 uart4_dma_enable=0 rd=md0"` |
| window decoded, driver bound | `AppleS5L8900XSerial: Identified Serial Port on ARM Device=uart4 at 0x3cc10000(0xea9d6000)` |
| **launchd spawned our job** | `syscall 244 posix_spawn ... path "/usr/sbin/pppd"` **at instruction 557,124,470** |
| bytes on uart4 | **none by 700e6** |

That `posix_spawn` is the load-bearing observation: it proves the hijack
survived into the work image, that launchd parsed the rewritten plist, and that
`RunAtLoad` fired. run73 capped only 143e6 instructions later, which is not
enough for dyld to map a 284,608-byte binary and its frameworks, open the tty
and transmit.

### Measured, run74 (1.2e9 cap) — THE BLOCKER, AND WHAT IT IS NOT

**`pppd` runs, and then it calls `exit(1)`.** From the process-lifecycle
section:

```text
#65  @557124470  syscall 244 posix_spawn   path "/usr/sbin/pppd"
                 task/task-proc/pid c2e151d8/e03832cc/19
#84  @739184188  syscall 1   exit          args a0=00000001
                 task/task-proc/pid c2e151d8/e03832cc/19
#85  @739184282  _exit1 proc=e03832cc rv/status=00000100
```

So pid 19 lived for **182,059,718 instructions** — it was not a failed exec,
dyld mapped it and it ran — and then exited **1**. Zero bytes ever reached
uart4, across the whole 1.2e9.

**The exit code is itself evidence, and it rules out four things.** pppd 2.4.2
has distinct exit codes, and ONE is `EXIT_FATAL_ERROR`. It is therefore:

| not | which would mean |
|---|---|
| `EXIT_OPTION_ERROR` = 2 | the command line failed to parse |
| `EXIT_NOT_ROOT` = 3 | the job ran unprivileged |
| `EXIT_NO_KERNEL_SUPPORT` = 4 | `ppp_available()` said no — the line discipline is missing |
| `EXIT_OPEN_FAILED` = 7 | `/dev/uart.debug` could not be opened |

**So the devfs node exists under the predicted name, the argv parsed, the job
ran as root, and `com.apple.nke.ppp` answered.** Four of the nine unknowns S0
was supposed to settle are settled, by an exit code. What remains is a
`fatal()` call, and the two candidates the strings support are
`"Couldn't set tty to PPP discipline: %m"` (`TIOCSETD` with `PPPDISC`) and
`"Baud rate for %s is 0; need explicit baud rate"` — both confirmed present in
the image, once each.

**Why that could not be narrowed further in run74, and the fix.** `fatal()`
writes to stderr, and launchd gives a job with no `StandardErrorPath`
`/dev/null`. The message was destroyed. The job now carries
`StandardErrorPath = /dev/console`, so it lands in the same console capture
every other guest message does; the key is confirmed present in the image
rather than assumed supported. Two arguments were spent to afford the 61 bytes
— see `PPP_PLIST_JOB`'s comment for the trade, which is stated there rather
than made quietly, and note that dropping the explicit speed makes the
*second* candidate above self-diagnosing rather than hiding it.

### Reading the verdict without fooling yourself

`0x7E` is a byte, not a proof. The section reports three distinct outcomes and
they are not interchangeable:

- **`NOTHING was written to uart4`** — the guest never transmitted. This says
  nothing about *which* step failed.
- **`NOT the milestone`** — something opened the port and wrote to it. Compare
  the dump against RFC 1662 before concluding anything about `pppd`.
- **`*** MILESTONE ***`** — the exact six bytes, with the stream offset **and**
  the instruction they landed at. Both are printed because "it appeared" and
  "it appeared at a plausible point in the boot" are different claims.

The scan uses a six-byte sliding window rather than a match counter, because
`0x7E` starts every frame and a counter would miss `7E 7E FF 7D 23 C0 21` —
an idle flag followed by a frame, which is exactly what a real line looks like.

### Measured, run75 (850e6 cap) — the arguments are not the cause, and
### `StandardErrorPath` did not deliver the message

Same configuration, one change: `StandardErrorPath = /dev/console` added,
`115200` and `noauth` removed to pay for it. Two results, and the second is
more useful than the one that was being chased.

**1. The failure is invariant to those arguments.** Side by side:

| | spawn | exit(1) | `pppd` lifetime |
|---|---|---|---|
| run74 (`115200`, `noauth`) | 557,124,470 | 739,184,188 | 182,059,718 |
| run75 (neither) | 557,135,323 | 739,143,287 | 182,007,964 |

**51,754 instructions of difference out of 182 million — 0.028%.** Removing an
argument shortens option parsing by about that much and changes nothing else.
So `pppd` is failing at the *same place* with or without an explicit baud rate,
which retires the `"Baud rate for %s is 0"` hypothesis: that fatal would fire
only in the second row and would have moved the exit. Do not spend the 25
bytes putting the speed back on the strength of that theory — it has been
tested.

**2. `pppd` printed nothing, anywhere.** The console is byte-identical to
run74's across all 5,371 bytes both runs produced. So either launchd on 3.1.3
does not honour `StandardErrorPath`, or `pppd` never gets far enough to have a
usable stderr, or its `fatal()` on this build logs only through syslog. This
was worth trying — it is one plist key and the alternative was guessing — but
it did not work, and the honest reading is that **`pppd`'s message is not
reachable this way** rather than that `pppd` had nothing to say.

### The next step, concretely: probe the exit site

The lifecycle record carries a lead that costs one flag to follow. At the exit
syscall, register **r3 = `0x00039c30`**:

```text
#84  @739143287  syscall 1 exit   args a0=00000001 a1=0000000c a2=2ffffeec a3=00039c30
                 user pc 33ad7138 (ARM, spsr 00000010)
```

`a0 = 1` is the status. `user pc 33ad7138` is inside the dyld shared cache —
libSystem's `exit` thunk, i.e. every caller looks the same there and it is
useless as an identifier. But `0x00039c30` is in the **pppd image's own**
address range (the binary is 284,608 = `0x457C0` bytes and loads low), so it is
a `pppd` text address and almost certainly the neighbourhood of the `fatal()`
or `die()` that called `exit`.

So: **`--call-probe 0x00039c30`**, which captures `pc/lr/sp`, `r0`-`r3` and two
stack words at a user-mode PC. `lr` from that capture names `pppd`'s own caller
and turns "a fatal() somewhere" into an address to disassemble. Pair it with
`--call-probe` on a second candidate if the first is a thunk. A cap of 850e6 is
enough, so this is about twelve minutes per iteration.

The static side is cheap too and has not been done: `pppd` is at
`/usr/sbin/pppd` in the image and can be extracted and disassembled directly
(`tools/kdisasm.py --arm`), so `0x39c30` can be resolved to a function and its
string references read before any run is spent. Both `"Couldn't set tty to PPP
discipline: %m"` and `"Baud rate for %s is 0; need explicit baud rate"` are
present in the image exactly once, so their addresses are findable and can be
compared against whatever `0x39c30` turns out to be near.

What is already settled and should not be re-litigated: the plist hijack, the
boot argument, the devfs node name, launchd starting the job, AMFI accepting
the binary, dyld loading it, and `com.apple.nke.ppp` answering — the first six
of the nine unknowns S0 was posed to settle.

### 23.10a `pppd` disassembled: the blocker is `tty_establish_ppp`, and
### `0x39c30` is not a string

The static work proposed above was done, and it answers the question without a
run. `/usr/sbin/pppd` extracted from `work/run75-ppp-log/rootfs-run75.img` with
`tools/hfsx_extract.py` is 284,608 bytes, `MH_MAGIC` `cputype 12 cpusubtype 6`
(armv6), **not stripped** — 576 defined symbols, and **zero** carry
`N_ARM_THUMB_DEF`, so the whole `__text` decodes as ARM. That symbol table is
what makes everything below cheap; disassemble with it, not without it.

**`0x00039c30` is `fd_ppp`, not a message.** `__cstring` ends at `0x00035d9e`;
`0x39c30` is in `__DATA,__data`, its file image is `ffffffff` (`= -1`), and the
whole binary references it in exactly two places:

- `_die+0x10` (`0x00013a1c`) — `if (fd_ppp >= 0) the_channel->disestablish_ppp(devfd)`
- `_main+0xc6c` (`0x00015684`) — `ldr sl, [pc, #0x45c]`, held across the store
  at `_main+0xca8`: `fd_ppp = the_channel->establish_ppp(devfd)`

That is `fd_ppp`'s definition, use and initialiser exactly. The lead was worth
following and it did not lead where §23.10's last block guessed.

**The exit is `die(EXIT_FATAL_ERROR)`.** `_fatal` (`0x00023ad0`) ends in
`mov r0,#1; bl _die`, and `_die` (`0x00013a0c`) ends in `mov r0,r5; bl _exit`.
Every `exit(1)` in the image is either that or `_load_kext+0x6c`, and
`load_kext` `fork()`s first — **pid 19 never forked** (the run75 lifecycle ring
retained 92 of 92 events, so it is complete), so `load_kext` never ran. Two
things follow immediately: `ppp_available()`'s
`socket(PF_PPP /*34*/, SOCK_RAW, PPPPROTO_CTL)` at `_ppp_available+0x24`
**succeeded** — it only forks when that fails — so `com.apple.nke.ppp` really
does answer; and no connector/initializer/welcomer script ran either.

**`status = 1` has exactly one reachable writer.** Scanning every reference to
`_status` (`0x00040418`, reached through the non-lazy pointer at `0x000373c8`)
finds 36 stores. Only two store `1`:

- `_connect_tty+0x9c` (`0x000207fc`), guarded by `using_pty || record_file` —
  unreachable with this argv, which names a real device
- `_main+0xcb0` (`0x000156c8`), immediately after
  `fd_ppp = the_channel->establish_ppp(devfd); if (fd_ppp < 0)`

`the_channel` is `tty_channel` at `0x0003ae2c` — `+0xc = _connect_tty`,
`+0x14 = _tty_establish_ppp`, `+0x24 = _cleanup_tty`, the standard `struct
channel` layout. **So `_tty_establish_ppp` (`0x0001c528`) returned negative.**

### A per-process execution trace, from page faults, at zero run cost

`=== DISTINCT ABORT SITES ===` is a symbolisable trace of userspace, and this
had not been used. Each record carries `L1=`, the L1 descriptor for the faulting
VA — a *per-address-space* value, which is what separates one process's
low-address faults from another's. `pppd`'s is `0x0b139001` in both run74 and
run75 (identified by the `IFETCH FAR 0x00002334` right after the spawn:
`0x2334` is `start`, pppd's entry point). Filtering on it and symbolising
against the extracted binary gives 98 records for run75. The tail:

```text
IFETCH @731392297  0x000040b4  _auth_check_options
IFETCH @731778474  0x0001ce7c  _sys_init
IFETCH @735408996  0x000294ac  _acsp_init_plugins
IFETCH @735607256  0x00020760  _connect_tty
IFETCH @735794554  0x0001bf80  _set_up_tty
IFETCH @738641962  0x0001a6ec  _sys_cleanup      <- first call inside die()
       @739143287  exit(1)
```

run74 is the same sequence, offset by ~37k instructions. Read it carefully: a
missing entry proves nothing (`_tty_establish_ppp` at `0x1c528` shares page
`0x1c000` with `_sys_init`, already resident since 731.8M), but a *present*
entry is proof the function was entered. So `sys_init` ran to completion — none
of its six `fatal()`s fired, including `"Couldn't open PF_PPP: %m"` and
`"SCDynamicStoreCreate failed: %s"` — and `connect_tty` reached `set_up_tty`.

**Reuse this.** `L1=` plus an unstripped guest binary turns the abort table into
a function-entry trace for any process, retroactively, on logs already on disk.

### Both of `set_up_tty`'s `fatal()`s are excluded

`_set_up_tty` (`0x0001bf80`) has two, and one non-fatal early return that has
been mistaken for one:

| site | call | condition |
|---|---|---|
| `0x0001bfd0` | `error("tcgetattr: %m")` | **returns**, does not die |
| `0x0001c12c` | `fatal("Baud rate for %s is 0…")` | `inspeed == 0 && cfgetospeed() == 0` |
| `0x0001c164` | `fatal("tcsetattr: %m")` | `tcsetattr(fd, TCSAFLUSH, &tios) < 0` |

The baud fatal is excluded by construction, not by the 0.028% argument above:
run74's argv carries `115200`, so `inspeed != 0`, so the branch at `0x0001c0ec`
takes `cfsetospeed`/`cfsetispeed` and the fatal is *unreachable in that run* —
and run74 still exits 1, at the same place, with the same `r3`. run75 takes the
same path: between `_set_up_tty` and `exit` the two runs differ by **3,905
instructions out of 3.35 million (0.12%)**, which is not enough to contain a
skipped `tcsetattr` and everything after it.

And `tcsetattr` ran. The uart4 register census is the witness:

| run | cap | `0x3cc10000` | reached |
|---|---|---|---|
| run73 | 700e6 | `r=8 w=15` | died at the cap **before** `connect_tty` (735.6M) |
| run74 | 850e6 | `r=41 w=100` | `set_up_tty` |
| run75 | 850e6 | `r=41 w=100` | `set_up_tty` |

`r=8 w=15` is the "identified, never opened" baseline — uart1 reads exactly
`r=8 w=15` and uart3 `r=10 w=17` in every run. The +33/+85 in run74/run75 is the
`open()` and the termios programming, and it is **byte-identical** between a run
that asked for 115200 and one that asked for whatever the port reported. The
port was opened and programmed; `set_up_tty` returned.

### The message, and the honest limit of what the binary can say

`_tty_establish_ppp` returns `-1` from five places. All five call `error()`, not
`fatal()` — which is why exit code 1 arrives with no `fatal()` anywhere, and why
chasing `fatal()` call sites was the wrong search. In execution order:

| # | string VA | message | guard |
|---|---|---|---|
| 1 | `0x00032a34` | `Couldn't set tty to PPP discipline: %m` | `ioctl(tty_fd, TIOCSETD 0x8004741b, &disc)`, `disc = PPPDISC = 5` (14 if `sync_serial`), failing with `errno != EIO` |
| 2 | `0x0003296c` | `Couldn't get link number: %m` | `ioctl(tty_fd, PPPIOCGCHAN 0x40047437, &chindex)` |
| 3 | `0x0003299c` | `Couldn't reopen PF_PPP: %m` | `socket(PF_PPP,SOCK_RAW,PPPPROTO_CTL)` + `connect()` in the helper at `0x0001c2b0` |
| 4 | `0x000329b8` | `Couldn't attach to the ppp link %d: %m` | `ioctl(sock, PPPIOCATTCHAN 0x80047438, &chindex)` |
| 5 | `0x00032a10` | `Couldn't attach to PPP unit %d: %m` | `ioctl(fd_ppp, PPPIOCCONNECT 0x8004743a, &ifunit)` |

Each string occurs exactly once in the image. **#3 is excluded** — the identical
`socket(PF_PPP,…)` already succeeded twice (in `ppp_available`, proved by the
absent fork, and in `sys_init`, proved by `sys_init` returning). **#1 is the
first and the one every later step presupposes**, and is the single most likely
answer. But say plainly what the binary cannot: it cannot distinguish #1 from
#2/#4/#5, because all four are `error()` calls on already-resident pages and
leave no trace the run logs captured.

### What must hold, named exactly — and it is not a device model

Nothing here is a missing register, a device-tree property or a file. The four
operations that must succeed on the fd for `/dev/uart.debug` are:

```text
TIOCSETD        0x8004741b   _IOW('t',27,int)   value 5 (PPPDISC)
PPPIOCGCHAN     0x40047437   _IOR('t',55,int)
PPPIOCATTCHAN   0x80047438   _IOW('t',56,int)
PPPIOCCONNECT   0x8004743a   _IOW('t',58,int)
```

All four are served by `com.apple.nke.ppp`, and that kext is present and armed
in this exact kernelcache: `pppserial_ioctl: … TIOCSETD` / `… PPPIOCGCHAN = %d`
are at `0x005583fc`+ in `firmware/kernel.macho`, and the **only** reference to
`&linesw[PPPDISC]` (`0xc0221b4c`) in the entire 7.9 MB image is the literal at
`0xc05925c0`, loaded by an eight-word `ldm/stm` structure copy at
`0xc0592554`-`0xc0592568` — i.e. `linesw[PPPDISC] = pppdisc`, by direct
assignment rather than `ldisc_register` (which has zero call sites anywhere).
`nlinesw` is 8, so `PPPDISC = 5` also passes `ttioctl`'s range check.

**This is a small change or none at all, and that is the finding.** There is no
new emulated device between here and the LCP frame. The blocker is one of four
ioctls inside a kext that is loaded and whose sibling entry point (the PF_PPP
socket) already works — so the fix is either a one-line device-tree/`ioctl`
detail or nothing, and the remaining cost is *identification*, not construction.
Do not size S0's remainder as a driver.

### Why run75's console was empty, and the one-key fix

`StandardErrorPath` was aimed at a file descriptor `pppd` never writes to.
`error()`/`fatal()` funnel into the formatter at `0x000230d0`, which calls the
emitter at `0x0002245c`, and that function does exactly two things: `syslog()`
unconditionally, then — if `*log_to_fd >= 0` — a timestamp and the message with
`write(*log_to_fd, …)`. `_log_to_fd` is at `0x00039c70` and its file image is
**`00000001`, i.e. stdout**, and `nodetach` means nothing lowers it. fd 2 is
never touched.

So the next run's plist needs **`StandardOutPath`**, not `StandardErrorPath` —
one key, same 530-byte budget, and it puts the exact string from the table above
on the console and collapses the four-way ambiguity to one. That is the whole
measurement: *which of those four `error()` strings `pppd` prints*. Nothing else
needs to change, and no probe or new flag is required.

### 23.10b run78 answered it: ENOTTY, because `/dev/uart.debug` is a character
### device that is not a tty

`StandardOutPath` worked. run78's console carries `pppd`'s own words:

```text
Wed Dec 31 16:00:06 1969 : set_up_tty, can't set controlling terminal: Inappropriate ioctl for device
Wed Dec 31 16:00:06 1969 : Couldn't set tty to PPP discipline: Inappropriate ioctl for device
```

Candidate #1 from 23.10a, and a second failure ahead of it. Both are **ENOTTY**.
Everything else repeats exactly: pid 19, `exit(1)`, `r3 = 0x00039c30`, uart4
`r=41 w=100`.

**Read the second line as the only blocker.** `set_up_tty`'s `TIOCSCTTY` is
`error()`, not `fatal()` — `0x0001bfa0` calls it, `0x0001bfb0` prints, and
`0x0001bfb4` falls straight through to `tcgetattr`. It has never been in the
failure path. Do not spend a run on it.

### The origin of ENOTTY, exactly

`/dev/uart.debug` is **not** an `IOSerialBSDClient` tty. `IOSerialFamily`
(prelinked `0xc0464000+0xa000`) builds its names from `'/dev/tty.'`
(`0xc046b164`) and `'/dev/cu.'` (`0xc046b158`). The node `pppd` opened comes
from a different kext: `com.apple.driver.AppleOnboardSerial`, prelinked at
`0xc046e000+0xb000`, whose `AppleOnboardSerialBSDClient::start` does

```text
c0470492  movs r0, #1 ; rsbs r0,r0,#0     ; index = -1, allocate any major
c0470496  ldr  r1, [pc] -> 0xc0478060     ; &cdevsw
c047049a  blx  _cdevsw_add                ; c015473c
c04704f6  … getProperty("IOTTYBaseName")  ; 0xc047500c
c047050a  … getProperty("IOTTYSuffix")    ; 0xc0475000
c047055a  ldr  r3, [pc] -> 'uart.%s%s'    ; 0xc047520c, mode 0666 at c0470554
```

so the name is literally `uart.` + base + suffix. That cdevsw at `0xc0478060`
is the whole answer:

| field | value | |
|---|---|---|
| `d_open`/`d_close`/`d_read`/`d_write` | `c046fad5`/`c046faf7`/`c0470419`/`c046fb19` | real |
| `d_ioctl` | `c0470341` | real |
| `d_stop`/`d_reset`/`d_select`/`d_mmap` | `c0130b2d` | `_enodev` |
| **`d_ttys`** | **`00000000`** | no `struct tty` |
| **`d_type`** | **`00000000`** | not `D_TTY` (3) |

`d_ioctl` → `c0470340` (look up the client, else `ENXIO`) → `c047032c` →
**`c046fd52`**, a flat switch on the command. It recognises exactly `TIOCEXCL`
/`TIOCNXCL` (via `+0xdfff8bf3, <=1`), `TIOCCDTR`/`TIOCSDTR`/`TIOCCBRK`/`TIOCSBRK`,
`TIOCFLUSH`, `TIOCGETA`, `TIOCSETA`/`SETAW`/`SETAF` (via `+0x7fd38bec, <=2`),
`TIOCMGET`/`MBIS`/`MBIC`/`MSET`, `FIONBIO`, `FIOASYNC`, and four private
`_IOW('T', n, int)` commands. Everything else falls to

```text
c046fe30  movs r0, #0x19        ; ENOTTY = 25
c046fe32  b    c04702a0         ; return
```

`TIOCSCTTY` (`0x20007461`) and `TIOCSETD` (`0x8004741b`) are not in that table.
**That is the address the two console lines come from.**

And the clincher, because absence usually proves nothing: `_ttioctl`'s Thumb
pointer `0xc01368a9` occurs **exactly once in the whole 7.9 MB kernelcache**, at
`0xc0469430` — inside `IOSerialFamily`. `AppleOnboardSerial` contains no
reference to it at all. `/dev/uart.debug`'s ioctl path cannot reach the BSD tty
layer, by construction, so `ttioctl` never runs, `linesw[PPPDISC]` is never
consulted, and no session-leader check is ever reached.

So of the four hypotheses: it is **(a)** — the fd is not a tty — with **(b)**'s
mechanism, the driver's own `d_ioctl` default arm, as the proximate return.
**(c) is excluded**: `TIOCSCTTY` never reaches any session logic, and its failure
is non-fatal to `pppd` regardless.

Two things this also explains rather than leaves open. `open()`, `tcgetattr()`
and `tcsetattr()` all succeeded because `TIOCGETA` and `TIOCSETA*` *are* in the
table — which is precisely why uart4 reads `r=41 w=100` against a `r=8 w=15`
"identified but never opened" baseline. And `default_device` stayed 0, so
`setdevname()` stat'd the node and `S_ISCHR` passed: the node exists and is a
character device. Both were previously inferences; they are now consequences.

### Correction: the `_cttyopen` burst is a symbolisation artifact

`_cttyopen+0x48` (`c013a0dc`) in the last-200-instructions trace at ~849.96e6 is
not `cttyopen`. `_cttyopen` is Thumb, `c013a094..c013a0db`, and ends with `pop
{r4,r5,r6,r7,pc}` at `c013a0da`; **`c013a0dc` begins a new, unnamed static** —
`push {r4,lr}` then a 64-bit range-containment test — called in a loop from
`_ubc_cs_getcdhash` and `_cs_validate_page`, with which it interleaves in the
trace. It is code-signing work, 111 million instructions after `pppd` died, and
the kernel's symbol table simply does not name it. Nearest-preceding-symbol
labels in these logs are a hint, never a fact.

### What has to change on our side: nothing

No device register, no device-tree property, no cdevsw of ours, no ioctl the
emulated UART must answer. Every emulated thing worked; the ENOTTY is real,
unmodified Apple kext code behaving correctly on a node that was never meant to
carry a line discipline. `/dev/uart.debug` is the debug console UART's raw
character interface — `AppleOnboardSerialBSDClient` also registers a kernel
control socket `com.apple.uart.%s` (`0xc0475260`, via `ctl_register` at
`0xc0470616`), which is the other half of how Apple expects that port to be used.

The change is to `pppd`'s invocation. Two candidates, in cost order:

1. **`/dev/tty.debug`** — one byte *shorter* than the current argv, so it fits
   with slack to spare. `AppleOnboardSerialSync`'s metaclass constructor
   (`0xc0470952`) passes superclass `0xc046d280`, which is inside
   `IOSerialFamily`'s image, and the kext declares `com.apple.iokit.IOSerialFamily`
   as a library — so the nub is an `IOSerialFamily` stream class, and
   `IOSerialBSDClientSync` (`IOProviderClass = IOSerialStreamSync`, probe score
   1000, `IOMatchCategory` distinct from `AppleOnboardSerialBSDClient`'s) could
   attach to the same nub and publish `/dev/tty.<base><suffix>`. **Unproven** — I
   could not name the class at `0xc046d280`, and `IOSerialFamily` does not appear
   in run78's top-10 kext time table, which is truncated and therefore not
   evidence of absence.
   `pppd` is its own probe here, at zero extra cost: if the node does not exist,
   `setdevname()` returns 0, `default_device` stays 1, and
   `tty_process_extra_options+0x68` (`0x00021628`) prints **`no device specified
   and stdin is not a tty`** and exits **2** — a different console line and a
   different exit code from today's 1. One run answers it either way.

2. **`notty`** — works whether or not any tty node exists. `pppd` then calls
   `get_pty` (`0x0001bd70` → `openpty@stub`), which gives it a **pty slave**: a
   real BSD tty, so `TIOCSETD`/`PPPDISC` applies there, and `pppd` shuttles bytes
   between the pty master and its own stdin/stdout with plain `read`/`write`,
   which the raw cdev fully supports. Costs a `StandardInPath` key alongside the
   `StandardOutPath` already added, and puts `pppd`'s log output onto the serial
   line interleaved with the PPP frames.

Small either way — it is an argv/plist edit and no emulator code. The residual
risk is that both fail, which would mean the built-in tty channel is unusable on
this device and `pppd` needs Apple's `PPPSerial` plugin instead. That would be a
larger job, but it still would not be an emulator job.

### Step S1, when someone takes it

S0 is transmit-only *by design*, and the header comment in `soc.h` says what
changes when that stops being true. The receive direction needs, in this order:
a bounded host-to-guest queue; `UFSTAT`'s receive count and `UTRSTAT` bit 0
becoming real; `UTRSTAT` growing a genuine write-one-to-clear latch; VIC line
**28** becoming a wired `S5L8900_IRQ_UART4` and a wake-source entry; and the
host-to-guest handoff happening **on the CPU thread between run slices** —
§23.5.1 is right that a socket-callback race there would be the worst bug this
project could acquire. `core/tests/test_uart4.c`'s
`test_uart4_raises_no_interrupt_line` is the test that must be rewritten rather
than deleted at that point.

Only after that does the host-side LCP/IPCP peer (RFC 1661/1332/1662) become
worth writing, and `pppd` will Protocol-Reject-test us first: it sends CCP
(0x80FD) by default with bsdcomp and deflate, and IPV6CP, Apple ACSP and ECP
protents are all compiled in.
