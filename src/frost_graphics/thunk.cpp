// frost_graphics/thunk.cpp — graphic API thunking (v1.4.5-alpha).
//
// 1.5.3-alpha: also hosts the FrostGraphics::audio_thunk() and
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
#include "frost/graphics.hpp"
#include "frost/thunk.hpp"
#include "frost/audio_thunk.hpp"    // 1.5.3-alpha: AudioThunk full def
#include "frost/display_thunk.hpp"  // 1.5.3-alpha: DisplayThunk full def
#include "frost/gl_state.hpp"       // 1.5.3-alpha: GLStateTracker
#include "opgen_thunk.hpp"          // 1.5.3-alpha: symbol signature table
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
struct GraphicThunkImpl {
    bool   enabled = false;
    Memory* mem    = nullptr;
    bool   initialized = false;
    // The trampoline page: a single 64 KiB region of guest memory.
    uint64_t trampoline_base = 0;
    static constexpr uint64_t TRAMPOLINE_PAGE_SIZE =
        GraphicThunk::TRAMPOLINE_SIZE * GraphicThunk::MAX_SYMBOLS;  // 64 KiB
    // Guest-visible scratch page for host→guest string returns
    // (glGetString, SDL_GetError, …). Ring-allocated.
    uint64_t string_cache_base = 0;
    static constexpr uint64_t STRING_CACHE_SIZE = 4096;
    uint32_t string_cache_off = 0;
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
    // 1.5.3-alpha: GL state tracker for consistent query results.
    std::unique_ptr<GLStateTracker> gl_state_tracker_;
    // 1.5.3-alpha: SDL_Texture* → {w, h} so SDL_UpdateTexture's guest
    // pixel buffer can be bounced at its full size (pitch * height)
    // instead of the 64 KiB default (which truncates larger frames).
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> sdl_tex_sizes_;
    // 1.5.3-alpha: generic GLFW callback delivery. Each glfwSetXxxCallback
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
    // 1.5.3-alpha: glfwCreateWindow HiDPI compensation. Resolved at init.
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
        for (auto& [window, cbs] : glfw_cbs_) {
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
                auto& last = it->second;
                // glfwGetKey only accepts keys >= GLFW_KEY_SPACE (32);
                // polling 0-31 makes host GLFW fire "Invalid key" errors.
                for (int k = GLFW_KEY_SPACE; k < GLFW_KEY_LAST; k++) {
                    int cur = getkey(w, k);
                    uint8_t pressed = (cur == GLFW_PRESS) ? 1 : 0;
                    if (last[k] != pressed) {
                        last[k] = pressed;
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
                it->second = last;
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
    // 1.5.3-alpha: thunking is now ENABLED BY DEFAULT.
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
    // 1.5.3-alpha: initialize GL state tracker.
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
    // 1.5.3-alpha: symbol_id includes ID_BASE_GRAPHICS to
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
                           n_stack, n_float, flags, spec});
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
            if (impl_->mem && impl_->string_cache_base) {
                if (impl_->string_cache_off + 8 > impl_->STRING_CACHE_SIZE)
                    impl_->string_cache_off = 0;
                uint64_t guest = impl_->string_cache_base + impl_->string_cache_off;
                impl_->mem->write(guest, kVer, sizeof(kVer));
                impl_->string_cache_off = static_cast<uint32_t>(
                    (impl_->string_cache_off + 8 + 7u) & ~7u);
                if (dbg().thunk_trace) {
                    fprintf(stderr, "[thunk] Mix_Linked_Version -> {2,0,1} @ 0x%llx\n",
                            static_cast<unsigned long long>(guest));
                }
                cpu.regs[0] = guest;
                return 0;
            }
            cpu.regs[0] = 0;
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

    // ── GLFW callback registration (CURSOR_CB/KEY_CB/MOUSE_CB/…) ─────
    // 1.5.3-alpha: the guest callback is AArch64 code host GLFW cannot
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
        if (impl_->gl_state_tracker_) {
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
        } else {
            // Unsupported mixed shape — no-op rather than corrupt.
            if (dbg().thunk_trace) {
                fprintf(stderr, "[thunk] mixed FP shape unsupported for %s\n",
                        entry.name.c_str());
            }
        }
        cpu.regs[0] = 0;
        if (impl_->gl_state_tracker_) {
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
    auto is_buffer_consumer_ = [](const std::string& n) {
        if (n.compare(0, 6, "glDraw") == 0) return true;  // glDraw* family
        if (n == "glCopyBufferSubData" || n == "glGetBufferSubData") return true;
        if (n == "glTexBuffer" || n == "glTexBufferRange") return true;
        return false;
    };
    if (entry.spec) {
        thunk::Policy mpol = entry.spec->policy;
        if (mpol == thunk::Policy::MAP_BUFFER) {
            bool is_range = (entry.name == "glMapBufferRange");
            uint32_t target = static_cast<uint32_t>(args[0]);
            uint32_t access = static_cast<uint32_t>(is_range ? args[3] : args[1]);
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
                // Re-check + insert under the same lock so two threads
                // mapping the same buffer can't double-allocate.
                std::lock_guard<std::mutex> g(impl_->mu);
                if (impl_->gl_buffer_mappings_.count(buffer)) {
                    if (dbg().thunk_trace)
                        fprintf(stderr, "[thunk] %s: buffer %u already mapped\n",
                                entry.name.c_str(), buffer);
                    return 0;  // GL: mapping an already-mapped buffer → NULL
                }
            }
            if (!is_range) {
                // Whole-buffer map: query the host for the buffer size.
                int32_t size = 0;
                using ParamFn = void (*)(uint32_t, uint32_t, int32_t*);
                reinterpret_cast<ParamFn>(impl_->gl_get_buffer_parameteriv_fn_)(
                    target, kGLBufferSize, &size);
                length = (size > 0) ? static_cast<uint64_t>(size) : 0;
            }
            if (length == 0) {
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] %s: buffer %u has zero size\n",
                            entry.name.c_str(), buffer);
                return 0;
            }
            uint64_t bounce = impl_->mem->mmap_alloc(length);
            if (bounce == 0) {
                if (dbg().thunk_trace)
                    fprintf(stderr, "[thunk] %s: mmap_alloc(%llu) failed\n",
                            entry.name.c_str(),
                            static_cast<unsigned long long>(length));
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
            for (const auto& lib : impl_->libs_) {
                for (const auto& e : lib.entries) {
                    if (e.name == name) {
                        found = e.guest_addr;
                        break;
                    }
                }
                if (found) break;
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
        case thunk::SizeKind::TEX2D:
        case thunk::SizeKind::TEXSUB:
            // pixels buffer (arg8): width * height * bytes-per-pixel from
            // format/type. Host reads exactly this much.
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
                if (sz > 0 && sz < (64ull << 20)) kBounce = static_cast<size_t>(sz);
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
                if (sz > 0 && sz < (64ull << 20)) kBounce = static_cast<size_t>(sz);
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
        bounce->resize(kBounce);
        try {
            impl_->mem->read(a, bounce->data(), kBounce);
        } catch (...) {
            bounce->assign(kBounce, 0);
        }
        *guest_orig = a;
        *need_wb = true;
        a = reinterpret_cast<uint64_t>(bounce->data());
        (void)idx;
    };

    std::vector<uint8_t> bounce_bufs[kMaxArgs];
    uint64_t bounce_guest[kMaxArgs] = {0};
    bool bounce_wb[kMaxArgs] = {false};

    if (entry.pointer_args && impl_->mem) {
        for (int i = 0; i < kMaxArgs; i++) {
            if (entry.pointer_args & (1u << i)) {
                translate_ptr(args[i], i, &bounce_bufs[i],
                              &bounce_guest[i], &bounce_wb[i]);
            }
        }
    }

    // ── glfwCreateWindow HiDPI compensation ───────────────────────────
    // 1.5.3-alpha: the game requests a LOGICAL window size (1280x720),
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
            if (impl_->gl_state_tracker_->array_buffer_binding() == 0 &&
                args[5] != 0) {
                translate_ptr(args[5], 5, &bounce_bufs[5],
                              &bounce_guest[5], &bounce_wb[5]);
            }
        } else if (entry.spec->policy == thunk::Policy::EL_PTR) {
            if (impl_->gl_state_tracker_->element_array_buffer_binding() == 0 &&
                args[3] != 0) {
                translate_ptr(args[3], 3, &bounce_bufs[3],
                              &bounce_guest[3], &bounce_wb[3]);
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
        fprintf(stderr, "[thunk] dispatch: %s (host_fn=%p) "
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

    // 1.5.3-alpha: GL state query interception — after pointer translation
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
    if (handled_by_tracker) {
        // Skip host call, but still write back any bounced pointer args
        // so the guest sees the query result.
        if (impl_->mem) {
            for (int i = 0; i < kMaxArgs; i++) {
                if (bounce_wb[i] && bounce_guest[i]) {
                    impl_->mem->write(bounce_guest[i], bounce_bufs[i].data(),
                                      bounce_bufs[i].size());
                }
            }
        }
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
    if (entry.spec && !impl_->gl_buffer_mappings_.empty() &&
        is_buffer_consumer_(entry.name)) {
        sync_persistent_mappings_();
    }
    if (entry.n_stack >= 1) {
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

    // Write bounced pointer args back into guest memory.
    if (impl_->mem) {
        for (int i = 0; i < kMaxArgs; i++) {
            if (bounce_wb[i] && bounce_guest[i]) {
                impl_->mem->write(bounce_guest[i], bounce_bufs[i].data(),
                                  bounce_bufs[i].size());
            }
        }
    }

    // 1.5.3-alpha: GLFW event pump — after the host poll/ wait returns,
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

    // glDeleteBuffers(n, names): free any live mappings (persistent bounces
    // are kept alive across unmap, so this is their only release point).
    if (entry.name == "glDeleteBuffers" && impl_->mem) {
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

    if (entry.spec && entry.spec->ret == thunk::RetKind::STRING) {
        ret = impl_->cache_host_string_(reinterpret_cast<const char*>(ret));
    }
    cpu.regs[0] = ret;
    if (impl_->gl_state_tracker_ &&
        impl_->gl_state_tracker_->tracks_state(entry.name)) {
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
// 1.5.3-alpha: the symbol inventory is TABLE-DRIVEN. tools/opgen/thunk_dp.txt
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
        // 1.5.3-alpha: the table now also carries the DisplayThunk
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
        if (fd.have) host_fn = dlsym(RTLD_DEFAULT, spec.name);
        if (spec.policy == thunk::Policy::GET_PROC && !host_fn) {
            host_fn = reinterpret_cast<void*>(1);
        }

        // Derive the legacy ABI-shape fields from the ARGS column so the
        // dispatcher's float/mixed/generic paths keep working unchanged.
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
// 1.5.3-alpha: audio_thunk() and display_thunk() — same lazy pattern.
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
