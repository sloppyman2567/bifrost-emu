// yggdrasil/memfd_node.hpp — synthetic content served from a memfd.
//
// MemfdNode wraps a memfd_create'd host fd. The content is written
// once at creation time and the guest reads/seeks through it like a
// regular file.
//
// v1.4.5-alpha improvement: optional lazy regeneration. A MemfdNode
// can be constructed with a "regenerator" callback instead of static
// content. When the guest seeks to offset 0 (or opens the file fresh),
// the regenerator is called to produce fresh content — useful for
// /proc/self/maps, /proc/self/status, etc. that should reflect live
// state on each read. The old behavior (write-once at open time) is
// still available via the static-content constructor; the lazy path
// is opt-in.
#pragma once
#include "yggdrasil/node.hpp"
#include <functional>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace arm64emu::yggdrasil {
class MemfdNode : public Node {
public:
    // Create a memfd with the given name and static content. Returns
    // nullptr on failure (with errno set). This is the original
    // behavior — content is written once and never refreshed.
    static std::unique_ptr<MemfdNode> create(const std::string& name,
                                             const std::string& content,
                                             int flags);
    // Create a memfd with a regenerator callback. The callback is
    // invoked on construction AND whenever the guest seeks to offset 0
    // (SEEK_SET 0). The memfd is rewritten with the new content each
    // time. Useful for /proc files that should reflect live state.
    //
    // Returns nullptr on failure (with errno set).
    static std::unique_ptr<MemfdNode> create_lazy(
        const std::string& name,
        std::function<std::string()> regenerator,
        int flags);
    ~MemfdNode() override { if (fd_ >= 0) ::close(fd_); }
    ssize_t read(uint64_t off, void* buf, size_t n) override;
    ssize_t write(uint64_t off, const void* buf, size_t n) override;
    ssize_t lseek(int64_t off, int whence) override;
    int     fstat(struct stat* st) override;
    bool    seekable() const override { return true; }
    int     flags() const override { return flags_; }
    int host_fd() const override { return fd_; }
private:
    MemfdNode(int fd, int flags,
              std::function<std::string()> regenerator = nullptr)
        : fd_(fd), flags_(flags), regenerator_(std::move(regenerator)) {}
    // Rewrite the memfd content from the regenerator. Called on
    // construction (by create_lazy) and on SEEK_SET 0. No-op if no
    // regenerator is set (static content). Returns 0 on success,
    // -errno on failure.
    int regenerate_();
    int fd_;
    int flags_;
    std::function<std::string()> regenerator_;
};
} // namespace arm64emu::yggdrasil
