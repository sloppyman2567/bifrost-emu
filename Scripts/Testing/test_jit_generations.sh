#!/usr/bin/env bash
# Code-cache generations: a 1 MiB budget makes the shared JIT fill and be replaced hundreds of times
# while threads sleep in syscalls, run nested calls, rewrite code and take signals (the fixture checks
# every result). The default layout must release every retired generation; the ordinary-frame layout
# (BIFROST_NO_CHAIN_SKIP=1, native nested calls everywhere) pins its generations, so the cap on
# unreleased instances keeps it at the old behaviour (interpreter fallback) and only correctness is checked.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
emu=${BIFROST_EMU:-$repo/bifrost-emu}
fixture="$repo/Tests/Unit/jit_cache_generations.elf"
if [[ "$emu" != /* ]]; then emu="$PWD/$emu"; fi
if [[ ! -f "$fixture" ]]; then echo "Missing $(basename "$fixture"); build with make cross" >&2; exit 1; fi
logs=$(mktemp -d /tmp/bifrost-generations-test.XXXXXX)
trap 'rm -rf -- "$logs"' EXIT
for layout in skip ordinary; do
    args=(BIFROST_JIT_CACHE_MB=1 BIFROST_JIT_CACHE_INITIAL_MB=1)
    [[ "$layout" == ordinary ]] && args+=(BIFROST_NO_CHAIN_SKIP=1)
    env -u BIFROST_ROOT -u BIFROST_NO_CHAIN_SKIP "${args[@]}" \
        timeout -s KILL 120 "$emu" --verbose "$fixture" > "$logs/$layout.log" 2>&1
done
python3 - "$logs" <<'PY'
import pathlib,re,sys
for layout in ('skip','ordinary'):
    log=(pathlib.Path(sys.argv[1])/f'{layout}.log').read_text()
    assert 'jit_cache_generations: ALL PASS' in log,(layout,log[-3000:])
    rows=re.findall(r'generations: retired=(\d+) released=(\d+) blocked_by_threads=(\d+)',log)
    assert rows,(layout,'No generation counters')
    retired,released,_blocked=map(int,rows[-1])
    if layout=='skip':
        # Every retired generation is released (the last one may still be queued when the line prints).
        assert retired>=20,('too few renewals',retired)
        assert released>=retired-1,('retired generations not released',retired,released)
    else:
        assert retired>=1,('no renewal',retired)
    print(f'{layout}: correct results, {retired} generations retired, {released} released')
print('jit_generations: ALL PASS')
PY
