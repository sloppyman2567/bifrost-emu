#!/usr/bin/env bash
# run_tests.sh — bifrost-emu test runner
#
# A clean, colorized test runner that:
#   - Categorizes tests (unit / integration / interactive / toybox / bench)
#   - Detects pass/fail via exit code + output keyword scan
#   - Prints a summary table with counts and timing
#   - Supports filtering by category (--unit, --toybox, etc.)
#   - Supports running under interpreter (--no-jit) or FWD (--fwd)
#
# Usage:
#   ./scripts/run_tests.sh              # run everything (default = JIT)
#   ./scripts/run_tests.sh --unit       # only unit tests (ctest/)
#   ./scripts/run_tests.sh --toybox     # only toybox integration tests
#   ./scripts/run_tests.sh --no-jit     # run under interpreter
#   ./scripts/run_tests.sh --fwd        # run with BIFROST_ENABLE_FWD=1
#   ./scripts/run_tests.sh --verbose    # show full output of each test
#   ./scripts/run_tests.sh --filter foo # only run tests matching "foo"
#   ./scripts/run_tests.sh --quick      # skip bench + slow tests
#
# Exit code: 0 if all tests pass, 1 if any fail.

set -u

# ── Config ─────────────────────────────────────────────────────────────
EMU="./bifrost-emu"
TIMEOUT=15
TIMEOUT_LONG=60

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
RUN_ALL=1
VERBOSE=0
QUICK=0
FILTER=""
EMU_FLAGS=""
ENV_PREFIX=""

while [ $# -gt 0 ]; do
    case "$1" in
        --unit)         RUN_UNIT=1; RUN_ALL=0 ;;
        --integration)  RUN_INTEGRATION=1; RUN_ALL=0 ;;
        --interactive)  RUN_INTERACTIVE=1; RUN_ALL=0 ;;
        --toybox)       RUN_TOYBOX=1; RUN_ALL=0 ;;
        --bench)        RUN_BENCH=1; RUN_ALL=0 ;;
        --no-jit)       EMU_FLAGS="--no-jit" ;;
        --fwd)          ENV_PREFIX="BIFROST_ENABLE_FWD=1" ;;
        --verbose|-v)   VERBOSE=1 ;;
        --quick)        QUICK=1 ;;
        --filter)       FILTER="$2"; shift ;;
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

if [ "$RUN_ALL" = "1" ]; then
    RUN_UNIT=1; RUN_INTEGRATION=1; RUN_INTERACTIVE=0; RUN_TOYBOX=1
    [ "$QUICK" = "0" ] && RUN_BENCH=1
fi

# ── Check emulator exists ──────────────────────────────────────────────
if [ ! -x "$EMU" ]; then
    echo -e "${C_RED}error: $EMU not found — run 'make' first${C_RST}"
    exit 1
fi

# ── Test definitions ───────────────────────────────────────────────────
# Each test: name | file | stdin | timeout | expected_pattern (regex)
# A test PASSES if:
#   - exit code is 0, AND
#   - output contains expected_pattern (or expected_pattern is empty)
# A test FAILS if output contains "FAIL" or "ERROR" (case-insensitive)
# and no "PASS"/"OK"/"ALL.*PASS" counterbalances it.

