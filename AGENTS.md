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
- [Guest rootfs](Docs/ROOTFS.md): discovery order, installing and the install layout.
- `Docs/archive/`: dated history moved out of the guides and changelog; not required reading. Keep guides on how things
  work now, and move a checkpoint there once it stops describing current behaviour.
- [Android runtime](Docs/Android/ANDROID_RUNTIME.md), [Binder](Docs/Android/ANDROID_BINDER.md),
  [APK tooling](Docs/Android/ANDROID_APK.md), [tests](Docs/TESTS.md).

Implementation headers live beside their sources under `Source/`; public C++
headers live in `Include/Bifrost/`, generated tables in `Include/Generated/`,
and guest-side code in `Guest/` (Android adapters, launchers and managed
framework; vDSO source). Includes are relative to `Source/` or `Include/`.
Keep downloaded SDK and guest-platform paths in their original spelling when
moving repository files. Name files after what they implement, not catch-alls
such as "misc", "common" or "core". The current version lives in
`Include/Bifrost/Version.hpp`; keep README, Makefile, sample config, rootfs
`os-release` and the changelog in step with it.

## Working rules

- Start with the concrete failure, trace the first wrong value and compare
  with a trusted reference. Prefer primary sources and reproducible commands.
- The project site (home and games pages; `Tools/Site/build-site.py`, deployed by `.github/workflows/pages.yml`) is generated from the
  README's "What runs" summary, table and screenshots, `compatibility.json` and `Version.hpp`; edit those, never the
  generated pages. It is for players: plain language, technical detail behind links or the games page's Details;
  its own copy is in `Tools/Site/home.html` and `games.html`, and its name is bifrost-emu (Bifrost as the alternate name).
  The icon and social preview card live in `Assets/Branding/`; the card is also uploaded by hand in the repository's
  Settings, Social preview, so re-upload it when it changes. Its build fails when a playable or blocked app in `compatibility.json` has no matching README row
  or a different status, so record a compatibility change in both.
- Downloads in the setup tools go through `download_progress.fetch` in `Tools/Android/download_progress.py` (progress on a
  terminal, sparse lines in a log; an interrupted download stays as `NAME.part` and resumes, and only a verified file replaces
  the destination), so a long fetch never looks hung and Ctrl-C never costs it; a new fetcher uses it too.
- The desktop library takes gamepads through `Scripts/Android/android_gamepad.py` (SDL3 by ctypes) as key presses on the focused
  widget, never as a second input path, and only while a library window is active: a game started from it reads the same pad.
  It must work without SDL3 or a pad, and an exception in the pad path switches pads off rather than aborting the process
  (PyQt aborts on an exception in a slot). The commands the library window starts (`QProcess` children) are ended before Qt
  destroys them (`Library.stop_jobs`, on close and on `aboutToQuit`): a QProcess destroyed while running calls its slot from its
  destructor, and the exception that raises aborts the process. In the gamepad layout menus, confirmations and the settings page are drawn inside the
  library window (`Sheet`, the embedded settings page), never as popup windows or message boxes the pad does not reach.
- `asgard setup` and the library's **Set up…** offer run the steps named in `Scripts/Android/android_provisioning.py`. Keep
  that list in step with the Makefile's `android-provision` and `install-asgard.py`'s `REQUIRED`/`MANIFESTS`
  (`test_android_provisioning.py` fails when they drift). An installed copy is never set up from its own files: it is
  reinstalled.
- Built-in per-app settings are the `settings` of an app's `compatibility.json` entry (`android_settings.app_defaults`), applied
  under saved settings, never written back to them: a launch, `asgard config` and the library page all read `effective`/`app_defaults`,
  and the page saves only a change, so a default that later changes still reaches an untouched app.
- Setup scripts find host tools through `Scripts/Setup/find-host-tools.sh` (sbin, JDK and LLVM directories, `~/.local/bin`,
  `BIFROST_TOOL_DIRS`), never a bare `command -v`: `debugfs` is off an ordinary Debian user's `PATH`. Hosts without SDL3 build
  it with `Tools/Fetch/fetch-sdl3.py` into `Tools/sdl3`.
- Preserve existing uncommitted work. Keep changes scoped; work on the current
  branch unless asked otherwise. Commit related code and docs only; never push
  without explicit approval.
- `core.fileMode` is false here, so git does not notice executable bits: a new
  script with a shebang is committed non-executable unless you run
  `git update-index --chmod=+x PATH`. A clone or archive then fails on
  `./Scripts/...`. Check with `git ls-files -s PATH` (100755).
- Keep throwaway probes/logs outside the repository. Diagnostic switches belong
  in `Source/System/Core/DebugFlags.hpp`, not repeated getenv calls in hot paths.
- Android runtime provisioning must retain `/system/lib64/{libc,libm,libdl}.so`
  compatibility links to canonical APEX Bionic libraries. Upgrade verified cached
  inventories before rebuilding managed roots; never duplicate Bionic DSOs.
- Gralloc's allocator belongs to the Binder realm, independently of app
  lifetimes. Keep its sealed memfd transport in `Include/Bifrost/AndroidGralloc.h`
  in step with the guest mapper; validate concurrent clients and peer exit.
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

- Managed HOME uses Asgard via the authenticated session supervisor; preserve background/resume lifecycle and the pinned shell/am provisioning contract.
