#include "frost/android_window_backend.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#if defined(BIFROST_USE_SDL3)
#include <SDL3/SDL.h>
#endif
namespace frost {
namespace {
#if defined(BIFROST_USE_SDL3)
// SDL2 guest thunks remain linked to SDL2. Resolve every SDL3 entry through its
// own DSO, as sdl2-compat does, rather than interposing overlapping SDL names.
#define SDL3_FUNCTIONS(X) \
  X(InitSubSystem) X(QuitSubSystem) X(SetHint) X(GetError) \
  X(CreateWindow) X(DestroyWindow) X(GetWindowProperties) \
  X(GetPointerProperty) X(GetNumberProperty) X(SetWindowSize) \
  X(GetWindowSize) X(GetWindowSizeInPixels) X(GetWindowID) X(PollEvent)
struct Sdl3 {
#define DECLARE(fn) decltype(&SDL_##fn) fn = nullptr;
  SDL3_FUNCTIONS(DECLARE)
#undef DECLARE
  ~Sdl3() { if (module) dlclose(module); }
  bool load() {
    if (module) return true;
    void *candidate = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!candidate) return false;
#define RESOLVE(fn) fn = reinterpret_cast<decltype(fn)>(dlsym(candidate, "SDL_" #fn)); \
    if (!fn) { dlclose(candidate); return false; }
    SDL3_FUNCTIONS(RESOLVE)
#undef RESOLVE
    module = candidate;
    return true;
  }
  void *module = nullptr;
};
#undef SDL3_FUNCTIONS
struct WlEglFns {
  WlEglFns() {
    handle = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!handle) return;
    create = reinterpret_cast<decltype(create)>(dlsym(handle, "wl_egl_window_create"));
    destroy = reinterpret_cast<decltype(destroy)>(dlsym(handle, "wl_egl_window_destroy"));
    resize = reinterpret_cast<decltype(resize)>(dlsym(handle, "wl_egl_window_resize"));
    if (!create || !destroy || !resize) {
      dlclose(handle); handle = nullptr; create = nullptr;
    }
  }
  ~WlEglFns() { if (handle) dlclose(handle); }
  void *handle = nullptr;
  void *(*create)(void *, int, int) = nullptr;
  void (*destroy)(void *) = nullptr;
  void (*resize)(void *, int, int, int, int) = nullptr;
};
// Android keycodes; SDL scancodes never cross the plugin ABI.
int android_key(SDL_Scancode key) {
  if (key >= SDL_SCANCODE_A && key <= SDL_SCANCODE_Z) return 29 + key - SDL_SCANCODE_A;
  if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_9) return 8 + key - SDL_SCANCODE_1;
  if (key >= SDL_SCANCODE_F1 && key <= SDL_SCANCODE_F12) return 131 + key - SDL_SCANCODE_F1;
  switch (key) {
  case SDL_SCANCODE_0: return 7;
  case SDL_SCANCODE_SPACE: return 62;
  case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 66;
  case SDL_SCANCODE_BACKSPACE: return 67;
  case SDL_SCANCODE_ESCAPE: return 111;
  case SDL_SCANCODE_TAB: return 61;
  case SDL_SCANCODE_DELETE: return 112;
  case SDL_SCANCODE_LEFT: return 21;
  case SDL_SCANCODE_RIGHT: return 22;
  case SDL_SCANCODE_UP: return 19;
  case SDL_SCANCODE_DOWN: return 20;
  case SDL_SCANCODE_HOME: return 122;
  case SDL_SCANCODE_END: return 123;
  case SDL_SCANCODE_PAGEUP: return 92;
  case SDL_SCANCODE_PAGEDOWN: return 93;
  case SDL_SCANCODE_INSERT: return 124;
  case SDL_SCANCODE_LSHIFT: return 59;
  case SDL_SCANCODE_RSHIFT: return 60;
  case SDL_SCANCODE_LCTRL: return 129;
  case SDL_SCANCODE_RCTRL: return 130;
  case SDL_SCANCODE_LALT: return 57;
  case SDL_SCANCODE_RALT: return 58;
  case SDL_SCANCODE_MINUS: return 69;
  case SDL_SCANCODE_EQUALS: return 70;
  case SDL_SCANCODE_LEFTBRACKET: return 71;
  case SDL_SCANCODE_RIGHTBRACKET: return 72;
  case SDL_SCANCODE_BACKSLASH: return 73;
  case SDL_SCANCODE_SEMICOLON: return 74;
  case SDL_SCANCODE_APOSTROPHE: return 75;
  case SDL_SCANCODE_COMMA: return 55;
  case SDL_SCANCODE_PERIOD: return 56;
  case SDL_SCANCODE_SLASH: return 76;
  case SDL_SCANCODE_GRAVE: return 68;
  default: return 0;
  }
}
#endif
class SdlAndroidWindowBackend final : public AndroidWindowBackend {
public:
  ~SdlAndroidWindowBackend() override { close(); }
  const char *name() const noexcept override { return "sdl3-egl"; }
  int open(const char *title, int32_t width, int32_t height) override {
    if (!title || width <= 0 || height <= 0) return -EINVAL;
#if defined(BIFROST_USE_SDL3)
    if (window_) return -EBUSY;
    if (!sdl_.load()) return -ENOSYS;
    sdl_.SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!sdl_.InitSubSystem(SDL_INIT_VIDEO)) return fail("video initialization");
    window_ = sdl_.CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE);
    if (!window_) { sdl_.QuitSubSystem(SDL_INIT_VIDEO); return fail("window creation"); }
    width_ = width; height_ = height;
    if (int result = resize(width, height, true)) { close(); return result; }
    return 0;
