// host_timezone: the host's zone as each distribution records it, from a fake root.
#include "System/Android/Properties/HostTimeZone.hpp"
#include "System/Android/Properties/Properties.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using arm64emu::yggdrasil::host_timezone;
using arm64emu::yggdrasil::timezone_name;

static int failures = 0;
static void expect(const std::string& got, const std::string& wanted, const char* what) {
    if (got == wanted) return;
    std::fprintf(stderr, "%s: got '%s', wanted '%s'\n", what, got.c_str(), wanted.c_str());
    ++failures;
}
static void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

int main() {
    char pattern[] = "/tmp/bifrost-timezone-XXXXXX";
    if (!mkdtemp(pattern)) return 1;
    const fs::path temporary = pattern;
    int next = 0;
    auto root = [&] { const fs::path path = temporary / std::to_string(next++); fs::create_directories(path / "etc"); return path; };

    expect(timezone_name(":America/New_York"), "America/New_York", "TZ with a colon");
    expect(timezone_name("/usr/share/zoneinfo/Asia/Tokyo"), "Asia/Tokyo", "TZ as a path");
    expect(timezone_name("/usr/share/zoneinfo/posix/Asia/Tokyo"), "Asia/Tokyo", "posix tree");
    expect(timezone_name("/nix/store/abc-tzdata/share/zoneinfo/Europe/Paris"), "Europe/Paris", "NixOS store");
    expect(timezone_name("Etc/GMT+5"), "Etc/GMT+5", "sign in a name");
    expect(timezone_name("EST5EDT,M3.2.0,M11.1.0"), "", "POSIX rule");
    expect(timezone_name("<+03>-3"), "", "POSIX quoted rule");
    expect(timezone_name("CET-1CEST"), "", "POSIX rule without a comma");
    expect(timezone_name("EST5"), "", "POSIX rule, standard time only");
    expect(timezone_name("UTC0"), "", "POSIX rule for UTC");
    expect(timezone_name("EST5EDT"), "EST5EDT", "tzdata zone that looks like a rule");
    expect(timezone_name("GMT+0"), "GMT+0", "numbered GMT alias");
    expect(timezone_name("NZ-CHAT"), "NZ-CHAT", "hyphen without a digit");
    expect(timezone_name("America/Port-au-Prince"), "America/Port-au-Prince", "hyphens in a city");
    expect(timezone_name("../../etc/passwd"), "", "path escape");
    expect(timezone_name("/etc/localtime"), "", "path outside zoneinfo");

    // A set TZ beats the files, an empty one means UTC, and a rule falls through to them.
    fs::path r = root();
    write(r / "etc/timezone", "Europe/Paris\n");
    expect(host_timezone("Asia/Tokyo", r), "Asia/Tokyo", "TZ first");
    expect(host_timezone("", r), "UTC", "empty TZ");
    expect(host_timezone("EST5EDT,M3.2.0", r), "Europe/Paris", "unusable TZ");
    expect(host_timezone("CET-1CEST", r), "Europe/Paris", "unusable TZ without a comma");

    // Most distributions: a symlink, absolute or relative, even when the zone itself is a link.
    r = root();
    write(r / "usr/share/zoneinfo/US/Eastern", "tzif");
    fs::create_directories(r / "usr/share/zoneinfo/America");
    fs::create_symlink("../US/Eastern", r / "usr/share/zoneinfo/America/New_York");
    fs::create_symlink("../usr/share/zoneinfo/America/New_York", r / "etc/localtime");
    expect(host_timezone(nullptr, r), "America/New_York", "the first link names the zone");

    // NixOS: /etc/localtime -> /etc/zoneinfo/X -> the store. The chain ends in a zoneinfo tree.
    r = root();
    write(r / "nix/store/abc-tzdata/share/zoneinfo/Europe/Paris", "tzif");
    fs::create_symlink(r / "nix/store/abc-tzdata/share/zoneinfo", r / "etc/zoneinfo");
    fs::create_symlink("zoneinfo/Europe/Paris", r / "etc/localtime");
    expect(host_timezone(nullptr, r), "Europe/Paris", "NixOS");

    // Debian and derivatives, with a comment; older RHEL and SUSE.
    r = root();
    write(r / "etc/localtime", "copy");
    write(r / "etc/timezone", "# set by hand\n\nAmerica/Chicago\n");
    expect(host_timezone(nullptr, r), "America/Chicago", "/etc/timezone");
    r = root();
    write(r / "etc/sysconfig/clock", "UTC=true\nZONE=\"Australia/Sydney\"\n");
    expect(host_timezone(nullptr, r), "Australia/Sydney", "RHEL clock");
    r = root();
    write(r / "etc/sysconfig/clock", "TIMEZONE=\"Europe/Berlin\"\n");
    expect(host_timezone(nullptr, r), "Europe/Berlin", "SUSE clock");

    // Alpine and containers: a copy, found by content. Region/City beats an alias.
    r = root();
    write(r / "etc/localtime", "tokyo-rules");
    write(r / "usr/share/zoneinfo/Japan", "tokyo-rules");
    write(r / "usr/share/zoneinfo/Asia/Tokyo", "tokyo-rules");
    write(r / "usr/share/zoneinfo/posix/Asia/Tokyo", "tokyo-rules");
    write(r / "usr/share/zoneinfo/Asia/Seoul", "seoul-rules!");
    expect(host_timezone(nullptr, r), "Asia/Tokyo", "copied localtime");

    // Nothing to go on, and a copy that matches no zone: Android's own GMT applies.
    expect(host_timezone(nullptr, root()), "", "no information");
    r = root();
    write(r / "etc/localtime", "unknown");
    write(r / "usr/share/zoneinfo/Asia/Tokyo", "tokyo-rules");
    expect(host_timezone(nullptr, r), "", "no matching zone");

    // Bionic's CachedProperty, which tzset uses, never looks a name up in an area whose serial is 0.
    const std::string area = arm64emu::yggdrasil::build_property_area({{"persist.sys.timezone", "Asia/Tokyo"}});
    uint32_t serial = 0;
    std::memcpy(&serial, area.data() + 4, sizeof(serial));
    if (!serial) { std::fputs("property area serial is 0\n", stderr); ++failures; }

    fs::remove_all(temporary);
    if (failures) return 1;
    std::puts("host_timezone: OK");
    return 0;
}
