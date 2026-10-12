#!/usr/bin/env bash
# run_tests.sh — bifrost-emu test runner
#
# A clean, colorized test runner that:
#   - Categorizes tests (unit / integration / interactive / toybox /
#     real-world / dynamic / bench)
#   - Detects pass/fail via exit code + output keyword scan
#   - Prints a summary table with counts and timing
#   - Supports filtering by category (--unit, --toybox, etc.)
#   - Supports running under interpreter (--no-jit) or FWD (--fwd)
#
# Usage:
#   ./Scripts/Testing/run_tests.sh              # run the standard JIT suite
#   ./Scripts/Testing/run_tests.sh --unit       # only unit tests (Tests/Unit/)
#   ./Scripts/Testing/run_tests.sh --toybox     # only toybox integration tests
#   ./Scripts/Testing/run_tests.sh --no-jit     # run under interpreter
#   ./Scripts/Testing/run_tests.sh --fwd        # run with BIFROST_ENABLE_FWD=1
#   ./Scripts/Testing/run_tests.sh --no-chain-skip # run with BIFROST_NO_CHAIN_SKIP=1
#   ./Scripts/Testing/run_tests.sh --verbose    # show full output of each test
#   ./Scripts/Testing/run_tests.sh --filter foo # only run tests matching "foo"
#   ./Scripts/Testing/run_tests.sh --filter 'sig|brk' # filter without downloading fixtures
#   ./Scripts/Testing/run_tests.sh --quick      # skip benchmarks
#   ./Scripts/Testing/run_tests.sh --strict     # missing selected fixtures are failures
#   ./Scripts/Testing/run_tests.sh --allow-missing REGEX # explicit optional fixture exception
#   ./Scripts/Testing/run_tests.sh --emu PATH   # use an emulator build at PATH
#
# Fully provisioned suite (303 selected test runs):
#   Unit        122  — focused guest and host JIT regressions
#   Integration  87  — Tests/Apps/*.elf + Tests/Asm/*.elf
#   Sandbox       1  — BIFROST_ROOT path-boundary regression
#   Per-thread JIT 1 — cross-thread invalidation without the shared JIT
#   Toybox        9  — Tests/Apps/toybox subcommands
#   Real-world   56  — static BusyBox/Toybox commands
#   Real-world dynamic 7 — glibc binaries; rootfs required
#   Dynamic      17  — musl/glibc and XWayland tests; rootfs required
#   Benchmarks    5  — omitted with --quick
#   Interactive   5  — stdin-driven tests
# Rootfs-dependent tests require a rootfs; --strict rejects missing fixtures.
#
# Exit code: 0 if all tests pass, 1 if any fail.

set -u

TEMP_FILES=()
TEMP_DIRS=()
cleanup_temp_files() {
    local path
    for path in "${TEMP_FILES[@]}"; do
        [ -n "$path" ] && rm -f -- "$path"
    done
    for path in "${TEMP_DIRS[@]}"; do
        [ -n "$path" ] && rm -rf -- "$path"
    done
}
trap cleanup_temp_files EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ── Config ─────────────────────────────────────────────────────────────
EMU="${BIFROST_EMU:-./bifrost-emu}"
TIMEOUT=15
TIMEOUT_LONG=60
STRICT=0
ALLOW_MISSING=()

# Colors (disabled if not a TTY)
if [ -t 1 ]; then
    C_RED='\033[0;31m'; C_GRN='\033[0;32m'; C_YLW='\033[0;33m'
    C_BLU='\033[0;34m'; C_DIM='\033[2m'; C_RST='\033[0m'
    C_BOLD='\033[1m'
else
    C_RED=''; C_GRN=''; C_YLW=''; C_BLU=''; C_DIM=''; C_RST=''; C_BOLD=''
fi

# ── Parse args ─────────────────────────────────────────────────────────
RUN_UNIT=0
RUN_INTEGRATION=0
RUN_INTERACTIVE=0
RUN_TOYBOX=0
RUN_BENCH=0
RUN_REALWORLD=0
RUN_DYNAMIC=0
RUN_ALL=1
VERBOSE=0
QUICK=0
FILTER=""
EMU_FLAGS=()
RUN_ENV=()

while [ $# -gt 0 ]; do
    case "$1" in
        --unit)         RUN_UNIT=1; RUN_ALL=0 ;;
        --integration)  RUN_INTEGRATION=1; RUN_ALL=0 ;;
        --interactive)  RUN_INTERACTIVE=1; RUN_ALL=0 ;;
        --toybox)       RUN_TOYBOX=1; RUN_ALL=0 ;;
        --realworld)    RUN_REALWORLD=1; RUN_ALL=0 ;;
        --dynamic)      RUN_DYNAMIC=1; RUN_ALL=0 ;;
        --bench)        RUN_BENCH=1; RUN_ALL=0 ;;
        --no-jit)       EMU_FLAGS+=(--no-jit) ;;
        --fwd)          RUN_ENV+=(BIFROST_ENABLE_FWD=1) ;;
        --no-chain-skip) RUN_ENV+=(BIFROST_NO_CHAIN_SKIP=1) ;;
        --verbose|-v)   VERBOSE=1 ;;
        --quick)        QUICK=1 ;;
        --strict)       STRICT=1 ;;
        --allow-missing)
            if [ "$#" -lt 2 ] || [ -z "$2" ]; then
                echo "error: --allow-missing requires a non-empty regular expression" >&2
                exit 2
            fi
            ALLOW_MISSING+=("$2"); shift ;;
        --allow-missing=*)
            optional_pattern="${1#--allow-missing=}"
            if [ -z "$optional_pattern" ]; then
                echo "error: --allow-missing requires a non-empty regular expression" >&2
                exit 2
            fi
            ALLOW_MISSING+=("$optional_pattern") ;;
        --emu)
            if [ "$#" -lt 2 ] || [ -z "$2" ]; then
                echo "error: --emu requires an executable path" >&2
                exit 2
            fi
            EMU="$2"; shift ;;
        --filter)
            if [ "$#" -lt 2 ] || [ -z "$2" ]; then
                echo "error: --filter requires a non-empty regular expression" >&2
                exit 2
            fi
            FILTER="$2"; shift ;;
        --filter=*)     FILTER="${1#--filter=}" ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "Unknown option: $1 (try --help)"
            exit 1
            ;;
    esac
    shift
done

if [ -n "$FILTER" ]; then
    grep -E -- "$FILTER" </dev/null >/dev/null 2>&1
    grep_rc=$?
    if [ "$grep_rc" -eq 2 ]; then
        echo "error: invalid --filter regular expression: $FILTER" >&2
        exit 2
    fi
fi
for optional_pattern in "${ALLOW_MISSING[@]}"; do
    grep -E -- "$optional_pattern" </dev/null >/dev/null 2>&1
    grep_rc=$?
    if [ "$grep_rc" -eq 2 ]; then
        echo "error: invalid --allow-missing regular expression: $optional_pattern" >&2
        exit 2
    fi
done

# ── Real-world binary download ────────────────────────────────────────
# Downloads Alpine musl busybox (static AArch64) to Tests/Apps/realworld/.
# The toybox binary is already committed in the repo (Tests/Apps/toybox).
# The default full run (RUN_ALL=1) downloads these when missing and also
# runs the interactive tests, so a bare `run_tests.sh` exercises every
# available category. Rootfs-dependent categories are enabled when present.
#
# The download itself is bounded by a 90-second timeout — same cap as the
# toolchain fetch scripts — so a stalled Alpine mirror can't hang the
# test runner indefinitely.
DOWNLOAD_TIMEOUT=90
if [ "$RUN_ALL" = "1" ] && [ -z "$FILTER" ]; then
    echo -e "${C_BOLD}Downloading real-world binaries...${C_RST}"
    mkdir -p Tests/Apps/realworld

    # Alpine musl busybox (static AArch64, ~1.1 MB)
    if [ ! -f "Tests/Apps/realworld/busybox-aarch64" ]; then
        echo -n "  busybox-aarch64 (Alpine musl static)... "
        if curl -fL --connect-timeout 15 --max-time "$DOWNLOAD_TIMEOUT" --retry 1 \
                -o /tmp/bb.apk \
                "https://dl-cdn.alpinelinux.org/alpine/v3.19/main/aarch64/busybox-static-1.36.1-r21.apk" 2>/dev/null; then
            mkdir -p /tmp/bb_extract
            tar -xzf /tmp/bb.apk -C /tmp/bb_extract 2>/dev/null
            cp /tmp/bb_extract/bin/busybox.static Tests/Apps/realworld/busybox-aarch64
            chmod +x Tests/Apps/realworld/busybox-aarch64
            rm -rf /tmp/bb.apk /tmp/bb_extract
            echo -e "${C_GRN}OK${C_RST}"
        else
            echo -e "${C_RED}FAILED (download error)${C_RST}"
        fi
    else
        echo -e "  busybox-aarch64: ${C_DIM}already present${C_RST}"
    fi

    # Toybox (already committed in repo, just symlink it)
    if [ ! -f "Tests/Apps/realworld/toybox" ] && [ -f "Tests/Apps/toybox" ]; then
        ln -sf ../toybox Tests/Apps/realworld/toybox
    fi

    echo -e "${C_DIM}Download complete. Running full test suite...${C_RST}\n"
fi

if [ "$RUN_ALL" = "1" ]; then
    RUN_UNIT=1; RUN_INTEGRATION=1; RUN_TOYBOX=1
    RUN_INTERACTIVE=1
    RUN_REALWORLD=1
    [ "$QUICK" = "0" ] && RUN_BENCH=1
    # Dynamic tests require rootfs + toolchains; auto-enable if present.
    # Turn 72: also check for glibc libs (libc.so.6) so the glibc dynamic
    # test (test_dyn_write) runs when the glibc toolchain was fetched.
    if [ -d "rootfs/lib" ]; then
        if [ -f "Tests/Apps/hello_dyn_musl.elf" ] || [ -f "rootfs/lib/libc.so.6" ]; then
            RUN_DYNAMIC=1
        fi
    fi
fi

# ── Check emulator exists ──────────────────────────────────────────────
if [ ! -x "$EMU" ]; then
    echo -e "${C_RED}error: $EMU not found — run 'make' first${C_RST}"
    exit 1
fi

