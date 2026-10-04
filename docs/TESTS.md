# Tests

The test runner is [`scripts/run_tests.sh`](../scripts/run_tests.sh). It does
not build guest fixtures; use `make setup-tests` for the C and assembly
fixtures, or `make cross SRC=... OUT=...` for one C test. Missing fixtures are
skipped by default and fail with `--strict`. An ELF older than its matching
C or assembly source fails with a rebuild instruction, so ignored local
fixtures cannot silently test outdated code. Some integration tests also
require SDL2, a display, or a configured guest rootfs.

## Latest full validation

The final strict suite passes **257/257 in JIT mode** with no skips and
**254/254 in interpreter mode**, with three JIT-only skips. Both runs used
`BIFROST_IR_VALIDATE=1` and desktop/audio access. The JIT inventory now contains
80 unit tests. These counts supersede the earlier 256/253 full-suite results.

The retained Doom gap harness passes 20,000 randomized generated-code checks
for 625 captured encodings, with zero mismatches and zero explicit IR
fallbacks. The fixed-oracle guest passes 2,054 checks in QEMU, interpreter and
JIT with register/memory verification and no divergence logs. SDL thread
lifecycle/nonlocal-return checks pass separately in both modes. The complete
fallback-profiler and window-stats regressions also pass independently.

Restricted sandbox runs that cannot initialize desktop/audio/Vulkan resources
do not replace these integration checks. Rebuild stale guest fixtures before
interpreting failures as emulator regressions.

### Fresh-fixture corrections (2026-10-04)

Rebuilding the freestanding assembly fixtures exposed invalid memory-permission
assumptions in `count`, `fib_basic` and `repl`: writable buffers had been placed
in a read/execute text segment. They now reside in `.data`; the REPL buffer
also matches its 63-character capacity, and count prints the documented 1–5.
All three pass in QEMU and both emulator engines. The builder publishes from
a temporary directory beside the fixtures, avoiding cross-filesystem rename
failures when `/tmp` is on another mount.

The extra histogram fixture used halfword loads from a byte array, producing
out-of-bounds count-table indices. Its input now uses halfwords and checks the
complete table against an expected histogram; QEMU, interpreter and JIT with
register/memory verification agree on hash `0x0d693c2a`.

### Verification gate repair (2026-10-04)

The earlier concurrent replay warnings were reproduced. The verifier restored
shared guest memory and patched live code while other guest CPUs could execute;
this is unsafe and cannot provide a sound differential comparison. Replay now
stops at the sticky multithread transition, with an explicit diagnostic.

`make verify` passes **56 single-thread differential fixtures plus four threaded
oracle workloads**. Cache invalidation, pressure, dispatch and call-helper
fixtures have single-thread modes for differential checks; the gate also runs
the original concurrent workloads at their original loop counts. Single-thread
checks force the BL/BLR helper path. Cache pressure has a 120-second replay
budget; other differential fixtures have 30 seconds, and threaded oracles 60.
These are separate forms of coverage, not multithread differential verification.

The gate discovers fixtures from source, rejects missing/stale ELFs and any
reported PC/register/memory divergence or suspended replay, and preserves
emulator/timeout exit status. `scripts/test_verify_gate.py` exercises failure
propagation and runs in CI. Follow-up headless units pass **80/80 JIT** and
**78/78 interpreter**, with two explicit JIT-only skips. Host API checks pass
**55/55 C API** and **61/61 native bridge**.

Both headless and SDL/GL configurations build. The headless Android surface
probe requires SDL and fails without it; activity and audio probes pass there.
After approval review recovered, the desktop Android surface/activity/audio
probes passed **3/3 in both JIT and interpreter modes** with the SDL/GL build.
CI's host API step now uses the same system AArch64 compiler as fixture setup;
headless compilation no longer depends on indirect SDL math includes or
unconditionally linked Android SDL event/window functions.

The host C API probe likewise assumed an executable guest stack. It now loads
`ctest/capi_testlib.so` for integer, FP and syscall call probes, preserving the
non-executable-stack contract. `make test-capi` passes 55/55 checks;
`make test-nb` passes 61/61. These host API tests are separate from the main
257-run guest inventory.

