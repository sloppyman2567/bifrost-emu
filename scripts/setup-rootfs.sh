#!/bin/bash
# setup-rootfs.sh — create a minimal Linux rootfs for bifrost-emu.
#
# This script creates a minimal FHS-style root filesystem under
# $BIFROST_ROOT (default: ./rootfs) that contains the AArch64 glibc
# shared libraries and the dynamic linker. With this rootfs in place,
# dynamically-linked AArch64 binaries (both glibc and musl) can run
# under bifrost-emu using BIFROST_ROOT=$PWD/rootfs.
#
# Layout created:
#   rootfs/
#     lib/             -> ld-linux-aarch64.so.1, libc.so.6, libm.so.6, ...
#     lib/ld-musl-aarch64.so.1  (musl dynamic linker, if available)
#     usr/lib/         -> multiarch libs (libstdc++, libgcc_s, etc.)
#     etc/
#       passwd         (minimal)
#       group          (minimal)
#       hostname       ("bifrost")
#       hosts          (localhost entries)
#       nsswitch.conf  (files dns)
#     bin/             -> sh (symlink to busybox/toybox if available)
#     tmp/             (writable, mode 1777)
#     proc/            (mountpoint; populated at runtime by Yggdrasil)
#     dev/             (mountpoint; populated at runtime by Yggdrasil)
#
# Usage:
#   ./scripts/setup-rootfs.sh [rootfs-dir]
#
# If no argument is given, defaults to ./rootfs (relative to the
# project root). The script is idempotent: re-running it refreshes
# the libraries from the toolchain.

set -euo pipefail

# Resolve project root (parent of the scripts/ directory).
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

ROOTFS="${1:-$PROJECT_ROOT/rootfs}"
TOOLCHAIN_GLIBC="tools/aarch64-linux-gnu-cross"
TOOLCHAIN_MUSL="tools/aarch64-linux-musl-cross"

echo "Setting up rootfs at: $ROOTFS"
mkdir -p "$ROOTFS"/{lib,lib64,usr/lib,usr/lib64,usr/bin,usr/sbin,etc,bin,sbin,tmp,proc,dev,sys,var/run,var/log,var/tmp,root,home,run,dev/pts,dev/shm}

# ── Copy glibc libraries ──────────────────────────────────────────────
if [ -d "$TOOLCHAIN_GLIBC/aarch64-none-linux-gnu/libc/lib64" ]; then
    echo "Copying glibc libraries from $TOOLCHAIN_GLIBC ..."
    # Core runtime libs (libc, libm, libdl, libpthread, librt, libresolv,
    # libcrypt, libutil, libBrokenLocale, libanl, libnsl)
    for lib in libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 \
               libresolv.so.2 libcrypt.so.1 libutil.so.1 libBrokenLocale.so.1 \
               libanl.so.1 libnsl.so.1 libmvec.so.1; do
        src="$TOOLCHAIN_GLIBC/aarch64-none-linux-gnu/libc/lib64/$lib"
        [ -f "$src" ] && cp -f "$src" "$ROOTFS/lib/"
    done
    # NSS modules (glibc loads these dynamically at runtime via dlopen)
    for lib in libnss_compat.so.2 libnss_files.so.2 libnss_dns.so.2 \
               libnss_hesiod.so.2 libnss_db.so.2; do
        src="$TOOLCHAIN_GLIBC/aarch64-none-linux-gnu/libc/lib64/$lib"
        [ -f "$src" ] && cp -f "$src" "$ROOTFS/lib/"
    done
    # Dynamic linker
    src="$TOOLCHAIN_GLIBC/aarch64-none-linux-gnu/libc/lib/ld-linux-aarch64.so.1"
    if [ -f "$src" ]; then
        cp -f "$src" "$ROOTFS/lib/"
    fi
    # libgcc_s, libstdc++, libatomic (in usr/lib64)
    for lib in libgcc_s.so.1 libstdc++.so.6 libatomic.so.1; do
        src="$TOOLCHAIN_GLIBC/aarch64-none-linux-gnu/libc/usr/lib64/$lib"
        [ -f "$src" ] && cp -f "$src" "$ROOTFS/usr/lib/"
    done
    # Create ld-linux symlink (some programs look for it in /lib64)
    mkdir -p "$ROOTFS/lib64"
    [ -f "$ROOTFS/lib/ld-linux-aarch64.so.1" ] && \
        ln -sf ../lib/ld-linux-aarch64.so.1 "$ROOTFS/lib64/ld-linux-aarch64.so.1"
