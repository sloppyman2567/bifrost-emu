// VK_ANDROID_native_buffer: the private extension Android's libvulkan loader
// needs from its driver to run the surface/swapchain itself.
//
// The loader owns the swapchain: it allocates gralloc buffers, locks them and
// hands the driver a native_handle_t per swapchain image through
// VkNativeBufferANDROID. This file makes those buffers usable by the host
// driver, which cannot import them (the software allocator is a sealed memfd,
// not a dma-buf):
//
//   vkCreateImage          strips VkNativeBufferANDROID, creates a normal host
//                          image, binds host memory and remembers the gralloc
//                          fd/metadata for that image.
//   vkAcquireImageANDROID  waits the loader's acquire fence, uploads the
//                          gralloc pixels through a host staging buffer, then
//                          signals the loader's semaphore (and its fence) with
//                          the same queue submission, so the app's first use is
//                          ordered after the upload. After the image's first
//                          release the upload is skipped: the host image
//                          already holds what the release wrote into the
//                          gralloc buffer, and nothing else writes that buffer.
//                          An empty submission signals the semaphore and fence.
//   vkQueueSignalReleaseImageANDROID
//                          submits the reverse copy on the app's queue after
//                          the loader's wait semaphores, waits for it, writes
//                          the pixels back into the gralloc mapping and returns
//                          an already-signaled native fence (-1).
//
// Every host call goes through the DisplayThunk, exactly like the rest of the
// module; no host pointer or host Vulkan handle is ever exposed to the loader.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_android.h>
#include "Bifrost/AndroidGralloc.h"
#include "vulkan_dispatchable.h"
#include "vulkan_native_buffer.h"

#define EXPORT __attribute__((visibility("default")))
#define HOST(name) ((PFN_##name)bifrost_host_vk(#name))

enum {
    VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID = 1000010000u,
    VK_STRUCTURE_TYPE_SWAPCHAIN_IMAGE_CREATE_INFO_ANDROID = 1000010001u,
};

// The private create-info the loader appends (vulkan_android.h). Only the
// leading fields matter here: the handle is a native_handle_t.
typedef struct VkNativeBufferANDROID {
    VkStructureType sType;
    const void *pNext;
    const uint32_t *handle;
    int stride;
    int format;
    int usage;
    uint64_t consumer;
    uint64_t producer;
} VkNativeBufferANDROID;

// native_handle_t is three ints followed by fds then ints.
struct native_handle_prefix {
    int version;
    int num_fds;
    int num_ints;
    int data[];
};

struct device_state {
    VkDevice device;
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t queue_family;
    bool queue_family_valid;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer commands;
    VkFence fence;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    void *staging_map;
    VkDeviceSize staging_size;
    struct device_state *next;
};

struct image_binding {
    VkImage image;
    struct device_state *device;
    VkDeviceMemory memory;
    void *map;
    size_t map_size;
    int32_t info[BF_GRALLOC_INTS];
    // The host image holds exactly the gralloc pixels: set when a release has written them, so the next acquire
    // needs no upload. Guarded by copy_mutex.
    bool host_current;
    struct image_binding *next;
};

static struct device_state *devices;
static struct image_binding *images;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
// The staging buffer, command buffer and fence are per device and shared by
// acquire and release; an app may present from a different thread than the one
// that acquires, and the loader calls both through us.
static pthread_mutex_t copy_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct device_state *find_device(VkDevice device) {
    for (struct device_state *d = devices; d; d = d->next)
        if (d->device == device) return d;
    return NULL;
}

static struct image_binding *find_image(VkImage image) {
    for (struct image_binding *b = images; b; b = b->next)
        if (b->image == image) return b;
    return NULL;
}

