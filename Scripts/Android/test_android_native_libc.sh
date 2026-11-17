#!/usr/bin/env bash
# Bionic's memcpy, memmove and memset under linker64: the host-native path (Source/System/Core/NativeLibc.hpp) finds them
# in the mapped libc.so's symbol table (Bionic's own linker loads it, so the emulator's DynamicLinker never sees them).
# The fixture gives the same results with the path on and off; it runs where it should and stays off where it must
# (BIFROST_NO_NATIVE_LIBC, the interpreter). Needs clang, lld and the pinned runtime (Tools/Android/fetch-android-runtime.py).
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
emu=${BIFROST_EMU:-$repo/bifrost-emu}
runtime=${BIFROST_ANDROID_ROOT:-$repo/Tools/android-runtime/rootfs}
logs=$(mktemp -d /tmp/bifrost-android-native-libc.XXXXXX)
trap 'rm -rf -- "$logs"' EXIT
python3 Tools/Android/fetch-android-runtime.py --destination "$runtime" > /dev/null
mkdir -p "$runtime/data/local/tmp"
clang --target=aarch64-linux-android35 -fuse-ld=lld -nostdlib -ffreestanding -fno-builtin -fPIE -pie -O2 \
    -Wl,-z,max-page-size=16384 -Wl,--dynamic-linker=/system/bin/linker64 \
    -Wl,-rpath,/apex/com.android.runtime/lib64/bionic -Wl,--allow-shlib-undefined \
    Tests/Unit/android_bionic_start.S Tests/Unit/android_bionic_memory.c \
    "$runtime/apex/com.android.runtime/lib64/bionic/libc.so" \
    "$runtime/apex/com.android.runtime/lib64/bionic/libdl.so" \
    -o "$runtime/data/local/tmp/memory_fixture"

# run NAME EXPECT_NATIVE(yes|no) [ENV...] -- [EMULATOR ARGS...]
run() {
    local name=$1 expect=$2; shift 2
    local -a env_args=() emu_args=()
    while (($#)) && [[ "$1" != -- ]]; do env_args+=("$1"); shift; done
    [[ "${1:-}" == -- ]] && shift
    emu_args=("$@")
    local rc=0
    env -u BIFROST_ROOT "${env_args[@]}" timeout -s KILL 120 "$emu" --verbose "${emu_args[@]}" \
        --android-runtime --rootfs "$runtime" /data/local/tmp/memory_fixture > "$logs/$name.log" 2>&1 || rc=$?
    if ((rc != 0)) || ! grep -qE '^android_bionic_memory: [0-9]+ checks: ALL PASS$' "$logs/$name.log" ||
       grep -qE 'Fatal signal|CANNOT LINK|\[crash\]' "$logs/$name.log"; then
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

run jit yes
run jit-off no BIFROST_NO_NATIVE_LIBC=1
run interpreter no -- --no-jit
echo "android_native_libc: ALL PASS"
