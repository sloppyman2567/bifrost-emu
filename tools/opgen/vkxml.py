#!/usr/bin/env python3
"""vkxml.py — minimal parser for the Khronos vk.xml registry.

Extracts what the thunk layer needs to derive AAPCS64 argument kinds
(the ARGS column of thunk_dp.txt) mechanically instead of by hand:

  commands[name] = Command(ret, params=[Param(...)], successcodes, errorcodes)
  structs[name]  = Struct(members=[Member(...)])
  handles, enums_names, scalar types classification helpers.

Only understands the subset of vk.xml that exists in the vendored copy
(tools/vulkan-headers/registry/vk.xml): <commands><command>, <structs>
<struct>, <handles>, <enums>. Aliases (commands defined with alias=) are
resolved to their target.
"""
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field


@dataclass
class Param:
    type: str          # base type name, e.g. VkDevice / void / char / uint32_t
    name: str
    len_: str = None   # vk.xml len="count,null-terminated" attribute
    optional: bool = False
    const: bool = False
    is_ptr: int = 0    # pointer level (* count)


@dataclass
class Command:
    name: str
    ret: str
    params: list = field(default_factory=list)
    alias: str = None


@dataclass
class Member:
    type: str
    name: str
    len_: str = None
    optional: bool = False
    values: str = None   # sType values= attribute
    is_ptr: int = 0      # pointer level (* count)
    array_len: int = 0   # fixed C array length ([N]), 0 = none
    is_out: bool = False # non-const pointer (host may write)


# Types that pass as plain integer registers on AAPCS64.
SCALAR_INT = {
    # Vulkan fixed-width scalars
    'void', 'char', 'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t',
    'int8_t', 'int16_t', 'int32_t', 'int64_t', 'size_t', 'VkBool32',
    'VkDeviceSize', 'VkDeviceAddress', 'VkFlags', 'VkFlags64',
    'int', 'float', 'double', 'WlDisplay',  # (placeholder; wl handled separately)
}

def classify_type(tname, handles, enum_names, bitmask_names, funcptrs):
    """Return one of: 'handle', 'enum', 'bitmask', 'scalar', 'float',
    'struct', 'funcptr', 'unknown'."""
    if tname in ('float',):
        return 'float'
    if tname == 'double':
        return 'float'
    if tname in handles:
        return 'handle'
    if tname in enum_names:
        return 'enum'
    if tname in bitmask_names or tname.startswith('VkFlags'):
        return 'bitmask'
    if tname in SCALAR_INT or tname.endswith('_t') and not tname.startswith('PFN_'):
        return 'scalar'
    if tname.startswith('PFN_') or tname in funcptrs:
        return 'funcptr'
    return 'struct'


def parse_structure_types(root):
    """Numeric values of every VkStructureType constant.

    Sources: children of <enums name="VkStructureType"> AND <enum
    extends="VkStructureType"> elements scattered through <extension>
    blocks. Value encodings:
      - value="N"                      → direct
      - offset="N" [extnumber="M"]     → 1000000000 + (M-1)*1000 + N
      - dir="-" negates the offset     (dir defaults to "+")
      - alias="OTHER"                  → resolved in a second pass
    Returns {CONST_NAME: int}.
    """
    def raw_value(e, extnumber_default=1):
        if e.get('value') is not None:
            return int(e.get('value'))
        off = e.get('offset')
        if off is None:
            return None
        ext = int(e.get('extnumber', extnumber_default))
        sign = -1 if e.get('dir') == '-' else 1
        return 1000000000 + (ext - 1) * 1000 + sign * int(off)

    vals = {}
    aliases = {}
    seen_elems = []
    # Containers of VkStructureType enum definitions:
    #   - <enums name="VkStructureType"> children (core, explicit value=)
    #   - <enum extends="VkStructureType"> inside <extension number=M>
    #     (extnumber defaults to M)
    #   - ... inside <feature> blocks (promotions; explicit extnumber=)
    for enums in root.iter('enums'):
        if enums.get('name') == 'VkStructureType':
            seen_elems.extend((e, 1) for e in enums.iter('enum'))
    containers = [(ext, int(ext.get('number') or 1))
                  for ext in root.iter('extension')]
    containers += [(feat, 1) for feat in root.iter('feature')]
    for cont, dflt in containers:
        for e in cont.iter('enum'):
            if e.get('extends') == 'VkStructureType':
                seen_elems.append((e, dflt))
    for e, dflt in seen_elems:
        nm = e.get('name')
        if not nm:
            continue
        if e.get('alias'):
            aliases[nm] = e.get('alias')
            continue
        v = raw_value(e, dflt)
        if v is not None:
            vals[nm] = v
    for nm, target in aliases.items():
        t = target
        while t in aliases:
            t = aliases[t]
        if t in vals:
            vals[nm] = vals[t]
    return vals


