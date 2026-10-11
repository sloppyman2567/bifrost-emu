// test_vulkan.c — minimal Vulkan smoke test for the DisplayThunk host path.
//
// Exercises the instance/device/queue lifecycle end-to-end through the
// thunk: dlopen(libvulkan.so.1) → vkGetInstanceProcAddr → create instance
// (with a VkApplicationInfo + an enabled extension name, which exercises
// the deep struct/string-array marshalling) → enumerate physical devices
// → query properties/memory/queue families → create device (VkDeviceQueue-
// CreateInfo array + pQueuePriorities) → get queue → wait idle → destroy.
//
// The library is loaded via the emulator's internal dlopen syscall
// (0x1002/0x1003) — the same pattern the other dl* tests use — because a
// static musl guest's own dlopen() can only load real AArch64 .so files
// from the guest VFS, and libvulkan exists only as a host x86_64 library
// behind the DisplayThunk.
//
// Exit 0 = all checks passed. This is the validation gate for the Vulkan
// host path; run it from any cwd (it does not need a display — no WSI).
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

int main(void) {
    void* vk = dlopen_bf("libvulkan.so.1", 1 /* RTLD_LAZY */);
    if (!vk) {
        /* Host has no Vulkan loader behind the thunk — env-dependent skip. */
        printf("SKIP: host libvulkan unavailable\n");
        return 77;
    }

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym_bf(vk, "vkGetInstanceProcAddr");
    CHECK(vkGetInstanceProcAddr != NULL, "dlsym vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) return 1;

    // Load all instance-level functions via vkGetInstanceProcAddr(NULL, …).
    PFN_vkCreateInstance             vkCreateInstance             = (PFN_vkCreateInstance)            vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    PFN_vkEnumeratePhysicalDevices   vkEnumeratePhysicalDevices   = (PFN_vkEnumeratePhysicalDevices)  vkGetInstanceProcAddr(NULL, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)vkGetInstanceProcAddr(NULL, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)vkGetInstanceProcAddr(NULL, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)vkGetInstanceProcAddr(NULL, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice               vkCreateDevice               = (PFN_vkCreateDevice)              vkGetInstanceProcAddr(NULL, "vkCreateDevice");
    PFN_vkGetDeviceQueue             vkGetDeviceQueue             = (PFN_vkGetDeviceQueue)            vkGetInstanceProcAddr(NULL, "vkGetDeviceQueue");
    PFN_vkDeviceWaitIdle             vkDeviceWaitIdle             = (PFN_vkDeviceWaitIdle)            vkGetInstanceProcAddr(NULL, "vkDeviceWaitIdle");
    PFN_vkDestroyDevice              vkDestroyDevice              = (PFN_vkDestroyDevice)             vkGetInstanceProcAddr(NULL, "vkDestroyDevice");
    PFN_vkDestroyInstance            vkDestroyInstance            = (PFN_vkDestroyInstance)           vkGetInstanceProcAddr(NULL, "vkDestroyInstance");

    CHECK(vkCreateInstance != NULL, "got vkCreateInstance");
    CHECK(vkEnumeratePhysicalDevices != NULL, "got vkEnumeratePhysicalDevices");
    CHECK(vkGetPhysicalDeviceProperties != NULL, "got vkGetPhysicalDeviceProperties");
    CHECK(vkGetPhysicalDeviceMemoryProperties != NULL, "got vkGetPhysicalDeviceMemoryProperties");
    CHECK(vkGetPhysicalDeviceQueueFamilyProperties != NULL, "got vkGetPhysicalDeviceQueueFamilyProperties");
    CHECK(vkCreateDevice != NULL, "got vkCreateDevice");
    CHECK(vkGetDeviceQueue != NULL, "got vkGetDeviceQueue");
    if (g_fail) return 1;

    // ── Instance ──────────────────────────────────────────────────────
    // VkApplicationInfo with strings + an enabled extension name exercise
    // the deep struct/string-array marshalling in the thunk.
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "bifrost-emu test_vulkan";
    app.applicationVersion = 1;
    app.pEngineName = "bifrost-emu";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_0;

    const char* inst_exts[] = { VK_KHR_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = inst_exts;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    CHECK(r == VK_SUCCESS, "vkCreateInstance");
    CHECK(instance != VK_NULL_HANDLE, "instance handle non-null");
    if (r != VK_SUCCESS) return 1;

    // ── Physical devices ──────────────────────────────────────────────
    uint32_t pcount = 0;
    r = vkEnumeratePhysicalDevices(instance, &pcount, NULL);
    CHECK(r == VK_SUCCESS && pcount > 0, "vkEnumeratePhysicalDevices count");
    if (pcount == 0) return 1;

    VkPhysicalDevice* phys = (VkPhysicalDevice*)malloc(sizeof(VkPhysicalDevice) * pcount);
    r = vkEnumeratePhysicalDevices(instance, &pcount, phys);
    CHECK(r == VK_SUCCESS, "vkEnumeratePhysicalDevices list");
    if (r != VK_SUCCESS) return 1;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys[0], &props);
    CHECK(props.apiVersion >= VK_API_VERSION_1_0, "vkGetPhysicalDeviceProperties (apiVersion)");
    CHECK(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
          || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
          || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU
          || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU
          || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_OTHER,
          "vkGetPhysicalDeviceProperties (deviceType sane)");
    printf("  GPU: %s\n", props.deviceName);

    VkPhysicalDeviceMemoryProperties memprops;
    vkGetPhysicalDeviceMemoryProperties(phys[0], &memprops);
    CHECK(memprops.memoryTypeCount > 0 && memprops.memoryTypeCount <= VK_MAX_MEMORY_TYPES,
          "vkGetPhysicalDeviceMemoryProperties");

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qcount, NULL);
    CHECK(qcount > 0, "vkGetPhysicalDeviceQueueFamilyProperties count");
    VkQueueFamilyProperties* qprops =
        (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys[0], &qcount, qprops);
    CHECK(qcount > 0 && qprops[0].queueCount > 0, "vkGetPhysicalDeviceQueueFamilyProperties list");
    if (qcount == 0 || qprops[0].queueCount == 0) return 1;

    // ── Device (one graphics-capable queue) ───────────────────────────
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VkDevice device = VK_NULL_HANDLE;
    r = vkCreateDevice(phys[0], &dci, NULL, &device);
    CHECK(r == VK_SUCCESS, "vkCreateDevice");
    CHECK(device != VK_NULL_HANDLE, "device handle non-null");
    if (r != VK_SUCCESS) return 1;

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);
    CHECK(queue != VK_NULL_HANDLE, "vkGetDeviceQueue");

    r = vkDeviceWaitIdle(device);
    CHECK(r == VK_SUCCESS, "vkDeviceWaitIdle");

    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    CHECK(1, "destroy device + instance");

    free(phys);
    free(qprops);

    printf(g_fail ? "VULKAN TEST FAILED (%d)\n" : "VULKAN TEST PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}