#!/usr/bin/env python3
"""glxmlcheck.py — mechanically audit every GL/GLES row in thunk_dp.txt
against the Khronos gl.xml registry (vendored at ThirdParty/GLRegistry/gl.xml).

For each GL/GLES-family row we compare:
  1. ARITY   — hand-written ARGS length vs gl.xml parameter count
               (trailing-'i' elision is allowed by spec convention, except
               that rows with more than eight integer/pointer params must
               list them all: the ninth onwards are read from the stack).
  2. POINTERS— every position where gl.xml declares a pointer ('*' in the
               param text) must carry 'p'/'z' in the hand ARGS, and vice
               versa. Mismatches are exactly the class of bug that bit us
               on the Vulkan side (shifted pointer masks sending raw guest
               addresses into host memcpy).
  2b. NESTED — a pointer-to-pointer parameter (`const GLchar *const*names`)
               is not translated by the generic 'p' rule, which only turns the
               array's own address into a host pointer: every entry stays a
               guest address and the host driver dereferences it. A row with
               such a parameter must carry a policy that marshals it
               (SHADER_SOURCE, TF_VARYINGS, STRING_ARRAY, EL_PTR_ARRAY ...);
               a plain '-' policy is an ERROR unless the command is on the
               NESTED_PENDING list below with the reason. (glGetUniformIndices
               had '-' and a WebView ad's call killed Geometry Dash Lite.)
  3. DYN     — pointer positions whose gl.xml <param len=...> marks a
               dynamically-sized array (COMPSIZE(...) or a sibling param)
               but which ride the default 64 KiB bounce. Reported as WARN;
               they are the candidates for 'z'+SIZE promotion.

Exit code 0 = clean, 2 = errors found.

Usage: python3 Tools/CodeGen/glxmlcheck.py [spec] [glxml]
"""
import sys
import os
import xml.etree.ElementTree as ET

SPEC_DEFAULT = 'Tools/CodeGen/thunk_dp.txt'
XML_DEFAULT = 'ThirdParty/GLRegistry/gl.xml'
# gl.xml scalar types passed in SIMD registers; everything else is x-class.
FLOAT_TYPES = {'GLfloat', 'GLdouble', 'GLclampf', 'GLclampd'}

# Policies whose dispatch arms read guest registers directly and own their
# marshalling — the generic ARGS pointer mask is not applied for them.
OWN_MARSHALLING_POLICIES = {
    'GET_STRING',      # exact-prototype string-cache arm
    'DELETE_BUFFERS',  # post-call mapping cleanup
    'SHADER_SOURCE',   # nested string-array re-marshal after generic translate
    'MAP_BUFFER', 'UNMAP_BUFFER', 'FLUSH_BUFFER',   # window-bounce mapping
    'EL_PTR',          # conditional client-array translate at a fixed arg
    'VA_PTR',          # conditional client-array translate at a per-name arg
    'GET_PROC',        # returns host proc addresses
    'TF_VARYINGS',     # nested string-array marshal
    'STRING_ARRAY',    # nested string-array re-marshal after generic translate
}

# Rows intercepted by name before any marshalling (with the reason).
DELIBERATE = {}

WARN_CAP = 40

# Rows with a pointer-to-pointer parameter and the generic policy that are known
# and not yet handled. These are OUTPUT pointers: the driver writes a pointer
# value into guest memory, so the guest can be handed a host address (for
# example the host mapping behind glGetBufferPointerv(GL_BUFFER_MAP_POINTER)).
# Reported as WARN so they stay visible; a new entry here needs a reason.
NESTED_PENDING = {
    'glGetPointerv': 'out void**: returns a host-side pointer value',
    'glGetPointervEXT': 'out void**: returns a host-side pointer value',
    'glGetPointervKHR': 'out void**: returns a host-side pointer value',
    'glGetBufferPointerv': 'out void**: GL_BUFFER_MAP_POINTER is a host mapping',
    'glGetBufferPointervOES': 'out void**: GL_BUFFER_MAP_POINTER is a host mapping',
    'glGetVertexAttribPointerv': 'out void**: returns the translated client array',
    'glGetVertexAttribPointervNV': 'out void**: returns the translated client array',
}


def parse_glxml(path):
    """name -> list of {name, type, ptr, len}."""
    root = ET.parse(path).getroot()
    cmds = {}
    for c in root.iter('command'):
        proto = c.find('proto')
        if proto is None:
            continue
        pname = proto.find('name')
        if pname is None:
            continue
        params = []
        for p in c.findall('param'):
            txt = ''.join(p.itertext())
            n = p.find('name')
            t = p.find('ptype')
            params.append({
                'name': n.text if n is not None else '?',
                'type': t.text if t is not None else '',
                'ptr': '*' in txt,
                'depth': txt.count('*'),
                'len': p.get('len'),
            })
        cmds[pname.text] = params
    return cmds


