#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
task_tmp=$(mktemp -d)
trap 'rm -rf "$task_tmp"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -pthread -IInclude \
    Tests/Unit/fallback_profile.cpp Source/FrostJIT/Runtime/FallbackProfile.cpp \
    -o "$task_tmp/test_fallback_profile"
"$task_tmp/test_fallback_profile" "$task_tmp/inventory.tsv"
