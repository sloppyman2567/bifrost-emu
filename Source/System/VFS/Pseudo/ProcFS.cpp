// ProcFS.cpp — ProcFS: synthetic /proc/* content.
//
// Each virtual file is served from a MemfdNode. Files that should
// reflect live state (maps, status) use the lazy-regenerator form;
// static files (version, cpuinfo) use the write-once form.
//
// Behavior:
//   - /proc and /proc/self directories are now DirNodes, so
//     `ls /proc` and `ls /proc/self` work (previously returned nothing).
//   - /proc/self/maps and /proc/self/status use lazy regeneration, so
//     they reflect live state on each open instead of being baked at
//     emulator startup.
#include "System/VFS/VFS.hpp"
#include "System/VFS/Nodes/MemfdNode.hpp"
#include "System/VFS/Nodes/ProcMemoryNode.hpp"
#include "System/VFS/Nodes/DirectoryNode.hpp"
#include "System/VFS/Nodes/HostNode.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Graphics/Display/Graphics.hpp"
#include "System/Core/GuestMemoryView.hpp"
#include <sstream>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/sysmacros.h>
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
// Helper: is /proc/self/exe or /proc/<pid>/exe (mirrors the syscall-layer
// helper in Source/System/Syscalls/Files/FileSystem.cpp — keep both in sync).
static bool is_proc_exe_path(const std::string& p) {
    if (p == "/proc/self/exe" || p == "/proc/self/exe/") return true;
    if (p.compare(0, 6, "/proc/") != 0) return false;
    size_t slash = p.find('/', 6);
    if (slash == std::string::npos) return false;
    std::string pid = p.substr(6, slash - 6);
    if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) return false;
    std::string tail = p.substr(slash + 1);
    return tail == "exe" || tail == "exe/";
}
// /proc/self/task/<tid>/comm (fs/proc/base.c comm_show/comm_write): a
// read shows the name and '\n' as of open; each write renames the task
// to its first 15 bytes and consumes the whole buffer.
class TaskCommNode : public Node {
public:
    TaskCommNode(int tid, std::string content, int flags,
                 std::function<bool(int, const std::string&)> set)
        : tid_(tid), content_(std::move(content)), flags_(flags), set_(std::move(set)) {}
    ssize_t read(uint64_t off, void* buf, size_t n) override {
        const uint64_t at = off == UINT64_MAX ? pos_ : off;
        if (at >= content_.size()) return 0;
        n = std::min<size_t>(n, content_.size() - at);
        std::memcpy(buf, content_.data() + at, n);
        if (off == UINT64_MAX) pos_ += n;
        return static_cast<ssize_t>(n);
    }
    ssize_t write(uint64_t, const void* buf, size_t n) override {
        std::string name(static_cast<const char*>(buf), std::min<size_t>(n, 15));
        name.resize(std::strlen(name.c_str()));
        if (!set_ || !set_(tid_, name)) return -ESRCH;
        return static_cast<ssize_t>(n);
    }
    ssize_t lseek(int64_t off, int whence) override {
        int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<int64_t>(pos_)
                     : whence == SEEK_END ? static_cast<int64_t>(content_.size()) : -1;
        if (base < 0 || base + off < 0) return -EINVAL;
        pos_ = static_cast<uint64_t>(base + off);
        return static_cast<ssize_t>(pos_);
    }
    int fstat(struct stat* st) override {
        std::memset(st, 0, sizeof(*st));
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        st->st_blksize = 1024;
        return 0;
    }
    int flags() const override { return flags_; }
    void set_flags(int f) override { flags_ = f; }
private:
    int tid_;
    std::string content_;
    int flags_;
    uint64_t pos_ = 0;
    std::function<bool(int, const std::string&)> set_;
};
// <tid> of "/proc/self/task/<tid>/comm", or -1.
static int task_comm_tid(const std::string& p) {
    static const std::string prefix = "/proc/self/task/";
    if (p.compare(0, prefix.size(), prefix) != 0) return -1;
    const size_t slash = p.find('/', prefix.size());
    if (slash == std::string::npos || p.compare(slash, std::string::npos, "/comm") != 0)
        return -1;
    const std::string digits = p.substr(prefix.size(), slash - prefix.size());
    if (digits.empty() || digits.size() > 9 ||
        digits.find_first_not_of("0123456789") != std::string::npos)
        return -1;
    return std::stoi(digits);
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
        {"stat",    0x1 /*DT_REG*/},
        {"status",  0x1 /*DT_REG*/},
        {"auxv",    0x1 /*DT_REG*/},
        {"environ", 0x1 /*DT_REG*/},
        {"limits",  0x1 /*DT_REG*/},
        {"mounts",  0x1 /*DT_REG*/},
        {"fd",      0x4 /*DT_DIR*/},
    };
}
// ── Yggdrasil::open_procfs ────────────────────────────────────────────
std::unique_ptr<Node> Yggdrasil::open_procfs(const std::string& requested,
                                             int flags, mode_t mode,
                                             int* err_out) {
    std::string path = canonical_proc_path(requested);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    auto node = resolve_procfs(path, flags, mode, err_out);
    if (node && !requested.empty() && requested.back() == '/' && !node->is_dir()) {
        *err_out = ENOTDIR;
        return nullptr;
    }
    // /proc/self/exe follows the real executable's identity, or presents
    // its existing symlink node for O_PATH|O_NOFOLLOW.
    if (node && !is_proc_exe_path(path)) node->set_proc_identity(path);
    return node;
}

