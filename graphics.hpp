// graphics.hpp — Graphics backend for bifrost-emu.
//
// Provides a virtual framebuffer device (/dev/fb0) that guest programs
// can mmap and write pixels to. The framebuffer is backed by a memfd
// (so the guest can mmap it directly into its address space).
//
// In 1.3.0-beta.4, this is a HEADLESS backend:
//   - The framebuffer memory is allocated and mmap-able.
//   - FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO ioctls are supported
//     (so guest programs can query the mode).
//   - refresh() dumps the framebuffer to a PPM file when invoked
//     (headless-friendly; no SDL2 dependency).
//   - dump_to_ppm(path) can be called explicitly to snapshot the fb.
//
// SDL2 window support is planned for 1.4.0 (requires SDL2 dev headers,
// which are not always available on the build host).
//
// Usage from the emulator:
//   GraphicsBackend gfx;
//   gfx.init(640, 480);          // 640x480, 32-bit BGRA
//   int fb_fd = gfx.open_dev_fb0();  // guest-visible fd (memfd-backed)
//   // ... guest mmaps fb_fd and writes pixels ...
//   gfx.refresh();               // dump to "bifrost-fb.ppm" by default
//   gfx.dump_to_ppm("out.ppm");  // explicit dump
//
// Integration: the Emulator class owns a GraphicsBackend instance and
// wires /dev/fb0 openat() and FBIOGET_* ioctls in syscalls.cpp to it.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace arm64emu {

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

class GraphicsBackend {
public:
    GraphicsBackend() = default;
    ~GraphicsBackend();

    GraphicsBackend(const GraphicsBackend&) = delete;
    GraphicsBackend& operator=(const GraphicsBackend&) = delete;

    // Initialize the framebuffer with the given dimensions.
    // width x height pixels, 32 bits per pixel (BGRA/XRGB).
    // Returns true on success, false on failure (errno-style: check
    // stderr for the diagnostic).
    bool init(uint32_t width, uint32_t height);

    // Get a guest-visible file descriptor (memfd-backed) for the
    // framebuffer. The guest can mmap this fd and write pixels
    // directly. The fd is dup'd on each call so the guest can close
    // it independently. Auto-inits to 640x480 if init() wasn't called.
    int open_dev_fb0();

    // Query the framebuffer dimensions.
    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    size_t   size()   const { return (size_t)width_ * height_ * 4; }
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

    // Refresh the display. In 1.3.0-beta.4 (headless), this dumps
    // the framebuffer to the default path "bifrost-fb.ppm" if the
    // framebuffer is non-empty (any non-zero pixel). In 1.4.0 (with
    // SDL2), this will push the framebuffer to an SDL2 window.
    void refresh();

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

private:
    uint32_t      width_         = 0;
    uint32_t      height_        = 0;
    int           fb_fd_         = -1;   // memfd backing the framebuffer
    void*         fb_data_       = nullptr;
    std::string   dump_path_     = "bifrost-fb.ppm";
    uint64_t      guest_fb_addr_ = 0;    // guest address of the fb mmap
};

} // namespace arm64emu
