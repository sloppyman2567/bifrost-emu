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
    // v1.5.0.alpha: display thunking enabled by default.
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
                                        void* host_fn,
                                        uint8_t pointer_args) {
    bool trace = (getenv("BIFROST_THUNK_TRACE") != nullptr);
    thunk_register(*impl_->mem, impl_->libs_, impl_->id_to_idx_,
                   impl_->trampoline_base, TRAMPOLINE_SIZE, MAX_SYMBOLS,
                   static_cast<uint16_t>(SYSCALL_NUMBER),
                   DisplayThunk::ID_BASE, trace,
                   lib, sym, host_fn, pointer_args);
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
    // v1.5.0.alpha: check ID range to route correctly.
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
    // Wayland/X11/Vulkan functions often take pointer args that need
    // guest→host translation.
    return thunk_dispatch_with_ptrs(cpu, entry.host_fn, entry.name,
                                     entry.pointer_args, impl_->mem, trace);
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
    // Wayland functions take pointer args (const char* name, wl_proxy*,
    // wl_listener*, void* impl, etc.) that need guest→host translation.
    const char* wl_libs[] = {"libwayland-client.so.0", "libwayland-client.so"};
    void* wl_handle = dlopen("libwayland-client.so.0", RTLD_LAZY);
    if (!wl_handle) wl_handle = dlopen("libwayland-client.so", RTLD_LAZY);
    #define REG_WL(name) do { \
        void* p = wl_handle ? dlsym(wl_handle, #name) : nullptr; \
        for (const char* L : wl_libs) register_function_(L, #name, p); \
    } while(0)
    #define REG_WL_PTR(name, ptrs) do { \
        void* p = wl_handle ? dlsym(wl_handle, #name) : nullptr; \
        for (const char* L : wl_libs) register_function_(L, #name, p, ptrs); \
    } while(0)
    REG_WL_PTR(wl_display_connect, 0x01);       // arg 0: const char *name
    REG_WL(wl_display_connect_to_fd);            // arg 0: int fd (not pointer)
    REG_WL(wl_display_disconnect);               // arg 0: wl_display* (opaque handle, not translated)
    REG_WL(wl_display_get_fd);
    REG_WL(wl_display_dispatch);
    REG_WL(wl_display_dispatch_pending);
    REG_WL(wl_display_dispatch_queue);
    REG_WL_PTR(wl_display_dispatch_queue, 0x02); // arg 1: wl_event_queue*
    REG_WL_PTR(wl_display_roundtrip, 0x01);      // arg 0: wl_display*
    REG_WL_PTR(wl_display_flush, 0x01);          // arg 0: wl_display*
    REG_WL_PTR(wl_display_read_events, 0x01);    // arg 0: wl_display*
    REG_WL_PTR(wl_display_prepare_read, 0x01);   // arg 0: wl_display*
    REG_WL_PTR(wl_display_cancel_read, 0x01);    // arg 0: wl_display*
    REG_WL(wl_proxy_marshal);                     // variadic — can't thunk safely
    REG_WL_PTR(wl_proxy_create, 0x01);           // arg 0: wl_proxy* (factory)
    REG_WL_PTR(wl_proxy_destroy, 0x01);          // arg 0: wl_proxy*
    REG_WL_PTR(wl_proxy_get_user_data, 0x01);    // arg 0: wl_proxy*
    REG_WL_PTR(wl_proxy_set_user_data, 0x03);    // arg 0: wl_proxy*, arg 1: void*
    REG_WL_PTR(wl_proxy_get_id, 0x01);           // arg 0: wl_proxy*
    REG_WL_PTR(wl_proxy_get_class, 0x01);        // arg 0: wl_proxy*
    REG_WL_PTR(wl_proxy_add_listener, 0x03);     // arg 0: wl_proxy*, arg 1: void** impl
    REG_WL_PTR(wl_proxy_get_listener, 0x01);     // arg 0: wl_proxy*
    REG_WL(wl_proxy_marshal_constructor);         // variadic
    REG_WL(wl_proxy_marshal_constructor_versioned); // variadic
    REG_WL_PTR(wl_proxy_set_tag, 0x03);          // arg 0: wl_proxy*, arg 1: const char**
    REG_WL_PTR(wl_proxy_get_tag, 0x01);          // arg 0: wl_proxy*
    REG_WL_PTR(wl_proxy_wrapper_destroy, 0x01);  // arg 0: wl_proxy*
    REG_WL_PTR(wl_event_queue_destroy, 0x01);    // arg 0: wl_event_queue*
    #undef REG_WL
    #undef REG_WL_PTR
    // ── libwayland-egl.so.1 ──────────────────────────────────────
    const char* wl_egl_libs[] = {"libwayland-egl.so.1", "libwayland-egl.so"};
    void* wl_egl_handle = dlopen("libwayland-egl.so.1", RTLD_LAZY);
    if (!wl_egl_handle) wl_egl_handle = dlopen("libwayland-egl.so", RTLD_LAZY);
    #define REG_WL_EGL(name) do { \
        void* p = wl_egl_handle ? dlsym(wl_egl_handle, #name) : nullptr; \
        for (const char* L : wl_egl_libs) register_function_(L, #name, p); \
    } while(0)
    #define REG_WL_EGL_PTR(name, ptrs) do { \
        void* p = wl_egl_handle ? dlsym(wl_egl_handle, #name) : nullptr; \
        for (const char* L : wl_egl_libs) register_function_(L, #name, p, ptrs); \
    } while(0)
    REG_WL_EGL_PTR(wl_egl_window_create, 0x03);    // args: wl_surface*, int, int
    REG_WL_EGL_PTR(wl_egl_window_destroy, 0x01);   // arg 0: wl_egl_window*
    REG_WL_EGL_PTR(wl_egl_window_get_attached_size, 0x03); // arg 0: window, arg 1: int*, arg 2: int*
    REG_WL_EGL_PTR(wl_egl_window_resize, 0x01);    // arg 0: wl_egl_window*
    REG_WL_EGL_PTR(wl_egl_window_get_buffer_scale, 0x01);
    REG_WL_EGL_PTR(wl_egl_window_set_buffer_scale, 0x01);
    REG_WL_EGL_PTR(wl_egl_window_set_buffer_transform, 0x01);
    #undef REG_WL_EGL
    #undef REG_WL_EGL_PTR
    // ── libX11.so.6 ───────────────────────────────────────────────
    // X11 functions take pointer args (Display*, Window, GC, XEvent*,
    // char*, etc.) that need guest→host translation.
    const char* x11_libs[] = {"libX11.so.6", "libX11.so"};
    void* x11_handle = dlopen("libX11.so.6", RTLD_LAZY);
    if (!x11_handle) x11_handle = dlopen("libX11.so", RTLD_LAZY);
    #define REG_X11(name) do { \
        void* p = x11_handle ? dlsym(x11_handle, #name) : nullptr; \
        for (const char* L : x11_libs) register_function_(L, #name, p); \
    } while(0)
    #define REG_X11_PTR(name, ptrs) do { \
        void* p = x11_handle ? dlsym(x11_handle, #name) : nullptr; \
        for (const char* L : x11_libs) register_function_(L, #name, p, ptrs); \
    } while(0)
    REG_X11_PTR(XOpenDisplay, 0x01);             // arg 0: const char* name
    REG_X11_PTR(XCloseDisplay, 0x01);            // arg 0: Display*
    REG_X11_PTR(XCreateWindow, 0x80);            // arg 6: XSetWindowAttributes*
    REG_X11_PTR(XCreateSimpleWindow, 0x01);      // arg 0: Display*
    REG_X11_PTR(XDestroyWindow, 0x01);           // arg 0: Display*
    REG_X11_PTR(XMapWindow, 0x01);               // arg 0: Display*
    REG_X11_PTR(XUnmapWindow, 0x01);             // arg 0: Display*
    REG_X11_PTR(XFlush, 0x01);                   // arg 0: Display*
    REG_X11_PTR(XSync, 0x01);                    // arg 0: Display*
    REG_X11_PTR(XPending, 0x01);                 // arg 0: Display*
    REG_X11_PTR(XNextEvent, 0x03);               // arg 0: Display*, arg 1: XEvent*
    REG_X11_PTR(XPeekEvent, 0x03);               // arg 0: Display*, arg 1: XEvent*
    REG_X11_PTR(XEventsQueued, 0x01);            // arg 0: Display*
    REG_X11_PTR(XWindowEvent, 0x0B);             // arg 0: Display*, arg 2: XEvent*
    REG_X11_PTR(XCheckWindowEvent, 0x0B);        // arg 0: Display*, arg 3: XEvent*
    REG_X11_PTR(XMaskEvent, 0x03);               // arg 0: Display*, arg 2: XEvent*
    REG_X11_PTR(XCheckMaskEvent, 0x03);          // arg 0: Display*, arg 2: XEvent*
    REG_X11_PTR(XCheckTypedEvent, 0x03);         // arg 0: Display*, arg 2: XEvent*
    REG_X11_PTR(XCheckTypedWindowEvent, 0x0B);   // arg 0: Display*, arg 3: XEvent*
    REG_X11_PTR(XPutBackEvent, 0x03);            // arg 0: Display*, arg 1: XEvent*
    REG_X11_PTR(XSendEvent, 0x10);               // arg 0: Display*, arg 4: XEvent*
    REG_X11_PTR(XDisplayWidth, 0x01);            // arg 0: Display*
    REG_X11_PTR(XDisplayHeight, 0x01);           // arg 0: Display*
    REG_X11_PTR(XDisplayWidthMM, 0x01);          // arg 0: Display*
    REG_X11_PTR(XDisplayHeightMM, 0x01);         // arg 0: Display*
    REG_X11_PTR(DefaultRootWindow, 0x01);        // arg 0: Display*
    REG_X11_PTR(BlackPixel, 0x01);               // arg 0: Display*
    REG_X11_PTR(WhitePixel, 0x01);               // arg 0: Display*
    REG_X11_PTR(XSetForeground, 0x01);           // arg 0: Display*
    REG_X11_PTR(XSetBackground, 0x01);           // arg 0: Display*
    REG_X11_PTR(XFillRectangle, 0x01);           // arg 0: Display*
    REG_X11_PTR(XDrawRectangle, 0x01);           // arg 0: Display*
    REG_X11_PTR(XDrawLine, 0x01);                // arg 0: Display*
    REG_X11_PTR(XDrawPoint, 0x01);               // arg 0: Display*
    REG_X11_PTR(XCopyArea, 0x01);                // arg 0: Display*
    REG_X11_PTR(XCreateGC, 0x05);                // arg 0: Display*, arg 2: XGCValues*
    REG_X11_PTR(XFreeGC, 0x01);                  // arg 0: Display*
    REG_X11_PTR(XCreatePixmap, 0x01);            // arg 0: Display*
    REG_X11_PTR(XFreePixmap, 0x01);              // arg 0: Display*
    REG_X11_PTR(XSetWindowBackground, 0x01);     // arg 0: Display*
    REG_X11_PTR(XSetWindowBackgroundPixmap, 0x01); // arg 0: Display*
    REG_X11_PTR(XStoreName, 0x03);               // arg 0: Display*, arg 2: const char*
    REG_X11_PTR(XFetchName, 0x07);               // arg 0: Display*, arg 2: char**
    REG_X11_PTR(XSetWMProtocols, 0x08);          // arg 0: Display*, arg 3: Atom*
    REG_X11_PTR(XInternAtom, 0x03);              // arg 0: Display*, arg 1: const char*
    REG_X11_PTR(XInternAtoms, 0x1F);             // arg 0: Display*, arg 1: char**, arg 4: Atom*
    REG_X11_PTR(XGetAtomName, 0x01);             // arg 0: Display*
    REG_X11_PTR(XCreateColormap, 0x01);          // arg 0: Display*
    REG_X11_PTR(XFreeColormap, 0x01);            // arg 0: Display*
    REG_X11_PTR(XAllocColor, 0x03);              // arg 0: Display*, arg 2: XColor*
    REG_X11_PTR(XFreeColors, 0x07);              // arg 0: Display*, arg 2: unsigned long*
    REG_X11_PTR(XSetClipMask, 0x01);             // arg 0: Display*
    REG_X11_PTR(XSetClipOrigin, 0x01);           // arg 0: Display*
    REG_X11_PTR(XCopyGC, 0x01);                  // arg 0: Display*
    REG_X11_PTR(XChangeGC, 0x05);                // arg 0: Display*, arg 2: XGCValues*
    REG_X11_PTR(XSetFunction, 0x01);             // arg 0: Display*
    REG_X11_PTR(XSetLineAttributes, 0x01);       // arg 0: Display*
    REG_X11_PTR(XSetDashes, 0x07);               // arg 0: Display*, arg 3: const char*
    REG_X11_PTR(XDrawString, 0x20);              // arg 0: Display*, arg 4: const char*
    REG_X11_PTR(XDrawImageString, 0x20);         // arg 0: Display*, arg 4: const char*
    REG_X11_PTR(XTextExtents, 0x90);             // arg 0: XFontStruct*, arg 1: const char*, arg 4: int*, arg 5: int*, arg 6: int*
    REG_X11_PTR(XLoadFont, 0x03);                // arg 0: Display*, arg 1: const char*
    REG_X11_PTR(XUnloadFont, 0x01);              // arg 0: Display*
    REG_X11_PTR(XQueryFont, 0x01);               // arg 0: Display*
    REG_X11_PTR(XFreeFont, 0x01);                // arg 0: Display*
    REG_X11_PTR(XListFonts, 0x1B);               // arg 0: Display*, arg 1: const char*, arg 3: char***
    REG_X11_PTR(XFreeFontNames, 0x01);           // arg 0: char**
    REG_X11_PTR(XCreateBitmapFromData, 0x08);    // arg 0: Display*, arg 3: const char*
    REG_X11_PTR(XCreatePixmapFromBitmapData, 0x01); // arg 0: Display*
    REG_X11_PTR(XQueryPointer, 0x3D);            // args: Display*, Window, and 5 pointer out-args
    REG_X11_PTR(XWarpPointer, 0x01);             // arg 0: Display*
    REG_X11_PTR(XGrabPointer, 0x01);             // arg 0: Display*
    REG_X11_PTR(XUngrabPointer, 0x01);           // arg 0: Display*
    REG_X11_PTR(XGrabKeyboard, 0x01);            // arg 0: Display*
    REG_X11_PTR(XUngrabKeyboard, 0x01);          // arg 0: Display*
    REG_X11_PTR(XBell, 0x01);                    // arg 0: Display*
    REG_X11_PTR(XScreenCount, 0x01);             // arg 0: Display*
    REG_X11_PTR(XSetInputFocus, 0x01);           // arg 0: Display*
    REG_X11_PTR(XGetInputFocus, 0x05);           // arg 0: Display*, arg 2: int*
    REG_X11_PTR(XChangeProperty, 0x80);          // arg 0: Display*, arg 6: const unsigned char*
    REG_X11_PTR(XGetWindowProperty, 0xFE);       // multiple pointer args
    REG_X11_PTR(XDeleteProperty, 0x01);          // arg 0: Display*
    REG_X11_PTR(XGetWindowAttributes, 0x07);     // arg 0: Display*, arg 2: XWindowAttributes*
    #undef REG_X11
    #undef REG_X11_PTR
    // ── libX11-xcb.so.1 ─────────────────────────────────────────
    const char* x11xcb_libs[] = {"libX11-xcb.so.1", "libX11-xcb.so"};
    void* x11xcb_handle = dlopen("libX11-xcb.so.1", RTLD_LAZY);
    if (!x11xcb_handle) x11xcb_handle = dlopen("libX11-xcb.so", RTLD_LAZY);
    #define REG_X11XCB(name) do { \
        void* p = x11xcb_handle ? dlsym(x11xcb_handle, #name) : nullptr; \
        for (const char* L : x11xcb_libs) register_function_(L, #name, p); \
    } while(0)
    REG_X11XCB(XGetXCBConnection);
    #undef REG_X11XCB
    // ── libxcb.so.1 ─────────────────────────────────────────────
    const char* xcb_libs[] = {"libxcb.so.1", "libxcb.so"};
    void* xcb_handle = dlopen("libxcb.so.1", RTLD_LAZY);
    if (!xcb_handle) xcb_handle = dlopen("libxcb.so", RTLD_LAZY);
    #define REG_XCB(name) do { \
        void* p = xcb_handle ? dlsym(xcb_handle, #name) : nullptr; \
        for (const char* L : xcb_libs) register_function_(L, #name, p); \
    } while(0)
    #define REG_XCB_PTR(name, ptrs) do { \
        void* p = xcb_handle ? dlsym(xcb_handle, #name) : nullptr; \
        for (const char* L : xcb_libs) register_function_(L, #name, p, ptrs); \
    } while(0)
    REG_XCB_PTR(xcb_connect, 0x02);              // arg 0: const char*, arg 1: int*
    REG_XCB(xcb_disconnect);
    REG_XCB(xcb_connection_has_error);
    REG_XCB_PTR(xcb_get_setup, 0x01);            // arg 0: xcb_connection_t*
    REG_XCB(xcb_setup_roots_iterator);
    REG_XCB(xcb_screen_allowed_depths_iterator);
    REG_XCB(xcb_depth_visuals_iterator);
    REG_XCB(xcb_generate_id);
    REG_XCB_PTR(xcb_create_window, 0x01);        // arg 0: xcb_connection_t*
    REG_XCB_PTR(xcb_create_window_checked, 0x01);
    REG_XCB_PTR(xcb_destroy_window, 0x01);
    REG_XCB_PTR(xcb_map_window, 0x01);
    REG_XCB_PTR(xcb_unmap_window, 0x01);
    REG_XCB_PTR(xcb_flush, 0x01);
    REG_XCB_PTR(xcb_get_file_descriptor, 0x01);
    REG_XCB_PTR(xcb_wait_for_event, 0x01);
    REG_XCB_PTR(xcb_poll_for_event, 0x01);
    REG_XCB_PTR(xcb_free, 0x01);
    REG_XCB(xcb_visualtype_get);
    #undef REG_XCB
    #undef REG_XCB_PTR
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
