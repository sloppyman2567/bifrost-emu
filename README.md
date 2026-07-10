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

  bifrost-emu  v1.5.0.alpha
  x86_64 ◄─────────────────► ARM64
```

[![License: Unlicense](https://img.shields.io/badge/license-Unlicense-blue.svg)](http://unlicense.org/)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform: Linux x86_64](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)]()
[![Version: 1.5.0.alpha](https://img.shields.io/badge/version-1.5.0.alpha-orange.svg)](CHANGELOG.md)

## What is bifrost-emu?

bifrost-emu is a user-mode emulator that runs AArch64 (ARM64) Linux
binaries on x86_64 Linux hosts. It translates ARM64 instructions to x86_64
at runtime using a JIT compiler, with a switch-based interpreter fallback.

**What it can do:**
- Run static and dynamically-linked AArch64 ELF binaries
- Load shared libraries (musl and glibc) with GOT/PLT relocation, TLS,
  ifuncs, and DT_INIT_ARRAY constructors
- JIT-compile ARM64 to x86_64 native code (~6x faster than the interpreter)
- Emulate 200+ Linux syscalls (threads, signals, filesystem, memory)
- Provide a virtual filesystem (/proc, /dev, /sys, framebuffer, audio)
- Forward GL/EGL/Vulkan/SDL2/ALSA calls to host libraries (thunking)
- Run multi-threaded guest programs (clone + futex + per-thread JIT)

**What it is NOT:**
- Not a full-system emulator (no kernel — use QEMU-system for that)
- Not an Android emulator (no APK/ART/Dalvik — it runs Linux ARM64 binaries)
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
supported** — the dynamic linker now processes DT_RELR (compact
relative relocations) which glibc 2.40 ships by default. **glibc
dynamic pthreads work end-to-end**
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

bifrost-emu includes Android-compatible infrastructure for running
Android-ported Linux apps and games:

```bash
# The rootfs has /system/lib64 → /lib64, /vendor/lib64 → /lib64
# so Android-style DT_NEEDED entries resolve automatically.
# build.prop advertises arm64-v8a ABI, SDK 29, ro.kernel.qemu=1.
```

For full Android app support (APK loading, Dalvik/ART), use a dedicated
Android emulator. bifrost-emu targets **Linux ARM64 applications** that
happen to use Android-style paths.

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
# Default build (headless, no SDL2)
make

# With SDL2 window backend for /dev/fb0
make USE_SDL2=1

# With GL/EGL thunking (forwards guest GL calls to host)
make USE_SDL2=1 USE_THUNK_GL=1

# Debug build with sanitizers
make debug

# Static library (for embedding bifrost-emu in other projects)
make lib
```

### Cross-Compiling Test Binaries

```bash
# Fetch the musl cross-toolchain (104 MB)
./tools/fetch-musl-toolchain.sh

# Cross-compile all test .c files
make setup-tests

# Or compile a single file
make cross SRC=ctest_real/my_test.c OUT=ctest_real/my_test.elf
```

## Testing

```bash
# Run the full test suite (168 tests defined; 124 pass + 30 skip without
# --test-all because busybox-aarch64 is not downloaded, and 7 dynamic +
# 7 dynamic-glibc real-world tests skip without a rootfs)
make check

# Quick mode (skip benchmarks — 114 pass + 30 skip = 144 total attempted)
make check-quick

# Run under the interpreter (catches JIT drift)
make check-nojit

# JIT divergence checker (slow, catches codegen bugs)
make verify

# Download real-world binaries (busybox, iperf2) and run all tests
make check ARGS="--test-all"

# Run only specific categories
./scripts/run_tests.sh --unit         # JIT regression tests
./scripts/run_tests.sh --toybox       # ToyBox integration
./scripts/run_tests.sh --dynamic      # Dynamic linking tests
./scripts/run_tests.sh --bench        # Performance benchmarks

# Filter by name
./scripts/run_tests.sh --filter "sig|brk|pipe"
```

### Test Categories

| Category | Count | Description |
|----------|-------|-------------|
| Unit | 35 | Focused JIT codegen regression tests (`ctest/`) |
| Integration | 51 | Real-world programs exercising multiple subsystems (`ctest_real/` + `test/`) |
| Toybox | 9 | ToyBox subcommands (echo, seq, ls, md5sum, etc.) |
| Real-world | 56 | Downloaded static + dynamic glibc binaries (BusyBox, iperf3, coreutils) |
| Dynamic | 7 | Dynamically-linked binaries (musl + glibc) — need rootfs |
| Benchmarks | 5 | Performance (MIPS, memcpy, sort, matrix, fib) — skipped with `--quick` |
| **Total** | **163** | |

Interactive tests (5: echo, repl, cat, sh, fgets_test) are opt-in via
`--interactive` and not counted in the 163.

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
no env var needed. When the host has the dev libraries installed, guest
graphic/audio programs use host hardware acceleration. When the host
doesn't have the libraries, symbols resolve to stubs that return 0
(safe fallback — the guest falls back to software rendering or no-op).

The thunk uses per-type symbol ID ranges (Graphics: 0x0000-0x0FFF,
Audio: 0x1000-0x1FFF, Display: 0x2000-0x2FFF) to avoid collisions.
Pointer arguments are automatically translated from guest addresses to
host addresses via the emulator's direct memory window, enabling
functions like `glVertexPointer`, `glDrawElements`, and `glGetIntegerv`
to work correctly.

See `bifrost.toml.sample` for all options.

## Performance

On a typical x86_64 host (Ryzen 7, GCC -O3):

| Benchmark | Interpreter | JIT | Speedup |
|-----------|-------------|-----|---------|
| fib(35) | 8.2s | 1.4s | 5.9x |
| memcpy 1GB | 280 MiB/s | 1850 MiB/s | 6.6x |
| qsort 1M ints | 2.1s | 0.38s | 5.5x |
| matrix 1024² | 3.8s | 0.72s | 5.3x |
| MIPS est. | 97 MIPS | 571 MIPS | 5.9x |

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
- Analyze ARM64 binaries in a sandboxed environment
- Learn how AArch64 instruction emulation and Linux syscalls work

## Limitations

- **Linux user-mode only** — no kernel/system emulation (use QEMU-system)
- **AArch64 only** — no AArch32 (32-bit ARM) support
- **x86_64 host only** — no ARM host support (use native execution)
- **No vDSO** — some clock_gettime paths are emulated, not native

## Documentation

- [CHANGELOG.md](CHANGELOG.md) — Release history
- [TESTS.md](TESTS.md) — Test suite details
- [ROADMAP.md](ROADMAP.md) — Future plans
- [bifrost.toml.sample](bifrost.toml.sample) — Config file reference
- `context.md` (in tarball) — Detailed development history (not in git)

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
