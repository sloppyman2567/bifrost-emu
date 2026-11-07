#!/usr/bin/env bash
# Presentation-clock, overlay/sensor and guest title/log regression. UI placement is a desktop check.
set -euo pipefail
# run_tests.sh names the emulator under test in BIFROST_EMU, possibly relative to the caller's directory.
EMU=${BIFROST_EMU:-${EMU:-}}
[[ -z $EMU || $EMU = /* ]] || EMU="$PWD/$EMU"
cd "$(dirname "$0")/../.."
EMU=${EMU:-./bifrost-emu}
TASK_TMP=$(mktemp -d)
trap 'rm -rf "$TASK_TMP"' EXIT
display=Source/System/Graphics/Display
${CXX:-g++} -std=c++17 -pthread -IInclude -ISource -IThirdParty/VulkanHeaders/include Tests/Unit/window_stats_clock.cpp \
    $display/WindowStats.cpp $display/HostSensors.cpp $display/StatsOverlay.cpp -ldl -o "$TASK_TMP/clock"
"$TASK_TMP/clock"
${CXX:-g++} -std=c++17 -pthread -IInclude -ISource -IThirdParty/VulkanHeaders/include Tests/Unit/window_stats_overlay.cpp \
    $display/WindowStats.cpp $display/HostSensors.cpp $display/StatsOverlay.cpp -ldl -o "$TASK_TMP/overlay"
"$TASK_TMP/overlay"
make cross SRC=Tests/Apps/test_window_stats.c OUT=Tests/Apps/test_window_stats.elf > "$TASK_TMP/build.log" 2>&1
run_probe() {
    local log=$1; shift
    local rc=0
    "$EMU" "$@" > "$log" 2>&1 || rc=$?
    if [[ $rc == 77 ]]; then cat "$log"; exit 77; fi
    if [[ $rc != 0 ]]; then cat "$log"; exit "$rc"; fi
    grep -qE 'window_stats: ALL PASS' "$log"
}
# Override inherited diagnostics so each invocation checks a distinct mode. Stats are opt-in: the guest's
# title stays its own until --window-stats (or a mode) asks for them.
unset BIFROST_WINDOW_STATS BIFROST_WINDOW_STATS_TITLE BIFROST_WINDOW_STATS_MODE
export BIFROST_WINDOW_STATS_WINDOW=0 BIFROST_WINDOW_STATS_OVERLAY=0 BIFROST_RENDER_LOG=1
run_probe "$TASK_TMP/default.log" Tests/Apps/test_window_stats.elf
[[ $(grep -cE '^\[render\]' "$TASK_TMP/default.log") == 2 ]]
export BIFROST_WINDOW_STATS_MODE=1   # in the environment a mode only picks a layout; it does not turn stats on
run_probe "$TASK_TMP/env-mode.log" Tests/Apps/test_window_stats.elf
unset BIFROST_WINDOW_STATS_MODE
export BIFROST_WINDOW_STATS=0
run_probe "$TASK_TMP/env-disabled.log" Tests/Apps/test_window_stats.elf
run_probe "$TASK_TMP/title.log" --window_stats Tests/Apps/test_window_stats.elf title
grep -qE 'idle 0.0 FPS' "$TASK_TMP/title.log"
# Modes: 1/minimal and 2/normal show the title; 3/overlay leaves it alone (the overlay needs an Android window).
unset BIFROST_WINDOW_STATS BIFROST_WINDOW_STATS_TITLE
for mode in 1 minimal 2 normal; do
    run_probe "$TASK_TMP/mode-$mode.log" --window-stats-mode "$mode" Tests/Apps/test_window_stats.elf title
done
run_probe "$TASK_TMP/mode-equals.log" --window-stats-mode=minimal Tests/Apps/test_window_stats.elf title
for mode in 3 overlay; do
    run_probe "$TASK_TMP/mode-$mode.log" --window-stats-mode "$mode" Tests/Apps/test_window_stats.elf
done
# --no-window-stats disables every display explicitly; it ends the stats whether it comes before or after a mode.
run_probe "$TASK_TMP/mode-off.log" --window-stats-mode 2 --no-window-stats Tests/Apps/test_window_stats.elf
run_probe "$TASK_TMP/off-mode.log" --no-window-stats --window-stats-mode 2 Tests/Apps/test_window_stats.elf
if "$EMU" --window-stats-mode loud Tests/Apps/test_window_stats.elf > "$TASK_TMP/bad-mode.log" 2>&1; then
    cat "$TASK_TMP/bad-mode.log"; exit 1
fi
grep -qE 'takes 1 \(minimal\), 2 \(normal\) or 3 \(overlay\)' "$TASK_TMP/bad-mode.log"
run_probe "$TASK_TMP/disabled.log" --window-stats --no-window-stats-title --no-render-log Tests/Apps/test_window_stats.elf
if grep -qE '^\[render\]' "$TASK_TMP/disabled.log"; then cat "$TASK_TMP/disabled.log"; exit 1; fi
unset BIFROST_WINDOW_STATS BIFROST_WINDOW_STATS_TITLE
run_probe "$TASK_TMP/all-disabled.log" --no-window-stats Tests/Apps/test_window_stats.elf
printf 'window_stats regression: ALL PASS\n'
