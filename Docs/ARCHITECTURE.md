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
| Guest address space and mappings | `Source/System/Memory/`: `Memory.cpp` (mapping index, access, file backing), `Allocation.cpp`, `Discard.cpp` (madvise, userfaultfd) |
| ELF and dynamic linking | `Source/System/Loader/ElfLoader.cpp`, `DynamicLinker/` |
| Linux syscall dispatch | `Source/System/Syscalls/Syscalls.cpp`; families in `Files/`, `Process/`, `Memory.cpp`, `Time.cpp`, `Compatibility.cpp`; Bifrost-private calls in `HostCalls/` |
| Guest filesystem and devices | `Source/System/VFS/`: `VFS.cpp`, `HostPaths.cpp`, `Nodes/`, `Pseudo/` |
| Android runtime integration | `Source/System/Android/`: `Binder/`, `Properties/`, `Services/`, `Surface/`, `Window/`; guest-side code lives in `Guest/Android/` |
| Guest-side code | `Guest/Android/` Bionic adapters, launchers and managed framework; `Guest/VDSO/` vDSO source |
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
replay. IR construction, scalar SSA and optimization are in `IR/Builder.*`,
`IR/SSA/` and `IR/Passes/`; translation is in `IR/Translate/`. `IR/IR.hpp`
declares `IROp` and `IRInst` and includes the typed parameter structs
(`IR/Parameters.hpp`) and their inline factories/readers (`IR/Factories.hpp`).

`CodeGen/Integer/` and `CodeGen/FloatSIMD/` emit operation families, including
the `FloatSIMD/SIMD/` subdirectory. `Backend/X86Emitter.cpp` encodes host
instructions; `RegisterAlloc/RegisterAllocator.cpp` manages scalar homes and
spills. Runtime dispatch, fault recovery, code caching, profiling and emulator
bridges live in `Runtime/`. Tier-2 separates trace collection, region planning,
region compilation, promotion, feedback, profiling and trial measurement.
`Tier2Internal.hpp` shares implementation helpers; it is not a public interface.

## Host adapters and the dynamic linker

Large host adapters are divided by the APIs or protocols they implement:

- `Audio/AudioThunk/`: lifecycle and export registration in `AudioThunk.cpp`,
  dispatch, stream queue/callback helpers in `Streams.cpp`, then SDL, ALSA,
  Pulse, OpenAL, OpenSL and AAudio backends.
- `Graphics/Thunks/GraphicThunk/`: lifecycle and registration in
  `GraphicThunk.cpp`, generic dispatch, special policies, floating-point ABI
  handling and callback bridges.
- `Graphics/Thunks/DisplayThunk/`: lifecycle and registration in
  `DisplayThunk.cpp`, generic dispatch, proxy calls, Wayland forwarding,
  Vulkan dispatch and Vulkan staging.
- `Graphics/Display/DisplayProxy/`: SDL window/handle ownership in
  `HostWindow.cpp`, X11 forwarding, Wayland object/lifecycle handling, Wayland
  marshalling and Wayland event listeners. These files implement one
  `DisplayProxy` class and share its state; splitting does not create separate
  display instances.
- `Loader/DynamicLinker/`: parsing, library search/loading, symbol lookup,
  relocations, linking, dlopen, glibc patches, ld-linux shims and thread TLS.

## Syscall families

`Syscalls/Syscalls.cpp` tries each family handler in turn; families own
disjoint syscall numbers, so order affects only lookup cost.

- `Files/`: `FileSystem.cpp` dispatches `Descriptors.cpp` (fd I/O, dup,
  fcntl, sync), `Paths.cpp` (open/mkdir/rename/link/chdir/mount), `Metadata.cpp`
  (stat, getdents, chmod/chown, utimensat) and `Transfer.cpp` (splice,
  sendfile); shared helpers are in `FilesInternal.hpp`. `EventIO.cpp` owns
  epoll/eventfd/timerfd/poll/inotify/signalfd, `Sockets.cpp` the socket calls
  and `Ioctl.cpp` ioctl.
- `Process/`: `Process.cpp` dispatches scheduling, identity, wait and exit;
  `Threads.cpp` (clone/futex) and `Signals.cpp` are separate families.
- `Memory.cpp`, `Time.cpp`, `Compatibility.cpp` (newer or rarely used calls).
- `HostCalls/`: Bifrost-private numbers from 0x1000 — API thunk dispatch and
  adapter export lookup (`Thunks.cpp`), the Android launcher window/input/audio
  ABI (`Android.cpp`) and dynamic-linker services (`DynamicLinker.cpp`).

## Guest code

Code under `Guest/` is compiled for, or runs inside, the guest:

- `Guest/Android/Include/`: freestanding Bionic and NativeActivity headers.
- `Guest/Android/Adapters/`: Bionic adapter libraries (EGL, surface, AAudio,
  assets, log, Activity window, managed looper) built by
  `Tools/Android/build-android-adapters.py`.
- `Guest/Android/Launchers/`: native and ART launch executables.
- `Guest/Android/Managed/`: the managed launcher dex (`ManagedLauncher.java`,
  `ActivityProbe.java`) and its in-process framework services in
  `Framework/`, built by `Scripts/Android/android_managed.py build`.
- `Guest/Android/Probes/`: standalone framework probe classes.
- `Guest/VDSO/`: AArch64 vDSO source; its linked image is embedded as
  `Source/System/Loader/VdsoBytes.h`.

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
