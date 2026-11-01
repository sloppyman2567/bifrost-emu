// RootfsDiscovery.cpp — default AArch64 guest rootfs lookup.
#include "System/Loader/RootfsDiscovery.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <dirent.h>
#include <unistd.h>

namespace arm64emu {

static std::string trim_slashes(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

std::string self_exe_dir() {
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

bool valid_rootfs_profile_name(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] == '.') return false;
    for (char c : name)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) return false;
    return true;
}

std::vector<std::string> default_rootfs_candidates(const std::string& exe_dir,
                                                   const char* xdg_data_home,
                                                   const char* home,
                                                   const std::string& profile) {
    // <data>/bifrost/rootfs is the default rootfs; named profiles sit beside it in rootfs.d/.
    const std::string leaf = profile.empty() ? "rootfs" : "rootfs.d/" + profile;
    std::vector<std::string> dirs;
    if (!exe_dir.empty()) {
        dirs.push_back(exe_dir + "/" + leaf);
        dirs.push_back(exe_dir + "/../share/bifrost/" + leaf);
    }
    if (xdg_data_home && xdg_data_home[0] == '/')
        dirs.push_back(trim_slashes(xdg_data_home) + "/bifrost/" + leaf);
    else if (home && home[0] == '/')
        dirs.push_back(trim_slashes(home) + "/.local/share/bifrost/" + leaf);
    dirs.push_back("/usr/local/share/bifrost/" + leaf);
    dirs.push_back("/usr/share/bifrost/" + leaf);
    return dirs;
}

std::string requested_rootfs_profile() {
    const char* name = getenv("BIFROST_ROOTFS_PROFILE");
    if (!name || !name[0] || std::string(name) == "default") return {};
    return name;
}

static std::string first_rootfs(const std::string& profile) {
    if (!profile.empty() && !valid_rootfs_profile_name(profile)) return {};
    for (const auto& dir : default_rootfs_candidates(self_exe_dir(), getenv("XDG_DATA_HOME"), getenv("HOME"), profile))
        if (is_guest_rootfs(dir)) return trim_slashes(dir);
    return {};
}

std::string find_default_rootfs() {
    // The environment and executable are fixed for the process; look once.
    static const std::string found = first_rootfs(requested_rootfs_profile());
    return found;
}

std::vector<RootfsProfile> list_rootfs_profiles() {
    std::vector<std::string> names{""};
    // A profile is any rootfs.d/<name> that holds a loader, in any of the places a rootfs is looked for.
    for (const auto& dir : default_rootfs_candidates(self_exe_dir(), getenv("XDG_DATA_HOME"), getenv("HOME"), "x")) {
        const std::string parent = dir.substr(0, dir.size() - 1);  // ".../rootfs.d/"
        if (DIR* d = opendir(parent.c_str())) {
            while (const dirent* e = readdir(d)) {
                const std::string name = e->d_name;
                if (valid_rootfs_profile_name(name) && is_guest_rootfs(parent + name) &&
                    std::find(names.begin(), names.end(), name) == names.end())
                    names.push_back(name);
            }
            closedir(d);
        }
    }
    std::sort(names.begin() + 1, names.end());
    const std::string active = requested_rootfs_profile();
    if (!active.empty() && std::find(names.begin(), names.end(), active) == names.end()) names.push_back(active);
    std::vector<RootfsProfile> profiles;
    for (const auto& name : names)
        profiles.push_back({name.empty() ? "default" : name, first_rootfs(name), name == active});
    return profiles;
}

std::string library_root() {
    if (const char* root = getenv("BIFROST_ROOT"))
        if (root[0] != '\0') return trim_slashes(root);
    return find_default_rootfs();
}

std::string rootfs_search_hint() {
    const std::string profile = requested_rootfs_profile();
    std::string text = profile.empty() ? "no AArch64 rootfs found (looked in:"
                                       : "no AArch64 rootfs for profile '" + profile + "' (looked in:";
    for (const auto& dir : default_rootfs_candidates(
             self_exe_dir(), getenv("XDG_DATA_HOME"), getenv("HOME"), profile))
        text += " " + dir;
    return text + "); pass --rootfs PATH, set BIFROST_ROOT, or run " +
                  (profile.empty() ? "`bifrost-emu --setup-rootfs`"
                                       : "`bifrost-emu --setup-rootfs --profile " + profile + "`");
}

}  // namespace arm64emu
