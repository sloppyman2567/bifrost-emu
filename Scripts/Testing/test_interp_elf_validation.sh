#!/usr/bin/env bash
# A PT_INTERP file is untrusted input: a malformed interpreter must end the launch with an "interpreter:" error,
# never a host fault (the loader once copied PT_LOAD bytes from past the end of the file). Each case pairs a valid
# AArch64 program with one interpreter built by the generator below; a valid interpreter is the control.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
emu=${BIFROST_EMU:-$repo/bifrost-emu}
[[ -x "$emu" ]] || { echo "missing: $emu" >&2; exit 2; }
work=$(mktemp -d "${TMPDIR:-/tmp}/bifrost-interp-elf.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
python3 -I - "$work" <<'PY'
import struct, sys
out = sys.argv[1]
def ehdr(etype, entry, phnum, phoff=64, machine=183, phentsize=56):
    return (b'\x7fELF' + bytes([2, 1, 1, 0]) + b'\0' * 8 +
            struct.pack('<HHIQQQIHHHHHH', etype, machine, 1, entry, phoff, 0, 0, 64, phentsize, phnum, 0, 0, 0))
def ph(t, flags, off, va, fsz, msz, align=0x1000):
    return struct.pack('<IIQQQQQQ', t, flags, off, va, va, fsz, msz, align)
code = struct.pack('<III', 0xD2800000, 0xD2800BA8, 0xD4000001)  # mov x0,#0; mov x8,#93; svc #0
def program(name):
    path = ('%s/%s.interp' % (out, name)).encode() + b'\0'
    off_interp = 64 + 56 * 2
    off_code = (off_interp + len(path) + 15) & ~15
    base = 0x400000
    elf = ehdr(2, base + off_code, 2) + ph(3, 4, off_interp, base + off_interp, len(path), len(path), 1)
    elf += ph(1, 5, 0, base, off_code + len(code), off_code + len(code)) + path
    open('%s/%s.elf' % (out, name), 'wb').write(elf.ljust(off_code, b'\0') + code)
cases = {
    'valid':        ehdr(3, 0, 1) + ph(1, 5, 0, 0, 120, 0x1000),
    'file_range':   ehdr(3, 0, 1) + ph(1, 5, 0x10000000, 0, 0x2000000, 0x2000000),
    'filesz_gt_memsz': ehdr(3, 0, 1) + ph(1, 5, 0, 0, 0x1000, 0x10),
    'vaddr_overflow': ehdr(3, 0, 1) + ph(1, 5, 0, 0xFFFFFFFFFFFFF000, 120, 0x2000),
    'phnum_past_file': ehdr(3, 0, 60000) + ph(1, 5, 0, 0, 120, 0x1000),
    'phoff_past_file': ehdr(3, 0, 1, phoff=0x7FFFFFFF00),
    'not_aarch64':  ehdr(3, 0, 1, machine=62) + ph(1, 5, 0, 0, 120, 0x1000),
}
for name, data in cases.items():
    program(name)
    open('%s/%s.interp' % (out, name), 'wb').write(data)
PY
chmod +x "$work"/*.elf
fail=0
run() { env -u BIFROST_ROOT timeout -s KILL 30 "$emu" "$work/$1.elf" 2>&1 || echo "exit=$?"; }
for name in file_range filesz_gt_memsz vaddr_overflow phnum_past_file phoff_past_file not_aarch64; do
    log=$(run "$name")
    if grep -q 'interpreter: ' <<<"$log" && ! grep -q -E 'host-fault|exit=(124|137|139)' <<<"$log"; then
        echo "PASS $name rejected"
    else
        echo "FAIL $name: expected an 'interpreter:' error"; echo "$log" | head -5; fail=1
    fi
done
log=$(run valid)
if grep -q 'exit=' <<<"$log" || grep -q 'interpreter: ' <<<"$log"; then
    echo "FAIL valid interpreter was not accepted"; echo "$log" | head -5; fail=1
else
    echo "PASS valid interpreter accepted"
fi
(( fail == 0 )) || exit 1
echo "interp_elf_validation: ALL PASS"
