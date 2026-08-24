// frost_graphics/display_thunk.cpp — DisplayThunk implementation (1.5.4-alpha).
//
// See include/frost/display_thunk.hpp for the design overview. This file
// implements the DisplayThunk class for Vulkan / Wayland / X11 / GBM.
//
// 1.5.4-alpha: DisplayThunk now uses the same full dispatch logic as
// GraphicThunk (stack args, float args, string returns, GetProcAddress,
// mixed int+float) and integrates DisplayProxy for X11/Wayland fallback
// when host libraries are unavailable.
#include "frost/display_thunk.hpp"
#include "frost/thunk.hpp"  // for SYSCALL_NUMBER
#include "frost/display_proxy.hpp"
#include "frost/android_surface.hpp"
#include "thunk_common.hpp" // shared SymbolEntry (single definition — see header)
#include "opgen_thunk.hpp"  // 1.5.4-alpha: symbol signature table (single source of truth)
#include "opgen_vkmarshal.hpp" // VK_CMD_DEEP: generated deep-marshal descriptors
#include "debug_flags.h"    // dbg() — cached trace gates (BIFROST_THUNK_TRACE)
#include "core/cpu.h"
#include "core/memory.h"
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <cstring>
#include <unordered_map>
#include <vector>
namespace arm64emu {

struct DisplayThunkImpl {
    bool   enabled = false;
    Memory* mem    = nullptr;
    bool   initialized = false;
    uint64_t trampoline_base = 0;
    static constexpr uint64_t TRAMPOLINE_PAGE_SIZE =
        DisplayThunk::TRAMPOLINE_SIZE * DisplayThunk::MAX_SYMBOLS;  // 32 KiB
    struct LibTable {
        std::string lib;
        std::vector<SymbolEntry> entries;
    };
    std::vector<LibTable> libs_;
    std::vector<std::pair<uint32_t, uint32_t>> id_to_idx_;
    // DisplayProxy for X11/Wayland fallback (1.5.4-alpha).
    std::unique_ptr<DisplayProxy> proxy_;
    // Guest-visible scratch page for host→guest string returns
    // (XGetAtomName, glGetString, …). Ring-allocated.
    uint64_t string_cache_base = 0;
    static constexpr uint64_t STRING_CACHE_SIZE = 4096;
    uint32_t string_cache_off = 0;
    std::mutex mu;
    // ── vkMapMemory guest-window bounce bookkeeping (2026-08-21) ──────
    // The host mapping address is meaningless in the guest (48-bit host
    // heap, outside the 4 GiB direct window), so vkMapMemory allocates a
    // bounce inside the window and returns ITS guest address; the guest
    // reads/writes it at full JIT speed. Push (bounce→host) before every
    // GPU-consuming call (queue submit/present), pull (host→bounce) after
    // every completion wait — the practical HOST_COHERENT guarantee for
    // both coherent and non-coherent memory (over-pushing non-coherent
    // memory is harmless). Explicit flush/invalidate move their ranges.
    struct VkMapped {
        uint64_t host_ptr;    // host mapping base (a HOST address)
        uint64_t bounce;      // guest address of the window bounce
        uint64_t map_offset;  // offset passed to vkMapMemory
        uint64_t map_size;    // bytes mapped (resolved from VK_WHOLE_SIZE)
        uint64_t alloc_size;  // total VkDeviceMemory allocation size
    };
    std::unordered_map<uint64_t, VkMapped> vk_maps_;    // memory handle → map
    std::unordered_map<uint64_t, uint64_t> vk_allocs_;  // handle → alloc size
    std::mutex vk_maps_mu;
    // Push all bounces back into the host mappings (before submits/presents).
    void vk_sync_push_all() {
        std::lock_guard<std::mutex> g(vk_maps_mu);
        for (auto& kv : vk_maps_) {
            const VkMapped& m = kv.second;
            uint8_t* src = mem->guest_to_host_ptr(m.bounce);
            if (src && m.host_ptr && m.map_size)
                std::memcpy(reinterpret_cast<void*>(m.host_ptr), src, m.map_size);
        }
    }
    // Pull host mappings into the bounces (after completion waits — the
    // GPU may have written readback data into the host mapping).
    void vk_sync_pull_all() {
        std::lock_guard<std::mutex> g(vk_maps_mu);
        for (auto& kv : vk_maps_) {
            const VkMapped& m = kv.second;
            uint8_t* dst = mem->guest_to_host_ptr(m.bounce);
            if (dst && m.host_ptr && m.map_size)
                std::memcpy(dst, reinterpret_cast<const void*>(m.host_ptr), m.map_size);
        }
    }
    uint64_t cache_host_string_(const char* host_str) {
        if (!mem || !string_cache_base || !host_str) return 0;
        size_t len = std::strlen(host_str) + 1;
        if (len > STRING_CACHE_SIZE) len = STRING_CACHE_SIZE;
        if (string_cache_off + len > STRING_CACHE_SIZE)
            string_cache_off = 0;
        uint64_t guest = string_cache_base + string_cache_off;
        mem->write(guest, host_str, len);
        string_cache_off = static_cast<uint32_t>(
            (string_cache_off + len + 7u) & ~7u);
        return guest;
    }
};
DisplayThunk::DisplayThunk() {
    impl_ = std::make_unique<DisplayThunkImpl>();
    // 1.5.4-alpha: display thunking enabled by default.
    // Set BIFROST_NO_THUNK_DISPLAY=1 to disable.
    const char* disable = getenv("BIFROST_NO_THUNK_DISPLAY");
    impl_->enabled = !(disable && disable[0] != '0');
    if (impl_->enabled) {
        if (dbg().thunk_trace || getenv("BIFROST_VERBOSE")) {
            fprintf(stderr, "[display-thunk] display API thunking enabled "
                    "(Vulkan/Wayland/X11/GBM → host, with fallback)\n");
        }
    }
}
DisplayThunk::~DisplayThunk() = default;
bool DisplayThunk::enabled() const { return impl_ && impl_->enabled; }
bool DisplayThunk::init(Memory& mem) {
    if (!impl_->enabled) return false;
    if (impl_->initialized) return true;
    std::lock_guard<std::mutex> g(impl_->mu);
    impl_->mem = &mem;
    impl_->trampoline_base = mem.mmap_alloc(DisplayThunkImpl::TRAMPOLINE_PAGE_SIZE);
    if (impl_->trampoline_base == 0) {
        fprintf(stderr, "[display-thunk] init: failed to allocate trampoline page\n");
        return false;
    }
    impl_->string_cache_base = mem.mmap_alloc(DisplayThunkImpl::STRING_CACHE_SIZE);
    if (impl_->string_cache_base == 0) {
        fprintf(stderr, "[display-thunk] init: failed to allocate string cache\n");
        return false;
    }
    impl_->string_cache_off = 0;
    // 1.5.4-alpha: lazily create the DisplayProxy. It owns an SDL2 window
    // and provides a software fallback for X11/Wayland calls when the host
    // libraries are unavailable or have no display.
    impl_->proxy_ = std::make_unique<DisplayProxy>();
    impl_->proxy_->set_memory(&mem);
    register_known_symbols_();
    impl_->initialized = true;
    if (dbg().thunk_trace) {
        fprintf(stderr, "[display-thunk] init: %zu symbols registered, "
                "trampoline_base=0x%llx\n",
                impl_->id_to_idx_.size(),
                static_cast<unsigned long long>(impl_->trampoline_base));
    }
    return true;
}
DisplayProxy* DisplayThunk::proxy() {
    if (!impl_ || !impl_->initialized) return nullptr;
    return impl_->proxy_.get();
}
void* DisplayThunk::ensure_android_window() {
    if (!impl_ || !impl_->mem) return nullptr;
    if (!impl_->proxy_) {
        impl_->proxy_ = std::make_unique<DisplayProxy>();
        impl_->proxy_->set_memory(impl_->mem);
    }
    if (!impl_->proxy_->ready()) impl_->proxy_->init(800, 600, impl_->mem);
    if (!impl_->proxy_->ready()) return nullptr;
    auto& mgr = frost::AndroidSurfaceManager::instance();
    mgr.set_memory(impl_->mem);
    mgr.set_host_sdl_window(impl_->proxy_->host_window());
    mgr.host_native_window();
    return impl_->proxy_->host_window();
}
void DisplayThunk::register_function_(const std::string& lib,
                                        const std::string& sym,
                                        void* host_fn,
                                        uint16_t pointer_args,
                                        uint8_t n_stack,
                                        uint8_t n_float,
                                        uint8_t flags,
                                        const thunk::Spec* spec) {
    bool trace = dbg().thunk_trace;
    // Find or create the LibTable for `lib`.
    DisplayThunkImpl::LibTable* lt = nullptr;
    for (auto& l : impl_->libs_) {
        if (l.lib == lib) { lt = &l; break; }
    }
    if (!lt) {
        impl_->libs_.push_back({lib, {}});
        lt = &impl_->libs_.back();
    }
    // Idempotent: skip if already registered.
    for (const auto& e : lt->entries) {
        if (e.name == sym) return;
    }
    uint32_t local_id = static_cast<uint32_t>(impl_->id_to_idx_.size());
    if (local_id >= DisplayThunk::MAX_SYMBOLS) {
        fprintf(stderr, "[display-thunk] register: symbol table full (%zu)\n",
                impl_->id_to_idx_.size());
        return;
    }
    uint32_t sym_id = DisplayThunk::ID_BASE + local_id;
    uint64_t addr = impl_->trampoline_base + local_id * DisplayThunk::TRAMPOLINE_SIZE;
    // Write the trampoline via the shared helper: movz x9, #sym_id;
    // movz x8, #SYSCALL; svc #0; ret. (Hand-rolled encodings here
    // previously dropped the Rd field and loaded sym_id into x0, which
    // the dispatcher reads from x9 — every display call misrouted.)
    write_thunk_trampoline(*impl_->mem, addr, sym_id,
                           static_cast<uint16_t>(DisplayThunk::SYSCALL_NUMBER));
    lt->entries.push_back({sym, host_fn, addr, sym_id, pointer_args,
                           n_stack, n_float, flags, spec});
    impl_->id_to_idx_.push_back({
        static_cast<uint32_t>(std::distance(impl_->libs_.data(), lt)),
        static_cast<uint32_t>(lt->entries.size() - 1)
    });
    if (trace) {
        fprintf(stderr, "[display-thunk] registered %s:%s -> 0x%llx "
                "(id=%u ptrs=0x%x stack=%u fp=%u flags=0x%x)\n",
                lib.c_str(), sym.c_str(),
                static_cast<unsigned long long>(addr), sym_id,
                pointer_args, n_stack, n_float, flags);
    }
}
void DisplayThunk::write_trampoline_(Memory& mem, uint64_t addr, uint32_t sym_id) {
    write_thunk_trampoline(mem, addr, sym_id,
                           static_cast<uint16_t>(DisplayThunk::SYSCALL_NUMBER));
}
uint64_t DisplayThunk::resolve(const std::string& lib, const std::string& sym) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return 0;
    std::lock_guard<std::mutex> g(impl_->mu);
    for (auto& l : impl_->libs_) {
        if (l.lib == lib) {
            for (const auto& e : l.entries) {
                if (e.name == sym) return e.guest_addr;
            }
            return 0;
        }
    }
    return 0;
}
size_t DisplayThunk::enumerate_symbols(const std::string& lib,
    const std::function<void(const std::string&, uint64_t)>& cb) const {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return 0;
    std::lock_guard<std::mutex> g(impl_->mu);
    for (auto& l : impl_->libs_) {
        if (l.lib == lib) {
            for (const auto& e : l.entries) {
                cb(e.name, e.guest_addr);
            }
            return l.entries.size();
        }
    }
    return 0;
}
struct VkStage {
    std::vector<uint8_t> buf;
    // Reserve once up front: `alloc`/`bytes`/`guest_str*` hand out pointers
    // into `buf.data()`, and `resize` would REALLOCATE (dangling every
    // earlier pointer) once a later string/array grows the buffer. Every
    // size below is capped (guest_str 511 chars, string arrays/queue
    // arrays <= 1024/16 entries), so total staging stays well under this.
    explicit VkStage() { buf.reserve(65536); }
    size_t put(size_t sz, size_t align) {
        size_t off = (buf.size() + align - 1u) & ~(align - 1u);
        buf.resize(off + sz);
        return off;
    }
    template <typename T> T* alloc() {
        return reinterpret_cast<T*>(buf.data() + put(sizeof(T), alignof(T)));
    }
    void* bytes(size_t sz, size_t align) {
        return buf.data() + put(sz, align);
    }
    // Copy a guest string into staging. Returns the host pointer (or
    // nullptr when the guest pointer is 0). Short strings only — a cap of
    // 511 chars is fine for extension / app names.
    const char* guest_str(Memory* mem, uint64_t g) {
        if (!g) return nullptr;
        char tmp[512];
        size_t n = 0;
        try {
            while (n + 1 < sizeof(tmp)) {
                uint8_t c = 0;
                mem->read(g + n, &c, 1);
                tmp[n++] = static_cast<char>(c);
                if (c == 0) break;
            }
        } catch (...) { /* unmapped — truncate */ }
        tmp[n] = 0;
        size_t off = put(n + 1, 1);
        std::memcpy(buf.data() + off, tmp, n + 1);
        return reinterpret_cast<const char*>(buf.data() + off);
    }
    // Copy a guest array of `count` char* into staging, re-pointing each
    // string into the staging buffer. Returns the host array pointer (or
    // nullptr when the guest array / count is 0).
    const char** guest_str_array(Memory* mem, uint64_t arr, uint32_t count) {
        if (!arr || count == 0 || count > 1024) return nullptr;
        const char** out = reinterpret_cast<const char**>(
            bytes(static_cast<size_t>(count) * sizeof(const char*), 8));
        for (uint32_t i = 0; i < count; i++) {
            uint64_t p = 0;
            try { mem->read(arr + static_cast<uint64_t>(i) * 8u, &p, 8); }
            catch (...) { p = 0; }
            out[i] = guest_str(mem, p);
        }
        return out;
    }
};

