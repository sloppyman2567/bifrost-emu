#!/usr/bin/env python3
"""vkmarshalgen.py — generate Vulkan deep-marshal descriptors from vk.xml.

For an allowlist of commands whose dynamically-sized/nested parameters are
currently dispatched as plain 'p' (a host-driver crash waiting for a
multi-element array), emit:

  - per-struct descriptor tables (size + pointer-field map), where every
    pointer field records how to find its element count (same-struct u32
    count member offset, or fixed);
  - per-command "plans": which argument indices hold arrays of which
    struct descriptor, and which register holds the element count.

The runtime consumer lives in display_thunk.cpp (policy VK_CMD_DEEP):
a two-pass staging marshal (dry size pass, then fill) into a VkStage
arena, replacing guest pointers with host pointers before the host call.
All covered vkCmd*/device commands are INPUT-ONLY, so there is no
writeback.

Layouts come from vkxml.layout_all(), already validated byte-for-byte
against the vendored vulkan_core.h (see vk_layout validation).

Usage: python3 tools/opgen/vkmarshalgen.py <vk.xml> <out.hpp>
"""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vkxml import parse_registry, layout_all

# Commands converted to VK_CMD_DEEP / VK_CMD_DEEP_OUT. The count parameter
# for each dynamic array is derived AUTOMATICALLY from the registry's
# len="..." attribute (matched against sibling parameter names) — no
# hand-maintained indices. RAW = byte-counted void* handled specially.
#
# Direction is derived per pointer parameter ("auto"):
#   - count sibling in a REGISTER            → plain IN staging (out=0)
#   - count sibling is a POINTER (u32*)      → enumeration: the host
#     writes the actual count through it; staged array + count are
#     copied back after the call (out=1)
#   - no counted sibling but the param is non-const (void* pData) →
#     copyback-only raw staging (out=2)
CMD_PLANS = {
    'vkCmdSetViewport': None, 'vkCmdSetScissor': None,
    'vkCmdPipelineBarrier': None, 'vkCmdClearAttachments': None,
    'vkCmdClearColorImage': None, 'vkCmdClearDepthStencilImage': None,
    'vkCmdCopyBuffer': None, 'vkCmdCopyImage': None,
    'vkCmdCopyBufferToImage': None, 'vkCmdCopyImageToBuffer': None,
    'vkCmdExecuteCommands': None, 'vkCmdWaitEvents': None,
    'vkResetFences': None,
    # A2: enumerations (OUT) + IN arrays dispatched as plain 'p' before
    'vkEnumeratePhysicalDevices': None,
    'vkGetSwapchainImagesKHR': None,
    'vkGetPhysicalDeviceSurfaceFormatsKHR': None,
    'vkGetPhysicalDeviceSurfacePresentModesKHR': None,
    'vkGetPhysicalDeviceQueueFamilyProperties': None,
    'vkGetQueryPoolResults': None,
    'vkFreeCommandBuffers': None, 'vkFreeDescriptorSets': None,
    'vkCmdBindDescriptorSets': None, 'vkCmdBindVertexBuffers': None,
    'vkCmdUpdateBuffer': None, 'vkCmdPushConstants': None,
    # C1 modern rows (flat top-level arrays only). NOTE: the WithCount /
    # BindVertexBuffers2 aliases are EXT (extended_dynamic_state), not KHR.
    'vkCmdBindVertexBuffers2': None,
    'vkCmdBindVertexBuffers2EXT': None,
    'vkCmdSetViewportWithCount': None, 'vkCmdSetViewportWithCountEXT': None,
    'vkCmdSetScissorWithCount': None, 'vkCmdSetScissorWithCountEXT': None,
    # Phase B batch 4: submit + descriptor-update batches (auto-derived
    # struct arrays)
    'vkQueueSubmit': None,
    'vkQueueSubmit2': None, 'vkQueueSubmit2KHR': None,
    'vkUpdateDescriptorSets': None,
    # Phase B batch 5: fence waits
    'vkWaitForFences': None,
}

