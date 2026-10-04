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

Android surface, activity/looper and audio regression probes were freshly
checked in both JIT and interpreter modes on October 4, 2026. The host native
bridge API probe also passes 61/61; it does not run an actual ART instance.

The current standalone Android path targets native NativeActivity binaries;
it does not provide integrated APK/ART execution. The existing native-bridge
interface is an integration API, not a complete Android framework or runtime.
Bionic compatibility, integrated ART/JNI, APK loading and selected Android
game support are planned in the [v2.0 roadmap](../roadmap.md). Keep those plans
separate from verified current support.

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
