#!/usr/bin/env bash
# Test late dynamic TLS with real glibc pthread creation, existing threads,
# and stack-cache reuse. Requires an AArch64 glibc rootfs.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$REPO_ROOT"

EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
AARCH64_CC=${AARCH64_CC:-"$REPO_ROOT/tools/aarch64-linux-gnu-cross/bin/aarch64-none-linux-gnu-gcc"}
SOURCE_ROOTFS=${BIFROST_ROOT:-"$REPO_ROOT/rootfs"}
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
for lib in ld-linux-aarch64.so.1 libc.so.6; do
    if [[ ! -r "$SOURCE_ROOTFS/lib/$lib" ]]; then
        echo "missing $SOURCE_ROOTFS/lib/$lib; set BIFROST_ROOT to a glibc rootfs" >&2
        exit 2
    fi
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-late-tls-pthread.XXXXXX")
trap 'rm -rf -- "$WORK"' EXIT
GUEST_ROOT="$WORK/root"
GUEST_DIR="$GUEST_ROOT/tmp/bifrost_late_tls_pthread"
mkdir -p "$GUEST_DIR" "$GUEST_ROOT/lib"
cp -L "$SOURCE_ROOTFS/lib/ld-linux-aarch64.so.1" "$GUEST_ROOT/lib/"
cp -L "$SOURCE_ROOTFS/lib/libc.so.6" "$GUEST_ROOT/lib/"

"$AARCH64_CC" -O2 -pthread -o "$GUEST_DIR/main" \
    ctest_real/late_tls_pthread_test.c
"$AARCH64_CC" -shared -fPIC -O2 -nostdlib -mtls-dialect=desc \
    -DLATE_TLS_NAME=late_tls_pthread -DLATE_TLS_ACCESSOR=tls_value_addr \
    -DLATE_TLS_PADDING=4096 -Wl,-soname,liblate_tls_pthread.so \
    -o "$GUEST_DIR/liblate_tls_pthread.so" ctest_real/late_tls_module.c

"$AARCH64_CC" -O2 -Wall -Wextra -pthread -o "$GUEST_DIR/edge_main" \
    ctest_real/late_tls_edge_test.c
for variant in static dynamic; do
    padding=16
    [[ "$variant" == dynamic ]] && padding=4096
    # Unique exported names keep the dynamic control independent of the
    # static module. Both ABS64 and RELATIVE relocate its TLS pointers.
    "$AARCH64_CC" -shared -fPIC -O2 -nostdlib -mtls-dialect=desc \
        -DTLS_POINTER "-DTLS_PADDING=$padding" \
        "-DTLS_NAMESPACE=edge_pointer_${variant}" \
        -o "$GUEST_DIR/edge_pointer_${variant}.so" \
        ctest_real/late_tls_edge_module.c
done
"$AARCH64_CC" -shared -fPIC -O2 -nostdlib -mtls-dialect=desc \
    -DTLS_ALIGNED -o "$GUEST_DIR/edge_aligned.so" \
    ctest_real/late_tls_edge_module.c
"$AARCH64_CC" -shared -fPIC -O2 -nostdlib -mtls-dialect=desc \
    -DTLS_WEAK -o "$GUEST_DIR/edge_weak.so" \
    ctest_real/late_tls_edge_module.c

for mode in jit interp; do
    flags=()
    [[ "$mode" == interp ]] && flags+=(--no-jit)
    echo "late TLS pthread regression ($mode)"
    BIFROST_ROOT="$GUEST_ROOT" "$EMU" "${flags[@]}" \
        /tmp/bifrost_late_tls_pthread/main
    for edge in pointers alignment weak; do
        BIFROST_ROOT="$GUEST_ROOT" "$EMU" "${flags[@]}" \
            /tmp/bifrost_late_tls_pthread/edge_main "$edge"
    done
done
echo "late TLS pthread regression: PASS"
