#!/usr/bin/env bash
# The AArch64 syscall names the emulator prints (statistics, crash lines, the Tracy timeline, BIFROST_SYSCALL_TRACE) must be the
# kernel's: every entry of the name table is compared with <asm-generic/unistd.h> on this machine, and a guest that calls
# sendto, recvfrom, sendmsg and recvmsg is traced to see them under their real numbers. Without the header the table check is
# skipped.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
EMU=${BIFROST_EMU:-"$REPO_ROOT/bifrost-emu"}
[[ $EMU = /* ]] || EMU="$PWD/$EMU"
FIXTURE="$REPO_ROOT/Tests/Unit/test_syscall_trace_names.elf"
for need in "$EMU" "$FIXTURE"; do [[ -e "$need" ]] || { echo "missing: $need" >&2; exit 2; }; done
work=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-syscall-names.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
fail=0

"${CXX:-g++}" -O1 -std=c++17 -I"$REPO_ROOT/Source" "$REPO_ROOT/Tests/Unit/syscall_names.cpp" \
    "$REPO_ROOT/Source/System/Syscalls/SyscallNames.cpp" -o "$work/names"
"$work/names" > "$work/table.txt" && echo "PASS the lookup finds, and refuses, the right numbers" || { echo "FAIL the lookup"; fail=1; }

header=""
for candidate in /usr/include/asm-generic/unistd.h /usr/include/linux/asm-generic/unistd.h; do
    [[ -f $candidate ]] && { header=$candidate; break; }
done
if [[ -n $header ]]; then
    # Entries the header does not define (a newer syscall than this machine's headers) cannot be judged; every other one must match.
    result=$(python3 -I - "$header" "$work/table.txt" <<'PY'
import re, sys
names = {}
for line in open(sys.argv[1]):
    m = re.match(r'#define __NR(?:3264)?_(\w+)\s+(\d+)\s*$', line)
    # `syscalls` is the count of syscalls and `arch_specific_syscall` a base number, not syscalls: an older header's count equals
    # the number of the newest syscall in a newer one.
    if m and m.group(1) not in ('syscalls', 'arch_specific_syscall'):
        names.setdefault(int(m.group(2)), set()).add(m.group(1))
wrong, checked, skipped = [], 0, 0
for line in open(sys.argv[2]):
    number, name = line.split()
    number = int(number)
    if number not in names:
        skipped += 1
    elif name not in names[number]:
        wrong.append(f'{number} is "{name}" in the table, {sorted(names[number])} in the header')
    else:
        checked += 1
print(f'{checked} {skipped}')
for w in wrong: print('WRONG ' + w)
PY
)
    read -r checked skipped <<<"$(head -1 <<<"$result")"
    if grep -q '^WRONG' <<<"$result"; then echo "FAIL the table disagrees with $header:"; grep '^WRONG' <<<"$result" | sed 's/^/   /'; fail=1
    elif [[ ${checked:-0} -lt 150 ]]; then echo "FAIL only ${checked:-0} table entries could be checked against $header"; fail=1
    else echo "PASS $checked table entries match $header ($skipped not in that header)"; fi
else
    echo "SKIP no asm-generic/unistd.h on this machine: the table is not checked"
fi

trace=$(BIFROST_SYSCALL_TRACE=1 timeout 60 "$EMU" "$FIXTURE" 2>&1 || true)
grep -q 'test_syscall_trace_names: ALL PASS' <<<"$trace" || { echo "FAIL the fixture did not pass"; fail=1; }
for pair in "206 sendto" "207 recvfrom" "211 sendmsg" "212 recvmsg"; do
    if grep -q -E "\[syscall t[0-9]+\] $pair " <<<"$trace"; then echo "PASS the trace names syscall ${pair% *} ${pair#* }"
    else echo "FAIL the trace has no line for syscall $pair"; fail=1; fi
done
if grep -q -E '\[syscall t[0-9]+\] (23 select|40 sendto|41 sendmsg|43 recvmsg|45 recvfrom) ' <<<"$trace"; then
    echo "FAIL the trace still uses an old wrong number"; fail=1
else echo "PASS no old wrong number appears in the trace"; fi

[[ $fail -eq 0 ]] && echo "syscall_names: ALL PASS"
exit $fail
