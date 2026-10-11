#!/usr/bin/env bash
# Presentation-clock and guest title/log regression. UI placement is a desktop check.
set -euo pipefail
cd "$(dirname "$0")/../.."
EMU=${EMU:-./bifrost-emu}
TASK_TMP=$(mktemp -d)
trap 'rm -rf "$TASK_TMP"' EXIT
${CXX:-g++} -std=c++17 -pthread -IInclude -Itools/vulkan-headers/include Tests/Unit/window_stats_clock.cpp \
    Source/System/Graphics/Display/WindowStats.cpp -ldl -o "$TASK_TMP/clock"
"$TASK_TMP/clock"
make cross SRC=Tests/Apps/test_window_stats.c OUT=Tests/Apps/test_window_stats.elf > "$TASK_TMP/build.log" 2>&1
run_probe() {
    local log=$1; shift
    local rc=0
    "$EMU" "$@" > "$log" 2>&1 || rc=$?
    if [[ $rc == 77 ]]; then cat "$log"; exit 77; fi
    if [[ $rc != 0 ]]; then cat "$log"; exit "$rc"; fi
    rg -q 'window_stats: ALL PASS' "$log"
}
# Override inherited diagnostics so each invocation checks a distinct mode.
unset BIFROST_WINDOW_STATS BIFROST_WINDOW_STATS_TITLE
export BIFROST_WINDOW_STATS_WINDOW=0 BIFROST_WINDOW_STATS_OVERLAY=0 BIFROST_RENDER_LOG=1
run_probe "$TASK_TMP/default.log" Tests/Apps/test_window_stats.elf title
[[ $(rg -c '^\[render\]' "$TASK_TMP/default.log") == 2 ]]
export BIFROST_WINDOW_STATS=0
run_probe "$TASK_TMP/env-disabled.log" Tests/Apps/test_window_stats.elf
run_probe "$TASK_TMP/title.log" --window_stats Tests/Apps/test_window_stats.elf title
rg -q 'idle 0.0 FPS' "$TASK_TMP/title.log"
run_probe "$TASK_TMP/disabled.log" --window-stats --no-window-stats-title --no-render-log Tests/Apps/test_window_stats.elf
if rg -q '^\[render\]' "$TASK_TMP/disabled.log"; then cat "$TASK_TMP/disabled.log"; exit 1; fi
unset BIFROST_WINDOW_STATS BIFROST_WINDOW_STATS_TITLE
run_probe "$TASK_TMP/all-disabled.log" --no-window-stats Tests/Apps/test_window_stats.elf
printf 'window_stats regression: ALL PASS\n'
