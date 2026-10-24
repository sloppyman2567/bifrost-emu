// PackageLauncher.cpp — start an Android package through Asgard.
#include "System/Android/Package/PackageLauncher.hpp"
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace arm64emu {

static bool is_zip_archive(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char magic[4];
    bool zip = fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
               magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
    fclose(f);
    return zip;
}

static std::string self_exe() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

bool has_android_package_extension(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return ext == ".apk" || ext == ".apkm" || ext == ".apks" || ext == ".xapk";
}

int exec_android_package(const std::string& package,
                         const std::vector<std::string>& args,
                         const std::string& window_plugin) {
    if (access(package.c_str(), R_OK) != 0) {
        fprintf(stderr, "bifrost-emu: cannot open '%s': %s\n", package.c_str(), strerror(errno));
        return 127;
    }
    if (!is_zip_archive(package)) {
        fprintf(stderr, "bifrost-emu: '%s' is not an Android package (APK, APKM, APKS or XAPK ZIP archive).\n",
                package.c_str());
        return 126;
    }
    const std::string exe = self_exe();
    std::string asgard;
    if (const char* override_path = getenv("BIFROST_ASGARD"); override_path && *override_path) {
        asgard = override_path;
    } else if (size_t slash = exe.rfind('/'); slash != std::string::npos) {
        asgard = exe.substr(0, slash) + "/Scripts/Android/asgard.py";
    }
    if (asgard.empty() || access(asgard.c_str(), R_OK) != 0) {
        fprintf(stderr,
                "bifrost-emu: Android packages run through Asgard, but '%s' was not found.\n"
                "  Run bifrost-emu from a source checkout with the Android tooling provisioned\n"
                "  (see Docs/Android/ANDROID_APK.md), or set BIFROST_ASGARD to its asgard.py.\n",
                asgard.empty() ? "Scripts/Android/asgard.py" : asgard.c_str());
        return 127;
    }
    // The package runs in Asgard's own managed roots; a BIFROST_ROOT meant for
    // a plain guest would confine every Android process to it.
    unsetenv("BIFROST_ROOT");
    std::vector<std::string> command = {"python3", asgard};
    if (!exe.empty()) { command.push_back("--emu"); command.push_back(exe); }
    command.push_back("run");
    command.push_back(package);
    command.insert(command.end(), args.begin(), args.end());
    if (!window_plugin.empty()) { command.push_back("--android-window-plugin"); command.push_back(window_plugin); }
    std::vector<char*> argv;
    for (auto& word : command) argv.push_back(word.data());
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    fprintf(stderr, "bifrost-emu: cannot run python3 for Asgard: %s\n", strerror(errno));
    return 127;
}

}  // namespace arm64emu
