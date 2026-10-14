# bifrost-emu v2.0 roadmap

Status: in progress; the current pre-release is 2.0.0-alpha.1. Updated
October 14, 2026. Except for checkpoints explicitly marked implemented below,
these are development targets, not compatibility guarantees or a release date.

v2.0 aims to run selected ARM64 Android games through an integrated Android
runtime, improve sustained Linux game performance, and make input reliable
across mouse, keyboard, touch and gamepads. Correctness remains the gate for
every optimization. Existing Linux applications must keep working.

## Next release priorities

These are proposed scopes for the next pre-releases, not announced release dates.
The runtime remains 2.0.0-alpha.1. The October 14
[SpongeBob screenshots](Apps/spongebob.md#screenshots-2026-10-14) show cooking,
the lobby, Help Center and a level-14 pause menu; the
[Azur Lane screenshots](Apps/azur-lane.md#screenshots-2026-10-14) show loading
and the title/login screen. Still images do not establish sustained performance
or successful ads, sign-in and gameplay in every app.

### 2.0.0-alpha.2: broader game support and less stutter

- Repair concrete startup and interaction blockers in selected games, starting
  with Azur Lane's toast failure and delayed login UI. Complete connected
  common-service contracts rather than returning success from empty handlers.
- Profile matching cold-load and warm gameplay scenes. Reduce compilation stalls,
  repeated translation, paired-memory guard overhead and vector spills where
  measurements justify it. Preserve fault, alias, invalidation and FP semantics.
- Measure rendering, ART/GC, Binder/service work and input delays alongside JIT
  cost. Keep supported software and hardware media routes explicit.
- Exercise multiple selected Android engines/apps, with audio, saved data,
  resize, pause/resume and process exit; retain working Linux game behavior.

Acceptance: repeatable GPU-enabled runs report load time, CPU/compilation cost
and median/p95/p99 frame times against the current baseline. Lower tail frame
times must accompany correctness checks and sustained interaction; a window
title's instantaneous FPS is not the acceptance measurement.

### 2.0.0-alpha.3: WebView, media and possible ad support

- Finish attached WebView pointer/focus/scroll delivery and desktop text/IME
  composition. Help Center rendering is visible now; complete browser/editor
  interaction still needs its own tests.
- Complete external HTTP/HTTPS intent routing and supported return/deep-link
  delivery, with app lifecycle and failed-browser handling.
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
- Build on the current cards, icons, search, per-game Back-key/stats settings
  and last-session drawer. Add clear update/reinstall and save-preservation
  flows, with routine controls outside Developer.
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
levels and menus; Azur Lane's loading and title/login UI are visibly captured.
Those checkpoints do not establish arbitrary APK compatibility or sustained
Azur Lane gameplay.

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
- Implement toast enqueue/cancel/render/timeout behavior. Azur Lane's observed
  Google sign-in failure path currently exits on `notification.enqueueToast`.
- Connect notification posting, interaction and PendingIntent delivery, then
  foreground-service notification/deadline behavior and lifecycle policy.
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

- Extend EGL/OpenGL ES/Vulkan, ANativeWindow and buffer ownership so
  Android game surfaces present correctly through the host graphics path.
  Use the [AOSP graphics architecture](https://source.android.com/docs/core/graphics/architecture)
  to define the required surface/buffer contracts; choose a supported service
  bridge or compositor integration before promising full SurfaceFlinger.
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

Acceptance: rendered gameplay with working audio and input survives repeated
surface recreation and lifecycle transitions without leaks or stale handles.

## Doom 3 performance and frame pacing

Target sustained improvement at 3440×1440 on a documented reference host.
The recorded baseline supports long gameplay and occasional 60 FPS scenes,
but demanding gameplay still falls to a few FPS. Aim first for sustained
5 FPS in the previously difficult underground route, then raise the floor
and pursue 60 FPS or more in representative scenes where host resources
permit. Cutscene FPS alone is not the acceptance criterion.

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
reserve-accounting underflow. See [implementation](JIT.md#tier-1-hot-path-code-reduction-2026-10-14)
and [validation](TESTS.md#tier-1-hot-path-code-reduction-2026-10-14).

Next, prioritize sampled codegen paths rather than emitted size alone:

- Streamline paired-memory guards while preserving whole-instruction footprint
  checks, precise faults and register/writeback ordering.
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
the benchmark's minimum ten-second duration. The latest final three-run median
is 9,893.87 reported iterations/s for 110,000 iterations after a warmup, about
1.1% below that throughput target. Keep reported benchmark throughput separate
from process wall time. The remaining target is not a game-FPS prediction.

The proposed **26% of native** milestone assumes a native reference near
38,460 iterations/s. Confirm that reference with matched source, seeds,
workload, thread count and documented compiler/host settings. Native and guest
runs may require different iteration counts to satisfy the ten-second rule;
compare normalized throughput, not their total wall times. This percentage
describes CoreMark only. See [the benchmark acceptance plan](TESTS.md#planned-codegen-and-coremark-acceptance).

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
Code and metadata remain unrecycled; quiescent reclamation and loop-carried
MT region optimization remain future work. Gameplay FPS gains need measurement.

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
  a background compiler (interpret first, publish asynchronously) and a
  persistent warm-up list keyed by file identity, offset and code contents.
  Retranslation after the one-time ST-to-MT purge is a separate startup cost.
  Distinguish JIT warm-up from shader compilation, asset loading and ordinary
  CPU caches.
- Improve cache capacity management and hot-block retention without
  unbounded memory growth or unsafe eviction of executing code.
- Evaluate persistent translated-code caching with executable identity,
  guest code contents, emulator version, host ISA features, options and
  relocations in its compatibility rules. Never reuse stale code blindly.
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
  queues; keep optional desktop-to-touch mappings configurable.
- Test keyboard layout, held/released buttons, text input and pause/menu
  transitions without stuck keys or duplicated events.

Acceptance: repeatable input probes and live game checks pass across the
supported desktop modes and Android lifecycle transitions.

## WebView text input and external webpages

Current boundary: WebView construction and local bitmap rendering are tested,
but the input-method service models no installed IME. Physical key events are
forwarded directly; committed text/composition and wheel events are missing,
and full ViewRoot input stages/focus arbitration are incomplete. Activity starts
also reject implicit intents, including ordinary external browser links.

1. Reproduce clicks, focus, dragging and scrolling on an attached real WebView
   page, then the failing app. Missing IME alone does not explain broken clicks.
2. Connect desktop text input to the focused Android InputConnection: Unicode,
   composition, selection, deletion, paste and editor actions, with connection
   cleanup on focus changes and app/Activity exit. Preserve window-plugin ABI
   compatibility. An on-screen keyboard is a later option.
3. Resolve manifest intents consistently across PackageManager queries and
   Activity starts, then route eligible HTTP/HTTPS ACTION_VIEW requests through
   the desktop default browser using a validated host URI interface. Preserve
   explicit component/package restrictions and embedded WebViews.
4. Deliver supported browser callbacks/deep links through the correct app and
   onNewIntent/lifecycle path. Browser launch, Custom Tabs, verified App Links
   and Google Play Services authentication have separate contracts; launching
   a URL does not establish successful sign-in.

Acceptance: real editor/WebView tests cover composition, scrolling, focus swaps,
resize, browser failure, callback delivery and process exit. Finish by checking
the requested Azur Lane interaction in the actual window and profiling its
frame delays; separate input, service, renderer and JIT findings.

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
