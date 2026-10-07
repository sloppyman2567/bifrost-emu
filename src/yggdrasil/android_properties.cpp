// yggdrasil/android_properties.cpp — Bionic prop_area serializer.
#include "yggdrasil/android_properties.hpp"
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace arm64emu::yggdrasil {
namespace {

// prop_area header: bytes_used, serial, magic, version, reserved[28].
constexpr size_t kAreaSize = 128 * 1024;  // PA_SIZE without LARGE_SYSTEM_PROPERTY_NODE
constexpr size_t kHeaderSize = 4 * 4 + 28 * 4;
constexpr size_t kDataSize = kAreaSize - kHeaderSize;
constexpr uint32_t kMagic = 0x504f5250;
constexpr uint32_t kVersion = 0xfc6ed0ab;
// prop_trie_node: namelen, prop, left, right, children, name[].
constexpr uint32_t kNodeProp = 4, kNodeLeft = 8, kNodeRight = 12, kNodeChildren = 16;
constexpr uint32_t kNodeSize = 20;
// prop_info: serial, value[92] (or error_message[56] + offset), name[].
constexpr uint32_t kValueMax = 92;
constexpr uint32_t kInfoSize = 4 + kValueMax;
constexpr uint32_t kLongFlag = 1u << 16;
constexpr uint32_t kLongOffsetField = 4 + 56;
constexpr char kLongLegacyError[] = "Must use __system_property_read_callback() to read";

class AreaBuilder {
public:
    AreaBuilder() : image_(kAreaSize, 0) {
        // Root node at data offset 0, then the dirty-backup value area.
        used_ = kNodeSize + kValueMax;
    }

    bool add(const std::string& name, const std::string& value) {
        // Check space for the worst case first so a failed add leaves no
        // half-built nodes behind.
        const size_t need = name.size() * 2 + kNodeSize * 8 + kInfoSize + value.size() + 16;
        if (used_ + need > kDataSize) return false;
        uint32_t node = 0;
        size_t start = 0;
        while (true) {
            const size_t dot = name.find('.', start);
            const size_t len = (dot == std::string::npos ? name.size() : dot) - start;
            if (len == 0) return false;
            node = child(node, name.data() + start, static_cast<uint32_t>(len));
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        if (get(node + kNodeProp) != 0) return true;  // first definition wins
        set(node + kNodeProp, new_info(name, value));
        return true;
    }

    std::string finish() {
        set_raw(0, static_cast<uint32_t>(used_));
        set_raw(8, kMagic);
        set_raw(12, kVersion);
        return std::string(reinterpret_cast<const char*>(image_.data()), image_.size());
    }

private:
    std::vector<uint8_t> image_;
    size_t used_;

    uint32_t get(uint32_t off) const {
        uint32_t v;
        std::memcpy(&v, image_.data() + kHeaderSize + off, 4);
        return v;
    }
    void set(uint32_t off, uint32_t v) { std::memcpy(image_.data() + kHeaderSize + off, &v, 4); }
    void set_raw(size_t off, uint32_t v) { std::memcpy(image_.data() + off, &v, 4); }
    uint8_t* at(uint32_t off) { return image_.data() + kHeaderSize + off; }

    uint32_t allocate(size_t size) {
        const uint32_t off = static_cast<uint32_t>(used_);
        used_ += (size + 3) & ~size_t(3);
        return off;
    }

    uint32_t new_node(const char* name, uint32_t len) {
        const uint32_t off = allocate(kNodeSize + len + 1);
        set(off, len);
        std::memcpy(at(off + kNodeSize), name, len);
        return off;
    }

    // Bionic's cmp_prop_name: shorter names sort first, then strncmp.
    int compare(const char* name, uint32_t len, uint32_t node) {
        const uint32_t node_len = get(node);
        if (len != node_len) return len < node_len ? -1 : 1;
        return std::strncmp(name, reinterpret_cast<const char*>(at(node + kNodeSize)), len);
    }

    // Find or insert segment `name` in `parent`'s child binary tree.
    uint32_t child(uint32_t parent, const char* name, uint32_t len) {
        uint32_t current = get(parent + kNodeChildren);
        if (current == 0) {
            current = new_node(name, len);
            set(parent + kNodeChildren, current);
            return current;
        }
        while (true) {
            const int c = compare(name, len, current);
            if (c == 0) return current;
            const uint32_t link = current + (c < 0 ? kNodeLeft : kNodeRight);
            if (get(link) == 0) {
                const uint32_t created = new_node(name, len);
                set(link, created);
                return created;
            }
            current = get(link);
        }
    }

    uint32_t new_info(const std::string& name, const std::string& value) {
        const uint32_t off = allocate(kInfoSize + name.size() + 1);
        std::memcpy(at(off + kInfoSize), name.data(), name.size());
        if (value.size() < kValueMax) {
            set(off, static_cast<uint32_t>(value.size()) << 24);
            std::memcpy(at(off + 4), value.data(), value.size());
        } else {
            // Long value: stored separately; the offset is relative to the
            // prop_info, and the legacy buffer holds an error for old readers.
            const uint32_t long_off = allocate(value.size() + 1);
            std::memcpy(at(long_off), value.data(), value.size());
            set(off, static_cast<uint32_t>(sizeof(kLongLegacyError) - 1) << 24 | kLongFlag);
            std::memcpy(at(off + 4), kLongLegacyError, sizeof(kLongLegacyError));
            set(off + kLongOffsetField, long_off - off);
        }
        return off;
    }
};

bool valid_name(const std::string& name) {
    if (name.empty() || name.front() == '.' || name.back() == '.') return false;
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' ||
              c == '-' || c == '@' || c == ':')) return false;
    return name.find("..") == std::string::npos;
}

std::string read_root_file(const std::string& root, const char* relative) {
    // The provisioned root is trusted content, but refuse a symlinked file.
    const std::string path = root + relative;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return std::string();
    std::string text;
    char buf[8192];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0 && text.size() < (1u << 20))
        text.append(buf, static_cast<size_t>(n));
    ::close(fd);
    return text;
}

}  // namespace