Android surface, activity/looper and audio integration probes pass in both
engines. This verifies those existing native Android contracts, not integrated
APK launch, a complete Android framework, or ART runtime execution.

## Test inventory

Original regression sources ship under Bifrost's Unlicense. Generated guest
ELFs are rebuilt locally; included and downloaded third-party fixtures keep
their upstream licenses. See [fixture distribution](THIRD_PARTY.md) for the
source/binary distinction and game-data boundaries.

The `fcsel_high_regs` regression has 61 checks for high FCSEL source registers,
FP destination 31, single/double precision, aliasing, GPR preservation, and
discarded FP-to-GPR writes to XZR. It passes under QEMU, JIT, and interpreter;
see [the vkQuake reproduction](vkquake.md).

The fully provisioned suite contains 257 configured test runs:

| Category | Runs | Notes |
|---|---:|---|
| Unit | 80 | Focused instruction/JIT regressions in `ctest/` |
| Integration | 86 | Guest programs and subsystem checks in `ctest_real/` and `test/` |
| Sandbox | 1 | `BIFROST_ROOT` path-boundary regression |
| Toybox | 9 | Commands run through the committed AArch64 Toybox binary |
| Real-world static | 49 | BusyBox and Toybox command checks; BusyBox may be fetched by the runner |
| Real-world dynamic (glibc) | 7 | Part of the real-world fixture set; requires rootfs libraries |
| Dynamic | 15 | Dynamically linked musl/glibc tests; requires rootfs and built fixtures |
| Benchmarks | 5 | Performance smoke benchmarks; omitted by `--quick` |
| Interactive | 5 | Stdin-driven programs run with scripted input |
| **Full configured suite** | **257** | Includes the sandbox and rootfs-dependent dynamic runs |

Without a configured rootfs, the 7 dynamic real-world runs and 15 dynamic
tests are not selected, for 235 configured runs (230 with `--quick`). These
figures describe test definitions selected by the runner; missing guest
fixtures and unavailable display/SDL support can result in skips.

## Dispatch cache and profiling regression (2026-10-04)

`jit_dispatch_cache` is registered in the main suite. It checks exact results
for indirect calls across three guest threads, code replacement after workers
join, and data-only mmap/munmap churn. QEMU and both engines agree. The bounded
cache-pressure script also passes at 1 MiB and 64 MiB, including cached
fallback decisions and logical growth. SDL thread lifecycle/nonlocal-return
checks pass in both engines.

`bash scripts/test_jit_profiler.sh` passes the independent host regression for
per-thread/JIT-owner sampler registration and the full reserved growth range.
It uses deterministic range checks rather than statistical sample thresholds
and is not another guest-inventory entry. Optional dispatch statistics expose
both cache ways and table lookups; see [JIT.md](JIT.md).

### Guest-call helpers

`jit_call_helpers` checks nested arithmetic calls and callee-written NZCV
across three threads; QEMU, JIT and interpreter agree. A single-thread
helper-forced run (`1000 single`, `BIFROST_NO_DIRECT_CALL=1`) passes register
and memory verification with no divergence reports. Concurrent verification
reports library replay divergences also reproduced by the baseline, so it is
not claimed as a clean differential run.

After the final duplicate-PC-store removal, the complete JIT unit inventory
passes 80/80 and SDL callback/nonlocal returns pass again. The full suites
above passed immediately before that last narrowing; the retained native Doom
harness passes 20,000 checks with no mismatches on the final build.

## Doom JIT, SDL threads and diagnostics

The following focused tests accompany the native instruction and SDL worker
changes. Standalone host/SDL probes are not additional entries in the 257-run
inventory unless explicitly registered in `run_tests.sh`.

- `ctest/jit_simd_fp_indexed.c`: 28 indexed FMUL lane, aliasing and upper-half
  checks, independently validated in QEMU and both engines.
- `ctest/jit_round_away.c`: 6,984 FRINTA/FCVTAS fixed-oracle cases for ties,
  adjacent values, signed zero, NaNs and saturation.
- `ctest/jit_scalar_dup.c`: 60 scalar lane-copy cases covering all 30 valid
  lane/width combinations with and without source/destination aliasing.
