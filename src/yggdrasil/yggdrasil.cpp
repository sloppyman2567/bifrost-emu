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
    int fd = 0;
    while (table_.count(fd)) fd++;
    table_[fd] = std::move(node);
    return fd;
}

std::shared_ptr<Node> FdTable::get(int fd) const {
    auto it = table_.find(fd);
    if (it == table_.end()) return nullptr;
    return it->second;
}

int FdTable::close(int fd) {
    auto it = table_.find(fd);
    if (it == table_.end()) return -EBADF;
    table_.erase(it);
    return 0;
}

int FdTable::dup(int fd) {
    auto node = get(fd);
    if (!node) return -EBADF;
    return allocate(node);
}

int FdTable::dup2(int fd, int new_fd) {
    auto node = get(fd);
    if (!node) return -EBADF;
    if (fd == new_fd) return new_fd;
    // Close new_fd if open
    auto it = table_.find(new_fd);
    if (it != table_.end()) table_.erase(it);
    table_[new_fd] = node;
    return new_fd;
}

} // namespace arm64emu::yggdrasil
