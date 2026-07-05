#!/usr/bin/env bash
# setup.sh — one-click bootstrap for bifrost-emu.
#
# This script performs everything a fresh checkout needs to go from zero
# to a passing test suite:
#
#   1. Verifies the build toolchain (g++, make, curl/wget) is present.
#   2. Builds the emulator binary (`make -j$(nproc)`).
#   3. Optionally fetches the musl aarch64 cross-toolchain (only if the
#      user wants to rebuild the bundled .elf test programs).
#   4. Cross-compiles every .c test under ctest/ and ctest_real/ that
#      ships with the source tree, so `make check` can run them.
#   5. Optionally sets up the rootfs for dynamic linking tests.
#   6. Runs the test suite (`make check`) to confirm everything works.
#
# Usage:
#   ./scripts/setup.sh                 # full setup + tests
#   ./scripts/setup.sh --no-tests      # full setup, skip test run
#   ./scripts/setup.sh --no-toolchain  # skip toolchain fetch + cross-compile
#   ./scripts/setup.sh --no-rootfs     # skip rootfs setup
#   ./scripts/setup.sh --quick         # only run --quick test subset
#   ./scripts/setup.sh --help
#
# Idempotent: re-running it refreshes the build and re-checks everything.
# Exit codes: 0 on success, non-zero on any failure.

set -euo pipefail

# ── Resolve project root (parent of scripts/) ──────────────────────────
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

# ── Pretty printing ────────────────────────────────────────────────────
if [ -t 1 ]; then
    C_RED='\033[0;31m'; C_GRN='\033[0;32m'; C_YLW='\033[0;33m'
    C_BLU='\033[0;34m'; C_DIM='\033[2m'; C_RST='\033[0m'; C_BOLD='\033[1m'
else
    C_RED=''; C_GRN=''; C_YLW=''; C_BLU=''; C_DIM=''; C_RST=''; C_BOLD=''
fi

log()     { printf "${C_BLU}▸${C_RST} %s\n" "$*"; }
log_ok()  { printf "${C_GRN}✓${C_RST} %s\n" "$*"; }
log_warn(){ printf "${C_YLW}⚠${C_RST} %s\n" "$*"; }
log_err() { printf "${C_RED}✗${C_RST} %s\n" "$*" >&2; }
log_step(){ printf "\n${C_BOLD}${C_BLU}━━ %s ━━${C_RST}\n" "$*"; }

# ── Parse args ─────────────────────────────────────────────────────────
RUN_TESTS=1
FETCH_TOOLCHAIN=1
SETUP_ROOTFS=1
QUICK=0

while [ $# -gt 0 ]; do
    case "$1" in
        --no-tests)     RUN_TESTS=0 ;;
        --no-toolchain) FETCH_TOOLCHAIN=0 ;;
        --no-rootfs)    SETUP_ROOTFS=0 ;;
        --quick)        QUICK=1 ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            log_err "Unknown option: $1 (try --help)"
            exit 1
            ;;
    esac
    shift
done

# ── Step 1: Check build prerequisites ──────────────────────────────────
log_step "Step 1/5: Checking prerequisites"

MISSING=()
if ! command -v g++ >/dev/null 2>&1; then
    MISSING+=("g++")
fi
if ! command -v make >/dev/null 2>&1; then
    MISSING+=("make")
fi
if ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; then
    MISSING+=("curl or wget")
fi

