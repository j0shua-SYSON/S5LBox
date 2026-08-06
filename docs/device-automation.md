# iOS device automation

S5LBox has one narrow, opt-in launch hook for repeatable physical-device
profiling:

```text
--s5lbox-automation-open-first-machine
```

When that exact process argument is present, the app opens the first configured
machine after its root navigation controller appears. It calls the same method
as a real row tap, so instance selection, the opened timestamp, persistent work
image selection and engine startup do not have a second automation-only path.
A fresh app container creates its normal initial machine first. With no imported
firmware, that machine runs the built-in test guest exactly as the UI says. The
same opt-in launch disables the app process's idle timer so a long profile does
not become a lock-screen sample; ordinary launches keep the user's Auto-Lock
setting.

The first firmware launch normally prepares a machine-specific writable root
filesystem while the built-in test guest runs, then asks the user to reopen the
machine. In automation mode only, a bounded setup observer waits for that same
provisioner to report success, stops the test guest, waits until its emulator
thread has actually exited, and reopens the same first machine through the same
list-controller path. A provisioning refusal or failure is logged and is never
turned into a firmware launch. The observer samples at 0.5-second intervals and
stops after 30 minutes. Frame work is recorded at the two measured pipeline
boundaries rather than by doing Objective-C work on every frame.

There is also a non-driving observation path for tools that cannot supply launch
arguments. When Developer Mode is already enabled at app launch, S5LBox enables
the same frame telemetry and publishes it through the guest display's
accessibility value and a one-point transparent accessibility label. The latter
exists because compact accessibility clients can omit a custom view's value; it
does not accept touches or draw visible text. The observer attaches after a
person or accessibility client opens any machine normally. It never opens,
stops, reopens, provisions or otherwise steers a machine, and it does not disable
Auto-Lock. Turning Developer Mode on takes effect for this purpose on the next
app launch.

For example, after mounting the matching Developer Disk Image:

```powershell
pymobiledevice3 developer dvt launch --udid DEVICE_UDID `
  "com.j0shua.S5LBox.MBXExperiment --s5lbox-automation-open-first-machine"
```

The normal bundle ID is `com.j0shua.S5LBox`. The isolated MBX experiment uses
`com.j0shua.S5LBox.MBXExperiment`, so both builds can remain installed without
sharing application data.

The machine list also exposes stable accessibility identifiers:

- `s5lbox.machines.list`
- `s5lbox.machines.add`
- `s5lbox.machines.settings`
- `s5lbox.machines.edit`
- `s5lbox.machine.<persistent-instance-id>`
- `s5lbox.emulator.root`
- `s5lbox.emulator.screen`
- `s5lbox.emulator.status`
- `s5lbox.emulator.console`
- `s5lbox.emulator.keys`
- `s5lbox.emulator.toolbar`

With telemetry enabled, `s5lbox.emulator.screen` has an accessibility value and
the transparent diagnostic label has accessibility text beginning with
`frame_pipeline_v1`. They distinguish guest scanout attempts and changed
scanouts from UIKit layer submissions and rejected submissions. A layer
submission is not proof that iOS displayed the frame; the record states that
limitation explicitly. Updating this diagnostic string at 0.5 Hz is additional
Developer Mode work and is not present in an ordinary launch.

## What this does not prove

This is startup automation and observation, not unrestricted remote control. It
does not inject arbitrary touches, dismiss system dialogs, import firmware,
reset app data or select a different machine. A successful launch proves only
that the requested machine reached the emulator screen and attempted to start.
The accessibility value reports pipeline counters, not visible display FPS: a
screenshot, process launch, or accepted layer submission is not proof that the
device compositor displayed a new frame.

The hook also cannot bypass the iOS lock screen. A jailbreak can provide SSH
file and process access while locked, but SpringBoard still refuses a foreground
application launch and a locked compositor cannot be used as an FPS result.
