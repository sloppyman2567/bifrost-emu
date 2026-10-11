# bifrost-emu

**A bridge between worlds** — a fast ARM64 (AArch64) Linux user-mode emulator
for x86_64 Linux hosts. Run ARM64 Linux applications and selected games
without QEMU. Running existing binaries needs no cross-compiler; building
guest tests does.

```
  ____ _____ ______ _____   ____   _____ _______ 
 |  _ \_   _|  ____|  __ \ / __ \ / ____|__   __|
 | |_) || | | |__  | |__) | |  | | (___    | |   
 |  _ < | | |  __| |  _  /| |  | |\___ \   | |   
 | |_) || |_| |    | | \ \| |__| |____) |  | |   
 |____/_____|_|    |_|  \_\\____/|_____/   |_|   

  bifrost-emu  v1.5.5
  x86_64 ◄─────────────────► ARM64
```

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](http://unlicense.org/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform: Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)]()
[![Version: 1.5.5](https://img.shields.io/badge/version-1.5.5-orange.svg)](Docs/CHANGELOG.md)

## What is bifrost-emu?

bifrost-emu is a user-mode emulator that runs AArch64 (ARM64) Linux
binaries on x86_64 Linux hosts. It translates ARM64 instructions to x86_64
at runtime using a JIT compiler, with a switch-based interpreter fallback.

**What it can do:**

- Run static and dynamically-linked AArch64 ELF binaries
- Load shared libraries (musl and glibc) with GOT/PLT relocation, TLS,
  ifuncs, and DT_INIT_ARRAY constructors
- JIT-compile ARM64 to x86_64 native code, with interpreter fallback and
  focused correctness regressions
- Recover eligible native memory faults through host page protection;
  paired accesses and userfaultfd retain software checks. See the
  [JIT memory contracts](Docs/JIT.md#registered-native-memory-faults).
- Emulate 200+ Linux syscalls (threads, signals, filesystem, memory)
- Provide a virtual filesystem (/proc, /dev, /sys, framebuffer, audio)
- Forward GL/EGL/Vulkan/SDL2/ALSA calls to host libraries (thunking)
- Run multi-threaded guest programs (clone + futex + per-thread JIT)
- Run SDL2/OpenGL demos and a Minecraft-like voxel game; Teeworlds reaches
  a rendering menu with sound initialized in the latest launch
- Run Neverball 1.6.0 with user-confirmed stable gameplay, replay, readable
  menus and fonts, and repaired JPEG previews; see the
  [Neverball setup and validation notes](Docs/Apps/neverball.md)
- Run vkQuake 1.33.1 through the Vulkan thunks with user-confirmed gameplay,
  menus, and no sudden crashes during the observed session. Texture corruption
  remains; the user later reported the mouse issue resolved, with its cause
  and repeatable input validation still pending. See [vkQuake status](Docs/Apps/vkquake.md).
- Run Doom 3 through ARM64 dhewm3, with user-reported extended gameplay,
  repaired character animation and working audio. Demanding scenes remain
  slow, and save compatibility depends on the game build; see
  [Doom 3 status](Docs/Apps/doom3.md).

**Full-suite checkpoint (October 5, 2026, including ART foundations):**
After the audio wake-up fix, strict full-suite reruns pass 275/275 in JIT
mode and 268/268 in interpreter mode, with seven expected JIT-only skips.
AAudio passes in both full runs; 20 callback stress runs and four focused
audio/thread tests per engine also pass. See [test evidence](Docs/TESTS.md).
The verification gate now passes 60 single-thread differential fixtures and
five separate threaded oracle workloads. Shared-memory replay is suspended
when a second guest CPU starts; see [test evidence](Docs/TESTS.md).
Dispatch and nested-call microbenchmarks improved 2.32× and 1.93× respectively;
those figures are not whole-game FPS gains. Signed native APK launch now works;
ART/framework integration remains planned for [v2.0](Docs/roadmap.md).

**What it is NOT:**

- Not a full-system emulator (no kernel — use QEMU-system for that)
- Android support is experimental; Asgard supplies real Bionic/ART and selected
  framework contracts, with incomplete Android service and renderer support
- Not as mature as QEMU-user — it's a smaller, simpler alternative

## Quick Start

```bash
# Build (see Building for required and optional dependencies)
make

# Run a static ARM64 binary — silent by default, just shows program output
./bifrost-emu hello.elf

# Verbose — show execution stats on exit
./bifrost-emu -v Tests/Apps/fib.elf

# Pass arguments to the emulated program
./bifrost-emu Tests/Apps/toybox seq 1 10

# Run a dynamically-linked binary (requires rootfs — see below)
export BIFROST_ROOT=$PWD/rootfs
./bifrost-emu my_dynamic_app.elf

# Debug mode — trace every instruction to stderr
./bifrost-emu -d hello.elf

# Use --no-jit to force the interpreter (debugging)
./bifrost-emu --no-jit Tests/Apps/fib.elf
```

The tier-1 JIT and block chaining are enabled by default. Experimental
tier-2 region promotion is opt-in with `BIFROST_TIER2=1`; leave it unset or
set `BIFROST_TIER2=0` for tier-1 execution.

Graphical programs log their first successful frame presentation to stderr.
Use `--no-render-log` to suppress that message. Bifrost's version and
presentation FPS appear in the game title by default. Use `--no-window-stats`
to disable stats, or `--window-stats` (also `--window_stats`) to enable the title.
Independent title, separate-window and overlay switches are described in
[window stats](Docs/window-stats.md).

## Running ARM64 Applications

### Static Binaries

Static AArch64 ELF binaries work out of the box — just run them:

```bash
./bifrost-emu my_static_arm64_binary
```

This covers most cross-compiled Go/Rust/C/C++ binaries, BusyBox (static),
ToyBox (static), and many game engines that ship as static binaries.

### Dynamically-Linked Binaries

For binaries that need shared libraries (libc.so.6, libm.so.6, etc.),
set up a rootfs with a single command:

```bash
# One-command rootfs setup (fetches toolchains + libs, ~250 MB total):
./Scripts/Setup/setup-rootfs-all.sh

# Or step-by-step:
./Tools/Fetch/fetch-glibc-toolchain.sh     # glibc toolchain (130 MB)
./Tools/Fetch/fetch-musl-toolchain.sh      # musl toolchain (104 MB)
./Scripts/Setup/setup-rootfs.sh            # create rootfs with glibc + musl libs
./Scripts/Setup/fetch-realworld-libs.sh    # libselinux, libpcre2, libssl, etc.
./Scripts/Setup/fetch-curl-deps.sh         # curl's 24 dependency libs

# Run with --rootfs (or BIFROST_ROOT env var):
./bifrost-emu --rootfs $PWD/rootfs my_dynamic_app.elf
# or:
export BIFROST_ROOT=$PWD/rootfs
./bifrost-emu my_dynamic_app.elf
```

Both **glibc** and **musl** dynamically-linked binaries are supported.
glibc dynamic printf/puts/fprintf/fputs work fully (integers, strings,
hex, padded formats, float formatting). musl's printf works perfectly
for all format specifiers. **glibc 2.40+ (Arm GNU 14.2.Rel1) is
supported** — the dynamic linker processes DT_RELR (compact relative
relocations) which glibc 2.40 ships by default. **glibc dynamic
pthreads work end-to-end**
(`pthread_create`/`pthread_join`, mutexes, condition variables,
`__thread` TLS) — the emulator initializes NPTL's stack-cache list
heads, routes `_dl_allocate_tls` through a native syscall, reports
`rseq` success, and propagates `clone3`'s `child_tid` so thread exit
wakes joiners. **8+ threads across multiple waves (stack-cache reuse)
work correctly** with full TLS isolation, including multi-waiter
condvar (producer/consumer) patterns. The dynamic linker detects TLS
field offsets at runtime by disassembling `__libc_early_init`, making
it robust across glibc versions (2.36–2.40+) without hardcoded
offsets. musl dynamic pthreads and all static pthread tests also
pass under both JIT and interpreter.

The rootfs includes:
- `/lib/libc.so.6`, `/lib/ld-linux-aarch64.so.1` (glibc)
- `/lib/ld-musl-aarch64.so.1`, `/lib/libc.so` (musl)
- `/etc/passwd`, `/etc/group`, `/etc/hosts`, `/etc/nsswitch.conf`, timezone
- `/system/build.prop`, `/system/etc/permissions/` (Android-compatible)
- `/data/app`, `/data/data`, `/sdcard` (Android-style storage)

### Android Applications

**Asgard** is Bifrost's Android compatibility layer. Its app supervisor and
shared Binder broker let independent guest processes exchange native Binder
transactions, object handles and host-backed file descriptors. Use
`python3 Scripts/Android/asgard.py start PACKAGE` to launch into a shared realm, and
`python3 Scripts/Android/asgard.py realm status` to inspect it. See
[Asgard setup, process tests and current limits](Docs/Android/ANDROID_RUNTIME.md).

The standalone Android path targets **NativeActivity-style native libraries**
through `--android`. Surface, activity/looper and audio probes pass in both
engines; this does not establish compatibility with arbitrary Android game
`.so` files, Bionic packages or APKs. The emulator plays the framework role:
it synthesizes the `ANativeActivity` struct, fires
`onCreate/onStart/onResume/onNativeWindowCreated/...` on guest callbacks,
and provides `ALooper`, `AInputQueue` (SDL mouse/touch/keyboard → motion/
key events), `AConfiguration` and `__android_log_*` thunks so
`android_native_app_glue` code runs unmodified.

```bash
# Run a NativeActivity .so (renders through the host GL/EGL thunk)
./bifrost-emu --android libgame.so

# Test hooks:
#   BIFROST_ANDROID_TAP=1            inject one synthetic screen tap
#   BIFROST_ANDROID_TIMEOUT_SECS=N   hard wall-clock cap (default: none)
#   BIFROST_ANDROID_LEGACY_CB=1      pre-API-26 callback-table layout
```

The rootfs also has `/system/lib64` → `/lib64`, `/vendor/lib64` → `/lib64`
as Android-style search-path scaffolding, and build.prop advertises
arm64-v8a ABI, SDK 29, ro.kernel.qemu=1. These properties do not provide a
complete Android 10 runtime or prove Android library compatibility.

An experimental `--android-runtime --rootfs PATH /guest/program` profile now
boots a real guest linker64 and passes a pinned Bionic native fixture covering
constructors, pthread/TLS and dlopen in both engines. The signed APK installer
uses this profile for native-only Activities, with split assets and persistent
per-app data:

```bash
python3 Tools/Android/fetch-android-runtime.py
python3 Tools/Android/fetch-aapt2.py
python3 Tools/Android/fetch-android-package-tools.py
make android-runtime-adapters
python3 Scripts/Android/android_package.py install /path/to/package.apkm
python3 Scripts/Android/android_package.py run org.example.package
python3 Scripts/Android/android_package.py probe org.example.package --library libunity.so
```

See [APK setup, validation and limits](Docs/Android/ANDROID_APK.md) and the
[bootstrap instructions](Docs/Android/ANDROID_RUNTIME.md). Virtual Binder devices
support bounded transactions and object handles. `--android-services` adds a
native service-manager registry; see [Binder limits and tests](Docs/Android/ANDROID_BINDER.md).
The Android surface adapter uses an SDL3 window backend behind a versioned
host C plugin interface. Plugins own their windows and return native EGL handles
and normalized input events; no SDL object crosses the interface. Guest SDL2
thunks remain compatible with SDL2. Build the optional module with
`make android-window-plugin`, then pass
`--android-window-plugin ./build/release-sdl1-gl1/plugins/android-window-sdl.so`
before `--android` or the guest ELF. V1 plugins must be rebuilt for ABI V2.
Failed resizes preserve reported geometry and unsupported formats return errors.
The V2 checkpoint passes 21 focused graphics tests in each engine, host/CLI
contracts, 256 SDL3 teardown cycles and a complete signed APK gate; these are
not a new full-suite or gameplay-performance claim. See the
[window plugin guide](Docs/Android/ANDROID_WINDOW_PLUGINS.md) for coverage and limits.

The same-image ART/libcore fixture creates a real VM and passes DEX/JNI checks
in both engines. A framework probe creates a real Activity, and the separate
installed-app probe now reaches SpongeBob's CleverTap Activity `onCreate` and
IL2CPP JNI startup in JIT and interpreter modes. See the
[installed-app checkpoint](Docs/Android/ANDROID_RUNTIME.md#installed-app-creation-checkpoint-2026-10-07).
That probe exits after creation. The normal managed runner additionally supplies
Activity resume, a SurfaceView window and input; those paths remain experimental.

The separate native-load probe loads all 21 SpongeBob libraries with the real
Media NDK bundle and an explicit Firebase App preload. It reports loading and
shutdown separately and does not invoke JNI entrypoints. Guest adapters use
official AOSP headers, real guest zlib, and EGL/looper/choreographer/sensor
exports covering Unity's imports. See the [APK guide](Docs/Android/ANDROID_APK.md) for
dependency pins and verification limits. The optional ARM64 Media NDK bundle
(`Tools/Android/android_media.py`, installer `--media-root`) supports metadata operations;
Android media services and decoding remain unimplemented.
The native-bridge adapter below is a separate integration interface, not a
complete Android runtime.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      bifrost-emu                             │
│                                                              │
│  ┌─────────────┐   ┌──────────────┐   ┌─────────────────┐  │
│  │  ELF Loader  │──▶│   Decoder    │──▶│  IR Translator  │  │
│  │ (static/dyn) │   │ (ARM64→IR)   │   │  (ARM64→x86)    │  │
│  └─────────────┘   └──────────────┘   └────────┬────────┘  │
│                                                 │            │
│  ┌─────────────┐   ┌──────────────┐   ┌────────▼────────┐  │
│  │  Dynamic     │   │   Yggdrasil   │   │   FrostJIT      │  │
│  │  Linker      │   │   VFS         │   │  (x86 codegen)  │  │
│  │ (GOT/PLT/TLS)│   │ (/proc,/dev)  │   └────────┬────────┘  │
│  └─────────────┘   └──────────────┘            │            │
│                                              ┌──▼──┐         │
│  ┌─────────────┐   ┌──────────────┐         │ CPU │         │
│  │  Syscall     │   │  Signal/     │         │State│         │
│  │  Layer       │   │  Thread Mgr  │         └─────┘         │
│  │ (200+ calls) │   │              │                         │
│  └─────────────┘   └──────────────┘                         │
│                                                              │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  Graphics/Audio/Display Thunks                       │    │
│  │  (GL/EGL/Vulkan/SDL2/ALSA/PulseAudio → host)        │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

### Key Components

| Component | Files | Description |
|-----------|-------|-------------|
| **ELF Loader** | `Source/System/Loader/ElfLoader.cpp` | Loads static & PIE ELF binaries, processes RELA relocations |
| **Dynamic Linker** | `Source/System/Loader/DynamicLinker/` | DT_NEEDED, GOT/PLT, TLS, ifuncs, versioned symbols, ld-linux shim |
| **Decoder** | `Source/Interpreter/Decoder/Decoder.cpp` | Hierarchical ARM64 instruction decoder |
| **IR Translator** | `Source/FrostJIT/IR/Translate/` | ARM64 → typed IR, scalar SSA and optimization passes |
| **FrostJIT** | `Source/FrostJIT/Compiler/FrostJIT.cpp` | IR → x86_64 native codegen (AVX2/AVX-512) |
| **Interpreter** | `Source/Interpreter/Core/Interpreter.cpp` | Switch-based interpreter (JIT verify mode) |
| **Syscall Layer** | `Source/System/Syscalls/*.cpp` | 200+ Linux AArch64 syscalls |
| **Yggdrasil VFS** | `Source/System/VFS/*.cpp` | Virtual /proc, /dev, /sys, framebuffer, audio, input |
| **Signal/Thread** | `Source/System/Core/Signals.cpp`, `Threads.cpp` | rt_sigaction, clone, futex, per-thread JIT |
| **Thunks** | `Source/System/Graphics/Thunks/`, `Source/System/Audio/AudioThunk/` | GL/EGL/Vulkan/SDL2/audio forwarding |

See [the source layout](Docs/ARCHITECTURE.md) for the compiler phases, host
adapter splits, test directories and tooling entry points.

## Building

### Prerequisites

- **g++ 9+** (supports C++17)
- **make**
- Linux x86_64 host

Optional (for full feature set):
- **SDL2 dev headers** (`./Tools/Fetch/fetch-sdl2-headers.sh`) — for framebuffer and guest SDL2 thunks
- **SDL3 dev headers >=3.2**, plus the SDL3 runtime — for Android host windows
- **AArch64 cross-toolchain** — for cross-compiling test binaries

### Build

```bash
# Default build — auto-detects SDL2 and enables host GL/EGL thunking
# when sdl2-config is available (BIFROST_USE_SDL2 / GL/EGL thunks)
make

# Force a headless build even if SDL2/SDL3 are installed
make USE_SDL2=0 USE_SDL3=0

# Android SDL3 host windows; preserve guest SDL2 thunk support
make USE_SDL2=1 USE_SDL3=1 USE_THUNK_GL=1

# Force the SDL2 build (errors if sdl2-config is not installed)
make USE_SDL2=1

# With GL/EGL thunking (forwards guest GL calls to host) — on by default
make USE_SDL2=1 USE_THUNK_GL=1

# Debug build with sanitizers
make debug
# The sanitizer binary is isolated at build/debug/bifrost-emu-dbg.

# Static library (for embedding bifrost-emu in other projects)
make lib
make test-capi  # build + run the host-side C API test (55 checks)
make test-nb    # build + run the native bridge adapter test (61 checks)
```

Each SDL/GL selection gets its own `build/release-sdl*-gl*` directory, and
the sanitizer build uses `build/debug`. The default release executable is
also copied to `./bifrost-emu`; `make clean` removes all build profiles.

### Cross-Compiling Test Binaries

```bash
# Fetch the musl cross-toolchain (104 MB)
./Tools/Fetch/fetch-musl-toolchain.sh

# Cross-compile test C sources and assemble the freestanding test programs
make setup-tests

# Or compile a single file
make cross SRC=Tests/Apps/my_test.c OUT=Tests/Apps/my_test.elf
```

## Testing

```bash
# Run the standard suite (benchmarks included; dynamic tests run when a
# configured rootfs is present; missing real-world fixtures may be fetched)
make check

# Quick mode (skips benchmarks)
make check-quick

# Run the quick suite under the interpreter
make check-nojit

# JIT divergence checker (slow, catches codegen bugs)
make verify

# Local guest-correctness gate (generated files, APIs, JIT/interpreter,
# sandbox and dynamic glibc regressions)
make ci

# Full strict suite, including the provisioned dynamic-linker coverage
make check-all

# Run only specific categories
./Scripts/Testing/run_tests.sh --unit         # JIT regression tests
./Scripts/Testing/run_tests.sh --toybox       # ToyBox integration
./Scripts/Testing/run_tests.sh --dynamic      # Dynamic linking tests
./Scripts/Testing/run_tests.sh --bench        # Performance benchmarks

# Filter by name
./Scripts/Testing/run_tests.sh --filter "sig|brk|pipe"
```

With all fixtures available, the runner selects **275 tests**, including 97
unit regressions. Without rootfs-dependent tests it selects 252 (247 with
`--quick`). Interpreter mode intentionally skips seven JIT-only cases.
Sources live in `Tests/Unit/`, `Tests/Apps/` and `test/`; build their ignored guest
ELFs with `make setup-tests`. External fixtures and desktop/audio availability
can change coverage. Strict mode fails missing selected fixtures, while
exit-77 environment skips remain visible.

`make verify` checks single-thread differential fixtures and runs the original
concurrent cache/call workloads separately against their expected results.
It fails missing/stale fixtures, nonzero exits, timeouts and reported
divergences. See [test evidence](Docs/TESTS.md) and
[fixture licensing and distribution](Docs/THIRD_PARTY.md).

### Host Input

With SDL2 enabled, `FrostInput` translates host keyboard, mouse, and game
controller events into Linux-style records. `/dev/input/event0` exposes
`input_event` records; `/dev/input/js0` exposes legacy `js_event` records.
Mouse movement uses relative axes, while absolute axes are reserved for
game controller sticks and triggers. Controller connection queues
`JS_EVENT_INIT` state, and focus loss synthesizes key releases to avoid
stuck keys. In headless builds input queues remain empty; `/dev/input/mice`
ImPS/2 reads are currently unsupported.

## Configuration

bifrost-emu supports a TOML config file and environment variables:

```bash
# Use a config file
./bifrost-emu --config /path/to/bifrost.toml my_app.elf

# Print effective configuration
./bifrost-emu --print-config

# Environment variables (see bifrost.toml.sample for full list)
BIFROST_ROOT=/path/to/rootfs     # Rootfs for dynamic linking
BIFROST_NO_NATIVE_DYNLINK=1      # Use guest-side ld.so (debugging)
BIFROST_DYNLINK_TRACE=1          # Trace dynamic linker
BIFROST_SYSCALL_TRACE=1          # Trace syscalls
BIFROST_JIT_VERIFY=1             # JIT/interpreter divergence check
BIFROST_THUNK_TRACE=1            # Trace graphic/audio/display thunk calls
BIFROST_NO_THUNK_GRAPHICS=1      # Disable GL/EGL/SDL2 thunking (on by default)
BIFROST_NO_THUNK_AUDIO=1         # Disable ALSA/PulseAudio thunking (on by default)
BIFROST_NO_THUNK_DISPLAY=1       # Disable Vulkan/Wayland thunking (on by default)
```

### Graphics/Audio/Display Thunking

bifrost-emu forwards guest GL/EGL/SDL2/ALSA/Vulkan calls to the host's
native libraries via a **thunk** layer. This is **enabled by default** —
no env var needed. Build with `make USE_SDL2=1 USE_THUNK_GL=1` so host
headers/libs are linked.

Marshalling supports AAPCS64 stack args (e.g. `glTexImage2D` data),
FP args in `v0..` (`glClearColor`, `glVertex3f`), host→guest string
returns (`glGetString`, `SDL_GetError`), nested `glShaderSource`
pointers, and bounce buffers for pointers whose range is not one accessible
host range (`SDL_PollEvent`). Trampolines end with `ret` after `svc`.
dlopen of libGL/libSDL2 uses the thunk when the on-disk `.so` is not
AArch64 (so host x86_64 libs are never executed as guest code).

Modern GL is covered: `glMapBuffer`/`glMapBufferRange` and
persistent-coherent mappings use a guest-window bounce (PCWFC
writeback before buffer-consuming calls), and GL3.3+/4.x rows cover
uniform blocks, instancing, compute, transform feedback, sampler and
query objects, DSA (`glCreateBuffers`/`glCreateVertexArrays`),
`glBufferStorage`, and `glTexImage3D`. GLFW callback setters
(`glfwSetKeyCallback`, `glfwSetCursorPosCallback`, …) store guest
AArch64 callbacks and deliver them after each `glfwPollEvents`.

Demo:

```bash
make USE_SDL2=1 USE_THUNK_GL=1
make cross SRC=Tests/Apps/test_sdl_gl_triangle.c OUT=Tests/Apps/test_sdl_gl_triangle.elf
DISPLAY=:0 ./bifrost-emu Tests/Apps/test_sdl_gl_triangle.elf
```

See `bifrost.toml.sample` for all options.

## C API (libbifrost)

bifrost-emu exposes a stable C API (`Source/System/API/Bifrost.h`) for embedding the
emulator in host programs. Build it with `make lib` (produces
`libbifrost.a`; `Source/Main.cpp` is excluded — link your own driver):

```c
#include "Bifrost.h"

bifrost_emu_t* emu = bifrost_create();
bifrost_load_elf(emu, "hello.elf", argc, argv);
int exit_code = bifrost_run(emu);   // JIT is on by default
bifrost_destroy(emu);
```

Key features:

- **JIT default-on.** `bifrost_run` enables the frostJIT automatically;
  call `bifrost_set_jit(emu, 0)` to force the interpreter, or
  `bifrost_set_jit_verify(emu, 1)` (before run) for JIT-vs-interpreter
  divergence checking.
- **Register/FP/flag/memory access** — `bifrost_get/set_reg`,
  `bifrost_get/set_fp_reg_*`, `bifrost_get/set_pstate`, flags,
  `bifrost_read/write_mem`, SP/PC getters and setters.
- **Step-loop debugging** — `bifrost_step`/`bifrost_step_n` plus real
  breakpoints: `bifrost_set_breakpoint(emu, addr)` makes the next
  `bifrost_step` stop *before* executing that instruction and return 1.
  (`bifrost_run` ignores breakpoints and runs to completion.)
- **Guest function calls** — `bifrost_call(emu, fn, iargs, n, fargs, n)`
  invokes any guest function with up to 8 integer + 8 FP arguments,
  saving/restoring all CPU state around the call and returning x0.
  `bifrost_call_f` returns the FP (d0) result instead — use it for
  functions returning `double`/`float`.
- **Symbol lookup** — `bifrost_lookup_symbol(emu, "name")` resolves a
  symbol across loaded dynamic objects and thunk-registered libraries.
- **Syscall hook** — `bifrost_set_svc_hook(emu, fn, userdata)` installs
  a callback invoked for every guest syscall before emulator dispatch;
  return 1 and set `*result` to handle the syscall yourself (or 0 to
  pass it through). Note: the internal thunk fast path (syscall
  `0x1000`) bypasses the hook.
- **Guest dlopen/dlsym/dlclose** — `bifrost_dlopen(emu, path, flags)`
  loads a guest shared object and returns its base address as an opaque
  handle (re-load bumps the refcount), `bifrost_dlsym(emu, handle, name)`
  resolves a guest symbol (0 on miss), `bifrost_dlclose(emu, handle)`
  unloads it (0 success / -1 error). Foundation for the native bridge
  adapter below.

The full API reference and semantics live in `Source/System/API/Bifrost.h`. A
host-side test (`Tests/Unit/test_capi.c`, built via `make test-capi`, wired
into `make check-all`) covers the API with 55 checks.

## Android native bridge adapter (`Source/System/API/NativeBridge.h`)

bifrost exposes an Android native-bridge integration adapter. Its host
API tests do not demonstrate an actual ART runtime or a drop-in Android
translation environment. `Source/System/API/NativeBridge.h` is a clean-room ABI mirror of Android's
`NativeBridgeCallbacks`; `Source/System/API/NativeBridge.cpp` fills the table over
the C API:

```c
#include "NativeBridge.h"
bifrost_emu_t* emu = bifrost_create();
bifrost_load_elf(emu, "system_lib.elf", argc, argv);
bifrost_nb_init(emu);                     // fills NativeBridgeItf
void* lib = NativeBridgeItf.loadLibrary("game_native.so", 0);
// ART: NativeBridgeGetTrampoline(lib, "Java_com_example_Native_foo", "JID", 3)
long (*foo)(void*, void*, long, double) =
    (long(*)(void*, void*, long, double))
    NativeBridgeItf.getTrampoline(lib, "Java_com_example_Native_foo", "JID", 3);
long r = foo(NULL, NULL, 42, 2.5);        // borrows the CPU, calls into guest
bifrost_nb_shutdown();
```

The thin adapter scope: `loadLibrary`/`isSupported`/`getError`/
`getSignalHandler` + borrow-CPU trampolines for scalar shorty
signatures (JNIEnv/jobject prefix, x/d-reg split per AAPCS, libffi
closures, `bifrost_call`/`bifrost_call_f` drive). No namespaces, no
CriticalNative, no borrowed method pointers — `version = 4` claims the
pre-Q ABI so ART uses the legacy `getTrampoline` path. Host test
`Tests/Unit/test_nb.c` (61 checks, `make test-nb`, wired into `check-all`).

## Performance

Historical development measurements on a Ryzen 7 5800X3D with GCC -O3,
reported by `Tests/Apps/bench_*.elf` itself (not freshly rerun for this
documentation update):

| Benchmark | Interpreter | JIT | Speedup |
|-----------|-------------|-----|---------|
| fib(35) | 6.4s | 0.20s | ~32x |
| memcpy 256 MiB | 238 MiB/s | ~2,990 MiB/s | ~13x |
| qsort 100K ints | 4.0s | 0.27s | ~15x |
| matrix 256×256 | 15.2 MFLOPS | ~641 MFLOPS | ~42x |
| MIPS (bench_mips, 800M instr) | ~56 MIPS | ~4,400 MIPS | ~78x |

Repeatable samples with checksums and host/compiler metadata can be captured
with `Scripts/Bench/run_benchmarks.py`; see [Docs/TESTS.md](Docs/TESTS.md).

These compute results do not predict game FPS. Current dispatch/cache and
short-call benchmarks show targeted improvements, while the observed Doom
profile still spends most sampled CPU time in translated game/library code.
Matched scenes and frame-time measurements are required to quantify gameplay
improvement; see [Doom 3 measurements](Docs/Apps/doom3.md).

The audio callback worker starts only when a callback stream opens, so
single-threaded guests without callback audio retain JIT chaining and Tier-2
compilation. Multithreaded games keep those publication paths disabled.

The JIT uses:
- **AVX-512** (when available) for 512-bit SIMD
- **AVX2** for 256-bit SIMD
- **SSE4.2** for 128-bit SIMD
- **AES-NI** / **PCLMULQDQ** / **SHA-NI** for crypto
- **BMI1/BMI2** for bit manipulation
- **FMA3** for fused multiply-add

## Use Cases

- Run ARM64 Linux apps on x86_64 (CLI tools, scripts, daemons)
- Test ARM64 builds on x86_64 CI runners
- Test ARM64 game builds (SDL2, OpenGL ES) during development
- Run ARM64 binaries with a remapped guest filesystem; see the [security
  model](Docs/SECURITY.md) before using untrusted binaries
- Run real ARM64 games (SDL2/OpenGL voxel games, teeworlds) on x86_64
  desktops without QEMU

## Limitations

- **Linux user-mode only** — no kernel/system emulation (use QEMU-system)
- **AArch64 only** — no AArch32 (32-bit ARM) support
- **x86_64 host only** — no ARM host support (use native execution)

## Documentation

The current runtime version is **1.5.5**. Older version numbers and test counts
in dated changelog entries describe their original checkpoints; v2.0 is the
planned roadmap. The changelog includes the 211-commit unpublished history
through `aa5dcc7`.

Game setup, screenshots, and current compatibility issues:

- [Neverball](Docs/Apps/neverball.md)
- [vkQuake](Docs/Apps/vkquake.md)
- [Doom 3 / dhewm3](Docs/Apps/doom3.md)
- [Minecraft Weekend](Docs/Apps/minecraft-weekend.md)

Build, test, and maintenance references:

- [v2.0 roadmap](Docs/roadmap.md) — Android games, runtime integration, performance and input

- [Docs/JIT.md](Docs/JIT.md) — JIT architecture and instruction-change guide
- [Docs/SUPPORT.md](Docs/SUPPORT.md) — platform and compatibility support matrix
- [Docs/SECURITY.md](Docs/SECURITY.md) — BIFROST_ROOT scope and guest trust model
- [Docs/CHANGELOG.md](Docs/CHANGELOG.md) — Release history
- [Docs/TESTS.md](Docs/TESTS.md) — Test suite details
- [Docs/THIRD_PARTY.md](Docs/THIRD_PARTY.md) — Test distribution and third-party licenses
- [window stats](Docs/window-stats.md) — FPS title, stats window and overlay
- [bifrost.toml.sample](bifrost.toml.sample) — Config file reference

## License

Bifrost's original code and original regression tests use the
[Unlicense](LICENSE). Third-party fixtures, vendored dependencies and game
assets retain their own terms; the root license does not relicense them.
See [third-party and test distribution notes](Docs/THIRD_PARTY.md).

## Contributing

1. Fork the repo
2. Make your changes (follow the existing code style)
3. Build current guest fixtures with `make setup-tests`, then run relevant
   JIT and interpreter checks; use `make check-all` for provisioned coverage
4. For JIT changes, read [Docs/JIT.md](Docs/JIT.md) and run differential checks.
   Report existing verifier failures separately from newly introduced failures
5. Preserve upstream notices for reused code and include validation evidence
   with your pull request. Original contributions, including tests, use the
   project's Unlicense terms

For bug reports, include:
- The AArch64 binary (or a minimal reproducer)
- The exact command line
- `./bifrost-emu -v -d ... 2>&1 | tail -50` output
