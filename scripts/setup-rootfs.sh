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
    # NEW (Turn 74): gconv (character conversion) libraries. glibc loads
    # these dynamically when programs use iconv() with non-UTF-8 charsets.
    # Without them, programs that convert between character sets fail.
    GCONV_DIR="$ROOTFS/usr/lib/gconv"
    mkdir -p "$GCONV_DIR"
    SRC_GCONV="$TOOLCHAIN_GLIBC/aarch64-none-linux-gnu/libc/usr/lib64/gconv"
    if [ -d "$SRC_GCONV" ]; then
        cp -f "$SRC_GCONV"/*.so "$GCONV_DIR/" 2>/dev/null || true
        cp -f "$SRC_GCONV"/gconv-modules "$GCONV_DIR/" 2>/dev/null || true
    fi
    # libthread_db (debugger interface, loaded by gdb but also by some
    # programs that introspect thread state).
    src="$TOOLCHAIN_GLIBC/aarch64-none-linux-gnu/libc/lib64/libthread_db.so.1"
    [ -f "$src" ] && cp -f "$src" "$ROOTFS/lib/"
    # libnss_* modules (glibc loads these dynamically for name resolution).
    # Already copied above, but also copy libnss_compat if in usr/lib.
    for lib in libnss_compat.so.2 libnss_files.so.2 libnss_dns.so.2; do
        src="$TOOLCHAIN_GLIBC/aarch64-none-linux-gnu/libc/usr/lib64/$lib"
        [ -f "$src" ] && cp -f "$src" "$ROOTFS/lib/" 2>/dev/null || true
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
PRETTY_NAME="Bifrost Linux 1.5.0.alpha (AArch64 Emulator)"
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

# ── Android-compatible directory structure ────────────────────────────
# Android uses /system/lib64 and /vendor/lib64 for its libraries.
# Some Android games and Android-ported Linux programs look for libs
# in these paths. Create them as symlinks to the standard FHS paths
# so the dynamic linker can find them.
mkdir -p "$ROOTFS/system" "$ROOTFS/vendor"
[ -e "$ROOTFS/system/lib" ]   || ln -sf ../lib     "$ROOTFS/system/lib"
[ -e "$ROOTFS/system/lib64" ] || ln -sf ../lib64   "$ROOTFS/system/lib64"
[ -e "$ROOTFS/system/bin" ]   || ln -sf ../bin     "$ROOTFS/system/bin"
[ -e "$ROOTFS/system/etc" ]   || ln -sf ../etc     "$ROOTFS/system/etc"
[ -e "$ROOTFS/vendor/lib" ]   || ln -sf ../lib     "$ROOTFS/vendor/lib"
[ -e "$ROOTFS/vendor/lib64" ] || ln -sf ../lib64   "$ROOTFS/vendor/lib64"

# /system/build.prop — minimal Android-style properties file.
# Some Android programs read this to detect the platform.
cat > "$ROOTFS/system/build.prop" <<'EOF'
# Bifrost-emu Android-compatible properties
ro.build.version.sdk=29
ro.build.version.release=10
ro.product.cpu.abi=arm64-v8a
ro.product.cpu.abilist=arm64-v8a,armeabi-v7a,armeabi
ro.hardware=bifrost
ro.product.model=Bifrost Emulator
ro.product.manufacturer=Bifrost
ro.product.brand=Bifrost
ro.product.name=bifrost
ro.product.device=bifrost
ro.board.platform=bifrost
# Indicate this is an emulator (some apps check this)
ro.kernel.qemu=1
ro.boot.qemu=1
EOF

# /system/etc/permissions — minimal Android permissions file
mkdir -p "$ROOTFS/system/etc/permissions"
cat > "$ROOTFS/system/etc/permissions/handheld_core_hardware.xml" <<'EOF'
<?xml version="1.0" encoding="utf-8"?>
<permissions>
    <feature name="android.hardware.touchscreen" />
    <feature name="android.hardware.audio.output" />
    <feature name="android.hardware.microphone" />
    <feature name="android.hardware.screen.landscape" />
    <feature name="android.hardware.screen.portrait" />
    <feature name="android.hardware.opengles.aep" />
</permissions>
EOF

# /data — Android-style data directory (writable)
mkdir -p "$ROOTFS/data/app" "$ROOTFS/data/data" "$ROOTFS/data/local/tmp"
chmod 1777 "$ROOTFS/data/local/tmp"

# /sdcard — Android external storage symlink
[ -e "$ROOTFS/sdcard" ] || ln -sf /data/media/0 "$ROOTFS/sdcard"
mkdir -p "$ROOTFS/data/media/0"

# ── Timezone setup ─────────────────────────────────────────────────────
# Propagate the host's timezone to the guest rootfs so locale-aware
# programs (date, uptime, ls -l's month names, etc.) display the user's
# local time. We try (in order):
#   1. /etc/localtime on the host (canonical zoneinfo file or symlink)
#   2. $TZ env var → /usr/share/zoneinfo/$TZ
#   3. Fallback: copy /usr/share/zoneinfo/UTC (always available)
# We also write /etc/timezone with the zone name (e.g., "America/New_York")
# — some programs (e.g., toybox date) read this directly.
ZONEINFO_DIR="$ROOTFS/usr/share/zoneinfo"
mkdir -p "$ZONEINFO_DIR"
TZ_NAME=""
if [ -n "${TZ:-}" ]; then
    TZ_NAME="$TZ"
elif [ -f /etc/timezone ]; then
    TZ_NAME="$(cat /etc/timezone)"
elif [ -L /etc/localtime ]; then
    # /etc/localtime is a symlink → zoneinfo path is in the link target
    LINK_TARGET="$(readlink -f /etc/localtime 2>/dev/null || true)"
    case "$LINK_TARGET" in
        */zoneinfo/*)
            TZ_NAME="${LINK_TARGET##*zoneinfo/}"
            ;;
    esac
fi

# Resolve the zoneinfo file to copy.
TZ_FILE=""
if [ -n "$TZ_NAME" ] && [ -f "/usr/share/zoneinfo/$TZ_NAME" ]; then
    TZ_FILE="/usr/share/zoneinfo/$TZ_NAME"
elif [ -f /etc/localtime ]; then
    # /etc/localtime is a regular file (copy of zoneinfo)
    TZ_FILE="/etc/localtime"
    TZ_NAME="${TZ_NAME:-UTC}"
elif [ -f /usr/share/zoneinfo/UTC ]; then
    TZ_FILE="/usr/share/zoneinfo/UTC"
    TZ_NAME="UTC"
fi

if [ -n "$TZ_FILE" ]; then
    # Create the zoneinfo subdirectory structure (e.g., America/New_York
    # needs /usr/share/zoneinfo/America/ to exist).
    if [ -n "$TZ_NAME" ] && [[ "$TZ_NAME" == */* ]]; then
        TZ_DIR="${TZ_NAME%/*}"
        mkdir -p "$ZONEINFO_DIR/$TZ_DIR"
        cp -f "$TZ_FILE" "$ZONEINFO_DIR/$TZ_NAME"
    fi
    # /etc/localtime — copy (not symlink) so the guest doesn't need to
    # resolve a host path.
    cp -f "$TZ_FILE" "$ROOTFS/etc/localtime"
    # /etc/timezone — text file with the zone name
    echo "$TZ_NAME" > "$ROOTFS/etc/timezone"
    echo "Timezone set to: $TZ_NAME"
else
    echo "Warning: could not find a zoneinfo file. Guest will default to UTC."
fi

# ── Locale setup (Turn 74) ───────────────────────────────────────────
# glibc programs use locale data for decimal separators, date formats,
# character classification, etc. Without locale data, glibc falls back
# to the C locale (which is usually fine, but some programs expect
# locale.dir to exist).
mkdir -p "$ROOTFS/usr/share/locale"
mkdir -p "$ROOTFS/usr/share/locale/C/LC_MESSAGES"
# Minimal C locale — glibc's default when no locale is set.
cat > "$ROOTFS/usr/share/locale/locale.alias" <<'EOF'
C C
POSIX C
EOF

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
