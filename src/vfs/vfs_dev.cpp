// vfs/vfs_dev.cpp — DevFS: /dev/{null,zero,urandom,random,tty,fb0,stdin,stdout,stderr}.
//
// Most device paths are passed through to the host (which already has
// them). /dev/fb0 is special — it returns a memfd backed by the
// GraphicsBackend's framebuffer memory.
//
// /dev/stdin, /dev/stdout, /dev/stderr are dup()'d from the host's
// 0/1/2 so close(guest_fd) doesn't close the host's stdio.
#include "vfs/vfs.h"
#include "vfs/vfs_table.h"
#include "graphics.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace arm64emu {

std::unique_ptr<VNode> VFS::open_devfs(const std::string& path,
                                       int flags, mode_t mode, int* err_out) {
    // /dev/null, /dev/zero, /dev/urandom, /dev/random → host passthrough
    if (path == "/dev/null" || path == "/dev/zero" ||
        path == "/dev/urandom" || path == "/dev/random") {
        int fd = ::openat(AT_FDCWD, path.c_str(), flags, mode);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostVNode>(fd, flags);
    }

    // /dev/fb0 → virtual framebuffer (memfd-backed via GraphicsBackend)
    if (path == "/dev/fb0") {
        if (!gfx_) { *err_out = -ENODEV; return nullptr; }
        int guest_fd = gfx_->open_dev_fb0();
        if (guest_fd < 0) { *err_out = -ENODEV; return nullptr; }
        return std::make_unique<FbVNode>(guest_fd, flags, gfx_);
    }

    // /dev/tty → open the host's controlling terminal.
    if (path == "/dev/tty") {
        int fd = ::openat(AT_FDCWD, "/dev/tty", flags, mode);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostVNode>(fd, flags);
    }

    // /dev/stdin, /dev/stdout, /dev/stderr → dup the host fd.
    if (path == "/dev/stdin") {
        int r = ::dup(0);
        if (r < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostVNode>(r, flags);
    }
    if (path == "/dev/stdout") {
        int r = ::dup(1);
        if (r < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostVNode>(r, flags);
    }
    if (path == "/dev/stderr") {
        int r = ::dup(2);
        if (r < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostVNode>(r, flags);
    }

    // /dev/snd → audio device (OSS-style /dev/dsp passthrough)
    if (path == "/dev/snd" || path == "/dev/dsp" || path == "/dev/audio") {
        int fd = ::openat(AT_FDCWD, "/dev/dsp", flags, mode);
        if (fd < 0) {
            // No real audio device — return /dev/null as a sink so writes succeed
            fd = ::openat(AT_FDCWD, "/dev/null", flags, mode);
            if (fd < 0) { *err_out = -errno; return nullptr; }
        }
        return std::make_unique<HostVNode>(fd, flags);
    }

    // /dev/ptmx → pseudo-terminal master (passthrough for interactive apps)
    if (path == "/dev/ptmx") {
        int fd = ::openat(AT_FDCWD, "/dev/ptmx", flags, mode);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostVNode>(fd, flags);
    }

    // /dev/pts/N → pseudo-terminal slaves (passthrough)
    if (path.rfind("/dev/pts/", 0) == 0) {
        int fd = ::openat(AT_FDCWD, path.c_str(), flags, mode);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostVNode>(fd, flags);
    }

    *err_out = 0;
    return nullptr;  // not a /dev path we handle
}

// ── FbVNode method implementations ─────────────────────────────────────
FbVNode::~FbVNode() {
    if (fd_ >= 0) ::close(fd_);
}

ssize_t FbVNode::read(uint64_t off, void* buf, size_t n) {
    if (off != UINT64_MAX) ::lseek(fd_, off, SEEK_SET);
    ssize_t r = ::read(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t FbVNode::write(uint64_t off, const void* buf, size_t n) {
    if (off != UINT64_MAX) ::lseek(fd_, off, SEEK_SET);
    ssize_t r = ::write(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t FbVNode::lseek(int64_t off, int whence) {
    ssize_t r = ::lseek(fd_, off, whence);
    return r < 0 ? -errno : r;
}

int FbVNode::fstat(struct stat* st) {
    int r = ::fstat(fd_, st);
    return r < 0 ? -errno : 0;
}

} // namespace arm64emu
