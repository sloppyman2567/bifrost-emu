// yggdrasil/stdio_node.cpp — StdioNode implementation.
//
// v1.4.5-alpha: added ioctl() handling (delegates to host fd for
// TIOCGWINSZ / TCGETS / TCSETS / FIONREAD). Previously ioctls.cpp
// dispatched on the request code with a big if-else chain and guessed
// the fd type — now the Node owns its ioctls.
// Turn 37: refactored to use the shared dispatch_terminal_ioctl()
// helper. Was duplicated verbatim in HostNode.
#include "yggdrasil/stdio_node.hpp"
#include "yggdrasil/terminal_ioctls.hpp"  // shared ioctl dispatch
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
// Forward to the underlying host fd (0/1/2) via the shared helper.
int StdioNode::ioctl(uint32_t request, uint64_t argp, Memory& mem) {
    int r = dispatch_terminal_ioctl(fd_, request, argp, mem);
    if (r != Node::IOCTL_NOT_HANDLED) return r;
    // Pass-through for anything else.
    return pass_through_ioctl(fd_, request, argp);
}

} // namespace arm64emu::yggdrasil
