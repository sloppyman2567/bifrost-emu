# Tests

The test runner is [`scripts/run_tests.sh`](../scripts/run_tests.sh). It does
not build guest fixtures; use `make setup-tests` for the C and assembly
fixtures, or `make cross SRC=... OUT=...` for one C test. Missing fixtures are
skipped by default and fail with `--strict`. Some integration tests also
require SDL2, a display, or a configured guest rootfs.

## Test inventory

The fully provisioned suite contains 240 configured test runs:

| Category | Runs | Notes |
|---|---:|---|
| Unit | 64 | Focused instruction/JIT regressions in `ctest/` |
| Integration | 85 | Guest programs and subsystem checks in `ctest_real/` and `test/` |
| Sandbox | 1 | `BIFROST_ROOT` path-boundary regression |
| Toybox | 9 | Commands run through the committed AArch64 Toybox binary |
| Real-world static | 49 | BusyBox and Toybox command checks; BusyBox may be fetched by the runner |
| Real-world dynamic (glibc) | 7 | Part of the real-world fixture set; requires rootfs libraries |
| Dynamic | 15 | Dynamically linked musl/glibc tests; requires rootfs and built fixtures |
| Benchmarks | 5 | Performance smoke benchmarks; omitted by `--quick` |
| Interactive | 5 | Stdin-driven programs run with scripted input |
| **Full configured suite** | **240** | Includes the sandbox and rootfs-dependent dynamic runs |

Without a configured rootfs, the 7 dynamic real-world runs and 15 dynamic
tests are not selected, for 218 configured runs (213 with `--quick`). These
figures describe test definitions selected by the runner; missing guest
fixtures and unavailable display/SDL support can result in skips.

## Memory and signal regressions

`ctest/test_memory_permissions.c` checks existing read-only protections,
permissions and unmapped holes inherited across fork, isolation of child
repairs, syscall `EFAULT`, and brk shrink/regrowth/collision behavior. Fault
probes check the guest PC and bound retries. Page sizes come from `sysconf`;
LSE checks run only when advertised by `AT_HWCAP`.

`ctest/test_sigreturn_context.c` checks handler edits to registers, PC, SP,
NZCV, FP/SIMD state and signal masks, nested alternate stacks, and nine
malformed signal-frame cases. These exercise the Linux AArch64 ABI; they
are not a complete Arm architecture conformance suite.

For independent QEMU runs, see the
[reference signal-mask patch](../tools/qemu-reference/README.md). It fixes
QEMU's handling of unblockable mask bits while keeping the Linux assertions
strict.

`ctest/test_vm_signal_contracts.c` checks fixed `PROT_NONE` mapping contents,
file-offset preservation with concurrent reads during `mmap`, `mremap` move
permission and fixed destinations, and alternate-stack changes during a
handler. Its 47 checks also pass on native Linux. QEMU 11.1.50 disagrees on
the zero-size `mremap` errno (`ENOMEM` instead of Linux's `EINVAL`); that
assertion remains strict.

## zstd, shift, and bitfield regressions

`ctest/jit_variable_shift_source.c` checks that all four variable shifts and
rotates preserve their count source, including packed high bits, at both
32-bit and 64-bit widths. `ctest/jit_shifted_operand_width.c` checks W-form
shifted operands with nonzero upper X bits and block-local constant folding.
Build these sources with `make setup-tests` or `make cross`.

`ctest/jit_bitfield_regalloc.c` checks bitfield results and live source values
under register pressure, memory stores/reloads, and Tier-2 side exits.

`scripts/run_zstd_regression.sh` uses the bundled static AArch64 zstd fixture
and requires the host `zstd` CLI. It compresses a deterministic corpus using
both JIT and interpreter, with default workers and `--single-thread`; host
zstd validates each archive and restores bytes for comparison. Both guest
engines also decode a host-generated archive. The main suite runs this host
harness once through `HOST_JIT`, and skips it under `--no-jit` because the
harness already exercises both engines.

```bash
./scripts/run_tests.sh --filter 'variable_shift_source|shifted_operand_width|zstd'
./scripts/run_tests.sh --no-jit --filter 'variable_shift_source|shifted_operand_width|bitfield_regalloc'
./scripts/run_zstd_regression.sh
```

The full working-tree suite passed 240/240 on 2026-10-01 with
`BIFROST_IR_VALIDATE=1` and `BIFROST_REGALLOC_CHECK=1`; that run included the
bitfield regression described above. Both focused shift tests
also passed JIT verification and memory verification. Applying those settings
to threaded zstd logged divergences and exceeded its 90-second compression
timeout, so that diagnostic run remains unresolved. See [zstd.md](zstd.md)
for the two fixes, fixture scope, and verification limits.

## Running tests

```bash
make check                 # standard JIT suite
make check-quick           # JIT suite without the five benchmarks
make check-nojit           # quick suite using the interpreter
make check-fwd              # quick suite with BIFROST_ENABLE_FWD=1
make ci                    # guest-correctness gate including sandbox and glibc dynamic checks
make verify                # strict JIT/interpreter differential gate

./scripts/run_tests.sh --unit                 # focused JIT regressions
./scripts/run_tests.sh --unit --strict        # fail if a selected fixture is missing
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

`--strict` makes a missing selected guest binary a failure. Use
`--allow-missing REGEX` only for named external fixture bundles that are
intentionally optional; each such omission is printed as a skip. Exit 77
remains an environment skip for display/driver tests, and JIT-only cases
remain skipped under `--no-jit`. CI uses strict mode so fixture compilation
failures cannot silently reduce test coverage.

For repeated performance runs, use `scripts/run_benchmarks.py`. It performs
warmups, validates result markers, measures host wall time, and saves host,
compiler, emulator, environment, and sample metadata to JSON:

```bash
make USE_SDL2=0 USE_THUNK_GL=0
make setup-tests
./scripts/run_benchmarks.py --emulator build/release-sdl0-gl0/bifrost-emu \
  --warmups 2 --runs 7 --mode both
```

CoreMark is added to the default benchmark set when `ctest/coremark.elf` is
available; it can also be selected explicitly with `--bench coremark`. Its
four CRCs are checked. The CoreMark source is not bundled with the repo.

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
