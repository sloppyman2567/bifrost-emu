// frost_graphics/display_thunk.cpp — DisplayThunk implementation (v1.5.0.alpha).
//
// See include/frost/display_thunk.hpp for the design overview. This file
// implements the DisplayThunk class for Vulkan / Wayland / X11 / GBM.
//
// v1.5.0.alpha: DisplayThunk now uses the same full dispatch logic as
// GraphicThunk (stack args, float args, string returns, GetProcAddress,
// mixed int+float) and integrates DisplayProxy for X11/Wayland fallback
// when host libraries are unavailable.
#include "frost/display_thunk.hpp"
#include "frost/thunk.hpp"  // for SYSCALL_NUMBER
#include "frost/display_proxy.hpp"
#include "thunk_common.hpp" // shared SymbolEntry (single definition — see header)
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
    // Vulkan handle table: guest VkInstance/VkDevice → host handle.
    std::unordered_map<uint64_t, uint64_t> vk_handle_map_;
    // DisplayProxy for X11/Wayland fallback (v1.5.0.alpha).
    std::unique_ptr<DisplayProxy> proxy_;
    // Guest-visible scratch page for host→guest string returns
    // (XGetAtomName, glGetString, …). Ring-allocated.
    uint64_t string_cache_base = 0;
    static constexpr uint64_t STRING_CACHE_SIZE = 4096;
    uint32_t string_cache_off = 0;
    std::mutex mu;
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
    // v1.5.0.alpha: display thunking enabled by default.
    // Set BIFROST_NO_THUNK_DISPLAY=1 to disable.
    const char* disable = getenv("BIFROST_NO_THUNK_DISPLAY");
    impl_->enabled = !(disable && disable[0] != '0');
    if (impl_->enabled) {
        if (getenv("BIFROST_THUNK_TRACE") || getenv("BIFROST_VERBOSE")) {
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
    // v1.5.0.alpha: lazily create the DisplayProxy. It owns an SDL2 window
    // and provides a software fallback for X11/Wayland calls when the host
    // libraries are unavailable or have no display.
    impl_->proxy_ = std::make_unique<DisplayProxy>();
    impl_->proxy_->set_memory(&mem);
    register_known_symbols_();
    impl_->initialized = true;
    if (getenv("BIFROST_THUNK_TRACE")) {
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
void DisplayThunk::register_function_(const std::string& lib,
                                        const std::string& sym,
                                        void* host_fn,
                                        uint16_t pointer_args,
                                        uint8_t n_stack,
                                        uint8_t n_float,
                                        uint8_t flags) {
    bool trace = (getenv("BIFROST_THUNK_TRACE") != nullptr);
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
                           n_stack, n_float, flags});
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
    bool trace = (getenv("BIFROST_THUNK_TRACE") != nullptr);

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
        // small guest object (mirrors the SizeKind sizing in thunk.cpp).
        size_t kBounce = 65536;
        const std::string& n = entry.name;
        if (n == "XChangeProperty" && idx == 6) {
            // (display, window, prop, type, format, mode, data, nelements)
            uint64_t sz = args[7] * (args[4] / 8u);
            if (sz > 0 && sz < (64ull << 20)) kBounce = static_cast<size_t>(sz);
        } else if ((n == "XDrawString" || n == "XDrawImageString") && idx == 5) {
            uint64_t sz = args[6];
            if (sz > 0 && sz < (64ull << 20)) kBounce = static_cast<size_t>(sz);
        } else if (n == "XSetWMProtocols" && idx == 2) {
            uint64_t sz = args[3] * sizeof(unsigned long);  // Atom = 8 bytes
            if (sz > 0 && sz < (64ull << 20)) kBounce = static_cast<size_t>(sz);
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

    if (entry.pointer_args && impl_->mem) {
        for (int i = 0; i < kMaxArgs; i++) {
            if (entry.pointer_args & (1u << i)) {
                translate_ptr(args[i], i, &bounce_bufs[i],
                              &bounce_pristine[i],
                              &bounce_guest[i], &bounce_wb[i]);
            }
        }
    }

    if (trace) {
        fprintf(stderr, "[display-thunk] dispatch: %s (host_fn=%p) "
                "a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx "
                "a8=0x%llx ptrs=0x%x stack=%u\n",
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
// ── proxy_dispatch_ — route X11/Wayland calls to DisplayProxy ──────────
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
    bool trace = (getenv("BIFROST_THUNK_TRACE") != nullptr);

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
void DisplayThunk::register_known_symbols_() {
    // ── libvulkan.so.1 ─────────────────────────────────────────────
    const char* vk_libs[] = {"libvulkan.so.1", "libvulkan.so"};
    void* vk_handle = dlopen("libvulkan.so.1", RTLD_LAZY);
    if (!vk_handle) vk_handle = dlopen("libvulkan.so", RTLD_LAZY);
    #define REG_VK(name) do { \
        void* p = vk_handle ? dlsym(vk_handle, #name) : nullptr; \
        for (const char* L : vk_libs) register_function_(L, #name, p); \
    } while(0)
    #define REG_VK_PTR(name, ptrs) do { \
        void* p = vk_handle ? dlsym(vk_handle, #name) : nullptr; \
        for (const char* L : vk_libs) register_function_(L, #name, p, ptrs); \
    } while(0)
    #define REG_VK_PROC(name) do { \
        void* p = vk_handle ? dlsym(vk_handle, #name) : nullptr; \
        if (!p) p = reinterpret_cast<void*>(1); \
        for (const char* L : vk_libs) \
            register_function_(L, #name, p, 0x01, 0, 0, THUNK_GET_PROC); \
    } while(0)
    // Core instance/device functions.
    REG_VK_PTR(vkCreateInstance, 0x03);         // arg 0: pCreateInfo, arg 1: pAllocator
    REG_VK_PTR(vkDestroyInstance, 0x02);        // arg 1: pAllocator
    REG_VK_PTR(vkEnumeratePhysicalDevices, 0x06); // arg 1: pCount, arg 2: pPhysicalDevices
    REG_VK_PTR(vkGetPhysicalDeviceProperties, 0x02); // arg 1: pProperties (large struct)
    REG_VK_PTR(vkGetPhysicalDeviceFeatures, 0x02); // arg 1: pFeatures
    REG_VK_PTR(vkGetPhysicalDeviceMemoryProperties, 0x02); // arg 1: pMemoryProperties
    REG_VK_PTR(vkGetPhysicalDeviceQueueFamilyProperties, 0x06); // arg 1: pCount, arg 2: pQueueFamilyProperties
    REG_VK_PTR(vkCreateDevice, 0x07);           // arg 0: pCreateInfo, arg 2: ppDevice, arg 3: pAllocator
    REG_VK_PTR(vkDestroyDevice, 0x02);          // arg 1: pAllocator
    REG_VK_PTR(vkGetDeviceQueue, 0x06);          // arg 2: ppQueue, arg 3: pAllocator
    REG_VK(vkDeviceWaitIdle);
    REG_VK_PTR(vkQueueWaitIdle, 0x02);          // arg 1: pFence (actually VkQueue, but opaque)
    // Swapchain (KHR extension).
    REG_VK_PTR(vkCreateSwapchainKHR, 0x07);     // arg 0: pCreateInfo, arg 2: ppSwapchain, arg 3: pAllocator
    REG_VK_PTR(vkDestroySwapchainKHR, 0x02);    // arg 1: pAllocator
    REG_VK_PTR(vkGetSwapchainImagesKHR, 0x06);  // arg 1: pCount, arg 2: pImages
    REG_VK_PTR(vkAcquireNextImageKHR, 0x1E);    // arg 3: pImageIndex, arg 4: pAcquireFence
    REG_VK_PTR(vkQueuePresentKHR, 0x02);         // arg 1: pPresentInfo
    // Command buffers.
    REG_VK_PTR(vkCreateCommandPool, 0x06);       // arg 0: pCreateInfo, arg 2: ppCommandPool, arg 3: pAllocator
    REG_VK_PTR(vkDestroyCommandPool, 0x02);      // arg 1: pAllocator
    REG_VK_PTR(vkAllocateCommandBuffers, 0x06);  // arg 0: pAllocateInfo, arg 1: pCommandBuffers
    REG_VK_PTR(vkFreeCommandBuffers, 0x02);      // arg 1: pCommandBuffers
    REG_VK_PTR(vkBeginCommandBuffer, 0x02);      // arg 1: pBeginInfo
    REG_VK(vkEndCommandBuffer);
    REG_VK(vkResetCommandBuffer);
    REG_VK_PTR(vkQueueSubmit, 0x06);             // arg 1: pSubmitInfo, arg 3: pFence
    // Image / image views.
    REG_VK_PTR(vkCreateImage, 0x06);             // arg 0: pCreateInfo, arg 2: ppImage, arg 3: pAllocator
    REG_VK_PTR(vkDestroyImage, 0x02);            // arg 1: pAllocator
    REG_VK_PTR(vkGetImageMemoryRequirements, 0x02); // arg 1: pMemoryRequirements
    REG_VK_PTR(vkBindImageMemory, 0x06);         // arg 2: pMemory, arg 3: pBindInfo
    REG_VK_PTR(vkCreateImageView, 0x06);         // arg 0: pCreateInfo, arg 2: ppImageView, arg 3: pAllocator
    REG_VK_PTR(vkDestroyImageView, 0x02);        // arg 1: pAllocator
    // Buffers.
    REG_VK_PTR(vkCreateBuffer, 0x06);            // arg 0: pCreateInfo, arg 2: ppBuffer, arg 3: pAllocator
    REG_VK_PTR(vkDestroyBuffer, 0x02);           // arg 1: pAllocator
    REG_VK_PTR(vkGetBufferMemoryRequirements, 0x02); // arg 1: pMemoryRequirements
    REG_VK_PTR(vkBindBufferMemory, 0x06);        // arg 2: pMemory, arg 3: pBindInfo
    // Memory.
    REG_VK_PTR(vkAllocateMemory, 0x06);          // arg 0: pAllocateInfo, arg 2: ppDeviceMemory, arg 3: pAllocator
    REG_VK_PTR(vkFreeMemory, 0x02);              // arg 1: pAllocator
    REG_VK_PTR(vkMapMemory, 0x0E);               // arg 2: pOffset, arg 3: pSize, arg 4: ppData
    REG_VK(vkUnmapMemory);
    REG_VK_PTR(vkFlushMappedMemoryRanges, 0x02); // arg 0: pMemoryRange
    REG_VK_PTR(vkInvalidateMappedMemoryRanges, 0x02); // arg 0: pMemoryRange
    // Render pass / framebuffers.
    REG_VK_PTR(vkCreateRenderPass, 0x06);        // arg 0: pCreateInfo, arg 2: ppRenderPass, arg 3: pAllocator
    REG_VK_PTR(vkDestroyRenderPass, 0x02);       // arg 1: pAllocator
    REG_VK_PTR(vkCreateFramebuffer, 0x06);       // arg 0: pCreateInfo, arg 2: ppFramebuffer, arg 3: pAllocator
    REG_VK_PTR(vkDestroyFramebuffer, 0x02);     // arg 1: pAllocator
    // Shaders / pipelines.
    REG_VK_PTR(vkCreateShaderModule, 0x06);      // arg 0: pCreateInfo, arg 2: ppShaderModule, arg 3: pAllocator
    REG_VK_PTR(vkDestroyShaderModule, 0x02);    // arg 1: pAllocator
    REG_VK_PTR(vkCreatePipelineCache, 0x06);     // arg 0: pCreateInfo, arg 2: ppPipelineCache, arg 3: pAllocator
    REG_VK_PTR(vkDestroyPipelineCache, 0x02);   // arg 1: pAllocator
    REG_VK_PTR(vkCreateGraphicsPipelines, 0x0E); // arg 2: pPipelines, arg 3: pPipelineCache
    REG_VK_PTR(vkCreateComputePipelines, 0x0E);  // arg 2: pPipelines, arg 3: pPipelineCache
    REG_VK_PTR(vkDestroyPipeline, 0x02);         // arg 1: pAllocator
    REG_VK_PTR(vkCreatePipelineLayout, 0x06);    // arg 0: pCreateInfo, arg 2: ppPipelineLayout, arg 3: pAllocator
    REG_VK_PTR(vkDestroyPipelineLayout, 0x02);   // arg 1: pAllocator
    REG_VK_PTR(vkCreateDescriptorSetLayout, 0x06); // arg 0: pCreateInfo, arg 2: ppLayout, arg 3: pAllocator
    REG_VK_PTR(vkDestroyDescriptorSetLayout, 0x02); // arg 1: pAllocator
    REG_VK_PTR(vkAllocateDescriptorSets, 0x02);  // arg 0: pAllocateInfo, arg 1: pDescriptorSets
    REG_VK(vkFreeDescriptorSets);
    REG_VK_PTR(vkUpdateDescriptorSets, 0x02);    // arg 0: pWriteInfo, arg 1: pCopyInfo
    REG_VK_PTR(vkCreateDescriptorPool, 0x06);     // arg 0: pCreateInfo, arg 2: ppPool, arg 3: pAllocator
    REG_VK_PTR(vkDestroyDescriptorPool, 0x02);   // arg 1: pAllocator
    // Fences / semaphores / events.
    REG_VK_PTR(vkCreateFence, 0x06);             // arg 0: pCreateInfo, arg 2: ppFence, arg 3: pAllocator
    REG_VK_PTR(vkDestroyFence, 0x02);            // arg 1: pAllocator
    REG_VK(vkResetFences);
    REG_VK_PTR(vkGetFenceStatus, 0x02);          // arg 1: pFence (actually VkFence, but opaque)
    REG_VK_PTR(vkWaitForFences, 0x06);            // arg 1: pFences, arg 2: pWaitValues, arg 3: pFlags
    REG_VK_PTR(vkCreateSemaphore, 0x06);          // arg 0: pCreateInfo, arg 2: ppSemaphore, arg 3: pAllocator
    REG_VK_PTR(vkDestroySemaphore, 0x02);        // arg 1: pAllocator
    REG_VK_PTR(vkCreateEvent, 0x06);             // arg 0: pCreateInfo, arg 2: ppEvent, arg 3: pAllocator
    REG_VK_PTR(vkDestroyEvent, 0x02);            // arg 1: pAllocator
    REG_VK(vkSetEvent);
    REG_VK(vkResetEvent);
    // Query pools.
    REG_VK_PTR(vkCreateQueryPool, 0x06);         // arg 0: pCreateInfo, arg 2: ppQueryPool, arg 3: pAllocator
    REG_VK_PTR(vkDestroyQueryPool, 0x02);        // arg 1: pAllocator
    REG_VK_PTR(vkGetQueryPoolResults, 0x0E);     // arg 1: pCount, arg 2: pData, arg 3: pFlags
    // Sampler.
    REG_VK_PTR(vkCreateSampler, 0x06);           // arg 0: pCreateInfo, arg 2: ppSampler, arg 3: pAllocator
    REG_VK_PTR(vkDestroySampler, 0x02);          // arg 1: pAllocator
    // vkGetInstanceProcAddr / vkGetDeviceProcAddr (essential for extension loading).
    REG_VK_PROC(vkGetInstanceProcAddr);
    REG_VK_PROC(vkGetDeviceProcAddr);
    #undef REG_VK
    #undef REG_VK_PTR
    #undef REG_VK_PROC
    // ── libwayland-client.so.0 ────────────────────────────────────
    // Wayland functions take pointer args (const char* name, wl_proxy*,
    // wl_listener*, void* impl, etc.) that need guest→host translation.
    const char* wl_libs[] = {"libwayland-client.so.0", "libwayland-client.so"};
    void* wl_handle = dlopen("libwayland-client.so.0", RTLD_LAZY);
    if (!wl_handle) wl_handle = dlopen("libwayland-client.so", RTLD_LAZY);
    #define REG_WL(name) do { \
        void* p = wl_handle ? dlsym(wl_handle, #name) : nullptr; \
        for (const char* L : wl_libs) register_function_(L, #name, p, 0, 0, 0, THUNK_PROXY); \
    } while(0)
    #define REG_WL_PTR(name, ptrs) do { \
        void* p = wl_handle ? dlsym(wl_handle, #name) : nullptr; \
        for (const char* L : wl_libs) register_function_(L, #name, p, ptrs, 0, 0, THUNK_PROXY); \
    } while(0)
    REG_WL_PTR(wl_display_connect, 0x01);       // arg 0: const char *name
    REG_WL(wl_display_connect_to_fd);            // arg 0: int fd (not pointer)
    REG_WL(wl_display_disconnect);               // arg 0: wl_display* (opaque handle, not translated)
    REG_WL(wl_display_get_fd);
    REG_WL(wl_display_dispatch);
    REG_WL(wl_display_dispatch_pending);
    REG_WL_PTR(wl_display_dispatch_queue, 0x02); // arg 1: wl_event_queue*
    REG_WL_PTR(wl_display_roundtrip, 0x01);      // arg 0: wl_display*
    REG_WL_PTR(wl_display_flush, 0x01);          // arg 0: wl_display*
    REG_WL_PTR(wl_display_read_events, 0x01);    // arg 0: wl_display*
    REG_WL_PTR(wl_display_prepare_read, 0x01);   // arg 0: wl_display*
    REG_WL_PTR(wl_display_cancel_read, 0x01);    // arg 0: wl_display*
    REG_WL(wl_proxy_marshal);                     // variadic — can't thunk safely
    REG_WL_PTR(wl_proxy_create, 0x01);           // arg 0: wl_proxy* (factory)
    REG_WL_PTR(wl_proxy_destroy, 0x01);          // arg 0: wl_proxy*
    REG_WL_PTR(wl_proxy_get_user_data, 0x01);    // arg 0: wl_proxy*
    REG_WL_PTR(wl_proxy_set_user_data, 0x03);    // arg 0: wl_proxy*, arg 1: void*
    REG_WL_PTR(wl_proxy_get_id, 0x01);           // arg 0: wl_proxy*
    REG_WL_PTR(wl_proxy_get_class, 0x01);        // arg 0: wl_proxy*
    REG_WL_PTR(wl_proxy_add_listener, 0x03);     // arg 0: wl_proxy*, arg 1: void** impl
    REG_WL_PTR(wl_proxy_get_listener, 0x01);     // arg 0: wl_proxy*
    REG_WL(wl_proxy_marshal_constructor);         // variadic
    REG_WL(wl_proxy_marshal_constructor_versioned); // variadic
    REG_WL_PTR(wl_proxy_set_tag, 0x03);          // arg 0: wl_proxy*, arg 1: const char**
    REG_WL_PTR(wl_proxy_get_tag, 0x01);          // arg 0: wl_proxy*
    REG_WL_PTR(wl_proxy_wrapper_destroy, 0x01);  // arg 0: wl_proxy*
    REG_WL_PTR(wl_event_queue_destroy, 0x01);    // arg 0: wl_event_queue*
    #undef REG_WL
    #undef REG_WL_PTR
    // ── libwayland-egl.so.1 ──────────────────────────────────────
    const char* wl_egl_libs[] = {"libwayland-egl.so.1", "libwayland-egl.so"};
    void* wl_egl_handle = dlopen("libwayland-egl.so.1", RTLD_LAZY);
    if (!wl_egl_handle) wl_egl_handle = dlopen("libwayland-egl.so", RTLD_LAZY);
    #define REG_WL_EGL(name) do { \
        void* p = wl_egl_handle ? dlsym(wl_egl_handle, #name) : nullptr; \
        for (const char* L : wl_egl_libs) register_function_(L, #name, p, 0, 0, 0, THUNK_PROXY); \
    } while(0)
    #define REG_WL_EGL_PTR(name, ptrs) do { \
        void* p = wl_egl_handle ? dlsym(wl_egl_handle, #name) : nullptr; \
        for (const char* L : wl_egl_libs) register_function_(L, #name, p, ptrs, 0, 0, THUNK_PROXY); \
    } while(0)
    REG_WL_EGL_PTR(wl_egl_window_create, 0x03);    // args: wl_surface*, int, int
    REG_WL_EGL_PTR(wl_egl_window_destroy, 0x01);   // arg 0: wl_egl_window*
    REG_WL_EGL_PTR(wl_egl_window_get_attached_size, 0x03); // arg 0: window, arg 1: int*, arg 2: int*
    REG_WL_EGL_PTR(wl_egl_window_resize, 0x01);    // arg 0: wl_egl_window*
    REG_WL_EGL_PTR(wl_egl_window_get_buffer_scale, 0x01);
    REG_WL_EGL_PTR(wl_egl_window_set_buffer_scale, 0x01);
    REG_WL_EGL_PTR(wl_egl_window_set_buffer_transform, 0x01);
    #undef REG_WL_EGL
    #undef REG_WL_EGL_PTR
    // ── libX11.so.6 ───────────────────────────────────────────────
    // X11 functions take pointer args (Display*, Window, GC, XEvent*,
    // char*, etc.) that need guest→host translation.
    const char* x11_libs[] = {"libX11.so.6", "libX11.so"};
    void* x11_handle = dlopen("libX11.so.6", RTLD_LAZY);
    if (!x11_handle) x11_handle = dlopen("libX11.so", RTLD_LAZY);
    #define REG_X11(name) do { \
        void* p = x11_handle ? dlsym(x11_handle, #name) : nullptr; \
        for (const char* L : x11_libs) register_function_(L, #name, p, 0, 0, 0, THUNK_PROXY); \
    } while(0)
    #define REG_X11_PTR(name, ptrs) do { \
        void* p = x11_handle ? dlsym(x11_handle, #name) : nullptr; \
        for (const char* L : x11_libs) register_function_(L, #name, p, ptrs, 0, 0, THUNK_PROXY); \
    } while(0)
    #define REG_X11_EX(name, ptrs, nstack) do { \
        void* p = x11_handle ? dlsym(x11_handle, #name) : nullptr; \
        for (const char* L : x11_libs) register_function_(L, #name, p, ptrs, nstack, 0, THUNK_PROXY); \
    } while(0)
    REG_X11_PTR(XOpenDisplay, 0x01);             // arg 0: const char* name
    REG_X11_PTR(XCloseDisplay, 0x01);            // arg 0: Display*
    REG_X11_EX(XCreateWindow, 0xA01, 3);         // args 0,9,11 ptrs; 3 stack args
    REG_X11_EX(XCreateSimpleWindow, 0x01, 1);    // arg 0: Display*; 1 stack arg (background pixel)
    REG_X11_PTR(XDestroyWindow, 0x01);           // arg 0: Display*
    REG_X11_PTR(XMapWindow, 0x01);               // arg 0: Display*
    REG_X11_PTR(XUnmapWindow, 0x01);             // arg 0: Display*
    REG_X11_PTR(XFlush, 0x01);                   // arg 0: Display*
    REG_X11_PTR(XSync, 0x01);                    // arg 0: Display*
    REG_X11_PTR(XPending, 0x01);                 // arg 0: Display*
    REG_X11_PTR(XNextEvent, 0x03);               // arg 0: Display*, arg 1: XEvent*
    REG_X11_PTR(XPeekEvent, 0x03);               // arg 0: Display*, arg 1: XEvent*
    REG_X11_PTR(XEventsQueued, 0x01);            // arg 0: Display*
    REG_X11_PTR(XWindowEvent, 0x09);             // arg 0: Display*, arg 3: XEvent*
    REG_X11_PTR(XCheckWindowEvent, 0x09);        // arg 0: Display*, arg 3: XEvent*
    REG_X11_PTR(XMaskEvent, 0x05);               // arg 0: Display*, arg 2: XEvent*
    REG_X11_PTR(XCheckMaskEvent, 0x05);          // arg 0: Display*, arg 2: XEvent*
    REG_X11_PTR(XCheckTypedEvent, 0x09);         // arg 0: Display*, arg 3: XEvent*
    REG_X11_PTR(XCheckTypedWindowEvent, 0x11);   // arg 0: Display*, arg 4: XEvent*
    REG_X11_PTR(XPutBackEvent, 0x03);            // arg 0: Display*, arg 1: XEvent*
    REG_X11_PTR(XSendEvent, 0x10);               // arg 0: Display*, arg 4: XEvent*
    REG_X11_PTR(XDisplayWidth, 0x01);            // arg 0: Display*
    REG_X11_PTR(XDisplayHeight, 0x01);           // arg 0: Display*
    REG_X11_PTR(XDisplayWidthMM, 0x01);          // arg 0: Display*
    REG_X11_PTR(XDisplayHeightMM, 0x01);         // arg 0: Display*
    REG_X11_PTR(DefaultRootWindow, 0x01);        // arg 0: Display*
    REG_X11_PTR(BlackPixel, 0x01);               // arg 0: Display*
    REG_X11_PTR(WhitePixel, 0x01);               // arg 0: Display*
    REG_X11_PTR(XSetForeground, 0x01);           // arg 0: Display*
    REG_X11_PTR(XSetBackground, 0x01);           // arg 0: Display*
    REG_X11_PTR(XFillRectangle, 0x01);           // arg 0: Display*
    REG_X11_PTR(XDrawRectangle, 0x01);           // arg 0: Display*
    REG_X11_PTR(XDrawLine, 0x01);                // arg 0: Display*
    REG_X11_PTR(XDrawPoint, 0x01);               // arg 0: Display*
    REG_X11_PTR(XCopyArea, 0x01);                // arg 0: Display*
    REG_X11_PTR(XCreateGC, 0x09);                // arg 0: Display*, arg 3: XGCValues*
    REG_X11_PTR(XFreeGC, 0x01);                  // arg 0: Display*
    REG_X11_PTR(XCreatePixmap, 0x01);            // arg 0: Display*
    REG_X11_PTR(XFreePixmap, 0x01);              // arg 0: Display*
    REG_X11_PTR(XSetWindowBackground, 0x01);     // arg 0: Display*
    REG_X11_PTR(XSetWindowBackgroundPixmap, 0x01); // arg 0: Display*
    REG_X11_PTR(XStoreName, 0x03);               // arg 0: Display*, arg 2: const char*
    REG_X11_PTR(XFetchName, 0x07);               // arg 0: Display*, arg 2: char**
    REG_X11_PTR(XSetWMProtocols, 0x04);          // arg 0: Display*, arg 2: Atom*
    REG_X11_PTR(XInternAtom, 0x03);              // arg 0: Display*, arg 1: const char*
    REG_X11_PTR(XInternAtoms, 0x13);             // arg 0: Display*, arg 1: char**, arg 4: Atom*
    REG_X11_PTR(XGetAtomName, 0x01);             // arg 0: Display*
    REG_X11_PTR(XCreateColormap, 0x01);          // arg 0: Display*
    REG_X11_PTR(XFreeColormap, 0x01);            // arg 0: Display*
    REG_X11_PTR(XAllocColor, 0x05);              // arg 0: Display*, arg 2: XColor*
    REG_X11_PTR(XFreeColors, 0x05);              // arg 0: Display*, arg 2: unsigned long*
    REG_X11_PTR(XSetClipMask, 0x01);             // arg 0: Display*
    REG_X11_PTR(XSetClipOrigin, 0x01);           // arg 0: Display*
    REG_X11_PTR(XCopyGC, 0x01);                  // arg 0: Display*
    REG_X11_PTR(XChangeGC, 0x09);                // arg 0: Display*, arg 3: XGCValues*
    REG_X11_PTR(XSetFunction, 0x01);             // arg 0: Display*
    REG_X11_PTR(XSetLineAttributes, 0x01);       // arg 0: Display*
    REG_X11_PTR(XSetDashes, 0x09);               // arg 0: Display*, arg 3: const char*
    REG_X11_PTR(XDrawString, 0x20);              // arg 0: Display*, arg 4: const char*
    REG_X11_PTR(XDrawImageString, 0x20);         // arg 0: Display*, arg 4: const char*
    REG_X11_PTR(XTextExtents, 0x3F);             // args 0,1,2,3,4,5 ptrs (font, str, 3 int* outs, XCharStruct*)
    REG_X11_PTR(XLoadFont, 0x03);                // arg 0: Display*, arg 1: const char*
    REG_X11_PTR(XUnloadFont, 0x01);              // arg 0: Display*
    REG_X11_PTR(XQueryFont, 0x01);               // arg 0: Display*
    REG_X11_PTR(XFreeFont, 0x01);                // arg 0: Display*
    REG_X11_PTR(XListFonts, 0x1B);               // arg 0: Display*, arg 1: const char*, arg 3: char***
    REG_X11_PTR(XFreeFontNames, 0x01);           // arg 0: char**
    REG_X11_PTR(XCreateBitmapFromData, 0x08);    // arg 0: Display*, arg 3: const char*
    REG_X11_PTR(XCreatePixmapFromBitmapData, 0x01); // arg 0: Display*
    REG_X11_EX(XQueryPointer, 0x1FD, 1);         // args 0,2..8 ptrs; 1 stack arg
    REG_X11_EX(XWarpPointer, 0x01, 1);           // arg 0: Display*; 1 stack arg
    REG_X11_PTR(XGrabPointer, 0x01);             // arg 0: Display*
    REG_X11_PTR(XUngrabPointer, 0x01);           // arg 0: Display*
    REG_X11_PTR(XGrabKeyboard, 0x01);            // arg 0: Display*
    REG_X11_PTR(XUngrabKeyboard, 0x01);          // arg 0: Display*
    REG_X11_PTR(XBell, 0x01);                    // arg 0: Display*
    REG_X11_PTR(XScreenCount, 0x01);             // arg 0: Display*
    REG_X11_PTR(XSetInputFocus, 0x01);           // arg 0: Display*
    REG_X11_PTR(XGetInputFocus, 0x05);           // arg 0: Display*, arg 2: int*
    REG_X11_PTR(XChangeProperty, 0x80);          // arg 0: Display*, arg 6: const unsigned char*
    REG_X11_EX(XGetWindowProperty, 0xF81, 4);    // args 0,7,8,9,10,11 ptrs; 4 stack args
    REG_X11_PTR(XDeleteProperty, 0x01);          // arg 0: Display*
    REG_X11_PTR(XGetWindowAttributes, 0x07);     // arg 0: Display*, arg 2: XWindowAttributes*
    #undef REG_X11
    #undef REG_X11_PTR
    // ── libX11-xcb.so.1 ─────────────────────────────────────────
    const char* x11xcb_libs[] = {"libX11-xcb.so.1", "libX11-xcb.so"};
    void* x11xcb_handle = dlopen("libX11-xcb.so.1", RTLD_LAZY);
    if (!x11xcb_handle) x11xcb_handle = dlopen("libX11-xcb.so", RTLD_LAZY);
    #define REG_X11XCB(name) do { \
        void* p = x11xcb_handle ? dlsym(x11xcb_handle, #name) : nullptr; \
        for (const char* L : x11xcb_libs) register_function_(L, #name, p, 0, 0, 0, THUNK_PROXY); \
    } while(0)
    REG_X11XCB(XGetXCBConnection);
    #undef REG_X11XCB
    // ── libxcb.so.1 ─────────────────────────────────────────────
    const char* xcb_libs[] = {"libxcb.so.1", "libxcb.so"};
    void* xcb_handle = dlopen("libxcb.so.1", RTLD_LAZY);
    if (!xcb_handle) xcb_handle = dlopen("libxcb.so", RTLD_LAZY);
    #define REG_XCB(name) do { \
        void* p = xcb_handle ? dlsym(xcb_handle, #name) : nullptr; \
        for (const char* L : xcb_libs) register_function_(L, #name, p, 0, 0, 0, THUNK_PROXY); \
    } while(0)
    #define REG_XCB_PTR(name, ptrs) do { \
        void* p = xcb_handle ? dlsym(xcb_handle, #name) : nullptr; \
        for (const char* L : xcb_libs) register_function_(L, #name, p, ptrs, 0, 0, THUNK_PROXY); \
    } while(0)
    REG_XCB_PTR(xcb_connect, 0x02);              // arg 0: const char*, arg 1: int*
    REG_XCB(xcb_disconnect);
    REG_XCB(xcb_connection_has_error);
    REG_XCB_PTR(xcb_get_setup, 0x01);            // arg 0: xcb_connection_t*
    REG_XCB(xcb_setup_roots_iterator);
    REG_XCB(xcb_screen_allowed_depths_iterator);
    REG_XCB(xcb_depth_visuals_iterator);
    REG_XCB(xcb_generate_id);
    REG_XCB_PTR(xcb_create_window, 0x01);        // arg 0: xcb_connection_t*
    REG_XCB_PTR(xcb_create_window_checked, 0x01);
    REG_XCB_PTR(xcb_destroy_window, 0x01);
    REG_XCB_PTR(xcb_map_window, 0x01);
    REG_XCB_PTR(xcb_unmap_window, 0x01);
    REG_XCB_PTR(xcb_flush, 0x01);
    REG_XCB_PTR(xcb_get_file_descriptor, 0x01);
    REG_XCB_PTR(xcb_wait_for_event, 0x01);
    REG_XCB_PTR(xcb_poll_for_event, 0x01);
    REG_XCB_PTR(xcb_free, 0x01);
    REG_XCB(xcb_visualtype_get);
    #undef REG_XCB
    #undef REG_XCB_PTR
    // ── libgbm.so.1 ───────────────────────────────────────────────
    const char* gbm_libs[] = {"libgbm.so.1", "libgbm.so"};
    void* gbm_handle = dlopen("libgbm.so.1", RTLD_LAZY);
    if (!gbm_handle) gbm_handle = dlopen("libgbm.so", RTLD_LAZY);
    #define REG_GBM(name) do { \
        void* p = gbm_handle ? dlsym(gbm_handle, #name) : nullptr; \
        for (const char* L : gbm_libs) register_function_(L, #name, p, 0, 0, 0, THUNK_PROXY); \
    } while(0)
    REG_GBM(gbm_create_device);
    REG_GBM(gbm_device_destroy);
    REG_GBM(gbm_bo_create);
    REG_GBM(gbm_bo_destroy);
    REG_GBM(gbm_bo_get_width);
    REG_GBM(gbm_bo_get_height);
    REG_GBM(gbm_bo_get_stride);
    REG_GBM(gbm_bo_get_format);
    REG_GBM(gbm_bo_get_handle);
    REG_GBM(gbm_bo_map);
    REG_GBM(gbm_bo_unmap);
    REG_GBM(gbm_surface_create);
    REG_GBM(gbm_surface_destroy);
    REG_GBM(gbm_surface_lock_front_buffer);
    REG_GBM(gbm_surface_release_buffer);
    #undef REG_GBM
    // ── libXext.so.6 (XShm) ─────────────────────────────────────────
    const char* xext_libs[] = {"libXext.so.6", "libXext.so"};
    void* xext_handle = dlopen("libXext.so.6", RTLD_LAZY);
    if (!xext_handle) xext_handle = dlopen("libXext.so", RTLD_LAZY);
    #define REG_XEXT(name) do { \
        void* p = xext_handle ? dlsym(xext_handle, #name) : nullptr; \
        for (const char* L : xext_libs) register_function_(L, #name, p, 0, 0, 0, THUNK_PROXY); \
    } while(0)
    #define REG_XEXT_PTR(name, ptrs) do { \
        void* p = xext_handle ? dlsym(xext_handle, #name) : nullptr; \
        for (const char* L : xext_libs) register_function_(L, #name, p, ptrs, 0, 0, THUNK_PROXY); \
    } while(0)
    #define REG_XEXT_EX(name, ptrs, nstack) do { \
        void* p = xext_handle ? dlsym(xext_handle, #name) : nullptr; \
        for (const char* L : xext_libs) register_function_(L, #name, p, ptrs, nstack, 0, THUNK_PROXY); \
    } while(0)
    REG_XEXT(XShmQueryExtension);
    REG_XEXT(XShmGetEventBase);
    REG_XEXT_PTR(XShmCreateImage, 0x33);          // args 0,1,4,5 are pointers
    REG_XEXT(XShmAttach);
    REG_XEXT(XShmDetach);
    REG_XEXT_EX(XShmPutImage, 0x0F, 3);           // args 0-3 are ptrs; 3 stack args
    REG_XEXT_PTR(XShmGetImage, 0x04);            // arg 2: XImage*
    #undef REG_XEXT
    #undef REG_XEXT_PTR
    // ── libGLX.so.2 (GLX) ───────────────────────────────────────────
    const char* glx_libs[] = {"libGLX.so.2", "libGLX.so"};
    void* glx_handle = dlopen("libGLX.so.2", RTLD_LAZY);
    if (!glx_handle) glx_handle = dlopen("libGLX.so", RTLD_LAZY);
    #define REG_GLX(name) do { \
        void* p = glx_handle ? dlsym(glx_handle, #name) : nullptr; \
        for (const char* L : glx_libs) register_function_(L, #name, p, 0, 0, 0, THUNK_PROXY); \
    } while(0)
    #define REG_GLX_PTR(name, ptrs) do { \
        void* p = glx_handle ? dlsym(glx_handle, #name) : nullptr; \
        for (const char* L : glx_libs) register_function_(L, #name, p, ptrs, 0, 0, THUNK_PROXY); \
    } while(0)
    REG_GLX_PTR(glXChooseVisual, 0x04);           // arg 2: int* (NULL-terminated attrib list)
    REG_GLX(glXCreateContext);
    REG_GLX(glXDestroyContext);
    REG_GLX(glXMakeCurrent);
    REG_GLX(glXSwapBuffers);
    REG_GLX(glXGetClientString);
    REG_GLX(glXQueryExtensionsString);
    REG_GLX(glXQueryServerString);
    REG_GLX(glXGetFBConfigs);
    REG_GLX_PTR(glXGetFBConfigAttrib, 0x08);      // arg 3: int* (value out)
    REG_GLX_PTR(glXCreateWindow, 0x08);           // arg 3: int* attrib_list
    REG_GLX(glXDestroyWindow);
    REG_GLX(glXCreatePbuffer);
    REG_GLX(glXDestroyPbuffer);
    #undef REG_GLX
    #undef REG_GLX_PTR
    // ── libXrandr.so.2 (RandR) ──────────────────────────────────────
    const char* randr_libs[] = {"libXrandr.so.2", "libXrandr.so"};
    void* randr_handle = dlopen("libXrandr.so.2", RTLD_LAZY);
    if (!randr_handle) randr_handle = dlopen("libXrandr.so", RTLD_LAZY);
    #define REG_RANDR(name) do { \
        void* p = randr_handle ? dlsym(randr_handle, #name) : nullptr; \
        for (const char* L : randr_libs) register_function_(L, #name, p, 0, 0, 0, THUNK_PROXY); \
    } while(0)
    #define REG_RANDR_PTR(name, ptrs) do { \
        void* p = randr_handle ? dlsym(randr_handle, #name) : nullptr; \
        for (const char* L : randr_libs) register_function_(L, #name, p, ptrs, 0, 0, THUNK_PROXY); \
    } while(0)
    #define REG_RANDR_EX(name, ptrs, nstack) do { \
        void* p = randr_handle ? dlsym(randr_handle, #name) : nullptr; \
        for (const char* L : randr_libs) register_function_(L, #name, p, ptrs, nstack, 0, THUNK_PROXY); \
    } while(0)
    REG_RANDR(XRRGetScreenResources);
    REG_RANDR(XRRGetScreenResourcesCurrent);
    REG_RANDR(XRRFreeScreenResources);
    REG_RANDR(XRRGetCrtcInfo);
    REG_RANDR(XRRFreeCrtcInfo);
    REG_RANDR(XRRGetOutputInfo);
    REG_RANDR(XRRFreeOutputInfo);
    REG_RANDR_EX(XRRSetCrtcConfig, 0x103, 2);   // args 0,1,8 ptrs; 2 stack args
    REG_RANDR(XRRGetScreenSizeRange);
    #undef REG_RANDR
    #undef REG_RANDR_PTR
    // ── libXkblib.so (Xkb) ──────────────────────────────────────────
    const char* xkb_libs[] = {"libXkblib.so", "libX11-xcb.so"};
    void* xkb_handle = dlopen("libXkblib.so", RTLD_LAZY);
    if (!xkb_handle) xkb_handle = dlopen("libX11-xcb.so", RTLD_LAZY);
    #define REG_XKB(name) do { \
        void* p = xkb_handle ? dlsym(xkb_handle, #name) : nullptr; \
        for (const char* L : xkb_libs) register_function_(L, #name, p, 0, 0, 0, THUNK_PROXY); \
    } while(0)
    #define REG_XKB_PTR(name, ptrs) do { \
        void* p = xkb_handle ? dlsym(xkb_handle, #name) : nullptr; \
        for (const char* L : xkb_libs) register_function_(L, #name, p, ptrs, 0, 0, THUNK_PROXY); \
    } while(0)
    REG_XKB(XkbOpenDevice);
    REG_XKB(XkbGetMap);
    REG_XKB_PTR(XkbGetState, 0x04);               // arg 2: XkbState* out
    REG_XKB(XkbSetState);
    REG_XKB(XkbSetAutoRepeatRate);
    REG_XKB_PTR(XkbGetAutoRepeatRate, 0x0C);      // arg 2: unsigned int* delay, arg 3: unsigned int* interval
    REG_XKB(XkbFreeKeyboard);
    #undef REG_XKB
    #undef REG_XKB_PTR
}
} // namespace arm64emu
