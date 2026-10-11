// Write the Bionic property area Bifrost serves at /dev/__properties__ for
// an Android root, so reference runs (QEMU) can use identical properties.
// Usage: dump_android_properties <root> <output-file>
#include "System/Android/Properties/Properties.hpp"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <android-root> <output>\n", argv[0]);
        return 2;
    }
    size_t skipped = 0;
    const auto props = arm64emu::yggdrasil::load_android_properties(argv[1]);
    const std::string area = arm64emu::yggdrasil::build_property_area(props, &skipped);
    std::FILE* out = std::fopen(argv[2], "wb");
    if (!out || std::fwrite(area.data(), 1, area.size(), out) != area.size()) return 1;
    std::fclose(out);
    std::printf("%zu properties, %zu skipped\n", props.size() - skipped, skipped);
    return skipped ? 1 : 0;
}
