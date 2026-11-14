#!/usr/bin/env bash
# check_fresh.sh — run the whole test suite on a fresh Linux machine and say what it covered.
#
# Usage: Scripts/Testing/check_fresh.sh [--emu PATH] [--quick] [--no-provision]      (or: make check-fresh)
#   --emu PATH       the emulator to test (default ./bifrost-emu; make check-fresh builds it first)
#   --quick          only the host tests and the JIT runner (no interpreter runner, no make verify)
#   --no-provision   do not fetch toolchains, build the rootfs or fetch fixtures: they must already be there
#
# Needs the packages of `Scripts/Setup/install-deps.sh --yes --no-asgard --tests` and network access for the provisioning
# (the glibc and musl cross toolchains, Debian arm64 libraries and programs). Every stage runs even when
# an earlier one failed; the full logs are in build/check-fresh/. The exit status is 1 when a stage failed or a test was
# skipped for a reason other than the known ones below, so a fresh machine that lacks something says so.
#
# Skipped on a machine with no display, GPU or input device (Docs/TESTS.md):
#   gles_native_fence  needs the EGL native-fence extension, which software rendering lacks
#   android_surface    needs SDL3 and a window system
#   vulkan_texture     needs a window system that can present Vulkan
#   wayland_bridge, wayland_input   need a Wayland compositor (the second also an input seat)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
KNOWN_SKIPS='^(gles_native_fence|android_surface|vulkan_texture|wayland_bridge|wayland_input)$'

EMU=./bifrost-emu
QUICK=0
PROVISION=1
while [ $# -gt 0 ]; do
    case "$1" in
        --emu)          [ $# -ge 2 ] || { echo "--emu needs a path" >&2; exit 2; }; EMU=$2; shift ;;
        --quick)        QUICK=1 ;;
        --no-provision) PROVISION=0 ;;
        --help|-h)      sed -n '2,/^set /p' "$0" | sed '$d; s/^# \?//'; exit 0 ;;
        *)              echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

missing=()
for tool in make g++ python3 curl ar xz zstd pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "error: missing tools: ${missing[*]}" >&2
    echo "  Scripts/Setup/install-deps.sh --yes --no-asgard --tests installs them (and the libraries the build needs)." >&2
    exit 2
fi
[ -x "$EMU" ] || { echo "error: no emulator at $EMU (make builds it; make check-fresh does)" >&2; exit 2; }

# The Android host tests are named once, in the Makefile; make passes them in.
ANDROID_HOST_TESTS=${ANDROID_HOST_TESTS:-$(make -pn 2>/dev/null | sed -n 's/^ANDROID_HOST_TESTS := //p')}

LOGS=build/check-fresh
mkdir -p "$LOGS"
FAILED=()
RESULTS=()
stage() {  # NAME COMMAND...
    local name=$1; shift
    local start=$SECONDS
    printf '== %s ...' "$name"
    if "$@" >"$LOGS/$name.log" 2>&1; then
        RESULTS+=("ok      $name ($((SECONDS - start)) s)")
        printf ' ok (%s s)\n' $((SECONDS - start))
    else
        RESULTS+=("FAILED  $name ($((SECONDS - start)) s): $LOGS/$name.log")
        FAILED+=("$name")
        printf ' FAILED (%s s), see %s\n' $((SECONDS - start)) "$LOGS/$name.log"
    fi
}

if [ "$PROVISION" = 1 ]; then
    stage musl-toolchain ./Tools/Fetch/fetch-musl-toolchain.sh --bootlin
    # setup-rootfs-all.sh fetches the glibc toolchain, builds the rootfs and fetches the real-world libraries; it is
    # skipped once its results are there. Its libraries are what xwayland_wire links against.
    if [ -x Tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc ] && [ -e rootfs/lib/ld-linux-aarch64.so.1 ] \
        && [ -e rootfs/usr/lib/libX11.so.6 ]; then
        RESULTS+=("ok      rootfs (already provisioned)")
    else
        stage rootfs ./Scripts/Setup/setup-rootfs-all.sh
    fi
    have=1
    for fixture in iperf3-aarch64 echo cat uname date readlink; do [ -e "Tests/Apps/realworld/$fixture" ] || have=0; done
    if [ "$have" = 1 ]; then
        RESULTS+=("ok      realworld fixtures (already fetched)")
    else
        stage realworld-fixtures ./Scripts/Setup/fetch-realworld-dynamic.sh
    fi
    stage test-fixtures make setup-tests
fi

stage generated-check make generated-check
stage host-api make test-capi test-nb
# shellcheck disable=SC2086  # the list is words
stage host-android make $ANDROID_HOST_TESTS
stage runner-jit ./Scripts/Testing/run_tests.sh --emu "$EMU" --strict
if [ "$QUICK" = 0 ]; then
    stage runner-interpreter ./Scripts/Testing/run_tests.sh --emu "$EMU" --no-jit --strict
    stage verify make verify
fi

# Every skip, from the runners' own lists, against the known ones.
echo
echo "Stages:"
printf '  %s\n' "${RESULTS[@]}"
echo
unexpected=()
known=()
for log in "$LOGS"/runner-*.log; do
    [ -e "$log" ] || continue
    name=$(basename "$log" .log)
    sed 's/\x1b\[[0-9;]*m//g' "$log" | grep -E '^Total:' | sed "s/^/  $name: /"
    while read -r test; do
        if [[ "$test" =~ $KNOWN_SKIPS ]]; then known+=("$name:$test"); else unexpected+=("$name:$test"); fi
    done < <(sed 's/\x1b\[[0-9;]*m//g' "$log" | sed -n -E 's/^[[:space:]]+SKIP[[:space:]]+([^[:space:]]+).*/\1/p')
done
[ "${#known[@]}" -eq 0 ] || echo "  Skipped as expected on a machine without a window system or GPU extensions: ${known[*]}"
if [ "${#unexpected[@]}" -gt 0 ]; then
    echo "  Skipped unexpectedly (a fixture or tool is missing here): ${unexpected[*]}"
fi
if [ "${#FAILED[@]}" -gt 0 ]; then
    echo "  Failed stages: ${FAILED[*]}"
fi
[ "${#FAILED[@]}" -eq 0 ] && [ "${#unexpected[@]}" -eq 0 ]
