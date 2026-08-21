// frost/android_surface.hpp — Android NativeActivity surface layer (v1).
//
// ANativeWindow shims for NativeActivity-style guest apps. A guest surface
// handle is a fake opaque pointer (0xA9xx… range) that the EGL interception
// in GraphicThunk::dispatch substitutes for the real host native window
// (X11 Window) when the guest calls eglCreateWindowSurface.
//
// v1 scope: SINGLE surface backed by the DisplayProxy host SDL window.
// fromSurface ignores the JNIEnv/jobject pair (opaque to us). Touch input
// and SurfaceTexture are future phases — the handle layout leaves room.
#ifndef FROST_ANDROID_SURFACE_HPP_
#define FROST_ANDROID_SURFACE_HPP_

#include <cstdint>

namespace frost {

class AndroidSurfaceManager {
public:
    // Guest-visible shim handle range. Unmistakable, never a real pointer.
    static constexpr uint64_t kHandleBase = 0xA90000000000ULL;
    // ANativeWindow format codes (android/rect.h + nativewindow.h).
    static constexpr int32_t kFormatRGBA8888 = 1;
    static constexpr int32_t kFormatRGBX8888 = 2;

    static AndroidSurfaceManager& instance();

    // Called by DisplayThunk after the DisplayProxy window exists.
    // sdl_window is the host SDL_Window*.
    void set_host_sdl_window(void* sdl_window);
    bool ready() const { return sdl_window_ != nullptr; }

    // ANativeWindow_fromSurface: returns the singleton shim handle (0 on
    // failure — matches Android returning NULL without a valid surface).
    uint64_t from_surface();
    static bool is_shim(uint64_t handle) {
        return handle >= kHandleBase && handle <= kHandleBase + 0xFFFF;
    }
    // Refcounting is bookkeeping-only in v1 (the shim never dies).
    void acquire(uint64_t handle);
    void release(uint64_t handle);

    int32_t width() const { return width_; }
    int32_t height() const { return height_; }
    int32_t format() const { return format_; }
    // ANativeWindow_setBuffersGeometry: 0 = keep current dimension.
    int32_t set_buffers_geometry(int32_t w, int32_t h, int32_t fmt);

    // The host-side native window value host EGL expects for
    // eglCreateWindowSurface: X11 Window id on an X11-backed display,
    // wl_egl_window* on Wayland. 0 when unknown — callers must fail the
    // surface creation.
    uint64_t host_native_window();
    // The native display the guest's eglGetDisplay(EGL_DEFAULT_DISPLAY)
    // should wrap so it lands on the SAME connection as the surface:
    // wl_display* on Wayland, nullptr on X11 (default display is fine).
    void* native_display() const { return wl_display_; }

private:
    AndroidSurfaceManager() = default;
    void* sdl_window_ = nullptr;
    uint64_t host_native_window_ = 0;
    bool host_native_window_resolved_ = false;
    void* wl_egl_window_ = nullptr;  // Wayland: live wl_egl_window*
    void* wl_display_ = nullptr;     // Wayland: SDL's wl_display connection
    uint64_t surface_handle_ = 0;   // 0 = not created yet
    int refs_ = 0;
    int32_t width_ = 800;
    int32_t height_ = 600;
    int32_t format_ = kFormatRGBA8888;
};

}  // namespace frost

#endif  // FROST_ANDROID_SURFACE_HPP_
