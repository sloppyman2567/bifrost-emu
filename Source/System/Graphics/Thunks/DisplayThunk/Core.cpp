#include "System/Graphics/Display/WindowStats.hpp"
// frost_graphics/display_thunk.cpp — DisplayThunk implementation (1.5.4-alpha).
//
// See Source/System/Graphics/Thunks/DisplayThunk.hpp for the design overview. This file
// implements the DisplayThunk class for Vulkan / Wayland / X11 / GBM.
//
// 1.5.4-alpha: DisplayThunk now uses the same full dispatch logic as
// GraphicThunk (stack args, float args, string returns, GetProcAddress,
// mixed int+float) and integrates DisplayProxy for X11/Wayland fallback
#include "System/Graphics/Thunks/DisplayThunk/DisplayThunkInternal.hpp"
namespace arm64emu {
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
    if (impl_->wl_fd_publisher_) impl_->proxy_->set_fd_publisher(impl_->wl_fd_publisher_);
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
void DisplayThunk::set_wl_fd_resolver(std::function<int(int)> r) {
    if (!impl_ || !impl_->proxy_) return;
    impl_->proxy_->set_fd_resolver(std::move(r));
}
void DisplayThunk::set_wl_fd_publisher(WlFdPublisher p) {
    if (!impl_) return;
    impl_->wl_fd_publisher_ = std::move(p);
    if (impl_->proxy_) impl_->proxy_->set_fd_publisher(impl_->wl_fd_publisher_);
}
bool DisplayThunk::ensure_android_window() {
    if (!impl_ || !impl_->mem) return false;
    auto& mgr = frost::AndroidSurfaceManager::instance();
    mgr.set_memory(impl_->mem);
    if (!mgr.open_host_window()) return false;
    return mgr.host_native_window() != 0;
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
size_t DisplayThunk::symbol_count() const {
    if (!impl_) return 0;
    return impl_->id_to_idx_.size();
}
uint64_t DisplayThunk::trampoline_base() const {
    if (!impl_) return 0;
    return impl_->trampoline_base;
}
// ── register_known_symbols_ ────────────────────────────────────────────
// 1.5.4-alpha: TABLE-DRIVEN. Tools/CodeGen/thunk_dp.txt (generated into
// Include/Generated/ThunkTable.hpp) is now the single source of truth for which
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
        case thunk::Policy::VK_PRESENT:
        case thunk::Policy::VK_ALLOC_MEMORY:
        case thunk::Policy::VK_FREE_MEMORY:
        case thunk::Policy::VK_MAP_MEMORY:
        case thunk::Policy::VK_UNMAP_MEMORY:
        case thunk::Policy::VK_FLUSH_MAPPED:
        case thunk::Policy::VK_INVALIDATE_MAPPED:
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
    // Cache the host vkUnmapMemory for the map/unmap/free arms (they must
    // never call the MAP arm's entry.host_fn as a 2-arg unmap).
    if (kFamilies[0].handle)
        impl_->vk_unmap_host_ = dlsym(kFamilies[0].handle, "vkUnmapMemory");
}
}  // namespace arm64emu