def parse_registry(xml_path):
    tree = ET.parse(xml_path)
    root = tree.getroot()
    stypes = parse_structure_types(root)

    handles, bitmask_names, funcptrs = set(), set(), set()
    bitmask64 = set()
    struct_aliases = {}
    union_names = set()
    struct_elems = {}
    bitfield_structs = set()
    for t in root.iter('type'):
        cat = t.get('category')
        nm = t.get('name')
        if cat == 'bitmask':
            # three forms: alias (name=attr), typedef VkFlags (child
            # <name>, 32-bit), typedef VkFlags64 (child <name>, 64-bit)
            txt = ''.join(t.itertext())
            m2 = t.find('name')
            bname = nm or (m2.text if m2 is not None else None)
            if bname:
                bitmask_names.add(bname)
                if 'VkFlags64' in txt:
                    bitmask64.add(bname)
            continue
        if cat == 'handle':
            # primary handles embed their name: VK_DEFINE_HANDLE(<name>VkInstance</name>)
            if nm:
                handles.add(nm)
            else:
                m = re.search(r'VK_DEFINE_[A-Z_]+\((\w+)\)',
                              ''.join(t.itertext()) or '')
                if m:
                    handles.add(m.group(1))
            continue
        if not nm:
            continue
        elif cat == 'bitmask':
            pass   # handled below (needs to run before the name check)
        elif cat == 'funcpointer':
            funcptrs.add(nm)
        elif cat == 'struct' or cat == 'union':
            if t.get('alias'):
                struct_aliases[nm] = t.get('alias')
            else:
                struct_elems[nm] = t
                if cat == 'union':
                    union_names.add(nm)
    enum_names = {e.get('name') for e in root.iter('enums') if e.get('name')}
    # <type category="enum" name=...> forward declarations too
    for t in root.iter('type'):
        if t.get('category') == 'enum' and t.get('name'):
            enum_names.add(t.get('name'))

    # API constants (<enums name="API Constants">: VK_MAX_*_SIZE etc.)
    # — used to resolve enum-sized C array members like
    # char driverName[<enum>VK_MAX_DRIVER_NAME_SIZE</enum>]
    api_consts = {}
    for enums in root.iter('enums'):
        if enums.get('name') == 'API Constants':
            for e in enums.iter('enum'):
                nm, v = e.get('name'), e.get('value')
                if nm and v is not None:
                    try:
                        api_consts[nm] = int(v)
                    except ValueError:
                        pass   # float constants (1000.0F) — unused here

    commands = {}
    commands_elem = root.find('commands')
    for c in commands_elem.iter('command'):
        name = c.get('name')
        if name:
            commands[name] = Command(name=name, ret='', alias=None)
            continue
        proto = c.find('proto')
        if proto is None:
            continue
        cname = proto.findtext('name')
        rtype = proto.findtext('type')
        cmd = Command(name=cname, ret=rtype or 'void')
        # <param api="vulkansc"> duplicates are Safety-Critical variants
        # of the same parameter — keep only the plain-Vulkan ones.
        def keep_api(el):
            api = el.get('api')
            if not api:
                return True
            return 'vulkan' in [a.strip() for a in api.split(',')]
        for p in c.findall('param'):
            if not keep_api(p):
                continue
            ptype_el = p.find('type')
            pname_el = p.find('name')
            text = ''.join(p.itertext())
            ptype = ptype_el.text if ptype_el is not None else ''
            pname = pname_el.text if pname_el is not None else ''
            is_ptr = text.count('*')
            param = Param(
                type=ptype, name=pname,
                len_=p.get('len'),
                optional=(p.get('optional', 'false').split(',')[0] == 'true'),
                const=text.strip().startswith('const'),
                is_ptr=is_ptr,
            )
            cmd.params.append(param)
        commands[cname] = cmd

    # command aliases (e.g. vkCmdBeginRenderingKHR -> vkCmdBeginRendering)
    for c in commands_elem.iter('command'):
        aname, alias = c.get('name'), c.get('alias')
        if aname and alias and alias in commands:
            src = commands[alias]
            import copy
            dst = copy.deepcopy(src)
            dst.name = aname
            commands[aname] = dst

    structs = {}
    for sname, st in struct_elems.items():
        members = []
        def keep_api(el):
            api = el.get('api')
            if not api:
                return True
            return 'vulkan' in [a.strip() for a in api.split(',')]
        for m in st.findall('member'):
            if not keep_api(m):
                continue
            mtype_el = m.find('type')
            mname_el = m.find('name')
            text = ''.join(m.itertext())
            mname = mname_el.text if mname_el is not None else ''
            # fixed C arrays: <name>color</name>[4] — the suffix is
            # SIBLING TEXT after the <name> element, not inside it.
            # Enum-sized arrays use [<enum>VK_MAX_X_SIZE</enum>]; resolve
            # through the API-constants table (fall back to 0 → the
            # member stages as a scalar, which would UNDER-SIZE structs).
            arr = 0
            import re as _re
            ma = _re.search(_re.escape(mname) + r'\[(\d+)\]', text)
            if ma:
                arr = int(ma.group(1))
            else:
                # NOTE: itertext() strips tags, so the suffix is
                # "[NAME]", not "[<enum>NAME</enum>]"
                me = _re.search(
                    _re.escape(mname) + r'\[([A-Za-z_]\w*)\]', text)
                if me:
                    arr = api_consts.get(me.group(1), 0)
                    if not arr:
                        import sys as _sys
                        print(f'vkxml: {sname}.{mname}: unknown array '
                              f'constant {me.group(1)}', file=_sys.stderr)
            ptr = text.count('*')
            import re as _re2
            if _re2.search(_re2.escape(mname) + r':\d+', text):
                bitfield_structs.add(sname)
            members.append(Member(
                type=mtype_el.text if mtype_el is not None else '',
                name=mname,
                len_=m.get('len'),
                optional=m.get('optional', 'false') == 'true',
                values=m.get('values'),
                is_ptr=ptr,
                array_len=arr,
                is_out=(ptr > 0 and not text.strip().startswith('const')),
            ))
        structs[sname] = members

    reg = {
        'handles': handles,
        'enum_names': enum_names,
        'bitmask_names': bitmask_names,
        'bitmask64': bitmask64,
        'funcptrs': funcptrs,
        'commands': commands,
        'structs': structs,
        'struct_aliases': struct_aliases,
        'union_names': union_names,
        'stypes': stypes,
        'bitfield_structs': bitfield_structs,
    }
    # resolve struct aliases AFTER member parsing (aliases may chain)
    def _resolve(name, depth=0):
        while depth < 8 and name in struct_aliases:
            name = struct_aliases[name]
            depth += 1
        return name
    for aname, target in list(struct_aliases.items()):
        tgt = _resolve(target)
        if tgt in structs:
            import copy as _copy
            structs[aname] = _copy.deepcopy(structs[tgt])
    return reg


