// Internal interface between the guest Vulkan HAL module (vulkan_hal.c) and
// its VK_ANDROID_native_buffer implementation (vulkan_native_buffer.c).
#pragma once

#include <vulkan/vulkan_core.h>

// Resolve a host-loader symbol to its guest DisplayThunk trampoline; NULL when
// the host does not export it (the loader treats that as unsupported).
__attribute__((visibility("hidden"))) void *bifrost_host_vk(const char *name);

// The native-buffer entry points are driver-private: they are not in vk.xml,
// so the thunk table cannot provide them. NULL for any other name.
__attribute__((visibility("hidden")))
PFN_vkVoidFunction vulkan_native_buffer_symbol(const char *name);