# ── Test definitions ───────────────────────────────────────────────────
# Each test: name | file | stdin | timeout | expected_pattern (regex)
#
# Test categories (Turn 73 standardization):
#   Unit        — Tests/Unit/*.elf, focused regression tests for specific JIT
#                 codegen paths. Fast (<5s each), deterministic.
#   Integration — Tests/Apps/*.elf, real-world test programs that exercise
#                 multiple subsystems (signals, threads, FS, memory).
#   Interactive — Tests/Asm/*.elf that need stdin input (echo, repl, sh, cat).
#                 Part of the standard suite (stdin is piped, timeout-guarded).
#   Toybox      — Tests/Apps/toybox with various subcommands.
#   Real-world  — Downloaded static AArch64 binaries (busybox, toybox, iperf2).
#                 Auto-downloaded when missing; skipped only if the download
#                 failed or the binaries are absent.
#   Dynamic     — Dynamically-linked test binaries (musl + glibc). Requires
#                 rootfs + toolchain. Skipped if rootfs not present.
#   Benchmarks  — Performance benchmarks. Included in standard suite;
#                 skipped with --quick.
#
# A test PASSES if:
#   - exit code is 0, AND
#   - output contains expected_pattern (or expected_pattern is empty)
# A test FAILS if output contains "FAIL" or "ERROR" (case-insensitive)
# and no "PASS"/"OK"/"ALL.*PASS" counterbalances it.

# Standard suite includes 57 unit, 84 integration, 1 sandbox, 9 Toybox, 5 interactive,
# 49 static real-world, 5 benchmark, and optional rootfs-dependent tests.

