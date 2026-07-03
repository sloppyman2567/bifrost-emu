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
    // /proc/self/maps → real memory layout
    // BUGFIX: the old code hardcoded 5 fixed address ranges that did not
    // reflect the actual guest memory layout. Programs parsing
    // /proc/self/maps (debuggers, profilers, libunwind) got wrong answers.
    // The Emulator registers a maps_provider() callback that returns the
    // live allocations + brk + stack range; we format them per the
    // /proc/self/maps spec: "start-end perms offset dev inode pathname".
    if (path == "/proc/self/maps") {
        std::string maps;
        if (maps_provider_) {
            for (const auto& e : maps_provider_()) {
                // Use a stack buffer generously sized for the fixed fields
                // (44 chars) plus the label, and append via std::string.
                // The old `char line[160]` truncated long paths (deeply
                // nested .so paths, [anon:...] labels) and broke
                // libunwind/gdb parsing.
                char line[256];
                int n = snprintf(line, sizeof(line),
                    "%08llx-%08llx %s 00000000 00:00 0",
                    static_cast<unsigned long long>(e.start),
                    static_cast<unsigned long long>(e.end),
                    e.perms);
                if (n < 0) continue;
                maps.append(line, static_cast<size_t>(n));
                if (!e.label.empty()) {
                    maps += "  ";
                    maps += e.label;
                }
                maps += '\n';
            }
        }
        if (maps.empty()) {
            // Fallback if no provider is registered.
            maps += "00400000-004bf000 r-xp 00000000 00:00 0\n";
            maps += "004bf000-004ce000 r--p 00000000 00:00 0\n";
            maps += "004ce000-004df000 rw-p 00000000 00:00 0\n";
            maps += "5000000000-5001000000 rw-p 00000000 00:00 0\n";
            maps += "7fff000000-8000000000 rw-p 00000000 00:00 0 [stack]\n";
        }
        return serve(maps);
    }
    // /proc/self/status → process info. Real Linux status has ~30
    // fields; tools that parse these (ps, top, pmap, debuggers) expect
    // them all. We previously returned only 10 fields, breaking those
    // tools. Now we return the full set with sane defaults.
    if (path == "/proc/self/status") {
        // Name is the basename of argv[0], truncated to 15 chars (kernel
        // TASK_COMM_LEN). Falls back to "bifrost-emu" if no argv.
        std::string name = "bifrost-emu";
        if (!argv_.empty()) {
            auto pos = argv_[0].rfind('/');
            name = (pos == std::string::npos) ? argv_[0] : argv_[0].substr(pos + 1);
            if (name.size() > 15) name.resize(15);
        }
        // VmSize/VmRSS: query the maps provider for live memory usage
        // instead of hardcoded "8192 kB"/"4096 kB".
        uint64_t vm_total = 0;
        if (maps_provider_) {
            for (const auto& e : maps_provider_()) {
                vm_total += (e.end - e.start);
            }
        }
        if (vm_total == 0) vm_total = 8 * 1024 * 1024;  // 8 MB fallback
        char vm_sz[64], vm_rss[64];
        snprintf(vm_sz,  sizeof(vm_sz),  "%8llu kB",
                 static_cast<unsigned long long>(vm_total / 1024));
        snprintf(vm_rss, sizeof(vm_rss), "%8llu kB",
                 static_cast<unsigned long long>(vm_total / 1024));
        std::string s;
        s += "Name:\t" + name + "\n";
        s += "Umask:\t0022\n";
        s += "State:\tR (running)\n";
        s += "Tgid:\t1\n";
        s += "Ngid:\t0\n";
        s += "Pid:\t1\n";
        s += "PPid:\t0\n";
        s += "TracerPid:\t0\n";
        s += "Uid:\t0\t0\t0\t0\n";
        s += "Gid:\t0\t0\t0\t0\n";
        s += "NStgid:\t1\n";
        s += "NSpid:\t1\n";
        s += "NSpgid:\t1\n";
        s += "NSsid:\t1\n";
        s += "VmPeak:"; s += vm_sz;  s += "\n";
        s += "VmSize:"; s += vm_sz;  s += "\n";
        s += "VmLck:\t 0 kB\n";
        s += "VmPin:\t 0 kB\n";
        s += "VmHWM:";  s += vm_rss; s += "\n";
        s += "VmRSS:";  s += vm_rss; s += "\n";
        s += "RssAnon:\t 4096 kB\n";
        s += "RssFile:\t 0 kB\n";
        s += "RssShmem:\t 0 kB\n";
        s += "VmData:"; s += vm_sz;  s += "\n";
        s += "VmStk:\t 128 kB\n";
        s += "VmExe:\t 8 kB\n";
        s += "VmLib:\t 4096 kB\n";
        s += "VmPTE:\t 32 kB\n";
        s += "VmSwap:\t 0 kB\n";
        s += "HugetlbPages:\t 0 kB\n";
        s += "CoreDumping:\t0\n";
        s += "Threads:\t1\n";
        s += "SigQ:\t0/256\n";
        s += "SigPnd:\t0000000000000000\n";
        s += "ShdPnd:\t0000000000000000\n";
        s += "SigBlk:\t0000000000000000\n";
        s += "SigIgn:\t0000000000000000\n";
        s += "SigCgt:\t0000000000000000\n";
        s += "CapInh:\t0000000000000000\n";
        s += "CapPrm:\t0000003fffffffff\n";
        s += "CapEff:\t0000003fffffffff\n";
        s += "CapBnd:\t0000003fffffffff\n";
        s += "CapAmb:\t0000000000000000\n";
        s += "NoNewPrivs:\t0\n";
        s += "Seccomp:\t0\n";
        s += "Speculation_Store_Bypass:\tvulnerable\n";
        s += "Cpus_allowed:\t01\n";
        s += "Mems_allowed:\t1\n";
        s += "voluntary_ctxt_switches:\t0\n";
        s += "nonvoluntary_ctxt_switches:\t0\n";
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
    // /proc/self/environ → environment as NUL-separated KEY=VALUE pairs.
    // The old `std::string("PATH=/bin:/usr/bin\0", 16)` only copied 16
    // bytes ("PATH=/bin:/usr/b"), truncating the env var mid-token and
    // dropping the trailing NUL. Fix: copy the full 18 bytes + NUL = 19.
    if (path == "/proc/self/environ") {
        return serve(std::string("PATH=/bin:/usr/bin\0", 19));
    }
    // /proc/version
    if (path == "/proc/version") {
        return serve(std::string("Linux version 6.5.0 (bifrost-emu) (gcc) #1 SMP\n"));
    }
    // /proc/sys/kernel/osrelease
    if (path == "/proc/sys/kernel/osrelease") {
        return serve(std::string("6.5.0\n"));
    }
    // /proc/sys/kernel/hostname
    if (path == "/proc/sys/kernel/hostname") {
        return serve(std::string("bifrost\n"));
    }
    // /proc/self/limits → basic resource limits
    if (path == "/proc/self/limits") {
        std::string s;
        s += "Limit                     Soft Limit           Hard Limit           Units     \n";
        s += "Max cpu time              unlimited            unlimited            seconds   \n";
        s += "Max file size             unlimited            unlimited            bytes     \n";
        s += "Max data size             unlimited            unlimited            bytes     \n";
        s += "Max stack size            8388608              unlimited            bytes     \n";
        s += "Max core file size        0                    unlimited            bytes     \n";
        s += "Max resident set          unlimited            unlimited            bytes     \n";
        s += "Max processes             unlimited            unlimited            processes \n";
        s += "Max open files            1024                 4096                 files     \n";
        return serve(s);
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

std::shared_ptr<VNode> FdTable::get(int fd) const {
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
