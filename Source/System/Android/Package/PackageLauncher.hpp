// PackageLauncher.hpp — `bifrost-emu app.apk`: hand an Android package to Asgard.
#pragma once
#include <string>
#include <vector>

namespace arm64emu {

// True for a path named like an Android package: .apk, .apkm, .apks or .xapk.
bool has_android_package_extension(const std::string& path);

// Replace this process with `asgard.py run PACKAGE ARGS...`, which installs the
// package (a no-op when the same file is installed) and runs it, as `wine
// app.exe` does. Asgard is $BIFROST_ASGARD, else Scripts/Android/asgard.py next
// to the executable. Returns only on failure, after printing why: the exit
// status for main to use (126 not a package, 127 Asgard not found).
int exec_android_package(const std::string& package,
                         const std::vector<std::string>& args,
                         const std::string& window_plugin);

}  // namespace arm64emu
