#!/usr/bin/env bash
# find-host-tools.sh — find the host tools the Android setup runs, without root and wherever they are.
#
# Usage:
#   Scripts/Setup/find-host-tools.sh TOOL...    # make each tool reachable, or say what is missing (exit 1)
#   Scripts/Setup/find-host-tools.sh --where TOOL   # print where TOOL is, searching the same places
#   source Scripts/Setup/find-host-tools.sh         # defines find_host_tool NAME
#
# A tool that is not on PATH is looked for in the places a distribution or a user puts it: the sbin
# directories (Debian and Fedora keep debugfs in /usr/sbin, which an ordinary user's PATH lacks), JDK and
# LLVM directories (clang-18, a non-default JDK's keytool), ~/.local/bin, /opt, Nix, Homebrew, conda and
# SDKMAN. BIFROST_TOOL_DIRS (a colon-separated list) adds directories; BIFROST_TOOL_SEARCH=0 turns the
# built-in list off, leaving PATH and BIFROST_TOOL_DIRS. A tool found off PATH is linked
# into build/host-bin, which the Makefile appends to PATH, so the setup scripts find it by name and an
# earlier tool on PATH is never shadowed.

HOST_TOOLS_HERE=${BASH_SOURCE[0]%/*}; [ "$HOST_TOOLS_HERE" != "${BASH_SOURCE[0]}" ] || HOST_TOOLS_HERE=.
HOST_TOOLS_ROOT=$(cd "$HOST_TOOLS_HERE/../.." && pwd)

# Directories to search, one per line; patterns are expanded and sorted newest first.
host_tool_dirs() {
    local dir
    if [ -n "${BIFROST_TOOL_DIRS:-}" ]; then
        local IFS=:
        for dir in $BIFROST_TOOL_DIRS; do [ -z "$dir" ] || echo "$dir"; done
    fi
    [ "${BIFROST_TOOL_SEARCH:-1}" != 0 ] || return 0
    for dir in /usr/local/sbin /usr/sbin /sbin; do echo "$dir"; done
    [ -z "${JAVA_HOME:-}" ] || echo "$JAVA_HOME/bin"
    [ -z "${CONDA_PREFIX:-}" ] || echo "$CONDA_PREFIX/bin"
    {
        for dir in /usr/lib/jvm/*/bin /usr/lib64/jvm/*/bin /usr/java/*/bin /usr/lib/llvm*/bin /usr/lib64/llvm*/bin \
                   /usr/local/llvm*/bin "${HOME:-/nonexistent}"/.sdkman/candidates/java/*/bin; do
            [ -d "$dir" ] && echo "$dir"
        done
    } | sort -rV
    for dir in "${HOME:-/nonexistent}/.local/bin" "${HOME:-/nonexistent}/bin" /opt/*/bin \
               "${HOME:-/nonexistent}/.nix-profile/bin" /nix/var/nix/profiles/default/bin /run/current-system/sw/bin \
               /home/linuxbrew/.linuxbrew/bin "${HOME:-/nonexistent}/.linuxbrew/bin" \
               "${HOME:-/nonexistent}"/miniconda3/bin "${HOME:-/nonexistent}"/mambaforge/bin \
               "${HOME:-/nonexistent}"/.cargo/bin; do
        [ -d "$dir" ] && echo "$dir"
    done
}

# Prints the path of the tool, the newest of NAME-<version> when only versioned ones exist; fails when absent.
find_host_tool() {
    local name=$1 dir found
    found=$(command -v "$name" 2>/dev/null) && [ -x "$found" ] && { echo "$found"; return 0; }
    while IFS= read -r dir; do
        if [ -x "$dir/$name" ] && [ ! -d "$dir/$name" ]; then echo "$dir/$name"; return 0; fi
    done < <(host_tool_dirs)
    while IFS= read -r dir; do
        found=$(ls -1 "$dir/$name"-[0-9]* 2>/dev/null | sort -rV | head -n 1)
        if [ -n "$found" ] && [ -x "$found" ]; then echo "$found"; return 0; fi
    done < <(host_tool_dirs)
    return 1
}

# What provides a tool, for the message when it cannot be found.
host_tool_hint() {
    case "$1" in
        debugfs)      echo "e2fsprogs" ;;
        readelf)      echo "binutils" ;;
        ld.lld)       echo "lld" ;;
        clang)        echo "clang" ;;
        java|javac|keytool) echo "a JDK (default-jdk-headless, java-latest-openjdk-devel, jdk-openjdk)" ;;
        python3)      echo "python3" ;;
        curl)         echo "curl" ;;
        *)            echo "$1" ;;
    esac
}

host_tools_main() {
    local bin="${BIFROST_HOST_BIN:-$HOST_TOOLS_ROOT/build/host-bin}" tool found missing=() linked=()
    if [ "${1:-}" = --where ]; then
        [ $# -eq 2 ] || { echo "usage: find-host-tools.sh --where TOOL" >&2; return 2; }
        find_host_tool "$2"
        return
    fi
    [ $# -gt 0 ] || { echo "usage: find-host-tools.sh TOOL..." >&2; return 2; }
    for tool in "$@"; do
        found=$(PATH="${PATH//$bin/}" command -v "$tool" 2>/dev/null) && [ -x "$found" ] && continue
        if found=$(find_host_tool "$tool"); then
            mkdir -p "$bin"
            ln -sfn "$found" "$bin/$tool"
            linked+=("$tool ($found)")
        else
            rm -f "$bin/$tool"
            missing+=("$tool")
        fi
    done
    [ "${#linked[@]}" -eq 0 ] || echo "Found off PATH, using: ${linked[*]}"
    if [ "${#missing[@]}" -gt 0 ]; then
        echo "error: could not find: ${missing[*]}" >&2
        echo "  looked on PATH, in the sbin directories, JDK and LLVM directories, ~/.local/bin, /opt/*/bin," >&2
        echo "  Nix, Homebrew, conda and SDKMAN directories, and BIFROST_TOOL_DIRS." >&2
        for tool in "${missing[@]}"; do echo "  $tool: provided by $(host_tool_hint "$tool")" >&2; done
        echo "  make deps installs them (needs sudo); without root, install them under ~/.local/bin or set" >&2
        echo "  BIFROST_TOOL_DIRS=/their/directory and run again." >&2
        return 1
    fi
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    host_tools_main "$@"
fi