# Unit tests (Tests/Unit/ — focused JIT regression tests)
UNIT_TESTS=(
    "proc_exe|Tests/Unit/test_proc_exe.elf||5|test_proc_exe: ALL PASS"
    "mmap_hint|Tests/Unit/test_mmap_hint.elf||10|test_mmap_hint: ALL PASS"
    "hello|Tests/Unit/hello.elf||5"
    "addsub_imm|Tests/Unit/jit_addsub_imm.elf||5|PASS"
    "bitfield|Tests/Unit/jit_bitfield.elf||5|PASS"
    "block_split|Tests/Unit/jit_block_split.elf||5|PASS"
    "carry|Tests/Unit/jit_carry.elf||5|PASS"
    "bfext|Tests/Unit/jit_bfext.elf||5|PASS"
    "cls|Tests/Unit/jit_cls.elf||5|PASS"
    "csel|Tests/Unit/jit_csel.elf||5|PASS"
    "extend|Tests/Unit/jit_extend.elf||5|PASS"
    "fma|Tests/Unit/jit_fma.elf||5|PASS"
    "fp_scalar|Tests/Unit/jit_fp_scalar.elf||5|PASS"
    "int_fp_conv|Tests/Unit/jit_int_fp_conv.elf||5|ALL TESTS PASSED"
    "ldp_stp|Tests/Unit/jit_ldp_stp.elf||5|PASS"
    "madd|Tests/Unit/jit_madd.elf||5|PASS"
    "neon|Tests/Unit/jit_neon.elf||5|PASS"
    "neon_advanced|Tests/Unit/jit_neon_advanced.elf||5|PASS"
    "neon_cmp|Tests/Unit/jit_neon_cmp.elf||5|checks passed"
    "fp_compare_zero|Tests/Unit/jit_fp_compare_zero.elf||5|checks passed"
    "simd_mul32|Tests/Unit/jit_simd_mul32.elf||5|simd_mul32: ALL PASS"
    "simd_fmov_imm|Tests/Unit/jit_simd_fmov_imm.elf||5|simd_fmov_imm: ALL PASS"
    "scalar_dup|Tests/Unit/jit_scalar_dup.elf||5|scalar_dup: ALL PASS"
    "simd_unary_fp|Tests/Unit/jit_simd_unary_fp.elf||5|simd_unary_fp: ALL PASS"
    "simd_fp_indexed|Tests/Unit/jit_simd_fp_indexed.elf||5|simd_fp_indexed: ALL PASS"
    "round_away|Tests/Unit/jit_round_away.elf||5|round_away: ALL PASS"
    "doom_gap|Tests/Unit/jit_doom_gap.elf||10|doom_gap: ALL PASS"
    "jit_cache|Scripts/Testing/test_jit_cache.sh||65|jit_cache: ALL PASS|HOST_JIT"
    "jit_tier2_mutex|Scripts/Testing/test_jit_tier2_mutex.sh||180|jit_tier2_mutex: 20 promotion-stress runs PASS|HOST_JIT"
    "jit_tier2_discovery|Scripts/Testing/test_jit_tier2_discovery.sh||180|jit_tier2_discovery: direct edges, capped bodies, exits and invalidation PASS|HOST_JIT"
    "jit_tier2_feedback|Scripts/Testing/test_jit_tier2_feedback.sh||180|jit_tier2_feedback: neighboring regions, recoveries and concurrent phases PASS|HOST_JIT"
    "jit_tier2_measure|Scripts/Testing/test_jit_tier2_measure.sh||180|jit_tier2_measure: real timing keeps faster code, rejects slower/mismatched samples PASS|HOST_JIT"
    "jit_cache_invalidation|Tests/Unit/jit_cache_invalidation.elf||15|jit_cache_invalidation: ALL PASS"
    "jit_dispatch_cache|Tests/Unit/jit_dispatch_cache.elf||15|jit_dispatch_cache: ALL PASS"
    "jit_call_helpers|Tests/Unit/jit_call_helpers.elf||15|jit_call_helpers: ALL PASS"
    "jit_call_depth|Tests/Unit/jit_call_helpers.elf 20000 deep||15|jit_call_helpers: ALL PASS deep calls"
    "jit_mt_tier2|Tests/Unit/jit_mt_tier2.elf||30|jit_mt_tier2: ALL PASS"
    "shift_register|Tests/Unit/jit_shift_register.elf||5|checks passed"
    "neon_permute|Tests/Unit/jit_neon_permute.elf||5|checks passed"
    "neon_fixups|Tests/Unit/jit_neon_fixups.elf||5|checks passed"
    "neon_roundingshift|Tests/Unit/jit_neon_roundingshift.elf||5|checks passed"
    "neon_pngfix|Tests/Unit/jit_neon_pngfix.elf||5|checks passed"
    "mvni_softfloat|Tests/Unit/jit_mvni_softfloat.elf||5|checks passed"
    "rev|Tests/Unit/jit_rev.elf||5|PASS"
    "simd|Tests/Unit/jit_simd.elf||5|PASS"
    "simd_misc|Tests/Unit/jit_simd_misc.elf||5|checks passed"
    "simd_recip|Tests/Unit/jit_simd_recip.elf||10|45/45 checks passed"
    "simd_faddp|Tests/Unit/jit_simd_faddp.elf||5|14/14 checks passed"
    "simd_fixconv|Tests/Unit/jit_simd_fixconv.elf||5|35/35 checks passed"
    # Vector FP NaN/zero rules, including native exceptional exits.
    "simd_fp_nan|Tests/Unit/jit_simd_fp_nan.elf||10|checks passed"
    # msr nzcv reads its source vreg, not a cpu.regs slot the optimizer
    # skipped storing because the block overwrites the register next.
    "msr_nzcv|Tests/Unit/jit_msr_nzcv.elf||10|64000/64000 checks passed"
    "simd_addlv|Tests/Unit/jit_simd_addlv.elf||5|checks passed"
    "simd_pairlong|Tests/Unit/jit_simd_pairlong.elf||5|3072 checks passed"
    "simd_pmull|Tests/Unit/jit_simd_pmull.elf||15|6144 checks passed"
    "crc32_instructions|Tests/Unit/jit_crc32.elf||5|checks passed"
    "simd_sat_narrow|Tests/Unit/jit_simd_sat_narrow.elf||5|672 checks passed"
    "simd_sat|Tests/Unit/test_simd_sat.elf||5|ALL PASS"
    "fp_pw_elem|Tests/Unit/jit_fp_pw_elem.elf||5|ALL PASS"
    "loop|Tests/Unit/loop.elf||5|Loop value"
    "test_fb|Tests/Unit/test_fb.elf||5|mode:"
    "test_float|Tests/Unit/test_float.elf||5|3.14"
    "test_jit_native|Tests/Unit/test_jit_native.elf||10|ALL PASS"
    "test_malloc|Tests/Unit/test_malloc.elf||5|malloc test done"
    "test_simd_arith|Tests/Unit/test_simd_arith.elf||10|ALL PASS"
    "tier2_smov|Tests/Unit/jit_tier2_smov.elf||10|ALL PASS|JIT"
    "memory_guard_address|Tests/Unit/jit_memory_guard_address.elf||15|ALL PASS"
    "bitfield_regalloc|Tests/Unit/jit_bitfield_regalloc.elf||10|ALL PASS"
    "ccmp_forward|Tests/Unit/jit_ccmp_forward.elf||10|ALL PASS"
    "same_operand|Tests/Unit/jit_same_operand.elf||10|ALL PASS"
    "variable_shift_source|Tests/Unit/jit_variable_shift_source.elf||10|ALL PASS"
    "shifted_operand_width|Tests/Unit/jit_shifted_operand_width.elf||10|shifted_operand_width: PASS"
    "test_tls_static|Tests/Unit/test_tls_static.elf||10|ALL PASS"
    # Multi-threaded pthread test (default: 4 threads, fib(35)).
    # Exercises clone/clone3 + futex (FUTEX_WAIT/WAKE/REQUEUE) +
    # set_tid_address + per-thread JIT + __tl_lock release-via-ctid.
    "test_pthread|Tests/Unit/test_pthread.elf||15|ALL PASS"
    # POSIX threads primitive tests (Task 4). Cross-compiled with musl.
    # Each tests a specific pthread/semaphore primitive at a conservative
    # scale that stays within the emulator's current futex-wakeup throughput.
    "test_pthread_mutex|Tests/Unit/test_pthread_mutex.elf||10|ALL PASS"
    "test_pthread_cond|Tests/Unit/test_pthread_cond.elf||10|ALL PASS"
    "test_pthread_rwlock|Tests/Unit/test_pthread_rwlock.elf||10|ALL PASS"
    # Concurrent GraphicThunk dispatch (4 threads hammering proc-address
    # + string-cache paths). Guards the state_mu hardening.
    "test_thunk_mt|Tests/Unit/test_thunk_mt.elf||30|ALL PASS"
    "test_sem|Tests/Unit/test_sem.elf||10|ALL PASS"
    "test_pthread_once|Tests/Unit/test_pthread_once.elf||10|ALL PASS"
    # exit_group must not hang on siblings blocked in host syscalls
    # (read/nanosleep): the emulator kicks each vCPU to interrupt the
    # blocking call so shutdown completes. A hang fails via the timeout.
    "test_exit_blocked|Tests/Unit/test_exit_blocked.elf||10"
    # Dedicated signal-delivery regression: rt_sigaction
    # aliasing, sigsuspend pre-suspend mask restore, and cross-thread
    # delivery through the per-CPU pending queue + per-CPU signal frames.
    "test_signal_semantics|Tests/Unit/test_signal_semantics.elf||20|ALL PASS"
    "test_sigreturn_context|Tests/Unit/test_sigreturn_context.elf||20|ALL PASS"
    # rt_sigtimedwait blocks/dequeues instead of always returning EAGAIN.
    "test_sigtimedwait|Tests/Unit/test_sigtimedwait.elf||20|15/15 checks passed"
    # FUTEX_CMP_REQUEUE takes nr_requeue from the timeout slot, not val3.
    "test_futex_requeue|Tests/Unit/test_futex_requeue.elf||15|4/4 checks passed"
    # fork() with live guest threads: the child must not inherit joinable
    # host std::threads for threads that no longer exist.
    "test_fork_threads|Tests/Unit/test_fork_threads.elf||20|ALL PASS"
    # Memory range-safety: wrapping munmap rejected,
    # MAP_FIXED_NOREPLACE refuses the stack, huge madvise returns promptly,
    # and above-window MAP_FIXED r/w cycles run sequentially.
    "test_mem_guard|Tests/Unit/test_mem_guard.elf||20|ALL PASS"
    # Guest memory is one host reservation: fixed mappings at 1 TiB, faults
    # after munmap/mprotect, a 1 TiB PROT_NONE reservation, high+low MAP_SHARED
    # views, MADV_DONTNEED and mremap above 4 GiB.
    "test_mem_reservation|Tests/Unit/test_mem_reservation.elf||30|ALL PASS"
    # MADV_REMOVE: hole punch for writable shared files, EINVAL for private
    # anonymous (ART's CMC arena release depends on it), EACCES, ENOMEM.
    "test_madv_remove|Tests/Unit/test_madv_remove.elf||10|ALL PASS"
    # /dev/urandom and /dev/random never run dry: one open stream reads
    # far past 256 bytes (Teeworlds' secure_random_fill traps on a short read).
    "test_dev_urandom|Tests/Unit/test_dev_urandom.elf||10|ALL PASS"
    # Synthetic /sys CPU topology: sysconf(_SC_NPROCESSORS_ONLN), the
    # /sys/devices/system/cpu/{online,possible,present} files, their
    # stat types/sizes, the getdents64 listing (3 files + 8 cpu dirs),
    # sched_getaffinity, and the per-CPU capacity/freq leaves. libunity
    # sizes its job-system worker pool from these (SpongeBob stall).
    "test_sysfs_cpu|Tests/Unit/test_sysfs_cpu.elf||10|ALL PASS"
    # userfaultfd SIGBUS mode as ART's CMC GC uses it, including
    # MREMAP_DONTUNMAP and registered ranges on both sides of 4 GiB.
    "test_userfaultfd|Tests/Unit/test_userfaultfd.elf||10|ALL PASS"
    # /proc/self/fd/N[/name] reopens the GUEST descriptor's file (not the
    # emulator's fd N); openat2 shares openat's flags and resolution.
    "test_proc_fd_reopen|Tests/Unit/test_proc_fd_reopen.elf||10|ALL PASS"
    # Rewind static and lazy synthetic files, including after EOF.
    "test_proc_rewind|Tests/Unit/test_proc_rewind.elf||10|ALL PASS"
    # POSIX record locks marshal guest flock input and GETLK output.
    "test_fcntl_lock|Tests/Unit/test_fcntl_lock.elf||10|ALL PASS"
    # Linux argv/env string layout, live /proc/self/cmdline (setArgV0 and
    # setproctitle rewrites, also after execve), /proc/<getpid()> == self.
    "test_proc_cmdline|Tests/Unit/test_proc_cmdline.elf||10|ALL PASS"
    # exit_group / fatal default signals from any thread end the process;
    # main pthread_exit does not; fork-child threads share getpid().
    "test_group_exit|Tests/Unit/test_group_exit.elf||60|ALL PASS"
    # Cross-thread JIT invalidation of a running thread's blocks (also run
    # with per-thread JITs in the "Per-thread JIT" category below).
    "test_jit_thread_invalidate|Tests/Unit/test_jit_thread_invalidate.elf||60|ALL PASS"
    "test_memory_permissions|Tests/Unit/test_memory_permissions.elf||30|ALL PASS"
    "jit_host_fault|Tests/Unit/jit_host_fault.elf||15|ALL PASS|JIT"
    "interp_host_fault|Tests/Unit/jit_host_fault.elf single||15|ALL PASS|INTERP"
    # Fixed mapping contents, concurrent fd reads during mmap, mremap
    # relocation contracts, and active alternate-stack changes (47 checks).
    "test_vm_signal_contracts|Tests/Unit/test_vm_signal_contracts.elf||30|ALL PASS"
    "test_syscall_regress|Tests/Unit/test_syscall_regress.elf||10|0 checks failed"
    "test_producer_consumer|Tests/Unit/test_producer_consumer.elf||10|ALL PASS"
    # High-contention atomic stress test (8 threads). Validates CAS, LL/SC
    # (mutex), and LDADD atomics under game-scale contention.
    "test_atomic_stress|Tests/Unit/test_atomic_stress.elf||30|ALL PASS"
    # LSE inline-asm test — forces CAS/LDADD/LDSET/LDCLR/LDEOR/SWP via
    # -march=armv8.1-a+lse, testing the JIT's native lock-prefixed codegen.
    "test_lse_inline|Tests/Unit/test_lse_inline.elf||10|ALL PASS"
    # NEW (Turn 56): CCMP 32-bit flag computation test. Validates the fix
    # for the JIT using 64-bit sub for 32-bit CCMP (wrong Sign Flag).
    "jit_ccmp|Tests/Unit/jit_ccmp.elf||5|ALL PASS"
    # NEW (Turn 57): FCVT and FP load/store test. Validates the fix for
    # FCVT being misidentified as SCVTF (mask collision), and FP LDR/STR
    # accessing cpu.regs[] instead of cpu.v_lo[].
    "jit_fcvt|Tests/Unit/jit_fcvt.elf||5|ALL PASS"

    # NEW (Turn 89): FRINT (FP round to integer) native JIT codegen test.
    # Validates the fix for FRINTA/FRINTX/FRINTI being silently NOP'd
    # (is_fp_1source decoder bug) and the IR translator passing vregs
    # instead of FP reg indices. Tests all 7 FRINT variants (N/P/M/Z/A/X/I)
    # for both single and double precision.
    "jit_frint|Tests/Unit/jit_frint.elf||5|PASS"

    # NEW (Turn 63): SCVTF/UCVTF/FCVTZS/FCVTZU with FP source/dest —
    # the "Advanced SIMD scalar two-register miscellaneous" group (0x5E...).
    # GCC emits these for `(double)long_var` when the long is already in
    # an FP register. Without this fix, `(double)19` returned 0.0, which
    # broke toybox `time` (rusage delta computation used SCVTF).
    "jit_scvtf_fp|Tests/Unit/jit_scvtf_fp.elf||5|ALL PASS"
    # NEW: FCSEL native FP conditional select (VBLENDVPS/VPD). All 16
    # conditions x single+double, the HI/LS cmc path (flags from ADDS),
    # the pstate-load path (flag setter and fcsel in different blocks +
    # volatile int op between), NaN bitwise selection, +0.0/-0.0 bit-exact,
    # and real C ternaries.
    "jit_fcsel|Tests/Unit/jit_fcsel.elf||5|fcsel: PASS"
    "fcsel_high_regs|Tests/Unit/jit_fcsel_high_regs.elf||5|61 checks passed"
    # PRFM (prefetch) must be a NOP (it was decoded as a load and clobbered
    # the prfop field's register number); LDAPR/LDAPRB/LDAPRH (ARMv8.3 RCpc
    # acquire loads) were misdecoded as an LSE CAS.
    "jit_prfm_ldapr|Tests/Unit/jit_prfm_ldapr.elf||5|checks passed"
    # SIMD&FP LDR/STR register offset: Q forms scale the index by 16.
    "jit_simd_ldst_regoff|Tests/Unit/jit_simd_ldst_regoff.elf||5|checks passed"
    # Two-operand ALU loads under full register pressure (OpenSSL SHA-256).
    "jit_two_operand_pressure|Tests/Unit/jit_two_operand_pressure.elf||5|1920/1920 checks passed"
    # Compatibility syscalls: permissive no-ops (NUMA/ioprio/timeofday/
    # vhangup/swapoff) and explicit -ENOSYS/-EPERM stubs (fanotify/bpf/
    # userfaultfd/mqueue/quotactl).
    "android_binder_exit|Tests/Unit/android_binder_exit.elf||5|checks passed"
    "android_binder|Tests/Unit/android_binder.elf||10|checks passed"
    "android_binder_features|Tests/Unit/android_binder_features.elf||10|checks passed"
    "android_binder_objects|Tests/Unit/android_binder_objects.elf||10|checks passed"
    "jit_bfm_masks|Tests/Unit/jit_bfm_masks.elf||30|checks passed"
    "jit_android_semantics|Tests/Unit/jit_android_semantics.elf||10|checks passed"
    "jit_literal_load|Tests/Unit/jit_literal_load.elf||10|checks passed"
    "test_shared_alias|Tests/Unit/test_shared_alias.elf||30|checks passed"
    "jit_call_exit|Tests/Unit/jit_call_exit.elf||10|checks passed"
    "jit_blr_lr|Tests/Unit/jit_blr_lr.elf||10|checks passed"
    "jit_interp_exit|Tests/Unit/jit_interp_exit.elf||10|2001/2001 checks passed"
    "jit_flags_passthrough|Tests/Unit/jit_flags_passthrough.elf||10|checks passed"
    "socket_msg_layout|Tests/Unit/socket_msg_layout.elf||10|checks passed"
    "android_syscall_options|Tests/Unit/android_syscall_options.elf||10|checks passed"
    "android_service_manager|Tests/Unit/android_service_manager.elf||10|android_service_manager: ALL PASS"
    "test_compat_syscalls|Tests/Unit/test_compat_syscalls.elf||5|ALL TESTS PASSED"
    # cwd/dirfd-relative path syscalls, chmod argument count, statx AT_EMPTY_PATH.
    "test_path_syscalls|Tests/Unit/test_path_syscalls.elf||10|test_path_syscalls: ALL PASS"
)

