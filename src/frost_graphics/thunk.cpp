// frost_graphics/thunk.cpp — graphic API thunking (v1.4.5-alpha).
//
// 1.5.4-alpha: also hosts the FrostGraphics::audio_thunk() and
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
// triangle, swap buffers). Real-world GL programs will hit unthunked
// entry points and get a stub that logs + returns 0.
//
// Supported libraries (partial):
//   - libGL.so: glClear, glClearColor, glBegin, glEnd, glVertex3f,
//     glColor3f, glFlush, glFinish, glGetError, glEnable, glDisable,
//     glViewport, glMatrixMode, glLoadIdentity, glOrtho
//   - libEGL.so: eglGetDisplay, eglInitialize, eglChooseConfig,
//     eglCreateContext, eglMakeCurrent, eglSwapBuffers, eglTerminate
//   - libSDL2.so: SDL_Init, SDL_Quit, SDL_CreateWindow,
//     SDL_GL_CreateContext, SDL_GL_SwapWindow, SDL_PollEvent,
//     SDL_GetWindowSurface, SDL_UpdateWindowSurface
//   - libGLESv2.so: glClear, glClearColor, glFlush, glGetError,
//     glEnable, glDisable, glViewport (subset of libGL)
//
// ── How to use ───────────────────────────────────────────────────────
// Set BIFROST_THUNK_GRAPHICS=1 in the environment. The emulator's
// dynamic linker (src/frontend/dynamic_linker.cpp) checks this env var
// and, when set, consults FrostGraphics::thunk() to resolve symbols
// from the graphic libraries before falling back to the host's
// dlopen/dlsym.
//
// Without BIFROST_THUNK_GRAPHICS=1, the thunk returns 0 for every
// lookup — the guest falls back to its own software rendering (or
// fails gracefully if it has no software path).
//
// ── Limitations ──────────────────────────────────────────────────────
//   - No state tracking: the thunk doesn't maintain GL state across
//     calls (e.g. glEnable(GL_DEPTH_TEST) is forwarded but not
//     recorded). This is fine for immediate-mode GL but breaks
//     retained-mode programs.
//   - No shader translation: GLSL shaders compiled by the guest are
//     passed to the host's glCompileShader verbatim. AArch64 and
//     x86-64 GLSL are identical (it's a text format), so this works.
//   - No EGL surface management: the thunk creates a single hidden
//     SDL2 window for EGL and ignores the guest's window/surface
//     requests. Real EGL programs that create multiple surfaces will
//     break.
//   - No GLESv1 (fixed-function) support: only GLESv2 (programmable).
//   - Pointer arguments are translated assuming the guest pointer is
//     in the emulator's address space (via Memory::host_addr()). This
//     works for heap/stack pointers but NOT for pointers the guest
//     got from mmap'd host resources (rare).
//   - Variadic functions (printf-style) are NOT supported — the thunk
//     passes exactly 8 args, ignoring variadic extras.
//
// See frost/thunk.hpp for the class definition and frost/graphics.hpp
// for the FrostGraphics::thunk() accessor.
#include <map>
#include "frost/graphics.hpp"
#include "frost/thunk.hpp"
#include "frost/audio_thunk.hpp"    // 1.5.4-alpha: AudioThunk full def
#include "frost/display_thunk.hpp"  // 1.5.4-alpha: DisplayThunk full def
#include "frost/gl_state.hpp"       // 1.5.4-alpha: GLStateTracker
#include "frost/android_surface.hpp" // Android NativeActivity surface layer
#include "opgen_thunk.hpp"          // 1.5.4-alpha: symbol signature table
#include "thunk_common.hpp"         // shared SymbolEntry + trampoline encodings
#include "debug_flags.h"            // dbg() — cached trace gates (BIFROST_THUNK_TRACE)
#include "core/cpu.h"
#include "core/memory.h"
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
// Host GL/EGL/SDL2 headers — optional. If not available, the thunk
// compiles but all entry points return stubs. The Makefile sets
// BIFROST_THUNK_HAVE_GL / BIFROST_THUNK_HAVE_EGL / BIFROST_THUNK_HAVE_SDL2
// when the host has the dev headers installed.
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
// ── GraphicThunkImpl — the real implementation (pimpl) ────────────────
// The GraphicThunk class in frost/thunk.hpp exposes only void* opaque
// members to keep the header free of GL/EGL/SDL2 includes. The real
// state lives here.
//
// SymbolEntry is the shared registry entry defined in thunk_common.hpp.
// (Do NOT re-define arm64emu::SymbolEntry in this TU — a second TU-local
// definition with a different layout is an ODR violation that corrupts
// the vector stride after COMDAT folding.)
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
    // Memory::mmap_alloc, inside the 4 GiB direct window so the guest JIT
    // can read/write it fast), seeds it from the host buffer on map
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
        // First-fit a freed slot large enough to hold the blob, so an
        // SDL_free of a previously cached string actually reuses the space.
        for (auto it = string_cache_freed_.begin();
             it != string_cache_freed_.end(); ++it) {
            if (it->second >= len) {
                uint32_t off = it->first;
                string_cache_freed_.erase(it);
                mem->write(string_cache_base + off, bytes, len);
                string_cache_live_[off] = static_cast<uint32_t>(len);
                return string_cache_base + off;
            }
        }
        if (string_cache_off + len > STRING_CACHE_SIZE) {
            string_cache_off = 0;
            // Bump wrap: every prior slot is stale — drop the tracking
            // (live and freed) so freed-slot reuse can't resurrect old data.
            string_cache_live_.clear();
            string_cache_freed_.clear();
        }
        uint64_t guest = string_cache_base + string_cache_off;
        mem->write(guest, bytes, len);
        string_cache_live_[string_cache_off] = static_cast<uint32_t>(len);
        string_cache_off = static_cast<uint32_t>(
            (string_cache_off + len + 7u) & ~7u);
        return guest;
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
        if (glfw_error_cb_ && glfw_last_error_code_ != 0 &&
            (glfw_last_error_code_ != glfw_last_delivered_err_code_ ||
             glfw_last_error_desc_ != glfw_last_delivered_err_desc_)) {
            int64_t iargs[2];
            iargs[0] = glfw_last_error_code_;
            iargs[1] = cache_host_string_(glfw_last_error_desc_.c_str());
            if (dbg().thunk_trace)
                fprintf(stderr, "[thunk] error cb → 0x%llx (%d, \"%s\")\n",
                        static_cast<unsigned long long>(glfw_error_cb_),
                        glfw_last_error_code_, glfw_last_error_desc_.c_str());
            glfw_cb_runner_(cpu, glfw_error_cb_, iargs, 2, nullptr, 0);
            glfw_last_delivered_err_code_ = glfw_last_error_code_;
            glfw_last_delivered_err_desc_ = glfw_last_error_desc_;
        }
        glfw_last_error_code_ = 0;
        glfw_last_error_desc_.clear();
        if (glfw_cbs_.empty()) return;
        // Reentrancy: the runner below executes GUEST code, which can
        // re-enter the thunk (glfwSetKeyCallback unregistering itself —
        // the one-shot "press any key" pattern — or a nested poll). That
        // mutates glfw_cbs_ (erase on last-slot-unregister) and the
        // last-state maps, invalidating any iterator we hold. So iterate
        // a snapshot of the callback registrations, and never hold a
        // reference into the last-state maps across a runner call.
        std::vector<std::pair<uint64_t, GlfwWindowCbs>> cbs_snapshot;
        cbs_snapshot.reserve(glfw_cbs_.size());
        for (const auto& kv : glfw_cbs_) cbs_snapshot.push_back(kv);
        using GetPosFn = void (*)(void*, double*, double*);
        using GetKeyFn = int (*)(void*, int);
        using GetSizeFn = void (*)(void*, int*, int*);
        using GetAttribFn = int (*)(void*, int);
        auto getpos = reinterpret_cast<GetPosFn>(glfw_get_cursor_pos_fn_);
        auto getkey = reinterpret_cast<GetKeyFn>(glfw_get_key_fn_);
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
                auto it = glfw_cursor_last_.find(window);
                if (it == glfw_cursor_last_.end()) {
                    glfw_cursor_last_[window] = {x, y};  // seed
                } else if (it->second.first != x || it->second.second != y) {
                    it->second = {x, y};
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
                auto it = glfw_key_last_.find(window);
                if (it == glfw_key_last_.end()) {
                    // First poll: seed current state, don't fire (GLFW
                    // semantics — no spurious PRESS for keys already held).
                    std::vector<uint8_t> seed(GLFW_KEY_LAST, 0);
                    for (int k = GLFW_KEY_SPACE; k < GLFW_KEY_LAST; k++)
                        seed[k] = (getkey(w, k) == GLFW_PRESS) ? 1 : 0;
                    glfw_key_last_[window] = std::move(seed);
                    continue;
                }
                // Copy the per-key state by value: the runner below can
                // re-enter delivery and insert into this map (rehash →
                // a held reference would dangle). Write back after.
                auto lastv = it->second;
                // glfwGetKey only accepts keys >= GLFW_KEY_SPACE (32);
                // polling 0-31 makes host GLFW fire "Invalid key" errors.
                for (int k = GLFW_KEY_SPACE; k < GLFW_KEY_LAST; k++) {
                    int cur = getkey(w, k);
                    uint8_t pressed = (cur == GLFW_PRESS) ? 1 : 0;
                    if (lastv[k] != pressed) {
                        lastv[k] = pressed;
                        iargs[0] = window; iargs[1] = k; iargs[2] = 0;  // scancode
                        iargs[3] = pressed ? GLFW_PRESS : GLFW_RELEASE; // action
                        iargs[4] = 0;                                   // mods
                        if (dbg().thunk_trace)
                            fprintf(stderr, "[thunk] key cb → 0x%llx (%d, %s)\n",
                                    static_cast<unsigned long long>(cbs.key), k,
                                    pressed ? "PRESS" : "RELEASE");
                        glfw_cb_runner_(cpu, cbs.key, iargs, 5, nullptr, 0);
                    }
                }
                glfw_key_last_[window] = std::move(lastv);
            }
            // ── Mouse button state ─────────────────────────────────────
            if (cbs.mouse && getbtn) {
                auto it = glfw_mouse_last_.find(window);
                if (it == glfw_mouse_last_.end()) {
                    // First poll: seed current state, don't fire.
                    uint32_t seed = 0;
                    for (int b = 0; b <= GLFW_MOUSE_BUTTON_LAST; b++)
                        if (getbtn(w, b) == GLFW_PRESS) seed |= (1u << b);
                    glfw_mouse_last_[window] = seed;
                    continue;
                }
                uint32_t last = it->second;
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
                // rehashed this map — never hold `it` across it).
                glfw_mouse_last_[window] = last;
            }
            // ── Framebuffer size ───────────────────────────────────────
            if (cbs.framebuffer && getfbs) {
                int fbw = 0, fbh = 0;
                getfbs(w, &fbw, &fbh);
                auto it = glfw_fb_last_.find(window);
                if (it == glfw_fb_last_.end()) {
                    glfw_fb_last_[window] = {fbw, fbh};  // seed
                } else if (it->second.first != fbw || it->second.second != fbh) {
                    it->second = {fbw, fbh};
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
                auto it = glfw_winsz_last_.find(window);
                if (it == glfw_winsz_last_.end()) {
                    glfw_winsz_last_[window] = {ww, wh};  // seed
                } else if (it->second.first != ww || it->second.second != wh) {
                    it->second = {ww, wh};
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
                auto it = glfw_focus_last_.find(window);
                if (it == glfw_focus_last_.end()) {
                    glfw_focus_last_[window] = focused;  // seed
                } else if (it->second != focused) {
                    it->second = focused;
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
// ── GraphicThunk method implementations ───────────────────────────────
GraphicThunk::GraphicThunk() {
    impl_ = std::make_unique<GraphicThunkImpl>();
    // 1.5.4-alpha: thunking is now ENABLED BY DEFAULT.
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
    // 1.5.4-alpha: initialize GL state tracker.
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
    // 1.5.4-alpha: symbol_id includes ID_BASE_GRAPHICS to
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
int64_t GraphicThunk::dispatch(CPU& cpu, uint32_t symbol_id) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) {
        return -ENOSYS;
    }
    if ((symbol_id & GraphicThunk::ID_MASK) != GraphicThunk::ID_BASE_GRAPHICS) {
        return -ENOENT;
    }
    uint32_t local_id = symbol_id - GraphicThunk::ID_BASE_GRAPHICS;
    if (local_id >= impl_->id_to_idx_.size()) {
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: unknown symbol_id=%u\n", symbol_id);
        }
        return -ENOENT;
    }
    auto [lib_idx, ent_idx] = impl_->id_to_idx_[local_id];
    const auto& entry = impl_->libs_[lib_idx].entries[ent_idx];

    // ── SDL_mixer policies ────────────────────────────────────────────
    // SDL_mixer is never linked into the emulator, so every Mix_* entry's
    // host_fn is NULL and the generic stub path below would return 0. Two
    // symbols need non-zero semantics:
    //   MIX_VERSION   -> Mix_Linked_Version(): the game dereferences the
    //                    returned SDL_version* (reads major/minor/patch),
    //                    so return a guest-addressable SDL_version{2,0,1}.
    //   MIX_OPEN_AUDIO-> Mix_OpenAudio/OpenAudioDevice: return -1 so the
    //                    game takes its graceful no-audio path (it checks
    //                    for != 0 and disables sound/music) instead of
    //                    believing audio is open.
    if (entry.spec) {
        thunk::Policy pol = entry.spec->policy;
        if (pol == thunk::Policy::MIX_VERSION) {
            // SDL_version is {Uint8 major, Uint8 minor, Uint8 patch}.
            static const uint8_t kVer[8] = {2, 0, 1, 0, 0, 0, 0, 0};
            uint64_t guest = impl_->cache_host_bytes_(kVer, sizeof(kVer));
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] Mix_Linked_Version -> {2,0,1} @ 0x%llx\n",
                        static_cast<unsigned long long>(guest));
            }
            cpu.regs[0] = guest;
            return 0;
        }
        if (pol == thunk::Policy::MIX_OPEN_AUDIO) {
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] %s -> -1 (audio unavailable)\n",
                        entry.name.c_str());
            }
            cpu.regs[0] = static_cast<uint64_t>(-1);
            return 0;
        }
        // NOTE: SDL2 audio symbols (SDL_OpenAudio/QueueAudio/etc.) are
        // intentionally NOT registered here — they live in the AudioThunk
        // registry (callback mode + pump thread), and the dynlinker
        // resolves them to AudioThunk trampolines directly.
        if (!strcmp(entry.name.c_str(), "SDL_Vulkan_GetVkGetInstanceProcAddr")) {
            // vkQuake stores this result and calls it to load EVERY
            // Vulkan entry point — it must be a GUEST-CALLABLE trampoline
            // (DisplayThunk registry), never the raw host fn pointer.
            cpu.regs[0] = impl_->vk_proc_lookup_
                ? impl_->vk_proc_lookup_("vkGetInstanceProcAddr") : 0;
            return 0;
        }
        if (pol == thunk::Policy::SDL_STUB0) {
            // vkQuake traps: SDL_SetWindowIcon derefs a guest SDL_Surface
            // whose `pixels` is a GUEST address (host deref = SIGSEGV).
            // SDL_GetWindowWMInfo is FATAL-failed by vkQuake when it
            // returns false, so it must reach the host: the window handle
            // round-trips verbatim and the SDL_SysWMinfo buffer lives on
            // the guest stack (inside the direct window) -> alias pass.
            // The host pointers the host writes into it are opaque to the
            // game (it only checks the BOOL return).
            const char* nm = entry.name.c_str();
            if (!strcmp(nm, "SDL_GetWindowWMInfo")) {
                using WmFn = int (*)(void*, void*);
                auto fn = reinterpret_cast<WmFn>(entry.host_fn);
                uint8_t* hp = (impl_ && impl_->mem)
                    ? impl_->mem->guest_to_host_ptr(cpu.regs[1]) : nullptr;
                cpu.regs[0] = (fn && hp)
                    ? static_cast<uint64_t>(fn(reinterpret_cast<void*>(
                          cpu.regs[0]), hp))
                    : 0;
                return 0;
            }
            cpu.regs[0] = 0;
            return 0;
        }
        if (pol == thunk::Policy::SDL_EVENT_FILTER) {
            // in_sdl.c probes/sets the event filter to suppress mouse
            // motion while unfocused. Never hand the GUEST callback to
            // host SDL2: Set returns success (filter silently ignored —
            // events still flow via PollEvent), Get reports "no filter".
            const char* nm = entry.name.c_str();
            cpu.regs[0] = strcmp(nm, "SDL_SetEventFilter") == 0 ? 1 : 0;
            return 0;
        }
        if (pol == thunk::Policy::NONE && impl_ && impl_->mem) {
            // SDL2 display-mode getters RETURN a const SDL_DisplayMode*
            // (host static memory) — no out-param. Copy the 24-byte struct
            // into a cached guest block so the guest can deref it.
            // SDL_DisplayMode { Uint32 format; int w, h, refresh_rate;
            //                   void *driverdata; } = 24 bytes.
            const char* nm = entry.name.c_str();
            if (strcmp(nm, "SDL_GetCurrentDisplayMode") == 0 ||
                strcmp(nm, "SDL_GetDesktopDisplayMode") == 0) {
                // On sdl2-compat hosts the resolved impl may be SDL3-
                // flavored: bool f(SDL_DisplayID, SDL_DisplayMode* out).
                // ALWAYS pass an explicit output buffer — a one-arg call
                // makes it write through whatever sits in RSI (observed:
                // our own strcmp literal, → SIGSEGV inside libSDL2).
                // SDL2 ABI returns const SDL_DisplayMode* (never 0/1);
                // SDL3 ABI returns bool and fills buf.
                using ModeFn2 = void* (*)(int, void*);
                auto fn = reinterpret_cast<ModeFn2>(entry.host_fn);
                if (!fn) { cpu.regs[0] = 0; return 0; }
                alignas(8) uint8_t buf[32];
                std::memset(buf, 0, sizeof buf);
                void* m = fn(static_cast<int>(cpu.regs[0]), buf);
                const void* src;
                if (reinterpret_cast<uintptr_t>(m) <= 1) {
                    // SDL3 flavor: bool result + filled buf.
                    src = m ? buf : nullptr;
                } else {
                    src = m;  // SDL2 flavor: direct pointer
                }
                if (!src) { cpu.regs[0] = 0; return 0; }
                // One reused guest block per symbol (games poll per frame).
                static std::mutex mu;
                static std::map<std::string, uint64_t> cache;
                uint64_t g = 0;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    auto it = cache.find(nm);
                    if (it != cache.end()) {
                        g = it->second;
                    } else {
                        g = impl_->mem->mmap_alloc(32);
                        if (g) cache[nm] = g;
                    }
                }
                if (!g) { cpu.regs[0] = 0; return 0; }  // alloc failed
                try { impl_->mem->write(g, m, 24); }
                catch (...) { cpu.regs[0] = 0; return 0; }
                cpu.regs[0] = g;
                return 0;
            }
            // SDL_RWFromMem / SDL_RWFromConstMem(mem, size): SDL KEEPS the
            // mem pointer in the RWops and reads it on every later
            // SDL_LoadBMP_RW/etc. A temporary per-call bounce buffer dies
            // at dispatch return → use-after-free + heap corruption in
            // whatever reallocates that memory. Use the stable direct-
            // window alias when possible; otherwise take a persistent
            // host copy (tracked, freed at thunk shutdown).
            if (strcmp(nm, "SDL_RWFromMem") == 0 ||
                strcmp(nm, "SDL_RWFromConstMem") == 0) {
                using RwFn = void* (*)(void*, size_t);
                auto fn = reinterpret_cast<RwFn>(entry.host_fn);
                if (!fn) { cpu.regs[0] = 0; return 0; }
                uint64_t src = cpu.regs[0];
                size_t sz = static_cast<size_t>(cpu.regs[1]);
                uint8_t* hp = impl_->mem->guest_to_host_ptr(src);
                if (!hp && sz && sz < (64u << 20)) {
                    hp = static_cast<uint8_t*>(std::malloc(sz));
                    if (hp) {
                        try { impl_->mem->read(src, hp, sz); }
                        catch (...) { std::memset(hp, 0, sz); }
                        std::lock_guard<std::mutex> lk(impl_->mu);
                        impl_->rw_kept_.emplace_back(hp);
                    }
                }
                cpu.regs[0] = reinterpret_cast<uint64_t>(fn(hp, sz));
                return 0;
            }
        }
        if (pol == thunk::Policy::SDLVK_EXT) {
            // SDL_Vulkan_GetInstanceExtensions(window, pCount*, pNames*):
            // pNames is the CALLER'S ARRAY to fill directly with char*
            // (NOT a slot receiving a new char**). Fill it with guest
            // string-cache copies of the host names.
            using VkExtFn = int (*)(void*, unsigned*, const char**);
            auto fn = reinterpret_cast<VkExtFn>(entry.host_fn);
            if (!fn || !impl_->mem) { cpu.regs[0] = 0; return 0; }
            void* win = reinterpret_cast<void*>(cpu.regs[0]);
            uint64_t pcount = cpu.regs[1], pnames = cpu.regs[2];
            unsigned count = 0;
            bool ok1 = fn(win, &count, nullptr) != 0;
            if (dbg().thunk_trace)
                fprintf(stderr, "[thunk] SDLVK_EXT: win=%p ok=%d count=%u pnames=0x%llx\n",
                        win, ok1 ? 1 : 0, count, (unsigned long long)pnames);
            if (!ok1) { cpu.regs[0] = 0; return 0; }
            if (pnames && count) {
                std::vector<const char*> names(count, nullptr);
                if (!fn(win, &count, names.data())) {
                    cpu.regs[0] = 0;
                    return 0;
                }
                for (unsigned i = 0; i < count; i++) {
                    uint64_t gs = impl_->cache_host_string_(names[i]);
                    if (dbg().thunk_trace)
                        fprintf(stderr, "[thunk] SDLVK_EXT: names[%u]=%s -> gs=0x%llx\n",
                                i, names[i] ? names[i] : "(null)", (unsigned long long)gs);
                    impl_->mem->write(pnames + static_cast<uint64_t>(i) * 8,
                                      &gs, 8);
                }
            }
            if (pcount) impl_->mem->write(pcount, &count, sizeof(count));
            cpu.regs[0] = 1;
            return 0;
        }
        if (pol == thunk::Policy::SDL_ALLOC) {
            // SDL_malloc/calloc/realloc: return GUEST-window memory so the
            // guest can deref the pointer. mmap_alloc is zero-filled
            // (calloc semantics for free). realloc copies the old block's
            // contents (size looked up from the allocation map).
            uint64_t ret = 0;
            if (impl_ && impl_->mem) {
                const char* nm = entry.name.c_str();
                if (!strcmp(nm, "SDL_malloc")) {
                    ret = impl_->mem->mmap_alloc(cpu.regs[0]);
                } else if (!strcmp(nm, "SDL_calloc")) {
                    ret = impl_->mem->mmap_alloc(cpu.regs[0] * cpu.regs[1]);
                } else { // SDL_realloc(ptr, size)
                    uint64_t oldp = cpu.regs[0], newsz = cpu.regs[1];
                    ret = impl_->mem->mmap_alloc(newsz);
                    if (oldp && ret) {
                        // Point lookup — the old code copied the whole
                        // allocation map per call (O(live allocs)).
                        auto [found, oldsz] = impl_->mem->find_allocation(oldp);
                        if (found) {
                            uint64_t n = std::min(oldsz, newsz);
                            uint8_t* hp = impl_->mem->guest_to_host_ptr(oldp);
                            uint8_t* hq = impl_->mem->guest_to_host_ptr(ret);
                            if (hp && hq) memcpy(hq, hp, n);
                            impl_->mem->untrack_allocation(oldp, oldsz);
                        }
                    }
                }
            }
            cpu.regs[0] = ret;
            return 0;
        }
        if (pol == thunk::Policy::SDL_FREE) {
            // SDL_free(ptr): the guest may free a string returned by
            // SDL_GetClipboardText / SDL_GetError / joystick-name getters,
            // which lives in the GUEST string cache — reclaim the cache slot
            // instead of calling host free() (the pointer is a guest address,
            // not a host heap allocation). Pointers handed out by SDL_ALLOC
            // (mmap_alloc'd guest-window blocks) are untracked here so the
            // page cap reflects live memory. Any other guest pointer is a
            // leak-safe no-op. Must run BEFORE the generic host-fn stub
            // check (SDL_free has no host call at all).
            if (impl_ && impl_->mem) {
                uint64_t p = cpu.regs[0];
                bool freed = false;
                if (p) {
                    auto [found, sz] = impl_->mem->find_allocation(p);
                    if (found) {
                        impl_->mem->untrack_allocation(p, sz);
                        freed = true;
                    }
                }
                if (!freed && impl_->string_cache_base)
                    impl_->free_cache_string_(p);
            } else if (impl_ && impl_->string_cache_base) {
                impl_->free_cache_string_(cpu.regs[0]);
            }
            cpu.regs[0] = 0;
            return 0;
        }
        if (pol == thunk::Policy::THREAD_CREATE) {
            // SDL_CreateThread(fn, name, data): spawn a REAL guest thread
            // running fn(data) on its own CPU. The Emulator wires the
            // runner; it returns a guest SDL_Thread* handle (or 0 on
            // failure, which the game cbz-checks → fatal error path).
            if (impl_->sdl_thread_runner_) {
                uint64_t handle = impl_->sdl_thread_runner_(
                    cpu, 0, cpu.regs[0], cpu.regs[1], cpu.regs[2]);
                if (dbg().thunk_trace) {
                    fprintf(stderr, "[thunk] SDL_CreateThread(fn=0x%llx, "
                            "name=0x%llx, data=0x%llx) -> 0x%llx\n",
                            static_cast<unsigned long long>(cpu.regs[0]),
                            static_cast<unsigned long long>(cpu.regs[1]),
                            static_cast<unsigned long long>(cpu.regs[2]),
                            static_cast<unsigned long long>(handle));
                }
                cpu.regs[0] = handle;
            } else {
                cpu.regs[0] = 0;
            }
            return 0;
        }
        if (pol == thunk::Policy::THREAD_WAIT) {
            // SDL_WaitThread(thread, status): block until the thread's
            // function returns, write its exit code to *status (if
            // non-null), and free the thread's resources.
            if (impl_->sdl_thread_runner_) {
                impl_->sdl_thread_runner_(cpu, 1, cpu.regs[0], cpu.regs[1], 0);
            }
            cpu.regs[0] = 0;
            return 0;
        }
    }

    if (!entry.host_fn) {
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: %s (stub, returns 0)\n",
                    entry.name.c_str());
        }
        cpu.regs[0] = 0;
        return 0;
    }

    // ── Android surface interception (eglGetDisplay) ─────────────────
    // On a Wayland host, EGL_DEFAULT_DISPLAY must wrap SDL's wl_display —
    // the SAME connection the ANativeWindow's wl_surface lives on — or
    // Mesa rejects eglCreateWindowSurface cross-connection.
    static const bool no_disp_subst =
        (getenv("BIFROST_NO_DISPLAY_SUBST") != nullptr);
    if (entry.name == "eglGetDisplay" && !no_disp_subst) {
        auto& mgr = frost::AndroidSurfaceManager::instance();
        if (cpu.regs[0] == 0 && mgr.ready() && mgr.native_display()) {
            cpu.regs[0] =
                reinterpret_cast<uint64_t>(mgr.native_display());
        }
    }
    // ── Android surface interception (eglCreateWindowSurface) ────────
    // When the native-window argument is an ANativeWindow shim handle
    // (Android surface layer), substitute the HOST native window value so
    // host EGL creates a real window surface. eglSwapBuffers then presents
    // through the normal host path with no further interception.
    if (entry.name == "eglCreateWindowSurface") {
        auto& mgr = frost::AndroidSurfaceManager::instance();
        if (frost::AndroidSurfaceManager::is_shim(cpu.regs[2])) {
            uint64_t hw = mgr.host_native_window();
            if (hw == 0) {
                // No host window backing the shim — EGL_NO_SURFACE.
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] eglCreateWindowSurface: "
                            "android shim without host window\n");
                cpu.regs[0] = 0;
                return 0;
            }
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] eglCreateWindowSurface: android "
                        "shim 0x%llx -> host window 0x%llx\n",
                        static_cast<unsigned long long>(cpu.regs[2]),
                        static_cast<unsigned long long>(hw));
            }
            cpu.regs[2] = hw;
        }
    }

    // ── GLFW callback registration (CURSOR_CB/KEY_CB/MOUSE_CB/…) ─────
    // 1.5.4-alpha: the guest callback is AArch64 code host GLFW cannot
    // invoke, so we store it keyed by window and deliver it from the
    // GLFW_POLL path. NEVER hand the guest address to host
    // glfwSetCursorPosCallback etc — the host would call it as x86-64
    // (SIGSEGV). cpu.regs[0]=window handle (except ERROR_CB, global),
    // cpu.regs[1]=guest callback.
    if (entry.spec) {
        thunk::Policy pol = entry.spec->policy;
        if (pol == thunk::Policy::CURSOR_CB || pol == thunk::Policy::KEY_CB ||
            pol == thunk::Policy::MOUSE_CB || pol == thunk::Policy::FRAMEBUFFER_CB ||
            pol == thunk::Policy::WINDOW_SIZE_CB || pol == thunk::Policy::FOCUS_CB ||
            pol == thunk::Policy::ERROR_CB) {
            if (pol == thunk::Policy::ERROR_CB) {
                // Global callback, single arg: glfwSetErrorCallback(cb).
                impl_->glfw_error_cb_ = cpu.regs[0];
                if (dbg().thunk_trace) {
                    fprintf(stderr, "[thunk] glfwSetErrorCallback cb=0x%llx\n",
                            static_cast<unsigned long long>(cpu.regs[0]));
                }
                cpu.regs[0] = 0;
                return 0;
            }
            uint64_t window = cpu.regs[0];
            uint64_t guest_cb = cpu.regs[1];
            auto& cbs = impl_->glfw_cbs_[window];
            uint64_t* slot = nullptr;
            switch (pol) {
                case thunk::Policy::CURSOR_CB:       slot = &cbs.cursor; break;
                case thunk::Policy::KEY_CB:          slot = &cbs.key; break;
                case thunk::Policy::MOUSE_CB:        slot = &cbs.mouse; break;
                case thunk::Policy::FRAMEBUFFER_CB:  slot = &cbs.framebuffer; break;
                case thunk::Policy::WINDOW_SIZE_CB:  slot = &cbs.window_size; break;
                case thunk::Policy::FOCUS_CB:        slot = &cbs.focus; break;
                default: break;
            }
            if (slot) {
                *slot = guest_cb;
                if (guest_cb == 0) {
                    // Unregister: drop the window entry if all slots empty.
                    if (cbs.cursor == 0 && cbs.key == 0 && cbs.mouse == 0 &&
                        cbs.framebuffer == 0 && cbs.window_size == 0 &&
                        cbs.focus == 0) {
                        impl_->glfw_cbs_.erase(window);
                    }
                }
            }
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] %s: window=0x%llx cb=0x%llx\n",
                        entry.name.c_str(),
                        static_cast<unsigned long long>(window),
                        static_cast<unsigned long long>(guest_cb));
            }
            cpu.regs[0] = 0;
            return 0;
        }
    }

    // ── Double-only AAPCS64 path (glOrtho, glClearDepth, …) ─────────
    // Guest AArch64 passes GLdouble args in d0..d{n-1} (low 64 bits of
    // v0..). Host SysV AMD64 expects them in XMM0.. — the C++ cast below
    // places them there automatically.
    if (entry.flags & THUNK_DOUBLE) {
        double dv[8] = {0};
        for (uint8_t i = 0; i < entry.n_float && i < 8; i++) {
            std::memcpy(&dv[i], &cpu.v_lo[i], sizeof(double));
        }
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: %s (double×%u) d0=%g d1=%g d2=%g d3=%g\n",
                    entry.name.c_str(), entry.n_float,
                    dv[0], dv[1], dv[2], dv[3]);
        }
        switch (entry.n_float) {
        case 1: {
            using Fn = void (*)(double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0]);
            break;
        }
        case 2: {
            using Fn = void (*)(double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1]);
            break;
        }
        case 3: {
            using Fn = void (*)(double, double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2]);
            break;
        }
        case 4: {
            using Fn = void (*)(double, double, double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3]);
            break;
        }
        case 5: {
            using Fn = void (*)(double, double, double, double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3], dv[4]);
            break;
        }
        case 6: {
            using Fn = void (*)(double, double, double, double, double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3], dv[4], dv[5]);
            break;
        }
        default: {
            using Fn = void (*)(double, double, double, double, double, double,
                                double, double);
            reinterpret_cast<Fn>(entry.host_fn)(dv[0], dv[1], dv[2], dv[3],
                                                dv[4], dv[5], dv[6], dv[7]);
            break;
        }
        }
        cpu.regs[0] = 0;
        // No GLStateTracker handler consumes double-typed state setters
        // (glOrtho/glClearDepth are not tracked), so skip it here.
        return 0;
    }

    // ── Float-only AAPCS64 path (glClearColor, glVertex3f, …) ────────
    if (entry.n_float > 0 && !(entry.flags & THUNK_MIXED_FP)
        && !(entry.flags & THUNK_GET_PROC)) {
        float fv[8] = {0};
        for (uint8_t i = 0; i < entry.n_float && i < 8; i++) {
            std::memcpy(&fv[i], &cpu.v_lo[i], sizeof(float));
        }
        uint64_t local_args[12] = {0};
        for (int i = 0; i < 8; i++) local_args[i] = cpu.regs[i];
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: %s (fp×%u) f0=%g f1=%g f2=%g f3=%g\n",
                    entry.name.c_str(), entry.n_float,
                    fv[0], fv[1], fv[2], fv[3]);
        }
        switch (entry.n_float) {
        case 1: {
            using Fn = void (*)(float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0]);
            break;
        }
        case 2: {
            using Fn = void (*)(float, float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1]);
            break;
        }
        case 3: {
            using Fn = void (*)(float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1], fv[2]);
            break;
        }
        default: {
            using Fn = void (*)(float, float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(fv[0], fv[1], fv[2], fv[3]);
            break;
        }
        }
        cpu.regs[0] = 0;
        if (impl_->gl_state_tracker_ && entry.tracks_state) {
            impl_->gl_state_tracker_->track_state_change(entry.name, local_args, fv, entry.n_float);
        }
        return 0;
    }

    // ── Mixed int + float (glUniform*f, glTexParameterf, …) ─────────
    // n_stack holds the integer arity (x0..); n_float holds float arity
    // (v0..). Used only when THUNK_MIXED_FP is set.
    if (entry.flags & THUNK_MIXED_FP) {
        uint64_t iv[4] = {0};
        float fv[4] = {0};
        uint8_t ni = entry.n_stack;
        if (ni > 4) ni = 4;
        for (uint8_t i = 0; i < ni; i++) iv[i] = cpu.regs[i];
        for (uint8_t i = 0; i < entry.n_float && i < 4; i++) {
            std::memcpy(&fv[i], &cpu.v_lo[i], sizeof(float));
        }
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: %s (mixed int×%u fp×%u) "
                    "i0=%lld f0=%g\n",
                    entry.name.c_str(), ni, entry.n_float,
                    static_cast<long long>(iv[0]), fv[0]);
        }
        if (ni == 1 && entry.n_float == 1) {
            using Fn = void (*)(int32_t, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0]);
        } else if (ni == 1 && entry.n_float == 2) {
            using Fn = void (*)(int32_t, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0], fv[1]);
        } else if (ni == 1 && entry.n_float == 3) {
            using Fn = void (*)(int32_t, float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0], fv[1], fv[2]);
        } else if (ni == 1 && entry.n_float >= 4) {
            using Fn = void (*)(int32_t, float, float, float, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<int32_t>(iv[0]), fv[0], fv[1], fv[2], fv[3]);
        } else if (ni == 2 && entry.n_float == 1) {
            using Fn = void (*)(uint32_t, uint32_t, float);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<uint32_t>(iv[0]),
                static_cast<uint32_t>(iv[1]), fv[0]);
        } else if (ni == 3 && entry.n_float == 4) {
            // glBitmap(width, height, xorig, yorig, xmove, ymove, bits):
            // two count ints + four floats + the bitmap pointer. Rows are
            // (w+7)/8 bytes strided to UNPACK_ALIGNMENT (assume 4).
            uint64_t bw = iv[0], bh = iv[1];
            uint64_t brow = (bw + 7) / 8;
            uint64_t bstride = (brow + 3) & ~(uint64_t)3;
            uint64_t bsz = bh ? (bh - 1) * bstride + brow : 0;
            const void* bits = nullptr;
            std::vector<uint8_t> bbounce;
            if (impl_->mem) {
                uint8_t* hp = impl_->mem->guest_to_host_ptr(iv[2]);
                if (hp) {
                    bits = hp;
                } else if (bsz > 0 && bsz < (1ull << 20)) {
                    try {
                        bbounce.resize(static_cast<size_t>(bsz));
                        impl_->mem->read(iv[2], bbounce.data(),
                                         static_cast<size_t>(bsz));
                        bits = bbounce.data();
                    } catch (...) { bits = nullptr; }
                }
            }
            using Fn = void (*)(uint32_t, uint32_t, float, float, float,
                                float, const void*);
            reinterpret_cast<Fn>(entry.host_fn)(
                static_cast<uint32_t>(iv[0]), static_cast<uint32_t>(iv[1]),
                fv[0], fv[1], fv[2], fv[3], bits);
        } else {
            // Unsupported mixed shape — no-op rather than corrupt.
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] mixed FP shape unsupported for %s\n",
                        entry.name.c_str());
            }
        }
        cpu.regs[0] = 0;
        if (impl_->gl_state_tracker_ && entry.tracks_state) {
            uint64_t mixed_args[12] = {0};
            mixed_args[0] = iv[0];
            impl_->gl_state_tracker_->track_state_change(entry.name, mixed_args, fv, entry.n_float);
        }
        return 0;
    }

    // ── Integer/pointer path with optional stack args ────────────────
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

    // ── SDL_JoystickGetGUID — 16-byte struct return by value ───────────
    // Host SysV x86-64 returns SDL_JoystickGUID (16 bytes, two INTEGER
    // fields) in RAX:RDX; the guest expects it in x0:x1. The row's ARGS is
    // 'i', so the pointer loop never translates the opaque SDL_Joystick*
    // handle — it round-trips as the host pointer SDL_JoystickOpen returned.
    // This arm MUST run before the pointer-args loop.
    if (entry.spec && entry.spec->policy == thunk::Policy::JOY_GUID) {
        struct Guid16 { uint64_t lo, hi; };
        Guid16 g{0, 0};
        if (entry.host_fn) {
            using Fn = Guid16 (*)(uint64_t);
            g = reinterpret_cast<Fn>(entry.host_fn)(cpu.regs[0]);
        }
        cpu.regs[0] = g.lo;
        cpu.regs[1] = g.hi;
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] SDL_JoystickGetGUID(0x%llx) -> %016llx%016llx\n",
                    static_cast<unsigned long long>(args[0]),
                    static_cast<unsigned long long>(g.hi),
                    static_cast<unsigned long long>(g.lo));
        }
        return 0;
    }

    // ── glMapBuffer / glMapBufferRange / glUnmapBuffer / glFlushMappedBufferRange ──
    // 2026-08: the host glMapBuffer returns a HOST pointer the guest cannot
    // deref (address-space mismatch). Instead, bounce through a guest-window
    // allocation: mmap_alloc() hands out guest addresses inside the 4 GiB
    // direct window, so the guest JIT can read/write the bounce fast. On
    // map, seed the bounce from the host buffer when GL_MAP_READ_BIT is
    // set (contents undefined otherwise / under GL_MAP_INVALIDATE_*); on
    // unmap, copy the bounce back into the host buffer when GL_MAP_WRITE_BIT
    // is set, then free the bounce. GL_MAP_FLUSH_EXPLICIT_BIT ranges are
    // pushed early by the FLUSH_BUFFER arm. Transient map/write/unmap per
    // frame works fully; persistent-coherent-without-explicit-flush stays
    // unsupported (the writes would never land on the host).
    constexpr uint32_t kGLMapReadBit = 0x0001;
    constexpr uint32_t kGLMapWriteBit = 0x0002;
    constexpr uint32_t kGLMapInvalidateRangeBit = 0x0004;
    constexpr uint32_t kGLMapInvalidateBufferBit = 0x0008;
    constexpr uint32_t kGLMapPersistentBit = 0x0040;
    constexpr uint32_t kGLMapCoherentBit = 0x0080;
    constexpr uint32_t kGLBufferSize = 0x8764;
    // PCWFC (Persistent-Coherent Writeback For Coherence): a mapping with
    // GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_MAP_WRITE_BIT is a
    // "serious engine" streaming buffer — the guest maps it ONCE, writes
    // it every frame through the returned pointer, and never unmaps it
    // (glUnmapBuffer on a persistent mapping keeps it valid per GL). The
    // bounce only wrote back on unmap/flush, so the host GPU never saw the
    // writes. sync_persistent_mappings_() pushes every live persistent
    // bounce back to the host buffer, and dispatch() calls it right before
    // any call that consumes buffer data (draws, copies, getSubData,
    // texbuffer) — the practical "coherence" guarantee (host sees writes at
    // the moment the GPU would read them).
    auto sync_persistent_mappings_ = [&]() {
        if (impl_->gl_buffer_mappings_.empty() || !impl_->gl_buffer_subdata_fn_)
            return;
        std::vector<GraphicThunkImpl::BufferMapping> snaps;
        {
            std::lock_guard<std::mutex> g(impl_->mu);
            for (const auto& kv : impl_->gl_buffer_mappings_) {
                const auto& m = kv.second;
                if ((m.access & (kGLMapPersistentBit | kGLMapCoherentBit |
                                 kGLMapWriteBit)) ==
                    (kGLMapPersistentBit | kGLMapCoherentBit | kGLMapWriteBit)) {
                    snaps.push_back(m);
                }
            }
        }
        if (snaps.empty()) return;
        using SubFn = void (*)(uint32_t, uint64_t, uint64_t, const void*);
        for (const auto& m : snaps) {
            uint8_t* hp = impl_->mem->guest_to_host_ptr(m.bounce);
            if (hp) {
                reinterpret_cast<SubFn>(impl_->gl_buffer_subdata_fn_)(
                    m.target, m.offset, m.size, hp);
            }
        }
    };
    if (entry.spec) {
        thunk::Policy mpol = entry.spec->policy;
        if (mpol == thunk::Policy::MAP_BUFFER) {
            bool is_range = (entry.name == "glMapBufferRange");
            uint32_t target = static_cast<uint32_t>(args[0]);
            uint32_t access = static_cast<uint32_t>(is_range ? args[3] : args[1]);
            if (!is_range) {
                // glMapBuffer takes an ACCESS ENUM, not a bitfield:
                // GL_READ_ONLY=0x88B8 / GL_WRITE_ONLY=0x88B9 /
                // GL_READ_WRITE=0x88BA. None of their bits overlap the
                // GL_MAP_*_BIT masks (e.g. 0x88B9 & GL_MAP_WRITE_BIT==0),
                // so without this normalization the writeback-on-unmap
                // check below silently dropped every glMapBuffer write.
                if (access == 0x88B8)      access = kGLMapReadBit;
                else if (access == 0x88B9) access = kGLMapWriteBit;
                else if (access == 0x88BA) access = kGLMapReadBit | kGLMapWriteBit;
            }
            uint64_t offset = is_range ? args[1] : 0;
            uint64_t length = is_range ? args[2] : 0;
            cpu.regs[0] = 0;
            if (!impl_->mem || !impl_->gl_state_tracker_ ||
                !impl_->gl_get_buffer_parameteriv_fn_) {
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] %s: unavailable (mem/tracker/host fns)\n",
                            entry.name.c_str());
                return 0;
            }
            uint32_t buffer = impl_->gl_state_tracker_->buffer_binding(target);
            if (buffer == 0) {
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] %s: no buffer bound to target 0x%x\n",
                            entry.name.c_str(), target);
                return 0;  // GL: map of an unbound buffer → NULL
            }
            {
                // Reserve the buffer key with a PLACEHOLDER (bounce=0)
                // under the lock — this is what actually makes the
                // double-map check atomic. The old code only re-checked
                // here and inserted much later (after mmap_alloc + host
                // GL queries), so two threads could both pass the check,
                // both allocate a bounce, and the second insert would
                // silently orphan the first. Holders see bounce=0 and
                // skip writeback/untrack until the real mapping lands.
                std::lock_guard<std::mutex> g(impl_->mu);
                if (impl_->gl_buffer_mappings_.count(buffer)) {
                    if (dbg().thunk_trace)
                        fprintf(stderr, "[thunk] %s: buffer %u already mapped\n",
                                entry.name.c_str(), buffer);
                    return 0;  // GL: mapping an already-mapped buffer → NULL
                }
                impl_->gl_buffer_mappings_[buffer] =
                    GraphicThunkImpl::BufferMapping{0, 0, 0, target, access};
            }
            if (!is_range) {
                // Whole-buffer map: query the host for the buffer size.
                int32_t size = 0;
                using ParamFn = void (*)(uint32_t, uint32_t, int32_t*);
                reinterpret_cast<ParamFn>(impl_->gl_get_buffer_parameteriv_fn_)(
                    target, kGLBufferSize, &size);
                length = (size > 0) ? static_cast<uint64_t>(size) : 0;
            }
            // Slow-path failure cleanup: drop OUR placeholder (and only
            // the placeholder — bounce==0 — so a real mapping installed
            // by a racing map after our unmap can't be nuked) or the
            // buffer stays "mapped" forever.
            auto drop_placeholder_ = [&]() {
                std::lock_guard<std::mutex> g(impl_->mu);
                auto it = impl_->gl_buffer_mappings_.find(buffer);
                if (it != impl_->gl_buffer_mappings_.end() && it->second.bounce == 0)
                    impl_->gl_buffer_mappings_.erase(it);
            };
            if (length == 0) {
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] %s: buffer %u has zero size\n",
                            entry.name.c_str(), buffer);
                drop_placeholder_();
                return 0;
            }
            uint64_t bounce = impl_->mem->mmap_alloc(length);
            if (bounce == 0) {
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] %s: mmap_alloc(%llu) failed\n",
                            entry.name.c_str(),
                            static_cast<unsigned long long>(length));
                drop_placeholder_();
                return 0;
            }
            // Seed the bounce from the host buffer if the guest may read it.
            if ((access & kGLMapReadBit) &&
                !(access & (kGLMapInvalidateRangeBit | kGLMapInvalidateBufferBit)) &&
                impl_->gl_get_buffer_subdata_fn_) {
                uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(bounce);
                if (host_ptr) {
                    using GetSubFn = void (*)(uint32_t, uint64_t, uint64_t, void*);
                    reinterpret_cast<GetSubFn>(impl_->gl_get_buffer_subdata_fn_)(
                        target, offset, length, host_ptr);
                }
            }
            {
                std::lock_guard<std::mutex> g(impl_->mu);
                impl_->gl_buffer_mappings_[buffer] =
                    GraphicThunkImpl::BufferMapping{bounce, length, offset,
                                                    target, access};
            }
            cpu.regs[0] = bounce;
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] %s: buffer=%u target=0x%x off=%llu "
                        "len=%llu acc=0x%x → bounce=0x%llx\n",
                        entry.name.c_str(), buffer, target,
                        static_cast<unsigned long long>(offset),
                        static_cast<unsigned long long>(length), access,
                        static_cast<unsigned long long>(bounce));
            }
            return 0;
        }
        if (mpol == thunk::Policy::UNMAP_BUFFER) {
            uint32_t target = static_cast<uint32_t>(args[0]);
            if (impl_->mem && impl_->gl_state_tracker_ && impl_->gl_buffer_subdata_fn_) {
                uint32_t buffer = impl_->gl_state_tracker_->buffer_binding(target);
                GraphicThunkImpl::BufferMapping m{};
                bool found = false;
                bool persistent = false;
                {
                    std::lock_guard<std::mutex> g(impl_->mu);
                    auto it = impl_->gl_buffer_mappings_.find(buffer);
                    if (it != impl_->gl_buffer_mappings_.end()) {
                        m = it->second;
                        // PCWFC: a persistent mapping stays valid after
                        // glUnmapBuffer (GL_ARB_buffer_storage semantics) —
                        // keep the bounce + mapping alive so per-frame
                        // writes through the returned pointer still land.
                        persistent = (m.access & kGLMapPersistentBit) != 0;
                        if (!persistent) impl_->gl_buffer_mappings_.erase(it);
                        found = true;
                    }
                }
                if (found && (m.access & kGLMapWriteBit)) {
                    uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(m.bounce);
                    if (host_ptr) {
                        using SubFn = void (*)(uint32_t, uint64_t, uint64_t, const void*);
                        reinterpret_cast<SubFn>(impl_->gl_buffer_subdata_fn_)(
                            m.target, m.offset, m.size, host_ptr);
                    }
                }
                if (found && !persistent) impl_->mem->untrack_allocation(m.bounce, m.size);
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] glUnmapBuffer: buffer=%u target=0x%x "
                            "%s%s\n", buffer, target, found ? "ok" : "(not mapped)",
                            persistent ? " (persistent, kept)" : "");
            }
            cpu.regs[0] = 1;  // GL_TRUE
            return 0;
        }
        if (mpol == thunk::Policy::FLUSH_BUFFER) {
            // glFlushMappedBufferRange(target, offset, length): push the
            // guest-written range to the host now (GL_MAP_FLUSH_EXPLICIT /
            // persistent+coherent best-effort). The bounce holds
            // buffer[offset..offset+size), so flush its [offset..+length).
            uint32_t target = static_cast<uint32_t>(args[0]);
            uint64_t offset = args[1];
            uint64_t length = args[2];
            if (impl_->mem && impl_->gl_state_tracker_ && impl_->gl_buffer_subdata_fn_) {
                uint32_t buffer = impl_->gl_state_tracker_->buffer_binding(target);
                GraphicThunkImpl::BufferMapping m{};
                bool found = false;
                {
                    std::lock_guard<std::mutex> g(impl_->mu);
                    auto it = impl_->gl_buffer_mappings_.find(buffer);
                    if (it != impl_->gl_buffer_mappings_.end()) {
                        m = it->second;
                        found = true;
                    }
                }
                if (found && (m.access & kGLMapWriteBit)) {
                    // The bounce holds buffer[offset..offset+size); map the
                    // flush request into it and clamp to the mapped range.
                    uint64_t rel = (offset >= m.offset) ? offset - m.offset : 0;
                    uint64_t n = length;
                    if (rel + n > m.size) n = (rel < m.size) ? m.size - rel : 0;
                    uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(m.bounce + rel);
                    if (host_ptr && n > 0) {
                        using SubFn = void (*)(uint32_t, uint64_t, uint64_t, const void*);
                        reinterpret_cast<SubFn>(impl_->gl_buffer_subdata_fn_)(
                            m.target, offset, n, host_ptr);
                    }
                }
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] glFlushMappedBufferRange: buffer=%u "
                            "target=0x%x off=%llu len=%llu %s\n",
                            buffer, target,
                            static_cast<unsigned long long>(offset),
                            static_cast<unsigned long long>(length),
                            found ? "ok" : "(not mapped)");
            }
            cpu.regs[0] = 0;
            return 0;
        }
    }

    // GetProcAddress(name): return guest trampoline for a registered
    // GL/EGL/SDL symbol, or 0 if unknown. Games resolve most GL via this.
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
            // ARB-suffixed aliases (glBindBufferARB, glActiveTextureARB,
            // …): games built against GL_ARB_* extensions fetch the
            // suffixed name while the table registers the core name.
            // Strip a trailing "ARB" and retry before giving up.
            char corebuf[256];
            const char* try_names[2] = { name, nullptr };
            size_t len = strlen(name);
            if (len > 3 && len < sizeof(corebuf) &&
                    !strcmp(name + len - 3, "ARB")) {
                memcpy(corebuf, name, len - 3);
                corebuf[len - 3] = 0;
                try_names[1] = corebuf;
            }
            for (int ti = 0; ti < 2 && !found; ti++) {
                if (!try_names[ti]) break;
                for (const auto& lib : impl_->libs_) {
                    for (const auto& e : lib.entries) {
                        if (e.name == try_names[ti]) {
                            found = e.guest_addr;
                            break;
                        }
                    }
                    if (found) break;
                }
            }
        }
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] GetProcAddress('%s') → 0x%llx\n",
                    name ? name : "(null)",
                    static_cast<unsigned long long>(found));
        }
        cpu.regs[0] = found;
        return 0;
    }

    // SizeKinds whose pointer arg is a pure HOST INPUT (upload/read):
    // the host never writes through that bounce, so marking it
    // need_wb would copy stale bounce bytes back over live guest
    // memory after the call. Download/query kinds (READPIXELS,
    // STIPPLE, X_*) still write back normally.
    auto input_only_size = [](thunk::SizeKind sk) {
        switch (sk) {
            case thunk::SizeKind::ARG1: case thunk::SizeKind::ARG2:
            case thunk::SizeKind::ARG6:
            case thunk::SizeKind::TEX2D: case thunk::SizeKind::TEXSUB:
            case thunk::SizeKind::TEX3D:
            case thunk::SizeKind::TEXIMAGE1D: case thunk::SizeKind::TEXSUBIMAGE1D:
            case thunk::SizeKind::TEXSUBIMAGE3D:
            case thunk::SizeKind::DRAWPIXELS: case thunk::SizeKind::BITMAP:
            case thunk::SizeKind::QUEUEAUDIO: case thunk::SizeKind::PITCH_H:
            case thunk::SizeKind::VK_REGIONS:
                return true;
            default:
                return false;
        }
    };

    auto translate_ptr = [&](uint64_t& a, int idx,
                             std::vector<uint8_t>* bounce,
                             uint64_t* guest_orig,
                             bool* need_wb) {
        if (a == 0 || !impl_->mem) return;
        uint8_t* host_ptr = impl_->mem->guest_to_host_ptr(a);
        if (host_ptr) {
            a = reinterpret_cast<uint64_t>(host_ptr);
            return;
        }
        // High-stack / sparse-page pointer: bounce through a host buffer.
        // Default 64 KiB covers modest textures/VBO uploads; the SIZE
        // column of the spec overrides it where the exact size is known.
        size_t kBounce = 65536;
        // Padded row extent the host driver actually accesses on the
        // bounce (rows strided to the tracked UNPACK/PACK alignment).
        // The ALLOCATION must cover it even though the vector's size()
        // stays at the packed logical size, so the writeback still
        // copies exactly the guest's logical bytes.
        uint64_t pad_extent = 0;
        auto pixel_extent_ = [&](bool upload, uint64_t h,
                                 uint64_t row_bytes) -> uint64_t {
            int align = 4;
            if (impl_->gl_state_tracker_) {
                align = upload
                    ? impl_->gl_state_tracker_->pixel_store_unpack_alignment()
                    : impl_->gl_state_tracker_->pixel_store_pack_alignment();
            }
            uint64_t stride = (row_bytes + align - 1) & ~(uint64_t)(align - 1);
            return h ? (h - 1) * stride + row_bytes : 0;
        };
        // Bytes per pixel for a GL format/type pair (channel count ×
        // component size). Unknown combos default to 1 byte/pixel.
        auto pixel_bps_ = [](uint64_t fmt, uint64_t type) -> uint64_t {
            uint64_t channels = 1;
            switch (fmt) {
                case 0x1907: case 0x80E0: channels = 3; break;  // GL_RGB / GL_BGR
                case 0x1908: case 0x80E1: channels = 4; break;  // GL_RGBA / GL_BGRA
                case 0x190A: channels = 2; break;               // GL_LUMINANCE_ALPHA
                default: break;                                  // 1 (GL_RED/GL_ALPHA/...)
            }
            uint64_t type_sz = 1;
            switch (type) {
                case 0x1403: case 0x1405: type_sz = 2; break;   // SHORT / FLOAT16
                case 0x1406: case 0x1404: case 0x140C: type_sz = 4; break; // FLOAT/INT/UINT
                default: break;                                  // UNSIGNED_BYTE etc.
            }
            return channels * type_sz;
        };
        const thunk::SizeKind sk = entry.spec ? entry.spec->size
                                              : thunk::SizeKind::NONE;
        switch (sk) {
        case thunk::SizeKind::ARG1:
            // glBufferData(target, size, data, usage): data is arg2.
            if (idx == 2) {
                uint64_t sz = args[1];
                if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        case thunk::SizeKind::ARG2:
            // glBufferSubData(target, offset, size, data): data is arg3.
            if (idx == 3) {
                uint64_t sz = args[2];
                if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        case thunk::SizeKind::ARG6:
            // glCompressedTexImage2D(target, level, internalformat, width,
            // height, border, imageSize, data): data is arg7, sized by
            // arg6 (imageSize).
            if (idx == 7) {
                uint64_t sz = args[6];
                if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        case thunk::SizeKind::TEX2D:
            // glTexImage2D(target, level, internalformat, width, height,
            // border, format, type, pixels): pixels is arg8. Width/height
            // are args 3/4.
            if (idx == 8) {
                uint64_t w = args[3], h = args[4];
                uint64_t fmt = args[6], type = args[7];
                uint64_t channels = 1;
                switch (fmt) {
                    case 0x1907: case 0x80E0: channels = 3; break;  // GL_RGB / GL_BGR
                    case 0x1908: case 0x80E1: channels = 4; break;  // GL_RGBA / GL_BGRA
                    case 0x190A: channels = 2; break;               // GL_LUMINANCE_ALPHA
                    default: break;                                  // 1 (GL_RED/GL_ALPHA/...)
                }
                uint64_t type_sz = 1;
                switch (type) {
                    case 0x1403: case 0x1405: type_sz = 2; break;   // SHORT / FLOAT16
                    case 0x1406: case 0x1404: case 0x140C: type_sz = 4; break; // FLOAT/INT/UINT
                    default: break;                                  // UNSIGNED_BYTE etc.
                }
                uint64_t sz = w * h * channels * type_sz;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h,
                                               w * channels * type_sz);
                }
            }
            break;
        case thunk::SizeKind::TEXSUB:
            // glTexSubImage2D(target, level, xoffset, yoffset, width,
            // height, format, type, pixels): pixels is arg8. Width/height
            // are args 4/5 (args 3 is yoffset, NOT width).
            if (idx == 8) {
                uint64_t w = args[4], h = args[5];
                uint64_t fmt = args[6], type = args[7];
                uint64_t channels = 1;
                switch (fmt) {
                    case 0x1907: case 0x80E0: channels = 3; break;  // GL_RGB / GL_BGR
                    case 0x1908: case 0x80E1: channels = 4; break;  // GL_RGBA / GL_BGRA
                    case 0x190A: channels = 2; break;               // GL_LUMINANCE_ALPHA
                    default: break;                                  // 1 (GL_RED/GL_ALPHA/...)
                }
                uint64_t type_sz = 1;
                switch (type) {
                    case 0x1403: case 0x1405: type_sz = 2; break;   // SHORT / FLOAT16
                    case 0x1406: case 0x1404: case 0x140C: type_sz = 4; break; // FLOAT/INT/UINT
                    default: break;                                  // UNSIGNED_BYTE etc.
                }
                uint64_t sz = w * h * channels * type_sz;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h,
                                               w * channels * type_sz);
                }
            }
            break;
        case thunk::SizeKind::TEX3D:
            // glTexImage3D(target, level, internalformat, width, height,
            // depth, border, format, type, pixels): pixels is arg9. Host
            // reads width*height*depth*channels*type_size — a font atlas
            // volume dwarfs the default 64 KiB bounce.
            if (idx == 9) {
                uint64_t w = args[3], h = args[4], d = args[5];
                uint64_t fmt = args[7], type = args[8];
                uint64_t channels = 1;
                switch (fmt) {
                    case 0x1907: case 0x80E0: channels = 3; break;  // GL_RGB / GL_BGR
                    case 0x1908: case 0x80E1: channels = 4; break;  // GL_RGBA / GL_BGRA
                    case 0x190A: channels = 2; break;               // GL_LUMINANCE_ALPHA
                    default: break;                                  // 1 (GL_RED/GL_ALPHA/...)
                }
                uint64_t type_sz = 1;
                switch (type) {
                    case 0x1403: case 0x1405: type_sz = 2; break;   // SHORT / FLOAT16
                    case 0x1406: case 0x1404: case 0x140C: type_sz = 4; break; // FLOAT/INT/UINT
                    default: break;                                  // UNSIGNED_BYTE etc.
                }
                uint64_t sz = w * h * d * channels * type_sz;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h * d,
                                               w * channels * type_sz);
                }
            }
            break;
        case thunk::SizeKind::DRAWPIXELS:
            // glDrawPixels(width, height, format, type, pixels): pixels is
            // arg4 (upload — host reads it).
            if (idx == 4) {
                uint64_t w = args[0], h = args[1];
                uint64_t sz = w * h * pixel_bps_(args[2], args[3]);
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h,
                                               w * pixel_bps_(args[2], args[3]));
                }
            }
            break;
        case thunk::SizeKind::BITMAP:
            // glBitmap(w, h, xorig, yorig, xmove, ymove, bits): bits is
            // arg6; rows are (w+7)/8 bytes.
            if (idx == 6) {
                uint64_t w = args[0], h = args[1];
                uint64_t row = (w + 7) / 8;
                uint64_t sz = h * row;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h, row);
                }
            }
            break;
        case thunk::SizeKind::TEXIMAGE1D:
            // glTexImage1D(target, level, internalformat, width, border,
            // format, type, data): data is arg7, one row of width pixels.
            if (idx == 7) {
                uint64_t w = args[3];
                uint64_t sz = w * pixel_bps_(args[5], args[6]);
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, 1,
                                               w * pixel_bps_(args[5], args[6]));
                }
            }
            break;
        case thunk::SizeKind::TEXSUBIMAGE1D:
            // glTexSubImage1D(target, level, xoffset, width, format, type,
            // data): data is arg6, one row of width pixels.
            if (idx == 6) {
                uint64_t w = args[3];
                uint64_t sz = w * pixel_bps_(args[4], args[5]);
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, 1,
                                               w * pixel_bps_(args[4], args[5]));
                }
            }
            break;
        case thunk::SizeKind::TEXSUBIMAGE3D:
            // glTexSubImage3D(target, level, xo, yo, zo, w, h, d, format,
            // type, data): data is arg10.
            if (idx == 10) {
                uint64_t w = args[5], h = args[6], d = args[7];
                uint64_t bps = pixel_bps_(args[8], args[9]);
                uint64_t sz = w * h * d * bps;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(true, h * d, w * bps);
                }
            }
            break;
        case thunk::SizeKind::STIPPLE:
            // glPolygonStipple/glGetPolygonStipple(mask): fixed 32×32-bit
            // image = 128 bytes (rows of 4 bytes match default alignment).
            kBounce = 128;
            break;
        case thunk::SizeKind::VK_REGIONS:
            // vkCmdBlitImage(cmd, src, srcLayout, dst, dstLayout,
            // regionCount, pRegions): pRegions is an array of
            // VkImageBlit2 (96 B each, no guest-pointer members — a
            // byte-exact bounce copy is a faithful deep copy).
            if (idx == 6) {
                uint64_t sz = args[5] * 96;
                if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        case thunk::SizeKind::PITCH_H:
            // SDL_UpdateTexture: pixels buffer = pitch (args[3]) * height.
            if (idx == 2) {
                auto it = impl_->sdl_tex_sizes_.find(args[0]);
                if (it != impl_->sdl_tex_sizes_.end()) {
                    uint64_t sz = args[3] * it->second.second;
                    if (sz > 0 && sz < (16ull << 20)) kBounce = static_cast<size_t>(sz);
                }
            }
            break;
        case thunk::SizeKind::READPIXELS:
            // glReadPixels(x, y, width, height, format, type, pixels):
            // host writes width*height*channels*type_size into the out-buffer.
            if (idx == 6) {
                uint64_t w = args[2], h = args[3];
                uint64_t fmt = args[4], type = args[5];
                uint64_t channels = 1;
                switch (fmt) {
                    case 0x1907: case 0x80E0: channels = 3; break;  // GL_RGB / GL_BGR
                    case 0x1908: case 0x80E1: channels = 4; break;  // GL_RGBA / GL_BGRA
                    case 0x190A: channels = 2; break;               // GL_LUMINANCE_ALPHA
                    default: break;                                  // 1 (GL_RED/GL_ALPHA/...)
                }
                uint64_t type_sz = 1;
                switch (type) {
                    case 0x1403: case 0x1405: type_sz = 2; break;   // SHORT / FLOAT16
                    case 0x1406: case 0x1404: case 0x140C: type_sz = 4; break; // FLOAT/INT/UINT
                    default: break;                                  // UNSIGNED_BYTE etc.
                }
                uint64_t sz = w * h * channels * type_sz;
                if (sz > 0 && sz < (64ull << 20)) {
                    kBounce = static_cast<size_t>(sz);
                    pad_extent = pixel_extent_(false, h,
                                               w * channels * type_sz);
                }
            }
            break;
        case thunk::SizeKind::QUEUEAUDIO:
            // SDL_QueueAudio(dev, data, len): host reads exactly len bytes.
            if (idx == 1) {
                uint64_t sz = args[2];
                if (sz > 0 && sz < (64ull << 20)) kBounce = static_cast<size_t>(sz);
            }
            break;
        default:
            break;
        }
        if (entry.name == "glTexImage2D" && dbg().thunk_trace) {
            static int tex_trace_n = 0;
            if (tex_trace_n++ < 80) {
                fprintf(stderr,
                        "[thunk] glTexImage2D full: target=0x%llx level=%llu "
                        "internal=0x%llx %llux%llu border=%llu format=0x%llx "
                        "type=0x%llx bytes=%zu\n",
                        (unsigned long long)args[0],
                        (unsigned long long)args[1],
                        (unsigned long long)args[2],
                        (unsigned long long)args[3],
                        (unsigned long long)args[4],
                        (unsigned long long)args[5],
                        (unsigned long long)args[6],
                        (unsigned long long)args[7], kBounce);
            }
        }
        // Reserve capacity for the padded extent BEFORE resize so the
        // allocation covers every byte the host may stride to, while
        // size() stays packed for an exact writeback.
        if (pad_extent > static_cast<uint64_t>(kBounce) &&
            pad_extent < (64ull << 20))
            bounce->reserve(static_cast<size_t>(pad_extent));
        bounce->resize(kBounce);
        try {
            impl_->mem->read(a, bounce->data(), kBounce);
        } catch (...) {
            // Zero in place — assign() could reallocate and shrink the
            // reserved padded capacity below the host's access extent.
            std::fill(bounce->begin(), bounce->end(), 0);
        }
        *guest_orig = a;
        *need_wb = !input_only_size(sk);
        a = reinterpret_cast<uint64_t>(bounce->data());
        (void)idx;
    };

    std::vector<uint8_t> bounce_bufs[kMaxArgs];
    uint64_t bounce_guest[kMaxArgs] = {0};
    bool bounce_wb[kMaxArgs] = {false};

    // VA_PTR/EL_PTR symbols own their pointer args (the binding-aware block
    // below translates them ONLY when no VBO is bound — with a buffer bound
    // the arg is a byte offset into that buffer and must pass through raw).
    // The generic loop must NOT touch them: translating offset 4/12 as guest
    // addresses turned them into host alias pointers that host GL then
    // reinterpreted as ~2^47 byte offsets (neverball's menu GUI drew
    // nothing). Also prevents a double translation when binding==0.
    const bool va_ptr_owned =
        entry.spec && (entry.spec->policy == thunk::Policy::VA_PTR ||
                       entry.spec->policy == thunk::Policy::EL_PTR);
    if (entry.pointer_args && impl_->mem && !va_ptr_owned) {
        for (int i = 0; i < kMaxArgs; i++) {
            if (entry.pointer_args & (1u << i)) {
                translate_ptr(args[i], i, &bounce_bufs[i],
                              &bounce_guest[i], &bounce_wb[i]);
            }
        }
    }

    // ── SDL_JoystickGetGUIDString — guid BY VALUE, out-buffer bounce ──
    // Guest AAPCS64: x0/x1 = 16-byte SDL_JoystickGUID (by value), x2 =
    // char* pszGUID (OUT), x3 = cbGUID. The pointer-args loop above already
    // translated args[2]: either a bounce (write back below) or a direct
    // host alias (host wrote into guest memory in place, nothing to copy).
    // Must run AFTER the pointer-args loop so args[2] is host-addressable.
    if (entry.spec && entry.spec->policy == thunk::Policy::JOY_GUID_STR) {
        if (entry.host_fn && args[2] != 0) {
            uint64_t lo = args[0], hi = args[1];
            int size = static_cast<int>(args[3]);
            if (size < 0) size = 0;
            using Fn = void (*)(uint64_t, uint64_t, char*, int);
            reinterpret_cast<Fn>(entry.host_fn)(
                lo, hi, reinterpret_cast<char*>(args[2]), size);
            if (impl_->mem && bounce_guest[2] && bounce_wb[2]) {
                // Clamp the writeback to the guest's buffer capacity
                // (cbGUID) so we never overrun pszGUID[cbGUID].
                size_t wb = static_cast<size_t>(size);
                if (wb > bounce_bufs[2].size()) wb = bounce_bufs[2].size();
                impl_->mem->write(bounce_guest[2], bounce_bufs[2].data(), wb);
            }
        }
        cpu.regs[0] = 0;
        return 0;
    }

    // ── glfwCreateWindow HiDPI compensation ───────────────────────────
    // 1.5.4-alpha: the game requests a LOGICAL window size (1280x720),
    // but on HiDPI sessions (KDE Scale=2, Wayland/X11) host GLFW makes
    // the PHYSICAL framebuffer scale * the request (2560x1440), and
    // window.c then adopts the framebuffer size as window.size — the
    // game runs at double resolution and the HUD/matrices are laid out
    // for a fullscreen-scale window. Resize the host window by
    // 1/content_scale so glfwGetFramebufferSize returns exactly the
    // requested size. args[2] (title) was translated to a host pointer
    // by the pointer-args loop above.
    if (entry.spec && entry.spec->policy == thunk::Policy::GLFW_CREATE) {
        if (!impl_->glfw_get_window_content_scale_fn_ ||
            !impl_->glfw_set_window_size_fn_ || !entry.host_fn) {
            if (dbg().thunk_trace)
                fprintf(stderr, "[thunk] glfwCreateWindow: host fns unavailable\n");
            cpu.regs[0] = 0;
            return 0;
        }
        using CreateFn = uint64_t (*)(int, int, const char*, uint64_t, uint64_t);
        uint64_t win = reinterpret_cast<CreateFn>(entry.host_fn)(
            static_cast<int>(args[0]), static_cast<int>(args[1]),
            reinterpret_cast<const char*>(args[2]), args[3], args[4]);
        cpu.regs[0] = win;
        if (win) {
            float xs = 1.0f, ys = 1.0f;
            using ScaleFn = void (*)(uint64_t, float*, float*);
            reinterpret_cast<ScaleFn>(impl_->glfw_get_window_content_scale_fn_)(
                win, &xs, &ys);
            if (xs > 1.01f || ys > 1.01f) {
                int nw = static_cast<int>(static_cast<int64_t>(args[0]) / xs + 0.5f);
                int nh = static_cast<int>(static_cast<int64_t>(args[1]) / ys + 0.5f);
                using SizeFn = void (*)(uint64_t, int, int);
                reinterpret_cast<SizeFn>(impl_->glfw_set_window_size_fn_)(win, nw, nh);
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] glfwCreateWindow %llux%llu -> resize "
                            "%dx%d (scale %.2f)\n",
                            static_cast<unsigned long long>(args[0]),
                            static_cast<unsigned long long>(args[1]), nw, nh, xs);
            } else if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] glfwCreateWindow %llux%llu (scale %.2f)\n",
                        static_cast<unsigned long long>(args[0]),
                        static_cast<unsigned long long>(args[1]), xs);
            }
        }
        return 0;
    }

    // VA_PTR/EL_PTR: these take a *byte offset* into the bound buffer (must
    // NOT be translated) when a buffer is bound, but a real guest pointer
    // for client-side vertex / index arrays (must be translated).
    // GLStateTracker knows the binding; the policy comes from the spec.
    if (impl_->gl_state_tracker_ && entry.spec) {
        if (entry.spec->policy == thunk::Policy::VA_PTR) {
            // Pointer-arg position differs per function:
            //   glVertexAttribPointer*(index,...,stride,ptr) -> arg5
            //   glVertexPointer(size,type,stride,ptr)        -> arg3
            //   glColorPointer(size,size,type,stride,ptr)    -> arg4
            //   glTexCoordPointer(size,type,stride,ptr)      -> arg1
            //   glNormalPointer(type,stride,ptr)             -> arg2
            int pi = 5;
            const char* nm = entry.name.c_str();
            if (!strcmp(nm, "glVertexPointer") || !strcmp(nm, "glColorPointer"))
                pi = 3;                               // (size,type,stride,ptr)
            else if (!strcmp(nm, "glTexCoordPointer")) pi = 3;  // (size,type,stride,ptr)
            else if (!strcmp(nm, "glNormalPointer"))   pi = 2;  // (type,stride,ptr)
            static const bool vaptr_dbg = getenv("BIFROST_GUI_DBG");
            if (vaptr_dbg && (!strcmp(nm, "glVertexPointer") ||
                              !strcmp(nm, "glTexCoordPointer")))
                fprintf(stderr, "[vaptr] %s binding=%u arg=%llx -> %s\n",
                        nm, impl_->gl_state_tracker_->array_buffer_binding(),
                        (unsigned long long)args[pi],
                        impl_->gl_state_tracker_->array_buffer_binding() ?
                            "OFFSET" : "translate");
            if (impl_->gl_state_tracker_->array_buffer_binding() == 0 &&
                args[pi] != 0) {
                translate_ptr(args[pi], pi, &bounce_bufs[pi],
                              &bounce_guest[pi], &bounce_wb[pi]);
            }
        } else if (entry.spec->policy == thunk::Policy::EL_PTR) {
            // Index-array arg position differs per function:
            //   glDrawElements*/glDrawElementsInstanced -> arg3
            //   glDrawRangeElements(mode,start,end,count,type,indices) -> arg5
            //   glDrawRangeElementsBaseVertex(...,type,indices,base) -> arg5
            int pi = 3;
            if (entry.name == "glDrawRangeElements" ||
                entry.name == "glDrawRangeElementsBaseVertex") pi = 5;
            if (impl_->gl_state_tracker_->element_array_buffer_binding() == 0 &&
                args[pi] != 0) {
                translate_ptr(args[pi], pi, &bounce_bufs[pi],
                              &bounce_guest[pi], &bounce_wb[pi]);
            }
        }
    }

    // glShaderSource(shader, count, const char* const* strings, const int* len)
    if (entry.flags & THUNK_SHADER_SOURCE) {
        int count = static_cast<int>(args[1]);
        if (count < 0) count = 0;
        if (count > 64) count = 64;
        const char* host_strs[64];
        std::vector<std::vector<uint8_t>> bounces;
        bounces.reserve(static_cast<size_t>(count));
        // args[2] already translated to host pointer to guest pointer array
        const uint64_t* guest_arr = reinterpret_cast<const uint64_t*>(args[2]);
        // args[3] (guest GLint* lengths, may be NULL) is already translated.
        const int* lens = args[3] ? reinterpret_cast<const int*>(args[3]) : nullptr;
        for (int i = 0; i < count; i++) {
            uint64_t gp = guest_arr ? guest_arr[i] : 0;
            if (gp == 0) { host_strs[i] = ""; continue; }
            uint8_t* hp = impl_->mem ? impl_->mem->guest_to_host_ptr(gp) : nullptr;
            if (hp) { host_strs[i] = reinterpret_cast<const char*>(hp); continue; }
            // Heap/stack string above the 4 GiB direct window: bounce it.
            size_t n = (lens && lens[i] > 0)
                           ? static_cast<size_t>(lens[i])
                           : (lens ? 0 : 65536);
            if (n == 0 && lens && lens[i] <= 0) n = 0;
            if (n == 0) {
                // Null-terminated string; scan for the terminator.
                constexpr size_t kMaxStr = 1 << 20;
                for (; n < kMaxStr; n++) {
                    uint8_t c = 0;
                    try { impl_->mem->read(gp + n, &c, 1); } catch (...) { break; }
                    if (c == 0) break;
                }
            }
            if (n > (16ull << 20)) n = 16ull << 20;
            bounces.emplace_back(n + 1, 0);
            try {
                impl_->mem->read(gp, bounces.back().data(), n);
            } catch (...) {
                bounces.back().assign(n + 1, 0);
            }
            host_strs[i] = reinterpret_cast<const char*>(bounces.back().data());
        }
        using Fn = void (*)(uint64_t, int, const char* const*, const int*);
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] shaderSource: shader=0x%llx count=%d "
                    "str0='%.60s' len0=%d\n",
                    static_cast<unsigned long long>(args[0]), count,
                    (host_strs[0] ? host_strs[0] : ""),
                    (lens ? lens[0] : -1));
        }
        reinterpret_cast<Fn>(entry.host_fn)(
            args[0], count, host_strs,
            args[3] ? reinterpret_cast<const int*>(args[3]) : nullptr);
        cpu.regs[0] = 0;
        return 0;
    }

    // glTransformFeedbackVaryings(program, count, const GLchar *const *varyings,
    // bufferMode): the varyings array is a nested array of C-string pointers
    // (exactly like glShaderSource's strings), so it needs the same per-string
    // translation/bounce. args[3] is bufferMode (a GLenum, NOT a length array),
    // so unlike shaderSource there is no lens vector — strings are read until
    // their NUL terminator.
    if (entry.flags & THUNK_TF_VARYINGS) {
        int count = static_cast<int>(args[1]);
        if (count < 0) count = 0;
        if (count > 64) count = 64;
        const char* host_strs[64];
        std::vector<std::vector<uint8_t>> bounces;
        bounces.reserve(static_cast<size_t>(count));
        // args[2] already translated to a host pointer to the guest pointer array.
        const uint64_t* guest_arr = reinterpret_cast<const uint64_t*>(args[2]);
        for (int i = 0; i < count; i++) {
            uint64_t gp = guest_arr ? guest_arr[i] : 0;
            if (gp == 0) { host_strs[i] = ""; continue; }
            uint8_t* hp = impl_->mem ? impl_->mem->guest_to_host_ptr(gp) : nullptr;
            if (hp) { host_strs[i] = reinterpret_cast<const char*>(hp); continue; }
            size_t n = 0;
            constexpr size_t kMaxStr = 1 << 20;
            for (; n < kMaxStr; n++) {
                uint8_t c = 0;
                try { impl_->mem->read(gp + n, &c, 1); } catch (...) { break; }
                if (c == 0) break;
            }
            if (n > (16ull << 20)) n = 16ull << 20;
            bounces.emplace_back(n + 1, 0);
            try {
                impl_->mem->read(gp, bounces.back().data(), n);
            } catch (...) {
                bounces.back().assign(n + 1, 0);
            }
            host_strs[i] = reinterpret_cast<const char*>(bounces.back().data());
        }
        using Fn = void (*)(uint64_t, int, const char* const*, uint32_t);
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] transformFeedbackVaryings: program=0x%llx "
                    "count=%d str0='%.60s' mode=0x%llx\n",
                    static_cast<unsigned long long>(args[0]), count,
                    (host_strs[0] ? host_strs[0] : ""),
                    static_cast<unsigned long long>(args[3]));
        }
        reinterpret_cast<Fn>(entry.host_fn)(
            args[0], count, host_strs, static_cast<uint32_t>(args[3]));
        cpu.regs[0] = 0;
        return 0;
    }

    if (dbg().thunk_trace) {
        fprintf(stderr, "[thunk] dispatch[T%lx]: %s (host_fn=%p) "
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

    // 1.5.4-alpha: GL state query interception — after pointer translation
    // so that pointer args in queries (glGetIntegerv, etc.) point to host
    // memory. Only intercept functions tagged QUERY in the spec.
    bool is_gl_query = (entry.spec &&
                        entry.spec->policy == thunk::Policy::QUERY);
    bool handled_by_tracker = false;
    if (is_gl_query && impl_->gl_state_tracker_) {
        // Pass the post-translation `args` so the tracker writes query
        // results into the host buffer (direct-window alias or bounce)
        // that dispatch() will write back to guest memory.
        handled_by_tracker = impl_->gl_state_tracker_->try_handle_query(entry.name, args, cpu);
    }
    // ── Writeback with LIVE-RANGE CLAMP (heap-shredder fix) ───────────
    // Each bounced pointer arg is written back only up to the end of the
    // enclosing tracked guest allocation, and never at all when no live
    // allocation encloses it (untracked space must not be written).
    // Bounce targets are almost always ABOVE the 4 GiB direct window —
    // that is exactly when guests whose allocator outgrows the window
    // (vkQuake/mimalloc arenas) have live data there, and the old
    // unconditional full-bounce writeback sprayed up to 64 KiB of stale
    // bytes over adjacent live allocations per thunk call.
    auto safe_writeback_all = [&]() {
        if (!impl_->mem) return;
        static int wb_skip_diag = 4, wb_clamp_diag = 4;
        for (int i = 0; i < kMaxArgs; i++) {
            if (!bounce_wb[i] || !bounce_guest[i]) continue;
            const uint64_t g = bounce_guest[i];
            const size_t want = bounce_bufs[i].size();
            size_t wb = 0;
            bool in_live = false;
            for (const auto& kv : impl_->mem->allocations_snapshot()) {
                if (g >= kv.first && g - kv.first < kv.second) {
                    uint64_t room = kv.second - (g - kv.first);
                    wb = (want <= room) ? want : static_cast<size_t>(room);
                    in_live = true;
                    break;
                }
            }
            if (!in_live) {
                if (wb_skip_diag > 0) {
                    --wb_skip_diag;
                    fprintf(stderr,
                            "[thunk] wb-skip %s arg%d @0x%llx: target "
                            "UNTRACKED, %zu B writeback dropped\n",
                            entry.name.c_str(), i,
                            (unsigned long long)g, want);
                }
                continue;
            }
            if (wb != want && wb_clamp_diag > 0) {
                --wb_clamp_diag;
                fprintf(stderr,
                        "[thunk] wb-clamp %s arg%d @0x%llx: %zu → %zu B "
                        "(live allocation end)\n",
                        entry.name.c_str(), i, (unsigned long long)g,
                        want, wb);
            }
            impl_->mem->write(g, bounce_bufs[i].data(), wb);
        }
    };

    if (handled_by_tracker) {
        // Skip host call, but still write back any bounced pointer args
        // so the guest sees the query result.
        safe_writeback_all();
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] dispatch: %s handled by GL state tracker\n",
                    entry.name.c_str());
        }
        return 0;
    }

    uint64_t ret = 0;
    // PCWFC: before any call that consumes buffer data (draw/copy/get/
    // texbuffer), push every live persistent+coherent bounce back to the
    // host so the GPU reads the guest's per-frame writes. Coherent mapping
    // semantics: the server sees client writes at the moment it reads them.
    // The consumer set is the spec's SYNC column (table-driven).
    if (entry.spec && entry.spec->sync_before &&
        !impl_->gl_buffer_mappings_.empty()) {
        sync_persistent_mappings_();
    }
    if (entry.name == "glTexImage2D" && dbg().thunk_trace) {
        static int full_tex_trace_n = 0;
        if (full_tex_trace_n++ < 80)
            fprintf(stderr, "[thunk] glTexImage2D call: %llx %llx %llx %llx %llx %llx %llx %llx %llx\n",
                    (unsigned long long)args[0], (unsigned long long)args[1],
                    (unsigned long long)args[2], (unsigned long long)args[3],
                    (unsigned long long)args[4], (unsigned long long)args[5],
                    (unsigned long long)args[6], (unsigned long long)args[7],
                    (unsigned long long)args[8]);
    }
    // GL string queries (GET_STRING policy) have a single GLenum argument
    // (glGetString) or GLenum+GLuint (glGetStringi) and a const GLubyte*
    // return.  Do not route them through the generic integer Fn8 call: the
    // result is a host pointer which is consumed by the string-cache path
    // below, and an untyped variadic-shaped call is especially fragile on
    // hosts where glGetString is dispatched through a GL ABI wrapper.
    // Calling it with its exact prototype also keeps the pname intact.
    bool is_get_string = entry.spec &&
                         entry.spec->policy == thunk::Policy::GET_STRING;
    if (is_get_string && entry.spec->args &&
        entry.spec->args[1] == '\0') {  // "i" — glGetString
        using GetStringFn = const unsigned char* (*)(unsigned int);
        const unsigned char* s = entry.host_fn
            ? reinterpret_cast<GetStringFn>(entry.host_fn)(
                  static_cast<unsigned int>(args[0]))
            : nullptr;
        ret = reinterpret_cast<uint64_t>(s);
        if (dbg().thunk_trace) {
            fprintf(stderr, "[thunk] %s pname=0x%x host='%.*s'\n",
                    entry.name.c_str(),
                    static_cast<unsigned int>(args[0]), 48,
                    s ? reinterpret_cast<const char*>(s) : "(null)");
        }
    } else if (is_get_string) {         // "ii" — glGetStringi
        using GetStringIFn = const unsigned char* (*)(unsigned int, unsigned int);
        const unsigned char* s = entry.host_fn
            ? reinterpret_cast<GetStringIFn>(entry.host_fn)(
                  static_cast<unsigned int>(args[0]),
                  static_cast<unsigned int>(args[1]))
            : nullptr;
        ret = reinterpret_cast<uint64_t>(s);
    } else if (entry.n_stack >= 4) {
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

    // Write bounced pointer args back into guest memory (clamped to the
    // enclosing live allocation — see safe_writeback_all above).
    safe_writeback_all();

    static int gui_dbg_n_ = -1;
    if (gui_dbg_n_ < 0) gui_dbg_n_ = getenv("BIFROST_GUI_DBG") ? 4000 : 0;
    if (gui_dbg_n_ > 0) {
        --gui_dbg_n_;
        fprintf(stderr, "[g] %s(%llx,%llx,%llx,%llx)%s\n",
                entry.name.c_str(),
                (unsigned long long)args[0], (unsigned long long)args[1],
                (unsigned long long)args[2], (unsigned long long)args[3],
                "");
    }
    if (entry.name == "SDL_GL_SwapWindow" && getenv("BIFROST_FB_DUMP")) {
        static int frame_n = 0;
        static bool warned = false;
        if (!warned) {
            warned = true;
            fprintf(stderr,
                    "[fbdump] BIFROST_FB_DUMP is set — frames WILL stall for "
                    "screenshots. Unset it for normal gameplay!\n");
        }
        // BIFROST_FB_DUMP="1" → default list; "300,600,900" → explicit.
        static const int* dump_frames = nullptr;
        static int dump_n = 0;
        if (!dump_frames) {
            static int list_buf[64];
            int n = 0;
            const char* spec = getenv("BIFROST_FB_DUMP");
            if (spec && strcmp(spec, "1") != 0) {
                for (const char* p = spec; *p && n < 64; ) {
                    char* end = nullptr;
                    long v = strtol(p, &end, 10);
                    if (end == p) break;
                    list_buf[n++] = static_cast<int>(v);
                    p = end;
                    while (*p == ',' || *p == ' ') p++;
                }
            }
            if (n == 0) {
                static const int def[] = {20, 100, 300, 600, 1000, 1500};
                memcpy(list_buf, def, sizeof(def));
                n = 6;
            }
            dump_frames = list_buf;
            dump_n = n;
        }
        bool do_dump = false;
        int dump_at = 0;
        for (int di = 0; di < dump_n; di++)
            if (frame_n == dump_frames[di]) {
                dump_at = dump_frames[di];
                do_dump = true;
                break;
            }
        if (do_dump) {
            using GSzFn = void (*)(void*, int*, int*);
            auto gsz = reinterpret_cast<GSzFn>(
                dlsym(RTLD_DEFAULT, "SDL_GL_GetDrawableSize"));
            auto rpx = reinterpret_cast<void (*)(int,int,int,int,
                          unsigned,unsigned,void*)>(
                dlsym(RTLD_DEFAULT, "glReadPixels"));
            int dw = 0, dh = 0;
            if (gsz) gsz(reinterpret_cast<void*>(args[0]), &dw, &dh);
            if (rpx && dw > 0 && dh > 0 && impl_->mem) {
                std::vector<uint8_t> px((size_t)dw * dh * 4);
                rpx(0, 0, dw, dh, 0x1908 /*GL_RGBA*/, 0x1401 /*UNSIGNED_BYTE*/,
                    px.data());
                char fbpath[64];
                snprintf(fbpath, sizeof(fbpath), "/tmp/opencode/fb%d.ppm",
                         dump_at);
                FILE* f = fopen(fbpath, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", dw, dh);
                    for (int i = 0; i < dw * dh; i++)
                        fwrite(&px[i*4], 1, 3, f);
                    fclose(f);
                    fprintf(stderr, "[fbdump] wrote %dx%d\n", dw, dh);
                }
            }
        }
        ++frame_n;
    }
    // 1.5.4-alpha: GLFW event pump — after the host poll/ wait returns,
    // deliver any registered guest cursor-position callbacks (the game
    // computes per-frame mouse deltas from them, which drives camera
    // look). The guest callback runs via the borrow-CPU runner.
    if (entry.spec && entry.spec->policy == thunk::Policy::GLFW_POLL) {
        impl_->deliver_glfw_callbacks_(cpu);
    }

    // Track SDL_Texture* dimensions for SDL_UpdateTexture bounce sizing
    // (TRACK_TEX records on create, UNTRACK_TEX drops on destroy).
    if (entry.spec) {
        if (entry.spec->policy == thunk::Policy::TRACK_TEX) {
            impl_->sdl_tex_sizes_[ret] = {static_cast<uint32_t>(args[3]),
                                          static_cast<uint32_t>(args[4])};
        } else if (entry.spec->policy == thunk::Policy::UNTRACK_TEX) {
            impl_->sdl_tex_sizes_.erase(args[0]);
        }
    }

    // Track host SDL semaphores the guest creates so wake_sdl_semaphores()
    // can release a worker thread blocked in SDL_SemWait when the guest
    // exits (exit_group only stops the calling CPU). SDL_CreateSemaphore
    // returns the host SDL_sem*; SDL_DestroySemaphore takes it as a0.
    if (entry.name == "SDL_CreateSemaphore") {
        if (ret) {
            std::lock_guard<std::mutex> g(impl_->mu);
            impl_->sdl_sems_.insert(ret);
        }
    } else if (entry.name == "SDL_DestroySemaphore") {
        std::lock_guard<std::mutex> g(impl_->mu);
        impl_->sdl_sems_.erase(args[0]);
    }

    // glDeleteBuffers(n, names) [DELETE_BUFFERS policy]: free any live
    // mappings (persistent bounces are kept alive across unmap, so this
    // is their only release point).
    if (entry.spec && entry.spec->policy == thunk::Policy::DELETE_BUFFERS &&
        impl_->mem) {
        uint32_t n = static_cast<uint32_t>(args[0]);
        if (n > 0 && n < 1024 && args[1]) {
            const uint32_t* names =
                reinterpret_cast<const uint32_t*>(args[1]);
            // Erase under the lock (so concurrent map/unmap can't see a
            // half-freed mapping), then untrack the bounces OUTSIDE it —
            // same pattern as the UNMAP_BUFFER arm (untrack_allocation
            // touches Memory's own locks and must not run under impl_->mu).
            std::vector<GraphicThunkImpl::BufferMapping> freed;
            {
                std::lock_guard<std::mutex> g(impl_->mu);
                for (uint32_t i = 0; i < n; i++) {
                    auto it = impl_->gl_buffer_mappings_.find(names[i]);
                    if (it != impl_->gl_buffer_mappings_.end()) {
                        freed.push_back(it->second);
                        impl_->gl_buffer_mappings_.erase(it);
                    }
                }
            }
            for (auto& m : freed)
                impl_->mem->untrack_allocation(m.bounce, m.size);
        }
    }

    if (entry.name == "SDL_PollEvent" && dbg().thunk_trace) {
        uint32_t ev = 0, evx = 0, evy = 0;
        if (args[0]) {
            uint8_t* hp = nullptr;
            if (bounce_guest[0]) {
                hp = bounce_bufs[0].data();
            } else {
                hp = reinterpret_cast<uint8_t*>(args[0]);
            }
            if (hp) {
                ev = *reinterpret_cast<uint32_t*>(hp);
                evx = *reinterpret_cast<uint32_t*>(hp + 16);
                evy = *reinterpret_cast<uint32_t*>(hp + 20);
            }
        }
        fprintf(stderr, "[thunk] SDL_PollEvent -> %llu type=0x%x x=%u y=%u\n",
                static_cast<unsigned long long>(ret), ev, evx, evy);
    }
    if (entry.spec && entry.spec->ret == thunk::RetKind::STRING) {
        ret = impl_->cache_host_string_(reinterpret_cast<const char*>(ret));
    }
    cpu.regs[0] = ret;
    if (impl_->gl_state_tracker_ && entry.tracks_state) {
        impl_->gl_state_tracker_->track_state_change(entry.name, args, nullptr, 0);
    }
    // Lightweight frame counter: print every 5 present/swap calls when
    // BIFROST_FRAME_TRACE=1 (avoids the heavy per-call dispatch trace).
    // Gate is cached in dbg() — getenv() on every dispatch was measurable.
    if (dbg().frame_trace && entry.spec &&
        entry.spec->policy == thunk::Policy::PRESENT) {
        static uint64_t frame_count = 0;
        static uint64_t t0 = 0;
        auto now_us = []() -> uint64_t {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
        if (frame_count == 0) t0 = now_us();
        if (++frame_count % 5 == 0) {
            uint64_t dt = now_us() - t0;
            fprintf(stderr, "[frame] %s: %llu frames in %.2fs (%.2f fps)\n",
                    entry.name.c_str(),
                    static_cast<unsigned long long>(frame_count),
                    dt / 1e6,
                    frame_count * 1e6 / static_cast<double>(dt));
        }
    }
    return 0;
}
// ── Diagnostics ────────────────────────────────────────────────────────
size_t GraphicThunk::symbol_count() const {
    if (!impl_) return 0;
    return impl_->id_to_idx_.size();
}
uint64_t GraphicThunk::trampoline_base() const {
    if (!impl_) return 0;
    return impl_->trampoline_base;
}
// ── set_glfw_cb_runner — guest GLFW-callback delivery hook ───────────
void GraphicThunk::set_glfw_cb_runner(GlfwCbRunner runner) {
    if (!impl_) return;
    impl_->glfw_cb_runner_ = std::move(runner);
    // Fresh registration: drop stale last-delivered state so a newly-
    // wired runner doesn't skip the first motion/size event.
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
// 1.5.4-alpha: the symbol inventory is TABLE-DRIVEN. tools/opgen/thunk_dp.txt
// (generated into include/opgen_thunk.hpp) lists every (lib family, symbol)
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
    // SDL_GetCurrentDisplayMode(displayID, SDL_DisplayMode* out) —
    // out-param flavor). RTLD_DEFAULT can pick those, and calling them
    // with the SDL2 ABI writes through stale registers. A direct handle
    // pins resolution to the real SDL2 ABI library.
    void* sdl_handle = nullptr;
    if (kHaveSDL) {
        sdl_handle = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
        if (!sdl_handle) sdl_handle = dlopen("libSDL2.so", RTLD_LAZY | RTLD_GLOBAL);
        if (!sdl_handle && dbg().thunk_trace)
            fprintf(stderr, "[thunk] sdl: dlopen failed: %s\n", dlerror());
    }
    // GLFW callback delivery needs the raw host state-query fns (the
    // GLFW_POLL dispatch path reads them to decide whether guest
    // callbacks should fire). These are resolved once at init.
    impl_->glfw_get_cursor_pos_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwGetCursorPos") : nullptr;
    impl_->glfw_get_key_fn_ =
        kHaveGL ? dlsym(RTLD_DEFAULT, "glfwGetKey") : nullptr;
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
    impl_->glfw_last_error_code_ = 0;
    impl_->glfw_last_error_desc_.clear();
    if (impl_->glfw_set_error_callback_fn_) {
        using SetErrCbFn = void (*)(void (*)(int, const char*));
        auto setcb = reinterpret_cast<SetErrCbFn>(impl_->glfw_set_error_callback_fn_);
        setcb([](int code, const char* desc) {
            if (!host_err_sink) return;
            host_err_sink->glfw_last_error_code_ = code;
            host_err_sink->glfw_last_error_desc_ =
                desc ? desc : std::string();
        });
    }

    for (const thunk::Spec& spec : thunk::specs) {
        // 1.5.4-alpha: the table now also carries the DisplayThunk
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
        if (n_double > 0) {
            // Double-only AAPCS64 ABI: args in d0..d{n-1}. n_float carries
            // the double count; THUNK_DOUBLE tells dispatch to read them as
            // 8-byte doubles (from cpu.v_lo) rather than 4-byte floats.
            flags |= THUNK_DOUBLE;
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
// Lives here (not in graphics.cpp) because it needs the full
// GraphicThunk type to construct via `new`. Returns the lazily-created
// GraphicThunk instance owned by FrostGraphics.
GraphicThunk* FrostGraphics::thunk() {
    if (!thunk_) {
        thunk_ = std::unique_ptr<GraphicThunk>(new GraphicThunk());
    }
    return thunk_.get();
}
// 1.5.4-alpha: audio_thunk() and display_thunk() — same lazy pattern.
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
} // namespace arm64emu
