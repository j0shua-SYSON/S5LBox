# Jailbroken iPhone core benchmark

The manual `ios-device-benchmark` workflow builds the existing `insnbench`
harness as an arm64 iPhoneOS command-line executable. It is intended for a
maintainer-controlled jailbroken device and is not installable or runnable on
stock iOS.

The build deliberately measures the signed-static AArch64 candidate under
`-O3`, LLVM LTO, the full non-JIT core source graph, and its direct-write
contract. It is a diagnostic experiment, **not a mirror of the current app
default**. The shipping app still compiles the signed engine and native handler
assembly, but defaults to the interpreter because the candidate was a severe
regression in a physical-device full-guest replay. It retains the separately
measured direct-RAM-write contract. The benchmark links no dynamic recompiler
source and requests no JIT entitlement. Its artifact
contains synthetic instruction loops only; it contains no firmware, rootfs,
machine image, snapshot, device address, pairing record, or credential.

After downloading the artifact, copy it to a user-owned path on the test phone.
Verify its SHA-256 before changing it, inspect the embedded signature, and add
that exact CDHash to Dopamine's dynamic trust cache as root. The artifact is
ad-hoc signed with the three CLI entitlements in the accompanying
`entitlements.plist`:

- `platform-application`
- `com.apple.private.security.no-container`
- `com.apple.private.skip-library-validation`

This is a privileged jailbreak-only executable. None of those keys grants a
JIT or writable-executable memory. If a specific phone rejects the hosted
signature and re-signing is genuinely necessary, preserve the same contract:

```sh
ldid -Sentitlements.plist -Cadhoc ./s5lbox-insnbench-ios-arm64
```

Recompute the file SHA-256 and CDHash after signing, then admit only that new
CDHash. Plain `ldid -S` is not equivalent: on the tested Dopamine device it
produced CodeDirectory flags `none`, stripped the required CLI entitlements,
and the kernel killed the trust-cached binary with status 137 before `main`.

A fast validation run is:

```sh
./s5lbox-insnbench-ios-arm64 \
  --filter tick=run --insns 1000000 --reps 1
```

For a sustained comparison, increase the instruction count and use at least
three interleaved repetitions. Preserve the complete output and the artifact
SHA-256; the harness verifies exact retired counts and architectural end state
before reporting a rate.

## What the number means

The `tick=run` rows cross the same app-facing `s5l8900_run()` entry point while
selecting the signed-static candidate graph. Running them on the target A9
measures a real device/core boundary that desktop and hosted-Mac results cannot,
but it does not measure the interpreter selected by the shipping app.

It is still **not phone FPS**. The harness uses a synthetic 16 MiB machine and
omits firmware, disk I/O, the real iPhone OS workload, framebuffer publication,
UIKit, Core Animation, touch, and thermal behavior during a foreground run. A
high result proves only that raw no-JIT core execution is not the immediate
ceiling; a low result identifies core throughput as a real device bottleneck.
Final performance claims still require the unlocked app's changed-frame counter
and simultaneous device CPU/compositor telemetry.

## Proven A9 checkpoint

On an iPhone 6s Plus (A9, iOS 15.8.5), the hosted-signed no-JIT artifact from
commit `06d9d5f` was admitted by its exact CDHash without phone-side re-signing.
A 1M-instruction smoke passed; the sustained run used 20M guest instructions
per row and three interleaved repetitions:

- ALU/branch: 85.010 M guest instructions/s median
- load/store: 66.330 M guest instructions/s median

Both rows retired the requested counts, passed their architectural end-state
checks, and ended with `failures=0`. This is substantial physical-device core
evidence, but it is still not an iPhone OS frame-rate measurement.

That synthetic result did **not** predict the real guest. Commit `fe1b45c`
added a same-binary interpreter control to the firmware-backed iPhone replay.
From the same retained checkpoint, two 100M-instruction interpreter controls
measured 6.524984 and 6.529058 Minsn/s; the signed graph between them measured
only 0.963142 Minsn/s, about 6.78 times slower. All three reached the same guest
machine state, and the graph and interpreter produced byte-identical screen
captures on the phone. The graph reported 80.91% native retirement, so merely
entering generated native handlers is not proof of a speedup. The replay's
diagnostic bus also revokes direct RAM writes, whereas this synthetic benchmark
grants that narrower contract.

Commit `94ed9df` then closed that caveat by adding an exact canonical-bus replay
that restores the app's callbacks and direct-write consent before timing. Over
100M instructions, graph plus direct writes reached only 1.125446 Minsn/s.
Repeated interpreter-plus-direct controls reached 6.951659 and 6.937394
Minsn/s, while an interleaved interpreter-without-direct control reached
6.668303 Minsn/s. All four produced the same final work-image and screen hashes.
The graph is therefore a proven 6.17x regression even under the app bus, while
direct writes provide a smaller but repeatable 4.14% interpreter gain. Product
policy disables only the graph and keeps the direct-write contract.

This contradiction is why the workflow remains useful as a candidate-engine
microbenchmark but no longer defines product policy. A signed-engine change
must now win a firmware-backed replay on physical hardware before it can be
made the app default again.
