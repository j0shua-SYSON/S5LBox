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
firmware, that machine runs the built-in test guest exactly as the UI says.

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

## What this does not prove

This is startup automation, not unrestricted remote control. It does not inject
arbitrary touches, dismiss system dialogs, import firmware, reset app data or
select a different machine. A successful launch proves only that the requested
machine reached the emulator screen and attempted to start. FPS still has to be
measured from target-device frame publication and graphics telemetry; neither a
screenshot nor a successful process launch is an FPS result.
