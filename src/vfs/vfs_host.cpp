// vfs/vfs_host.cpp — host passthrough + BIFROST_ROOT path remap.
//
// If BIFROST_ROOT is set in the environment, guest absolute paths
// starting with "/" (except /proc and /dev which are virtual) are
// remapped to "$BIFROST_ROOT/<path>". This lets the user sandbox guest
// file I/O to a specific directory.
//
// If BIFROST_ROOT is not set, the path is passed through to the host
// openat() unchanged.
#include "vfs/vfs.h"
#include "vfs/vfs_table.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace arm64emu {

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

// Static method exposed via VFS so syscall handlers don't need to friend
// vfs_host.cpp's free function.
std::string VFS::remap_path(const std::string& guest_path) {
    return map_guest_path(guest_path);
}

std::unique_ptr<VNode> VFS::open_host(const std::string& guest_path,
                                      int flags, mode_t mode, int* err_out) {
    std::string host_path = map_guest_path(guest_path);
    int fd = ::openat(AT_FDCWD, host_path.c_str(), flags, mode);
    if (fd < 0) {
        *err_out = -errno;
        return nullptr;
    }
    return std::make_unique<HostVNode>(fd, flags);
}

// ── HostVNode method implementations ───────────────────────────────────
// host_fd() is inline in vfs_table.h

ssize_t HostVNode::read(uint64_t off, void* buf, size_t n) {
    if (off != UINT64_MAX) {
        ssize_t r = ::lseek(fd_, off, SEEK_SET);
        if (r < 0) return -errno;
    }
    ssize_t r = ::read(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t HostVNode::write(uint64_t off, const void* buf, size_t n) {
    if (off != UINT64_MAX) {
        ssize_t r = ::lseek(fd_, off, SEEK_SET);
        if (r < 0) return -errno;
    }
    ssize_t r = ::write(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t HostVNode::lseek(int64_t off, int whence) {
    ssize_t r = ::lseek(fd_, off, whence);
    return r < 0 ? -errno : r;
}

int HostVNode::fstat(struct stat* st) {
    int r = ::fstat(fd_, st);
    return r < 0 ? -errno : 0;
}

} // namespace arm64emu
