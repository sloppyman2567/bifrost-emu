# Tests

How the suite is organized and run. Passing fixtures do not establish untested application behavior, and a smoke
launch is not game playability. The dated checkpoints that used to live here (what was run, when, and what it found)
are kept unchanged in [the test history](archive/TESTS-history.md); the changelog records what changed.

## Test inventory

Original regression sources ship under Bifrost's Unlicense. Generated guest
ELFs are rebuilt locally; included and downloaded third-party fixtures keep
their upstream licenses. See [fixture distribution](THIRD_PARTY.md) for the
source/binary distinction and game-data boundaries.

The `fcsel_high_regs` regression has 61 checks for high FCSEL source registers,
FP destination 31, single/double precision, aliasing, GPR preservation, and
discarded FP-to-GPR writes to XZR. It passes under QEMU, JIT, and interpreter;
see [the vkQuake reproduction](Apps/vkquake.md).

The fully provisioned suite was 377 runs under `run_tests.sh --strict` (2026-11-01) with the JIT and 362 with `--no-jit` (15 tests that exist only in the JIT run are not selected there), in these categories: unit
(focused guest instruction and JIT regressions and host gates), integration (guest programs in `Tests/Apps/` and
`Tests/Asm/`), sandbox, per-thread JIT, uncached FP registers (the by-element FP fixtures with `BIFROST_NO_VEC_CACHE=1` and
`BIFROST_NO_FP_CACHE=1`), interactive (scripted stdin), toybox, real-world static and dynamic programs,
dynamic linking (musl/glibc and XWayland; needs a rootfs) and benchmarks (omitted by `--quick`). The runner prints
each category's count when it starts, and `--filter REGEX` runs only the tests whose names match. Without a configured rootfs the
dynamic runs are not selected; missing guest fixtures and unavailable display or SDL support appear as skips, never as passes.
Since that count, `jit_ld1r` and the uncached FP registers category added three runs in each mode; without a rootfs the suite
was 353 runs with the JIT and 337 with `--no-jit` (16 skips) on 2026-11-03. The fully provisioned total was not re-run.
On 2026-11-06, in a tree that adds `memory_high_gap` (a host `Memory` test for a 16 GiB reservation in a 512 GiB window,
JIT run only), `--strict` ran 365 tests with the JIT, all passing and none skipped, and 337 with `--no-jit`, all passing, with
28 JIT-only tests not selected there (15 in the 2026-11-01 count).

### Persistent JIT cache (2026-10-26)