void parse_build_prop(const std::string& text, std::map<std::string, std::string>& out) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        line = line.substr(first);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;  // includes `import` lines
        std::string key = line.substr(0, eq);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        out.emplace(key, line.substr(eq + 1));
    }
}

std::string build_property_area(const std::map<std::string, std::string>& props,
                                size_t* skipped) {
    AreaBuilder builder;
    size_t dropped = 0;
    for (const auto& [name, value] : props) {
        if (!valid_name(name) || !builder.add(name, value)) dropped++;
    }
    if (skipped) *skipped = dropped;
    return builder.finish();
}

std::map<std::string, std::string> load_android_properties(const std::string& root) {
    std::map<std::string, std::string> props;
    // init/property_service.cpp order: a later partition overrides an
    // earlier one. Vendor and ODM props describe the image's emulator
    // hardware, which Bifrost replaces, so they are not loaded. The debug
    // ramdisk file (init/debug_ramdisk.h) carries test overrides.
    for (const char* path : {"/system/build.prop", "/system_ext/etc/build.prop",
                             "/product/etc/build.prop", "/debug_ramdisk/adb_debug.prop"}) {
        std::map<std::string, std::string> file;
        parse_build_prop(read_root_file(root, path), file);
        for (auto& [key, value] : file) props.insert_or_assign(key, std::move(value));
    }
    // Values device/vendor partitions normally supply; the image wins.
    static const std::pair<const char*, const char*> kDefaults[] = {
        {"ro.product.cpu.abi", "arm64-v8a"},
        {"ro.product.cpu.abilist", "arm64-v8a"},
        {"ro.product.cpu.abilist64", "arm64-v8a"},
        {"ro.product.cpu.abilist32", ""},
        {"ro.dalvik.vm.native.bridge", "0"},
        {"ro.zygote", "zygote64"},
        {"ro.hardware", "bifrost"},
        {"ro.debuggable", "0"},
    };
    for (const auto& [key, value] : kDefaults) props.emplace(key, value);
    return props;
}

}  // namespace arm64emu::yggdrasil
