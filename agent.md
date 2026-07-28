# Repository Guide & Rules

## Project Overview
bifrost-emu is an AArch64 Linux user-mode emulator for x86_64 hosts, written
in C++17. It runs static and dynamically-linked AArch64 ELF binaries with a
JIT (FrostJIT) and interpreter, syscall/VFS layer, dynamic linking, and host
GL/EGL/SDL2/Vulkan thunks.

## Tech Stack
- Framework / Runtime: C++17 on x86_64 Linux
- Language: C++17 (strict, -Wall -Wextra, zero `any` types)
- Database / ORM: None (VFS is in-memory Yggdrasil)
- Styling: N/A (headless emulator with optional SDL2 fb backend)
- Testing: Custom test runner (`scripts/run_tests.sh`) with 175 test binaries across 6 categories (unit, integration, toybox, real-world, dynamic, benchmarks)

## Codebase Map
- `/src/core/` — Emulator, Memory, CPU, SignalTable, thread_mgr
- `/src/yggdrasil/` — Yggdrasil VFS (Node + FdTable + procfs + devfs)
- `/src/ir/` — IR builder/translator/optimizer/executor
- `/src/jit/` — FrostJIT (x86 backend, regalloc, cache, profiler, codegen)
- `/src/syscalls/` — Linux AArch64 syscall layer (split by concern)
- `/src/frontend/` — decoder + ELF loader + dynamic linker
- `/src/frost_graphics/` — FrostGraphics + GraphicThunk (fb + GL thunking)
- `/src/frost_graphics/` — DisplayThunk + DisplayProxy (X11/Wayland/Vulkan/GBM)
- `/src/interp/` — switch-based instruction interpreter
- `/src/audio/` — Audio subsystem
- `/include/frost/` — Public headers (thunk.hpp, graphics.hpp, display_thunk.hpp, display_proxy.hpp)
- `/ctest/` — JIT regression test sources + binaries
- `/ctest_real/` — Real-world test sources + binaries
- `/scripts/` — Test runner, setup, rootfs scripts
- `/tools/` — Cross-compiler toolchains (musl, glibc, SDL2 SDK)

## Development Commands
- Install: `make setup` (one-click: build + fetch toolchain + cross-compile tests + rootfs + run tests)
- Dev Server: `./bifrost-emu <binary.elf>`
- Build: `make` (or `make USE_SDL2=1 USE_THUNK_GL=1` for host GL/SDL)
- Debug Build: `make debug` (ASan + UBSan)
- Run Tests: `./scripts/run_tests.sh --quick` (or `--quick --no-jit`, `--fwd`)
- Full Tests: `./scripts/run_tests.sh` (175 tests: unit + integration + toybox + real-world + dynamic + benchmarks)
- Full Tests with Real-World Binaries: `./scripts/run_tests.sh --test-all`
- Lint / Format: N/A (no linter configured; -Wall -Wextra enforced)
- Cross-compile: `make cross SRC=ctest/foo.c OUT=ctest/foo.elf`

## Core Rules & Constraints
1. Type Safety: Strict mode enabled; zero `any` types permitted.
2. Architecture: Keep code modular and decoupled.
3. Safety: Back up files to `.backup/` before making edits.
4. Execution: Always verify code functionality using development commands before task handoff.
5. Commit author: `sloppyman2567 <sloppyman2567@users.noreply.github.com>`
6. Always commit to existing git repo on `main` branch.
7. Build before committing — `make` must compile cleanly with no new warnings.
8. Run full test suite before claiming success — `./scripts/run_tests.sh --test-all` (175 tests).
9. Never reference `context.md` in commit messages or shipped docs.
10. Always create a fresh tarball after committing (see context.md rules).
11. Thunk trampolines end with `ret` after `svc`; pointer args outside the 4 GiB direct window bounce through a host buffer with writeback.
12. dlopen of libGL/libSDL2 prefers thunk registration when the on-disk `.so` is not AArch64 (do not map host x86_64 libs as guest code).
13. Absolute-path `dlopen` rejects non-AArch64 ELFs and falls back to soname search under BIFROST_ROOT / toolchain paths.
