#!/usr/bin/env bash
# install-rootfs.sh — install a standard AArch64 guest rootfs where bifrost-emu
# finds it without flags (see Docs/ROOTFS.md).
#
# Usage:
#   ./Scripts/Setup/install-rootfs.sh                # per user: ~/.local/share/bifrost/rootfs
#   ./Scripts/Setup/install-rootfs.sh --system       # /usr/local/share/bifrost/rootfs (needs root)
#   ./Scripts/Setup/install-rootfs.sh --prefix DIR   # DIR/share/bifrost/rootfs
#   ./Scripts/Setup/install-rootfs.sh --full         # also real-world and curl libraries
#   ./Scripts/Setup/install-rootfs.sh --profile NAME # a named rootfs: <data>/rootfs.d/NAME (bifrost-emu --rootfs-profile NAME)
#   ./Scripts/Setup/install-rootfs.sh --games        # also game libraries (OpenAL, SDL2_ttf, Vorbis, ...); profile "games"
#   ./Scripts/Setup/install-rootfs.sh --no-fetch     # never download; use toolchains already in Tools/
#   ./Scripts/Setup/install-rootfs.sh --uninstall    # remove the rootfs at the chosen location
#   ./Scripts/Setup/install-rootfs.sh --help
#
# The rootfs is built next to its destination and renamed into place, so a
# failed run leaves an existing install untouched. Run it as your own user;
# it downloads the glibc and musl toolchains into the checkout's Tools/ unless
# --no-fetch is given (the make targets use --no-fetch so root never downloads).
# --games downloads the game libraries from Debian (fetch-game-libs.py) even with --no-fetch.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$PROJECT_ROOT"

DEST=""
PROFILE=""
GAMES=0
FULL=0
FETCH=1
UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --user)      BASE="${XDG_DATA_HOME:-${HOME:?HOME is not set}/.local/share}/bifrost" ;;
        --system)    BASE="/usr/local/share/bifrost" ;;
        --prefix)    [ $# -ge 2 ] || { echo "--prefix needs a directory" >&2; exit 2; }
                     BASE="${2%/}/share/bifrost"; shift ;;
        --profile)   [ $# -ge 2 ] || { echo "--profile needs a name" >&2; exit 2; }
                     PROFILE="$2"; shift ;;
        --games)     GAMES=1 ;;
        --full)      FULL=1 ;;
        --no-fetch)  FETCH=0 ;;
        --uninstall) UNINSTALL=1 ;;
        --help|-h)   sed -n '2,/^set /p' "$0" | sed '$d; s/^# \?//'; exit 0 ;;
        *)           echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done
# The default rootfs is <base>/rootfs; a named profile is <base>/rootfs.d/NAME. --games alone means "games".
[ "$GAMES" = 1 ] && [ -z "$PROFILE" ] && PROFILE=games
if [ -n "$PROFILE" ] && ! [[ "$PROFILE" =~ ^[A-Za-z0-9_][A-Za-z0-9._-]{0,63}$ ]]; then
    echo "error: invalid profile name '$PROFILE' (letters, digits, '.', '_', '-'; max 64)" >&2; exit 2
fi
[ -n "${BASE:-}" ] || BASE="${XDG_DATA_HOME:-${HOME:?HOME is not set}/.local/share}/bifrost"
if [ -n "$PROFILE" ]; then DEST="$BASE/rootfs.d/$PROFILE"; else DEST="$BASE/rootfs"; fi

has_loader() {
    [ -e "$1/lib/ld-linux-aarch64.so.1" ] || [ -e "$1/lib/ld-musl-aarch64.so.1" ]
}

if [ "$UNINSTALL" = 1 ]; then
    if [ ! -e "$DEST" ]; then echo "nothing to remove at $DEST"; exit 0; fi
    # Only delete something that looks like a rootfs, never an arbitrary directory.
    has_loader "$DEST" || { echo "refusing to remove $DEST: no AArch64 loader inside" >&2; exit 1; }
    rm -rf -- "$DEST"
    echo "removed $DEST"
    exit 0
fi

if [ ! -d Tools/aarch64-linux-gnu-cross ] || [ ! -d Tools/aarch64-linux-musl-cross ]; then
    if [ "$FETCH" = 0 ]; then
        echo "error: the glibc/musl toolchains are missing from Tools/." >&2
        echo "  Run ./Scripts/Setup/install-rootfs.sh as your own user first (it downloads them)," >&2
        echo "  or ./Tools/Fetch/fetch-glibc-toolchain.sh and ./Tools/Fetch/fetch-musl-toolchain.sh." >&2
        exit 1
    fi
    [ -d Tools/aarch64-linux-gnu-cross ] || ./Tools/Fetch/fetch-glibc-toolchain.sh
    [ -d Tools/aarch64-linux-musl-cross ] || ./Tools/Fetch/fetch-musl-toolchain.sh
fi

PARENT="$(dirname "$DEST")"
mkdir -p "$PARENT"
STAGE="$(mktemp -d "$PARENT/.rootfs-new.XXXXXX")"
OLD=""
cleanup() { rm -rf -- "$STAGE" ${OLD:+"$OLD"}; }
trap cleanup EXIT

echo "Building rootfs in $STAGE"
BIFROST_ROOTFS_INSTALLER=1 ./Scripts/Setup/setup-rootfs.sh "$STAGE"
if [ "$FULL" = 1 ]; then
    ./Scripts/Setup/fetch-realworld-libs.sh "$STAGE"
    ./Scripts/Setup/fetch-curl-deps.sh "$STAGE"
    python3 -I Scripts/Setup/make-ld-cache.py "$STAGE"
fi
if [ "$GAMES" = 1 ]; then
    python3 -I Scripts/Setup/fetch-game-libs.py "$STAGE"
    python3 -I Scripts/Setup/make-ld-cache.py "$STAGE"
fi
has_loader "$STAGE" || { echo "error: the built rootfs has no AArch64 loader" >&2; exit 1; }
chmod 755 "$STAGE"   # mktemp creates 0700; other users must be able to read an installed rootfs

if [ -e "$DEST" ]; then
    OLD="$PARENT/.rootfs-old.$$"
    mv -- "$DEST" "$OLD"
fi
if ! mv -- "$STAGE" "$DEST"; then
    [ -z "$OLD" ] || { mv -- "$OLD" "$DEST" && OLD=""; }
    echo "error: cannot move the new rootfs to $DEST" >&2
    exit 1
fi
echo "Installed rootfs: $DEST"
if [ -n "$PROFILE" ]; then
    echo "Use it with: bifrost-emu --rootfs-profile $PROFILE PROGRAM  (see bifrost-emu --rootfs-list)"
else
    echo "bifrost-emu finds it automatically (see Docs/ROOTFS.md for the search order)."
fi
