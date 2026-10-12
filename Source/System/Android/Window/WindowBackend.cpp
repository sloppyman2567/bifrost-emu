#include "System/Android/Window/WindowBackend.hpp"
#include "System/Core/DebugFlags.hpp"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#if defined(BIFROST_USE_SDL3)
#include <SDL3/SDL.h>
#include <wayland-client.h>
#include <X11/Xlib.h>
#endif
namespace frost {
namespace {
#if defined(BIFROST_USE_SDL3)
// SDL2 guest thunks remain linked to SDL2. Resolve every SDL3 entry through its
// own DSO, as sdl2-compat does, rather than interposing overlapping SDL names.
#define SDL3_FUNCTIONS(X) \
  X(InitSubSystem) X(QuitSubSystem) X(SetHint) X(GetError) \
  X(CreateWindowWithProperties) X(CreateProperties) X(DestroyProperties) \
  X(SetStringProperty) X(SetNumberProperty) X(SetBooleanProperty) \
  X(DestroyWindow) X(GetWindowProperties) \
  X(GetPointerProperty) X(GetNumberProperty) X(SetWindowSize) \
  X(GetWindowSize) X(GetWindowSizeInPixels) X(GetWindowID) X(PollEvent) \
  X(GetWindowTitle) X(SetWindowTitle) X(GetWindowPosition) X(GetWindowFlags) \
  X(CreateSurfaceFrom) X(SetWindowIcon) X(DestroySurface)
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
#if defined(BIFROST_USE_SDL3)
class WaylandSurfaceBackend final : public AndroidWindowBackend {
public:
  WaylandSurfaceBackend(wl_display* display, wl_surface* parent, int32_t width, int32_t height)
      : display_(display), width_(width), height_(height) {
    queue_ = wl_display_create_queue(display_);
    if (!queue_) return;
    auto wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display_));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue_);
    registry_ = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    static const wl_registry_listener listener = {
      [](void* data, wl_registry* registry, uint32_t id, const char* name, uint32_t) {
        auto& self = *static_cast<WaylandSurfaceBackend*>(data);
        if (!std::strcmp(name, "wl_compositor"))
          self.compositor_ = static_cast<wl_compositor*>(wl_registry_bind(registry, id, &wl_compositor_interface, 1));
        else if (!std::strcmp(name, "wl_subcompositor"))
          self.subcompositor_ = static_cast<wl_subcompositor*>(wl_registry_bind(registry, id, &wl_subcompositor_interface, 1));
      }, [](void*, wl_registry*, uint32_t) {}};
    wl_registry_add_listener(registry_, &listener, this);
    if (wl_display_roundtrip_queue(display_, queue_) < 0 || !compositor_ || !subcompositor_) return;
    surface_ = wl_compositor_create_surface(compositor_);
    subsurface_ = wl_subcompositor_get_subsurface(subcompositor_, surface_, parent);
    wl_subsurface_set_desync(subsurface_);
    wl_subsurface_place_above(subsurface_, parent);
    if (egl_.create) window_ = egl_.create(surface_, width_, height_);
    wl_surface_commit(parent);
    wl_display_flush(display_);
  }
  ~WaylandSurfaceBackend() override { close(); }
  const char* name() const noexcept override { return "wayland-subsurface-egl"; }
  int open(const char*, int32_t, int32_t) override { return window_ ? 0 : -ENODEV; }
  void close() noexcept override {
    if (window_) egl_.destroy(window_);
    window_ = nullptr;
    if (subsurface_) wl_subsurface_destroy(subsurface_);
    if (surface_) wl_surface_destroy(surface_);
    if (subcompositor_) wl_subcompositor_destroy(subcompositor_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (queue_) wl_event_queue_destroy(queue_);
    queue_ = nullptr;
    subsurface_ = nullptr; surface_ = nullptr; subcompositor_ = nullptr;
    compositor_ = nullptr; registry_ = nullptr;
  }
  int resize(int32_t width, int32_t height, bool) override {
    if (width <= 0 || height <= 0 || !window_) return -EINVAL;
    egl_.resize(window_, width, height, 0, 0); width_ = width; height_ = height; return 0;
  }
  AndroidWindowBinding binding() override { return {reinterpret_cast<uint64_t>(window_), display_}; }
  int poll(AndroidWindowEvent& event) override { event = {}; return 0; }
  void set_visible(bool visible) override {
    if (!visible && surface_) {
      wl_surface_attach(surface_, nullptr, 0, 0);
      wl_surface_commit(surface_);
      wl_display_flush(display_);
    }
  }
private:
  wl_display* display_;
  wl_event_queue* queue_ = nullptr;
  wl_registry* registry_ = nullptr;
  wl_compositor* compositor_ = nullptr;
  wl_subcompositor* subcompositor_ = nullptr;
  wl_surface* surface_ = nullptr;
  wl_subsurface* subsurface_ = nullptr;
  WlEglFns egl_;
  void* window_ = nullptr;
  int32_t width_, height_;
};
class X11SurfaceBackend final : public AndroidWindowBackend {
public:
  X11SurfaceBackend(Display* display, Window parent, int32_t width, int32_t height) : display_(display) {
    XWindowAttributes attrs{};
    if (!XGetWindowAttributes(display, parent, &attrs)) return;
    XSetWindowAttributes settings{};
    settings.colormap = attrs.colormap;
    settings.background_pixel = 0;
    window_ = XCreateWindow(display, parent, 0, 0, width, height, 0, attrs.depth,
                            InputOutput, attrs.visual, CWColormap | CWBackPixel, &settings);
    if (window_) { XMapRaised(display_, window_); XFlush(display_); }
  }
  ~X11SurfaceBackend() override { close(); }
  const char* name() const noexcept override { return "x11-child-egl"; }
  int open(const char*, int32_t, int32_t) override { return window_ ? 0 : -ENODEV; }
  void close() noexcept override {
    if (window_) { XDestroyWindow(display_, window_); XFlush(display_); window_ = 0; }
  }
  int resize(int32_t width, int32_t height, bool) override {
    if (!window_ || width <= 0 || height <= 0) return -EINVAL;
    XResizeWindow(display_, window_, width, height); XFlush(display_); return 0;
  }
  AndroidWindowBinding binding() override { return {window_, display_}; }
  int poll(AndroidWindowEvent& event) override { event = {}; return 0; }
  void set_visible(bool visible) override {
    if (!window_) return;
    if (visible) XMapRaised(display_, window_); else XUnmapWindow(display_, window_);
    XFlush(display_);
  }
private:
  Display* display_;
  Window window_ = 0;
};
#endif
class SdlAndroidWindowBackend final : public AndroidWindowBackend {
public:
  bool window_info(arm64emu::window_stats::WindowInfo &info) override {
#if defined(BIFROST_USE_SDL3)
    if (!window_) return false;
    info.title = sdl_.GetWindowTitle(window_);
    sdl_.GetWindowPosition(window_, &info.x, &info.y);
    sdl_.GetWindowSizeInPixels(window_, &info.w, &info.h);
    auto flags = sdl_.GetWindowFlags(window_);
    info.visible = (flags & SDL_WINDOW_INPUT_FOCUS) &&
                   !(flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
    auto props = sdl_.GetWindowProperties(window_);
    info.xwindow = sdl_.GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    return true;
#else
    (void)info;
    return false;
#endif
  }
  void set_title(const char *title) override {
#if defined(BIFROST_USE_SDL3)
    if (window_) sdl_.SetWindowTitle(window_, title);
#else
    (void)title;
#endif
  }
  bool set_icon(const uint8_t *rgba, int32_t width, int32_t height) override {
#if defined(BIFROST_USE_SDL3)
    if (!window_) return false;
    // SDL copies the icon; the guest buffer and temporary surface can expire.
    auto *surface = sdl_.CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32,
                                           const_cast<uint8_t *>(rgba), width * 4);
    if (!surface) return false;
    bool result = sdl_.SetWindowIcon(window_, surface);
    sdl_.DestroySurface(surface);
    return result;
#else
    (void)rgba; (void)width; (void)height;
    return false;
#endif
  }
  ~SdlAndroidWindowBackend() override { close(); }
  const char *name() const noexcept override { return "sdl3-egl"; }
  int open(const char *title, int32_t width, int32_t height) override {
    if (!title || width <= 0 || height <= 0) return -EINVAL;
#if defined(BIFROST_USE_SDL3)
    if (window_) return -EBUSY;
    if (!sdl_.load()) return -ENOSYS;
    sdl_.SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!sdl_.InitSubSystem(SDL_INIT_VIDEO)) return fail("video initialization");
    auto props = sdl_.CreateProperties();
    if (!props) { sdl_.QuitSubSystem(SDL_INIT_VIDEO); return fail("window properties"); }
    bool configured = sdl_.SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title) &&
        sdl_.SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width) &&
        sdl_.SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height) &&
        sdl_.SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true) &&
        sdl_.SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
    window_ = configured ? sdl_.CreateWindowWithProperties(props) : nullptr;
    sdl_.DestroyProperties(props);
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
  std::unique_ptr<AndroidWindowBackend> create_surface(int32_t width, int32_t height) override {
#if defined(BIFROST_USE_SDL3)
    if (!window_) return {};
    auto props = sdl_.GetWindowProperties(window_);
    auto display = static_cast<wl_display*>(sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    auto parent = static_cast<wl_surface*>(sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    if (display && parent) {
      auto layer = std::make_unique<WaylandSurfaceBackend>(display, parent, width, height);
      if (layer->binding().window) return layer;
    } else {
      auto xdisplay = static_cast<Display*>(sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
      auto parent_window = sdl_.GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
      if (xdisplay && parent_window) {
        auto layer = std::make_unique<X11SurfaceBackend>(xdisplay, parent_window, width, height);
        if (layer->binding().window) return layer;
      }
    }
#endif
    return {};
  }
  int poll(AndroidWindowEvent &out) override {
    out = {};
#if defined(BIFROST_USE_SDL3)
    if (!window_) return 0;
    SDL_Event e;
    const auto id = sdl_.GetWindowID(window_);
    if (dbg().input_trace) {
      const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      if (now - last_pump_log_ms_ >= 2000) {
        std::fprintf(stderr, "[android-window] pump mono_ms=%lld window=%u size=%dx%d\n",
                     static_cast<long long>(now), id, width_, height_);
        last_pump_log_ms_ = now;
      }
    }
    // Bound discarded events as well as delivered ones.
    for (unsigned i = 0; i < 256 && sdl_.PollEvent(&e); ++i) {
      out = {};
      out.time_ms = static_cast<int64_t>(e.common.timestamp / 1000000);
      if (dbg().input_trace && e.type >= SDL_EVENT_WINDOW_FIRST &&
          e.type <= SDL_EVENT_WINDOW_LAST && e.window.windowID == id) {
        const char *name = "other";
        switch (e.type) {
        case SDL_EVENT_WINDOW_SHOWN: name = "shown"; break;
        case SDL_EVENT_WINDOW_HIDDEN: name = "hidden"; break;
        case SDL_EVENT_WINDOW_EXPOSED: name = "exposed"; break;
        case SDL_EVENT_WINDOW_MINIMIZED: name = "minimized"; break;
        case SDL_EVENT_WINDOW_RESTORED: name = "restored"; break;
        case SDL_EVENT_WINDOW_OCCLUDED: name = "occluded"; break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: name = "focus-gained"; break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: name = "focus-lost"; break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: name = "pixel-size-changed"; break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: name = "close-requested"; break;
        }
        std::fprintf(stderr, "[android-window] event sdl_ms=%lld window=%u name=%s type=0x%x data=%d,%d\n",
                     static_cast<long long>(out.time_ms), id, name,
                     static_cast<unsigned>(e.type), e.window.data1, e.window.data2);
      }
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
  int64_t last_pump_log_ms_ = 0;
#endif
};
} // namespace
std::unique_ptr<AndroidWindowBackend> make_sdl_android_window_backend() {
  return std::make_unique<SdlAndroidWindowBackend>();
}
} // namespace frost
