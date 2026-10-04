#!/usr/bin/env bash
# Replay requires one guest CPU; threaded runs check independent result oracles.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
emu=${1:-$repo/bifrost-emu}
fixtures=${2:-$repo/ctest}
[[ "$emu" = /* ]] || emu="$PWD/$emu"
logs=$(mktemp -d)
trap 'rm -rf -- "$logs"' EXIT
failed=0
count=0
threaded=0
shopt -s nullglob
sources=("$fixtures"/jit_*.c)
if ((${#sources[@]} == 0)); then echo 'FAIL: no JIT fixture sources'; exit 1; fi
run_check() {
    local name=$1 mode=$2 limit=$3 rc=0
    shift 3
    local log="$logs/$name-$mode.log"
    echo "--- $name ($mode) ---"
    if [[ "$mode" == verify ]]; then
        BIFROST_JIT_VERIFY=1 BIFROST_NO_DIRECT_CALL=1 timeout -k 5 "$limit" "$emu" "$@" </dev/null >"$log" 2>&1 || rc=$?
    else
        env -u BIFROST_JIT_VERIFY timeout -k 5 "$limit" "$emu" "$@" </dev/null >"$log" 2>&1 || rc=$?
    fi
    if ((rc != 0)) || grep -Eq '\[(VERIFY[^]]*|MEMFULL)\].*(DIVERGENCE|suspended)' "$log"; then
        echo "FAIL: $name ($mode), exit=$rc"
        tail -30 "$log"
        failed=1
    fi
}
for source in "${sources[@]}"; do
    fixture="${source%.c}.elf"
    name=$(basename "${source%.c}")
    if [[ ! -f "$fixture" || "$source" -nt "$fixture" ]]; then
        echo "FAIL: missing/stale $fixture; run make setup-tests"; failed=1; continue
    fi
    args=()
    limit=30
    case "$name" in
        jit_cache_invalidation) args=(single) ;;
        jit_cache_pressure) args=(single); limit=120 ;;
        jit_dispatch_cache|jit_call_helpers) args=(1000 single) ;;
    esac
    run_check "$name" verify "$limit" "$fixture" "${args[@]}"
    count=$((count+1))
    if ((${#args[@]})); then
        # Keep the original concurrent workloads and their full loop counts.
        run_check "$name" threaded-oracle 60 "$fixture"
        threaded=$((threaded+1))
    fi
done
((failed == 0)) || exit 1
echo "JIT verification passed: $count differential fixtures, $threaded threaded oracle runs."
