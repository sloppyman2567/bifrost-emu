// vfs/vfs_table.h — declarations for the FdTable helpers used by syscalls.
//
// (FdTable itself is declared in vfs.h. This header exposes the
//  VNode concrete subclasses for syscalls/fs.cpp and ioctls.cpp so they
//  can downcast when needed — e.g. to call host_fd() on a HostVNode.)
#pragma once

#include "vfs/vfs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace arm64emu {

// ── HostVNode: wraps a host fd ─────────────────────────────────────────
class HostVNode : public VNode {
public:
    explicit HostVNode(int fd, int flags) : fd_(fd), flags_(flags) {}
    ~HostVNode() override { if (fd_ >= 0) ::close(fd_); }

    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int     fstat(struct stat* st) override;
    bool    seekable() const override { return true; }
    int     flags() const override { return flags_; }

    int host_fd() const { return fd_; }

private:
    int fd_;
    int flags_;
};

// ── MemfdVNode: synthetic content served from a memfd ──────────────────
// Used by /proc/* virtual files. Content is written once at creation
// time; the guest reads/seek through it like a regular file.
class MemfdVNode : public VNode {
public:
    // Create a memfd with the given name and content. Returns nullptr
    // on failure (with errno set).
    static std::unique_ptr<MemfdVNode> create(const std::string& name,
                                              const std::string& content,
                                              int flags);

    ~MemfdVNode() override { if (fd_ >= 0) ::close(fd_); }

    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int     fstat(struct stat* st) override;
    bool    seekable() const override { return true; }
    int     flags() const override { return flags_; }

    int host_fd() const { return fd_; }

private:
    MemfdVNode(int fd, int flags) : fd_(fd), flags_(flags) {}
    int fd_;
    int flags_;
};

// ── StdioVNode: stdin/stdout/stderr wrapper ────────────────────────────
// Backed by the host's fd 0/1/2. Doesn't own the fd (won't close it).
class StdioVNode : public VNode {
public:
    explicit StdioVNode(int host_fd, int flags)
        : fd_(host_fd), flags_(flags) {}

    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int     fstat(struct stat* st) override;
    bool    seekable() const override { return false; }
    int     flags() const override { return flags_; }

    int host_fd() const { return fd_; }

private:
    int fd_;     // 0, 1, or 2
    int flags_;
};

// ── FbVNode: /dev/fb0 wrapper ──────────────────────────────────────────
// Delegates to GraphicsBackend::open_dev_fb0() which returns a memfd
// backed by the framebuffer memory. lseek/fstat work on the memfd.
class FbVNode : public VNode {
public:
    explicit FbVNode(int guest_fd, int flags, GraphicsBackend* gfx)
        : fd_(guest_fd), flags_(flags), gfx_(gfx) {}
    ~FbVNode() override;

    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int     fstat(struct stat* st) override;
    bool    seekable() const override { return true; }
    int     flags() const override { return flags_; }

    int host_fd() const { return fd_; }

private:
    int fd_;
    int flags_;
    GraphicsBackend* gfx_;
};

} // namespace arm64emu
