#include "System/Loader/RootfsDiscovery.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "rootfs discovery check failed at %d: %s\n", __LINE__, #x); return 1; } } while (0)
using namespace arm64emu;
static void touch(const std::string& path) { if (FILE* f = std::fopen(path.c_str(), "w")) std::fclose(f); }
int main() {
    char tmpl[] = "/tmp/bifrost-rootfs-discovery.XXXXXX";
    CHECK(mkdtemp(tmpl));
    const std::string dir = tmpl;
    CHECK(!is_guest_rootfs("") && !is_guest_rootfs(dir));
    mkdir((dir + "/lib").c_str(), 0755);
    CHECK(!is_guest_rootfs(dir));  // a lib dir alone is not a rootfs
    touch(dir + "/lib/ld-musl-aarch64.so.1");
    CHECK(is_guest_rootfs(dir) && is_guest_rootfs(dir + "/"));
    std::remove((dir + "/lib/ld-musl-aarch64.so.1").c_str());
    touch(dir + "/lib/ld-linux-aarch64.so.1");
    CHECK(is_guest_rootfs(dir));

    // Most specific first: executable, user data, system.
    const std::vector<std::string> xdg = default_rootfs_candidates("/opt/b", "/data/", "/home/u");
    CHECK(xdg.size() == 5);
    CHECK(xdg[0] == "/opt/b/rootfs" && xdg[1] == "/opt/b/../share/bifrost/rootfs");
    CHECK(xdg[2] == "/data/bifrost/rootfs");
    CHECK(xdg[3] == "/usr/local/share/bifrost/rootfs" && xdg[4] == "/usr/share/bifrost/rootfs");
    // XDG_DATA_HOME must be absolute; otherwise fall back to ~/.local/share.
    CHECK(default_rootfs_candidates("/opt/b", "relative", "/home/u")[2] == "/home/u/.local/share/bifrost/rootfs");
    CHECK(default_rootfs_candidates("/opt/b", nullptr, "/home/u")[2] == "/home/u/.local/share/bifrost/rootfs");
    CHECK(default_rootfs_candidates("", nullptr, nullptr).size() == 2);

    // An explicit BIFROST_ROOT is the library root, even without a loader in it.
    setenv("BIFROST_ROOT", (dir + "//").c_str(), 1);
    CHECK(library_root() == dir);
    CHECK(rootfs_search_hint().find("--rootfs") != std::string::npos);

    std::remove((dir + "/lib/ld-linux-aarch64.so.1").c_str());
    rmdir((dir + "/lib").c_str());
    rmdir(dir.c_str());
    std::puts("rootfs discovery: all checks passed");
    return 0;
}
