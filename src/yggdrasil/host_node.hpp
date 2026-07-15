// yggdrasil/host_node.hpp — wraps a host file descriptor.
//
// HostNode is the "passthrough" Node: every operation is forwarded to
// the host kernel via the wrapped fd. Used for regular files, pipes,
// sockets, /dev/null, /dev/zero, /dev/urandom, /dev/tty, /dev/ptmx,
// /dev/pts/*, and any guest path that isn't claimed by procfs or devfs.
#pragma once
#include "yggdrasil/node.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace arm64emu::yggdrasil {
class HostNode : public Node {
public:
    explicit HostNode(int fd, int flags) : fd_(fd), flags_(flags) {}
    ~HostNode() override { if (fd_ >= 0) ::close(fd_); }
    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int     fstat(struct stat* st) override;
    int     ioctl(uint32_t request, uint64_t argp, Memory& mem) override;
    bool    seekable() const override { return true; }
    int     flags() const override { return flags_; }
    int host_fd() const override { return fd_; }
private:
    int fd_;
    int flags_;
};
} // namespace arm64emu::yggdrasil
