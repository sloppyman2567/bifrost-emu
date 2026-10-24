// RootfsSetup.cpp — locate and run the rootfs installer for `--setup-rootfs`.
#include "System/Loader/RootfsSetup.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace arm64emu {

std::string find_rootfs_installer(const std::string& exe_dir) {
    // <checkout>/bifrost-emu, or <checkout>/build/<config>/bifrost-emu.
    for (const char* up : {"", "/..", "/../.."}) {
        const std::string script = exe_dir + up + "/Scripts/Setup/install-rootfs.sh";
        if (::access(script.c_str(), R_OK) == 0) return script;
    }
    return {};
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
    const std::string script = find_rootfs_installer(exe_dir);
    if (script.empty()) {
        fprintf(stderr,
                "bifrost-emu: --setup-rootfs needs the Scripts/Setup/install-rootfs.sh of a bifrost-emu checkout,\n"
                "  which was not found next to %s. Run that script from the checkout instead.\n",
                exe_dir.empty() ? "the executable" : exe_dir.c_str());
        return 127;
    }
    std::vector<std::string> argv_store{"bash", script};
    argv_store.insert(argv_store.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : argv_store) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        fprintf(stderr, "bifrost-emu: --setup-rootfs: fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        ::execvp("bash", argv.data());
        fprintf(stderr, "bifrost-emu: --setup-rootfs: cannot run bash: %s\n", strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

}  // namespace arm64emu
