#include "System/Android/Window/WindowBackend.hpp"
#include "System/Core/DebugFlags.hpp"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <climits>
#include <cmath>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <map>
#include <vector>
#if defined(BIFROST_USE_SDL3)
#include <SDL3/SDL.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include "Generated/ViewporterClient.h"
// wayland-scanner's interface tables; this translation unit is their only user.
// C++ ignores WL_PRIVATE's visibility on definitions the header declared.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#include "Generated/ViewporterProtocol.inc"
#pragma GCC diagnostic pop
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
  X(CreateSurfaceFrom) X(SetWindowIcon) X(DestroySurface) \
  X(GetDisplayForWindow) X(GetDisplayBounds) X(GetTicksNS) \
  X(StartTextInput) X(StopTextInput) X(SetTextInputArea) X(PeepEvents) \
  X(PushEvent) X(GetScancodeFromName) X(GetKeyFromScancode)
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
// Host pixels per desktop unit of the SDL window, shared with its layers.
using PixelRatio = std::shared_ptr<std::atomic<double>>;
// Maps a layer's pixel-sized buffers onto the parent window's desktop
// coordinates, as SDL does for the primary surface. wp_viewporter scales by
// any (fractional) ratio; without it, an integer ratio uses the core buffer
// scale (wl_surface v3), whose buffers must be multiples of the scale. Both
// are double-buffered surface state: the next commit applies them.
class SurfaceScale {
public:
  ~SurfaceScale() { reset(); }
  // Before the surface and the event queue its proxies use are destroyed.
  void reset() {
    if (viewport_) wp_viewport_destroy(viewport_);
    if (viewporter_) wp_viewporter_destroy(viewporter_);
    viewport_ = nullptr; viewporter_ = nullptr; surface_ = nullptr;
  }
  void bind(wl_registry *registry, uint32_t id, const char *name) {
    if (!viewporter_ && !std::strcmp(name, wp_viewporter_interface.name))
      viewporter_ = static_cast<wp_viewporter *>(wl_registry_bind(registry, id, &wp_viewporter_interface, 1));
  }
  void attach(wl_surface *surface, uint32_t compositor_version) {
    surface_ = surface;
    compositor_version_ = compositor_version;
    if (viewporter_) viewport_ = wp_viewporter_get_viewport(viewporter_, surface);
  }
  void apply(int32_t w, int32_t h, double ratio) {
    if (!surface_) return;
    if (viewport_) {
      // -1 unsets the destination: an unscaled layer keeps its buffer size.
      int32_t dw = ratio == 1.0 ? -1 : std::max<int32_t>(1, int32_t(std::lround(w / ratio)));
      int32_t dh = ratio == 1.0 ? -1 : std::max<int32_t>(1, int32_t(std::lround(h / ratio)));
      if (dw != dest_w_ || dh != dest_h_) wp_viewport_set_destination(viewport_, dw, dh);
      dest_w_ = dw; dest_h_ = dh;
      return;
    }
    const long n = std::lround(ratio);
    const int32_t scale = n >= 1 && std::fabs(ratio - double(n)) < 1e-6 && w % n == 0 && h % n == 0
        ? int32_t(n) : 1;
    if (scale != buffer_scale_ && compositor_version_ >= 3) {
      wl_surface_set_buffer_scale(surface_, scale);
      buffer_scale_ = scale;
    }
  }
  // A window-pixel coordinate in the parent's desktop coordinates.
  static int32_t to_desktop(int32_t pixels, double ratio) {
    return int32_t(std::lround(pixels / ratio));
  }
private:
  wp_viewporter *viewporter_ = nullptr;
  wp_viewport *viewport_ = nullptr;
  wl_surface *surface_ = nullptr;
  uint32_t compositor_version_ = 1;
  int32_t dest_w_ = -1, dest_h_ = -1, buffer_scale_ = 1;
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
// Popup layers receive straight-alpha 0xAARRGGBB rows from the guest renderer.
inline uint32_t premultiply(uint32_t p) {
  const uint32_t a = p >> 24;
  if (a == 255) return p;
  if (a == 0) return 0;
  auto channel = [a](uint32_t c) { return (c * a + 127) / 255; };
  return (a << 24) | (channel((p >> 16) & 255) << 16) | (channel((p >> 8) & 255) << 8) |
         channel(p & 255);
}
class PixelLayer {
public:
  virtual ~PixelLayer() = default;
  virtual int show(int32_t x, int32_t y, int32_t w, int32_t h, const uint32_t *argb) = 0;
  virtual uintptr_t native_layer() const = 0;
};
// A desynchronized subsurface with shared-memory buffers. Its empty input
// region leaves pointer focus on the primary surface.
class WaylandPixels final : public PixelLayer {
public:
  WaylandPixels(wl_display *display, wl_surface *parent, PixelRatio ratio)
      : display_(display), parent_(parent), ratio_(std::move(ratio)) {
    queue_ = wl_display_create_queue(display_);
    if (!queue_) return;
    auto wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display_));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue_);
    registry_ = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    static const wl_registry_listener listener = {
      [](void *data, wl_registry *registry, uint32_t id, const char *name, uint32_t version) {
        auto &self = *static_cast<WaylandPixels *>(data);
        if (!std::strcmp(name, "wl_compositor")) {
          self.compositor_version_ = version < 3 ? version : 3;
          self.compositor_ = static_cast<wl_compositor *>(
              wl_registry_bind(registry, id, &wl_compositor_interface, self.compositor_version_));
        } else if (!std::strcmp(name, "wl_subcompositor"))
          self.subcompositor_ = static_cast<wl_subcompositor *>(
              wl_registry_bind(registry, id, &wl_subcompositor_interface, 1));
        else if (!std::strcmp(name, "wl_shm"))
          self.shm_ = static_cast<wl_shm *>(wl_registry_bind(registry, id, &wl_shm_interface, 1));
        else self.scale_.bind(registry, id, name);
      }, [](void *, wl_registry *, uint32_t) {}};
    wl_registry_add_listener(registry_, &listener, this);
    if (wl_display_roundtrip_queue(display_, queue_) < 0 || !compositor_ || !subcompositor_ || !shm_)
      return;
    surface_ = wl_compositor_create_surface(compositor_);
    subsurface_ = wl_subcompositor_get_subsurface(subcompositor_, surface_, parent_);
    wl_subsurface_set_desync(subsurface_);
    scale_.attach(surface_, compositor_version_);
    wl_region *none = wl_compositor_create_region(compositor_);
    wl_surface_set_input_region(surface_, none);
    wl_region_destroy(none);
  }
  ~WaylandPixels() override {
    for (auto &buffer : buffers_) release(buffer);
    scale_.reset();
    if (subsurface_) wl_subsurface_destroy(subsurface_);
    if (surface_) wl_surface_destroy(surface_);
    if (parent_ && surface_) wl_surface_commit(parent_);
    if (shm_) wl_shm_destroy(shm_);
    if (subcompositor_) wl_subcompositor_destroy(subcompositor_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_flush(display_);
    if (queue_) wl_event_queue_destroy(queue_);
  }
  bool ready() const { return surface_ != nullptr; }
  wl_surface* surface() const { return surface_; }
  uintptr_t native_layer() const override { return reinterpret_cast<uintptr_t>(surface_); }
  void below(uintptr_t sibling) {
    if (sibling) { wl_subsurface_place_below(subsurface_, reinterpret_cast<wl_surface*>(sibling)); wl_surface_commit(parent_); }
  }
  void geometry(int32_t x, int32_t y, int32_t w, int32_t h) {
    const double ratio = ratio_->load();
    scale_.apply(w, h, ratio);
    const int32_t dx = SurfaceScale::to_desktop(x, ratio), dy = SurfaceScale::to_desktop(y, ratio);
    if (dx != x_ || dy != y_) {
      wl_subsurface_set_position(subsurface_, dx, dy);
      wl_surface_commit(parent_);
      x_ = dx; y_ = dy;
    }
  }
  void hide() {
    if (!surface_) return;
    wl_surface_attach(surface_, nullptr, 0, 0);
    wl_surface_commit(surface_); wl_display_flush(display_);
  }

  int show(int32_t x, int32_t y, int32_t w, int32_t h, const uint32_t *argb) override {
    wl_display_dispatch_queue_pending(display_, queue_);  // buffer releases
    Buffer *target = nullptr;
    for (auto &buffer : buffers_)
      if (!buffer.busy && buffer.w == w && buffer.h == h) { target = &buffer; break; }
    if (!target)
      for (auto &buffer : buffers_)
        if (!buffer.busy) { release(buffer); if (!allocate(buffer, w, h)) return -ENOMEM; target = &buffer; break; }
    if (!target) return -EAGAIN;  // both buffers are still on screen; redraw next frame
    auto *out = static_cast<uint32_t *>(target->data);
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i) out[i] = premultiply(argb[i]);
    target->busy = true;
    geometry(x, y, w, h);
    wl_surface_attach(surface_, target->buffer, 0, 0);
    wl_surface_damage(surface_, 0, 0, w, h);
    wl_surface_commit(surface_);
    wl_display_flush(display_);
    return 0;
  }
