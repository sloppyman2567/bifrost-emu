// See AndroidNativeFences.hpp. Contract: EGL_ANDROID_native_fence_sync
// (registry.khronos.org/EGL/extensions/ANDROID/EGL_ANDROID_native_fence_sync.txt).
#include "System/Graphics/Thunks/GraphicThunk/AndroidNativeFences.hpp"
#include "System/Android/Surface/Surface.hpp"
#include "System/Core/CPU.hpp"
#include "System/Graphics/Thunks/ThunkCommon.hpp"
#include "System/Memory/Memory.hpp"
#include "Bifrost/Types.hpp"
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

namespace arm64emu {
namespace {
constexpr uint64_t kSyncNativeFence = 0x3144;  // EGL_SYNC_NATIVE_FENCE_ANDROID
constexpr int64_t kSyncNativeFenceFd = 0x3145;  // EGL_SYNC_NATIVE_FENCE_FD_ANDROID
constexpr int64_t kNone = 0x3038;
constexpr int64_t kNoFence = -1;  // EGL_NO_NATIVE_FENCE_FD_ANDROID
constexpr int kMaxAttributes = 16;

void* host_proc(const char* name) {
    using Proc = void* (*)(const char*);
    static const auto proc = reinterpret_cast<Proc>(dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
    return proc ? proc(name) : nullptr;
}

int64_t dup_fence(uint64_t display, uint64_t sync) {
    using Dup = int (*)(uint64_t, uint64_t);
    static const auto host_dup = reinterpret_cast<Dup>(host_proc("eglDupNativeFenceFDANDROID"));
    const int fd = host_dup ? host_dup(display, sync) : -1;  // the driver reports its own error
    return fd < 0 ? kNoFence : frost::AndroidSurfaceManager::instance().publish_fd(fd);
}

// eglCreateSyncKHR takes EGLint attributes and eglCreateSync EGLAttrib ones. The
// driver would otherwise take a guest fd number as its own descriptor and close it.
template <typename Attribute>
std::optional<uint64_t> import_fence(uint64_t display, uint64_t attributes, Memory& mem, const char* name) {
    using Create = uint64_t (*)(uint64_t, uint64_t, const Attribute*);
    Attribute copy[kMaxAttributes * 2 + 1];
    int fence_index = -1;
    try {
        for (int i = 0;; i += 2) {
            if (i == kMaxAttributes * 2) return std::nullopt;
            copy[i] = mem.load<Attribute>(attributes + i * sizeof(Attribute));
            if (copy[i] == static_cast<Attribute>(kNone)) break;
            copy[i + 1] = mem.load<Attribute>(attributes + (i + 1) * sizeof(Attribute));
            if (copy[i] == static_cast<Attribute>(kSyncNativeFenceFd)) fence_index = i + 1;
        }
    } catch (const EmuError&) {
        return std::nullopt;  // the host path reports the unreadable list
    }
    if (fence_index < 0 || copy[fence_index] == static_cast<Attribute>(kNoFence)) return std::nullopt;

    auto& manager = frost::AndroidSurfaceManager::instance();
    const int guest_fd = static_cast<int>(copy[fence_index]);
    const int host_fd = manager.host_fd(guest_fd);
    const int owned = host_fd < 0 ? -1 : fcntl(host_fd, F_DUPFD_CLOEXEC, 0);
    const auto create = reinterpret_cast<Create>(host_proc(name));
    if (owned < 0 || !create) {
        if (owned >= 0) close(owned);
        return uint64_t{0};  // EGL_NO_SYNC
    }
    copy[fence_index] = static_cast<Attribute>(owned);
    const uint64_t sync = create(display, kSyncNativeFence, copy);
    if (sync) manager.close_fd(guest_fd);  // success takes ownership; the driver owns `owned`
    else close(owned);
    return sync;
}
}  // namespace

std::optional<int64_t> AndroidNativeFences::dispatch(CPU& cpu, const SymbolEntry& entry, Memory& mem) {
    const std::string& name = entry.name;
    if (name == "eglDupNativeFenceFDANDROID") {
        cpu.regs[0] = static_cast<uint64_t>(dup_fence(cpu.regs[0], cpu.regs[1]));
        return 0;
    }
    if (cpu.regs[1] != kSyncNativeFence || !cpu.regs[2]) return std::nullopt;
    std::optional<uint64_t> imported;
    if (name == "eglCreateSyncKHR") imported = import_fence<int32_t>(cpu.regs[0], cpu.regs[2], mem, "eglCreateSyncKHR");
    else if (name == "eglCreateSync") imported = import_fence<int64_t>(cpu.regs[0], cpu.regs[2], mem, "eglCreateSync");
    if (!imported) return std::nullopt;
    cpu.regs[0] = *imported;
    return 0;
}
}  // namespace arm64emu
