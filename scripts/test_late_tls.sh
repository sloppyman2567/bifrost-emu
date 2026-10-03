#!/usr/bin/env bash
# Build isolated AArch64 TLS DSOs and verify late TLS for a static executable
# that starts without PT_TLS. No project rootfs files are changed.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$REPO_ROOT"

EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
AARCH64_CC=${AARCH64_CC:-"$REPO_ROOT/tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc"}
if [[ ! -x "$AARCH64_CC" ]]; then
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        AARCH64_CC=$(command -v aarch64-linux-gnu-gcc)
    else
        echo "AArch64 GCC not found; set AARCH64_CC" >&2
        exit 2
    fi
fi
if [[ ! -x "$EMU" ]]; then
    echo "emulator not found: $EMU (set BIFROST_EMU)" >&2
    exit 2
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-late-tls.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
GUEST_ROOT="$WORK/root"
GUEST_DIR="$GUEST_ROOT/tmp/bifrost_late_tls"
mkdir -p "$GUEST_DIR"

"$AARCH64_CC" -nostdlib -static -Wl,-e,_start \
    -o "$GUEST_DIR/main" ctest_real/late_tls_test.S

build_module() {
    local name=$1 dialect=$2 padding=$3
    "$AARCH64_CC" -shared -fPIC -O2 -nostdlib \
        "-mtls-dialect=$dialect" \
        "-DLATE_TLS_NAME=$name" "-DLATE_TLS_PADDING=$padding" \
        -Wl,-soname,"lib${name}.so" \
        -o "$GUEST_DIR/lib${name}.so" ctest_real/late_tls_module.c
}

# Large TLS blocks exceed the static dlopen surplus and use per-thread DTV
# allocations. The small TLSDESC module fits the static surplus.
build_module late_tls_gd trad 4096
build_module late_tls_desc_static desc 16
build_module late_tls_desc_dynamic desc 4096
"$AARCH64_CC" -shared -fPIC -O2 -nostdlib \
    -mtls-dialect=trad -ftls-model=initial-exec \
    -DLATE_TLS_NAME=late_tls_rejected -DLATE_TLS_PADDING=4096 \
    -DLATE_TLS_ACCESSOR=late_tls_rejected_marker \
    -Wl,-soname,liblate_tls_rejected.so \
    -o "$GUEST_DIR/liblate_tls_rejected.so" ctest_real/late_tls_module.c

GUEST_MAIN=/tmp/bifrost_late_tls/main
for mode in jit interp; do
    flags=()
    [[ "$mode" == interp ]] && flags+=(--no-jit)
    echo "late TLS regression ($mode)"
    BIFROST_ROOT="$GUEST_ROOT" "$EMU" "${flags[@]}" "$GUEST_MAIN"
done
echo "late TLS regression: PASS"
