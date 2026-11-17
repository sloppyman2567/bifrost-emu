#!/usr/bin/env bash
# Host-native memcpy/memmove/memset (Source/System/Core/NativeLibc.hpp): the fixtures give the same results with the
# native path on and off, it runs where it should (a static musl binary through its .symtab, a dynamic glibc binary
# through the linker's ifunc-resolved symbols, several threads) and stays off where it must (BIFROST_NO_NATIVE_LIBC,
# the interpreter, differential replay, which would re-run the guest routine for one block and diverge).
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
emu=${BIFROST_EMU:-$repo/bifrost-emu}
if [[ "$emu" != /* ]]; then emu="$PWD/$emu"; fi
# Relative to the repository root: a sandboxed run (BIFROST_ROOT) resolves an absolute path inside the rootfs.
single=Tests/Unit/jit_native_libc.elf
glibc=Tests/Unit/jit_native_libc_glibc.elf
threads=Tests/Unit/test_native_libc_mt.elf
for f in "$single" "$threads"; do
    if [[ ! -f "$repo/$f" ]]; then echo "Missing $(basename "$f"); build with make setup-tests" >&2; exit 1; fi
done
logs=$(mktemp -d /tmp/bifrost-native-libc-test.XXXXXX)
trap 'rm -rf -- "$logs"' EXIT

# run NAME EXPECT_NATIVE(yes|no) FIXTURE [ENV...] -- [EMULATOR ARGS...]
run() {
    local name=$1 expect=$2 fixture=$3; shift 3
    local -a env_args=() emu_args=()
    while (($#)) && [[ "$1" != -- ]]; do env_args+=("$1"); shift; done
    [[ "${1:-}" == -- ]] && shift
    emu_args=("$@")
    local rc=0
    cd "$repo"
    env -u BIFROST_ROOT "${env_args[@]}" timeout -s KILL 120 "$emu" --verbose "${emu_args[@]}" "$fixture" \
        > "$logs/$name.log" 2>&1 || rc=$?
    if ((rc != 0)) || ! grep -Eq 'ALL PASS' "$logs/$name.log"; then
        echo "FAIL: $name (exit $rc)"; tail -20 "$logs/$name.log"; exit 1
    fi
    local calls
    calls=$(sed -n 's/.*native libc: \([0-9]*\) memcpy.*/\1/p' "$logs/$name.log" | tail -1)
    if [[ "$expect" == yes ]]; then
        if [[ -z "$calls" || "$calls" -lt 100 ]]; then
            echo "FAIL: $name: the native path did not run (calls=${calls:-none})"; tail -20 "$logs/$name.log"; exit 1
        fi
        echo "$name: ALL PASS, $calls native calls"
    else
        if [[ -n "$calls" ]]; then echo "FAIL: $name: the native path ran ($calls calls) where it must not"; exit 1; fi
        echo "$name: ALL PASS, native path off"
    fi
}

run static yes "$single"
run static-off no "$single" BIFROST_NO_NATIVE_LIBC=1
run interpreter no "$single" -- --no-jit
run verify no "$single" BIFROST_JIT_VERIFY=1 BIFROST_NO_DIRECT_CALL=1
run threads yes "$threads"
run threads-off no "$threads" BIFROST_NO_NATIVE_LIBC=1
# The dynamic glibc binary needs the rootfs (its libc); the guest's memcpy and memset are ifuncs there.
if [[ -f "$repo/$glibc" && -d "$repo/rootfs/lib" ]]; then
    run glibc yes "$glibc" BIFROST_ROOT="$repo/rootfs"
    run glibc-off no "$glibc" BIFROST_ROOT="$repo/rootfs" BIFROST_NO_NATIVE_LIBC=1
else
    echo "glibc: skipped (no Tests/Unit/jit_native_libc_glibc.elf or rootfs)"
fi
echo "jit_native_libc: ALL PASS"
