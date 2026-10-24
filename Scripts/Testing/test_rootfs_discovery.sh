#!/usr/bin/env bash
# Dynamic AArch64 programs must start with no --rootfs/BIFROST_ROOT when a
# rootfs sits next to the executable or in the user's data directory, and must
# fail with a clear message (not a crash) when none exists. The emulator is
# copied to a temporary directory so the checkout's bundled toolchain and
# rootfs are not reachable by accident.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO_ROOT"

EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
ROOTFS=${BIFROST_TEST_ROOTFS:-"$REPO_ROOT/rootfs"}
for need in "$EMU" Tests/Apps/hello_dyn_musl.elf Tests/Apps/hello_dyn_glibc.elf \
            "$ROOTFS/lib/ld-musl-aarch64.so.1" "$ROOTFS/lib/ld-linux-aarch64.so.1"; do
    [[ -e "$need" ]] || { echo "missing: $need (run make setup-tests and setup-rootfs-all.sh)" >&2; exit 2; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-rootfs-discovery.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
mkdir -p "$WORK/inst" "$WORK/xdg/bifrost" "$WORK/home"
cp "$EMU" "$WORK/inst/bifrost-emu"
INST="$WORK/inst/bifrost-emu"

fail=0
expect_hello() {  # name, then the command
    local name=$1; shift
    if out=$(env -u BIFROST_ROOT -u LD_LIBRARY_PATH "$@" 2>&1) && grep -q 'Hello, dynamic world' <<<"$out"; then
        echo "PASS $name"
    else
        echo "FAIL $name"; echo "$out" | head -5; fail=1
    fi
}

ln -sfn "$ROOTFS" "$WORK/xdg/bifrost/rootfs"
for libc in musl glibc; do
    expect_hello "$libc via XDG_DATA_HOME" HOME="$WORK/home" XDG_DATA_HOME="$WORK/xdg" \
        "$INST" Tests/Apps/hello_dyn_$libc.elf
done
mkdir -p "$WORK/home2/.local/share/bifrost"
ln -sfn "$ROOTFS" "$WORK/home2/.local/share/bifrost/rootfs"
expect_hello "musl via ~/.local/share" HOME="$WORK/home2" XDG_DATA_HOME= "$INST" Tests/Apps/hello_dyn_musl.elf

ln -sfn "$ROOTFS" "$WORK/inst/rootfs"
expect_hello "musl via executable-adjacent rootfs" HOME="$WORK/home" XDG_DATA_HOME= "$INST" Tests/Apps/hello_dyn_musl.elf
expect_hello "glibc via executable-adjacent rootfs" HOME="$WORK/home" XDG_DATA_HOME= "$INST" Tests/Apps/hello_dyn_glibc.elf
rm "$WORK/inst/rootfs"

# No rootfs anywhere: a clear error, exit status 1, no guest crash.
set +e
out=$(env -u BIFROST_ROOT HOME="$WORK/home" XDG_DATA_HOME= "$INST" Tests/Apps/hello_dyn_musl.elf 2>&1)
status=$?
set -e
if [[ $status -eq 1 ]] && grep -q 'no AArch64 rootfs found' <<<"$out" \
        && grep -q 'cannot find the C library' <<<"$out" && ! grep -q 'signal 11' <<<"$out"; then
    echo "PASS missing rootfs reports a hint and exits 1"
else
    echo "FAIL missing rootfs (status $status)"; echo "$out" | head -5; fail=1
fi

exit $fail
