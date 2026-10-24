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

    // Named profiles live in rootfs.d/NAME beside the default rootfs.
    CHECK(valid_rootfs_profile_name("games") && valid_rootfs_profile_name("dev-2.x_y"));
    CHECK(!valid_rootfs_profile_name("") && !valid_rootfs_profile_name(".hidden") && !valid_rootfs_profile_name("..") &&
          !valid_rootfs_profile_name("a/b") && !valid_rootfs_profile_name("a b") && !valid_rootfs_profile_name(std::string(65, 'x')));
    const std::vector<std::string> games = default_rootfs_candidates("/opt/b", "/data", "/home/u", "games");
    CHECK(games.size() == 5);
    CHECK(games[0] == "/opt/b/rootfs.d/games" && games[1] == "/opt/b/../share/bifrost/rootfs.d/games");
    CHECK(games[2] == "/data/bifrost/rootfs.d/games" && games[4] == "/usr/share/bifrost/rootfs.d/games");
    unsetenv("BIFROST_ROOTFS_PROFILE");
    CHECK(requested_rootfs_profile().empty());
    setenv("BIFROST_ROOTFS_PROFILE", "default", 1);
    CHECK(requested_rootfs_profile().empty());
    setenv("BIFROST_ROOTFS_PROFILE", "games", 1);
    CHECK(requested_rootfs_profile() == "games");
    CHECK(rootfs_search_hint().find("profile 'games'") != std::string::npos &&
          rootfs_search_hint().find("--profile games") != std::string::npos);
    {   // Listing finds an installed profile under XDG_DATA_HOME and marks the selected one.
        const std::string data = dir + "/data";
        mkdir(data.c_str(), 0755); mkdir((data + "/bifrost").c_str(), 0755);
        mkdir((data + "/bifrost/rootfs.d").c_str(), 0755); mkdir((data + "/bifrost/rootfs.d/games").c_str(), 0755);
        mkdir((data + "/bifrost/rootfs.d/games/lib").c_str(), 0755);
        touch(data + "/bifrost/rootfs.d/games/lib/ld-linux-aarch64.so.1");
        mkdir((data + "/bifrost/rootfs.d/empty").c_str(), 0755);  // no loader: not a profile
        setenv("XDG_DATA_HOME", data.c_str(), 1);
        const auto profiles = list_rootfs_profiles();
        bool saw_games = false;
        for (const auto& p : profiles) {
            CHECK(p.name != "empty");
            if (p.name == "games") { saw_games = true; CHECK(p.active && p.path == data + "/bifrost/rootfs.d/games"); }
        }
        CHECK(saw_games && profiles[0].name == "default" && !profiles[0].active);
        // A selected profile that is not installed is still listed, with no path.
        setenv("BIFROST_ROOTFS_PROFILE", "missing", 1);
        bool saw_missing = false;
        for (const auto& p : list_rootfs_profiles()) if (p.name == "missing") { saw_missing = p.active && p.path.empty(); }
        CHECK(saw_missing);
        std::remove((data + "/bifrost/rootfs.d/games/lib/ld-linux-aarch64.so.1").c_str());
        rmdir((data + "/bifrost/rootfs.d/games/lib").c_str()); rmdir((data + "/bifrost/rootfs.d/games").c_str());
        rmdir((data + "/bifrost/rootfs.d/empty").c_str()); rmdir((data + "/bifrost/rootfs.d").c_str());
        rmdir((data + "/bifrost").c_str()); rmdir(data.c_str());
        unsetenv("XDG_DATA_HOME");
    }
    unsetenv("BIFROST_ROOTFS_PROFILE");

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