# Create-style commands (Phase B batches 1–3): create/record shape with
# pCreateInfo-style params. Ref roles are DERIVED per parameter:
#   - const pointer to a described struct        -> out=4 STRUCT_IN
#     (count from the param's len= sibling when present, else single)
#   - const VkAllocationCallbacks*               -> out=3 NULLIFY
#   - non-const handle-typed pointer             -> out=5 OUT_HANDLE(S)
CMD_CREATE_PLANS = {
    'vkCreateShaderModule', 'vkCreatePipelineLayout',
    'vkCreateDescriptorPool', 'vkCreateFramebuffer',
    # Phase B batch 2: nested attachment/subpass/binding trees — the
    # generated recursion consumes them wholesale
    'vkCreateRenderPass', 'vkCreateDescriptorSetLayout',
    # Phase B batch 3: pipeline trees (pName strings, specialization
    # blobs, state sub-structs) + OUT handle arrays
    'vkCreateGraphicsPipelines', 'vkCreateComputePipelines',
    # Phase B batch 4: OUT array whose count lives INSIDE the staged
    # struct (pDescriptorSets count = pAllocateInfo->descriptorSetCount)
    'vkAllocateDescriptorSets',
    # Phase B batch 5
    'vkCreateInstance', 'vkCreateDevice',
    'vkAllocateCommandBuffers',
}
# vkQueuePresentKHR STAYS a hand arm by design (plan Phase B item 5):
# pResults needs FIELD-level OUT semantics inside the staged struct,
# which plans do not express — plus present-mode/PCWFC behavior.

# For out=5 refs whose count comes from a member of the staged STRUCT_IN
# struct (count_arg=0xFE): command -> count-member name. The offset is
# resolved against that plan's STRUCT_IN descriptor.
CMD_OUT_COUNT_MEMBER = {
    'vkAllocateDescriptorSets': 'descriptorSetCount',
    'vkAllocateCommandBuffers': 'commandBufferCount',
}

HEADER = r"""// opgen_vkmarshal.hpp — GENERATED. DO NOT EDIT.
//
// Generated by tools/opgen/vkmarshalgen.py from the Khronos vk.xml
// registry (tools/vulkan-headers/registry/vk.xml). Struct sizes/layouts
// are validated against vulkan_core.h by the generator self-test.
//
// Consumed by DisplayThunk's VK_CMD_DEEP policy: a two-pass staging
// deep-marshal for command-batch parameters (barrier arrays, copy
// regions, clears, handle arrays...) that plain pointer translation
// cannot express.
#pragma once
#include <cstdint>
#include <cstddef>

namespace arm64emu {
namespace thunk {

enum : uint8_t {
    VKM_NONE = 0,
    VKM_STRUCT = 1,     // pointee is another descriptor
    VKM_HANDLE = 2,     // opaque handle(s) — copy verbatim
    VKM_RAW = 3,        // untyped bytes (void*)

    VKM_PNEXT = 0x80,   // FLAG (OR'd into elem): this field is a pNext
                        // chain link — walk it via vk_find_struct_by_stype
    VKM_STR = 0x40,     // FLAG: NUL-terminated char* — stage strlen+1
    VKM_STRARR = 0x20,  // FLAG: array of NUL-terminated char* — stage the
                        // pointer array PLUS every string (elem_size 8)
};

struct VkFieldDesc {
    uint16_t off;          // byte offset of the pointer member
    uint8_t  elem;         // VKM_*
    uint8_t  elem_size;    // bytes per scalar/handle element
    int16_t  elem_struct;  // descriptor index when elem == VKM_STRUCT
    uint16_t count_off;    // offset of u32 count member in THIS struct
                           // (0xFFFF = use fixed_count)
    uint32_t fixed_count;  // element count when count_off == 0xFFFF
};

struct VkStructDesc {
    const char* name;
    uint16_t size;
    const VkFieldDesc* fields;
    uint16_t nfields;
};

struct VkPlanRef {
    uint8_t arg;           // register/args index of the array pointer
    uint8_t count_arg;     // register/args index holding the count
                           // (0xFF = single element; 0xFE = count is a
                           // u32 member of the staged struct at aux)
    uint8_t count_in_bytes;// 1 = count is a BYTE count (raw buffers)
    uint8_t out;           // 0 = input staging
                           // 1 = enumeration: count_arg is a guest u32*
                           //     pointer — host writes the actual count;
                           //     copy back min(staged, actual) elements
                           //     AND the count after the host call
                           // 2 = copyback-only (non-const raw pData)
                           // 3 = NULLIFY: write nullptr into this arg
                           // 4 = STRUCT_IN: stage struct(s) of `desc` at
                           //     args[arg] — count = args[count_arg]
                           //     (count_arg 0xFF = exactly one)
                           // 5 = OUT_HANDLE: zeroed bounce; host writes
                           //     handle(s); copied back to the guest
                           //     pointer after a successful call
                           //     (count_arg 0xFF = single handle,
                           //      0xFE = count from staged struct aux)
    uint8_t elem_size;     // bytes per element for desc==nullptr arrays
    uint8_t aux;           // role-specific: byte offset of the count
                           // member for out=5/count_arg=0xFE
    const VkStructDesc* desc;   // nullptr for verbatim byte staging
};

struct VkStypeEntry {
    int32_t stype;         // numeric VK_STRUCTURE_TYPE_* value
    uint16_t desc;         // kVkStructs[] index of the struct layout
};

struct VkCmdPlan {
    const char* name;
    uint8_t nrefs;
    const VkPlanRef* refs;
};

{STRUCT_TABLES}

// sentinel: opaque 8-byte element (handle arrays) copied verbatim
inline constexpr VkStructDesc kVkHandleElem = {
    "<handle>", 8, nullptr, 0};

inline constexpr VkStructDesc kVkStructs[] = {
{STRUCT_ROWS}
};

inline constexpr size_t kVkStructCount =
    sizeof(kVkStructs) / sizeof(kVkStructs[0]);

inline const VkStructDesc* vk_find_struct(const char* name) {
    for (size_t i = 0; i < kVkStructCount; i++)
        if (__builtin_strcmp(kVkStructs[i].name, name) == 0)
            return &kVkStructs[i];
    return nullptr;
}

{PLAN_TABLES}

inline constexpr VkCmdPlan kVkCmdPlans[] = {
{PLAN_ROWS}
};

inline constexpr size_t kVkCmdPlanCount =
    sizeof(kVkCmdPlans) / sizeof(kVkCmdPlans[0]);

inline const VkCmdPlan* vk_find_cmd_plan(const char* name) {
    for (size_t i = 0; i < kVkCmdPlanCount; i++)
        if (__builtin_strcmp(kVkCmdPlans[i].name, name) == 0)
            return &kVkCmdPlans[i];
    return nullptr;
}

// sType → struct-descriptor index for EVERY chainable struct in the
// registry (sorted ascending; binary search). Unknown sTypes are not
// present — callers truncate the chain there.
{STYPE_TABLE}

inline constexpr size_t kVkStypeCount =
    sizeof(kVkStypeIndex) / sizeof(kVkStypeIndex[0]);

inline const VkStructDesc* vk_find_struct_by_stype(int32_t stype) {
    size_t lo = 0, hi = kVkStypeCount;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (kVkStypeIndex[mid].stype < stype) lo = mid + 1;
        else hi = mid;
    }
    if (lo < kVkStypeCount && kVkStypeIndex[lo].stype == stype)
        return &kVkStructs[kVkStypeIndex[lo].desc];
    return nullptr;
}

} // namespace thunk
} // namespace arm64emu
"""


