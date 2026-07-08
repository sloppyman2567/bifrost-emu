// frost_graphics/display_thunk.cpp — DisplayThunk implementation (v1.5.0.alpha).
//
// See include/frost/display_thunk.hpp for the design overview. This file
// implements the DisplayThunk class for Vulkan / Wayland / X11 / GBM.
#include "frost/display_thunk.hpp"
#include "frost/thunk.hpp"  // for SYSCALL_NUMBER
#include "thunk_common.hpp"

#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace arm64emu {

struct DisplayThunkImpl {
    bool   enabled = false;
    Memory* mem    = nullptr;
    bool   initialized = false;

    uint64_t trampoline_base = 0;
    static constexpr uint64_t TRAMPOLINE_PAGE_SIZE =
        DisplayThunk::TRAMPOLINE_SIZE * DisplayThunk::MAX_SYMBOLS;  // 32 KiB

    std::vector<ThunkLibTable> libs_;
    std::vector<std::pair<uint32_t, uint32_t>> id_to_idx_;

    // Vulkan handle table: guest VkInstance/VkDevice → host handle.
    // Real Vulkan handles are uint64_t on AArch64 (opaque pointers
    // cast to uint64_t). We maintain a 1:1 mapping.
    std::unordered_map<uint64_t, uint64_t> vk_handle_map_;
    std::mutex mu;
};

DisplayThunk::DisplayThunk() {
    impl_ = std::make_unique<DisplayThunkImpl>();
    // v1.5.0.alpha (Turn 74): display thunking enabled by default.
    // Set BIFROST_NO_THUNK_DISPLAY=1 to disable.
    const char* disable = getenv("BIFROST_NO_THUNK_DISPLAY");
    impl_->enabled = !(disable && disable[0] != '0');
    if (impl_->enabled) {
        if (getenv("BIFROST_THUNK_TRACE") || getenv("BIFROST_VERBOSE")) {
            fprintf(stderr, "[display-thunk] display API thunking enabled "
                    "(Vulkan/Wayland/X11/GBM → host, with fallback)\n");
        }
    }
}

DisplayThunk::~DisplayThunk() = default;

bool DisplayThunk::enabled() const { return impl_ && impl_->enabled; }

bool DisplayThunk::init(Memory& mem) {
    if (!impl_->enabled) return false;
    if (impl_->initialized) return true;

    std::lock_guard<std::mutex> g(impl_->mu);
    impl_->mem = &mem;
    impl_->trampoline_base = mem.mmap_alloc(DisplayThunkImpl::TRAMPOLINE_PAGE_SIZE);
    if (impl_->trampoline_base == 0) {
        fprintf(stderr, "[display-thunk] init: failed to allocate trampoline page\n");
        return false;
    }
    register_known_symbols_();
    impl_->initialized = true;

    if (getenv("BIFROST_THUNK_TRACE")) {
        fprintf(stderr, "[display-thunk] init: %zu symbols registered, "
                "trampoline_base=0x%llx\n",
                impl_->id_to_idx_.size(),
                static_cast<unsigned long long>(impl_->trampoline_base));
    }
    return true;
}

void DisplayThunk::register_function_(const std::string& lib,
                                        const std::string& sym,
                                        void* host_fn) {
    bool trace = (getenv("BIFROST_THUNK_TRACE") != nullptr);
    thunk_register(*impl_->mem, impl_->libs_, impl_->id_to_idx_,
                   impl_->trampoline_base, TRAMPOLINE_SIZE, MAX_SYMBOLS,
                   static_cast<uint16_t>(SYSCALL_NUMBER),
                   DisplayThunk::ID_BASE, trace,
                   lib, sym, host_fn);
}

void DisplayThunk::write_trampoline_(Memory& mem, uint64_t addr, uint32_t sym_id) {
    write_thunk_trampoline(mem, addr, sym_id,
                            static_cast<uint16_t>(SYSCALL_NUMBER));
}

uint64_t DisplayThunk::resolve(const std::string& lib, const std::string& sym) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return 0;
    std::lock_guard<std::mutex> g(impl_->mu);
    auto* lt = find_lib(impl_->libs_, lib);
    if (!lt) return 0;
    for (const auto& e : lt->entries) {
        if (e.name == sym) return e.guest_addr;
    }
    return 0;
}

