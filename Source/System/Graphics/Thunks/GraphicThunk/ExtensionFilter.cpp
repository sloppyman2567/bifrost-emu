// Guest-visible OpenGL ES extension list (see ExtensionFilter.hpp).
#include "System/Graphics/Thunks/GraphicThunk/ExtensionFilter.hpp"

#include "Generated/GLExtensions.hpp"

#include <dlfcn.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace arm64emu::gl_extensions {
namespace {

constexpr unsigned kVersion = 0x1F02;
constexpr unsigned kExtensions = 0x1F03;
using GetString = const unsigned char* (*)(unsigned);

GetString host_get_string() {
    static const GetString fn = [] {
        void* f = dlsym(RTLD_DEFAULT, "glGetString");
        using GetProc = void* (*)(const char*);
        if (!f) {
            if (auto proc = reinterpret_cast<GetProc>(dlsym(RTLD_DEFAULT, "eglGetProcAddress")))
                f = proc("glGetString");
        }
        return reinterpret_cast<GetString>(f);
    }();
    return fn;
}

bool hidden(std::string_view name) {
    const auto* begin = std::begin(thunk::kHiddenGlesExtensions);
    const auto* end = std::end(thunk::kHiddenGlesExtensions);
    const auto* it = std::lower_bound(begin, end, name,
        [](const char* entry, std::string_view key) { return std::string_view(entry) < key; });
    return it != end && name == *it;
}

struct List {
    std::string host;  // the host string this list was built from
    std::string joined;
    std::vector<std::string> names;
};

// Contexts are current per thread. Rebuild when the host list changes
// (another context made current on this thread).
const List* current() {
    GetString get = host_get_string();
    if (!get) return nullptr;
    const char* version = reinterpret_cast<const char*>(get(kVersion));
    if (!version || std::strncmp(version, "OpenGL ES", 9) != 0) return nullptr;
    const char* host = reinterpret_cast<const char*>(get(kExtensions));
    if (!host) return nullptr;
    thread_local List list;
    if (list.host != host) {
        list.host = host;
        list.joined.clear();
        list.names.clear();
        std::string_view rest(host);
        while (!rest.empty()) {
            size_t start = rest.find_first_not_of(' ');
            if (start == std::string_view::npos) break;
            rest.remove_prefix(start);
            std::string_view name = rest.substr(0, rest.find(' '));
            rest.remove_prefix(name.size());
            if (hidden(name)) continue;
            list.names.emplace_back(name);
            if (!list.joined.empty()) list.joined += ' ';
            list.joined += name;
        }
    }
    return &list;
}

}  // namespace

const char* filtered_string() {
    const List* list = current();
    return list ? list->joined.c_str() : nullptr;
}

bool filtered_at(uint32_t index, const char** name) {
    const List* list = current();
    if (!list) return false;
    *name = index < list->names.size() ? list->names[index].c_str() : nullptr;
    return true;
}

bool filtered_count(int32_t* count) {
    const List* list = current();
    if (!list) return false;
    *count = static_cast<int32_t>(list->names.size());
    return true;
}

}  // namespace arm64emu::gl_extensions
