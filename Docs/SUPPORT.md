# Platform and compatibility support

This document separates supported configurations from configurations that
are only expected to work. The emulator is an evolving user-mode emulator;
passing a focused test is not a claim of complete Linux ABI compatibility.

## Current support matrix

| Area | Supported / checked | Limits |
|---|---|---|
| Host | Linux x86_64; pull-request CI builds on Ubuntu 24.04 | Other Linux distributions are not release-gated. Non-Linux and ARM hosts are unsupported. |
| Host compilers | CI builds with GCC on Ubuntu 24.04. A local run on 2026-10-27 built with GCC 16.2.1 and clang 22.1.8 (no errors) and passed the quick unit suite with the JIT (183 of 183; one test that matches the binary's file name was rerun under it) and with `--no-jit` (169 of 169, 14 skipped) | Not in CI. Other compilers and versions, and libc++, are untested. clang reports about 540 warnings GCC does not, almost all `struct`/`class` tag mismatches and unused generated constants. |
| Other distributions | A manual Ubuntu 25.10 run (2026-10-24; GCC 15.2, SDL 3.2.20, ffmpeg 7.1) compiled the source, installed the packages and ran `--setup-rootfs` and `asgard-setup`; it found and fixed bugs in tool URLs, `lpunpack` and `linker64` handling | No Fedora, Arch or Debian stable run, and no automated run on any other distribution. Android games there were reported second-hand, and Hungry Shark World's black screen on that VM is undiagnosed. |
| Guest | Linux AArch64 LP64 ELF applications | AArch32 and full-system/kernel emulation are unsupported. |
| Static libc | musl-linked guest tests via the bundled AArch64 cross toolchain in `make check-all` | This covers the registered fixtures, not every musl version or Linux syscall. |
| Dynamic libc | glibc and musl loader paths exercised by the provisioned full suite; glibc dynamic smoke tests run in PR CI | A guest rootfs must provide the matching AArch64 loader and libraries. Host x86_64 libraries are never guest code. |
| JIT/interpreter | Focused guest regressions run in both modes; JIT differential runs use `make verify` | JIT verification has documented limits for calls, syscalls, and memory side effects. Keep output checks in guest tests. |
| SDL/GL/Vulkan | Optional SDL2/GL host build; thunk tests run when the host display and driver are available | Headless CI skips display-dependent tests with exit 77. A skip is not evidence that a graphics path works. |
| Host CPU features | Runtime-gated x86 SIMD paths with fallbacks where implemented | CI does not yet run a hardware-feature matrix across different x86 generations. |

## Android scope and v2.0 plans

As of 2.0.0-alpha.3 (October 27, 2026), Asgard installs signed APK sets and runs
native NativeActivity and managed Activity/Service paths through the pinned
Android 15 Bionic, ART and framework. A shared Binder realm owns graphics
allocation and codec services independently of app lifetimes. Focused checks
cover JNI, manager contracts, Activity/Service lifecycle, process death, audio
callbacks, surfaces and small exact-frame codec probes in both engines.

Per-app results are recorded in `Scripts/Android/compatibility.json` (shown by
`asgard compat`), each with the version and date it was checked. A person played
SpongeBob: Krusty Cook-Off and Subway Surfers (the latter on the maintainer's
report, with no capture). Azur Lane reaches its title and login screen. Hungry
Shark World, Hungry Shark Evolution, The Battle Cats, Uptodown and Umamusume
start and run a bounded session. Roblox and TrickCal are blocked by the app
itself ending the session. These are single checks, not compatibility
guarantees, and gameplay is confirmed only where the file says `playable`.

Play services come from microG, so behaviour that needs real Google services
(device attestation, a Play Games profile, an adult-age check) is unavailable.
An Android Vulkan HAL, Settings screens, host mDNS, HDR and arbitrary APK
compatibility are not provided or claimed. An APK that ships only 32-bit
libraries cannot start. See
[runtime contracts](Android/ANDROID_RUNTIME.md), [APK tooling](Android/ANDROID_APK.md)
and the [roadmap](roadmap.md) for open items and the known blockers by cause.

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
