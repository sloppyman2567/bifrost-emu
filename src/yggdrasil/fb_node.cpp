// yggdrasil/fb_node.cpp — FbNode implementation.
//
// v1.4.5-alpha: moved framebuffer ioctl handling (FBIOGET_VSCREENINFO,
// FBIOGET_FSCREENINFO) from ioctls.cpp into FbNode::ioctl(). The
// syscall layer now dispatches via node->ioctl() instead of guessing
// fd type from the request code.
#include "yggdrasil/fb_node.hpp"
#include "core/memory.h"
#include "frost/graphics.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/stat.h>
#include <unistd.h>
namespace arm64emu::yggdrasil {
FbNode::FbNode(int guest_fd, int flags, ::arm64emu::FrostGraphics* gfx)
    : fd_(guest_fd), flags_(flags), gfx_(gfx) {}
FbNode::~FbNode() {
    if (fd_ >= 0) ::close(fd_);
}
ssize_t FbNode::read(uint64_t off, void* buf, size_t n) {
    if (off != UINT64_MAX) ::lseek(fd_, off, SEEK_SET);
    ssize_t r = ::read(fd_, buf, n);
    return r < 0 ? -errno : r;
}
ssize_t FbNode::write(uint64_t off, const void* buf, size_t n) {
    if (off != UINT64_MAX) ::lseek(fd_, off, SEEK_SET);
    ssize_t r = ::write(fd_, buf, n);
    return r < 0 ? -errno : r;
}
ssize_t FbNode::lseek(int64_t off, int whence) {
    ssize_t r = ::lseek(fd_, off, whence);
    return r < 0 ? -errno : r;
}
int FbNode::fstat(struct stat* st) {
    int r = ::fstat(fd_, st);
    return r < 0 ? -errno : 0;
}
// FbNode::ioctl — handle FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO by
// delegating to GraphicsBackend. Other ioctl requests fall through to
// IOCTL_NOT_HANDLED so the syscall layer returns -ENOTTY.
int FbNode::ioctl(uint32_t request, uint64_t argp, ::arm64emu::Memory& mem) {
    if (request == FBIOGET_VSCREENINFO || request == FBIOGET_FSCREENINFO) {
        if (!gfx_ || !gfx_->ready()) return -ENODEV;
        // The two structs are different sizes; use the larger as buffer.
        char host_buf[192];
        memset(host_buf, 0, sizeof(host_buf));
        int r = gfx_->ioctl(request, host_buf);
        if (r < 0) return r;
        // sizeof(struct fb_var_screeninfo) = 160
        // sizeof(struct fb_fix_screeninfo) = 80 (LP64)
        size_t out_sz = (request == FBIOGET_VSCREENINFO) ? 160 : 80;
        mem.write(argp, host_buf, out_sz);
        return 0;
    }
    return Node::IOCTL_NOT_HANDLED;
}
} // namespace arm64emu::yggdrasil
