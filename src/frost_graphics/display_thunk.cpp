// frost_graphics/display_thunk.cpp — DisplayThunk implementation (1.5.2-alpha).
//
// See include/frost/display_thunk.hpp for the design overview. This file
// implements the DisplayThunk class for Vulkan / Wayland / X11 / GBM.
//
// 1.5.2-alpha: DisplayThunk now uses the same full dispatch logic as
// GraphicThunk (stack args, float args, string returns, GetProcAddress,
// mixed int+float) and integrates DisplayProxy for X11/Wayland fallback
// when host libraries are unavailable.
#include "frost/display_thunk.hpp"
#include "frost/thunk.hpp"  // for SYSCALL_NUMBER
#include "frost/display_proxy.hpp"
#include "thunk_common.hpp" // shared SymbolEntry (single definition — see header)
#include "opgen_thunk.hpp"  // 1.5.3-alpha: symbol signature table (single source of truth)
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
    // DisplayProxy for X11/Wayland fallback (1.5.2-alpha).
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
    // 1.5.2-alpha: display thunking enabled by default.
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
    // 1.5.2-alpha: lazily create the DisplayProxy. It owns an SDL2 window
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

    // ── Vulkan marshalling path ──────────────────────────────────────
    // vkCreateInstance/vkCreateDevice carry nested guest pointers (string
    // arrays + struct arrays) that the generic bounce can't fix, and the
    // proc-addr functions read the pName string from arg 1. Everything
    // else falls through to the generic path with the (corrected) pointer
    // masks — opaque handles pass verbatim, out pointers bounce back.
    if (entry.flags & THUNK_VULKAN) {
        if (vk_dispatch_(cpu, entry, trace)) return 0;
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
// ── Vulkan marshalling (1.5.3-alpha) ───────────────────────────────────
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
} // namespace

bool DisplayThunk::vk_dispatch_(CPU& cpu, const SymbolEntry& entry, bool trace) {
    Memory* mem = impl_->mem;
    if (!mem) return false;

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
        info->ppEnabledLayerNames = st.guest_str_array(
            mem, reinterpret_cast<uint64_t>(info->ppEnabledLayerNames), info->enabledLayerCount);
        info->ppEnabledExtensionNames = st.guest_str_array(
            mem, reinterpret_cast<uint64_t>(info->ppEnabledExtensionNames), info->enabledExtensionCount);
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
        uint64_t host_device = 0;
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, const void*, const void*, void*)>(entry.host_fn)(
            cpu.regs[0], info, nullptr, &host_device);
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
// 1.5.3-alpha: TABLE-DRIVEN. tools/opgen/thunk_dp.txt (generated into
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
        uint8_t n_float = 0, n_int = 0, n_args = 0;
        for (const char* a = spec.args; *a; ++a, ++n_args) {
            switch (*a) {
            case 'p': case 'z':
                pointer_args |= static_cast<uint16_t>(1u << n_args);
                break;
            case 'f': n_float++; break;
            default:  n_int++; break;
            }
        }
        uint8_t n_stack = 0;
        uint8_t flags = 0;
        if (n_float > 0 && n_int > 0) {
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
        case thunk::Policy::VK_GET_PROC:
            flags |= THUNK_VULKAN | THUNK_GET_PROC;
            break;
        case thunk::Policy::VK_CREATE_INSTANCE:
        case thunk::Policy::VK_CREATE_DEVICE:
        case thunk::Policy::VK_PRESENT:
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
