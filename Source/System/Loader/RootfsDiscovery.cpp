// RootfsDiscovery.cpp — default AArch64 guest rootfs lookup.
#include "System/Loader/RootfsDiscovery.hpp"
#include <cstdlib>
#include <unistd.h>

namespace arm64emu {

static std::string trim_slashes(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

static std::string self_exe_dir() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    std::string exe(buf, static_cast<size_t>(n));
    size_t slash = exe.rfind('/');
    return slash == std::string::npos ? std::string() : exe.substr(0, slash);
}

bool is_guest_rootfs(const std::string& dir) {
    if (dir.empty()) return false;
    static const char* const loaders[] = {
        "/lib/ld-linux-aarch64.so.1", "/lib64/ld-linux-aarch64.so.1",
        "/lib/ld-musl-aarch64.so.1",
    };
    for (const char* loader : loaders)
        if (::access((trim_slashes(dir) + loader).c_str(), R_OK) == 0) return true;
    return false;
}

std::vector<std::string> default_rootfs_candidates(const std::string& exe_dir,
                                                   const char* xdg_data_home,
                                                   const char* home) {
    std::vector<std::string> dirs;
    if (!exe_dir.empty()) {
        dirs.push_back(exe_dir + "/rootfs");
        dirs.push_back(exe_dir + "/../share/bifrost/rootfs");
    }
    if (xdg_data_home && xdg_data_home[0] == '/')
        dirs.push_back(trim_slashes(xdg_data_home) + "/bifrost/rootfs");
    else if (home && home[0] == '/')
        dirs.push_back(trim_slashes(home) + "/.local/share/bifrost/rootfs");
    dirs.push_back("/usr/local/share/bifrost/rootfs");
    dirs.push_back("/usr/share/bifrost/rootfs");
    return dirs;
}

std::string find_default_rootfs() {
    // The environment and executable are fixed for the process; look once.
    static const std::string found = [] {
        for (const auto& dir : default_rootfs_candidates(
                 self_exe_dir(), getenv("XDG_DATA_HOME"), getenv("HOME")))
            if (is_guest_rootfs(dir)) return trim_slashes(dir);
        return std::string();
    }();
    return found;
}

std::string library_root() {
    if (const char* root = getenv("BIFROST_ROOT"))
        if (root[0] != '\0') return trim_slashes(root);
    return find_default_rootfs();
}

std::string rootfs_search_hint() {
    std::string text = "no AArch64 rootfs found (looked in:";
    for (const auto& dir : default_rootfs_candidates(
             self_exe_dir(), getenv("XDG_DATA_HOME"), getenv("HOME")))
        text += " " + dir;
    return text + "); pass --rootfs PATH, set BIFROST_ROOT, or run "
                  "`bifrost-emu --setup-rootfs`";
}

}  // namespace arm64emu
