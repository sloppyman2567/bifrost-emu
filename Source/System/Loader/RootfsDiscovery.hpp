// RootfsDiscovery.hpp — locate the AArch64 guest rootfs without flags.
//
// An explicit --rootfs / BIFROST_ROOT both selects the library root and
// sandboxes guest paths. A rootfs found by default is library-only: guest
// paths keep resolving on the host, as with `qemu-aarch64 -L`.
#pragma once
#include <string>
#include <vector>

namespace arm64emu {

// True when `dir` holds a glibc or musl AArch64 program interpreter.
bool is_guest_rootfs(const std::string& dir);

// A rootfs profile name: letters, digits, '.', '_' and '-', not starting with '.'
// (it becomes a directory name, so no separators or "..").
bool valid_rootfs_profile_name(const std::string& name);

// Default locations, most specific first: next to the executable, the user's
// data directory, then system-wide. `xdg_data_home` and `home` may be null.
// With a non-empty `profile` the same places are searched for the named profile
// instead (`.../bifrost/rootfs.d/<profile>`, `<exe dir>/rootfs.d/<profile>`).
std::vector<std::string> default_rootfs_candidates(const std::string& exe_dir,
                                                   const char* xdg_data_home,
                                                   const char* home,
                                                   const std::string& profile = "");

// The profile selected by BIFROST_ROOTFS_PROFILE ("" and "default" mean the
// unnamed default rootfs).
std::string requested_rootfs_profile();

// Every installed profile, "default" first. `active` marks the one a run would use
// (`path` is empty for a profile that is selected but not installed).
struct RootfsProfile {
    std::string name;
    std::string path;
    bool active = false;
};
std::vector<RootfsProfile> list_rootfs_profiles();

// First default candidate (of the requested profile) that is_guest_rootfs, or "" when there is none.
std::string find_default_rootfs();

// Directory whose lib dirs the dynamic linker searches: BIFROST_ROOT when set,
// else the requested profile's rootfs, else the default rootfs ("" when neither exists). No trailing slash.
std::string library_root();

// One-line description of where a rootfs is looked for, for error hints.
std::string rootfs_search_hint();

}  // namespace arm64emu
