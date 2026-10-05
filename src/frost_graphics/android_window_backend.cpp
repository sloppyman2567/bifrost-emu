#include "frost/android_window_backend.hpp"
#include <cstdio>
#include <dlfcn.h>
#include <mutex>
#if defined(BIFROST_USE_SDL2)
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#endif
namespace frost {
namespace {
// Keep the module loaded while a backend has a Wayland EGL wrapper. Function
// local initialization also makes probing safe across multiple backends.
struct WlEglFns {
  void *handle = nullptr;
  void *(*window_create)(void *, int, int) = nullptr;
  void (*window_destroy)(void *) = nullptr;
  void (*window_resize)(void *, int, int, int, int) = nullptr;
};
const WlEglFns &wl_egl_fns() {
  static const WlEglFns functions = [] {
    WlEglFns f;
    f.handle = dlopen("libwayland-egl.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!f.handle)
      return f;
    f.window_create = reinterpret_cast<decltype(f.window_create)>(
        dlsym(f.handle, "wl_egl_window_create"));
    f.window_destroy = reinterpret_cast<decltype(f.window_destroy)>(
        dlsym(f.handle, "wl_egl_window_destroy"));
    f.window_resize = reinterpret_cast<decltype(f.window_resize)>(
        dlsym(f.handle, "wl_egl_window_resize"));
    if (!f.window_create || !f.window_destroy || !f.window_resize) {
      dlclose(f.handle);
      f = {};
    }
    return f;
  }();
  return functions;
}

class SdlAndroidWindowBackend final : public AndroidWindowBackend {
public:
  ~SdlAndroidWindowBackend() override { clear_binding(); }
  const char *name() const noexcept override { return "sdl-egl"; }
  void attach_host_window(void *window) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (sdl_window_ == window)
      return;
    clear_binding();
    sdl_window_ = window;
  }
  void resize(int32_t width, int32_t height, bool resize_host) override {
    std::lock_guard<std::mutex> lock(mu_);
    width_ = width;
    height_ = height;
#if defined(BIFROST_USE_SDL2)
    if (resize_host && sdl_window_)
      SDL_SetWindowSize(static_cast<SDL_Window *>(sdl_window_), width, height);
#else
    (void)resize_host;
#endif
    if (wl_egl_window_ && wl_egl_fns().window_resize)
      wl_egl_fns().window_resize(wl_egl_window_, width, height, 0, 0);
  }
  AndroidWindowBinding binding() override {
    std::lock_guard<std::mutex> lock(mu_);
#if defined(BIFROST_USE_SDL2)
    if (!sdl_window_)
      return {};
    if (host_native_window_resolved_)
      return {host_native_window_, wl_display_};
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(static_cast<SDL_Window *>(sdl_window_), &info)) {
      std::fprintf(stderr, "[android] SDL_GetWindowWMInfo failed: %s\n",
                   SDL_GetError());
      host_native_window_resolved_ = true;
      return {};
    }
    if (info.subsystem == SDL_SYSWM_X11) {
      host_native_window_ = reinterpret_cast<uint64_t>(info.info.x11.window);
    } else if (info.subsystem == SDL_SYSWM_WAYLAND) {
      // Host EGL's Wayland platform consumes a wl_egl_window* (NOT the
      // raw wl_surface). Create one from the SDL window's wl_surface.
      // ALSO remember SDL's wl_display: eglGetDisplay(EGL_DEFAULT_DISPLAY)
      // must wrap THE SAME connection or Mesa rejects the surface
      // (wl_surfaces are connection-private).
      wl_display_ = info.info.wl.display;
      const WlEglFns &fns = wl_egl_fns();
      if (fns.window_create) {
        void *w = fns.window_create(info.info.wl.surface, width_, height_);
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
    return {host_native_window_, wl_display_};
#else
    return {};
#endif
  }

private:
  void clear_binding() {
    if (wl_egl_window_ && wl_egl_fns().window_destroy)
      wl_egl_fns().window_destroy(wl_egl_window_);
    wl_egl_window_ = nullptr;
    wl_display_ = nullptr;
    host_native_window_ = 0;
    host_native_window_resolved_ = false;
  }
  std::mutex mu_;
  void *sdl_window_ = nullptr;
  void *wl_egl_window_ = nullptr;
  void *wl_display_ = nullptr;
  uint64_t host_native_window_ = 0;
  bool host_native_window_resolved_ = false;
  int32_t width_ = 800, height_ = 600;
};
} // namespace
std::unique_ptr<AndroidWindowBackend> make_sdl_android_window_backend() {
  return std::make_unique<SdlAndroidWindowBackend>();
}
} // namespace frost
