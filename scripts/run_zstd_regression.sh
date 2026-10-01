#!/usr/bin/env bash
# Exercise the static AArch64 zstd CLI against the host implementation.
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
emu=${BIFROST_EMU:-"$repo_dir/bifrost-emu"}
zstd_bin=${BIFROST_ZSTD:-"$repo_dir/ctest_real/zstd-aarch64"}
host_zstd=${BIFROST_HOST_ZSTD:-zstd}

if [[ "$emu" != /* ]]; then
    if [[ -x "$repo_dir/$emu" ]]; then
        emu="$repo_dir/$emu"
    elif command -v "$emu" >/dev/null 2>&1; then
        emu=$(command -v "$emu")
    fi
fi

if [[ ! -x "$emu" ]]; then
    echo "zstd regression: emulator not executable: $emu" >&2
    exit 1
fi
if [[ ! -x "$zstd_bin" ]]; then
    echo "zstd regression: guest binary not executable: $zstd_bin" >&2
    exit 1
fi
if ! command -v "$host_zstd" >/dev/null 2>&1; then
    echo "zstd regression: host validator not found: $host_zstd (install zstd or set BIFROST_HOST_ZSTD)" >&2
    exit 1
fi

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-zstd.XXXXXX")
trap 'rm -rf -- "$tmp_dir"' EXIT
mkdir -p "$tmp_dir"

# Keep a text-like, varied input so the CLI takes the normal compressed-block
# and Huffman-table paths. The fixed corpus makes the test repeatable.
input="$tmp_dir/input.txt"
for ((line = 0; line < 10000; line++)); do
    printf 'record=%08x; Neverball zstd regression; the quick brown fox jumps over the lazy dog; AArch64 JIT and interpreter round-trip.\n' "$line"
done > "$input"

# Check both threaded and single-thread compression in both engines. The
# host implementation validates every archive and checks the restored bytes.
for engine in jit interp; do
    engine_flags=()
    [[ "$engine" == interp ]] && engine_flags+=(--no-jit)
    for threading in default single; do
        thread_flags=()
        [[ "$threading" == single ]] && thread_flags+=(--single-thread)
        archive="$tmp_dir/$engine-$threading.zst"
        restored="$tmp_dir/$engine-$threading.txt"
        log="$tmp_dir/$engine-$threading.log"
        if ! env -u BIFROST_ROOT timeout -s KILL 90 \
            "$emu" "${engine_flags[@]}" "$zstd_bin" "${thread_flags[@]}" \
            -q -f "$input" -o "$archive" >"$log" 2>&1; then
            echo "zstd $engine/$threading compression failed:" >&2
            tail -n 40 "$log" >&2
            exit 1
        fi
        if ! timeout -s KILL 30 "$host_zstd" -q -t "$archive" >"$log" 2>&1 ||
           ! timeout -s KILL 30 "$host_zstd" -q -d -f "$archive" -o "$restored" >>"$log" 2>&1; then
            echo "host zstd rejected $engine/$threading output:" >&2
            tail -n 40 "$log" >&2
            exit 1
        fi
        cmp "$input" "$restored"
        echo "zstd $engine/$threading compression passed host validation and round-trip"
    done
    # A host-generated archive independently exercises the guest decoder.
    host_archive="$tmp_dir/host.zst"
    "$host_zstd" -q -f "$input" -o "$host_archive"
    decoded="$tmp_dir/$engine-decoded.txt"
    log="$tmp_dir/$engine-decode.log"
    if ! env -u BIFROST_ROOT timeout -s KILL 90 \
        "$emu" "${engine_flags[@]}" "$zstd_bin" -q -d -f \
        "$host_archive" -o "$decoded" >"$log" 2>&1; then
        echo "zstd $engine decompression failed:" >&2
        tail -n 40 "$log" >&2
        exit 1
    fi
    cmp "$input" "$decoded"
    echo "zstd $engine decoded the host archive correctly"
done
echo "ZSTD REGRESSION PASSED"