- `ctest/jit_doom_gap.c`: 2,054 oracle checks, including all conditional
  compare conditions/NZCV inputs in single/double precision, FCVTL, SHLL,
  ADDV result width and fused indexed FMLA.
- `ctest/native_doom_gaps.cpp` with `ctest/doom_gap_opcodes.inc`: actual JIT
  execution for all 625 captured gap encodings, 32 randomized states each.
  Checks registers, flags, memory, post-index writeback and exclusive-monitor
  success/failure. Interpreter comparison complements the independent fixed
  oracles; it is not itself a hardware oracle. Also passes with
  `BIFROST_TIER2_HITS=1` to exercise early promotion decisions.
- `ctest_real/test_sdl_thread.c`: create/wait, eight concurrent workers,
  detach, and 16 nested `setjmp`/`longjmp` returns. Tests both the JIT SDL
  runner and the explicit `--no-jit` path.
- `scripts/test_fallback_profile.sh`: exact concurrent snapshot retention
  after worker exit, with 192 complete entries and 192,000 counts.
- `scripts/test_window_stats.sh`: presentation clock, default rendering log,
  title/idle FPS and enable/disable controls. UI placement, compositor stacking
  and overlay capture require separate desktop checks; see
  [window-stats limits](window-stats.md).

```bash
make -j8 USE_SDL2=1 USE_THUNK_GL=1 build/release-sdl1-gl1/bifrost-emu
make cross SRC=ctest/jit_doom_gap.c OUT=ctest/jit_doom_gap.elf
make cross SRC=ctest_real/test_sdl_thread.c OUT=ctest_real/test_sdl_thread.elf
BIFROST_IR_VALIDATE=1 scripts/run_tests.sh --emu build/release-sdl1-gl1/bifrost-emu --strict
BIFROST_IR_VALIDATE=1 scripts/run_tests.sh --emu build/release-sdl1-gl1/bifrost-emu --no-jit --strict
bash scripts/test_native_doom_gaps.sh
BIFROST_JIT_VERIFY=1 BIFROST_JIT_VERIFY_MEM=1 BIFROST_IR_VALIDATE=1 build/release-sdl1-gl1/bifrost-emu ctest/jit_doom_gap.elf
build/release-sdl1-gl1/bifrost-emu ctest_real/test_sdl_thread.elf
build/release-sdl1-gl1/bifrost-emu --no-jit ctest_real/test_sdl_thread.elf
bash scripts/test_fallback_profile.sh
EMU=build/release-sdl1-gl1/bifrost-emu bash scripts/test_window_stats.sh
```

The standalone captured-opcode harness links the built SDL/GL objects and
expects that configuration. Build all registered guest fixtures with
`make setup-tests` before running the complete suite on a new checkout.

`ctest/bench_doom_vector_fp.c` and `ctest/bench_doom_round_lane.c` are optional
instruction-throughput probes, not full-game FPS benchmarks. See
[Doom 3 validation](doom3.md) for measured scope and the longer user gameplay
run. Neither the tests nor the recorded presentation counter establish the
planned v2.0 60+ FPS target across demanding gameplay scenes.

## Game compatibility follow-up (2026-10-03)

`ctest/jit_shift_register.c` adds 182 SSHL/USHL checks for signed low-byte
counts, logical/arithmetic right shifts, lane widths, Q forms, aliasing, and
out-of-range counts. It passes under QEMU and both emulator engines.
The six selected shift/permute regressions also pass in both engines.

`scripts/run_thunk_compat.sh` exercises SDL event-filter registration,
accept/reject returns, nested callbacks, signed mouse deltas, and exact-size
output bounces, alongside GL ABI and Vulkan mapped-memory upload/readback
checks. These focused tests do not establish correct interactive vkQuake
sensitivity; see [vkQuake status](vkquake.md).

## Memory and signal regressions

`ctest/test_mem_guard.c` checks wrapping `munmap`, stack overlap rejection,
bounded huge `madvise`, and sequential above-window fixed mapping cycles.
On 2026-10-02, the failing local ELF still contained the older threaded
implementation. Rebuilding the current source passed 40/40 runs in each of
JIT and interpreter mode; this does not establish correctness of that older
threaded workload.

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

