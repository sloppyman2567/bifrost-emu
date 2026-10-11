// VK_CMD_DEEP: descriptor-driven deep marshal of guest Vulkan structures.
#include "System/Graphics/Thunks/DisplayThunk/DisplayThunkInternal.hpp"
namespace arm64emu {
// ── VK_CMD_DEEP: descriptor-driven deep marshal for command batches ──
// Two passes over the guest structures: a dry size pass (so VkStage can
// reserve exactly once — its buffer reallocates past the initial 64 KiB,
// which would dangle every earlier pointer), then a fill pass. All
// covered commands are INPUT-ONLY (command recording), so there is no
// writeback. Defensive caps mirror the hand arms (≤1024 elements).

// Read `size` bytes of guest memory: direct-window alias when possible,
// otherwise copy into buf (>= size). nullptr on unmapped/garbage.
const void* vk_deep_read(Memory* mem, uint64_t g, size_t size,
                         void* buf, size_t bufsz) {
    if (!g || g > (~uint64_t(0)) - size) return nullptr;
    uint8_t* hp = mem->guest_to_host_ptr(g);
    if (hp && mem->guest_to_host_ptr(g + size - 1)) return hp;
    if (size > bufsz) return nullptr;
    try { mem->read(g, buf, size); } catch (...) { return nullptr; }
    return buf;
}

const void* vk_deep_ptr_field(const void* elem, const thunk::VkFieldDesc& f) {
    const void* p;
    std::memcpy(&p, reinterpret_cast<const uint8_t*>(elem) + f.off,
                sizeof(p));
    return p;
}
uint32_t vk_deep_count(const void* elem, const thunk::VkFieldDesc& f) {
    if (f.count_off == 0xFFFF) return f.fixed_count;
    uint32_t n;
    std::memcpy(&n, reinterpret_cast<const uint8_t*>(elem) + f.count_off, 4);
    return n;
}

// One-shot diagnostic for an unrecognized sType in a guest pNext chain.
// The chain truncates at that node (safe: the guest sees the rest of its
// chain untouched instead of garbage); this prints ONCE per process.
void vk_deep_unknown_stype_once(int32_t s) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
        fprintf(stderr,
                "[vk-deep] unknown pNext sType %d — chain truncated "
                "(struct not in vk.xml descriptor set)\n", s);
    }
}

// One-shot diagnostic for an element count whose AAPCS64 slot carried
// garbage above bit 31 even AFTER masking (i.e. a real u32 count larger
// than kVkDeepMaxElems). The plan fails closed and the generic translate
// path handles every planned arg with its original guest pointer.
void vk_deep_oversize_count_once(const char* name, uint8_t arg,
                                 uint64_t raw64, uint32_t masked) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
        fprintf(stderr,
                "[vk-deep] %s: element count arg[%u] exceeds staging cap "
                "(raw slot=0x%016llx masked=%u) — deep-marshal plan "
                "declined, generic bounce fallback\n",
                name, arg,
                static_cast<unsigned long long>(raw64),
                masked);
    }
}

// One-shot diagnostic for a misaligned u64-handle array pointer (e.g. an
// odd pDescriptorSets). Guest handle arrays are always 8-aligned; a
// misaligned address is a corrupt pointer (vkQuake hands one bad bind in
// ~13k after 30+ good frames). The caller degrades it to an empty array
// (count 0 + null) so the host sees a legal no-op instead of faulting.
void vk_deep_misaligned_once(const char* name, uint8_t arg, uint64_t ptr) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
        fprintf(stderr,
                "[vk-deep] %s: handle array arg[%u] misaligned (ptr=0x%llx) "
                "— corrupt guest pointer, binding empty\n",
                name, arg, static_cast<unsigned long long>(ptr));
    }
    // Always log the thread (even after the one-shot): consecutive
    // corruptions on different threads point at shared machinery.
    fprintf(stderr, "[vk-deep] misaligned context: T%lx\n",
            (unsigned long)pthread_self());
}

// Bounded guest strlen (strings staged by VKM_STR fields; capped well
// below any sane pipeline stage name).
size_t vk_deep_guest_strlen(Memory* mem, uint64_t g) {
    constexpr size_t kMaxStr = 256;
    if (!g) return 0;
    const uint8_t* hp =
        reinterpret_cast<const uint8_t*>(mem->guest_to_host_ptr(g));
    if (hp) {
        size_t n = 0;
        while (n < kMaxStr && mem->guest_to_host_ptr(g + n) && hp[n]) n++;
        return n;
    }
    size_t n = 0;
    while (n < kMaxStr) {
        uint8_t c = 0;
        try { mem->read(g + n, &c, 1); } catch (...) { break; }
        if (!c) break;
        n++;
    }
    return n;
}


