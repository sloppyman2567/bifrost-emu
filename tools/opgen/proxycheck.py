#!/usr/bin/env python3
"""proxycheck.py — DisplayProxy coverage audit.

Compares PROXY-policy rows in tools/opgen/thunk_dp.txt against the
hand-written handlers in DisplayThunk::proxy_dispatch_
(src/frost_graphics/display_thunk.cpp) and lists table rows with no
proxy implementation. Those symbols log-and-return-0 at runtime, so
this report is the backlog for proxy work: implement what real apps
call (demand-driven), leave the obscure corners alone.

Usage: python3 tools/opgen/proxycheck.py
Exit 0 always (informational — missing handlers are by design until
a game needs them, unlike opgen drift which fails CI).
"""
import re
import sys

SPEC = "tools/opgen/thunk_dp.txt"
DISPATCH = "src/frost_graphics/display_thunk.cpp"


def main():
    proxy_rows = []
    with open(SPEC) as fh:
        for line in fh:
            parts = line.split("#", 1)[0].split()
            if len(parts) >= 6 and parts[4] == "PROXY":
                proxy_rows.append((parts[0], parts[1]))
    src = open(DISPATCH).read()
    handled = set(re.findall(r'sym_name == "([^"]+)"', src))
    missing = [(n, l) for (n, l) in proxy_rows if n not in handled]
    impl = sorted(set(n for (n, _) in proxy_rows if n in handled))
    print(f"PROXY rows: {len(proxy_rows)}, "
          f"implemented: {len(impl)}, missing: {len(missing)}")
    for name, lib in missing:
        print(f"  - {name} ({lib})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
