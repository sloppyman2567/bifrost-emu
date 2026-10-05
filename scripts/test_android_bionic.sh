#!/usr/bin/env bash
# Exercise real Bionic/linker64. Fetch the pinned fixture separately.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
emu=${BIFROST_EMU:-$repo/bifrost-emu}
runtime=${BIFROST_ANDROID_ROOT:-$repo/tools/android-runtime/rootfs}
task_tmp=$(mktemp -d /tmp/bifrost-bionic-test.XXXXXX)
trap 'rm -rf -- "$task_tmp"' EXIT
python3 tools/fetch-android-runtime.py --destination "$runtime"
clang --target=aarch64-linux-android35 -fuse-ld=lld -nostdlib -fPIE -pie -O2 \
    -Wl,-z,max-page-size=16384 -Wl,--dynamic-linker=/system/bin/linker64 \
    -Wl,-rpath,/apex/com.android.runtime/lib64/bionic -Wl,--allow-shlib-undefined \
    ctest/android_bionic_start.S ctest/android_bionic_bootstrap.c \
    "$runtime/apex/com.android.runtime/lib64/bionic/libc.so" \
    "$runtime/apex/com.android.runtime/lib64/bionic/libdl.so" \
    -o "$runtime/data/local/tmp/bootstrap"
for aslr in fixed random; do
    settings=()
    [[ "$aslr" != fixed ]] || settings=(BIFROST_NO_ASLR=1)
    for mode in jit interp tier2; do
        args=()
        [[ "$mode" != interp ]] || args=(--no-jit)
        tier=0
        [[ "$mode" != tier2 ]] || tier=1
        log="$task_tmp/$aslr-$mode.log"
        env -u BIFROST_ROOT -u BIFROST_NO_ASLR BIFROST_TIER2="$tier" \
            BIFROST_ANDROID_DEVICE_TEST=1 BIFROST_TIER2_HITS=32 "${settings[@]}" timeout -s KILL 30 \
            "$emu" "${args[@]}" --android-runtime --rootfs "$runtime" \
            /data/local/tmp/bootstrap > "$log" 2>&1 || { cat "$log" >&2; exit 1; }
        rg -q '^android_bionic_bootstrap: ALL PASS$' "$log" || { cat "$log" >&2; exit 1; }
        if rg -q 'Fatal signal|CANNOT LINK|\[crash\]' "$log"; then cat "$log" >&2; exit 1; fi
        echo "Bionic bootstrap: $aslr $mode passed"
    done
done
