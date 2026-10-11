#!/usr/bin/env bash
# Independent arithmetic/SMC oracle plus proof that MT optimizations execute.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
emu=${BIFROST_EMU:-$repo/bifrost-emu}
[[ $emu = /* ]] || emu="$PWD/$emu"
fixture="$repo/ctest/jit_mt_tier2.elf"
[[ -f $fixture && ! $repo/ctest/jit_mt_tier2.c -nt $fixture ]] || {
    echo 'Build a fresh jit_mt_tier2.elf first' >&2; exit 1;
}
logs=$(mktemp -d)
trap 'rm -rf -- "$logs"' EXIT
for mode in default tier2 no-tier2 no-chain no-chain-skip; do
    args=()
    case $mode in
        tier2) args=(BIFROST_TIER2=1);;
        no-tier2) args=(BIFROST_TIER2=0);;
        no-chain) args=(BIFROST_TIER2=1 BIFROST_NO_CHAIN=1);;
        no-chain-skip) args=(BIFROST_TIER2=1 BIFROST_NO_CHAIN_SKIP=1);;
    esac
    env -u BIFROST_ROOT -u BIFROST_JIT_VERIFY -u BIFROST_NO_CHAIN \
        -u BIFROST_TIER2 -u BIFROST_CHAIN_SKIP -u BIFROST_NO_CHAIN_SKIP \
        BIFROST_IR_VALIDATE=1 BIFROST_TIER2_HITS=32 "${args[@]}" \
        timeout -s KILL 45 "$emu" --verbose "$fixture" > "$logs/$mode" 2>&1
    python3 - "$logs/$mode" "$mode" <<'PY'
import pathlib, re, sys
log = pathlib.Path(sys.argv[1]).read_text()
mode = sys.argv[2]
assert 'jit_mt_tier2: ALL PASS' in log, log[-4000:]
assert re.search(r'code-cache: .*mt=1', log), log[-4000:]
rows = re.findall(r'MT publication: edges=(\d+) regions=(\d+)', log)
assert rows, log[-4000:]
edges, regions = map(int, rows[-1])
assert (edges == 0 if mode == 'no-chain' else edges > 0), (mode, edges)
assert (regions == 0 if mode in ('default', 'no-tier2') else regions > 0), (mode, regions)
print(f'{mode}: arithmetic and invalidation oracle passed; MT edges={edges}, regions={regions}')
PY
done
