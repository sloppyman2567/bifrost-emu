#!/usr/bin/env python3
"""thunkhdrcheck.py — audit GLFW/SDL rows in thunk_dp.txt against system headers.

Catches the bug class that bit glfwGetVersion / glfwGetWindowSize /
SDL_CreateWindowAndRenderer: hand-written ARGS arity or pointer positions
drifting from the real C prototype, so the generic dispatch path passes a
raw guest address (or integer bit pattern) to the host.

For each GLFW/SDL-family row we compare:
  1. ARITY   — hand ARGS length vs header parameter count
               ('void' / empty == 0; trailing-'i' elision allowed).
  2. POINTERS— every header pointer ('*' in the param text) must carry
               'p'/'z' in hand ARGS and vice versa.
  3. DOUBLES — header double/float params must not be plain 'i'
               (glfwSetCursorPos-style silent garbage).

Rows whose dispatch arm owns its marshalling (callbacks, GET_PROC,
GLFW_CREATE/POLL, MAP/UNMAP, etc.) are skipped — the generic pointer
mask is not applied for them.

Exit code 0 = clean, 2 = errors found.

Usage: python3 tools/opgen/thunkhdrcheck.py [spec]
"""
import sys
import os
import re
import glob

SPEC_DEFAULT = 'tools/opgen/thunk_dp.txt'
GLFW_HDR = '/usr/include/GLFW/glfw3.h'
SDL_DIR = '/usr/include/SDL2'

# Policies whose dispatch arms read guest registers directly and own their
# marshalling — the generic ARGS pointer mask is not applied for them.
OWN_MARSHALLING_POLICIES = {
    'GET_PROC', 'GET_STRING', 'SHADER_SOURCE', 'TF_VARYINGS',
    'MAP_BUFFER', 'UNMAP_BUFFER', 'FLUSH_BUFFER',
    'DELETE_BUFFERS', 'DELETE_TRACK', 'ELIDE_BIND',
    'TRACK_TEX', 'UNTRACK_TEX', 'VA_PTR', 'EL_PTR', 'EL_PTR_ARRAY',
    'INDIRECT_PTR', 'QUERY', 'PRESENT',
    'CURSOR_CB', 'KEY_CB', 'MOUSE_CB', 'FRAMEBUFFER_CB', 'WINDOW_SIZE_CB',
    'FOCUS_CB', 'ERROR_CB', 'GLFW_POLL', 'GLFW_CREATE', 'GL_DEBUG_CB',
    'SDL_FREE', 'SDL_OPEN_AUDIO', 'SDL_ALLOC', 'SDL_EVENT_FILTER',
    'SDLVK_EXT', 'JOY_GUID', 'JOY_GUID_STR',
    # SDL_STUB0 owns its marshalling: the dispatch arm returns 0 without
    # calling the host (SDL_SetWindowIcon's guest pixels can never reach
    # host; SDL_qsort's guest comparator can never run on host), except
    # SDL_GetWindowWMInfo which does its own verbatim-handle + OUT-buffer
    # translation. The generic ARGS mask is never applied for these rows.
    'SDL_STUB0',
    'MIX_VERSION', 'MIX_OPEN_AUDIO', 'THREAD_CREATE', 'THREAD_WAIT',
    'THREAD_DETACH', 'PROXY',
}

# Opaque handle types are passed VERBATIM ('i') by design: the host created
# the object and the guest passes the cookie back (GLFWwindow*,
# SDL_Window*, SDL_Surface*, ...). Only OUT/data pointers must be 'p'/'z'.
# NOTE: SDL_Rect / SDL_DisplayMode / raw buffers are GUEST-allocated data
# (they need translation) — deliberately NOT in this set, so a verbatim
# 'i' on them stays an ERROR.
OPAQUE_TYPES = {
    'GLFWwindow', 'GLFWmonitor', 'GLFWcursor',
    'SDL_Window', 'SDL_Renderer', 'SDL_Texture', 'SDL_Surface',
    'SDL_PixelFormat', 'SDL_RWops',
    'SDL_Joystick', 'SDL_GameController', 'SDL_Haptic',
    'SDL_GLContext', 'SDL_Thread', 'SDL_mutex', 'SDL_sem', 'SDL_cond',
    'SDL_Cursor',
}


def parse_prototypes():
    """name -> (params, variadic); param = {text, ptr, opaque, is_double}."""
    protos = {}
    files = []
    if os.path.isfile(GLFW_HDR):
        files.append(GLFW_HDR)
    if os.path.isdir(SDL_DIR):
        files += glob.glob(os.path.join(SDL_DIR, '*.h'))
    # Anchor on real declarations only (extern DECLSPEC ... SDLCALL name(
    # for SDL, GLFWAPI ... name( for GLFW) — a bare name( match also hits
    # doc comments ("disable it with SDL_EventState()"), macros, and
    # string literals.
    pat = re.compile(
        r'(?:extern\s+DECLSPEC\s+.+?[\s\*]SDLCALL|GLFWAPI\s+.+?)\s+'
        r'([A-Za-z_][A-Za-z0-9_]*)\s*\(([^;{}]*)\)\s*;', re.S)
    for path in files:
        try:
            with open(path, errors='ignore') as fh:
                text = fh.read()
        except OSError:
            continue
        for m in pat.finditer(text):
            name, argstr = m.group(1), m.group(2).strip()
            if name in protos:
                continue
            argstr = re.sub(r'\s+', ' ', argstr).strip()
            if argstr in ('', 'void'):
                protos[name] = ([], False)
                continue
            variadic = '...' in argstr
            params = []
            for p in argstr.split(','):
                p = p.strip()
                if not p or p == '...':
                    continue
                low = p.lower()
                base = re.sub(r'[\s\*\[\]\(\)]+', ' ', p).strip().split()
                opaque = any(t in OPAQUE_TYPES for t in base)
                params.append({
                    'text': p,
                    'ptr': '*' in p or '[' in p,
                    'out_ptr': p.count('*') >= 2,
                    'is_cb': 'callback' in low or '(*' in p,
                    'opaque': opaque,
                    'is_double': 'double' in low,
                    'is_float': re.search(r'(?<![a-z_])float(?![a-z_])', low) is not None,
                })
            protos[name] = (params, variadic)
    return protos


