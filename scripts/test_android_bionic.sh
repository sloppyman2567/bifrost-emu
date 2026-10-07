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
        for services in off on; do
            service_args=()
            service_env=(BIFROST_GUEST_ENV=BIFROST_ANDROID_DEVICE_TEST=1)
            if [[ "$services" == on ]]; then
                service_args=(--android-services)
                service_env=('BIFROST_GUEST_ENV=BIFROST_ANDROID_DEVICE_TEST=1;BIFROST_ANDROID_SERVICES_TEST=1')
            fi
            log="$task_tmp/$aslr-$mode-$services.log"
            env -u BIFROST_ROOT -u BIFROST_NO_ASLR -u BIFROST_ANDROID_SERVICES_TEST BIFROST_TIER2="$tier" \
                BIFROST_ANDROID_DEVICE_TEST=1 BIFROST_TIER2_HITS=32 "${settings[@]}" "${service_env[@]}" timeout -s KILL 30 \
                "$emu" "${args[@]}" "${service_args[@]}" --android-runtime --rootfs "$runtime" \
                /data/local/tmp/bootstrap > "$log" 2>&1 || { cat "$log" >&2; exit 1; }
            rg -q '^android_bionic_bootstrap: ALL PASS$' "$log" || { cat "$log" >&2; exit 1; }
            rg -q '^Bionic: Binder device and prctl passed$' "$log" || { cat "$log" >&2; exit 1; }
            if rg -q 'Fatal signal|CANNOT LINK|\[crash\]' "$log"; then cat "$log" >&2; exit 1; fi
            echo "Bionic bootstrap: $aslr $mode services=$services passed"
        done
    done
done
# Managed libaaudio adapter: data callbacks on the adapter's own Bionic thread.
python3 tools/build-android-adapters.py --runtime-root "$runtime" >/dev/null
mkdir -p "$runtime/data/local/tmp/aaudio"
cp build/android-adapters/managed/libaaudio.so "$runtime/data/local/tmp/aaudio/"
clang --target=aarch64-linux-android35 -fuse-ld=lld -nostdlib -fPIE -pie -O2 \
    -Wl,-z,max-page-size=16384 -Wl,--dynamic-linker=/system/bin/linker64 \
    -Wl,-rpath,/data/local/tmp/aaudio:/apex/com.android.runtime/lib64/bionic \
    -Wl,--allow-shlib-undefined \
    -mno-outline-atomics ctest/android_bionic_start.S ctest/android_aaudio_thread.c \
    "$runtime/data/local/tmp/aaudio/libaaudio.so" \
    "$runtime/apex/com.android.runtime/lib64/bionic/libc.so" \
    "$runtime/apex/com.android.runtime/lib64/bionic/libdl.so" \
    -o "$runtime/data/local/tmp/aaudio_thread"
for mode in jit interp tier2; do
    args=()
    [[ "$mode" != interp ]] || args=(--no-jit)
    tier=0
    [[ "$mode" != tier2 ]] || tier=1
    log="$task_tmp/aaudio-$mode.log"
    env -u BIFROST_ROOT SDL_AUDIODRIVER=dummy BIFROST_TIER2="$tier" BIFROST_TIER2_HITS=32 \
        timeout -s KILL 60 "$emu" "${args[@]}" --android-runtime --rootfs "$runtime" \
        /data/local/tmp/aaudio_thread > "$log" 2>&1 || { cat "$log" >&2; exit 1; }
    rg -q '^android_aaudio_thread: ALL PASS$' "$log" || { cat "$log" >&2; exit 1; }
    if rg -q 'Fatal signal|CANNOT LINK|\[crash\]' "$log"; then cat "$log" >&2; exit 1; fi
    echo "Bionic AAudio callback thread: $mode passed"
done