# Integration tests (Tests/Apps/ — real-world test programs)
INTEGRATION_TESTS=(
    # SDL2 mode-query ABI, fullscreen recreation, and resize-event delivery.
    "sdl_fullscreen|Tests/Apps/test_sdl_fullscreen.elf||30|ALL PASS"
    # Host validator checks guest compression and decompression in both engines.
    "zstd_compression|Scripts/Testing/run_zstd_regression.sh||120|ZSTD REGRESSION PASSED|HOST_JIT"
    "audio_test|Tests/Apps/audio_test.elf||10|audio_test: done"
    "div_loop|Tests/Apps/div_loop.elf||5"
    "div_loop2|Tests/Apps/div_loop2.elf||5"
    "div_simple|Tests/Apps/div_simple.elf||5"
    "div_test|Tests/Apps/div_test.elf||5"
    "dynlink_test|Tests/Apps/test_dynlink.elf||5|test_dynlink: ALL PASS"
    "fcvtzu_test|Tests/Apps/fcvtzu_test.elf||5|OK"
    "fcvtzu_test2|Tests/Apps/fcvtzu_test2.elf||5|passed"
    # FCVT rounding-mode regression: FP→int conversions only
    # handled FCVTZS/FCVTZU (rmode=3, toward zero). floor() compiles to
    # FCVTMS (rmode=2) and ceil() to FCVTPS (rmode=1), so chunk/mesh math
    # fell back to CALL_INTERP. Forced encodings check FCVTNS/MS/PS/ZS in
    # single+double with 32/64-bit dests (no libm — builds under setup-tests).
    "fcvt_round|Tests/Apps/test_fcvt_round.elf||10|PASS"
    # FP-source int<->FP regression: SIMD-scalar SCVTF/UCVTF
    # (FP source) and FCVTZS/FCVTZU (FP dest) in the 0x5E200800 two-register-
    # misc group fell back to CALL_INTERP (and a codegen fbits=64 sentinel
    # divided terrain coords by 2^64). Covers signed/unsigned x 32/64-bit
    # + NaN and saturation edges (no libm — builds under setup-tests).
    "fcvt_fpsrc|Tests/Apps/test_fcvt_fpsrc.elf||10|ALL PASS"
    "movi_imm|Tests/Apps/test_movi_imm.elf||10|test_movi_imm: ALL PASS"
    "fib|Tests/Apps/fib.elf||5"
    "fwd_repro|Tests/Apps/fwd_repro.elf||5"
    "fwd_repro2|Tests/Apps/fwd_repro2.elf||5"
    "fwd_repro3|Tests/Apps/fwd_repro3.elf||5"
    "fwd_repro4|Tests/Apps/fwd_repro4.elf||5"
    "fwd_repro5|Tests/Apps/fwd_repro5.elf||5"
    # FP correctness regression: FABS single-precision
    # in the JIT was using a double-width sign mask (cleared bit 63 not
    # bit 31), so fabsf() of a negative float left it negative. This
    # test forces the FABS instruction via volatile and checks the sign
    # bit is cleared. Also confirms 0.3f-0.1f matches host IEEE-754.
    "fabs_sign|Tests/Apps/test_fabs2.elf||10|ALL PASS"
    # FP correctness regression: FABD (floating-point
    # absolute difference, |a-b|) was completely unimplemented — musl's
    # fabsf(got-want) is lowered to `fabd` by the compiler, so a broken
    # FABD silently returned the first operand, breaking float
    # comparisons. Tests single + double precision.
    "fabd|Tests/Apps/test_fabd.elf||10|ALL PASS"
    # SIMD vector FP 2-source regression: the vector forms
    # of FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FABD/FMAXNM/FMINNM (0x0E/0x2E group,
    # .2s/.4s) were completely unimplemented — NEON-vectorized FP silently
    # produced wrong results. Tests all ops in single precision (.4s, .2s).
    "simd_vec_fp|Tests/Apps/test_simd_vec_fp.elf||10|ALL PASS"
    # SIMD vector FP fused 3-source regression: FMLA/FMLS
    # (Vd = Vd +/- Vn*Vm) were completely unimplemented — NEON-vectorized
    # matmul/convolution kernels (GCC lowers a*b+c to fmla) hit DecodeError
    # (SIGILL). Tests both ops in single (.4s/.2s) and double (.2d).
    "simd_fmla|Tests/Apps/test_simd_fmla.elf||10|ALL PASS"
    # SADDW/SADDW2 + UMINP vector regression: widening add
    # and pairwise unsigned min, all sizes + low/high half selection. These
    # were silently NOP'd; now covered (busybox df / iperf3 depend on them).
    "simd_saddw_uminp|Tests/Apps/test_simd_saddw_uminp.elf||10|ALL PASS"
    # vDSO fast-path regression: clock_gettime /
    # clock_getres / gettimeofday served entirely from the JIT/instr fast
    # path without entering the syscall dispatcher (mirrors real Linux,
    # where the vDSO never traps into the kernel). 10 checks, incl. a
    # 100k-call monotonic non-decreasing loop.
    "vdso_clock|Tests/Apps/test_vdso_clock.elf||10|ALL PASS"
    # UMOV regression: vector element → GPR extraction for
    # B/H/S/D across both halves. Covered the pre-existing bug report
    # (`umov v.d[1]` returning garbage) which turned out to be a test-asm
    # constraint artifact — the handler is correct and stays locked in.
    "simd_umov|Tests/Apps/test_simd_umov.elf||10|ALL PASS"
    # Vector shift-by-immediate regression: USRA/SSRA/SLI/SRI
    # across H/S/D, Q=1 and Q=0, incl. boundary shifts (0 and esize*8).
    # Exercises the AVX2 256-bit and SSE2 128-bit codegen paths plus the
    # interpreter; also guards the Q=0 v_hi-zeroing fix.
    "simd_shift|Tests/Apps/test_simd_shift.elf||10|ALL PASS"
    "head|Tests/Apps/head.elf||5"
    "input_test|Tests/Apps/test_input.elf||5|test_input: done"
    "gamepad_test|Tests/Apps/test_gamepad.elf||5|test_gamepad: done"
    "sdl_demo|Tests/Apps/test_sdl_demo.elf||15|drew 60 frames"
    # SDL_CreateThread / WaitThread / DetachThread lifecycle via the thunk:
    # status+data round-trip, 8 concurrent create/wait pairs, and detach
    # (record reaped, guest stack stays valid). Exit 77 = skip without SDL.
    "sdl_thread|Tests/Apps/test_sdl_thread.elf||30|ALL PASS"
    # SDL2 + OpenGL triangle via GraphicThunk (needs DISPLAY + host GL).
    # Exit 77 = skip when SDL/GL unavailable.
    "sdl_gl_triangle|Tests/Apps/test_sdl_gl_triangle.elf||30|ALL PASS"
    # Bind-elision regression (needs DISPLAY + host GL 3.x).
    # Redundant ARRAY/ELEMENT binds + VAO switches + delete/reuse must
    # render identical pixels with elision on (default) — pixel-checked
    # via glReadPixels. Exit 77 = skip when SDL/GL unavailable.
    "sdl_gl_elide|Tests/Apps/test_sdl_gl_elide.elf||30|ELIDE TEST PASSED"
    # GLES draw-row EBO offsets via GraphicThunk (needs DISPLAY + ES 3.x).
    # EBO-bound nonzero-offset draw through the GLES EL_PTR rows with a
    # readback pin; control offset-0 draw isolates ES setup failures.
    # Exit 77 = skip when SDL/ES3 unavailable.
    "sdl_gles_ebo|Tests/Apps/test_sdl_gles_ebo.elf||30|ALL PASS"
    # VAO + EBO + multidraw + indirect draws with readback pins
    # (needs DISPLAY + host GL). Exit 77 = skip when SDL/GL unavailable.
    "sdl_gl_vao|Tests/Apps/test_sdl_gl_vao.elf||60|ALL PASS"
    # glMapBuffer/glMapBufferRange/glUnmapBuffer/glFlushMappedBufferRange
    # bounce (2026-08): verifies the guest gets a guest-window bounce that
    # seeds from the host buffer (GL_MAP_READ_BIT), writebacks on unmap
    # (GL_MAP_WRITE_BIT), and that flush pushes ranges early. Needs
    # DISPLAY + host GL. Exit 77 = skip when unavailable.
    "sdl_gl_mapbuffer|Tests/Apps/test_sdl_gl_mapbuffer.elf||30|ALL PASS"
    # Android NativeActivity surface layer (2026-08-20): ANativeWindow
    # shim + EGL window-surface interception. The guest dlopens
    # libandroid.so, gets a shim window, and creates a REAL host EGL
    # window-surface through it (thunk substitutes the host X11 window).
    # Needs DISPLAY + host GL. Exit 77 = skip when unavailable.
    "android_surface|Tests/Apps/test_android_surface.elf||30|ALL PASS"
    # Android lifecycle layer v2 (2026-08): ALooper/AInputQueue/event
    # getters/AConfiguration/liblog plumbing for --android
    # native_app_glue .so's. No display needed (AConfiguration + looper
    # fd readiness + queue empty path all headless).
    "android_activity|Tests/Apps/test_android_activity.elf||30|ALL PASS"
    # Per-thread ALooper + AChoreographer frame callbacks + no-sensor
    # ASensorManager (AOSP Looper/Choreographer/sensor.cpp semantics).
    "android_looper|Tests/Apps/test_android_looper.elf||30|ALL PASS"
    # Audio thunk arms (2026-08-22): ALSA subset / SDL2 queue / Pulse
    # simple / OSS front doors → shared AudioEngine ring. Headless-safe
    # (WAV-dump backend counts bytes; no host device needed).
    "linux_audio|Tests/Apps/test_linux_audio.elf||30|ALL PASS"
    # Android audio (2026-08-22): AAudio builder/open/write/close +
    # OpenSL ES synthetic-vtable object model (Enqueue + RegisterCallback).
    # Headless-safe.
    "android_audio|Tests/Apps/test_android_audio.elf||30|ALL PASS"
    # MAMBO VULKAN TEST (2026-08-22): SDL2 window + Vulkan textured quad
    # (Matikanetannhauser) + one-shot mambo sound via the ALSA arm. Needs
    # DISPLAY + host Vulkan; exit 77 = skip without them.
    "mambo_vulkan|Tests/Apps/test_mambo_vulkan.elf||40|MAMBO VULKAN TEST PASSED"
    # Wayland host bridge: real compositor connect + fd + flush +
    # roundtrip + dispatch_pending + disconnect. Needs WAYLAND_DISPLAY;
    # exit 77 = skip headless.
    "wayland_bridge|Tests/Apps/test_wayland_bridge.elf||30|ALL PASS"
    # Wayland input/output payloads: seat caps, keymap fd+mmap content,
    # output geometry/mode/scale with real non-null listeners (10-word
    # geometry exercises the call_guest_function stack spill).
    # Needs WAYLAND_DISPLAY; exit 77 = skip headless.
    "wayland_input|Tests/Apps/test_wayland_input.elf||30|ALL PASS"
    # Modern GL 3.3+/4.x "AAA future-proofing" rows (2026-08-15): uniform
    # blocks (GetUniformBlockIndex/Binding/GetActiveUniformBlockiv/Name),
    # shader introspection (GetActiveUniform/GetActiveAttrib), instancing
    # (VertexAttribDivisor), base-vertex/range/restart draws,
    # query + sampler objects, compute dispatch/memory barrier/image
    # texture, and the nested-string glTransformFeedbackVaryings arm.
    # Needs DISPLAY + host GL >= 3.3. Exit 77 = skip when unavailable.
    "sdl_gl_modern|Tests/Apps/test_sdl_gl_modern.elf||40|ALL PASS"
    # KHR_debug callback interception: the guest callback is stored, NULL
    # goes to the host (a guest address handed to host GL would crash on
    # the first driver message). Needs DISPLAY + host GL. Exit 77 = skip.
    "sdl_gl_debugcb|Tests/Apps/test_sdl_gl_debugcb.elf||30|ALL PASS"
    # Vulkan host-path smoke test: dlopen(libvulkan.so.1) via the
    # internal dlopen syscall → vkGetInstanceProcAddr → create instance
    # (deep-marshalled VkApplicationInfo + extension string array) →
    # enumerate physical devices → create device (queue create-info array
    # + pQueuePriorities + features) → get queue → wait idle → destroy.
    # Exercises the THUNK_VULKAN dispatch. Exit 77 = skip when the host has
    # no Vulkan loader/driver (env-dependent).
    "vulkan|Tests/Apps/test_vulkan.elf||30|VULKAN TEST PASSED"
    # Headless WSI swapchain path: VK_KHR_surface +
    # VK_EXT_headless_surface — create headless surface, query caps/formats/
    # present modes, create swapchain, get images, acquire, present, teardown.
    # Exit 77 = skip when the host driver lacks VK_EXT_headless_surface.
    "vulkan_swapchain|Tests/Apps/test_vulkan_swapchain.elf||30|SWAPCHAIN TEST PASSED"
    # pNext-chain query coverage (2026-08-24): Properties2 →
    # DriverProperties → IDProperties and Features2 → BDA guest chains —
    # driver must fill every staged node, guest chain links + guard
    # canaries must survive, unknown sTypes truncate safely. Regression
    # net for the enum-sized-array layout bug vkQuake exposed (no other
    # suite test touches this path). Headless-capable; 77 = no Vulkan.
    "vulkan_pnext|Tests/Apps/test_vulkan_pnext.elf||30|VK PNEXT TEST PASSED"
    # GL state tracker regression: verifies GLStateTracker
    # mirrors guest GL state and answers queries (glIsEnabled,
    # glGetIntegerv, glGetFloatv, glGetBooleanv) consistently. Headless —
    # no DISPLAY needed (state setters are thunked to host libGL, queries
    # are answered from the tracker). Catches the guest-vs-host pointer
    # crash and the typed-query-dispatch bug that previously made this
    # test SIGSEGV with 57 failures.
    "gl_state|Tests/Apps/test_gl_state.elf||15|ALL PASS"
    # NEW (Turn 74): comprehensive game demo — bouncing ball with
    # framebuffer, input, audio, and game loop.
    "game_demo|Tests/Apps/test_game_demo.elf||15|game: done"
    # Signal handler test: verifies that a real SIGINT handler runs
    # before read() returns -EINTR (Turn 43 fix). Self-contained —
    # forks a child that sends SIGINT after 200ms. No pty needed.
    # Works under both JIT and interpreter (Turn 55 fixed the
    # rt_sigreturn stack-corruption bug that previously made the
    # interpreter lose x0/x19 after signal delivery).
    "sigint_handler|Tests/Apps/test_sigint_handler.elf||10|PASS: handler ran"
    # Comprehensive signal registration test: rt_sigaction install/query/
    # SA_RESETHAND/SIG_IGN/SIGKILL-EINVAL, rt_sigprocmask block/unblock/
    # setmask, rt_sigpending, rt_sigsuspend. Validates the Turn 46 fixes
    # (SA_RESETHAND dangling-pointer, 1-based bit numbering, rt_sigpending
    # returns actual pending mask, sigsuspend drains pending before blocking).
    "sigaction|Tests/Apps/test_sigaction.elf||10|ALL PASS"
    # Focused sigsuspend test: forked child sends SIGUSR1 after 100ms.
    "sigsuspend|Tests/Apps/test_sigsuspend.elf||10|PASS"
    # NEW (Turn 54): signal subsystem production-hardening tests.
    # sigaltstack verifies SA_ONSTACK + sigaltstack() install/query/disable.
    # sig_nested verifies a handler can be interrupted by another signal.
    # sig_sa_mask verifies sa_mask blocks additional signals during handler.
    # sig_pending verifies multiple pending signals are delivered on unblock.
    # sig_callee_saved verifies x19-x28 are preserved across signal delivery.
    # All 5 now work under both JIT and interpreter (Turn 55 fix).
    "sigaltstack|Tests/Apps/test_sigaltstack.elf||10|ALL PASS"
    "sig_nested|Tests/Apps/test_sig_nested.elf||10|ALL PASS"
    "sig_sa_mask|Tests/Apps/test_sig_sa_mask.elf||10|ALL PASS"
    "sig_pending|Tests/Apps/test_sig_pending.elf||10|ALL PASS"
    "sig_callee_saved|Tests/Apps/test_sig_callee_saved.elf||10|ALL PASS"
    # NEW (Turn 55): regression test for the interpreter rt_sigreturn
    # stack-corruption bug. Before Turn 55, this crashed the interpreter
    # with SIGSEGV because cpu.regs[31] (XZR) was corrupted to hold SP.
    # Must pass under both JIT and interpreter.
    "sig_interp_regression|Tests/Apps/test_sig_interp_regression.elf||10|ALL PASS"
    "jit_new_ops|Tests/Apps/jit_new_ops.elf||5|ALL TESTS PASSED"
    "loop_div|Tests/Apps/loop_div.elf||5"
    "md5_neon_test|Tests/Apps/md5_neon_test.elf||5"
    "md5_scalar_test|Tests/Apps/md5_scalar_test.elf||5|5d41402abc4b2a76b9719d911017c592"
    "rev_real|Tests/Apps/rev.elf||5"
    "ror_imm_test|Tests/Apps/ror_imm_test.elf||5|OK"
    "simple_div|Tests/Apps/simple_div.elf||5|100 / 7 = 14"
    "sin_test|Tests/Apps/sin_test.elf||10|K\\[7\\] = 0xfd469501"
    "sort|Tests/Apps/sort.elf||10"
    "strtod_nan_test|Tests/Apps/strtod_nan_test.elf||5"
    "ubfiz_test|Tests/Apps/ubfiz_test.elf||5|OK"
    "wc|Tests/Apps/wc.elf||5"
    # SHA crypto-extension regression test: verifies SHA1SU0/SU1 and
    # SHA256SU0/SU1 instructions produce correct schedule updates.
    # This turn fixed three bugs: wrong SHA1SU1 encoding (was 0x5E280000,
    # should be 0x5E281800), wrong SHA256SU0 encoding (was 0x5E282000,
    # should be 0x5E282800), and missing SHA1SU0/SHA256SU1 dispatch.
    # Also fixed a 128-bit V register load bug (memcpy of 16 bytes from
    # v_lo[n] read v_lo[n] + v_lo[n+1] instead of v_lo[n] + v_hi[n]).
    "sha256_crypto|Tests/Apps/test_sha256_crypto.elf||5|16/16 checks passed"
    # SHA256H/H2/SHA1C/SHA1P/SHA1M/SHA1H crypto extension regression test.
    # Verifies JIT/interp parity for the hash-round instructions added
    # in this turn. 8/8 checks pass under both modes.
    "sha256_full|Tests/Apps/test_sha256_full.elf||5|8/8 checks passed"
    # Basic test/ programs (non-interactive)
    "count|Tests/Asm/count.elf||5"
    "extr|Tests/Asm/extr.elf||5|OK"
    "fib_basic|Tests/Asm/fib.elf||5|832040"
    "hello_basic|Tests/Asm/hello.elf||5|Hello, ARM64"
    # Heap stress test: exercises mremap_grow + munmap + mmap patterns
    # that previously corrupted musl's mallocng metadata (Turn 51 fix).
    "heap_stress|Tests/Apps/heap_stress.elf||10|heap_stress OK"
    # NEW (Turn 54): syscall/memory correctness tests.
    # brk verifies brk() extend/contract + rejection of absurd addresses.
    # pipe verifies pipe() + fork + read/write + EOF on close.
    # auxv verifies AT_PAGESZ/AT_PHDR/AT_ENTRY/AT_RANDOM/AT_HWCAP/etc.
    # getenv verifies envp[] + setenv/unsetenv round-trips.
    "brk|Tests/Apps/test_brk.elf||5|ALL PASS"
    "pipe|Tests/Apps/test_pipe.elf||5|ALL PASS"
    "auxv|Tests/Apps/test_auxv.elf||5|ALL PASS"
    "getenv|Tests/Apps/test_getenv.elf||5|ALL PASS"

    # NEW (Turn 64): host env var propagation (TZ, LANG, LC_*). Verifies
    # core env vars are set and localtime/strftime/setenv work.
    "env|Tests/Apps/test_env.elf||5|ALL PASS"

    # NEW (Turn 65): prctl — PR_SET_NAME/PR_GET_NAME round-trip,
    # truncation, PR_GET_DUMPABLE, PR_GET_NO_NEW_PRIVS, unknown option
    # returns -EINVAL.
    "prctl|Tests/Apps/test_prctl.elf||5|ALL PASS"

    # NEW: preadv/pwritev + additional stubbed syscalls. Verifies:
    #   - preadv/pwritev roundtrip with vectorized I/O and offset semantics
    #   - acct/reboot return -EPERM
    #   - syslog returns -ENOSYS
    #   - sched_rr_get_interval returns 0 with a valid interval
    "syscalls|Tests/Apps/test_syscalls.elf||5|ALL PASS"
)

