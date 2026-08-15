// frost/display_thunk.hpp — DisplayThunk: forward guest display/Vulkan/
// Wayland/X11/GBM calls to host.
//
// 1.5.2-alpha: NEW. DisplayThunk handles the modern display stack:
//   - Vulkan (libvulkan.so): vkCreateInstance, vkCreateDevice,
//     vkCreateSwapchainKHR, vkQueuePresentKHR, vkAcquireNextImageKHR,
//     vkAllocateCommandBuffers, vkQueueSubmit, etc.
//   - Wayland (libwayland-client.so): wl_display_connect,
//     wl_display_disconnect, wl_display_dispatch, wl_proxy_marshal.
//   - X11 (libX11.so.6): XOpenDisplay, XCloseDisplay, XCreateWindow,
//     XMapWindow, XFlush, XPending, XNextEvent.
//   - XShm (libXext.so): XShmCreateImage, XShmAttach, XShmPutImage, etc.
//   - GLX (libGLX.so.2): glXChooseVisual, glXCreateContext, glXMakeCurrent.
//   - XRandR (libXrandr.so.2): XRRGetScreenResources, XRRGetCrtcInfo.
//   - Xkb (libXkblib.so): XkbGetMap, XkbGetState.
//   - GBM (libgbm.so.1): gbm_create_device, gbm_bo_create, gbm_bo_destroy.
//   - DMA-BUF (no library — ioctls on /dev/dri/card0).
//
// The thunk shares the __NR_bifrost_thunk syscall number with
// GraphicThunk and AudioThunk — the dispatcher routes by symbol_id,
// which is per-thunk.
//
// Limitations:
//   - Vulkan thunking is the hardest case because Vulkan passes around
//     opaque VkInstance/VkDevice/VkQueue handles (uint64_t on AArch64).
//     Translating these between guest and host requires a handle table
//     (guest handle → host handle). The thunk maintains this table
//     for vkCreateInstance/vkCreateDevice but NOT for every entry point.
//   - Wayland's wl_proxy_marshal is variadic — we thunk the fixed-arg
//     subset ( wl_display_connect, wl_display_disconnect, etc.) and
//     stub the rest.
//   - X11 pointer args (Display*, Window, GC) are translated via the
//     emulator's Memory. Works for heap-allocated structs; stack
//     structs with pointers to other stack structs may break.
//   - GBM/DMA-BUF require DRM fd translation — currently stubbed.
//
// Opt-in: BIFROST_THUNK_DISPLAY=1 or [thunk] display = true in config.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace arm64emu {
class Memory;
class CPU;
class DisplayProxy;
struct DisplayThunkImpl;
struct SymbolEntry;  // defined in frost_graphics/thunk_common.hpp
class DisplayThunk {
public:
    DisplayThunk();
    ~DisplayThunk();
    DisplayThunk(const DisplayThunk&) = delete;
    DisplayThunk& operator=(const DisplayThunk&) = delete;
    bool enabled() const;
    bool init(Memory& mem);
    uint64_t resolve(const std::string& lib, const std::string& sym);
    size_t enumerate_symbols(const std::string& lib,
        const std::function<void(const std::string&, uint64_t)>& cb) const;
    int64_t dispatch(CPU& cpu, uint32_t symbol_id);
    size_t symbol_count() const;
    uint64_t trampoline_base() const;
    static constexpr uint64_t SYSCALL_NUMBER = 0x1000;
    static constexpr uint64_t TRAMPOLINE_SIZE = 16;
    static constexpr uint64_t MAX_SYMBOLS = 2048;  // 32 KiB page
    static constexpr uint32_t ID_BASE = 0x2000;
    static constexpr uint32_t ID_MASK = 0x3000;
    // Dispatch flags (bitmask in the `flags` field of SymbolEntry).
    // These mirror GraphicThunk's flags so the same dispatch logic works.
    static constexpr uint8_t THUNK_RET_STRING    = 1u << 0;
    static constexpr uint8_t THUNK_SHADER_SOURCE = 1u << 1;
    static constexpr uint8_t THUNK_MIXED_FP      = 1u << 2;
    static constexpr uint8_t THUNK_GET_PROC      = 1u << 3;
    static constexpr uint8_t THUNK_PROXY         = 1u << 4;
    static constexpr uint8_t THUNK_VULKAN        = 1u << 5;
 private:
    std::unique_ptr<DisplayThunkImpl> impl_;
    void register_function_(const std::string& lib,
                            const std::string& sym,
                            void* host_fn,
                            uint16_t pointer_args = 0,
                            uint8_t n_stack = 0,
                            uint8_t n_float = 0,
                            uint8_t flags = 0);
    void write_trampoline_(Memory& mem, uint64_t addr, uint32_t sym_id);
    void register_known_symbols_();
    uint64_t proxy_dispatch_(CPU& cpu, const std::string& sym_name);
    bool vk_dispatch_(CPU& cpu, const SymbolEntry& entry, bool trace);
    DisplayProxy* proxy();
};
} // namespace arm64emu
