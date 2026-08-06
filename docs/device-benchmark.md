# Jailbroken iPhone core benchmark

The manual `ios-device-benchmark` workflow builds the existing `insnbench`
harness as an arm64 iPhoneOS command-line executable. It is intended for a
maintainer-controlled jailbroken device and is not installable or runnable on
stock iOS.

The build deliberately matches the performance-relevant app policy: `-O3`,
LLVM LTO, the full non-JIT core source graph, and the build-time-signed static
AArch64 engine with its app-default graph and direct-write contract. It links
no dynamic recompiler source and requests no JIT entitlement. The artifact
contains synthetic instruction loops only; it contains no firmware, rootfs,
machine image, snapshot, device address, pairing record, or credential.

After downloading the artifact, copy it to a user-owned path on the test phone.
If the phone rejects the hosted ad-hoc signature, re-sign that copy with the
phone's rootless `ldid` before execution. A fast validation run is:

```sh
./s5lbox-insnbench-ios-arm64 \
  --filter tick=run --insns 1000000 --reps 1
```

For a sustained comparison, increase the instruction count and use at least
three interleaved repetitions. Preserve the complete output and the artifact
SHA-256; the harness verifies exact retired counts and architectural end state
before reporting a rate.

## What the number means

The `tick=run` rows cross the same app-facing `s5l8900_run()` entry point and
the same static execution graph as the iOS app. Running them on the target A9
measures a real device/core boundary that desktop and hosted-Mac results cannot.

It is still **not phone FPS**. The harness uses a synthetic 16 MiB machine and
omits firmware, disk I/O, the real iPhone OS workload, framebuffer publication,
UIKit, Core Animation, touch, and thermal behavior during a foreground run. A
high result proves only that raw no-JIT core execution is not the immediate
ceiling; a low result identifies core throughput as a real device bottleneck.
Final performance claims still require the unlocked app's changed-frame counter
and simultaneous device CPU/compositor telemetry.
