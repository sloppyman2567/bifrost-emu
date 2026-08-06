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
#include "yggdrasil/terminal_ioctls.hpp"  // shared ioctl dispatch
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
    // X11 clients need to read the host X auth cookie. If the guest opens
    // the exact path the host XAUTHORITY points at, pass it through
    // unchanged — it lives in the host session dir (e.g. /run/user/...),
    // outside the BIFROST_ROOT sandbox. Without this, xcb connects to the
    // X socket but the server rejects it with "Authorization required".
    const char* xauth = getenv("XAUTHORITY");
    if (xauth && xauth[0] == '/' && guest_path == xauth) return guest_path;
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
    // on x86_64 hosts (the kernel rejects it). But on real AArch64 Linux,
    // O_DIRECT is silently ignored for directories. BusyBox's `ls` opens
    // directories with O_RDONLY|O_DIRECTORY|O_NONBLOCK|O_CLOEXEC (and
    // sometimes O_DIRECT from certain code paths), so we need to handle
    // this difference. If the open fails with EINVAL and O_DIRECT is set,
    // retry without O_DIRECT.
    if (fd < 0 && errno == EINVAL && (flags & O_DIRECT)) {
        fd = ::openat(AT_FDCWD, host_path.c_str(), flags & ~O_DIRECT, mode);
    }
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
// dispatch. refactored to use the shared
// dispatch_terminal_ioctl() helper (was duplicated in StdioNode).
int HostNode::ioctl(uint32_t request, uint64_t argp, Memory& mem) {
    int r = dispatch_terminal_ioctl(fd_, request, argp, mem);
    if (r != Node::IOCTL_NOT_HANDLED) return r;
    // Anything else: pass through to the host. The host will return
    // -ENOTTY for unrecognized ioctls, which is what we want.
    return pass_through_ioctl(fd_, request, argp);
}
} // namespace arm64emu::yggdrasil
