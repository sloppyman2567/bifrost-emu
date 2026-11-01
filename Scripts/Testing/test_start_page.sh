#!/usr/bin/env bash
# What `bifrost-emu` prints with no arguments: its Status lines must report what a real launch would find, and name
# the command that fixes what is missing. --help-all must list every option the parser accepts.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-start-page.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
mkdir -p "$WORK/inst" "$WORK/xdg/bifrost" "$WORK/home"
cp "$EMU" "$WORK/inst/bifrost-emu"   # away from the checkout, so its rootfs and Asgard are not found by accident
run() { env -u BIFROST_ROOT -u BIFROST_ROOTFS_PROFILE -u BIFROST_ASGARD -u XDG_CONFIG_HOME HOME="$WORK/home" XDG_DATA_HOME="$WORK/xdg" "$WORK/inst/bifrost-emu" "$@" 2>&1; }

fail=0
expect() {  # name, pattern, text
    if grep -qE -- "$2" <<<"$3"; then echo "PASS $1"; else echo "FAIL $1: no match for '$2'"; echo "$3" | head -30; fail=1; fi
}
refuse() {  # name, pattern, text
    if grep -qE -- "$2" <<<"$3"; then echo "FAIL $1: '$2' should not appear"; fail=1; else echo "PASS $1"; fi
}

# A system-wide install is really there for a launch to find, so its "missing" cases cannot be tested on this host.
system_has() { [[ -e /usr/local/share/bifrost/$1 || -e /usr/share/bifrost/$1 ]]; }
page=$(run)
if system_has rootfs; then echo "SKIP rootfs missing (a system rootfs is installed)"
else expect "rootfs missing names its fix" 'Linux libraries +not installed +bifrost-emu --setup-rootfs$' "$page"; fi
if system_has rootfs.d/games; then echo "SKIP games missing (a system games profile is installed)"
else expect "games missing names its fix" 'Linux game libraries +not installed +bifrost-emu --setup-rootfs --games$' "$page"; fi
if system_has asgard; then echo "SKIP Asgard missing (a system Asgard is installed)"
else
    expect "Asgard missing is reported" 'Android \(Asgard\) +not installed' "$page"
    refuse "no library command without Asgard" 'open the app library' "$page"
fi
refuse "the page leaves out internal options" '--asgard-broker' "$page"
expect "the page shows how to set up libraries" '--setup-rootfs +install or update' "$page"

mkdir -p "$WORK/xdg/bifrost/rootfs/lib" "$WORK/xdg/bifrost/rootfs.d/games/lib"
touch "$WORK/xdg/bifrost/rootfs/lib/ld-musl-aarch64.so.1" "$WORK/xdg/bifrost/rootfs.d/games/lib/ld-musl-aarch64.so.1"
mkdir -p "$WORK/xdg/bifrost/asgard/Scripts/Android" "$WORK/xdg/bifrost/asgard/Tools/android-tools"
touch "$WORK/xdg/bifrost/asgard/Scripts/Android/asgard.py"
page=$(run)
expect "rootfs found" 'Linux libraries +installed' "$page"
expect "games profile found" 'Linux game libraries +installed' "$page"
if system_has asgard; then echo "SKIP a script without its toolchain (a system Asgard is installed)"
else expect "a script without its toolchain is not set up" 'Android \(Asgard\) +not installed' "$page"; fi
install -m 0755 /dev/null "$WORK/xdg/bifrost/asgard/Tools/android-tools/aapt2"
page=$(run)
expect "Asgard found" 'Android \(Asgard\) +installed' "$page"
expect "library command shown, clear of its description" 'asgard launcher +open the app library' "$page"

help=$(run --help); all=$(run --help-all)
expect "--help points to the full list" '--help-all' "$help"
refuse "--help leaves out internal options" '--asgard-broker' "$help"
expect "--help says options go first" 'Options go before the program' "$help"
# Every option the parser accepts, except the easter eggs and the prefixes it matches on.
if [[ -r "$REPO_ROOT/Source/Main.cpp" ]]; then
    while read -r option; do
        case "$option" in --bifrost|--rainbow|--|--no-|--window-stats-|--window-stats-mode=|--window_stats) continue ;; esac
        grep -qF -- "$option" <<<"$all" || { echo "FAIL --help-all omits $option"; fail=1; }
    done < <(grep -oE '"--?[a-zA-Z][a-zA-Z_-]*=?"' "$REPO_ROOT/Source/Main.cpp" | tr -d '"' | sort -u)
fi
[[ $fail == 0 ]] && echo "test_start_page: ALL PASS"
exit "$fail"