`ctest/jit_memory_guard_address.c` exercises cached constant addresses across
permission guards, stack saves, pointer loads, and a conditional exit. It
checks the value after repeated calls, including JIT verification and the
interpreter.

## SDL fullscreen regression

`ctest_real/test_sdl_fullscreen.c` checks SDL2 desktop/current display-mode
status returns and caller-owned output structs, invalid display indices,
three fullscreen/windowed recreation cycles, fullscreen flag transitions,
restored dimensions, and resize-event payloads. It exercises the sequence used
by Neverball to save its dimensions. Run it on a desktop, or use SDL's dummy
video driver for a bounded headless check:

```bash
make cross SRC=ctest_real/test_sdl_fullscreen.c OUT=ctest_real/test_sdl_fullscreen.elf
SDL_VIDEODRIVER=dummy ./scripts/run_tests.sh --filter '^sdl_fullscreen$'
SDL_VIDEODRIVER=dummy ./scripts/run_tests.sh --no-jit --filter '^sdl_fullscreen$'
```

The regression creates hidden windows so compositor placement rules and an
interactive game do not interfere with its requested-size assertions.
It returns exit 77 when SDL video cannot initialize. The pre-fix build fails
because the mode-query thunk does not fill the guest's dimensions. The repaired
build passes with the dummy driver and real desktop in both execution modes.
See [neverball.md](neverball.md) for the root cause and current interactive status.

## JPEG SIMD narrowing regression

`ctest/jit_simd_sat_narrow.c` covers the six signed/unsigned saturating narrowing
shift forms and nonsaturating RSHRN, each at 16→8, 32→16, and 64→32 bits.
Its 672 checks cover minimum/maximum shift amounts, both destination halves,
source/destination aliasing, rounding overflow, and sticky FPSR.QC.

```bash
make cross SRC=ctest/jit_simd_sat_narrow.c OUT=ctest/jit_simd_sat_narrow.elf
./scripts/run_tests.sh --filter '^simd_sat_narrow$'
./scripts/run_tests.sh --no-jit --filter '^simd_sat_narrow$'
qemu-aarch64 ctest/jit_simd_sat_narrow.elf
```

On 2026-10-02, all 672 checks passed under QEMU, JIT with IR validation and
register-allocation checks, and interpreter. The saved pre-fix interpreter
failed 632 checks. The related SIMD suite passed 19/19 in each Bifrost mode.
Direct libjpeg decodes of four Neverball JPEGs, including the hard-set preview
and two Help images, matched QEMU's raw RGB output byte-for-byte in both modes.
The affected JIT narrowing family now uses the corrected interpreter handler.
See [neverball.md](neverball.md) for the diagnosis and image coverage.

## Stability follow-up: 2026-10-02

At the October 2 checkpoint, the working-tree quick suite passed 238/238
in JIT mode and 236/236
in interpreter mode, with two intentional interpreter skips (`tier2_smov`
and the dual-engine `zstd_compression` host harness). Four stale fixtures were
rebuilt before the clean full JIT rerun. `test_mem_guard` separately passed
100 consecutive runs in each mode with no failures; its high-address mapping
coverage is single-threaded. See [neverball.md](neverball.md) for the associated
interactive fullscreen mouse confirmation and coverage limits.

Before the compatibility work was committed on the same date, the quick
suite was rerun with the added FCSEL regression: JIT passed 239/239 and
interpreter passed 237/237 with the same two expected skips. Standalone late
TLS and late-TLS pthread/edge scripts passed in both modes; SDL/GL/Vulkan
thunk compatibility probes passed in both modes, and the default JIT SDL
lifecycle soak passed 64 cycles. These checks use the current SDL2/GL build.

## Running tests

Additional standalone regressions build their own fixtures in temporary
directories and exercise the current emulator (`BIFROST_EMU` can override it):

```sh
./scripts/test_late_tls.sh          # late GD/TLSDESC and rejected initial-exec
./scripts/test_late_tls_pthread.sh  # existing/new threads, reuse, pointer/alignment/weak cases
./scripts/run_thunk_compat.sh       # SDL/GL/Vulkan ABI probes in both modes
./scripts/run_thunk_soak.sh 64      # repeated SDL window/mapping/thread lifecycle
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