// ── VK_CMD_DEEP: descriptor-driven deep marshal for command batches ──
// Two passes over the guest structures: a dry size pass (so VkStage can
// reserve exactly once — its buffer reallocates past the initial 64 KiB,
// which would dangle every earlier pointer), then a fill pass. All
// covered commands are INPUT-ONLY (command recording), so there is no
// writeback. Defensive caps mirror the hand arms (≤1024 elements).
namespace {
constexpr size_t kVkDeepMaxElems = 1024;
constexpr size_t kVkDeepMaxBytes = 4u << 20;   // 4 MiB staging cap
constexpr int kVkDeepMaxPnextNodes = 8;        // chain-node cap (hand-arm parity)

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

// Dry-size walk of a guest pNext chain (must mirror vk_deep_fill_chain
// allocation-for-allocation — the two-pass staging contract).
size_t vk_deep_chain_size(Memory* mem, uint64_t head, bool* ok) {
    size_t total = 0;
    int guard = 0;
    while (head && guard++ < kVkDeepMaxPnextNodes) {
        int32_t s = 0;
        uint64_t pn = 0;
        try { mem->read(head, &s, 4); mem->read(head + 8, &pn, 8); }
        catch (...) { break; }
        const thunk::VkStructDesc* d = thunk::vk_find_struct_by_stype(s);
        if (!d) { vk_deep_unknown_stype_once(s); break; }
        total = (total + 7u) & ~size_t(7);
        total += d->size;
        if (total > kVkDeepMaxBytes) { *ok = false; return 0; }
        head = pn;
    }
    return total;
}

// Fill pass: stage every known chain node and relink host-side. *slot
// receives the HOST head (nullptr when nothing was staged).
void vk_deep_fill_chain(Memory* mem, VkStage& st, uint64_t head,
                        void** slot) {
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
        try { mem->read(head, h, d->size); }
        catch (...) { std::memset(h, 0, d->size); }
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
                        int depth, bool* ok) {
    if (!ok || depth > 4 || count > kVkDeepMaxElems) { *ok = false; return 0; }
    size_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t g = guest + (uint64_t)i * d->size;
        total = (total + 7u) & ~size_t(7);
        total += d->size;
        if (total > kVkDeepMaxBytes) { *ok = false; return 0; }
        for (uint16_t fi = 0; fi < d->nfields; fi++) {
            const thunk::VkFieldDesc& f = d->fields[fi];
            if (f.elem & thunk::VKM_PNEXT) {
                // chain link: account for every known node's staging
                uint8_t stackbuf2[512];
                const void* he2 =
                    vk_deep_read(mem, g, d->size, stackbuf2, sizeof(stackbuf2));
                if (!he2) { *ok = false; return 0; }
                uint64_t head =
                    reinterpret_cast<uint64_t>(vk_deep_ptr_field(he2, f));
                total = (total + 7u) & ~size_t(7);
                total += vk_deep_chain_size(mem, head, ok);
                if (!*ok) return 0;
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
    return total;
}

// Fill one element of descriptor `d` from guest `guest` into host `out`
// (already staged). Nested arrays are allocated from the arena FIRST and
// re-pointed, then filled recursively.
void vk_deep_fill_elem(Memory* mem, VkStage& st, uint64_t guest,
                       const thunk::VkStructDesc* d, uint8_t* out,
                       int depth) {
    try { mem->read(guest, out, d->size); }
    catch (...) { std::memset(out, 0, d->size); return; }
    for (uint16_t fi = 0; fi < d->nfields; fi++) {
        const thunk::VkFieldDesc& f = d->fields[fi];
        if (f.elem & thunk::VKM_PNEXT) {
            // chain link: walk the guest chain via the generated
            // sType→descriptor map (input-only — no node writeback)
            void** slot = reinterpret_cast<void**>(out + f.off);
            vk_deep_fill_chain(mem, st,
                               reinterpret_cast<uint64_t>(
                                   vk_deep_ptr_field(out, f)),
                               slot);
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
} // namespace

int64_t DisplayThunk::dispatch(CPU& cpu, uint32_t symbol_id) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) {
        return -ENOSYS;
    }
    if ((symbol_id & DisplayThunk::ID_MASK) != DisplayThunk::ID_BASE) {
        return -ENOENT;
    }
    uint32_t local_id = symbol_id - DisplayThunk::ID_BASE;
    if (local_id >= impl_->id_to_idx_.size()) {
        return -ENOENT;
    }
    auto [lib_idx, ent_idx] = impl_->id_to_idx_[local_id];
    const auto& entry = impl_->libs_[lib_idx].entries[ent_idx];
    bool trace = dbg().thunk_trace;

    

// ── Proxy dispatch: route X11/Wayland calls to DisplayProxy ──────
    // When the THUNK_PROXY flag is set, the symbol is handled by the
    // DisplayProxy (SDL2-based software fallback). The proxy is preferred
    // over the host library: its handles (Display*, Window, GC) are guest
    // addresses, so they round-trip through guest memory correctly. The
    // host-lib path returns a HOST Display* which cannot be translated
    // back through guest memory — marking it as a pointer arg bounces it
    // and crashes. The host library is only a fallback when no SDL proxy
    // can be initialized (headless host).
    if (entry.flags & THUNK_PROXY) {
        if (impl_->proxy_) {
            if (!impl_->proxy_->ready()) {
                impl_->proxy_->init(640, 480, impl_->mem);
            }
            if (impl_->proxy_->ready()) {
                return proxy_dispatch_(cpu, entry.name);
            }
        }
        if (!entry.host_fn) {
            // No host function and no proxy — return 0 (NULL).
            cpu.regs[0] = 0;
            return 0;
        }
        // Proxy unavailable (headless) — fall through to host dispatch.
    }

    // ── Android NativeActivity surface layer (ANDROID_WINDOW) ────────
    // v1: ANativeWindow shims backed by the DisplayProxy host SDL window
    // (the window argument is a GUEST shim handle — raw integer, never
    // translated). fromSurface lazily initializes the proxy so a pure
    // Android app still gets a host window.
    // v2 (2026-08): the framework plumbing
    // (ALooper/AInputQueue/event getters/AConfiguration/liblog stubs) is
    // dispatched here too and must NOT require the proxy — glue code
    // calls AConfiguration_* before any window exists.
    if (entry.flags & THUNK_ANDROID_WINDOW) {
        auto& mgr = frost::AndroidSurfaceManager::instance();
        if (impl_->mem) mgr.set_memory(impl_->mem);
        const std::string& n = entry.name;

        auto set_f32 = [&](float f) {
            std::memcpy(&cpu.v_lo[0], &f, sizeof(float));
            cpu.v_hi[0] = 0;
            cpu.regs[0] = 0;
        };
        static constexpr uint64_t kFakeConfig = 0xA90004000005ULL;
        // Guest NUL-string reader (up to 1 KiB; tolerant of unmapped).
        auto guest_str = [&](uint64_t addr) -> std::string {
            if (!addr || !impl_->mem) return {};
            std::string s;
            char tmp[64];
            for (size_t off = 0; off < 1024;) {
                size_t want = std::min(sizeof(tmp), size_t(1024 - off));
                try {
                    impl_->mem->read(addr + off, tmp, want);
                } catch (...) {
                    break;
                }
                size_t i = 0;
                for (; i < want; i++) {
                    if (tmp[i] == '\0') {
                        s.append(tmp, i);
                        return s;
                    }
                }
                s.append(tmp, want);
                off += want;
            }
            return s;
        };

        // ── Framework plumbing: no host window required ───────────────
        if (n.rfind("ALooper_", 0) == 0) {
            if (n == "ALooper_prepare") {
                cpu.regs[0] = mgr.looper_handle();
            } else if (n == "ALooper_acquire" || n == "ALooper_release") {
                cpu.regs[0] = 0;
            } else if (n == "ALooper_pollOnce" || n == "ALooper_pollAll") {
                int timeout = static_cast<int>(static_cast<int32_t>(cpu.regs[0]));
                int ofd = -1, oev = 0;
                void* odata = nullptr;
                // pollAll drains callback-mode registrations internally;
                // pollOnce fires ONE callback and reports POLL_CALLBACK (-2).
                int ident = mgr.looper_poll(cpu, timeout, &ofd, &oev, &odata,
                                            n == "ALooper_pollAll");
                if (cpu.regs[1] && impl_->mem) {
                    int32_t v = ofd;
                    impl_->mem->write(cpu.regs[1], &v, sizeof(v));
                }
                if (cpu.regs[2] && impl_->mem) {
                    int32_t v = oev;
                    impl_->mem->write(cpu.regs[2], &v, sizeof(v));
                }
                if (cpu.regs[3] && impl_->mem) {
                    uint64_t v = reinterpret_cast<uintptr_t>(odata);
                    impl_->mem->write(cpu.regs[3], &v, sizeof(v));
                }
                cpu.regs[0] = static_cast<uint64_t>(
                    static_cast<int64_t>(ident));
            } else if (n == "ALooper_addFd") {
                cpu.regs[0] = static_cast<uint64_t>(mgr.looper_add_fd(
                    cpu.regs[0], static_cast<int>(cpu.regs[1]),
                    static_cast<int>(cpu.regs[2]),
                    static_cast<int>(cpu.regs[3]), cpu.regs[4],
                    reinterpret_cast<void*>(cpu.regs[5])));
            } else if (n == "ALooper_removeFd") {
                cpu.regs[0] = static_cast<uint64_t>(mgr.looper_remove_fd(
                    cpu.regs[0], static_cast<int>(cpu.regs[1])));
            } else if (n == "ALooper_wake") {
                mgr.looper_wake(cpu.regs[0]);
                cpu.regs[0] = 0;
            } else {
                cpu.regs[0] = static_cast<uint64_t>(-22);
            }
            return 0;
        }
        if (n.rfind("AInputQueue_", 0) == 0) {
            if (n == "AInputQueue_attachLooper") {
                mgr.queue_attach_looper(cpu.regs[0], cpu.regs[1],
                                        static_cast<int>(cpu.regs[2]),
                                        cpu.regs[3],
                                        reinterpret_cast<void*>(cpu.regs[4]));
                cpu.regs[0] = 0;
            } else if (n == "AInputQueue_detachLooper") {
                mgr.queue_detach_looper(cpu.regs[0]);
                cpu.regs[0] = 0;
            } else if (n == "AInputQueue_getEvent") {
                uint64_t evh = 0;
                int rc = mgr.queue_get_event(cpu.regs[0], &evh);
                if (rc == 0 && cpu.regs[1] && impl_->mem) {
                    impl_->mem->write(cpu.regs[1], &evh, sizeof(evh));
                }
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(rc));
            } else if (n == "AInputQueue_preDispatchEvent") {
                cpu.regs[0] = static_cast<uint64_t>(
                    mgr.queue_pre_dispatch(cpu.regs[0], cpu.regs[1]));
            } else if (n == "AInputQueue_finishEvent") {
                mgr.queue_finish_event(cpu.regs[0], cpu.regs[1]);
                cpu.regs[0] = 0;
            } else {
                cpu.regs[0] = static_cast<uint64_t>(-22);
            }
            return 0;
        }
        if (n.rfind("AInputEvent_", 0) == 0) {
            if (n == "AInputEvent_getType")
                cpu.regs[0] = static_cast<uint64_t>(mgr.event_type(cpu.regs[0]));
            else if (n == "AInputEvent_getDeviceId")
                cpu.regs[0] = static_cast<uint64_t>(mgr.event_device_id(cpu.regs[0]));
            else if (n == "AInputEvent_getSource")
                cpu.regs[0] = static_cast<uint64_t>(mgr.event_source(cpu.regs[0]));
            else
                cpu.regs[0] = 0;
            return 0;
        }
        if (n.rfind("AMotionEvent_", 0) == 0) {
            if (n == "AMotionEvent_getAction")
                cpu.regs[0] = static_cast<uint64_t>(mgr.motion_action(cpu.regs[0]));
            else if (n == "AMotionEvent_getPointerCount")
                cpu.regs[0] = static_cast<uint64_t>(mgr.motion_pointer_count(cpu.regs[0]));
            else if (n == "AMotionEvent_getDownTime")
                cpu.regs[0] = static_cast<uint64_t>(
                    static_cast<int64_t>(mgr.motion_down_time(cpu.regs[0])));
            else if (n == "AMotionEvent_getEventTime")
                cpu.regs[0] = static_cast<uint64_t>(
                    static_cast<int64_t>(mgr.motion_event_time(cpu.regs[0])));
            else if (n == "AMotionEvent_getEdgeFlags")
                cpu.regs[0] = 0;
            else if (n == "AMotionEvent_getXPrecision" || n == "AMotionEvent_getYPrecision")
                set_f32(1.0f);
            else if (n == "AMotionEvent_getHistorySize")
                cpu.regs[0] = 0;
            else if (n == "AMotionEvent_getPointerId")
                cpu.regs[0] = static_cast<uint64_t>(mgr.motion_pointer_id(
                    cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getX")
                set_f32(mgr.motion_x(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getY")
                set_f32(mgr.motion_y(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getRawX")
                set_f32(mgr.motion_x(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getRawY")
                set_f32(mgr.motion_y(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getPressure")
                set_f32(mgr.motion_pressure(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getSize")
                set_f32(mgr.motion_size(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getTouchMajor")
                set_f32(mgr.motion_touch_major(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getTouchMinor")
                set_f32(mgr.motion_touch_minor(cpu.regs[0], static_cast<size_t>(cpu.regs[1])));
            else if (n == "AMotionEvent_getAxisValue")
                set_f32(mgr.motion_axis_value(cpu.regs[0],
                                              static_cast<int>(cpu.regs[1]),
                                              static_cast<size_t>(cpu.regs[2])));
            else
                cpu.regs[0] = 0;
            return 0;
        }
        if (n.rfind("AKeyEvent_", 0) == 0) {
            if (n == "AKeyEvent_getAction")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_action(cpu.regs[0]));
            else if (n == "AKeyEvent_getKeyCode")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_code(cpu.regs[0]));
            else if (n == "AKeyEvent_getMetaState")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_meta_state(cpu.regs[0]));
            else if (n == "AKeyEvent_getRepeatCount")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_repeat_count(cpu.regs[0]));
            else if (n == "AKeyEvent_getScanCode")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_scan_code(cpu.regs[0]));
            else if (n == "AKeyEvent_getFlags")
                cpu.regs[0] = static_cast<uint64_t>(mgr.key_flags(cpu.regs[0]));
            else if (n == "AKeyEvent_getDownTime")
                cpu.regs[0] = static_cast<uint64_t>(
                    static_cast<int64_t>(mgr.key_down_time(cpu.regs[0])));
            else if (n == "AKeyEvent_getEventTime")
                cpu.regs[0] = static_cast<uint64_t>(
                    static_cast<int64_t>(mgr.key_event_time(cpu.regs[0])));
            else
                cpu.regs[0] = 0;
            return 0;
        }
        if (n.rfind("AConfiguration_", 0) == 0) {
            if (n == "AConfiguration_new") {
                cpu.regs[0] = kFakeConfig;
            } else if (n == "AConfiguration_getLanguage") {
                if (impl_->mem && cpu.regs[1]) {
                    const char s[] = "en";
                    impl_->mem->write(cpu.regs[1], s, sizeof(s));
                }
                cpu.regs[0] = 0;
            } else if (n == "AConfiguration_getCountry") {
                if (impl_->mem && cpu.regs[1]) {
                    const char s[] = "US";
                    impl_->mem->write(cpu.regs[1], s, sizeof(s));
                }
                cpu.regs[0] = 0;
            } else if (n == "AConfiguration_getDensity") {
                cpu.regs[0] = 160;
            } else if (n == "AConfiguration_getSdkVersion") {
                cpu.regs[0] = 34;
            } else if (n == "AConfiguration_getOrientation") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getTouchscreen") {
                cpu.regs[0] = 3;
            } else if (n == "AConfiguration_getKeyboard") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getNavigation") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getKeysHidden") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getNavHidden") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getScreenSize") {
                cpu.regs[0] = 2;
            } else if (n == "AConfiguration_getScreenLong") {
                cpu.regs[0] = 0;
            } else if (n == "AConfiguration_getUiModeType") {
                cpu.regs[0] = 1;
            } else if (n == "AConfiguration_getUiModeNight") {
                cpu.regs[0] = 0x10;
            } else if (n == "AConfiguration_getMcc" || n == "AConfiguration_getMnc") {
                cpu.regs[0] = 0;
            } else {
                // delete / fromAssetManager / setTo / diff — no-op
                cpu.regs[0] = 0;
            }
            return 0;
        }
        if (n.rfind("__android_log", 0) == 0) {
            std::string tag = guest_str(cpu.regs[1]);
            if (n == "__android_log_write" || n == "__android_log_buf_write") {
                std::string text = guest_str(cpu.regs[2]);
                std::fprintf(stderr, "[android-log] %s: %s\n",
                             tag.c_str(), text.c_str());
                cpu.regs[0] = 0;
                return 0;
            }
            // __android_log_print / __android_log_buf_print: varargs fmt
            std::string fmt = guest_str(cpu.regs[2]);
            uint64_t slots[12] = {0};
            size_t nslots = 0;
            for (int r = 3; r < 8 && nslots < 12; r++)
                slots[nslots++] = cpu.regs[r];
            for (size_t i = 0; i < 7 && nslots < 12; i++) {
                uint64_t v = 0;
                try { impl_->mem->read(cpu.sp + i * 8, &v, sizeof(v)); }
                catch (...) { break; }
                slots[nslots++] = v;
            }
            std::string out;
            out.reserve(512);
            size_t si = 0;
            for (size_t i = 0; i < fmt.size() && out.size() < 1024; i++) {
                char c = fmt[i];
                if (c != '%') { out.push_back(c); continue; }
                if (i + 1 >= fmt.size()) break;
                size_t j = i + 1;
                while (j < fmt.size() && (fmt[j]=='-'||fmt[j]=='+'||fmt[j]==' '||fmt[j]=='#'||(fmt[j]>='0'&&fmt[j]<='9')||fmt[j]=='.')) j++;
                std::string lm;
                while (j < fmt.size() && (fmt[j]=='l'||fmt[j]=='z'||fmt[j]=='h'||fmt[j]=='j'||fmt[j]=='t')) { lm.push_back(fmt[j]); j++; }
                if (j >= fmt.size()) break;
                char conv = fmt[j];
                i = j;
                if (conv == '%') { out.push_back('%'); continue; }
                uint64_t iv = si < nslots ? slots[si++] : 0;
                char tmp[64];
                switch (conv) {
                case 's': {
                    std::string s = guest_str(iv);
                    out += s.empty() ? std::string("(null)") : s;
                    break;
                }
                case 'd': case 'i': {
                    if (lm == "ll" || lm == "j")
                        std::snprintf(tmp, sizeof(tmp), "%lld", (long long)iv);
                    else if (lm == "l")
                        std::snprintf(tmp, sizeof(tmp), "%ld", (long)iv);
                    else if (lm == "z")
                        std::snprintf(tmp, sizeof(tmp), "%zd", (ssize_t)iv);
                    else
                        std::snprintf(tmp, sizeof(tmp), "%d", (int)(int32_t)iv);
                    out += tmp;
                    break;
                }
                case 'u': {
                    if (lm == "ll" || lm == "l")
                        std::snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)iv);
                    else
                        std::snprintf(tmp, sizeof(tmp), "%u", (unsigned)iv);
                    out += tmp;
                    break;
                }
                case 'x': case 'X': {
                    std::snprintf(tmp, sizeof(tmp), conv=='x'?"%llx":"%llX",
                                  (unsigned long long)iv);
                    out += tmp;
                    break;
                }
                case 'o':
                    std::snprintf(tmp, sizeof(tmp), "%llo", (unsigned long long)iv);
                    out += tmp;
                    break;
                case 'p':
                    std::snprintf(tmp, sizeof(tmp), "%p",
                                  reinterpret_cast<void*>(uintptr_t(iv)));
                    out += tmp;
                    break;
                case 'c': {
                    out.push_back(char(iv & 0xFF));
                    break;
                }
                case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
                    double d = 0;
                    if (conv == 'f' || conv == 'F') {
                        // Varargs double promotion: 64-bit pattern in integer slot.
                        std::memcpy(&d, &iv, sizeof(d));
                    } else {
                        std::memcpy(&d, &iv, sizeof(d));
                    }
                    std::snprintf(tmp, sizeof(tmp), "%g", d);
                    out += tmp;
                    break;
                }
                default:
                    out.push_back('%');
                    out.push_back(conv);
                    break;
                }
            }
            std::fprintf(stderr, "[android-log] %s: %s\n",
                         tag.c_str(), out.c_str());
            cpu.regs[0] = static_cast<uint64_t>(out.size());
            return 0;
        }

        // ── ANativeWindow shims require the host window ───────────────
        if (!impl_->proxy_) {
            impl_->proxy_ = std::make_unique<DisplayProxy>();
            impl_->proxy_->set_memory(impl_->mem);
        }
        if (!impl_->proxy_->ready()) {
            impl_->proxy_->init(800, 600, impl_->mem);
        }
        if (impl_->proxy_->ready()) {
            mgr.set_host_sdl_window(impl_->proxy_->host_window());
            // Eagerly resolve the host native window + wl_display NOW so
            // the guest's later eglGetDisplay(EGL_DEFAULT_DISPLAY)
            // substitution (same wl_display connection) is armed before
            // any EGL call.
            mgr.host_native_window();
        }
        if (n == "ANativeWindow_fromSurface") {
            // (JNIEnv*, jobject) — both opaque; v1 is a single-surface
            // singleton. Returns 0 (NULL) when no host window exists,
            // matching Android's behavior without a valid surface.
            cpu.regs[0] = mgr.from_surface();
            if (trace) fprintf(stderr, "[android] fromSurface -> 0x%llx\n",
                               static_cast<unsigned long long>(cpu.regs[0]));
            return 0;
        }
        if (!frost::AndroidSurfaceManager::is_shim(cpu.regs[0])) {
            // Unknown/foreign handle — mirror Android's EINVAL-ish 0xBAD.../
            // -EINVAL convention loosely: return -EINVAL.
            cpu.regs[0] = static_cast<uint64_t>(-22);
            return 0;
        }
        if (n == "ANativeWindow_acquire") {
            mgr.acquire(cpu.regs[0]);
            cpu.regs[0] = 0;
        } else if (n == "ANativeWindow_release" || n == "ANativeWindow_free") {
            mgr.release(cpu.regs[0]);
            cpu.regs[0] = 0;
        } else if (n == "ANativeWindow_getWidth") {
            cpu.regs[0] = static_cast<uint64_t>(
                static_cast<int64_t>(mgr.width()));
        } else if (n == "ANativeWindow_getHeight") {
            cpu.regs[0] = static_cast<uint64_t>(
                static_cast<int64_t>(mgr.height()));
        } else if (n == "ANativeWindow_getFormat") {
            cpu.regs[0] = static_cast<uint64_t>(
                static_cast<int64_t>(mgr.format()));
        } else if (n == "ANativeWindow_setBuffersGeometry") {
            // (window, width, height, format); 0 keeps the current value.
            cpu.regs[0] = static_cast<uint64_t>(mgr.set_buffers_geometry(
                static_cast<int32_t>(cpu.regs[1]),
                static_cast<int32_t>(cpu.regs[2]),
                static_cast<int32_t>(cpu.regs[3])));
        } else {
            cpu.regs[0] = 0;
        }
        return 0;
    }

    // ── Vulkan marshalling path ──────────────────────────────────────
    // vkCreateInstance/vkCreateDevice carry nested guest pointers (string
    // arrays + struct arrays) that the generic bounce can't fix, and the
    // proc-addr functions read the pName string from arg 1. Everything
    // else falls through to the generic path with the (corrected) pointer
    // masks — opaque handles pass verbatim, out pointers bounce back.
    if (entry.flags & THUNK_VULKAN) {
        if (vk_dispatch_(cpu, entry, trace)) return 0;
    }

    // ── Double-only AAPCS64 path (glOrtho, glClearDepth, …) ──────────
    if (entry.flags & DisplayThunk::THUNK_DOUBLE) {
        double dv[8] = {0};
        for (uint8_t i = 0; i < entry.n_float && i < 8; i++) {
            std::memcpy(&dv[i], &cpu.v_lo[i], sizeof(double));
        }
        if (trace) {
            fprintf(stderr, "[display-thunk] dispatch: %s (double×%u) d0=%g d1=%g d2=%g d3=%g\n",
                    entry.name.c_str(), entry.n_float,
                    dv[0], dv[1], dv[2], dv[3]);
        }
        switch (entry.n_float) {
        case 1: { using Fn = void (*)(double); reinterpret_cast<Fn>(entry.host_fn)(dv[0]); break; }
        case 2: { using Fn = void (*)(double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1]); break; }
        case 3: { using Fn = void (*)(double, double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2]); break; }
        case 4: { using Fn = void (*)(double, double, double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3]); break; }
        case 5: { using Fn = void (*)(double, double, double, double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3], dv[4]); break; }
        case 6: { using Fn = void (*)(double, double, double, double, double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3], dv[4], dv[5]); break; }
        default: { using Fn = void (*)(double, double, double, double, double, double, double, double); reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3], dv[4], dv[5], dv[6], dv[7]); break; }
        }
        cpu.regs[0] = 0;
        return 0;
    }

