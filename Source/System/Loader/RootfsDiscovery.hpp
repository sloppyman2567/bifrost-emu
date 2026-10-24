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

// Default locations, most specific first: next to the executable, the user's
// data directory, then system-wide. `xdg_data_home` and `home` may be null.
std::vector<std::string> default_rootfs_candidates(const std::string& exe_dir,
                                                   const char* xdg_data_home,
                                                   const char* home);

// First default candidate that is_guest_rootfs, or "" when there is none.
std::string find_default_rootfs();

// Directory whose lib dirs the dynamic linker searches: BIFROST_ROOT when set,
// else the default rootfs ("" when neither exists). No trailing slash.
std::string library_root();

// One-line description of where a rootfs is looked for, for error hints.
std::string rootfs_search_hint();

}  // namespace arm64emu
