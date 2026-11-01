// SysFS.cpp — SysFS: synthetic /sys CPU topology.
//
// The emulator has no kernel, so there is no real sysfs.
// Guests that ask for the CPU topology must still see a
// coherent answer that matches the affinity mask
// sched_getaffinity advertises (kGuestCpuCount CPUs):
//   - Bionic's sysconf(_SC_NPROCESSORS_ONLN) reads
//     /sys/devices/system/cpu/online and returns -1 when the
//     file does not exist. libunity.so feeds that into its
//     job-system worker-count math; with -1 the count drops
//     to 0 and the presenting thread stalls (SpongeBob
//     presentation: window goes black with cursor trails).
//   - Unity also reads /sys/devices/system/cpu/possible,
//     /present, cpuN/cpu_capacity and cpuN/cpufreq/
//     cpuinfo_max_freq (strings in libunity.so).
//
// /sys/class/power_supply is the one part that mirrors the HOST: Android's battery model (healthd) reads the
// machine's own supplies (battery, mains, USB). Entries are re-read at every open/stat, names and attributes
// are validated (no path separators, no dot names), only regular files are served, read-only and capped at
// 4 KiB. A host without power supplies has an empty directory.
//
// Served from MemfdNode/DirNode like procfs, always
// synthetic — a rootfs has no /sys, and a host /sys would
// report the HOST's core count and contradict
// sched_getaffinity. sysfs_stat() (below) shares the
// classifier with open_sysfs() so stat/statx/faccessat see
// the same types and sizes the open path serves.
#include "System/VFS/VFS.hpp"
#include "System/VFS/Nodes/MemfdNode.hpp"
#include "System/VFS/Nodes/DirectoryNode.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace arm64emu::yggdrasil {
namespace {
// ── Synthetic tree classifier ──────────────────────────────
// One function decides every /sys path we serve: whether it
// is a directory, and its entries (dirs) or content
// (regular files). open_sysfs builds nodes from it and
// sysfs_stat fills struct stat from it, so the two can never
// drift apart. DT_* constants as in procfs/devfs.
struct SysfsNode {
    bool is_dir = false;
    std::string content;                       // regular files
    std::vector<DirNode::Entry> entries;     // directories
};
// ── Host power supplies ────────────────────────────────────
constexpr const char kSupplyRoot[] = "/sys/class/power_supply";
constexpr size_t kMaxAttribute = 4096;
bool safe_sysfs_name(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] == '.') return false;
    for (char c : name) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        c == '_' || c == '.' || c == '-' || c == ':';
        if (!ok) return false;
    }
    return true;
}
// Names in a host directory: directories (supplies are symlinks to them) or regular files.
bool host_entries(const std::string& dir, bool want_files, std::vector<DirNode::Entry>* out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (!safe_sysfs_name(name)) continue;
        if (want_files ? e->d_type == DT_REG : (e->d_type == DT_DIR || e->d_type == DT_LNK))
            out->push_back({name, static_cast<uint8_t>(want_files ? 0x1 /*DT_REG*/ : 0x4 /*DT_DIR*/)});
    }
    closedir(d);
    return true;
}
bool host_attribute(const std::string& supply, const std::string& attr, std::string* out) {
    const std::string path = std::string(kSupplyRoot) + "/" + supply + "/" + attr;
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    char buf[kMaxAttribute];
    ssize_t n = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0) return false;
    out->assign(buf, static_cast<size_t>(n));
    return true;
}
bool classify_power_supply(const std::string& path, SysfsNode* out) {
    const std::string root = kSupplyRoot;
    if (path.compare(0, root.size(), root) != 0) return false;
    std::string rest = path.substr(root.size());
    if (!rest.empty() && rest.back() == '/') rest.pop_back();
    if (rest.empty()) {
        out->is_dir = true;
        host_entries(root, false, &out->entries);  // none when the host has no supplies
        return true;
    }
    if (rest[0] != '/') return false;
    rest = rest.substr(1);
    const size_t slash = rest.find('/');
    const std::string supply = rest.substr(0, slash);
    if (!safe_sysfs_name(supply)) return false;
    if (slash == std::string::npos) {
        out->is_dir = true;
        return host_entries(root + "/" + supply, true, &out->entries);
    }
    const std::string attr = rest.substr(slash + 1);
    return safe_sysfs_name(attr) && host_attribute(supply, attr, &out->content);
}
bool classify_sysfs(const std::string& path, SysfsNode* out) {
    // /sys → devices, class
    if (path == "/sys" || path == "/sys/") {
        out->is_dir = true;
        out->entries = {{"class", 0x4 /*DT_DIR*/}, {"devices", 0x4 /*DT_DIR*/}};
        return true;
    }
    if (path == "/sys/class" || path == "/sys/class/") {
        out->is_dir = true;
        out->entries = {{"power_supply", 0x4 /*DT_DIR*/}};
        return true;
    }
    if (path.compare(0, 23, "/sys/class/power_supply") == 0)
        return classify_power_supply(path, out);
    if (path == "/sys/devices" || path == "/sys/devices/") {
        out->is_dir = true;
        out->entries = {{"system", 0x4 /*DT_DIR*/}};
        return true;
    }
    if (path == "/sys/devices/system" ||
        path == "/sys/devices/system/") {
        out->is_dir = true;
        out->entries = {{"cpu", 0x4 /*DT_DIR*/}};
        return true;
    }
    const std::string cpu_root = "/sys/devices/system/cpu";
    if (path == cpu_root || path == cpu_root + "/") {
        // The topology files sysconf/Unity read, then one
        // directory per advertised CPU.
        out->is_dir = true;
        out->entries = {
            {"online",   0x1 /*DT_REG*/},
            {"possible", 0x1 /*DT_REG*/},
            {"present",  0x1 /*DT_REG*/},
        };
        for (int i = 0; i < kGuestCpuCount; ++i)
            out->entries.push_back(
                {"cpu" + std::to_string(i), 0x4 /*DT_DIR*/});
        return true;
    }
    if (path == cpu_root + "/online" ||
        path == cpu_root + "/possible" ||
        path == cpu_root + "/present") {
        // Linux lists CPUs as a range string ("0-7\n"), which
        // is what Bionic's parse_cpu_list expects.
        out->content = "0-" + std::to_string(kGuestCpuCount - 1) + "\n";
        return true;
    }
    // Per-CPU subtree: cpuN, cpuN/cpu_capacity,
    // cpuN/cpufreq, cpuN/cpufreq/cpuinfo_max_freq.
    const std::string per = cpu_root + "/cpu";
    if (path.compare(0, per.size(), per) != 0) return false;
    const std::string rest = path.substr(per.size());
    size_t slash = rest.find('/');
    std::string num = rest.substr(0, slash);
    if (num.empty() || num.size() > 6) return false;
    for (char c : num)
        if (c < '0' || c > '9') return false;
    // No leading zeros; only CPUs inside the advertised
    // topology exist.
    if (num.size() > 1 && num[0] == '0') return false;
    if (std::strtol(num.c_str(), nullptr, 10) >= kGuestCpuCount)
        return false;
    if (slash == std::string::npos) {
        // /sys/devices/system/cpu/cpuN
        out->is_dir = true;
        out->entries = {
            {"cpu_capacity", 0x1 /*DT_REG*/},
            {"cpufreq",      0x4 /*DT_DIR*/},
        };
        return true;
    }
    const std::string tail = rest.substr(slash);
    if (tail == "/cpu_capacity") {
        // Normalized capacity (1024 = one "full" CPU).
        out->content = "1024\n";
        return true;
    }
    if (tail == "/cpufreq" || tail == "/cpufreq/") {
        out->is_dir = true;
        out->entries = {{"cpuinfo_max_freq", 0x1 /*DT_REG*/}};
        return true;
    }
    if (tail == "/cpufreq/cpuinfo_max_freq") {
        // 1.8 GHz, in kHz.
        out->content = "1800000\n";
        return true;
    }
    return false;
}
} // namespace
// ── Yggdrasil::open_sysfs ──────────────────────────────────
std::unique_ptr<Node> Yggdrasil::open_sysfs(const std::string& path,
                                                 int flags, mode_t /*mode*/,
                                                 int* err_out) {
    SysfsNode node;
    if (!classify_sysfs(path, &node)) {
        *err_out = 0;
        return nullptr;
    }
    // The host's power supply attributes are not the guest's to change.
    if (path.compare(0, 23, "/sys/class/power_supply") == 0 && (flags & O_ACCMODE) != O_RDONLY) {
        *err_out = node.is_dir ? -EISDIR : -EACCES;
        return nullptr;
    }
    if (node.is_dir)
        return std::make_unique<DirNode>(path, std::move(node.entries),
                                         flags);
    return MemfdNode::create("bifrost-sysfs", node.content, flags);
}
// ── Yggdrasil::sysfs_stat ──────────────────────────────────
// fstatat/statx/faccessat bypass Yggdrasil::open and stat the
// host path directly; a guest that stats before opening would
// see the host's /sys (wrong CPU count) or ENOENT. Consulted
// before the host path exactly like binder_device_stat.
bool Yggdrasil::sysfs_stat(const std::string& path, struct stat* st) {
    SysfsNode node;
    if (!classify_sysfs(path, &node)) return false;
    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    if (node.is_dir) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_size = static_cast<off_t>(node.entries.size());
        return true;
    }
    st->st_mode = S_IFREG | 0444;
    st->st_size = static_cast<off_t>(node.content.size());
    st->st_blksize = 4096;
    st->st_blocks =
        static_cast<blkcnt_t>((node.content.size() + 511) / 512);
    return true;
}
} // namespace arm64emu::yggdrasil