#else
    return -ENOSYS;
#endif
  }
  void close() noexcept override {
#if defined(BIFROST_USE_SDL3)
    clear_binding();
    if (window_) {
      sdl_.DestroyWindow(window_);
      window_ = nullptr;
      sdl_.QuitSubSystem(SDL_INIT_VIDEO);
    }
#endif
  }
  int resize(int32_t width, int32_t height, bool resize_host) override {
    if (width <= 0 || height <= 0) return -EINVAL;
#if defined(BIFROST_USE_SDL3)
    // Commit buffer geometry only after the host accepts the operation.
    if (resize_host && window_) {
      int pixel_w = 0, pixel_h = 0, logical_w = 0, logical_h = 0;
      if (!sdl_.GetWindowSizeInPixels(window_, &pixel_w, &pixel_h) ||
          !sdl_.GetWindowSize(window_, &logical_w, &logical_h) ||
          pixel_w <= 0 || pixel_h <= 0 || logical_w <= 0 || logical_h <= 0)
        return fail("window size query");
      // SDL_SetWindowSize uses logical coordinates. The adapter's geometry
      // and native EGL wrapper use pixels, including on scaled displays.
      int requested_w = static_cast<int>((int64_t(width) * logical_w + pixel_w - 1) / pixel_w);
      int requested_h = static_cast<int>((int64_t(height) * logical_h + pixel_h - 1) / pixel_h);
      if ((requested_w != logical_w || requested_h != logical_h) &&
          !sdl_.SetWindowSize(window_, requested_w, requested_h))
        return fail("window resize");
    }
    if (wl_window_) wl_fns_->resize(wl_window_, width, height, 0, 0);
    width_ = width; height_ = height;
#else
    (void)resize_host;
#endif
    return 0;
  }
  AndroidWindowBinding binding() override {
#if defined(BIFROST_USE_SDL3)
    if (!window_) return {};
    if (binding_.window) return binding_;
    auto props = sdl_.GetWindowProperties(window_);
    auto display = sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    auto surface = sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    if (display && surface) {
      wl_fns_ = std::make_unique<WlEglFns>();
      if (wl_fns_->create) wl_window_ = wl_fns_->create(surface, width_, height_);
      if (wl_window_) binding_ = {reinterpret_cast<uint64_t>(wl_window_), display};
    } else {
      display = sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
      auto window = sdl_.GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
      if (display && window) binding_ = {static_cast<uint64_t>(window), display};
    }
    if (!binding_.window) clear_binding(); // Later calls may recover.
    return binding_;
#else
    return {};
#endif
  }
  int poll(AndroidWindowEvent &out) override {
    out = {};
#if defined(BIFROST_USE_SDL3)
    if (!window_) return 0;
    SDL_Event e;
    const auto id = sdl_.GetWindowID(window_);
    // Bound discarded events as well as delivered ones.
    for (unsigned i = 0; i < 256 && sdl_.PollEvent(&e); ++i) {
      out = {};
      out.time_ms = static_cast<int64_t>(e.common.timestamp / 1000000);
      if (e.type == SDL_EVENT_QUIT) { out.type = BIFROST_WINDOW_CLOSE; return 1; }
      if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == id) {
        out.type = BIFROST_WINDOW_CLOSE; return 1;
      }
      if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && e.window.windowID == id) {
        if (e.window.data1 <= 0 || e.window.data2 <= 0) continue;
        out.type = BIFROST_WINDOW_RESIZE;
        out.width = e.window.data1; out.height = e.window.data2; return 1;
      }
      if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && e.key.windowID == id) {
        int key = android_key(e.key.scancode);
        if (!key) continue;
        out.type = BIFROST_WINDOW_KEY; out.key_code = key;
        out.scan_code = e.key.raw;
        out.action = e.type == SDL_EVENT_KEY_DOWN ? 0 : 1;
        out.repeat_count = e.key.repeat ? 1 : 0;
        if (e.key.mod & SDL_KMOD_SHIFT) out.meta_state |= 1;
        if (e.key.mod & SDL_KMOD_ALT) out.meta_state |= 2;
        if (e.key.mod & SDL_KMOD_CTRL) out.meta_state |= 0x1000;
        if (e.key.mod & SDL_KMOD_GUI) out.meta_state |= 0x10000;
        if (e.key.mod & SDL_KMOD_CAPS) out.meta_state |= 0x100000;
        return 1;
      }
      bool pointer = false;
      int pixel_w = 0, pixel_h = 0, logical_w = 0, logical_h = 0;
      if (!sdl_.GetWindowSizeInPixels(window_, &pixel_w, &pixel_h) ||
          !sdl_.GetWindowSize(window_, &logical_w, &logical_h) ||
          pixel_w <= 0 || pixel_h <= 0 || logical_w <= 0 || logical_h <= 0) continue;
      if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
          e.button.windowID == id && e.button.button == SDL_BUTTON_LEFT) {
        out.x = e.button.x; out.y = e.button.y;
        out.action = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 0 : 1;
        out.pressure = out.action == 0 ? 1 : 0; pointer = true;
      } else if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.windowID == id) {
        out.x = e.motion.x; out.y = e.motion.y;
        out.pressure = (e.motion.state & SDL_BUTTON_LMASK) ? 1 : 0;
        out.action = out.pressure ? 2 : 7; pointer = true;
      } else if ((e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_UP ||
                  e.type == SDL_EVENT_FINGER_MOTION) && e.tfinger.windowID == id) {
        out.type = BIFROST_WINDOW_POINTER;
        out.x = e.tfinger.x * pixel_w; out.y = e.tfinger.y * pixel_h;
        out.pressure = e.tfinger.pressure;
        out.pointer_id = static_cast<int32_t>(e.tfinger.fingerID & 0x7fffffff);
        out.action = e.type == SDL_EVENT_FINGER_DOWN ? 0 : e.type == SDL_EVENT_FINGER_UP ? 1 : 2;
        return 1;
      }
      if (pointer) {
        out.type = BIFROST_WINDOW_POINTER;
        out.x *= static_cast<float>(pixel_w) / logical_w;
        out.y *= static_cast<float>(pixel_h) / logical_h;
        return 1;
      }
    }
#endif
    out = {};
    return 0;
  }
private:
#if defined(BIFROST_USE_SDL3)
  int fail(const char *operation) {
    std::fprintf(stderr, "[android] SDL3 %s failed: %s\n", operation, sdl_.GetError());
    return -EIO;
  }
  void clear_binding() noexcept {
    if (wl_window_) wl_fns_->destroy(wl_window_);
    wl_window_ = nullptr;
    wl_fns_.reset();
    binding_ = {};
  }
  Sdl3 sdl_;
  SDL_Window *window_ = nullptr;
  std::unique_ptr<WlEglFns> wl_fns_;
  void *wl_window_ = nullptr;
  AndroidWindowBinding binding_;
  int32_t width_ = 800, height_ = 600;
#endif
};
} // namespace
std::unique_ptr<AndroidWindowBackend> make_sdl_android_window_backend() {
  return std::make_unique<SdlAndroidWindowBackend>();
}
} // namespace frost
