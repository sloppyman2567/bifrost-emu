// frost_graphics/thunk.cpp — EXPERIMENTAL graphic API thunking.
//
// v1.4.5-alpha (Turn 36): NEW. This file implements the GraphicThunk
// class declared in frost/thunk.hpp. The thunk intercepts guest
// dlsym calls for graphic libraries (libGL.so, libEGL.so, libSDL2.so,
// libGLESv2.so) and returns host function pointers that marshal the
// call to the host's equivalent library.
//
// ── Why thunking? ────────────────────────────────────────────────────
// bifrost-emu runs AArch64 guests on x86-64 hosts. A guest that uses
// OpenGL/EGL/SDL2 normally links against the AArch64 versions of those
// libraries, which can't run on the x86-64 host. Without thunking, the
// guest would fail at dlopen time ("wrong ELF class").
//
// Thunking solves this by intercepting the guest's dlsym calls and
// returning pointers to HOST functions (compiled into the emulator
// process). When the guest calls one of these "thunked" functions, the
// host function:
//   1. Reads the guest's registers (x0-x7 = first 8 args, per AArch64
//      AAPCS) — done by the JIT's CALL_INTERP fallback path.
//   2. Translates any guest-pointer arguments (vertex arrays, shader
//      source strings, etc.) to host pointers via the emulator's Memory.
//   3. Calls the host's equivalent function (e.g. host's glClear).
//   4. Writes the return value back to the guest's x0.
//
// ── What's supported ─────────────────────────────────────────────────
// This is EXPERIMENTAL. Only a small subset of GL/EGL/SDL2 entry points
// are thunked — enough to run trivial programs (clear the screen, draw
// a triangle, swap buffers). Real-world GL programs will hit unthunked
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
// Without BIFROST_THUNK_GRAPHICS=1, the thunk returns nullptr for
// every lookup — the guest falls back to its own software rendering
// (or fails gracefully if it has no software path).
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
//
// See frost/thunk.hpp for the class definition and frost/graphics.hpp
// for the FrostGraphics::thunk() accessor.
#include "frost/graphics.hpp"
#include "frost/thunk.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unordered_map>

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
struct GraphicThunkImpl {
    bool enabled = false;
    std::unordered_map<std::string, void*> cache;

#if defined(BIFROST_THUNK_HAVE_SDL2)
    bool          sdl_init_done = false;
    SDL_Window*   sdl_window    = nullptr;
    SDL_GLContext sdl_gl_ctx    = nullptr;
#endif
#if defined(BIFROST_THUNK_HAVE_EGL)
    EGLDisplay    egl_display   = nullptr;
    EGLContext    egl_context   = nullptr;
#endif
};

// ── GraphicThunk method implementations ───────────────────────────────
GraphicThunk::GraphicThunk() {
    impl_ = std::make_unique<GraphicThunkImpl>();
    impl_->enabled = (getenv("BIFROST_THUNK_GRAPHICS") != nullptr);
    if (impl_->enabled) {
        fprintf(stderr, "[thunk] graphic API thunking enabled "
                "(EXPERIMENTAL, partial GL/EGL/SDL2 support)\n");
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

void* GraphicThunk::resolve(const std::string& lib, const std::string& sym) {
    if (!impl_ || !impl_->enabled) return nullptr;

    auto key = lib + ":" + sym;
    auto it = impl_->cache.find(key);
    if (it != impl_->cache.end()) return it->second;

    void* p = nullptr;
    if (lib.rfind("libGL", 0) == 0 || lib.rfind("libGLESv2", 0) == 0) {
        p = resolve_gl_(sym);
    } else if (lib.rfind("libEGL", 0) == 0) {
        p = resolve_egl_(sym);
    } else if (lib.rfind("libSDL2", 0) == 0) {
        p = resolve_sdl_(sym);
    }

    impl_->cache[key] = p;
    return p;
}

void* GraphicThunk::resolve_gl_(const std::string& sym) {
#if defined(BIFROST_THUNK_HAVE_GL)
    void* p = dlsym(RTLD_DEFAULT, sym.c_str());
    if (p && getenv("BIFROST_THUNK_TRACE")) {
        fprintf(stderr, "[thunk] GL %s -> %p\n", sym.c_str(), p);
    }
    return p;
#else
    (void)sym;
    return nullptr;
#endif
}

void* GraphicThunk::resolve_egl_(const std::string& sym) {
#if defined(BIFROST_THUNK_HAVE_EGL)
    void* p = dlsym(RTLD_DEFAULT, sym.c_str());
    if (p && getenv("BIFROST_THUNK_TRACE")) {
        fprintf(stderr, "[thunk] EGL %s -> %p\n", sym.c_str(), p);
    }
    return p;
#else
    (void)sym;
    return nullptr;
#endif
}

void* GraphicThunk::resolve_sdl_(const std::string& sym) {
#if defined(BIFROST_THUNK_HAVE_SDL2)
    void* p = dlsym(RTLD_DEFAULT, sym.c_str());
    if (p && getenv("BIFROST_THUNK_TRACE")) {
        fprintf(stderr, "[thunk] SDL2 %s -> %p\n", sym.c_str(), p);
    }
    return p;
#else
    (void)sym;
    return nullptr;
#endif
}

// ── FrostGraphics::thunk() — out-of-line definition ───────────────────
// Lives here (not in graphics.cpp) because it needs the full
// GraphicThunk type to construct via `new`.
GraphicThunk* FrostGraphics::thunk() {
    if (!thunk_) {
        thunk_ = std::unique_ptr<GraphicThunk>(new GraphicThunk());
    }
    return thunk_.get();
}

} // namespace arm64emu
