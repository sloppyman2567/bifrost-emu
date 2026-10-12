// GraphicThunk.cpp — graphic API thunking.
//
// also hosts the FrostGraphics::audio_thunk() and
// FrostGraphics::display_thunk() lazy-creator methods (the AudioThunk
// and DisplayThunk implementations live in their own .cpp files).
//
// ── Overview ──────────────────────────────────────────────────────────
// bifrost-emu runs AArch64 guests on x86-64 hosts. A guest that uses
// OpenGL/EGL/SDL2 normally links against the AArch64 versions of those
// libraries, which can't run on the x86-64 host. The thunk solves this
// by:
//
//   1. Maintaining a registry of (library, symbol) → (host_fn_ptr,
//      guest_trampoline_addr).
//   2. Each registered symbol gets a 16-byte guest trampoline that
//      traps to the host via the __NR_bifrost_thunk syscall.
//   3. The host's syscall dispatcher calls GraphicThunk::dispatch(),
//      which reads the symbol_id from x9, looks up the host function,
//      reads args from x0..x7, calls the host function, and writes
//      the return value to x0.
//
// This is the standard "thunk" pattern used by user-mode emulators
// (QEMU user, Rosetta 2, FEX-Emu) to forward guest calls to host
// libraries without recompiling them.
//
// ── What's supported ─────────────────────────────────────────────────
// EXPERIMENTAL. Only a small subset of GL/EGL/SDL2 entry points are
// thunked — enough to run trivial programs (clear the screen, draw a
#include "System/Graphics/Thunks/GraphicThunk/GraphicThunkInternal.hpp"
namespace arm64emu {
// ── GraphicThunk method implementations ───────────────────────────────
GraphicThunk::GraphicThunk() {
    impl_ = std::make_unique<GraphicThunkImpl>();
    // thunking is now ENABLED BY DEFAULT.
    // Previously required BIFROST_THUNK_GRAPHICS=1. Now we always try
    // to thunk graphic calls; if the host doesn't have GL/EGL/SDL2
    // libraries, the symbols resolve to stubs that return 0 (safe
    // fallback). Set BIFROST_NO_THUNK_GRAPHICS=1 to disable.
    //
    // This makes graphics "just work" for programs that use GL/EGL/SDL2
    // when the host has the libraries, and degrade gracefully (software
    // rendering or no-op) when it doesn't.
    const char* disable = getenv("BIFROST_NO_THUNK_GRAPHICS");
    impl_->enabled = !(disable && disable[0] != '0');
    if (impl_->enabled) {
        // Only print if verbose or trace — don't clutter default output
        if (dbg().thunk_trace || getenv("BIFROST_VERBOSE")) {
            fprintf(stderr, "[thunk] graphic API thunking enabled "
                    "(GL/EGL/SDL2 → host, with emulation fallback)\n");
        }
    }
}
GraphicThunk::~GraphicThunk() {
    if (impl_) {
        for (void* p : impl_->rw_kept_) std::free(p);
    }
#if defined(BIFROST_THUNK_HAVE_SDL2)
    SDL_EventFilter filter = nullptr;
    void* userdata = nullptr;
    if (impl_ && SDL_GetEventFilter(&filter, &userdata) && userdata == impl_.get() &&
        filter == GraphicThunkImpl::sdl_event_filter_)
        SDL_SetEventFilter(nullptr, nullptr);
    if (impl_ && impl_->sdl_window) {
        SDL_DestroyWindow(impl_->sdl_window);
    }
    if (impl_ && impl_->sdl_init_done) {
        SDL_Quit();
    }
#endif
}
bool GraphicThunk::enabled() const { return impl_ && impl_->enabled; }
// ── init() — allocate trampoline page, register known symbols ─────────
bool GraphicThunk::init(Memory& mem) {
    if (!impl_->enabled) return false;
    if (impl_->initialized) return true;
    std::lock_guard<std::mutex> g(impl_->mu);
    if (impl_->initialized) return true;  // raced init: first one won
    impl_->mem = &mem;
    // Allocate a 64 KiB page for trampolines. mmap_alloc returns a
    // fresh, zeroed region. The page is mapped RWX in guest memory
    // (the JIT will execute the trampolines directly).
    impl_->trampoline_base = mem.mmap_alloc(GraphicThunkImpl::TRAMPOLINE_PAGE_SIZE);
    if (impl_->trampoline_base == 0) {
        fprintf(stderr, "[thunk] init: failed to allocate trampoline page\n");
        return false;
    }
    impl_->string_cache_base = mem.mmap_alloc(GraphicThunkImpl::STRING_CACHE_SIZE);
    if (impl_->string_cache_base == 0) {
        fprintf(stderr, "[thunk] init: failed to allocate string cache\n");
        return false;
    }
    impl_->string_cache_off = 0;
    // Register the known GL/EGL/SDL2 entry points.
    register_known_symbols_();
    impl_->initialized = true;
    // initialize GL state tracker.
    impl_->gl_state_tracker_ = std::make_unique<GLStateTracker>();
    if (dbg().thunk_trace) {
        fprintf(stderr, "[thunk] init: %zu symbols registered, "
                "trampoline_base=0x%llx\n",
                impl_->id_to_idx_.size(),
                static_cast<unsigned long long>(impl_->trampoline_base));
    }
    return true;
}
// ── register_function_ — add a (lib, sym) → host_fn mapping ───────────
// Allocates a symbol_id, writes the trampoline into guest memory, and
// stores the entry. Thread-safe (called from init() under lock).
void GraphicThunk::register_function_(const std::string& lib,
                                       const std::string& sym,
                                       void* host_fn,
                                       uint16_t pointer_args,
                                       uint8_t n_stack,
                                       uint8_t n_float,
                                       uint8_t flags,
                                       const thunk::Spec* spec) {
    auto* lt = impl_->find_or_create_lib_(lib);
    // Check if already registered (idempotent).
    for (const auto& e : lt->entries) {
        if (e.name == sym) return;
    }
    // symbol_id includes ID_BASE_GRAPHICS to
    // avoid collisions with AudioThunk/DisplayThunk IDs.
    uint32_t local_id = static_cast<uint32_t>(impl_->id_to_idx_.size());
    if (local_id >= GraphicThunk::MAX_SYMBOLS) {
        fprintf(stderr, "[thunk] register: symbol table full (%zu)\n",
                impl_->id_to_idx_.size());
        return;
    }
    uint32_t sym_id = GraphicThunk::ID_BASE_GRAPHICS + local_id;
    uint64_t addr = impl_->trampoline_base + local_id * GraphicThunk::TRAMPOLINE_SIZE;
    write_trampoline_(*impl_->mem, addr, sym_id);
    lt->entries.push_back({sym, host_fn, addr, sym_id, pointer_args,
                           n_stack, n_float, flags, spec,
                           GLStateTracker::tracks_state(sym)});
    impl_->id_to_idx_.push_back({
        static_cast<uint32_t>(std::distance(impl_->libs_.data(), lt)),
        static_cast<uint32_t>(lt->entries.size() - 1)
    });
    if (dbg().thunk_trace) {
        fprintf(stderr, "[thunk] registered %s:%s -> 0x%llx "
                "(id=%u ptrs=0x%x stack=%u fp=%u flags=0x%x)\n",
                lib.c_str(), sym.c_str(),
                static_cast<unsigned long long>(addr), sym_id,
                pointer_args, n_stack, n_float, flags);
    }
}
// ── write_trampoline_ — emit 16-byte AArch64 trampoline ────────────────
// Layout (little-endian, each instruction is 4 bytes):
//   movz x9, #sym_id        ; load symbol_id into x9
//   movz x8, #SYSCALL_NUM   ; load __NR_bifrost_thunk into x8
//   svc #0                  ; trap to host
//   nop                     ; pad to 16 bytes (alignment)
void GraphicThunk::write_trampoline_(Memory& mem, uint64_t addr, uint32_t sym_id) {
    // Truncation: sym_id is bounded by MAX_SYMBOLS (4096), so it always
    // fits in 16 bits.
    uint32_t buf[4];
    buf[0] = trampoline_enc::MOVZ_Xd_IMM16(9, static_cast<uint16_t>(sym_id));
    buf[1] = trampoline_enc::MOVZ_Xd_IMM16(8,
                static_cast<uint16_t>(GraphicThunk::SYSCALL_NUMBER));
    buf[2] = trampoline_enc::SVC_0;
    buf[3] = trampoline_enc::RET;  // return to guest caller (x30)
    mem.write(addr, buf, sizeof(buf));
}
// ── resolve() — look up a (lib, sym) and return guest trampoline addr ─
uint64_t GraphicThunk::resolve(const std::string& lib, const std::string& sym) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return 0;
    std::lock_guard<std::mutex> g(impl_->mu);
    auto* lt = impl_->find_lib_(lib);
    if (!lt) return 0;
    for (const auto& e : lt->entries) {
        if (e.name == sym) return e.guest_addr;
    }
    return 0;
}
// ── enumerate_symbols() — list all (name, addr) pairs for `lib` ────────
size_t GraphicThunk::enumerate_symbols(const std::string& lib,
    const std::function<void(const std::string&, uint64_t)>& cb) const {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return 0;
    std::lock_guard<std::mutex> g(impl_->mu);
    auto* lt = impl_->find_lib_(lib);
    if (!lt) return 0;
    for (const auto& e : lt->entries) {
        cb(e.name, e.guest_addr);
    }
    return lt->entries.size();
}
// ── dispatch() — call the host function for a given symbol_id ──────────
// Called from the syscall handler when a trampoline traps. Reads args
// from cpu.regs[0..7], calls the host function, writes the return
// value to cpu.regs[0].
//
// IMPORTANT: this function is on the hot path for thunked calls. Keep
// it lock-free — the id_to_idx_ vector is set once during init() and
// never resized afterwards, so we can read it without the mutex.
// ── Diagnostics ────────────────────────────────────────────────────────
size_t GraphicThunk::symbol_count() const {
    if (!impl_) return 0;
    return impl_->id_to_idx_.size();
}
uint64_t GraphicThunk::trampoline_base() const {
    if (!impl_) return 0;
    return impl_->trampoline_base;
}
void GraphicThunk::set_sdl_filter_cpu_factory(SdlFilterCpuFactory factory) {
#if defined(BIFROST_THUNK_HAVE_SDL2)
    if (impl_) impl_->sdl_filter_cpu_factory_ = std::move(factory);
#else
    (void)factory;
#endif
}
// ── set_glfw_cb_runner — guest GLFW-callback delivery hook ───────────
void GraphicThunk::set_glfw_cb_runner(GlfwCbRunner runner) {
    if (!impl_) return;
    impl_->glfw_cb_runner_ = std::move(runner);
    // Fresh registration: drop stale last-delivered state so a newly-
    // wired runner doesn't skip the first motion/size event.
    std::lock_guard<std::mutex> lk(impl_->state_mu);
    impl_->glfw_cursor_last_.clear();
    impl_->glfw_key_last_.clear();
    impl_->glfw_mouse_last_.clear();
    impl_->glfw_fb_last_.clear();
    impl_->glfw_winsz_last_.clear();
    impl_->glfw_focus_last_.clear();
    impl_->glfw_last_delivered_err_code_ = 0;
    impl_->glfw_last_delivered_err_desc_.clear();
}
// ── set_vk_proc_lookup — guest-callable vkGetInstanceProcAddr hook ────
void GraphicThunk::set_vk_proc_lookup(VkProcLookup lookup) {
    if (!impl_) return;
    impl_->vk_proc_lookup_ = std::move(lookup);
}
// ── set_sdl_thread_runner — SDL thread spawn/join hook ────────────────
void GraphicThunk::set_sdl_thread_runner(SdlThreadRunner runner) {
    if (!impl_) return;
    impl_->sdl_thread_runner_ = std::move(runner);
}
// ── wake_sdl_semaphores — release SDL worker threads at guest exit ─────
void GraphicThunk::wake_sdl_semaphores() {
    if (!impl_ || !impl_->sdl_sem_post_fn_) return;
    // Snapshot under mu_, then post OUTSIDE the lock: SDL_SemPost is a
    // host call and must not run while holding the registry mutex.
    std::vector<uint64_t> sems;
    {
        std::lock_guard<std::mutex> g(impl_->mu);
        sems.reserve(impl_->sdl_sems_.size());
        for (uint64_t s : impl_->sdl_sems_) sems.push_back(s);
    }
    if (sems.empty()) return;
    using SemPostFn = int (*)(void*);
    SemPostFn post = reinterpret_cast<SemPostFn>(impl_->sdl_sem_post_fn_);
    for (uint64_t s : sems) {
        // Post twice: a binary semaphore with a blocked waiter needs one
        // post to wake it; a second covers a just-reposted waiter racing
        // back into SDL_SemWait before the thread loop observes
        // cpu.running==false. Extra posts on an unwaited semaphore merely
        // bump the count (harmless at shutdown).
        post(reinterpret_cast<void*>(s));
        post(reinterpret_cast<void*>(s));
    }
}
// ── register_known_symbols_ — populate the registry ────────────────────
// Called once by init(). Each entry maps a (library, symbol) pair to
// the host function pointer (when the host has the dev headers) or to
// a null stub (when the host doesn't have the headers, but we still
// want the symbol to resolve so the guest doesn't fail at dlsym time).
//
// the symbol inventory is TABLE-DRIVEN. Tools/CodeGen/thunk_dp.txt
// (generated into Include/Generated/ThunkTable.hpp) lists every (lib family, symbol)
// with its AAPCS64 arg kinds, return kind, dispatch policy and bounce size.
// This loop derives the legacy ABI-shape fields (pointer_args / n_stack /
// n_float / flags) from the ARGS column and registers each symbol under
// every soname of its family. Adding a symbol is now a one-line spec row,
// not a REG_* macro + a dispatch() special case.

