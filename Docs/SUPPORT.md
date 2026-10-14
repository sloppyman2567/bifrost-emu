# Platform and compatibility support

This document separates supported configurations from configurations that
are only expected to work. The emulator is an evolving user-mode emulator;
passing a focused test is not a claim of complete Linux ABI compatibility.

## Current support matrix

| Area | Supported / checked | Limits |
|---|---|---|
| Host | Linux x86_64; pull-request CI builds on Ubuntu 24.04 | Other Linux distributions are not release-gated. Non-Linux and ARM hosts are unsupported. |
| Guest | Linux AArch64 LP64 ELF applications | AArch32 and full-system/kernel emulation are unsupported. |
| Static libc | musl-linked guest tests via the bundled AArch64 cross toolchain in `make check-all` | This covers the registered fixtures, not every musl version or Linux syscall. |
| Dynamic libc | glibc and musl loader paths exercised by the provisioned full suite; glibc dynamic smoke tests run in PR CI | A guest rootfs must provide the matching AArch64 loader and libraries. Host x86_64 libraries are never guest code. |
| JIT/interpreter | Focused guest regressions run in both modes; JIT differential runs use `make verify` | JIT verification has documented limits for calls, syscalls, and memory side effects. Keep output checks in guest tests. |
| SDL/GL/Vulkan | Optional SDL2/GL host build; thunk tests run when the host display and driver are available | Headless CI skips display-dependent tests with exit 77. A skip is not evidence that a graphics path works. |
| Host CPU features | Runtime-gated x86 SIMD paths with fallbacks where implemented | CI does not yet run a hardware-feature matrix across different x86 generations. |

## Android scope and v2.0 plans

As of October 14, 2026, Asgard installs signed APK sets and runs native
NativeActivity and managed Activity/Service paths through the pinned Android
15 Bionic, ART and framework. The shared Binder realm owns graphics allocation
and codec services independently of app lifetimes. Focused checks cover JNI,
manager contracts, Activity/Service lifecycle, process death, audio callbacks,
surfaces and small exact-frame codec probes in both engines. The final managed
service batch passes 176 assertions per engine.

SpongeBob has user-confirmed cooking levels and menus through the documented
child-age path; Google Play-dependent behavior remains incomplete. Azur Lane
reached its logged login UI with actual host GPU access, but repeated UI delays
remain and its Google sign-in failure path can exit on unsupported toast
enqueue. Its gameplay, WebView interaction and sustained performance have not
been verified by this checkpoint.

Missing or partial areas include IME/text composition and wheel delivery,
implicit intent/browser resolution, notification posting/toasts, foreground
services, cross-app background delivery, isolated WebView handshake and extended
media/Surface recreation. HDR and arbitrary APK compatibility are not claimed.
See [runtime contracts](Android/ANDROID_RUNTIME.md), [APK tooling](Android/ANDROID_APK.md)
and the [v2.0 roadmap](roadmap.md) for current checkpoints and remaining work.

## CI gates

- Every push and pull request builds headless and SDL/GL configurations in
  separate output directories.
- Guest correctness CI compiles fixtures, runs strict unit tests under JIT
  and interpreter modes, runs JIT differential checks, tests the host APIs,
  and exercises dynamically linked glibc fixtures against an AArch64 sysroot.
- A weekly or manually triggered job runs the provisioned full suite with
  strict fixture requirements. The separately downloadable iperf3/coreutils
  bundle is reported as an explicit skip when absent.
- `make ci` runs the local guest-correctness gate. The GitHub workflow also
  builds both host feature configurations. `make check-all` provisions and
  runs the full local suite.

When a configuration is not listed as checked above, report it as unverified
and include the host, compiler, guest libc, and relevant feature flags with
any compatibility report.
