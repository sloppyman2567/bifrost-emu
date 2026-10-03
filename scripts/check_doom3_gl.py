#!/usr/bin/env python3
"""Audit dhewm3 1.5.5's complete GL loader against Bifrost's thunk spec.

Use --source with an unpacked matching upstream source tree. --emit-probe
writes a guest C program that checks every lookup in one run; it does not
invoke the GL functions or certify their implementation semantics.
"""
import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools/opgen'))
from glxmlcheck import parse_glxml


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--emit-probe', type=Path)
    args = parser.parse_args()
    renderer = args.source / 'neo/renderer'
    required = re.findall(r'^QGLPROC\((gl\w+),', (renderer / 'qgl_proc.h').read_text(), re.M)
    extensions = sorted(set(re.findall(r'GLimp_ExtensionPointer\(\s*"(gl\w+)"',
                                      (renderer / 'RenderSystem_init.cpp').read_text())))
    specs = {}
    for line in (REPO / 'tools/opgen/thunk_dp.txt').read_text().splitlines():
        fields = line.split('#', 1)[0].split()
        if len(fields) < 6 or fields[1] not in ('GL', 'GLES'):
            continue
        if fields[-1] == 'SYNC':
            fields.pop()
        shape = ''.join(fields[2:-3])
        # GET_PROC scans all graphic libraries; prefer the desktop GL row
        # where both families register the same name.
        if fields[1] == 'GL' or fields[0] not in specs:
            specs[fields[0]] = ('' if shape == '-' else shape, fields[-2])

    def resolve(name):
        if name in specs:
            return name
        core = name[:-3] if name.endswith('ARB') else None
        return core if core in specs else None

    registry = parse_glxml(str(REPO / 'tools/gl-registry/gl.xml'))
    mismatches = []
    for name in sorted(set(required + extensions)):
        resolved = resolve(name)
        if not resolved or resolved not in registry:
            continue
        shape, policy = specs[resolved]
        if policy not in ('-', 'GL_TYPED'):  # Other dedicated dispatch arms own their ABI.
            continue
        errors = []
        for index, param in enumerate(registry[resolved]):
            if param['ptr']:
                continue
            expected = ('f' if param['type'] in ('GLfloat', 'GLclampf') else
                        'd' if param['type'] in ('GLdouble', 'GLclampd') else 'i')
            actual = shape[index] if index < len(shape) else 'i'
            if expected != actual:
                errors.append({'argument': index, 'actual': actual, 'expected': expected})
        if errors:
            mismatches.append({'name': name, 'arguments': errors})
    result = {
        'required_count': len(required),
        'required_missing': [name for name in required if not resolve(name)],
        'extension_count': len(extensions),
        'extension_missing': [name for name in extensions if not resolve(name)],
        'scalar_abi_mismatches': mismatches,
        'scope': 'Name coverage and scalar argument metadata, not rendering correctness or full pointer sizing.',
    }
    print(json.dumps(result, indent=2))
    if args.emit_probe:
        template = (REPO / 'ctest_real/test_sdl_surface_format.c').read_text().split('static const uint32_t icon_pixels')[0]
        names = [(name, 'required') for name in required]
        names += [(name, 'extension') for name in extensions]
        rows = ',\n'.join('{%s,%s}' % (json.dumps(name), json.dumps(kind)) for name, kind in names)
        program = template + '''
struct Request { const char *name, *kind; };
static const struct Request requests[] = {
''' + rows + '''
};
int main(void) {
    CHECK(thunk_dlopen("libGL.so.1"));
    uint64_t sdl=thunk_dlopen("libSDL2.so"); CHECK(sdl);
    void *(*get_proc)(const char*)=sym(sdl,"SDL_GL_GetProcAddress");
    unsigned required_missing=0, extension_missing=0;
    for(unsigned i=0;i<sizeof(requests)/sizeof(requests[0]);i++) {
        if(!get_proc(requests[i].name)) {
            printf("MISSING %s %s\\n",requests[i].kind,requests[i].name);
            if(!strcmp(requests[i].kind,"required")) required_missing++;
            else extension_missing++;
        }
    }
    printf("missing required=%u extension=%u\\n",required_missing,extension_missing);
    return required_missing ? 1 : 0;
}
'''
        args.emit_probe.write_text(program)


if __name__ == '__main__':
    main()
