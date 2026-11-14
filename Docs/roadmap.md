# bifrost-emu v2.0 roadmap

Status: in progress. The current pre-release is **2.0.0-beta.2** (November 14, 2026): its scope is
only partly met. Updated November 14, 2026.

Everything below is a development target, not a compatibility guarantee or a release date. "Done" means the contract is
implemented and checked as the linked document says, not that every app using it works. Dated checkpoints, measurements
and investigations that used to live here are in [the roadmap snapshot of 2026-11-03](archive/roadmap-2026-11-03.md) and
the [changelog](CHANGELOG.md).

## Goals for v2.0

- Run selected ARM64 Android games through the integrated Android runtime (real ART, framework and Bionic), installed and
  started from the desktop library.
- Sustain Linux game performance. Linux games run from the command line; the desktop library is for Android apps (see
  [not planned](#not-planned)).
- Make input dependable across mouse, keyboard, touch and gamepads.
- Keep correctness the gate for every optimization, and keep existing Linux applications working.

## Direction: apps that feel native

The aim is an Android game that behaves like any other window on a Linux desktop. In the order that costs a session most:

1. **It does not crash.** Ads come first: most free games carry them, and an ad is the commonest thing that ends a session
   (Geometry Dash Lite's WebView crash on `InputDevice.hasKeys`, SpongeBob's Kidoz ad layer failure).
2. **The window behaves:** tiling, resizing, fullscreen and a remembered size, and an app that takes the window's new shape.
3. **Input works** from the mouse, the keyboard, a pad, and a pad-driven pointer for apps that only take touch.
4. **It does not hitch**, ads included. Measure before fixing. The session logs already say where the main thread stalls
   ([measured so far](#measured-so-far-ads-webview-and-windows)): WebView's first start is the commonest, then cold app and SDK
   code and other WebView work. What no log shows is frame pacing while an ad is up, which needs a frame-time record.
5. **It starts quickly and stays small:** the persistent JIT cache and the memory breakdown.

Not the focus: games behind attestation or anti-cheat, and compatibility sweeps with no concrete game to fix (see
[not planned](#not-planned)).

## Where things stand

| Milestone | State | Highlights |
|---|---|---|
| 2.0.0-alpha.1 (2026-10-12) | Released | Subsystem layout; Asgard on the pinned Android 15 framework; SpongeBob playable |
| 2.0.0-alpha.2 (2026-10-21) | Released; goals partly met | Toasts, `NsdManager`, all of `IAppOpsService`, declined Settings intents, scaled fixed-size apps |
| 2.0.0-alpha.3 (2026-11-01) | Released; WebView, media and ad scope mostly open | `media.extractor`, POSIX timers, installable Asgard and rootfs, `bifrost-emu app.apk`, window stats |
| 2.0.0-beta.1 (2026-11-03) | Released as a label; scope partly met | Library cards, per-app settings, clear-data/uninstall, run-ended cards; app Keystore, WebView GPU fences, Vulkan for apps through a guest HAL, a faster Tier-1 JIT |
| 2.0.0-beta.2 (2026-11-14) | Current; scope partly met | Installs and updates that rebuild what changed, `make check-fresh`, a session Zygote and one shared Android image, gamepad control of the library, Tier-2 through calls and returns, NativeActivity games |

**In 2.0.0-beta.2, since the beta.1 label:** the desktop library can be driven with a gamepad (a gamepad layout, menus,
confirmations and the settings page drawn inside its window); per-app `window_mode` and `controller` settings, with a touch
cursor driven by a pad; fixed-size apps can be tiled to a half or a corner; the Android setup's main downloads (the system image, microG, AAPT2, the Bionic runtime, the package tools, SDL3) resume
when interrupted, and `make bootstrap` finds host tools and libraries without root, building SDL3 where the distribution has none; a minimised or hidden game window pauses the app, F11 toggles fullscreen and the size is remembered, Activities are relaunched on a resize they do not handle, the library offers **Set up…**, packages up to 4 GiB install, a realm with no app ends after 10 minutes, and the JIT has native NEON structure loads and stores and cheaper FP compares. All of it was checked with fakes, on Qt's
offscreen platform or in an Ubuntu 24.04 chroot, apart from what the maintainer saw on one KDE/Wayland desktop with a Steam
Controller; on a Steam Deck only Desktop mode has run (Subway Surfers, 2026-11-08); none has run in Game Mode (a black screen), on a fresh Debian 13 or against a plain pad.
On November 8: Vulkan apps get ASTC LDR textures on hosts that have BC7 but no ASTC (converted on the CPU at upload; Zenless Zone Zero's
Unity stopped decompressing them itself), a swapchain acquire no longer re-uploads the frame, `/proc/self/pagemap` is read without
rebuilding every mapping, and the crash line names the library. Each is checked as its changelog entry says; none is yet measured
across a long game session.
On November 10 and 11: an app's helper processes fork from a session Zygote, microG maps a compiled vdex, games link one shared
read-only copy of Android, launch preparation reuses file hashes and compiles after the session, the package parse cache
survives updates, static AudioTracks and SoundPool play, the JIT disk cache reads its records instead of mapping them, and
Tier-1 and Tier-2 got the changes listed under [the JIT workstream](#jit-codegen-retain-guest-state-across-hot-regions).
Measured on Battle Cats and Subway Surfers launches and kernels; the shared copy has not run from an installed Asgard and no
game was timed for the JIT changes.

**Games:** 19 recorded, 16 of them playable (12 Android, 4 Linux), 1 starts without confirmed gameplay and 2 are blocked; see
[What runs](../README.md#what-runs) and `Scripts/Android/compatibility.json`. Statuses are user reports unless a page says
otherwise.

**Known blockers, by cause:**

- **Needs real Google attestation or a real device:** Roblox is disconnected after joining (reason 304); TrickCal's
  anti-cheat ends it; Umamusume JP stops at its device integrity check. Bifrost does not bypass or forge any of them.
- **Needs Google Play services beyond microG:** SpongeBob's adult-age path stalls at 73%; Hungry Shark World's Play Games
  sign-in gets HTTP 404 and `SIGN_IN_REQUIRED`.
- **Architecture:** Bifrost runs AArch64 only; an APK with only `armeabi-v7a` or `x86` libraries (Angry Birds Space)
  cannot start.
- **Platform gaps:** YouTube stays in its landscape layout when the window is narrowed, although a host resize reaches an app
  as a new orientation and size and an Activity that does not declare the change is relaunched; YouTube declares it and takes it
  in `onConfigurationChanged`, so the cause is untraced (the per-app Window setting's "Keep the app's shape" scales a game
  instead); no Settings screens, no host mDNS, no toast animations. The Android Vulkan HAL copies every
  presented frame through a staging buffer (no zero-copy swapchain; the upload on acquire is skipped after an image's first
  release, 2026-11-08, not yet run in a game).
- **Undiagnosed:** Firefox 159 reaches onboarding and a GeckoView surface but loads no pages; not re-tested since the
  November 1 Binder fix.

## Next release priorities

In order. Each carries into the next pre-release until its acceptance test passes.

### 1. Easier setup and a dependable app library (the beta.1 scope)

- Guide runtime provisioning and signed APK installation from the library, with actionable missing-runtime and
  window-backend errors. Done: a card says why a run ended and the drawer what to try; the library asks `asgard setup --check`
  at start and, when a checkout lacks part of the runtime, offers **Set up…** (downloads and builds the missing steps, with
  progress, resuming after a failure), refuses to start an install that cannot work, and an installed copy that is incomplete
  is told to reinstall; a host window that would not open ends a run as "Couldn't open a window" with advice (no SDL3, no
  desktop). Checked with fake commands and Qt's offscreen platform; the real downloads and builds were not run, nor was a
  fresh machine used.
- Make the launcher pick the intended graphics-capable executable, and stop headless validation from silently replacing the
  desktop default.
- A clear update/reinstall flow, with routine controls outside Developer mode. Done: cards, icons, search, per-app
  Back-key, stats, Window and Controller settings, clear-data/uninstall and the last-session drawer; a library a gamepad can
  drive (gamepad layout, Select for the last session); `make bootstrap` that resumes downloads and finds tools and libraries
  without root. Open: Game Mode on a Steam Deck (a black screen on 2026-11-08; Desktop mode ran Subway Surfers) and a fresh Debian 13.
- Validate install → play → settings → stop/relaunch on a fresh setup, and broaden the app matrix with longer sessions and
  several apps in one realm on the supported Linux display stacks.

Acceptance: a fresh user provisions and launches a supported app without editing paths or environment variables.
Supported apps complete repeatable 30-minute interactive sessions with correct saves, audio and input; a failed install or
app exit leaves the library and the other apps usable.

### 2. Ads, WebView and windows that behave (carried from alpha.3)

- Finish attached WebView pointer, focus and scroll delivery, then Chromium editors, caret-anchored candidate windows and
  clipboard exchange ([WebView input](#webview-text-input-and-external-pages)).
- Deliver returns and deep links from the host browser; Custom Tabs, verified App Links and other implicit intents.
- Trace SpongeBob's stuck Kidoz ad; extend Surface/media lifecycle, reconfiguration and audio synchronization for the path
  ad SDKs use.
- Investigate supported rewarded and interstitial ad SDKs with their test creatives; never synthesize reward success.
- Answer what an ad's WebView asks of the device so that it never throws out of the app: done for the gamepad `hasKeys`
  call (the cause of Geometry Dash Lite's crash); the next unsupported call shows in the session log.
- Activity relaunch on a host resize: done in 2.0.0-beta.2 (an Activity that does not declare the change is relaunched
  as Android does; [details](Android/ANDROID_RUNTIME_REFERENCE.md#activity-relaunch-2026-11-07)), checked with a fixture on the real
  runtime, not with a real app that needs it. It does not explain YouTube, which declares orientation, size and layout in
  `configChanges` and so is never relaunched. Open: trace why YouTube keeps its landscape layout when narrowed (what its
  `onConfigurationChanged` receives, the metrics it reads).
- Measure frame pacing around ads before choosing a rendering fix: an ad-heavy session with the window-stats overlay on and an
  opt-in frame-time log (not written yet), split into the ad showing, the game while the ad loads, and closing it. The
  stall data below covers only main-thread blocks of about 1.5 s or more.
- Move WebView's first start off the critical path: the UI thread waits in `startBrowserProcessesSync` the first time an ad
  opens a WebView (53 episodes, median 1.7 s). Creating a hidden WebView during the loading screen is untried; the stall
  log shows whether the median drops.
- Tell the app about its window. The window plugin has seven events (close, resize, key, pointer, text, gamepad,
  visibility). Done in 2.0.0-beta.2: a window the desktop minimises, hides or covers stops the app's Activities
  and resumes them when it is visible again ([visibility](Android/ANDROID_RUNTIME_REFERENCE.md#window-visibility-2026-11-07));
  checked against a fake SDL, not on a real compositor. Open: whether KWin and other Wayland compositors suspend a
  minimised window (without that, a minimised game on Wayland keeps running), focus loss (traced, not delivered, and
  Android keeps an unfocused app resumed), and idle CPU of a stopped game before and after.
- Game windows: F11 toggles fullscreen and the size is remembered per app, with no setting for either (done in 2.0.0-beta.2,
  checked against a fake SDL, not on a real compositor; [details](Android/ANDROID_RUNTIME_REFERENCE.md#fullscreen-and-remembered-size-2026-11-07)).
  Position and fullscreen are deliberately not remembered (a Wayland client cannot place its window). Open: a real
  compositor check, including whether the window visibly resizes once at launch. `--fullscreen` still belongs to the library.
- Realm idle policy: done for the broker (it ends after 10 minutes with no app attached, `ASGARD_REALM_IDLE_SECONDS`; tested
  with the real emulator, not with media services attached; the retired broker leaves its 750 MB service root until the next
  sweep). Open: every service child (WebView service and renderers, microG) is a whole emulated runtime of 350 to 450 MiB
  (forked from the session Zygote since 2026-11-11; their memory has not been re-measured since)
  that idle-stops after 60 s only without a service record or provider reference; in Battle Cats the WebView service stays
  alive for an untraced reason. Trace that service; remove the retired realm's service root when it ends.

#### Measured so far (ads, WebView and windows)

From 135 session logs of ten installed apps (2026-10-24 to 11-07), read on 2026-11-07. `APP_MAIN_STALL` reports a main-thread
block of about 1.5 s or more once a second, so reports are merged into episodes (430 reports were 146 episodes; one 50 s
hang was 56 reports). By cause: WebView first start 53 episodes (median 1.7 s, p90 4.6 s, worst 11.7 s), cold app and SDK
code 40 (median 1.7 s), other WebView and Chromium work 32 (median 2.6 s). The two longest were Geometry Dash Lite's
`InputDevice.hasKeys` crash, where the ad SDKs' uncaught-exception handlers held the main thread for 45 s (fixed in
`f69bc8bd`), and one 50 s hardware-frame hang on 10-25. Only three sessions follow that fix, so the current state is thin.
Seven of the eight installed games scanned bundle the same mediation stack of about ten ad networks; Oddmar has none. The
logs are in `<state>/<package>/managed/sessions/*/stdout*.log`.

Acceptance: a supported SDK test creative loads, displays, accepts input, closes and returns to the game without losing
input or audio; rewarded tests deliver the SDK's real completion callback once; no-fill, cancellation, network failure and
renderer exit leave the game usable. Narrowing a resizable app's window (YouTube) gives its other layout, a fixed-size game
is tiled to a half or a corner without distortion, and a recorded ad-heavy session says where the lag is before any fix
for it is chosen.

### 3. Broader game support and less stutter (carried from alpha.2)

- Fix concrete startup and interaction blockers in selected games, starting with Azur Lane's delayed login UI, by
  completing service contracts rather than answering with empty success.
- Profile matching cold-load and warm gameplay scenes; reduce compilation stalls, repeated translation, memory-guard and
  spill costs where measurements justify it.
- Measure rendering, ART/GC, Binder and input delays alongside JIT cost.
- Exercise several engines and apps with audio, saves, resize, pause/resume and process exit, keeping Linux games working.

Acceptance: repeatable GPU-enabled runs report load time, CPU and compilation cost and median/p95/p99 frame times against a
baseline, with correctness checks and sustained interaction. A window title's instantaneous FPS is not the measurement.

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

- Steam Deck: Game Mode shows a black screen (not investigated), and a plain non-Steam shortcut to the library cannot reach the
  Android library because Steam's environment variables have to be unset first (`LD_PRELOAD` and related ones, per the maintainer; the exact set was not recorded). A per-app Steam
  shortcut helper that clears that environment and launches an app directly, full screen with a gamepad layout, would also
  skip the Qt library. The Deck's CPU ran hot at 20 to 40 FPS. A window that did not return on Return to app after quitting
  Subway Surfers is, in the maintainer's conclusion, that slow CPU and not a bug (not traced).
- A zero-copy Android Vulkan swapchain. The HAL's `VK_ANDROID_native_buffer` reads a full frame back per present (a GPU copy,
  a wait for the GPU and a CPU copy, then another copy into the window) because the software gralloc allocator is a sealed
  memfd the host driver cannot import as image memory; it needs a dma-buf-backed allocation. Planned in two stages: dma-buf
  gralloc buffers from the render node (removes the CPU copies; touches every Android app), then presenting them through
  `zwp_linux_dmabuf_v1` with explicit sync and a fallback to the current path. Worth doing if a 60 FPS game cannot hold 60
  with `present_ms` near 6; at 30 FPS the cost is about a sixth of the frame. The compositor and driver here (KWin, RADV)
  offer what it needs; `/dev/udmabuf` is root-only on this machine, so it is not a basis.
- ASTC through BC7 follow-ups: compare the decoder with ARM's reference (multi-partition and dual-plane blocks are checked
  only for self-consistency); a cache of finished transcodes that survives a restart (the in-memory one does not) and a
  faster encoder (the transcoder took 60 to 70% of Zenless Zone Zero's CPU samples in story scenes before the lowest-priority
  helpers and the cache); measure memory, load time and the cache's hit rate in a game; the GL/GLES path; reading back,
  copying and blitting ASTC images.
- Zenless Zone Zero: very low-resolution models (always, before and after the ASTC path; likely a quality tier or render
  scale chosen for an "unknown" device, unconfirmed: trace the 3D render-target size) and an unplaced guest null dereference
  seen once on 2026-11-08.
- `MediaPlayer` beyond FNAF Mobile Port (video to a Surface, streaming, seeking, long sessions); decoder validation beyond small
  fixtures (flush/reconfigure, B-frames, sustained playback).
- Audio effects: `android.media.audiofx` loads and reports no effects; `Equalizer` and the like need DSP in front of the host's audio.
- Activities from other processes beyond Play services; a real Play Games sign-in has not been observed.
- Orientation, resize, focus, surface loss/recreation and background/foreground tests with real apps. Activity relaunch for apps
  that do not declare `configChanges` is done (fixture only); `onSaveInstanceState` on an ordinary stop and `PersistableBundle`
  state are not.
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
  The desktop library takes a pad too (`android_gamepad.py`), and a per-app `controller` setting gives touch-only apps a pad-driven
  pointer (Start+Select, left stick, A; `PadCursor`). Still open: rumble, back paddles, button remapping, scrolling with the
  right stick in cursor mode, the Install APK file dialog (a window of its own the pad only steers like a keyboard), pads for
  Linux guests' own input paths, and a check of all of it with a real pad on Game Mode and a plain Xbox-style pad.
- Android touch, multitouch, keys and controller events; desktop-to-touch mappings stay configurable. Wheel/pinch zoom
  gestures and app pointer icons exist but are untried in live games.
- Keyboard layouts, held keys, text input and pause/menu transitions without stuck or duplicated events.

Acceptance: repeatable input probes and live game checks pass across the desktop modes and Android lifecycle transitions.

### WebView text input and external pages

Done: desktop text and composition reach native editors through `InputConnection`; http(s) and Play Store links open in
the host browser ([host text input](Android/ANDROID_RUNTIME_REFERENCE.md#host-text-input-2026-10-20),
[web links](Android/ANDROID_RUNTIME_REFERENCE.md#web-links-and-play-store-links-2026-10-20)).

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

Done: experimental Tier-2 regions (opt-in); a cheaper Tier-1 (CoreMark 13,156 iterations/s on October 27 and 14,874 on November 8, over the 10,000
target; [method](archive/JIT-history.md#coremark-re-measurement-2026-11-08)); compact scalar memory guards; shared cold stubs; native scalar and vector FP by element and LD1R; loop bodies up
to twice the block cap; lighter Q-register accesses. On a set of engine-shaped kernels the JIT went from 5.8× to 4.2× the
time of a native AVX2 build ([details](JIT.md)).

Also done on 2026-11-07, measured on a different, reconstructed kernel set (one clang source built for AArch64 and for
x86-64-v3, one pinned core, a game running; not a game): `LD2`-`LD4`/`ST2`-`ST4` native inside the block (a quaternion blend
22.7× to 4.2× native, 4-bone skinning 45.8× to 11.2×); scalar FP NaN steps emitted after the block (the profiled loop's
executed code spans 29% fewer cache lines); `fcmp` feeding a branch or integer select left in the host flags (cull 33.9× to
27.1×, collide 11.4× to 8.1×). The set's geometric mean went from 7.63× to about 4.96×. A kernel's ratio is guest
instructions over native instructions (the compiler: cull's AArch64 loop has 4.5 times the x86 build's instructions) times
host instructions per guest instruction (the JIT: 5 to 15 here); only the second is the JIT's to fix. L1 miss counters did
not move beyond noise, and CoreMark's last-level misses are fixed startup and translation costs
([details](archive/JIT-history.md#fp-compares-in-the-host-flags-and-where-last-level-misses-come-from-2026-11-07)).

Also done on 2026-11-10 and 11-11 (instruction and cycle counts on CoreMark, the `bench_*` programs and the kernel set; no
game measured): NZCV stored as the `lahf` image, PC stores behind the chain slot, `b.cond` loop pins kept in registers,
`fcsel` without `pushfq`, `ldr s`/`ldr d` straight into the cached XMM, one 16-byte slot per vector register, and `ccmp`/`ccmn`
flags left in the host. CoreMark runs 11.9% fewer host instructions; skinning and animation blending 35-42% fewer cycles.
A fork child keeps its parent's JIT ([details](JIT.md)).

Open, in priority order:

1. **Per-instruction overhead in scalar FP loops.** The cheap part is done (loads into the cached XMM, one-instruction
   result writes). Sampled FP kernels still spend 2-6% of ops on arithmetic and most of the rest on XMM copies, `v_hi`
   traffic, `pxor` zeroing and NaN checks. Dropping NaN checks exactly fits 3 of 277 checked ops; more would need relaxed
   NaN semantics, which is the maintainer's decision. A plain `v_hi` zero store (cull and collide about 30% slower) and
   `insertps` result writes were slower, so measure each change on a branchy and a predictable kernel, over several code
   placements ([details](archive/JIT-history.md#scalar-fp-loads-into-cached-xmms-and-cheaper-scalar-results-2026-11-10)).
2. **Guest-register traffic across blocks.** Guest register loads and stores are 20-27% of executed ops in the integer
   kernels, mostly block-entry loads and live-out stores that only cross-block allocation removes; a pass that hoists
   scratch work above a flag setter is measured but not merged (cycles within placement noise,
   [details](archive/JIT-history.md#guest-register-traffic-and-flag-windows-measured-not-merged-2026-11-10)).
3. **Paired-memory guards**, SIMD values kept past the all-or-nothing cache limits, and safe forwarding around atomics.
4. **Model guest state explicitly**, define recovery and helper contracts, then hand values between two regions and choose
   layouts by measured benefit (the five-stage plan in [the snapshot](archive/roadmap-2026-11-03.md#jit-codegen-retain-guest-state-across-hot-regions)).
5. **Tier-2**: regions now follow calls and returns, every side exit chains, a kept region retires when its exit rate
   outgrows its trial, and enabling it no longer slows Tier-1 code (2026-11-11). CoreMark runs 1.2% fewer host instructions
   than Tier-1 (run time within placement noise) and a particle loop 7.7% fewer cycles. It does not take single-block loops,
   which Tier-1 already keeps in registers. Loops whose iterations take different arms (culling), loops with an inner loop
   (skinning) and dispatch loops (a bytecode VM) are declined, because promotion judges a linear trace; raising the 8-block
   cap alone admits none of them. Next: regions with internal joins, then real Android titles. It stays opt-in until then
   ([details](archive/JIT-history.md#tier-2-exits-that-chain-exit-rate-feedback-and-profiler-costs-2026-11-11)).
6. Candidate: on AVX-512 hosts, `xmm16`-`xmm31` could hold more of the 32 guest vectors than the 13 cached today.

Targets: CoreMark at least 10,000 iterations/s (met); the proposed 26%-of-native CoreMark milestone (met on CoreMark: on the
5800X3D on November 7, native CoreMark v1.01 built with gcc 16.2.1 `-O2` ran 37,950 iterations/s on one core and the JIT 14,785,
about 39%; the Ryzen 5 9600X at 5.68 GHz that measured about 24,000 on November 4 is also at roughly 40%; both with the CPU
governor on `performance`; see [the checkpoint](archive/JIT-history.md#coremark-native-reference-2026-11-07)); game kernels toward 2× native
where the guest code allows it.

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
- `make bootstrap` on a fresh Debian 13 (tools off `PATH`, SDL3 built from source, no root): checked in an Ubuntu 24.04
  chroot and with a real SDL3 build here, not on Debian 13 itself.
- Fedora is unit-tested in a chroot only (run a game on Bazzite); Debian is compile-tested in containers; `make deps` failed
  on openSUSE; Alpine is untested ([support](SUPPORT.md)).

## Not planned

- **Linux games in the desktop library.** The library stays an Android app library; Linux games are started from the
  command line with their rootfs profile (decided by the maintainer, 2026-11-07, as a niche use).
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
