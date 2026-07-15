// yggdrasil/memfd_node.cpp — MemfdNode implementation.
//
// v1.4.5-alpha: added lazy regeneration. A MemfdNode constructed via
// create_lazy() rewrites its memfd content from the regenerator
// callback on construction AND whenever the guest seeks to offset 0.
// This lets /proc/self/maps, /proc/self/status, etc. reflect live
// state on each open without re-creating the VNode.
#include "yggdrasil/memfd_node.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
namespace arm64emu::yggdrasil {
// ── Static-content factory ────────────────────────────────────────────
std::unique_ptr<MemfdNode> MemfdNode::create(const std::string& name,
                                             const std::string& content,
                                             int flags) {
    int fd = memfd_create(name.c_str(), 0);
    if (fd < 0) return nullptr;
    if (!content.empty()) {
        ssize_t w = ::write(fd, content.data(), content.size());
        if (w < 0 || static_cast<size_t>(w) != content.size()) {
            ::close(fd);
            return nullptr;
        }
    }
    ::lseek(fd, 0, SEEK_SET);
    return std::unique_ptr<MemfdNode>(new MemfdNode(fd, flags, /*regen=*/nullptr));
}
// ── Lazy-regenerator factory ──────────────────────────────────────────
std::unique_ptr<MemfdNode> MemfdNode::create_lazy(
    const std::string& name,
    std::function<std::string()> regenerator,
    int flags) {
    int fd = memfd_create(name.c_str(), 0);
    if (fd < 0) return nullptr;
    auto node = std::unique_ptr<MemfdNode>(new MemfdNode(fd, flags, std::move(regenerator)));
    // Populate initial content.
    if (node->regenerate_() < 0) {
        return nullptr;
    }
    return node;
}
// Rewrite the memfd content from the regenerator. Truncates the memfd
// to the new content size and resets the file position to 0.
int MemfdNode::regenerate_() {
    if (!regenerator_) return 0;  // static content
    std::string content = regenerator_();
    // Truncate to 0, write new content, reset to start.
    if (::ftruncate(fd_, 0) < 0) return -errno;
    ::lseek(fd_, 0, SEEK_SET);
    if (!content.empty()) {
        ssize_t w = ::write(fd_, content.data(), content.size());
        if (w < 0 || static_cast<size_t>(w) != content.size()) {
            return -EIO;
        }
    }
    ::lseek(fd_, 0, SEEK_SET);
    return 0;
}
ssize_t MemfdNode::read(uint64_t off, void* buf, size_t n) {
    if (off != UINT64_MAX) ::lseek(fd_, off, SEEK_SET);
    ssize_t r = ::read(fd_, buf, n);
    return r < 0 ? -errno : r;
}
ssize_t MemfdNode::write(uint64_t off, const void* buf, size_t n) {
    if (off != UINT64_MAX) ::lseek(fd_, off, SEEK_SET);
    ssize_t r = ::write(fd_, buf, n);
    return r < 0 ? -errno : r;
}
ssize_t MemfdNode::lseek(int64_t off, int whence) {
    // v1.4.5-alpha: lazy regeneration on SEEK_SET to 0. This makes
    // re-reads of /proc/self/maps etc. return fresh content without
    // the caller needing to close and re-open the fd. (Previously the
    // memfd's content was write-once at open time; re-reading returned
    // stale data for any file that should reflect live state.)
    if (off == 0 && whence == SEEK_SET) {
        if (regenerate_() == 0) return 0;
        // Fall through to host lseek on failure.
    }
    ssize_t r = ::lseek(fd_, off, whence);
    return r < 0 ? -errno : r;
}
int MemfdNode::fstat(struct stat* st) {
    int r = ::fstat(fd_, st);
    return r < 0 ? -errno : 0;
}
} // namespace arm64emu::yggdrasil
