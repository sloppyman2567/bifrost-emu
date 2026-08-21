// frost_graphics/android_surface.cpp — Android NativeActivity surface
// layer v1 (see android_surface.hpp for the design).
//
// The host native window is resolved once from the SDL window via
// SDL_GetWindowWMInfo (X11 backend — the only host EGL platform this
// emulator targets today; Wayland would go through wl_egl_window_create).
#include "frost/android_surface.hpp"

#include <cstdio>
#include <dlfcn.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

namespace frost {

// libwayland-egl entry points (resolved lazily on Wayland hosts).
struct WlEglFns {
    void* handle = nullptr;
    void* (*window_create)(void* surface, int width, int height) = nullptr;
    void (*window_destroy)(void* window) = nullptr;
    int (*window_resize)(void* window, int width, int height) = nullptr;
    bool ok = false;
};
static WlEglFns g_wl_egl;

static const WlEglFns& wl_egl_fns() {
    if (!g_wl_egl.handle && !g_wl_egl.ok) {
        g_wl_egl.handle = dlopen("libwayland-egl.so.1", RTLD_LAZY);
        if (!g_wl_egl.handle) g_wl_egl.handle = dlopen("libwayland-egl.so", RTLD_LAZY);
        if (g_wl_egl.handle) {
            g_wl_egl.window_create =
                reinterpret_cast<void* (*)(void*, int, int)>(
                    dlsym(g_wl_egl.handle, "wl_egl_window_create"));
            g_wl_egl.window_destroy =
                reinterpret_cast<void (*)(void*)>(
                    dlsym(g_wl_egl.handle, "wl_egl_window_destroy"));
            g_wl_egl.window_resize =
                reinterpret_cast<int (*)(void*, int, int)>(
                    dlsym(g_wl_egl.handle, "wl_egl_window_resize"));
            g_wl_egl.ok = g_wl_egl.window_create != nullptr;
        }
        if (!g_wl_egl.ok) {
            std::fprintf(stderr, "[android] libwayland-egl unavailable\n");
            g_wl_egl.handle = (void*)1;  // sentinel: tried and failed
        }
    }
    return g_wl_egl;
}

AndroidSurfaceManager& AndroidSurfaceManager::instance() {
    static AndroidSurfaceManager mgr;
    return mgr;
}

void AndroidSurfaceManager::set_host_sdl_window(void* sdl_window) {
    if (sdl_window_ == sdl_window) return;
    sdl_window_ = sdl_window;
    host_native_window_resolved_ = false;
    host_native_window_ = 0;
}

uint64_t AndroidSurfaceManager::from_surface() {
    if (!ready()) return 0;
    if (surface_handle_ == 0) {
        surface_handle_ = kHandleBase + 1;
        refs_ = 1;
    } else {
        ++refs_;
    }
    return surface_handle_;
}

void AndroidSurfaceManager::acquire(uint64_t handle) {
    if (is_shim(handle) && handle == surface_handle_) ++refs_;
}

void AndroidSurfaceManager::release(uint64_t handle) {
    if (is_shim(handle) && handle == surface_handle_ && refs_ > 0) --refs_;
}

int32_t AndroidSurfaceManager::set_buffers_geometry(int32_t w, int32_t h,
                                                    int32_t fmt) {
    if (w > 0) width_ = w;
    if (h > 0) height_ = h;
    if (fmt > 0) format_ = fmt;
    // Best-effort: keep the host window matching the requested geometry so
    // eglQuerySurface and the visible window agree.
    if (sdl_window_) {
        SDL_SetWindowSize(static_cast<SDL_Window*>(sdl_window_), width_,
                          height_);
    }
    // Wayland: resize the live wl_egl_window so the next frame renders at
    // the new size (X11 needs nothing — the X server tracks the window).
    if (wl_egl_window_ && wl_egl_fns().ok && wl_egl_fns().window_resize) {
        wl_egl_fns().window_resize(wl_egl_window_, width_, height_);
    }
    return 0;
}

uint64_t AndroidSurfaceManager::host_native_window() {
    if (!sdl_window_) return 0;
    if (host_native_window_resolved_) return host_native_window_;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(static_cast<SDL_Window*>(sdl_window_), &info)) {
        std::fprintf(stderr, "[android] SDL_GetWindowWMInfo failed: %s\n",
                     SDL_GetError());
        host_native_window_resolved_ = true;
        return 0;
    }
    if (info.subsystem == SDL_SYSWM_X11) {
        host_native_window_ =
            reinterpret_cast<uint64_t>(info.info.x11.window);
    } else if (info.subsystem == SDL_SYSWM_WAYLAND) {
        // Host EGL's Wayland platform consumes a wl_egl_window* (NOT the
        // raw wl_surface). Create one from the SDL window's wl_surface.
        // ALSO remember SDL's wl_display: eglGetDisplay(EGL_DEFAULT_DISPLAY)
        // must wrap THE SAME connection or Mesa rejects the surface
        // (wl_surfaces are connection-private).
        wl_display_ = info.info.wl.display;
        const WlEglFns& fns = wl_egl_fns();
        if (fns.ok && fns.window_create) {
            void* w = fns.window_create(info.info.wl.surface, width_,
                                        height_);
            wl_egl_window_ = w;
            host_native_window_ = reinterpret_cast<uint64_t>(w);
        } else {
            std::fprintf(stderr, "[android] cannot create wl_egl_window\n");
            host_native_window_ = 0;
        }
    } else {
        std::fprintf(stderr, "[android] unsupported WM subsystem %d\n",
                     static_cast<int>(info.subsystem));
        host_native_window_ = 0;
    }
    host_native_window_resolved_ = true;
    if (host_native_window_) {
        std::fprintf(stderr, "[android] host native window = 0x%llx\n",
                     static_cast<unsigned long long>(host_native_window_));
    }
    return host_native_window_;
}

}  // namespace frost
