#!/usr/bin/env bash
# Bounded pressure and growth checks; three guest threads validate results.
# Selective invalidation: line publishes and mprotect keep unchanged
# translations, and direct BL callers (chain-skip off) see rewritten callees.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
emu=${BIFROST_EMU:-$repo/bifrost-emu}
fixture="$repo/Tests/Unit/jit_cache_pressure.elf"
retain="$repo/Tests/Unit/jit_cache_invalidation.elf"
if [[ "$emu" != /* ]]; then emu="$PWD/$emu"; fi
for f in "$fixture" "$retain"; do
    if [[ ! -f "$f" ]]; then echo "Missing $(basename "$f"); build with make cross" >&2; exit 1; fi
done
logs=$(mktemp -d /tmp/bifrost-cache-test.XXXXXX)
trap 'rm -rf -- "$logs"' EXIT
for budget in 1 64; do
    env -u BIFROST_ROOT BIFROST_JIT_CACHE_MB="$budget" BIFROST_JIT_CACHE_INITIAL_MB=1 \
        timeout -s KILL 30 "$emu" --verbose "$fixture" > "$logs/$budget.log" 2>&1
done
env -u BIFROST_ROOT timeout -s KILL 30 "$emu" --verbose "$retain" retain > "$logs/retain.log" 2>&1
env -u BIFROST_ROOT BIFROST_NO_CHAIN_SKIP=1 timeout -s KILL 30 "$emu" --verbose "$retain" retain \
    > "$logs/retain-direct.log" 2>&1
python3 - "$logs" <<'PY'
import pathlib,re,sys
for name in ('retain','retain-direct'):
    log=(pathlib.Path(sys.argv[1])/f'{name}.log').read_text()
    assert 'jit_cache_invalidation: ALL PASS' in log,log[-3000:]
    stats=dict((k,tuple(map(int,v.split('/')))) for k,v in
               re.findall(r' (\w+)=(\d+/\d+/\d+)',re.findall(r'invalidate \(calls/retired/kept\):.*',log)[-1]))
    assert stats['cache'][1]>=1 and stats['cache'][2]>=1,('line publish retired/kept',stats['cache'])
    assert stats['mprotect'][1]>=1 and stats['mprotect'][2]>=1,('mprotect retired/kept',stats['mprotect'])
    print(f'{name}: rewritten code retired, unchanged code kept {stats["cache"][2]+stats["mprotect"][2]} times')
for budget in (1,64):
    log=(pathlib.Path(sys.argv[1])/f'{budget}.log').read_text()
    assert 'jit_cache_pressure: ALL PASS' in log,log[-3000:]
    rows=re.findall(r'code-cache: used=(\d+) capacity=(\d+) limit=(\d+) blocks=(\d+) growths=(\d+) overflows=(\d+) budget_fallbacks=(\d+) mt=(\d+)',log)
    assert rows,'No cache diagnostics'
    used,capacity,limit,blocks,growths,overflows,fallbacks,mt=map(int,rows[-1])
    assert 0<used<=capacity<=limit==budget*1024*1024
    assert mt==1,'Test did not enter shared multithread mode'
    if budget==1:
        assert overflows==1 and fallbacks>0,(overflows,fallbacks)
    else:
        assert growths>0 and overflows==0 and fallbacks==0,(growths,overflows,fallbacks)
    translated=int(re.findall(r'(\d+) blocks translated',log)[-1])
    assert translated<20000,'Cached fallback targets are being recompiled'
    print(f'cache budget {budget} MiB: correct results, {translated} translations, {overflows} overflows')
print('jit_cache: ALL PASS')
PY
