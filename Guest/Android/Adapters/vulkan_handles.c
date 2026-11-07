// The command entry points that cannot be a generated shim: they create or
// destroy dispatchable objects, or carry them inside an array, so the module
// has to wrap/unwrap more than the first argument.
//
// Everything else (see the generated shims in build-android-adapters.py) is a
// straight forward to the host loader with x0 replaced by the host handle.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#include "vulkan_dispatchable.h"
#include "vulkan_native_buffer.h"

#define EXPORT __attribute__((visibility("default")))
#define HOST(name) ((PFN_##name)bifrost_host_vk(#name))

EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
        VkInstance instance, const VkAllocationCallbacks *allocator) {
    PFN_vkDestroyInstance destroy = HOST(vkDestroyInstance);
    const uint64_t host = bf_vk_unwrap(instance);
    if (destroy && host) destroy((VkInstance)(uintptr_t)host, allocator);
    bf_vk_drop((void *)instance);
}

// Never returns NULL on success; a NULL result means the guest is out of
// memory, which fails the whole call rather than handing the loader a handle
// it cannot dispatch.
static int wrap_physical_devices(VkPhysicalDevice *devices, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        void *object = bf_vk_wrap((uint64_t)(uintptr_t)devices[i]);
        if (!object) {
            for (uint32_t j = 0; j < i; ++j) bf_vk_drop(devices[j]);
            return 0;
        }
        devices[i] = (VkPhysicalDevice)object;
    }
    return 1;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
        VkInstance instance, uint32_t *count, VkPhysicalDevice *devices) {
    PFN_vkEnumeratePhysicalDevices enumerate = HOST(vkEnumeratePhysicalDevices);
    if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = enumerate(HOST_HANDLE(VkInstance, instance), count, devices);
    if (result == VK_SUCCESS && devices && count && *count &&
        !wrap_physical_devices(devices, *count))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    return result;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroups(
        VkInstance instance, uint32_t *count,
        VkPhysicalDeviceGroupProperties *groups) {
    PFN_vkEnumeratePhysicalDeviceGroups enumerate =
        HOST(vkEnumeratePhysicalDeviceGroups);
    if (!enumerate) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult result = enumerate(HOST_HANDLE(VkInstance, instance), count, groups);
    if (result != VK_SUCCESS || !groups || !count) return result;
    for (uint32_t i = 0; i < *count; ++i) {
        if (!groups[i].physicalDeviceCount) continue;
        if (!wrap_physical_devices(groups[i].physicalDevices,
                                   groups[i].physicalDeviceCount))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return result;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDeviceGroupsKHR(
        VkInstance instance, uint32_t *count,
        VkPhysicalDeviceGroupProperties *groups) {
    // The host loader exports the alias too, but the wrapping is ours.
    PFN_vkEnumeratePhysicalDeviceGroupsKHR enumerate =
        HOST(vkEnumeratePhysicalDeviceGroupsKHR);
    if (!enumerate) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult result = enumerate(HOST_HANDLE(VkInstance, instance), count, groups);
    if (result != VK_SUCCESS || !groups || !count) return result;
    for (uint32_t i = 0; i < *count; ++i) {
        if (!groups[i].physicalDeviceCount) continue;
        if (!wrap_physical_devices(groups[i].physicalDevices,
                                   groups[i].physicalDeviceCount))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return result;
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
        VkDevice device, uint32_t family, uint32_t index, VkQueue *queue) {
    PFN_vkGetDeviceQueue get_queue = HOST(vkGetDeviceQueue);
    if (!get_queue || !queue) return;
    VkQueue host = VK_NULL_HANDLE;
    get_queue(HOST_HANDLE(VkDevice, device), family, index, &host);
    void *object = host ? bf_vk_wrap((uint64_t)(uintptr_t)host) : NULL;
    *queue = (VkQueue)object;  // NULL stays NULL when the queue is absent
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
        VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue) {
    PFN_vkGetDeviceQueue2 get_queue = HOST(vkGetDeviceQueue2);
    if (!get_queue || !info || !queue) return;
    VkQueue host = VK_NULL_HANDLE;
    get_queue(HOST_HANDLE(VkDevice, device), info, &host);
    void *object = host ? bf_vk_wrap((uint64_t)(uintptr_t)host) : NULL;
    *queue = (VkQueue)object;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
        VkDevice device, const VkCommandBufferAllocateInfo *info,
        VkCommandBuffer *buffers) {
    PFN_vkAllocateCommandBuffers allocate = HOST(vkAllocateCommandBuffers);
    if (!allocate) return VK_ERROR_INITIALIZATION_FAILED;
    const VkDevice host_device = HOST_HANDLE(VkDevice, device);
    VkResult result = allocate(host_device, info, buffers);
    if (result != VK_SUCCESS || !info || !buffers) return result;
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
        void *object = bf_vk_wrap((uint64_t)(uintptr_t)buffers[i]);
        if (!object) {
            PFN_vkFreeCommandBuffers free_buffers = HOST(vkFreeCommandBuffers);
            for (uint32_t j = 0; j < i; ++j) bf_vk_drop(buffers[j]);
            if (free_buffers)
                free_buffers(host_device, info->commandPool, info->commandBufferCount,
                             buffers);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        buffers[i] = (VkCommandBuffer)object;
    }
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
        VkDevice device, VkCommandPool pool, uint32_t count,
        const VkCommandBuffer *buffers) {
    PFN_vkFreeCommandBuffers free_buffers = HOST(vkFreeCommandBuffers);
    if (!free_buffers || !buffers || !count) return;
    VkCommandBuffer *host = (VkCommandBuffer *)__builtin_alloca(
        sizeof(VkCommandBuffer) * count);
    for (uint32_t i = 0; i < count; ++i)
        host[i] = HOST_HANDLE(VkCommandBuffer, buffers[i]);
    free_buffers(HOST_HANDLE(VkDevice, device), pool, count, host);
    for (uint32_t i = 0; i < count; ++i) bf_vk_drop((void *)buffers[i]);
}

// Command buffers travel inside submit structs, so the shim's first-argument
// swap is not enough: the arrays have to be translated as well. Everything
// else in the struct is passed through untouched.
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
        VkQueue queue, uint32_t submit_count, const VkSubmitInfo *submits,
        VkFence fence) {
    PFN_vkQueueSubmit submit = HOST(vkQueueSubmit);
    if (!submit) return VK_ERROR_INITIALIZATION_FAILED;
    const VkSubmitInfo *translated = submits;
    if (submits && submit_count) {
        VkSubmitInfo *copy = (VkSubmitInfo *)__builtin_alloca(
            sizeof(VkSubmitInfo) * submit_count);
        for (uint32_t i = 0; i < submit_count; ++i) {
            copy[i] = submits[i];
            const uint32_t count = submits[i].commandBufferCount;
            if (!count || !submits[i].pCommandBuffers) continue;
            VkCommandBuffer *commands = (VkCommandBuffer *)__builtin_alloca(
                sizeof(VkCommandBuffer) * count);
            for (uint32_t j = 0; j < count; ++j)
                commands[j] = HOST_HANDLE(VkCommandBuffer,
                                          submits[i].pCommandBuffers[j]);
            copy[i].pCommandBuffers = commands;
        }
        translated = copy;
    }
    return submit(HOST_HANDLE(VkQueue, queue), submit_count, translated, fence);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(
        VkQueue queue, uint32_t submit_count, const VkSubmitInfo2 *submits,
        VkFence fence) {
    PFN_vkQueueSubmit2 submit = HOST(vkQueueSubmit2);
    if (!submit) return VK_ERROR_INITIALIZATION_FAILED;
    const VkSubmitInfo2 *translated = submits;
    if (submits && submit_count) {
        VkSubmitInfo2 *copy = (VkSubmitInfo2 *)__builtin_alloca(
            sizeof(VkSubmitInfo2) * submit_count);
        for (uint32_t i = 0; i < submit_count; ++i) {
            copy[i] = submits[i];
            const uint32_t count = submits[i].commandBufferInfoCount;
            if (!count || !submits[i].pCommandBufferInfos) continue;
            VkCommandBufferSubmitInfo *infos = (VkCommandBufferSubmitInfo *)
                __builtin_alloca(sizeof(VkCommandBufferSubmitInfo) * count);
            for (uint32_t j = 0; j < count; ++j) {
                infos[j] = submits[i].pCommandBufferInfos[j];
                infos[j].commandBuffer = HOST_HANDLE(
                    VkCommandBuffer, submits[i].pCommandBufferInfos[j].commandBuffer);
            }
            copy[i].pCommandBufferInfos = infos;
        }
        translated = copy;
    }
    return submit(HOST_HANDLE(VkQueue, queue), submit_count, translated, fence);
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(
        VkCommandBuffer command_buffer, uint32_t count,
        const VkCommandBuffer *buffers) {
    PFN_vkCmdExecuteCommands execute = HOST(vkCmdExecuteCommands);
    if (!execute) return;
    const VkCommandBuffer *translated = buffers;
    if (buffers && count) {
        VkCommandBuffer *copy = (VkCommandBuffer *)__builtin_alloca(
            sizeof(VkCommandBuffer) * count);
        for (uint32_t i = 0; i < count; ++i)
            copy[i] = HOST_HANDLE(VkCommandBuffer, buffers[i]);
        translated = copy;
    }
    execute(HOST_HANDLE(VkCommandBuffer, command_buffer), count, translated);
}

PFN_vkVoidFunction vulkan_handles_symbol(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkDestroyInstance"))
        return (PFN_vkVoidFunction)vkDestroyInstance;
    if (!strcmp(name, "vkEnumeratePhysicalDevices"))
        return (PFN_vkVoidFunction)vkEnumeratePhysicalDevices;
    if (!strcmp(name, "vkEnumeratePhysicalDeviceGroups"))
        return (PFN_vkVoidFunction)vkEnumeratePhysicalDeviceGroups;
    if (!strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR"))
        return (PFN_vkVoidFunction)vkEnumeratePhysicalDeviceGroupsKHR;
    if (!strcmp(name, "vkGetDeviceQueue"))
        return (PFN_vkVoidFunction)vkGetDeviceQueue;
    if (!strcmp(name, "vkGetDeviceQueue2"))
        return (PFN_vkVoidFunction)vkGetDeviceQueue2;
    if (!strcmp(name, "vkAllocateCommandBuffers"))
        return (PFN_vkVoidFunction)vkAllocateCommandBuffers;
    if (!strcmp(name, "vkFreeCommandBuffers"))
        return (PFN_vkVoidFunction)vkFreeCommandBuffers;
    if (!strcmp(name, "vkQueueSubmit"))
        return (PFN_vkVoidFunction)vkQueueSubmit;
    if (!strcmp(name, "vkQueueSubmit2"))
        return (PFN_vkVoidFunction)vkQueueSubmit2;
    if (!strcmp(name, "vkCmdExecuteCommands"))
        return (PFN_vkVoidFunction)vkCmdExecuteCommands;
    return NULL;
}
