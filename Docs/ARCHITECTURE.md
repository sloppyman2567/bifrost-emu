# Source layout

Bifrost is an AArch64 Linux user-mode emulator for x86_64 Linux hosts. Guest
instructions run through FrostJIT or the reference interpreter; both share
the emulator's memory, signal, syscall, loader and host-service implementations.

## Where to start

| Responsibility | Directory and entry points |
|---|---|
| CLI and configuration | `Source/Main.cpp`, `Source/System/Core/Config.cpp` |
| Emulator startup and lifetime | `Source/System/Core/Emulator/`: `Lifecycle.cpp`, `ElfLoading.cpp`, `GuestStartup.cpp`, `Android.cpp`, `ThunkWiring.cpp` |
| Decode and reference execution | `Source/Interpreter/Decoder/`, `Core/`, `FloatSIMD/` |
| JIT pipeline | `Source/FrostJIT/`: `IR/`, `Compiler/`, `CodeGen/`, `RegisterAlloc/`, `Backend/`, `Runtime/`, `Tier2/` |
| Guest address space and mappings | `Source/System/Memory/Memory.hpp`, `Memory.cpp` |
| ELF and dynamic linking | `Source/System/Loader/ElfLoader.cpp`, `DynamicLinker/` |
| Linux syscall dispatch | `Source/System/Syscalls/Syscalls.cpp`; `Files/` and `Process/` hold their respective syscall families |
| Guest filesystem and devices | `Source/System/VFS/`: `VFS.cpp`, `HostPaths.cpp`, `Nodes/`, `Pseudo/` |
| Android runtime integration | `Source/System/Android/`: `Binder/`, `Properties/`, `Services/`, `Surface/`, `Window/`; guest adapters and Java services live in `Guest/Android/` |
| Graphics and display | `Source/System/Graphics/`: `Display/`, `Input/`, `Thunks/` |
| Host audio and API forwarding | `Source/System/Audio/Audio.cpp`, `AudioThunk/` |
| Embedding interfaces | `Include/Bifrost/` C++ API, `Source/System/API/` C API and native bridge |

Paths in this table are relative to the repository root. Headers that describe
an implementation live beside it under `Source/`; public C++ headers live in
`Include/Bifrost/`. C/C++ includes use paths relative to `Source/` or `Include/`,
for example `FrostJIT/Compiler/FrostJIT.hpp` and `Bifrost/Emulator.hpp`.
`Include/Generated/` contains committed generated tables, not hand-written APIs.

## FrostJIT

Read [the JIT guide](JIT.md) before changing the compiler or JIT-facing decoder.
`Compiler/BlockAnalysis.cpp` predicts fallback boundaries and scans candidate
inline leaves; `BlockCompiler.cpp` orchestrates Tier-1 translation, emission and
publication. `VerificationStores.cpp` collects store footprints for differential
replay. IR construction, typed parameters, scalar SSA and optimization are in
`IR/Builder.*`, `IR/IR.hpp`, `IR/SSA/` and `IR/Passes/`; translation is in
`IR/Translate/`.

`CodeGen/Integer/` and `CodeGen/FloatSIMD/` emit operation families, including
the `FloatSIMD/SIMD/` subdirectory. `Backend/X86Emitter.cpp` encodes host
instructions; `RegisterAlloc/RegisterAllocator.cpp` manages scalar homes and
spills. Runtime dispatch, fault recovery, code caching, profiling and emulator
bridges live in `Runtime/`. Tier-2 separates trace collection, region planning,
region compilation, promotion, feedback, profiling and trial measurement.
`Tier2Internal.hpp` shares implementation helpers; it is not a public interface.

## Host adapters and the dynamic linker

Large host adapters are divided by the APIs or protocols they implement:

- `Audio/AudioThunk/`: lifecycle and export registration in `Core.cpp`, dispatch
  and common helpers, then SDL, ALSA, Pulse, OpenAL, OpenSL and AAudio backends.
- `Graphics/Thunks/GraphicThunk/`: registration, generic dispatch, special
  policies, floating-point ABI handling and callback bridges.
- `Graphics/Thunks/DisplayThunk/`: registration, generic dispatch, proxy calls,
  Wayland forwarding, Vulkan dispatch and Vulkan staging.
- `Graphics/Display/DisplayProxy/`: SDL window/handle ownership in
  `HostWindow.cpp`, X11 forwarding, Wayland object/lifecycle handling, Wayland
  marshalling and Wayland event listeners. These files implement one
  `DisplayProxy` class and share its state; splitting does not create separate
  display instances.
- `Loader/DynamicLinker/`: parsing, library search/loading, symbol lookup,
  relocations, linking, dlopen, glibc patches, ld-linux shims and thread TLS.

Subsystem `*Internal.hpp` headers are private implementation details. Keep a
protocol's callbacks and their file-local state together. Splits should follow
responsibilities, rather than an arbitrary maximum line count: a coherent
decoder or syscall dispatch table can reasonably remain large.

## Tools, tests and documentation

| Directory | Contents |
|---|---|
| `Scripts/Android/` | Launch, install, supervise and test Android applications |
| `Scripts/Testing/` | Guest suite, JIT verification and focused compatibility checks |
| `Scripts/Setup/` | Rootfs and runtime setup, fixture download orchestration |
| `Scripts/Bench/` | Benchmark runner |
| `Tools/Android/` | Android provisioning and adapter builders |
| `Tools/CodeGen/` | Table specifications, generators and consistency checks |
| `Tools/Fetch/` | Toolchain/header fetchers |
| `Tools/Reference/` | Independent instruction and reference implementation utilities |
| `Tests/Unit/`, `Tests/Apps/`, `Tests/Asm/` | Guest fixtures, application probes and assembly tests |
| `Tests/Host/` | Native host checks and Java framework probe sources |
| `ThirdParty/` | Vendored headers, registries and dependencies |
| `Docs/Android/`, `Docs/Apps/` | Android contracts and application status |

Downloaded toolchains and SDKs retain their own internal names, such as
`Tools/aarch64-linux-musl-cross/` and the Android NDK's `include/android/`.
Guest paths such as `/proc`, `/system` and `/data/local/tmp` also retain their
platform spelling. Repository renames must not rewrite those external paths.

The Makefile discovers `.cpp` files recursively under `Source/`. Objects and
dependency files mirror their paths inside each build configuration. A source
manifest triggers relinking after removals or renames; static archives are
created fresh and replaced after success so removed members cannot survive.
SDL/GL and headless builds retain separate directories under `build/`.
Generated guest binaries, runtime state and downloaded dependencies are ignored.

## Validation after a move or split

Build both supported host configurations, run `make generated-check`, then run
the guest suite in JIT and interpreter modes. For compiler changes, also run
`make verify`. API, Android adapter and framework probe tests exercise paths
that an emulator-only build does not cover. Rebuild stale ignored guest fixtures
before treating their failures as runtime regressions. Graphics runs need GPU
and display access; audio-only stress tests may intentionally select dummy audio.

See [the test guide](TESTS.md), [Android runtime guide](Android/ANDROID_RUNTIME.md)
and [JIT guide](JIT.md) for the exact contracts and current validation limits.
