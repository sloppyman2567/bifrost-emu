# Platform and compatibility support

This document separates supported configurations from configurations that
are only expected to work. The emulator is an evolving user-mode emulator;
passing a focused test is not a claim of complete Linux ABI compatibility.

## Current support matrix

| Area | Supported / checked | Limits |
|---|---|---|
| Host | Linux x86_64; pull-request CI builds on Ubuntu 24.04 | Other Linux distributions are not release-gated. Non-Linux and ARM hosts are unsupported. |
| Host compilers | CI builds with GCC and clang on Ubuntu 24.04. On 2026-11-01 the strict quick unit suite with the JIT passed 183 of 183 with GCC 13.3 and clang 18.1 on Ubuntu 24.04, GCC 14.2 on Ubuntu 25.04, and the local GCC 16.2 | Other compilers and versions, and libc++, are untested. clang reports about 540 warnings GCC does not, almost all `struct`/`class` tag mismatches and unused generated constants. |
| Other distributions | The Ubuntu 24.04 and 25.04 runs above were headless builds in a chroot on this machine's kernel. A manual Ubuntu 25.10 run (2026-10-24; GCC 15.2, SDL 3.2.20, ffmpeg 7.1) compiled the source, installed the packages and ran `--setup-rootfs` and `asgard-setup` | On 2026-11-01 a Fedora 44 chroot (GCC 16.2, SDL 3.4, FFmpeg 8.1 from `ffmpeg-free`) on this machine's kernel built the source, passed the quick unit suite with both engines and installed both rootfs; no display, GPU, Android or Distrobox/Bazzite run. `make deps` (2026-11-01), in containers on this machine's kernel: on Debian 13 and 12 it installed the packages and a plain `make` compiled, with only `--version` run afterwards; Debian 12 has no SDL3 package, so no Android windows, and its Python 3.11 is below what Asgard needs. It failed in Arch and openSUSE Tumbleweed containers, undiagnosed, with a test harness that piped `yes` into the package manager, so the cause may be the harness; Fedora was not run. On this CachyOS machine it installed the one package that was missing. `make provision-asgard` passed from a fresh clone on this machine, and `make bootstrap` is not confirmed end to end. Apart from those, no Arch, Debian stable, openSUSE or musl-host run, and no run on another kernel. Graphics and audio were part of one 2026-11-01 run only, the CI full-suite job in an Ubuntu 24.04 chroot with Mesa's software drivers and no display, GPU or sound; Android was not. Android games on other distributions were reported second-hand, and Hungry Shark World's black screen on an Ubuntu VM is undiagnosed. |
| Guest | Linux AArch64 LP64 ELF applications | AArch32 and full-system/kernel emulation are unsupported. |
| Static libc | musl-linked guest tests via the bundled AArch64 cross toolchain in `make check-all` | This covers the registered fixtures, not every musl version or Linux syscall. |
| Dynamic libc | glibc and musl loader paths exercised by the provisioned full suite; glibc dynamic smoke tests run in PR CI | A guest rootfs must provide the matching AArch64 loader and libraries. Host x86_64 libraries are never guest code. |
| JIT/interpreter | Focused guest regressions run in both modes; JIT differential runs use `make verify` | JIT verification has documented limits for calls, syscalls, and memory side effects. Keep output checks in guest tests. |
| SDL/GL/Vulkan | Optional SDL2/GL host build; thunk tests run when the host display and driver are available | CI has no display: most GL and Vulkan tests run on SDL's offscreen driver and Mesa's software drivers, while the GLES/EGL, Wayland, Android surface and SDL Vulkan surface tests skip with exit 77. A skip is not evidence that a graphics path works, and software drivers say nothing about a real GPU. |
| Vulkan texture formats | On a host with BC7 but no ASTC (an RX 7600 on RADV is the one checked), Android apps are told ASTC LDR works and the thunk keeps each ASTC image as BC7, transcoding the blocks on the CPU when the upload is submitted (all 14 2D footprints, UNORM and sRGB). Checked 2026-11-08 by a host test, a headless guest test through the thunk on that GPU, and the GPU's own BC7 decode of 2,232 encoded blocks (every mode used, all partitions), which matched an independent decoder exactly | Not compared with ARM's reference decoder (multi-partition and dual-plane blocks are checked only for self-consistency) or run on a host with native ASTC, where it should pass through unchanged. Reading back, copying or blitting an ASTC image, 3D and HDR ASTC, and the GL/GLES path are not covered. It costs CPU while textures stream in (60 to 70% of Zenless Zone Zero's CPU time in one story-scene profile before helper threads were given the lowest priority and finished transcodes were cached); `BIFROST_VK_NO_ASTC=1` turns it off. |
| Steam Deck | On 2026-11-08 the maintainer ran Subway Surfers on a Steam Deck (SteamOS, Desktop mode; the build was not recorded) at 20 to 40 FPS, mostly 30 to 40, stable, with stutters, a hot CPU and close to 30 W of total system power. It is the first run on a CPU other than the development desktop's (the Deck's is a Zen 2 APU) | Desktop mode only. In Game Mode the maintainer got a black screen, not investigated, and the library cannot simply be added as a non-Steam game: Steam's environment variables have to be unset first or it cannot reach the Android library (LD_PRELOAD and related variables, per the maintainer; the exact set was not recorded). After quitting Subway Surfers from its own dialog, Return to app showed no window for a while; the maintainer's conclusion is that the Deck's CPU was slow to handle windows, not a bug (not traced). One game, one device, one session. |
| Host CPU features | The JIT needs SSE4.1 and FMA3 (AVX state included) and refuses to start without them; `--no-jit` runs on any x86-64 CPU. `test_host_cpu_models.sh` runs JIT fixtures on emulated Haswell and checks the refusal on older models; it has not been run since this minimum was set. Earlier history: On 2026-11-01 those models had no failure beyond `-cpu max` in the unit suite (QEMU 11.1; 192 pass, and 43 fail on every model because of qemu-user) | Emulated only: nothing ran on real old hardware. LZCNT without BMI1 (AMD K10, early Bulldozer) is not testable because QEMU 11.1 runs LZCNT as BSR there. AVX-512 hosts are not covered. A CPU without LAHF in 64-bit mode (the earliest x86-64 models) is unsupported. |

