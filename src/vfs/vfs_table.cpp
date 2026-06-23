// vfs/vfs_table.cpp — VNode concrete subclass implementations.
//
// HostVNode, FbVNode methods are implemented in their respective resolver
// files (vfs_host.cpp, vfs_dev.cpp). This file holds:
//   - MemfdVNode::create + method implementations (used by /proc/*)
//   - StdioVNode method implementations (used for fd 0/1/2 default entries)
//   - AudioVNode method implementations (used by /dev/dsp, /dev/snd)
#include "vfs/vfs_table.h"
#include "audio/audio.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace arm64emu {

// ── MemfdVNode ──────────────────────────────────────────────────────────
std::unique_ptr<MemfdVNode> MemfdVNode::create(const std::string& name,
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
    return std::unique_ptr<MemfdVNode>(new MemfdVNode(fd, flags));
}

ssize_t MemfdVNode::read(uint64_t off, void* buf, size_t n) {
    if (off != UINT64_MAX) ::lseek(fd_, off, SEEK_SET);
    ssize_t r = ::read(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t MemfdVNode::write(uint64_t off, const void* buf, size_t n) {
    if (off != UINT64_MAX) ::lseek(fd_, off, SEEK_SET);
    ssize_t r = ::write(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t MemfdVNode::lseek(int64_t off, int whence) {
    ssize_t r = ::lseek(fd_, off, whence);
    return r < 0 ? -errno : r;
}

int MemfdVNode::fstat(struct stat* st) {
    int r = ::fstat(fd_, st);
    return r < 0 ? -errno : 0;
}

// ── StdioVNode ──────────────────────────────────────────────────────────
ssize_t StdioVNode::read(uint64_t /*off*/, void* buf, size_t n) {
    ssize_t r = ::read(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t StdioVNode::write(uint64_t /*off*/, const void* buf, size_t n) {
    ssize_t r = ::write(fd_, buf, n);
    return r < 0 ? -errno : r;
}

ssize_t StdioVNode::lseek(int64_t /*off*/, int /*whence*/) {
    return -ESPIPE;  // not seekable
}

int StdioVNode::fstat(struct stat* st) {
    int r = ::fstat(fd_, st);
    return r < 0 ? -errno : 0;
}

// ── AudioVNode ──────────────────────────────────────────────────────────
AudioVNode::~AudioVNode() = default;

ssize_t AudioVNode::read(uint64_t /*off*/, void* /*buf*/, size_t /*n*/) {
    // Recording not yet supported — return 0 (EOF) immediately.
    return 0;
}

ssize_t AudioVNode::write(uint64_t /*off*/, const void* buf, size_t n) {
    if (!audio_) return static_cast<ssize_t>(n);  // no backend — discard
    return audio_->write(static_cast<const uint8_t*>(buf), n);
}

ssize_t AudioVNode::lseek(int64_t /*off*/, int /*whence*/) {
    return -ESPIPE;  // not seekable
}

int AudioVNode::fstat(struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;  // character device
    st->st_size = 0;
    return 0;
}

} // namespace arm64emu
