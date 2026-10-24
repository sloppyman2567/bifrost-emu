#!/usr/bin/env bash
# `bifrost-emu app.apk` must hand the package to Asgard (install and run) with
# the right arguments, keep its exit status, never leak BIFROST_ROOT, and give
# clear errors. A stub asgard.py in a temporary tree records what it receives,
# so no window opens and no Android state is touched.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO_ROOT"
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ -x "$EMU" ]] || { echo "emulator not found: $EMU (set BIFROST_EMU)" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-apk-launch.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
TREE="$WORK/tree"
mkdir -p "$TREE/Scripts/Android" "$WORK/elsewhere"
cp "$EMU" "$TREE/bifrost-emu"
cat > "$TREE/Scripts/Android/asgard.py" <<'PY'
import json, os, sys
print(json.dumps({"argv": sys.argv[1:], "root": os.environ.get("BIFROST_ROOT"), "cwd": os.getcwd()}))
sys.exit(int(os.environ.get("STUB_STATUS", "0")))
PY
python3 -I - "$WORK" <<'PY'
import sys, zipfile
w = sys.argv[1]
for name in ("game.apk", "UPPER.APK", "bundle.apkm", "splits.apks", "data.xapk"):
    with zipfile.ZipFile(f"{w}/{name}", "w") as z:
        z.writestr("AndroidManifest.xml", "x")
open(f"{w}/fake.apk", "w").write("not a zip")
PY

fail=0
check() { if "$@"; then echo "PASS $NAME"; else echo "FAIL $NAME"; fail=1; fi; }
field() { python3 -I -c 'import json,sys; d=json.loads(sys.stdin.read().splitlines()[-1]); print(json.dumps(d[sys.argv[1]]))' "$1"; }

cd "$WORK/elsewhere"   # the launch directory must not matter
out=$("$TREE/bifrost-emu" "$WORK/game.apk" -d 2>&1)
NAME="runs asgard.py run PACKAGE with the emulator path and extra arguments"
check test "$(field argv <<<"$out")" = "$(python3 -I -c 'import json,sys; print(json.dumps(sys.argv[1:]))' \
    --emu "$TREE/bifrost-emu" run "$WORK/game.apk" -d)"
NAME="keeps the launch directory"
check test "$(field cwd <<<"$out")" = "\"$WORK/elsewhere\""

for pkg in UPPER.APK bundle.apkm splits.apks data.xapk; do
    NAME="$pkg is treated as a package"
    check grep -q '"run"' <<<"$("$TREE/bifrost-emu" "$WORK/$pkg" 2>&1)"
done

NAME="forwards --android-window-plugin"
out=$("$TREE/bifrost-emu" --android-window-plugin /p/plugin.so "$WORK/game.apk" 2>&1)
check grep -q -- '"--android-window-plugin", "/p/plugin.so"' <<<"$out"

NAME="exit status of the Asgard run is returned"
set +e; STUB_STATUS=7 "$TREE/bifrost-emu" "$WORK/game.apk" >/dev/null 2>&1; status=$?; set -e
check test "$status" = 7

NAME="BIFROST_ROOT is not passed to the Android session"
out=$(BIFROST_ROOT=/some/root "$TREE/bifrost-emu" "$WORK/game.apk" 2>&1)
check test "$(field root <<<"$out")" = null

NAME="BIFROST_ASGARD selects another asgard.py"
mkdir -p "$WORK/lone"; cp "$EMU" "$WORK/lone/bifrost-emu"
out=$(BIFROST_ASGARD="$TREE/Scripts/Android/asgard.py" "$WORK/lone/bifrost-emu" "$WORK/game.apk" 2>&1)
check grep -q '"run"' <<<"$out"

NAME="missing Asgard stops with status 127 and a hint"
set +e; out=$(env -u BIFROST_ASGARD "$WORK/lone/bifrost-emu" "$WORK/game.apk" 2>&1); status=$?; set -e
check test "$status" = 127 -a -n "$(grep 'BIFROST_ASGARD' <<<"$out")"

NAME="a file that is not a ZIP is rejected with status 126"
set +e; out=$("$TREE/bifrost-emu" "$WORK/fake.apk" 2>&1); status=$?; set -e
check test "$status" = 126 -a -n "$(grep 'not an Android package' <<<"$out")"

NAME="a missing package is status 127"
set +e; "$TREE/bifrost-emu" "$WORK/none.apk" >/dev/null 2>&1; status=$?; set -e
check test "$status" = 127

NAME="an ELF without a package extension still runs as before"
cd "$REPO_ROOT"
check test "$("$TREE/bifrost-emu" Tests/Apps/toybox seq 1 3 2>&1 | tr '\n' ' ')" = "1 2 3 "

exit $fail
