// Guest Vulkan HAL module: the driver that Android's libvulkan loader opens
// from /vendor/lib64/hw/vulkan.<ro.hardware.vulkan>.so.
//
// The module owns no Vulkan state. Every entry point forwards to the host
// loader (libvulkan.so.1 on the host) through the emulator's existing
// DisplayThunk table: 0x100D resolves a host symbol to its guest trampoline,
// and the thunk marshals the call. That means the Vulkan object handles the
// loader holds are host handles wrapped in guest opaque values, exactly like
// the app-facing path in the vk tests.
//
// The two conversions the module does perform are the ones the host loader
// cannot know about:
//   * instance-extension filtering, because the loader advertises
//     VK_KHR_android_surface (implemented by the loader) and the host Linux
//     loader would reject it as unknown;
//   * the Android swapchain gralloc-usage queries, which are driver policy.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>
#include "vulkan_dispatchable.h"
#include "vulkan_native_buffer.h"

#define EXPORT __attribute__((visibility("default")))

// ── libhardware ABI (hw_module_t / hw_device_t / hwvulkan_device_t) ──────
// Only the layout matters: the loader reads these fields by offset, so they
// must match hardware/libhardware/include/hardware/hardware.h exactly.
enum {
    HARDWARE_MODULE_TAG = ('H' << 24) | ('W' << 16) | ('M' << 8) | 'T',
    HARDWARE_DEVICE_TAG = ('H' << 24) | ('W' << 16) | ('D' << 8) | 'T',
};

struct hw_module_t;
struct hw_device_t;

struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id,
                struct hw_device_t **device);
};

struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
};

struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t *device);
};

// Declared here because module_symbol() refers to them before definition.
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
        VkInstance instance, const char *name);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(
        VkInstance instance, const char *name);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
        VkInstance instance, const char *name);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
        VkDevice device, const char *name);

typedef struct hwvulkan_device_t {
    struct hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion;
} hwvulkan_device_t;

#define HWVULKAN_HARDWARE_MODULE_ID "vulkan"
#define HAL_MODULE_INFO_SYM HMI

// ── Host symbol resolution ───────────────────────────────────────────────
// DisplayThunk registers the host loader's exports under both libvulkan
// names; an unthunked name resolves to NULL and the loader treats the
// function as unsupported (never a made-up address).
__attribute__((visibility("hidden"))) void *bifrost_host_vk(const char *name) {
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)"libvulkan.so";
    register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)name;
    register uint64_t x8 __asm__("x8") = 0x100d;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return (void *)(uintptr_t)x0;
}

