#!/usr/bin/env bash
# Bounded pressure and growth checks; three guest threads validate results.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
emu=${BIFROST_EMU:-$repo/bifrost-emu}
fixture="$repo/ctest/jit_cache_pressure.elf"
if [[ "$emu" != /* ]]; then emu="$PWD/$emu"; fi
if [[ ! -f "$fixture" ]]; then echo 'Missing jit_cache_pressure.elf; build with make cross' >&2; exit 1; fi
logs=$(mktemp -d /tmp/bifrost-cache-test.XXXXXX)
trap 'rm -rf -- "$logs"' EXIT
for budget in 1 64; do
    env -u BIFROST_ROOT BIFROST_JIT_CACHE_MB="$budget" BIFROST_JIT_CACHE_INITIAL_MB=1 \
        timeout -s KILL 30 "$emu" --verbose "$fixture" > "$logs/$budget.log" 2>&1
done
python3 - "$logs" <<'PY'
import pathlib,re,sys
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
