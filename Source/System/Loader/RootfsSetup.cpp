// RootfsSetup.cpp — locate and run the rootfs installer for `--setup-rootfs`.
#include "System/Loader/RootfsSetup.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace arm64emu {

static const char* const kPackagedTree = "/share/bifrost/setup";

std::string find_rootfs_installer(const std::string& exe_dir) {
    // <checkout>/bifrost-emu, or <checkout>/build/<config>/bifrost-emu.
    for (const char* up : {"", "/..", "/../.."}) {
        const std::string script = exe_dir + up + "/Scripts/Setup/install-rootfs.sh";
        if (::access(script.c_str(), R_OK) == 0) return script;
    }
    // An installed package keeps the scripts in <prefix>/share/bifrost/setup.
    const std::string packaged = exe_dir + "/.." + kPackagedTree + "/Scripts/Setup/install-rootfs.sh";
    if (::access(packaged.c_str(), R_OK) == 0) return packaged;
    return {};
}

// Runs argv[0] (found on PATH) and returns its exit status, 127 if it cannot start.
static int run_command(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        fprintf(stderr, "bifrost-emu: --setup-rootfs: fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        ::execvp(argv[0], argv.data());
        fprintf(stderr, "bifrost-emu: --setup-rootfs: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int run_rootfs_setup(const std::vector<std::string>& args) {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string exe_dir;
    if (n > 0) {
        const std::string exe(buf, static_cast<size_t>(n));
        const size_t slash = exe.rfind('/');
        if (slash != std::string::npos) exe_dir = exe.substr(0, slash);
    }
    std::string script = find_rootfs_installer(exe_dir);
    if (script.empty()) {
        fprintf(stderr,
                "bifrost-emu: --setup-rootfs needs the Scripts/Setup/install-rootfs.sh of a bifrost-emu checkout,\n"
                "  or an installed package; neither was found next to %s. Run that script from the checkout instead.\n",
                exe_dir.empty() ? "the executable" : exe_dir.c_str());
        return 127;
    }
    // The scripts download and build next to themselves, so a packaged (read-only) copy runs from a
    // per-user cache. Copying again on each run keeps the scripts current; downloaded toolchains stay.
    const size_t packaged_at = script.find(kPackagedTree);
    if (packaged_at != std::string::npos) {
        const char* cache_home = getenv("XDG_CACHE_HOME");
        const char* home = getenv("HOME");
        std::string cache = cache_home && cache_home[0] == '/' ? cache_home
                          : home && home[0] == '/'             ? std::string(home) + "/.cache" : std::string();
        if (cache.empty()) {
            fprintf(stderr, "bifrost-emu: --setup-rootfs: HOME is not set, no place for the setup cache\n");
            return 1;
        }
        const std::string tree = script.substr(0, script.size() - strlen("/Scripts/Setup/install-rootfs.sh"));
        const std::string work = cache + "/bifrost/setup";
        if (run_command({"mkdir", "-p", work}) != 0 || run_command({"cp", "-a", tree + "/.", work + "/"}) != 0) {
            fprintf(stderr, "bifrost-emu: --setup-rootfs: cannot prepare %s\n", work.c_str());
            return 1;
        }
        script = work + "/Scripts/Setup/install-rootfs.sh";
    }
    std::vector<std::string> command{"bash", script};
    command.insert(command.end(), args.begin(), args.end());
    return run_command(command);
}

}  // namespace arm64emu