bool Yggdrasil::procfs_stat(const std::string& requested, int flags,
                          struct stat* st, int& result) {
    const std::string path = canonical_proc_path(requested);
    if (is_proc_exe_path(path)) return false;
    // /proc/mounts is a symlink; retain the existing lstat handling.
    if (path == "/proc/mounts" && (flags & AT_SYMLINK_NOFOLLOW)) return false;
    int error = 0;
    auto node = open_procfs(path, O_PATH | ((flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0), 0, &error);
    if (!node && error == 0) return false;
    result = node ? node->guest_stat(st) : -error;
    return true;
}

// A complete Linux /proc/<pid>/status for this process; the debugger's view of a task (name, state, ids,
// tracer, credentials) replaces the fields it knows, so a task's file has every field and the same layout.
std::string Yggdrasil::proc_status_text(const std::string& task_fields) const {
    // Linux reports the task comm (prctl PR_SET_NAME) here.
    std::string name = comm_provider_ ? comm_provider_() : "bifrost-emu";
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
    const ProcessIds ids = process_ids();
    s += "Tgid:\t" + std::to_string(ids.pid) + "\n";
    s += "Ngid:\t0\n";
    s += "Pid:\t" + std::to_string(ids.pid) + "\n";
    s += "PPid:\t" + std::to_string(ids.ppid) + "\n";
    std::string debug_status;
    if (debug_status_provider_ && debug_status_provider_(ids.pid, debug_status)) {
        size_t at = debug_status.find("TracerPid:");
        size_t end = debug_status.find('\n', at);
        s += at == std::string::npos ? "TracerPid:\t0\n" : debug_status.substr(at, end - at + 1);
    } else s += "TracerPid:\t0\n";
    for (const auto& identity : {std::make_pair("Uid", process_uid_), std::make_pair("Gid", process_gid_)}) {
        s += std::string(identity.first) + ":";
        for (int i = 0; i < 4; ++i) s += '\t' + std::to_string(identity.second);
        s += '\n';
    }
    s += "NStgid:\t" + std::to_string(ids.pid) + "\n";
    s += "NSpid:\t" + std::to_string(ids.pid) + "\n";
    s += "NSpgid:\t" + std::to_string(ids.pgid) + "\n";
    s += "NSsid:\t" + std::to_string(ids.sid) + "\n";
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
    s += process_uid_ ? "CapPrm:\t0000000000000000\n" : "CapPrm:\t0000003fffffffff\n";
    s += process_uid_ ? "CapEff:\t0000000000000000\n" : "CapEff:\t0000003fffffffff\n";
    s += "CapBnd:\t0000003fffffffff\n";
    s += "CapAmb:\t0000000000000000\n";
    s += "NoNewPrivs:\t0\n";
    s += "Seccomp:\t0\n";
    s += "Speculation_Store_Bypass:\tvulnerable\n";
    s += "Cpus_allowed:\t01\n";
    s += "Mems_allowed:\t1\n";
    s += "voluntary_ctxt_switches:\t0\n";
    s += "nonvoluntary_ctxt_switches:\t0\n";
    // Replace each field the task text carries; the namespace ids follow Tgid and Pid.
    auto replace_field = [&s](const std::string& key, const std::string& value) {
        const size_t at = s.find(key + ":\t");
        if (at == std::string::npos) return;
        s.replace(at, s.find('\n', at) - at + 1, key + ":\t" + value + "\n");
    };
    for (size_t line = 0; line < task_fields.size();) {
        const size_t end = task_fields.find('\n', line);
        const std::string text = task_fields.substr(line, end == std::string::npos ? std::string::npos : end - line);
        const size_t colon = text.find(":\t");
        if (colon != std::string::npos) {
            const std::string key = text.substr(0, colon), value = text.substr(colon + 2);
            replace_field(key, value);
            if (key == "Tgid") replace_field("NStgid", value);
            if (key == "Pid") replace_field("NSpid", value);
        }
        if (end == std::string::npos) break;
        line = end + 1;
    }
    return s;
}

std::unique_ptr<Node> Yggdrasil::resolve_procfs(const std::string& path,
                                             int flags, mode_t /*mode*/,
                                             int* err_out) {
    // Only tasks registered in this emulator's fork family are visible.
    // Unrelated numeric PIDs still fall through to the existing denial.
    if (path.compare(0, 6, "/proc/") == 0 && path.size() > 13 &&
        path.compare(path.size() - 7, 7, "/status") == 0 && debug_status_provider_) {
        const std::string number = path.substr(6, path.size() - 13);
        if (!number.empty() && number.size() < 11 && number.find_first_not_of("0123456789") == std::string::npos) {
            uint64_t tid = std::stoull(number);
            std::string text;
            if (tid <= INT32_MAX && debug_status_provider_(static_cast<int>(tid), text))
                return serve_static(proc_status_text(text), flags);
        }
    }
    // Guest memory and its page flags, over the guest's own address space (not the host's).
    if (path == "/proc/self/mem" || path == "/proc/self/pagemap") {
        const bool mem = path == "/proc/self/mem";
        if (!mem && (flags & O_ACCMODE) != O_RDONLY) { *err_out = -EACCES; return nullptr; }
        return std::make_unique<ProcMemoryNode>(
            mem ? ProcMemoryNode::Kind::Mem : ProcMemoryNode::Kind::Pagemap, flags, memory_read_, memory_write_,
            maps_provider_);
    }
    // Directory listings.
    if (path == "/proc" || path == "/proc/") {
        return serve_dir("/proc", proc_entries(), flags);
    }
    if (path == "/proc/self" || path == "/proc/self/") {
        return serve_dir("/proc/self", proc_self_entries(), flags);
    }
    // ── /proc/self/exe → symlink to the ELF path ────────────────────
    // Also serves /proc/<pid>/exe (the guest's getpid() returns 1, and
    // Qt resolves its own binary via /proc/<pid>/exe). stat/lstat/readlink
    // on these paths are served synthetically in the fstatat/statx/readlinkat
    // syscall handlers. Opening follows the link to the executable itself:
    // Bionic realpath() uses O_PATH followed by /proc/self/fd/<fd>.
    if (is_proc_exe_path(path)) {
        if (flags & O_NOFOLLOW) {
            // O_PATH|O_NOFOLLOW needs a synthetic symlink descriptor; do not
            // return an unrelated regular-file descriptor for that request.
            *err_out = (flags & O_PATH) ? -ENOTSUP : -ELOOP;
            return nullptr;
        }
        if (!(flags & O_PATH) && ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC))) {
            *err_out = -ETXTBSY;
            return nullptr;
        }
        if (elf_backing_path_.empty()) return open_host(elf_path_, flags, 0, err_out);
        // This path comes from the loader, never from a guest-controlled proc
        // pathname. A relative ELF explicitly loaded outside rootfs still has
        // a real backing file and must not be remapped a second time.
        int fd = ::open(elf_backing_path_.c_str(), flags);
        if (fd < 0) { *err_out = -errno; return nullptr; }
        return std::make_unique<HostNode>(fd, flags);
    }
    // ── /proc/self/cmdline → the guest's live argv block ────────────
    if (path == "/proc/self/cmdline") {
        return serve_lazy([this]() -> std::string {
            return cmdline_provider_ ? cmdline_provider_() : elf_path_ + '\0';
        }, flags);
    }
    // ── /proc/self/comm → process name (set by prctl PR_SET_NAME) ────
    // The kernel returns the basename of the executable by default,
    // which prctl(PR_SET_NAME) can override. Tools like `ps`, `top`,
    // and `htop` display this field.
    if (path == "/proc/self/comm") {
        // The thread-group leader's task file: writes rename the leader.
        std::string name;
        if (task_comm_get_ && task_comm_get_(self_pid_, name))
            return std::make_unique<TaskCommNode>(self_pid_, name + '\n', flags, task_comm_set_);
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
    // ── /proc/self/task/<tid>/comm → one thread's name ──────────────
    // Bionic pthread_setname_np/getname_np use it for other threads.
    // Guest TIDs are not host TIDs, so it is never host passthrough.
    if (int tid = task_comm_tid(path); tid >= 0) {
        std::string name;
        if (!task_comm_get_ || !task_comm_get_(tid, name)) {
            *err_out = -ENOENT;
            return nullptr;
        }
        return std::make_unique<TaskCommNode>(tid, name + '\n', flags, task_comm_set_);
    }
    // Bionic finds the main stack via stat field 28, then /proc/self/maps.
    // Host passthrough supplied the emulator's host SP, outside guest maps.
    // Keep unmodeled accounting fields zero rather than exposing host state.
    // Identity fields match getpid/getppid/getpgid/getsid (process_ids);
    // the comm is the task name, as /proc/self/comm reports it.
    if (path == "/proc/self/stat") {
        return serve_lazy([this]() -> std::string {
            const ProcessIds ids = process_ids();
            std::string comm = comm_provider_ ? comm_provider_() : elf_path_;
            if (!comm_provider_) {
                size_t slash = comm.find_last_of('/');
                if (slash != std::string::npos) comm = comm.substr(slash + 1);
                if (comm.size() > 15) comm = comm.substr(0, 15);
            }
            // The guest is a host process: its state, CPU times, nice value, thread count, start time and
            // memory sizes are the host's own (a stat file with a zero start time and size is not a process's).
            std::vector<std::string> host_fields(53);
            if (FILE* host = fopen("/proc/self/stat", "r")) {
                char text[1024];
                const size_t n = fread(text, 1, sizeof text - 1, host);
                fclose(host);
                text[n] = '\0';
                if (const char* close = strrchr(text, ')')) {
                    std::istringstream rest(close + 1);
                    for (unsigned field = 3; field <= 52 && (rest >> host_fields[field]); ++field) {}
                }
            }
            auto from_host = [&host_fields](unsigned field, const char* fallback) {
                return host_fields[field].empty() ? std::string(fallback) : host_fields[field];
            };
            std::string stat = std::to_string(ids.pid) + " (" + comm + ") " + from_host(3, "R");
            for (unsigned field = 4; field <= 52; ++field) {
                stat += ' ';
                switch (field) {
                    case 23: {  // vsize: what the guest has mapped, not the host's window reservation
                        uint64_t mapped = 0;
                        if (maps_provider_)
                            for (const auto& e : maps_provider_()) mapped += e.end - e.start;
                        stat += std::to_string(mapped ? mapped : 8ull * 1024 * 1024);
                        break;
                    }
                    case 14: case 15: case 16: case 17: case 19: case 20: case 22: case 24:
                        stat += from_host(field, field == 20 ? "1" : "0"); break;
                    case 4: stat += std::to_string(ids.ppid); break;
                    case 5: stat += std::to_string(ids.pgid); break;
                    case 6: stat += std::to_string(ids.sid); break;
                    case 8: stat += "-1"; break;
                    case 18: stat += "20"; break;
                    case 28: stat += std::to_string(initial_stack_); break;
                    default: stat += "0"; break;
                }
            }
            stat += '\n';
            return stat;
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
                // fs/proc/task_mmu.c show_map_vma: the header ends with a
                // space; a name is padded to column 73 (64-bit seq_setwidth).
                constexpr size_t NAME_COLUMN = 25 + sizeof(uint64_t) * 6 - 1;
                for (const auto& e : maps_provider_()) {
                    char line[160];
                    int n = snprintf(line, sizeof(line),
                        "%08llx-%08llx %s %08llx %02x:%02x %llu ",
                        static_cast<unsigned long long>(e.start),
                        static_cast<unsigned long long>(e.end),
                        e.perms, static_cast<unsigned long long>(e.offset),
                        major(static_cast<dev_t>(e.dev)), minor(static_cast<dev_t>(e.dev)),
                        static_cast<unsigned long long>(e.inode));
                    if (n < 0 || static_cast<size_t>(n) >= sizeof(line)) continue;
                    maps.append(line, static_cast<size_t>(n));
                    if (!e.label.empty()) {
                        if (static_cast<size_t>(n) < NAME_COLUMN)
                            maps.append(NAME_COLUMN - static_cast<size_t>(n), ' ');
                        maps += ' ';
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
        return serve_lazy([this]() { return proc_status_text(); }, flags);
    }
    // ── Static-content files ────────────────────────────────────────
    if (path == "/proc/meminfo") {
        // The host's real memory pressure under a phone-sized total (GuestMemoryView.hpp).
        // Field order matters! toybox's `free` reads fields consecutively
        // by name (MemTotal, MemFree, Buffers, Cached, Shmem, SwapTotal,
        // SwapFree, SwapCached) and breaks when a name doesn't match.
        // We put those 8 fields FIRST, then the rest. Format matches
        // Linux's /proc/meminfo exactly (field name, colons, right-
        // justified width, " kB").
        const GuestMemoryInfo mem = guest_memory_info();
        const uint64_t total_kb = mem.total / 1024;
        // The remaining fields are plausible fixed shares of the total (tuned for a 16 GiB total).
        auto share = [&](uint64_t kb16) { return kb16 * total_kb / 16777216ull; };
        std::string s;
        auto field = [&s](const char* name, uint64_t kb) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%-15s %8llu kB\n", name, static_cast<unsigned long long>(kb));
            s += buf;
        };
        // ── Fields read by toybox `free` (must be consecutive) ──────
        field("MemTotal:", total_kb);
        field("MemFree:", mem.free / 1024);
        field("Buffers:", mem.buffers / 1024);
        field("Cached:", mem.cached / 1024);
        field("Shmem:", mem.shmem / 1024);
        field("SwapTotal:", 0);
        field("SwapFree:", 0);
        field("SwapCached:", 0);
        // ── Other fields (order doesn't matter for `free`) ──────────
        field("MemAvailable:", mem.available / 1024);
        field("Active:", share(4194304));
        field("Inactive:", share(2097152));
        field("Active(anon):", share(2097152));
        field("Inactive(anon):", 0);
        field("Active(file):", share(2097152));
        field("Inactive(file):", share(2097152));
        field("Unevictable:", 0);
        field("Mlocked:", 0);
        field("Dirty:", 0);
        field("Writeback:", 0);
        field("AnonPages:", share(2097152));
        field("Mapped:", share(524288));
        field("KReclaimable:", share(262144));
        field("Slab:", share(262144));
        field("SReclaimable:", share(262144));
        field("SUnreclaim:", 0);
        field("KernelStack:", share(8192));
        field("PageTables:", share(16384));
        field("CommitLimit:", total_kb / 2);
        field("Committed_AS:", share(2097152));
        field("VmallocTotal:", share(8388608));
        field("VmallocUsed:", share(262144));
        field("VmallocChunk:", share(8126464));
        s += "HugePages_Total:       0\n";
        s += "HugePages_Free:        0\n";
        s += "Hugepagesize:       2048 kB\n";
        return serve_static(s, flags);
    }
    if (path == "/proc/cpuinfo") {
        // One block per advertised CPU (kGuestCpuCount) so this
        // source agrees with /sys/devices/system/cpu/* and
        // sched_getaffinity.
        std::string s;
        for (int i = 0; i < kGuestCpuCount; ++i) {
            s += "processor\t: " + std::to_string(i) + "\n";
            s += "BogoMIPS\t: 100.00\n";
            s += "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics\n";
            s += "CPU implementer\t: 0x41\n";
            s += "CPU architecture: 8\n";
        }
        return serve_static(s, flags);
    }
    if (path == "/proc/self/auxv") return serve_static(std::string(), flags);
    if (path == "/proc/self/environ") {
        return serve_lazy([this]() {
            return environ_provider_ ? environ_provider_() : std::string();
        }, flags);
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
        return serve_lazy([]() -> std::string {
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
        return serve_lazy([]() -> std::string {
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