    // ── Float-only AAPCS64 path (Vulkan float params, etc.) ──────────
    if (entry.n_float > 0 && !(entry.flags & THUNK_MIXED_FP)
        && !(entry.flags & THUNK_GET_PROC)) {
        float fv[8] = {0};
        for (uint8_t i = 0; i < entry.n_float && i < 8; i++) {
            std::memcpy(&fv[i], &cpu.v_lo[i], sizeof(float));
        }
        if (trace) {
            fprintf(stderr, "[display-thunk] dispatch: %s (fp×%u) f0=%g f1=%g f2=%g f3=%g\n",
                    entry.name.c_str(), entry.n_float,
                    fv[0], fv[1], fv[2], fv[3]);
            if (entry.n_float > 4) {
                fprintf(stderr, "[display-thunk] dispatch: %s fp×%u — only first 4 floats forwarded\n",
                        entry.name.c_str(), entry.n_float);
            }
        }
        switch (entry.n_float) {
        case 1: { using Fn = void (*)(float); reinterpret_cast<Fn>(entry.host_fn)(fv[0]); break; }
        case 2: { using Fn = void (*)(float, float); reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1]); break; }
        case 3: { using Fn = void (*)(float, float, float); reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1], fv[2]); break; }
        default: { using Fn = void (*)(float, float, float, float); reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1], fv[2], fv[3]); break; }
        }
        cpu.regs[0] = 0;
        return 0;
    }

    // ── Mixed int + float (Vulkan mixed params) ──────────────────────
    if (entry.flags & THUNK_MIXED_FP) {
        uint64_t iv[4] = {0};
        float fv[4] = {0};
        uint8_t ni = entry.n_stack;
        if (ni > 4) ni = 4;
        for (uint8_t i = 0; i < ni; i++) iv[i] = cpu.regs[i];
        for (uint8_t i = 0; i < entry.n_float && i < 4; i++) {
            std::memcpy(&fv[i], &cpu.v_lo[i], sizeof(float));
        }
        if (trace) {
            fprintf(stderr, "[display-thunk] dispatch: %s (mixed int×%u fp×%u) i0=%lld f0=%g\n",
                    entry.name.c_str(), ni, entry.n_float,
                    static_cast<long long>(iv[0]), fv[0]);
        }
        if (ni == 1 && entry.n_float == 1) {
            using Fn = void (*)(int32_t, float); reinterpret_cast<Fn>(entry.host_fn)(static_cast<int32_t>(iv[0]), fv[0]);
        } else if (ni == 1 && entry.n_float == 2) {
            using Fn = void (*)(int32_t, float, float); reinterpret_cast<Fn>(entry.host_fn)(static_cast<int32_t>(iv[0]), fv[0], fv[1]);
        } else if (ni == 1 && entry.n_float == 3) {
            using Fn = void (*)(int32_t, float, float, float); reinterpret_cast<Fn>(entry.host_fn)(static_cast<int32_t>(iv[0]), fv[0], fv[1], fv[2]);
        } else if (ni == 1 && entry.n_float >= 4) {
            using Fn = void (*)(int32_t, float, float, float, float); reinterpret_cast<Fn>(entry.host_fn)(static_cast<int32_t>(iv[0]), fv[0], fv[1], fv[2], fv[3]);
        } else if (ni == 2 && entry.n_float == 1) {
            using Fn = void (*)(uint32_t, uint32_t, float); reinterpret_cast<Fn>(entry.host_fn)(static_cast<uint32_t>(iv[0]), static_cast<uint32_t>(iv[1]), fv[0]);
        } else {
            if (trace) {
                fprintf(stderr, "[display-thunk] dispatch: %s unsupported mixed ABI (int×%u fp×%u) — call dropped\n",
                        entry.name.c_str(), ni, entry.n_float);
            }
        }
        cpu.regs[0] = 0;
        return 0;
    }

    // ── Integer/pointer path with optional stack args ─────────────────
    constexpr int kMaxArgs = 12;
    uint64_t args[kMaxArgs] = {0};
    for (int i = 0; i < 8; i++) args[i] = cpu.regs[i];
    // AAPCS64: args 8+ live on the guest stack at SP, 8-byte slots.
    if (entry.n_stack && impl_->mem) {
        for (uint8_t i = 0; i < entry.n_stack && (8 + i) < kMaxArgs; i++) {
            uint64_t slot = cpu.sp + static_cast<uint64_t>(i) * 8ull;
            impl_->mem->read(slot, &args[8 + i], sizeof(uint64_t));
        }
    }

    // ── GetProcAddress: return guest trampoline for a registered symbol ─
    if (entry.flags & THUNK_GET_PROC) {
        char namebuf[256];
        const char* name = nullptr;
        if (args[0] && impl_->mem) {
            uint8_t* hp = impl_->mem->guest_to_host_ptr(args[0]);
            if (hp) {
                name = reinterpret_cast<const char*>(hp);
            } else {
                size_t n = 0;
                for (; n + 1 < sizeof(namebuf); n++) {
                    uint8_t c = 0;
                    try { impl_->mem->read(args[0] + n, &c, 1); }
                    catch (...) { break; }
                    namebuf[n] = static_cast<char>(c);
                    if (c == 0) break;
                }
                namebuf[sizeof(namebuf) - 1] = 0;
                name = namebuf;
            }
        }
        uint64_t found = 0;
        if (name && name[0]) {
            for (const auto& lib : impl_->libs_) {
                for (const auto& e : lib.entries) {
                    if (e.name == name) { found = e.guest_addr; break; }
                }
                if (found) break;
            }
        }
        if (trace) {
            fprintf(stderr, "[display-thunk] GetProcAddress('%s') → 0x%llx\n",
                    name ? name : "(null)",
                    static_cast<unsigned long long>(found));
        }
        cpu.regs[0] = found;
        return 0;
    }

    // ── Pointer arg translation ────────────────────────────────────────
    auto translate_ptr = [&](uint64_t& a, int idx,
                             std::vector<uint8_t>* bounce,
                             std::vector<uint8_t>* pristine,
                             uint64_t* guest_orig, bool* need_wb) {
        if (a == 0 || !impl_->mem) return;
        uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(a);
        if (host_ptr) { a = reinterpret_cast<uint64_t>(host_ptr); return; }
        // High-stack / sparse-page pointer: bounce through a host buffer.
        // Default 64 KiB covers fixed-size output structs (XEvent, etc.);
        // known big-buffer functions size the bounce from their args so we
        // neither truncate the data nor write 64 KiB of garbage back over a
        // small guest object. The per-symbol SIZE column of the spec
        // overrides it (mirrors the SizeKind sizing in thunk.cpp); the
        // argument positions are implied by the symbol, so no name compares.
        size_t kBounce = 65536;
        const thunk::SizeKind sk = entry.spec ? entry.spec->size
                                              : thunk::SizeKind::NONE;
        switch (sk) {
        case thunk::SizeKind::X_DRAWSTR:
            // XDrawString/XDrawImageString(display, d, gc, x, y, string, len):
            // the string (arg 5) is sized by the length arg 6.
            if (idx == 5) {
                uint64_t sz = args[6];
                if (sz > 0 && sz < (64ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        case thunk::SizeKind::X_SETWMPROTO:
            // XSetWMProtocols(display, w, protocols, count): the Atom array
            // (arg 2) is sized by the count arg 3 (Atom = 8 bytes).
            if (idx == 2) {
                uint64_t sz = args[3] * sizeof(unsigned long);
                if (sz > 0 && sz < (64ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        default:
            break;
        }
        bounce->resize(kBounce);
        try { impl_->mem->read(a, bounce->data(), kBounce); }
        catch (...) { bounce->assign(kBounce, 0); }
        *pristine = *bounce;   // snapshot before the host call
        *guest_orig = a;
        *need_wb = true;
        a = reinterpret_cast<uint64_t>(bounce->data());
    };

    std::vector<uint8_t> bounce_bufs[kMaxArgs];
    std::vector<uint8_t> bounce_pristine[kMaxArgs];
    uint64_t bounce_guest[kMaxArgs] = {0};
    bool bounce_wb[kMaxArgs] = {false};
    VkStage vk_deep_stage;   // lives until after the host call

    // ── VK_CMD_DEEP / VK_CMD_DEEP_OUT: descriptor-driven staging ────
    // Replaces listed array args with staged host pointers so the plain
    // translate_ptr pass never sees them (it would bounce ONE element
    // while the host driver walks count elements past it).
    // OUT refs (VK_CMD_DEEP_OUT): enumerations stage a 4-byte count
    // bounce + the array; after the host call min(staged, actual)
    // elements AND the actual count are copied back to guest memory.
    uint32_t vk_deep_done = 0;
    struct DeepOutRec {
        int arg = 0;
        int count_arg_idx = -1;
        uint64_t guest_array = 0;
        uint64_t guest_count_ptr = 0;   // 0 = copyback-only
        void* staged = nullptr;
        void* staged_count = nullptr;
        uint32_t staged_elems = 0;
        size_t elem_size = 0;
    };
    DeepOutRec vk_out_recs[4];
    int vk_n_out_recs = 0;
    if (entry.spec &&
        (entry.spec->policy == thunk::Policy::VK_CMD_DEEP ||
         entry.spec->policy == thunk::Policy::VK_CMD_DEEP_OUT) &&
        impl_->mem && entry.host_fn) {
        if (const thunk::VkCmdPlan* plan =
                thunk::vk_find_cmd_plan(entry.name.c_str())) {
            bool ok = true;
            size_t need = 0;
            struct DeepJob { int arg = 0; uint32_t count = 0;
                             uint64_t guest = 0; bool raw = false;
                             const thunk::VkStructDesc* desc = nullptr; };
            DeepJob jobs[8] = {};
            int njobs = 0;
            for (uint8_t ri = 0; ri < plan->nrefs && ok; ri++) {
                const thunk::VkPlanRef& r = plan->refs[ri];
                if (r.arg >= kMaxArgs) continue;

                // ── NULLIFY: force this arg to nullptr at the host call
                if (r.out == 3) {
                    args[r.arg] = 0;
                    vk_deep_done |= 1u << r.arg;
                    continue;
                }
                // ── SINGLE_STRUCT_IN: stage ONE struct recursively ──
                if (r.out == 4) {
                    if (!args[r.arg]) continue;
                    need = (need + 7u) & ~size_t(7);
                    need += vk_deep_size_one(
                        impl_->mem, args[r.arg], r.desc, 1, 0, &ok);
                    if (!ok) break;
                    jobs[njobs++] = {static_cast<int>(r.arg), 1u,
                                     args[r.arg], false, r.desc};
                    vk_deep_done |= 1u << r.arg;
                    continue;
                }
                // ── OUT_HANDLE: 8-byte bounce + post-call writeback ──
                if (r.out == 5) {
                    DeepOutRec rec{};
                    rec.arg = r.arg;
                    rec.count_arg_idx = -1;
                    rec.guest_array = args[r.arg];
                    rec.elem_size = 8;
                    rec.staged_elems = 1;
                    need = (need + 7u) & ~size_t(7);
                    need += 8;
                    vk_out_recs[vk_n_out_recs++] = rec;
                    vk_deep_done |= 1u << r.arg;
                    continue;
                }

                // ── OUT refs: enumeration / copyback-only staging ──
                if (r.out == 1 || r.out == 2) {
                    DeepOutRec rec{};
                    rec.arg = r.arg;
                    rec.count_arg_idx = r.count_arg;
                    rec.guest_array = args[r.arg];
                    size_t esz = r.desc ? r.desc->size : r.elem_size;
                    rec.elem_size = esz;
                    if (r.out == 2 && !args[r.arg]) continue;
                    if (r.out == 1) {
                        // count lives behind a guest u32* pointer
                        rec.guest_count_ptr = args[r.count_arg];
                        uint32_t cnt = 0;
                        if (rec.guest_count_ptr) {
                            try { impl_->mem->read(rec.guest_count_ptr,
                                                   &cnt, 4); }
                            catch (...) { cnt = 0; }
                        }
                        if (cnt > kVkDeepMaxElems) cnt = kVkDeepMaxElems;
                        rec.staged_elems = cnt;
                        // count bounce is allocated AFTER reserve (fill
                        // phase below) — never hand out arena pointers
                        // before the final reserve
                        need = (need + 3u) & ~size_t(3);
                        need += 4;
                        vk_deep_done |= 1u << r.count_arg;
                        if (!args[r.arg] || !cnt) {
                            vk_out_recs[vk_n_out_recs++] = rec;
                            continue;
                        }
                        need = (need + 7u) & ~size_t(7);
                        if (r.desc) {
                            need += vk_deep_size_one(
                                impl_->mem, rec.guest_array, r.desc,
                                cnt, 0, &ok);
                        } else {
                            size_t bytes =
                                (size_t)cnt * esz;
                            if (bytes > kVkDeepMaxBytes) ok = false;
                            need += bytes;
                        }
                    } else {
                        // copyback-only raw bytes (GetQueryPoolResults
                        // pData): byte count arrives in a register
                        size_t bytes = static_cast<size_t>(
                            args[r.count_arg]);
                        if (bytes > kVkDeepMaxBytes) ok = false;
                        rec.staged_elems =
                            static_cast<uint32_t>(bytes);
                        need += bytes;
                    }
                    if (!ok) break;
                    // raw jobs carry a BYTE count (the fill loop reads/
                    // stages exactly j.count bytes); struct jobs carry
                    // an ELEMENT count
                    jobs[njobs++] = {r.arg,
                                     r.desc ? rec.staged_elems :
                                              static_cast<uint32_t>(
                                                  (size_t)rec.staged_elems *
                                                  esz),
                                     rec.guest_array,
                                     !r.desc, r.desc};
                    vk_out_recs[vk_n_out_recs++] = rec;
                    continue;
                }

                // ── IN refs (existing behavior) ──────────────────────
                uint64_t cnt_arg = args[r.count_arg];
                uint32_t n = r.count_in_bytes
                    ? 1   // raw: arg IS a byte count, marshalled below
                    : static_cast<uint32_t>(
                          cnt_arg > kVkDeepMaxElems ? 0 : cnt_arg);
                if (!args[r.arg]) continue;
                DeepJob j{};
                j.arg = r.arg;
                j.guest = args[r.arg];
                j.desc = r.desc ? r.desc : nullptr;
                if (r.count_in_bytes || !r.desc) {
                    // raw buffer: stage verbatim in one block. Element
                    // size comes from the ref (handles 8, enums 4, ...)
                    size_t bytes = r.count_in_bytes
                        ? static_cast<size_t>(cnt_arg)
                        : static_cast<size_t>(cnt_arg) * r.elem_size;
                    if (bytes > kVkDeepMaxBytes) ok = false;
                    j.count = static_cast<uint32_t>(bytes);   // bytes
                    j.raw = true;
                    need += bytes;
                } else {
                    j.count = n;
                    need = (need + 7u) & ~size_t(7);
                    need += vk_deep_size_one(impl_->mem, j.guest,
                                             j.desc, j.count, 0, &ok);
                }
                jobs[njobs++] = j;
            }
            if (ok && need < kVkDeepMaxBytes) {
                VkStage& st = vk_deep_stage;
                st.buf.reserve(std::max<size_t>(need + 4096, 65536));
                // count + OUT-handle bounces FIRST (allocation order
                // mirrors `need`: out-refs contributed their bytes in
                // scan order before any IN-ref bytes)
                for (int oi = 0; oi < vk_n_out_recs; oi++) {
                    DeepOutRec& rec = vk_out_recs[oi];
                    if (rec.guest_count_ptr) {
                        void* cb = st.bytes(4, 4);
                        uint32_t cnt = rec.staged_elems;
                        std::memcpy(cb, &cnt, 4);
                        rec.staged_count = cb;
                        args[rec.count_arg_idx] =
                            reinterpret_cast<uint64_t>(cb);
                    } else if (rec.guest_array && rec.staged_elems == 1) {
                        // OUT_HANDLE bounce (zeroed; host writes the
                        // handle through it)
                        void* hb = st.bytes(8, 8);
                        std::memset(hb, 0, 8);
                        rec.staged = hb;
                        args[rec.arg] =
                            reinterpret_cast<uint64_t>(hb);
                    }
                }
                for (int ji = 0; ji < njobs; ji++) {
                    DeepJob& j = jobs[ji];
                    void* dst;
                    if (j.raw) {
                        dst = st.bytes(j.count, 8);
                        try { impl_->mem->read(j.guest, dst, j.count); }
                        catch (...) { std::memset(dst, 0, j.count); }
                    } else {
                        dst = st.bytes(
                            (size_t)j.count * j.desc->size, 8);
                        for (uint32_t k2 = 0; k2 < j.count; k2++)
                            vk_deep_fill_elem(
                                impl_->mem, st,
                                j.guest + (uint64_t)k2 * j.desc->size,
                                j.desc,
                                reinterpret_cast<uint8_t*>(dst) +
                                    (uint64_t)k2 * j.desc->size, 0);
                    }
                    args[j.arg] = reinterpret_cast<uint64_t>(dst);
                    vk_deep_done |= 1u << j.arg;
                    for (int oi = 0; oi < vk_n_out_recs; oi++) {
                        if (vk_out_recs[oi].arg == j.arg)
                            vk_out_recs[oi].staged = dst;
                    }
                }
            } else {
                // plan failed (garbage counts, oversized staging): undo
                // EVERYTHING so the generic translate path sees the
                // ORIGINAL guest pointers for every planned arg.
                // (Count-bounce rewrites happened only in the fill
                // phase, which never ran; args[] still hold guests.)
                for (uint8_t ri2 = 0; ri2 < plan->nrefs; ri2++) {
                    const thunk::VkPlanRef& r2 = plan->refs[ri2];
                    if (r2.arg < kMaxArgs)
                        vk_deep_done &= ~(1u << r2.arg);
                    if (r2.out == 1 && r2.count_arg < kMaxArgs)
                        vk_deep_done &= ~(1u << r2.count_arg);
                }
                vk_n_out_recs = 0;
            }
        }
    }

    if (entry.pointer_args && impl_->mem) {
        for (int i = 0; i < kMaxArgs; i++) {
            if ((entry.pointer_args & (1u << i)) &&
                !(vk_deep_done & (1u << i))) {
                translate_ptr(args[i], i, &bounce_bufs[i],
                              &bounce_pristine[i],
                              &bounce_guest[i], &bounce_wb[i]);
            }
        }
    }

    if (trace) {
        fprintf(stderr, "[display-thunk] dispatch[T%lx]: %s (host_fn=%p) "
                "a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx "
                "a8=0x%llx ptrs=0x%x stack=%u\n",
                (unsigned long)pthread_self(),
                entry.name.c_str(), entry.host_fn,
                static_cast<unsigned long long>(args[0]),
                static_cast<unsigned long long>(args[1]),
                static_cast<unsigned long long>(args[2]),
                static_cast<unsigned long long>(args[3]),
                static_cast<unsigned long long>(args[8]),
                entry.pointer_args, entry.n_stack);
    }

    uint64_t ret = 0;
    if (entry.n_stack >= 4) {
        using Fn12 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn12>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7],
            args[8], args[9], args[10], args[11]);
    } else if (entry.n_stack == 3) {
        using Fn11 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn11>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7],
            args[8], args[9], args[10]);
    } else if (entry.n_stack == 2) {
        using Fn10 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t);
        ret = reinterpret_cast<Fn10>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7],
            args[8], args[9]);
    } else if (entry.n_stack == 1) {
        using Fn9 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t);
        ret = reinterpret_cast<Fn9>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7], args[8]);
    } else {
        using Fn8 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t);
        ret = reinterpret_cast<Fn8>(entry.host_fn)(
            args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7]);
    }

    // ── VK_CMD_DEEP_OUT copyback: enumerations + OUT data ────────────
    // The host wrote the actual count into our 4-byte bounce and filled
    // min(count, staged) elements of the staged array. Publish both to
    // guest memory. (VkStage outlives this — declared at function scope.)
    for (int oi = 0; oi < vk_n_out_recs; oi++) {
        const DeepOutRec& rec = vk_out_recs[oi];
        if (rec.staged_count && rec.guest_count_ptr) {
            uint32_t actual = 0;
            std::memcpy(&actual, rec.staged_count, 4);
            try { impl_->mem->write(rec.guest_count_ptr, &actual, 4); }
            catch (...) { /* unmapped count pointer */ }
        }
        if (rec.staged && rec.guest_array && rec.staged_elems) {
            size_t bytes = (size_t)rec.staged_elems * rec.elem_size;
            try { impl_->mem->write(rec.guest_array, rec.staged, bytes); }
            catch (...) { /* unmapped array */ }
        }
    }

    // Write bounced pointer args back into guest memory. Only the byte
    // range the host actually modified is written back — writing the full
    // bounce (64 KiB by default) over a small guest object (e.g. a stack
    // XEvent or XColor above the 4 GiB direct window) clobbered adjacent
    // guest memory. Diffing against the pre-call snapshot also makes
    // input-only pointers (host never writes them) a no-op writeback.
    // A wrong pointer mask (e.g. an XID marked as a pointer) can bounce an
    // unmapped guest address; Memory::write throws UnmappedMemory there, so
    // guard the writeback or the exception escapes dispatch into the
    // syscall handler.
    if (impl_->mem) {
        for (int i = 0; i < kMaxArgs; i++) {
            if (bounce_wb[i] && bounce_guest[i]) {
                const auto& after = bounce_bufs[i];
                const auto& before = bounce_pristine[i];
                if (after.size() != before.size()) continue;
                size_t first = after.size(), last = 0;
                for (size_t j = 0; j < after.size(); j++) {
                    if (after[j] != before[j]) {
                        if (first > j) first = j;
                        last = j + 1;
                    }
                }
                if (last <= first) continue;  // host didn't touch it
                try {
                    impl_->mem->write(bounce_guest[i] + first,
                                      after.data() + first, last - first);
                } catch (...) {
                    // Best-effort: the host call already happened; don't let
                    // a bad writeback corrupt the emulator's control flow.
                }
            }
        }
    }

    if (entry.flags & THUNK_RET_STRING) {
        // Cache the host string into guest memory and return the guest address.
        if (ret) {
            ret = impl_->cache_host_string_(reinterpret_cast<const char*>(ret));
        }
    }

    cpu.regs[0] = ret;
    return 0;
}
// ── Vulkan marshalling (1.5.4-alpha) ───────────────────────────────────
// The generic bounce path copies pointer args byte-for-byte, which cannot
// fix the NESTED guest pointers inside the instance/device create infos
// (ppEnabledExtensionNames string arrays, pQueueCreateInfos struct array
// with pQueuePriorities, pApplicationInfo). Those two entry points get a
// deep-copy into a per-call host staging buffer. Opaque Vk handles are
// intentionally NOT translated: the guest stores the host pointer the
// host returned, so passing handle args verbatim round-trips them. Only
// the OUT handle slots (pInstance / pDevice) are written back after the
// host call.
namespace {
// Per-call host staging buffer. All nested strings, struct copies and
// pointer arrays for one Vulkan call live here; it is destroyed when the
// call returns.
// Frozen (spec-stable) layouts of the Vulkan structs we deep-copy. These
// are plain C structs with natural alignment, so they match the AArch64
// guest layout exactly.
struct VkAppInfoH {
    int32_t  sType; void* pNext; const char* pApplicationName;
    uint32_t applicationVersion; const char* pEngineName;
    uint32_t engineVersion; uint32_t apiVersion;
};
struct VkInstanceCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags;
    const VkAppInfoH* pApplicationInfo; uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames; uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};
