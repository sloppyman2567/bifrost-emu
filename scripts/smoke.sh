#!/bin/bash
# Fast smoke test: minimal subset, runs in under 15s.
set -u
EMU="${1:-./bifrost-emu}"
[ -x "$EMU" ] || { echo "FAIL: emulator not found at $EMU" >&2; exit 1; }

PASS=0; FAIL=0; FAILED_TESTS=()
run_one() {
    local desc="$1"; shift; local expect="$1"; shift
    local timeout_s="${1:-5}"; shift
    local out
    out=$(timeout "$timeout_s" "$EMU" "$@" 2>&1); local rc=$?
    if [ $rc -eq 124 ]; then
        echo "FAIL  $desc (timeout)"; FAIL=$((FAIL+1)); FAILED_TESTS+=("$desc (timeout)"); return
    fi
    if echo "$out" | grep -qF "$expect"; then
        echo "PASS  $desc"; PASS=$((PASS+1))
    else
        echo "FAIL  $desc (expected '$expect') got: $(echo "$out" | head -2 | tr '\n' '|')"
        FAIL=$((FAIL+1)); FAILED_TESTS+=("$desc")
    fi
}

echo "=== Smoke: $EMU ==="
run_one "hello(test)"        "Hello, ARM64!" 4 test/hello.elf
run_one "fib(test)"          "832040"        4 test/fib.elf
run_one "hello(ctest)"       "Hello, ARM64!" 4 ctest/hello.elf
run_one "loop"               "Loop value"    4 ctest/loop.elf
run_one "test_malloc"        "malloc test done" 6 ctest/test_malloc.elf
run_one "test_float"         "3.140000"      6 ctest/test_float.elf
run_one "jit_addsub_imm"     "PASS"          6 ctest/jit_addsub_imm.elf
run_one "jit_bitfield"       "PASS"          6 ctest/jit_bitfield.elf
run_one "jit_block_split"    "PASS"          6 ctest/jit_block_split.elf
run_one "jit_carry"          "PASS"          6 ctest/jit_carry.elf
run_one "jit_cls"            "PASS"          6 ctest/jit_cls.elf
run_one "jit_csel"           "PASS"          6 ctest/jit_csel.elf
run_one "jit_extend"         "PASS"          6 ctest/jit_extend.elf
run_one "jit_fp_scalar"      "PASS"          6 ctest/jit_fp_scalar.elf
run_one "jit_ldp_stp"        "PASS"          6 ctest/jit_ldp_stp.elf
run_one "jit_madd"           "PASS"          6 ctest/jit_madd.elf
run_one "jit_rev"            "PASS"          6 ctest/jit_rev.elf
run_one "jit_simd"           "PASS"          6 ctest/jit_simd.elf
run_one "fib(real)"          "55"            6 ctest_real/fib.elf 10
run_one "hello(JIT)"         "Hello, ARM64!" 5 --jit test/hello.elf
run_one "fib(JIT)"           "832040"        5 --jit test/fib.elf
run_one "fib(real,JIT)"      "55"            8 --jit ctest_real/fib.elf 10

echo
echo "=== $PASS passed, $FAIL failed ==="
[ $FAIL -gt 0 ] && { for t in "${FAILED_TESTS[@]}"; do echo "  - $t"; done; exit 1; }
exit 0
