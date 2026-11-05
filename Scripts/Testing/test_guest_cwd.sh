#!/usr/bin/env bash
# Without a sandbox the guest's cwd is the host's: getcwd reports it and a shell that builds an
# absolute path from it (BusyBox `cd dir` does chdir(getcwd + "/dir")) must reach the directory.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ $EMU = /* ]] || EMU="$PWD/$EMU"  # this script changes directory
TOYBOX="$REPO_ROOT/Tests/Apps/toybox"
for need in "$EMU" "$TOYBOX"; do [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }; done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-guest-cwd.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
mkdir -p "$WORK/folder/sub"
cd "$WORK"
HERE=$(pwd -P)
fail=0
run() { env -u BIFROST_ROOT "$EMU" "$TOYBOX" sh -c "$1" 2>&1; }
check() { # name, expected, actual
    if [[ "$2" == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1"; echo "  expected: $2"; echo "  actual:   $3"; fail=1; fi
}

check "getcwd is the host cwd at startup" "$HERE" "$(run 'pwd')"
check "cd to an absolute path built from getcwd, then getcwd follows" "$HERE/folder/sub" \
    "$(run 'cd $(pwd)/folder/sub && pwd')"
check "a relative cd and .. keep getcwd in step with the host" "$HERE/folder|$HERE" \
    "$(run 'cd folder && pwd && cd .. && pwd' | paste -sd'|')"
check "an absolute cd back out works" "$HERE" "$(run "cd $HERE/folder; cd $HERE; pwd")"

exit $fail
