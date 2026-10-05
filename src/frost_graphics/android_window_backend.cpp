#include "frost/android_window_backend.hpp"
#include <cstdio>
#include <cerrno>
#include <dlfcn.h>
#include <mutex>
#if defined(BIFROST_USE_SDL2)
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#endif
namespace frost {
namespace {
// Each backend owns its library reference until its wrapper is destroyed.
// A process-static handle would leak a reference on every plugin reload;
// closing one during static teardown could precede the manager's destructor.
struct WlEglFns {
  WlEglFns() {
    handle = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!handle) return;
    window_create = reinterpret_cast<decltype(window_create)>(
        dlsym(handle, "wl_egl_window_create"));
    window_destroy = reinterpret_cast<decltype(window_destroy)>(
        dlsym(handle, "wl_egl_window_destroy"));
    window_resize = reinterpret_cast<decltype(window_resize)>(
        dlsym(handle, "wl_egl_window_resize"));
    if (!window_create || !window_destroy || !window_resize) {
      dlclose(handle);
      handle = nullptr;
      window_create = nullptr;
      window_destroy = nullptr;
      window_resize = nullptr;
    }
  }
  ~WlEglFns() { if (handle) dlclose(handle); }
  WlEglFns(const WlEglFns &) = delete;
  WlEglFns &operator=(const WlEglFns &) = delete;
  void *handle = nullptr;
  void *(*window_create)(void *, int, int) = nullptr;
  void (*window_destroy)(void *) = nullptr;
  void (*window_resize)(void *, int, int, int, int) = nullptr;
};
class SdlAndroidWindowBackend final : public AndroidWindowBackend {
public:
  ~SdlAndroidWindowBackend() override { clear_binding(); }
  const char *name() const noexcept override { return "sdl-egl"; }
  int attach_host_window(void *window) override {
    if (!window) return -EINVAL;
    std::lock_guard<std::mutex> lock(mu_);
    if (sdl_window_ == window)
      return 0;
    clear_binding();
    sdl_window_ = window;
    return 0;
  }
  void detach_host_window() noexcept override {
    std::lock_guard<std::mutex> lock(mu_);
    clear_binding();
    sdl_window_ = nullptr;
  }
  int resize(int32_t width, int32_t height, bool resize_host) override {
    if (width <= 0 || height <= 0) return -EINVAL;
    std::lock_guard<std::mutex> lock(mu_);
    width_ = width;
    height_ = height;
#if defined(BIFROST_USE_SDL2)
    if (resize_host && sdl_window_)
      SDL_SetWindowSize(static_cast<SDL_Window *>(sdl_window_), width, height);
#else
    (void)resize_host;
#endif
    if (wl_egl_window_)
      wl_egl_fns_->window_resize(wl_egl_window_, width, height, 0, 0);
    return 0;
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
      return {};
    }
    if (info.subsystem == SDL_SYSWM_X11) {
      host_native_window_ = static_cast<uint64_t>(info.info.x11.window);
    } else if (info.subsystem == SDL_SYSWM_WAYLAND) {
      // Host EGL's Wayland platform consumes a wl_egl_window* (NOT the
      // raw wl_surface). Create one from the SDL window's wl_surface.
      // ALSO remember SDL's wl_display: eglGetDisplay(EGL_DEFAULT_DISPLAY)
      // must wrap THE SAME connection or Mesa rejects the surface
      // (wl_surfaces are connection-private).
      wl_display_ = info.info.wl.display;
      wl_egl_fns_ = std::make_unique<WlEglFns>();
      if (wl_egl_fns_->window_create) {
        void *w = wl_egl_fns_->window_create(info.info.wl.surface, width_, height_);
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
    if (!host_native_window_) {
      // Do not expose a display without a native window, and allow a later
      // attempt to recover from a transient SDL/helper allocation failure.
      clear_binding();
      return {};
    }
    host_native_window_resolved_ = true;
    std::fprintf(stderr, "[android] host native window = 0x%llx\n",
                 static_cast<unsigned long long>(host_native_window_));
    return {host_native_window_, wl_display_};
#else
    return {};
#endif
  }

private:
  void clear_binding() {
    if (wl_egl_window_)
      wl_egl_fns_->window_destroy(wl_egl_window_);
    wl_egl_window_ = nullptr;
    wl_egl_fns_.reset();
    wl_display_ = nullptr;
    host_native_window_ = 0;
    host_native_window_resolved_ = false;
  }
  std::mutex mu_;
  void *sdl_window_ = nullptr;
  void *wl_egl_window_ = nullptr;
  std::unique_ptr<WlEglFns> wl_egl_fns_;
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
