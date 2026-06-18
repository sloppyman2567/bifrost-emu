// graphics.cpp — Graphics backend implementation (stub for 1.3.0-beta.1).
//
// In 1.3.0-beta.1, the framebuffer is just a chunk of memory that the
// guest can mmap and write to. No actual rendering happens — we're
// headless. The SDL2 window support is planned for 1.3.0.
//
// The framebuffer is backed by a memfd_create'd file descriptor, so
// the guest can mmap it directly (our mmap syscall handler recognizes
// the fd and maps it into guest address space).

#include "graphics.hpp"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

namespace arm64emu {

GraphicsBackend::~GraphicsBackend() {
    if (fb_data_) {
        munmap(fb_data_, size());
        fb_data_ = nullptr;
    }
    if (fb_fd_ >= 0) {
        close(fb_fd_);
        fb_fd_ = -1;
    }
}

uint64_t GraphicsBackend::init(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;

    // Create a memfd to back the framebuffer. The guest will mmap this
    // fd and write pixels directly into it.
    fb_fd_ = memfd_create("bifrost-fb0", 0);
    if (fb_fd_ < 0) {
        fprintf(stderr, "[graphics] memfd_create failed: %s\n", strerror(errno));
        return 0;
    }

    // Set the size of the memfd
    size_t fb_size = size();
    if (ftruncate(fb_fd_, fb_size) < 0) {
        fprintf(stderr, "[graphics] ftruncate failed: %s\n", strerror(errno));
        close(fb_fd_);
        fb_fd_ = -1;
        return 0;
    }

    // Map it into our address space too (for refresh()/debugging)
    fb_data_ = mmap(nullptr, fb_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fb_fd_, 0);
    if (fb_data_ == MAP_FAILED) {
        fprintf(stderr, "[graphics] mmap failed: %s\n", strerror(errno));
        fb_data_ = nullptr;
        close(fb_fd_);
        fb_fd_ = -1;
        return 0;
    }

    // Clear to black
    memset(fb_data_, 0, fb_size);

    // Guest address will be assigned by the Emulator when it maps the
    // framebuffer into guest address space. For now, return 0 — the
    // Emulator will call open_dev_fb0() and handle the mmap itself.
    fb_addr_ = 0;
    fprintf(stderr, "[graphics] framebuffer initialized: %ux%u, %zu bytes, fd=%d\n",
            width_, height_, fb_size, fb_fd_);
    return fb_addr_;
}

void GraphicsBackend::refresh() {
    // Stub: no rendering in 1.3.0-beta.1.
    // In 1.3.0, this will:
    //   1. Copy fb_data_ to an SDL2 surface
    //   2. SDL_UpdateTexture / SDL_RenderCopy / SDL_RenderPresent
    //   3. Poll for SDL2 events (mouse, keyboard) and forward to guest
}

int GraphicsBackend::open_dev_fb0() {
    if (fb_fd_ < 0) {
        // Auto-init with default 640x480 if not already initialized
        init(640, 480);
    }
    // Return a dup'd fd so the guest can close it independently
    return dup(fb_fd_);
}

} // namespace arm64emu
