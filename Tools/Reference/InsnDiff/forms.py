#!/usr/bin/env python3
"""Pick up to N encodings per normalized instruction form from objdump output.

Usage: objdump -d lib... | forms.py N > picked.raw  (hex<TAB>mnem<TAB>ops)
"""
import re
import sys

limit = int(sys.argv[1])
line_re = re.compile(r"^\s+[0-9a-f]+:\t([0-9a-f]{8}) \t([^\t]+)\t?([^\t]*)")
counts = {}
seen = set()
for line in sys.stdin:
    m = line_re.match(line)
    if not m:
        continue
    word, mnem, ops = m.group(1), m.group(2).strip(), m.group(3).split("//")[0].split("<")[0].strip()
    if word in seen:
        continue
    seen.add(word)
    form = re.sub(r"\b[wx]([0-9]+|zr)\b", "R", ops)
    form = re.sub(r"\b[bhsdqv][0-9]+\b", "V", form)
    form = re.sub(r"#-?(0x[0-9a-f]+|[0-9.e+-]+)", "#I", form)
    form = re.sub(r"\b[0-9a-f]{4,}\b", "T", form)
    key = mnem + " " + form
    if counts.get(key, 0) >= limit:
        continue
    counts[key] = counts.get(key, 0) + 1
    print(f"{word}\t{mnem}\t{ops}")
print(f"{len(counts)} forms", file=sys.stderr)
