// yggdrasil/stdio_node.hpp — stdin/stdout/stderr wrapper.
//
// Backed by the host's fd 0/1/2. Doesn't own the fd (won't close it).
// Not seekable (returns -ESPIPE). Handles O_NONBLOCK toggling via
// fcntl F_SETFL — v1.4.5-alpha improvement (previously O_NONBLOCK was
// silently ignored on stdio fds, breaking guest poll/select loops
// that depended on non-blocking stdin).
#pragma once
#include "yggdrasil/node.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace arm64emu::yggdrasil {
class StdioNode : public Node {
public:
    explicit StdioNode(int host_fd, int flags)
        : fd_(host_fd), flags_(flags) {}
    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int     fstat(struct stat* st) override;
    int     ioctl(uint32_t request, uint64_t argp, Memory& mem) override;
    bool    seekable() const override { return false; }
    int     flags() const override { return flags_; }
    int host_fd() const override { return fd_; }
    // fcntl F_SETFL updates our cached flags_ — used by the syscall
    // layer to honor O_NONBLOCK on stdio.
    void set_flags(int new_flags) { flags_ = new_flags; }
private:
    int fd_;     // 0, 1, or 2
    int flags_;
};
} // namespace arm64emu::yggdrasil
