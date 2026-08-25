#!/usr/bin/env python3
"""eglcheck.py — mechanically audit every EGL row in thunk_dp.txt against
the Khronos egl.xml registry (vendored at tools/gl-registry/egl.xml).

Same contract as glxmlcheck: arity (trailing-'i' elision allowed) +
pointer-position agreement ('*' in the egl.xml param text must be a
'p'/'z' in the hand ARGS and vice versa).

Exit code 0 = clean, 2 = errors found.
"""
import sys
import os
import xml.etree.ElementTree as ET

SPEC_DEFAULT = 'tools/opgen/thunk_dp.txt'
XML_DEFAULT = 'tools/gl-registry/egl.xml'

# Not in egl.xml: libwayland-egl helpers registered under the same family.
DELIBERATE = {
    'wl_egl_window_create': 'libwayland-egl helper',
    'wl_egl_window_destroy': 'libwayland-egl helper',
    'wl_egl_window_get_attached_size': 'libwayland-egl helper',
    'wl_egl_window_get_buffer_scale': 'libwayland-egl helper',
    'wl_egl_window_resize': 'libwayland-egl helper',
    'wl_egl_window_set_buffer_scale': 'libwayland-egl helper',
    'wl_egl_window_set_buffer_transform': 'libwayland-egl helper',
}


def parse_eglxml(path):
    root = ET.parse(path).getroot()
    cmds = {}
    for c in root.iter('command'):
        pr = c.find('proto')
        nm = pr.find('name') if pr is not None else None
        if nm is None or not nm.text:
            continue
        ps = []
        for p in c.findall('param'):
            txt = ''.join(p.itertext())
            n = p.find('name')
            t = p.find('ptype')
            ps.append({
                'name': n.text if n is not None else '?',
                'type': t.text if t is not None else '',
                'ptr': '*' in txt,
            })
        cmds[nm.text] = ps
    return cmds


def main():
    spec_path = sys.argv[1] if len(sys.argv) > 1 else SPEC_DEFAULT
    xml_path = sys.argv[2] if len(sys.argv) > 2 else XML_DEFAULT
    reg = parse_eglxml(xml_path)

    errors, checked, skipped = [], 0, []
    with open(spec_path) as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.split('#', 1)[0].strip()
            if not line:
                continue
            toks = line.split()
            name, lib = toks[0], toks[1]
            ret, policy, size = toks[-3], toks[-2], toks[-1]
            if lib != 'EGL':
                continue
            if name in DELIBERATE:
                skipped.append((name, lineno, DELIBERATE[name]))
                continue
            hand_args = ''.join(toks[2:-3])
            if hand_args == '-':
                hand_args = ''
            cmd = reg.get(name)
            if cmd is None:
                skipped.append((name, lineno, 'not in egl.xml'))
                continue
            checked += 1

            last_ptr = max([i for i, p in enumerate(cmd) if p['ptr']],
                           default=-1)
            n_eff = len(hand_args.rstrip('i')) if hand_args else 0
            if len(hand_args) < last_ptr + 1:
                errors.append(
                    f"{spec_path}:{lineno}: {name}: ARITY — hand ARGS "
                    f"'{hand_args}' ({len(hand_args)}) shorter than needed "
                    f"for egl.xml params ({len(cmd)}, last pointer at "
                    f"{last_ptr})")
                continue
            if n_eff and len(cmd) < len(hand_args.rstrip('i')):
                errors.append(
                    f"{spec_path}:{lineno}: {name}: EXTRA ARGS — hand ARGS "
                    f"'{hand_args}' longer than egl.xml param list "
                    f"({len(cmd)} params)")
                continue
            for i, xp in enumerate(cmd):
                if i >= len(hand_args):
                    break
                h = hand_args[i]
                hand_ptr = h in 'pz'
                if xp['ptr'] and not hand_ptr:
                    errors.append(
                        f"{spec_path}:{lineno}: {name}: param {i} "
                        f"({xp['name']}, {xp['type']}) is a POINTER in "
                        f"egl.xml but '{h}' in hand ARGS")
                elif hand_ptr and not xp['ptr']:
                    errors.append(
                        f"{spec_path}:{lineno}: {name}: param {i} "
                        f"({xp['name']}) translated as pointer but egl.xml "
                        f"says scalar ({xp['type']})")

    print(f"checked {checked} EGL rows against egl.xml")
    for s in skipped:
        print(f"  SKIP {s[0]} (line {s[1]}): {s[2]}")
    for e in errors:
        print("ERROR " + e)
    print(f"errors={len(errors)} checked={checked} skipped={len(skipped)}")
    sys.exit(2 if errors else 0)


if __name__ == '__main__':
    main()
