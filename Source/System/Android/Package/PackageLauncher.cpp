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

std::vector<std::string> asgard_candidates(const std::string& exe_dir, const char* override_path,
                                           const char* xdg_data_home, const char* home) {
    static const char* const script = "/Scripts/Android/asgard.py";
    std::vector<std::string> paths;
    if (override_path && *override_path) paths.push_back(override_path);
    if (!exe_dir.empty()) {
        paths.push_back(exe_dir + script);                               // source checkout
        paths.push_back(exe_dir + "/../share/bifrost/asgard" + script);  // <prefix>/bin + <prefix>/share
    }
    if (xdg_data_home && xdg_data_home[0] == '/')
        paths.push_back(std::string(xdg_data_home) + "/bifrost/asgard" + script);
    else if (home && home[0] == '/')
        paths.push_back(std::string(home) + "/.local/share/bifrost/asgard" + script);
    paths.push_back(std::string("/usr/local/share/bifrost/asgard") + script);
    paths.push_back(std::string("/usr/share/bifrost/asgard") + script);
    return paths;
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
    const std::vector<std::string> candidates = asgard_candidates(
        exe.substr(0, exe.rfind('/') == std::string::npos ? 0 : exe.rfind('/')),
        getenv("BIFROST_ASGARD"), getenv("XDG_DATA_HOME"), getenv("HOME"));
    std::string asgard;
    for (const auto& path : candidates)
        if (access(path.c_str(), R_OK) == 0) { asgard = path; break; }
    if (asgard.empty()) {
        fprintf(stderr, "bifrost-emu: Android packages run through Asgard, which was not found in:\n");
        for (const auto& path : candidates) fprintf(stderr, "  %s\n", path.c_str());
        fprintf(stderr, "  Install it with Scripts/Setup/install-asgard.py from a source checkout whose\n"
                        "  Android tooling is provisioned (Docs/Android/ANDROID_APK.md), or set\n"
                        "  BIFROST_ASGARD to an asgard.py.\n");
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