// Dry-size walk of a guest pNext chain (must mirror vk_deep_fill_chain
// allocation-for-allocation — the two-pass staging contract).
size_t vk_deep_chain_size(Memory* mem, uint64_t head, bool* ok, int depth) {
    size_t total = 0;
    int guard = 0;
    while (head && guard++ < kVkDeepMaxPnextNodes) {
        int32_t s = 0;
        uint64_t pn = 0;
        try { mem->read(head, &s, 4); mem->read(head + 8, &pn, 8); }
        catch (...) { break; }
        const thunk::VkStructDesc* d = thunk::vk_find_struct_by_stype(s);
        if (!d) { vk_deep_unknown_stype_once(s); return total; }
        total = (total + 7u) & ~size_t(7);
        // Marshal the node's arrays/strings/structs too. The chain walk
        // owns pNext; the element walker must skip that field here.
        total += vk_deep_size_one(mem, head, d, 1, depth, ok, true);
        if (!*ok || total > kVkDeepMaxBytes) { *ok = false; return 0; }
        head = pn;
    }
    if (head) { *ok = false; return 0; }  // cycle / overlong chain
    return total;
}

// Fill pass: stage every known chain node and relink host-side. *slot
// receives the HOST head (nullptr when nothing was staged).
void vk_deep_fill_chain(Memory* mem, VkStage& st, uint64_t head,
                        void** slot, int depth) {
    *slot = nullptr;
    void* prev = nullptr;
    int guard = 0;
    while (head && guard++ < kVkDeepMaxPnextNodes) {
        int32_t s = 0;
        uint64_t pn = 0;
        try { mem->read(head, &s, 4); mem->read(head + 8, &pn, 8); }
        catch (...) { break; }
        const thunk::VkStructDesc* d = thunk::vk_find_struct_by_stype(s);
        if (!d) { vk_deep_unknown_stype_once(s); break; }
        uint8_t* h = reinterpret_cast<uint8_t*>(st.bytes(d->size, 8));
        vk_deep_fill_elem(mem, st, head, d, h, depth, true);
        *reinterpret_cast<void**>(h + 8) = nullptr;
        if (prev)
            *reinterpret_cast<void**>(static_cast<uint8_t*>(prev) + 8) = h;
        else
            *slot = h;
        prev = h;
        head = pn;
    }
}

size_t vk_deep_size_one(Memory* mem, uint64_t guest,
                        const thunk::VkStructDesc* d, uint32_t count,
                        int depth, bool* ok, bool skip_pnext) {
    if (!ok) return 0;
    if (depth > 4 || count > kVkDeepMaxElems) { *ok = false; return 0; }
    size_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t g = guest + (uint64_t)i * d->size;
        total = (total + 7u) & ~size_t(7);
        total += d->size;
        if (total > kVkDeepMaxBytes) { *ok = false; return 0; }
        for (uint16_t fi = 0; fi < d->nfields; fi++) {
            const thunk::VkFieldDesc& f = d->fields[fi];
            if (f.elem & thunk::VKM_PNEXT) {
                if (skip_pnext) continue;
                // chain link: account for every known node's staging
                uint8_t stackbuf2[512];
                const void* he2 =
                    vk_deep_read(mem, g, d->size, stackbuf2, sizeof(stackbuf2));
                if (!he2) { *ok = false; return 0; }
                uint64_t head =
                    reinterpret_cast<uint64_t>(vk_deep_ptr_field(he2, f));
                total = (total + 7u) & ~size_t(7);
                total += vk_deep_chain_size(mem, head, ok, depth + 1);
                if (!*ok) return 0;
                continue;
            }
            if (f.elem & thunk::VKM_STRARR) {
                // array of NUL-terminated strings: stage the pointer
                // array plus every string (must mirror fill exactly)
                uint8_t stackbuf4[512];
                const void* he4 =
                    vk_deep_read(mem, g, d->size, stackbuf4, sizeof(stackbuf4));
                if (!he4) { *ok = false; return 0; }
                const void* spa = vk_deep_ptr_field(he4, f);
                uint32_t sn = vk_deep_count(he4, f);
                if (!spa || !sn) continue;
                if (sn > kVkDeepMaxElems) { *ok = false; return 0; }
                uint64_t base = reinterpret_cast<uint64_t>(spa);
                total = (total + 7u) & ~size_t(7);
                total += (size_t)sn * 8;
                for (uint32_t k = 0; k < sn; k++) {
                    uint64_t sp2 = 0;
                    try { mem->read(base + (uint64_t)k * 8, &sp2, 8); }
                    catch (...) { continue; }
                    if (!sp2) continue;
                    total += vk_deep_guest_strlen(mem, sp2) + 1;
                }
                continue;
            }
            if (f.elem & thunk::VKM_STR) {
                uint8_t stackbuf3[512];
                const void* he3 =
                    vk_deep_read(mem, g, d->size, stackbuf3, sizeof(stackbuf3));
                if (!he3) { *ok = false; return 0; }
                uint64_t sp = reinterpret_cast<uint64_t>(
                    vk_deep_ptr_field(he3, f));
                if (!sp) continue;
                total = (total + 7u) & ~size_t(7);
                total += vk_deep_guest_strlen(mem, sp) + 1;
                continue;
            }
            uint8_t stackbuf[512];
            const void* host_elem =
                vk_deep_read(mem, g, d->size, stackbuf, sizeof(stackbuf));
            if (!host_elem) { *ok = false; return 0; }
            const void* p = vk_deep_ptr_field(host_elem, f);
            uint32_t n = vk_deep_count(host_elem, f);
            if (!p || !n) continue;
            if (f.elem == thunk::VKM_STRUCT && f.elem_struct >= 0) {
                total = (total + 7u) & ~size_t(7);
                total += vk_deep_size_one(
                    mem, reinterpret_cast<uint64_t>(p),
                    &thunk::kVkStructs[f.elem_struct], n, depth + 1, ok);
            } else {
                size_t bytes = (size_t)n * f.elem_size;
                if (bytes > kVkDeepMaxBytes) { *ok = false; return 0; }
                total = (total + 7u) & ~size_t(7);
                total += bytes;
            }
            if (!*ok) return 0;
        }
    }
    if (total > kVkDeepMaxBytes) { *ok = false; return 0; }
    return total;
}

