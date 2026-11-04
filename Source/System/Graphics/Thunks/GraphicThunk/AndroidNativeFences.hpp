// EGL_ANDROID_native_fence_sync across the guest/host descriptor boundary. The
// driver's fence is a host sync_file, while the guest only knows guest fd numbers.
#pragma once
#include <cstdint>
#include <optional>

namespace arm64emu {
class CPU;
class Memory;
struct SymbolEntry;

class AndroidNativeFences {
public:
    // eglDupNativeFenceFDANDROID, and eglCreateSync[KHR] importing a guest fence.
    std::optional<int64_t> dispatch(CPU& cpu, const SymbolEntry& entry, Memory& mem);
};
}  // namespace arm64emu
