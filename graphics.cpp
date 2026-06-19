// graphics.cpp — Headless graphics backend for bifrost-emu (1.3.0-beta.4).
//
// Provides a virtual /dev/fb0 backed by a memfd_create'd file
// descriptor. The guest mmaps the fd and writes pixels directly into
// it. The host can dump the framebuffer to a PPM file for headless
// debugging.
//
// This is intentionally SDL2-free so it builds and runs on any Linux
// host without extra dev dependencies. SDL2 window support is planned
// for 1.4.0.

#include "graphics.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <string>

namespace arm64emu {

// ── Linux framebuffer structs (sufficient subset) ──────────────────────
// These match the kernel's struct fb_var_screeninfo and struct
// fb_fix_screeninfo for the fields that real fb programs query. We
// define them here (instead of #including <linux/fb.h>) so the rest
// of the codebase doesn't need the kernel headers.
//
// Layout note: we use explicit padding to match the kernel ABI on
// aarch64/x86_64 (both are 64-bit little-endian, so the struct
// padding is identical).

struct fb_var_screeninfo {
    uint32_t xres;          // visible resolution
    uint32_t yres;
    uint32_t xres_virtual;  // virtual resolution
    uint32_t yres_virtual;
    uint32_t xoffset;       // offset from virtual to visible
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;

    struct {
        uint32_t offset;    // bitfield offset
        uint32_t length;    // bitfield length
        uint32_t msb_right; // MSB != 0?
    } red, green, blue, transp;

    uint32_t nonstd;        // non-standard pixel format
    uint32_t activate;
    uint32_t height;        // physical mm
    uint32_t width;         // physical mm
    uint32_t accel_flags;
    uint32_t pixclock;      // picoseconds
    uint32_t left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t reserved[6];
};

struct fb_fix_screeninfo {
    char     id[16];        // "bifrost_fb"
    unsigned long smem_start;  // unused (we're not on real hardware)
    uint32_t smem_len;      // total framebuffer size in bytes
    uint32_t type;          // FB_TYPE_PACKED_PIXELS = 0
    uint32_t type_aux;
    uint32_t visual;        // FB_VISUAL_TRUECOLOR = 2
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;   // bytes per scanline
    unsigned long mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

// ── Destructor ─────────────────────────────────────────────────────────
GraphicsBackend::~GraphicsBackend() {
    if (fb_data_ && fb_data_ != MAP_FAILED) {
        munmap(fb_data_, size());
    }
    if (fb_fd_ >= 0) {
        close(fb_fd_);
    }
}

// ── init() ─────────────────────────────────────────────────────────────
bool GraphicsBackend::init(uint32_t width, uint32_t height) {
    // Reject absurd sizes early. Real fb programs sometimes probe with
    // 0x0 to query capabilities; we treat that as a config error.
    if (width == 0 || height == 0 || width > 8192 || height > 8192) {
        fprintf(stderr, "[graphics] init: invalid dimensions %ux%u\n",
                width, height);
        return false;
    }

    // Clean up any prior state (idempotent init).
    if (fb_data_ && fb_data_ != MAP_FAILED) {
        munmap(fb_data_, size());
        fb_data_ = nullptr;
    }
    if (fb_fd_ >= 0) {
        close(fb_fd_);
        fb_fd_ = -1;
    }

    width_  = width;
    height_ = height;

    // Create a memfd to back the framebuffer. The guest will mmap
    // this fd and write pixels directly into it. memfd_create gives
    // us a sealed, anonymous file that's perfect for this.
    fb_fd_ = memfd_create("bifrost-fb0", 0);
    if (fb_fd_ < 0) {
        fprintf(stderr, "[graphics] memfd_create failed: %s\n",
                strerror(errno));
        return false;
    }

    // Size the memfd to the framebuffer size.
    size_t fb_size = size();
    if (ftruncate(fb_fd_, (off_t)fb_size) < 0) {
        fprintf(stderr, "[graphics] ftruncate failed: %s\n",
                strerror(errno));
        close(fb_fd_);
        fb_fd_ = -1;
        return false;
    }

    // Map it into our address space too — needed for refresh() and
    // dump_to_ppm() to read the pixels the guest wrote.
    fb_data_ = mmap(nullptr, fb_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fb_fd_, 0);
    if (fb_data_ == MAP_FAILED) {
        fprintf(stderr, "[graphics] mmap failed: %s\n", strerror(errno));
        fb_data_ = nullptr;
        close(fb_fd_);
        fb_fd_ = -1;
        return false;
    }

    // Clear to black (BGRA 0,0,0,0).
    memset(fb_data_, 0, fb_size);

    if (getenv("BIFROST_GRAPHICS_VERBOSE")) {
        fprintf(stderr,
            "[graphics] framebuffer initialized: %ux%u, %zu bytes, fd=%d\n",
            width_, height_, fb_size, fb_fd_);
    }
    return true;
}

// ── open_dev_fb0() ─────────────────────────────────────────────────────
int GraphicsBackend::open_dev_fb0() {
    if (!ready()) {
        // Auto-init with a sensible default. Real Linux fbdev defaults
        // vary (often the actual console mode); 640x480x32 is a safe
        // baseline that any fb program will accept.
        if (!init(640, 480)) {
            return -1;
        }
    }
    // dup() the fd so the guest can close() its copy without
    // tearing down the framebuffer. The guest's mmap of the dup'd
    // fd shares the same underlying memfd, so guest writes are
    // visible to refresh()/dump_to_ppm().
    int guest_fd = dup(fb_fd_);
    if (guest_fd < 0) {
        fprintf(stderr, "[graphics] dup failed: %s\n", strerror(errno));
        return -1;
    }
    return guest_fd;
}

// ── dump_to_ppm() ──────────────────────────────────────────────────────
bool GraphicsBackend::dump_to_ppm(const std::string& path) const {
    if (!fb_data_ || fb_data_ == MAP_FAILED) {
        fprintf(stderr, "[graphics] dump_to_ppm: framebuffer not mapped\n");
        return false;
    }
    if (width_ == 0 || height_ == 0) {
        fprintf(stderr, "[graphics] dump_to_ppm: zero-size framebuffer\n");
        return false;
    }

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "[graphics] dump_to_ppm: cannot open '%s': %s\n",
                path.c_str(), strerror(errno));
        return false;
    }

