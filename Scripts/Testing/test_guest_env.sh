#!/usr/bin/env bash
# The environment a guest starts with. Without a sandbox it is the caller's own (HOME, TERM, LANG,
# PATH, custom variables), minus the loader's LD_* and the emulator's BIFROST_*; PATH and HOME get
# a default only when missing and PWD always names the real cwd. A BIFROST_ROOT sandbox gets the
# curated environment instead. Nothing the caller did not set may leak in (libraries linked into the
# emulator change its own environment, which is why the caller's block is read from /proc/self/environ).
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
BB="$REPO_ROOT/Tests/Apps/realworld/busybox-aarch64"
for need in "$EMU" "$BB"; do [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }; done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-guest-env.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
mkdir -p "$WORK/real/sub"
ln -s real "$WORK/link"
HERE=$(cd "$WORK/real/sub" && pwd -P)
fail=0
check() { # name, expected, actual
    if [[ "$2" == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1"; echo "  expected: $2" | head -c 600; echo; echo "  actual:   $3" | head -c 600; echo; fail=1; fi
}
guest_env() { (cd "$HERE" && "$@" "$EMU" "$BB" env 2>/dev/null | sort | tr '\n' ' '); }
CALLER=(env -i PATH=/usr/bin:/bin HOME=/home/someone TERM=xterm-256color LANG=en_US.UTF-8 FOO=bar SHLVL=3)

check "the caller's variables arrive, nothing else is added" \
    "FOO=bar HOME=/home/someone LANG=en_US.UTF-8 PATH=/usr/bin:/bin PWD=$HERE SHLVL=3 TERM=xterm-256color " \
    "$(guest_env "${CALLER[@]}")"
check "LD_* and BIFROST_* are not passed" \
    "FOO=bar HOME=/home/someone LANG=en_US.UTF-8 PATH=/usr/bin:/bin PWD=$HERE SHLVL=3 TERM=xterm-256color " \
    "$(guest_env "${CALLER[@]}" LD_LIBRARY_PATH=/hostlibs BIFROST_NO_CHAIN=1 BIFROST_GUEST_ENV=)"
check "a PWD that names another directory is replaced by the real one" \
    "FOO=bar HOME=/home/someone LANG=en_US.UTF-8 PATH=/usr/bin:/bin PWD=$HERE SHLVL=3 TERM=xterm-256color " \
    "$(guest_env "${CALLER[@]}" PWD=/bogus)"
check "a logical PWD through a symlink to the cwd is kept" \
    "FOO=bar HOME=/home/someone LANG=en_US.UTF-8 PATH=/usr/bin:/bin PWD=$WORK/link/sub SHLVL=3 TERM=xterm-256color " \
    "$(guest_env "${CALLER[@]}" PWD="$WORK/link/sub")"
MISSING=$(guest_env env -i FOO=1)
check "PATH and HOME get defaults only when missing" "yes" \
    "$([[ "$MISSING" == *"FOO=1 "* && "$MISSING" == *"HOME="* && "$MISSING" == *"PATH=/usr/local/bin:/usr/bin:/bin "* ]] && echo yes || echo "no: $MISSING")"

if [[ -x "$REPO_ROOT/rootfs/bin/sh" ]]; then
    SANDBOXED=$(cd "$WORK" && env -i PATH=/usr/bin HOME=/home/someone TERM=xterm FOO=bar LANG=C "$EMU" --rootfs "$REPO_ROOT/rootfs" /bin/sh -c 'echo "HOME=$HOME TERM=$TERM FOO=${FOO-unset} LANG=$LANG"' 2>/dev/null)
    check "a sandboxed guest keeps the curated environment" "HOME=/root TERM=linux FOO=unset LANG=C" "$SANDBOXED"
else
    echo "SKIP sandboxed environment (no rootfs/bin/sh)"
fi
exit $fail