size_t DisplayThunk::enumerate_symbols(const std::string& lib,
    const std::function<void(const std::string&, uint64_t)>& cb) const {
    if (!impl_ || !impl_->enabled || !impl_->initialized) return 0;
    std::lock_guard<std::mutex> g(impl_->mu);
    auto* lt = find_lib(const_cast<std::vector<ThunkLibTable>&>(impl_->libs_), lib);
    if (!lt) return 0;
    for (const auto& e : lt->entries) {
        cb(e.name, e.guest_addr);
    }
    return lt->entries.size();
}

int64_t DisplayThunk::dispatch(CPU& cpu, uint32_t symbol_id) {
    if (!impl_ || !impl_->enabled || !impl_->initialized) {
        return -ENOSYS;
    }
    // v1.5.0.alpha (Turn 74): check ID range to route correctly.
    if ((symbol_id & DisplayThunk::ID_MASK) != DisplayThunk::ID_BASE) {
        return -ENOENT;  // belongs to a different thunk
    }
    uint32_t local_id = symbol_id - DisplayThunk::ID_BASE;
    if (local_id >= impl_->id_to_idx_.size()) {
        return -ENOENT;
    }
    auto [lib_idx, ent_idx] = impl_->id_to_idx_[local_id];
    const auto& entry = impl_->libs_[lib_idx].entries[ent_idx];
    bool trace = (getenv("BIFROST_THUNK_TRACE") != nullptr);
    return thunk_dispatch_generic(cpu, entry.host_fn, entry.name, trace);
}

size_t DisplayThunk::symbol_count() const {
    if (!impl_) return 0;
    return impl_->id_to_idx_.size();
}

uint64_t DisplayThunk::trampoline_base() const {
    if (!impl_) return 0;
    return impl_->trampoline_base;
}

