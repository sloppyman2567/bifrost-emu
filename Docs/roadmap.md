# bifrost-emu v2.0 roadmap

Status: in progress. The current pre-release is **2.0.0-beta.1** (November 3, 2026), a version label: its own scope is
only partly met. Updated November 3, 2026.

Everything below is a development target, not a compatibility guarantee or a release date. "Done" means the contract is
implemented and checked as the linked document says, not that every app using it works. Dated checkpoints, measurements
and investigations that used to live here are in [the roadmap snapshot of 2026-11-03](archive/roadmap-2026-11-03.md) and
the [changelog](CHANGELOG.md).

## Goals for v2.0

- Run selected ARM64 Android games through the integrated Android runtime (real ART, framework and Bionic), installed and
  started from the desktop library.
- Sustain Linux game performance, and run Linux games from the same library.
- Make input dependable across mouse, keyboard, touch and gamepads.
- Keep correctness the gate for every optimization, and keep existing Linux applications working.

## Where things stand

| Milestone | State | Highlights |
|---|---|---|
| 2.0.0-alpha.1 (2026-10-12) | Released | Subsystem layout; Asgard on the pinned Android 15 framework; SpongeBob playable |
| 2.0.0-alpha.2 (2026-10-21) | Released; goals partly met | Toasts, `NsdManager`, all of `IAppOpsService`, declined Settings intents, scaled fixed-size apps |
| 2.0.0-alpha.3 (2026-11-01) | Released; WebView, media and ad scope mostly open | `media.extractor`, POSIX timers, installable Asgard and rootfs, `bifrost-emu app.apk`, window stats |
| 2.0.0-beta.1 (2026-11-03) | Released as a label; scope partly met | Library cards, per-app settings, clear-data/uninstall, run-ended cards; app Keystore, WebView GPU fences, Vulkan for apps through a guest HAL, a faster Tier-1 JIT |

