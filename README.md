# bifrost-emu

**A bridge between worlds** — a fast ARM64 (AArch64) Linux user-mode emulator
for x86_64 hosts. Run ARM64 Linux applications and games on any x86_64 Linux
machine without QEMU or a cross-compiler.

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
[![Version: 1.5.5](https://img.shields.io/badge/version-1.5.5-orange.svg)](docs/CHANGELOG.md)

## What is bifrost-emu?

bifrost-emu is a user-mode emulator that runs AArch64 (ARM64) Linux
binaries on x86_64 Linux hosts. It translates ARM64 instructions to x86_64
at runtime using a JIT compiler, with a switch-based interpreter fallback.

**What it can do:**
- Run static and dynamically-linked AArch64 ELF binaries
- Load shared libraries (musl and glibc) with GOT/PLT relocation, TLS,
  ifuncs, and DT_INIT_ARRAY constructors
- JIT-compile ARM64 to x86_64 native code (10-40x faster than the interpreter on compute benchmarks)
- Emulate 200+ Linux syscalls (threads, signals, filesystem, memory)
- Provide a virtual filesystem (/proc, /dev, /sys, framebuffer, audio)
- Forward GL/EGL/Vulkan/SDL2/ALSA calls to host libraries (thunking)
- Run multi-threaded guest programs (clone + futex + per-thread JIT)
- Run real games end-to-end (SDL2/OpenGL demos, a Minecraft-like voxel
  game, and teeworlds boot to a stable menu/frame loop under `DISPLAY=:0`)
- Run Neverball 1.6.0 with user-confirmed stable gameplay, replay, readable
  menus and fonts, and repaired JPEG previews; see the
  [Neverball setup and validation notes](docs/neverball.md)
- Run vkQuake 1.33.1 through the Vulkan thunks with user-confirmed gameplay,
  menus, and no sudden crashes during the observed session. Texture corruption
  and buggy camera behavior remain; see [vkQuake status and screenshots](docs/vkquake.md).

**What it is NOT:**
- Not a full-system emulator (no kernel — use QEMU-system for that)
- No APK/ART/Dalvik — Android apps run as bare NativeActivity `.so`s
  (see below); there is no Java runtime
- Not as mature as QEMU-user — it's a smaller, simpler alternative

## Quick Start

```bash
# Build (requires only g++ and the standard library)
make

# Run a static ARM64 binary — silent by default, just shows program output
./bifrost-emu hello.elf

# Verbose — show execution stats on exit
./bifrost-emu -v ctest_real/fib.elf

# Pass arguments to the emulated program
./bifrost-emu ctest_real/toybox seq 1 10

# Run a dynamically-linked binary (requires rootfs — see below)
export BIFROST_ROOT=$PWD/rootfs
./bifrost-emu my_dynamic_app.elf

# Debug mode — trace every instruction to stderr
./bifrost-emu -d hello.elf

# Use --no-jit to force the interpreter (debugging)
./bifrost-emu --no-jit ctest_real/fib.elf
```

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
./scripts/setup-rootfs-all.sh

# Or step-by-step:
./tools/fetch-glibc-toolchain.sh     # glibc toolchain (130 MB)
./tools/fetch-musl-toolchain.sh      # musl toolchain (104 MB)
./scripts/setup-rootfs.sh            # create rootfs with glibc + musl libs
./scripts/fetch-realworld-libs.sh    # libselinux, libpcre2, libssl, etc.
./scripts/fetch-curl-deps.sh         # curl's 24 dependency libs

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

bifrost-emu runs Android **NativeActivity** apps — the `.so` a game ships
as — without ART/Java or an APK. The emulator plays the framework role:
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
so Android-style DT_NEEDED entries resolve automatically, and build.prop
advertises arm64-v8a ABI, SDK 29, ro.kernel.qemu=1.

For full Android app support (APK loading, Dalvik/ART), use a dedicated
Android emulator (or libbifrost's native-bridge adapter). bifrost-emu
targets NativeActivity-style Linux ARM64 binaries.

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
| **ELF Loader** | `src/frontend/elf_loader.cpp` | Loads static & PIE ELF binaries, processes RELA relocations |
| **Dynamic Linker** | `src/frontend/dynamic_linker.cpp` | DT_NEEDED, GOT/PLT, TLS, ifuncs, versioned symbols, ld-linux shim |
| **Decoder** | `src/frontend/decoder.cpp` | Hierarchical ARM64 instruction decoder |
| **IR Translator** | `src/ir/ir_translate*.cpp` | ARM64 → IR (peephole-optimizable) |
| **FrostJIT** | `src/jit/frostjit.cpp` | IR → x86_64 native codegen (AVX2/AVX-512) |
| **Interpreter** | `src/interp/interpreter.cpp` | Switch-based interpreter (JIT verify mode) |
| **Syscall Layer** | `src/syscalls/*.cpp` | 200+ Linux AArch64 syscalls |
| **Yggdrasil VFS** | `src/yggdrasil/*.cpp` | Virtual /proc, /dev, /sys, framebuffer, audio, input |
| **Signal/Thread** | `src/core/signal.cpp`, `thread_mgr.cpp` | rt_sigaction, clone, futex, per-thread JIT |
| **Thunks** | `src/frost_graphics/*.cpp` | GL/EGL/Vulkan/SDL2/ALSA forwarding |

## Building

### Prerequisites

- **g++ 9+** (supports C++17)
- **make**
- Linux x86_64 host

Optional (for full feature set):
- **SDL2 dev headers** (`./tools/fetch-sdl2-headers.sh`) — for framebuffer window
- **AArch64 cross-toolchain** — for cross-compiling test binaries

### Build

```bash
# Default build — auto-detects SDL2 and enables host GL/EGL thunking
# when sdl2-config is available (BIFROST_USE_SDL2 / GL/EGL thunks)
make

# Force a headless build even if SDL2 is installed
make USE_SDL2=0

# Force the SDL2 build (errors if sdl2-config is not installed)
make USE_SDL2=1

# With GL/EGL thunking (forwards guest GL calls to host) — on by default
make USE_SDL2=1 USE_THUNK_GL=1

# Debug build with sanitizers
make debug
# The sanitizer binary is isolated at build/debug/bifrost-emu-dbg.

# Static library (for embedding bifrost-emu in other projects)
make lib
make test-capi  # build + run the host-side C API test (54 checks)
make test-nb    # build + run the native bridge adapter test (61 checks)
```

Each SDL/GL selection gets its own `build/release-sdl*-gl*` directory, and
the sanitizer build uses `build/debug`. The default release executable is
also copied to `./bifrost-emu`; `make clean` removes all build profiles.

### Cross-Compiling Test Binaries

```bash
# Fetch the musl cross-toolchain (104 MB)
./tools/fetch-musl-toolchain.sh

# Cross-compile test C sources and assemble the freestanding test programs
make setup-tests

# Or compile a single file
make cross SRC=ctest_real/my_test.c OUT=ctest_real/my_test.elf
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
./scripts/run_tests.sh --unit         # JIT regression tests
./scripts/run_tests.sh --toybox       # ToyBox integration
./scripts/run_tests.sh --dynamic      # Dynamic linking tests
./scripts/run_tests.sh --bench        # Performance benchmarks

# Filter by name
./scripts/run_tests.sh --filter "sig|brk|pipe"
```

With all fixtures available, the runner selects 245 test runs: 68 unit,
86 integration, 1 sandbox, 9 Toybox, 49 static real-world, 7 dynamic glibc real-world,
15 dynamic-linking, 5 benchmark, and 5 interactive. The dynamic runs need
a configured rootfs; the real-world dynamic binaries are part of the
real-world fixture set. `--quick` skips the five benchmarks. Without a
rootfs, the runner selects 223 runs (218 with `--quick`). Missing guest
fixtures are reported as skips in developer mode; CI uses `--strict` to
turn missing selected fixtures into failures. Display/driver exit-77 skips
remain environment-dependent. See
[docs/TESTS.md](docs/TESTS.md) for details.

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
pointers, and bounce buffers for stack pointers outside the 4 GiB
direct window (`SDL_PollEvent`). Trampolines end with `ret` after `svc`.
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
make cross SRC=ctest_real/test_sdl_gl_triangle.c OUT=ctest_real/test_sdl_gl_triangle.elf
DISPLAY=:0 ./bifrost-emu ctest_real/test_sdl_gl_triangle.elf
```

See `bifrost.toml.sample` for all options.

## C API (libbifrost)

bifrost-emu exposes a stable C API (`api/bifrost.h`) for embedding the
emulator in host programs. Build it with `make lib` (produces
`libbifrost.a`; `main.cpp` is excluded — link your own driver):

```c
#include "bifrost.h"

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

The full API reference and semantics live in `api/bifrost.h`. A
host-side test (`ctest/test_capi.c`, built via `make test-capi`, wired
into `make check-all`) covers the API with 54 checks.

## Android native bridge adapter (`api/native_bridge.h`)

bifrost can act as an ART native bridge (`-XX:NativeBridge`), the
drop-in replacement for QEMU-TCG in an ATL-style Android translation
layer. `api/native_bridge.h` is a clean-room ABI mirror of Android's
`NativeBridgeCallbacks`; `api/native_bridge.cpp` fills the table over
the C API:

```c
#include "native_bridge.h"
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
`ctest/test_nb.c` (61 checks, `make test-nb`, wired into `check-all`).

## Performance

On a typical x86_64 host (Ryzen 7 5800X3D, GCC -O3), timings reported
by the `ctest_real/bench_*.elf` binaries themselves:

| Benchmark | Interpreter | JIT | Speedup |
|-----------|-------------|-----|---------|
| fib(35) | 6.4s | 0.20s | ~32x |
| memcpy 256 MiB | 238 MiB/s | ~2,990 MiB/s | ~13x |
| qsort 100K ints | 4.0s | 0.27s | ~15x |
| matrix 256×256 | 15.2 MFLOPS | ~641 MFLOPS | ~42x |
| MIPS (bench_mips, 800M instr) | ~56 MIPS | ~4,400 MIPS | ~78x |

Repeatable samples with checksums and host/compiler metadata can be captured
with `scripts/run_benchmarks.py`; see [docs/TESTS.md](docs/TESTS.md).

The tight-ALU self-loop speedup is the top end; mixed real workloads
(games, worldgen, GL) land in the 10-40x range. Tight loops benefit
from the dispatch/flag-skip/regalloc work of the 1.5.4-alpha cycle and the vk.xml registry-driven marshalling of 1.5.5-alpha —
the interpreter is unchanged and runs ~56 MIPS regardless.
CoreMark (AArch64 guest) scores ~4,100 iterations/sec on a Ryzen 7
5800X3D, with all CRCs validated. That is about 9.8% of a published
41,946 iterations/sec native result for the same CPU
([CoreMark result](https://zephray.me/coremark/)). The audio callback
worker starts only when a callback stream opens, so single-threaded guests
that do not use callback audio retain JIT chaining and tier-2 compilation.

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
  model](docs/SECURITY.md) before using untrusted binaries
- Run real ARM64 games (SDL2/OpenGL voxel games, teeworlds) on x86_64
  desktops without QEMU

## Limitations

- **Linux user-mode only** — no kernel/system emulation (use QEMU-system)
- **AArch64 only** — no AArch32 (32-bit ARM) support
- **x86_64 host only** — no ARM host support (use native execution)

## Documentation

Game setup, screenshots, and current compatibility issues:

- [Neverball](docs/neverball.md)
- [vkQuake](docs/vkquake.md)

Build, test, and maintenance references:

- [docs/JIT.md](docs/JIT.md) — JIT architecture and instruction-change guide
- [docs/SUPPORT.md](docs/SUPPORT.md) — platform and compatibility support matrix
- [docs/SECURITY.md](docs/SECURITY.md) — BIFROST_ROOT scope and guest trust model
- [docs/CHANGELOG.md](docs/CHANGELOG.md) — Release history
- [docs/TESTS.md](docs/TESTS.md) — Test suite details
- [bifrost.toml.sample](bifrost.toml.sample) — Config file reference

## License

[Unlicense](LICENSE) — public domain. Use it for anything.

## Contributing

1. Fork the repo
2. Make your changes (follow the existing code style)
3. Run `make check` — all tests must pass
4. Run `make verify` — no JIT divergences
5. Submit a pull request

For bug reports, include:
- The AArch64 binary (or a minimal reproducer)
- The exact command line
- `./bifrost-emu -v -d ... 2>&1 | tail -50` output