// ── register_known_symbols_ ────────────────────────────────────────────
void DisplayThunk::register_known_symbols_() {
    // ── libvulkan.so.1 ─────────────────────────────────────────────
    const char* vk_libs[] = {"libvulkan.so.1", "libvulkan.so"};
    void* vk_handle = dlopen("libvulkan.so.1", RTLD_LAZY);
    if (!vk_handle) vk_handle = dlopen("libvulkan.so", RTLD_LAZY);
    #define REG_VK(name) do { \
        void* p = vk_handle ? dlsym(vk_handle, #name) : nullptr; \
        for (const char* L : vk_libs) register_function_(L, #name, p); \
    } while(0)

    // Core instance/device functions.
    REG_VK(vkCreateInstance);
    REG_VK(vkDestroyInstance);
    REG_VK(vkEnumeratePhysicalDevices);
    REG_VK(vkGetPhysicalDeviceProperties);
    REG_VK(vkGetPhysicalDeviceFeatures);
    REG_VK(vkGetPhysicalDeviceMemoryProperties);
    REG_VK(vkGetPhysicalDeviceQueueFamilyProperties);
    REG_VK(vkCreateDevice);
    REG_VK(vkDestroyDevice);
    REG_VK(vkGetDeviceQueue);
    REG_VK(vkDeviceWaitIdle);
    REG_VK(vkQueueWaitIdle);

    // Swapchain (KHR extension — but the entry point symbols are
    // available without the KHR suffix in libvulkan.so.1).
    REG_VK(vkCreateSwapchainKHR);
    REG_VK(vkDestroySwapchainKHR);
    REG_VK(vkGetSwapchainImagesKHR);
    REG_VK(vkAcquireNextImageKHR);
    REG_VK(vkQueuePresentKHR);

    // Command buffers.
    REG_VK(vkCreateCommandPool);
    REG_VK(vkDestroyCommandPool);
    REG_VK(vkAllocateCommandBuffers);
    REG_VK(vkFreeCommandBuffers);
    REG_VK(vkBeginCommandBuffer);
    REG_VK(vkEndCommandBuffer);
    REG_VK(vkResetCommandBuffer);
    REG_VK(vkQueueSubmit);

    // Image / image views.
    REG_VK(vkCreateImage);
    REG_VK(vkDestroyImage);
    REG_VK(vkGetImageMemoryRequirements);
    REG_VK(vkBindImageMemory);
    REG_VK(vkCreateImageView);
    REG_VK(vkDestroyImageView);

    // Buffers.
    REG_VK(vkCreateBuffer);
    REG_VK(vkDestroyBuffer);
    REG_VK(vkGetBufferMemoryRequirements);
    REG_VK(vkBindBufferMemory);

    // Memory.
    REG_VK(vkAllocateMemory);
    REG_VK(vkFreeMemory);
    REG_VK(vkMapMemory);
    REG_VK(vkUnmapMemory);
    REG_VK(vkFlushMappedMemoryRanges);
    REG_VK(vkInvalidateMappedMemoryRanges);

    // Render pass / framebuffers.
    REG_VK(vkCreateRenderPass);
    REG_VK(vkDestroyRenderPass);
    REG_VK(vkCreateFramebuffer);
    REG_VK(vkDestroyFramebuffer);

    // Shaders / pipelines.
    REG_VK(vkCreateShaderModule);
    REG_VK(vkDestroyShaderModule);
    REG_VK(vkCreatePipelineCache);
    REG_VK(vkDestroyPipelineCache);
    REG_VK(vkCreateGraphicsPipelines);
    REG_VK(vkCreateComputePipelines);
    REG_VK(vkDestroyPipeline);
    REG_VK(vkCreatePipelineLayout);
    REG_VK(vkDestroyPipelineLayout);
    REG_VK(vkCreateDescriptorSetLayout);
    REG_VK(vkDestroyDescriptorSetLayout);
    REG_VK(vkAllocateDescriptorSets);
    REG_VK(vkFreeDescriptorSets);
    REG_VK(vkUpdateDescriptorSets);
    REG_VK(vkCreateDescriptorPool);
    REG_VK(vkDestroyDescriptorPool);

    // Fences / semaphores / events.
    REG_VK(vkCreateFence);
    REG_VK(vkDestroyFence);
    REG_VK(vkResetFences);
    REG_VK(vkGetFenceStatus);
    REG_VK(vkWaitForFences);
    REG_VK(vkCreateSemaphore);
    REG_VK(vkDestroySemaphore);
    REG_VK(vkCreateEvent);
    REG_VK(vkDestroyEvent);
    REG_VK(vkSetEvent);
    REG_VK(vkResetEvent);

    // Query pools.
    REG_VK(vkCreateQueryPool);
    REG_VK(vkDestroyQueryPool);
    REG_VK(vkGetQueryPoolResults);

    // Sampler.
    REG_VK(vkCreateSampler);
    REG_VK(vkDestroySampler);

    // vkGetInstanceProcAddr / vkGetDeviceProcAddr (essential for
    // extension loading).
    REG_VK(vkGetInstanceProcAddr);
    REG_VK(vkGetDeviceProcAddr);
    #undef REG_VK

    // ── libwayland-client.so.0 ────────────────────────────────────
    const char* wl_libs[] = {"libwayland-client.so.0", "libwayland-client.so"};
    void* wl_handle = dlopen("libwayland-client.so.0", RTLD_LAZY);
    if (!wl_handle) wl_handle = dlopen("libwayland-client.so", RTLD_LAZY);
    #define REG_WL(name) do { \
        void* p = wl_handle ? dlsym(wl_handle, #name) : nullptr; \
        for (const char* L : wl_libs) register_function_(L, #name, p); \
    } while(0)

    REG_WL(wl_display_connect);
    REG_WL(wl_display_connect_to_fd);
    REG_WL(wl_display_disconnect);
    REG_WL(wl_display_get_fd);
    REG_WL(wl_display_dispatch);
    REG_WL(wl_display_dispatch_pending);
    REG_WL(wl_display_dispatch_queue);
    REG_WL(wl_display_roundtrip);
    REG_WL(wl_display_flush);
    REG_WL(wl_display_read_events);
    REG_WL(wl_display_prepare_read);
    REG_WL(wl_display_cancel_read);
    REG_WL(wl_proxy_marshal);
    REG_WL(wl_proxy_create);
    REG_WL(wl_proxy_destroy);
    REG_WL(wl_proxy_get_user_data);
    REG_WL(wl_proxy_set_user_data);
    REG_WL(wl_proxy_get_id);
    REG_WL(wl_proxy_get_class);
    REG_WL(wl_proxy_add_listener);
    REG_WL(wl_proxy_get_listener);
    #undef REG_WL

    // ── libX11.so.6 ───────────────────────────────────────────────
    const char* x11_libs[] = {"libX11.so.6", "libX11.so"};
    void* x11_handle = dlopen("libX11.so.6", RTLD_LAZY);
    if (!x11_handle) x11_handle = dlopen("libX11.so", RTLD_LAZY);
    #define REG_X11(name) do { \
        void* p = x11_handle ? dlsym(x11_handle, #name) : nullptr; \
        for (const char* L : x11_libs) register_function_(L, #name, p); \
    } while(0)

    REG_X11(XOpenDisplay);
    REG_X11(XCloseDisplay);
    REG_X11(XCreateWindow);
    REG_X11(XDestroyWindow);
    REG_X11(XMapWindow);
    REG_X11(XUnmapWindow);
    REG_X11(XFlush);
    REG_X11(XSync);
    REG_X11(XPending);
    REG_X11(XNextEvent);
    REG_X11(XPeekEvent);
    REG_X11(XEventsQueued);
    REG_X11(XWindowEvent);
    REG_X11(XCheckWindowEvent);
    REG_X11(XMaskEvent);
    REG_X11(XCheckMaskEvent);
    REG_X11(XPutBackEvent);
    REG_X11(XSendEvent);
    REG_X11(XDisplayWidth);
    REG_X11(XDisplayHeight);
    REG_X11(XDisplayWidthMM);
    REG_X11(XDisplayHeightMM);
    REG_X11(DefaultRootWindow);
    REG_X11(BlackPixel);
    REG_X11(WhitePixel);
    REG_X11(XSetForeground);
    REG_X11(XSetBackground);
    REG_X11(XFillRectangle);
    REG_X11(XDrawRectangle);
    REG_X11(XDrawLine);
    REG_X11(XDrawPoint);
    REG_X11(XCopyArea);
    REG_X11(XCreateGC);
    REG_X11(XFreeGC);
    REG_X11(XCreatePixmap);
    REG_X11(XFreePixmap);
    REG_X11(XSetWindowBackground);
    REG_X11(XSetWindowBackgroundPixmap);
    REG_X11(XStoreName);
    REG_X11(XFetchName);
    REG_X11(XSetWMProtocols);
    REG_X11(XInternAtom);
    REG_X11(XGetAtomName);
    REG_X11(XCreateColormap);
    REG_X11(XFreeColormap);
    REG_X11(XAllocColor);
    REG_X11(XFreeColors);
    #undef REG_X11

    // ── libgbm.so.1 ───────────────────────────────────────────────
    const char* gbm_libs[] = {"libgbm.so.1", "libgbm.so"};
    void* gbm_handle = dlopen("libgbm.so.1", RTLD_LAZY);
    if (!gbm_handle) gbm_handle = dlopen("libgbm.so", RTLD_LAZY);
    #define REG_GBM(name) do { \
        void* p = gbm_handle ? dlsym(gbm_handle, #name) : nullptr; \
        for (const char* L : gbm_libs) register_function_(L, #name, p); \
    } while(0)

    REG_GBM(gbm_create_device);
    REG_GBM(gbm_device_destroy);
    REG_GBM(gbm_bo_create);
    REG_GBM(gbm_bo_destroy);
    REG_GBM(gbm_bo_get_width);
    REG_GBM(gbm_bo_get_height);
    REG_GBM(gbm_bo_get_stride);
    REG_GBM(gbm_bo_get_format);
    REG_GBM(gbm_bo_get_handle);
    REG_GBM(gbm_bo_map);
    REG_GBM(gbm_bo_unmap);
    REG_GBM(gbm_surface_create);
    REG_GBM(gbm_surface_destroy);
    REG_GBM(gbm_surface_lock_front_buffer);
    REG_GBM(gbm_surface_release_buffer);
    #undef REG_GBM
}

} // namespace arm64emu
