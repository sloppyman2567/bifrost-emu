// test_vulkan_swapchain.c — headless WSI swapchain smoke test (1.5.4-alpha).
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

    /* ── Real frame: render-pass clear + submit + present ──────────────
     * Records a command buffer that begins a render pass (clearing the
     * swapchain image to red), ends it, submits the buffer to the queue,
     * and presents. Exercises the whole command-recording + submission
     * path through the DisplayThunk: vkCmd* family (new rows), the
     * VK_SUBMIT / VK_BEGIN_RENDERPASS / VK_CREATE_RENDERPASS /
     * VK_CREATE_FRAMEBUFFER deep-marshal arms, and the existing
     * vkQueuePresentKHR arm. */
    PFN_vkCreateCommandPool vkCreateCommandPool =
        (PFN_vkCreateCommandPool)vkGetDeviceProcAddr(device, "vkCreateCommandPool");
    PFN_vkDestroyCommandPool vkDestroyCommandPool =
        (PFN_vkDestroyCommandPool)vkGetDeviceProcAddr(device, "vkDestroyCommandPool");
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers =
        (PFN_vkAllocateCommandBuffers)vkGetDeviceProcAddr(device, "vkAllocateCommandBuffers");
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers =
        (PFN_vkFreeCommandBuffers)vkGetDeviceProcAddr(device, "vkFreeCommandBuffers");
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer =
        (PFN_vkBeginCommandBuffer)vkGetDeviceProcAddr(device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer vkEndCommandBuffer =
        (PFN_vkEndCommandBuffer)vkGetDeviceProcAddr(device, "vkEndCommandBuffer");
    PFN_vkCreateRenderPass vkCreateRenderPass =
        (PFN_vkCreateRenderPass)vkGetDeviceProcAddr(device, "vkCreateRenderPass");
    PFN_vkDestroyRenderPass vkDestroyRenderPass =
        (PFN_vkDestroyRenderPass)vkGetDeviceProcAddr(device, "vkDestroyRenderPass");
    PFN_vkCreateImageView vkCreateImageView =
        (PFN_vkCreateImageView)vkGetDeviceProcAddr(device, "vkCreateImageView");
    PFN_vkDestroyImageView vkDestroyImageView =
        (PFN_vkDestroyImageView)vkGetDeviceProcAddr(device, "vkDestroyImageView");
    PFN_vkCreateFramebuffer vkCreateFramebuffer =
        (PFN_vkCreateFramebuffer)vkGetDeviceProcAddr(device, "vkCreateFramebuffer");
    PFN_vkDestroyFramebuffer vkDestroyFramebuffer =
        (PFN_vkDestroyFramebuffer)vkGetDeviceProcAddr(device, "vkDestroyFramebuffer");
    PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass =
        (PFN_vkCmdBeginRenderPass)vkGetDeviceProcAddr(device, "vkCmdBeginRenderPass");
    PFN_vkCmdEndRenderPass vkCmdEndRenderPass =
        (PFN_vkCmdEndRenderPass)vkGetDeviceProcAddr(device, "vkCmdEndRenderPass");
    PFN_vkQueueSubmit vkQueueSubmit =
        (PFN_vkQueueSubmit)vkGetDeviceProcAddr(device, "vkQueueSubmit");
    CHECK(vkCreateCommandPool != NULL && vkBeginCommandBuffer != NULL &&
          vkEndCommandBuffer != NULL, "got command-buffer fns");
    CHECK(vkCreateRenderPass != NULL && vkCreateFramebuffer != NULL, "got render-pass fns");
    CHECK(vkCmdBeginRenderPass != NULL && vkCmdEndRenderPass != NULL, "got cmd render-pass fns");
    CHECK(vkQueueSubmit != NULL, "got vkQueueSubmit");
    if (!vkCreateCommandPool || !vkBeginCommandBuffer || !vkEndCommandBuffer ||
        !vkCreateRenderPass || !vkCreateFramebuffer || !vkCmdBeginRenderPass ||
        !vkCmdEndRenderPass || !vkQueueSubmit) return 1;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = present_family,
    };
    r = vkCreateCommandPool(device, &cpci, NULL, &pool);
    CHECK(r == VK_SUCCESS && pool != VK_NULL_HANDLE, "vkCreateCommandPool");
    if (r != VK_SUCCESS) return 1;

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    r = vkAllocateCommandBuffers(device, &cbai, &cmd);
    CHECK(r == VK_SUCCESS && cmd != VK_NULL_HANDLE, "vkAllocateCommandBuffers");
    if (r != VK_SUCCESS) return 1;

    VkAttachmentDescription att = {
        .format = formats[0].format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
    };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };
    VkRenderPass renderpass = VK_NULL_HANDLE;
    r = vkCreateRenderPass(device, &rpci, NULL, &renderpass);
    CHECK(r == VK_SUCCESS && renderpass != VK_NULL_HANDLE, "vkCreateRenderPass");
    if (r != VK_SUCCESS) return 1;

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = images[imageIndex],
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = formats[0].format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    VkImageView view = VK_NULL_HANDLE;
    r = vkCreateImageView(device, &ivci, NULL, &view);
    CHECK(r == VK_SUCCESS && view != VK_NULL_HANDLE, "vkCreateImageView");
    if (r != VK_SUCCESS) return 1;

    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = renderpass,
        .attachmentCount = 1,
        .pAttachments = &view,
        .width = extent.width,
        .height = extent.height,
        .layers = 1,
    };
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    r = vkCreateFramebuffer(device, &fbci, NULL, &framebuffer);
    CHECK(r == VK_SUCCESS && framebuffer != VK_NULL_HANDLE, "vkCreateFramebuffer");
    if (r != VK_SUCCESS) return 1;

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    r = vkBeginCommandBuffer(cmd, &cbbi);
    CHECK(r == VK_SUCCESS, "vkBeginCommandBuffer");

    VkClearValue clear = { .color = { { 1.0f, 0.2f, 0.2f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = renderpass,
        .framebuffer = framebuffer,
        .renderArea = { .offset = { 0, 0 }, .extent = extent },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cmd);
    r = vkEndCommandBuffer(cmd);
    CHECK(r == VK_SUCCESS, "record clear frame + vkEndCommandBuffer");

    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    r = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    CHECK(r == VK_SUCCESS, "vkQueueSubmit");
    if (r != VK_SUCCESS) return 1;

    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &imageIndex,
    };
    r = vkQueuePresentKHR(queue, &present);
    CHECK(r == VK_SUCCESS, "vkQueuePresentKHR after submit");

    if (vkDeviceWaitIdle) vkDeviceWaitIdle(device);
    CHECK(1, "vkDeviceWaitIdle");

    if (vkDestroyFramebuffer) vkDestroyFramebuffer(device, framebuffer, NULL);
    if (vkDestroyImageView) vkDestroyImageView(device, view, NULL);
    if (vkDestroyRenderPass) vkDestroyRenderPass(device, renderpass, NULL);
    if (vkFreeCommandBuffers) vkFreeCommandBuffers(device, pool, 1, &cmd);
    CHECK(1, "destroy clear-frame objects");

    /* ── Graphics pipeline + real vkCmdDraw triangle (2026-08-21) ──────
     * Exercises the new deep-marshal arms: vkCreateShaderModule (nested
     * pCode), vkCreateGraphicsPipelines (the full state-struct tree),
     * vkCreatePipelineLayout, vkCreateDescriptorSetLayout / Pool /
     * Allocate / Update (UBO binding), plus per-image framebuffers,
     * a depth attachment, vertex buffer upload via vkCmdUpdateBuffer
     * (avoids vkMapMemory — its host-pointer bounce is future work),
     * and a 3-frame draw loop through per-image command buffers. */
    #include "test_vulkan_spv.h"

    PFN_vkCreateShaderModule vkCreateShaderModule =
        (PFN_vkCreateShaderModule)vkGetDeviceProcAddr(device, "vkCreateShaderModule");
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines =
        (PFN_vkCreateGraphicsPipelines)vkGetDeviceProcAddr(device, "vkCreateGraphicsPipelines");
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout =
        (PFN_vkCreatePipelineLayout)vkGetDeviceProcAddr(device, "vkCreatePipelineLayout");
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout =
        (PFN_vkCreateDescriptorSetLayout)vkGetDeviceProcAddr(device, "vkCreateDescriptorSetLayout");
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool =
        (PFN_vkCreateDescriptorPool)vkGetDeviceProcAddr(device, "vkCreateDescriptorPool");
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets =
        (PFN_vkAllocateDescriptorSets)vkGetDeviceProcAddr(device, "vkAllocateDescriptorSets");
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets =
        (PFN_vkUpdateDescriptorSets)vkGetDeviceProcAddr(device, "vkUpdateDescriptorSets");
    PFN_vkCreateBuffer vkCreateBuffer =
        (PFN_vkCreateBuffer)vkGetDeviceProcAddr(device, "vkCreateBuffer");
    PFN_vkAllocateMemory vkAllocateMemory =
        (PFN_vkAllocateMemory)vkGetDeviceProcAddr(device, "vkAllocateMemory");
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements =
        (PFN_vkGetBufferMemoryRequirements)vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements");
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements =
        (PFN_vkGetImageMemoryRequirements)vkGetDeviceProcAddr(device, "vkGetImageMemoryRequirements");
    PFN_vkBindBufferMemory vkBindBufferMemory =
        (PFN_vkBindBufferMemory)vkGetDeviceProcAddr(device, "vkBindBufferMemory");
    PFN_vkCreateImage vkCreateImage =
        (PFN_vkCreateImage)vkGetDeviceProcAddr(device, "vkCreateImage");
    PFN_vkBindImageMemory vkBindImageMemory =
        (PFN_vkBindImageMemory)vkGetDeviceProcAddr(device, "vkBindImageMemory");
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers =
        (PFN_vkCmdBindVertexBuffers)vkGetDeviceProcAddr(device, "vkCmdBindVertexBuffers");
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets =
        (PFN_vkCmdBindDescriptorSets)vkGetDeviceProcAddr(device, "vkCmdBindDescriptorSets");
    PFN_vkCmdDraw vkCmdDraw =
        (PFN_vkCmdDraw)vkGetDeviceProcAddr(device, "vkCmdDraw");
    PFN_vkCmdUpdateBuffer vkCmdUpdateBuffer =
        (PFN_vkCmdUpdateBuffer)vkGetDeviceProcAddr(device, "vkCmdUpdateBuffer");
    PFN_vkCmdBindPipeline vkCmdBindPipeline =
        (PFN_vkCmdBindPipeline)vkGetDeviceProcAddr(device, "vkCmdBindPipeline");
    CHECK(vkCreateShaderModule && vkCreateGraphicsPipelines &&
          vkCreatePipelineLayout && vkCreateDescriptorSetLayout, "got pipeline fns");
    CHECK(vkCreateBuffer && vkAllocateMemory && vkBindBufferMemory, "got buffer fns");
    CHECK(vkCmdDraw && vkCmdBindVertexBuffers && vkCmdBindDescriptorSets &&
          vkCmdBindPipeline && vkCmdUpdateBuffer, "got draw fns");
    if (!vkCreateShaderModule || !vkCreateGraphicsPipelines || !vkCreatePipelineLayout ||
        !vkCreateDescriptorSetLayout || !vkCreateBuffer || !vkAllocateMemory ||
        !vkCmdDraw || !vkCmdBindVertexBuffers || !vkCmdBindPipeline) return 1;

    /* Shader modules (nested pCode through the arm). */
    VkShaderModule vs_module = VK_NULL_HANDLE, fs_module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(kVertSpv),
        .pCode = kVertSpv,
    };
    r = vkCreateShaderModule(device, &smci, NULL, &vs_module);
    CHECK(r == VK_SUCCESS && vs_module != VK_NULL_HANDLE, "vkCreateShaderModule (vertex)");
    smci.pCode = kFragSpv; smci.codeSize = sizeof(kFragSpv);
    r = vkCreateShaderModule(device, &smci, NULL, &fs_module);
    CHECK(r == VK_SUCCESS && fs_module != VK_NULL_HANDLE, "vkCreateShaderModule (fragment)");
    if (r != VK_SUCCESS) return 1;

    /* Buffers: interleaved vertex data (pos vec2 + col vec3) + UBO vec4.
     * Filled per frame via vkCmdUpdateBuffer — no vkMapMemory needed. */
    VkBuffer vbuf = VK_NULL_HANDLE, ubobuf = VK_NULL_HANDLE;
    VkMemoryRequirements vreq = {0};
    VkDeviceMemory vmem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 3 * 20,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    r = vkCreateBuffer(device, &bci, NULL, &vbuf);
    CHECK(r == VK_SUCCESS && vbuf != VK_NULL_HANDLE, "vkCreateBuffer (vertex)");
    bci.size = 16;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    r = vkCreateBuffer(device, &bci, NULL, &ubobuf);
    CHECK(r == VK_SUCCESS && ubobuf != VK_NULL_HANDLE, "vkCreateBuffer (ubo)");
    vkGetBufferMemoryRequirements(device, vbuf, &vreq);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = vreq.size,
    };
    /* Find HOST_VISIBLE|HOST_COHERENT memory type. */
    VkPhysicalDeviceMemoryProperties mp = {0};
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)vkGetInstanceProcAddr(instance,
            "vkGetPhysicalDeviceMemoryProperties");
    CHECK(vkGetPhysicalDeviceMemoryProperties != NULL, "got memprops fn");
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    mai.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((vreq.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mai.memoryTypeIndex = i;
            break;
        }
    }
    CHECK(mai.memoryTypeIndex != UINT32_MAX, "found HOST_VISIBLE|COHERENT memory type");
    if (mai.memoryTypeIndex == UINT32_MAX) return 1;
    r = vkAllocateMemory(device, &mai, NULL, &vmem);
    CHECK(r == VK_SUCCESS && vmem != VK_NULL_HANDLE, "vkAllocateMemory (host visible)");
    if (r != VK_SUCCESS) return 1;
    r = vkBindBufferMemory(device, vbuf, vmem, 0);
    CHECK(r == VK_SUCCESS, "vkBindBufferMemory (vertex)");
    r = vkBindBufferMemory(device, ubobuf, vmem, 0);
    CHECK(r == VK_SUCCESS, "vkBindBufferMemory (ubo)");
    if (r != VK_SUCCESS) return 1;

    /* Map the UBO memory: the emulator's vkMapMemory bounce returns a
     * GUEST address inside the 4 GiB direct window — assert exactly
     * that (nonzero, < 4 GiB) and write the UBO through it directly. */
    PFN_vkMapMemory vkMapMemory =
        (PFN_vkMapMemory)vkGetDeviceProcAddr(device, "vkMapMemory");
    PFN_vkUnmapMemory vkUnmapMemory =
        (PFN_vkUnmapMemory)vkGetDeviceProcAddr(device, "vkUnmapMemory");
    PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges =
        (PFN_vkFlushMappedMemoryRanges)vkGetDeviceProcAddr(device, "vkFlushMappedMemoryRanges");
    CHECK(vkMapMemory && vkUnmapMemory && vkFlushMappedMemoryRanges, "got map fns");
    void* mapped = NULL;
    r = vkMapMemory ? vkMapMemory(device, vmem, 0, 16, 0, &mapped) : VK_ERROR_INITIALIZATION_FAILED;
    CHECK(r == VK_SUCCESS && mapped != NULL, "vkMapMemory (ubo)");
    CHECK(mapped != NULL && ((uintptr_t)mapped >> 32) == 0,
          "mapped pointer is a guest address inside the direct window");
    if (r != VK_SUCCESS || !mapped) return 1;
    float* ubo_words = (float*)mapped;

    /* Depth image + view (D32_SFLOAT, shared across framebuffers). */
    VkImage depth_image = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkImageCreateInfo dici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    r = vkCreateImage(device, &dici, NULL, &depth_image);
    CHECK(r == VK_SUCCESS && depth_image != VK_NULL_HANDLE, "vkCreateImage (depth)");
    VkMemoryRequirements dreq = {0};
    vkGetImageMemoryRequirements(device, depth_image, &dreq);
    mai.allocationSize = dreq.size;
    mai.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((dreq.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mai.memoryTypeIndex = i;
            break;
        }
    }
    CHECK(mai.memoryTypeIndex != UINT32_MAX, "found DEVICE_LOCAL memory type");
    VkDeviceMemory dmem = VK_NULL_HANDLE;
    r = mai.memoryTypeIndex != UINT32_MAX
        ? vkAllocateMemory(device, &mai, NULL, &dmem) : VK_ERROR_INITIALIZATION_FAILED;
    CHECK(r == VK_SUCCESS && dmem != VK_NULL_HANDLE, "vkAllocateMemory (device local)");
    if (r != VK_SUCCESS) return 1;
    r = vkBindImageMemory(device, depth_image, dmem, 0);
    CHECK(r == VK_SUCCESS, "vkBindImageMemory (depth)");
    VkImageViewCreateInfo dvci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D32_SFLOAT,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    r = vkCreateImageView(device, &dvci, NULL, &depth_view);
    CHECK(r == VK_SUCCESS && depth_view != VK_NULL_HANDLE, "vkCreateImageView (depth)");
    if (r != VK_SUCCESS) return 1;

    /* Render pass #2: color + depth attachments. */
    VkAttachmentDescription atts2[2] = {
        {
            .format = formats[0].format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        },
        {
            .format = VK_FORMAT_D32_SFLOAT,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };
    VkAttachmentReference refs2[2] = {
        { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        { .attachment = 1, .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
    };
    VkSubpassDescription subpass2 = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &refs2[0],
        .pDepthStencilAttachment = &refs2[1],
    };
    VkRenderPassCreateInfo rpci2 = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = atts2,
        .subpassCount = 1,
        .pSubpasses = &subpass2,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };
    VkRenderPass renderpass2 = VK_NULL_HANDLE;
    r = vkCreateRenderPass(device, &rpci2, NULL, &renderpass2);
    CHECK(r == VK_SUCCESS && renderpass2 != VK_NULL_HANDLE, "vkCreateRenderPass (color+depth)");
    if (r != VK_SUCCESS) return 1;

    /* Per-image color views + framebuffers. */
    VkImageView* cviews = calloc(nimages, sizeof(VkImageView));
    VkFramebuffer* fbs = calloc(nimages, sizeof(VkFramebuffer));
    for (uint32_t i = 0; i < nimages; i++) {
        VkImageViewCreateInfo cvci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = formats[0].format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        r = vkCreateImageView(device, &cvci, NULL, &cviews[i]);
        if (r != VK_SUCCESS) break;
        VkImageView fbatts[2] = { cviews[i], depth_view };
        VkFramebufferCreateInfo fbci2 = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = renderpass2,
            .attachmentCount = 2,
            .pAttachments = fbatts,
            .width = extent.width,
            .height = extent.height,
            .layers = 1,
        };
        r = vkCreateFramebuffer(device, &fbci2, NULL, &fbs[i]);
        if (r != VK_SUCCESS) break;
    }
    CHECK(r == VK_SUCCESS, "per-image views + framebuffers (color+depth)");
    if (r != VK_SUCCESS) return 1;

    /* Descriptor set: one UBO at binding 0, vertex stage. */
    VkDescriptorSetLayoutBinding dslb = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &dslb,
    };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    r = vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl);
    CHECK(r == VK_SUCCESS && dsl != VK_NULL_HANDLE, "vkCreateDescriptorSetLayout");
    if (r != VK_SUCCESS) return 1;
    VkDescriptorPoolSize poolsz = {
        .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &poolsz,
    };
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    r = vkCreateDescriptorPool(device, &dpci, NULL, &dpool);
    CHECK(r == VK_SUCCESS && dpool != VK_NULL_HANDLE, "vkCreateDescriptorPool");
    if (r != VK_SUCCESS) return 1;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dsl,
    };
    VkDescriptorSet dset = VK_NULL_HANDLE;
    r = vkAllocateDescriptorSets(device, &dsai, &dset);
    CHECK(r == VK_SUCCESS && dset != VK_NULL_HANDLE, "vkAllocateDescriptorSets");
    if (r != VK_SUCCESS) return 1;
    VkDescriptorBufferInfo dbi = { .buffer = ubobuf, .offset = 0, .range = 16 };
    VkWriteDescriptorSet dwrite = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = dset,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &dbi,
    };
    vkUpdateDescriptorSets(device, 1, &dwrite, 0, NULL);
    CHECK(1, "vkUpdateDescriptorSets (ubo write)");

    /* Pipeline layout + graphics pipeline (full state tree). */
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dsl,
    };
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    r = vkCreatePipelineLayout(device, &plci, NULL, &pipe_layout);
    CHECK(r == VK_SUCCESS && pipe_layout != VK_NULL_HANDLE, "vkCreatePipelineLayout");
    if (r != VK_SUCCESS) return 1;

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vs_module,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fs_module,
            .pName = "main",
        },
    };
    VkVertexInputBindingDescription vbind = { .binding = 0, .stride = 20,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription vattrs[2] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 8 },
    };
    VkPipelineVertexInputStateCreateInfo visa = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vbind,
        .vertexAttributeDescriptionCount = 2,
        .pVertexAttributeDescriptions = vattrs,
    };
    VkPipelineInputAssemblyStateCreateInfo iasc = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport viewport = { .x = 0.0f, .y = 0.0f,
        .width = (float)extent.width, .height = (float)extent.height,
        .minDepth = 0.0f, .maxDepth = 1.0f };
    VkRect2D scissor = { .offset = { 0, 0 }, .extent = extent };
    VkPipelineViewportStateCreateInfo vpsc = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo rast = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo msc = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo dss = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
    };
    VkPipelineColorBlendAttachmentState cba = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cbs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cba,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &visa,
        .pInputAssemblyState = &iasc,
        .pViewportState = &vpsc,
        .pRasterizationState = &rast,
        .pMultisampleState = &msc,
        .pDepthStencilState = &dss,
        .pColorBlendState = &cbs,
        .layout = pipe_layout,
        .renderPass = renderpass2,
        .subpass = 0,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipeline);
    CHECK(r == VK_SUCCESS && pipeline != VK_NULL_HANDLE, "vkCreateGraphicsPipelines (full tree)");
    if (r != VK_SUCCESS) return 1;

    /* Per-image command buffers + 3-frame draw loop. */
    VkCommandBufferAllocateInfo cbai2 = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = nimages,
    };
    VkCommandBuffer* cmds = calloc(nimages, sizeof(VkCommandBuffer));
    r = vkAllocateCommandBuffers(device, &cbai2, cmds);
    CHECK(r == VK_SUCCESS, "vkAllocateCommandBuffers (per-image)");
    if (r != VK_SUCCESS) return 1;

    static const float verts[3][5] = {
        { -0.7f, -0.7f,  1.0f, 0.2f, 0.2f },
        {  0.7f, -0.7f,  0.2f, 1.0f, 0.2f },
        {  0.0f,  0.7f,  0.2f, 0.2f, 1.0f },
    };
    int frames_ok = 1;
    for (int frame = 0; frame < 3; frame++) {
        uint32_t idx = 0;
        r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                  VK_NULL_HANDLE, VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS) { frames_ok = 0; break; }
        VkCommandBufferBeginInfo cbbi2 = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        r = vkBeginCommandBuffer(cmds[idx], &cbbi2);
        if (r != VK_SUCCESS) { frames_ok = 0; break; }
        /* Vertex data via vkCmdUpdateBuffer; UBO written DIRECTLY
         * through the mapped pointer — the submit-side push makes it
         * GPU-visible (coherent-style), the flush exercises the range
         * arm explicitly. */
        ubo_words[0] = 1.0f; ubo_words[1] = 1.0f; ubo_words[2] = 1.0f;
        ubo_words[3] = 0.05f * (float)frame;
        vkCmdUpdateBuffer(cmds[idx], vbuf, 0, sizeof(verts), verts);
        VkMappedMemoryRange flush_range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = vmem,
            .offset = 0,
            .size = 16,
        };
        vkFlushMappedMemoryRanges(device, 1, &flush_range);
        VkClearValue clears[2] = {
            { .color = { { 0.06f, 0.06f, 0.09f, 1.0f } } },
            { .depthStencil = { .depth = 1.0f, .stencil = 0 } },
        };
        VkRenderPassBeginInfo rpbi2 = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = renderpass2,
            .framebuffer = fbs[idx],
            .renderArea = { .offset = { 0, 0 }, .extent = extent },
            .clearValueCount = 2,
            .pClearValues = clears,
        };
        vkCmdBeginRenderPass(cmds[idx], &rpbi2, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmds[idx], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmds[idx], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipe_layout, 0, 1, &dset, 0, NULL);
        VkDeviceSize voff = 0;
        vkCmdBindVertexBuffers(cmds[idx], 0, 1, &vbuf, &voff);
        vkCmdDraw(cmds[idx], 3, 1, 0, 0);
        vkCmdEndRenderPass(cmds[idx]);
        r = vkEndCommandBuffer(cmds[idx]);
        if (r != VK_SUCCESS) { frames_ok = 0; break; }
        VkSubmitInfo submit2 = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmds[idx],
        };
        r = vkQueueSubmit(queue, 1, &submit2, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) { frames_ok = 0; break; }
        VkPresentInfoKHR present2 = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &idx,
        };
        r = vkQueuePresentKHR(queue, &present2);
        if (r != VK_SUCCESS) { frames_ok = 0; break; }
        if (vkDeviceWaitIdle) vkDeviceWaitIdle(device);
    }
    CHECK(frames_ok, "3-frame vkCmdDraw triangle loop (pipeline+descriptors+depth+mapmemory)");

    /* Unmap: pushes the bounce back and releases the window range. */
    if (vkUnmapMemory) vkUnmapMemory(device, vmem);
    CHECK(1, "vkUnmapMemory (ubo bounce released)");

    /* Teardown of the triangle phase. */
    if (vkFreeCommandBuffers) vkFreeCommandBuffers(device, pool, nimages, cmds);
    PFN_vkDestroyPipeline vkDestroyPipeline =
        (PFN_vkDestroyPipeline)vkGetDeviceProcAddr(device, "vkDestroyPipeline");
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout =
        (PFN_vkDestroyPipelineLayout)vkGetDeviceProcAddr(device, "vkDestroyPipelineLayout");
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool =
        (PFN_vkDestroyDescriptorPool)vkGetDeviceProcAddr(device, "vkDestroyDescriptorPool");
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout =
        (PFN_vkDestroyDescriptorSetLayout)vkGetDeviceProcAddr(device, "vkDestroyDescriptorSetLayout");
    PFN_vkDestroyBuffer vkDestroyBuffer =
        (PFN_vkDestroyBuffer)vkGetDeviceProcAddr(device, "vkDestroyBuffer");
    PFN_vkDestroyImage vkDestroyImage =
        (PFN_vkDestroyImage)vkGetDeviceProcAddr(device, "vkDestroyImage");
    PFN_vkFreeMemory vkFreeMemory =
        (PFN_vkFreeMemory)vkGetDeviceProcAddr(device, "vkFreeMemory");
    PFN_vkDestroyShaderModule vkDestroyShaderModule =
        (PFN_vkDestroyShaderModule)vkGetDeviceProcAddr(device, "vkDestroyShaderModule");
    if (vkDestroyPipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (vkDestroyPipelineLayout) vkDestroyPipelineLayout(device, pipe_layout, NULL);
    if (vkDestroyDescriptorPool) vkDestroyDescriptorPool(device, dpool, NULL);
    if (vkDestroyDescriptorSetLayout) vkDestroyDescriptorSetLayout(device, dsl, NULL);
    if (vkDestroyBuffer) { vkDestroyBuffer(device, vbuf, NULL); vkDestroyBuffer(device, ubobuf, NULL); }
    if (vkDestroyImage) vkDestroyImage(device, depth_image, NULL);
    for (uint32_t i = 0; i < nimages; i++) {
        if (vkDestroyFramebuffer) vkDestroyFramebuffer(device, fbs[i], NULL);
        if (vkDestroyImageView) vkDestroyImageView(device, cviews[i], NULL);
    }
    if (vkDestroyRenderPass) vkDestroyRenderPass(device, renderpass2, NULL);
    if (vkDestroyImageView) vkDestroyImageView(device, depth_view, NULL);
    if (vkFreeMemory) { vkFreeMemory(device, vmem, NULL); vkFreeMemory(device, dmem, NULL); }
    if (vkDestroyShaderModule) {
        vkDestroyShaderModule(device, vs_module, NULL);
        vkDestroyShaderModule(device, fs_module, NULL);
    }
    CHECK(1, "destroy triangle objects");

    if (vkDestroyCommandPool) vkDestroyCommandPool(device, pool, NULL);
    CHECK(1, "destroy command pool");

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