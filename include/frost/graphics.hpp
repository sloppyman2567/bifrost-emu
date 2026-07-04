// frost/graphics.hpp — FrostGraphics: graphics backend for bifrost-emu.
//
// Provides a virtual framebuffer device (/dev/fb0) that guest programs
// can mmap and write pixels to. The framebuffer is backed by a memfd
// (so the guest can mmap it directly into its address space).
//
// Two display backends are supported, selectable at BUILD time:
//
//   1. HEADLESS (default) — fb memory is allocated and mmap-able,
//      FBIOGET_VSCREENINFO/FSCREENINFO ioctls work, and `refresh()`
//      dumps the framebuffer to a PPM file. No third-party deps.
//
//   2. SDL2 (build with `make USE_SDL2=1`) — opens a real SDL2 window,
//      pushes the framebuffer to it on `refresh()`. Useful for running
//      graphical guest programs interactively. SDL2 dev headers must
//      be available on the build host.
//
// v1.4.5-alpha (Turn 36): renamed from `GraphicsBackend` to
// `FrostGraphics` to match the FrostJIT naming convention. The class
// is now in `include/frost/graphics.hpp` and the implementation in
// `src/frost_graphics/`. `GraphicsBackend` is kept as a typedef alias
// for backward compatibility with existing call sites — new code
// should use `FrostGraphics`.
//
// v1.4.5-alpha (Turn 36): EXPERIMENTAL graphic API thunking. The
// `FrostGraphics::thunk()` method returns a `GraphicThunk*` that the
// emulator's dynamic linker uses to intercept guest `dlsym` calls for
// `libGL.so` / `libEGL.so` / `libSDL2.so` / `libGLESv2.so` and forward
// them to the host's equivalent libraries. This lets guest programs
// that link against OpenGL/EGL/SDL2 run by thunking every call to the
// host's implementation — no GPU emulation, just marshalling. See
// `thunk.cpp` and the `GraphicThunk` class below.
//
// v1.4.5-alpha (Turn 38): SMARTER SDL2 INIT + INPUT EVENTS.
//   - SDL2 is now initialized lazily on first init() or poll_events()
//     call, not eagerly when the framebuffer is opened. Headless
//     programs that never display the fb won't open an SDL2 window.
//   - Window resize is now handled: the SDL2 renderer auto-scales the
//     fb texture to the window size, so guest programs that hardcode a
//     640x480 fb still display correctly in a larger window.
//   - Input events (keyboard, mouse) are captured by the SDL2 event
//     loop and exposed via the `FrostInput` instance owned by
//     FrostGraphics. The guest reads them via /dev/input/eventX
//     (wired through Yggdrasil DevFS).
//   - New `set_window_title()` and `set_window_size()` methods for
//     programs that want to control the window size.
//
// Usage from the emulator:
//   FrostGraphics gfx;
//   gfx.init(640, 480);          // 640x480, 32-bit BGRA
//   int fb_fd = gfx.open_dev_fb0();  // guest-visible fd (memfd-backed)
//   // ... guest mmaps fb_fd and writes pixels ...
//   gfx.refresh();               // dump to PPM (headless) or SDL present
//   gfx.dump_to_ppm("out.ppm");  // explicit dump (always available)
//
// Integration: the Emulator class owns a FrostGraphics instance and
// wires /dev/fb0 openat() and FBIOGET_* ioctls in syscalls.cpp to it.
#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

namespace arm64emu {

// Forward-declare GraphicThunk (defined in src/frost_graphics/thunk.cpp).
class GraphicThunk;
// Forward-declare FrostInput (defined in frost/input.hpp).
class FrostInput;

// Linux framebuffer ioctl numbers (from <linux/fb.h>).
// We define them here so we don't have to #include <linux/fb.h> in
// every translation unit that handles ioctls.
//
//   FBIOGET_VSCREENINFO  = 0x4600  (read variable screen info)
//   FBIOGET_FSCREENINFO  = 0x4602  (read fixed screen info)
//
// The struct layouts we expose match the kernel's struct
// fb_var_screeninfo and struct fb_fix_screeninfo (sufficient subset
// for musl/glibc fb programs). See graphics.cpp for the structs.
constexpr uint32_t FBIOGET_VSCREENINFO = 0x4600;
constexpr uint32_t FBIOGET_FSCREENINFO = 0x4602;

// ── FrostGraphics: framebuffer + (optional) window + (optional) thunk ──
//
// "FrostGraphics" matches the project's Norse/cold theme (bifrost,
// FrostJIT, Yggdrasil). The class wraps:
//   - a memfd-backed framebuffer (always available)
//   - an optional SDL2 window (build with USE_SDL2=1)
//   - an optional GraphicThunk for forwarding guest GL/EGL/SDL2 calls
//     to the host (experimental, v1.4.5-alpha)
class FrostGraphics {
public:
    // Default ctor and destructor must be out-of-line (defined in
    // graphics.cpp) because the unique_ptr<GraphicThunk> member needs
    // the full GraphicThunk type to construct/destroy, and GraphicThunk
    // is only forward-declared here.
    FrostGraphics();
    ~FrostGraphics();

