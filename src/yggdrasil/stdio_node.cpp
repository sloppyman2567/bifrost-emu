// yggdrasil/stdio_node.cpp — StdioNode implementation.
//
// v1.4.5-alpha: added ioctl() handling (delegates to host fd for
// TIOCGWINSZ / TCGETS / TCSETS / FIONREAD). Previously ioctls.cpp
// dispatched on the request code with a big if-else chain and guessed
// the fd type — now the Node owns its ioctls.
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
//
// redirected to a file or pipe), TCGETS ioctl fails on the host,
// causing glibc's isatty() to return 0. glibc then fully buffers
// stdout, and on exit the buffer is lost (our exit_group doesn't
// run fini_array/atexit handlers). By intercepting TCGETS and
// returning a fake termios struct, we make glibc think stdout is a
// TTY, so it line-buffers (flushes on every \n). This makes printf
// output appear immediately without needing exit() to flush.
//
// Set BIFROST_NO_FAKE_TTY=1 to disable this (e.g., for testing
// fully-buffered behavior).
int StdioNode::ioctl(uint32_t request, uint64_t argp, Memory& mem) {
    // Check if this is a TCGETS request and the host fd is not a TTY.
    // If so, return a fake termios struct so isatty() returns 1.
    static const bool fake_tty = (getenv("BIFROST_NO_FAKE_TTY") == nullptr);
    if (fake_tty && request == 0x5401) {  // TCGETS
        // First try the real ioctl. If it succeeds, the host fd IS a
        // TTY — return the real termios.
        struct termios t;
        if (::ioctl(fd_, TCGETS, &t) == 0) {
            try { mem.write(argp, &t, sizeof(t)); } catch (...) {}
            return 0;
        }
        // Host fd is NOT a TTY. Return a fake termios struct so
        // glibc's isatty() returns 1 and it line-buffers stdout.
        memset(&t, 0, sizeof(t));
        t.c_iflag = ICRNL | IXON;
        t.c_oflag = OPOST | ONLCR;
        t.c_cflag = B38400 | CS8 | CREAD;
        t.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;
        t.c_cc[VINTR] = 0x03;    // ^C
        t.c_cc[VQUIT] = 0x1C;    // ^Backslash
        t.c_cc[VERASE] = 0x7F;   // DEL
        t.c_cc[VKILL] = 0x15;    // ^U
        t.c_cc[VEOF] = 0x04;     // ^D
        t.c_cc[VTIME] = 0;
        t.c_cc[VMIN] = 1;
        t.c_cc[VSWTC] = 0;
        t.c_cc[VSTART] = 0x11;   // ^Q
        t.c_cc[VSTOP] = 0x13;    // ^S
        t.c_cc[VSUSP] = 0x1A;    // ^Z
        t.c_cc[VEOL] = 0;
        t.c_cc[VREPRINT] = 0x12; // ^R
        t.c_cc[VDISCARD] = 0x0F; // ^O
        t.c_cc[VWERASE] = 0x17;  // ^W
        t.c_cc[VLNEXT] = 0x16;   // ^V
        t.c_cc[VEOL2] = 0;
        try { mem.write(argp, &t, sizeof(t)); } catch (...) {}
        return 0;
    }
    int r = dispatch_terminal_ioctl(fd_, request, argp, mem);
    if (r != Node::IOCTL_NOT_HANDLED) return r;
    // Pass-through for anything else.
    return pass_through_ioctl(fd_, request, argp);
}
} // namespace arm64emu::yggdrasil
