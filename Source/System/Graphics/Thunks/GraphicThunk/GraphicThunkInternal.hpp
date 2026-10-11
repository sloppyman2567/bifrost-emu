// GraphicThunkInternal.hpp — GraphicThunk private state shared by the files in
// Source/System/Graphics/Thunks/GraphicThunk/. Not an interface header.
#pragma once
#include "System/Graphics/Thunks/LegacyGL.hpp"
#include "System/Graphics/Display/WindowStats.hpp"
#include <map>
#include <atomic>
#include "System/Graphics/Display/Graphics.hpp"
#include "System/Graphics/Thunks/GraphicThunk.hpp"
#include "System/Audio/AudioThunk.hpp"    // 1.5.4-alpha: AudioThunk full def
#include "System/Graphics/Thunks/DisplayThunk.hpp"  // 1.5.4-alpha: DisplayThunk full def
#include "System/Graphics/Thunks/GLState.hpp"       // 1.5.4-alpha: GLStateTracker
#include "System/Android/Surface/Surface.hpp" // Android NativeActivity surface layer
#include "Generated/ThunkTable.hpp"          // 1.5.4-alpha: symbol signature table
#include "System/Graphics/Display/SDLSurfaceBridge.hpp"
#include "System/Graphics/Thunks/ThunkCommon.hpp"         // shared SymbolEntry + trampoline encodings
#include "System/Core/DebugFlags.hpp"            // dbg() — cached trace gates (BIFROST_THUNK_TRACE)
#include "System/Core/CPU.hpp"
#include "System/Memory/Memory.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(BIFROST_THUNK_HAVE_SDL2)
#  include <SDL2/SDL.h>
#endif
#if defined(BIFROST_THUNK_HAVE_EGL)
#  include <EGL/egl.h>
#endif
#if defined(BIFROST_THUNK_HAVE_GL)
#  include <GL/gl.h>
#endif
namespace arm64emu {
static constexpr uint8_t THUNK_RET_STRING    = 1u << 0;
static constexpr uint8_t THUNK_SHADER_SOURCE = 1u << 1;
static constexpr uint8_t THUNK_MIXED_FP      = 1u << 2;
static constexpr uint8_t THUNK_GET_PROC      = 1u << 3;
static constexpr uint8_t THUNK_TF_VARYINGS   = 1u << 4;
static constexpr uint8_t THUNK_DOUBLE        = 1u << 5;
struct GraphicThunkImpl {
    bool   enabled = false;
    Memory* mem    = nullptr;
    bool   initialized = false;
    // SDL_RWFromMem/ConstMem persistent host copies: SDL keeps the mem
    // pointer inside the RWops beyond the dispatch call, so out-of-window
    // sources need a host copy that outlives it. Freed at shutdown.
    std::vector<void*> rw_kept_;
#if defined(BIFROST_THUNK_HAVE_SDL2)
    SdlSurfaceBridge surface_bridge;
#endif
    std::mutex keyboard_mu;
    void* keyboard_state_fn = nullptr;
    uint64_t keyboard_guest = 0;
    int keyboard_count = 0;
    // SDL promises a lifetime-stable pointer. Refresh this same guest block
    // after event pumps, including when the caller caches the first result.
    uint64_t refresh_keyboard() {
        std::lock_guard<std::mutex> lock(keyboard_mu);
        if (!mem || !keyboard_state_fn) return 0;
        int count = 0;
        auto fn = reinterpret_cast<const uint8_t* (*)(int*)>(keyboard_state_fn);
        const uint8_t* keys = fn(&count);
        constexpr size_t kMaxKeys = 65536;
        if (!keys || count < 0 || static_cast<size_t>(count) > kMaxKeys) return 0;
        if (!keyboard_guest) keyboard_guest = mem->mmap_alloc(kMaxKeys);
        if (!keyboard_guest) return 0;
        mem->write(keyboard_guest, keys, static_cast<size_t>(count));
        keyboard_count = count;
        return keyboard_guest;
    }
    // The trampoline page: a single 64 KiB region of guest memory.
    uint64_t trampoline_base = 0;
    static constexpr uint64_t TRAMPOLINE_PAGE_SIZE =
        GraphicThunk::TRAMPOLINE_SIZE * GraphicThunk::MAX_SYMBOLS;  // 64 KiB
    // Guest-visible scratch page for host→guest string returns
    // (glGetString, SDL_GetError, …). Ring-allocated. 64 KiB so the
    // wrap (which invalidates every outstanding cached pointer) needs
    // an implausible amount of live string traffic — SDL semantics
    // promise SDL_GetError/GetClipboardText results stay valid until
    // SDL_free, and a wrap hands those addresses to new strings.
    uint64_t string_cache_base = 0;
    static constexpr uint64_t STRING_CACHE_SIZE = 65536;
    uint32_t string_cache_off = 0;
    // 2026-08: SDL_free bookkeeping for the string cache. SDL_GetClipboardText
    // / SDL_GetError / joystick-name returns hand the guest a pointer INTO
    // this cache; the guest later SDL_free()s it. Host free() must never see
    // these GUEST addresses (they are not host heap pointers), so track each
    // allocation and let SDL_free reclaim the slot instead.
    std::unordered_map<uint32_t, uint32_t> string_cache_live_;   // cache offset -> len, allocated since last wrap
    std::vector<std::pair<uint32_t, uint32_t>> string_cache_freed_; // {offset, len} reusable slots (first-fit)
    // Registry: (library, symbol_name) → SymbolEntry.
    // We use a flat vector per-library for cache-friendly enumeration
    // (the dynamic linker iterates all symbols when populating its
    // global symbol table for a synthetic LoadedObject).
    struct LibTable {
        std::string lib;
        std::vector<SymbolEntry> entries;
    };
    std::vector<LibTable> libs_;
    // Reverse lookup: symbol_id → (lib index, entry index).
    std::vector<std::pair<uint32_t, uint32_t>> id_to_idx_;
    // SDL2-side state (only when BIFROST_THUNK_HAVE_SDL2 is defined).
#if defined(BIFROST_THUNK_HAVE_SDL2)
    bool          sdl_init_done = false;
    SDL_Window*   sdl_window    = nullptr;
    SDL_GLContext sdl_gl_ctx    = nullptr;
#endif
    // EGL-side state (only when BIFROST_THUNK_HAVE_EGL is defined).
#if defined(BIFROST_THUNK_HAVE_EGL)
    EGLDisplay    egl_display   = nullptr;
    EGLContext    egl_context   = nullptr;
#endif
    // Mutex protecting the registry (init() registers symbols, resolve()
    // reads them; concurrent calls from multiple guest threads must be
    // safe). The dispatch() path is lock-free after init() — it only
    // reads id_to_idx_, which is set once and never resized.
    std::mutex mu;
    // Leaf lock for mutable dispatch-time state (string-cache ring,
    // sdl_tex_sizes_, glfw_cbs_ + last-state maps, error slots,
    // query state). Rules: never held across the guest
    // borrow-CPU runner (re-entrant dispatch), host GL/SDL calls, or
    // Memory::* (own locks) — snapshot under it, act outside, like the
    // sync/UNMAP/wake sites already do. Separate from mu so registry
    // paths never interact with per-call state.
    std::mutex state_mu;
    // 1.5.4-alpha: GL state tracker for consistent query results.
    std::unique_ptr<GLStateTracker> gl_state_tracker_;
    // 1.5.4-alpha: SDL_Texture* → {w, h} so SDL_UpdateTexture's guest
    // pixel buffer can be bounced at its full size (pitch * height)
    // instead of the 64 KiB default (which truncates larger frames).
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> sdl_tex_sizes_;
    // 1.5.4-alpha: generic GLFW callback delivery. Each glfwSetXxxCallback
    // is a *_CB-policy symbol: dispatch() stores the guest callback here
    // instead of handing it to host GLFW (host can't invoke guest AArch64
    // callbacks). After glfwPollEvents/glfwWaitEvents (GLFW_POLL policy),
    // deliver_glfw_callbacks_() reads the host state, detects changes, and
    // fires the stored guest callbacks via the borrow-CPU runner.
    struct GlfwWindowCbs {
        uint64_t cursor = 0;       // (window, x, y)
        uint64_t key = 0;          // (window, key, scancode, action, mods)
        uint64_t mouse = 0;        // (window, button, action, mods)
        uint64_t framebuffer = 0;  // (window, w, h)
        uint64_t window_size = 0;  // (window, w, h)
        uint64_t focus = 0;        // (window, focused)
    };
    std::unordered_map<uint64_t, GlfwWindowCbs> glfw_cbs_;
    // Error callback is global (no window): void(int code, const char* desc).
    uint64_t glfw_error_cb_ = 0;
    // GL_KHR_debug callback is global (no window): void(source, type, id,
    // severity, length, message, userParam). Stored, never handed to host
    // GL (guest address as x86-64 code = SIGSEGV on first driver message).
    // Delivery is a future milestone; NULL is passed to the host so the
    // driver stays quiet instead of crashing.
    uint64_t gl_debug_cb_ = 0;
    uint64_t gl_debug_param_ = 0;
    // Last delivered host state per window for change detection. The
    // first poll only seeds (no spurious callback at startup), matching
    // GLFW semantics: callbacks fire on real events only.
    std::unordered_map<uint64_t, std::pair<double, double>> glfw_cursor_last_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> glfw_key_last_;
    std::unordered_map<uint64_t, uint32_t> glfw_mouse_last_;
    std::unordered_map<uint64_t, std::pair<int, int>> glfw_fb_last_;
    std::unordered_map<uint64_t, std::pair<int, int>> glfw_winsz_last_;
    std::unordered_map<uint64_t, bool> glfw_focus_last_;
    // Raw host GLFW fns, resolved at init (GLFW is dlopen'd by
    // register_known_symbols_). The GLFW_POLL delivery path needs the raw
    // host fns, not the entries' (which are trampolines).
    void* glfw_get_cursor_pos_fn_ = nullptr;
    void* glfw_get_key_fn_ = nullptr;
    void* glfw_get_key_scancode_fn_ = nullptr;
    void* glfw_get_mouse_button_fn_ = nullptr;
    void* glfw_get_window_size_fn_ = nullptr;
    void* glfw_get_framebuffer_size_fn_ = nullptr;
    void* glfw_get_window_attrib_fn_ = nullptr;
    void* glfw_set_error_callback_fn_ = nullptr;
    // 1.5.4-alpha: glfwCreateWindow HiDPI compensation. Resolved at init.
    void* glfw_get_window_content_scale_fn_ = nullptr;
    void* glfw_set_window_size_fn_ = nullptr;
    // Last error captured from host GLFW's error callback (ERROR_CB
    // delivery). Cleared after each delivery.
    int glfw_last_error_code_ = 0;
    std::string glfw_last_error_desc_;
    // Last error actually forwarded to the guest, for change-dedup.
    int glfw_last_delivered_err_code_ = 0;
    std::string glfw_last_delivered_err_desc_;
    // Borrow-CPU runner installed by the Emulator to invoke stored guest
    // GLFW callbacks (see GraphicThunk::GlfwCbRunner).
    GraphicThunk::GlfwCbRunner glfw_cb_runner_;
#if defined(BIFROST_THUNK_HAVE_SDL2)
    uint64_t sdl_filter_fn_ = 0, sdl_filter_userdata_ = 0;
    GraphicThunk::SdlFilterCpuFactory sdl_filter_cpu_factory_;
    // Host SDL may invoke filters outside a guest thunk (e.g. its event
    // thread). Never borrow another thread's live CPU in that case.
    inline static thread_local CPU* sdl_dispatch_cpu_ = nullptr;
    static int sdl_event_filter_(void* userdata, SDL_Event* event) noexcept {
        auto* self = static_cast<GraphicThunkImpl*>(userdata);
        uint64_t fn, data;
        {
            std::lock_guard<std::mutex> lock(self->state_mu);
            fn = self->sdl_filter_fn_; data = self->sdl_filter_userdata_;
        }
        if (!fn || !self->glfw_cb_runner_ || !self->mem) return 1;
        try {
            // Separate event storage at each nesting level, since a guest
            // filter can push an event and recursively enter this callback.
            static thread_local Memory* owner = nullptr;
            static thread_local std::vector<uint64_t> events;
            static thread_local size_t depth = 0;
            static thread_local std::unique_ptr<CPU> fallback;
            if (owner != self->mem) { events.clear(); fallback.reset(); owner = self->mem; }
            struct Depth {
                size_t& value;
                explicit Depth(size_t& v) : value(v) { ++value; }
                ~Depth() { --value; }
            };
            size_t slot = depth;
            Depth scope(depth);
            if (events.size() <= slot) events.resize(slot + 1);
            if (!events[slot]) events[slot] = self->mem->mmap_alloc(sizeof(SDL_Event));
            uint64_t guest_event = events[slot];
            if (!guest_event) return 0;
            self->mem->write(guest_event, event, sizeof(*event));
            CPU* cpu = sdl_dispatch_cpu_;
            if (!cpu) {
                if (!fallback && self->sdl_filter_cpu_factory_)
                    fallback = self->sdl_filter_cpu_factory_();
                if (!fallback) return 0;
                cpu = fallback.get();
            }
            int64_t args[] = {static_cast<int64_t>(data), static_cast<int64_t>(guest_event)};
            uint64_t result = self->glfw_cb_runner_(*cpu, fn, args, 2, nullptr, 0);
            self->mem->read(guest_event, event, sizeof(*event));
            return static_cast<int32_t>(result) != 0;
        } catch (...) {
            // Never unwind a C++ exception through SDL's C callback ABI.
            return 0;
        }
    }
#endif
    // Vulkan proc-address lookup wired by the Emulator (resolves a VK
    // symbol name to its guest trampoline via the DisplayThunk registry).
    GraphicThunk::VkProcLookup vk_proc_lookup_;
    // Borrow-CPU / thread-spawn runner installed by the Emulator for
    // SDL_CreateThread/SDL_WaitThread (see GraphicThunk::SdlThreadRunner).
    GraphicThunk::SdlThreadRunner sdl_thread_runner_;
    // Host SDL_sem* objects the guest created via SDL_CreateSemaphore.
    // On exit_group the SDL worker threads may be blocked inside host
    // SDL_SemWait; wake_sdl_semaphores() posts each so the blocked host
    // call returns and the SDL thread's interpreter loop can observe
    // cpu.running==false (see the SDL shutdown wakeup contract above).
    std::unordered_set<uint64_t> sdl_sems_;
    // Host SDL_SemPost function pointer, resolved at init (SDL3/SDL2-compat
    // exports SDL_SemPost; on the generic dispatch path the registered
    // entry.host_fn may be a guest trampoline, so use the raw dlsym result).
    void* sdl_sem_post_fn_ = nullptr;
    // 2026-08: glMapBuffer/glMapBufferRange/glUnmapBuffer bounce support.
    // The host glMapBuffer returns a HOST pointer the guest cannot deref,
    // so dispatch() instead allocates a guest-window bounce buffer (via
    // Memory::mmap_alloc, in the guest reservation so the guest JIT can
    // read/write it inline), seeds it from the host buffer on map
    // (GL_MAP_READ_BIT), and copies it back on unmap (GL_MAP_WRITE_BIT).
    // Raw host GL fns resolved at init (not the registered entries, whose
    // host_fn may be trampolines on some paths).
    void* gl_get_buffer_parameteriv_fn_ = nullptr;  // buffer size query
    void* gl_get_buffer_subdata_fn_ = nullptr;      // host buffer → bounce
    void* gl_buffer_subdata_fn_ = nullptr;          // bounce → host buffer
    // Active mappings: buffer id → bounce state (guest addr, size, offset
    // into the GL buffer, access bits). Keys on the buffer NAME so
    // glUnmapBuffer (which only knows the target) can find it via the
    // tracker's target→buffer binding.
    struct BufferMapping {
        uint64_t bounce = 0;   // guest address of the bounce allocation
        uint64_t size = 0;     // bytes mapped (range length / whole buffer)
        uint64_t offset = 0;   // byte offset into the GL buffer
        uint32_t target = 0;   // target used at map time (for writeback)
        uint32_t access = 0;   // GL_MAP_* bits from the caller
    };
    std::unordered_map<uint32_t, BufferMapping> gl_buffer_mappings_;
    // Find or create the LibTable for `lib`. Returns pointer into libs_.
    LibTable* find_or_create_lib_(const std::string& lib) {
        for (auto& l : libs_) {
            if (l.lib == lib) return &l;
        }
        libs_.push_back({lib, {}});
        return &libs_.back();
    }
    LibTable* find_lib_(const std::string& lib) {
        for (auto& l : libs_) {
            if (l.lib == lib) return &l;
        }
        return nullptr;
    }
    // Unified ring-cache allocator for host→guest byte blobs. Registers
    // the slot in string_cache_live_ (so SDL_free can reclaim it) and
    // clears the live/freed maps on wrap — BOTH properties the old
    // MIX_VERSION arm bypassed, desyncing free-slot reuse.
    uint64_t cache_host_bytes_(const void* bytes, size_t len) {
        if (!mem || !string_cache_base || !bytes || len == 0) return 0;
        if (len > STRING_CACHE_SIZE) len = STRING_CACHE_SIZE;
        // Slot selection under the leaf lock; the mem->write happens
        // outside it (Memory has its own locks).
        uint32_t off = 0;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lk(state_mu);
            // First-fit a freed slot large enough to hold the blob, so an
            // SDL_free of a previously cached string actually reuses space.
            for (auto it = string_cache_freed_.begin();
                 it != string_cache_freed_.end(); ++it) {
                if (it->second >= len) {
                    off = it->first;
                    string_cache_freed_.erase(it);
                    string_cache_live_[off] = static_cast<uint32_t>(len);
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                if (string_cache_off + len > STRING_CACHE_SIZE) {
                    string_cache_off = 0;
                    // Bump wrap: every prior slot is stale — drop the
                    // tracking so reuse can't resurrect old data.
                    string_cache_live_.clear();
                    string_cache_freed_.clear();
                }
                off = string_cache_off;
                string_cache_live_[off] = static_cast<uint32_t>(len);
                string_cache_off = static_cast<uint32_t>(
                    (string_cache_off + len + 7u) & ~7u);
            }
        }
        mem->write(string_cache_base + off, bytes, len);
        return string_cache_base + off;
    }
    uint64_t cache_host_string_(const char* host_str) {
        if (!host_str) return 0;
        return cache_host_bytes_(host_str, std::strlen(host_str) + 1);
    }
    // SDL_free on a pointer that may live in the string cache (a guest
    // address, NOT a host heap allocation). Reclaim the cache slot so the
    // next cache_host_string_ can reuse it. Anything outside the cache or
    // not tracked is a leak-safe no-op (arbitrary guest heap pointers pass
    // straight through — SDL's own allocator owns those on the real system).
    void free_cache_string_(uint64_t guest) {
        if (!guest || !string_cache_base) return;
        if (guest < string_cache_base) return;
        uint64_t off = guest - string_cache_base;
        if (off >= STRING_CACHE_SIZE) return;
        std::lock_guard<std::mutex> lk(state_mu);
        auto it = string_cache_live_.find(static_cast<uint32_t>(off));
        if (it == string_cache_live_.end()) return;
        string_cache_freed_.push_back({it->first, it->second});
        string_cache_live_.erase(it);
    }
    // Deliver stored GLFW callbacks after a host poll. For each
    // registered window, read the host state (cursor position, key/button
    // state, framebuffer/window size, focus) and fire any callback whose
    // value changed since the last delivery. The first poll only seeds
    // (no spurious callback at startup), matching GLFW semantics — the
    // game computes per-frame mouse deltas from the cursor callback.
    void deliver_glfw_callbacks_(CPU& cpu) {
        if (!glfw_cb_runner_) return;
        // ── Error callback (global, no window) ────────────────────────
        // Dedup: only forward when the code or description CHANGED since
        // the last delivery. The game polls glfwGetKey for keys < 32
        // (invalid in GLFW), which host GLFW flags as an error on every
        // poll — forwarding each would spam the guest error callback.
        // The slot is copied+cleared under the leaf lock (the host
        // trampoline writes it from any thread); everything after runs
        // unlocked.
        uint64_t error_cb = 0;
        int err_code = 0;
        std::string err_desc;
        int last_code = 0;
        std::string last_desc;
        {
            std::lock_guard<std::mutex> lk(state_mu);
            error_cb = glfw_error_cb_;
            err_code = glfw_last_error_code_;
            err_desc = glfw_last_error_desc_;
            last_code = glfw_last_delivered_err_code_;
            last_desc = glfw_last_delivered_err_desc_;
            glfw_last_error_code_ = 0;
            glfw_last_error_desc_.clear();
        }
        if (error_cb && err_code != 0 &&
            (err_code != last_code || err_desc != last_desc)) {
            int64_t iargs[2];
            iargs[0] = err_code;
            iargs[1] = cache_host_string_(err_desc.c_str());
            if (dbg().thunk_trace)
                fprintf(stderr, "[thunk] error cb → 0x%llx (%d, \"%s\")\n",
                        static_cast<unsigned long long>(error_cb),
                        err_code, err_desc.c_str());
            glfw_cb_runner_(cpu, error_cb, iargs, 2, nullptr, 0);
            std::lock_guard<std::mutex> lk(state_mu);
            glfw_last_delivered_err_code_ = err_code;
            glfw_last_delivered_err_desc_ = err_desc;
        }
        {
            std::lock_guard<std::mutex> lk(state_mu);
            if (glfw_cbs_.empty()) return;
        }
        // Reentrancy: the runner below executes GUEST code, which can
        // re-enter the thunk (glfwSetKeyCallback unregistering itself —
        // the one-shot "press any key" pattern — or a nested poll). That
        // mutates glfw_cbs_ (erase on last-slot-unregister) and the
        // last-state maps, invalidating any iterator we hold. So iterate
        // a snapshot of the callback registrations, and never hold a
        // reference into the last-state maps across a runner call.
        std::vector<std::pair<uint64_t, GlfwWindowCbs>> cbs_snapshot;
        {
            std::lock_guard<std::mutex> lk(state_mu);
            cbs_snapshot.reserve(glfw_cbs_.size());
            for (const auto& kv : glfw_cbs_) cbs_snapshot.push_back(kv);
        }
        using GetPosFn = void (*)(void*, double*, double*);
        using GetKeyFn = int (*)(void*, int);
        using GetScancodeFn = int (*)(int);
        using GetSizeFn = void (*)(void*, int*, int*);
        using GetAttribFn = int (*)(void*, int);
        auto getpos = reinterpret_cast<GetPosFn>(glfw_get_cursor_pos_fn_);
        auto getkey = reinterpret_cast<GetKeyFn>(glfw_get_key_fn_);
        auto getscan = reinterpret_cast<GetScancodeFn>(glfw_get_key_scancode_fn_);
        auto getbtn = reinterpret_cast<GetKeyFn>(glfw_get_mouse_button_fn_);
        auto getsz  = reinterpret_cast<GetSizeFn>(glfw_get_window_size_fn_);
        auto getfbs = reinterpret_cast<GetSizeFn>(glfw_get_framebuffer_size_fn_);
        auto getat  = reinterpret_cast<GetAttribFn>(glfw_get_window_attrib_fn_);
        constexpr int GLFW_FOCUSED = 0x00020001;
        constexpr int GLFW_KEY_SPACE = 32;
        constexpr int GLFW_KEY_LAST = 348;
        constexpr int GLFW_MOUSE_BUTTON_LAST = 7;
        constexpr int GLFW_RELEASE = 0, GLFW_PRESS = 1;
        for (const auto& entry : cbs_snapshot) {
            const uint64_t window = entry.first;
            const GlfwWindowCbs& cbs = entry.second;
            void* w = reinterpret_cast<void*>(window);
            int64_t iargs[8];
            double fargs[2];
            // ── Cursor position ────────────────────────────────────────
            if (cbs.cursor && getpos) {
                double x = 0.0, y = 0.0;
                getpos(w, &x, &y);
                bool fire = false;
                {
                    std::lock_guard<std::mutex> lk(state_mu);
                    auto it = glfw_cursor_last_.find(window);
                    if (it == glfw_cursor_last_.end()) {
                        glfw_cursor_last_[window] = {x, y};  // seed
                    } else if (it->second.first != x || it->second.second != y) {
                        it->second = {x, y};
                        fire = true;
                    }
                }
                if (fire) {
                    int64_t warg = static_cast<int64_t>(window);
                    fargs[0] = x; fargs[1] = y;
                    if (dbg().thunk_trace)
                        fprintf(stderr, "[thunk] cursor cb → 0x%llx (%.2f, %.2f)\n",
                                static_cast<unsigned long long>(cbs.cursor), x, y);
                    glfw_cb_runner_(cpu, cbs.cursor, &warg, 1, fargs, 2);
                }
            }
            // ── Keyboard state ─────────────────────────────────────────
            if (cbs.key && getkey) {
                bool seeded = false;
                {
                    std::lock_guard<std::mutex> lk(state_mu);
                    if (glfw_key_last_.find(window) == glfw_key_last_.end())
                        seeded = true;
                }
                if (seeded) {
                    // First poll: seed current state, don't fire (GLFW
                    // semantics — no spurious PRESS for keys already held).
                    std::vector<uint8_t> seed(GLFW_KEY_LAST, 0);
                    for (int k = GLFW_KEY_SPACE; k < GLFW_KEY_LAST; k++)
                        seed[k] = (getkey(w, k) == GLFW_PRESS) ? 1 : 0;
                    std::lock_guard<std::mutex> lk(state_mu);
                    glfw_key_last_[window] = std::move(seed);
                    continue;
                }
                // Copy the per-key state by value: the runner below can
                // re-enter delivery and insert into this map (rehash →
                // a held reference would dangle). Write back after.
                std::vector<uint8_t> lastv;
                {
                    std::lock_guard<std::mutex> lk(state_mu);
                    lastv = glfw_key_last_[window];
                }
                if (lastv.size() < (size_t)GLFW_KEY_LAST) continue;  // raced unregister
                // glfwGetKey only accepts keys >= GLFW_KEY_SPACE (32);
                // polling 0-31 makes host GLFW fire "Invalid key" errors.
                // note: mods stay 0 (polling cannot observe them) and only
                // PRESS/RELEASE edges fire (no GLFW_REPEAT synthesis).
                for (int k = GLFW_KEY_SPACE; k < GLFW_KEY_LAST; k++) {
                    int cur = getkey(w, k);
                    uint8_t pressed = (cur == GLFW_PRESS) ? 1 : 0;
                    if (lastv[k] != pressed) {
                        lastv[k] = pressed;
                        int sc = getscan ? getscan(k) : 0;
                        iargs[0] = window; iargs[1] = k; iargs[2] = sc;
                        iargs[3] = pressed ? GLFW_PRESS : GLFW_RELEASE; // action
                        iargs[4] = 0;                                   // mods
                        if (dbg().thunk_trace)
                            fprintf(stderr, "[thunk] key cb → 0x%llx (%d, %s)\n",
                                    static_cast<unsigned long long>(cbs.key), k,
                                    pressed ? "PRESS" : "RELEASE");
                        glfw_cb_runner_(cpu, cbs.key, iargs, 5, nullptr, 0);
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(state_mu);
                    glfw_key_last_[window] = std::move(lastv);
                }
            }
            // ── Mouse button state ─────────────────────────────────────
            if (cbs.mouse && getbtn) {
                bool seeded = false;
                {
                    std::lock_guard<std::mutex> lk(state_mu);
                    if (glfw_mouse_last_.find(window) == glfw_mouse_last_.end())
                        seeded = true;
                }
                if (seeded) {
                    // First poll: seed current state, don't fire.
                    uint32_t seed = 0;
                    for (int b = 0; b <= GLFW_MOUSE_BUTTON_LAST; b++)
                        if (getbtn(w, b) == GLFW_PRESS) seed |= (1u << b);
                    std::lock_guard<std::mutex> lk(state_mu);
                    glfw_mouse_last_[window] = seed;
                    continue;
                }
                uint32_t last;
                {
                    std::lock_guard<std::mutex> lk(state_mu);
                    last = glfw_mouse_last_[window];
                }
                for (int b = 0; b <= GLFW_MOUSE_BUTTON_LAST; b++) {
                    int cur = getbtn(w, b);
                    uint8_t pressed = (cur == GLFW_PRESS) ? 1 : 0;
                    uint32_t bit = 1u << b;
                    if (!!(last & bit) != pressed) {
                        if (pressed) last |= bit; else last &= ~bit;
                        iargs[0] = window; iargs[1] = b;
                        iargs[2] = pressed ? GLFW_PRESS : GLFW_RELEASE;
                        iargs[3] = 0;  // mods
                        if (dbg().thunk_trace)
                            fprintf(stderr, "[thunk] mouse cb → 0x%llx (btn %d, %s)\n",
                                    static_cast<unsigned long long>(cbs.mouse), b,
                                    pressed ? "PRESS" : "RELEASE");
                        glfw_cb_runner_(cpu, cbs.mouse, iargs, 4, nullptr, 0);
                    }
                }
                // By-key write-back (the runner may have re-entered and
                // rehashed this map — never hold an iterator across it).
                {
                    std::lock_guard<std::mutex> lk(state_mu);
                    glfw_mouse_last_[window] = last;
                }
            }
            // ── Framebuffer size ───────────────────────────────────────
            if (cbs.framebuffer && getfbs) {
                int fbw = 0, fbh = 0;
                getfbs(w, &fbw, &fbh);
                bool fire = false;
                {
                    std::lock_guard<std::mutex> lk(state_mu);
                    auto it = glfw_fb_last_.find(window);
                    if (it == glfw_fb_last_.end()) {
                        glfw_fb_last_[window] = {fbw, fbh};  // seed
                    } else if (it->second.first != fbw || it->second.second != fbh) {
                        it->second = {fbw, fbh};
                        fire = true;
                    }
                }
                if (fire) {
                    iargs[0] = window; iargs[1] = fbw; iargs[2] = fbh;
                    if (dbg().thunk_trace)
                        fprintf(stderr, "[thunk] fb cb → 0x%llx (%d, %d)\n",
                                static_cast<unsigned long long>(cbs.framebuffer), fbw, fbh);
                    glfw_cb_runner_(cpu, cbs.framebuffer, iargs, 3, nullptr, 0);
                }
            }
            // ── Window size ────────────────────────────────────────────
            if (cbs.window_size && getsz) {
                int ww = 0, wh = 0;
                getsz(w, &ww, &wh);
                bool fire = false;
                {
                    std::lock_guard<std::mutex> lk(state_mu);
                    auto it = glfw_winsz_last_.find(window);
                    if (it == glfw_winsz_last_.end()) {
                        glfw_winsz_last_[window] = {ww, wh};  // seed
                    } else if (it->second.first != ww || it->second.second != wh) {
                        it->second = {ww, wh};
                        fire = true;
                    }
                }
                if (fire) {
                    iargs[0] = window; iargs[1] = ww; iargs[2] = wh;
                    if (dbg().thunk_trace)
                        fprintf(stderr, "[thunk] winsz cb → 0x%llx (%d, %d)\n",
                                static_cast<unsigned long long>(cbs.window_size), ww, wh);
                    glfw_cb_runner_(cpu, cbs.window_size, iargs, 3, nullptr, 0);
                }
            }
            // ── Window focus ───────────────────────────────────────────
            if (cbs.focus && getat) {
                bool focused = getat(w, GLFW_FOCUSED) != 0;
                bool fire = false;
                {
                    std::lock_guard<std::mutex> lk(state_mu);
                    auto it = glfw_focus_last_.find(window);
                    if (it == glfw_focus_last_.end()) {
                        glfw_focus_last_[window] = focused;  // seed
                    } else if (it->second != focused) {
                        it->second = focused;
                        fire = true;
                    }
                }
                if (fire) {
                    iargs[0] = window; iargs[1] = focused ? 1 : 0;
                    if (dbg().thunk_trace)
                        fprintf(stderr, "[thunk] focus cb → 0x%llx (%d)\n",
                                static_cast<unsigned long long>(cbs.focus),
                                focused ? 1 : 0);
                    glfw_cb_runner_(cpu, cbs.focus, iargs, 2, nullptr, 0);
                }
            }
        }
    }
};
}  // namespace arm64emu