private:
  struct Buffer { wl_buffer *buffer = nullptr; void *data = nullptr; size_t size = 0; int32_t w = 0, h = 0; bool busy = false; };
  bool allocate(Buffer &buffer, int32_t w, int32_t h) {
    const size_t size = size_t(w) * h * 4;
    int fd = memfd_create("bifrost-popup", MFD_CLOEXEC);
    if (fd < 0) return false;
    void *data = ftruncate(fd, off_t(size)) ? MAP_FAILED
        : mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { ::close(fd); return false; }
    wl_shm_pool *pool = wl_shm_create_pool(shm_, fd, int32_t(size));
    buffer.buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    ::close(fd);
    static const wl_buffer_listener listener = {
      [](void *data, wl_buffer *) { static_cast<Buffer *>(data)->busy = false; }};
    wl_buffer_add_listener(buffer.buffer, &listener, &buffer);
    buffer.data = data; buffer.size = size; buffer.w = w; buffer.h = h; buffer.busy = false;
    return true;
  }
  static void release(Buffer &buffer) {
    if (buffer.buffer) wl_buffer_destroy(buffer.buffer);
    if (buffer.data) munmap(buffer.data, buffer.size);
    buffer = Buffer{};
  }
  wl_display *display_;
  wl_surface *parent_;
  wl_event_queue *queue_ = nullptr;
  wl_registry *registry_ = nullptr;
  wl_compositor *compositor_ = nullptr;
  wl_subcompositor *subcompositor_ = nullptr;
  wl_shm *shm_ = nullptr;
  wl_surface *surface_ = nullptr;
  wl_subsurface *subsurface_ = nullptr;
  PixelRatio ratio_;
  uint32_t compositor_version_ = 1;
  SurfaceScale scale_;
  Buffer buffers_[2];
  int32_t x_ = INT32_MIN, y_ = INT32_MIN; // desktop coordinates
};
// Opaque black content for a toplevel that nothing else draws into. A Wayland
// toplevel, and every subsurface below it, stays unmapped until a buffer is
// committed to its own surface. SurfaceView layers are subsurfaces, so the
// SDL window needs a backdrop whenever no EGL surface renders into it.
class WaylandBackdrop {
public:
  ~WaylandBackdrop() { reset(); }
  // Sized in buffer pixels; SDL's viewport or buffer scale maps it to the window.
  bool show(wl_display *display, wl_surface *surface, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    if (surface_ && (surface_ != surface || display_ != display)) reset();
    if (surface_ == surface && w_ == w && h_ == h) return true;
    if (!shm_ && !bind(display)) { reset(); return false; }
    const size_t size = size_t(w) * h * 4;
    int fd = memfd_create("bifrost-backdrop", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, off_t(size))) { ::close(fd); return false; }
    wl_shm_pool *pool = wl_shm_create_pool(shm_, fd, int32_t(size));  // zero-filled: black
    wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    ::close(fd);
    if (!buffer) return false;
    // Replace the previous buffer only once the new one is committed.
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);
    if (buffer_) wl_buffer_destroy(buffer_);
    buffer_ = buffer; surface_ = surface; w_ = w; h_ = h;
    wl_display_flush(display_);
    return true;
  }
  bool active() const { return buffer_ != nullptr; }
  // The SDL window, and so its wl_surface, outlives this backdrop.
  void reset() {
    if (buffer_) wl_buffer_destroy(buffer_);
    if (shm_) wl_shm_destroy(shm_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_flush(display_);
    if (queue_) wl_event_queue_destroy(queue_);
    buffer_ = nullptr; shm_ = nullptr; registry_ = nullptr; queue_ = nullptr;
    display_ = nullptr; surface_ = nullptr; w_ = h_ = 0;
  }
private:
  bool bind(wl_display *display) {
    display_ = display;
    queue_ = wl_display_create_queue(display);
    if (!queue_) return false;
    auto wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue_);
    registry_ = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    static const wl_registry_listener listener = {
      [](void *data, wl_registry *registry, uint32_t id, const char *name, uint32_t) {
        auto &self = *static_cast<WaylandBackdrop *>(data);
        if (!std::strcmp(name, "wl_shm"))
          self.shm_ = static_cast<wl_shm *>(wl_registry_bind(registry, id, &wl_shm_interface, 1));
      }, [](void *, wl_registry *, uint32_t) {}};
    wl_registry_add_listener(registry_, &listener, this);
    return wl_display_roundtrip_queue(display, queue_) >= 0 && shm_;
  }
  wl_display *display_ = nullptr;
  wl_event_queue *queue_ = nullptr;
  wl_registry *registry_ = nullptr;
  wl_shm *shm_ = nullptr;
  wl_buffer *buffer_ = nullptr;
  wl_surface *surface_ = nullptr;
  int32_t w_ = 0, h_ = 0;
};
// An unselected child window: its input events propagate to the SDL parent.
// A 32-bit ARGB visual differs from the parent's, so the Composite extension
// implicitly redirects the child and blends its premultiplied alpha. Without
// one, the parent's visual is used and translucent pixels blend on black.
class X11Pixels final : public PixelLayer {
public:
  X11Pixels(Display *display, Window parent, bool surface = false) : display_(display), parent_(parent) {
    XWindowAttributes attrs{};
    if (!XGetWindowAttributes(display_, parent_, &attrs) || !attrs.visual) return;
    XVisualInfo argb{};
    if (!surface && XMatchVisualInfo(display_, XScreenNumberOfScreen(attrs.screen), 32, TrueColor, &argb) &&
        rgb_masks(argb.visual)) {
      visual_ = argb.visual; depth_ = 32; alpha_ = true;
      colormap_ = owned_colormap_ = XCreateColormap(display_, parent_, visual_, AllocNone);
    } else if (rgb_masks(attrs.visual) && attrs.depth >= 24) {
      visual_ = attrs.visual; depth_ = attrs.depth; colormap_ = attrs.colormap;
    }
  }
  ~X11Pixels() override {
    if (gc_) XFreeGC(display_, gc_);
    if (window_) XDestroyWindow(display_, window_);
    if (owned_colormap_) XFreeColormap(display_, owned_colormap_);
    XFlush(display_);
  }
  bool ready() const { return visual_ != nullptr; }
  Window window() const { return window_; }
  uintptr_t native_layer() const override { return window_; }
  void below(uintptr_t sibling) {
    if (sibling && window_) {
      XWindowChanges changes{}; changes.sibling = sibling; changes.stack_mode = Below;
      XConfigureWindow(display_, window_, CWSibling | CWStackMode, &changes); XFlush(display_);
    }
  }
  void visible(bool shown) {
    if (!window_) return;
    if (shown) XMapWindow(display_, window_); else XUnmapWindow(display_, window_);
    XFlush(display_);
  }
  int show(int32_t x, int32_t y, int32_t w, int32_t h, const uint32_t *argb) override {
    if (!window_) {
      XSetWindowAttributes settings{};
      settings.colormap = colormap_;
      settings.background_pixel = 0;
      settings.border_pixel = 0;  // required when the depth differs from the parent's
      window_ = XCreateWindow(display_, parent_, x, y, unsigned(w), unsigned(h), 0, depth_,
                              InputOutput, visual_, CWColormap | CWBackPixel | CWBorderPixel,
                              &settings);
      if (!window_) return -ENODEV;
      gc_ = XCreateGC(display_, window_, 0, nullptr);
      XMapRaised(display_, window_);
    } else if (x != x_ || y != y_ || w != w_ || h != h_) {
      XMoveResizeWindow(display_, window_, x, y, unsigned(w), unsigned(h));
    }
    x_ = x; y_ = y; w_ = w; h_ = h;
    if (!argb) { XFlush(display_); return 0; }
    pixels_.resize(size_t(w) * h);
    const uint32_t keep = alpha_ ? 0xffffffffu : 0xffffffu;
    for (size_t i = 0; i < pixels_.size(); ++i) pixels_[i] = premultiply(argb[i]) & keep;
    XImage *image = XCreateImage(display_, visual_, unsigned(depth_), ZPixmap, 0,
                                 reinterpret_cast<char *>(pixels_.data()), unsigned(w), unsigned(h), 32, w * 4);
    if (!image) return -ENOMEM;
    XPutImage(display_, window_, gc_, image, 0, 0, 0, 0, unsigned(w), unsigned(h));
    image->data = nullptr;  // owned by pixels_
    XDestroyImage(image);
    XFlush(display_);
    return 0;
  }
private:
  static bool rgb_masks(const Visual *visual) {
    return visual && visual->red_mask == 0xff0000 && visual->green_mask == 0xff00 &&
           visual->blue_mask == 0xff;
  }
  Display *display_;
  Window parent_;
  Visual *visual_ = nullptr;
  int depth_ = 0;
  bool alpha_ = false;
  Colormap colormap_ = 0, owned_colormap_ = 0;
  Window window_ = 0;
  GC gc_ = nullptr;
  int32_t x_ = 0, y_ = 0, w_ = 0, h_ = 0;
  std::vector<uint32_t> pixels_;
};
class WaylandSurfaceBackend final : public AndroidWindowBackend {
public:
  WaylandSurfaceBackend(wl_display* display, wl_surface* parent, int32_t width, int32_t height,
                        PixelRatio ratio, uintptr_t below = 0) : display_(display), pixels_(std::make_unique<WaylandPixels>(display, parent, std::move(ratio))) {
    if (pixels_->ready() && egl_.create) {
      pixels_->geometry(0, 0, width, height);
      window_ = egl_.create(pixels_->surface(), width, height);
      pixels_->below(below);
    }
  }
  ~WaylandSurfaceBackend() override { close(); }
  const char* name() const noexcept override { return "wayland-surface"; }
  uintptr_t layer() const override { return pixels_ ? pixels_->native_layer() : 0; }
  int place_below(uintptr_t sibling) override {
    if (!pixels_ || !sibling) return -EINVAL;
    pixels_->below(sibling);
    return 0;
  }
  int open(const char*, int32_t, int32_t) override { return window_ ? 0 : -ENODEV; }
  void close() noexcept override {
    if (window_) egl_.destroy(window_);
    window_ = nullptr;
    pixels_.reset();
  }
  int resize(int32_t width, int32_t height, bool) override {
    if (!window_ || width <= 0 || height <= 0) return -EINVAL;
    pixels_->geometry(0, 0, width, height);
    egl_.resize(window_, width, height, 0, 0);
    return 0;
  }
  AndroidWindowBinding binding() override { return {reinterpret_cast<uint64_t>(window_), display_}; }
  int poll(AndroidWindowEvent& event) override { event = {}; return 0; }
  void set_visible(bool shown) override { if (pixels_ && !shown) pixels_->hide(); }
  int present_pixels(int32_t x, int32_t y, int32_t w, int32_t h, const uint32_t* pixels) override {
    return pixels_ ? pixels_->show(x, y, w, h, pixels) : -ENODEV;
  }
  int surface_geometry(int32_t x, int32_t y, int32_t w, int32_t h) override {
    if (!window_) return -ENODEV;
    pixels_->geometry(x, y, w, h); egl_.resize(window_, w, h, 0, 0); return 0;
  }
private:
  wl_display* display_;
  std::unique_ptr<WaylandPixels> pixels_;
  WlEglFns egl_;
  void* window_ = nullptr;
};
class X11SurfaceBackend final : public AndroidWindowBackend {
public:
  X11SurfaceBackend(Display* display, Window parent, int32_t width, int32_t height, uintptr_t below = 0)
      : display_(display), pixels_(std::make_unique<X11Pixels>(display, parent, true)) {
    if (pixels_->ready()) pixels_->show(0, 0, width, height, nullptr);
    pixels_->below(below);
  }
  ~X11SurfaceBackend() override { close(); }
  const char* name() const noexcept override { return "x11-surface"; }
  uintptr_t layer() const override { return pixels_ ? pixels_->native_layer() : 0; }
  int place_below(uintptr_t sibling) override {
    if (!pixels_ || !sibling) return -EINVAL;
    pixels_->below(sibling);
    return 0;
  }
  int open(const char*, int32_t, int32_t) override { return pixels_ && pixels_->window() ? 0 : -ENODEV; }
  void close() noexcept override { pixels_.reset(); }
  int resize(int32_t width, int32_t height, bool) override {
    if (!pixels_ || width <= 0 || height <= 0) return -EINVAL;
    return pixels_->show(0, 0, width, height, nullptr);
  }
  AndroidWindowBinding binding() override { return {pixels_ ? pixels_->window() : 0, display_}; }
  int poll(AndroidWindowEvent& event) override { event = {}; return 0; }
  void set_visible(bool shown) override { if (pixels_) pixels_->visible(shown); }
  int present_pixels(int32_t x, int32_t y, int32_t w, int32_t h, const uint32_t* pixels) override {
    return pixels_ ? pixels_->show(x, y, w, h, pixels) : -ENODEV;
  }
  int surface_geometry(int32_t x, int32_t y, int32_t w, int32_t h) override {
    return pixels_ ? pixels_->show(x, y, w, h, nullptr) : -ENODEV;
  }
private:
  Display* display_;
  std::unique_ptr<X11Pixels> pixels_;
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
  bool pixel_size(int32_t &w, int32_t &h) override {
#if defined(BIFROST_USE_SDL3)
    return window_ && update_ratio(w, h);
#else
    (void)w; (void)h;
    return false;
#endif
  }
  bool display_pixel_size(int32_t &w, int32_t &h) override {
#if defined(BIFROST_USE_SDL3)
    // Display bounds are in desktop units; the window's pixel density
    // converts them as it does the window's own size.
    SDL_Rect bounds{};
    const SDL_DisplayID display = window_ ? sdl_.GetDisplayForWindow(window_) : 0;
    const double ratio = ratio_->load();
    if (!display || !sdl_.GetDisplayBounds(display, &bounds) || bounds.w <= 0 || bounds.h <= 0 ||
        ratio <= 0)
      return false;
    w = static_cast<int32_t>(std::lround(bounds.w * ratio));
    h = static_cast<int32_t>(std::lround(bounds.h * ratio));
    return true;
#else
    (void)w; (void)h;
    return false;
#endif
  }
  double pixel_density() override {
#if defined(BIFROST_USE_SDL3)
    return ratio_->load();
#else
    return 1.0;
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
        sdl_.SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true) &&
        // Buffers at the display's native pixel density; SDL scales the
        // surface back to the requested desktop size (Wayland viewport or
        // buffer scale). Without it a scaled compositor upsamples the window.
        sdl_.SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN,
                                !dbg().android_logical_pixels);
    window_ = configured ? sdl_.CreateWindowWithProperties(props) : nullptr;
    sdl_.DestroyProperties(props);
    if (!window_) { sdl_.QuitSubSystem(SDL_INIT_VIDEO); return fail("window creation"); }
    if (!update_ratio(width_, height_)) { close(); return fail("window size query"); }
    return 0;