    FrostGraphics(const FrostGraphics&) = delete;
    FrostGraphics& operator=(const FrostGraphics&) = delete;

    // Initialize the framebuffer with the given dimensions.
    // width x height pixels, 32 bits per pixel (BGRA/XRGB).
    // Returns true on success, false on failure (errno-style: check
    // stderr for the diagnostic).
    //
    // With SDL2 backend: also opens an SDL2 window of the requested
    // size. If SDL_Init fails, falls back to headless mode (memory
    // fb only, no window).
    bool init(uint32_t width, uint32_t height);

    // Get a guest-visible file descriptor (memfd-backed) for the
    // framebuffer. The guest can mmap this fd and write pixels
    // directly. The fd is dup'd on each call so the guest can close
    // it independently. Auto-inits to 640x480 if init() wasn't called.
    int open_dev_fb0();

    // Query the framebuffer dimensions.
    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    size_t   size()   const { return static_cast<size_t>(width_) * height_ * 4; }
    int      fd()     const { return fb_fd_; }
    bool     ready()  const { return fb_fd_ >= 0; }

    // Returns true if the given fd refers to the framebuffer (either
    // the original memfd or a dup of it). Used by the syscall layer's
    // mmap handler to detect when the guest is mmap'ing the fb. Uses
    // fstat to compare file identity (st_ino + st_dev), so it works
    // across dup'd fds.
    bool owns_fd(int fd) const;

    // Dump the framebuffer to a PPM file. PPM (P6) is a trivial
    // uncompressed image format readable by ImageMagick, GIMP, etc.
    // Returns true on success, false on failure.
    //
    // The framebuffer is 32-bit BGRA; PPM is 24-bit RGB. We drop the
    // alpha and swap B/R on the fly.
    //
    // NOTE: call sync_from() first if the guest has written to its
    // mmap'd copy of the framebuffer — the host's fb_data_ does NOT
    // auto-update when the guest writes to its own pages (the
    // emulator's mmap handler allocates separate pages for the guest
    // and copies the initial fd contents in, but does not propagate
    // writes back).
    bool dump_to_ppm(const std::string& path) const;

    // Sync the host-side fb_data_ from a guest-side source buffer.
    // The Emulator calls this before dump_to_ppm() to capture the
    // guest's latest writes. `src` must point to at least size()
    // bytes.
    void sync_from(const void* src);

    // Record/query the guest address where the framebuffer is mapped.
    // Set by the syscall layer when the guest mmaps the fb fd; used
    // by the Emulator to find the guest's framebuffer pages for
    // sync_from().
    void     set_guest_fb_addr(uint64_t addr) { guest_fb_addr_ = addr; }
    uint64_t guest_fb_addr() const { return guest_fb_addr_; }

    // Refresh the display. In headless mode, this dumps the framebuffer
    // to the default PPM path. In SDL2 mode, this pushes the framebuffer
    // to the SDL2 window (creating one if needed) and pumps the SDL
    // event loop so the window remains responsive.
    void refresh();

    // Pump the SDL2 event loop without presenting. No-op in headless mode.
    // Returns false if the user has closed the SDL2 window (caller may
    // decide to terminate the guest); true otherwise.
    bool poll_events();

    // Set the default dump path for refresh(). Defaults to
    // "bifrost-fb.ppm" in the current working directory.
    void set_dump_path(const std::string& path) { dump_path_ = path; }
    const std::string& dump_path() const { return dump_path_; }