def arg_kinds(cmd, cls):
    """Derive thunkgen-style ARGS letters for a command.

    i = integer-register scalar/enum/handle/flag (passed verbatim)
    f = float register
    P = pointer to scalar/string/handle array — generic translate+bounce
    S = struct pointer without dynamic length — generic translate+bounce
    D = DEEP: dynamically-sized/nested content (has len= pointing at
        another member, or nested struct arrays) — needs a marshal arm
    """
    out = []
    for p in cmd.params:
        kind = classify_type(p.type, cls['handles'], cls['enum_names'],
                              cls['bitmask_names'], cls['funcptrs'])
        if p.is_ptr == 0:
            if kind == 'float':
                out.append('f')
            else:
                out.append('i')
            continue
        # pointer parameter
        if p.len_:
            # dynamically-sized: null-terminated string OR counted array
            if p.type == 'char' and 'null-terminated' in (p.len_ or ''):
                out.append('P')
                continue
            out.append('D')
            continue
        if p.type == 'void':
            out.append('P')      # raw buffer — generic bounce
            continue
        if kind == 'struct':
            out.append('S')
            continue
        out.append('P')          # scalar*/handle*/string* — translate+bounce
    return ''.join(out)


if __name__ == '__main__':
    import sys
    reg = parse_registry(sys.argv[1] if len(sys.argv) > 1 else
                         'tools/vulkan-headers/registry/vk.xml')
    print(f"commands={len(reg['commands'])} structs={len(reg['structs'])} "
          f"handles={len(reg['handles'])} enums={len(reg['enum_names'])} "
          f"bitmasks={len(reg['bitmask_names'])}")




