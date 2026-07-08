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

bifrost-emu is a **user-mode emulator** that runs AArch64 (ARM64) Linux
binaries on x86_64 Linux hosts. It translates ARM64 instructions to x86_64
in real-time using a high-performance JIT compiler, with an interpreter
fallback for correctness verification.

**Key capabilities:**
- Runs **static** and **dynamically-linked** AArch64 ELF binaries
- Supports **musl** and **glibc** libc (dynamic linking with shared library
  loading, GOT/PLT relocation, TLS, ifuncs, DT_INIT_ARRAY constructors)
- **JIT compiler** with x86_64 native codegen (SSE4.2/AVX2/AVX-512 when
  available) for ~6x speedup over the interpreter
- **Linux syscall layer** — 200+ syscalls including threads (clone/futex),
  signals (rt_sigaction/sigaltstack), filesystem, memory management,
  epoll, timerfd, eventfd, io_uring stubs
- **Yggdrasil VFS** — virtual filesystem with /proc, /dev, /sys, host
  file passthrough, framebuffer (/dev/fb0), audio (/dev/dsp), input
  (/dev/input/event*)
- **Android-compatible rootfs** — /system, /vendor, /data, /sdcard
  structure with build.prop and hardware permissions for Android apps
- **Graphics/Audio/Display thunking** — forward guest OpenGL/EGL/Vulkan/
  SDL2/ALSA/PulseAudio calls to host libraries
- **Multi-threaded guest support** — clone() with CLONE_VM/CLONE_SETTLS,
  per-thread JIT instances, futex-based synchronization

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
set up a rootfs:

```bash
# 1. Fetch a cross-toolchain (provides AArch64 glibc/musl libraries)
./tools/fetch-glibc-toolchain.sh    # glibc (130 MB)
./tools/fetch-musl-toolchain.sh     # musl (104 MB)

# 2. Create the rootfs (copies libs, creates /etc, /system, /data, etc.)
./scripts/setup-rootfs.sh

# 3. Run with BIFROST_ROOT pointing to the rootfs
export BIFROST_ROOT=$PWD/rootfs
./bifrost-emu my_dynamic_app.elf
```

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
# Run the full test suite (115+ tests)
make check

# Quick mode (skip benchmarks)
make check-quick

# Run under the interpreter (catches JIT drift)
make check-nojit

# JIT divergence checker (slow, catches codegen bugs)
make verify

# Download real-world binaries (busybox, iperf2) and run all 150 tests
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
| Unit | 34 | Focused JIT codegen regression tests (`ctest/`) |
| Integration | 48 | Real-world programs exercising multiple subsystems (`ctest_real/`) |
| Toybox | 9 | ToyBox subcommands (echo, seq, ls, md5sum, etc.) |
| Real-world | 18 | Downloaded static binaries (BusyBox, iperf2, curl) |
| Dynamic | 2 | Dynamically-linked binaries (musl + glibc) |
| Benchmarks | 5 | Performance (MIPS, memcpy, sort, matrix, fib) |

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
BIFROST_THUNK_GRAPHICS=1         # Enable GL/EGL thunking
BIFROST_THUNK_AUDIO=1            # Enable ALSA/PulseAudio thunking
```

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

- **Run ARM64 Linux apps on x86_64** — CLI tools, scripts, daemons
- **Cross-platform CI** — test ARM64 builds on x86_64 CI runners
- **Game development** — test ARM64 game builds (SDL2, OpenGL ES)
- **Security research** — sandboxed analysis of ARM64 binaries
- **Education** — learn AArch64 instruction set and Linux syscalls
- **Embedded development** — test ARM64 firmware/userspace on x86_64

## Limitations

- **Linux user-mode only** — no kernel/system emulation (use QEMU-system)
- **AArch64 only** — no AArch32 (32-bit ARM) support
- **x86_64 host only** — no ARM host support (use native execution)
- **No vDSO** — some clock_gettime paths are emulated, not native
- **glibc printf SIMD path** — glibc's SIMD-optimized printf may produce
  garbled output (musl printf works fully); use write()/writev() for
  reliable output with glibc dynamic binaries

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
