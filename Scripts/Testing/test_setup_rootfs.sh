#!/usr/bin/env bash
# `bifrost-emu --setup-rootfs`: installs a rootfs the emulator then finds by itself, uninstalls it,
# and reports (status 127) when the executable has no checkout's installer beside it. Builds from the
# toolchains already in Tools/ (--no-fetch), so it never downloads.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ -e "$EMU" ]] || { echo "missing: $EMU" >&2; exit 2; }
[[ -d "$REPO_ROOT/Tools/aarch64-linux-gnu-cross" && -d "$REPO_ROOT/Tools/aarch64-linux-musl-cross" ]] ||
    { echo "SKIP toolchains missing from Tools/ (run Scripts/Setup/install-rootfs.sh once)"; exit 0; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-setup-rootfs.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
fail=0
check() { # name, expected, actual
    if [[ "$2" == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1 (expected '$2', got '$3')"; fail=1; fi
}
DEST="$WORK/prefix/share/bifrost/rootfs"

"$EMU" --setup-rootfs --prefix "$WORK/prefix" --no-fetch >"$WORK/install.log" 2>&1 && rc=0 || rc=$?
check "install exits 0" 0 "$rc"
check "the rootfs has a glibc loader" yes "$([[ -e $DEST/lib/ld-linux-aarch64.so.1 ]] && echo yes || echo no)"

# A copy of the emulator in <prefix>/bin finds <prefix>/share/bifrost/rootfs without flags.
mkdir -p "$WORK/prefix/bin" && cp "$EMU" "$WORK/prefix/bin/"
out=$(env HOME="$WORK/home" XDG_DATA_HOME="$WORK/xdg" "$WORK/prefix/bin/bifrost-emu" "$REPO_ROOT/Tests/Apps/test_dyn_main_locks.elf" 2>&1 || true)
check "a dynamic program runs from the installed rootfs" yes "$([[ $out == *"ALL PASS"* ]] && echo yes || echo no)"

"$EMU" --setup-rootfs --prefix "$WORK/prefix" --uninstall >/dev/null 2>&1 && rc=0 || rc=$?
check "uninstall exits 0" 0 "$rc"
check "uninstall removes the rootfs" no "$([[ -e $DEST ]] && echo yes || echo no)"

mkdir "$WORK/solo" && cp "$EMU" "$WORK/solo/"
"$WORK/solo/bifrost-emu" --setup-rootfs >/dev/null 2>&1 && rc=0 || rc=$?
check "without an installer beside it the status is 127" 127 "$rc"

exit $fail
