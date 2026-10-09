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
That connection failure is now resolved. The 7E18 initialization sequence never
clears DCTL.SDIS after core reset; using the older integration's connected reset
state, and waiting for DMA/reset-interrupt readiness before issuing reset,
produced real enumeration at 6,567,000,000 retired instructions. Guest descriptors
reported VID/PID `05ac/1292`, configuration 3, bulk IN 3 / OUT 2. This reset value
is inferred from the actual driver sequence and cross-checked against the older
S5L8900 model, not measured from physical silicon.

The real guest also negotiated usbmux v2 and answered `QueryType` over a TCP-like
stream to port 62078: `com.apple.mobile.lockdown`, 346 framed reply bytes. Host
fixtures live under `work/usb-live-validation/lockdown-04.*` (not committed).
Two failed assumptions are covered by regressions: the guest's v2 reserved
signature is zero, not the host-to-device `feedface`; control warning/info
messages are not fatal transport errors.

The portable multiplexing layer has four bounded streams, fixed-scale windows,
fragment reassembly, sequence/ACK validation, cancellation, and bulk ZLP framing.
The native bridge uses nonblocking local socket pairs so service/TLS work runs
on a worker without touching emulator-owned state. Pairing and TLS client code
is under development and is not an installation-success claim.

Next gates: native service-layer build/tests; lockdown pairing/session;
AFC upload to `PublicStaging`; installation_proxy
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
