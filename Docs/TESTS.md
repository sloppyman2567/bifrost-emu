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

The fully provisioned suite is 361 runs under `run_tests.sh --strict` (2026-10-24), in these categories: unit
(focused guest instruction and JIT regressions and host gates), integration (guest programs in `Tests/Apps/` and
`Tests/Asm/`), sandbox, per-thread JIT, interactive (scripted stdin), toybox, real-world static and dynamic programs,
dynamic linking (musl/glibc and XWayland; needs a rootfs) and benchmarks (omitted by `--quick`). The runner prints
each category's count when it starts, and `--filter REGEX` runs only the tests whose names match. Without a configured rootfs the
dynamic runs are not selected; missing guest fixtures and unavailable display or SDL support appear as skips, never as passes.

## Running tests

Additional standalone regressions build their own fixtures in temporary
directories and exercise the current emulator (`BIFROST_EMU` can override it):

```sh
./Scripts/Testing/test_rootfs_discovery.sh  # dynamic programs find a rootfs without flags
./Scripts/Testing/test_rootfs_install.sh    # install-rootfs.sh output runs from an installed layout
./Scripts/Testing/test_guest_cwd.sh         # getcwd/cd follow the host cwd without a sandbox
./Scripts/Android/test_android_super.py        # dynamic-partition unpacker vs a synthetic image (--compare SUPER.img: vs lpunpack)
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


