// RootfsSetup.hpp — `bifrost-emu --setup-rootfs`: build and install the guest rootfs.
#pragma once
#include <string>
#include <vector>

namespace arm64emu {

// Installer script next to the executable (a checkout or build tree) or in an installed package's
// <prefix>/share/bifrost/setup, or "" when there is none. `exe_dir` is the executable's directory.
std::string find_rootfs_installer(const std::string& exe_dir);

// Runs Scripts/Setup/install-rootfs.sh with `args` (--system, --prefix DIR, --full,
// --no-fetch, --uninstall, ...) and returns its exit status, or 127 when the installer
// is not available. The script does the work; this only locates it.
int run_rootfs_setup(const std::vector<std::string>& args);

}  // namespace arm64emu
