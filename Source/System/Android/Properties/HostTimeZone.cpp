// HostTimeZone.cpp — find the host's time zone on any distribution.
//
// Only the name is wanted: Bionic and libcore look it up in the image's own
// tzdata. /etc/localtime is usually a symlink, but a copy on Alpine, in
// containers and on systems set up by hand; timedatectl needs systemd.
#include "System/Android/Properties/HostTimeZone.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace arm64emu::yggdrasil {
namespace {
namespace fs = std::filesystem;

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string trimmed(std::string value) {
    const char* blank = " \t\r\n\"'";
    const size_t first = value.find_first_not_of(blank);
    if (first == std::string::npos) return "";
    return value.substr(first, value.find_last_not_of(blank) - first + 1);
}

// /etc/timezone holds the name alone, after any comment lines.
std::string debian_zone(const fs::path& path) {
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
        line = trimmed(line);
        if (!line.empty() && line[0] != '#') return timezone_name(line);
    }
    return "";
}

// RHEL before systemd says ZONE="...", SUSE says TIMEZONE="...".
std::string sysconfig_zone(const fs::path& path) {
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
        for (const char* key : {"ZONE=", "TIMEZONE="})
            if (line.rfind(key, 0) == 0) return timezone_name(trimmed(line.substr(std::char_traits<char>::length(key))));
    }
    return "";
}

// A copied /etc/localtime: the zone whose file is identical. Region/City beats
// a bare alias, and the sorted walk keeps the answer stable.
std::string matching_zone(const fs::path& localtime, const fs::path& tree) {
    std::error_code error;
    const auto size = fs::file_size(localtime, error);
    if (error) return "";
    const std::string wanted = read_file(localtime);
    std::vector<std::string> matches;
    fs::recursive_directory_iterator walk(tree, fs::directory_options::skip_permission_denied, error);
    for (const fs::recursive_directory_iterator end; !error && walk != end; walk.increment(error)) {
        const std::string name = fs::relative(walk->path(), tree, error).string();
        if (error) return "";
        if (name == "posix" || name == "right") { walk.disable_recursion_pending(); continue; }
        if (name == "localtime" || name == "posixrules" || name == "Factory") continue;
        if (walk->is_regular_file(error) && walk->file_size(error) == size && read_file(walk->path()) == wanted)
            matches.push_back(name);
    }
    std::sort(matches.begin(), matches.end());
    const auto regional = std::find_if(matches.begin(), matches.end(), [](const std::string& name) {
        return name.find('/') != std::string::npos && name.rfind("Etc/", 0) != 0;
    });
    if (regional != matches.end()) return timezone_name(*regional);
    return matches.empty() ? "" : timezone_name(matches.front());
}
}  // namespace

std::string timezone_name(std::string value) {
    if (!value.empty() && value[0] == ':') value.erase(0, 1);
    const std::string marker = "zoneinfo/";
    if (const size_t at = value.rfind(marker); at != std::string::npos) value.erase(0, at + marker.size());
    for (const char* variant : {"posix/", "right/"})
        if (value.rfind(variant, 0) == 0) value.erase(0, std::char_traits<char>::length(variant));
    if (value.empty() || value[0] == '/' || value.size() > 64) return "";
    bool start = true;
    for (const char c : value) {
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool part = letter || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+';
        if (start ? !letter : !(part || c == '/')) return "";
        start = c == '/';
    }
    if (start) return "";
    // Outside these, a slashless name with a digit is a POSIX rule ("CET-1CEST"): Bionic would parse it, but
    // libcore knows only tzdata ids and would take GMT.
    if (value.find('/') == std::string::npos && value.find_first_of("0123456789") != std::string::npos) {
        static const char* const kNumberedZones[] = {"GMT0", "GMT+0", "GMT-0", "EST5EDT", "CST6CDT", "MST7MDT", "PST8PDT"};
        if (std::none_of(std::begin(kNumberedZones), std::end(kNumberedZones),
                         [&](const char* zone) { return value == zone; })) return "";
    }
    return value;
}

std::string host_timezone(const char* tz, const std::string& root) {
    // TZ="" means UTC to libc, whatever /etc/localtime says.
    if (tz) {
        if (!*tz) return "UTC";
        if (std::string name = timezone_name(tz); !name.empty()) return name;
    }
    const fs::path base = root.empty() ? fs::path("/") : fs::path(root);
    const fs::path localtime = base / "etc/localtime";
    std::error_code error;
    // The first link names the zone the user chose; following it to the end
    // can land on an alias. NixOS links through /etc/zoneinfo into the store.
    for (const fs::path& link : {fs::read_symlink(localtime, error), fs::canonical(localtime, error)}) {
        if (link.string().find("zoneinfo/") == std::string::npos) continue;
        if (std::string name = timezone_name(link.string()); !name.empty()) return name;
    }
    if (std::string name = debian_zone(base / "etc/timezone"); !name.empty()) return name;
    if (std::string name = sysconfig_zone(base / "etc/sysconfig/clock"); !name.empty()) return name;
    return matching_zone(localtime, base / "usr/share/zoneinfo");
}

}  // namespace arm64emu::yggdrasil
