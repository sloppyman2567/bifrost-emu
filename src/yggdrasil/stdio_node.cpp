// yggdrasil/stdio_node.cpp — StdioNode implementation.
//
// v1.4.5-alpha: added ioctl() handling (delegates to host fd for
// TIOCGWINSZ / TCGETS / TCSETS / FIONREAD). Previously ioctls.cpp
// dispatched on the request code with a big if-else chain and guessed
// the fd type — now the Node owns its ioctls.
#include "yggdrasil/stdio_node.hpp"
#include "core/memory.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace arm64emu::yggdrasil {

ssize_t StdioNode::read(uint64_t /*off*/, void* buf, size_t n) {
    ssize_t r = ::read(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t StdioNode::write(uint64_t /*off*/, const void* buf, size_t n) {
    ssize_t r = ::write(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t StdioNode::lseek(int64_t /*off*/, int /*whence*/) {
    return -ESPIPE;  // not seekable
}

int StdioNode::fstat(struct stat* st) {
    int r = ::fstat(fd_, st);
    return r < 0 ? -errno : 0;
}

// StdioNode::ioctl — same set as HostNode (terminal + FIONREAD).
// Forward to the underlying host fd (0/1/2).
int StdioNode::ioctl(uint32_t request, uint64_t argp, Memory& mem) {
    if (request == 0x5413 /*TIOCGWINSZ*/) {
        struct winsize ws;
        int r = ::ioctl(fd_, TIOCGWINSZ, &ws);
        if (r < 0) return -errno;
        mem.write(argp, &ws, sizeof(ws));
        return 0;
    }
    if (request == 0x5401 /*TCGETS*/) {
        struct termios t;
        int r = ::ioctl(fd_, TCGETS, &t);
        if (r < 0) return -errno;
        mem.write(argp, &t, sizeof(t));
        return 0;
    }
    if (request == 0x5402 || request == 0x5403 || request == 0x5404) {
        struct termios t;
        mem.read(argp, &t, sizeof(t));
        int r = ::ioctl(fd_, static_cast<unsigned long>(request), &t);
        if (r < 0) return -errno;
        return 0;
    }
    if (request == 0x541B /*FIONREAD*/) {
        int n = 0;
        int r = ::ioctl(fd_, FIONREAD, &n);
        if (r < 0) return -errno;
        mem.store<int32_t>(argp, n);
        return 0;
    }
    // Pass-through for anything else.
    int r = ::ioctl(fd_, static_cast<unsigned long>(request),
                    reinterpret_cast<void*>(argp));
    if (r < 0) return -errno;
    return r;
}

} // namespace arm64emu::yggdrasil