def main():
    xml_path = sys.argv[1] if len(sys.argv) > 1 else \
        'tools/vulkan-headers/registry/vk.xml'
    out_path = sys.argv[2] if len(sys.argv) > 2 else \
        'include/opgen_vkmarshal.hpp'
    reg = parse_registry(xml_path)
    layouts = layout_all(reg)

    # ── collect needed structs (closure over plans) ──────────────────
    needed = {}       # struct name -> list of (offset, FieldPtr-like info)
    order = []
    def member_offset(lay, mname):
        # recompute offsets to find the count member's byte position
        import re
        off = 0

        def au(v, a):
            return (v + a - 1) & ~(a - 1)
        for m in reg['structs'][lay.name]:
            cnt = m.array_len
            if m.is_ptr:
                off = au(off, 8)
                o = off
                off += 8 * (cnt or 1)
                if m.name == mname:
                    return o
                continue
            if m.type in layouts:
                sub = layouts[m.type]
                esz, eal = sub.size, sub.align
            else:
                from vkxml import SCALAR_SIZES
                esz = SCALAR_SIZES.get(m.type)
                if esz is None and m.type in reg['handles']:
                    esz = 8
                if esz is None and m.type in reg['enum_names']:
                    esz = 4
                if esz is None and m.type in reg['bitmask_names']:
                    esz = 8 if m.type in reg['bitmask64'] else 4
                eal = min(esz or 1, 8)
            if esz is None:
                return None
            off = au(off, eal)
            o = off
            off += esz * (cnt or 1)
            if m.name == mname:
                return o
        return None

    def want_struct(name):
        if name in needed:
            return True
        lay = layouts.get(name)
        if lay is None:
            return False
        needed[name] = True
        order.append(name)
        return True

    plans = {}
    for cmd in CMD_PLANS:
        c = reg['commands'].get(cmd)
        if c is None:
            print(f'WARN: {cmd} not in registry', file=sys.stderr)
            continue
        plist = []
        ok = True
        for pidx, p in enumerate(c.params):
            if not p.is_ptr:
                continue
            ln = p.len_ or ''
            # resolve len expression to a sibling parameter (the count
            # may itself be a pointer for enumeration-style commands)
            cidx = None
            count_is_ptr = False
            base = ln.split(',')[0].strip()
            for j, q in enumerate(c.params):
                if q.name == base:
                    cidx = j
                    count_is_ptr = q.is_ptr > 0
                    break
            # direction ("auto", see CMD_PLANS comment)
            if cidx is not None and count_is_ptr:
                out = 1     # enumeration: count written through a pointer
            elif not p.const:
                out = 2     # non-const data pointer: copy back after call
            else:
                out = 0     # plain input staging
            byte_count = False
            if out == 2 and cidx is not None:
                # byte counts arrive directly (dataSize); element counts
                # multiply by the element size (pResults)
                cq = c.params[cidx]
                byte_count = cq.type in ('size_t', 'VkDeviceSize')
            if cidx is None:
                if out == 2 and p.type == 'void':
                    # uncounted OUT void* cannot be staged safely — leave
                    # it on the generic bounce path
                    print(f'WARN: {cmd} param {p.name}: uncounted OUT '
                          f'pointer left generic', file=sys.stderr)
                continue
            if p.type == 'void':
                plist.append((pidx, cidx, ('raw', 1, True), out))
                continue
            if p.type in reg['handles']:
                plist.append((pidx, cidx, ('raw', 8, byte_count), out))
                continue
            # NOTE: the struct-layouts check MUST come before the scalar
            # type_size fallback — type_size() resolves struct names too,
            # and a struct staged as flat raw bytes would hand the host
            # driver untranslated interior guest pointers.
            if p.type in layouts:
                want_struct(p.type)
                plist.append((pidx, cidx, p.type, out))
                continue
            from vkxml import type_size
            tsz = type_size(p.type, reg, {})
            if tsz is not None:
                # scalar/enum/bitmask array — stage verbatim
                plist.append((pidx, cidx, ('raw', tsz, byte_count), out))
                continue
            print(f'WARN: {cmd} param {p.name}: no layout for '
                  f'{p.type}', file=sys.stderr)
            ok = False
            break
        if ok:
            plans[cmd] = plist

    CMD_CREATE_PLANS.add('vkCmdBeginRenderPass')

    # ── create-style plans (see CMD_CREATE_PLANS comment) ─────────────
    out_aux = {}   # cmd -> byte offset of count member (count_arg=0xFE)
    # Batch 3 extension: len-carrying params are supported —
    #   - const Struct* WITH len  -> out=4 staging an ARRAY of structs
    #     (count = sibling param named by len; count_arg=0xFF = single)
    #   - Handle* WITH len        -> out=6 OUT handle array written by
    #     the host, copied back after a successful call
    for cmd in sorted(CMD_CREATE_PLANS):
        c = reg['commands'].get(cmd)
        if c is None:
            print(f'WARN: {cmd} not in registry', file=sys.stderr)
            continue
        plist = []
        ok = True
        def sibling_index(name):
            for j, q in enumerate(c.params):
                if q.name == name:
                    return j
            return None
        for pidx, p in enumerate(c.params):
            if not p.is_ptr:
                continue
            ln = (p.len_ or '').split(',')[0].strip()
            cidx = sibling_index(ln) if ln else None
            if not p.const and p.type in reg['handles']:
                plist.append((pidx, cidx if cidx is not None else 0xFF,
                              ('hnd',), 5))          # OUT handle(s)
                continue
            if p.const and p.type == 'VkAllocationCallbacks':
                plist.append((pidx, 0, ('hnd',), 3))       # NULLIFY
                continue
            if p.const and p.type in layouts and \
                    (not p.len_ or cidx is not None):
                want_struct(p.type)
                plist.append((pidx,
                              cidx if cidx is not None else 0xFF,
                              p.type, 4))              # STRUCT_IN (1..n)
                continue
            # anything else on a create-style command is unexpected —
            # fail loudly rather than half-marshal
            print(f'WARN: {cmd} param {p.name} ({p.type}): no create-'
                  f'plan role — command left generic', file=sys.stderr)
            ok = False
            break
        if ok and any(r[3] == 4 for r in plist):
            # count-from-staged-member OUT arrays (count_arg=0xFE):
            # resolve the count member offset against this plan's
            # STRUCT_IN descriptor
            member = CMD_OUT_COUNT_MEMBER.get(cmd)
            if member:
                struct_name = next(r[2] for r in plist if r[3] == 4)
                co = member_offset(layouts[struct_name], member)
                if co is None:
                    print(f'WARN: {cmd}: cannot locate out-count member '
                          f'{member} in {struct_name}', file=sys.stderr)
                    continue
                plist = [r if r[3] != 5 else
                         (r[0], 0xFE, r[2], r[3]) for r in plist]
                out_aux[cmd] = co
            plans[cmd] = plist

    # ── chainable structs: EVERY struct with a resolvable sType member
    # gets a descriptor so generated pNext-chain walking covers the full
    # registry, not just the plan closure.
    stype_of = {}     # struct name -> numeric sType value
    for name, members in reg['structs'].items():
        for m in members:
            if m.values and m.values in reg['stypes']:
                stype_of[name] = reg['stypes'][m.values]
                break
    n_chain_added = 0
    for name in sorted(stype_of):
        if want_struct(name):
            n_chain_added += 1

    # closure: recurse into struct-typed pointer fields (covers nested
    # refs of both plan structs and newly-added chainable structs)
    i = 0
    while i < len(order):
        name = order[i]
        i += 1
        lay = layouts[name]
        for pf in lay.ptrs:
            if pf.elem.startswith('struct:'):
                want_struct(pf.elem.split(':', 1)[1])

    # A pointer FIELD doesn't require its pointee to have a computable
    # layout (vk.xml LUNARG funcptr members etc.) — but our descriptors
    # must be complete. Drop any struct whose nested struct refs never
    # registered (iterate: dropping may orphan others). Such structs are
    # also excluded from the sType map below, so the runtime truncates
    # guest chains at them (the safe, documented behavior).
    pruned = []
    changed = True
    while changed:
        changed = False
        for name in list(order):
            bad_ref = False
            for pf in layouts[name].ptrs:
                if pf.elem.startswith('struct:') and \
                        pf.elem.split(':', 1)[1] not in needed:
                    bad_ref = True
                    break
            if bad_ref:
                order.remove(name)
                needed.pop(name, None)
                pruned.append(name)
                changed = True
    for name in pruned:
        print(f'WARN: {name}: pruned (nested struct has no layout)',
              file=sys.stderr)
    stype_of = {k: v for k, v in stype_of.items() if k in needed}

    # ── field descriptors ────────────────────────────────────────────
    struct_index = {n: i for i, n in enumerate(order)}
    tables = []
    rows = []
    for name in order:
        lay = layouts[name]
        fields = []
        for pf in lay.ptrs:
            elem = {'struct': 1, 'handle': 2, 'ptr': 3,
                    'char': 3}.get(pf.elem.split(':')[0], 3)
            elem_struct = 0
            elem_size = 8
            if pf.elem.startswith('struct:'):
                elem_struct = struct_index[pf.elem.split(':', 1)[1]]
                elem = 1
                elem_size = layouts[pf.elem.split(':', 1)[1]].size
            elif pf.elem == 'handle':
                elem, elem_size = 2, 8
            elif pf.elem == 'char':
                elem, elem_size = 3, 1   # treated as verbatim bytes
            elif pf.elem.startswith('scalar:'):
                # typed scalar array (uint32_t*, VkDeviceSize*, ...)
                elem, elem_size = 3, int(pf.elem.split(':', 1)[1])
            if getattr(pf, 'name', '') == 'pNext':
                # chain-link field — runtime walks the guest chain via
                # vk_find_struct_by_stype instead of staging raw bytes
                elem |= 0x80
            elif pf.elem == 'char' and pf.count == 'nullterm':
                # NUL-terminated string — runtime stages strlen+1 bytes
                elem |= 0x40
            elif pf.elem == 'char' and pf.count.startswith('member:'):
                # counted array of strings (ppEnabledExtensionNames &
                # friends) — runtime stages the pointer array AND every
                # string; elem_size stays 8 (slot stride)
                elem, elem_size = 0x60, 8    # STR|STRARR
            if pf.count == 'fixed:1':
                count_off, fixed = 0xFFFF, 1
            elif pf.count == 'nullterm':
                count_off, fixed = 0xFFFF, 1   # single element; NUL-safe
            else:
                mname = pf.count.split(':', 1)[1]
                latex = 'latexmath' in mname
                co = member_offset(lay, mname.split('.')[0])
                if co is None and latex:
                    # len="latexmath:[\textrm{codeSize} \over 4]" etc.:
                    # recover the referenced member name and stage the
                    # buffer BYTE-granular (elem_size=1) so the full
                    # byte count is staged regardless of the divisor
                    import re as _re
                    for ident in _re.findall(r'[A-Za-z_]\w*', mname):
                        co = member_offset(lay, ident)
                        if co is not None:
                            elem, elem_size = 3, 1
                            break
                if co is None:
                    print(f'WARN: {name}: cannot locate count member '
                          f'{mname}', file=sys.stderr)
                    continue
                count_off, fixed = co, 0
            fields.append((pf.offset, elem, elem_size, elem_struct,
                           count_off, fixed))
        if fields:
            arr_name = f'kFields_{name}'
            tables.append(
                f'inline constexpr VkFieldDesc {arr_name}[] = {{\n' +
                ''.join(
                    f'    {{{off},{el},{esz},{est},'
                    f'{("0x%04X" % co) if co != 0xFFFF else str(co)},{fx}}},\n'
                    for (off, el, esz, est, co, fx) in fields) +
                '};')
            rows.append(f'    {{"{name}", {lay.size}, {arr_name}, '
                        f'{len(fields)}}},')
        else:
            rows.append(f'    {{"{name}", {lay.size}, nullptr, 0}},')

    plan_tables, plan_rows = [], []
    for cmd, plist in plans.items():
        refs = []
        for (pidx, cidx, sname, out) in plist:
            if out == 3:
                # NULLIFY: no descriptor, no count
                refs.append(f'    {{{pidx},0,0,{out},8,0,nullptr}},')
                continue
            if out == 5:
                # OUT_HANDLE(S): count_arg = len sibling (0xFF = single,
                # 0xFE = count member inside the staged struct at aux)
                aux = out_aux.get(cmd, 0)
                refs.append(f'    {{{pidx},{cidx},0,{out},8,{aux},nullptr}},')
                continue
            if isinstance(sname, tuple):
                if sname[0] == 'hnd':
                    refs.append(f'    {{{pidx},{cidx},0,{out},8,0,nullptr}},')
                    continue
                _, esz, bc = sname
                refs.append(f'    {{{pidx},{cidx},{1 if bc else 0},'
                            f'{out},{esz},0,nullptr}},')
                continue
            desc = f'&kVkStructs[{struct_index[sname]}]' if sname else 'nullptr'
            refs.append(f'    {{{pidx},{cidx},0,{out},8,0,{desc}}},')
        arr = f'kRefs_{cmd}'
        plan_tables.append(
            f'inline constexpr VkPlanRef {arr}[] = {{\n' +
            '\n'.join(refs) + '\n};')
        plan_rows.append(f'    {{"{cmd}", {len(plist)}, {arr}}},')

    # sType → descriptor index (sorted ascending for binary search;
    # duplicate values deduped — keep the first struct alphabetically)
    stype_rows = sorted(
        ((v, k) for k, v in stype_of.items() if k in struct_index),
        key=lambda t: (t[0], t[1]))
    seen_st = {}
    entries = []
    for v, k in stype_rows:
        if v in seen_st:
            continue
        seen_st[v] = k
        entries.append((v, struct_index[k]))
    stype_table = (
        'inline constexpr VkStypeEntry kVkStypeIndex[] = {\n' +
        ''.join(f'    {{{v},{i}}},\n' for (v, i) in entries) +
        '};')

    out = HEADER
    out = out.replace('{STRUCT_TABLES}', '\n\n'.join(tables))
    out = out.replace('{STRUCT_ROWS}', '\n'.join(rows))
    out = out.replace('{PLAN_TABLES}', '\n\n'.join(plan_tables))
    out = out.replace('{PLAN_ROWS}', '\n'.join(plan_rows))
    out = out.replace('{STYPE_TABLE}', stype_table)
    with open(out_path, 'w') as fh:
        fh.write(out)
    print(f'wrote {out_path}: {len(order)} struct descriptors '
          f'({n_chain_added} chainable), {len(plans)} command plans, '
          f'{len(entries)} sType entries')


if __name__ == '__main__':
    main()
