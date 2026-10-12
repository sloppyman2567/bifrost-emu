// VFS.cpp — top-level resolver + FdTable + read_path helper.
//
// Yggdrasil::open() consults procfs, sysfs, devfs, and host
// passthrough in order. Each sub-resolver lives in its own file:
//   procfs → yggdrasil/procfs.cpp
//   sysfs  → yggdrasil/sysfs.cpp
//   devfs  → yggdrasil/devfs.cpp
//   host   → yggdrasil/host.cpp (path remap + openat passthrough)
#include "System/VFS/VFS.hpp"
#include "System/VFS/Nodes/StdioNode.hpp"
#include "System/VFS/Nodes/HostNode.hpp"
#include "System/Memory/Memory.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
namespace arm64emu::yggdrasil {
// ── Yggdrasil::read_path ──────────────────────────────────────────────
std::string Yggdrasil::read_path(Memory& mem, uint64_t addr) {
    if (addr == 0) return std::string();
    std::string s;
    s.reserve(64);
    for (uint64_t off = 0; off < 4096; off++) {
        uint8_t c = 0;
        try { c = mem.load<uint8_t>(addr + off); } catch (...) { break; }
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}
Yggdrasil::Yggdrasil() = default;
std::string Yggdrasil::canonical_proc_path(const std::string& path) const {
    const std::string own = "/proc/" + std::to_string(self_pid_);
    if (path.compare(0, own.size(), own) != 0) return path;
    if (path.size() != own.size() && path[own.size()] != '/') return path;
    return "/proc/self" + path.substr(own.size());
}
std::unique_ptr<Node> Yggdrasil::open(const std::string& requested_path,
                                      int flags, mode_t mode, int* err_out) {
    const std::string guest_path = canonical_proc_path(requested_path);
    // 1. ProcFS
    auto node = open_procfs(guest_path, flags, mode, err_out);
    if (node || *err_out != 0) return node;
    // 2. SysFS — always synthetic (before DevFS and the host
    // passthrough): a rootfs has no /sys, and a host /sys would
    // report the host's core count, contradicting sched_getaffinity.
    node = open_sysfs(guest_path, flags, mode, err_out);
    if (node || *err_out != 0) return node;
    // 3. DevFS
    node = open_devfs(guest_path, flags, mode, err_out);
    if (node || *err_out != 0) return node;
    // 4. Host passthrough (with BIFROST_ROOT remap)
    return open_host(guest_path, flags, mode, err_out);
}
// ── Yggdrasil::open_at: explicit-dirfd open ──────────────────────────
// Absolute paths ignore dirfd per POSIX (same as open()). Relative paths
// resolve under host_dirfd, which must be a real host dir fd from
// resolve_dirfd (AT_FDCWD keeps legacy host-cwd behavior via open()).
// ProcFS/DevFS only match absolute paths, so relative opens go straight
// to the confined host helper.
std::unique_ptr<Node> Yggdrasil::open_at(int host_dirfd,
                                         const std::string& guest_path,
                                         int flags, mode_t mode,
                                         int* err_out) {
    if (!guest_path.empty() && guest_path[0] == '/')
        return open(guest_path, flags, mode, err_out);
    int fd = open_host_at(host_dirfd, guest_path, flags, mode);
    if (fd < 0 && errno == EINVAL && (flags & O_DIRECT)) {
        fd = open_host_at(host_dirfd, guest_path, flags & ~O_DIRECT, mode);
    }
    if (fd < 0) {
        *err_out = -errno;
        return nullptr;
    }
    return std::make_unique<HostNode>(fd, flags);
}
// ── FdTable ───────────────────────────────────────────────────────────
FdTable::FdTable() {
    // Pre-populate fd 0/1/2 with StdioNode so close(0/1/2) doesn't crash.
    // (constructor runs on a single thread; no lock needed here.)
    table_[0] = std::make_shared<StdioNode>(0, O_RDONLY);
    table_[1] = std::make_shared<StdioNode>(1, O_WRONLY);
    table_[2] = std::make_shared<StdioNode>(2, O_WRONLY);
}
int FdTable::allocate(std::shared_ptr<Node> node) {
    // POSIX: open()/dup() return the *lowest* available fd. The old
    // implementation used a monotonic `next_fd_` counter and never
    // recycled closed fds, so a guest that closed stdin/stdout/stderr
    // and re-opened a file expected to get fd 0/1/2 (a common shell
    // idiom) but got a higher fd instead. Fix: scan from 0 upward and
    // return the first fd not in table_. (0/1/2 are pre-populated, so
    // in the common case the scan returns 3 immediately.)
    std::lock_guard<std::mutex> g(mu_);
    int fd = 0;
    while (table_.count(fd)) fd++;
    if (node->owns_guest_mappings()) {
        mapping_devices_.push_back(node);
        has_mapping_devices_.store(true, std::memory_order_release);
    }
    table_[fd] = std::move(node);
    return fd;
}
std::shared_ptr<Node> FdTable::get(int fd) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = table_.find(fd);
    if (it == table_.end()) return nullptr;
    return it->second;
}
int FdTable::close(int fd) {
    // Device destructors may acquire their domain locks. Binder transfer
    // takes a domain lock before consulting this table: never destroy a
    // device while holding mu_, or concurrent close/transfer can deadlock.
    std::shared_ptr<Node> closing;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = table_.find(fd);
        if (it == table_.end()) return -EBADF;
        closing = std::move(it->second);
        table_.erase(it);
    }
    return 0;
}
int FdTable::dup(int fd, int min_fd) {
    // Lock once for both lookup and allocate to avoid a TOCTOU race
    // where another thread closes/reuses the fd between get() and
    // allocate(). We inline the allocation logic under the same lock.
    std::lock_guard<std::mutex> g(mu_);
    auto it = table_.find(fd);
    if (it == table_.end()) return -EBADF;
    auto node = it->second;
    int new_fd = std::max(min_fd, 0);
    while (table_.count(new_fd)) new_fd++;
    table_[new_fd] = node;
    return new_fd;
}
int FdTable::dup2(int fd, int new_fd) {
    if (new_fd < 0) return -EBADF;
    std::shared_ptr<Node> closing;
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = table_.find(fd);
        if (it == table_.end()) return -EBADF;
        if (fd == new_fd) return new_fd;
        auto source = it->second;
        auto& slot = table_[new_fd];
        closing = std::move(slot);
        slot = std::move(source);
    }
    return new_fd;
}
void FdTable::close_range(int first, int last) {
    if (first > last) return;
    std::vector<std::shared_ptr<Node>> closing;
    std::unique_lock<std::mutex> g(mu_);
    // Iterate only over the open fds in the range, not every integer.
    // This makes close_range(0, INT_MAX) O(open_fds), not O(2^31).
    // std::unordered_map has no lower_bound, so we collect-then-erase.
    std::vector<int> to_close;
    to_close.reserve(table_.size());
    for (const auto& kv : table_) {
        if (kv.first >= first && kv.first <= last) {
            to_close.push_back(kv.first);
        }
    }
    for (int fd : to_close) {
        closing.push_back(std::move(table_.at(fd)));
        table_.erase(fd);
    }
    g.unlock();
}
bool FdTable::is_open(int fd) const {
    std::lock_guard<std::mutex> g(mu_);
    return table_.count(fd) != 0;
}
size_t FdTable::size() const {
    std::lock_guard<std::mutex> g(mu_);
    return table_.size();
}
} // namespace arm64emu::yggdrasil

namespace arm64emu::yggdrasil {
std::vector<std::shared_ptr<Node>> FdTable::mapping_devices_snapshot() {
    std::vector<std::shared_ptr<Node>> nodes;
    if (!has_mapping_devices_.load(std::memory_order_acquire)) return nodes;
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = mapping_devices_.begin(); it != mapping_devices_.end();) {
        if (auto node = it->lock()) { nodes.push_back(std::move(node)); ++it; }
        else it = mapping_devices_.erase(it);
    }
    if (mapping_devices_.empty()) has_mapping_devices_.store(false, std::memory_order_release);
    return nodes;
}
void FdTable::invalidate_device_mappings(uint64_t addr, uint64_t size) {
    for (auto& node : mapping_devices_snapshot()) node->invalidate_mapping(addr, size);
}
int FdTable::protect_device_mappings(uint64_t addr, uint64_t size, uint64_t prot) {
    for (auto& node : mapping_devices_snapshot()) {
        int error = node->protect_mapping(addr, size, prot); if (error) return error;
    }
    return 0;
}
}