if [ ${#MISSING[@]} -gt 0 ]; then
    log_err "Missing required tools: ${MISSING[*]}"
    log_err "On Debian/Ubuntu: sudo apt install build-essential curl"
    log_err "On Fedora/RHEL:   sudo dnf install gcc-c++ make curl"
    log_err "On Alpine:         apk add build-base curl"
    exit 1
fi

GXX_VERSION=$(g++ -dumpversion 2>/dev/null || echo "unknown")
log_ok "Found g++ $GXX_VERSION, make $(make --version 2>/dev/null | head -1 | awk '{print $3}')"

# ── Step 2: Build the emulator ─────────────────────────────────────────
log_step "Step 2/5: Building bifrost-emu"

log "Running make -j$(nproc) ..."
if ! make -j"$(nproc)" 2>&1 | tail -5; then
    log_err "Build failed. See above for compiler errors."
    exit 1
fi

if [ ! -x ./bifrost-emu ]; then
    log_err "Build produced no bifrost-emu binary."
    exit 1
fi

log_ok "bifrost-emu built: $(./bifrost-emu --version)"

# ── Step 3: Fetch musl toolchain + cross-compile tests ─────────────────
if [ "$FETCH_TOOLCHAIN" = "1" ]; then
    log_step "Step 3/5: Fetching musl toolchain + cross-compiling tests"

    if [ ! -x tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc ]; then
        log "Fetching musl toolchain (one-time ~104 MB download) ..."
        if ! ./tools/fetch-musl-toolchain.sh; then
            log_warn "Toolchain fetch failed. Skipping cross-compile step."
            log_warn "Pre-built .elf files in ctest/ and ctest_real/ will be used as-is."
        fi
    else
        log_ok "musl toolchain already present"
    fi

    if [ -x tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc ]; then
        CC=tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc
        CROSS_COUNT=0
        SKIP_COUNT=0

        # Cross-compile every .c test that doesn't yet have a matching .elf.
        # This is the inverse of the test runner's "skip if .elf missing"
        # behavior — we pre-build all the .elf files so the test runner
        # has something to run.
        for src in ctest/*.c ctest_real/*.c; do
            [ -f "$src" ] || continue
            elf="${src%.c}.elf"

            # Skip if .elf is newer than .c (already up-to-date).
            if [ -f "$elf" ] && [ "$elf" -nt "$src" ]; then
                SKIP_COUNT=$((SKIP_COUNT + 1))
                continue
            fi

            if "$CC" -static -O2 -o "$elf" "$src" 2>/dev/null; then
                CROSS_COUNT=$((CROSS_COUNT + 1))
            else
                # Some test files require special flags (e.g. test_lse_inline.c
                # needs -march=armv8.1-a+lse). Skip failures silently — the
                # test runner will mark them as "binary not found" and skip.
                :
            fi
        done

        log_ok "Cross-compiled $CROSS_COUNT test binaries ($SKIP_COUNT already up-to-date)"
    fi
else
    log_step "Step 3/5: Skipping toolchain fetch (--no-toolchain)"
fi

# ── Step 4: Set up rootfs for dynamic linking tests ────────────────────
if [ "$SETUP_ROOTFS" = "1" ]; then
    log_step "Step 4/5: Setting up rootfs (for dynamic linking tests)"

    if [ -d tools/aarch64-linux-musl-cross ] || [ -d tools/aarch64-linux-gnu-cross ]; then
        if ./scripts/setup-rootfs.sh 2>&1 | tail -3; then
            log_ok "rootfs ready at $PROJECT_ROOT/rootfs/"
        else
            log_warn "rootfs setup had issues — dynamic tests may be skipped."
        fi
    else
        log_warn "No toolchain present — skipping rootfs setup."
        log_warn "Run ./scripts/setup.sh again without --no-toolchain to enable."
    fi
else
    log_step "Step 4/5: Skipping rootfs setup (--no-rootfs)"
fi

# ── Step 5: Run the test suite ─────────────────────────────────────────
if [ "$RUN_TESTS" = "1" ]; then
    log_step "Step 5/5: Running test suite"

    ARGS=""
    [ "$QUICK" = "1" ] && ARGS="--quick"

    if ./scripts/run_tests.sh $ARGS; then
        log_ok "All tests passed."
    else
        log_err "Some tests failed. Run './scripts/run_tests.sh --verbose' for details."
        exit 1
    fi
else
    log_step "Step 5/5: Skipping tests (--no-tests)"
fi

# ── Done ───────────────────────────────────────────────────────────────
printf "\n${C_BOLD}${C_GRN}━━ Setup complete ━━${C_RST}\n"
echo
echo "  bifrost-emu is ready. Try:"
echo "    ./bifrost-emu ctest_real/hello.elf"
echo "    ./bifrost-emu ctest_real/toybox echo hello"
echo "    ./scripts/run_tests.sh --quick"
echo
echo "  For dynamic linking tests:"
echo "    BIFROST_ROOT=$PROJECT_ROOT/rootfs ./bifrost-emu ctest_real/hello_dyn_musl.elf"
echo
