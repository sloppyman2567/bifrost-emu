#!/usr/bin/env bash
# A program started by execve (every command a shell runs) must get its own working JIT. A fork
# child used to inherit a disabled copy of the parent's JIT and stay in the interpreter, so a
# `sort` run from a shell was about 70 times slower than the same `sort` run directly. -v prints
# "frostJIT enabled" once per JIT instance: the shell plus each exec'd pipeline stage.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ $EMU = /* ]] || EMU="$PWD/$EMU"  # this script changes directory
BB="$REPO_ROOT/Tests/Apps/realworld/busybox-aarch64"
[[ -e "$EMU" ]] || { echo "missing: $EMU" >&2; exit 2; }
# busybox is a downloaded real-world fixture (make setup-tests does not build it).
[[ -e "$BB" ]] || { echo "exec_jit: skipped, $BB is not provisioned"; exit 77; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-exec-jit.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
cd "$WORK"
fail=0
check() { if [[ "$2" == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1 (expected $2, got $3)"; fail=1; fi; }
count_jits() { env -i "$@" 2>&1 | grep -c 'frostJIT enabled' || true; }

PIPELINE="$BB seq 1 2000 | $BB sort -rn | $BB tail -n 1"
check "the shell and each of its three exec'd pipeline stages have a JIT" 4 \
    "$(count_jits "$EMU" -v "$BB" ash -c "$PIPELINE")"
check "--no-jit stays interpreter-only everywhere" 0 \
    "$(count_jits "$EMU" -v --no-jit "$BB" ash -c "$PIPELINE")"
check "the pipeline still computes the right answer" 1 \
    "$(env -i "$EMU" "$BB" ash -c "$PIPELINE" 2>/dev/null)"
exit $fail
