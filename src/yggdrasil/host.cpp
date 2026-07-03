// yggdrasil/host.cpp — host passthrough + BIFROST_ROOT path remap.
//
// If BIFROST_ROOT is set in the environment, guest absolute paths
// starting with "/" (except /proc and /dev which are virtual) are
// remapped to "$BIFROST_ROOT/<path>". This lets the user sandbox guest
// file I/O to a specific directory.
//
// If BIFROST_ROOT is not set, the path is passed through to the host
// openat() unchanged.
#include "yggdrasil/yggdrasil.hpp"
#include "yggdrasil/host_node.hpp"
#include "core/memory.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace arm64emu::yggdrasil {

// Helper: remap a guest path to a host path using BIFROST_ROOT.
std::string map_guest_path(const std::string& guest_path) {
    const char* bifrost_root = getenv("BIFROST_ROOT");
    if (!bifrost_root || !bifrost_root[0]) return guest_path;
    if (guest_path.empty() || guest_path[0] != '/') return guest_path;
    if (guest_path.substr(0, 5) == "/proc") return guest_path;
    if (guest_path.substr(0, 4) == "/dev") return guest_path;
    std::string root(bifrost_root);
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    return root + guest_path;
}

// Static method exposed via Yggdrasil so syscall handlers don't need to
// friend host.cpp's free function.
std::string Yggdrasil::remap_path(const std::string& guest_path) {
    return map_guest_path(guest_path);
}

std::unique_ptr<Node> Yggdrasil::open_host(const std::string& guest_path,
                                           int flags, mode_t mode, int* err_out) {
    std::string host_path = map_guest_path(guest_path);
    int fd = ::openat(AT_FDCWD, host_path.c_str(), flags, mode);
    if (fd < 0) {
        *err_out = -errno;
        return nullptr;
    }
    return std::make_unique<HostNode>(fd, flags);
}

// ── HostNode method implementations ───────────────────────────────────
// host_fd() is inline in host_node.hpp

ssize_t HostNode::read(uint64_t off, void* buf, size_t n) {
    if (off != UINT64_MAX) {
        ssize_t r = ::lseek(fd_, off, SEEK_SET);
        if (r < 0) return -errno;
    }
    ssize_t r = ::read(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t HostNode::write(uint64_t off, const void* buf, size_t n) {
    if (off != UINT64_MAX) {
        ssize_t r = ::lseek(fd_, off, SEEK_SET);
        if (r < 0) return -errno;
    }
    ssize_t r = ::write(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t HostNode::lseek(int64_t off, int whence) {
    ssize_t r = ::lseek(fd_, off, whence);
    return r < 0 ? -errno : r;
}

int HostNode::fstat(struct stat* st) {
    int r = ::fstat(fd_, st);
    return r < 0 ? -errno : 0;
}

// HostNode::ioctl — handle terminal and FIONREAD ioctls by forwarding
// to the host fd. v1.4.5-alpha: moved here from ioctls.cpp's heuristic
// dispatch. The syscall layer now calls node->ioctl() for every ioctl;
// if it returns Node::IOCTL_NOT_HANDLED, the syscall layer returns
// -ENOTTY.
int HostNode::ioctl(uint32_t request, uint64_t argp, Memory& mem) {
    // TIOCGWINSZ — query terminal window size.
    if (request == 0x5413 /*TIOCGWINSZ*/) {
        struct winsize ws;
        int r = ::ioctl(fd_, TIOCGWINSZ, &ws);
        if (r < 0) return -errno;
        mem.write(argp, &ws, sizeof(ws));
        return 0;
    }
    // TCGETS — read terminal attributes.
    if (request == 0x5401 /*TCGETS*/) {
        struct termios t;
        int r = ::ioctl(fd_, TCGETS, &t);
        if (r < 0) return -errno;
        mem.write(argp, &t, sizeof(t));
        return 0;
    }
    // TCSETS / TCSETSW / TCSETSF — write terminal attributes.
    if (request == 0x5402 /*TCSETS*/ || request == 0x5403 /*TCSETSW*/ ||
        request == 0x5404 /*TCSETSF*/) {
        struct termios t;
        mem.read(argp, &t, sizeof(t));
        int r = ::ioctl(fd_, static_cast<unsigned long>(request), &t);
        if (r < 0) return -errno;
        return 0;
    }
    // FIONREAD — bytes available to read without blocking.
    if (request == 0x541B /*FIONREAD*/) {
        int n = 0;
        int r = ::ioctl(fd_, FIONREAD, &n);
        if (r < 0) return -errno;
        mem.store<int32_t>(argp, n);
        return 0;
    }
    // Anything else: pass through to the host. The host will return
    // -ENOTTY for unrecognized ioctls, which is what we want.
    int r = ::ioctl(fd_, static_cast<unsigned long>(request),
                    reinterpret_cast<void*>(argp));
    if (r < 0) return -errno;
    return r;
}

} // namespace arm64emu::yggdrasil