#else
    return -ENOSYS;
#endif
  }
  void close() noexcept override {
#if defined(BIFROST_USE_SDL3)
    popups_.clear();
    backdrop_.reset();
    clear_binding();
    mouse_fingers_ = 0;
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
    if (window_ && backdrop_.active()) refresh_backdrop();
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
    // Window layers stay above SurfaceViews, including a surface created
    // after its window was already drawn.
    uintptr_t below = popups_.empty() ? 0 : popups_.begin()->second->native_layer();
    auto props = sdl_.GetWindowProperties(window_);
    auto display = static_cast<wl_display*>(sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    auto parent = static_cast<wl_surface*>(sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    if (display && parent) {
      auto layer = std::make_unique<WaylandSurfaceBackend>(display, parent, width, height, ratio_, below);
      if (layer->binding().window) { refresh_backdrop(); return layer; }
    } else {
      auto xdisplay = static_cast<Display*>(sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
      auto parent_window = sdl_.GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
      if (xdisplay && parent_window) {
        auto layer = std::make_unique<X11SurfaceBackend>(xdisplay, parent_window, width, height, below);
        if (layer->binding().window) return layer;
      }
    }
#endif
    return {};
  }
  int show_popup(uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                 const uint32_t *argb) override {
#if defined(BIFROST_USE_SDL3)
    if (!window_) return -ENODEV;
    auto &layer = popups_[id];
    if (!layer) {
      auto props = sdl_.GetWindowProperties(window_);
      auto display = static_cast<wl_display *>(
          sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
      auto parent = static_cast<wl_surface *>(
          sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
      if (display && parent) {
        auto popup = std::make_unique<WaylandPixels>(display, parent, ratio_);
        if (popup->ready()) layer = std::move(popup);
      } else {
        auto xdisplay = static_cast<Display *>(
            sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
        auto xparent = sdl_.GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (xdisplay && xparent) {
          auto popup = std::make_unique<X11Pixels>(xdisplay, Window(xparent));
          if (popup->ready()) layer = std::move(popup);
        }
      }
      if (!layer) { popups_.erase(id); return -ENOTSUP; }
    }
    return layer->show(x, y, w, h, argb);
#else
    (void)id; (void)x; (void)y; (void)w; (void)h; (void)argb;
    return -ENOTSUP;
#endif
  }
  void remove_popup(uint32_t id) override {
#if defined(BIFROST_USE_SDL3)
    popups_.erase(id);
#else
    (void)id;
#endif
  }
  int set_text_input(bool active, int32_t x, int32_t y, int32_t w, int32_t h) override {
#if defined(BIFROST_USE_SDL3)
    if (!window_) return -ENODEV;
    if (active) {
      // The anchor is in surface pixels; SDL wants window coordinates.
      const double scale = std::max(0.01, ratio_->load());
      SDL_Rect area{static_cast<int>(x / scale), static_cast<int>(y / scale),
                    std::max(1, static_cast<int>(w / scale)), std::max(1, static_cast<int>(h / scale))};
      sdl_.SetTextInputArea(window_, &area, 0);
      if (!sdl_.StartTextInput(window_)) return fail("StartTextInput");
      text_active_ = true;
      if (!dbg().text_script.empty() && script_.empty()) load_script();
      return 0;
    }
    text_active_ = false;
    text_.clear();
    text_reports_ = 0;
    sdl_.StopTextInput(window_);
    return 0;
#else
    (void)active; (void)x; (void)y; (void)w; (void)h;
    return -ENOTSUP;
#endif
  }
  int poll_text(AndroidWindowText &text) override {
    text = {};
#if defined(BIFROST_USE_SDL3)
    if (text_.empty()) return 0;
    text = text_.front();
    text_.pop_front();
    return 1;
#else
    return 0;
#endif
  }
  int poll(AndroidWindowEvent &out) override {
    out = {};
#if defined(BIFROST_USE_SDL3)
    if (!window_) return 0;
    if (text_reports_ > 0) {  // the rest of a long commit, before any later event
      --text_reports_;
      out.type = BIFROST_WINDOW_TEXT;
      out.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      return 1;
    }
    run_script();
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
    // SDL timestamps count from SDL initialization; the plugin ABI and
    // Android (System.nanoTime) use CLOCK_MONOTONIC, i.e. steady_clock.
    const int64_t sdl_to_monotonic_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() -
        static_cast<int64_t>(sdl_.GetTicksNS());
    // Bound discarded events as well as delivered ones.
    for (unsigned i = 0; i < 256 && sdl_.PollEvent(&e); ++i) {
      out = {};
      out.time_ms = std::max<int64_t>(
          0, (static_cast<int64_t>(e.common.timestamp) + sdl_to_monotonic_ns) / 1000000);
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
      // Also sent when the window moves to a display with another scale.
      if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && e.window.windowID == id) {
        if (e.window.data1 <= 0 || e.window.data2 <= 0) continue;
        int32_t pixel_w, pixel_h;
        update_ratio(pixel_w, pixel_h);
        out.type = BIFROST_WINDOW_RESIZE;
        out.width = e.window.data1; out.height = e.window.data2; return 1;
      }
      if ((e.type == SDL_EVENT_TEXT_INPUT || e.type == SDL_EVENT_TEXT_EDITING) && text_active_) {
        const bool commit = e.type == SDL_EVENT_TEXT_INPUT;
        if ((commit ? e.text.windowID : e.edit.windowID) != id) continue;
        const unsigned queued = queue_text(commit ? BIFROST_WINDOW_TEXT_COMMIT : BIFROST_WINDOW_TEXT_COMPOSING,
                                           commit ? e.text.text : e.edit.text);
        if (!queued) continue;
        text_reports_ += queued - 1;
        out.type = BIFROST_WINDOW_TEXT;
        return 1;
      }
      if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && e.key.windowID == id) {
        if (key_became_text(e)) continue;
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
      // Android's InputReader synthesizes BACK/FORWARD key events from a
      // mouse's back/forward buttons (synthesizeButtonKeys).
      if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
          e.button.windowID == id &&
          (e.button.button == SDL_BUTTON_X1 || e.button.button == SDL_BUTTON_X2)) {
        out.type = BIFROST_WINDOW_KEY;
        out.key_code = e.button.button == SDL_BUTTON_X1 ? 4 : 125;  // AKEYCODE_BACK/FORWARD
        out.action = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 0 : 1;
        return 1;
      }
      bool pointer = false;
      int pixel_w = 0, pixel_h = 0, logical_w = 0, logical_h = 0;
      if (!sdl_.GetWindowSizeInPixels(window_, &pixel_w, &pixel_h) ||
          !sdl_.GetWindowSize(window_, &logical_w, &logical_h) ||
          pixel_w <= 0 || pixel_h <= 0 || logical_w <= 0 || logical_h <= 0) continue;
      // Touchscreens already report SDL finger events; skip the mouse events
      // SDL synthesizes from them so one finger is not delivered twice.
      if ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
          e.button.windowID == id && e.button.which != SDL_TOUCH_MOUSEID &&
          (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT)) {
        // The left button is the primary finger, which follows the cursor.
        // The right button is a second finger that stays where it went down,
        // so two-finger controls (steer while holding boost) work by mouse.
        out.x = e.button.x; out.y = e.button.y;
        out.pointer_id = e.button.button == SDL_BUTTON_LEFT ? kMouseFinger : kMouseSecondFinger;
        out.action = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 0 : 1;
        out.pressure = out.action == 0 ? 1 : 0; pointer = true;
        if (out.action == 0) mouse_fingers_ |= 1u << out.pointer_id;
        else mouse_fingers_ &= ~(1u << out.pointer_id);
      } else if (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.windowID == id &&
                 e.motion.which != SDL_TOUCH_MOUSEID) {
        out.x = e.motion.x; out.y = e.motion.y;
        // A release this window never saw (focus moved away) would leave
        // that finger down; release it once motion shows the button up.
        const bool right_held = e.motion.state & SDL_BUTTON_RMASK;
        if ((mouse_fingers_ & (1u << kMouseSecondFinger)) && !right_held) {
          mouse_fingers_ &= ~(1u << kMouseSecondFinger);
          out.pointer_id = kMouseSecondFinger;
          out.action = 1; pointer = true;
        } else {
          out.pointer_id = kMouseFinger;
          out.pressure = (e.motion.state & SDL_BUTTON_LMASK) ? 1 : 0;
          if (!out.pressure && (mouse_fingers_ & (1u << kMouseFinger))) {
            mouse_fingers_ &= ~(1u << kMouseFinger);
            out.action = 1;
          } else {
            out.action = out.pressure ? 2 : 7;
          }
          pointer = true;
        }
      } else if ((e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_UP ||
                  e.type == SDL_EVENT_FINGER_MOTION) && e.tfinger.windowID == id) {
        out.type = BIFROST_WINDOW_POINTER;
        out.x = e.tfinger.x * pixel_w; out.y = e.tfinger.y * pixel_h;
        out.pressure = e.tfinger.pressure;
        out.pointer_id = kFirstTouchFinger + static_cast<int32_t>(
            e.tfinger.fingerID % static_cast<SDL_FingerID>(INT32_MAX - kFirstTouchFinger));
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
  // Read the window's pixel size and refresh the ratio its layers scale by.
  bool update_ratio(int32_t &pixel_w, int32_t &pixel_h) {
    int logical_w = 0, logical_h = 0;
    pixel_w = pixel_h = 0;
    if (!sdl_.GetWindowSizeInPixels(window_, &pixel_w, &pixel_h) ||
        !sdl_.GetWindowSize(window_, &logical_w, &logical_h) ||
        pixel_w <= 0 || pixel_h <= 0 || logical_w <= 0 || logical_h <= 0)
      return false;
    ratio_->store(double(pixel_w) / logical_w);
    return true;
  }
  void clear_binding() noexcept {
    if (wl_window_) wl_fns_->destroy(wl_window_);
    wl_window_ = nullptr;
    wl_fns_.reset();
    binding_ = {};
  }
  // Managed SurfaceView layers are subsurfaces; nothing else draws into the
  // SDL window's own surface then (its wl_egl_window may exist but stays
  // unused), and Wayland maps the window only once that surface has a buffer.
  void refresh_backdrop() {
    auto props = sdl_.GetWindowProperties(window_);
    auto display = static_cast<wl_display*>(sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    auto surface = static_cast<wl_surface*>(sdl_.GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    int pixel_w = 0, pixel_h = 0;
    if (display && surface && sdl_.GetWindowSizeInPixels(window_, &pixel_w, &pixel_h))
      backdrop_.show(display, surface, pixel_w, pixel_h);
  }
  // Host pointer keys: the two mouse fingers, then touchscreen fingers.
  // AndroidSurfaceManager assigns the Android pointer ids.
  static constexpr int32_t kMouseFinger = 0, kMouseSecondFinger = 1, kFirstTouchFinger = 2;
  uint32_t mouse_fingers_ = 0; // bit per mouse finger reported down
  // Desktop text input (set_text_input). Payloads wait here for poll_text;
  // text_reports_ counts TEXT events still owed beyond the one just returned.
  bool text_active_ = false;
  std::deque<AndroidWindowText> text_;
  unsigned text_reports_ = 0;
  // Keys whose press became text: their release is dropped with them.
  std::vector<SDL_Scancode> text_keys_;
  // BIFROST_TEXT_SCRIPT (tests): events pushed into SDL's own queue once text
  // input starts, so they take the real path through poll(). A step is its
  // items pushed together (a key and the text it produced stay adjacent).
  struct ScriptItem { int op; SDL_Scancode scancode; std::string text; };  // 0 down, 1 up, 2 text, 3 edit
  std::vector<std::vector<ScriptItem>> script_;
  size_t script_next_ = 0;
  int64_t script_due_ms_ = 0;
  static int64_t monotonic_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  void load_script() {
    script_.clear();
    script_next_ = 0;
    script_due_ms_ = monotonic_ms() + 100;
    const std::string &source = dbg().text_script;
    for (size_t at = 0; !source.empty() && at <= source.size();) {
      size_t end = source.find(';', at);
      if (end == std::string::npos) end = source.size();
      const std::string text = source.substr(at, end - at);
      std::vector<ScriptItem> step;
      for (size_t item_at = 0; item_at <= text.size();) {
        size_t item_end = text.find(',', item_at);
        if (item_end == std::string::npos) item_end = text.size();
        const std::string item = text.substr(item_at, item_end - item_at);
        const size_t colon = item.find(':');
        if (colon != std::string::npos) {
          const std::string op = item.substr(0, colon), arg = item.substr(colon + 1);
          if (op == "down" || op == "up") {
            const SDL_Scancode code = sdl_.GetScancodeFromName(arg.c_str());
            if (code != SDL_SCANCODE_UNKNOWN) step.push_back({op == "down" ? 0 : 1, code, {}});
          } else if (op == "text") {
            step.push_back({2, SDL_SCANCODE_UNKNOWN, arg});
          } else if (op == "edit") {
            step.push_back({3, SDL_SCANCODE_UNKNOWN, arg});
          }
        }
        item_at = item_end + 1;
      }
      if (!step.empty()) script_.push_back(std::move(step));
      at = end + 1;
    }
  }
  void run_script() {
    if (!text_active_ || script_next_ >= script_.size() || monotonic_ms() < script_due_ms_) return;
    const SDL_WindowID id = sdl_.GetWindowID(window_);
    for (const ScriptItem &item : script_[script_next_]) {
      SDL_Event e{};
      if (item.op <= 1) {
        e.type = item.op == 0 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.windowID = id;
        e.key.scancode = item.scancode;
        e.key.key = sdl_.GetKeyFromScancode(item.scancode, SDL_KMOD_NONE, false);
        e.key.down = item.op == 0;
      } else if (item.op == 2) {
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = id;
        e.text.text = item.text.c_str();
      } else {
        e.type = SDL_EVENT_TEXT_EDITING;
        e.edit.windowID = id;
        e.edit.text = item.text.c_str();
      }
      e.common.timestamp = sdl_.GetTicksNS();
      sdl_.PushEvent(&e);
    }
    ++script_next_;
    script_due_ms_ = monotonic_ms() + 100;
  }
  // Queues `s` as payloads of at most BIFROST_WINDOW_TEXT_MAX bytes, cut between
  // characters; a composition keeps only its first payload. Returns the count.
  unsigned queue_text(uint32_t kind, const char *s) {
    const size_t total = s ? std::strlen(s) : 0;
    size_t at = 0;
    unsigned count = 0;
    do {
      if (text_.size() >= 256) break;  // the guest is not draining: bound the queue
      size_t n = std::min<size_t>(total - at, BIFROST_WINDOW_TEXT_MAX);
      while (n > 0 && at + n < total && (static_cast<unsigned char>(s[at + n]) & 0xC0) == 0x80) --n;
      if (n == 0 && total > at) break;  // no valid character fits a payload
      AndroidWindowText text{};
      text.kind = kind;
      text.length = static_cast<uint32_t>(n);
      if (n) std::memcpy(text.utf8, s + at, n);
      text_.push_back(text);
      ++count;
      at += n;
    } while (at < total && kind == BIFROST_WINDOW_TEXT_COMMIT);
    return count;
  }
  // While the desktop's text input is active, a key press that produced text
  // reaches the editor as that text, not also as a key event (each character
  // would type twice). SDL queues the text right behind its key, so a press
  // followed by text is consumed with its release. Shortcuts and non-printing
  // keys produce no text and stay key events.
  bool key_became_text(const SDL_Event &e) {
    auto held = std::find(text_keys_.begin(), text_keys_.end(), e.key.scancode);
    if (e.type == SDL_EVENT_KEY_UP) {
      if (held == text_keys_.end()) return false;
      text_keys_.erase(held);
      return true;
    }
    if (!text_active_) return false;
    SDL_Event next;
    if (sdl_.PeepEvents(&next, 1, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) != 1 ||
        (next.type != SDL_EVENT_TEXT_INPUT && next.type != SDL_EVENT_TEXT_EDITING))
      return false;
    if (held == text_keys_.end()) {
      if (text_keys_.size() >= 32) text_keys_.erase(text_keys_.begin());
      text_keys_.push_back(e.key.scancode);
    }
    return true;
  }
  Sdl3 sdl_;
  SDL_Window *window_ = nullptr;
  std::unique_ptr<WlEglFns> wl_fns_;
  void *wl_window_ = nullptr;
  AndroidWindowBinding binding_;
  std::map<uint32_t, std::unique_ptr<PixelLayer>> popups_;
  WaylandBackdrop backdrop_;
  PixelRatio ratio_ = std::make_shared<std::atomic<double>>(1.0);
  int32_t width_ = 800, height_ = 600;
  int64_t last_pump_log_ms_ = 0;
#endif
};
} // namespace
std::unique_ptr<AndroidWindowBackend> make_sdl_android_window_backend() {
  return std::make_unique<SdlAndroidWindowBackend>();
}
} // namespace frost
