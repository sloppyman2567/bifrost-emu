// yggdrasil/fb_node.hpp — /dev/fb0 wrapper.
//
// Delegates read/write/lseek/fstat to a memfd backed by the
// GraphicsBackend's framebuffer memory. Owns the framebuffer ioctls
// (FBIOGET_VSCREENINFO, FBIOGET_FSCREENINFO) — v1.4.5-alpha
// improvement that moved ioctl dispatch from the syscall layer into
// the Node itself.
#pragma once

#include "yggdrasil/node.hpp"

namespace arm64emu {
    class GraphicsBackend;
}

namespace arm64emu::yggdrasil {

class FbNode : public Node {
public:
    explicit FbNode(int guest_fd, int flags, ::arm64emu::GraphicsBackend* gfx);
    ~FbNode() override;

    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int     fstat(struct stat* st) override;
    int     ioctl(uint32_t request, uint64_t argp, ::arm64emu::Memory& mem) override;
    bool    seekable() const override { return true; }
    int     flags() const override { return flags_; }

    int host_fd() const override { return fd_; }

private:
    int fd_;
    int flags_;
    ::arm64emu::GraphicsBackend* gfx_;
};

} // namespace arm64emu::yggdrasil
