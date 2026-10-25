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

The fully provisioned suite was 361 runs under `run_tests.sh --strict` (2026-10-24; `jit_generations` adds one), in these categories: unit
(focused guest instruction and JIT regressions and host gates), integration (guest programs in `Tests/Apps/` and
`Tests/Asm/`), sandbox, per-thread JIT, interactive (scripted stdin), toybox, real-world static and dynamic programs,
dynamic linking (musl/glibc and XWayland; needs a rootfs) and benchmarks (omitted by `--quick`). The runner prints
each category's count when it starts, and `--filter REGEX` runs only the tests whose names match. Without a configured rootfs the
dynamic runs are not selected; missing guest fixtures and unavailable display or SDL support appear as skips, never as passes.

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
./Scripts/Android/test_download_progress.py    # asgard-setup's download progress lines (terminal and piped), injected clock
./Scripts/Android/test_asgard_stale_broker.py  # a broker an upgrade left running: restarted when idle, refused when busy; realm stop --force
make test-android-wheel-gestures               # wheel/pinch/middle-drag as two-finger gestures (SDL-free); test-android-sdl-backend checks them through the backend
./Scripts/Testing/test_window_stats.sh         # window stats modes, overlay and sensors (SDL probes open windows unless SDL_VIDEODRIVER=offscreen)
./Scripts/Testing/test_rootfs_profile.sh   # named rootfs profiles: install, list, select, errors, uninstall
./Scripts/Testing/test_setup_rootfs.sh     # bifrost-emu --setup-rootfs installs, uninstalls, runs a packaged installer from the cache and reports a missing one
./Scripts/Testing/test_guest_env.sh         # the guest gets the caller's environment (curated one in a sandbox)
./Scripts/Testing/test_exec_jit.sh          # programs started by execve run under the JIT
./Scripts/Testing/test_apk_launch.sh        # bifrost-emu app.apk hands the package to Asgard (stub)
./Scripts/Testing/test_asgard_install.py    # install-asgard.py install, update and removal (fake checkout)
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
remains an environment skip for display/driver tests, and JIT-only cases
remain skipped under `--no-jit`. CI uses strict mode so fixture compilation
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