# Unit tests (ctest/ — focused JIT regression tests)
UNIT_TESTS=(
    "hello|ctest/hello.elf||5"
    "addsub_imm|ctest/jit_addsub_imm.elf||5|PASS"
    "bitfield|ctest/jit_bitfield.elf||5|PASS"
    "block_split|ctest/jit_block_split.elf||5|PASS"
    "carry|ctest/jit_carry.elf||5|PASS"
    "cls|ctest/jit_cls.elf||5|PASS"
    "csel|ctest/jit_csel.elf||5|PASS"
    "extend|ctest/jit_extend.elf||5|PASS"
    "fma|ctest/jit_fma.elf||5|PASS"
    "fp_scalar|ctest/jit_fp_scalar.elf||5|PASS"
    "int_fp_conv|ctest/jit_int_fp_conv.elf||5|ALL TESTS PASSED"
    "ldp_stp|ctest/jit_ldp_stp.elf||5|PASS"
    "madd|ctest/jit_madd.elf||5|PASS"
    "neon|ctest/jit_neon.elf||5|PASS"
    "rev|ctest/jit_rev.elf||5|PASS"
    "simd|ctest/jit_simd.elf||5|PASS"
    "loop|ctest/loop.elf||5|Loop value"
    "test_fb|ctest/test_fb.elf||5|mode:"
    "test_float|ctest/test_float.elf||5|3.14"
    "test_jit_native|ctest/test_jit_native.elf||10|ALL PASS"
    "test_malloc|ctest/test_malloc.elf||5|malloc test done"
    "test_simd_arith|ctest/test_simd_arith.elf||10|ALL PASS"
    "test_tls_static|ctest/test_tls_static.elf||10|ALL PASS"
    # Multi-threaded pthread test (default: 4 threads, fib(35)).
    # Exercises clone/clone3 + futex (FUTEX_WAIT/WAKE/REQUEUE) +
    # set_tid_address + per-thread JIT + __tl_lock release-via-ctid.
    "test_pthread|ctest/test_pthread.elf||15|ALL PASS"
    # POSIX threads primitive tests (Task 4). Cross-compiled with musl.
    # Each tests a specific pthread/semaphore primitive at a conservative
    # scale that stays within the emulator's current futex-wakeup throughput.
    "test_pthread_mutex|ctest/test_pthread_mutex.elf||10|ALL PASS"
    "test_pthread_cond|ctest/test_pthread_cond.elf||10|ALL PASS"
    "test_pthread_rwlock|ctest/test_pthread_rwlock.elf||10|ALL PASS"
    "test_sem|ctest/test_sem.elf||10|ALL PASS"
    "test_pthread_once|ctest/test_pthread_once.elf||10|ALL PASS"
    "test_producer_consumer|ctest/test_producer_consumer.elf||10|ALL PASS"
    # High-contention atomic stress test (8 threads). Validates CAS, LL/SC
    # (mutex), and LDADD atomics under game-scale contention.
    "test_atomic_stress|ctest/test_atomic_stress.elf||30|ALL PASS"
    # LSE inline-asm test — forces CAS/LDADD/LDSET/LDCLR/LDEOR/SWP via
    # -march=armv8.1-a+lse, testing the JIT's native lock-prefixed codegen.
    "test_lse_inline|ctest/test_lse_inline.elf||10|ALL PASS"
)

# Integration tests (ctest_real/ — real-world test programs)
INTEGRATION_TESTS=(
    "audio_test|ctest_real/audio_test.elf||10|audio_test: done"
    "div_loop|ctest_real/div_loop.elf||5"
    "div_loop2|ctest_real/div_loop2.elf||5"
    "div_simple|ctest_real/div_simple.elf||5"
    "div_test|ctest_real/div_test.elf||5"
    "dynlink_test|ctest_real/test_dynlink.elf||5|test_dynlink: ALL PASS"
    "fcvtzu_test|ctest_real/fcvtzu_test.elf||5|OK"
    "fcvtzu_test2|ctest_real/fcvtzu_test2.elf||5|passed"
    "fib|ctest_real/fib.elf||5"
    "fwd_repro|ctest_real/fwd_repro.elf||5"
    "fwd_repro2|ctest_real/fwd_repro2.elf||5"
    "fwd_repro3|ctest_real/fwd_repro3.elf||5"
    "fwd_repro4|ctest_real/fwd_repro4.elf||5"
    "fwd_repro5|ctest_real/fwd_repro5.elf||5"
    "head|ctest_real/head.elf||5"
    "input_test|ctest_real/test_input.elf||5|test_input: done"
    "gamepad_test|ctest_real/test_gamepad.elf||5|test_gamepad: done"
    "sdl_demo|ctest_real/test_sdl_demo.elf||15|drew 60 frames"
    "jit_new_ops|ctest_real/jit_new_ops.elf||5|ALL TESTS PASSED"
    "loop_div|ctest_real/loop_div.elf||5"
    "md5_neon_test|ctest_real/md5_neon_test.elf||5"
    "md5_scalar_test|ctest_real/md5_scalar_test.elf||5|5d41402abc4b2a76b9719d911017c592"
    "rev_real|ctest_real/rev.elf||5"
    "ror_imm_test|ctest_real/ror_imm_test.elf||5|OK"
    "simple_div|ctest_real/simple_div.elf||5|100 / 7 = 14"
    "sin_test|ctest_real/sin_test.elf||10|K\\[7\\] = 0xfd469501"
    "sort|ctest_real/sort.elf||10"
    "strtod_nan_test|ctest_real/strtod_nan_test.elf||5"
    "ubfiz_test|ctest_real/ubfiz_test.elf||5|OK"
    "wc|ctest_real/wc.elf||5"
    # Basic test/ programs (non-interactive)
    "count|test/count.elf||5"
    "extr|test/extr.elf||5|OK"
    "fib_basic|test/fib.elf||5|832040"
    "hello|test/hello.elf||5|Hello, ARM64"
)

# Interactive tests (need stdin input)
INTERACTIVE_TESTS=(
    "echo|test/echo.elf|q\n|5|echo>"
    "repl|test/repl.elf|q\n|5"
    "cat|test/cat.elf|/etc/hostname|5"
    "sh|ctest_real/sh.elf|exit\n|10"
    "fgets_test|ctest_real/fgets_test.elf||5"
)