static int memory_type(const VkPhysicalDeviceMemoryProperties *memory,
                       uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memory->memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (memory->memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

// The gralloc format and the app's VkFormat must describe the same bytes; the
// copy is a raw row copy, so a mismatch would silently corrupt pixels.
static int format_matches(int gralloc_format, VkFormat format) {
    switch (gralloc_format) {
    case 1: case 2:  // RGBA_8888, RGBX_8888
        return format == VK_FORMAT_R8G8B8A8_UNORM ||
               format == VK_FORMAT_R8G8B8A8_SRGB;
    case 5:          // BGRA_8888
        return format == VK_FORMAT_B8G8R8A8_UNORM ||
               format == VK_FORMAT_B8G8R8A8_SRGB;
    case 4:          // RGB_565
        return format == VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 56:         // R8
        return format == VK_FORMAT_R8_UNORM;
    case 22:         // RGBA_FP16
        return format == VK_FORMAT_R16G16B16A16_SFLOAT;
    default:
        return 0;
    }
}

// Lazily create the queue/pool/fence used for the staging copies. The loader
// never calls our device entry points before the device exists, so the first
// native-buffer image is a safe place to do this.
static VkResult prepare_device(struct device_state *d) {
    if (d->pool) return VK_SUCCESS;
    if (!d->queue_family_valid) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetDeviceQueue get_queue = HOST(vkGetDeviceQueue);
    PFN_vkCreateCommandPool create_pool = HOST(vkCreateCommandPool);
    PFN_vkAllocateCommandBuffers allocate = HOST(vkAllocateCommandBuffers);
    PFN_vkCreateFence create_fence = HOST(vkCreateFence);
    if (!get_queue || !create_pool || !allocate || !create_fence)
        return VK_ERROR_INITIALIZATION_FAILED;
    get_queue(d->device, d->queue_family, 0, &d->queue);
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = d->queue_family,
    };
    VkResult result = create_pool(d->device, &pool_info, NULL, &d->pool);
    if (result != VK_SUCCESS) return result;
    VkCommandBufferAllocateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = d->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    result = allocate(d->device, &buffer_info, &d->commands);
    if (result != VK_SUCCESS) return result;
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    return create_fence(d->device, &fence_info, NULL, &d->fence);
}

static void release_staging(struct device_state *d) {
    PFN_vkUnmapMemory unmap = HOST(vkUnmapMemory);
    PFN_vkDestroyBuffer destroy_buffer = HOST(vkDestroyBuffer);
    PFN_vkFreeMemory free_memory = HOST(vkFreeMemory);
    if (d->staging_map && unmap) unmap(d->device, d->staging_memory);
    if (d->staging && destroy_buffer) destroy_buffer(d->device, d->staging, NULL);
    if (d->staging_memory && free_memory)
        free_memory(d->device, d->staging_memory, NULL);
    d->staging = VK_NULL_HANDLE;
    d->staging_memory = VK_NULL_HANDLE;
    d->staging_map = NULL;
    d->staging_size = 0;
}

static VkResult prepare_staging(struct device_state *d, VkDeviceSize size) {
    if (d->staging && d->staging_size >= size) return VK_SUCCESS;
    PFN_vkCreateBuffer create_buffer = HOST(vkCreateBuffer);
    PFN_vkGetBufferMemoryRequirements requirements =
        HOST(vkGetBufferMemoryRequirements);
    PFN_vkAllocateMemory allocate = HOST(vkAllocateMemory);
    PFN_vkBindBufferMemory fn_bind = HOST(vkBindBufferMemory);
    PFN_vkMapMemory fn_map = HOST(vkMapMemory);
    if (!create_buffer || !requirements || !allocate || !fn_bind || !fn_map)
        return VK_ERROR_INITIALIZATION_FAILED;
    release_staging(d);
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = create_buffer(d->device, &buffer_info, NULL, &d->staging);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements need;
    requirements(d->device, d->staging, &need);
    // The CPU reads every released frame back out of this buffer. Cached
    // system memory first: the first merely coherent type is often
    // device-local VRAM behind the BAR, where CPU reads are uncached and
    // slow (the mapping is the real memory once maps are placed).
    int type = memory_type(&d->memory, need.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                           VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type < 0)
        type = memory_type(&d->memory, need.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo memory_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = need.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    result = allocate(d->device, &memory_info, NULL, &d->staging_memory);
    if (result != VK_SUCCESS) return result;
    result = fn_bind(d->device, d->staging, d->staging_memory, 0);
    if (result != VK_SUCCESS) return result;
    result = fn_map(d->device, d->staging_memory, 0, VK_WHOLE_SIZE, 0,
                    &d->staging_map);
    if (result != VK_SUCCESS) return result;
    d->staging_size = need.size;
    return VK_SUCCESS;
}

// One staging copy plus the two layout transitions around it. `upload`
// direction: staging buffer -> image (acquire); otherwise image -> buffer.
static VkResult stage_copy(struct device_state *d, struct image_binding *b,
                           bool upload, const VkSemaphore *wait,
                           uint32_t wait_count, VkSemaphore signal,
                           VkFence loader_fence, VkExtent3D extent,
                           uint32_t row_length) {
    PFN_vkResetFences reset = HOST(vkResetFences);
    PFN_vkResetCommandBuffer reset_commands = HOST(vkResetCommandBuffer);
    PFN_vkBeginCommandBuffer begin = HOST(vkBeginCommandBuffer);
    PFN_vkEndCommandBuffer end = HOST(vkEndCommandBuffer);
    PFN_vkCmdPipelineBarrier barrier = HOST(vkCmdPipelineBarrier);
    PFN_vkCmdCopyBufferToImage fn_copy_to_image = HOST(vkCmdCopyBufferToImage);
    PFN_vkCmdCopyImageToBuffer fn_copy_to_buffer = HOST(vkCmdCopyImageToBuffer);
    PFN_vkQueueSubmit submit = HOST(vkQueueSubmit);
    PFN_vkWaitForFences wait_fence = HOST(vkWaitForFences);
    if (!reset || !reset_commands || !begin || !end || !barrier ||
        !fn_copy_to_image || !fn_copy_to_buffer || !submit || !wait_fence)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    reset_commands(d->commands, 0);
    VkResult result = begin(d->commands, &begin_info);
    if (result != VK_SUCCESS) return result;

    VkImageMemoryBarrier entry = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = b->image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VkImageMemoryBarrier exit = entry;
    if (upload) {
        entry.srcAccessMask = 0;
        entry.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        entry.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        entry.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        exit.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        exit.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                             VK_ACCESS_MEMORY_WRITE_BIT;
        exit.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        exit.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    } else {
        entry.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                              VK_ACCESS_MEMORY_WRITE_BIT;
        entry.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        entry.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        entry.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        exit.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        exit.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT |
                             VK_ACCESS_MEMORY_WRITE_BIT;
        exit.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        exit.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    barrier(d->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &entry);
    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = row_length,
        .bufferImageHeight = extent.height,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = extent,
    };
    if (upload)
        fn_copy_to_image(d->commands, d->staging, b->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    else
        fn_copy_to_buffer(d->commands, b->image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->staging, 1,
                          &region);
    barrier(d->commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &exit);
    result = end(d->commands);
    if (result != VK_SUCCESS) return result;

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = wait_count,
        .pWaitSemaphores = wait_count ? wait : NULL,
        .pWaitDstStageMask = wait_count ? &(VkPipelineStageFlags){
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT} : NULL,
        .commandBufferCount = 1,
        .pCommandBuffers = &d->commands,
        .signalSemaphoreCount = signal ? 1u : 0u,
        .pSignalSemaphores = signal ? &signal : NULL,
    };
    VkFence fence = loader_fence ? loader_fence : d->fence;
    if (!loader_fence) reset(d->device, 1, &fence);
    result = submit(d->queue, 1, &submit_info, fence);
    if (result != VK_SUCCESS) return result;
    // Always wait: the staging buffer is reused by the next acquire, so the
    // previous copy has to be finished before its contents are overwritten.
    // Waiting on the loader's fence does not consume it, so the loader's own
    // wait still succeeds.
    result = wait_fence(d->device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) return result;
    if (!loader_fence) reset(d->device, 1, &fence);
    return VK_SUCCESS;
}

// Signals the loader's semaphore and fence in queue order, with no copy.
static VkResult signal_only(struct device_state *d, VkSemaphore signal, VkFence loader_fence) {
    PFN_vkQueueSubmit submit = HOST(vkQueueSubmit);
    if (!submit) return VK_ERROR_INITIALIZATION_FAILED;
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = signal ? 1u : 0u,
        .pSignalSemaphores = signal ? &signal : NULL,
    };
    return submit(d->queue, 1, &submit_info, loader_fence);
}

// ── Entry points ─────────────────────────────────────────────────────────

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
        VkPhysicalDevice physical, const VkDeviceCreateInfo *create_info,
        const VkAllocationCallbacks *allocator, VkDevice *device) {
    PFN_vkCreateDevice create = HOST(vkCreateDevice);
    if (!create || !create_info || !device) return VK_ERROR_INITIALIZATION_FAILED;
    // The loader hands back the physical-device object this module created;
    // the host needs the handle inside it.
    const VkPhysicalDevice host_physical = HOST_HANDLE(VkPhysicalDevice, physical);
    // Device groups list their members as physical devices. Translating that
    // array is not implemented, and forwarding our objects would fail in the
    // host with no useful diagnostic.
    for (const VkBaseInStructure *base = create_info->pNext; base; base = base->pNext)
        if (base->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO)
            return VK_ERROR_FEATURE_NOT_PRESENT;
    // The host does not know the private extension; passing it through fails
    // the whole device creation.
    const char *names[64];
    uint32_t kept = 0;
    VkDeviceCreateInfo filtered = *create_info;
    for (uint32_t i = 0; i < create_info->enabledExtensionCount && kept < 64; ++i) {
        const char *name = create_info->ppEnabledExtensionNames[i];
        if (!name || strcmp(name, "VK_ANDROID_native_buffer")) names[kept++] = name;
    }
    filtered.enabledExtensionCount = kept;
    filtered.ppEnabledExtensionNames = kept ? names : NULL;
    VkDevice host_device = VK_NULL_HANDLE;
    VkResult result = create(host_physical, &filtered, allocator, &host_device);
    if (result != VK_SUCCESS) return result;
    void *object = bf_vk_wrap((uint64_t)(uintptr_t)host_device);
    if (!object) {
        PFN_vkDestroyDevice destroy = HOST(vkDestroyDevice);
        if (destroy) destroy(host_device, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *device = (VkDevice)object;

    struct device_state *state = calloc(1, sizeof(*state));
    if (!state) {
        PFN_vkDestroyDevice destroy = HOST(vkDestroyDevice);
        if (destroy) destroy(host_device, allocator);
        bf_vk_drop(object);
        *device = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    state->device = host_device;
    state->physical = host_physical;
    PFN_vkGetPhysicalDeviceMemoryProperties memory =
        HOST(vkGetPhysicalDeviceMemoryProperties);
    if (memory) memory(host_physical, &state->memory);
    state->queue_family_valid = create_info->queueCreateInfoCount > 0;
    if (state->queue_family_valid)
        state->queue_family = create_info->pQueueCreateInfos[0].queueFamilyIndex;
    pthread_mutex_lock(&registry);
    state->next = devices;
    devices = state;
    pthread_mutex_unlock(&registry);
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
        VkDevice device, const VkAllocationCallbacks *allocator) {
    PFN_vkDestroyFence destroy_fence = HOST(vkDestroyFence);
    PFN_vkDestroyCommandPool destroy_pool = HOST(vkDestroyCommandPool);
    PFN_vkDestroyDevice destroy = HOST(vkDestroyDevice);
    const VkDevice host_device = HOST_HANDLE(VkDevice, device);
    pthread_mutex_lock(&registry);
    struct device_state **link = &devices;
    while (*link && (*link)->device != host_device) link = &(*link)->next;
    struct device_state *state = *link;
    if (state) *link = state->next;
    // Bindings referencing this device cannot outlive it.
    struct image_binding **image_link = &images;
    while (*image_link) {
        if ((*image_link)->device == state) {
            struct image_binding *drop = *image_link;
            *image_link = drop->next;
            if (drop->map) munmap(drop->map, drop->map_size);
            free(drop);
        } else {
            image_link = &(*image_link)->next;
        }
    }
    pthread_mutex_unlock(&registry);
    if (state) {
        release_staging(state);
        if (destroy_fence && state->fence)
            destroy_fence(host_device, state->fence, NULL);
        if (destroy_pool && state->pool)
            destroy_pool(host_device, state->pool, NULL);
        free(state);
    }
    if (destroy) destroy(host_device, allocator);
    bf_vk_drop((void *)device);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
        VkPhysicalDevice physical, const char *layer_name, uint32_t *count,
        VkExtensionProperties *properties) {
    PFN_vkEnumerateDeviceExtensionProperties enumerate =
        HOST(vkEnumerateDeviceExtensionProperties);
    if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;
    const VkPhysicalDevice host_physical = HOST_HANDLE(VkPhysicalDevice, physical);
    static const VkExtensionProperties extra = {"VK_ANDROID_native_buffer", 1};
    uint32_t host_count = 0;
    VkResult result = enumerate(host_physical, layer_name, &host_count, NULL);
    if (result != VK_SUCCESS) return result;
    VkExtensionProperties *host_list = NULL;
    if (host_count) {
        host_list = (VkExtensionProperties *)malloc(
            (size_t)host_count * sizeof *host_list);
        if (!host_list) return VK_ERROR_OUT_OF_HOST_MEMORY;
        result = enumerate(host_physical, layer_name, &host_count, host_list);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            free(host_list);
            return result;
        }
    }
    // Advertise only what this driver can answer for. An extension whose
    // command has no host symbol gets no entry point from the loader, so an
    // app that enabled it calls a NULL dispatch slot (Unity does exactly that
    // for VK_KHR_fragment_shading_rate once the host stops exporting it).
    uint32_t servable = 0;
    for (uint32_t i = 0; i < host_count; ++i)
        if (bf_vk_extension_servable(host_list[i].extensionName)) ++servable;
    const uint32_t total = servable + 1u;  // plus VK_ANDROID_native_buffer
    if (!properties) {
        if (count) *count = total;
        free(host_list);
        return VK_SUCCESS;
    }
    if (!count) {
        free(host_list);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const uint32_t capacity = *count;
    uint32_t written = 0;
    for (uint32_t i = 0; i < host_count && written < capacity; ++i)
        if (bf_vk_extension_servable(host_list[i].extensionName))
            properties[written++] = host_list[i];
    if (written < capacity) properties[written++] = extra;
    free(host_list);
    *count = written;
    // No room in the caller's array: the app re-queries with the larger count.
    return written < total ? VK_INCOMPLETE : VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
        VkDevice device, const VkImageCreateInfo *create_info,
        const VkAllocationCallbacks *allocator, VkImage *image) {
    PFN_vkCreateImage create = HOST(vkCreateImage);
    if (!create || !create_info) return VK_ERROR_INITIALIZATION_FAILED;
    const VkDevice host = HOST_HANDLE(VkDevice, device);
    const VkNativeBufferANDROID *native = NULL;
    const void *next = create_info->pNext;
    if (next && ((const VkBaseInStructure *)next)->sType ==
                    VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID) {
        native = (const VkNativeBufferANDROID *)next;
        next = native->pNext;
    }
    // The host takes its own handle, never the guest's wrapper object.
    if (!native) return create(host, create_info, allocator, image);

    struct device_state *state = find_device(host);
    if (!state) return VK_ERROR_INITIALIZATION_FAILED;
    const struct native_handle_prefix *handle =
        (const struct native_handle_prefix *)native->handle;
    if (!handle || handle->version != (int)sizeof(struct native_handle_prefix) ||
        handle->num_fds != 1 || handle->num_ints < BF_GRALLOC_INTS)
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    const int32_t *info = handle->data + 1;
    if (info[BF_MAGIC] != BF_GRALLOC_MAGIC ||
        info[BF_WIDTH] != (int32_t)create_info->extent.width ||
        info[BF_HEIGHT] != (int32_t)create_info->extent.height ||
        !format_matches(info[BF_FORMAT], create_info->format))
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    const int bpp = bf_gralloc_bpp(info[BF_FORMAT]);
    if (bpp <= 0) return VK_ERROR_FORMAT_NOT_SUPPORTED;

    VkResult result = prepare_device(state);
    if (result != VK_SUCCESS) return result;

    // The host image owns its own storage; the gralloc buffer is copied in and
    // out around the loader's acquire/release calls.
    VkImageCreateInfo filtered = *create_info;
    filtered.pNext = next;
    filtered.tiling = VK_IMAGE_TILING_OPTIMAL;
    filtered.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    filtered.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage created = VK_NULL_HANDLE;
    result = create(host, &filtered, allocator, &created);
    if (result != VK_SUCCESS) return result;

    PFN_vkGetImageMemoryRequirements requirements =
        HOST(vkGetImageMemoryRequirements);
    PFN_vkAllocateMemory allocate = HOST(vkAllocateMemory);
    PFN_vkBindImageMemory fn_bind = HOST(vkBindImageMemory);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *map = MAP_FAILED;
    size_t map_size = (size_t)bf_gralloc_file_size(info);
    if (!requirements || !allocate || !fn_bind) result = VK_ERROR_INITIALIZATION_FAILED;
    if (result == VK_SUCCESS) {
        VkMemoryRequirements need;
        requirements(host, created, &need);
        int type = memory_type(&state->memory, need.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo memory_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = need.size,
            .memoryTypeIndex = type < 0 ? 0u : (uint32_t)type,
        };
        result = allocate(host, &memory_info, NULL, &memory);
        if (result == VK_SUCCESS)
            result = fn_bind(host, created, memory, 0);
    }
    if (result == VK_SUCCESS) {
        int fd = fcntl(handle->data[0], F_DUPFD_CLOEXEC, 0);
        if (fd < 0) {
            result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        } else {
            map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            if (map == MAP_FAILED) result = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    if (result != VK_SUCCESS) {
        PFN_vkFreeMemory free_memory = HOST(vkFreeMemory);
        PFN_vkDestroyImage destroy = HOST(vkDestroyImage);
        if (created && destroy) destroy(host, created, NULL);
        if (memory && free_memory) free_memory(host, memory, NULL);
        if (map != MAP_FAILED) munmap(map, map_size);
        return result;
    }

    struct image_binding *binding = calloc(1, sizeof(*binding));
    if (!binding) {
        PFN_vkFreeMemory free_memory = HOST(vkFreeMemory);
        PFN_vkDestroyImage destroy = HOST(vkDestroyImage);
        if (destroy) destroy(host, created, NULL);
        if (free_memory) free_memory(host, memory, NULL);
        munmap(map, map_size);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    binding->image = created;
    binding->device = state;
    binding->memory = memory;
    binding->map = map;
    binding->map_size = map_size;
    memcpy(binding->info, info, sizeof(binding->info));
    pthread_mutex_lock(&registry);
    binding->next = images;
    images = binding;
    pthread_mutex_unlock(&registry);
    *image = created;
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyImage(
        VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
    PFN_vkFreeMemory free_memory = HOST(vkFreeMemory);
    PFN_vkDestroyImage destroy = HOST(vkDestroyImage);
    const VkDevice host = HOST_HANDLE(VkDevice, device);
    pthread_mutex_lock(&registry);
    struct image_binding **link = &images;
    while (*link && (*link)->image != image) link = &(*link)->next;
    struct image_binding *binding = *link;
    if (binding) *link = binding->next;
    pthread_mutex_unlock(&registry);
    // Vulkan requires the image to be gone before its memory is freed.
    if (destroy) destroy(host, image, allocator);
    if (binding) {
        munmap(binding->map, binding->map_size);
        if (free_memory && binding->memory)
            free_memory(host, binding->memory, NULL);
        free(binding);
    }
}

static VkResult acquire_image(VkDevice device, VkImage image,
                              int native_fence_fd, VkSemaphore semaphore,
                              VkFence fence) {
    struct image_binding *binding = find_image(image);
    if (!binding || binding->device->device != device)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (native_fence_fd >= 0) {
        // The producer is the compositor: block until it has released the
        // handle. The fence is a plain guest fd, so poll it directly.
        struct pollfd wait = {native_fence_fd, POLLIN, 0};
        int status;
        do {
            status = poll(&wait, 1, -1);
        } while (status < 0 && errno == EINTR);
        close(native_fence_fd);
        if (status <= 0 || !(wait.revents & POLLIN))
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    const int32_t *info = binding->info;
    const uint32_t width = (uint32_t)info[BF_WIDTH];
    const uint32_t height = (uint32_t)info[BF_HEIGHT];
    const uint32_t stride = (uint32_t)info[BF_STRIDE];
    const VkDeviceSize bytes = (VkDeviceSize)stride * height * bf_gralloc_bpp(info[BF_FORMAT]);
    struct device_state *state = binding->device;
    // The compositor only reads the buffer, and release left the image in the layout the upload would: nothing to do
    // but signal. This saves a full-frame copy in each direction (and the wait for it) on every frame after the first.
    if (binding->host_current) return signal_only(state, semaphore, fence);
    VkResult result = prepare_staging(state, bytes);
    if (result != VK_SUCCESS) return result;
    memcpy(state->staging_map, (const char *)binding->map + BF_GRALLOC_OFFSET,
           (size_t)bytes);
    VkExtent3D extent = {width, height, 1};
    return stage_copy(state, binding, true, NULL, 0, semaphore, fence, extent,
                      stride);
}

static VkResult release_image(VkQueue queue, uint32_t wait_count,
                              const VkSemaphore *wait_semaphores,
                              VkImage image, int *native_fence_fd) {
    struct image_binding *binding = find_image(image);
    if (!binding) return VK_ERROR_INITIALIZATION_FAILED;
    struct device_state *state = binding->device;
    const int32_t *info = binding->info;
    const uint32_t width = (uint32_t)info[BF_WIDTH];
    const uint32_t height = (uint32_t)info[BF_HEIGHT];
    const uint32_t stride = (uint32_t)info[BF_STRIDE];
    const VkDeviceSize bytes = (VkDeviceSize)stride * height * bf_gralloc_bpp(info[BF_FORMAT]);
    VkResult result = prepare_staging(state, bytes);
    if (result != VK_SUCCESS) return result;
    if (queue) state->queue = queue;  // the loader's queue is the one in use
    VkExtent3D extent = {width, height, 1};
    result = stage_copy(state, binding, false, wait_semaphores, wait_count,
                        VK_NULL_HANDLE, VK_NULL_HANDLE, extent, stride);
    if (result != VK_SUCCESS) return result;
    memcpy((char *)binding->map + BF_GRALLOC_OFFSET, state->staging_map,
           (size_t)bytes);
    binding->host_current = true;
    // The copy already completed on the host, so there is no native fence to
    // hand the consumer; -1 is the spec's "nothing to wait for".
    if (native_fence_fd) *native_fence_fd = -1;
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAcquireImageANDROID(
        VkDevice device, VkImage image, int native_fence_fd,
        VkSemaphore semaphore, VkFence fence) {
    pthread_mutex_lock(&copy_mutex);
    VkResult result = acquire_image(HOST_HANDLE(VkDevice, device), image,
                                    native_fence_fd, semaphore, fence);
    pthread_mutex_unlock(&copy_mutex);
    return result;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSignalReleaseImageANDROID(
        VkQueue queue, uint32_t wait_count, const VkSemaphore *wait_semaphores,
        VkImage image, int *native_fence_fd) {
    pthread_mutex_lock(&copy_mutex);
    VkResult result = release_image(HOST_HANDLE(VkQueue, queue), wait_count,
                                    wait_semaphores, image, native_fence_fd);
    pthread_mutex_unlock(&copy_mutex);
    return result;
}

// ── Android hardware buffer format probe ────────────────────────────────
// The loader's swapchain path hands gralloc buffers to the driver as Android
// hardware buffers, so Unity probes
// VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER with
// vkGetPhysicalDeviceImageFormatProperties2 before it will render. The host
// driver has no such handle type (radv on Linux answers
// VK_ERROR_FORMAT_NOT_SUPPORTED), which Unity reports as
// "VkGetPhysicalDeviceImageFormatProperties2 for AHB usage failed" and then
// stops (Zenless Zone Zero, 2026-11-06). This bridge never imports AHB memory
// — it copies through a sealed memfd — so answer the probe from the host's
// plain format query and describe the handle type as supported.
enum {
    // AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | ....GPU_COLOR_OUTPUT
    BF_AHB_USAGE_GPU = 0x100u | 0x200u,
};

static const VkPhysicalDeviceExternalImageFormatInfo *ahb_external(
        const void *next) {
    for (const VkBaseInStructure *node = (const VkBaseInStructure *)next; node;
         node = node->pNext) {
        if (node->sType ==
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO)
            return (const VkPhysicalDeviceExternalImageFormatInfo *)node;
    }
    return NULL;
}

static void describe_external_format(VkImageFormatProperties2 *properties,
                                     VkFormat format) {
    for (VkBaseOutStructure *node = (VkBaseOutStructure *)properties->pNext;
         node; node = node->pNext) {
        switch (node->sType) {
        case VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES: {
            VkExternalImageFormatProperties *out =
                (VkExternalImageFormatProperties *)node;
            out->externalMemoryProperties.compatibleHandleTypes =
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
            break;
        }
        case VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID: {
            VkAndroidHardwareBufferFormatPropertiesANDROID *out =
                (VkAndroidHardwareBufferFormatPropertiesANDROID *)node;
            out->format = format;
            out->externalFormat = 0;
            out->formatFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                  VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            out->samplerYcbcrConversionComponents = (VkComponentMapping){
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            out->suggestedYcbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
            out->suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
            out->suggestedXChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
            out->suggestedYChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
            break;
        }
        case VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID: {
            VkAndroidHardwareBufferUsageANDROID *out =
                (VkAndroidHardwareBufferUsageANDROID *)node;
            out->androidHardwareBufferUsage = BF_AHB_USAGE_GPU;
            break;
        }
        default:
            break;
        }
    }
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(
        VkPhysicalDevice physical, const VkPhysicalDeviceImageFormatInfo2 *info,
        VkImageFormatProperties2 *properties) {
    if (!info) return VK_ERROR_INITIALIZATION_FAILED;
    const VkPhysicalDeviceExternalImageFormatInfo *external =
        ahb_external(info->pNext);
    const bool ahb = external && properties &&
        external->handleType ==
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    if (!ahb) {
        PFN_vkGetPhysicalDeviceImageFormatProperties2 forward =
            HOST(vkGetPhysicalDeviceImageFormatProperties2);
        if (!forward) return VK_ERROR_INITIALIZATION_FAILED;
        return forward(HOST_HANDLE(VkPhysicalDevice, physical), info, properties);
    }
    PFN_vkGetPhysicalDeviceImageFormatProperties plain =
        HOST(vkGetPhysicalDeviceImageFormatProperties);
    if (!plain) return VK_ERROR_INITIALIZATION_FAILED;
    VkImageFormatProperties base;
    VkResult result = plain(HOST_HANDLE(VkPhysicalDevice, physical),
                            info->format, info->type, info->tiling,
                            info->usage, info->flags, &base);
    if (result != VK_SUCCESS) return result;
    properties->imageFormatProperties = base;
    describe_external_format(properties, info->format);
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2KHR(
        VkPhysicalDevice physical, const VkPhysicalDeviceImageFormatInfo2 *info,
        VkImageFormatProperties2 *properties) {
    return vkGetPhysicalDeviceImageFormatProperties2(physical, info, properties);
}

PFN_vkVoidFunction vulkan_native_buffer_symbol(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2"))
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR"))
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties2KHR;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)vkCreateDevice;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)vkDestroyDevice;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties"))
        return (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties;
    if (!strcmp(name, "vkCreateImage")) return (PFN_vkVoidFunction)vkCreateImage;
    if (!strcmp(name, "vkDestroyImage")) return (PFN_vkVoidFunction)vkDestroyImage;
    if (!strcmp(name, "vkAcquireImageANDROID"))
        return (PFN_vkVoidFunction)vkAcquireImageANDROID;
    if (!strcmp(name, "vkQueueSignalReleaseImageANDROID"))
        return (PFN_vkVoidFunction)vkQueueSignalReleaseImageANDROID;
    return NULL;
}
