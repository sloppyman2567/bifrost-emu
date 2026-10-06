#!/usr/bin/env python3
"""Report MADV_DONTNEED calls that hit file-backed or MAP_SHARED mappings."""
import re
import sys

open_re = re.compile(r'^\[syscall t\d+\] 56 openat .*path="([^"]+)"')
ret_re = re.compile(r'^  => ret=(-?\d+)')
mmap_re = re.compile(r'^\[mmap\(pc=0x[0-9a-f]+ addr=0x[0-9a-f]+, len=(\d+), prot=(\d+), flags=0x([0-9a-f]+), fd=(-?\d+), off=(\d+)\) → 0x([0-9a-f]+)\]')
munmap_re = re.compile(r'^\[munmap\(0x([0-9a-f]+), (\d+)\)\]')
madv_re = re.compile(r'^\[syscall t\d+\] 233 \S+ a0=0x([0-9a-f]+) a1=0x([0-9a-f]+) a2=0x4 ')
fds, maps, pending, hits = {}, [], None, 0
for n, line in enumerate(open(sys.argv[1], errors="replace"), 1):
    if "Inet6AddressImpl; failed" in line:
        print(f"--- failure at line {n}")
        break
    m = open_re.match(line)
    if m:
        pending = m.group(1); continue
    if pending is not None and (r := ret_re.match(line)):
        if int(r.group(1)) >= 0:
            fds[int(r.group(1))] = pending
        pending = None; continue
    if (m := mmap_re.match(line)):
        length, flags, fd, res = int(m.group(1)), int(m.group(3), 16), int(m.group(4)), int(m.group(6), 16)
        lo, hi = res, res + ((length + 4095) & ~4095)
        maps = [x for x in maps if not (x[0] < hi and lo < x[1])]
        if fd >= 0 and fd < (1 << 31) or flags & 1:
            kind = ("shared " if flags & 1 else "private ") + fds.get(fd, f"fd{fd}" if fd < (1 << 31) else "anon")
            maps.append((lo, hi, kind, n))
        continue
    if (m := munmap_re.match(line)):
        lo = int(m.group(1), 16); hi = lo + ((int(m.group(2)) + 4095) & ~4095)
        maps = [x for x in maps if not (x[0] < hi and lo < x[1])]
        continue
    if (m := madv_re.match(line)):
        lo = int(m.group(1), 16); hi = lo + int(m.group(2), 16)
        for x in maps:
            if x[0] < hi and lo < x[1]:
                hits += 1
                if hits <= 30:
                    print(f"line {n}: DONTNEED {lo:#x}+{hi-lo:#x} hits {x[2]} {x[0]:#x}-{x[1]:#x} (mapped line {x[3]})")
print(f"{hits} DONTNEED hits on file-backed/shared mappings")