// Fill one element of descriptor `d` from guest `guest` into host `out`
// (already staged). Nested arrays are allocated from the arena FIRST and
// re-pointed, then filled recursively.
void vk_deep_fill_elem(Memory* mem, VkStage& st, uint64_t guest,
                       const thunk::VkStructDesc* d, uint8_t* out,
                       int depth, bool skip_pnext) {
    try { mem->read(guest, out, d->size); }
    catch (...) { std::memset(out, 0, d->size); return; }
    for (uint16_t fi = 0; fi < d->nfields; fi++) {
        const thunk::VkFieldDesc& f = d->fields[fi];
        if (f.elem & thunk::VKM_PNEXT) {
            if (skip_pnext) continue;
            // chain link: walk the guest chain via the generated
            // sType→descriptor map (input-only — no node writeback)
            void** slot = reinterpret_cast<void**>(out + f.off);
            vk_deep_fill_chain(mem, st,
                               reinterpret_cast<uint64_t>(
                                   vk_deep_ptr_field(out, f)),
                               slot, depth + 1);
            continue;
        }
        if (f.elem & thunk::VKM_STRARR) {
            // array of strings: stage the slot array, then each string
            void** slot = reinterpret_cast<void**>(out + f.off);
            uint64_t arr = reinterpret_cast<uint64_t>(
                vk_deep_ptr_field(out, f));
            uint32_t sn = vk_deep_count(out, f);
            if (!arr || !sn || sn > kVkDeepMaxElems) {
                *slot = nullptr;
                continue;
            }
            char** slots = reinterpret_cast<char**>(st.bytes((size_t)sn * 8, 8));
            *slot = slots;
            for (uint32_t k = 0; k < sn; k++) {
                uint64_t sp2 = 0;
                try { mem->read(arr + (uint64_t)k * 8, &sp2, 8); }
                catch (...) { sp2 = 0; }
                if (!sp2) { slots[k] = nullptr; continue; }
                size_t n2 = vk_deep_guest_strlen(mem, sp2);
                char* dstp = reinterpret_cast<char*>(st.bytes(n2 + 1, 1));
                try { mem->read(sp2, dstp, n2 + 1); }
                catch (...) { std::memset(dstp, 0, n2 + 1); }
                dstp[n2] = 0;
                slots[k] = dstp;
            }
            continue;
        }
        if (f.elem & thunk::VKM_STR) {
            // NUL-terminated guest string: stage strlen+1 bytes
            void** slot = reinterpret_cast<void**>(out + f.off);
            uint64_t sp = reinterpret_cast<uint64_t>(
                vk_deep_ptr_field(out, f));
            if (!sp) { *slot = nullptr; continue; }
            size_t n = vk_deep_guest_strlen(mem, sp);
            char* dstp = reinterpret_cast<char*>(st.bytes(n + 1, 1));
            try { mem->read(sp, dstp, n + 1); }
            catch (...) { std::memset(dstp, 0, n + 1); }
            dstp[n] = 0;
            *slot = dstp;
            continue;
        }
        const void* p = vk_deep_ptr_field(out, f);
        uint32_t n = vk_deep_count(out, f);
        void** slot = reinterpret_cast<void**>(out + f.off);
        if (!p || !n) { *slot = nullptr; continue; }
        size_t bytes;
        const thunk::VkStructDesc* sub = nullptr;
        if (f.elem == thunk::VKM_STRUCT && f.elem_struct >= 0) {
            sub = &thunk::kVkStructs[f.elem_struct];
            bytes = (size_t)n * sub->size;
        } else {
            bytes = (size_t)n * f.elem_size;
        }
        void* dst = st.bytes(bytes, 8);
        if (sub) {
            *slot = dst;
            for (uint32_t k = 0; k < n; k++)
                vk_deep_fill_elem(mem, st,
                                  reinterpret_cast<uint64_t>(p) +
                                      (uint64_t)k * sub->size,
                                  sub,
                                  reinterpret_cast<uint8_t*>(dst) +
                                      (uint64_t)k * sub->size,
                                  depth + 1);
        } else {
            try { mem->read(reinterpret_cast<uint64_t>(p), dst, bytes); }
            catch (...) { std::memset(dst, 0, bytes); }
            *slot = dst;
        }
    }
}

}  // namespace arm64emu
