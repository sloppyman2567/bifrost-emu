# bifrost-emu v2.0 roadmap

Status: in progress. Updated October 4, 2026. Except for checkpoints explicitly
marked implemented below, these are development targets, not compatibility
guarantees or a release date.

v2.0 aims to run selected ARM64 Android games through an integrated Android
runtime, improve sustained Linux game performance, and make input reliable
across mouse, keyboard, touch and gamepads. Correctness remains the gate for
every optimization. Existing Linux applications must keep working.

## Android games: Bionic and platform compatibility

Build a dedicated Bionic/Android syscall and platform compatibility layer
on the existing AArch64 Linux syscall implementation. Bionic uses Linux
syscalls; this work must also cover the Android userspace environment,
runtime libraries, loader behavior and required platform services.

- Select and document an initial Android API/runtime version. Expand from
  a tested baseline instead of claiming every Android release works.
- Support Bionic libc, libm, libdl, pthread/TLS behavior, signals, futexes,
  memory mapping and relevant Android ioctl/device contracts.
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

Integrate ART and the required managed libraries so games with Java/Kotlin
startup code can reach their ARM64 native engines. ART executes DEX and
supports compiled execution; see the [AOSP runtime reference](https://source.android.com/docs/core/runtime).

- Start by evaluating a matching guest ARM64 ART/Bionic runtime executed
  through Bifrost. Reuse the existing native-bridge interface where suitable;
  a host ART route requires a separately validated JNI/ABI bridge.
- Support class loading, managed exceptions, JNI registration/calls,
  callbacks, thread attachment and managed/native reference lifetimes.
- Coordinate ART garbage collection, thread suspension and safepoints
  with guest signals and JIT execution. Never bypass these for speed.
- Handle ART-generated executable pages, write/execute protection changes
  and instruction-cache invalidation without stale Bifrost translations.
- Validate DEX interpretation first, then ART JIT/AOT execution. Keep ART's
  generated-code cache distinct from Bifrost's translated-code cache.
- Implement the framework subset required for app launch, activities,
  lifecycle, resources and the selected game compatibility targets.

Acceptance: a DEX test calls a native ARM64 library and receives a callback;
a mixed managed/native Android sample survives GC, pause/resume and thread
shutdown; a selected game reaches interactive gameplay.

## Android graphics, audio and lifecycle

- Extend EGL/OpenGL ES/Vulkan, ANativeWindow and buffer ownership so
  Android game surfaces present correctly through the host graphics path.
  Use the [AOSP graphics architecture](https://source.android.com/docs/core/graphics/architecture)
  to define the required surface/buffer contracts; choose a supported service
  bridge or compositor integration before promising full SurfaceFlinger.
- Cover the selected games' audio APIs, callback threading, buffer timing
  and device changes, preserving sound during normal gameplay.
- Test orientation, resize, focus, pause/resume, surface loss/recreation
  and background/foreground transitions.
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

## JIT and graphics caching

Implemented first checkpoint (October 4): a larger two-way per-thread
translation lookup cache, shared-lock multithreaded call-target hits,
optional dispatch-cache telemetry, and retention of dispatch entries during
data-only mapping changes. Worker CPU profiling now registers each thread's
code-buffer range and covers logical cache growth. See [JIT contracts](docs/JIT.md)
and [validation](docs/TESTS.md). This checkpoint does not implement persistent
translations, shader caching or multithreaded Tier-2/chaining.

Implemented helper checkpoint: batch nested-call statistics at outer returns
and remove redundant BL/BLR host-flag saves and BL return-PC writes. The
short-call benchmark improves 1.93×; this is not a Doom FPS guarantee. Guest
NZCV, stack alignment, vector publication and callback stop guards retain
correctness coverage.


- Instrument per-thread cache hits/misses, compile time, retranslation,
  invalidation reasons, code bytes, growth, eviction and fallback decisions.
- Confirm revisited unchanged code reuses its translation. Distinguish JIT
  warm-up from shader compilation, asset loading and ordinary CPU caches.
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

## Delivery gates

1. Establish trustworthy profiling, cache telemetry and resumable game tests.
2. Improve Linux game correctness, caching and demanding-scene performance.
3. Bring up the Bionic/Android profile and packaged native applications.
4. Integrate ART/JNI and the framework/service subset for selected games.
5. Expand the Android game matrix and complete lifecycle, audio/input and
   extended stability checks before declaring v2.0 compatibility.

Run the JIT/interpreter regression suites and relevant independent AArch64
oracle checks at each milestone. Keep planned features separate from the
current support matrix. Google Play services, protected-content systems and
arbitrary APK compatibility require their own scoped work and validation.
