# bifrost-emu v2.0 roadmap

Status: in progress. The current version is **2.0.0-rc.1** (November 25, 2026), a release candidate: most of what it
promises works, and it is not production grade ([why](#what-keeps-it-from-being-production-grade)). Updated November 25, 2026.

Everything below is a development target, not a compatibility guarantee or a release date. "Done" means the contract is
implemented and checked as the linked document says, not that every app using it works. The list of finished work that used to be here is in
[the snapshot of 2026-11-25](archive/roadmap-2026-11-25.md), and what changed for users is in the [changelog](CHANGELOG.md).

## Goals for v2.0

- Run selected ARM64 Android games through the integrated Android runtime (real ART, framework and Bionic), installed and
  started from the desktop library.
- Sustain Linux game performance. Linux games run from the command line; the desktop library is for Android apps (see
  [not planned](#not-planned)).
- Make input dependable across mouse, keyboard, touch and gamepads.
- Keep correctness the gate for every optimization, and keep existing Linux applications working.

## Direction: games that feel native

The aim is an Android game that behaves like any other window on a Linux desktop. It should not crash, its window and its input should behave, and then it
should run **smoothly**. With compatibility mostly in place, the next releases go to the last point, in two parts:

1. **[Less bookkeeping](#1-less-bookkeeping-in-generated-code).** Most of what generated code runs is not the program's own work.
2. **[Less stutter](#2-games-without-stutter).** A game that averages 60 FPS and stops for a second when an ad loads does not feel native.

Not the focus: games behind attestation or anti-cheat, and compatibility sweeps with no concrete game to fix (see
[not planned](#not-planned)).

## Where things stand

| Milestone | State | Highlights |
|---|---|---|
| 2.0.0-alpha.1 (2026-10-12) | Released | Subsystem layout; Asgard on the pinned Android 15 framework; SpongeBob playable |
| 2.0.0-alpha.2 (2026-10-21) | Released; goals partly met | Toasts, `NsdManager`, all of `IAppOpsService`, declined Settings intents, scaled fixed-size apps |
| 2.0.0-alpha.3 (2026-11-01) | Released; WebView, media and ad scope mostly open | `media.extractor`, POSIX timers, installable Asgard and rootfs, `bifrost-emu app.apk`, window stats |
| 2.0.0-beta.1 (2026-11-03) | Released as a label; scope partly met | Library cards, per-app settings, clear-data/uninstall, run-ended cards; app Keystore, WebView GPU fences, Vulkan for apps through a guest HAL, a faster Lean JIT |
| 2.0.0-beta.2 (2026-11-14) | Released; scope partly met | Installs and updates that rebuild what changed, `make check-fresh`, a session Zygote and one shared Android image, gamepad control of the library, Tier-2 through calls and returns (since removed), NativeActivity games |
| 2.0.0-beta.3 (2026-11-24) | Released; a large release, scope partly met | Graphics calls cost a game much less CPU, the JIT translates about twice as fast and runs cheaper scalar float compares and widening multiplies, large memcpy and memset run on the host, `BIFROST_PROF` stall reports and a Tracy timeline, a per-app Resolution setting, an app launcher that keeps a log, and uninstall targets that find a `make bootstrap` install |
| 2.0.0-rc.1 (2026-11-25) | Current; release candidate, not production grade | Several threads translate at once, registers stay in host registers across branches and blocks (Carry), Tier-2 removed, windows that keep their shape, rotation inside the window; CoreMark 28% faster than beta.3 on the same guest build, half of native speed |

### What keeps it from being production grade

- **Android is experimental.** Media decoding and many system services are partial or missing, and Google services come from microG.
- **Few hosts are tested.** One AMD desktop for most of it, Ubuntu 24.04 in CI without a display; a Steam Deck only in Desktop mode.
- **Long sessions are little tested.** Statuses come from people playing, not from repeatable 30-minute runs with frame times.
- **Two changes in this version have not been timed in a game:** translating in several threads, and Carry.

**Games:** 21 recorded, 18 of them playable (14 Android, 4 Linux), 1 starts without confirmed gameplay and 2 are blocked; see
[What has been run](COMPATIBILITY.md) and `Scripts/Android/compatibility.json`. Statuses are user reports unless a page says
otherwise.

**Known blockers, by cause:**

- **Needs real Google attestation or a real device:** TrickCal's anti-cheat ends it; Umamusume JP stops at its device
  integrity check. Bifrost does not bypass or forge any of them.
- **Cause not established:** Roblox logs in, joins live servers, and about 170 s later the server disconnects it with
  reason 304 ("missing or corrupted files", 2026-10-19). The client logs no file or hash failure and its Play Integrity
  token request also fails; that the failed request causes the kick is inferred, not proven, and so is whether Roblox
  checks Google attestation at all. Not investigated since.
- **Needs Google Play services beyond microG:** Play Games sign-in got HTTP 404 and `SIGN_IN_REQUIRED` in Hungry Shark World and Umamusume
  (2026-10-21). Hungry Shark World signed in on 2026-11-25; why it works now is not established, and Umamusume was not re-checked.
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

In order. Each carries into the next release until its acceptance test passes.

### 1. Less bookkeeping in generated code

Generated code runs many host instructions per guest instruction, and little of it is the program's arithmetic:

| Measured on | What it is |
|---|---|
| Zenless Zone Zero and Umamusume (2026-11-23) | Guest state traffic is 23 to 24% of the generated code's uops ([details](archive/JIT-history.md#game-profiles-zenless-zone-zero-and-umamusume-2026-11-23)) |
| SpongeBob loading (2026-11-24) | Body time: guest state moves 41%, memory with guards 22%, integer ALU 14%, branches 14%, FP and SIMD 6% ([details](archive/JIT-history.md#spongebob-profile-with-the-stats-counters-off-2026-11-24)) |
| Game-style kernels (2026-11-23) | XMM moves and zeroing 28 to 36%, vector state 15 to 21%, NaN checks 11 to 14%, FP math 5 to 7% |
| CoreMark with Carry (2026-11-24) | Guest state moves 37% of all samples ([details](archive/JIT-history.md#coremark-with-carry-on-the-bottleneck-is-still-guest-state-traffic-2026-11-24)) |

Work, in order of expected gain for its risk:

1. **Flag saves.** A guarded access saves NZCV first because a fault must show it (6.5% of CoreMark's generated-code ops). Keep the flags in the host across the
   access and rebuild them in the fault's recovery code.
2. **Loop bodies.** Loop-invariant registers are reloaded and overwritten temporaries stored on every iteration. RSI and RDI as more pins; align loop bodies (a
   body that crosses a 64-byte line costs a cycle per iteration on Zen 3, and placement alone moved bzip2 by 17%).
3. **Block edges.** Merge a block into its single predecessor (up to 6.6% of a game's uops, an upper bound), shorter chain cells (about 1.9%), a cheaper preemption
   poll (1.5%), and a cheaper return path.
4. **Vector copies.** 128-bit VEX three-operand forms instead of a copy and a two-operand one (untried; FMA3 is already required, so AVX is there), and the
   256-bit paths that handle 128-bit vectors. Measure over several code placements, on a branchy and a predictable kernel.
5. **Memory guards.** Their share in a game is unmeasured: break a game's generated code down first, then paired accesses and safe forwarding around atomics.
6. **What still reaches the interpreter:** `ldxp`/`stxp` pairs with their retry loops, and 64-bit-lane saturating narrowing shifts. Profile the ART
   start-up for the hottest of these.
7. **Carry regions** ([design](JIT.md#planned-codegen-retaining-guest-state-across-regions)): guest registers kept in host registers across loops of several blocks. A simulation on
   CoreMark puts the ceiling at 63 to 84% of register moves; only 11% of CoreMark's samples and 1.4% of SpongeBob's loading are in such loops, so it comes after the above.
8. **Candidates:** `xmm16` to `xmm31` on AVX-512 hosts; relaxed NaN checks are the maintainer's decision.

Acceptance: fewer host instructions per guest instruction on CoreMark, the kernels and a sampled game, with cycles checked over several code placements,
`make verify` and the fault and signal fixtures passing, and no slower Android start-up.

### 2. Games without stutter

Known causes, and what comes next for each:

| Cause | What is known | Next |
|---|---|---|
| No frame-time record | Stalls under about 1.5 s are invisible in the logs, and an average FPS hides them | An opt-in frame-time log, first; then an ad-heavy session split into the ad showing, the game while it loads, and closing it |
| Translating new code | Threads waited for one compiler 1.2 to 2 times as long as they compiled (SpongeBob, Battle Cats loading). Several compile at once now: eight threads finish a translate-only test ten times sooner than before ([numbers](archive/JIT-history.md#several-threads-translate-at-once-2026-11-25)) | `BIFROST_PROF` on a game's loading, which shows translating and waiting per thread; the publication itself is the serial rest |
| Cold code pays for Carry | 9.0 us a block against 4.8 us with Carry off on the ART start-up (2026-11-25) | **Lean first, Carry when hot**: a Lean trigger that goes away once it fires. Accept when the ART start-up is back near Lean's time and CoreMark keeps Carry's |
| WebView's first start | 53 of 146 main-thread stalls, median 1.7 s, worst 11.7 s (2026-11-07) | A hidden WebView during the loading screen (untried); the stall log shows whether the median drops |
| Cold app and SDK code | 40 of 146 stalls, median 1.7 s | Persistent JIT cache on by default, once a game shows a gain and plays from it |
| A full code cache | Replaced as a whole, so the working set is translated again | Keep hot blocks across the replacement, with bounded memory |
| Texture transcoding | 60 to 70% of Zenless Zone Zero's samples in story scenes before its helpers got the lowest priority | A faster encoder and a transcode cache that survives a restart |
| Presenting Vulkan frames | A full frame is read back and copied per present | dma-buf gralloc buffers, if a 60 FPS game cannot hold 60 (details under graphics below) |
| Shaders and pipelines | Not measured | A cache of their own, measured apart from translation |

Acceptance: repeatable runs of a game report load time, compile time and median, p95 and p99 frame times against a baseline, with correctness checks. A window title's
FPS is not the measurement.

### 3. Easier setup and a dependable app library

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
- A rebuilt app runtime no longer copies or hashes the app's data: "Preparing Android runtime" for Zenless Zone Zero (38 GB of
  downloaded resources) went from 105 s to 2 s (2026-11-23; measured on a real launch). The first launch after an update still
  checks the runtime files.
- Validate install → play → settings → stop/relaunch on a fresh setup, and broaden the app matrix with longer sessions and
  several apps in one realm on the supported Linux display stacks.

Acceptance: a fresh user provisions and launches a supported app without editing paths or environment variables.
Supported apps complete repeatable 30-minute interactive sessions with correct saves, audio and input; a failed install or
app exit leaves the library and the other apps usable.

### 4. Ads, WebView and windows that behave

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
- Rotation and scaling of the host window. Done 2026-11-25, checked against a fake SDL (opening maximised also in Hungry Shark World on KWin): a fixed-size app's
  host window keeps its size on rotation and the panel refits inside it, with a per-app Rotation setting for the old reshaping;
  a window that closed maximised opens maximised, so a game's first size is the one it keeps
  ([details](Android/ANDROID_RUNTIME_REFERENCE.md#fullscreen-and-remembered-size-2026-11-07)). Open: confirm rotation in Umamusume and opening
  maximised in Geometry Dash Lite and Family Island;
  `eglQuerySurface` answering the old size right after a resize (read from source, no fault tied to it); fractional scales and
  several monitors, with no other report so far.
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

### 5. Broader game support

- Fix concrete startup and interaction blockers in selected games, starting with Azur Lane's delayed login UI, by
  completing service contracts rather than answering with empty success.
- Measure rendering, ART/GC, Binder and input delays alongside JIT cost.
- Exercise several engines and apps with audio, saves, resize, pause/resume and process exit, keeping Linux games working.

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
- Activities from other processes beyond Play services; one real Play Games sign-in is on record (Hungry Shark World, 2026-11-25).
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
- `test_fork_lock_contention` fails on some machines and runs, on 2.0.0-beta.3 too: a fork tries every emulator lock at once for up to 3 s, and while two
  threads map and unmap memory nearly all its failed tries are on the memory index lock (43,978 of about 46,000 in a trace of 2026-11-25; 3 on the
  JIT's). It then forks without the locks. A game that starts helper processes while it loads can pay that wait. Give the fork a fair turn at that lock.
- A guest process hangs at exit while another of its threads is blocked in `pause()` (JIT and interpreter; QEMU exits). Found on 2026-11-25, not
  investigated.

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