    // PPM P6 header: "P6\n<width> <height>\n255\n" then raw RGB bytes.
    fprintf(f, "P6\n%u %u\n255\n", width_, height_);

    // Framebuffer is 32-bit BGRA. PPM is 24-bit RGB. We drop alpha
    // and swap B/R on the fly. We use a per-row buffer to amortize
    // the fwrite calls.
    const uint8_t* src = (const uint8_t*)fb_data_;
    std::string row;
    row.resize((size_t)width_ * 3);
    for (uint32_t y = 0; y < height_; y++) {
        const uint8_t* srow = src + (size_t)y * width_ * 4;
        for (uint32_t x = 0; x < width_; x++) {
            const uint8_t* px = srow + (size_t)x * 4;
            uint8_t* out = (uint8_t*)row.data() + (size_t)x * 3;
            out[0] = px[2];  // R (fb is BGRA)
            out[1] = px[1];  // G
            out[2] = px[0];  // B
        }
        if (fwrite(row.data(), 1, row.size(), f) != row.size()) {
            fprintf(stderr, "[graphics] dump_to_ppm: write failed: %s\n",
                    strerror(errno));
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

// ── owns_fd() ──────────────────────────────────────────────────────────
bool GraphicsBackend::owns_fd(int fd) const {
    if (fd < 0 || fb_fd_ < 0) return false;
    if (fd == fb_fd_) return true;
    // Compare file identity (inode + device) via fstat. This catches
    // dup'd fds (which have different fd numbers but refer to the
    // same underlying file).
    struct stat a, b;
    if (::fstat(fd, &a) != 0) return false;
    if (::fstat(fb_fd_, &b) != 0) return false;
    return a.st_ino == b.st_ino && a.st_dev == b.st_dev;
}

// ── sync_from() ────────────────────────────────────────────────────────
void GraphicsBackend::sync_from(const void* src) {
    if (fb_data_ && fb_data_ != MAP_FAILED && src) {
        memcpy(fb_data_, src, size());
    }
}

// ── refresh() ──────────────────────────────────────────────────────────
void GraphicsBackend::refresh() {
    if (!ready()) {
        return;  // nothing to refresh
    }
    // Headless refresh: dump to PPM if the framebuffer has any
    // non-zero pixel (avoids creating empty PPM files for programs
    // that never wrote to the fb).
    //
    // In 1.4.0 (with SDL2), this will instead push the framebuffer
    // to an SDL2 window via SDL_UpdateTexture / SDL_RenderCopy /
    // SDL_RenderPresent.
    const uint8_t* p = (const uint8_t*)fb_data_;
    size_t n = size();
    bool any_pixel = false;
    for (size_t i = 0; i < n; i += 64) {  // sample every 64th byte
        if (p[i]) { any_pixel = true; break; }
    }
    if (!any_pixel && n >= 64) {
        // double-check the last few bytes in case the fb is mostly
        // zero at the start
        for (size_t i = (n > 256 ? n - 256 : 0); i < n; i++) {
            if (p[i]) { any_pixel = true; break; }
        }
    }
    if (!any_pixel) {
        return;  // all black, don't dump
    }
    if (!dump_to_ppm(dump_path_)) {
        return;
    }
    if (getenv("BIFROST_GRAPHICS_VERBOSE")) {
        fprintf(stderr, "[graphics] refresh: dumped %ux%u to '%s'\n",
                width_, height_, dump_path_.c_str());
    }
}

// ── ioctl() ────────────────────────────────────────────────────────────
int GraphicsBackend::ioctl(uint32_t request, void* guest_buf) {
    if (!ready()) {
        return -ENODEV;
    }
    if (!guest_buf) {
        return -EFAULT;
    }
    switch (request) {
        case FBIOGET_VSCREENINFO: {
            // Variable screen info: pixel format, resolution, timing.
            // We report a 32-bit BGRA format (XRGB on most hardware).
            struct fb_var_screeninfo v;
            memset(&v, 0, sizeof(v));
            v.xres = width_;
            v.yres = height_;
            v.xres_virtual = width_;
            v.yres_virtual = height_;
            v.xoffset = 0;
            v.yoffset = 0;
            v.bits_per_pixel = 32;
            v.grayscale = 0;
            v.red.offset   = 16; v.red.length   = 8; v.red.msb_right   = 0;
            v.green.offset =  8; v.green.length = 8; v.green.msb_right = 0;
            v.blue.offset  =  0; v.blue.length  = 8; v.blue.msb_right  = 0;
            v.transp.offset = 24; v.transp.length = 8; v.transp.msb_right = 0;
            v.nonstd = 0;
            v.activate = 0;
            v.height = (uint32_t)(height_ * 1000 / 96);  // fake ~96 DPI
            v.width  = (uint32_t)(width_  * 1000 / 96);
            v.accel_flags = 0;
            v.pixclock = 1000000 / (width_ * height_ * 60 / 1000000ULL);
            v.left_margin = v.right_margin = 1;
            v.upper_margin = v.lower_margin = 1;
            v.hsync_len = v.vsync_len = 1;
            v.sync = 0;
            v.vmode = 0;  // FB_VMODE_NONINTERLACED
            memcpy(guest_buf, &v, sizeof(v));
            return 0;
        }
        case FBIOGET_FSCREENINFO: {
            // Fixed screen info: framebuffer ID, type, line length.
            struct fb_fix_screeninfo fix;
            memset(&fix, 0, sizeof(fix));
            strncpy(fix.id, "bifrost_fb", sizeof(fix.id) - 1);
            fix.smem_start = 0;       // unused (no real hardware)
            fix.smem_len   = (uint32_t)size();
            fix.type       = 0;       // FB_TYPE_PACKED_PIXELS
            fix.type_aux   = 0;
            fix.visual     = 2;       // FB_VISUAL_TRUECOLOR
            fix.xpanstep   = 1;
            fix.ypanstep   = 1;
            fix.ywrapstep  = 0;
            fix.line_length = width_ * 4;  // bytes per scanline
            fix.mmio_start = 0;
            fix.mmio_len   = 0;
            fix.accel      = 0;
            fix.capabilities = 0;
            memcpy(guest_buf, &fix, sizeof(fix));
            return 0;
        }
        default:
            // Unknown framebuffer ioctl — no-op success (matches the
            // kernel behavior for FBIOGET_FSCREENINFO etc. on drivers
            // that don't implement them).
            return 0;
    }
}

} // namespace arm64emu
