// yggdrasil/yggdrasil.cpp — top-level resolver + FdTable + read_path helper.
//
// Yggdrasil::open() consults procfs, devfs, and host passthrough in order.
// Each sub-resolver lives in its own file:
//   procfs → yggdrasil/procfs.cpp
//   devfs  → yggdrasil/devfs.cpp
//   host   → yggdrasil/host.cpp (path remap + openat passthrough)
#include "yggdrasil/yggdrasil.hpp"
#include "yggdrasil/stdio_node.hpp"
#include "core/memory.h"

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

std::unique_ptr<Node> Yggdrasil::open(const std::string& guest_path,
                                      int flags, mode_t mode, int* err_out) {
    // 1. ProcFS
    auto node = open_procfs(guest_path, flags, mode, err_out);
    if (node || *err_out != 0) return node;

    // 2. DevFS
    node = open_devfs(guest_path, flags, mode, err_out);
    if (node || *err_out != 0) return node;

    // 3. Host passthrough (with BIFROST_ROOT remap)
    return open_host(guest_path, flags, mode, err_out);
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
    std::lock_guard<std::mutex> g(mu_);
    auto it = table_.find(fd);
    if (it == table_.end()) return -EBADF;
    table_.erase(it);
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
    std::lock_guard<std::mutex> g(mu_);
    auto it = table_.find(fd);
    if (it == table_.end()) return -EBADF;
    if (fd == new_fd) return new_fd;
    // Close new_fd if open
    auto existing = table_.find(new_fd);
    if (existing != table_.end()) table_.erase(existing);
    table_[new_fd] = it->second;
    return new_fd;
}

void FdTable::close_range(int first, int last) {
    if (first > last) return;
    std::lock_guard<std::mutex> g(mu_);
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
        table_.erase(fd);
    }
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