# ── struct layout (LP64 natural alignment) + pointer-field map ─────────
SCALAR_SIZES = {
    'void': 1, 'char': 1, 'uint8_t': 1, 'int8_t': 1,
    'uint16_t': 2, 'int16_t': 2,
    'float': 4, 'uint32_t': 4, 'int32_t': 4,
    'double': 8, 'uint64_t': 8, 'int64_t': 8,
    'size_t': 8, 'VkDeviceSize': 8, 'VkDeviceAddress': 8,
    'VkBool32': 4, 'VkFlags': 4, 'VkFlags64': 8,
    'int': 4,
}


def type_size(tname, reg, cache):
    if tname in SCALAR_SIZES:
        return SCALAR_SIZES[tname]
    if tname in reg['handles']:
        return 8
    if tname in reg['enum_names']:
        return 4
    if tname.startswith('VkFlags'):
        return 8 if tname.endswith('64') else 4
    if tname in reg.get('bitmask64', set()):
        return 8
    if tname in reg['bitmask_names']:
        return 4
    if tname in reg['structs']:
        lay = layout_struct(tname, reg, cache)
        return lay.size if lay else None
    return None


@dataclass
class FieldPtr:
    offset: int          # byte offset of the pointer member
    elem: str            # 'struct:Name' | 'handle' | 'scalar:N' | 'ptr' | 'char'
    count: str           # 'fixed:N' | 'member:NAME' | 'nullterm'
    writable: bool       # OUT array (host writes it back)
    name: str = ''       # member name ('pNext' marks a chain-link field)


@dataclass
class Layout:
    name: str
    size: int
    align: int
    ptrs: list = field(default_factory=list)


