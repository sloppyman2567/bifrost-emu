// yggdrasil/android_properties.hpp — read-only Android system properties.
//
// On Android, init publishes system properties through shared memory that
// Bionic maps from /dev/__properties__. Bifrost plays init for Android
// runtime guests: it serializes the root's build.prop (plus defaults that
// device partitions would normally supply) into Bionic's single-file
// "pre-split" prop_area layout. Layout source: bionic android15-release
// libc/system_properties/{prop_area,prop_info}.{h,cpp}.
#pragma once
#include <map>
#include <string>

namespace arm64emu::yggdrasil {

// Merge "key=value" lines of build.prop text into `out` without replacing
// keys already present. Blank lines, comments and `import` are skipped.
void parse_build_prop(const std::string& text, std::map<std::string, std::string>& out);

// Serialize properties into a 128 KiB Bionic prop_area image. Properties
// that do not fit or have invalid names are skipped and counted in
// `skipped` (when non-null).
std::string build_property_area(const std::map<std::string, std::string>& props,
                                size_t* skipped = nullptr);

// Properties for an Android root: its /system/build.prop, then defaults
// for keys device partitions normally provide (ABI lists, native bridge).
std::map<std::string, std::string> load_android_properties(const std::string& root);

}  // namespace arm64emu::yggdrasil
