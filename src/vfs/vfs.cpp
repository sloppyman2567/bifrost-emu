// vfs/vfs.cpp — VFS top-level resolver + FdTable + read_path helper.
//
// VFS::open() consults procfs, devfs, and host passthrough in order.
// Each sub-resolver lives in its own file:
//   procfs → vfs_procfs.cpp (NOT in the user's spec, so folded into vfs.cpp)
//   devfs  → vfs_devfs.cpp
//   host   → vfs_host.cpp (path remap + openat passthrough)
//
// The user's directory listing has vfs.cpp + vfs_host.cpp + vfs_dev.cpp
// (no separate procfs file), so procfs content lives inline in vfs.cpp.
#include "vfs/vfs.h"
#include "vfs/vfs_table.h"
#include "core/memory.h"
#include "graphics.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace arm64emu {

// ── VFS::read_path ─────────────────────────────────────────────────────
std::string VFS::read_path(Memory& mem, uint64_t addr) {
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

VFS::VFS() = default;

std::unique_ptr<VNode> VFS::open(const std::string& guest_path,
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

// ── ProcFS: synthetic /proc/* content ──────────────────────────────────
// Each virtual file is a small string baked at open() time and served
// from a MemfdVNode. To add a new /proc file, add an `else if` here.
std::unique_ptr<VNode> VFS::open_procfs(const std::string& path,
                                        int flags, mode_t /*mode*/, int* err_out) {
    auto serve = [flags](const std::string& content) -> std::unique_ptr<VNode> {
        return MemfdVNode::create("bifrost-procfs", content, flags);
    };

    // /proc/self/exe → symlink to the ELF path
    if (path == "/proc/self/exe" || path == "/proc/self/exe/") {
        return serve(elf_path_);
    }
    // /proc/self/cmdline → argv[0]\0argv[1]\0...
    if (path == "/proc/self/cmdline") {
        std::string cmdline;
        for (auto& a : argv_) { cmdline += a; cmdline.push_back('\0'); }
        if (cmdline.empty()) cmdline = elf_path_ + '\0';
        return serve(cmdline);
    }
    // /proc/self/maps → basic memory map
    if (path == "/proc/self/maps") {
        std::string maps;
        maps += "00400000-004bf000 r-xp 00000000 00:00 0\n";
        maps += "004bf000-004ce000 r--p 00000000 00:00 0\n";
        maps += "004ce000-004df000 rw-p 00000000 00:00 0\n";
        maps += "5000000000-5001000000 rw-p 00000000 00:00 0\n";
        maps += "7fff000000-8000000000 rw-p 00000000 00:00 0 [stack]\n";
        return serve(maps);
    }
    // /proc/self/status → basic process info
    if (path == "/proc/self/status") {
        std::string s;
        s += "Name:\tbifrost-emu\n";
        s += "State:\tR (running)\n";
        s += "Tgid:\t1\n";
        s += "Pid:\t1\n";
        s += "PPid:\t0\n";
        s += "Uid:\t0\t0\t0\t0\n";
        s += "Gid:\t0\t0\t0\t0\n";
        s += "VmSize:\t  8192 kB\n";
        s += "VmRSS:\t  4096 kB\n";
        s += "Threads:\t1\n";
        return serve(s);
    }
    // /proc/meminfo
    if (path == "/proc/meminfo") {
        std::string s;
        s += "MemTotal:       16777216 kB\n";
        s += "MemFree:         8388608 kB\n";
        s += "MemAvailable:   12582912 kB\n";
        s += "Buffers:               0 kB\n";
        s += "Cached:          4194304 kB\n";
        return serve(s);
    }
    // /proc/cpuinfo
    if (path == "/proc/cpuinfo") {
        std::string s;
        s += "processor\t: 0\n";
        s += "BogoMIPS\t: 100.00\n";
        s += "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics\n";
        s += "CPU implementer\t: 0x41\n";
        s += "CPU architecture: 8\n";
        return serve(s);
    }
    // /proc/self/auxv → empty (we provide auxv on the stack)
    if (path == "/proc/self/auxv") return serve(std::string());
    // /proc/self/environ → just PATH
    if (path == "/proc/self/environ") {
        return serve(std::string("PATH=/bin:/usr/bin\0", 16));
    }
    // /proc/version
    if (path == "/proc/version") {
        return serve(std::string("Linux version 6.5.0 (bifrost-emu) (gcc) #1 SMP\n"));
    }
    // /proc/sys/kernel/osrelease
    if (path == "/proc/sys/kernel/osrelease") {
        return serve(std::string("6.5.0\n"));
    }

    *err_out = 0;
    return nullptr;
}

// ── DevFS: /dev/null, /dev/zero, /dev/urandom, /dev/tty, /dev/fb0, stdio ─
// (Implemented in vfs_devfs.cpp — vfs_dev.cpp in the user's spec.)
// Definitions live in vfs_dev.cpp (open_devfs) and vfs_host.cpp (open_host).

// ── FdTable ────────────────────────────────────────────────────────────
FdTable::FdTable() {
    // Pre-populate fd 0/1/2 with StdioVNode so close(0/1/2) doesn't crash.
    table_[0] = std::make_shared<StdioVNode>(0, O_RDONLY);
    table_[1] = std::make_shared<StdioVNode>(1, O_WRONLY);
    table_[2] = std::make_shared<StdioVNode>(2, O_WRONLY);
}

int FdTable::allocate(std::shared_ptr<VNode> node) {
    int fd = next_fd_++;
    while (table_.count(fd)) fd = next_fd_++;
    table_[fd] = std::move(node);
    return fd;
}

std::shared_ptr<VNode> FdTable::get(int fd) {
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

} // namespace arm64emu