struct VkDeviceQueueCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags; uint32_t queueFamilyIndex;
    uint32_t queueCount; const float* pQueuePriorities;
};
struct VkDeviceCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags;
    uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfoH* pQueueCreateInfos;
    uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
    const void* pEnabledFeatures;  // VkPhysicalDeviceFeatures (220 bytes, frozen)
};
// VkPresentInfoKHR (spec-stable, no padding on AArch64).
struct VkPresentInfoH {
    int32_t sType; void* pNext; uint32_t waitSemaphoreCount;
    const void* pWaitSemaphores; uint32_t swapchainCount;
    const void* pSwapchains; const uint32_t* pImageIndices;
    int32_t* pResults;  // VkResult array (may be NULL)
};
// VkSubmitInfo (command-buffer submission; handles round-trip verbatim).
struct VkSubmitInfoH {
    int32_t sType; void* pNext; uint32_t waitSemaphoreCount;
    const void* pWaitSemaphores; const void* pWaitDstStageMask;
    uint32_t commandBufferCount; const void* pCommandBuffers;
    uint32_t signalSemaphoreCount; const void* pSignalSemaphores;
};
// ── Graphics-pipeline stage (2026-08-21) ──────────────────────────────
// All layouts verified byte-for-byte against the vendored
// ctest_real/vulkan_headers vulkan_core.h (natural-alignment LP64 —
// identical on host x86-64 and guest AArch64).
struct VkPipelineShaderStageCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags; uint32_t stage;
    uint64_t module; const char* pName; const void* pSpecializationInfo;
};
struct VkSpecializationMapEntryH { uint32_t constantID, offset; size_t size; };
struct VkSpecializationInfoH {
    uint32_t mapEntryCount; const VkSpecializationMapEntryH* pMapEntries;
    size_t dataSize; const void* pData;
};
struct VkComputePipelineCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags;
    VkPipelineShaderStageCreateInfoH stage;
    uint64_t basePipelineHandle; int32_t basePipelineIndex;
};
struct VkPipelineVertexInputStateCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags;
    uint32_t vertexBindingDescriptionCount; const void* pVertexBindingDescriptions;
    uint32_t vertexAttributeDescriptionCount; const void* pVertexAttributeDescriptions;
};
struct VkPipelineInputAssemblyStateCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags;
    uint32_t topology; uint32_t primitiveRestartEnable;
};
struct VkPipelineViewportStateCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags; uint32_t viewportCount;
    const void* pViewports; uint32_t scissorCount; const void* pScissors;
};
struct VkPipelineRasterizationStateCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags;
    uint32_t depthClampEnable, rasterizerDiscardEnable, polygonMode,
             cullMode, frontFace, depthBiasEnable;
    float depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor, lineWidth;
};
struct VkPipelineMultisampleStateCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags; uint32_t rasterizationSamples;
    uint32_t sampleShadingEnable; float minSampleShading;
    const void* pSampleMask; uint32_t alphaToCoverageEnable; uint32_t alphaToOneEnable;
};
struct VkStencilOpStateH {
    uint32_t failOp, passOp, depthFailOp, compareOp, compareMask, writeMask, reference;
};
struct VkPipelineDepthStencilStateCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags;
    uint32_t depthTestEnable, depthWriteEnable, depthCompareOp,
             depthBoundsTestEnable, stencilTestEnable;
    VkStencilOpStateH front, back;
    float minDepthBounds, maxDepthBounds;
};
struct VkPipelineColorBlendStateCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags;
    uint32_t logicOpEnable, logicOp, attachmentCount;
    const void* pAttachments; float blendConstants[4];
};
struct VkPipelineDynamicStateCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags; uint32_t dynamicStateCount;
    const void* pDynamicStates;
};
struct VkGraphicsPipelineCreateInfoH {
    int32_t sType; void* pNext; uint32_t flags; uint32_t stageCount;
    const VkPipelineShaderStageCreateInfoH* pStages;
    const VkPipelineVertexInputStateCreateInfoH* pVertexInputState;
    const VkPipelineInputAssemblyStateCreateInfoH* pInputAssemblyState;
    const void* pTessellationState;
    const VkPipelineViewportStateCreateInfoH* pViewportState;
    const VkPipelineRasterizationStateCreateInfoH* pRasterizationState;
    const VkPipelineMultisampleStateCreateInfoH* pMultisampleState;
    const VkPipelineDepthStencilStateCreateInfoH* pDepthStencilState;
    const VkPipelineColorBlendStateCreateInfoH* pColorBlendState;
    const VkPipelineDynamicStateCreateInfoH* pDynamicState;
    uint64_t layout; uint64_t renderPass; uint32_t subpass;
    uint64_t basePipelineHandle; int32_t basePipelineIndex;
};
struct VkDescriptorSetAllocateInfoH {
    int32_t sType; void* pNext; uint64_t descriptorPool;
    uint32_t descriptorSetCount; const void* pSetLayouts;
};
struct VkWriteDescriptorSetH {
    int32_t sType; void* pNext; uint64_t dstSet;
    uint32_t dstBinding, dstArrayElement, descriptorCount, descriptorType;
    const void* pImageInfo; const void* pBufferInfo; const void* pTexelBufferView;
};
struct VkDescriptorImageInfoH { uint64_t sampler, imageView; uint32_t imageLayout; };
struct VkDescriptorBufferInfoH { uint64_t buffer, offset, range; };
struct VkCopyDescriptorSetH {
    int32_t sType; void* pNext; uint64_t srcSet;
    uint32_t srcBinding, srcArrayElement; uint64_t dstSet;
    uint32_t dstBinding, dstArrayElement, descriptorCount;
};
// vkMapMemory bounce stage (2026-08-21): VkMemoryAllocateInfo (records the
// allocation size for VK_WHOLE_SIZE maps) and VkMappedMemoryRange (flat —
// flush/invalidate move ranges between bounce and host mapping).
struct VkMemoryAllocateInfoH {
    int32_t sType; void* pNext; size_t allocationSize; uint32_t memoryTypeIndex;
};
struct VkMappedMemoryRangeH {
    int32_t sType; void* pNext; uint64_t memory; size_t offset; size_t size;
};
constexpr size_t kPhysicalDeviceFeaturesBytes = 220;
// Read a guest struct by value into `dst` (zero-fill on unmapped).
template <typename T> void read_guest_struct(Memory* mem, uint64_t g, T* dst) {
    if (dst) {
        if (!g) { std::memset(dst, 0, sizeof(*dst)); return; }
        try { mem->read(g, dst, sizeof(*dst)); }
        catch (...) { std::memset(dst, 0, sizeof(*dst)); }
    }
}
// Read `n` guest bytes into `dst` (zero-fill on unmapped).
void read_guest_bytes(Memory* mem, uint64_t g, void* dst, size_t n) {
    if (!g || !n) { if (dst && n) std::memset(dst, 0, n); return; }
    try { mem->read(g, dst, n); }
    catch (...) { std::memset(dst, 0, n); }
}

// ── pNext chain deep-marshal (generated, Phase A1) ───────────────────
// Extension structs chained via pNext are GUEST pointers; the host driver
// walks the chain and derefs them. Node layouts come from the GENERATED
// sType→descriptor map (vk_find_struct_by_stype, built from vk.xml) so
// every registry-known chainable struct is covered — not just a hand
// table. Unknown sTypes truncate the chain at that node (one-shot
// diagnostic). Sizes are validated against the vendored vulkan_core.h by
// the generator's layout engine.
constexpr uint32_t kMaxPnextNodes = kVkDeepMaxPnextNodes;
struct VkPnextNode { uint8_t* host; uint64_t guest; size_t size; };
// Walks the guest chain starting at `g`; fills up to kMaxPnextNodes nodes.
uint32_t vk_marshal_pnext_chain(Memory* mem, VkStage& st, uint64_t g,
                                VkPnextNode* nodes) {
    uint32_t n = 0;
    void* prev = nullptr;
    while (g && n < kMaxPnextNodes) {
        int32_t s = 0; uint64_t pn = 0;
        try { mem->read(g, &s, 4); mem->read(g + 8, &pn, 8); }
        catch (...) { break; }
        const thunk::VkStructDesc* d = thunk::vk_find_struct_by_stype(s);
        if (!d) { vk_deep_unknown_stype_once(s); break; }
        uint8_t* h = reinterpret_cast<uint8_t*>(st.bytes(d->size, 8));
        read_guest_bytes(mem, g, h, d->size);
        *reinterpret_cast<void**>(h + 8) = nullptr;
        if (prev) *reinterpret_cast<void**>(static_cast<uint8_t*>(prev) + 8) = h;
        nodes[n++] = {h, g, d->size};
        prev = h;
        g = pn;
    }
    return n;
}
void vk_writeback_pnext_chain(Memory* mem, const VkPnextNode* nodes,
                              uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        // Write back everything EXCEPT the pNext link (offsets 8..16):
        // our staged copy carries a HOST pointer there which must never
        // land in guest memory (it would zero/corrupt the guest chain).
        const VkPnextNode& nd = nodes[i];
        if (nd.size > 8) {
            try { mem->write(nd.guest, nd.host, 8); } catch (...) {}
        }
        if (nd.size > 16) {
            try { mem->write(nd.guest + 16, nd.host + 16,
                             nd.size - 16); } catch (...) {}
        }
    }
}
} // namespace

