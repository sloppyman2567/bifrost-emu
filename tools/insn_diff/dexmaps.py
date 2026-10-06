#!/usr/bin/env python3
"""Find dex/jar file mappings that are unmapped and whose range is later reused."""
import re
import sys

open_re = re.compile(r'^\[syscall t(\d+)\] 56 openat .*path="([^"]+)"')
ret_re = re.compile(r'^  => ret=(-?\d+)')
mmap_re = re.compile(r'^\[mmap\(pc=0x[0-9a-f]+ addr=0x([0-9a-f]+), len=(\d+), prot=(\d+), flags=0x([0-9a-f]+), fd=(-?\d+), off=(\d+)\) → 0x([0-9a-f]+)\]')
munmap_re = re.compile(r'^\[munmap\(0x([0-9a-f]+), (\d+)\)\]')
close_re = re.compile(r'^\[syscall t\d+\] 57 close a0=0x([0-9a-f]+)')
fail_re = re.compile(r'Inet6AddressImpl; failed')

fds = {}
live = []      # [lo, hi, path, line]
dead = []      # unmapped dex ranges: [lo, hi, path, mapline, unmapline]
pending = None
for n, line in enumerate(open(sys.argv[1], errors="replace"), 1):
    if fail_re.search(line):
        print(f"--- failure at line {n}")
        break
    m = open_re.match(line)
    if m:
        pending = m.group(2)
        continue
    if pending is not None:
        r = ret_re.match(line)
        if r:
            if int(r.group(1)) >= 0:
                fds[int(r.group(1))] = pending
            pending = None
            continue
    m = close_re.match(line)
    if m:
        fds.pop(int(m.group(1), 16), None)
        continue
    m = mmap_re.match(line)
    if m:
        length, fd, res = int(m.group(2)), int(m.group(5)), int(m.group(7), 16)
        lo, hi = res, res + ((length + 4095) & ~4095)
        path = fds.get(fd, "<anon>" if fd < 0 else f"<fd{fd}>")
        for d in dead:
            if lo < d[1] and d[0] < hi:
                print(f"REUSE line {n}: mmap {lo:#x}-{hi:#x} {path} overlaps freed "
                      f"{d[0]:#x}-{d[1]:#x} {d[2]} (mapped {d[3]}, unmapped {d[4]})")
        if re.search(r'\.(jar|dex|apk|vdex|odex|oat|art)$', path):
            live.append([lo, hi, path, n])
        continue
    m = munmap_re.match(line)
    if m:
        lo = int(m.group(1), 16)
        hi = lo + ((int(m.group(2)) + 4095) & ~4095)
        for l in live[:]:
            if lo < l[1] and l[0] < hi:
                dead.append([max(lo, l[0]), min(hi, l[1]), l[2], l[3], n])
                live.remove(l)
print(f"live dex-ish mappings: {len(live)}, unmapped: {len(dead)}")
for d in dead[:40]:
    print(f"  freed {d[0]:#x}-{d[1]:#x} {d[2]} mapped@{d[3]} unmapped@{d[4]}")
