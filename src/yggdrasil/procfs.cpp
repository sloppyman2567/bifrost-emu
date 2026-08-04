// yggdrasil/procfs.cpp — ProcFS: synthetic /proc/* content.
//
// Each virtual file is served from a MemfdNode. Files that should
// reflect live state (maps, status) use the lazy-regenerator form;
// static files (version, cpuinfo) use the write-once form.
//
// v1.4.5-alpha improvements:
//   - /proc and /proc/self directories are now DirNodes, so
//     `ls /proc` and `ls /proc/self` work (previously returned nothing).
//   - /proc/self/maps and /proc/self/status use lazy regeneration, so
//     they reflect live state on each open instead of being baked at
//     emulator startup.
#include "yggdrasil/yggdrasil.hpp"
#include "yggdrasil/memfd_node.hpp"
#include "yggdrasil/dir_node.hpp"
#include "core/memory.h"
#include "frost/graphics.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>
namespace arm64emu::yggdrasil {
// Helper: build a static-content MemfdNode.
static std::unique_ptr<Node> serve_static(const std::string& content, int flags) {
    return MemfdNode::create("bifrost-procfs", content, flags);
}
// Helper: build a lazy-regenerating MemfdNode.
static std::unique_ptr<Node> serve_lazy(std::function<std::string()> regen, int flags) {
    return MemfdNode::create_lazy("bifrost-procfs", std::move(regen), flags);
}
// Helper: build a DirNode for /proc or /proc/self.
static std::unique_ptr<Node> serve_dir(std::string name,
                                       std::vector<DirNode::Entry> entries,
                                       int flags) {
    return std::make_unique<DirNode>(std::move(name), std::move(entries), flags);
}
// ── /proc directory listing ──────────────────────────────────────────
// We expose a subset of /proc that real Linux guests can handle. The
// kernel exposes more (loadavg, stat, uptime, etc.) — those are
// future work; the entries we list here all have working handlers.
static std::vector<DirNode::Entry> proc_entries() {
    return {
        {"self",       0x4 /*DT_DIR*/},
        {"meminfo",    0x1 /*DT_REG*/},
        {"cpuinfo",    0x1 /*DT_REG*/},
        {"version",    0x1 /*DT_REG*/},
        {"mounts",     0x2 /*DT_LNK*/},
        {"filesystems",0x1 /*DT_REG*/},
        {"uptime",     0x1 /*DT_REG*/},
        {"loadavg",    0x1 /*DT_REG*/},
        {"sys",        0x4 /*DT_DIR*/},
    };
}
static std::vector<DirNode::Entry> proc_self_entries() {
    return {
        {"exe",     0x2 /*DT_LNK*/},
        {"cmdline", 0x1 /*DT_REG*/},
        {"comm",    0x1 /*DT_REG*/},
        {"maps",    0x1 /*DT_REG*/},
        {"status",  0x1 /*DT_REG*/},
        {"auxv",    0x1 /*DT_REG*/},
        {"environ", 0x1 /*DT_REG*/},
        {"limits",  0x1 /*DT_REG*/},
        {"mounts",  0x1 /*DT_REG*/},
        {"fd",      0x4 /*DT_DIR*/},
    };
}
// ── Yggdrasil::open_procfs ────────────────────────────────────────────
std::unique_ptr<Node> Yggdrasil::open_procfs(const std::string& path,
                                             int flags, mode_t /*mode*/,
                                             int* err_out) {
    // Directory listings.
    if (path == "/proc" || path == "/proc/") {
        return serve_dir("/proc", proc_entries(), flags);
    }
    if (path == "/proc/self" || path == "/proc/self/") {
        return serve_dir("/proc/self", proc_self_entries(), flags);
    }
    // ── /proc/self/exe → symlink to the ELF path ────────────────────
    if (path == "/proc/self/exe" || path == "/proc/self/exe/") {
        return serve_static(elf_path_, flags);
    }
    // ── /proc/self/cmdline → argv[0]\0argv[1]\0... ──────────────────
    if (path == "/proc/self/cmdline") {
        std::string cmdline;
        for (auto& a : argv_) { cmdline += a; cmdline.push_back('\0'); }
        if (cmdline.empty()) cmdline = elf_path_ + '\0';
        return serve_static(cmdline, flags);
    }
    // ── /proc/self/comm → process name (set by prctl PR_SET_NAME) ────
    // The kernel returns the basename of the executable by default,
    // which prctl(PR_SET_NAME) can override. Tools like `ps`, `top`,
    // and `htop` display this field.
    if (path == "/proc/self/comm") {
        // Use lazy so changes via prctl(PR_SET_NAME) are reflected.
        return serve_lazy([this]() -> std::string {
            std::string comm;
            if (comm_provider_) {
                comm = comm_provider_();
            } else {
                // Fallback: ELF basename.
                comm = elf_path_;
                size_t slash = comm.find_last_of('/');
                if (slash != std::string::npos) comm = comm.substr(slash + 1);
                if (comm.size() > 15) comm = comm.substr(0, 15);
            }
            comm.push_back('\n');
            return comm;
        }, flags);
    }
    // ── /proc/self/maps → real memory layout (LAZY) ─────────────────
    // The maps provider may return different results each time (memory
    // is allocated/freed during execution). Use lazy regeneration so
    // every open()/seek-to-0 reflects the current state.
    if (path == "/proc/self/maps") {
        return serve_lazy([this]() -> std::string {
            std::string maps;
            if (maps_provider_) {
                for (const auto& e : maps_provider_()) {
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
                maps += "00400000-004bf000 r-xp 00000000 00:00 0\n";
                maps += "004bf000-004ce000 r--p 00000000 00:00 0\n";
                maps += "004ce000-004df000 rw-p 00000000 00:00 0\n";
                maps += "5000000000-5001000000 rw-p 00000000 00:00 0\n";
                maps += "7fff000000-8000000000 rw-p 00000000 00:00 0 [stack]\n";
            }
            return maps;
        }, flags);
    }
    // ── /proc/mounts + /proc/self/mounts → host mount table (LAZY) ──
    // QFSFileEngine::drives() (Qt5) opens /etc/mtab then /proc/mounts;
    // if neither is readable it emits
    //   "QFSFileEngine::drives() no mounted file systems?!"
    // and the resulting qWarning re-enters QLoggingCategory static init,
    // which throws recursive_init_error (crash). Serve the host table.
    if (path == "/proc/mounts" || path == "/proc/self/mounts") {
        return serve_lazy([]() -> std::string {
            std::string out;
            FILE* f = fopen("/proc/mounts", "r");
            if (!f) {
                out = "none / rw,relatime 0 0\n";
            } else {
                char buf[1024];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                    out.append(buf, n);
                fclose(f);
            }
            return out;
        }, flags);
    }
    // ── /proc/self/status → process info (LAZY) ─────────────────────
    // Status includes VmSize/VmRSS which depend on live memory layout.
    if (path == "/proc/self/status") {
        return serve_lazy([this]() -> std::string {
            std::string name = "bifrost-emu";
            if (!argv_.empty()) {
                auto pos = argv_[0].rfind('/');
                name = (pos == std::string::npos) ? argv_[0] : argv_[0].substr(pos + 1);
                if (name.size() > 15) name.resize(15);
            }
            uint64_t vm_total = 0;
            if (maps_provider_) {
                for (const auto& e : maps_provider_()) {
                    vm_total += (e.end - e.start);
                }
            }
            if (vm_total == 0) vm_total = 8 * 1024 * 1024;
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
            return s;
        }, flags);
    }
    // ── Static-content files ────────────────────────────────────────
    if (path == "/proc/meminfo") {
        // Field order matters! toybox's `free` reads fields consecutively
        // by name (MemTotal, MemFree, Buffers, Cached, Shmem, SwapTotal,
        // SwapFree, SwapCached) and breaks when a name doesn't match.
        // We put those 8 fields FIRST, then the rest. Format matches
        // Linux's /proc/meminfo exactly (field name, colons, right-
        // justified width, " kB").
        std::string s;
        // ── Fields read by toybox `free` (must be consecutive) ──────
        s += "MemTotal:       16777216 kB\n";
        s += "MemFree:         8388608 kB\n";
        s += "Buffers:               0 kB\n";
        s += "Cached:          4194304 kB\n";
        s += "Shmem:                 0 kB\n";
        s += "SwapTotal:             0 kB\n";
        s += "SwapFree:              0 kB\n";
        s += "SwapCached:            0 kB\n";
        // ── Other fields (order doesn't matter for `free`) ──────────
        s += "MemAvailable:   12582912 kB\n";
        s += "Active:          4194304 kB\n";
        s += "Inactive:        2097152 kB\n";
        s += "Active(anon):    2097152 kB\n";
        s += "Inactive(anon):        0 kB\n";
        s += "Active(file):    2097152 kB\n";
        s += "Inactive(file):  2097152 kB\n";
        s += "Unevictable:           0 kB\n";
        s += "Mlocked:               0 kB\n";
        s += "Dirty:                 0 kB\n";
        s += "Writeback:             0 kB\n";
        s += "AnonPages:       2097152 kB\n";
        s += "Mapped:           524288 kB\n";
        s += "KReclaimable:     262144 kB\n";
        s += "Slab:             262144 kB\n";
        s += "SReclaimable:     262144 kB\n";
        s += "SUnreclaim:             0 kB\n";
        s += "KernelStack:        8192 kB\n";
        s += "PageTables:        16384 kB\n";
        s += "CommitLimit:     8388608 kB\n";
        s += "Committed_AS:    2097152 kB\n";
        s += "VmallocTotal:    8388608 kB\n";
        s += "VmallocUsed:      262144 kB\n";
        s += "VmallocChunk:    8126464 kB\n";
        s += "HugePages_Total:       0\n";
        s += "HugePages_Free:        0\n";
        s += "Hugepagesize:       2048 kB\n";
        return serve_static(s, flags);
    }
    if (path == "/proc/cpuinfo") {
        std::string s;
        s += "processor\t: 0\n";
        s += "BogoMIPS\t: 100.00\n";
        s += "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics\n";
        s += "CPU implementer\t: 0x41\n";
        s += "CPU architecture: 8\n";
        return serve_static(s, flags);
    }
    if (path == "/proc/self/auxv") return serve_static(std::string(), flags);
    if (path == "/proc/self/environ") {
        return serve_static(std::string("PATH=/bin:/usr/bin\0", 19), flags);
    }
    if (path == "/proc/version") {
        return serve_static(std::string("Linux version 6.5.0 (bifrost-emu) (gcc) #1 SMP\n"), flags);
    }
    if (path == "/proc/sys/kernel/osrelease") {
        return serve_static(std::string("6.5.0\n"), flags);
    }
    if (path == "/proc/sys/kernel/hostname") {
        return serve_static(std::string("bifrost\n"), flags);
    }
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
        return serve_static(s, flags);
    }
    // ── /proc/mounts + /proc/self/mounts ──────────────────
    // Many programs (df, mount, findmnt) read /proc/mounts. On real
    // Linux, /proc/mounts is a symlink to /proc/self/mounts. We serve
    // the same content from both paths so programs that open either
    // work. The content is a minimal mount table with the root fs and
    // /proc.
    if (path == "/proc/mounts" || path == "/proc/self/mounts") {
        std::string s;
        s += "rootfs / rootfs rw 0 0\n";
        s += "/dev/root / ext4 rw,relatime 0 0\n";
        s += "proc /proc proc rw,relatime 0 0\n";
        s += "sysfs /sys sysfs rw,relatime 0 0\n";
        s += "devtmpfs /dev devtmpfs rw,relatime 0 0\n";
        s += "tmpfs /tmp tmpfs rw,relatime 0 0\n";
        return serve_static(s, flags);
    }
    // ── /proc/filesystems ───────────────────────────────────────────
    if (path == "/proc/filesystems") {
        std::string s;
        s += "nodev\trootfs\n";
        s += "nodev\tproc\n";
        s += "nodev\tsysfs\n";
        s += "nodev\tdevtmpfs\n";
        s += "nodev\ttmpfs\n";
        s += "\text4\n";
        return serve_static(s, flags);
    }
    // ── /proc/self/fd → directory of open fds ─────────────
    // `ls /proc/self/fd` lists the guest's open file descriptors. We
    // return a DirNode with entries for 0, 1, 2 (stdin/stdout/stderr).
    // Real Linux has symlinks here (0 -> /dev/stdin etc.); we just
    // list them as regular files since we don't support readlink on
    // virtual paths yet.
    if (path == "/proc/self/fd" || path == "/proc/self/fd/") {
        std::vector<DirNode::Entry> fd_entries;
        fd_entries.push_back({"0", 0x2 /*DT_LNK*/});
        fd_entries.push_back({"1", 0x2 /*DT_LNK*/});
        fd_entries.push_back({"2", 0x2 /*DT_LNK*/});
        return serve_dir("/proc/self/fd", fd_entries, flags);
    }
    // ── /proc/uptime ─────────────────────────────────
    // toybox `uptime` reads /proc/uptime for the system uptime (seconds
    // with fractional part) and idle time. Without this, uptime showed
    // garbage (230961:11:19) because it read a non-existent file.
    // Format: "uptime_seconds idle_seconds\n"
    if (path == "/proc/uptime") {
        return serve_lazy([this]() -> std::string {
            struct sysinfo si;
            memset(&si, 0, sizeof(si));
            ::sysinfo(&si);
            char buf[128];
            // uptime with 2 decimal places, idle = uptime (we have 1 CPU)
            snprintf(buf, sizeof(buf), "%llu.%02llu %llu.%02llu\n",
                     (unsigned long long)si.uptime,
                     (unsigned long long)0,
                     (unsigned long long)si.uptime,
                     (unsigned long long)0);
            return std::string(buf);
        }, flags);
    }
    // ── /proc/loadavg ────────────────────────────────
    // toybox `uptime` and `top` read /proc/loadavg for load averages.
    // Format: "1min 5min 15min running/total last_pid\n"
    if (path == "/proc/loadavg") {
        return serve_lazy([this]() -> std::string {
            struct sysinfo si;
            memset(&si, 0, sizeof(si));
            ::sysinfo(&si);
            char buf[128];
            // sysinfo returns loads scaled by 65536 (1 << SI_LOAD_SHIFT)
            double load1  = static_cast<double>(si.loads[0])  / 65536.0;
            double load5  = static_cast<double>(si.loads[1])  / 65536.0;
            double load15 = static_cast<double>(si.loads[2])  / 65536.0;
            snprintf(buf, sizeof(buf), "%.2f %.2f %.2f %d/%d %d\n",
                     load1, load5, load15,
                     1, 1, 1);  // 1 running process, 1 total, PID 1
            return std::string(buf);
        }, flags);
    }
    *err_out = 0;
    return nullptr;
}
} // namespace arm64emu::yggdrasil
