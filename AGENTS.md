# bifrost-emu

AArch64 Linux user-mode emulator for x86_64 Linux hosts: FrostJIT, reference
interpreter, Linux syscalls/VFS, dynamic linking and host graphics/audio adapters.

## Navigation and required reading

- [Source layout](Docs/ARCHITECTURE.md): subsystem ownership and entry points.
- **Read [Docs/JIT.md](Docs/JIT.md) before JIT implementation, debugging,
  review or refactoring**, including shared decoder and IR changes.
- [FrostJIT guidance](Source/FrostJIT/AGENTS.md): compiler, IR/SSA, code generation,
  register allocation, caching and Tier-2.
- [Interpreter guidance](Source/Interpreter/AGENTS.md): shared decoder and
  reference instruction semantics.
- [System guidance](Source/System/AGENTS.md): memory, loader, syscalls, VFS,
  Android and host integration. Narrower guides live beside those subsystems.
- [Android runtime](Docs/Android/ANDROID_RUNTIME.md), [Binder](Docs/Android/ANDROID_BINDER.md),
  [APK tooling](Docs/Android/ANDROID_APK.md), [tests](Docs/TESTS.md).

Implementation headers live beside their sources under `Source/`; public C++
headers live in `Include/Bifrost/`, generated tables in `Include/Generated/`,
and guest Android shims/services in `Guest/Android/`. Includes are relative to
`Source/` or `Include/`. Keep downloaded SDK and guest-platform paths in their
original spelling when moving repository files.

## Working rules

- Start with the concrete failure, trace the first wrong value and compare
  with a trusted reference. Prefer primary sources and reproducible commands.
- Preserve existing uncommitted work. Keep changes scoped; work on the current
  branch unless asked otherwise. Commit related code and docs only; never push
  without explicit approval.
- Keep throwaway probes/logs outside the repository. Diagnostic switches belong
  in `Source/System/Core/DebugFlags.hpp`, not repeated getenv calls in hot paths.
- Preserve guest/host address boundaries, mapping and signal semantics, aliases,
  register widths and precise fault state. Do not trade correctness checks for
  benchmark speed or tailor runtime behavior to one host/kernel.
- State which claims were checked against a source and which were observed in
  tests. A passing build or smoke launch does not establish game playability.

## Build and validation

```sh
make -j8 USE_SDL2=1 USE_THUNK_GL=1 all lib
make setup-tests
make generated-check
bash Scripts/Testing/run_tests.sh --strict
bash Scripts/Testing/run_tests.sh --no-jit --strict
make verify
```

Run emulator/test commands with GPU access. Validate headless builds separately
with `USE_SDL2=0 USE_THUNK_GL=0`; build configurations have separate object trees.
`make cross SRC=... OUT=...` builds an individual guest fixture. Rebuild stale
ignored `.elf` files, using the fixture's correct musl/glibc and static/dynamic
configuration; see [the test guide](Docs/TESTS.md). Missing optional fixtures
need explicit reporting, not a claim that the full suite passed.

Run focused regressions for the changed contract and relevant host/API/Android
checks. Review before committing, update docs after the implementation and
report remaining limitations. Detailed bug history and performance checkpoints
belong in `Docs/`, rather than this instruction file.