#define HOST_FN(type, name) \
    ((type)bifrost_host_vk(#name))
// ── Extension advertisement ──────────────────────────────────────────────
// An extension is advertised only when this module can answer for every
// command in it. The loader installs a NULL dispatch slot for a command the
// driver refuses (its extension entry point never got a body), so an app that
// enabled the extension calls through NULL — Unity does that for
// VK_KHR_fragment_shading_rate once the host stops exporting its commands.
int bf_vk_extension_servable(const char *extension_name) {
    if (!extension_name) return 0;
    for (const struct bf_vk_extension *extension = bf_vk_extensions;
         extension->name; ++extension) {
        if (strcmp(extension->name, extension_name)) continue;
        for (uint32_t i = 0; i < extension->command_count; ++i) {
            const char *host_name = extension->commands[i];
            if (!bf_vk_shim(extension->commands[i], &host_name)) return 0;
            if (!bifrost_host_vk(host_name)) return 0;
        }
        return 1;
    }
    return 1;  // not in the registry: leave the host's own answer alone
}

// ── Instance-extension filtering ─────────────────────────────────────────
// The Android loader implements VK_KHR_android_surface itself and adds it to
// the app's requested list; the host Linux loader only knows its own
// extensions and would fail the whole vkCreateInstance. Drop everything the
// host does not advertise, keeping the app's order.
enum { MAX_HOST_EXTENSIONS = 512, MAX_EXTENSION_NAME = 256 };
static char host_extensions[MAX_HOST_EXTENSIONS][MAX_EXTENSION_NAME];
static uint32_t host_extension_count;
static int host_extensions_loaded;

static void load_host_extensions(void) {
    if (host_extensions_loaded) return;
    host_extensions_loaded = 1;
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        HOST_FN(PFN_vkEnumerateInstanceExtensionProperties,
                vkEnumerateInstanceExtensionProperties);
    if (!enumerate) return;
    uint32_t count = 0;
    if (enumerate(NULL, &count, NULL) != VK_SUCCESS || count > MAX_HOST_EXTENSIONS)
        return;
    VkExtensionProperties *properties = (VkExtensionProperties *)__builtin_alloca(
        (size_t)(count ? count : 1) * sizeof(VkExtensionProperties));
    if (enumerate(NULL, &count, properties) != VK_SUCCESS) return;
    for (uint32_t i = 0; i < count; ++i) {
        size_t length = strnlen(properties[i].extensionName, MAX_EXTENSION_NAME - 1);
        memcpy(host_extensions[i], properties[i].extensionName, length);
        host_extensions[i][length] = 0;
    }
    host_extension_count = count;
}

static int host_has_extension(const char *name) {
    load_host_extensions();
    for (uint32_t i = 0; i < host_extension_count; ++i)
        if (!strcmp(host_extensions[i], name)) return 1;
    return 0;
}

// ── Instance creation ────────────────────────────────────────────────────
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
        const VkInstanceCreateInfo *create_info,
        const VkAllocationCallbacks *allocator, VkInstance *instance) {
    PFN_vkCreateInstance create =
        HOST_FN(PFN_vkCreateInstance, vkCreateInstance);
    if (!create || !instance) return VK_ERROR_INITIALIZATION_FAILED;
    // Stack copy: the caller's struct and its strings belong to the guest and
    // stay valid for the duration of the call.
    const char *names[64];
    VkInstanceCreateInfo filtered;
    const VkInstanceCreateInfo *info = create_info;
    if (create_info) {
        filtered = *create_info;
        uint32_t kept = 0;
        for (uint32_t i = 0;
             i < create_info->enabledExtensionCount && kept < 64; ++i) {
            const char *name = create_info->ppEnabledExtensionNames[i];
            if (name && host_has_extension(name)) names[kept++] = name;
        }
        filtered.enabledExtensionCount = kept;
        filtered.ppEnabledExtensionNames = kept ? names : NULL;
        if (create_info->enabledExtensionCount > kept &&
            create_info->ppEnabledExtensionNames) {
            // Record what was dropped so a missing extension is diagnosable
            // from guest stderr instead of a bare
            // VK_ERROR_EXTENSION_NOT_PRESENT.
            for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i) {
                const char *name = create_info->ppEnabledExtensionNames[i];
                if (!name || host_has_extension(name)) continue;
                char line[320];
                size_t length = strnlen(name, MAX_EXTENSION_NAME - 1);
                memcpy(line, "[vulkan-hal] host lacks instance extension ", 42);
                memcpy(line + 42, name, length);
                line[42 + length] = '\n';
                register uint64_t x0 __asm__("x0") = 2;
                register uint64_t x1 __asm__("x1") = (uint64_t)(uintptr_t)line;
                register uint64_t x2 __asm__("x2") = 43 + length;
                register uint64_t x8 __asm__("x8") = 64;
                __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2),
                                 "r"(x8) : "memory");
            }
        }
        info = &filtered;
    }
    // The loader owns the dispatchable object: it stores its dispatch table in
    // its first word, which only works for guest memory.
    VkInstance host = VK_NULL_HANDLE;
    VkResult result = create(info, allocator, &host);
    if (result != VK_SUCCESS) return result;
    void *object = bf_vk_wrap((uint64_t)(uintptr_t)host);
    if (!object) {
        PFN_vkDestroyInstance destroy =
            HOST_FN(PFN_vkDestroyInstance, vkDestroyInstance);
        if (destroy) destroy(host, allocator);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *instance = (VkInstance)object;
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
        const char *layer_name, uint32_t *count, VkExtensionProperties *properties) {
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        HOST_FN(PFN_vkEnumerateInstanceExtensionProperties,
                vkEnumerateInstanceExtensionProperties);
    if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;
    return enumerate(layer_name, count, properties);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *version) {
    PFN_vkEnumerateInstanceVersion enumerate =
        HOST_FN(PFN_vkEnumerateInstanceVersion, vkEnumerateInstanceVersion);
    if (!enumerate) {
        if (version) *version = VK_API_VERSION_1_0;
        return VK_SUCCESS;
    }
    return enumerate(version);
}

// Layers are the loader's business; the host has none of Android's and a
// bogus list would make the app enable a layer that does not exist.
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
        uint32_t *count, VkLayerProperties *properties) {
    (void)properties;
    if (count) *count = 0;
    return VK_SUCCESS;
}

