#!/usr/bin/env bash
# Fail graphics SDK includes even on developer machines where they are installed.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
headers=$(mktemp -d)
trap 'rm -rf -- "$headers"' EXIT
mkdir -p "$headers/SDL2" "$headers/GL" "$headers/EGL"
for name in SDL2/SDL.h SDL2/SDL_syswm.h wayland-util.h GL/gl.h EGL/egl.h; do
    printf '#error Headless compilation must not include graphics SDK headers\n' > "$headers/$name"
done
mapfile -d '' -t graphics_sources < <(find Source/System/Graphics \
    Source/System/Android/Surface Source/System/Android/Window -name '*.cpp' -print0)
"${CXX:-g++}" -std=c++17 -pthread -I"$headers" -IInclude -ISource \
    -IThirdParty/VulkanHeaders/include -fsyntax-only \
    "${graphics_sources[@]}" Source/Interpreter/FloatSIMD/Dispatch.cpp Source/System/Audio/Audio.cpp
echo 'Headless graphics SDK independence: PASS'