def main():
    spec_path = sys.argv[1] if len(sys.argv) > 1 else SPEC_DEFAULT
    reg = parse_prototypes()
    if not reg:
        have_hdrs = os.path.isfile(GLFW_HDR) or os.path.isdir(SDL_DIR)
        if have_hdrs:
            print("thunkhdrcheck: headers present but 0 prototypes parsed "
                  "(regex regression?), failing")
            return 2
        print("thunkhdrcheck: no system headers found, skipping")
        return 0

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
            if lib not in ('GLFW', 'SDL'):
                continue
            if policy in OWN_MARSHALLING_POLICIES:
                skipped.append((name, lineno,
                                'policy %s owns its marshalling' % policy))
                continue
            hand_args = ''.join(toks[2:-3])
            if hand_args == '-':
                hand_args = ''
            hit = reg.get(name)
            if hit is None:
                skipped.append((name, lineno, 'not in system headers'))
                continue
            proto, variadic = hit
            if variadic:
                skipped.append((name, lineno, 'variadic — cannot check mechanically'))
                continue
            checked += 1

            # 1. arity (allow trailing-i elision in hand rows)
            n_hand_effective = len(hand_args.rstrip('i')) if hand_args else 0
            last_sig = -1
            for i, p in enumerate(proto):
                if p['ptr'] or p['is_double'] or p['is_float']:
                    last_sig = i
            if len(hand_args) < last_sig + 1:
                errors.append(
                    '%s:%d: %s: ARITY — hand ARGS %r (%d) shorter than '
                    'needed for header params (%d, last significant at %d)'
                    % (spec_path, lineno, name, hand_args,
                       len(hand_args), len(proto), last_sig))
                continue
            if n_hand_effective and len(proto) < len(hand_args.rstrip('i')):
                errors.append(
                    '%s:%d: %s: EXTRA ARGS — hand ARGS %r longer than '
                    'header param list (%d params)'
                    % (spec_path, lineno, name, hand_args, len(proto)))
                continue

            # 2/3. pointer + float agreement over overlapping range.
            # Opaque handles (host-created cookies) are verbatim 'i' by
            # design — but data-vs-cookie can't be decided mechanically for
            # Surface/RWops/Rect rows, so those go to WARN (design-review
            # backlog), keeping ERROR for certain crash-class mismatches.
            for i, hp in enumerate(proto):
                if i >= len(hand_args):
                    break
                h = hand_args[i]
                hand_ptr = h in 'pz'
                if hp['ptr'] and hp['opaque']:
                    if hp['out_ptr']:
                        continue  # OUT-pointer-to-opaque: correct 'p'
                    if hand_ptr:
                        warns.append(
                            '%s:%d: %s: param %d (%s) is an opaque handle '
                            'translated as pointer — review whether it '
                            'should stay verbatim %r'
                            % (spec_path, lineno, name, i,
                               hp['text'], h))
                    else:
                        warns.append(
                            '%s:%d: %s: param %d (%s) is an opaque-handle '
                            'pointer passed verbatim — confirm cookie '
                            'round-trip by design'
                            % (spec_path, lineno, name, i, hp['text']))
                    continue
                if hp['is_cb'] and hand_ptr:
                    errors.append(
                        '%s:%d: %s: param %d (%s) is a guest CALLBACK — '
                        'must never reach the host; needs an intercept/'
                        'stub arm, not a mask tweak'
                        % (spec_path, lineno, name, i, hp['text']))
                    continue
                if hp['ptr'] and not hand_ptr:
                    errors.append(
                        '%s:%d: %s: param %d (%s) is a POINTER in the '
                        'header but %r in hand ARGS — raw guest address '
                        'reaches the host' % (spec_path, lineno, name, i,
                                              hp['text'], h))
                elif hand_ptr and not hp['ptr']:
                    errors.append(
                        '%s:%d: %s: param %d (%s) translated as pointer '
                        'but header says scalar — mask shifted, later args '
                        'misrouted' % (spec_path, lineno, name, i,
                                       hp['text']))
                elif hp['is_double'] and h == 'f':
                    errors.append(
                        '%s:%d: %s: param %d (%s) is double in the header '
                        'but float %r in hand ARGS — width mismatch'
                        % (spec_path, lineno, name, i, hp['text'], h))
                elif hp['is_float'] and h == 'd':
                    errors.append(
                        '%s:%d: %s: param %d (%s) is float in the header '
                        'but double %r in hand ARGS — width mismatch'
                        % (spec_path, lineno, name, i, hp['text'], h))
                elif (hp['is_double'] or hp['is_float']) and h == 'i':
                    errors.append(
                        '%s:%d: %s: param %d (%s) is floating-point in '
                        'the header but %r in hand ARGS — int bits in GPRs '
                        'instead of float in XMM'
                        % (spec_path, lineno, name, i, hp['text'], h))

    print('checked %d GLFW/SDL rows against system headers' % checked)
    for s in skipped:
        print('  SKIP %s (line %d): %s' % (s[0], s[1], s[2]))
    for w in warns:
        print('WARN ' + w)
    for e in errors:
        print('ERROR ' + e)
    print('errors=%d warnings=%d skipped=%d'
          % (len(errors), len(warns), len(skipped)))
    return 2 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