**Games:** 17 recorded, 11 of them playable (7 Android, 4 Linux), 4 start without confirmed gameplay and 2 are blocked; see
[What runs](../README.md#what-runs) and `Scripts/Android/compatibility.json`. Statuses are user reports unless a page says
otherwise.

**Known blockers, by cause:**

- **Needs real Google attestation or a real device:** Roblox is disconnected after joining (reason 304); TrickCal's
  anti-cheat ends it; Umamusume JP stops at its device integrity check. Bifrost does not bypass or forge any of them.
- **Needs Google Play services beyond microG:** SpongeBob's adult-age path stalls at 73%; Hungry Shark World's Play Games
  sign-in gets HTTP 404 and `SIGN_IN_REQUIRED`.
- **Architecture:** Bifrost runs AArch64 only; an APK with only `armeabi-v7a` or `x86` libraries (Angry Birds Space)
  cannot start.
- **Platform gaps:** no Settings screens, no host mDNS, no toast animations. The Android Vulkan HAL copies every
  presented frame (no zero-copy swapchain).
- **Undiagnosed:** Firefox 159 reaches onboarding and a GeckoView surface but loads no pages; not re-tested since the
  November 1 Binder fix.

## Next release priorities

In order. Each carries into the next pre-release until its acceptance test passes.

### 1. Easier setup and a dependable app library (the beta.1 scope)

- Guide runtime provisioning and signed APK installation from the library, with actionable missing-runtime and
  window-backend errors. Done: a card says why a run ended and the drawer what to try.
- Run Linux games from the library, each with its rootfs profile and linker. Today Asgard runs Android apps only, and a
  Linux game started by hand without its setup can crash without explanation.
- Make the launcher pick the intended graphics-capable executable, and stop headless validation from silently replacing the
  desktop default.
- A clear update/reinstall flow, with routine controls outside Developer mode. Done: cards, icons, search, per-app
  Back-key and stats settings, clear-data/uninstall and the last-session drawer.
- Validate install → play → settings → stop/relaunch on a fresh setup, and broaden the app matrix with longer sessions and
  several apps in one realm on the supported Linux display stacks.

Acceptance: a fresh user provisions and launches a supported app without editing paths or environment variables.
Supported apps complete repeatable 30-minute interactive sessions with correct saves, audio and input; a failed install or
app exit leaves the library and the other apps usable.

### 2. Broader game support and less stutter (carried from alpha.2)

- Fix concrete startup and interaction blockers in selected games, starting with Azur Lane's delayed login UI, by
  completing service contracts rather than answering with empty success.
- Profile matching cold-load and warm gameplay scenes; reduce compilation stalls, repeated translation, memory-guard and
  spill costs where measurements justify it.
- Measure rendering, ART/GC, Binder and input delays alongside JIT cost.
- Exercise several engines and apps with audio, saves, resize, pause/resume and process exit, keeping Linux games working.

Acceptance: repeatable GPU-enabled runs report load time, CPU and compilation cost and median/p95/p99 frame times against a
baseline, with correctness checks and sustained interaction. A window title's instantaneous FPS is not the measurement.

### 3. WebView, media and possible ad support (carried from alpha.3)

- Finish attached WebView pointer, focus and scroll delivery, then Chromium editors, caret-anchored candidate windows and
  clipboard exchange ([WebView input](#webview-text-input-and-external-pages)).
- Deliver returns and deep links from the host browser; Custom Tabs, verified App Links and other implicit intents.
- Trace SpongeBob's stuck Kidoz ad; extend Surface/media lifecycle, reconfiguration and audio synchronization for the path
  ad SDKs use.
- Investigate supported rewarded and interstitial ad SDKs with their test creatives; never synthesize reward success.

Acceptance: a supported SDK test creative loads, displays, accepts input, closes and returns to the game without losing
input or audio; rewarded tests deliver the SDK's real completion callback once; no-fill, cancellation, network failure and
renderer exit leave the game usable.

## Workstreams

### Android platform: Bionic, linker and packages

Done: real guest `linker64`/Bionic startup; signed base and split installation, including asset packs over 1 GiB;
NativeActivity; persistent per-app storage and an app Keystore; the pinned Android 15 ART and framework with
WindowManager-owned Activity windows and a shared Binder realm ([APK scope](Android/ANDROID_APK.md),
[runtime](Android/ANDROID_RUNTIME.md)).

Open:

- Extend the pinned Android 15/API 35 baseline with documented provenance, rather than claiming every Android release.
- Extend Bionic libc, libm, libdl, pthread/TLS, signal, futex, mapping and ioctl coverage from tested contracts.
- Linker namespaces, Android library search rules and `android_dlopen_ext`, with per-app isolation
  ([namespaces](https://source.android.com/docs/core/architecture/vndk/linker-namespace),
  [Bionic linker changes](https://android.git.googlesource.com/platform/bionic/+/HEAD/android-changes-for-ndk-developers.md)).
- A defined permissions model; services added incrementally, with unsupported ones reported explicitly.

Acceptance: a Bionic native test, a packaged NativeActivity sample and a selected game launch through the Android profile
with working storage, lifecycle and native library loading.

### ART, JNI and framework services

Done: real ART runs DEX and JNI into ARM64 engines; Activities and local/remote Services have creation, lifecycle, binding
and process-death coverage; Settings, PendingIntents, broadcasts and jobs have process-local contracts. A call that a
stand-in service does not answer is logged once per process (`SERVICE_CALL_UNSUPPORTED`); toasts queue and show.

Open:

- Faster managed startup and runtime, guided by real-game profiles; stability under heavy GC, thread activity,
  pause/resume, shutdown and concurrent apps.
- Complete the services games use, with real state and cleanup. The largest unanswered call is
  `ISurfaceComposer.createConnection`, which needs the SurfaceControl layer model.
- Settings screens (so all-files access can be granted), host notifications with tap delivery, and an mDNS backend for
  `NsdManager`.
- PendingIntents, alarms and jobs that survive process exit and restart stopped apps; background-start restrictions.

Acceptance: manager clients exercise state changes, permission denials, copied replies and cleanup, with real callbacks and
lifecycle checks in both engines; concurrent-client claims need distinct UIDs, peer exit and cached Binder handles.

### Android graphics, audio, media and lifecycle

Done: OpenSL ES and AAudio playback; `SoundPool`, and `media.player` from the image's real `mediaserver` (`MediaPlayer` and
`MediaMetadataRetriever`; FNAF Mobile Port plays with sound); RenderScript intrinsics on the CPU driver; Media JNI and Codec2 with
realm-owned Gralloc; `media.extractor` (Oddmar gets past its
opening video); scaled fixed-size windows; WebView GPU fences; Play services activities in a relay window; a GLES thunk guard for
pointer-to-pointer parameters and a retry for mobile GLSL index conversions.

Open:

- A zero-copy Android Vulkan swapchain. The HAL's `VK_ANDROID_native_buffer` copies a full frame per present because
  the software gralloc allocator is a sealed memfd the host driver cannot import as image memory; it needs a
  dma-buf-backed allocation.
- `MediaPlayer` beyond FNAF Mobile Port (video to a Surface, streaming, seeking, long sessions); decoder validation beyond small
  fixtures (flush/reconfigure, B-frames, sustained playback).
- Audio effects: `android.media.audiofx` loads and reports no effects; `Equalizer` and the like need DSP in front of the host's audio.
- Activities from other processes beyond Play services; a real Play Games sign-in has not been observed.
- Orientation, resize, focus, surface loss/recreation and background/foreground tests.
- Graphics ABI coverage from dependency audits; confirm the GLSL retry in Roblox and explain its low-resolution textures.
- Memory: JIT code size, the remaining non-JIT heap, why the WebView service stays alive, and a controlled before/after
  measurement.

Acceptance: gameplay with working audio and input survives repeated surface recreation and lifecycle transitions without
leaks or stale handles.

### Input

- Verify relative mouse, axes, pointer capture, sensitivity and focus/fullscreen transitions; resolve vkQuake's texture
  issues.
- Gamepads: Android apps get host pads with hotplug and an Xbox 360 mapping (SDL3 backend, `FrameworkGamepads`),
  checked in fakes and the framework; Subway Surfers is played with the 2026 Steam Controller (maintainer's report).
  Still open: rumble, back paddles, per-game profiles and remapping, and pads for Linux guests' own input paths.
- Android touch, multitouch, keys and controller events; desktop-to-touch mappings stay configurable. Wheel/pinch zoom
  gestures and app pointer icons exist but are untried in live games.
- Keyboard layouts, held keys, text input and pause/menu transitions without stuck or duplicated events.

Acceptance: repeatable input probes and live game checks pass across the desktop modes and Android lifecycle transitions.

### WebView text input and external pages

Done: desktop text and composition reach native editors through `InputConnection`; http(s) and Play Store links open in
the host browser ([host text input](Android/ANDROID_RUNTIME.md#host-text-input-2026-10-20),
[web links](Android/ANDROID_RUNTIME.md#web-links-and-play-store-links-2026-10-20)).

Open:

1. Reproduce clicks, focus, dragging and scrolling on an attached real WebView page, then in the failing app.
2. Chromium editors in a WebView, selection, paste, editor actions, `CursorAnchorInfo` and clipboard; connection cleanup on
   focus changes and exit. An on-screen keyboard is a later option.
3. Resolve manifest intents consistently across PackageManager queries and Activity starts.
4. Deliver browser callbacks and deep links through the right app and lifecycle path. Launching a URL does not establish
   sign-in.

Acceptance: editor and WebView tests cover composition, scrolling, focus, resize, browser failure, callbacks and process
exit; then the Azur Lane interaction is checked in a real window and its frame delays profiled.

### JIT codegen: retain guest state across hot regions

Goal: keep guest registers, vectors and NZCV in host registers across hot paths, reconstructing state only at observable
boundaries ([design](JIT.md#planned-codegen-retaining-guest-state-across-regions)).

Done: experimental Tier-2 regions (opt-in); a cheaper Tier-1 (CoreMark 13,156 iterations/s on October 27, over the 10,000
target); compact scalar memory guards; shared cold stubs; native scalar and vector FP by element and LD1R; loop bodies up
to twice the block cap; lighter Q-register accesses. On a set of engine-shaped kernels the JIT went from 5.8× to 4.2× the
time of a native AVX2 build ([details](JIT.md)).

Open, in priority order:

1. **Per-instruction overhead in scalar FP loops** (an audio mixer kernel is still 9.9× native): one-instruction scalar FP
   result writes, `v_hi` zeroing once per block instead of per op, out-of-line scalar NaN steps, and Z-only flag consumers
   testing the result instead of packing NZCV.
2. **Deferred guest-register stores within a block**, so temporaries are not written to `cpu.regs` and read back.
3. **Paired-memory guards**, SIMD values kept past the all-or-nothing cache limits, and safe forwarding around atomics.
4. **Model guest state explicitly**, define recovery and helper contracts, then hand values between two regions and choose
   layouts by measured benefit (the five-stage plan in [the snapshot](archive/roadmap-2026-11-03.md#jit-codegen-retain-guest-state-across-hot-regions)).
5. **Tier-2**: level with Tier-1 on CoreMark and about 1.5 times faster on a clean multi-block loop, now that its trial
   measures correctly. It does not take single-block loops, which Tier-1 already keeps in registers. Next: measure real
   Android titles, then regions with internal branches (`BIFROST_TIER2_GRAPH` surveys them). It stays opt-in until then.
6. Candidate: on AVX-512 hosts, `xmm16`-`xmm31` could hold more of the 32 guest vectors than the 13 cached today.

Targets: CoreMark at least 10,000 iterations/s (met); the proposed 26%-of-native CoreMark milestone needs a confirmed native
reference; game kernels toward 2× native where the guest code allows it.

Acceptance: each stage passes architectural regressions, reference checks and real applications, and reduces spills and
state transfers without excessive compile time or code growth; then matched gameplay frame times are measured.

### JIT and graphics caching

Done: a larger per-thread lookup cache; MT publication through atomic target cells; whole-cache replacement at 512 MiB with
safe reclamation; compile-time, recompile and invalidation counters; an experimental persistent translation cache
(`--jit-disk-cache`).

Open:

- Profile ART startup (dispatch and interpreter fallbacks dominated it) and give the hottest fallbacks native IR.
- Measure gameplay compile bursts, then choose between a background compiler and the persistent cache.
- Keep hot blocks across cache replacement without unbounded memory or unsafe eviction.
- The persistent cache defaults on only after a measured gain on a game, play from the cache and other hosts.
- Shader and pipeline cache reuse, measured separately from CPU translation.

Acceptance: repeat visits avoid recompilation, invalidation stays correct under concurrent code updates, memory stays
bounded, and cold/warm A/B runs show fewer stutters.

### Linux games: linker, rootfs and performance

Done: the `games` rootfs profile; Doom 3 (dhewm3) at mostly 60 FPS in its demo by the maintainer's report; an experimental
guest dynamic linker (`--guest-ld`) that passes the dynamic suites.

Open:

- Make `--guest-ld` the default and retire the built-in linker. Blockers seen on November 1: Neverball stops at startup
  because the `games` profile lacks `libudev.so.1`, and vkQuake crashes within a second (signal 11, untraced); musl threads,
  `make verify` and cold-start measurements remain.
- Name missing imports instead of continuing: on the default rootfs, dhewm3 resolves eight OpenAL symbols to null and
  crashes at address 0.
- Doom 3 frame pacing: profiler attribution for every thread, matched-scene median and p95/p99 frame times, then the
  remaining drops. Fix save/load fidelity so routes are reproducible.

Acceptance: matched cold/warm scene runs show better frame times and demanding-scene minimums with no correctness
regressions, published with host, settings, scene and duration.

### Hosts and CI

- CI's full-suite, build, clang and script jobs pass on GitHub (Ubuntu 24.04, no display or GPU); GLES/EGL, Wayland and
  Android surface tests are not covered there. The guest job's interpreter run last failed `test_fork_lock_contention`; the
  fixes that followed have not run on GitHub.
- Run the strict suite and `make verify` on an AVX-512 host (the Ryzen 5 9600X); not done.
- More Android games on RDNA 4: so far Subway Surfers and Arknights by the maintainer's reports.
- Fedora is unit-tested in a chroot only (run a game on Bazzite); Debian is compile-tested in containers; `make deps` failed
  on openSUSE; Alpine is untested ([support](SUPPORT.md)).

## Not planned

- **Save export/import from the library.** Online games keep progress on their servers, and local saves stay with each
  app's data.
- **Bypassing or forging attestation, anti-cheat or integrity checks.**
- **Arbitrary APK compatibility or every Android release.** Support is per app on the pinned Android 15 baseline.
- **32-bit ARM or x86 guest code.** Bifrost emulates AArch64 only.

## Delivery gates for v2.0

1. Trustworthy profiling, cache telemetry and resumable game tests.
2. Linux game correctness, caching and demanding-scene performance.
3. The Bionic/Android profile and packaged native-app coverage.
4. ART/JNI and framework/service coverage for additional games.
5. A broader Android game matrix with lifecycle, audio/input and long-session stability checks before declaring v2.0
   compatibility.

At every milestone, run the JIT and interpreter regression suites and the relevant independent AArch64 oracle checks. Keep
planned features out of the support matrix; Google Play services, protected content and arbitrary APKs need their own
scoped work.