# Dynamic linking tests (Turn 53).
# These require the rootfs and toolchains. Skipped if not present.
# Uses a special env prefix (BIFROST_ROOT) to enable rootfs sandboxing.
DYNAMIC_TESTS=(
    "xwayland_wire|Tests/Apps/xwayland_wire.elf||15|checks passed"
    "hello_dyn_musl|Tests/Apps/hello_dyn_musl.elf||5|Hello, dynamic world"
    "test_dyn_write|Tests/Apps/test_dyn_write.elf||5|dyn_write_ok"
    # NEW (Turn 74): glibc dynamic tests. These verify the R_AARCH64_COPY
    # relocation support and CMEQ #0 SIMD fix that enable glibc's printf
    # and stdio to work under dynamic linking.
    "hello_dyn_glibc|Tests/Apps/hello_dyn_glibc.elf||5|Hello, dynamic world"
    "test_dyn_hello|Tests/Apps/test_dyn_hello.elf||5|Hello, glibc dynamic"
    "test_dyn_malloc|Tests/Apps/test_dyn_malloc.elf||5|^done$"
    "test_dyn_printf|Tests/Apps/test_dyn_printf.elf||10|^\\[done\\]$"
    "test_dyn_full_musl|Tests/Apps/test_dyn_full_musl.elf||15|ALL PASS"
    # NEW (Turn 77): glibc dynamic pthread tests. These verify the
    # NPTL stack-list init, _dl_allocate_tls shim override, rseq
    # success, TCB/struct-pthread placement, and clone3 ctid fixes
    # that make glibc dynamic pthread_create work end-to-end.
    "test_dyn_pthread_min|Tests/Apps/test_dyn_pthread_min.elf||10|joined, counter=1"
    "test_dyn_threads|Tests/Apps/test_dyn_threads.elf||20|test_dyn_threads: ALL PASS"
    "test_dyn_pthread_stress|Tests/Apps/test_dyn_pthread_stress.elf||30|test_dyn_pthread_stress: ALL PASS"
    # Turn 79: 8-thread multi-wave TLS isolation test. Verifies the fix
    # for the "8-thread race" bug (context.md Turn 78 Issue #3). Tests
    # 8 threads x 4 waves with __thread long tls_array[8] per thread.
    "test_dyn_pthread_8thread|Tests/Apps/test_dyn_pthread_8thread.elf||30|test_dyn_pthread_8thread: ALL PASS"
    # Main-thread struct pthread tid: rwlocks and error-checking/recursive
    # mutexes on the main thread returned EDEADLK (OpenSSL 3 providers).
    "test_dyn_main_locks|Tests/Apps/test_dyn_main_locks.elf||10|test_dyn_main_locks: ALL PASS"
    "test_dlopen|Tests/Apps/test_dlopen.elf||15|test_dlopen: ALL PASS"
    # Loader thread-safety regression: 4 dlopen/dlsym/dlclose threads x 500
    # iterations + dladdr/dl_iterate_phdr workers. Verifies the loader mutex
    # (recursive_lock) and CPU borrow make concurrent dl* calls race-free.
    "test_dlopen_mt|Tests/Apps/test_dlopen_mt.elf||60|test_dlopen_mt: ALL PASS"
    # dladdr tests. Verifies the dladdr symbol override is
    # enabled and routes user dladdr() calls through our implementation.
    # test_dladdr uses the internal syscall (static musl); test_dladdr_glibc
    # calls the real glibc dladdr@GLIBC_2.34 symbol (dynamic glibc).
    "test_dladdr|Tests/Apps/test_dladdr.elf||15|test_dladdr: ALL PASS"
    "test_dladdr_glibc|Tests/Apps/test_dladdr_glibc.elf||15|test_dladdr_glibc: ALL PASS"
)

