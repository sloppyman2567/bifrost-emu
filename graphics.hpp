// graphics.hpp — Graphics backend for bifrost-emu.
//
// Provides a virtual framebuffer device (/dev/fb0) that guest programs
// can mmap and write pixels to. The framebuffer is backed by a shared
// memory region; a host-side SDL2 window (when available) displays the
// contents in real-time.
//
// In 1.1.1-alpha.1, this is a STUB — the framebuffer memory is allocated
// and mmap-able, but no SDL2 window is opened (we're headless). The
// real SDL2 rendering is planned for 1.3.0.
//
// Usage from the emulator:
//   GraphicsBackend gfx;
//   gfx.init(640, 480);  // 640x480, 32-bit BGRA
//   uint64_t fb_addr = gfx.get_fb_addr();  // guest address to mmap
//   // ... guest writes pixels to fb_addr ...
//   gfx.refresh();  // call after guest writes (or on VSYNC)
//
// In 1.3.0, gfx.refresh() will push the framebuffer to an SDL2 window.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace arm64emu {

class GraphicsBackend {
public:
    GraphicsBackend() = default;
    ~GraphicsBackend();

    // Initialize the framebuffer with the given dimensions.
    // width x height pixels, 32 bits per pixel (BGRA).
    // Returns the guest address where the framebuffer is mapped.
    uint64_t init(uint32_t width, uint32_t height);

    // Get the guest address of the framebuffer (for mmap).
    uint64_t get_fb_addr() const { return fb_addr_; }

    // Get framebuffer dimensions.
    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    size_t   size()   const { return (size_t)width_ * height_ * 4; }

    // Refresh the display. In 1.1.1-alpha.1 this is a no-op.
    // In 1.3.0 this will push the framebuffer to an SDL2 window.
    void refresh();

    // Open a virtual /dev/fb0 file descriptor.
    // Returns a memfd-style fd that the guest can mmap.
    int open_dev_fb0();

private:
    uint32_t  width_   = 0;
    uint32_t  height_  = 0;
    uint64_t  fb_addr_ = 0;    // guest address of the framebuffer
    int       fb_fd_   = -1;   // memfd backing the framebuffer
    void*     fb_data_ = nullptr;
};

} // namespace arm64emu
