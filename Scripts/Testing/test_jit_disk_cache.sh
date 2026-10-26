#!/usr/bin/env bash
# Persistent JIT cache (BIFROST_JIT_DISK_CACHE): a run records translations,
# later runs at other ASLR bases reuse them with the same results, verify
# mode finds every reused record equal to a fresh compile, damaged files and
# a small budget degrade to compiling, concurrent writers are merged, a
# codegen switch selects another fingerprint, a new build prunes older ones
# without dropping what it records, and a cache directory that others can
# write to keeps the cache off.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
emu=${BIFROST_EMU:-$repo/bifrost-emu}
fixture="$repo/Tests/Unit/jit_dispatch_cache_growth.elf"
if [[ "$emu" != /* ]]; then emu="$PWD/$emu"; fi
if [[ ! -f "$fixture" ]]; then echo "Missing $(basename "$fixture"); build with make cross" >&2; exit 1; fi
work=$(mktemp -d /tmp/bifrost-disk-cache-test.XXXXXX)
trap 'rm -rf -- "$work"' EXIT

# What the budget limits: file bytes (du would add filesystem-specific directory sizes).
file_bytes() { find "$1" -type f -printf '%s\n' | awk '{s += $1} END {print s + 0}'; }
run() {  # run LOG [ENV...]: the fixture with the cache in $work/cache
    local log=$1; shift
    env -u BIFROST_ROOT -u BIFROST_JIT_CACHE_APP BIFROST_JIT_DISK_CACHE=1 BIFROST_JIT_DISK_CACHE_DIR="$work/cache" \
        BIFROST_JIT_DISK_CACHE_COMPACT_DELAY=0 "$@" timeout -s KILL 60 "$emu" --verbose "$fixture" > "$work/$log.log" 2>&1
}
run record
run reuse
run verify BIFROST_JIT_DISK_CACHE=verify
# Damage: flip bytes inside main.bin's records and cut a segment short.
python3 - "$work/cache" <<'PY'
import pathlib,sys
root=pathlib.Path(sys.argv[1])
main=next(root.glob('*/*/main.bin'),None)
assert main,'compaction wrote no main.bin'
data=bytearray(main.read_bytes())
for at in range(4096,len(data)//2,8192): data[at]^=0x5a
main.write_bytes(data)
for seg in root.glob('*/*/seg-*.bin'):
    seg.write_bytes(seg.read_bytes()[:max(48,seg.stat().st_size//2+3)])
PY
run damaged
rm -rf "$work/cache"
run budget BIFROST_JIT_DISK_CACHE_MB=1
budget_bytes=$(file_bytes "$work/cache")
rm -rf "$work/cache"
run writer1 & run writer2 & wait
run merged
run other_switch BIFROST_NO_FMA3=1
run third_switch BIFROST_NO_AVX2=1
# A fourth build with room for about two of the three directories already
# there: opening prunes the older builds first, so nothing it records is dropped.
one_dir=$(file_bytes "$(ls -d "$work"/cache/*/* | head -1)")
prune_mb=$(( one_dir * 26 / 10 / 1048576 + 1 ))
run pruned BIFROST_NO_SELFLOOP=1 BIFROST_JIT_DISK_CACHE_MB=$prune_mb
fingerprints_after_prune=$(ls -d "$work"/cache/*/* | wc -l)
pruned_bytes=$(file_bytes "$work/cache")
mkdir -m 0775 "$work/shared"
env -u BIFROST_ROOT BIFROST_JIT_DISK_CACHE=1 BIFROST_JIT_DISK_CACHE_DIR="$work/shared" \
    timeout -s KILL 60 "$emu" --verbose "$fixture" > "$work/shared.log" 2>&1
python3 - "$work" "$budget_bytes" "$fingerprints_after_prune" "$pruned_bytes" "$prune_mb" <<'PY'
import pathlib,re,sys
work=pathlib.Path(sys.argv[1])
def stats(name):
    log=(work/f'{name}.log').read_text()
    assert 'jit_dispatch_cache_growth: ALL PASS' in log,(name,log[-3000:])
    line=re.findall(r'\[JIT\] disk cache: (.*)',log)
    assert line,(name,'no disk cache statistics',log[-2000:])
    return {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line[-1])}
record,reuse,verify=stats('record'),stats('reuse'),stats('verify')
assert record['recorded']>1000 and record['hits']==0,record
assert reuse['hits']>0.9*(reuse['hits']+reuse['misses']),reuse
assert verify['verify_mismatches']==0 and verify['hits']>1000,verify
print(f"record {record['recorded']}, reuse {reuse['hits']} hits / {reuse['misses']} misses, verify {verify['hits']} equal")
damaged=stats('damaged')
assert damaged['rejected']+damaged['misses']>0,damaged
print(f"damaged files: {damaged['rejected']} rejected, {damaged['hits']} reused, results correct")
budget=stats('budget')
assert budget['over_budget']>0 and int(sys.argv[2])<=1<<20,(budget,sys.argv[2])
print(f"1 MiB budget: {budget['recorded']} recorded, {budget['over_budget']} dropped, {sys.argv[2]} bytes on disk")
w1,w2,merged=stats('writer1'),stats('writer2'),stats('merged')
assert w1['recorded']>1000 and w2['recorded']>1000 and merged['hits']>0.9*(merged['hits']+merged['misses']),(w1,w2,merged)
print(f"concurrent writers: {w1['recorded']}+{w2['recorded']} recorded, next run {merged['hits']} hits")
other=stats('other_switch')
assert other['hits']==0 and other['recorded']>1000,other
print('codegen switch change: separate fingerprint, no reuse')
pruned=stats('pruned')
assert pruned['over_budget']==0 and pruned['recorded']>1000,pruned
assert int(sys.argv[3])<4 and int(sys.argv[4])<=int(sys.argv[5])<<20,sys.argv[3:]
print(f"new build: older fingerprints pruned to {sys.argv[3]} directories, {pruned['recorded']} recorded, none dropped")
shared=(work/'shared.log').read_text()
assert 'jit_dispatch_cache_growth: ALL PASS' in shared and 'disk cache: off' in shared,shared[-2000:]
assert not list((work/'shared').rglob('*.bin')),'cache written to a group-writable directory'
print('group-writable cache directory: cache off, program unaffected')
print('jit_disk_cache: ALL PASS')
PY