def main():
    spec_path = sys.argv[1] if len(sys.argv) > 1 else SPEC_DEFAULT
    xml_path = sys.argv[2] if len(sys.argv) > 2 else XML_DEFAULT
    reg = parse_glxml(xml_path)

    errors, warns, pending, checked, skipped = [], [], [], 0, []
    with open(spec_path) as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.split('#', 1)[0].strip()
            if not line:
                continue
            toks = line.split()
            name, lib = toks[0], toks[1]
            if toks[-1] == 'SYNC':
                toks = toks[:-1]
            ret, policy, size = toks[-3], toks[-2], toks[-1]
            if lib not in ('GL', 'GLES'):
                continue
            if name in DELIBERATE:
                skipped.append((name, lineno, DELIBERATE[name]))
                continue
            # 2b. a nested pointer needs a policy that marshals it
            if policy == '-' and name in reg:
                for i, xp in enumerate(reg[name]):
                    if xp['depth'] < 2:
                        continue
                    if name in NESTED_PENDING:
                        pending.append(
                            f"{spec_path}:{lineno}: {name}: param {i} "
                            f"({xp['name']}) is a pointer-to-pointer with no "
                            f"policy — known: {NESTED_PENDING[name]}")
                    else:
                        errors.append(
                            f"{spec_path}:{lineno}: {name}: NESTED — param "
                            f"{i} ({xp['name']}, {xp['type']}) is a "
                            f"pointer-to-pointer in gl.xml but the row has "
                            f"no policy: the generic rule translates only "
                            f"the array's own address, so every entry stays "
                            f"a guest address the host driver dereferences")
            if policy in OWN_MARSHALLING_POLICIES:
                skipped.append((name, lineno,
                                f'policy {policy} owns its marshalling'))
                continue
            hand_args = ''.join(toks[2:-3])
            if hand_args == '-':
                hand_args = ''
            cmd = reg.get(name)
            if cmd is None:
                skipped.append((name, lineno, 'not in gl.xml'))
                continue
            checked += 1

            # 1. arity (allow trailing-i elision in hand rows)
            n_hand_effective = len(hand_args.rstrip('i')) \
                if hand_args else 0
            last_ptr = max(
                [i for i, p in enumerate(cmd) if p['ptr']],
                default=-1)
            if len(hand_args) < last_ptr + 1:
                errors.append(
                    f"{spec_path}:{lineno}: {name}: ARITY — hand ARGS "
                    f"'{hand_args}' ({len(hand_args)}) shorter than needed "
                    f"for gl.xml params ({len(cmd)}, last pointer at "
                    f"{last_ptr} ({cmd[last_ptr]['name'] if last_ptr >= 0 else '?'})")
                continue
            # Trailing-i elision is only harmless in x0-x7: AAPCS64 passes
            # the ninth and later integer/pointer arguments on the stack, so
            # a row that drops them hands stack garbage to the host.
            int_params = sum(1 for p in cmd if p['ptr'] or p['type'] not in FLOAT_TYPES)
            int_hand = sum(1 for c in hand_args if c != 'f')
            if int_params > 8 and int_hand < int_params:
                errors.append(
                    f"{spec_path}:{lineno}: {name}: STACK ARGS — hand ARGS "
                    f"'{hand_args}' lists {int_hand} of gl.xml's {int_params} "
                    f"integer/pointer params; those past the eighth are read "
                    f"from the stack and must not be elided")
                continue
            if n_hand_effective and len(cmd) < len(hand_args.rstrip('i')):
                errors.append(
                    f"{spec_path}:{lineno}: {name}: EXTRA ARGS — hand ARGS "
                    f"'{hand_args}' longer than gl.xml param list "
                    f"({len(cmd)} params)")
                continue

            # 2/3. pointer-position agreement over overlapping range
            for i, xp in enumerate(cmd):
                if i >= len(hand_args):
                    break
                h = hand_args[i]
                hand_ptr = h in 'pz'
                if xp['ptr'] and not hand_ptr:
                    errors.append(
                        f"{spec_path}:{lineno}: {name}: param {i} "
                        f"({xp['name']}, {xp['type']}) is a POINTER in "
                        f"gl.xml but '{h}' in hand ARGS — raw guest "
                        f"address reaches the host driver")
                elif hand_ptr and not xp['ptr']:
                    errors.append(
                        f"{spec_path}:{lineno}: {name}: param {i} "
                        f"({xp['name']}) translated as pointer but gl.xml "
                        f"says scalar ({xp['type']}) — mask shifted, later "
                        f"args misrouted")
                elif xp['ptr'] and xp['len'] and h == 'p' \
                        and size == '-' and policy == '-':
                    warns.append(
                        f"{spec_path}:{lineno}: {name}: param {i} "
                        f"({xp['name']}, len={xp['len']}) is dynamically "
                        f"sized but rides the default bounce")

    print(f"checked {checked} GL/GLES rows against gl.xml")
    for s in skipped:
        print(f"  SKIP {s[0]} (line {s[1]}): {s[2]}")
    for w in pending:  # never subject to the warning cap
        print("PENDING " + w)
    shown = 0
    for w in warns:
        if shown >= WARN_CAP:
            break
        print("WARN " + w)
        shown += 1
    if len(warns) > shown:
        print(f"WARN ... {len(warns) - shown} more dynamic-size warnings "
              f"suppressed")
    for e in errors:
        print("ERROR " + e)
    print(f"errors={len(errors)} warnings={len(warns)} pending-nested={len(pending)} "
          f"skipped={len(skipped)}")
    sys.exit(2 if errors else 0)


if __name__ == '__main__':
    main()