def layout_struct(name, reg, cache=None):
    """Natural-alignment layout + nested-pointer map for one struct.
    Unions take max(member) sizing and expose NO pointer fields
    (conservative: current registry unions used by marshalled commands
    are scalar-only). cache guards recursion."""
    if cache is None:
        cache = {}
    if name in cache:
        return cache[name] if cache[name] is not None else None
    members = reg['structs'].get(name)
    if members is None:
        return None
    info = Layout(name=name, size=0, align=1)
    cache[name] = info   # cycle guard
    if name in reg.get('union_names', set()):
        mx = 0
        ma = 1
        for m in members:
            if m.type in reg['structs']:
                sub = layout_struct(m.type, reg, cache)
                esz, eal = sub.size, sub.align
            else:
                esz = type_size(m.type, reg, cache)
                eal = min(esz, 8) if esz else 1
            if esz is None:
                raise ValueError(f'{name}.{m.name}: unknown size for {m.type}')
            mx = max(mx, esz * (m.array_len if m.array_len else 1))
            ma = max(ma, eal)
        info.size = mx
        info.align = ma
        return info

    def align_up(v, a):
        return (v + a - 1) & ~(a - 1)

    offset = 0
    max_align = 1
    for m in members:
        cnt = m.array_len
        if m.is_ptr:
            max_align = max(max_align, 8)
            offset = align_up(offset, 8)
            base = m.type
            if base == 'void':
                elem = 'ptr'
            elif base == 'char':
                elem = 'char'
            elif base in reg['handles']:
                elem = 'handle'
            elif base in reg['structs']:
                elem = 'struct:' + base
            else:
                sz = SCALAR_SIZES.get(base)
                elem = f'scalar:{sz}' if sz else 'ptr'
            ln = m.len_ or ''
            if 'null-terminated' in ln and ',' not in ln:
                count = 'nullterm'
            elif ln:
                count = 'member:' + ln.split(',')[0]
            else:
                count = 'fixed:1'
            info.ptrs.append(FieldPtr(offset=offset, elem=elem, count=count,
                                      writable=m.is_out, name=m.name))
            offset += 8 * (cnt if cnt else 1)
            continue
        # value member
        if mtype_sub := reg['structs'].get(m.type):
            sub = layout_struct(m.type, reg, cache)
            esz, eal = sub.size, sub.align
        else:
            esz = type_size(m.type, reg, cache)
            eal = min(esz, 8) if esz else 1
        if esz is None:
            raise ValueError(f'{name}.{m.name}: unknown size for {m.type}')
        max_align = max(max_align, eal)
        offset = align_up(offset, eal)
        offset += esz * (cnt if cnt else 1)
        m._offset = offset - esz * (cnt if cnt else 1)
        # Flatten the interior pointer fields of a SINGLE by-value
        # struct member into this layout's ptrs (offset-adjusted,
        # dotted count-member paths). Without this, structs embedding
        # another struct BY VALUE (VkComputePipelineCreateInfo.stage…)
        # expose no pointer rows at all and the runtime hands raw
        # guest interior pointers (pName/pSpecializationInfo) to the
        # host driver. Arrays of structs by value are NOT flattened —
        # per-element staging isn't expressible in one descriptor row.
        if mtype_sub:
            if not cnt:
                base = m._offset
                for sp in sub.ptrs:
                    if sp.count.startswith('member:'):
                        cntname = '%s.%s' % (
                            m.name, sp.count.split(':', 1)[1])
                        ncount = 'member:' + cntname
                    else:
                        ncount = sp.count   # fixed:N / nullterm
                    info.ptrs.append(FieldPtr(
                        offset=base + sp.offset,
                        elem=sp.elem,
                        count=ncount,
                        writable=sp.writable,
                        name=sp.name))
            elif sub.ptrs:
                print(f'{name}.{m.name}: array-of-struct by value '
                      f'carries pointer fields — not flattened',
                      file=sys.stderr)
    info.align = max_align
    info.size = align_up(offset, max_align)
    return info


def layout_all(reg):
    cache = {}
    out = {}
    for name in reg['structs']:
        try:
            lay = layout_struct(name, reg, cache)
        except (ValueError, KeyError):
            lay = None
        if lay:
            out[name] = lay
    return out