`Scripts/Testing/test_jit_disk_cache.sh` (`run_tests.sh` entry `jit_disk_cache`, about 2 s) runs
`Tests/Unit/jit_dispatch_cache_growth.elf` with the cache in a temporary directory: a recording run, a reuse
run (over 90 % hits), verify mode (no mismatch), damaged `main.bin` and truncated segments (rejected, results
correct), a 1 MiB budget (file bytes on disk stay within it), two concurrent writers and their merge, a codegen
switch (separate fingerprint), `Tests/Unit/jit_disk_cache_relocated.elf` mapping one code page from a file at
0x10000000 and then 0x7000000000 (verify finds the record equal to a fresh compile, the high mapping reuses it;
the previous distance-based immediate rule failed this), a fourth build whose budget holds about two of the three existing directories
(older ones pruned, nothing it records dropped) and a group-writable cache directory (cache off, program
unaffected). It passed 10 runs in a row. It does not cover Android apps, code-cache renewals in a game or
network filesystems; [JIT.md](JIT.md#persistent-translation-cache-2026-10-26-experimental-off-by-default)
records the manual checks.

### Thread memory and the guest's RAM view (2026-10-25)

`test_thread_memory.sh` runs 1 and 200 idle guest threads and reads the emulator's peak RSS from `/proc`; the 200-thread run
may exceed the 1-thread run by at most 120 MB (28 MB now, 352 MB before the decode cache became lazy; the script
fails on the old build). `test_guest_memory.sh` checks the policy function on the host (cap, pressure, inconsistent
and empty hosts) and then `/proc/meminfo` against `sysinfo` inside the guest at the default cap, 2048 MB and no cap
(toybox's field order, `free <= available <= total`, no swap, the expected total).

### Code-cache generations (2026-10-25)

`Tests/Unit/jit_cache_generations.c`, run by `Scripts/Testing/test_jit_generations.sh` (about 8 s), forces a
1 MiB code budget so the shared JIT is replaced about 500 times while threads sleep in nanosleep and futex,
one waits in a futex inside a nested call, two run nested call loops that compile throughout, the main thread
rewrites code with instruction-cache maintenance, a 3 ms timer delivers signals, and a child forks afterwards;
every result is checked. The default layout must release each retired generation; the ordinary-frame layout
(`BIFROST_NO_CHAIN_SKIP=1`) keeps only the first and falls back, which the script accepts. `test_jit_cache.sh` now
expects renewals at a 1 MiB budget.

Run on the final build: the 333 unit, integration, toybox and real-world runs (`--quick`) passed at the default
budget. With `BIFROST_JIT_CACHE_MB=1`, 332 of those 333 passed, and the 22 dynamic-linking runs selected with
`--filter 'test_dyn|hello_dyn|dlopen|dladdr'` passed. The one failure, `jit_tier2_discovery`, passes at the
default budget. It fails at 1 or 2 MiB with no renewal at all (a gdb breakpoint on `Emulator::renew_jit` never
fires): the test drives `lookup_or_translate` and `compile_tier2_region` directly, which do not go through
`translate_entry`, so it meets the plain budget limit as it did before; that the budget is simply too small for
its compiles is likely but was not checked. The generations fixture also passed 20 of 20 repeated runs
pinned to different CPU sets, three at a time. `make verify` reported two failures, both present before this
change: `jit_bfm_extract` times out (exit 124; the 95 s against the 30 s limit recorded on 2026-10-23, not
re-measured on the old tree), and `jit_dispatch_cache_growth` is a threaded fixture that `verify_jit.sh` runs
under differential replay, which logs a PC divergence and "suspended" (identical output from the pre-change
binary). The full strict suite and the Android tests were not run. One real game, Melon Playground, ran five
minutes on this build while being played and stayed up (memory numbers in [JIT.md](JIT.md)).

## Running tests

Additional standalone regressions build their own fixtures in temporary
directories and exercise the current emulator (`BIFROST_EMU` can override it):

```sh
./Scripts/Testing/test_rootfs_discovery.sh  # dynamic programs find a rootfs without flags
./Scripts/Testing/test_rootfs_install.sh    # install-rootfs.sh output runs from an installed layout
./Scripts/Testing/test_guest_cwd.sh         # getcwd/cd follow the host cwd without a sandbox
./Scripts/Android/test_android_super.py        # dynamic-partition unpacker vs a synthetic image (--compare SUPER.img: vs lpunpack)
./Scripts/Android/test_android_adapter_exports.py # the built managed libGLESv2.so exports every core ES 2.0-3.2 command and libandroid.so exports ASurfaceControl_createFromWindow (an app library's missing import fails its whole dlopen)
./Scripts/Android/test_android_gamepad.py     # the library's gamepad control: pad states to actions, repeat, stick, keys per widget (offline)
./Scripts/Android/test_download_progress.py    # asgard-setup's download progress lines (terminal and piped), and resumable fetch against a local HTTP server
./Scripts/Android/test_asgard_stale_broker.py  # a broker an upgrade left running: restarted when idle, refused when busy; realm stop --force; a broker that cannot start reports its exit and output (both start paths)
./Scripts/Android/test_asgard_process_host.py   # service children carry --asgard-service-parent and die with a killed supervisor (real emulator, plus a control)
make test-shader-index-conversion               # the int() rewrite glCompileShader retries a rejected ES 3.x source with; gles_uint_index (run_tests.sh) checks it through the thunk on the host driver
make test-android-wheel-gestures               # wheel/pinch/middle-drag as two-finger gestures (SDL-free); test-android-sdl-backend checks them through the backend
make test-android-pad-cursor                   # the gamepad touch cursor (SDL-free): Start+Select chord, modes, stick, fingers, edge crop; test-android-sdl-backend plays it through the backend
./Scripts/Testing/test_window_stats.sh         # window stats modes, overlay and sensors (SDL probes open windows unless SDL_VIDEODRIVER=offscreen; exit 77 without host SDL2)
./Scripts/Testing/test_rootfs_profile.sh   # named rootfs profiles: install, list, select, errors, uninstall (exit 77 without the fetched toolchains)
./Scripts/Testing/test_setup_rootfs.sh     # bifrost-emu --setup-rootfs installs, uninstalls, runs a packaged installer from the cache and reports a missing one
./Scripts/Testing/test_guest_env.sh         # the guest gets the caller's environment (curated one in a sandbox)
./Scripts/Testing/test_exec_jit.sh          # programs started by execve run under the JIT (exit 77 without the busybox fixture)
./Scripts/Testing/test_apk_launch.sh        # bifrost-emu app.apk hands the package to Asgard (stub)
./Scripts/Testing/test_host_cpu_models.sh   # JIT fixtures on older x86-64 CPUs under qemu-x86_64; exit 77 if it cannot run
make test-audio-declick                     # a stream leaving or rejoining the mix never steps the output (no sound device needed)
./Scripts/Testing/test_asgard_install.py    # install-asgard.py install, update, removal, provisioning checks and microG download (fake checkout, offline)
./Scripts/Testing/test_install_deps.py      # install-deps.sh per package manager, with fake managers (offline); tools/libraries found without their package, find-host-tools.sh, fetch-sdl3.py's window-system check
./Scripts/Testing/test_rootfs_library_fetch.py # the rootfs library fetchers and install-rootfs.sh --full against a broken or changed mirror (fake curl, offline)
./Scripts/Testing/test_late_tls.sh          # late GD/TLSDESC and rejected initial-exec
./Scripts/Testing/test_late_tls_pthread.sh  # existing/new threads, reuse, pointer/alignment/weak cases
./Scripts/Testing/run_thunk_compat.sh       # SDL/GL/Vulkan ABI probes in both modes
./Scripts/Testing/run_thunk_soak.sh 64      # repeated SDL window/mapping/thread lifecycle
```

The TLS scripts require the AArch64 glibc compiler; pthread tests also need
rootfs glibc libraries. Thunk scripts require an SDL2/GL-enabled emulator and
the musl cross compiler. The SDL soak uses the dummy video driver and does
not establish long-duration interactive game stability.

```bash
make check                 # standard JIT suite
make check-quick           # JIT suite without the five benchmarks
make check-nojit           # quick suite using the interpreter
make check-fwd              # quick suite with BIFROST_ENABLE_FWD=1
make ci                    # guest-correctness gate including sandbox and glibc dynamic checks
make verify                # strict JIT/interpreter differential gate

./Scripts/Testing/run_tests.sh --unit                 # focused JIT regressions
./Scripts/Testing/run_tests.sh --unit --strict        # fail if a selected fixture is missing
./Scripts/Testing/run_tests.sh --integration          # integration tests
./Scripts/Testing/run_tests.sh --dynamic              # dynamic-linker tests
./Scripts/Testing/run_tests.sh --filter 'sig|brk|pipe' # filter test names
./Scripts/Testing/run_tests.sh --no-jit --quick       # interpreter quick suite
./Scripts/Testing/run_tests.sh --help                 # all runner options
```

`make check-nojit` and `make check-fwd` use quick mode. To include
benchmarks in either mode, invoke the script directly with `--no-jit` or
`--fwd` and omit `--quick`.

The default runner includes interactive and real-world categories. It
attempts to fetch the BusyBox fixture when missing; set a name filter to
avoid this setup step. Dynamic tests are enabled when `rootfs/lib` and
the corresponding guest binaries are present. SDL/GL tests report a skip
when the host display or required APIs are unavailable.

`--strict` makes a missing selected guest binary a failure. Use
`--allow-missing REGEX` only for named external fixture bundles that are
intentionally optional; each such omission is printed as a skip. Exit 77
remains an environment skip for display/driver tests, SDL tests in a build
without host SDL2, and host scripts whose fetched fixtures or toolchains are
absent; a missing emulator stays a failure. JIT-only cases remain skipped
under `--no-jit`. Host harnesses that link `libbifrost.a` build and link the library of the
configuration under test (`Scripts/Testing/host_library.sh`: the build directory of `BIFROST_EMU`,
headless when run by hand), which the job has already compiled. A failing test's output shows its first
20 lines, the later lines that name a failure or an error, and its last 10. CI uses strict mode so fixture compilation
failures cannot silently reduce test coverage.

For repeated performance runs, use `Scripts/Bench/run_benchmarks.py`. It performs
warmups, validates result markers, measures host wall time, and saves host,
compiler, emulator, environment, and sample metadata to JSON:

```bash
make USE_SDL2=0 USE_THUNK_GL=0
make setup-tests
./Scripts/Bench/run_benchmarks.py --emulator build/release-sdl0-gl0/bifrost-emu \
  --warmups 2 --runs 7 --mode both
```

CoreMark is added to the default benchmark set when `Tests/Unit/coremark.elf` is
available; it can also be selected explicitly with `--bench coremark`. Its
four CRCs are checked. The CoreMark source is not bundled with the repo.

## Adding a test

1. Add a self-checking AArch64 source under `Tests/Unit/` or `Tests/Apps/`.
2. Add its `.elf` path and expected output pattern to the matching array
   in `Scripts/Testing/run_tests.sh`.
3. Build it with `make cross SRC=path/to/test.c OUT=path/to/test.elf`.
4. Run the focused category and relevant JIT/interpreter modes. Keep tests
   bounded; the runner enforces a per-test timeout and kills the process
   group if it hangs.
5. Update this inventory only when category definitions or setup
   requirements change.

A fixture must not depend on the machine that built or runs it. CI builds with the pinned Bootlin toolchain (GCC 13, musl
1.2.5, `fetch-musl-toolchain.sh --bootlin`) under an account that is not UID 1000; a build with `musl.cc`'s GCC 11 and older musl,
run as UID 1000, hid all three of these:

- Inline asm declares every register it writes, `v8`-`v15` included: the compiler keeps values there across the block.
  Check a recorded hash against more than QEMU, since QEMU reproduces the fixture's own mistake.
- `clone()` in musl 1.2.5 refuses `CLONE_THREAD`, `CLONE_SETTLS` and `CLONE_CHILD_CLEARTID` with `EINVAL`, without a
  syscall. Use `svc` for a raw thread.
- `getuid()` is the host account unless `--android-runtime` is on, while Binder's sender UID is 0 without it. Do not
  compare one with the other.
- A check of something the host may lack exits 77, and only for that cause: a missing Vulkan driver, an SDL video driver
  without Vulkan surfaces or a missing window backend skips; a failure on a host that has them still fails. Do not
  require variables such as `SHELL` or `TERM`.

CI's scheduled and manual `make check-all` runs on Ubuntu 24.04 (glibc 2.39, SDL 2.30, a toolchain linking `--as-needed`)
with no display, GPU or sound, so it uses Mesa's software GL and Vulkan. Every fixture it needs is built from git, so a
stray local `.elf` cannot stand in for a missing source. A run here proves little about it: test a clean clone with
Bootlin's toolchain, as a UID other than 1000, on that system. `xwayland_wire` (needs X11 in the rootfs) and
`rw_iperf3_version`/`rw_coreutils_*` are its optional fixtures.


