#!/usr/bin/env bash
# BIFROST_PROF=tracy[:TARGET] asks for a Tracy timeline. Any build must accept it: a normal build says it needs
# `make USE_TRACY=1` and runs the program, a Tracy build starts the profiler for this process or only for a matching target, and
# neither runs the BIFROST_PROF sampler. If the Tracy build exists (build/release-sdl1-gl1-tracy), a process that forks 40 times
# with the profiler started is run as a smoke check. It cannot show that a child stops using the parent's profiler (that
# matters for a child that lives long; these exit at once and pass without the guard too), only that nothing hangs.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ $EMU = /* ]] || EMU="$PWD/$EMU"
FIXTURE="$REPO_ROOT/Tests/Unit/test_wait_profile.elf"
FORKER="$REPO_ROOT/Tests/Unit/test_proc_maps_live.elf"
TRACY_EMU="$REPO_ROOT/build/release-sdl1-gl1-tracy/bifrost-emu"
for need in "$EMU" "$FIXTURE"; do [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }; done

fail=0
check() { if [[ $2 == "$3" ]]; then echo "PASS $1"; else echo "FAIL $1 (expected '$2', got '$3')"; fail=1; fi; }
run() { BIFROST_PROF=$1 timeout 60 "${2:-$EMU}" -v "$FIXTURE" 300 2>&1 || true; }

out=$(run tracy)
check "BIFROST_PROF=tracy runs the program" 1 "$(grep -c 'test_wait_profile: ALL PASS' <<<"$out")"
check "it starts Tracy or says the build lacks it, not both" 1 "$(grep -c -E 'Tracy profiling started|needs a build made with' <<<"$out")"
check "it does not run the BIFROST_PROF sampler" 0 "$(grep -c '\] waiting,' <<<"$out")"

out=$(run tracy:no-such-target)
check "a target that matches nothing starts no profiler" 0 "$(grep -c 'Tracy profiling started' <<<"$out")"
check "a target that matches nothing still runs the program" 1 "$(grep -c 'test_wait_profile: ALL PASS' <<<"$out")"

check "BIFROST_PROF=1 is still the sampler" 1 "$(run 1 | grep -c '\] waiting,')"

if [[ -x $TRACY_EMU && -e $FORKER ]]; then
    out=$(BIFROST_PROF=tracy timeout 120 "$TRACY_EMU" "$FORKER" 2>&1 || true)
    check "Tracy build: a target-less run profiles this process" 1 "$(run tracy "$TRACY_EMU" | grep -c 'Tracy profiling started')"
    check "Tracy build: a target in the program path starts it" 1 "$(run tracy:test_wait_profile "$TRACY_EMU" | grep -c 'Tracy profiling started')"
    check "Tracy build: 40 short-lived forked children finish with the profiler started" 1 "$(grep -c 'test_proc_maps_live: ALL PASS' <<<"$out")"
else
    echo "SKIP the Tracy build is not at build/release-sdl1-gl1-tracy (make USE_TRACY=1, after Tools/Fetch/fetch-tracy.sh)"
fi

[[ $fail -eq 0 ]] && echo "tracy_profile: ALL PASS"
exit $fail