void GraphicThunk::register_known_symbols_() {
#if defined(BIFROST_THUNK_HAVE_GL)
    constexpr bool kHaveGL = true;
#else
    constexpr bool kHaveGL = false;
#endif
#if defined(BIFROST_THUNK_HAVE_EGL)
    constexpr bool kHaveEGL = true;
#else
    constexpr bool kHaveEGL = false;
#endif
#if defined(BIFROST_THUNK_HAVE_SDL2)
    constexpr bool kHaveSDL = true;
#else
    constexpr bool kHaveSDL = false;
#endif

    struct FamilyDef {
        const char* const* sonames;
        uint32_t n;
        bool have;
    };
    static const char* kGlSonames[]   = {"libGL.so", "libGL.so.1"};
    static const char* kGlesSonames[] = {"libGLESv2.so", "libGLESv2.so.2"};
    static const char* kEglSonames[]  = {"libEGL.so", "libEGL.so.1"};
    static const char* kSdlSonames[]  = {"libSDL2.so", "libSDL2-2.0.so.0"};
    static const char* kGlfwSonames[] = {"libglfw.so.3", "libglfw.so"};
    static const char* kMixSonames[]  = {"libSDL2_mixer-2.0.so.0",
                                          "libSDL2_mixer.so"};
    // Indexed by thunk::LibFamily (GL, GLES, EGL, SDL, GLFW, MIX).
    static const FamilyDef kFamilies[] = {
        {kGlSonames,   2, kHaveGL},
        {kGlesSonames, 2, kHaveGL},
        {kEglSonames,  2, kHaveEGL},
        {kSdlSonames,  2, kHaveSDL},
        {kGlfwSonames, 2, kHaveGL},
        // SDL_mixer: never linked into the emulator, so `have` is false —
        // every Mix_* host_fn stays NULL and dispatch() takes the stub path
        // (returns 0). MIX_VERSION / MIX_OPEN_AUDIO are handled in
        // dispatch() before the host-fn stub check.
        {kMixSonames,  2, false},
    };

    // GLFW isn't linked into the emulator, so force-load it first so
    // dlsym(RTLD_DEFAULT) can find its symbols.
    if (kHaveGL) {
        void* h = dlopen("libglfw.so.3", RTLD_LAZY | RTLD_GLOBAL);
        if (!h) h = dlopen("libglfw.so", RTLD_LAZY | RTLD_GLOBAL);
        if (!h && dbg().thunk_trace)
            fprintf(stderr, "[thunk] glfw: dlopen failed: %s\n", dlerror());
    }
    // SDL symbols MUST resolve through the specific library handle:
    // on sdl2-compat hosts libSDL3 is also in the global scope and
    // exports same-named symbols with SDL3 ABI (e.g.
    // SDL_GetCurrentDisplayMode returns a pointer in SDL3, whereas SDL2
    // takes an output buffer). RTLD_DEFAULT can pick those, and calling them
    // with the SDL2 ABI writes through stale registers. A direct handle
    // pins resolution to the real SDL2 ABI library.
    void* sdl_handle = nullptr;
    if (kHaveSDL) {
        sdl_handle = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
        if (!sdl_handle) sdl_handle = dlopen("libSDL2.so", RTLD_LAZY | RTLD_GLOBAL);
        if (!sdl_handle && dbg().thunk_trace)
            fprintf(stderr, "[thunk] sdl: dlopen failed: %s\n", dlerror());
    }
    impl_->keyboard_state_fn = sdl_handle ? dlsym(sdl_handle, "SDL_GetKeyboardState") : nullptr;

    // GLFW callback delivery needs the raw host state-query fns (the
    // GLFW_POLL dispatch path reads them to decide whether guest
    // callbacks should fire). These are resolved once at init.
    impl_->glfw_get_cursor_pos_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwGetCursorPos") : nullptr;
    impl_->glfw_get_key_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwGetKey") : nullptr;
    impl_->glfw_get_key_scancode_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwGetKeyScancode") : nullptr;
    impl_->glfw_get_mouse_button_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwGetMouseButton") : nullptr;
    impl_->glfw_get_window_size_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwGetWindowSize") : nullptr;
    impl_->glfw_get_framebuffer_size_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwGetFramebufferSize") : nullptr;
    impl_->glfw_get_window_attrib_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwGetWindowAttrib") : nullptr;
    impl_->glfw_set_error_callback_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwSetErrorCallback") : nullptr;
    impl_->glfw_get_window_content_scale_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwGetWindowContentScale") : nullptr;
    impl_->glfw_set_window_size_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwSetWindowSize") : nullptr;

    // Raw host GL fns for the glMapBuffer bounce (buffer size query and
    // the two buffer-copy primitives). glGetBufferParameteriv is not a
    // registered thunk symbol, so resolve it directly.
    impl_->gl_get_buffer_parameteriv_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glGetBufferParameteriv") : nullptr;
    impl_->gl_get_buffer_subdata_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glGetBufferSubData") : nullptr;
    impl_->gl_buffer_subdata_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glBufferSubData") : nullptr;

    // Raw host SDL_SemPost for wake_sdl_semaphores() (the registered
    // SDL_SemPost entry's host_fn is a guest-callable trampoline, so the
    // wake path uses this raw dlsym result instead).
    impl_->sdl_sem_post_fn_ =
        kHaveSDL ? dlsym(RTLD_DEFAULT, "SDL_SemPost") : nullptr;

    // Install a HOST-side error-capture trampoline so real GLFW errors
    // (from host libglfw itself) are recorded and later forwarded to the
    // guest's glfwSetErrorCallback. We MUST NOT hand the guest callback
    // to host GLFW (it's AArch64), so dispatch() stores it and this
    // host trampoline feeds the shared error slot. Requires a global
    // pointer to the impl (single emulator per process).
    static GraphicThunkImpl* host_err_sink = nullptr;
    host_err_sink = impl_.get();
    {
        std::lock_guard<std::mutex> lk(impl_->state_mu);
        impl_->glfw_last_error_code_ = 0;
        impl_->glfw_last_error_desc_.clear();
    }
    if (impl_->glfw_set_error_callback_fn_) {
        using SetErrCbFn = void (*)(void (*)(int, const char*));
        auto setcb = reinterpret_cast<SetErrCbFn>(impl_->glfw_set_error_callback_fn_);
        setcb([](int code, const char* desc) {
            if (!host_err_sink) return;
            // Fires on an arbitrary host thread: leaf-lock the slot.
            // Cross-instance routing (second emulator overwrites the
            // global) stays a documented single-emulator limit.
            std::lock_guard<std::mutex> lk(host_err_sink->state_mu);
            host_err_sink->glfw_last_error_code_ = code;
            host_err_sink->glfw_last_error_desc_ =
                desc ? desc : std::string();
        });
    }

    for (const thunk::Spec& spec : thunk::specs) {
        // the table now also carries the DisplayThunk
        // families (VK/WL/X11/...). GraphicThunk only owns the graphic
        // families — skip the rest BEFORE indexing kFamilies (which is
        // sized for GL/GLES/EGL/SDL/GLFW only; display families have
        // higher LibFamily values).
        if (spec.lib != thunk::LibFamily::GL &&
            spec.lib != thunk::LibFamily::GLES &&
            spec.lib != thunk::LibFamily::EGL &&
            spec.lib != thunk::LibFamily::SDL &&
            spec.lib != thunk::LibFamily::GLFW &&
            spec.lib != thunk::LibFamily::MIX) {
            continue;
        }
        const FamilyDef& fd = kFamilies[static_cast<int>(spec.lib)];
        // Host fn resolution: real dlsym when the host has the library,
        // null stub otherwise. GET_PROC must stay non-null: dispatch
        // returns the trampoline before calling it, but a null host_fn
        // short-circuits to 0 first. GLFW *_CB setters are intercepted in
        // dispatch() before the host-fn path (the guest callback is
        // AArch64), so their host_fn is never used for the callback.
        void* host_fn = nullptr;
        if (spec.lib == thunk::LibFamily::SDL) {
            host_fn = sdl_handle ? dlsym(sdl_handle, spec.name) : nullptr;
        } else if (fd.have) {
            host_fn = dlsym(RTLD_DEFAULT, spec.name);
        }
        // libEGL exports only core symbols; extension entry points come
        // from eglGetProcAddress (EGL 1.5 §3.10).
        if (!host_fn && spec.lib == thunk::LibFamily::EGL && fd.have) {
            using GetProc = void* (*)(const char*);
            static const auto egl_get_proc = reinterpret_cast<GetProc>(
                dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
            if (egl_get_proc) host_fn = egl_get_proc(spec.name);
        }
        if (spec.policy == thunk::Policy::GET_PROC && !host_fn) {
            host_fn = reinterpret_cast<void*>(1);
        }

        // Derive the legacy ABI-shape fields from the ARGS column so the
        // dispatcher's float/mixed/generic paths keep working unchanged.
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
        if (n_double > 0 && n_int == 0) {
            // Double-only AAPCS64 ABI: args in d0..d{n-1}. n_float carries
            // the double count; THUNK_DOUBLE tells dispatch to read them as
            // 8-byte doubles (from cpu.v_lo) rather than 4-byte floats.
            flags |= THUNK_DOUBLE;
            n_float = n_double;
        } else if (n_double > 0) {
            // Mixed int + double (glfwSetCursorPos: window in x0, two
            // doubles in d0/d1). THUNK_DOUBLE marks the FP regs as 8-byte;
            // THUNK_MIXED_FP routes to the mixed arm with n_stack = int
            // arity and n_float = double count. The pure-double arm above
            // must NOT catch this (it would drop the window handle).
            flags |= THUNK_MIXED_FP | THUNK_DOUBLE;
            n_stack = n_int;
            n_float = n_double;
        } else if (n_float > 0 && n_int > 0) {
            // Mixed int+float ABI: n_stack holds the integer arity (x0..).
            flags |= THUNK_MIXED_FP;
            n_stack = n_int;
        } else if (n_float > 0) {
            n_stack = 0;
        } else if (n_args > 8) {
            // Args 8+ live on the guest stack at SP (AAPCS64).
            n_stack = static_cast<uint8_t>(n_args - 8);
        }
        if (spec.ret == thunk::RetKind::STRING)       flags |= THUNK_RET_STRING;
        if (spec.policy == thunk::Policy::SHADER_SOURCE) flags |= THUNK_SHADER_SOURCE;
        if (spec.policy == thunk::Policy::TF_VARYINGS) flags |= THUNK_TF_VARYINGS;
        if (spec.policy == thunk::Policy::GET_PROC)   flags |= THUNK_GET_PROC;

        for (uint32_t i = 0; i < fd.n; i++) {
            register_function_(fd.sonames[i], spec.name, host_fn,
                               pointer_args, n_stack, n_float, flags, &spec);
        }
    }
}
// ── FrostGraphics::thunk() — out-of-line definition ───────────────────
// Lives here (not in Graphics.cpp) because it needs the full
// GraphicThunk type to construct via `new`. Returns the lazily-created
// GraphicThunk instance owned by FrostGraphics.
GraphicThunk* FrostGraphics::thunk() {
    if (!thunk_) {
        thunk_ = std::unique_ptr<GraphicThunk>(new GraphicThunk());
    }
    return thunk_.get();
}
// audio_thunk() and display_thunk() — same lazy pattern.
// They live here for the same reason thunk() does: the FrostGraphics
// header only forward-declares AudioThunk / DisplayThunk, so the
// unique_ptr ctor needs the full type, which is only visible in this
// .cpp (which includes frost/audio_thunk.hpp and frost/display_thunk.hpp).
AudioThunk* FrostGraphics::audio_thunk() {
    if (!audio_thunk_) {
        audio_thunk_ = std::unique_ptr<AudioThunk>(new AudioThunk());
    }
    return audio_thunk_.get();
}
DisplayThunk* FrostGraphics::display_thunk() {
    if (!display_thunk_) {
        display_thunk_ = std::unique_ptr<DisplayThunk>(new DisplayThunk());
    }
    return display_thunk_.get();
}
}  // namespace arm64emu