## Android scope and v2.0 plans

As of 2.0.0-beta.1 (November 8, 2026), Asgard installs signed APK sets and runs
native NativeActivity and managed Activity/Service paths through the pinned
Android 15 Bionic, ART and framework. A shared Binder realm owns graphics
allocation and codec services independently of app lifetimes. Focused checks
cover JNI, manager contracts, Activity/Service lifecycle, process death, audio
callbacks, surfaces and small exact-frame codec probes in both engines.

Per-app results are recorded in `Scripts/Android/compatibility.json` (shown by
`asgard compat`), each with the version and date it was checked. A person played
SpongeBob: Krusty Cook-Off. Azur Lane and Umamusume Global are playable on the
maintainer's report, with screenshots on their pages; The Battle Cats, Subway Surfers
and Geometry Dash Lite on the report alone, with no capture. Zenless Zone Zero and
Doki Doki Literature Club are playable on the maintainer's report (2026-11-06): the first with
very low-resolution textures and heavy lag. The second's audio popped until the AAudio write fix
(clean, user-confirmed 2026-11-08), and its images blacked out when the window was resized (Ren'Py's own
resize path); it now opens with Window set to "Keep the app's shape", which avoids that (user-confirmed
2026-11-08). On 2026-11-08 the maintainer played Zenless Zone Zero's story scenes with the
ASTC-to-BC7 path (the overlay read 29 to 30 FPS at 3440x1384); the models are still very low resolution, which the
maintainer says they always were, and a session ended on a guest null dereference once (not reproduced in the next
7-minute session). Hungry Shark Evolution 14.5.0 and Hungry Shark World 8.2.6 have stable
gameplay on the maintainer's report (2026-11-06), with no capture. YouTube shows its home screen. Firefox reaches its onboarding and
browser screens, but pages do not load. Uptodown starts and runs a bounded session. Subway Surfers was run on a Steam Deck in Desktop mode (2026-11-08) at 20 to 40 FPS. Oddmar's first level plays very well on the maintainer's report
(2026-11-08, no stutter noticed; a recording frame shows gameplay); later levels are not confirmed. Roblox, TrickCal
and Umamusume JP are blocked by the app itself (Umamusume JP at a device
integrity check when it connects). These are single checks, not compatibility
guarantees, and gameplay is confirmed only where the file says `playable`.

Google account support is experimental. Play services come from microG, so behaviour that needs real Google services
(device attestation, a Play Games profile, an adult-age check) is unavailable.
Settings screens, host mDNS, HDR and arbitrary APK compatibility are not provided or
claimed. The Android Vulkan HAL copies each presented frame through a host staging buffer (no zero-copy swapchain): a GPU
read-back, a wait for the GPU and a CPU copy into the app's buffer on release, then another copy into the window; the
upload on acquire is skipped after an image's first release (2026-11-08, not yet run in a game). An APK that ships only 32-bit
libraries cannot start. See
[runtime contracts](Android/ANDROID_RUNTIME_REFERENCE.md), [APK tooling](Android/ANDROID_APK.md)
and the [roadmap](roadmap.md) for open items and the known blockers by cause.

## CI gates

- Every push and pull request builds headless and SDL/GL configurations in
  separate output directories.
- Guest correctness CI compiles fixtures, runs strict unit tests under JIT
  and interpreter modes, runs JIT differential checks, tests the host APIs,
  and exercises dynamically linked glibc fixtures against an AArch64 sysroot.
- A weekly or manually triggered job runs the provisioned full suite with
  strict fixture requirements, on a host with no display, GPU or sound. The
  separately downloadable iperf3/coreutils bundle and `xwayland_wire`, which
  needs X11 in the rootfs, are reported as explicit skips when absent.
- `make ci` runs the local guest-correctness gate. The GitHub workflow also
  builds both host feature configurations. `make check-all` provisions and
  runs the full local suite.

When a configuration is not listed as checked above, report it as unverified
and include the host, compiler, guest libc, and relevant feature flags with
any compatibility report.
