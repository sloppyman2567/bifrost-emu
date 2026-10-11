// Library search, segment mapping, shared-library loading and thunk libraries.
#include "System/Loader/DynamicLinker.hpp"
#include "System/Core/CPU.hpp"
#include "System/Memory/Memory.hpp"
#include "Bifrost/Types.hpp"
#include "Bifrost/Version.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include "System/Loader/DynamicLinker/LinkerInternal.hpp"
namespace arm64emu {
// ── find_library ───────────────────────────────────────────────────────
// parent object's DT_RUNPATH/DT_RPATH) and search them BEFORE the
// standard multiarch paths. This lets games that bundle their own libs
// (via DT_RUNPATH=$ORIGIN/lib) actually find them.
std::vector<uint8_t> DynamicLinker::find_library(const std::string& soname,
                                                 std::string& found_path,
                                                 const std::string& parent_runpath,
                                                 const std::string& parent_rpath) {
    // Search order (matches Linux ld.so behavior for AArch64 multiarch):
    //   0. Parent object's DT_RPATH (deprecated, global) — searched FIRST
    //      per the gABI (only if no DT_RUNPATH was seen in any object).
    //      We approximate by searching it before RUNPATH.
    //   0.5. Parent object's DT_RUNPATH (per-object, $ORIGIN-expanded).
    //   1. BIFROST_ROOT sandbox (if set) — $BIFROST_ROOT/lib and
    //      $BIFROST_ROOT/usr/lib. This lets the user provide a self-
    //      contained rootfs without polluting the host's multiarch dirs.
    //   2. LD_LIBRARY_PATH entries (user override).
    //   3. /usr/aarch64-linux-gnu/lib/         (Debian/Ubuntu multiarch)
    //   4. /usr/lib/aarch64-linux-gnu/         (newer Debian multiarch)
    //   5. /lib/aarch64-linux-gnu/             (Debian multiarch)
    //   6. /usr/lib/                            (host libs, fallback)
    //   7. /lib
    //   8. Bundled toolchain libs (auto-detected at startup, see below)
    //
    // The bundled toolchain paths (./Tools/aarch64-linux-gnu-cross/...
    // and ./Tools/aarch64-linux-musl-cross/...) are checked LAST so the
    // user can override with BIFROST_ROOT or LD_LIBRARY_PATH. They're
    // included so dynamically-linked test binaries work out-of-the-box
    // after fetching the toolchains, without requiring the user to set
    // up a rootfs or install aarch64 multiarch packages on the host.
    std::vector<std::string> dirs;
    // 0. Parent's DT_RPATH (semicolon-separated).
    if (!parent_rpath.empty()) {
        std::string s = parent_rpath;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t colon = s.find(':', pos);
            if (colon == std::string::npos) {
                dirs.push_back(s.substr(pos));
                break;
            }
            dirs.push_back(s.substr(pos, colon - pos));
            pos = colon + 1;
        }
    }
    // 0.5. Parent's DT_RUNPATH (semicolon-separated).
    if (!parent_runpath.empty()) {
        std::string s = parent_runpath;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t colon = s.find(':', pos);
            if (colon == std::string::npos) {
                dirs.push_back(s.substr(pos));
                break;
            }
            dirs.push_back(s.substr(pos, colon - pos));
            pos = colon + 1;
        }
    }
    // 1. BIFROST_ROOT sandbox.
    if (const char* root = getenv("BIFROST_ROOT")) {
        std::string r(root);
        // Strip trailing slash(es) for clean concatenation.
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        if (!r.empty()) {
            dirs.push_back(r + "/lib");
            dirs.push_back(r + "/lib64");
            dirs.push_back(r + "/usr/lib");
            dirs.push_back(r + "/usr/lib64");
            // Debian/Ubuntu multiarch triplet dirs (neverball's
            // libSDL2_ttf/vorbis/openhmd live here — without these the
            // loader fell through to host x86_64 libs or empty thunks
            // and the game died calling NULL).
            dirs.push_back(r + "/lib/aarch64-linux-gnu");
            dirs.push_back(r + "/usr/lib/aarch64-linux-gnu");
        }
    }
    // 2. LD_LIBRARY_PATH.
    if (const char* llp = getenv("LD_LIBRARY_PATH")) {
        std::string s = llp;
        size_t pos = 0;
        while (pos < s.size()) {
            size_t colon = s.find(':', pos);
            if (colon == std::string::npos) {
                dirs.push_back(s.substr(pos));
                break;
            }
            dirs.push_back(s.substr(pos, colon - pos));
            pos = colon + 1;
        }
    }
    // 3-7. Standard host multiarch paths.
    dirs.push_back("/usr/aarch64-linux-gnu/lib");
    dirs.push_back("/usr/lib/aarch64-linux-gnu");
    dirs.push_back("/lib/aarch64-linux-gnu");
    dirs.push_back("/usr/lib");
    dirs.push_back("/lib");
    // 3.5. Android-compatible library paths.
    // Android games and Android-ported apps look for shared libraries
    // in /system/lib64 and /vendor/lib64. When BIFROST_ROOT is set,
    // these resolve to $BIFROST_ROOT/system/lib64 etc. (created by
    // setup-rootfs.sh as symlinks to ../lib64). Adding them here lets
    // Android-style DT_NEEDED entries (e.g., "libGLESv2.so") resolve
    // from the rootfs.
    if (const char* root = getenv("BIFROST_ROOT")) {
        std::string r(root);
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        if (!r.empty()) {
            dirs.push_back(r + "/system/lib");
            dirs.push_back(r + "/system/lib64");
            dirs.push_back(r + "/vendor/lib");
            dirs.push_back(r + "/vendor/lib64");
        }
    }
    // 8. Bundled toolchain libs (auto-detected relative to the
    //    executable's directory, so it works regardless of CWD).
    //    We use /proc/self/exe to find the executable's path, then
    //    look for Tools/aarch64-{linux-gnu,linux-musl}-cross/...
    //    relative to that.
    {
        char exe_path[4096];
        ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (n > 0) {
            exe_path[n] = '\0';
            std::string exe(exe_path);
            // Walk up to the project root (the directory containing
            // the 'Tools' subdir). The executable is typically at
            // <project>/bifrost-emu, so the project root is its parent.
            size_t slash = exe.rfind('/');
            if (slash != std::string::npos) {
                std::string project_root = exe.substr(0, slash);
                dirs.push_back(project_root +
                    "/Tools/aarch64-linux-gnu-cross/aarch64-none-linux-gnu/libc/lib64");
                dirs.push_back(project_root +
                    "/Tools/aarch64-linux-gnu-cross/aarch64-none-linux-gnu/libc/lib");
                dirs.push_back(project_root +
                    "/Tools/aarch64-linux-gnu-cross/aarch64-none-linux-gnu/libc/usr/lib64");
                dirs.push_back(project_root +
                    "/Tools/aarch64-linux-musl-cross/aarch64-linux-musl/lib");
            }
        }
    }
    for (const auto& dir : dirs) {
        std::string path = dir + "/" + soname;
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) continue;
            std::streamsize sz = f.tellg();
            f.seekg(0);
            std::vector<uint8_t> data(sz);
            if (!f.read(reinterpret_cast<char*>(data.data()), sz)) continue;
            found_path = path;
            if (dynlink_trace_enabled()) {
                fprintf(stderr, "[dynlink] found '%s' at '%s'\n",
                        soname.c_str(), path.c_str());
            }
            return data;
        }
    }
    return {};
}
// ── map_segments ───────────────────────────────────────────────────────
uint64_t DynamicLinker::map_segments(const std::vector<uint8_t>& data,
                                     uint64_t base, uint64_t& entry) {
    if (data.size() < 64) return 0;
    uint64_t e_entry, e_phoff;
    uint16_t e_phentsize, e_phnum;
    memcpy(&e_entry,     data.data() + 24, 8);
    memcpy(&e_phoff,     data.data() + 32, 8);
    memcpy(&e_phentsize, data.data() + 54, 2);
    memcpy(&e_phnum,     data.data() + 56, 2);
    uint64_t end_addr = base;
    entry = base + e_entry;
    for (int i = 0; i < e_phnum; i++) {
        if (e_phoff + (i + 1) * e_phentsize > data.size()) break;
        const uint8_t* p = data.data() + e_phoff + i * e_phentsize;
        uint32_t p_type;
        uint64_t p_offset, p_vaddr, p_filesz, p_memsz;
        memcpy(&p_type,   p + 0,  4);
        memcpy(&p_offset, p + 8,  8);
        memcpy(&p_vaddr,  p + 16, 8);
        memcpy(&p_filesz, p + 32, 8);
        memcpy(&p_memsz,  p + 40, 8);
        if (p_type != 1) continue;  // PT_LOAD
        uint64_t addr = base + p_vaddr;
        mem_.map_range(addr, p_memsz);
        if (p_filesz > 0 && p_offset + p_filesz <= data.size()) {
            mem_.write(addr, data.data() + p_offset, p_filesz);
        }
        uint64_t end = addr + p_memsz;
        if (end > end_addr) end_addr = end;
    }
    return end_addr;
}
// ── load_shared_library ────────────────────────────────────────────────
// find_library can search the parent object's DT_RUNPATH/DT_RPATH for
// this library. DT_RUNPATH only applies to the immediate object's
// DT_NEEDED per the gABI; DT_RPATH is global (deprecated but still used).
uint64_t DynamicLinker::load_shared_library(CPU& cpu,
                                             const std::string& soname,
                                             const std::string& parent_runpath,
                                             const std::string& parent_rpath) {
    std::string path;
    auto data = find_library(soname, path, parent_runpath, parent_rpath);
    // Host search paths often surface x86_64 libGL/libSDL2. Those must
    // NOT be mapped as guest code — prefer the GraphicThunk synthetic
    // object whenever the file is missing or not AArch64.
    constexpr uint16_t EM_AARCH64 = 183;
    bool aarch64_elf = data.size() >= 20
        && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F'
        && data[18] == (EM_AARCH64 & 0xFF) && data[19] == (EM_AARCH64 >> 8);
    if (!aarch64_elf) {
        if (thunk_resolver_ && is_thunk_supported_lib_(soname)) {
            return register_thunk_library_(soname);
        }
        return 0;
    }
    // Allocate a fresh base address via the Memory's mmap_alloc().
    // (mmap_alloc's base is MMAP_BASE_MIN in the low region — the SAME
    // region where the thunk trampolines and
    // string caches are allocated. The allocator tracks ALL allocations
    // so library bases can't collide with the trampolines; that was the
    // bug this comment documents: the old code used a separate counter
    // starting at 0x5000000000, which could collide with the thunk's
    // trampoline page → "decode error at pc=0x5000000020". The fix is
    // using mem_.mmap_alloc() for library bases.)
    uint64_t max_end = 0;
    if (data.size() >= 56) {
        uint64_t e_phoff;
        uint16_t e_phentsize, e_phnum;
        memcpy(&e_phoff,     data.data() + 32, 8);
        memcpy(&e_phentsize, data.data() + 54, 2);
        memcpy(&e_phnum,     data.data() + 56, 2);
        for (int i = 0; i < e_phnum; i++) {
            const uint8_t* p = data.data() + e_phoff + i * e_phentsize;
            uint32_t p_type;
            uint64_t p_vaddr, p_memsz;
            memcpy(&p_type,  p + 0,  4);
            memcpy(&p_vaddr, p + 16, 8);
            memcpy(&p_memsz, p + 40, 8);
            if (p_type == 1) {
                uint64_t end = p_vaddr + p_memsz;
                if (end > max_end) max_end = end;
            }
        }
    }
    // Allocate via mmap_alloc (page-aligned, collision-free with other
    // high-memory allocations like the thunk's trampoline page).
    // We round up to 1 MiB alignment to match the old behavior (libraries
    // are typically 1 MiB aligned in real ld.so).
    max_end = (max_end + 0xFFFFF) & ~0xFFFFFULL;  // 1 MiB align
    uint64_t base = mem_.mmap_alloc(max_end);
    if (base == 0) return 0;
    LoadedObject obj;
    obj.name = soname;
    obj.base_addr = base;
    obj.is_main = false;
    obj.map_size = max_end;  // for find_object_by_addr (dladdr)
    uint64_t entry;
    uint64_t end = map_segments(data, base, entry);
    obj.entry = entry;
    (void)end;
    if (!parse_dynamic(data, base, obj)) {
        return 0;
    }
    parse_tls(data, obj);
    parse_eh_frame(data, base, obj);
    objects_.push_back(std::move(obj));
    index_symbols(cpu, objects_.back());
    parse_versions_(cpu, objects_.back());
    return base;
}
// ── register_thunk_library_ ──────────────────────────────────
// Synthesize a LoadedObject for a graphic library that's supported by
// the thunk resolver but couldn't be loaded from disk. The object has
// no PT_LOAD segments, no PT_DYNAMIC, no PT_TLS — its only purpose is
// to provide symbol addresses via the global `symbols_` map.
//
// The thunk resolver is called once with `soname` and returns the full
// list of (symbol_name, guest_trampoline_addr) pairs it supports for
// that library. We insert each into the global symbol table.
uint64_t DynamicLinker::register_thunk_library_(const std::string& soname) {
    if (!thunk_resolver_) return 0;
    // Use a synthetic base address in a high region that won't collide
    // with real libraries. We don't actually map anything at this
    // address — it's just a sentinel for the LoadedObject record.
    // The "real" addresses live in the thunk's trampoline page.
    constexpr uint64_t THUNK_LIB_BASE = 0x6000000000ULL;
    constexpr uint64_t THUNK_LIB_STRIDE = 0x1000000ULL;  // 16m per lib
    // Unique synthetic base per thunk lib: sharing one base made
    // close/dlsym-by-handle always hit the first thunk lib.
    // map_size stays 0 (no mapping), so find_object_by_addr skips us.
    uint64_t thunk_base = THUNK_LIB_BASE + objects_.size() * THUNK_LIB_STRIDE;
    LoadedObject obj;
    obj.name = soname;
    obj.base_addr = thunk_base;  // synthetic; never dereferenced
    obj.is_main = false;
    obj.dyn_addr = 0;     // no PT_DYNAMIC
    obj.symtab_addr = 0;  // no .dynsym
    obj.strtab_addr = 0;  // no .dynstr
    objects_.push_back(std::move(obj));
    // Ask the thunk for all symbols it supports for this library.
    // The thunk is the single source of truth for its symbol inventory;
    // we don't need a hardcoded list of GL/EGL/SDL2 entry points here.
    ThunkSymbolList syms = thunk_resolver_(soname);
    size_t added = 0;
    for (const auto& [sym, addr] : syms) {
        if (addr == 0) continue;
        // First definition wins (matches the existing index_symbols
        // behavior). Don't override a strong symbol from a real lib.
        if (symbols_.count(sym) == 0) {
            symbols_[sym] = SymEntry{addr, STB_GLOBAL_};
            added++;
        }
    }
    if (dynlink_trace_enabled()) {
        fprintf(stderr, "[dynlink] registered thunk library '%s': "
                "%zu/%zu symbols\n",
                soname.c_str(), added, syms.size());
    }
    return thunk_base;
}
// ── is_thunk_supported_lib_ ────────────────────────────────────────────
// Returns true if `soname` matches the naming pattern of a library that
// the thunk resolver might handle. We accept:
//   - Graphics (GraphicThunk): libGL*, libEGL*, libSDL2*, libGLESv2*
//   - Audio (AudioThunk, 1.5.4-alpha): libasound*, libpulse*,
//     libopenal*
//   - Display (DisplayThunk, 1.5.4-alpha): libvulkan*, libwayland-*,
//     libX11*, libgbm*
// (libSDL2 is in both graphics and audio — both thunks will try to
// resolve its symbols, and the dispatcher tries graphics first.)
//
// The thunk itself does the final accept/reject — this is just a fast
// filter to avoid calling the resolver for libc/libm/etc.
bool DynamicLinker::is_thunk_supported_lib_(const std::string& soname) {
    auto starts_with = [](const std::string& s, const char* p) {
        return s.rfind(p, 0) == 0;
    };
    // Graphics (v1.4.5-alpha).
    if (starts_with(soname, "libGL.so")
        || starts_with(soname, "libEGL.so")
        || starts_with(soname, "libSDL2")
        || starts_with(soname, "libGLESv2.so")
        || starts_with(soname, "libglfw.so")) {
        return true;
    }
    // Audio (1.5.4-alpha).
    if (starts_with(soname, "libasound")
        || starts_with(soname, "libpulse")
        || starts_with(soname, "libopenal")) {
        return true;
    }
    // Android audio (1.5.5-alpha): AAudio + OpenSL ES.
    if (starts_with(soname, "libaaudio") || starts_with(soname, "libOpenSLES")) {
        return true;
    }
    // Display (1.5.4-alpha).
    if (starts_with(soname, "libvulkan")
        || starts_with(soname, "libwayland-")
        || starts_with(soname, "libX11")
        || starts_with(soname, "libgbm")) {
        return true;
    }
    // Android NativeActivity surface layer (2026-08-20): ANativeWindow
    // shims live in DisplayThunk under the ANDROID family. liblog.so
    // carries the __android_log_* family — same thunk, second soname.
    if (starts_with(soname, "libandroid.so") || starts_with(soname, "liblog.so")) {
        return true;
    }
    return false;
}
}  // namespace arm64emu