// ── Android swapchain gralloc usage ──────────────────────────────────────
// GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER: the loader's swapchain
// hands the buffer to the compositor as a texture and renders into it.
enum { GRALLOC_USAGE_HW_TEXTURE = 0x00000100u, GRALLOC_USAGE_HW_RENDER = 0x00000200u };

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainGrallocUsageANDROID(
        VkDevice device, VkFormat format, VkImageUsageFlags image_usage,
        int *gralloc_usage) {
    (void)device; (void)format;
    if (!gralloc_usage) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t usage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER;
    if (image_usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        usage |= 0x2u;  // GRALLOC_USAGE_SW_READ_OFTEN, for the CPU readback path
    *gralloc_usage = (int)usage;
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainGrallocUsage2ANDROID(
        VkDevice device, VkFormat format, VkImageUsageFlags image_usage,
        VkFlags swapchain_usage, uint64_t *consumer_usage, uint64_t *producer_usage) {
    (void)swapchain_usage;
    int usage = 0;
    VkResult result = vkGetSwapchainGrallocUsageANDROID(device, format, image_usage,
                                                       &usage);
    if (result != VK_SUCCESS) return result;
    if (consumer_usage) *consumer_usage = (uint64_t)usage;
    if (producer_usage) *producer_usage = (uint64_t)usage;
    return VK_SUCCESS;
}

// The gralloc3/gralloc4 variants (Android 11+) are private to AOSP's loader:
// not in vk.xml, so no host symbol exists and no shim can answer them. The
// loader calls them with a VkGrallocUsageInfoANDROID (format/imageUsage) and a
// uint64_t* for the native usage; this bridge copies whole frames through a
// sealed memfd, so the answer is the same policy as the gralloc0/gralloc1 arms
// and the info struct is not interpreted.
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainGrallocUsage3ANDROID(
        VkDevice device, const void *gralloc_usage_info, uint64_t *native_usage) {
    (void)gralloc_usage_info;
    if (!native_usage) return VK_ERROR_INITIALIZATION_FAILED;
    uint64_t usage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER | 0x2u;
    (void)device;
    *native_usage = usage;
    return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainGrallocUsage4ANDROID(
        VkDevice device, const void *gralloc_usage_info, uint64_t *native_usage) {
    return vkGetSwapchainGrallocUsage3ANDROID(device, gralloc_usage_info,
                                              native_usage);
}

// ── ICD / HAL entry points ───────────────────────────────────────────────
// A name this driver does not provide becomes a NULL entry point for the
// loader, and an app that does not check the pointer calls NULL. Name each
// distinct refusal once: that is the only place the missing command is known.
enum { MAX_MISSING_NAMES = 64 };
static const char *missing_names[MAX_MISSING_NAMES];
static uint32_t missing_name_count;

static void note_missing(const char *name) {
    if (!name) return;
    for (uint32_t i = 0; i < missing_name_count; ++i)
        if (!strcmp(missing_names[i], name)) return;
    if (missing_name_count >= MAX_MISSING_NAMES) return;
    missing_names[missing_name_count++] = name;
    char line[256];
    int length = snprintf(line, sizeof line,
                          "[vulkan-hal] no entry point for %s\n", name);
    if (length > 0) {
        if (length > (int)sizeof line - 1) length = (int)sizeof line - 1;
        (void)!write(2, line, (size_t)length);
    }
}

// Every entry point this module serves, once per name. An app's function
// table is just addresses; these are the only names that can be mapped back.
enum { MAX_SERVED_NAMES = 1200 };
static const char *served_names[MAX_SERVED_NAMES];
static const void *served_addrs[MAX_SERVED_NAMES];
static uint32_t served_name_count;

static void note_served(const char *name, const void *addr) {
    if (!name || !addr) return;
    for (uint32_t i = 0; i < served_name_count; ++i)
        if (served_addrs[i] == addr || !strcmp(served_names[i], name)) return;
    if (served_name_count >= MAX_SERVED_NAMES) return;
    served_names[served_name_count] = name;
    served_addrs[served_name_count] = addr;
    ++served_name_count;
    char line[256];
    int length = snprintf(line, sizeof line, "[vulkan-hal] sym %s=%p\n", name, addr);
    if (length > 0) {
        if (length > (int)sizeof line - 1) length = (int)sizeof line - 1;
        (void)!write(2, line, (size_t)length);
    }
}

static PFN_vkVoidFunction module_symbol(const char *name) {    if (!name) return NULL;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)vkCreateInstance;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
        return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    if (!strcmp(name, "vkEnumerateInstanceVersion"))
        return (PFN_vkVoidFunction)vkEnumerateInstanceVersion;
    if (!strcmp(name, "vkEnumerateInstanceLayerProperties"))
        return (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties;
    if (!strcmp(name, "vkGetSwapchainGrallocUsageANDROID"))
        return (PFN_vkVoidFunction)vkGetSwapchainGrallocUsageANDROID;
    if (!strcmp(name, "vkGetSwapchainGrallocUsage2ANDROID"))
        return (PFN_vkVoidFunction)vkGetSwapchainGrallocUsage2ANDROID;
    if (!strcmp(name, "vkGetSwapchainGrallocUsage3ANDROID"))
        return (PFN_vkVoidFunction)vkGetSwapchainGrallocUsage3ANDROID;
    if (!strcmp(name, "vkGetSwapchainGrallocUsage4ANDROID"))
        return (PFN_vkVoidFunction)vkGetSwapchainGrallocUsage4ANDROID;
    if (!strcmp(name, "vk_icdGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    if (!strcmp(name, "vk_icdGetPhysicalDeviceProcAddr"))
        return (PFN_vkVoidFunction)vk_icdGetPhysicalDeviceProcAddr;
    // The loader asks the driver for these and installs the answer as the
    // app-facing function. They must resolve inside the guest: a shim would
    // forward to the host loader, which knows neither this module's handle
    // wrappers nor the Android-private commands (vkQueueSignalReleaseImageANDROID
    // came back NULL that way, and Unity tore the device down).
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    // VK_ANDROID_native_buffer plus the create/destroy wrappers the bridge
    // needs are not in vk.xml, so they are module symbols, not thunk rows.
    PFN_vkVoidFunction native = vulkan_native_buffer_symbol(name);
    if (native) return native;
    // Commands that create, destroy or carry dispatchable handles need more
    // than the generated shim's first-argument swap.
    return vulkan_handles_symbol(name);
}

// App-facing instance lookup: same answers as the ICD entry point, resolved in
// guest space (see the note in module_symbol).
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
        VkInstance instance, const char *name) {
    return vk_icdGetInstanceProcAddr(instance, name);
}

// App-facing device lookup: every command this module serves, then the
// generated shims. Device extension commands that are private to Android'
// native-buffer swapchain path only exist here.
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
        VkDevice device, const char *name) {
    (void)device;
    PFN_vkVoidFunction local = module_symbol(name);
    if (local) {
        note_served(name, (const void *)local);
        return local;
    }
    const char *host_name = name;
    void *shim = bf_vk_shim(name, &host_name);
    if (!shim || !bifrost_host_vk(host_name)) {
        note_missing(name);
        return NULL;
    }
    note_served(name, shim);
    return (PFN_vkVoidFunction)shim;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
        VkInstance instance, const char *name) {
    (void)instance;
    PFN_vkVoidFunction local = module_symbol(name);
    if (local) {
        note_served(name, (const void *)local);
        return local;
    }
    // Everything else is a host loader entry point (core or extension). The
    // generated shim replaces the dispatchable object this module handed the
    // loader with the host handle it wraps, then the DisplayThunk marshals the
    // call. NULL means "the driver does not implement it".
    const char *host_name = name;
    void *shim = bf_vk_shim(name, &host_name);
    if (!shim || !bifrost_host_vk(host_name)) {
        note_missing(name);
        return NULL;
    }
    note_served(name, shim);
    return (PFN_vkVoidFunction)shim;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(
        VkInstance instance, const char *name) {
    (void)instance;
    PFN_vkVoidFunction local = module_symbol(name);
    if (local) {
        note_served(name, (const void *)local);
        return local;
    }
    const char *host_name = name;
    void *shim = bf_vk_shim(name, &host_name);
    if (!shim || !bifrost_host_vk(host_name)) {
        note_missing(name);
        return NULL;
    }
    note_served(name, shim);
    return (PFN_vkVoidFunction)shim;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(
        uint32_t *supported_version) {
    if (!supported_version) return VK_ERROR_INCOMPATIBLE_DRIVER;
    const uint32_t loader = *supported_version;
    // v4 adds vk_icdGetPhysicalDeviceProcAddr, which this module provides. v5
    // would add vk_icdEnumerateAdapterPhysicalDevices, which it does not, so
    // advertise 4 and let the loader use the generic physical-device path.
    if (loader >= 4) *supported_version = 4;
    else if (loader >= 3) *supported_version = 3;
    else return VK_ERROR_INCOMPATIBLE_DRIVER;
    return VK_SUCCESS;
}

static hwvulkan_device_t module_device;

static int module_open(const struct hw_module_t *module, const char *id,
                       struct hw_device_t **device) {
    (void)id;
    if (!device) return -22 /* -EINVAL */;
    module_device.common.tag = HARDWARE_DEVICE_TAG;
    module_device.common.version = 0x00000100;  // HARDWARE_DEVICE_API_VERSION_2(0,1,0)
    module_device.common.module = (struct hw_module_t *)module;
    module_device.common.close = NULL;
    module_device.EnumerateInstanceExtensionProperties =
        vkEnumerateInstanceExtensionProperties;
    module_device.CreateInstance = vkCreateInstance;
    module_device.GetInstanceProcAddr = vk_icdGetInstanceProcAddr;
    module_device.EnumerateInstanceVersion = vkEnumerateInstanceVersion;
    *device = &module_device.common;
    return 0;
}

static struct hw_module_methods_t module_methods = { module_open };

EXPORT struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = 1,
    .hal_api_version = 0,
    .id = HWVULKAN_HARDWARE_MODULE_ID,
    .name = "bifrost Vulkan HAL",
    .author = "bifrost-emu",
    .methods = &module_methods,
    .dso = NULL,
};
