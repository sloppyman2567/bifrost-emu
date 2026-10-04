#!/usr/bin/env bash
# Presentation-clock and guest title/log regression. UI placement is a desktop check.
set -euo pipefail
cd "$(dirname "$0")/.."
EMU=${EMU:-./bifrost-emu}
TASK_TMP=$(mktemp -d)
trap 'rm -rf "$TASK_TMP"' EXIT
${CXX:-g++} -std=c++17 -pthread -Iinclude ctest/window_stats_clock.cpp \
    src/frost_graphics/window_stats.cpp -ldl -o "$TASK_TMP/clock"
"$TASK_TMP/clock"
make cross SRC=ctest_real/test_window_stats.c OUT=ctest_real/test_window_stats.elf > "$TASK_TMP/build.log" 2>&1
run_probe() {
    local log=$1; shift
    local rc=0
    "$EMU" "$@" > "$log" 2>&1 || rc=$?
    if [[ $rc == 77 ]]; then cat "$log"; exit 77; fi
    if [[ $rc != 0 ]]; then cat "$log"; exit "$rc"; fi
    rg -q 'window_stats: ALL PASS' "$log"
}
# Override inherited diagnostics so each invocation checks a distinct mode.
export BIFROST_WINDOW_STATS=0 BIFROST_WINDOW_STATS_TITLE=0
export BIFROST_WINDOW_STATS_WINDOW=0 BIFROST_WINDOW_STATS_OVERLAY=0 BIFROST_RENDER_LOG=1
run_probe "$TASK_TMP/default.log" ctest_real/test_window_stats.elf
[[ $(rg -c '^\[render\]' "$TASK_TMP/default.log") == 2 ]]
unset BIFROST_WINDOW_STATS_TITLE
run_probe "$TASK_TMP/title.log" --window_stats ctest_real/test_window_stats.elf title
rg -q 'idle 0.0 FPS' "$TASK_TMP/title.log"
run_probe "$TASK_TMP/disabled.log" --window-stats --no-window-stats-title --no-render-log ctest_real/test_window_stats.elf
if rg -q '^\[render\]' "$TASK_TMP/disabled.log"; then cat "$TASK_TMP/disabled.log"; exit 1; fi
printf 'window_stats regression: ALL PASS\n'
