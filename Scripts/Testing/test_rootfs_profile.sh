#!/usr/bin/env bash
# Named rootfs profiles: install-rootfs.sh --profile NAME puts a rootfs in <data>/rootfs.d/NAME, and
# bifrost-emu --rootfs-profile NAME / BIFROST_ROOTFS_PROFILE / [paths] profile selects it for libraries.
# Builds from the toolchains already in Tools/ (no downloads), into a temporary prefix.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO_ROOT"
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ -e "$EMU" ]] || { echo "missing: $EMU" >&2; exit 2; }
# install-rootfs.sh --no-fetch copies its libraries from these fetched toolchains.
for need in Tests/Apps/hello_dyn_glibc.elf Tools/aarch64-linux-gnu-cross Tools/aarch64-linux-musl-cross; do
    [[ -e "$need" ]] || { echo "rootfs_profile: skipped, $need is not provisioned"; exit 77; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-rootfs-profile.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
PREFIX="$WORK/prefix"
mkdir -p "$PREFIX/bin" && cp "$EMU" "$PREFIX/bin/bifrost-emu"
fail=0
check() { # name, expected, actual
    if [[ "$2" == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1 (expected '$2', got '$3')"; fail=1; fi
}
run() { env -u BIFROST_ROOT -u BIFROST_ROOTFS_PROFILE -u LD_LIBRARY_PATH HOME="$WORK/home" XDG_DATA_HOME= "$PREFIX/bin/bifrost-emu" "$@" 2>&1 || true; }
HELLO=Tests/Apps/hello_dyn_glibc.elf

./Scripts/Setup/install-rootfs.sh --prefix "$PREFIX" --profile demo --no-fetch >"$WORK/install.log" 2>&1 && rc=0 || rc=$?
check "install --profile exits 0" 0 "$rc"
check "the profile lands in rootfs.d/NAME" yes "$([[ -e $PREFIX/share/bifrost/rootfs.d/demo/lib/ld-linux-aarch64.so.1 ]] && echo yes || echo no)"
check "the default rootfs is not touched" no "$([[ -e $PREFIX/share/bifrost/rootfs ]] && echo yes || echo no)"
check "no staging directory is left in rootfs.d" "" "$(find "$PREFIX/share/bifrost/rootfs.d" -maxdepth 1 -name '.rootfs-*')"

list=$(run --rootfs-list)
check "--rootfs-list shows the profile" yes "$([[ $list == *"demo "*"rootfs.d/demo"* ]] && echo yes || echo no)"
check "--rootfs-list marks no profile active by default" no "$([[ $list == *"* demo"* ]] && echo yes || echo no)"
list=$(run --rootfs-profile demo --rootfs-list)
check "--rootfs-list marks the selected profile" yes "$([[ $list == *"* demo"* ]] && echo yes || echo no)"

check "a program runs from the profile" yes "$([[ $(run --rootfs-profile demo "$HELLO") == *"Hello, dynamic world"* ]] && echo yes || echo no)"
out=$(env -u BIFROST_ROOT -u LD_LIBRARY_PATH HOME="$WORK/home" XDG_DATA_HOME= BIFROST_ROOTFS_PROFILE=demo "$PREFIX/bin/bifrost-emu" "$HELLO" 2>&1 || true)
check "BIFROST_ROOTFS_PROFILE selects it too" yes "$([[ $out == *"Hello, dynamic world"* ]] && echo yes || echo no)"
printf '[paths]\nprofile = "demo"\n' >"$WORK/profile.toml"
out=$(run --config "$WORK/profile.toml" "$HELLO")
check "[paths] profile selects it too" yes "$([[ $out == *"Hello, dynamic world"* ]] && echo yes || echo no)"

out=$(run --rootfs-profile nope "$HELLO")
check "a missing profile is an error naming it" yes "$([[ $out == *"profile 'nope'"* && $out != *"Hello"* ]] && echo yes || echo no)"
"$PREFIX/bin/bifrost-emu" --rootfs-profile ../evil "$HELLO" >/dev/null 2>&1 && rc=0 || rc=$?
check "an invalid profile name is rejected (status 2)" 2 "$rc"

./Scripts/Setup/install-rootfs.sh --prefix "$PREFIX" --profile demo --uninstall >/dev/null 2>&1 && rc=0 || rc=$?
check "uninstall --profile exits 0" 0 "$rc"
check "uninstall removes the profile" no "$([[ -e $PREFIX/share/bifrost/rootfs.d/demo ]] && echo yes || echo no)"

exit $fail
