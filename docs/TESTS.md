# Tests

The test runner is [`scripts/run_tests.sh`](../scripts/run_tests.sh). It
does not build guest fixtures; compile missing C tests with `make setup-tests`
or `make cross SRC=... OUT=...`. The runner reports absent fixtures as
skipped. Some integration tests also require SDL2, a display, or a configured
guest rootfs.

## Test inventory

The fully provisioned suite contains 229 test runs:

| Category | Runs | Notes |
|---|---:|---|
| Unit | 55 | Focused instruction/JIT regressions in `ctest/` |
| Integration | 84 | Guest programs and subsystem checks in `ctest_real/` and `test/` |
| Toybox | 9 | Commands run through the committed AArch64 Toybox binary |
| Real-world static | 49 | BusyBox and Toybox command checks; BusyBox may be fetched by the runner |
| Real-world dynamic (glibc) | 7 | Part of the real-world fixture set; requires rootfs libraries |
| Dynamic | 15 | Dynamically linked musl/glibc tests; requires rootfs and built fixtures |
| Benchmarks | 5 | Performance smoke benchmarks; omitted by `--quick` |
| Interactive | 5 | Stdin-driven programs run with scripted input |
| **Full configured suite** | **229** | Includes the rootfs-dependent dynamic runs |

Without a configured rootfs, the 7 dynamic real-world runs and 15 dynamic
tests are not selected, for 207 configured runs (202 with `--quick`). These
figures describe test definitions selected by the runner; missing guest
fixtures and unavailable display/SDL support can result in skips.

## Running tests

```bash
make check                 # standard JIT suite
make check-quick           # JIT suite without the five benchmarks
make check-nojit           # quick suite using the interpreter
make check-fwd              # quick suite with BIFROST_ENABLE_FWD=1

./scripts/run_tests.sh --unit                 # focused JIT regressions
./scripts/run_tests.sh --integration          # integration tests
./scripts/run_tests.sh --dynamic              # dynamic-linker tests
./scripts/run_tests.sh --filter 'sig|brk|pipe' # filter test names
./scripts/run_tests.sh --no-jit --quick       # interpreter quick suite
./scripts/run_tests.sh --help                 # all runner options
```

`make check-nojit` and `make check-fwd` use quick mode. To include
benchmarks in either mode, invoke the script directly with `--no-jit` or
`--fwd` and omit `--quick`.

The default runner includes interactive and real-world categories. It
attempts to fetch the BusyBox fixture when missing; set a name filter to
avoid this setup step. Dynamic tests are enabled when `rootfs/lib` and
the corresponding guest binaries are present. SDL/GL tests report a skip
when the host display or required APIs are unavailable.

## Adding a test

1. Add a self-checking AArch64 source under `ctest/` or `ctest_real/`.
2. Add its `.elf` path and expected output pattern to the matching array
   in `scripts/run_tests.sh`.
3. Build it with `make cross SRC=path/to/test.c OUT=path/to/test.elf`.
4. Run the focused category and relevant JIT/interpreter modes. Keep tests
   bounded; the runner enforces a per-test timeout and kills the process
   group if it hangs.
5. Update this inventory only when category definitions or setup
   requirements change.