# Toybox integration tests
TOYBOX_TESTS=(
    "toybox_echo|ctest_real/toybox echo hello|hello|5|hello"
    "toybox_seq|ctest_real/toybox seq 1 5||5|^[12345]$"
    "toybox_ls|ctest_real/toybox ls /||5|^bin$"
    "toybox_md5sum|ctest_real/toybox md5sum|hello\n|5|b1946ac92492d2347c6235b4d2611184"
    "toybox_sha256sum|ctest_real/toybox sha256sum|hello\n|5|5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03"
    "toybox_wc|ctest_real/toybox wc|hello world\n|5|^[[:space:]]*1"
    "toybox_sort|ctest_real/toybox sort|3\n1\n2\n|5|^1$"
    "toybox_uname|ctest_real/toybox uname||5|Linux"
    # toybox yes is infinite — timeout is expected, just check it produced "y"
    "toybox_yes|ctest_real/toybox yes||2|^y$"
)

# Benchmarks (slow, skipped with --quick)
BENCH_TESTS=(
    "bench_mips|ctest_real/bench_mips.elf||30|done:"
)

# ── Helpers ────────────────────────────────────────────────────────────
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
FAILED_TESTS=()

run_test() {
    local name="$1" file="$2" stdin="$3" tout="$4" pattern="${5:-}"
    local full="$EMU $EMU_FLAGS $file"
    local output rc

    # Apply filter
    if [ -n "$FILTER" ] && ! echo "$name" | grep -qi "$FILTER"; then
        return 0
    fi

    # Skip if the test binary doesn't exist (e.g., ctest/*.elf files
    # are gitignored and must be cross-compiled on demand with
    # `make cross SRC=... OUT=...`). This keeps `make check` useful
    # even when the developer hasn't built every optional test.
    #
    # The file field may contain args (e.g. "ctest_real/toybox echo hello"),
    # so we extract just the first whitespace-separated token as the
    # binary path to test for existence.
    local bin_path="${file%% *}"
    if [ ! -f "$bin_path" ]; then
        SKIP_COUNT=$((SKIP_COUNT + 1))
        if [ "$VERBOSE" = "1" ]; then
            echo -e "  ${C_YLW}SKIP${C_RST}    $name"
            echo -e "         (binary $bin_path not found — build with 'make cross SRC=...')"
        fi
        return 0
    fi

    # Run with timeout
    if [ -n "$stdin" ]; then
        output=$(printf "$stdin" | env $ENV_PREFIX timeout "$tout" $EMU $EMU_FLAGS $file 2>&1)
    else
        output=$(env $ENV_PREFIX timeout "$tout" $EMU $EMU_FLAGS $file </dev/null 2>&1)
    fi
    rc=$?

    # Determine pass/fail
    local status="PASS"
    local reason=""

    # Timeout is OK for known-infinite tests (pattern starts with ^ and
    # the test name is "toybox_yes"). Otherwise timeout = fail.
    if [ $rc -eq 124 ]; then
        if [ "$name" = "toybox_yes" ]; then
            rc=0  # treat as success, pattern check below validates output
        else
            status="FAIL"
            reason="timeout"
        fi
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
    printf "  ${color}%-8s${C_RST} ${C_DIM}%-40s${C_RST}" "$status" "$name"
    if [ "$status" = "FAIL" ]; then
        printf " ${C_RED}%s${C_RST}\n" "$reason"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_TESTS+=("$name ($reason)")
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

    echo -e "\n${C_BOLD}${C_BLU}[$title]${C_RST} ($count tests)"
    for t in "${tests[@]}"; do
        IFS='|' read -r name file stdin tout pattern <<< "$t"
        run_test "$name" "$file" "$stdin" "$tout" "$pattern"
    done
}

# ── Run ────────────────────────────────────────────────────────────────
echo -e "${C_BOLD}bifrost-emu test runner${C_RST}"
echo -e "${C_DIM}mode: ${ENV_PREFIX:-JIT default} $EMU_FLAGS${C_RST}"
echo -e "${C_DIM}emulator: $($EMU --version 2>/dev/null || echo "$EMU")${C_RST}"

START=$(date +%s)

[ "$RUN_UNIT" = "1" ]        && run_category "Unit tests"        "${UNIT_TESTS[@]}"
[ "$RUN_INTEGRATION" = "1" ] && run_category "Integration tests" "${INTEGRATION_TESTS[@]}"
[ "$RUN_INTERACTIVE" = "1" ] && run_category "Interactive tests" "${INTERACTIVE_TESTS[@]}"
[ "$RUN_TOYBOX" = "1" ]      && run_category "Toybox tests"      "${TOYBOX_TESTS[@]}"
[ "$RUN_BENCH" = "1" ]       && run_category "Benchmarks"        "${BENCH_TESTS[@]}"

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