    // Handle a framebuffer ioctl. Returns 0 on success, -errno on
    // failure. Writes the response struct to the given guest buffer
    // (interpreted as a raw pointer — the caller is responsible for
    // ensuring the guest buffer is writable and large enough).
    //
    // Supported ioctls:
    //   FBIOGET_VSCREENINFO (0x4600) — write struct fb_var_screeninfo
    //   FBIOGET_FSCREENINFO (0x4602) — write struct fb_fix_screeninfo
    int ioctl(uint32_t request, void* guest_buf);

    // ── EXPERIMENTAL: Graphic API thunking (v1.4.5-alpha, Turn 36) ──
    //
    // Returns the GraphicThunk instance owned by this FrostGraphics.
    // The thunk intercepts guest dlsym calls for libGL/libEGL/libSDL2/
    // libGLESv2 and forwards them to the host's equivalent libraries.
    // The thunk is lazily created on first call — if the guest never
    // uses GL/EGL/SDL2, no host libraries are loaded.
    //
    // The thunk is EXPERIMENTAL:
    //   - Only a subset of GL/EGL/SDL2 entry points are thunked (the
    //     most common ones: glClear, glBegin, glEnd, glVertex3f,
    //     eglGetDisplay, eglInitialize, SDL_Init, SDL_CreateWindow,
    //     SDL_GL_SwapWindow, etc.).
    //   - Pointer arguments that are guest addresses (e.g. vertex
    //     arrays, shader source strings) are translated via the
    //     emulator's Memory — the thunk needs an Emulator& to do this.
    //   - The thunk is opt-in: set BIFROST_THUNK_GRAPHICS=1 in the
    //     environment to enable. Without it, the thunk returns
    //     nullptr for every dlsym, so the guest falls back to its
    //     own software rendering (or fails gracefully).
    //
    // See src/frost_graphics/thunk.cpp for the implementation and the
    // list of supported entry points.
    GraphicThunk* thunk();

    // ── Input events (v1.4.5-alpha, Turn 38) ──────────────────────
    // Returns the FrostInput instance owned by this FrostGraphics.
    // The input backend captures keyboard/mouse events from the SDL2
    // window and exposes them as Linux input_event records. The guest
    // reads them via /dev/input/eventX (wired through Yggdrasil DevFS).
    //
    // In headless builds (no SDL2), the input instance exists but is
    // always empty — reads return 0 (EOF). This keeps the API uniform
    // regardless of build configuration.
    FrostInput* input();

    // ── Window management (v1.4.5-alpha, Turn 38) ─────────────────
    // Set the SDL2 window title. No-op in headless mode. Safe to call
    // before init() — the title is cached and applied when the window
    // is created.
    void set_window_title(const std::string& title);

    // Resize the SDL2 window. No-op in headless mode. The framebuffer
    // dimensions are NOT changed — the SDL2 renderer auto-scales the
    // fb texture to the new window size.
    void set_window_size(uint32_t width, uint32_t height);

    // Whether the SDL2 window is currently open (diagnostic).
    bool has_window() const;

private:
    uint32_t      width_         = 0;
    uint32_t      height_        = 0;
    int           fb_fd_         = -1;   // memfd backing the framebuffer
    void*         fb_data_       = nullptr;
    std::string   dump_path_     = "bifrost-fb.ppm";
    uint64_t      guest_fb_addr_ = 0;    // guest address of the fb mmap

    // SDL2 backend state (opaque to header — defined in graphics.cpp).
    // Stored as void* to avoid pulling SDL2.h into this header.
    void*         sdl_state_     = nullptr;  // struct SDLWindowState*
    bool          sdl_init_done_ = false;    // SDL_Init succeeded
    bool          sdl_window_open_ = false;  // SDL_CreateWindow succeeded
    std::string   window_title_  = "bifrost-emu /dev/fb0";
    uint32_t      window_width_  = 0;        // 0 = use fb dimensions
    uint32_t      window_height_ = 0;        // 0 = use fb dimensions

    // GraphicThunk instance (lazily created by thunk()). Stored as
    // unique_ptr to avoid pulling the GraphicThunk definition into
    // this header.
    std::unique_ptr<GraphicThunk> thunk_;

    // FrostInput instance (created in the constructor, always present).
    // Stored as unique_ptr to avoid pulling FrostInput's full definition
    // into this header — FrostInput is forward-declared above.
    std::unique_ptr<FrostInput> input_;
};

// Backward-compatibility alias. Existing call sites use `GraphicsBackend`;
// new code should use `FrostGraphics`. The alias is in the public
// header so external consumers (libbifrost users) don't break.
using GraphicsBackend = FrostGraphics;

} // namespace arm64emu
