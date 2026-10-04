#!/usr/bin/env bash
# Fail graphics SDK includes even on developer machines where they are installed.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
headers=$(mktemp -d)
trap 'rm -rf -- "$headers"' EXIT
mkdir -p "$headers/SDL2" "$headers/GL" "$headers/EGL"
for name in SDL2/SDL.h SDL2/SDL_syswm.h wayland-util.h GL/gl.h EGL/egl.h; do
    printf '#error Headless compilation must not include graphics SDK headers\n' > "$headers/$name"
done
"${CXX:-g++}" -std=c++17 -pthread -I"$headers" -Iinclude -Isrc -fsyntax-only \
    src/frost_graphics/*.cpp src/interp/interp_fp.cpp src/audio/audio.cpp
echo 'Headless graphics SDK independence: PASS'
