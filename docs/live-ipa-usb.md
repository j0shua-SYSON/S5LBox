# Live IPA installation

Work branch: `feature/live-ipa-usb`. This is not yet an available installer.
The IPA library remains in `Documents/IPAs`; importing a file must never
automatically install it. The target is one app at a time, through the running
guest's own AFC and `com.apple.mobile.installation_proxy` services.

## Current boundary

- The old USB model only supplied configuration registers. The new opt-in
  device-mode DWC2 model handles RAM DMA, EP0 SETUP/data/status, bulk packets,
  short transfers, ZLPs, NAK/STALL, endpoint interrupts and VIC0 line 19.
- `usb_host` requests descriptors from the guest and selects its usbmux
  interface. It contains no fabricated descriptors and does not edit HFS.
- The virtual cable drives both DWC2 B-session-valid and PCF50635 USB supply
  status/insertion/removal events. No physical USB or global host driver is used.
- The app's existing experimental USB OTG option enables this only on a fresh
  boot. Default remains off while firmware validation is incomplete.
- Snapshot v33 includes all modeled USB registers and DMA cursors. V32 has an
  exact migration: its sole USB register is retained, all new state is disabled.
  Restoring an old running guest does not acquire a new device tree.
- The host session is not a snapshot. The app reconnects its virtual cable on
  restored USB guests; future upload/service requests must fail on that boundary,
  never silently resume a half-completed installation.

## Validation, not claims

Unit coverage includes guest RAM bounds, real IRQ routing and W1C behavior,
multi-packet DMA, control phases, snapshot continuation, v32 migration and a
small independent synthetic USB gadget for host-enumerator protocol tests.
Synthetic enumeration does not prove Apple firmware enumeration.

The first real 7E18 host run retired 4 billion instructions without a CPU error
and started `AppleSynopsysOTGCore`, but never connected: GAHBCFG/GINTMSK remained
zero and DCTL remained soft-disconnected. That run predated the PMU supply path.
No successful guest descriptor exchange or live installation has been observed.

Next gates: stock firmware enumeration; usbmux version/TCP transport;
lockdown pairing/session; AFC upload to `PublicStaging`; installation_proxy
progress/Complete; then guest icon and launch. Signing/provisioning rejection
must be surfaced honestly. Kernel jailbreak support alone does not prove that
the installation service will accept every unsigned IPA.

## References

- Synopsys DWC2 register definitions in Linux `drivers/usb/dwc2/hw.h`.
- iDroid openiBoot `plat-s5l8900/pmu.c` and `includes/hardware/pmu.h` for
  USB supply detection, cross-checked with PCF5063x register definitions.
- libimobiledevice/usbmuxd `usb.c` and `device.c` for the wire protocol, and
  ideviceinstaller for the AFC/installation_proxy service sequence.

Reference implementations are not vendored into this MIT-licensed code.