# Interactive tests (need stdin input)
INTERACTIVE_TESTS=(
    "echo|Tests/Asm/echo.elf|q\n|5|echo>"
    "repl|Tests/Asm/repl.elf|q\n|5"
    "cat|Tests/Asm/cat.elf /etc/hostname||5"
    "sh|Tests/Apps/sh.elf|exit\n|10"
    "fgets_test|Tests/Apps/fgets_test.elf|hello\n|5|got: \[hello"
)

# Toybox integration tests
TOYBOX_TESTS=(
    "toybox_echo|Tests/Apps/toybox echo hello|hello|5|hello"
    "toybox_seq|Tests/Apps/toybox seq 1 5||5|^[12345]$"
    "toybox_ls|Tests/Apps/toybox ls /||5|^bin$"
    "toybox_md5sum|Tests/Apps/toybox md5sum|hello\n|5|b1946ac92492d2347c6235b4d2611184"
    "toybox_sha256sum|Tests/Apps/toybox sha256sum|hello\n|5|5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03"
    "toybox_wc|Tests/Apps/toybox wc|hello world\n|5|^[[:space:]]*1"
    "toybox_sort|Tests/Apps/toybox sort|3\n1\n2\n|5|^1$"
    "toybox_uname|Tests/Apps/toybox uname||5|Linux"
    # toybox yes is infinite — timeout is expected, just check it produced "y"
    "toybox_yes|Tests/Apps/toybox yes||2|^y$"
)

# Benchmarks (slow, skipped with --quick)
BENCH_TESTS=(
    "bench_mips|Tests/Apps/bench_mips.elf||30|done:"
    "bench_memcpy|Tests/Apps/bench_memcpy.elf||30|MiB/s"
    "bench_sort|Tests/Apps/bench_sort.elf||30|qsort:"
    "bench_matrix|Tests/Apps/bench_matrix.elf||30|MFLOPS"
    "bench_fib|Tests/Apps/bench_fib.elf||30|fib\(35\)"
)

# Real-world binary tests (Turn 55).
# These use real AArch64 static binaries downloaded from the web:
#   - busybox-aarch64: BusyBox v1.37.0 from files.serverless.industries
#   - toybox-aarch64:  ToyBox 0.8.14 from landley.net
#   - iperf2-aarch64:  iperf 2.2.1 from files.serverless.industries
# Each tests a real-world program's ability to run basic commands.
# Skipped if the binary doesn't exist (e.g., not downloaded yet).
REALWORLD_TESTS=(
    "rw_busybox_echo|Tests/Apps/realworld/busybox-aarch64 echo hello|hello|5|^hello$"
    "rw_busybox_seq|Tests/Apps/realworld/busybox-aarch64 seq 1 5||5|^1$"
    "rw_busybox_uname|Tests/Apps/realworld/busybox-aarch64 uname||5|^Linux$"
    "rw_busybox_true|Tests/Apps/realworld/busybox-aarch64 true||5|"
    "rw_busybox_printf|Tests/Apps/realworld/busybox-aarch64 printf %d 42||5|^42$"
    "rw_busybox_hostname|Tests/Apps/realworld/busybox-aarch64 hostname||5|"
    "rw_busybox_nproc|Tests/Apps/realworld/busybox-aarch64 nproc||5|^[0-9]+$"
    "rw_busybox_id|Tests/Apps/realworld/busybox-aarch64 id||5|^uid="
    "rw_busybox_basename|Tests/Apps/realworld/busybox-aarch64 basename /tmp/foo.txt||5|^foo.txt$"
    "rw_busybox_dirname|Tests/Apps/realworld/busybox-aarch64 dirname /tmp/foo.txt||5|^/tmp$"
    "rw_busybox_sha256sum|Tests/Apps/realworld/busybox-aarch64 sha256sum|hello\n|5|^5891b5b5"
    "rw_busybox_base64|Tests/Apps/realworld/busybox-aarch64 base64|hello\n|5|^aGVsbG8K"
    "rw_busybox_factor|Tests/Apps/realworld/busybox-aarch64 factor 42||5|^42: 2 3 7$"
    "rw_busybox_expr|Tests/Apps/realworld/busybox-aarch64 expr 6 + 7||5|^13$"
    # NEW (Turn 67): Previously-broken commands now work after SMADDL fix.
    "rw_busybox_awk|Tests/Apps/realworld/busybox-aarch64 awk {print}|hello|5|hello"
    # NOTE: musl busybox sed/grep fail due to regex engine reading past
    # allocation boundaries. Toybox sed/grep work perfectly — use those
    # for the sed/grep test coverage instead.
    "rw_busybox_sort|Tests/Apps/realworld/busybox-aarch64 sort -n|10\n2\n33\n1\n20|5|^1$"
    "rw_busybox_md5sum|Tests/Apps/realworld/busybox-aarch64 md5sum|hello\n|5|^b1946ac9"
    "rw_busybox_date|Tests/Apps/realworld/busybox-aarch64 date||5|^[A-Z][a-z]"
    "rw_busybox_df|Tests/Apps/realworld/busybox-aarch64 df||5|Filesystem"
    "rw_busybox_uptime|Tests/Apps/realworld/busybox-aarch64 uptime||5|load average"
    "rw_busybox_wc|Tests/Apps/realworld/busybox-aarch64 wc|hello world|5|[0-9]"
    "rw_busybox_env|Tests/Apps/realworld/busybox-aarch64 env||5|PATH="
    "rw_busybox_cat|Tests/Apps/realworld/busybox-aarch64 cat|hello\n|5|^hello$"
    "rw_busybox_head|Tests/Apps/realworld/busybox-aarch64 head -n1|line1\nline2|5|^line1$"
    "rw_busybox_tail|Tests/Apps/realworld/busybox-aarch64 tail -n1|line1\nline2|5|^line2$"
    "rw_busybox_whoami|Tests/Apps/realworld/busybox-aarch64 whoami||5|^$(whoami)$"
    "rw_busybox_pwd|Tests/Apps/realworld/busybox-aarch64 pwd||5|^/$"
    "rw_busybox_mktemp|Tests/Apps/realworld/busybox-aarch64 mktemp||5|^/tmp/tmp"
    "rw_busybox_printf_str|Tests/Apps/realworld/busybox-aarch64 printf hello||5|^hello$"
    "rw_busybox_true2|Tests/Apps/realworld/busybox-aarch64 sh -c true||5|"
    "rw_toybox_echo|Tests/Apps/toybox echo hello|hello|5|^hello$"
    "rw_toybox_seq|Tests/Apps/toybox seq 1 5||5|^1$"
    "rw_toybox_uname|Tests/Apps/toybox uname||5|^Linux$"
    "rw_toybox_sed|Tests/Apps/toybox sed s/hello/hi/|hello world|5|^hi world$"
    "rw_toybox_grep|Tests/Apps/toybox grep an|apple\nbanana\ncherry|5|banana"
    "rw_toybox_cat|Tests/Apps/toybox cat|hello|5|^hello$"
    "rw_toybox_wc|Tests/Apps/toybox wc|hello world|5|[0-9]"
    "rw_toybox_sort|Tests/Apps/toybox sort -n|10\n2\n33\n1\n20|5|^1$"
    "rw_toybox_head|Tests/Apps/toybox head -n1|line1\nline2|5|^line1$"
    "rw_toybox_tail|Tests/Apps/toybox tail -n1|line1\nline2|5|^line2$"
    "rw_toybox_date|Tests/Apps/toybox date||5|^[A-Z][a-z]"
    "rw_toybox_id|Tests/Apps/toybox id||5|^uid="
    "rw_toybox_pwd|Tests/Apps/toybox pwd||5|^/"
    "rw_toybox_hostname|Tests/Apps/toybox hostname||5|."
    "rw_toybox_whoami|Tests/Apps/toybox whoami||5|^$(whoami)$"
    "rw_toybox_env|Tests/Apps/toybox env||5|PATH="
    "rw_toybox_printf|Tests/Apps/toybox printf %d 42||5|^42$"
    "rw_toybox_md5sum|Tests/Apps/toybox md5sum|hello\n|5|^b1946ac9"
    "rw_toybox_sha256sum|Tests/Apps/toybox sha256sum|hello\n|5|^5891b5b5"
    # NEW (Turn 74): real-world DYNAMICALLY-LINKED glibc binaries from
    # Debian. These exercise the full dynamic linking stack (R_AARCH64_COPY,
    # TLS .tdata mirroring, GLOB_DAT, JUMP_SLOT, ifuncs, init arrays).
    # iperf3 needs libiperf + libcrypto; coreutils need libselinux + libpcre2.
    # All run with BIFROST_ROOT=rootfs (set below in the dynamic section).
    # We test actual functionality (not --help) to avoid the "no error if
    # existing" text in --help output triggering the FAIL/ERROR heuristic.
    "rw_iperf3_version|Tests/Apps/realworld/iperf3-aarch64 --version||10|^iperf 3|DYN"
    "rw_coreutils_echo|Tests/Apps/realworld/echo hello||5|^hello$|DYN"
    "rw_coreutils_cat|Tests/Apps/realworld/cat|hello\n|5|^hello$|DYN"
    "rw_coreutils_echo_n|Tests/Apps/realworld/echo -n abc||5|^abc$|DYN"
    # NEW (Turn 74 cont.): more real-world glibc dynamic tests
    "rw_coreutils_uname|Tests/Apps/realworld/uname||5|^Linux$|DYN"
    "rw_coreutils_date|Tests/Apps/realworld/date||5|^.{3} [A-Z][a-z]{2}|DYN"
    "rw_coreutils_readlink|Tests/Apps/realworld/readlink /proc/self/exe||5|readlink|DYN"
)

