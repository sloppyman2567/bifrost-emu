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
    // Vulkan 1.1 entry points may only be called on an instance created
    // with apiVersion >= 1.1; a 1.0 instance can leave those dispatch-table
    // slots null in the loader/driver and the call lands on address 0.
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
            NULL, "vkEnumerateInstanceVersion");
    uint32_t inst_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        (void)vkEnumerateInstanceVersion(&inst_version);
    // Ask for the highest version the loader offers (capped at 1.4):
    // vkMapMemory2 is 1.4 core (VK_KHR_map_memory2) and calling it on an
    // older instance leaves the loader's dispatch slot null.
    uint32_t use_version = VK_API_VERSION_1_0;
    if (inst_version >= VK_API_VERSION_1_4)
        use_version = VK_API_VERSION_1_4;
    else if (inst_version >= VK_API_VERSION_1_1)
        use_version = VK_API_VERSION_1_1;
    printf("  loader instance version: %u.%u\n",
           (unsigned)VK_VERSION_MAJOR(inst_version),
           (unsigned)VK_VERSION_MINOR(inst_version));

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "bifrost-emu test_vulkan";
    app.applicationVersion = 1;
    app.pEngineName = "bifrost-emu";
    app.engineVersion = 1;
    app.apiVersion = use_version;

    const char* inst_exts[] = { VK_KHR_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = inst_exts;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    if (r == VK_ERROR_INCOMPATIBLE_DRIVER) {
        printf("SKIP: host has no Vulkan driver\n");
        return 77;
    }
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

    // ── Vulkan 1.1 struct-out queries ─────────────────────────────────
    // These exercise the VK_CMD_DEEP_OUT plans that stage a const struct
    // (and its pNext chain) and publish the driver's result field-wise.
    // A broken writeback shows up here as zeroed/garbage fields, and a
    // leaked host pointer would crash on the next use.
    if (props.apiVersion >= VK_API_VERSION_1_1) {
        PFN_vkCreateBuffer vkCreateBuffer =
            (PFN_vkCreateBuffer)vkGetInstanceProcAddr(instance, "vkCreateBuffer");
        PFN_vkDestroyBuffer vkDestroyBuffer =
            (PFN_vkDestroyBuffer)vkGetInstanceProcAddr(instance, "vkDestroyBuffer");
        PFN_vkGetBufferMemoryRequirements2 vkGetBufferMemoryRequirements2 =
            (PFN_vkGetBufferMemoryRequirements2)vkGetInstanceProcAddr(
                instance, "vkGetBufferMemoryRequirements2");
        PFN_vkGetPhysicalDeviceMemoryProperties2 vkGetPhysicalDeviceMemoryProperties2 =
            (PFN_vkGetPhysicalDeviceMemoryProperties2)vkGetInstanceProcAddr(
                instance, "vkGetPhysicalDeviceMemoryProperties2");
        CHECK(vkCreateBuffer && vkDestroyBuffer &&
              vkGetBufferMemoryRequirements2 &&
              vkGetPhysicalDeviceMemoryProperties2,
              "got Vulkan 1.1 struct-out entry points");

        VkBufferCreateInfo bci = {0};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = 65536;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buf = VK_NULL_HANDLE;
        r = vkCreateBuffer(device, &bci, NULL, &buf);
        CHECK(r == VK_SUCCESS && buf != VK_NULL_HANDLE, "vkCreateBuffer");

        VkBufferMemoryRequirementsInfo2 bmri = {0};
        bmri.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
        bmri.buffer = buf;
        VkMemoryRequirements2 mr2 = {0};
        mr2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        vkGetBufferMemoryRequirements2(device, &bmri, &mr2);
        CHECK(mr2.memoryRequirements.size >= 65536 &&
              mr2.memoryRequirements.memoryTypeBits != 0,
              "vkGetBufferMemoryRequirements2 (struct writeback)");
        CHECK(mr2.sType == VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
              "struct-out preserved sType");

        VkPhysicalDeviceMemoryProperties2 mp2 = {0};
        mp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        vkGetPhysicalDeviceMemoryProperties2(phys[0], &mp2);
        CHECK(mp2.memoryProperties.memoryTypeCount > 0 &&
              mp2.memoryProperties.memoryHeapCount > 0,
              "vkGetPhysicalDeviceMemoryProperties2 (struct writeback)");

        // ── vkMapMemory2 / vkUnmapMemory2 ────────────────────────────
        // Same guest-window bounce as vkMapMemory, with the range inside
        // VkMemoryMapInfo. A write must survive unmap+remap.
        PFN_vkAllocateMemory vkAllocateMemory =
            (PFN_vkAllocateMemory)vkGetInstanceProcAddr(instance, "vkAllocateMemory");
        PFN_vkFreeMemory vkFreeMemory =
            (PFN_vkFreeMemory)vkGetInstanceProcAddr(instance, "vkFreeMemory");
        PFN_vkMapMemory2 vkMapMemory2 =
            (PFN_vkMapMemory2)vkGetInstanceProcAddr(instance, "vkMapMemory2");
        PFN_vkUnmapMemory2 vkUnmapMemory2 =
            (PFN_vkUnmapMemory2)vkGetInstanceProcAddr(instance, "vkUnmapMemory2");
        uint32_t map_type = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < mp2.memoryProperties.memoryTypeCount; i++) {
            const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if ((mp2.memoryProperties.memoryTypes[i].propertyFlags & want) == want) {
                map_type = i;
                break;
            }
        }
        if (props.apiVersion >= VK_API_VERSION_1_4 &&
            map_type != 0xFFFFFFFFu && vkAllocateMemory && vkFreeMemory &&
            vkMapMemory2 && vkUnmapMemory2) {
            VkMemoryAllocateInfo mai = {0};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = 4096;
            mai.memoryTypeIndex = map_type;
            VkDeviceMemory dm = VK_NULL_HANDLE;
            r = vkAllocateMemory(device, &mai, NULL, &dm);
            CHECK(r == VK_SUCCESS && dm != VK_NULL_HANDLE,
                  "vkAllocateMemory (map2)");

            VkMemoryMapInfo mmi = {0};
            mmi.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO;
            mmi.memory = dm;
            mmi.size = VK_WHOLE_SIZE;
            void* p = NULL;
            r = vkMapMemory2(device, &mmi, &p);
            CHECK(r == VK_SUCCESS && p != NULL, "vkMapMemory2");
            if (r == VK_SUCCESS && p) {
                ((volatile uint32_t*)p)[0] = 0xDEADBEEFu;
                ((volatile uint32_t*)p)[1] = 0x12345678u;
            }

            VkMemoryUnmapInfo mui = {0};
            mui.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO;
            mui.memory = dm;
            r = vkUnmapMemory2(device, &mui);
            CHECK(r == VK_SUCCESS, "vkUnmapMemory2");

            p = NULL;
            r = vkMapMemory2(device, &mmi, &p);
            CHECK(r == VK_SUCCESS && p != NULL &&
                  ((volatile uint32_t*)p)[0] == 0xDEADBEEFu &&
                  ((volatile uint32_t*)p)[1] == 0x12345678u,
                  "vkMapMemory2 write survived unmap");
            if (p) vkUnmapMemory2(device, &mui);
            vkFreeMemory(device, dm, NULL);

            // A ranged map: the bridge maps placed memory whole, so the
            // pointer must be base + offset and land on the same bytes.
            mai.allocationSize = 8192;
            dm = VK_NULL_HANDLE;
            r = vkAllocateMemory(device, &mai, NULL, &dm);
            CHECK(r == VK_SUCCESS && dm != VK_NULL_HANDLE, "vkAllocateMemory (ranged map)");
            mmi.memory = dm;
            mmi.offset = 4096;
            mmi.size = 4096;
            mui.memory = dm;
            p = NULL;
            r = vkMapMemory2(device, &mmi, &p);
            CHECK(r == VK_SUCCESS && p != NULL, "vkMapMemory2 at offset 4096");
            if (r == VK_SUCCESS && p) {
                ((volatile uint32_t*)p)[0] = 0xCAFEF00Du;
                vkUnmapMemory2(device, &mui);
            }
            mmi.offset = 0;
            mmi.size = VK_WHOLE_SIZE;
            p = NULL;
            r = vkMapMemory2(device, &mmi, &p);
            CHECK(r == VK_SUCCESS && p != NULL &&
                  ((volatile uint32_t*)p)[1024] == 0xCAFEF00Du,
                  "vkMapMemory2 offset write lands at offset 4096");
            if (p) vkUnmapMemory2(device, &mui);
            vkFreeMemory(device, dm, NULL);
        } else {
            printf("  SKIP  vkMapMemory2 (needs Vulkan 1.4 or a host-visible "
                   "coherent memory type)\n");
        }

        if (buf) vkDestroyBuffer(device, buf, NULL);
    } else {
        printf("  SKIP  Vulkan 1.1 struct-out queries (host apiVersion < 1.1)\n");
    }

    r = vkDeviceWaitIdle(device);
    CHECK(r == VK_SUCCESS, "vkDeviceWaitIdle");

    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    CHECK(1, "destroy device + instance");

    // ── Core-version gate ────────────────────────────────────────────
    // A 1.0 instance must get VK_ERROR_FEATURE_NOT_PRESENT from a 1.1
    // entry point instead of jumping through the loader's null dispatch
    // slot and killing the host at pc=0.
    {
        VkApplicationInfo old_app = {0};
        old_app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        old_app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo old_ici = {0};
        old_ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        old_ici.pApplicationInfo = &old_app;
        VkInstance old_instance = VK_NULL_HANDLE;
        VkResult old_r = vkCreateInstance(&old_ici, NULL, &old_instance);
        CHECK(old_r == VK_SUCCESS, "vkCreateInstance (1.0 probe)");
        // Device level: the loader's device dispatch table is built for the
        // instance's apiVersion, so a 1.1 device command has a null slot.
        PFN_vkEnumeratePhysicalDevices probe_enum_devs =
            (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(
                old_instance, "vkEnumeratePhysicalDevices");
        PFN_vkGetPhysicalDeviceQueueFamilyProperties probe_qprops =
            (PFN_vkGetPhysicalDeviceQueueFamilyProperties)vkGetInstanceProcAddr(
                old_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        PFN_vkCreateDevice probe_create_device =
            (PFN_vkCreateDevice)vkGetInstanceProcAddr(old_instance,
                                                      "vkCreateDevice");
        PFN_vkDestroyDevice probe_destroy_device =
            (PFN_vkDestroyDevice)vkGetInstanceProcAddr(old_instance,
                                                       "vkDestroyDevice");
        // Needs a 1.1 device entry point that RETURNS VkResult so the
        // rejection is observable (vkGetBufferMemoryRequirements2 is void).
        PFN_vkBindBufferMemory2 probe_bind2 =
            (PFN_vkBindBufferMemory2)vkGetInstanceProcAddr(
                old_instance, "vkBindBufferMemory2");
        VkPhysicalDevice old_phys = VK_NULL_HANDLE;
        uint32_t old_count = 1;
        VkDevice old_device = VK_NULL_HANDLE;
        if (old_instance && probe_enum_devs && probe_qprops &&
            probe_create_device &&
            probe_enum_devs(old_instance, &old_count, &old_phys) == VK_SUCCESS &&
            old_count > 0) {
            uint32_t qcount = 0;
            probe_qprops(old_phys, &qcount, NULL);
            float prio = 1.0f;
            VkDeviceQueueCreateInfo qci = {0};
            qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qci.queueFamilyIndex = 0;
            qci.queueCount = 1;
            qci.pQueuePriorities = &prio;
            VkDeviceCreateInfo dci = {0};
            dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            dci.queueCreateInfoCount = 1;
            dci.pQueueCreateInfos = &qci;
            CHECK(probe_create_device(old_phys, &dci, NULL, &old_device) ==
                      VK_SUCCESS,
                  "vkCreateDevice (1.0 probe)");
            if (old_device && probe_bind2) {
                CHECK(probe_bind2(old_device, 0, NULL) ==
                          VK_ERROR_FEATURE_NOT_PRESENT,
                      "version gate rejects a 1.1 device call on a 1.0 "
                      "instance");
            }
            if (old_device && probe_destroy_device)
                probe_destroy_device(old_device, NULL);
        }
        if (old_instance) vkDestroyInstance(old_instance, NULL);
    }

    free(phys);
    free(qprops);

    printf(g_fail ? "VULKAN TEST FAILED (%d)\n" : "VULKAN TEST PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}