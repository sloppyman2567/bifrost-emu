#!/usr/bin/env python3
"""vkxmlcheck.py — mechanically audit every VK row in thunk_dp.txt against
the Khronos vk.xml registry.

For each VK-family row we compare:
  1. ARITY   — hand-written ARGS length vs vk.xml parameter count
               (trailing-'i' elision is allowed by spec convention).
  2. POINTERS— every position where vk.xml says "pointer" (kinds P/S/D)
               must carry 'p'/'z' in the hand ARGS, and vice versa.
               Mismatches here are exactly the class of bug that bit us
               before (vkCmdUpdateBuffer/vkCmdCopyBuffer had an extra 'i',
               shifting the pointer mask so pData reached host memcpy as a
               raw guest pointer).
  3. DEEP    — positions where vk.xml says dynamically-sized/nested ('D')
               but the row claims plain translate ('p'): these need a
               marshal arm or they will crash the host driver on nested
               guest pointers. Reported as WARN.

Exit code 0 = clean, 2 = errors found.

Usage: python3 tools/opgen/vkxmlcheck.py [spec] [vkxml]
"""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vkxml import parse_registry, arg_kinds

SPEC_DEFAULT = 'tools/opgen/thunk_dp.txt'
XML_DEFAULT = 'tools/vulkan-headers/registry/vk.xml'

# Policies whose dispatch arms read registers directly and own their own
# marshalling — the generic ARGS pointer mask is not applied for them.
OWN_MARSHALLING_POLICIES = {'VK_GET_PROC', 'PRESENT', 'SUBMIT', 'VK_CMD_DEEP'}

# (command, param_index) positions where a dedicated arm deliberately
# deviates from vk.xml (with the reason).
DELIBERATE = {
    # deep-marshal arms force pAllocator = nullptr at the host call
    ('vkCreateGraphicsPipelines', 4): 'arm forces NULL allocator',
    ('vkCreateComputePipelines', 4): 'arm forces NULL allocator',
    # intercepted by name in display_thunk dispatch: pfnUserCallback is a
    # GUEST function pointer and must never be forwarded to the host.
    'vkCreateDebugUtilsMessengerEXT': 'intercepted by name (guest callback)',
    'vkDestroyDebugUtilsMessengerEXT': 'intercepted by name (no-op)',
}


def main():
    spec_path = sys.argv[1] if len(sys.argv) > 1 else SPEC_DEFAULT
    xml_path = sys.argv[2] if len(sys.argv) > 2 else XML_DEFAULT
    reg = parse_registry(xml_path)

    errors, warns, checked, skipped = [], [], 0, []
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
            if lib != 'VK':
                continue
            if name in DELIBERATE and not isinstance(DELIBERATE[name], tuple):
                skipped.append((name, lineno, DELIBERATE[name]))
                continue
            if policy in OWN_MARSHALLING_POLICIES:
                skipped.append((name, lineno,
                                f'policy {policy} owns its marshalling'))
                continue
            hand_args = ''.join(toks[2:-3])
            if hand_args == '-':
                hand_args = ''
            cmd = reg['commands'].get(name)
            if cmd is None:
                skipped.append((name, lineno, 'not in vk.xml'))
                continue
            xml_kinds = arg_kinds(cmd, reg)
            checked += 1

            # 1. arity (allow trailing-i elision in hand rows)
            n_hand_effective = len(hand_args.rstrip('i')) \
                if hand_args else 0
            # count of non-'i' kinds in xml:
            n_xml_ptrish = sum(1 for k in xml_kinds if k in 'PSD')
            # effective hand length must cover at least the last ptrish xml pos
            last_ptrish = max(
                [i for i, k in enumerate(xml_kinds) if k in 'PSD'],
                default=-1)
            if len(hand_args) < last_ptrish + 1:
                errors.append(
                    f"{spec_path}:{lineno}: {name}: ARITY — hand ARGS "
                    f"'{hand_args}' ({len(hand_args)}) shorter than needed "
                    f"for vk.xml params ({len(xml_kinds)}, last pointer at "
                    f"{last_ptrish})")
                continue
            if n_hand_effective and len(xml_kinds) < len(hand_args.rstrip('i')):
                errors.append(
                    f"{spec_path}:{lineno}: {name}: EXTRA ARGS — hand ARGS "
                    f"'{hand_args}' longer than vk.xml param list "
                    f"({len(xml_kinds)} params)")
                continue

            # 2/3. pointer-position agreement over overlapping range
            for i, k in enumerate(xml_kinds):
                if i >= len(hand_args):
                    break
                if (name, i) in DELIBERATE:
                    continue   # deliberate deviation, see DELIBERATE docs
                h = hand_args[i]
                ptrish = k in 'PSD'
                hand_ptr = h in 'pz'
                if ptrish and not hand_ptr:
                    errors.append(
                        f"{spec_path}:{lineno}: {name}: param {i} "
                        f"({cmd.params[i].name}, {cmd.params[i].type}) is a "
                        f"POINTER in vk.xml but '{h}' in hand ARGS — raw "
                        f"guest address reaches the host driver")
                elif hand_ptr and not ptrish:
                    errors.append(
                        f"{spec_path}:{lineno}: {name}: param {i} translated "
                        f"as pointer but vk.xml says scalar/enum "
                        f"({cmd.params[i].type}) — mask shifted, later args "
                        f"misrouted")
                elif k == 'D' and h == 'p' and \
                        policy in ('-', 'VULKAN'):
                    warns.append(
                        f"{spec_path}:{lineno}: {name}: param {i} "
                        f"({cmd.params[i].name}) is dynamically-sized/"
                        f"nested in vk.xml but dispatched as plain 'p' — "
                        f"needs a marshal arm")

    print(f"checked {checked} VK rows against vk.xml")
    for s in skipped:
        print(f"  SKIP {s[0]} (line {s[1]}): {s[2]}")
    for w in warns:
        print("WARN " + w)
    for e in errors:
        print("ERROR " + e)
    print(f"errors={len(errors)} warnings={len(warns)} skipped={len(skipped)}")
    sys.exit(2 if errors else 0)


if __name__ == '__main__':
    main()