bool DisplayThunk::vk_dispatch_(CPU& cpu, const SymbolEntry& entry, bool trace) {
    Memory* mem = impl_->mem;
    if (!mem) return false;
    if (trace)
        fprintf(stderr, "[display-thunk] vk_dispatch_[T%lx]: %s\n",
                (unsigned long)pthread_self(), entry.name.c_str());

    // ── vkGetInstanceProcAddr / vkGetDeviceProcAddr ────────────────
    // The generic THUNK_GET_PROC path reads the name from arg 0 (GL
    // convention); Vulkan's proc-addr functions take the name in arg 1
    // (arg 0 is the instance/device handle).
    if (entry.flags & THUNK_GET_PROC) {
        char namebuf[256];
        size_t n = 0;
        uint64_t pname = cpu.regs[1];
        if (pname) {
            try {
                while (n + 1 < sizeof(namebuf)) {
                    uint8_t c = 0;
                    mem->read(pname + n, &c, 1);
                    namebuf[n++] = static_cast<char>(c);
                    if (c == 0) break;
                }
            } catch (...) { /* truncate */ }
        }
        namebuf[n] = 0;
        uint64_t found = 0;
        if (namebuf[0]) {
            for (const auto& lib : impl_->libs_) {
                for (const auto& e : lib.entries) {
                    if (e.name == namebuf) { found = e.guest_addr; break; }
                }
                if (found) break;
            }
        }
        if (trace) {
            fprintf(stderr, "[display-thunk] vkGetProcAddr('%s') → 0x%llx\n",
                    namebuf[0] ? namebuf : "(null)",
                    static_cast<unsigned long long>(found));
        }
        cpu.regs[0] = found;
        return true;
    }

    // ── vkCreateInstance ─────────────────────────────────────────────
    // (pCreateInfo, pAllocator, pInstance) — arg 2 is the OUT handle.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_CREATE_INSTANCE) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }  // VK_ERROR_INITIALIZATION_FAILED
        VkStage st;
        VkInstanceCreateInfoH* info = st.alloc<VkInstanceCreateInfoH>();
        read_guest_struct(mem, cpu.regs[0], info);
        if (info->pApplicationInfo) {
            VkAppInfoH* app = st.alloc<VkAppInfoH>();
            read_guest_struct(mem, reinterpret_cast<uint64_t>(info->pApplicationInfo), app);
            app->pApplicationName = st.guest_str(mem, reinterpret_cast<uint64_t>(app->pApplicationName));
            app->pEngineName      = st.guest_str(mem, reinterpret_cast<uint64_t>(app->pEngineName));
            info->pApplicationInfo = app;
        }
        uint64_t dbg_raw_ext_arr = reinterpret_cast<uint64_t>(info->ppEnabledExtensionNames);
        uint32_t dbg_ext_count = info->enabledExtensionCount;
        info->ppEnabledLayerNames = st.guest_str_array(
            mem, reinterpret_cast<uint64_t>(info->ppEnabledLayerNames), info->enabledLayerCount);
        info->ppEnabledExtensionNames = st.guest_str_array(
            mem, reinterpret_cast<uint64_t>(info->ppEnabledExtensionNames), info->enabledExtensionCount);
        if (trace) {
            fprintf(stderr,
                    "[display-thunk] vkCreateInstance: sType=%d pNext=%p app=%d "
                    "layers=%u exts=%u raw_ext_arr=0x%llx\n",
                    info->sType, info->pNext, info->pApplicationInfo ? 1 : 0,
                    info->enabledLayerCount, info->enabledExtensionCount,
                    static_cast<unsigned long long>(dbg_raw_ext_arr));
            for (uint32_t i = 0; i < dbg_ext_count && dbg_raw_ext_arr; i++) {
                uint64_t p = 0;
                try { mem->read(dbg_raw_ext_arr + i * 8u, &p, 8); }
                catch (...) { p = 0xdeaddead; }
                fprintf(stderr, "[display-thunk]   raw ext[%u] ptr=0x%llx\n",
                        i, static_cast<unsigned long long>(p));
            }
        }
        uint64_t host_instance = 0;
        // pAllocator (arg 1): always NULL. VkAllocationCallbacks contains
        // host function pointers that cannot be marshalled; passing NULL is
        // the only correct choice and is symmetric across create/destroy.
        uint64_t ret = reinterpret_cast<uint64_t (*)(const void*, const void*, void*)>(entry.host_fn)(
            info, nullptr, &host_instance);
        if (cpu.regs[2] && host_instance) {
            try { mem->write(cpu.regs[2], &host_instance, sizeof(host_instance)); }
            catch (...) { /* out pointer unmapped — result lost */ }
        }
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) {
            fprintf(stderr, "[display-thunk] vkCreateInstance → %d (instance=%p)\n",
                    static_cast<int32_t>(ret), reinterpret_cast<void*>(host_instance));
        }
        return true;
    }

    // ── vkCreateDevice ───────────────────────────────────────────────
    // (physicalDevice, pCreateInfo, pAllocator, pDevice) — arg 3 is the
    // OUT handle.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_CREATE_DEVICE) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        VkDeviceCreateInfoH* info = st.alloc<VkDeviceCreateInfoH>();
        read_guest_struct(mem, cpu.regs[1], info);
        if (info->pQueueCreateInfos && info->queueCreateInfoCount &&
            info->queueCreateInfoCount <= 16) {
            VkDeviceQueueCreateInfoH* arr = reinterpret_cast<VkDeviceQueueCreateInfoH*>(
                st.bytes(static_cast<size_t>(info->queueCreateInfoCount) * sizeof(VkDeviceQueueCreateInfoH), 8));
            for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
                uint64_t g = reinterpret_cast<uint64_t>(info->pQueueCreateInfos)
                           + static_cast<uint64_t>(i) * sizeof(VkDeviceQueueCreateInfoH);
                read_guest_struct(mem, g, &arr[i]);
                if (arr[i].pQueuePriorities && arr[i].queueCount && arr[i].queueCount <= 64) {
                    float* pf = reinterpret_cast<float*>(
                        st.bytes(static_cast<size_t>(arr[i].queueCount) * sizeof(float), 8));
                    try {
                        mem->read(reinterpret_cast<uint64_t>(arr[i].pQueuePriorities),
                                  pf, static_cast<size_t>(arr[i].queueCount) * sizeof(float));
                    } catch (...) { std::memset(pf, 0, static_cast<size_t>(arr[i].queueCount) * sizeof(float)); }
                    arr[i].pQueuePriorities = pf;
                }
            }
            info->pQueueCreateInfos = arr;
        }
        info->ppEnabledLayerNames = st.guest_str_array(
            mem, reinterpret_cast<uint64_t>(info->ppEnabledLayerNames), info->enabledLayerCount);
        info->ppEnabledExtensionNames = st.guest_str_array(
            mem, reinterpret_cast<uint64_t>(info->ppEnabledExtensionNames), info->enabledExtensionCount);
        if (info->pEnabledFeatures) {
            void* f = st.bytes(kPhysicalDeviceFeaturesBytes, 8);
            try { mem->read(reinterpret_cast<uint64_t>(info->pEnabledFeatures), f, kPhysicalDeviceFeaturesBytes); }
            catch (...) { std::memset(f, 0, kPhysicalDeviceFeaturesBytes); }
            info->pEnabledFeatures = f;
        }
        // pNext chain (subgroup/BDA/accel/ray-query/present feature
        // structs) are guest pointers — deep-marshal like Properties2.
        VkPnextNode dnodes[kMaxPnextNodes];
        uint32_t dn = vk_marshal_pnext_chain(
            mem, st, reinterpret_cast<uint64_t>(info->pNext), dnodes);
        // Re-point the create info at the HOST chain head (or truncate) —
        // the raw guest pointer must never reach the driver.
        info->pNext = dn ? dnodes[0].host : nullptr;
        if (trace) {
            fprintf(stderr,
                    "[display-thunk] vkCreateDevice: qcount=%u layers=%u exts=%u "
                    "feats=%d raw_pnext=0x%llx dn=%u\n",
                    info->queueCreateInfoCount, info->enabledLayerCount,
                    info->enabledExtensionCount, info->pEnabledFeatures ? 1 : 0,
                    reinterpret_cast<unsigned long long>(info->pNext), dn);
            for (uint32_t i = 0; i < dn; i++) {
                int32_t s = 0;
                std::memcpy(&s, dnodes[i].host, 4);
                fprintf(stderr, "[display-thunk]   node[%u] stype=%d size=%zu\n",
                        i, s, dnodes[i].size);
            }
        }
        uint64_t host_device = 0;
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, const void*, const void*, void*)>(entry.host_fn)(
            cpu.regs[0], info, nullptr, &host_device);
        vk_writeback_pnext_chain(mem, dnodes, dn);
        if (cpu.regs[3] && host_device) {
            try { mem->write(cpu.regs[3], &host_device, sizeof(host_device)); }
            catch (...) { /* out pointer unmapped — result lost */ }
        }
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) {
            fprintf(stderr, "[display-thunk] vkCreateDevice → %d (device=%p)\n",
                    static_cast<int32_t>(ret), reinterpret_cast<void*>(host_device));
        }
        return true;
    }

    // ── vkQueuePresentKHR ─────────────────────────────────────────────
    // (queue, pPresentInfo) — pPresentInfo's NESTED pointers
    // (pWaitSemaphores / pSwapchains / pImageIndices / pResults) are guest
    // addresses the host cannot dereference: the generic bounce copies the
    // top-level struct but leaves nested guest pointers untouched, so the
    // host faults reading e.g. pSwapchains[0]. Re-point every array into
    // the staging buffer (handles round-trip VERBATIM), and writeback
    // pResults after the call.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_PRESENT) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        VkPresentInfoH* pi = st.alloc<VkPresentInfoH>();
        read_guest_struct(mem, cpu.regs[1], pi);
        pi->pNext = nullptr;  // verbatim guest pNext chains are not host-readable
        if (pi->waitSemaphoreCount && pi->pWaitSemaphores && pi->waitSemaphoreCount <= 16) {
            uint64_t* arr = reinterpret_cast<uint64_t*>(
                st.bytes(static_cast<size_t>(pi->waitSemaphoreCount) * 8u, 8));
            read_guest_bytes(mem, reinterpret_cast<uint64_t>(pi->pWaitSemaphores), arr,
                             static_cast<size_t>(pi->waitSemaphoreCount) * 8u);
            pi->pWaitSemaphores = arr;
        }
        if (pi->swapchainCount && pi->pSwapchains && pi->swapchainCount <= 16) {
            uint64_t* arr = reinterpret_cast<uint64_t*>(
                st.bytes(static_cast<size_t>(pi->swapchainCount) * 8u, 8));
            read_guest_bytes(mem, reinterpret_cast<uint64_t>(pi->pSwapchains), arr,
                             static_cast<size_t>(pi->swapchainCount) * 8u);
            pi->pSwapchains = arr;
        }
        if (pi->pImageIndices && pi->swapchainCount && pi->swapchainCount <= 16) {
            uint32_t* arr = reinterpret_cast<uint32_t*>(
                st.bytes(static_cast<size_t>(pi->swapchainCount) * 4u, 4));
            read_guest_bytes(mem, reinterpret_cast<uint64_t>(pi->pImageIndices), arr,
                             static_cast<size_t>(pi->swapchainCount) * 4u);
            pi->pImageIndices = arr;
        }
        int32_t* results_host = nullptr;
        uint64_t results_guest = 0;
        if (pi->pResults && pi->swapchainCount && pi->swapchainCount <= 16) {
            results_host = reinterpret_cast<int32_t*>(
                st.bytes(static_cast<size_t>(pi->swapchainCount) * 4u, 4));
            read_guest_bytes(mem, reinterpret_cast<uint64_t>(pi->pResults), results_host,
                             static_cast<size_t>(pi->swapchainCount) * 4u);
            results_guest = reinterpret_cast<uint64_t>(pi->pResults);
            pi->pResults = results_host;
        }
        // Push all vkMapMemory bounces into their host mappings before the
        // GPU consumes the frame (PCWFC discipline — the practical
        // HOST_COHERENT guarantee for mapped memory).
        impl_->vk_sync_push_all();
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, const void*)>(entry.host_fn)(
            cpu.regs[0], pi);
        if (results_guest && results_host) {
            try { mem->write(results_guest, results_host, static_cast<size_t>(pi->swapchainCount) * 4u); }
            catch (...) { /* out pointer unmapped — results lost */ }
        }
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) {
            fprintf(stderr, "[display-thunk] vkQueuePresentKHR → %d\n", static_cast<int32_t>(ret));
        }
        return true;
    }

    // ── vkQueueSubmit ─────────────────────────────────────────────────
    // (queue, submitCount, pSubmits, fence) — each VkSubmitInfo has NESTED
    // pWaitSemaphores / pWaitDstStageMask / pCommandBuffers /
    // pSignalSemaphores arrays of opaque handles the host can't read from
    // guest addresses. Re-point every array into the staging buffer; the
    // handles themselves round-trip verbatim. No writeback (input-only).
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_SUBMIT) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        uint32_t submit_count = static_cast<uint32_t>(cpu.regs[1]);
        if (submit_count > 16) {
            cpu.regs[0] = 0xFFFFFFFEu;  // VK_ERROR_DEVICE_LOST
            return true;
        }
        VkSubmitInfoH* arr = reinterpret_cast<VkSubmitInfoH*>(
            st.bytes(static_cast<size_t>(submit_count) * sizeof(VkSubmitInfoH), 8));
        for (uint32_t i = 0; i < submit_count; i++) {
            uint64_t g = cpu.regs[2] + static_cast<uint64_t>(i) * sizeof(VkSubmitInfoH);
            read_guest_struct(mem, g, &arr[i]);
            arr[i].pNext = nullptr;  // guest pNext chains are not host-readable
            auto repoint_u64 = [&](const void*& p, uint32_t count) {
                if (count == 0 || count > 16) { p = nullptr; return; }
                uint64_t* a = reinterpret_cast<uint64_t*>(
                    st.bytes(static_cast<size_t>(count) * 8u, 8));
                read_guest_bytes(mem, reinterpret_cast<uint64_t>(p), a,
                                 static_cast<size_t>(count) * 8u);
                p = a;
            };
            auto repoint_u32 = [&](const void*& p, uint32_t count) {
                if (count == 0 || count > 16) { p = nullptr; return; }
                uint32_t* a = reinterpret_cast<uint32_t*>(
                    st.bytes(static_cast<size_t>(count) * 4u, 4));
                read_guest_bytes(mem, reinterpret_cast<uint64_t>(p), a,
                                 static_cast<size_t>(count) * 4u);
                p = a;
            };
            repoint_u64(arr[i].pWaitSemaphores, arr[i].waitSemaphoreCount);
            repoint_u32(arr[i].pWaitDstStageMask, arr[i].waitSemaphoreCount);
            repoint_u64(arr[i].pCommandBuffers, arr[i].commandBufferCount);
            repoint_u64(arr[i].pSignalSemaphores, arr[i].signalSemaphoreCount);
        }
        // Push all vkMapMemory bounces before submit — the GPU reads the
        // host mappings at execution time.
        impl_->vk_sync_push_all();
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, uint32_t, const void*, uint64_t)>(entry.host_fn)(
            cpu.regs[0], submit_count, arr, cpu.regs[3]);
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) fprintf(stderr, "[display-thunk] vkQueueSubmit → %d\n", static_cast<int32_t>(ret));
        return true;
    }

    // ── vkCreateGraphicsPipelines ────────────────────────────────────────
    // (device, pipelineCache, createInfoCount, pCreateInfos, pAllocator,
    // pPipelines) — each VkGraphicsPipelineCreateInfo carries a tree:
    // pStages (each with pName string + optional specialization blob),
    // pVertexInputState (two flat arrays), pViewportState (viewport +
    // scissor arrays), pMultisampleState (sample mask), pColorBlendState
    // (attachment array), pDynamicState (enum array). The flat-after-pNext
    // states (input assembly, tessellation, rasterization, depth/stencil)
    // are copied verbatim with pNext zeroed. OUT handles (arg 5) written
    // back. Worst case (8 pipes × 8 stages × 4 KiB spec data) is ~340 KiB,
    // so the staging vector is reserved up front (realloc would dangle
    // every pointer handed out so far).
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_CREATE_GRAPHICS_PIPELINES) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint32_t count = static_cast<uint32_t>(cpu.regs[2]);
        if (count == 0 || count > 8 || !cpu.regs[3]) {
            cpu.regs[0] = count == 0 ? 0u : 0xFFFFFFFDu;
            return true;
        }
        VkStage st;
        st.buf.reserve(512u * 1024u);
        VkGraphicsPipelineCreateInfoH* infos =
            reinterpret_cast<VkGraphicsPipelineCreateInfoH*>(st.bytes(count * sizeof(VkGraphicsPipelineCreateInfoH), 8));
        read_guest_bytes(mem, cpu.regs[3], infos, count * sizeof(VkGraphicsPipelineCreateInfoH));
        for (uint32_t p = 0; p < count; p++) {
            VkGraphicsPipelineCreateInfoH* g = &infos[p];
            g->pNext = nullptr;
            // Shader stages: pName string + optional specialization info.
            if (g->stageCount && g->pStages && g->stageCount <= 8) {
                VkPipelineShaderStageCreateInfoH* ss =
                    reinterpret_cast<VkPipelineShaderStageCreateInfoH*>(st.bytes(g->stageCount * sizeof(VkPipelineShaderStageCreateInfoH), 8));
                read_guest_bytes(mem, reinterpret_cast<uint64_t>(g->pStages), ss,
                                 g->stageCount * sizeof(VkPipelineShaderStageCreateInfoH));
                for (uint32_t s = 0; s < g->stageCount; s++) {
                    ss[s].pNext = nullptr;
                    ss[s].pName = st.guest_str(mem, reinterpret_cast<uint64_t>(ss[s].pName));
                    if (ss[s].pSpecializationInfo) {
                        VkSpecializationInfoH gi_spec;
                        read_guest_struct(mem, reinterpret_cast<uint64_t>(ss[s].pSpecializationInfo), &gi_spec);
                        VkSpecializationInfoH* spec = st.alloc<VkSpecializationInfoH>();
                        *spec = gi_spec;
                        if (gi_spec.mapEntryCount && gi_spec.pMapEntries && gi_spec.mapEntryCount <= 64) {
                            VkSpecializationMapEntryH* e = reinterpret_cast<VkSpecializationMapEntryH*>(
                                st.bytes(gi_spec.mapEntryCount * sizeof(VkSpecializationMapEntryH), 8));
                            read_guest_bytes(mem, reinterpret_cast<uint64_t>(gi_spec.pMapEntries), e,
                                             gi_spec.mapEntryCount * sizeof(VkSpecializationMapEntryH));
                            spec->pMapEntries = e;
                        } else { spec->mapEntryCount = 0; spec->pMapEntries = nullptr; }
                        if (gi_spec.dataSize && gi_spec.pData && gi_spec.dataSize <= 4096) {
                            void* d = st.bytes(gi_spec.dataSize, 8);
                            read_guest_bytes(mem, reinterpret_cast<uint64_t>(gi_spec.pData), d, gi_spec.dataSize);
                            spec->pData = d;
                        } else { spec->dataSize = 0; spec->pData = nullptr; }
                        ss[s].pSpecializationInfo = spec;
                    }
                }
                g->pStages = ss;
            } else { g->stageCount = 0; g->pStages = nullptr; }
            // Vertex input: two flat description arrays.
            if (g->pVertexInputState) {
                VkPipelineVertexInputStateCreateInfoH* vi = st.alloc<VkPipelineVertexInputStateCreateInfoH>();
                read_guest_struct(mem, reinterpret_cast<uint64_t>(g->pVertexInputState), vi);
                vi->pNext = nullptr;
                if (vi->vertexBindingDescriptionCount && vi->pVertexBindingDescriptions && vi->vertexBindingDescriptionCount <= 32) {
                    void* a = st.bytes(vi->vertexBindingDescriptionCount * 12u, 4);
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(vi->pVertexBindingDescriptions), a,
                                     vi->vertexBindingDescriptionCount * 12u);
                    vi->pVertexBindingDescriptions = a;
                } else { vi->vertexBindingDescriptionCount = 0; vi->pVertexBindingDescriptions = nullptr; }
                if (vi->vertexAttributeDescriptionCount && vi->pVertexAttributeDescriptions && vi->vertexAttributeDescriptionCount <= 32) {
                    void* a = st.bytes(vi->vertexAttributeDescriptionCount * 16u, 4);
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(vi->pVertexAttributeDescriptions), a,
                                     vi->vertexAttributeDescriptionCount * 16u);
                    vi->pVertexAttributeDescriptions = a;
                } else { vi->vertexAttributeDescriptionCount = 0; vi->pVertexAttributeDescriptions = nullptr; }
                g->pVertexInputState = vi;
            }
            // Flat-after-pNext states: verbatim copy, zero pNext.
            if (g->pInputAssemblyState) {
                VkPipelineInputAssemblyStateCreateInfoH* s = st.alloc<VkPipelineInputAssemblyStateCreateInfoH>();
                read_guest_struct(mem, reinterpret_cast<uint64_t>(g->pInputAssemblyState), s);
                s->pNext = nullptr;
                g->pInputAssemblyState = s;
            }
            if (g->pTessellationState) {
                void* s = st.bytes(32, 8);  // sType,pNext,flags,patchControlPoints
                read_guest_bytes(mem, reinterpret_cast<uint64_t>(g->pTessellationState), s, 32);
                static_cast<void**>(s)[1] = nullptr;  // zero pNext
                g->pTessellationState = s;
            }
            if (g->pViewportState) {
                VkPipelineViewportStateCreateInfoH* vs = st.alloc<VkPipelineViewportStateCreateInfoH>();
                read_guest_struct(mem, reinterpret_cast<uint64_t>(g->pViewportState), vs);
                vs->pNext = nullptr;
                if (vs->viewportCount && vs->pViewports && vs->viewportCount <= 16) {
                    void* a = st.bytes(vs->viewportCount * 24u, 4);
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(vs->pViewports), a, vs->viewportCount * 24u);
                    vs->pViewports = a;
                } else { vs->viewportCount = 0; vs->pViewports = nullptr; }
                if (vs->scissorCount && vs->pScissors && vs->scissorCount <= 16) {
                    void* a = st.bytes(vs->scissorCount * 16u, 4);
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(vs->pScissors), a, vs->scissorCount * 16u);
                    vs->pScissors = a;
                } else { vs->scissorCount = 0; vs->pScissors = nullptr; }
                g->pViewportState = vs;
            }
            if (g->pRasterizationState) {
                VkPipelineRasterizationStateCreateInfoH* s = st.alloc<VkPipelineRasterizationStateCreateInfoH>();
                read_guest_struct(mem, reinterpret_cast<uint64_t>(g->pRasterizationState), s);
                s->pNext = nullptr;
                g->pRasterizationState = s;
            }
            if (g->pMultisampleState) {
                VkPipelineMultisampleStateCreateInfoH* s = st.alloc<VkPipelineMultisampleStateCreateInfoH>();
                read_guest_struct(mem, reinterpret_cast<uint64_t>(g->pMultisampleState), s);
                s->pNext = nullptr;
                if (s->pSampleMask) {
                    // (rasterizationSamples + 31) / 32 u32 words; sane cap 8.
                    uint32_t words = (s->rasterizationSamples + 31u) / 32u;
                    if (words == 0 || words > 8) words = 1;
                    uint32_t* a = reinterpret_cast<uint32_t*>(st.bytes(words * 4u, 4));
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(s->pSampleMask), a, words * 4u);
                    s->pSampleMask = a;
                }
                g->pMultisampleState = s;
            }
            if (g->pDepthStencilState) {
                VkPipelineDepthStencilStateCreateInfoH* s = st.alloc<VkPipelineDepthStencilStateCreateInfoH>();
                read_guest_struct(mem, reinterpret_cast<uint64_t>(g->pDepthStencilState), s);
                s->pNext = nullptr;
                g->pDepthStencilState = s;
            }
            if (g->pColorBlendState) {
                VkPipelineColorBlendStateCreateInfoH* s = st.alloc<VkPipelineColorBlendStateCreateInfoH>();
                read_guest_struct(mem, reinterpret_cast<uint64_t>(g->pColorBlendState), s);
                s->pNext = nullptr;
                if (s->attachmentCount && s->pAttachments && s->attachmentCount <= 16) {
                    void* a = st.bytes(s->attachmentCount * 32u, 4);
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(s->pAttachments), a, s->attachmentCount * 32u);
                    s->pAttachments = a;
                } else { s->attachmentCount = 0; s->pAttachments = nullptr; }
                g->pColorBlendState = s;
            }
            if (g->pDynamicState) {
                VkPipelineDynamicStateCreateInfoH* s = st.alloc<VkPipelineDynamicStateCreateInfoH>();
                read_guest_struct(mem, reinterpret_cast<uint64_t>(g->pDynamicState), s);
                s->pNext = nullptr;
                if (s->dynamicStateCount && s->pDynamicStates && s->dynamicStateCount <= 32) {
                    void* a = st.bytes(s->dynamicStateCount * 4u, 4);
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(s->pDynamicStates), a, s->dynamicStateCount * 4u);
                    s->pDynamicStates = a;
                } else { s->dynamicStateCount = 0; s->pDynamicStates = nullptr; }
                g->pDynamicState = s;
            }
        }
        uint64_t* host_pipes = reinterpret_cast<uint64_t*>(st.bytes(count * 8u, 8));
        std::memset(host_pipes, 0, count * 8u);
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint32_t, const void*, const void*, void*)>(entry.host_fn)(
            cpu.regs[0], cpu.regs[1], count, infos, nullptr, host_pipes);
        if (cpu.regs[5]) {
            try { mem->write(cpu.regs[5], host_pipes, count * 8u); }
            catch (...) { /* out array unmapped — results lost */ }
        }
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) {
            fprintf(stderr, "[display-thunk] vkCreateGraphicsPipelines (n=%u) → %d (pipe0=%p)\n",
                    count, static_cast<int32_t>(ret), reinterpret_cast<void*>(host_pipes[0]));
        }
        return true;
    }

    // ── vkAllocateDescriptorSets ─────────────────────────────────────────
    // (device, pAllocateInfo, pDescriptorSets) — pSetLayouts nested handle
    // array; OUT pDescriptorSets receives the allocated set handles.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_ALLOC_DESCRIPTOR_SETS) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        VkDescriptorSetAllocateInfoH* info = st.alloc<VkDescriptorSetAllocateInfoH>();
        read_guest_struct(mem, cpu.regs[1], info);
        info->pNext = nullptr;
        if (info->descriptorSetCount && info->pSetLayouts && info->descriptorSetCount <= 32) {
            uint64_t* a = reinterpret_cast<uint64_t*>(st.bytes(info->descriptorSetCount * 8u, 8));
            read_guest_bytes(mem, reinterpret_cast<uint64_t>(info->pSetLayouts), a,
                             info->descriptorSetCount * 8u);
            info->pSetLayouts = a;
        } else { info->descriptorSetCount = 0; info->pSetLayouts = nullptr; }
        uint64_t* host_sets = reinterpret_cast<uint64_t*>(st.bytes(info->descriptorSetCount * 8u, 8));
        std::memset(host_sets, 0, info->descriptorSetCount * 8u);
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, const void*, void*)>(entry.host_fn)(
            cpu.regs[0], info, host_sets);
        if (cpu.regs[2] && ret == 0) {
            try { mem->write(cpu.regs[2], host_sets, info->descriptorSetCount * 8u); }
            catch (...) { /* out array unmapped */ }
        }
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) fprintf(stderr, "[display-thunk] vkAllocateDescriptorSets (n=%u) → %d\n",
                           info->descriptorSetCount, static_cast<int32_t>(ret));
        return true;
    }

    // ── vkUpdateDescriptorSets ───────────────────────────────────────────
    // (device, writeCount, pWrites, copyCount, pCopies) — each write carries
    // ONE of pImageInfo / pBufferInfo / pTexelBufferView depending on
    // descriptorType; copies are flat-after-pNext (handles round-trip).
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_UPDATE_DESCRIPTOR_SETS) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint32_t writes = static_cast<uint32_t>(cpu.regs[1]);
        const uint32_t copies = static_cast<uint32_t>(cpu.regs[3]);
        if (writes > 32 || copies > 32) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        VkWriteDescriptorSetH* w = nullptr;
        if (writes && cpu.regs[2]) {
            w = reinterpret_cast<VkWriteDescriptorSetH*>(st.bytes(writes * sizeof(VkWriteDescriptorSetH), 8));
            read_guest_bytes(mem, cpu.regs[2], w, writes * sizeof(VkWriteDescriptorSetH));
            for (uint32_t i = 0; i < writes; i++) {
                w[i].pNext = nullptr;
                if (w[i].descriptorCount > 64) w[i].descriptorCount = 0;
                if (!w[i].descriptorCount) {
                    w[i].pImageInfo = nullptr; w[i].pBufferInfo = nullptr; w[i].pTexelBufferView = nullptr;
                    continue;
                }
                // DescriptorType classification (vendored enum values):
                // image infos → 1 COMBINED_IMAGE_SAMPLER, 2 SAMPLED_IMAGE,
                // 3 STORAGE_IMAGE, 10 INPUT_ATTACHMENT; texel buffer views
                // → 4 UNIFORM_TEXEL_BUFFER, 5 STORAGE_TEXEL_BUFFER; buffer
                // infos → 6/7/8/9 (UNIFORM/STORAGE_[BUFFER_DYNAMIC]) and
                // any exotic type (best effort). DO NOT use a 1..6 range
                // for images — 6 is UNIFORM_BUFFER and 4/5 are texel
                // buffers; that misroute nulled the info pointers and
                // crashed RADV on the first UBO write.
                uint32_t t = w[i].descriptorType;
                bool is_img = t == 1 || t == 2 || t == 3 || t == 10;
                bool is_tbuf = t == 4 || t == 5;
                if (is_img && w[i].pImageInfo) {
                    VkDescriptorImageInfoH* a = reinterpret_cast<VkDescriptorImageInfoH*>(
                        st.bytes(w[i].descriptorCount * sizeof(VkDescriptorImageInfoH), 8));
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(w[i].pImageInfo), a,
                                     w[i].descriptorCount * sizeof(VkDescriptorImageInfoH));
                    w[i].pImageInfo = a;
                } else { w[i].pImageInfo = nullptr; }
                if (!is_img && !is_tbuf && w[i].pBufferInfo) {
                    VkDescriptorBufferInfoH* a = reinterpret_cast<VkDescriptorBufferInfoH*>(
                        st.bytes(w[i].descriptorCount * sizeof(VkDescriptorBufferInfoH), 8));
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(w[i].pBufferInfo), a,
                                     w[i].descriptorCount * sizeof(VkDescriptorBufferInfoH));
                    w[i].pBufferInfo = a;
                } else { w[i].pBufferInfo = nullptr; }
                if (is_tbuf && w[i].pTexelBufferView) {
                    uint64_t* a = reinterpret_cast<uint64_t*>(st.bytes(w[i].descriptorCount * 8u, 8));
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(w[i].pTexelBufferView), a,
                                     w[i].descriptorCount * 8u);
                    w[i].pTexelBufferView = a;
                } else { w[i].pTexelBufferView = nullptr; }
            }
        }
        VkCopyDescriptorSetH* c = nullptr;
        if (copies && cpu.regs[4]) {
            c = reinterpret_cast<VkCopyDescriptorSetH*>(st.bytes(copies * sizeof(VkCopyDescriptorSetH), 8));
            read_guest_bytes(mem, cpu.regs[4], c, copies * sizeof(VkCopyDescriptorSetH));
            for (uint32_t i = 0; i < copies; i++) c[i].pNext = nullptr;
        }
        reinterpret_cast<uint64_t (*)(uint64_t, uint32_t, const void*, uint32_t, const void*)>(entry.host_fn)(
            cpu.regs[0], writes, w, copies, c);
        cpu.regs[0] = 0;
        if (trace) fprintf(stderr, "[display-thunk] vkUpdateDescriptorSets (w=%u c=%u) → 0\n", writes, copies);
        return true;
    }

    // ── vkAllocateMemory ────────────────────────────────────────────────
    // (device, pAllocateInfo, pAllocator, pMemory) — records the allocation
    // size so later VK_WHOLE_SIZE maps can resolve. OUT handle at arg 3.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_ALLOC_MEMORY) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        VkMemoryAllocateInfoH* info = st.alloc<VkMemoryAllocateInfoH>();
        read_guest_struct(mem, cpu.regs[1], info);
        info->pNext = nullptr;
        size_t alloc_size = info->allocationSize;
        uint64_t host_mem = 0;
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, const void*, const void*, void*)>(entry.host_fn)(
            cpu.regs[0], info, nullptr, &host_mem);
        if (ret == 0 && host_mem) {
            try { mem->write(cpu.regs[3], &host_mem, sizeof(host_mem)); }
            catch (...) { /* out pointer unmapped */ }
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            impl_->vk_allocs_[host_mem] = alloc_size;
        }
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) fprintf(stderr, "[display-thunk] vkAllocateMemory (%zu bytes) → %d\n",
                           alloc_size, static_cast<int32_t>(ret));
        return true;
    }

    // ── vkFreeMemory ─────────────────────────────────────────────────────
    // (device, memory, pAllocator) — drops our records; if the memory is
    // still mapped (spec violation, but be defensive like glDeleteBuffers),
    // push the bounce back and release it before freeing.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_FREE_MEMORY) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint64_t mem_handle = cpu.regs[1];
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            auto it = impl_->vk_maps_.find(mem_handle);
            if (it != impl_->vk_maps_.end()) {
                uint8_t* src = mem->guest_to_host_ptr(it->second.bounce);
                if (src && it->second.host_ptr && it->second.map_size)
                    std::memcpy(reinterpret_cast<void*>(it->second.host_ptr), src, it->second.map_size);
                mem->untrack_allocation(it->second.bounce,
                                        (it->second.map_size + 0xFFFu) & ~0xFFFull);
                impl_->vk_maps_.erase(it);
            }
            impl_->vk_allocs_.erase(mem_handle);
        }
        reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, const void*)>(entry.host_fn)(
            cpu.regs[0], mem_handle, nullptr);
        cpu.regs[0] = 0;
        if (trace) fprintf(stderr, "[display-thunk] vkFreeMemory → 0\n");
        return true;
    }

    // ── vkMapMemory ──────────────────────────────────────────────────────
    // (device, memory, offset, size, flags, ppData) — the host driver writes
    // a HOST pointer into ppData, useless in the guest. Instead: map on the
    // host with a scratch pointer, allocate a bounce inside the 4 GiB direct
    // window, seed it from the host mapping, and write the BOUNCE's guest
    // address into ppData. Guest reads/writes then hit the window at full
    // JIT speed; coherence comes from the submit/present push + wait pull
    // (and explicit flush/invalidate ranges).
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_MAP_MEMORY) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint64_t mem_handle = cpu.regs[1];
        const uint64_t offset = cpu.regs[2];
        uint64_t size = cpu.regs[3];
        const uint64_t flags = cpu.regs[4];
        // Resolve VK_WHOLE_SIZE from the recorded allocation size.
        uint64_t alloc_size = 0;
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            auto ai = impl_->vk_allocs_.find(mem_handle);
            if (ai != impl_->vk_allocs_.end()) alloc_size = ai->second;
        }
        if (size == ~0ull) {
            if (alloc_size == 0 || offset >= alloc_size) {
                cpu.regs[0] = 0xFFFFFFFBu;  // VK_ERROR_MEMORY_MAP_FAILED
                return true;
            }
            size = alloc_size - offset;
        }
        // Remap of an already-mapped object: Vulkan returns the same
        // pointer — return the existing bounce (same offset/size shape).
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            auto it = impl_->vk_maps_.find(mem_handle);
            if (it != impl_->vk_maps_.end()) {
                uint64_t same = it->second.bounce;
                if (cpu.regs[5]) {
                    try { mem->write(cpu.regs[5], &same, sizeof(same)); }
                    catch (...) { /* out pointer unmapped */ }
                }
                cpu.regs[0] = 0;
                return true;
            }
        }
        if (size == 0 || size > (1ull << 30)) {  // sanity cap: 1 GiB per map
            cpu.regs[0] = 0xFFFFFFFBu;
            return true;
        }
        void* host_ptr = nullptr;
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, void**)>(entry.host_fn)(
            cpu.regs[0], mem_handle, offset, size, flags, &host_ptr);
        if (ret != 0 || !host_ptr) {
            cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
            if (trace) fprintf(stderr, "[display-thunk] vkMapMemory → %d (FAILED)\n",
                               static_cast<int32_t>(ret));
            return true;
        }
        uint64_t bounce = mem->mmap_alloc(size);
        if (bounce == 0 || bounce == ~0ull) {
            // No window space — unmap on the host and report failure.
            reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(entry.host_fn)(cpu.regs[0], mem_handle);
            cpu.regs[0] = 0xFFFFFFFBu;
            return true;
        }
        uint8_t* dst = mem->guest_to_host_ptr(bounce);
        std::memcpy(dst, host_ptr, size);  // seed: host → bounce
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            impl_->vk_maps_[mem_handle] = { reinterpret_cast<uint64_t>(host_ptr),
                                            bounce, offset, size, alloc_size };
        }
        if (cpu.regs[5]) {
            try { mem->write(cpu.regs[5], &bounce, sizeof(bounce)); }
            catch (...) { /* out pointer unmapped — address lost */ }
        }
        cpu.regs[0] = 0;
        if (trace) fprintf(stderr, "[display-thunk] vkMapMemory (off=0x%llx sz=%llu) → 0 (guest ptr=0x%llx)\n",
                           (unsigned long long)offset, (unsigned long long)size,
                           (unsigned long long)bounce);
        return true;
    }

    // ── vkUnmapMemory ────────────────────────────────────────────────────
    // (device, memory) — push the whole bounce back, release the window
    // range, then unmap on the host.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_UNMAP_MEMORY) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint64_t mem_handle = cpu.regs[1];
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            auto it = impl_->vk_maps_.find(mem_handle);
            if (it != impl_->vk_maps_.end()) {
                uint8_t* src = mem->guest_to_host_ptr(it->second.bounce);
                if (src && it->second.host_ptr && it->second.map_size)
                    std::memcpy(reinterpret_cast<void*>(it->second.host_ptr), src, it->second.map_size);
                mem->untrack_allocation(it->second.bounce,
                                        (it->second.map_size + 0xFFFu) & ~0xFFFull);
                impl_->vk_maps_.erase(it);
            }
        }
        reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(entry.host_fn)(cpu.regs[0], mem_handle);
        cpu.regs[0] = 0;
        if (trace) fprintf(stderr, "[display-thunk] vkUnmapMemory → 0\n");
        return true;
    }

    // ── vkFlushMappedMemoryRanges ────────────────────────────────────────
    // (device, rangeCount, pRanges) — VkMappedMemoryRange is flat; move each
    // range bounce → host BEFORE the host flush (which makes non-coherent
    // memory visible to the GPU).
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_FLUSH_MAPPED) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint32_t count = static_cast<uint32_t>(cpu.regs[1]);
        if (count == 0 || !cpu.regs[2]) { cpu.regs[0] = 0; return true; }
        if (count > 64) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        VkMappedMemoryRangeH* rr = reinterpret_cast<VkMappedMemoryRangeH*>(
            st.bytes(count * sizeof(VkMappedMemoryRangeH), 8));
        read_guest_bytes(mem, cpu.regs[2], rr, count * sizeof(VkMappedMemoryRangeH));
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            for (uint32_t i = 0; i < count; i++) {
                rr[i].pNext = nullptr;
                auto it = impl_->vk_maps_.find(rr[i].memory);
                if (it == impl_->vk_maps_.end()) continue;
                const auto& m = it->second;
                uint64_t off = rr[i].offset;
                uint64_t sz = rr[i].size;
                if (sz == ~0ull) sz = m.map_size - (off - m.map_offset);
                if (off < m.map_offset) continue;
                off -= m.map_offset;
                if (off >= m.map_size || off + sz > m.map_size) continue;
                uint8_t* src = mem->guest_to_host_ptr(m.bounce + off);
                if (src) std::memcpy(reinterpret_cast<void*>(m.host_ptr + off), src, sz);
            }
        }
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, uint32_t, const void*)>(entry.host_fn)(
            cpu.regs[0], count, rr);
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) fprintf(stderr, "[display-thunk] vkFlushMappedMemoryRanges (n=%u) → %d\n",
                           count, static_cast<int32_t>(ret));
        return true;
    }

    // ── vkInvalidateMappedMemoryRanges ───────────────────────────────────
    // (device, rangeCount, pRanges) — host invalidate FIRST (pulls GPU
    // writes into the host mapping), then copy host → bounce so the guest
    // sees them.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_INVALIDATE_MAPPED) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint32_t count = static_cast<uint32_t>(cpu.regs[1]);
        if (count == 0 || !cpu.regs[2]) { cpu.regs[0] = 0; return true; }
        if (count > 64) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        VkMappedMemoryRangeH* rr = reinterpret_cast<VkMappedMemoryRangeH*>(
            st.bytes(count * sizeof(VkMappedMemoryRangeH), 8));
        read_guest_bytes(mem, cpu.regs[2], rr, count * sizeof(VkMappedMemoryRangeH));
        for (uint32_t i = 0; i < count; i++) rr[i].pNext = nullptr;
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, uint32_t, const void*)>(entry.host_fn)(
            cpu.regs[0], count, rr);
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            for (uint32_t i = 0; i < count; i++) {
                auto it = impl_->vk_maps_.find(rr[i].memory);
                if (it == impl_->vk_maps_.end()) continue;
                const auto& m = it->second;
                uint64_t off = rr[i].offset;
                uint64_t sz = rr[i].size;
                if (sz == ~0ull) sz = m.map_size - (off - m.map_offset);
                if (off < m.map_offset) continue;
                off -= m.map_offset;
                if (off >= m.map_size || off + sz > m.map_size) continue;
                uint8_t* dst = mem->guest_to_host_ptr(m.bounce + off);
                if (dst) std::memcpy(dst, reinterpret_cast<const void*>(m.host_ptr + off), sz);
            }
        }
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) fprintf(stderr, "[display-thunk] vkInvalidateMappedMemoryRanges (n=%u) → %d\n",
                           count, static_cast<int32_t>(ret));
        return true;
    }

    // ── Completion waits: pull host mappings into the bounces ────────────
    // vkDeviceWaitIdle / vkQueueWaitIdle / vkWaitForFences — after a
    // successful wait the GPU may have written readback data into the host
    // mappings; refresh the bounces so the guest sees it. These symbols
    // otherwise forward verbatim (plain int args).
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_SYNC_PULL) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        uint64_t ret;
        if (entry.spec->args[0] == 'p') {
            // vkWaitForFences(device, fenceCount, pFences, waitAll, timeout)
            // — bounce the fence handle array, then wait.
            VkStage st;
            const uint32_t nf = static_cast<uint32_t>(cpu.regs[1]);
            if (nf && cpu.regs[2] && nf <= 64) {
                uint64_t* f = reinterpret_cast<uint64_t*>(st.bytes(nf * 8u, 8));
                read_guest_bytes(mem, cpu.regs[2], f, nf * 8u);
                ret = reinterpret_cast<uint64_t (*)(uint64_t, uint32_t, const void*, uint32_t, uint64_t)>(entry.host_fn)(
                    cpu.regs[0], nf, f, static_cast<uint32_t>(cpu.regs[3]), cpu.regs[4]);
            } else {
                ret = reinterpret_cast<uint64_t (*)(uint64_t, uint32_t, const void*, uint32_t, uint64_t)>(entry.host_fn)(
                    cpu.regs[0], 0, nullptr, static_cast<uint32_t>(cpu.regs[3]), cpu.regs[4]);
            }
        } else {
            ret = reinterpret_cast<uint64_t (*)(uint64_t)>(entry.host_fn)(cpu.regs[0]);
        }
        if (ret == 0) impl_->vk_sync_pull_all();
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) fprintf(stderr, "[display-thunk] wait+pull → %d\n", static_cast<int32_t>(ret));
        return true;
    }

    // ── vkGetPhysicalDeviceProperties2 / Features2 (+KHR aliases) ─────
    // The pNext CHAIN carries guest pointers (driver/subgroup/accel
    // properties, subgroup/BDA features) that the host driver derefs.
    // Walk the chain by sType, copy each known node into staging,
    // re-link host-side, call, then write every node back (the driver
    // FILLS them). Unknown sType truncates the chain there (safe: the
    // caller sees the node untouched instead of garbage).
    if (entry.spec &&
        entry.name.rfind("vkGetPhysicalDevice", 0) == 0 &&
        (entry.name.find("Properties2") != std::string::npos ||
         entry.name.find("Features2") != std::string::npos)) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        uint64_t g = cpu.regs[1];
        if (!g) { cpu.regs[0] = 0xFFFFFFFFFFFFFFFEull; return true; }
        VkStage st;
        VkPnextNode nodes[kMaxPnextNodes];
        uint32_t nnodes = vk_marshal_pnext_chain(mem, st, g, nodes);
        if (!nnodes) return false;  // not our shape — generic path
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, const void*)>(entry.host_fn)(
            cpu.regs[0], nodes[0].host);
        vk_writeback_pnext_chain(mem, nodes, nnodes);
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace)
            fprintf(stderr, "[display-thunk] %s → %d (%u chain nodes)\n",
                    entry.name.c_str(), static_cast<int32_t>(ret), nnodes);
        return true;
    }

    // ── vkEnumerateDeviceExtensionProperties: hide RT extensions ──────
    // Ray tracing needs deep-marshal arms that don't exist yet
    // (vkCmdBuildAccelerationStructures* carry nested geometry pointer
    // trees). Guests that see VK_KHR_ray_query demand its entry points
    // and Sys_Error when vkGetDeviceProcAddr misses — so filter the RT
    // extensions out of the reported list; apps fall back to non-RT
    // rendering paths. Remove this once build-acceleration marshalling
    // lands.
    // ── vkCreateDebugUtilsMessengerEXT — INTERCEPTED, never forwarded ──
    // The create info embeds pfnUserCallback, a GUEST function pointer.
    // Forwarding the struct to the host driver would hand it a raw guest
    // address to call as x86 (the glDebugMessageCallback rule: callbacks
    // never cross). We accept the messenger and mint a fake handle; debug
    // messages simply never fire.
    if (entry.spec && entry.name == "vkCreateDebugUtilsMessengerEXT") {
        static uint64_t dbg_messenger_ctr = 0;
        const uint64_t handle =
            0xD6C0FFEE00000000ull + ++dbg_messenger_ctr;
        if (cpu.regs[3])
            mem->write(cpu.regs[3], &handle, sizeof(handle));
        cpu.regs[0] = 0;   // VK_SUCCESS
        if (trace) {
            fprintf(stderr,
                    "[display-thunk] vkCreateDebugUtilsMessengerEXT → "
                    "intercepted (handle=0x%llx, callbacks suppressed)\n",
                    (unsigned long long)handle);
        }
        return true;
    }
    if (entry.spec && entry.name == "vkDestroyDebugUtilsMessengerEXT") {
        cpu.regs[0] = 0;   // accepted no-op
        return true;
    }
    if (entry.spec && entry.name == "vkEnumerateDeviceExtensionProperties") {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        // Declarative policy (plan C3): advertise ⟺ implemented.
        // RT needs acceleration-structure build marshalling; descriptor
        // buffer needs a whole bindless model. Guests that don't see
        // the strings take non-RT / conventional-binding paths.
        static const char* kHiddenExt[] = {
            "VK_KHR_ray_query", "VK_KHR_acceleration_structure",
            "VK_EXT_descriptor_buffer" };
        auto hidden = [](const char* n) {
            for (const char* h : kHiddenExt)
                if (std::strncmp(n, h, sizeof("VK_KHR_acceleration_structure")) == 0)
                    return true;
            return false;
        };
        uint32_t count = 0;
        if (!cpu.regs[2]) { cpu.regs[0] = 0xFFFFFFFFFFFFFFFEull; return true; }
        try { mem->read(cpu.regs[2], &count, 4); } catch (...) { count = 0; }
        VkStage st;
        uint32_t actual = 0;
        // VkExtensionProperties { char name[256]; uint32 specVersion; }
        constexpr uint32_t kExtPropSize = 260;
        uint8_t* props = nullptr;
        if (cpu.regs[3] && count && count <= 1024) {
            props = reinterpret_cast<uint8_t*>(st.bytes(count * kExtPropSize, 4));
            auto fn = reinterpret_cast<uint64_t (*)(uint64_t, const void*, uint32_t*, void*)>(entry.host_fn);
            uint32_t tmp = count;
            uint64_t ret = fn(cpu.regs[0], nullptr, &tmp, props);
            actual = tmp;
            // Filter in place.
            uint32_t w = 0;
            for (uint32_t i = 0; i < actual; i++) {
                const char* nm = reinterpret_cast<const char*>(props + i * kExtPropSize);
                if (hidden(nm)) continue;
                if (w != i) std::memcpy(props + w * kExtPropSize,
                                        props + i * kExtPropSize, kExtPropSize);
                w++;
            }
            try { mem->write(cpu.regs[2], &w, 4); } catch (...) {}
            try { mem->write(cpu.regs[3], props, w * kExtPropSize); } catch (...) {}
            cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
            return true;
        }
        // Count-only query: forward verbatim (hiding only matters when
        // the guest then fetches names).
        auto fn2 = reinterpret_cast<uint64_t (*)(uint64_t, const void*, uint32_t*, void*)>(entry.host_fn);
        uint32_t tmp = count;
        uint64_t ret = fn2(cpu.regs[0], nullptr, &tmp, nullptr);
        try { mem->write(cpu.regs[2], &tmp, 4); } catch (...) {}
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        return true;
    }

    // ── vkBeginCommandBuffer ─────────────────────────────────────────
    // (cmdBuffer, pBeginInfo) — VkCommandBufferBeginInfo is 32 bytes:
    // {sType, pNext, flags, pInheritanceInfo}. Secondary buffers
    // (multithreaded recording) chain a GUEST VkCommandBufferInheritanceInfo
    // (56 bytes: handles + pNext) that the driver derefs.
    if (entry.spec && entry.name == "vkBeginCommandBuffer") {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        uint8_t* top = reinterpret_cast<uint8_t*>(st.bytes(32, 8));
        read_guest_bytes(mem, cpu.regs[1], top, 32);
        VkPnextNode nodes[kMaxPnextNodes];
        uint32_t dn = vk_marshal_pnext_chain(
            mem, st, *reinterpret_cast<uint64_t*>(top + 8), nodes);
        *reinterpret_cast<void**>(top + 8) = dn ? nodes[0].host : nullptr;
        VkPnextNode inodes[kMaxPnextNodes];
        uint32_t din = 0;
        uint64_t ginh = *reinterpret_cast<uint64_t*>(top + 24);
        if (ginh) {
            uint8_t* inh = reinterpret_cast<uint8_t*>(st.bytes(56, 8));
            read_guest_bytes(mem, ginh, inh, 56);
            din = vk_marshal_pnext_chain(
                mem, st, *reinterpret_cast<uint64_t*>(inh + 8), inodes);
            *reinterpret_cast<void**>(inh + 8) = din ? inodes[0].host : nullptr;
            // Re-point the begin info at the HOST copy — the raw guest
            // pointer must never reach the driver.
            *reinterpret_cast<void**>(top + 24) = inh;
        }
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, const void*)>(
            entry.host_fn)(cpu.regs[0], top);
        vk_writeback_pnext_chain(mem, nodes, dn);
        vk_writeback_pnext_chain(mem, inodes, din);
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace)
            fprintf(stderr, "[display-thunk] vkBeginCommandBuffer → %d\n",
                    static_cast<int32_t>(ret));
        return true;
    }

    // ── vkCreateComputePipelines ─────────────────────────────────────
    // (device, pipelineCache, createInfoCount, pCreateInfos, pAllocator,
    //  pPipelines) — each VkComputePipelineCreateInfo embeds a shader
    // stage with a GUEST pName string and optional specialization info
    // (map entries + data blob). Mirror of the graphics-pipeline stage
    // marshalling. OUT handles written back to arg 5.
    if (entry.spec && entry.name == "vkCreateComputePipelines") {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint32_t count = static_cast<uint32_t>(cpu.regs[2]);
        if (count == 0 || count > 64 || !cpu.regs[3]) {
            cpu.regs[0] = count == 0 ? 0u : 0xFFFFFFFDu;
            return true;
        }
        VkStage st;
        st.buf.reserve(256u * 1024u);
        VkComputePipelineCreateInfoH* infos =
            reinterpret_cast<VkComputePipelineCreateInfoH*>(
                st.bytes(count * sizeof(VkComputePipelineCreateInfoH), 8));
        read_guest_bytes(mem, cpu.regs[3], infos,
                         count * sizeof(VkComputePipelineCreateInfoH));
        for (uint32_t p = 0; p < count; p++) {
            VkComputePipelineCreateInfoH* c = &infos[p];
            c->pNext = nullptr;
            c->stage.pNext = nullptr;
            c->stage.pName = st.guest_str(mem, reinterpret_cast<uint64_t>(c->stage.pName));
            if (c->stage.pSpecializationInfo) {
                VkSpecializationInfoH gi_spec;
                read_guest_struct(mem, reinterpret_cast<uint64_t>(c->stage.pSpecializationInfo), &gi_spec);
                VkSpecializationInfoH* spec = st.alloc<VkSpecializationInfoH>();
                *spec = gi_spec;
                if (gi_spec.mapEntryCount && gi_spec.pMapEntries && gi_spec.mapEntryCount <= 256) {
                    VkSpecializationMapEntryH* e = reinterpret_cast<VkSpecializationMapEntryH*>(
                        st.bytes(gi_spec.mapEntryCount * sizeof(VkSpecializationMapEntryH), 8));
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(gi_spec.pMapEntries), e,
                                     gi_spec.mapEntryCount * sizeof(VkSpecializationMapEntryH));
                    spec->pMapEntries = e;
                } else { spec->mapEntryCount = 0; spec->pMapEntries = nullptr; }
                if (gi_spec.dataSize && gi_spec.pData && gi_spec.dataSize <= 4096) {
                    void* d = st.bytes(gi_spec.dataSize, 8);
                    read_guest_bytes(mem, reinterpret_cast<uint64_t>(gi_spec.pData), d, gi_spec.dataSize);
                    spec->pData = d;
                } else { spec->dataSize = 0; spec->pData = nullptr; }
                c->stage.pSpecializationInfo = spec;
            }
        }
        uint64_t* pipes = reinterpret_cast<uint64_t*>(st.bytes(count * 8u, 8));
        std::memset(pipes, 0, count * 8u);
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint32_t,
                                                     const void*, const void*, void*)>(
            entry.host_fn)(cpu.regs[0], cpu.regs[1], count, infos, nullptr, pipes);
        if (cpu.regs[5]) {
            try { mem->write(cpu.regs[5], pipes, count * 8u); } catch (...) {}
        }
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace)
            fprintf(stderr, "[display-thunk] vkCreateComputePipelines(n=%u) → %d\n",
                    count, static_cast<int32_t>(ret));
        return true;
    }

    // Not one of the deep-marshalling entry points — let the generic path
    // handle it with the corrected pointer masks.
    return false;
}
// Called when a symbol has the THUNK_PROXY flag. The DisplayProxy provides
// a SDL2-based software fallback for X11/Wayland functions when the host
// libraries are unavailable or have no display.
uint64_t DisplayThunk::proxy_dispatch_(CPU& cpu, const std::string& sym_name) {
    if (!impl_->proxy_) {
        cpu.regs[0] = 0;
        return 0;
    }
    // Lazy-init: only create the SDL2 window when an X11/Wayland call is
    // actually made. This avoids SDL_Init interfering with the emulator's
    // signal handlers for tests that don't use display functions.
    if (!impl_->proxy_->ready()) {
        impl_->proxy_->init(640, 480, impl_->mem);
    }
    if (!impl_->proxy_->ready()) {
        cpu.regs[0] = 0;
        return 0;
    }
    DisplayProxy* proxy = impl_->proxy_.get();
    Memory* mem = impl_->mem;
    bool trace = dbg().thunk_trace;

    // Helper: translate a guest pointer arg to a host pointer.
    auto g2h = [&](uint64_t guest_addr) -> void* {
        if (!guest_addr || !mem) return nullptr;
        uint8_t* hp = mem->guest_to_host_ptr(guest_addr);
        return hp ? reinterpret_cast<void*>(hp) : nullptr;
    };
    auto g2h_str = [&](uint64_t guest_addr) -> const char* {
        return reinterpret_cast<const char*>(g2h(guest_addr));
    };

    if (sym_name == "XOpenDisplay") {
        const char* name = g2h_str(cpu.regs[0]);
        cpu.regs[0] = proxy->XOpenDisplay(name);
        return 0;
    }
    if (sym_name == "XCloseDisplay") {
        cpu.regs[0] = proxy->XCloseDisplay(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XCreateSimpleWindow") {
        // 9 args: Display*, Window, int, int, unsigned, unsigned, unsigned,
        // unsigned long, unsigned long. Arg 8 (background) is on the stack.
        uint64_t bg = 0;
        if (mem && cpu.sp) {
            mem->read(cpu.sp, &bg, sizeof(uint64_t));
        }
        cpu.regs[0] = proxy->XCreateSimpleWindow(
            cpu.regs[0], cpu.regs[1],
            static_cast<int>(cpu.regs[2]), static_cast<int>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]),
            static_cast<int>(cpu.regs[6]),
            static_cast<unsigned long>(cpu.regs[7]),
            static_cast<unsigned long>(bg));
        return 0;
    }
    if (sym_name == "XCreateWindow") {
        // 12 args: Display*, Window, int, int, unsigned, unsigned, unsigned,
        // int, unsigned long, Visual*, unsigned long, XSetWindowAttributes*.
        // Args 8-11 are on the stack.
        uint64_t stack_args[4] = {0};
        if (mem && cpu.sp) {
            mem->read(cpu.sp, stack_args, sizeof(stack_args));
        }
        cpu.regs[0] = proxy->XCreateWindow(
            cpu.regs[0], cpu.regs[1],
            static_cast<int>(cpu.regs[2]), static_cast<int>(cpu.regs[3]),
            static_cast<unsigned>(cpu.regs[4]), static_cast<unsigned>(cpu.regs[5]),
            static_cast<unsigned>(cpu.regs[6]),
            static_cast<int>(cpu.regs[7]),
            static_cast<unsigned long>(stack_args[0]),
            static_cast<uint64_t>(stack_args[1]),
            static_cast<unsigned long>(stack_args[2]),
            g2h(stack_args[3]));
        return 0;
    }
    if (sym_name == "XDestroyWindow") {
        cpu.regs[0] = proxy->XDestroyWindow(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XMapWindow") {
        cpu.regs[0] = proxy->XMapWindow(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XUnmapWindow") {
        cpu.regs[0] = proxy->XUnmapWindow(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XFlush") {
        cpu.regs[0] = proxy->XFlush(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XSync") {
        cpu.regs[0] = proxy->XSync(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XFillRectangle") {
        proxy->XFillRectangle(
            cpu.regs[0], cpu.regs[1],
            static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            static_cast<unsigned>(cpu.regs[5]), static_cast<unsigned>(cpu.regs[6]));
        cpu.regs[0] = 1;
        return 0;
    }
    if (sym_name == "XDrawRectangle") {
        proxy->XDrawRectangle(
            cpu.regs[0], cpu.regs[1],
            static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            static_cast<unsigned>(cpu.regs[5]), static_cast<unsigned>(cpu.regs[6]));
        cpu.regs[0] = 1;
        return 0;
    }
    if (sym_name == "XDrawLine") {
        proxy->XDrawLine(
            cpu.regs[0], cpu.regs[1],
            static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            static_cast<int>(cpu.regs[5]), static_cast<int>(cpu.regs[6]));
        cpu.regs[0] = 1;
        return 0;
    }
    if (sym_name == "XDrawPoint") {
        proxy->XDrawPoint(
            cpu.regs[0], cpu.regs[1],
            static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]));
        cpu.regs[0] = 1;
        return 0;
    }
    if (sym_name == "XSetForeground") {
        cpu.regs[0] = proxy->XSetForeground(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned long>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XSetBackground") {
        cpu.regs[0] = proxy->XSetBackground(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned long>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XCreateGC") {
        // 4 args: Display*, Drawable, unsigned long, XGCValues* — all in
        // x0..x3 (the extra screen/visual proxy params are unused).
        cpu.regs[0] = proxy->XCreateGC(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned long>(cpu.regs[2]),
            g2h(cpu.regs[3]), 0, 0);
        return 0;
    }
    if (sym_name == "XFreeGC") {
        cpu.regs[0] = proxy->XFreeGC(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XStoreName") {
        const char* name = g2h_str(cpu.regs[2]);
        cpu.regs[0] = proxy->XStoreName(cpu.regs[0], cpu.regs[1], name);
        return 0;
    }
    if (sym_name == "XGetWindowAttributes") {
        cpu.regs[0] = proxy->XGetWindowAttributes(
            cpu.regs[0], cpu.regs[1], g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XSelectInput") {
        cpu.regs[0] = proxy->XSelectInput(
            cpu.regs[0], cpu.regs[1], static_cast<long>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XInternAtom") {
        const char* name = g2h_str(cpu.regs[1]);
        cpu.regs[0] = proxy->XInternAtom(
            cpu.regs[0], name, static_cast<int>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XSetWMProtocols") {
        // 4 args: Display*, Window, Atom*, int — all in x0..x3.
        cpu.regs[0] = proxy->XSetWMProtocols(
            cpu.regs[0], cpu.regs[1], g2h(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XGetAtomName") {
        unsigned long host_ptr = proxy->XGetAtomName(cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]));
        const char* host_str = reinterpret_cast<const char*>(host_ptr);
        cpu.regs[0] = host_str ? impl_->cache_host_string_(host_str) : 0;
        return 0;
    }
    if (sym_name == "XPending") {
        cpu.regs[0] = proxy->XPending(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XNextEvent") {
        cpu.regs[0] = proxy->XNextEvent(cpu.regs[0], g2h(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XCheckMaskEvent") {
        cpu.regs[0] = proxy->XCheckMaskEvent(
            cpu.regs[0], static_cast<long>(cpu.regs[1]), g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XEventsQueued") {
        cpu.regs[0] = proxy->XEventsQueued(
            cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XDisplayWidth") {
        cpu.regs[0] = proxy->XDisplayWidth(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XDisplayHeight") {
        cpu.regs[0] = proxy->XDisplayHeight(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XDisplayWidthMM") {
        cpu.regs[0] = proxy->XDisplayWidthMM(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XDisplayHeightMM") {
        cpu.regs[0] = proxy->XDisplayHeightMM(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "DefaultRootWindow") {
        cpu.regs[0] = proxy->DefaultRootWindow(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "BlackPixel") {
        cpu.regs[0] = proxy->BlackPixel(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "WhitePixel") {
        cpu.regs[0] = proxy->WhitePixel(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XSetWindowBackground") {
        cpu.regs[0] = proxy->XSetWindowBackground(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned long>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XCreateColormap") {
        cpu.regs[0] = proxy->XCreateColormap(
            cpu.regs[0], cpu.regs[1], static_cast<uint64_t>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XFreeColormap") {
        cpu.regs[0] = proxy->XFreeColormap(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XAllocColor") {
        cpu.regs[0] = proxy->XAllocColor(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]), g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XSetLineAttributes") {
        // 6 args: Display*, GC, unsigned, unsigned, int, int — x0..x5.
        cpu.regs[0] = proxy->XSetLineAttributes(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned>(cpu.regs[2]), static_cast<unsigned>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]));
        return 0;
    }
    if (sym_name == "XDrawString") {
        const char* str = g2h_str(cpu.regs[5]);
        proxy->XDrawString(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            str, static_cast<int>(cpu.regs[6]));
        cpu.regs[0] = 1;
        return 0;
    }
    if (sym_name == "XQueryPointer") {
        uint64_t mask_ptr = 0;
        if (mem && cpu.sp) {
            mem->read(cpu.sp, &mask_ptr, sizeof(uint64_t));
        }
        cpu.regs[0] = proxy->XQueryPointer(
            cpu.regs[0], cpu.regs[1], g2h(cpu.regs[2]), g2h(cpu.regs[3]),
            g2h(cpu.regs[4]), g2h(cpu.regs[5]), g2h(cpu.regs[6]), g2h(cpu.regs[7]),
            g2h(mask_ptr));
        return 0;
    }
    if (sym_name == "XWarpPointer") {
        uint64_t dest_y = 0;
        if (mem && cpu.sp) {
            mem->read(cpu.sp, &dest_y, sizeof(uint64_t));
        }
        cpu.regs[0] = proxy->XWarpPointer(
            cpu.regs[0], cpu.regs[1], cpu.regs[2],
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            static_cast<int>(cpu.regs[5]), static_cast<int>(cpu.regs[6]),
            static_cast<int>(cpu.regs[7]), static_cast<int>(dest_y));
        return 0;
    }
    if (sym_name == "XBell") {
        cpu.regs[0] = proxy->XBell(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XScreenCount") {
        cpu.regs[0] = proxy->XScreenCount(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XSetInputFocus") {
        cpu.regs[0] = proxy->XSetInputFocus(
            cpu.regs[0], static_cast<uint64_t>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]), static_cast<uint64_t>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XGetInputFocus") {
        cpu.regs[0] = proxy->XGetInputFocus(
            cpu.regs[0], g2h(cpu.regs[1]), g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XChangeProperty") {
        // 8 args: Display*, Window, Atom, Atom, int, int, const unsigned char*,
        // long — all in x0..x7 (data = x6, nelements = x7).
        cpu.regs[0] = proxy->XChangeProperty(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned long>(cpu.regs[2]),
            static_cast<unsigned long>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]),
            g2h(cpu.regs[6]), static_cast<long>(cpu.regs[7]));
        return 0;
    }
    if (sym_name == "XDeleteProperty") {
        cpu.regs[0] = proxy->XDeleteProperty(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned long>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XCopyArea") {
        uint64_t stack_args[2] = {0};
        if (mem && cpu.sp) {
            mem->read(cpu.sp, stack_args, sizeof(stack_args));
        }
        cpu.regs[0] = proxy->XCopyArea(
            cpu.regs[0], cpu.regs[1], cpu.regs[2], static_cast<unsigned long>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]),
            static_cast<int>(cpu.regs[6]), static_cast<int>(cpu.regs[7]),
            static_cast<int>(stack_args[0]), static_cast<int>(stack_args[1]));
        return 0;
    }
    if (sym_name == "XCreatePixmap") {
        cpu.regs[0] = proxy->XCreatePixmap(
            cpu.regs[0], cpu.regs[1],
            static_cast<int>(cpu.regs[2]), static_cast<int>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]));
        return 0;
    }
    if (sym_name == "XFreePixmap") {
        cpu.regs[0] = proxy->XFreePixmap(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XSetWindowBackgroundPixmap") {
        cpu.regs[0] = proxy->XSetWindowBackgroundPixmap(
            cpu.regs[0], cpu.regs[1], cpu.regs[2]);
        return 0;
    }
    if (sym_name == "XSetClipMask") {
        cpu.regs[0] = proxy->XSetClipMask(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]), cpu.regs[2]);
        return 0;
    }
    if (sym_name == "XSetClipOrigin") {
        cpu.regs[0] = proxy->XSetClipOrigin(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]), static_cast<int>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XCopyGC") {
        // 4 args: Display*, GC, unsigned long, GC — all in x0..x3.
        cpu.regs[0] = proxy->XCopyGC(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned long>(cpu.regs[2]), cpu.regs[3]);
        return 0;
    }
    if (sym_name == "XChangeGC") {
        // 4 args: Display*, GC, unsigned long, XGCValues* — all in x0..x3.
        cpu.regs[0] = proxy->XChangeGC(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<unsigned long>(cpu.regs[2]), g2h(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XSetFunction") {
        cpu.regs[0] = proxy->XSetFunction(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XSetDashes") {
        const char* dashes = g2h_str(cpu.regs[3]);
        cpu.regs[0] = proxy->XSetDashes(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]), dashes);
        return 0;
    }
    if (sym_name == "XFreeColors") {
        cpu.regs[0] = proxy->XFreeColors(
            cpu.regs[0], static_cast<unsigned long>(cpu.regs[1]),
            reinterpret_cast<const unsigned long*>(g2h(cpu.regs[2])), static_cast<int>(cpu.regs[3]),
            static_cast<unsigned long>(cpu.regs[4]));
        return 0;
    }
    if (sym_name == "XDrawImageString") {
        const char* str = g2h_str(cpu.regs[5]);
        proxy->XDrawImageString(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned long>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            str, static_cast<int>(cpu.regs[6]));
        cpu.regs[0] = 1;
        return 0;
    }
    // Wayland proxy methods
    if (sym_name == "wl_display_connect") {
        const char* name = g2h_str(cpu.regs[0]);
        cpu.regs[0] = proxy->wl_display_connect(name);
        return 0;
    }
    if (sym_name == "wl_display_disconnect") {
        proxy->wl_display_disconnect(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "wl_surface_create") {
        const char* interface = g2h_str(cpu.regs[1]);
        cpu.regs[0] = proxy->wl_surface_create(
            cpu.regs[0], interface, static_cast<uint32_t>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "wl_surface_commit") {
        proxy->wl_surface_commit(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "wl_surface_destroy") {
        proxy->wl_surface_destroy(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "wl_egl_window_create") {
        cpu.regs[0] = proxy->wl_egl_window_create(
            cpu.regs[0], static_cast<int>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "wl_egl_window_destroy") {
        cpu.regs[0] = proxy->wl_egl_window_destroy(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "wl_egl_window_get_attached_size") {
        cpu.regs[0] = proxy->wl_egl_window_get_attached_size(
            cpu.regs[0], g2h(cpu.regs[1]), g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "wl_egl_window_resize") {
        cpu.regs[0] = proxy->wl_egl_window_resize(
            cpu.regs[0], static_cast<int>(cpu.regs[1]),
            static_cast<int>(cpu.regs[2]), static_cast<int>(cpu.regs[3]),
            static_cast<int>(cpu.regs[4]));
        return 0;
    }
    // ── XShm proxy ──────────────────────────────────────────────────
    if (sym_name == "XShmQueryExtension") {
        cpu.regs[0] = proxy->XShmQueryExtension(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XShmGetEventBase") {
        cpu.regs[0] = proxy->XShmGetEventBase(cpu.regs[0]);
        return 0;
    }
    if (sym_name == "XShmCreateImage") {
        cpu.regs[0] = proxy->XShmCreateImage(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned>(cpu.regs[2]),
            static_cast<int>(cpu.regs[3]), g2h(cpu.regs[4]), g2h(cpu.regs[5]),
            static_cast<unsigned>(cpu.regs[6]), static_cast<unsigned>(cpu.regs[7]));
        return 0;
    }
    if (sym_name == "XShmAttach") {
        cpu.regs[0] = proxy->XShmAttach(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XShmDetach") {
        cpu.regs[0] = proxy->XShmDetach(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XShmPutImage") {
        // 11 args: args 8-10 (src_width, src_height, send_event) on stack.
        uint64_t stack_args[3] = {0};
        if (impl_->mem && cpu.sp) {
            impl_->mem->read(cpu.sp, stack_args, sizeof(stack_args));
        }
        cpu.regs[0] = proxy->XShmPutImage(
            cpu.regs[0], cpu.regs[1], cpu.regs[2], cpu.regs[3],
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]),
            static_cast<int>(cpu.regs[6]), static_cast<int>(cpu.regs[7]),
            static_cast<unsigned>(stack_args[0]),
            static_cast<unsigned>(stack_args[1]),
            static_cast<bool>(stack_args[2]));
        return 0;
    }
    if (sym_name == "XShmGetImage") {
        cpu.regs[0] = proxy->XShmGetImage(
            cpu.regs[0], cpu.regs[1], cpu.regs[2],
            static_cast<int>(cpu.regs[3]), static_cast<int>(cpu.regs[4]),
            static_cast<unsigned>(cpu.regs[5]), static_cast<unsigned>(cpu.regs[6]),
            static_cast<unsigned long>(cpu.regs[7]));
        return 0;
    }
    // ── GLX proxy ───────────────────────────────────────────────────
    if (sym_name == "glXChooseVisual") {
        cpu.regs[0] = proxy->glXChooseVisual(
            cpu.regs[0], static_cast<int>(cpu.regs[1]),
            reinterpret_cast<const int*>(g2h(cpu.regs[2])));
        return 0;
    }
    if (sym_name == "glXCreateContext") {
        cpu.regs[0] = proxy->glXCreateContext(
            cpu.regs[0], cpu.regs[1], cpu.regs[2], static_cast<int>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "glXDestroyContext") {
        cpu.regs[0] = proxy->glXDestroyContext(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "glXMakeCurrent") {
        cpu.regs[0] = proxy->glXMakeCurrent(
            cpu.regs[0], cpu.regs[1], cpu.regs[2]);
        return 0;
    }
    if (sym_name == "glXSwapBuffers") {
        proxy->glXSwapBuffers(cpu.regs[0], cpu.regs[1]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "glXGetClientString") {
        const char* s = proxy->glXGetClientString(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        cpu.regs[0] = s ? impl_->cache_host_string_(s) : 0;
        return 0;
    }
    if (sym_name == "glXQueryExtensionsString") {
        const char* s = proxy->glXQueryExtensionsString(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        cpu.regs[0] = s ? impl_->cache_host_string_(s) : 0;
        return 0;
    }
    if (sym_name == "glXQueryServerString") {
        const char* s = proxy->glXQueryServerString(cpu.regs[0], static_cast<int>(cpu.regs[1]), static_cast<int>(cpu.regs[2]));
        cpu.regs[0] = s ? impl_->cache_host_string_(s) : 0;
        return 0;
    }
    if (sym_name == "glXGetFBConfigs") {
        int nelements = 0;
        uint64_t fbconfigs = proxy->glXGetFBConfigs(cpu.regs[0], static_cast<int>(cpu.regs[1]), &nelements);
        if (cpu.regs[2] && impl_->mem) {
            impl_->mem->write(cpu.regs[2], &nelements, sizeof(nelements));
        }
        cpu.regs[0] = fbconfigs;
        return 0;
    }
    if (sym_name == "glXGetFBConfigAttrib") {
        int value = 0;
        cpu.regs[0] = proxy->glXGetFBConfigAttrib(
            cpu.regs[0], cpu.regs[1], static_cast<int>(cpu.regs[2]),
            cpu.regs[3] ? &value : nullptr);
        if (cpu.regs[3] && impl_->mem) {
            impl_->mem->write(cpu.regs[3], &value, sizeof(value));
        }
        return 0;
    }
    if (sym_name == "glXCreateWindow") {
        cpu.regs[0] = proxy->glXCreateWindow(
            cpu.regs[0], cpu.regs[1], cpu.regs[2],
            reinterpret_cast<const int*>(g2h(cpu.regs[3])));
        return 0;
    }
    if (sym_name == "glXDestroyWindow") {
        cpu.regs[0] = proxy->glXDestroyWindow(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "glXCreatePbuffer") {
        cpu.regs[0] = proxy->glXCreatePbuffer(
            cpu.regs[0], cpu.regs[1],
            reinterpret_cast<const int*>(g2h(cpu.regs[2])));
        return 0;
    }
    if (sym_name == "glXDestroyPbuffer") {
        cpu.regs[0] = proxy->glXDestroyPbuffer(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    // ── XRandR proxy ────────────────────────────────────────────────
    if (sym_name == "XRRGetScreenResources") {
        cpu.regs[0] = proxy->XRRGetScreenResources(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XRRGetScreenResourcesCurrent") {
        cpu.regs[0] = proxy->XRRGetScreenResourcesCurrent(cpu.regs[0], cpu.regs[1]);
        return 0;
    }
    if (sym_name == "XRRFreeScreenResources") {
        proxy->XRRFreeScreenResources(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "XRRGetCrtcInfo") {
        cpu.regs[0] = proxy->XRRGetCrtcInfo(cpu.regs[0], cpu.regs[1], cpu.regs[2]);
        return 0;
    }
    if (sym_name == "XRRFreeCrtcInfo") {
        proxy->XRRFreeCrtcInfo(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "XRRGetOutputInfo") {
        cpu.regs[0] = proxy->XRRGetOutputInfo(cpu.regs[0], cpu.regs[1], cpu.regs[2]);
        return 0;
    }
    if (sym_name == "XRRFreeOutputInfo") {
        proxy->XRRFreeOutputInfo(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (sym_name == "XRRSetCrtcConfig") {
        // 10 args: x0..x7 + 2 stack args (outputs, noutputs).
        uint64_t outputs = 0;
        int32_t noutputs = 0;
        if (impl_->mem && cpu.sp) {
            impl_->mem->read(cpu.sp, &outputs, sizeof(uint64_t));
            impl_->mem->read(cpu.sp + 8, &noutputs, sizeof(int32_t));
        }
        cpu.regs[0] = proxy->XRRSetCrtcConfig(
            cpu.regs[0], cpu.regs[1], cpu.regs[2], cpu.regs[3],
            static_cast<int>(cpu.regs[4]), static_cast<int>(cpu.regs[5]),
            cpu.regs[6], static_cast<unsigned>(cpu.regs[7]),
            outputs, noutputs);
        return 0;
    }
    if (sym_name == "XRRGetScreenSizeRange") {
        int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
        cpu.regs[0] = proxy->XRRGetScreenSizeRange(
            cpu.regs[0], static_cast<int>(cpu.regs[1]),
            cpu.regs[2] ? &min_w : nullptr,
            cpu.regs[3] ? &min_h : nullptr,
            cpu.regs[4] ? &max_w : nullptr,
            cpu.regs[5] ? &max_h : nullptr);
        if (cpu.regs[2] && impl_->mem) impl_->mem->write(cpu.regs[2], &min_w, sizeof(min_w));
        if (cpu.regs[3] && impl_->mem) impl_->mem->write(cpu.regs[3], &min_h, sizeof(min_h));
        if (cpu.regs[4] && impl_->mem) impl_->mem->write(cpu.regs[4], &max_w, sizeof(max_w));
        if (cpu.regs[5] && impl_->mem) impl_->mem->write(cpu.regs[5], &max_h, sizeof(max_h));
        return 0;
    }
    // ── Xkb proxy ───────────────────────────────────────────────────
    if (sym_name == "XkbOpenDevice") {
        cpu.regs[0] = proxy->XkbOpenDevice(cpu.regs[0], static_cast<int>(cpu.regs[1]));
        return 0;
    }
    if (sym_name == "XkbGetMap") {
        cpu.regs[0] = proxy->XkbGetMap(cpu.regs[0], cpu.regs[1], static_cast<unsigned>(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XkbGetState") {
        cpu.regs[0] = proxy->XkbGetState(cpu.regs[0], cpu.regs[1], g2h(cpu.regs[2]));
        return 0;
    }
    if (sym_name == "XkbSetState") {
        cpu.regs[0] = proxy->XkbSetState(cpu.regs[0], cpu.regs[1], static_cast<unsigned>(cpu.regs[2]), g2h(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XkbSetAutoRepeatRate") {
        cpu.regs[0] = proxy->XkbSetAutoRepeatRate(
            cpu.regs[0], cpu.regs[1], static_cast<unsigned>(cpu.regs[2]),
            static_cast<unsigned>(cpu.regs[3]));
        return 0;
    }
    if (sym_name == "XkbGetAutoRepeatRate") {
        unsigned int delay = 0, interval = 0;
        cpu.regs[0] = proxy->XkbGetAutoRepeatRate(
            cpu.regs[0], cpu.regs[1],
            cpu.regs[2] ? &delay : nullptr,
            cpu.regs[3] ? &interval : nullptr);
        if (cpu.regs[2] && impl_->mem) impl_->mem->write(cpu.regs[2], &delay, sizeof(delay));
        if (cpu.regs[3] && impl_->mem) impl_->mem->write(cpu.regs[3], &interval, sizeof(interval));
        return 0;
    }
    if (sym_name == "XkbFreeKeyboard") {
        proxy->XkbFreeKeyboard(cpu.regs[0]);
        cpu.regs[0] = 0;
        return 0;
    }
    if (trace) {
        fprintf(stderr, "[display-thunk] proxy_dispatch: unknown symbol '%s'\n",
                sym_name.c_str());
    }
    cpu.regs[0] = 0;
    return 0;
}
size_t DisplayThunk::symbol_count() const {
    if (!impl_) return 0;
    return impl_->id_to_idx_.size();
}
uint64_t DisplayThunk::trampoline_base() const {
    if (!impl_) return 0;
    return impl_->trampoline_base;
}
// ── register_known_symbols_ ────────────────────────────────────────────
// 1.5.4-alpha: TABLE-DRIVEN. tools/opgen/thunk_dp.txt (generated into
// include/opgen_thunk.hpp) is now the single source of truth for which
// (library, symbol) DisplayThunk thunks and how it marshals each one.
// The former REG_VK*/REG_WL*/REG_X11*/REG_GBM*/REG_GLX*/REG_RANDR*/REG_XKB*
// macro ladders (275 registrations) lived as hand-written code that
// drifted from the dispatch masks and had no drift guard; they are gone.
// This loop iterates the table, keeps only the DisplayThunk families
// (VK/WL/WL_EGL/X11/X11XCB/XCB/GBM/XEXT/GLX/RANDR/XKB), derives the legacy
// ABI-shape fields (pointer_args / n_stack / n_float / flags) from the
// ARGS column, and registers each symbol under every soname of its family
// — the same pattern GraphicThunk's loop uses. Adding a symbol is now a
// one-line spec row, and `make opgen-thunk-check` fails CI if the spec
// and the generated header drift.
//
// Pointer-arg semantics: 'p'/'z' in ARGS marks a translated pointer (the
// identity/bounce path), 'i' marks a VERBATIM arg. Opaque Vk* / wl_* /
// XID / Display* handles pass VERBATIM — the guest stores the host pointer
// the host returned, so a handle round-trips back through the value it
// already holds; only true pointer args (structs / string arrays / output
// pointers) get the translation. ARGS length > 8 implies stack args
// (n_stack = len - 8). The derived masks reproduce the pre-migration
// registration EXACTLY, with one fix: XCreateWindow now carries all 12
// args (n_stack 4), so its XSetWindowAttributes* (arg 11, a stack pointer)
// is actually read and translated instead of being silently dropped.
void DisplayThunk::register_known_symbols_() {
    struct FamilyDef {
        const char* const* sonames;
        uint32_t n;
        void* handle;  // dlopen'd host handle (null = host lib unavailable)
    };
    static const char* kVkSonames[]   = {"libvulkan.so.1", "libvulkan.so"};
    static const char* kWlSonames[]   = {"libwayland-client.so.0", "libwayland-client.so"};
    static const char* kWlEglSonames[] = {"libwayland-egl.so.1", "libwayland-egl.so"};
    static const char* kX11Sonames[]  = {"libX11.so.6", "libX11.so"};
    static const char* kX11XcbSonames[] = {"libX11-xcb.so.1", "libX11-xcb.so"};
    static const char* kXcbSonames[]  = {"libxcb.so.1", "libxcb.so"};
    static const char* kGbmSonames[]  = {"libgbm.so.1", "libgbm.so"};
    static const char* kXextSonames[] = {"libXext.so.6", "libXext.so"};
    static const char* kGlxSonames[]  = {"libGLX.so.2", "libGLX.so"};
    static const char* kRandrSonames[] = {"libXrandr.so.2", "libXrandr.so"};
    static const char* kXkbSonames[]  = {"libXkblib.so", "libX11-xcb.so"};
    static const char* kAndroidSonames[] = {"libandroid.so", "liblog.so"};
    // Indexed by (LibFamily - LibFamily::VK). The generator emits the
    // display families contiguously after the GraphicThunk families, so
    // VK is the first DisplayThunk family.
    static FamilyDef kFamilies[] = {
        {kVkSonames, 2, nullptr},    // VK
        {kWlSonames, 2, nullptr},    // WL
        {kWlEglSonames, 2, nullptr}, // WL_EGL
        {kX11Sonames, 2, nullptr},   // X11
        {kX11XcbSonames, 2, nullptr},// X11XCB
        {kXcbSonames, 2, nullptr},   // XCB
        {kGbmSonames, 2, nullptr},   // GBM
        {kXextSonames, 2, nullptr},  // XEXT
        {kGlxSonames, 2, nullptr},   // GLX
        {kRandrSonames, 2, nullptr}, // RANDR
        {kXkbSonames, 2, nullptr},   // XKB
        {kAndroidSonames, 2, nullptr}, // ANDROID (host lib never present —
                                       // the dispatch arm owns everything)
    };

    constexpr int kFirstDisplayFamily = static_cast<int>(thunk::LibFamily::VK);

    for (const thunk::Spec& spec : thunk::specs) {
        const int fam = static_cast<int>(spec.lib) - kFirstDisplayFamily;
        if (fam < 0 || fam >= static_cast<int>(sizeof(kFamilies) / sizeof(kFamilies[0]))) {
            continue;  // GraphicThunk's families (GL/GLES/EGL/SDL/GLFW) or UNKNOWN
        }
        FamilyDef& fd = kFamilies[fam];
        // Lazy host-library load: first soname, then the second.
        if (!fd.handle) {
            fd.handle = dlopen(fd.sonames[0], RTLD_LAZY);
            if (!fd.handle) fd.handle = dlopen(fd.sonames[1], RTLD_LAZY);
        }
        void* host_fn = fd.handle ? dlsym(fd.handle, spec.name) : nullptr;
        // GetProcAddress must stay non-null: dispatch returns the
        // trampoline before calling it, but a null host_fn short-circuits
        // to 0 first.
        if (spec.policy == thunk::Policy::VK_GET_PROC && !host_fn) {
            host_fn = reinterpret_cast<void*>(1);
        }

        // Derive the legacy ABI-shape fields from the ARGS column so the
        // dispatcher's proxy/vulkan/generic paths keep working unchanged.
        uint16_t pointer_args = 0;
        uint8_t n_float = 0, n_double = 0, n_int = 0, n_args = 0;
        for (const char* a = spec.args; *a; ++a, ++n_args) {
            switch (*a) {
            case 'p': case 'z':
                pointer_args |= static_cast<uint16_t>(1u << n_args);
                break;
            case 'f': n_float++; break;
            case 'd': n_double++; break;
            default:  n_int++; break;
            }
        }
        uint8_t n_stack = 0;
        uint8_t flags = 0;
        if (n_double > 0) {
            // Double-only AAPCS64 ABI: args in d0..d{n-1}; n_float carries
            // the double count, THUNK_DOUBLE selects the double read path.
            flags |= DisplayThunk::THUNK_DOUBLE;
            n_float = static_cast<uint8_t>(n_double);
        } else if (n_float > 0 && n_int > 0) {
            // Mixed int+float ABI: n_stack holds the integer arity (x0..).
            flags |= THUNK_MIXED_FP;
            n_stack = static_cast<uint8_t>(n_int);
        } else if (n_float > 0) {
            n_stack = 0;
        } else if (n_args > 8) {
            // Args 8+ live on the guest stack at SP (AAPCS64).
            n_stack = static_cast<uint8_t>(n_args - 8);
        }
        switch (spec.policy) {
        case thunk::Policy::PROXY:
            flags |= THUNK_PROXY;
            break;
        case thunk::Policy::ANDROID_WINDOW:
            flags |= THUNK_ANDROID_WINDOW;
            break;
        case thunk::Policy::VK_GET_PROC:
            flags |= THUNK_VULKAN | THUNK_GET_PROC;
            break;
        case thunk::Policy::VK_CREATE_INSTANCE:
        case thunk::Policy::VK_CREATE_DEVICE:
        case thunk::Policy::VK_PRESENT:
        case thunk::Policy::VK_SUBMIT:
        case thunk::Policy::VK_CREATE_GRAPHICS_PIPELINES:
        case thunk::Policy::VK_ALLOC_DESCRIPTOR_SETS:
        case thunk::Policy::VK_UPDATE_DESCRIPTOR_SETS:
        case thunk::Policy::VK_ALLOC_MEMORY:
        case thunk::Policy::VK_FREE_MEMORY:
        case thunk::Policy::VK_MAP_MEMORY:
        case thunk::Policy::VK_UNMAP_MEMORY:
        case thunk::Policy::VK_FLUSH_MAPPED:
        case thunk::Policy::VK_INVALIDATE_MAPPED:
        case thunk::Policy::VK_SYNC_PULL:
        case thunk::Policy::VULKAN:
            flags |= THUNK_VULKAN;
            break;
        default:
            break;
        }
        if (spec.ret == thunk::RetKind::STRING) flags |= THUNK_RET_STRING;

        for (uint32_t i = 0; i < fd.n; i++) {
            register_function_(fd.sonames[i], spec.name, host_fn,
                               pointer_args, n_stack, n_float, flags, &spec);
        }
    }
}

} // namespace arm64emu
