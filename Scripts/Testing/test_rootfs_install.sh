#!/usr/bin/env bash
# install-rootfs.sh must produce a rootfs that an installed bifrost-emu uses
# with no flags: PREFIX/bin/bifrost-emu finds PREFIX/share/bifrost/rootfs.
# Builds from the toolchains already in Tools/ (no downloads), into a
# temporary prefix.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO_ROOT"

EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
for need in "$EMU" Tests/Apps/hello_dyn_musl.elf Tests/Apps/hello_dyn_glibc.elf \
            Tools/aarch64-linux-gnu-cross Tools/aarch64-linux-musl-cross; do
    [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-rootfs-install.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
PREFIX="$WORK/prefix"
ROOTFS="$PREFIX/share/bifrost/rootfs"
mkdir -p "$PREFIX/bin"
cp "$EMU" "$PREFIX/bin/bifrost-emu"
fail=0
quiet() { "$@" >>"$WORK/commands.log" 2>&1; }
check() { if "$@"; then echo "PASS $CHECK_NAME"; else echo "FAIL $CHECK_NAME"; fail=1; fi; }
run_hello() {
    local out
    out=$(env -u BIFROST_ROOT -u LD_LIBRARY_PATH HOME="$WORK/home" XDG_DATA_HOME= \
          "$PREFIX/bin/bifrost-emu" "$1" 2>&1) && grep -q 'Hello, dynamic world' <<<"$out"
}

CHECK_NAME="install-rootfs builds a rootfs under PREFIX/share"
check quiet ./Scripts/Setup/install-rootfs.sh --prefix "$PREFIX" --no-fetch
CHECK_NAME="installed rootfs has both loaders, ld.so.cache and /etc"
check test -e "$ROOTFS/lib/ld-linux-aarch64.so.1" -a -e "$ROOTFS/lib/ld-musl-aarch64.so.1" \
        -a -s "$ROOTFS/etc/ld.so.cache" -a -e "$ROOTFS/etc/passwd"
CHECK_NAME="no staging directories left behind"
check test -z "$(find "$PREFIX/share/bifrost" -maxdepth 1 -name '.rootfs-*')"
for libc in musl glibc; do
    CHECK_NAME="$libc program runs from the installed layout with no flags"
    check run_hello Tests/Apps/hello_dyn_$libc.elf
done

# Reinstalling replaces the previous rootfs and keeps working.
touch "$ROOTFS/stale-marker"
CHECK_NAME="reinstall replaces the old rootfs"
check quiet ./Scripts/Setup/install-rootfs.sh --prefix "$PREFIX" --no-fetch
CHECK_NAME="reinstalled rootfs drops files from the old one"
check test ! -e "$ROOTFS/stale-marker"
CHECK_NAME="reinstalled rootfs still runs programs"
check run_hello Tests/Apps/hello_dyn_musl.elf

CHECK_NAME="uninstall refuses a directory that is not a rootfs"
mkdir -p "$WORK/other/share/bifrost/rootfs"; touch "$WORK/other/share/bifrost/rootfs/keep"
if ./Scripts/Setup/install-rootfs.sh --prefix "$WORK/other" --uninstall >/dev/null 2>&1 \
        || [[ ! -e "$WORK/other/share/bifrost/rootfs/keep" ]]; then echo "FAIL $CHECK_NAME"; fail=1
else echo "PASS $CHECK_NAME"; fi
CHECK_NAME="uninstall removes the installed rootfs"
check quiet ./Scripts/Setup/install-rootfs.sh --prefix "$PREFIX" --uninstall
CHECK_NAME="uninstalled rootfs is gone"
check test ! -e "$ROOTFS"

exit $fail
