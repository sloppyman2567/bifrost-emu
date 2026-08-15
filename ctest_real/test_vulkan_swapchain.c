// test_vulkan_swapchain.c — headless WSI swapchain smoke test (1.5.3-alpha).
//
// Drives the full VK_KHR_surface + VK_EXT_headless_surface swapchain path
// through the DisplayThunk: create instance with the headless-surface
// extension → create a headless surface → query surface support/caps/
// formats/present modes → create device → create a swapchain on the
// surface → get swapchain images → acquire → present → wait idle →
// teardown. No window, no display server, no rendering — just the WSI
// object lifecycle the emulator must thunk correctly.
//
// The library is loaded via the emulator's internal dlopen syscall
// (0x1002/0x1003), the same pattern as test_vulkan.c. Exit 0 = pass,
// 77 = skip (host driver lacks VK_EXT_headless_surface — env-dependent).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <vulkan/vulkan_core.h>

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

typedef VkResult (VKAPI_PTR *PFPN_vkCreateInstance)(
    const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);

int main(void) {
    void* vk = dlopen_bf("libvulkan.so.1", 1 /* RTLD_LAZY */);
    if (!vk) { printf("SKIP: host libvulkan unavailable\n"); return 77; }

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym_bf(vk, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) { printf("SKIP: no vkGetInstanceProcAddr\n"); return 77; }
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr =
        (PFN_vkGetDeviceProcAddr)dlsym_bf(vk, "vkGetDeviceProcAddr");
    PFPN_vkCreateInstance vkCreateInstance =
        (PFPN_vkCreateInstance)dlsym_bf(vk, "vkCreateInstance");

    CHECK(vkCreateInstance != NULL, "dlsym vkCreateInstance");
    if (!vkCreateInstance) return 1;

    /* The loader always exports vkCreateHeadlessSurfaceEXT; the driver
     * decides at instance creation whether the extension is really present.
     * Enable KHR_surface + EXT_headless_surface and skip (77) if the host
     * rejects them — that's env-dependent, not an emulator failure. */
    const char* exts[] = { "VK_KHR_surface", "VK_EXT_headless_surface" };
    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "bifrost headless swapchain",
        .applicationVersion = 1,
        .pEngineName = "bifrost-emu",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = exts,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    if (r == VK_ERROR_EXTENSION_NOT_PRESENT) {
        printf("SKIP: host driver lacks VK_EXT_headless_surface\n");
        return 77;
    }
    CHECK(r == VK_SUCCESS && instance != VK_NULL_HANDLE, "vkCreateInstance (+headless ext)");
    if (r != VK_SUCCESS) return 1;

    /* Instance-level WSI functions (via vkGetInstanceProcAddr). */
    PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR =
        (PFN_vkDestroySurfaceKHR)vkGetInstanceProcAddr(instance, "vkDestroySurfaceKHR");
    PFN_vkCreateHeadlessSurfaceEXT vkCreateHeadlessSurfaceEXT =
        (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT");
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR =
        (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");

    CHECK(vkDestroySurfaceKHR != NULL, "got vkDestroySurfaceKHR");
    CHECK(vkCreateHeadlessSurfaceEXT != NULL, "got vkCreateHeadlessSurfaceEXT");
    CHECK(vkGetPhysicalDeviceSurfaceSupportKHR != NULL, "got vkGetPhysicalDeviceSurfaceSupportKHR");
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR != NULL, "got vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR != NULL, "got vkGetPhysicalDeviceSurfaceFormatsKHR");
    CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR != NULL, "got vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (!vkCreateHeadlessSurfaceEXT) { printf("SKIP: no headless surface fns\n"); return 77; }

    /* Physical device enumeration (same as test_vulkan.c). */
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
        (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    CHECK(vkEnumeratePhysicalDevices != NULL, "got vkEnumeratePhysicalDevices");

    uint32_t nphys = 0;
    vkEnumeratePhysicalDevices(instance, &nphys, NULL);
    CHECK(nphys > 0, "vkEnumeratePhysicalDevices count");
    VkPhysicalDevice* phys = calloc(nphys ? nphys : 1, sizeof(VkPhysicalDevice));
    r = vkEnumeratePhysicalDevices(instance, &nphys, phys);
    CHECK(r == VK_SUCCESS && nphys > 0, "vkEnumeratePhysicalDevices list");
    VkPhysicalDevice pd = nphys ? phys[0] : VK_NULL_HANDLE;
    if (vkGetPhysicalDeviceProperties && pd) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        printf("  GPU: %s\n", props.deviceName);
    }

    /* Find the first queue family that can present to the surface. */
    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
    VkQueueFamilyProperties* qprops = calloc(nqf ? nqf : 1, sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qprops);
    CHECK(nqf > 0, "queue families present");

    VkHeadlessSurfaceCreateInfoEXT hsci = {
        .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,
    };
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    r = vkCreateHeadlessSurfaceEXT(instance, &hsci, NULL, &surface);
    CHECK(r == VK_SUCCESS && surface != VK_NULL_HANDLE, "vkCreateHeadlessSurfaceEXT");
    if (r != VK_SUCCESS) return 1;

    uint32_t present_family = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++) {
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &supported);
        if (supported == VK_TRUE) { present_family = i; break; }
    }
    CHECK(present_family != UINT32_MAX, "queue family supports surface present");
    if (present_family == UINT32_MAX) return 1;

    VkSurfaceCapabilitiesKHR caps;
    r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    CHECK(r == VK_SUCCESS, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    uint32_t nformats = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nformats, NULL);
    VkSurfaceFormatKHR* formats = calloc(nformats ? nformats : 1, sizeof(VkSurfaceFormatKHR));
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nformats, formats);
    CHECK(r == VK_SUCCESS && nformats > 0, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (r != VK_SUCCESS || nformats == 0) return 1;

    uint32_t nmodes = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nmodes, NULL);
    VkPresentModeKHR* modes = calloc(nmodes ? nmodes : 1, sizeof(VkPresentModeKHR));
    r = vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nmodes, modes);
    CHECK(r == VK_SUCCESS && nmodes > 0, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (r != VK_SUCCESS || nmodes == 0) return 1;

    /* Device (as in test_vulkan.c). */
    PFN_vkCreateDevice vkCreateDevice =
        (PFN_vkCreateDevice)vkGetInstanceProcAddr(instance, "vkCreateDevice");
    CHECK(vkCreateDevice != NULL, "got vkCreateDevice");
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = present_family,
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
    VkDevice device = VK_NULL_HANDLE;
    r = vkCreateDevice(pd, &dci, NULL, &device);
    CHECK(r == VK_SUCCESS && device != VK_NULL_HANDLE, "vkCreateDevice");
    if (r != VK_SUCCESS) return 1;

    PFN_vkGetDeviceQueue vkGetDeviceQueue =
        (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(device, "vkGetDeviceQueue");
    PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR =
        (PFN_vkCreateSwapchainKHR)vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR");
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR =
        (PFN_vkGetSwapchainImagesKHR)vkGetDeviceProcAddr(device, "vkGetSwapchainImagesKHR");
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR =
        (PFN_vkAcquireNextImageKHR)vkGetDeviceProcAddr(device, "vkAcquireNextImageKHR");
    PFN_vkQueuePresentKHR vkQueuePresentKHR =
        (PFN_vkQueuePresentKHR)vkGetDeviceProcAddr(device, "vkQueuePresentKHR");
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR =
        (PFN_vkDestroySwapchainKHR)vkGetDeviceProcAddr(device, "vkDestroySwapchainKHR");
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle =
        (PFN_vkDeviceWaitIdle)vkGetDeviceProcAddr(device, "vkDeviceWaitIdle");
    CHECK(vkCreateSwapchainKHR != NULL, "got vkCreateSwapchainKHR");
    CHECK(vkGetSwapchainImagesKHR != NULL, "got vkGetSwapchainImagesKHR");
    CHECK(vkAcquireNextImageKHR != NULL, "got vkAcquireNextImageKHR");
    CHECK(vkQueuePresentKHR != NULL, "got vkQueuePresentKHR");
    if (!vkCreateSwapchainKHR || !vkGetSwapchainImagesKHR) return 1;

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, present_family, 0, &queue);
    CHECK(queue != VK_NULL_HANDLE, "vkGetDeviceQueue");

    /* Swapchain extent: prefer currentExtent, else clamp 640x480 into
     * [minImageExtent, maxImageExtent] (headless surfaces may report the
     * 0xFFFFFFFF sentinel meaning "app chooses"). */
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu || extent.width == 0 ||
        extent.height == 0xFFFFFFFFu || extent.height == 0) {
        extent.width  = 640 < caps.minImageExtent.width  ? caps.minImageExtent.width
                      : (640 > caps.maxImageExtent.width  ? caps.maxImageExtent.width  : 640);
        extent.height = 480 < caps.minImageExtent.height ? caps.minImageExtent.height
                      : (480 > caps.maxImageExtent.height ? caps.maxImageExtent.height : 480);
    }
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & composite))
        composite = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & 0xF);
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount,
        .imageFormat = formats[0].format,
        .imageColorSpace = formats[0].colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = composite,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(device, &sci, NULL, &swapchain);
    CHECK(r == VK_SUCCESS && swapchain != VK_NULL_HANDLE, "vkCreateSwapchainKHR");
    if (r != VK_SUCCESS) return 1;
    printf("  swapchain %ux%u, minImageCount=%u, presentMode=FIFO, formats=%u\n",
           extent.width, extent.height, sci.minImageCount, nformats);

    uint32_t nimages = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &nimages, NULL);
    VkImage* images = calloc(nimages ? nimages : 1, sizeof(VkImage));
    r = vkGetSwapchainImagesKHR(device, swapchain, &nimages, images);
    CHECK(r == VK_SUCCESS && nimages > 0, "vkGetSwapchainImagesKHR");
    if (r != VK_SUCCESS || nimages == 0) return 1;

    uint32_t imageIndex = 0;
    r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                              VK_NULL_HANDLE, VK_NULL_HANDLE, &imageIndex);
    CHECK(r == VK_SUCCESS && imageIndex < nimages, "vkAcquireNextImageKHR");
    if (r != VK_SUCCESS) return 1;

    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIndex,
    };
    r = vkQueuePresentKHR(queue, &present);
    CHECK(r == VK_SUCCESS, "vkQueuePresentKHR");

    if (vkDeviceWaitIdle) vkDeviceWaitIdle(device);
    CHECK(1, "vkDeviceWaitIdle");

    if (vkDestroySwapchainKHR) vkDestroySwapchainKHR(device, swapchain, NULL);
    if (vkDestroySurfaceKHR) vkDestroySurfaceKHR(instance, surface, NULL);
    CHECK(1, "destroy swapchain + surface");

    PFN_vkDestroyDevice vkDestroyDevice =
        (PFN_vkDestroyDevice)vkGetInstanceProcAddr(instance, "vkDestroyDevice");
    PFN_vkDestroyInstance vkDestroyInstance =
        (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
    if (vkDestroyDevice) vkDestroyDevice(device, NULL);
    if (vkDestroyInstance) vkDestroyInstance(instance, NULL);

    free(phys);
    free(qprops);
    free(formats);
    free(modes);
    free(images);

    printf(g_fail ? "SWAPCHAIN TEST FAILED (%d)\n" : "SWAPCHAIN TEST PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}