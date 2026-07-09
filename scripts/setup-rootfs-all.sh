#!/usr/bin/env bash
# setup-rootfs-all.sh — one-command rootfs setup for bifrost-emu.
#
# This script does EVERYTHING needed to set up a working rootfs:
#   1. Fetches the glibc and musl cross-toolchains (if not present)
#   2. Runs setup-rootfs.sh to create the rootfs with glibc + musl libs
#   3. Fetches real-world shared libraries (libssl, libpcre2, etc.)
#   4. Fetches curl's dependency tree (libnghttp2, libssh2, etc.)
#
# After this script completes, you can run dynamically-linked AArch64
# binaries (both glibc and musl) including curl, iperf3, coreutils, etc.
#
# Usage:
#   ./scripts/setup-rootfs-all.sh              # full setup (~250 MB download)
#   ./scripts/setup-rootfs-all.sh --no-glibc   # skip glibc toolchain
#   ./scripts/setup-rootfs-all.sh --no-musl    # skip musl toolchain
#   ./scripts/setup-rootfs-all.sh --no-realworld  # skip real-world libs
#   ./scripts/setup-rootfs-all.sh --no-curl    # skip curl deps
#   ./scripts/setup-rootfs-all.sh --help
#
# The script is idempotent: re-running it refreshes everything.
# Each download is bounded by a 90-second timeout (fail-fast).
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

# Pretty printing
if [ -t 1 ]; then
    C_GRN='\033[0;32m'; C_YLW='\033[0;33m'; C_BLU='\033[0;34m'
    C_RST='\033[0m'; C_BOLD='\033[1m'
else
    C_GRN=''; C_YLW=''; C_BLU=''; C_RST=''; C_BOLD=''
fi

log()     { printf "${C_BLU}▸${C_RST} %s\n" "$*"; }
log_ok()  { printf "${C_GRN}✓${C_RST} %s\n" "$*"; }
log_warn(){ printf "${C_YLW}⚠${C_RST} %s\n" "$*"; }
log_step(){ printf "\n${C_BOLD}${C_BLU}━━ %s ━━${C_RST}\n" "$*"; }

# Parse args
FETCH_GLIBC=1
FETCH_MUSL=1
FETCH_REALWORLD=1
FETCH_CURL=1

while [ $# -gt 0 ]; do
    case "$1" in
        --no-glibc)      FETCH_GLIBC=0 ;;
        --no-musl)       FETCH_MUSL=0 ;;
        --no-realworld)  FETCH_REALWORLD=0 ;;
        --no-curl)       FETCH_CURL=0 ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            log_warn "Unknown option: $1 (try --help)"
            shift
            ;;
    esac
    shift
done

log_step "bifrost-emu rootfs setup (all-in-one)"

# ── Step 1: Fetch toolchains ──────────────────────────────────────────
if [ "$FETCH_GLIBC" = "1" ]; then
    log_step "Step 1a: Fetching glibc cross-toolchain (~130 MB)"
    if [ -d tools/aarch64-linux-gnu-cross ]; then
        log_ok "glibc toolchain already present"
    else
        if ./tools/fetch-glibc-toolchain.sh; then
            log_ok "glibc toolchain fetched"
        else
            log_warn "glibc toolchain fetch failed — glibc dynamic tests will skip"
        fi
    fi
fi

if [ "$FETCH_MUSL" = "1" ]; then
    log_step "Step 1b: Fetching musl cross-toolchain (~104 MB)"
    if [ -d tools/aarch64-linux-musl-cross ]; then
        log_ok "musl toolchain already present"
    else
        if ./tools/fetch-musl-toolchain.sh; then
            log_ok "musl toolchain fetched"
        else
            log_warn "musl toolchain fetch failed — musl dynamic tests will skip"
        fi
    fi
fi

# ── Step 2: Create rootfs ─────────────────────────────────────────────
log_step "Step 2: Creating rootfs (glibc + musl libraries)"
if [ -d tools/aarch64-linux-gnu-cross ] || [ -d tools/aarch64-linux-musl-cross ]; then
    if ./scripts/setup-rootfs.sh; then
        log_ok "rootfs created at $PROJECT_ROOT/rootfs"
    else
        log_warn "rootfs setup had issues — some dynamic tests may skip"
    fi
else
    log_warn "No toolchain present — skipping rootfs creation"
    log_warn "Run this script again without --no-glibc/--no-musl to enable"
fi

# ── Step 3: Fetch real-world shared libraries ─────────────────────────
if [ "$FETCH_REALWORLD" = "1" ]; then
    log_step "Step 3: Fetching real-world shared libraries"
    if [ -d rootfs ]; then
        if ./scripts/fetch-realworld-libs.sh; then
            log_ok "real-world libraries fetched"
        else
            log_warn "real-world lib fetch had issues (some may already be present)"
        fi
    else
        log_warn "No rootfs — skipping real-world lib fetch"
    fi
fi

# ── Step 4: Fetch curl dependencies ───────────────────────────────────
if [ "$FETCH_CURL" = "1" ]; then
    log_step "Step 4: Fetching curl dependency libraries"
    if [ -d rootfs ]; then
        if ./scripts/fetch-curl-deps.sh; then
            log_ok "curl dependencies fetched"
        else
            log_warn "curl dep fetch had issues (some may already be present)"
        fi
    else
        log_warn "No rootfs — skipping curl dep fetch"
    fi
fi

# ── Summary ───────────────────────────────────────────────────────────
log_step "Setup complete!"
echo
if [ -d rootfs ]; then
    LIB_COUNT=$(find rootfs -name '*.so*' -o -name 'ld-*' | wc -l)
    echo "  rootfs at: $PROJECT_ROOT/rootfs"
    echo "  libraries: $LIB_COUNT files"
    echo
    echo "  Try:"
    echo "    ./bifrost-emu --rootfs ./rootfs ctest_real/hello_dyn_glibc.elf"
    echo "    ./bifrost-emu --rootfs ./rootfs ctest_real/test_dyn_write.elf"
    echo
    echo "  Or set BIFROST_ROOT:"
    echo "    export BIFROST_ROOT=\$PWD/rootfs"
    echo "    ./bifrost-emu ctest_real/hello_dyn_glibc.elf"
else
    echo "  rootfs was not created. Check for toolchain fetch errors above."
fi
echo
