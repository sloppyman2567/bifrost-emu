/* test_mambo_vulkan.c — MAMBO VULKAN TEST (2026-08-22).
 *
 * Opens a real SDL2 window through the thunks, renders Matikanetannhauser
 * (baked 256x256 RGBA texture, generated from Matikanetannhauser_(Race).webp
 * with alpha blending for webp transparency) as a textured quad through a
 * full Vulkan pipeline — window surface, swapchain, graphics pipeline,
 * combined-image-sampler descriptor, staging upload via
 * vkCmdCopyBufferToImage + pipeline barriers — and plays a mambo rhythm ONE
 * time through the ALSA audio-thunk arm (shared AudioEngine ring).
 *
 * NOTE: the texture is RGBA because 24-bit R8G8B8_UNORM image copies hard-
 * fault this RADV context even on the host (verified with a host-side repro;
 * data lands correctly but the context is lost afterwards).
 *
 * Exit 0 = pass, 1 = fail, 77 = skip without DISPLAY/Vulkan/audio env.
 *
 * Build:
 *   make cross SRC=ctest_real/test_mambo_vulkan.c OUT=ctest_real/test_mambo_vulkan.elf \
 *     CROSS_EXTRA=-Ictest_real/vulkan_headers/include
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/syscall.h>
#include <vulkan/vulkan_core.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define __NR_bifrost_dlopen 0x1002
#define __NR_bifrost_dlsym  0x1003

static void* dlopen_bf(const char* path, int mode) {
    return (void*)(long)syscall(__NR_bifrost_dlopen, path, (long)mode);
}
static void* dlsym_bf(void* handle, const char* name) {
    return (void*)(long)syscall(__NR_bifrost_dlsym, handle, name);
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  OK  %s\n", msg); } \
    else { printf(" FAIL  %s\n", msg); g_fail++; } \
} while (0)

#include "mambo_tex.h"
#include "mambo_audio.h"
#include "test_mambo_spv.h"

typedef int (*SDL_Init_t)(uint32_t);
typedef void* (*SDL_CreateWindow_t)(const char*, int, int, int, int, uint32_t);
typedef void (*SDL_DestroyWindow_t)(void*);
typedef void (*SDL_Quit_t)(void);
typedef int (*SDL_Vulkan_CreateSurface_t)(void*, VkInstance, VkSurfaceKHR*);
typedef int (*SDL_PollEvent_t)(void*);

int main(void) {
    /* ── SDL2 window ──────────────────────────────────────────────── */
    void* sdl = dlopen_bf("libSDL2-2.0.so.0", 1);
    if (!sdl) sdl = dlopen_bf("libSDL2.so", 1);
    if (!sdl) { printf("SKIP: libSDL2 unavailable\n"); return 77; }
    SDL_Init_t SDL_Init = (SDL_Init_t)dlsym_bf(sdl, "SDL_Init");
    SDL_CreateWindow_t SDL_CreateWindow =
        (SDL_CreateWindow_t)dlsym_bf(sdl, "SDL_CreateWindow");
    SDL_Vulkan_CreateSurface_t SDL_Vulkan_CreateSurface =
        (SDL_Vulkan_CreateSurface_t)dlsym_bf(sdl, "SDL_Vulkan_CreateSurface");
    SDL_PollEvent_t SDL_PollEvent = (SDL_PollEvent_t)dlsym_bf(sdl, "SDL_PollEvent");
    SDL_DestroyWindow_t SDL_DestroyWindow =
        (SDL_DestroyWindow_t)dlsym_bf(sdl, "SDL_DestroyWindow");
    SDL_Quit_t SDL_Quit = (SDL_Quit_t)dlsym_bf(sdl, "SDL_Quit");
    if (!SDL_Init || !SDL_CreateWindow || !SDL_Vulkan_CreateSurface ||
        !SDL_PollEvent || !SDL_Quit) {
        printf("SKIP: SDL window/surface fns unavailable\n"); return 77;
    }
    CHECK(SDL_Init(0x20u /*VIDEO*/) == 0, "SDL_Init(VIDEO)");
    void* win = SDL_CreateWindow("MAMBO! Matikanetannhauser",
                                 100, 100, 640, 480,
                                 0x00000002u /*SHOWN*/ | 0x00000004u /*RESIZABLE*/);
    CHECK(win != NULL, "SDL_CreateWindow");
    if (!win) return 1;

    /* ── Vulkan instance ──────────────────────────────────────────── */
    void* vk = dlopen_bf("libvulkan.so.1", 1);
    if (!vk) { printf("SKIP: host libvulkan unavailable\n"); return 77; }
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym_bf(vk, "vkGetInstanceProcAddr");
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)dlsym_bf(vk, "vkGetDeviceProcAddr");
    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)dlsym_bf(vk, "vkCreateInstance");
    if (!vkCreateInstance || !vkGetInstanceProcAddr) {
        printf("SKIP: no vkCreateInstance\n"); return 77;
    }

    /* Enable KHR_surface plus every platform surface ext present. */
    PFN_vkEnumerateInstanceExtensionProperties vkEnumInstExts =
        (PFN_vkEnumerateInstanceExtensionProperties)
            dlsym_bf(vk, "vkEnumerateInstanceExtensionProperties");
    const char* plat[] = {
        "VK_KHR_xcb_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface",
    };
    char have[3] = {1, 1, 1};
    if (vkEnumInstExts) {
        uint32_t n = 0;
        vkEnumInstExts(NULL, &n, NULL);
        VkExtensionProperties* ex = calloc(n ? n : 1, sizeof(*ex));
        if (vkEnumInstExts(NULL, &n, ex) == VK_SUCCESS) {
            have[0] = have[1] = have[2] = 0;
            for (uint32_t i = 0; i < n; i++)
                for (int p = 0; p < 3; p++)
                    if (!strcmp(ex[i].extensionName, plat[p])) have[p] = 1;
        }
        free(ex);
    }
    const char* exts[5];
    uint32_t next = 0;
    exts[next++] = "VK_KHR_surface";
    for (int p = 0; p < 3; p++) if (have[p]) exts[next++] = plat[p];

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bifrost mambo",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = next,
        .ppEnabledExtensionNames = exts,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    if (r == VK_ERROR_EXTENSION_NOT_PRESENT) {
        printf("SKIP: host driver lacks required surface extensions\n");
        return 77;
    }
    CHECK(r == VK_SUCCESS && instance != VK_NULL_HANDLE, "vkCreateInstance (+surface exts)");
    if (r != VK_SUCCESS) return 1;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    r = SDL_Vulkan_CreateSurface(win, instance, &surface) &&
        surface != VK_NULL_HANDLE
        ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    CHECK(r == VK_SUCCESS, "SDL_Vulkan_CreateSurface");
    if (r != VK_SUCCESS) return 1;

    /* ── Physical device / queue / swapchain ──────────────────────── */
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR =
        (PFN_vkDestroySurfaceKHR)vkGetInstanceProcAddr(instance, "vkDestroySurfaceKHR");
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
        (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties");

    uint32_t nphys = 0;
    vkEnumeratePhysicalDevices(instance, &nphys, NULL);
    VkPhysicalDevice* phys = calloc(nphys ? nphys : 1, sizeof(VkPhysicalDevice));
    r = vkEnumeratePhysicalDevices(instance, &nphys, phys);
    CHECK(r == VK_SUCCESS && nphys > 0, "physical devices");
    if (r != VK_SUCCESS || nphys == 0) return 1;
    VkPhysicalDevice pd = phys[0];

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
    VkQueueFamilyProperties* qprops = calloc(nqf ? nqf : 1, sizeof(*qprops));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qprops);

    uint32_t gfx_family = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++) {
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx_family = i; break; }
    }
    VkBool32 present_ok = VK_FALSE;
    if (gfx_family != UINT32_MAX && vkGetPhysicalDeviceSurfaceSupportKHR)
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, gfx_family, surface, &present_ok);
    CHECK(gfx_family != UINT32_MAX && present_ok, "graphics+present queue family");
    if (gfx_family == UINT32_MAX || !present_ok) return 1;

    VkSurfaceCapabilitiesKHR caps;
    r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    CHECK(r == VK_SUCCESS, "surface capabilities");
    uint32_t nformats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nformats, NULL);
    VkSurfaceFormatKHR* formats = calloc(nformats ? nformats : 1, sizeof(*formats));
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nformats, formats);
    CHECK(r == VK_SUCCESS && nformats > 0, "surface formats");
    if (r != VK_SUCCESS || nformats == 0) return 1;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = gfx_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = (const char* const[]){"VK_KHR_swapchain"},
    };
    PFN_vkCreateDevice vkCreateDevice =
        (PFN_vkCreateDevice)vkGetInstanceProcAddr(instance, "vkCreateDevice");
    VkDevice device = VK_NULL_HANDLE;
    r = vkCreateDevice(pd, &dci, NULL, &device);
    CHECK(r == VK_SUCCESS && device != VK_NULL_HANDLE, "vkCreateDevice");
    if (r != VK_SUCCESS) return 1;
    #define DEVFN(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
    DEVFN(vkGetDeviceQueue); DEVFN(vkCreateSwapchainKHR);
    DEVFN(vkGetSwapchainImagesKHR); DEVFN(vkAcquireNextImageKHR);
    DEVFN(vkQueuePresentKHR); DEVFN(vkDestroySwapchainKHR);
    DEVFN(vkDeviceWaitIdle); DEVFN(vkCreateCommandPool);
    DEVFN(vkAllocateCommandBuffers); DEVFN(vkBeginCommandBuffer);
    DEVFN(vkEndCommandBuffer); DEVFN(vkCreateRenderPass);
    DEVFN(vkCreateImageView); DEVFN(vkCreateFramebuffer);
    DEVFN(vkCmdBeginRenderPass); DEVFN(vkCmdEndRenderPass);
    DEVFN(vkQueueSubmit); DEVFN(vkCreateShaderModule);
    DEVFN(vkCreateGraphicsPipelines); DEVFN(vkCreatePipelineLayout);
    DEVFN(vkCreateDescriptorSetLayout); DEVFN(vkCreateDescriptorPool);
    DEVFN(vkAllocateDescriptorSets); DEVFN(vkUpdateDescriptorSets);
    DEVFN(vkCreateBuffer); DEVFN(vkAllocateMemory); DEVFN(vkBindBufferMemory);
    DEVFN(vkCreateImage); DEVFN(vkGetImageMemoryRequirements);
    DEVFN(vkBindImageMemory); DEVFN(vkMapMemory); DEVFN(vkUnmapMemory);
    DEVFN(vkFlushMappedMemoryRanges);
    DEVFN(vkCmdBindVertexBuffers); DEVFN(vkCmdBindDescriptorSets);
    DEVFN(vkCmdDraw); DEVFN(vkCmdUpdateBuffer); DEVFN(vkCmdBindPipeline);
    DEVFN(vkCmdPipelineBarrier); DEVFN(vkCmdCopyBufferToImage);
    DEVFN(vkCreateSampler);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, gfx_family, 0, &queue);

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu || extent.width == 0 ||
        extent.height == 0xFFFFFFFFu || extent.height == 0) {
        extent.width = 640; extent.height = 480;
    }
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount,
        .imageFormat = formats[0].format,
        .imageColorSpace = formats[0].colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(device, &sci, NULL, &swapchain);
    CHECK(r == VK_SUCCESS, "vkCreateSwapchainKHR");
    if (r != VK_SUCCESS) return 1;
    uint32_t nimages = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &nimages, NULL);
    VkImage* images = calloc(nimages ? nimages : 1, sizeof(VkImage));
    vkGetSwapchainImagesKHR(device, swapchain, &nimages, images);
    CHECK(nimages > 0, "swapchain images");

    VkPhysicalDeviceMemoryProperties mp = {0};
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    #define FIND_MEM(req, flags, out_idx) do { \
        out_idx = UINT32_MAX; \
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) { \
            if ((req.memoryTypeBits & (1u << i)) && \
                (mp.memoryTypes[i].propertyFlags & (flags))) { out_idx = i; break; } } \
    } while (0)

    /* ── Texture upload: staging buffer + RGBA image + barriers ───── */
    const VkDeviceSize tex_bytes = MAMBO_TEX_W * MAMBO_TEX_H * 4;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = tex_bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VkBuffer stage_buf = VK_NULL_HANDLE;
    r = vkCreateBuffer(device, &bci, NULL, &stage_buf);
    CHECK(r == VK_SUCCESS, "staging vkCreateBuffer");
    VkMemoryRequirements sreq = {0};
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements =
        (PFN_vkGetBufferMemoryRequirements)vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements");
    vkGetBufferMemoryRequirements(device, stage_buf, &sreq);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = sreq.size };
    FIND_MEM(sreq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mai.memoryTypeIndex);
    CHECK(mai.memoryTypeIndex != UINT32_MAX, "host-visible mem type");
    VkDeviceMemory stage_mem = VK_NULL_HANDLE;
    r = vkAllocateMemory(device, &mai, NULL, &stage_mem);
    CHECK(r == VK_SUCCESS, "staging vkAllocateMemory");
    r = vkBindBufferMemory(device, stage_buf, stage_mem, 0);
    CHECK(r == VK_SUCCESS, "vkBindBufferMemory (staging)");

    void* mapped = NULL;
    r = vkMapMemory(device, stage_mem, 0, tex_bytes, 0, &mapped);
    CHECK(r == VK_SUCCESS && mapped != NULL &&
          ((uintptr_t)mapped >> 32) == 0,
          "vkMapMemory bounce is a direct-window guest address");
    if (r == VK_SUCCESS && mapped) memcpy(mapped, mambo_tex, tex_bytes);
    VkMappedMemoryRange frange = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = stage_mem, .offset = 0, .size = VK_WHOLE_SIZE };
    vkFlushMappedMemoryRanges(device, 1, &frange);

    VkImageCreateInfo tici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { MAMBO_TEX_W, MAMBO_TEX_H, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage tex_image = VK_NULL_HANDLE;
    r = vkCreateImage(device, &tici, NULL, &tex_image);
    CHECK(r == VK_SUCCESS, "texture vkCreateImage (R8G8B8A8_UNORM)");
    VkMemoryRequirements treq = {0};
    vkGetImageMemoryRequirements(device, tex_image, &treq);
    mai.allocationSize = treq.size;
    FIND_MEM(treq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mai.memoryTypeIndex);
    VkDeviceMemory tex_mem = VK_NULL_HANDLE;
    r = mai.memoryTypeIndex != UINT32_MAX
        ? vkAllocateMemory(device, &mai, NULL, &tex_mem) : VK_ERROR_INITIALIZATION_FAILED;
    CHECK(r == VK_SUCCESS, "texture vkAllocateMemory");
    r = vkBindImageMemory(device, tex_image, tex_mem, 0);
    CHECK(r == VK_SUCCESS, "vkBindImageMemory (texture)");

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = gfx_family,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    r = vkCreateCommandPool(device, &cpci, NULL, &pool);
    CHECK(r == VK_SUCCESS, "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1 };
    VkCommandBuffer upcmd = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(device, &cbai, &upcmd);
    CHECK(r == VK_SUCCESS, "upload vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(upcmd, &cbbi);
    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = tex_image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(upcmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_dst);
    VkBufferImageCopy copy = {
        .bufferOffset = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { MAMBO_TEX_W, MAMBO_TEX_H, 1 },
    };
    vkCmdCopyBufferToImage(upcmd, stage_buf, tex_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier to_read = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image = tex_image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(upcmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &to_read);
    vkEndCommandBuffer(upcmd);
    VkSubmitInfo upsub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1, .pCommandBuffers = &upcmd };
    r = vkQueueSubmit(queue, 1, &upsub, VK_NULL_HANDLE);
    CHECK(r == VK_SUCCESS, "texture upload vkQueueSubmit");
    vkDeviceWaitIdle(device);
    CHECK(1, "texture uploaded (barriers + vkCmdCopyBufferToImage)");
    vkUnmapMemory(device, stage_mem);

    /* ── Sampler + combined image sampler descriptor ──────────────── */
    VkSamplerCreateInfo samci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 1.0f,
    };
    VkSampler sampler = VK_NULL_HANDLE;
    r = vkCreateSampler(device, &samci, NULL, &sampler);
    CHECK(r == VK_SUCCESS, "vkCreateSampler");

    VkDescriptorSetLayoutBinding dslb = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &dslb };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    r = vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl);
    CHECK(r == VK_SUCCESS, "vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize poolsz = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &poolsz };
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    r = vkCreateDescriptorPool(device, &dpci, NULL, &dpool);
    CHECK(r == VK_SUCCESS, "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset = VK_NULL_HANDLE;
    r = vkAllocateDescriptorSets(device, &dsai, &dset);
    CHECK(r == VK_SUCCESS, "vkAllocateDescriptorSets");

    VkImageViewCreateInfo tvci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = tex_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView tex_view = VK_NULL_HANDLE;
    r = vkCreateImageView(device, &tvci, NULL, &tex_view);
    CHECK(r == VK_SUCCESS, "texture vkCreateImageView");
    VkDescriptorImageInfo dii = {
        .sampler = sampler, .imageView = tex_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet dwrite = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = dset, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &dii };
    vkUpdateDescriptorSets(device, 1, &dwrite, 0, NULL);
    CHECK(1, "combined-image-sampler descriptor written");

    /* ── Render pass (color only) + per-image framebuffers ────────── */
    VkAttachmentDescription att = {
        .format = formats[0].format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &cref };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &subpass };
    VkRenderPass renderpass = VK_NULL_HANDLE;
    r = vkCreateRenderPass(device, &rpci, NULL, &renderpass);
    CHECK(r == VK_SUCCESS, "vkCreateRenderPass");

    VkImageView* cviews = calloc(nimages, sizeof(VkImageView));
    VkFramebuffer* fbs = calloc(nimages, sizeof(VkFramebuffer));
    for (uint32_t i = 0; i < nimages; i++) {
        VkImageViewCreateInfo cvci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = formats[0].format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCreateImageView(device, &cvci, NULL, &cviews[i]);
        VkFramebufferCreateInfo fbci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = renderpass, .attachmentCount = 1,
            .pAttachments = &cviews[i],
            .width = extent.width, .height = extent.height, .layers = 1 };
        vkCreateFramebuffer(device, &fbci, NULL, &fbs[i]);
    }
    CHECK(1, "per-image framebuffers");

    /* ── Pipeline: quad verts (pos2 uv2), TRIANGLE_STRIP, alpha blend ── */
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(kMamboVertSpv), .pCode = kMamboVertSpv };
    VkShaderModule vs_mod = VK_NULL_HANDLE, fs_mod = VK_NULL_HANDLE;
    r = vkCreateShaderModule(device, &smci, NULL, &vs_mod);
    CHECK(r == VK_SUCCESS, "vertex shader module");
    smci.pCode = kMamboFragSpv; smci.codeSize = sizeof(kMamboFragSpv);
    r = vkCreateShaderModule(device, &smci, NULL, &fs_mod);
    CHECK(r == VK_SUCCESS, "fragment shader module");

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    r = vkCreatePipelineLayout(device, &plci, NULL, &pipe_layout);
    CHECK(r == VK_SUCCESS, "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs_mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs_mod, .pName = "main" },
    };
    VkVertexInputBindingDescription vbind = {
        .binding = 0, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription vattrs[2] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 },
    };
    VkPipelineVertexInputStateCreateInfo visa = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbind,
        .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = vattrs };
    VkPipelineInputAssemblyStateCreateInfo iasc = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkViewport vp = { 0, 0, (float)extent.width, (float)extent.height, 0, 1 };
    VkRect2D sc = { {0,0}, extent };
    VkPipelineViewportStateCreateInfo vpsc = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rast = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo msc = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    /* Alpha blending: webp transparency. Premultiplied? No — straight
     * alpha from the PNG-style RGBA pixels. */
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cbs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &visa, .pInputAssemblyState = &iasc,
        .pViewportState = &vpsc, .pRasterizationState = &rast,
        .pMultisampleState = &msc, .pColorBlendState = &cbs,
        .layout = pipe_layout, .renderPass = renderpass, .subpass = 0 };
    VkPipeline pipeline = VK_NULL_HANDLE;
    r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline);
    CHECK(r == VK_SUCCESS, "textured-quad vkCreateGraphicsPipelines");

    /* Vertex buffer (quad), filled via vkCmdUpdateBuffer each frame. */
    bci.size = 4 * 16;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer vbuf = VK_NULL_HANDLE;
    r = vkCreateBuffer(device, &bci, NULL, &vbuf);
    CHECK(r == VK_SUCCESS, "vertex vkCreateBuffer");
    VkMemoryRequirements vreq = {0};
    PFN_vkGetBufferMemoryRequirements vkGetBufReq2 =
        (PFN_vkGetBufferMemoryRequirements)vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements");
    vkGetBufReq2(device, vbuf, &vreq);
    mai.allocationSize = vreq.size;
    FIND_MEM(vreq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mai.memoryTypeIndex);
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    r = vkAllocateMemory(device, &mai, NULL, &vmem);
    CHECK(r == VK_SUCCESS, "vertex vkAllocateMemory");
    vkBindBufferMemory(device, vbuf, vmem, 0);

    /* Vulkan NDC has y=-1 at screen TOP → v=0 pairs with y=-1. */
    static const float quad[4][4] = {
        { -0.45f, -1.0f, 0.0f, 0.0f },
        {  0.45f, -1.0f, 1.0f, 0.0f },
        { -0.45f,  1.0f, 0.0f, 1.0f },
        {  0.45f,  1.0f, 1.0f, 1.0f },
    };

    VkCommandBufferAllocateInfo cbai2 = cbai;
    cbai2.commandBufferCount = nimages;
    VkCommandBuffer* cmds = calloc(nimages, sizeof(VkCommandBuffer));
    r = vkAllocateCommandBuffers(device, &cbai2, cmds);
    CHECK(r == VK_SUCCESS, "per-image command buffers");

    /* ── Mambo sound: ONE play through the ALSA arm ─────────────────
     * Real decoded PCM from the source MP3 (see mambo_audio.h).       */
    static const int16_t* mambo_pcm = kMamboPcm;
    const size_t mambo_bytes = sizeof(kMamboPcm);

    /* ── Frame loop: render until window closes (~4 s max) ────────── */
    int played = 0, frames = 0, loop_ok = 1;
    for (int frame = 0; frame < 240; frame++) {
        uint8_t ev[64];
        while (SDL_PollEvent(ev)) {
            uint16_t etype;
            memcpy(&etype, ev, 2);
            if (etype == 0x100 /*SDL_QUIT*/) frame = 100000;
        }
        if (frame > 100000) break;

        uint32_t idx = 0;
        r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                  VK_NULL_HANDLE, VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS) { loop_ok = 0; break; }
        VkCommandBufferBeginInfo b2 = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cmds[idx], &b2);
        vkCmdUpdateBuffer(cmds[idx], vbuf, 0, sizeof(quad), quad);
        VkClearValue clear = { .color = { { 0.05f, 0.03f, 0.08f, 1.0f } } };
        VkRenderPassBeginInfo rpbi = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderpass, .framebuffer = fbs[idx],
            .renderArea = { {0,0}, extent },
            .clearValueCount = 1, .pClearValues = &clear };
        vkCmdBeginRenderPass(cmds[idx], &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmds[idx], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmds[idx], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipe_layout, 0, 1, &dset, 0, NULL);
        VkDeviceSize voff = 0;
        vkCmdBindVertexBuffers(cmds[idx], 0, 1, &vbuf, &voff);
        vkCmdDraw(cmds[idx], 4, 1, 0, 0);
        vkCmdEndRenderPass(cmds[idx]);
        r = vkEndCommandBuffer(cmds[idx]);
        if (r != VK_SUCCESS) { loop_ok = 0; break; }
        VkSubmitInfo sub2 = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .commandBufferCount = 1, .pCommandBuffers = &cmds[idx] };
        r = vkQueueSubmit(queue, 1, &sub2, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) { loop_ok = 0; break; }
        VkPresentInfoKHR pres = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                  .swapchainCount = 1, .pSwapchains = &swapchain,
                                  .pImageIndices = &idx };
        r = vkQueuePresentKHR(queue, &pres);
        if (r != VK_SUCCESS) { loop_ok = 0; break; }

        frames++;
        if (frames == 2 && !played) {
            /* ONE-TIME mambo playback through the ALSA subset arm. */
            uint64_t ha = (uint64_t)(uintptr_t)dlopen_bf("libasound.so.2", 2);
            typedef int (*open_t)(void**, const char*, int, int);
            typedef long (*writei_t)(void*, const void*, unsigned long);
            typedef int (*close_t)(void*);
            open_t pcm_open = ha ? (open_t)dlsym_bf((void*)ha, "snd_pcm_open") : NULL;
            writei_t wr = ha ? (writei_t)dlsym_bf((void*)ha, "snd_pcm_writei") : NULL;
            close_t cl = ha ? (close_t)dlsym_bf((void*)ha, "snd_pcm_close") : NULL;
            if (pcm_open && wr && cl) {
                void* pcm_h = NULL; void* hp = NULL; int dir = 0;
                typedef int (*sf_t)(void*, void*, unsigned);
                sf_t fmt_f = (sf_t)dlsym_bf((void*)ha, "snd_pcm_hw_params_set_format");
                sf_t ch_f = (sf_t)dlsym_bf((void*)ha, "snd_pcm_hw_params_set_channels");
                typedef int (*sr_t)(void*, void*, unsigned, int*);
                sr_t rate_f = (sr_t)dlsym_bf((void*)ha, "snd_pcm_hw_params_set_rate");
                typedef int (*hpm_t)(void**);
                hpm_t hp_malloc = (hpm_t)dlsym_bf((void*)ha, "snd_pcm_hw_params_malloc");
                if (pcm_open(&pcm_h, "default", 0, 0) == 0 && pcm_h &&
                    hp_malloc && hp_malloc(&hp) == 0) {
                    if (fmt_f) fmt_f(pcm_h, hp, 2 /*S16_LE*/);
                    if (ch_f) ch_f(pcm_h, hp, 2);
                    if (rate_f) rate_f(pcm_h, hp, 44100, &dir);
                    /* small chunks so the engine ring never drops samples */
                    size_t bytes = mambo_bytes;
                    const uint8_t* p = (const uint8_t*)mambo_pcm;
                    while (bytes > 0) {
                        size_t chunk = bytes > 16384 ? 16384 : bytes;
                        long w = wr(pcm_h, p, chunk / 4 /*frames*/);
                        if (w <= 0) break;
                        p += w * 4;
                        bytes -= w * 4;
                    }
                    cl(pcm_h);
                    played = 1;
                    printf("  OK  MAMBO played once (%zu bytes)\n",
                           mambo_bytes);
                }
            }
        }
        usleep(16000);   /* ~60 fps pacing */
    }
    printf("  ..  rendered %d textured-quad frames\n", frames);
    CHECK(loop_ok && frames >= 2, "textured-quad frame loop ran");
    CHECK(played, "mambo sound played exactly once");

    vkDeviceWaitIdle(device);

    /* ── Teardown ─────────────────────────────────────────────────── */
    {
        PFN_vkDestroySampler vkDestroySampler =
            (PFN_vkDestroySampler)vkGetDeviceProcAddr(device, "vkDestroySampler");
        PFN_vkFreeCommandBuffers vkFreeCommandBuffers =
            (PFN_vkFreeCommandBuffers)vkGetDeviceProcAddr(device, "vkFreeCommandBuffers");
        PFN_vkDestroyFramebuffer vkDestroyFramebuffer =
            (PFN_vkDestroyFramebuffer)vkGetDeviceProcAddr(device, "vkDestroyFramebuffer");
        PFN_vkDestroyImageView vkDestroyImageView =
            (PFN_vkDestroyImageView)vkGetDeviceProcAddr(device, "vkDestroyImageView");
        PFN_vkDestroyRenderPass vkDestroyRenderPass =
            (PFN_vkDestroyRenderPass)vkGetDeviceProcAddr(device, "vkDestroyRenderPass");
        PFN_vkDestroyPipeline vkDestroyPipeline =
            (PFN_vkDestroyPipeline)vkGetDeviceProcAddr(device, "vkDestroyPipeline");
        PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout =
            (PFN_vkDestroyPipelineLayout)vkGetDeviceProcAddr(device, "vkDestroyPipelineLayout");
        PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool =
            (PFN_vkDestroyDescriptorPool)vkGetDeviceProcAddr(device, "vkDestroyDescriptorPool");
        PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout =
            (PFN_vkDestroyDescriptorSetLayout)vkGetDeviceProcAddr(device, "vkDestroyDescriptorSetLayout");
        PFN_vkDestroyShaderModule vkDestroyShaderModule =
            (PFN_vkDestroyShaderModule)vkGetDeviceProcAddr(device, "vkDestroyShaderModule");
        PFN_vkDestroyBuffer vkDestroyBuffer =
            (PFN_vkDestroyBuffer)vkGetDeviceProcAddr(device, "vkDestroyBuffer");
        PFN_vkDestroyImage vkDestroyImage =
            (PFN_vkDestroyImage)vkGetDeviceProcAddr(device, "vkDestroyImage");
        PFN_vkFreeMemory vkFreeMemory =
            (PFN_vkFreeMemory)vkGetDeviceProcAddr(device, "vkFreeMemory");
        PFN_vkDestroyCommandPool vkDestroyCommandPool =
            (PFN_vkDestroyCommandPool)vkGetDeviceProcAddr(device, "vkDestroyCommandPool");
        if (vkFreeCommandBuffers) vkFreeCommandBuffers(device, pool, nimages, cmds);
        if (vkDestroySampler) vkDestroySampler(device, sampler, NULL);
        if (vkDestroyPipeline) vkDestroyPipeline(device, pipeline, NULL);
        if (vkDestroyPipelineLayout) vkDestroyPipelineLayout(device, pipe_layout, NULL);
        if (vkDestroyDescriptorPool) vkDestroyDescriptorPool(device, dpool, NULL);
        if (vkDestroyDescriptorSetLayout) vkDestroyDescriptorSetLayout(device, dsl, NULL);
        if (vkDestroyShaderModule) { vkDestroyShaderModule(device, vs_mod, NULL);
                                     vkDestroyShaderModule(device, fs_mod, NULL); }
        if (vkDestroyBuffer) { vkDestroyBuffer(device, vbuf, NULL);
                               vkDestroyBuffer(device, stage_buf, NULL); }
        if (vkDestroyImage) vkDestroyImage(device, tex_image, NULL);
        if (vkFreeMemory) { vkFreeMemory(device, vmem, NULL);
                            vkFreeMemory(device, tex_mem, NULL);
                            vkFreeMemory(device, stage_mem, NULL); }
        for (uint32_t i = 0; i < nimages; i++) {
            if (vkDestroyFramebuffer) vkDestroyFramebuffer(device, fbs[i], NULL);
            if (vkDestroyImageView) vkDestroyImageView(device, cviews[i], NULL);
        }
        if (vkDestroyRenderPass) vkDestroyRenderPass(device, renderpass, NULL);
        if (vkDestroyCommandPool) vkDestroyCommandPool(device, pool, NULL);
        if (vkDestroySwapchainKHR) vkDestroySwapchainKHR(device, swapchain, NULL);
        if (vkDestroySurfaceKHR) vkDestroySurfaceKHR(instance, surface, NULL);
        PFN_vkDestroyDevice vkDestroyDevice =
            (PFN_vkDestroyDevice)vkGetInstanceProcAddr(instance, "vkDestroyDevice");
        PFN_vkDestroyInstance vkDestroyInstance =
            (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
        if (vkDestroyDevice) vkDestroyDevice(device, NULL);
        if (vkDestroyInstance) vkDestroyInstance(instance, NULL);
    }
    if (SDL_DestroyWindow) SDL_DestroyWindow(win);
    SDL_Quit();
    CHECK(1, "teardown complete");

    free(phys); free(qprops); free(formats); free(images);
    free(cviews); free(fbs); free(cmds);

    printf(g_fail ? "MAMBO VULKAN TEST FAILED (%d)\n"
                  : "MAMBO VULKAN TEST PASSED — ¡MAMBO!\n", g_fail);
    return g_fail ? 1 : 0;
}