else
    echo "Warning: glibc toolchain not found at $TOOLCHAIN_GLIBC"
    echo "  Run ./tools/fetch-glibc-toolchain.sh first."
fi

# ── Copy musl libraries (if available) ────────────────────────────────
if [ -d "$TOOLCHAIN_MUSL/aarch64-linux-musl/lib" ]; then
    echo "Copying musl libraries from $TOOLCHAIN_MUSL ..."
    # musl libc.so is both the dynamic linker and libc
    src="$TOOLCHAIN_MUSL/aarch64-linux-musl/lib/libc.so"
    if [ -f "$src" ]; then
        cp -f "$src" "$ROOTFS/lib/ld-musl-aarch64.so.1"
        # Also as libc.musl-aarch64.so.1 (some binaries reference this)
        cp -f "$src" "$ROOTFS/lib/libc.musl-aarch64.so.1"
        # BUGFIX (Turn 53): musl's DT_NEEDED is "libc.so" (not libc.so.6
        # like glibc). Create a symlink so the dynamic linker finds it.
        ln -sf ld-musl-aarch64.so.1 "$ROOTFS/lib/libc.so"
    fi
fi

# ── /etc files ────────────────────────────────────────────────────────
cat > "$ROOTFS/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
nobody:x:65534:65534:nobody:/:/sbin/nologin
EOF

cat > "$ROOTFS/etc/group" <<'EOF'
root:x:0:
nobody:x:65534:
EOF

echo "bifrost" > "$ROOTFS/etc/hostname"

cat > "$ROOTFS/etc/hosts" <<'EOF'
127.0.0.1   localhost localhost.localdomain bifrost
::1         localhost ip6-localhost ip6-loopback
EOF

cat > "$ROOTFS/etc/nsswitch.conf" <<'EOF'
passwd:     files
group:      files
shadow:     files
hosts:      files dns
networks:   files
protocols:  files
services:   files
ethers:     files
rpc:        files
EOF

# resolv.conf (empty by default — Yggdrasil provides /dev/null for
# DNS lookups; the guest can override by writing to this file)
cat > "$ROOTFS/etc/resolv.conf" <<'EOF'
nameserver 127.0.0.1
EOF

# /etc/os-release — identifies the system for programs that check it
# (e.g., systemd, package managers). We claim "Bifrost Linux" to make
# it clear this is an emulated environment.
cat > "$ROOTFS/etc/os-release" <<'EOF'
NAME="Bifrost Linux"
ID=bifrost
VERSION_ID=1.4.5
PRETTY_NAME="Bifrost Linux 1.4.5-alpha (AArch64 Emulator)"
HOME_URL="https://github.com/sloppyman2567/bifrost-emu"
EOF

# /etc/profile — minimal shell profile for interactive shells
cat > "$ROOTFS/etc/profile" <<'EOF'
# /etc/profile — system-wide shell profile
export PATH=/bin:/usr/bin:/sbin:/usr/sbin
export HOME=/root
export TERM=linux
export PS1='$ '
EOF

# /etc/shells — list of valid login shells
cat > "$ROOTFS/etc/shells" <<'EOF'
/bin/sh
/bin/bash
EOF

# /etc/ld.so.conf — dynamic linker configuration
cat > "$ROOTFS/etc/ld.so.conf" <<'EOF'
/lib
/usr/lib
/lib64
/usr/lib64
EOF

# /etc/ld.so.cache — empty (the dynamic linker would normally generate
# this; we don't have ldconfig, so the dynamic linker falls back to
# the default search paths in /etc/ld.so.conf)
: > "$ROOTFS/etc/ld.so.cache"

# ── /tmp (writable) ───────────────────────────────────────────────────
chmod 1777 "$ROOTFS/tmp"

# ── /bin/sh symlink ───────────────────────────────────────────────────
# If the user has a static toybox or busybox aarch64 binary, link it
# as /bin/sh. Otherwise leave /bin/sh as a dangling symlink — the
# guest should provide its own shell.
if [ -f "$PROJECT_ROOT/ctest_real/toybox" ]; then
    cp -f "$PROJECT_ROOT/ctest_real/toybox" "$ROOTFS/bin/sh"
    chmod +x "$ROOTFS/bin/sh"
else
    ln -sf /bin/busybox "$ROOTFS/bin/sh" 2>/dev/null || true
fi

echo ""
echo "Rootfs setup complete at: $ROOTFS"
echo ""
echo "To use it:"
echo "  export BIFROST_ROOT=$ROOTFS"
echo "  ./bifrost-emu ctest_real/hello_dyn_glibc.elf"
echo ""
echo "To refresh libraries after a toolchain update, re-run this script."
