// PackageLauncher.hpp — `bifrost-emu app.apk`: hand an Android package to Asgard.
#pragma once
#include <string>
#include <vector>

namespace arm64emu {

// True for a path named like an Android package: .apk, .apkm, .apks or .xapk.
bool has_android_package_extension(const std::string& path);

// Where asgard.py is looked for, first match wins: an explicit override, the
// executable's own checkout, <prefix>/share/bifrost/asgard beside an installed
// <prefix>/bin, then the user's and the system's data directories.
std::vector<std::string> asgard_candidates(const std::string& exe_dir, const char* override_path,
                                           const char* xdg_data_home, const char* home);

// Replace this process with `asgard.py run PACKAGE ARGS...`, which installs the
// package (a no-op when the same file is installed) and runs it, as `wine
// app.exe` does. Asgard is found through asgard_candidates. Returns only on
// failure, after printing why: the exit status for main to use (126 not a
// package, 127 Asgard not found).
int exec_android_package(const std::string& package,
                         const std::vector<std::string>& args,
                         const std::string& window_plugin);

}  // namespace arm64emu
