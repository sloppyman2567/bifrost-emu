# bifrost-emu v2.0 roadmap

Status: in progress; the current pre-release is 2.0.0-alpha.3. Updated
November 1, 2026. Except for checkpoints explicitly marked implemented below,
these are development targets, not compatibility guarantees or a release date.

v2.0 aims to run selected ARM64 Android games through an integrated Android
runtime, improve sustained Linux game performance, and make input reliable
across mouse, keyboard, touch and gamepads. Correctness remains the gate for
every optimization. Existing Linux applications must keep working.

## Progress at a glance (2026-11-01)

Statuses are what the changelog and app pages record; none of them is a
compatibility guarantee. "Done" means the contract is implemented and checked
as the linked section says, not that every app using it works.

| Milestone | State | Done so far | Still open |
|---|---|---|---|
| 2.0.0-alpha.1 (2026-10-12) | Released | Subsystem layout and descriptive file names; Asgard Android runtime on the pinned Android 15 framework; SpongeBob playable | Goals below |
| 2.0.0-alpha.2 (2026-10-21) | Released; its goals were partly met | Toast queue and windows (Azur Lane's sign-in failure no longer exits); `NsdManager`; all 59 `IAppOpsService` methods; Settings intents declined for every app; fixed-size apps scaled to the window; large guest mappings no longer committed up front; real Google sign-in through a window (Hungry Shark World); Uptodown, Battle Cats and Hungry Shark Evolution start to a window. | Measured frame-time baselines; the roughly 7 s main-thread wait on the first ad load; cold-load and compile stalls; sustained 30-minute play |
| 2.0.0-alpha.3 (2026-11-01) | Released; its WebView, media and ad scope is mostly open | Since alpha.2: `media.extractor` and POSIX timers, so video opens through Android's media stack; a fix for the Java Binder abort on native threads; session-wide window stats; lower multithreaded JIT overhead and native pixel/media NEON forms, guest `ptrace`, per-process compatibility defaults and child service routing, a redesigned desktop library with per-app compatibility profiles, `bifrost-emu app.apk`, and an installable rootfs; guest pseudo terminals and fork/exec fixes; less generated JIT code per guest instruction and an experimental persistent translation cache; a window stats overlay; a Debian package; `fcntl` owner, signal and lease commands, so Umamusume's anti-tamper helper no longer ends the app about 8 s in (the maintainer has since reached a Career story scene); a full JIT code cache is replaced and idle threads cost less memory; Subway Surfers 3.70.3 recorded playable (the maintainer's report, no capture). Partial: WebView renders; desktop text input reaches native editors; http(s) and Play Store links open in the host browser after a recent input | WebView pointer/focus/scroll, return/deep-link delivery, ads with test creatives, `media.player` and sustained video |
| 2.0.0-beta.1 | Not started | Library cards, per-game settings, clear-data/uninstall | Guided provisioning, update/reinstall, save export/import, fresh-setup validation |

Known blockers, by cause:

- **Needs real Google attestation or a real device:** Roblox joins servers and
  is then disconnected (reason 304); TrickCal's own anti-cheat ends it;
  Umamusume JP 2.32.0 stops at its device integrity check when it connects (the
  user's report). Bifrost does not bypass or forge any of them.
- **Needs Google Play services:** SpongeBob's adult-age path stalls at 73%, and
  Hungry Shark World's Play Games sign-in gets HTTP 404 and `SIGN_IN_REQUIRED`.
- **Architecture:** Bifrost runs AArch64 only, so an APK that ships only
  `armeabi-v7a` or `x86` libraries (Angry Birds Space) cannot start.
- **Platform gaps still listed below:** no Settings screens, no host mDNS,
  no Android Vulkan HAL, no `media.player` service, no toast animations.
- **Undiagnosed:** Firefox 159 reaches onboarding and a GeckoView surface, but pages do not load. Most of its sessions
  ended on the Java Binder abort fixed on November 1; it has not been re-tested since.

## Next release priorities

These are proposed scopes, not announced release dates. The runtime is 2.0.0-alpha.3. The alpha.2 and alpha.3 scopes
below shipped partly; what they still list carries to the next pre-release, ahead of beta.1. The
[SpongeBob screenshots](Apps/spongebob.md#screenshots-2026-11-01) show cooking,
the lobby, restaurant progress, Help Center and a level-14 pause menu; the
[Azur Lane page](Apps/azur-lane.md#measurements-2026-11-01-200-alpha3) shows its home screen and the
[Umamusume screenshots](Apps/umamusume.md#screenshots-2026-11-01) show its title, home and a Career story scene.
Still images do not establish sustained performance
or successful ads, sign-in and gameplay in every app.

### Carried from 2.0.0-alpha.2: broader game support and less stutter

- Repair concrete startup and interaction blockers in selected games, starting
  with Azur Lane's delayed login UI (its toast failure is fixed; the maintainer has not said whether the delay remains). Complete
  connected common-service contracts rather than returning success from empty
  handlers.
- Remove the first-ad-load wait: every Google Mobile Ads game blocks its main
  thread (about 7 s in Battle Cats) in `DynamiteModule.getRemoteVersion`. Trace
  what the Play services provider waits for before choosing a fix.
- Profile matching cold-load and warm gameplay scenes. Reduce compilation stalls,
  repeated translation, paired-memory guard overhead and vector spills where
  measurements justify it. Preserve fault, alias, invalidation and FP semantics.
- Measure rendering, ART/GC, Binder/service work and input delays alongside JIT
  cost. Keep supported software and hardware media routes explicit.
- Exercise multiple selected Android engines/apps, with audio, saved data,
  resize, pause/resume and process exit; retain working Linux game behavior. The maintainer reports working audio and
  pause/resume in Azur Lane (November 1); the others are unrecorded.

Acceptance: repeatable GPU-enabled runs report load time, CPU/compilation cost
and median/p95/p99 frame times against the current baseline. Lower tail frame
times must accompany correctness checks and sustained interaction; a window
title's instantaneous FPS is not the acceptance measurement.

### Carried from 2.0.0-alpha.3: WebView, media and possible ad support

- Finish attached WebView pointer/focus/scroll delivery. Desktop text and IME
  composition now reach native editors through `InputConnection`
  ([host text input](Android/ANDROID_RUNTIME.md#host-text-input-2026-10-20));
  Chromium editors in a WebView, caret-anchored candidate windows
  (`CursorAnchorInfo`), clipboard exchange and an on-screen keyboard are not
  done. Help Center rendering is visible now; complete browser/editor
  interaction still needs its own tests.
- Plain http(s) and Play Store `ACTION_VIEW` links already open in the host
  browser within 5 s of an input
  ([web links](Android/ANDROID_RUNTIME.md#web-links-and-play-store-links-2026-10-20)).
  Still open: supported return/deep-link delivery, Custom Tabs, verified App
  Links and the other implicit intents, with app lifecycle and failed-browser
  handling.
- Trace SpongeBob's stuck Kidoz ad from the null read and incorrect layout size.
  Extend Surface/media lifecycle, reconfiguration, audio synchronization and
  renderer-process handling for the path the SDK actually uses.
- Investigate supported rewarded/interstitial ad SDKs with their test creatives.
  Supply the platform/service contracts they require, and report unmet Google
  Play or server prerequisites. Ad support remains experimental until a real
  request, display and completion path works; do not synthesize reward success.

Acceptance: a supported SDK test creative loads, displays, accepts interaction,
closes and returns to the game without losing input/audio. Rewarded tests must
deliver the SDK's actual completion callback once. Cancellation, no-fill,
network failure and renderer exit must leave the game usable. This milestone
does not promise every ad network or Google Play Services support.

### 2.0.0-beta.1: easier setup and a dependable game library

- Guide dependency/runtime provisioning and signed APK installation from the
  library; show actionable missing-runtime or window-backend errors.
- Make the launcher select the intended graphics-capable executable and keep
  headless validation from silently replacing the desktop default.
- Build on the current cards, icons, search, per-game Back-key/stats settings,
  clear-data/uninstall and last-session drawer. Add clear update/reinstall and
  save export/import flows, with routine controls outside Developer.
- Validate install → play → settings → stop/relaunch on a fresh setup. Broaden
  the game matrix, including longer gameplay and multi-app realm/lifecycle
  checks on supported Linux display stacks.

Acceptance: a fresh user can provision and launch a supported game without
editing paths or environment variables. Supported games complete repeatable
30-minute interactive sessions with correct save, audio and input behavior;
failed installs and app exits must leave the library and other apps usable.

## Android games: Bionic and platform compatibility

Build a dedicated Bionic/Android syscall and platform compatibility layer
on the existing AArch64 Linux syscall implementation. Bionic uses Linux
syscalls; this work must also cover the Android userspace environment,
runtime libraries, loader behavior and required platform services.

Implemented native checkpoint: real guest linker64/Bionic startup, signed
base/split installation, native-only NativeActivity launch, extracted assets,
persistent per-app storage, plugin-backed graphics and a bounded native Binder
service registry. The packaged fixture passes all six execution-mode/window
backend combinations. [APK setup and boundaries](Android/ANDROID_APK.md) document
the exact scope. Implemented managed checkpoints now include the pinned Android
15 ART/framework, APK Activity creation, resources, JNI, WindowManager-owned
Activity windows and a shared Binder realm. SpongeBob has user-confirmed cooking
levels and menus; Azur Lane and Umamusume Global are user-reported playable with screenshots, and The Battle Cats,
Subway Surfers and Geometry Dash Lite without captures (see
`Scripts/Android/compatibility.json`). Those checkpoints do not establish
arbitrary APK compatibility.

- Expand the pinned Android 15/API-35 baseline with documented runtime
  provenance and limits instead of claiming every Android release works.
- Extend Bionic libc, libm, libdl, pthread/TLS, signal, futex, mapping and
  Android ioctl/device coverage from the tested contracts.
- Implement linker namespaces, Android library search rules and
  `android_dlopen_ext` behavior, with isolated per-app library resolution.
  Android's [linker namespace documentation](https://source.android.com/docs/core/architecture/vndk/linker-namespace)
  and [Bionic linker changes](https://android.git.googlesource.com/platform/bionic/+/HEAD/android-changes-for-ndk-developers.md)
  are the compatibility references.
- Load APK manifests, resources, DEX and ARM64 native libraries; handle
  split packages and app assets as later milestones of the same loader.
- Provide per-app data/cache directories, persistent saves, configuration,
  logging and a defined permissions model.
- Add the required Binder/service contracts, properties, lifecycle and
  resource access incrementally. Report unsupported services explicitly.

Acceptance: a Bionic-linked native test, a packaged NativeActivity sample
and a selected Android game launch through the supported Android profile,
with working storage, lifecycle and native library loading.

## Integrated ART and JNI

ART is already integrated and working. Asgard runs the real Android runtime
and framework, including DEX execution and JNI calls into ARM64 game engines.
SpongeBob already uses this path.

Next steps:

- Make managed game startup and runtime faster, guided by real-game profiles.
- Improve stability during heavy garbage collection, thread activity,
  pause/resume, shutdown and concurrent apps.
- Test more games and broaden coverage of class loading, JNI callbacks and
  ART-generated code. Preserve correct memory protection and code invalidation.
- Complete the Android framework services needed by additional games.

Acceptance: more games sustain interactive play, survive lifecycle and memory
stress, and show measured startup/performance improvements without breaking
existing games.

Historical checkpoint (October 5): the pinned guest ART creates its VM and passes the
DEX/JNI fixture (`Scripts/Android/test_android_jni.py`) in JIT and interpreter modes,
in ~25 s under the JIT versus ~1 s under QEMU. The framework subset, GC under
load, pause/resume and Activity launch were still open at that checkpoint.

Current checkpoint (October 14): the installed-app path uses real guest ART,
the Android 15 framework and app resources; Activities and local/remote Services
have creation, lifecycle, binding and process-death regression coverage.
Settings, PendingIntents, broadcasts and jobs have connected process-local
contracts. The complete managed service batch passes 176 assertions in each
engine. Full background execution policy, cross-app framework delivery and the
isolated WebView handshake remain incomplete. See
[runtime contracts and limits](Android/ANDROID_RUNTIME.md).

## Common services and background work

Implemented checkpoint (October 21): the service-inventory probe lists 37
Java services; 329 signatures are classified as implemented, 1,085 as
unsupported and 1,168 are unclassified (lambda dispatch without a per-method
contract). Four services cover every interface method: `appops`,
`permission_checker`, `batteryproperties` and `vibrator_manager`. `NsdManager`
was added after that run. The counts say what is declared, not how much of the
API apps use.

Implemented checkpoint (October 14): the service-inventory probe lists 31
system-facing Java services and two internal entries. That inventory explicitly
classifies 187 signatures as supported and 954 as unsupported; 1,187 signatures
remain unclassified. Unclassified handlers can have implementations, so these
numbers do not measure complete API coverage. Native Binder and codec endpoints
are additional. UserManager supports 26/104 signatures, device-idle 13/22 and
power 18/67, following the fixed user-0/device policy with caller checks and
isolated replies.

- Complete related services through real state, ownership, creation and
  cleanup; keep unsupported methods explicit and improve coverage reporting.
  Since October 25 a call a stand-in does not answer is named once per process
  in the session log (`SERVICE_CALL_UNSUPPORTED <service> <call>`), and eleven
  `IDisplayManager` methods answer as Android 15 does on a one-panel phone.
  Ranking those lines across 606 recorded sessions (17 apps) found four small gaps, answered on October 28:
  `queryIntentReceivers`, `getZenMode`, `notifyProviderInheritance` and `getServices`. The largest still
  unanswered is `ISurfaceComposer.createConnection` (60 sessions, 5 apps), which needs the SurfaceControl layer
  model.
- Toasts queue, show for 2 s or 3.5 s and cancel (Azur Lane's sign-in failure
  no longer exits). Still open: background-toast policy and toast animations.
- `NsdManager` constructs and discovery runs but finds nothing: the host has
  no mDNS responder, so resolving and advertising fail. Real discovery needs a
  host backend.
- Settings screens do not exist: `android.settings.*` intents are declined, so
  an app's all-files-access request cannot be granted from its dialog.
- Show posted notifications on the host and deliver their taps, then the
  foreground-service start deadline, background-start restrictions and time limits.
- Extend current PendingIntent/receiver/alarm/job behavior toward scoped
  process-restart and cross-app delivery. Current sender records and alarms
  do not survive process exit or restart stopped applications.
- Extend Settings only where required: generated defaults, validated per-app
  System overrides, generations and observers already work. Device-wide
  shared mutable state, query/cursor APIs, DeviceConfig overrides and privileged
  administration remain separate work.

Acceptance: manager clients exercise state changes, permission denials, copied
replies and cleanup, with real callbacks and app lifecycle checks in both
engines. Concurrent-client claims additionally require distinct UIDs, peer exit
and continued use of cached Binder handles.

## Android graphics, audio and lifecycle

Implemented checkpoint (October 14): managed OpenSL ES PCM playback uses
AAudio/Bionic callbacks; the real Media JNI and Codec2 stacks are provisioned,
with realm-owned Gralloc5 and host CPU/GPU decoder routing. Bounded codec probes
check exact 64x64 frames, PTS/EOS, genuine Surface output and service survival
after client exit. Aliased native-window dimensions now follow SurfaceView
geometry; the user confirmed clean resizing. These probes do not establish
video-ad playback or full lifecycle/recreation behavior.

Implemented checkpoint (November 1): the real `media.extractor` daemon runs in the realm, so `MediaExtractor` and
`AMediaExtractor` open files. Oddmar gets past its opening video and on to the next one, with Unity's `VideoPlayer` and the
real codec stack, in a 75 s session and by the maintainer's report. `media.player` is still absent, so `MediaPlayer`
video is not expected to play. Oddmar's gameplay, its story videos and audio sync during video are unchecked.

- Extend EGL/OpenGL ES/Vulkan, ANativeWindow and buffer ownership so
  Android game surfaces present correctly through the host graphics path.
  Use the [AOSP graphics architecture](https://source.android.com/docs/core/graphics/architecture)
  to define the required surface/buffer contracts; choose a supported service
  bridge or compositor integration before promising full SurfaceFlinger.
  Android Vulkan has no HAL yet: AOSP's loader finds no `vulkan.*.so`, so
  apps see zero physical devices (Roblox falls back to GLES). A HAL over the
  host Vulkan thunk needs guest-side dispatchable objects (the loader writes
  its dispatch table into them; thunk handles are host pointers) translated
  wherever they cross, and `VK_ANDROID_native_buffer` swapchain images on
  gralloc buffers.
- Start activities of other processes, first Play services (microG) sign-in
  and credential UIs: run the activity in its own process with a window,
  deliver its result to the caller's `resultTo`, and resolve foreign
  PendingIntents through their creator. Done for Play services Activity senders
  (a foreign sender is asked what it targets, then runs in the relay window);
  other foreign senders are still reported cancelled. A real Azur Lane Play Games
  sign-in has not been observed.
- Cover the selected games' audio APIs, callback threading, buffer timing
  and device changes, preserving sound during normal gameplay.
- Test orientation, resize, focus, pause/resume, surface loss/recreation
  and background/foreground transitions.
- Validate decoded output beyond the small fixtures: flush/reconfigure,
  B-frames, adaptive streams, supported resolutions and sustained in-game
  playback. HDR, Wayland readback, full SurfaceView callback/recreation sequences
  and native EGL into aliased surfaces remain unestablished.
- Expand graphics ABI and extension coverage based on complete dependency
  audits and reproducible failures rather than one missing import at a time.
  The GL/GLES thunk registry now has a `make generated-check` guard
  (`Tools/CodeGen/glxmlcheck.py`) that fails a pointer-to-pointer parameter
  without a translation policy; string arrays and `glGet*Pointerv` queries are
  fixed (a Geometry Dash Lite WebView ad had crashed in `glGetUniformIndices`).
  Nested arrays in Vulkan were not examined.
- Mobile GLSL on desktop drivers: fixed in the thunk, not yet seen in the game.
  The failing Roblox vertex shaders (29 of the 925 in `shaders_glsles3.pack`:
  grass, smooth terrain, mesh) index a flattened uniform array with a `uint`
  scaled by an `int` literal (`CB3[(_500 & 63u) * 1 + 0]`, SPIRV-Cross output).
  GLSL ES 3.00 has no implicit int to uint conversion, so Mesa rejects it and
  mobile drivers accept it. `GL_EXT_shader_implicit_conversions` cannot be
  exposed instead: it needs ES 3.1, Mesa refuses it in a `300 es` shader, and
  moving one shader to `310 es` fails to link with its `300 es` partner. After
  a driver rejects an ES 3.x source, `glCompileShader` retries it once with the
  index operand wrapped in `int()`
  ([ShaderIndexConversion](../Source/System/Graphics/AGENTS.md)). All 29 compile
  on Mesa 26.2.3 with the rewrite; a run of Roblox on it, and shaders outside
  this one pattern, are unchecked. Programs built by `glCreateShaderProgramv`
  or from binaries do not take the retry.

- Window scaling (observed October 21, 3440x1440 monitor): fixed in `4e41f771`
  (October 21; seen working October 27). The Battle Cats surface and its WebView pages no longer sit in a
  corner on black; see [the changelog](archive/CHANGELOG-2.0.0-alpha.2.md#non-resizable-apps-keep-their-size-and-are-scaled-to-the-window-2026-10-21).
  Roblox's black bar was not rechecked separately, and X11 layers are not fitted.
- Low-resolution Roblox textures at desktop resolutions are unexplained. Compare
  the rendered texture size with the app's request before blaming filtering.
- Memory: one game (Hungry Shark World, observed October 21) used about
  4.3 GiB PSS over three processes (main 2.7 GiB, `oxed_process0` 1.1 GiB, Play
  Store 0.4 GiB) while the host swapped about 2 GiB; 910 MiB of the main
  process was the 1 GiB FrostJIT code buffer. Battle Cats measured 5.25 GiB PSS
  over five runtimes (October 24: mostly JIT code, about 1.4 GB, and live host
  heap). Since then: smaller per-block metadata, small per-thread caches, no
  Play Store billing runtime, a 512 MiB default code budget whose full cache is
  replaced, idle guest threads at 0.15 MiB instead of 1.8 MiB, and a `[mem]`
  line that splits resident memory by class. Battle Cats then peaked at 4.41 GiB
  and Melon Playground held 3.87 GB against 4.70 GB (runs of different length,
  indicative only; the 363-thread saving is calculated, not measured in the
  game). Still open: JIT code size, the game's remaining non-JIT heap (about
  500 MiB in Battle Cats), why the WebView service stays alive, and a
  controlled before/after run. The window title's VRAM figure for Battle Cats
  (about 205-257 MiB of 8573) has not been checked against the driver.
  Since November 1 the window statistics count the whole session by PSS rather than one process's RSS, so earlier
  title-bar figures read lower. On that measure Azur Lane held about 3.2 GiB and Umamusume Global about 4.4 GB;
  long-session memory is unmeasured.

Acceptance: rendered gameplay with working audio and input survives repeated
surface recreation and lifecycle transitions without leaks or stale handles.

## Doom 3 performance and frame pacing

Target sustained improvement at 3440×1440 on a documented reference host.
The maintainer reports (2026-11-01, not an instrumented measurement) mostly
sustained 60 FPS in the Doom 3 demo, even in its heavy scenes. Older reports, from
builds that predate many JIT changes and probably also the demo, gave about 20–30 FPS
in demanding scenes (October 28) and 1–5 FPS in the spider-robot encounter (October 5).
Measure first: matched-scene frame times (median, p95/p99) in
the demo and, where available, the full game on a documented host, to confirm that
report and find the remaining drops, then remove them where host resources permit.
Cutscene FPS alone is not the acceptance criterion.

- Fix profiler attribution for every guest thread and after code-cache
  growth. Separate translated execution, dispatch, native helpers, thunks,
  memory slow paths, contention, compilation and host graphics work.
- Benchmark repeatable scene entry, scan/dialogue, exploration and combat
  with frame times, median FPS, low-percentile FPS and stutter duration.
- Reduce block lookup/call overhead, register spill/reload traffic and
  expensive memory paths using measured hot spots and differential tests.
- Reintroduce beneficial chaining/Tier-2 behavior in multithreaded games
  only with safe code publication, invalidation and reclamation.
- Preserve animation, rendering, audio and input correctness. Fix save/load
  fidelity so performance routes can be resumed and reproduced reliably.

Acceptance: matched cold/warm scene runs demonstrate improvements in frame
times and demanding-scene minimums, with no new correctness regressions.
Publish host, settings, scene and measurement duration with each result.

## JIT codegen: retain guest state across hot regions

Planned v2 direction: keep guest integer registers, vector/FP values and
NZCV in host registers across compatible hot paths, with explicit state
reconstruction at observable boundaries. Extend the existing compiler in
measured stages. The detailed contracts are in
[the JIT design plan](JIT.md#planned-codegen-retaining-guest-state-across-regions).

Candidate, not started and not measured: the JIT detects AVX-512 but emits none. On such a host, `xmm16`-`xmm31` could hold more
of the 32 guest vector registers than the 13 of `XMM3`-`XMM15` today, and `psraq` and variable shifts could replace fallbacks.
Guest vectors are 128 bits, so a 512-bit datapath itself would add little.

Implemented foundation (October 12): experimental Tier-2 collects up to
eight blocks/256 instructions, retains compatible vector or scalar-FP cache
layouts inside regions, and uses live-work timing to select candidates.
Measured losses can retry smaller prefixes (8 -> 4 -> 2) after fresh profiling.
MT publication uses revocable data cells. Mixed FP/vector allocation,
register-layout handoff between regions and general state-recovery maps
remain planned; Tier-2 remains opt-in.

Implemented Tier-1 checkpoint (October 14): folded constants no longer retain
dead operands; W-form ALU/extension fusion, deferred scalar guard tails,
architectural-only exit spills, complete chain-slot restoration and bounded
known-entry loop growth reduce emitted work. Final interleaved CoreMark medians
improve from 9,049.03 to 9,893.87 iterations/s (9.3%), with every CRC checked.
The Android probe also exposed and regression-tested a dead-code compaction
reserve-accounting underflow. See [implementation](archive/JIT-history.md#tier-1-hot-path-code-reduction-2026-10-14)
and [validation](archive/TESTS-history.md#tier-1-hot-path-code-reduction-2026-10-14).

Implemented memory-access checkpoint (October 26): the bounds table doubles as
the software-check gate, so a scalar access tests one compare instead of a bounds
compare plus a flag test; the checked path is one compact sequence; fault exits
no longer reload the PC or save flags and share a frame return; chain-skip exits
no longer load RDI/RSI. `qt_test` emits 95.2 bytes per guest instruction
(110.2 before the fault-exit change, 98.6 after it), and `bench_fib`,
`bench_memcpy` and `bench_sort` ran 10%, 6% and 5% faster at the gate step. ART
apps no longer run every access through the software check because of
`userfaultfd` (1.8x on a load/store loop). See
[JIT.md](JIT.md#software-page-checks-only-while-a-page-needs-them-2026-10-26).
In-game frame times were not measured.

Next, prioritize sampled codegen paths rather than emitted size alone:

- Streamline paired-memory guards while preserving whole-instruction footprint
  checks, precise faults and register/writeback ordering. Scalar accesses have
  the compact path above; pairs still use the whole-footprint guard.
- Retain useful SIMD values when a block exceeds the current all-or-nothing
  cache limits; model helper clobbers, aliases and NaN recovery explicitly.
- Model forwarding barriers sufficiently to optimize safe segments around
  atomic/acquire operations. Keep the current conservative rules until verified.
- Remove redundant load extensions and use immediate bitfield/rotate forms
  where their semantics match the reference.

The corrected Azur Lane audit inspected 24 sampled blocks, about 12.1% of the
startup profile's cycles. Its first replay had confused ELF file offsets with
virtual addresses; the earlier 353-byte single-instruction hotspot claim was
withdrawn. Resolve offsets through PT_LOAD and distinguish a replay from live
cache/entry-order state. None of these new audit targets is implemented yet;
the estimated speedups are not measured results. Capture the actual delayed
login scene before assigning a cause to its repeated UI stutters.

1. **Establish the cost breakdown.** Profile CoreMark and repeatable game
   scenes, separating generated integer code, spills/reloads, flag handling,
   region boundaries, helpers, memory guards and compilation. Record code
   size, compile time and cold/warm frame times before choosing optimizations.
2. **Model guest state explicitly.** Extend IR value/state tracking for GPRs,
   vector lanes, scalar FP aliases and NZCV. The first allocation prototype
   supports mixed FP/vector operations inside existing superblocks, preserving
   partial writes and upper-lane clearing. Integer allocation and flags are
   the priority for the separate CoreMark target.
3. **Define recovery and helper contracts.** Describe where architectural
   values live and how to restore precise guest state for faults, signals,
   interpreter fallback and host calls. Classify helper reads, writes and
   clobbers; unclassified helpers retain full state synchronization. Validate
   these contracts before allowing dirty state to cross region boundaries.
4. **Prototype handoff between two regions.** Give each region an explicit
   entry/exit register layout. Compatible hot edges transfer values directly;
   other layouts use bounded parallel-copy adapters or the canonical memory
   entry. Preserve copy cycles, aliases, flags, frame layout, stop sentinels,
   execution leases and concurrent invalidation in ST/MT.
5. **Select layouts by measured benefit.** Try at most two layouts per head
   per search, within the existing region-size, code-growth, compiler-time
   and phase budgets. Include adapters and recovery metadata in growth
   accounting. Keep cold entries reliable and retire stale variants through
   the existing publication/invalidation protocol.

CoreMark target: at least **10,000 reported iterations/s** with valid CRCs and
the benchmark's minimum ten-second duration. The target is met on the measured
host: the October 27 three-run median is 13,156 reported iterations/s for
160,000 iterations after a warmup ([method and limits](JIT.md#coremark-checkpoint-2026-10-27)).
Keep reported benchmark throughput separate from process wall time. This is
not a game-FPS prediction.

The proposed **26% of native** milestone assumes a native reference near
38,460 iterations/s, which has not been confirmed or remeasured (13,156 would be
about 34% of it). Confirm that reference with matched source, seeds,
workload, thread count and documented compiler/host settings. Native and guest
runs may require different iteration counts to satisfy the ten-second rule;
compare normalized throughput, not their total wall times. This percentage
describes CoreMark only. See [the benchmark acceptance plan](archive/TESTS-history.md#planned-codegen-and-coremark-acceptance).

Acceptance: each stage passes focused architectural regressions, independent
reference checks and real applications. Demonstrate reduced spills/state
transfers without excessive compilation or code growth, then measure matched
gameplay frame times and demanding-scene minimums. Mixed FP/vector speedups
alone do not establish CoreMark or game-FPS gains. Preserve memory bounds,
permissions, precise faults and immutable published MT instructions on all
supported hosts; retain the current block/region budgets until measurements
justify a change.

## JIT and graphics caching

Implemented first checkpoint (October 4): a larger two-way per-thread
translation lookup cache, shared-lock multithreaded call-target hits,
optional dispatch-cache telemetry, and retention of dispatch entries during
data-only mapping changes. Worker CPU profiling now registers each thread's
code-buffer range and covers logical cache growth. See [JIT contracts](JIT.md)
and [validation](TESTS.md). This checkpoint does not implement persistent
translations or shader caching.

Implemented MT publication checkpoint: atomic target cells enable chaining
and Tier-2 promotion without rewriting published instructions. MT regions fuse
loop iterations and use revocable back-edges with bounded dispatch leases.
Concurrent invalidation and both frame layouts have independent oracle checks.
Individual blocks' code and metadata are still not recycled. Since October 25 a
full cache (512 MiB default) is replaced as a whole and a reaper frees the old
one once no thread can run it; the price is recompiling code that does not fit.
Per-block reclamation and loop-carried MT region optimization remain future
work. Gameplay FPS gains need measurement.

Implemented helper checkpoint: batch nested-call statistics at outer returns
and remove redundant BL/BLR host-flag saves and BL return-PC writes. The
short-call benchmark improves 1.93×; this is not a Doom FPS guarantee. Guest
NZCV, stack alignment, vector publication and callback stop guards retain
correctness coverage.


- **Next (deferred, October 5): ART startup profile.** After the
  back-reference scan fix, ART's JIT bootstrap spends ~50% of samples in
  dispatch (blocks average ~4 instructions) and ~28% in interpreter
  fallbacks. List the instruction encodings ART runs through the
  interpreter (`BIFROST_CLASS_PROF=1`, `BIFROST_FALLBACK_PROFILE`) and give
  the hottest ones native IR, then measure block length and chaining before
  touching dispatch. Measure with `BIFROST_PROF=1 BIFROST_STATS_PERIOD=10`.
- Implemented (October 13): Tier-1 compile time and maximum, recompiles,
  contended lock wait and invalidation reasons (calls/retired/kept) in
  `BIFROST_STATS_PERIOD`; line-exact cache maintenance; unchanged code kept
  across cache maintenance and `mprotect`; MT compiles under the shared table
  lock. Remaining: per-thread cache counters, eviction and fallback decisions.
- Measure gameplay compile bursts with those counters before choosing between
  a background compiler (interpret first, publish asynchronously) and extending
  the experimental persistent cache below.
  Retranslation after the one-time ST-to-MT purge is a separate startup cost.
  Distinguish JIT warm-up from shader compilation, asset loading and ordinary
  CPU caches.
- Improve hot-block retention without unbounded memory growth or unsafe
  eviction of executing code. Whole-cache replacement bounds memory now, but it
  discards hot blocks along with cold ones.
- Move glibc programs to the real guest `ld-linux-aarch64.so.1` instead of the
  native dynamic linker, as `--android-runtime` already does for Bionic's
  `linker64`. The experimental `--guest-ld` mode passes the dynamic test
  suites and loads thunked libraries through in-memory stubs. Doom 3 ran 120 s
  under it (2026-10-24, with the thread TLS and `__libc_single_threaded` fixes for
  emulator-created threads), dhewm3 ran 120 s with a `games` rootfs profile,
  and neverball survived 40 s in two of three runs (the third was closed
  cleanly); supertux2 does not start there (its `libcurl-gnutls` wants glibc
  2.43) and crashes under the built-in linker too. Direction: make `--guest-ld` the default, then
  deprecate the built-in linker. Other games, musl threads, `make verify` and cold-start measurements remain before
  it can become the default. The native linker imitates ld.so internals (`link_map`,
  `_rtld_global_ro` hooks, main-executable initializer ownership) and breaks
  when glibc changes; a legacy-start program ran every constructor twice until
  October 20. Needs a replacement for the thunk `dlopen`/`dlsym` interception,
  and cold-start measurements (Doom 3, large dynamic apps) against the native
  linker; the persistent translation cache below is its expected mitigation.
- Persistent translated-code caching exists as an experiment (`--jit-disk-cache`,
  off by default, October 26): its fingerprint covers executable identity, guest
  words, emulator version, host ISA features, options and relocations, and a
  block is stored only when a second compile at shifted PCs explains every
  difference. In Umamusume, verify mode found every reused record equal to a
  fresh compile and launches installed 200,000 to 390,000 blocks from the cache.
  No startup gain is established (`qt_test` 105 ms from the cache against 185 ms
  without, but the recording launch takes 287 ms) and play past the title from
  the cache is unchecked. Before it can default on: a measured gain on a game,
  play from the cache, other hosts. Never reuse stale code blindly.
- Evaluate shader/pipeline cache reuse and measured prewarming separately
  from the CPU translation cache.

Acceptance: repeat visits avoid unnecessary recompilation, invalidations
remain correct under concurrent code updates, and cache memory stays bounded.
Use cold/warm A/B measurements to verify that cache changes reduce stutters.

## Input and game compatibility

- Resolve vkQuake's remaining texture issues and verify the user-reported
  mouse fix; its causal link to the Doom fixes is not established. Preserve
  Neverball and Minecraft Weekend compatibility.
- Verify relative mouse deltas, horizontal/vertical axes, pointer capture,
  sensitivity, focus transitions and fullscreen/windowed changes.
- Add reliable gamepad discovery, hotplug, mappings, dead zones, triggers,
  rumble and per-game profiles, with consistent event ordering and latency.
- Bridge Android touch/multitouch, key codes, controller events and input
  queues; keep optional desktop-to-touch mappings configurable. The mouse wheel
  and trackpad pinch zoom, Shift+wheel pans and a middle-button drag moves two
  synthetic fingers (October 25; unit and fake-SDL3 checks, not yet tried on a
  live window).
- Pointer icons (October 28): an app's `PointerIcon` (hand, text, resize, hidden) changes the host cursor on
  mouse hover ([pointer icons](Android/ANDROID_RUNTIME.md#pointer-icons-2026-10-28)). An app's own bitmap
  cursor shows the arrow; no game that sets an icon has been tried.
- Test keyboard layout, held/released buttons, text input and pause/menu
  transitions without stuck keys or duplicated events.

Acceptance: repeatable input probes and live game checks pass across the
supported desktop modes and Android lifecycle transitions.

## WebView text input and external webpages

Current boundary: WebView construction and local bitmap rendering are tested.
An editor with a live `InputConnection` receives desktop text and composition
([host text input](Android/ANDROID_RUNTIME.md#host-text-input-2026-10-20)), and
plain http(s) and Play Store links open in the host browser within 5 s of an
input ([web links](Android/ANDROID_RUNTIME.md#web-links-and-play-store-links-2026-10-20)).
The input-method service still models no installed IME, there is no on-screen
keyboard, and the composition is anchored at the bottom of the window. The
mouse wheel zooms and pans touch apps through synthetic two-finger gestures
(2026-10-25); its effect on a WebView page is unchecked. Full ViewRoot input
stages/focus arbitration are incomplete, and every other implicit intent is
declined.

1. Reproduce clicks, focus, dragging and scrolling on an attached real WebView
   page, then the failing app. Missing IME alone does not explain broken clicks.
2. Desktop text input reaches the focused InputConnection (done for native
   editors). Remaining: Chromium editors in a WebView, selection, paste and
   editor actions, caret-anchored candidate windows (`CursorAnchorInfo`),
   clipboard exchange, and connection cleanup on focus changes and app/Activity
   exit. Preserve window-plugin ABI compatibility. An on-screen keyboard is a
   later option.
3. Eligible HTTP/HTTPS `ACTION_VIEW` requests already go through the validated
   `open_url` interface to the desktop default browser. Remaining: resolve
   manifest intents consistently across PackageManager queries and Activity
   starts, keeping explicit component/package restrictions and embedded WebViews.
4. Deliver supported browser callbacks/deep links through the correct app and
   onNewIntent/lifecycle path. Browser launch, Custom Tabs, verified App Links
   and Google Play Services authentication have separate contracts; launching
   a URL does not establish successful sign-in.

Acceptance: real editor/WebView tests cover composition, scrolling, focus swaps,
resize, browser failure, callback delivery and process exit. Finish by checking
the requested Azur Lane interaction in the actual window and profiling its
frame delays; separate input, service, renderer and JIT findings.

## Hosts and CI

Status on November 1, 2026:

- CI's full-suite job passes on GitHub (Ubuntu 24.04, no display, GPU or sound; run 41347586904). So do the build, clang and
  script-mode jobs. Those runs did not cover what they skip: GLES/EGL, Wayland, Android surface and SDL Vulkan surface tests.
- The guest job's JIT tests pass on GitHub, but on commit `30b48623` its interpreter run failed `test_fork_lock_contention`.
  The fork-hang and thread-exit fixes that followed pass locally and in an Ubuntu 24.04 rootfs; they have not run on GitHub.

Open:

- Run the strict suite and `make verify` on an AVX-512 host. The first will be a Zen 5 CPU with an RDNA 4 GPU on CachyOS. Neither
  has been run. CachyOS may install packages built for that CPU, so host libraries could use AVX-512 themselves.
- Check Android games on RDNA 4: GPU work so far has been on one RDNA 3 card.
- Find out how often the fork hang reached real apps (an SDK calling `Runtime.exec` from a busy game). It was only forced by a
  stress test.
- Re-test the Ubuntu VM's Hungry Shark World black screen: the libGL and thread ctype fixes may bear on it, unconfirmed.
- Fedora, Debian, Alpine and openSUSE stay untested.

## Delivery gates

1. Establish trustworthy profiling, cache telemetry and resumable game tests.
2. Improve Linux game correctness, caching and demanding-scene performance.
3. Extend the existing Bionic/Android profile and packaged native-app coverage.
4. Broaden the existing ART/JNI integration and framework/service coverage for
   additional games.
5. Expand the Android game matrix and complete lifecycle, audio/input and
   extended stability checks before declaring v2.0 compatibility.

Run the JIT/interpreter regression suites and relevant independent AArch64
oracle checks at each milestone. Keep planned features separate from the
current support matrix. Google Play services, protected-content systems and
arbitrary APK compatibility require their own scoped work and validation.