# ── Helpers ────────────────────────────────────────────────────────────
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
FAILED_TESTS=()
FILTER_MATCH_COUNT=0

record_failure() {
    local name="$1" reason="$2"
    printf "  ${C_RED}%-8s${C_RST} ${C_DIM}%-40s${C_RST} ${C_RED}%s${C_RST}\n" \
        "FAIL" "$name" "$reason"
    FAIL_COUNT=$((FAIL_COUNT + 1))
    FAILED_TESTS+=("$name ($reason)")
}

run_test() {
    local name="$1" file="$2" stdin="$3" tout="$4" pattern="${5:-}"
    local mode="${6:-}"
    local -a test_command=()
    local output rc

    # Apply filter (use grep -E so the user can pass alternation like
    # --filter "sig|brk" — basic regex doesn't support |).
    if [ -n "$FILTER" ] && ! grep -qiE -- "$FILTER" <<< "$name"; then
        return 0
    fi
    FILTER_MATCH_COUNT=$((FILTER_MATCH_COUNT + 1))

    # Mode filter: if the test specifies "JIT" and we're running
    # --no-jit, skip it. This is for tests that exercise code paths
    # the interpreter doesn't handle correctly (e.g., signal frame
    # stack corruption).
    if { [ "$mode" = "JIT" ] || [ "$mode" = "HOST_JIT" ]; } && [[ " ${EMU_FLAGS[*]} " == *" --no-jit "* ]]; then
        SKIP_COUNT=$((SKIP_COUNT + 1))
        if [ "$VERBOSE" = "1" ]; then
            echo -e "  ${C_YLW}SKIP${C_RST}    $name"
            echo -e "         (JIT-only test, skipped under --no-jit)"
        fi
        return 0
    fi

    # NEW (Turn 74): DYN mode = dynamically-linked binary that requires
    # BIFROST_ROOT to be set. Skip if BIFROST_ROOT is not in RUN_ENV
    # (these are tested separately in the "Real-world dynamic" section).
    local has_bifrost_root=0 env_entry
    for env_entry in "${RUN_ENV[@]}"; do
        [[ "$env_entry" == BIFROST_ROOT=* ]] && has_bifrost_root=1
    done
    if [ "$mode" = "DYN" ] && [ "$has_bifrost_root" -eq 0 ]; then
        return 0  # silent skip — will run in the dynamic section
    fi

    # Skip if the test binary doesn't exist (e.g., Tests/Unit/*.elf files
    # are gitignored and must be cross-compiled on demand with
    # `make cross SRC=... OUT=...`). This keeps `make check` useful
    # even when the developer hasn't built every optional test.
    #
    # The file field may contain args (e.g. "Tests/Apps/toybox echo hello"),
    # so we extract just the first whitespace-separated token as the
    # binary path to test for existence.
    read -r -a test_command <<< "$file"
    local bin_path="${test_command[0]}"
    if [ ! -f "$bin_path" ]; then
        local optional_fixture=0 optional_pattern
        for optional_pattern in "${ALLOW_MISSING[@]}"; do
            if grep -qiE -- "$optional_pattern" <<< "$name"; then
                optional_fixture=1
                break
            fi
        done
        if [ "$STRICT" = "1" ] && [ "$optional_fixture" = "0" ]; then
            record_failure "$name" "required binary $bin_path is missing"
        else
            SKIP_COUNT=$((SKIP_COUNT + 1))
        fi
        if [ "$optional_fixture" = "1" ]; then
            echo -e "  ${C_YLW}SKIP${C_RST}    $name (optional fixture $bin_path is missing)"
        elif [ "$VERBOSE" = "1" ] && [ "$STRICT" != "1" ]; then
            echo -e "  ${C_YLW}SKIP${C_RST}    $name"
            echo -e "         (binary $bin_path not found — build with 'make cross SRC=...')"
        fi
        return 0
    fi

    # Ignored guest fixtures can outlive source edits. Running an old ELF
    # tests different code and can resurrect already-replaced regressions.
    local fixture_source
    if [[ "$bin_path" == *.elf ]]; then
        for fixture_source in "${bin_path%.elf}.c" "${bin_path%.elf}.S"; do
            if [ -f "$fixture_source" ] && [ "$fixture_source" -nt "$bin_path" ]; then
                record_failure "$name" "stale fixture $bin_path; rebuild with make setup-tests (source: $fixture_source)"
                return 0
            fi
        done
    fi

    # Run with timeout. We use `-s KILL` (SIGKILL) instead of the default
    # SIGTERM because the emulator catches SIGTERM and forwards it to the
    # guest — if the guest doesn't exit, the emulator keeps running and
    # the test hangs. SIGKILL can't be caught, so it always kills the
    # emulator process.
    #
    # GNU `timeout` (without --foreground) creates a new process group for
    # the child and sends the signal to the entire group. This ensures
    # that forked child processes (guest fork()) are also killed — without
    # this, a hung test would leave orphaned processes that keep the test
    # runner hanging forever.
    # Use a temp file to capture output so we can get the real
    # exit code from timeout (PIPESTATUS is lost inside command
    # substitutions which run in subshells).
    local tmpout tmpin=""
    local -a test_runner=("$EMU" "${EMU_FLAGS[@]}" "${test_command[@]}")
    local -a test_env=("${RUN_ENV[@]}")
    # This fixture specifically checks region codegen, which is opt-in.
    [[ "$name" == tier2_smov ]] && test_env+=(BIFROST_TIER2=1)
    [[ "$name" == android_service_manager ]] && test_runner=("$EMU" "${EMU_FLAGS[@]}" --android-services "${test_command[@]}")
    # INTERP rows check interpreter semantics the JIT does not have yet
    # (see the row comment); they run under --no-jit in every mode.
    if [ "$mode" = "INTERP" ] && [[ " ${EMU_FLAGS[*]} " != *" --no-jit "* ]]; then
        test_runner=("$EMU" "${EMU_FLAGS[@]}" --no-jit "${test_command[@]}")
    fi
    if [ "$mode" = "HOST_JIT" ]; then
        # Host harnesses invoke the requested emulator themselves. This
        # zstd harness checks both engines, so run it once in the JIT suite.
        test_runner=(bash "${test_command[@]}")
        test_env+=("BIFROST_EMU=$EMU")
    fi
    tmpout=$(mktemp "${TMPDIR:-/tmp}/bifrost-test-out.XXXXXX") || {
        record_failure "$name" "could not create test output file"
        return 0
    }
    TEMP_FILES+=("$tmpout")
    if [ -n "$stdin" ]; then
        tmpin=$(mktemp "${TMPDIR:-/tmp}/bifrost-test-in.XXXXXX") || {
            record_failure "$name" "could not create test input file"
            return 0
        }
        TEMP_FILES+=("$tmpin")
        printf '%b' "$stdin" > "$tmpin"
        env "${test_env[@]}" timeout -s KILL "$tout" \
            "${test_runner[@]}" < "$tmpin" > "$tmpout" 2>&1
    else
        env "${test_env[@]}" timeout -s KILL "$tout" \
            "${test_runner[@]}" </dev/null > "$tmpout" 2>&1
    fi
    rc=$?
    output=$(tr -d '\0' < "$tmpout")
    rm -f -- "$tmpout"
    if [ -n "$tmpin" ]; then
        rm -f -- "$tmpin"
    fi

    # Determine pass/fail
    local status="PASS"
    local reason=""

    # Timeout is OK for known-infinite tests (pattern starts with ^ and
    # the test name is "toybox_yes"). Otherwise timeout = fail.
    # Exit code 124 = timeout's own "timed out" code.
    # Exit code 137 = killed by SIGKILL (128+9), which we use with
    # `timeout -s KILL` to force-kill hung tests.
    if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
        if [ "$name" = "toybox_yes" ]; then
            rc=0  # treat as success, pattern check below validates output
        else
            status="FAIL"
            reason="timeout (rc=$rc)"
        fi
    fi
    # Exit code 77 = skip (autoconf convention). Treat as success
    # for counting purposes — the test is not applicable in this
    # environment (e.g., SDL/GL unavailable, no display).
    if [ $rc -eq 77 ]; then
        status="SKIP"
        reason="exit 77 (skip)"
    fi
    if [ $rc -ne 0 ] && [ "$status" = "PASS" ]; then
        status="FAIL"
        reason="exit code $rc"
    elif [ "$status" = "PASS" ] && [ -n "$pattern" ]; then
        if ! echo "$output" | grep -qE "$pattern"; then
            status="FAIL"
            reason="pattern '$pattern' not found"
        fi
    fi

    # Check for explicit FAIL/ERROR in output (unless balanced by PASS/OK)
    if [ "$status" = "PASS" ] && echo "$output" | grep -qiE "FAIL|ERROR|segfault|decode error"; then
        if ! echo "$output" | grep -qiE "PASS|OK|ALL.*PASS"; then
            status="FAIL"
            reason="output contains FAIL/ERROR"
        fi
    fi

    # Print result
    local color="$C_GRN"
    [ "$status" = "FAIL" ] && color="$C_RED"
    [ "$status" = "SKIP" ] && color="$C_YLW"
    printf "  ${color}%-8s${C_RST} ${C_DIM}%-40s${C_RST}" "$status" "$name"
    if [ "$status" = "FAIL" ]; then
        printf " ${C_RED}%s${C_RST}\n" "$reason"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_TESTS+=("$name ($reason)")
    elif [ "$status" = "SKIP" ]; then
        printf " ${C_YLW}%s${C_RST}\n" "$reason"
        SKIP_COUNT=$((SKIP_COUNT + 1))
    else
        printf "\n"
        PASS_COUNT=$((PASS_COUNT + 1))
    fi

    # Verbose: show output
    if [ "$VERBOSE" = "1" ] || [ "$status" = "FAIL" ]; then
        echo "$output" | head -20 | sed 's/^/      /'
        if [ "$(echo "$output" | wc -l)" -gt 20 ]; then
            echo "      ... (truncated)"
        fi
    fi
}

run_category() {
    local title="$1"; shift
    local tests=("$@")
    local count=${#tests[@]}
    [ $count -eq 0 ] && return

    if [ -n "$FILTER" ]; then
        local selected=0 entry test_name
        for entry in "${tests[@]}"; do
            IFS='|' read -r test_name _ <<< "$entry"
            if grep -qiE -- "$FILTER" <<< "$test_name"; then
                selected=$((selected + 1))
            fi
        done
        [ "$selected" -eq 0 ] && return
        echo -e "\n${C_BOLD}${C_BLU}[$title]${C_RST} ($selected selected of $count)"
    else
        echo -e "\n${C_BOLD}${C_BLU}[$title]${C_RST} ($count tests)"
    fi
    for t in "${tests[@]}"; do
        IFS='|' read -r name file stdin tout pattern mode <<< "$t"
        run_test "$name" "$file" "$stdin" "$tout" "$pattern" "$mode"
    done
}

# ── Run ────────────────────────────────────────────────────────────────
echo -e "${C_BOLD}bifrost-emu test runner${C_RST}"
mode_label="JIT default"
[ "${#EMU_FLAGS[@]}" -gt 0 ] && mode_label="${EMU_FLAGS[*]}"
[ "${#RUN_ENV[@]}" -gt 0 ] && mode_label="$mode_label ${RUN_ENV[*]}"
echo -e "${C_DIM}mode: $mode_label${C_RST}"
echo -e "${C_DIM}emulator: $("$EMU" --version 2>/dev/null || echo "$EMU")${C_RST}"

START=$(date +%s)

[ "$RUN_UNIT" = "1" ]        && run_category "Unit tests"        "${UNIT_TESTS[@]}"
[ "$RUN_INTEGRATION" = "1" ] && run_category "Integration tests" "${INTEGRATION_TESTS[@]}"
# Sandbox confinement tests — provision a scratch root with an escaping
# symlink + outside canary, run with BIFROST_ROOT pointed at it, clean up.
# The test binary takes no fixture paths (fixed guest-absolute paths).
if [ "$RUN_INTEGRATION" = "1" ]; then
    SBX=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-sbx-test.XXXXXX")
    if [ -z "$SBX" ]; then
        record_failure "test_sandbox" "could not create sandbox fixture directory"
    else
        TEMP_DIRS+=("$SBX")
        if mkdir -p "$SBX/root/sub" "$SBX/outside" && \
                printf 'hello\n' > "$SBX/root/sub/real.txt" && \
                printf 'CANARY-DO-NOT-READ\n' > "$SBX/outside/secret" && \
                chmod 644 "$SBX/outside/secret" && \
                ln -sf "$SBX/outside/secret" "$SBX/root/sub/evil" && \
                ln -sf "$SBX/outside" "$SBX/root/sub/outlink"; then
            OLD_RUN_ENV=("${RUN_ENV[@]}")
            RUN_ENV=("BIFROST_ROOT=$SBX/root" "${RUN_ENV[@]}")
            run_category "Sandbox" "test_sandbox|Tests/Unit/test_sandbox.elf||10|checks passed"
            RUN_ENV=("${OLD_RUN_ENV[@]}")
            # The guest only sees its failures; confirm on the host that no
            # escaping call modified, renamed or linked the outside canary.
            if [ "$(cat "$SBX/outside/secret" 2>/dev/null)" != "CANARY-DO-NOT-READ" ] ||
                    [ "$(stat -c '%a %h' "$SBX/outside/secret" 2>/dev/null)" != "644 1" ] ||
                    [ "$(ls -A "$SBX/outside")" != "secret" ]; then
                record_failure "test_sandbox" "outside canary was modified through the sandbox"
            fi
        else
            record_failure "test_sandbox" "could not provision sandbox fixtures"
        fi
    fi
fi
# BIFROST_NO_SHARED_JIT=1 gives each thread its own JIT; other threads
# still invalidate it while it runs.
if [ "$RUN_UNIT" = "1" ]; then
    OLD_RUN_ENV=("${RUN_ENV[@]}")
    RUN_ENV=("BIFROST_NO_SHARED_JIT=1" "${RUN_ENV[@]}")
    run_category "Per-thread JIT" \
        "test_jit_thread_invalidate_perthread|Tests/Unit/test_jit_thread_invalidate.elf||60|ALL PASS"
    RUN_ENV=("${OLD_RUN_ENV[@]}")
fi
[ "$RUN_INTERACTIVE" = "1" ] && run_category "Interactive tests" "${INTERACTIVE_TESTS[@]}"
[ "$RUN_TOYBOX" = "1" ]      && run_category "Toybox tests"      "${TOYBOX_TESTS[@]}"
[ "$RUN_REALWORLD" = "1" ]   && run_category "Real-world binaries" "${REALWORLD_TESTS[@]}"

# NEW (Turn 74): if rootfs exists, run the dynamically-linked glibc
# real-world tests (iperf3, coreutils) with BIFROST_ROOT set so they
# can find their shared libraries. These are SEPARATE from the static
# real-world tests above (which don't need BIFROST_ROOT).
if [ "$RUN_REALWORLD" = "1" ] && [ -d "rootfs/lib" ]; then
    OLD_RUN_ENV=("${RUN_ENV[@]}")
    RUN_ENV=("BIFROST_ROOT=$PWD/rootfs" "${RUN_ENV[@]}")
    DYN_REALWORLD_TESTS=()
    for t in "${REALWORLD_TESTS[@]}"; do
        case "$t" in
            rw_iperf3_*|rw_coreutils_*) DYN_REALWORLD_TESTS+=("$t") ;;
        esac
    done
    if [ ${#DYN_REALWORLD_TESTS[@]} -gt 0 ]; then
        run_category "Real-world dynamic (glibc)" "${DYN_REALWORLD_TESTS[@]}"
    fi
    RUN_ENV=("${OLD_RUN_ENV[@]}")
fi

[ "$RUN_BENCH" = "1" ]       && run_category "Benchmarks"        "${BENCH_TESTS[@]}"

# Dynamic linking tests — use BIFROST_ROOT env prefix.
# These run the same test binary but with BIFROST_ROOT set to the
# rootfs directory, enabling shared library loading.
if [ "$RUN_DYNAMIC" = "1" ]; then
    # Save the original environment and append BIFROST_ROOT.
    OLD_RUN_ENV=("${RUN_ENV[@]}")
    RUN_ENV=("BIFROST_ROOT=$PWD/rootfs" "${RUN_ENV[@]}")
    run_category "Dynamic linking" "${DYNAMIC_TESTS[@]}"
    RUN_ENV=("${OLD_RUN_ENV[@]}")
fi

if [ -n "$FILTER" ] && [ "$FILTER_MATCH_COUNT" -eq 0 ]; then
    echo "error: no tests matched --filter: $FILTER" >&2
    exit 2
fi

END=$(date +%s)
ELAPSED=$((END - START))

# ── Summary ────────────────────────────────────────────────────────────
TOTAL=$((PASS_COUNT + FAIL_COUNT))
echo -e "\n${C_BOLD}────────────────────────────────────────${C_RST}"
echo -ne "${C_BOLD}Total: ${TOTAL}  "
echo -ne "${C_GRN}Pass: $PASS_COUNT${C_RST}  "
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -ne "${C_RED}Fail: $FAIL_COUNT${C_RST}"
else
    echo -ne "${C_DIM}Fail: $FAIL_COUNT${C_RST}"
fi
if [ "$SKIP_COUNT" -gt 0 ]; then
    echo -ne "  ${C_DIM}Skip: $SKIP_COUNT${C_RST}"
fi
echo -e "  ${C_DIM}(${ELAPSED}s)${C_RST}"

if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "\n${C_RED}Failed tests:${C_RST}"
    for t in "${FAILED_TESTS[@]}"; do
        echo -e "  ${C_RED}✗${C_RST} $t"
    done
    echo -e "\n${C_RED}✗ TESTS FAILED${C_RST}"
    exit 1
else
    echo -e "\n${C_GRN}✓ ALL TESTS PASSED${C_RST}"
    exit 0
fi
