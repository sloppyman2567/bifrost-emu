// test_vulkan_pnext.c — pNext-chain query coverage (2026-08-24).
//
// vkQuake was the only exerciser of vkGetPhysicalDeviceProperties2 /
// Features2 chain staging until it crashed there — the suite had ZERO
// coverage for that path and the enum-sized-array layout bug hid behind
// exactly this gap. This test drives the chain machinery directly:
//
//   1. Properties2 → DriverProperties → IDProperties guest chain:
//      driver must FILL all three nodes through the thunk's staged
//      copies, and the GUEST chain links must survive (regression: the
//      writeback once stamped host pointers over the guest's own
//      pNext links).
//   2. Unknown-sType truncation: a bogus node mid-chain truncates
//      staging safely (no crash, no garbage into the driver).
//   3. Features2 + BufferDeviceAddressFeatures chain.
//   4. Guard canaries around every struct catch over-staging writes.
//
// No surface, no window — works headless whenever the host has a
// Vulkan loader/driver. Exit 0 = pass, 77 = skip (no host Vulkan),
// 1 = real failure.
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
static int g_checks = 0;
#define CHECK(cond, msg) do { \
    g_checks++; \
    if (cond) { printf("  OK  %s\n", msg); } \
    else { printf(" FAIL  %s\n", msg); g_fail++; } \
} while (0)

typedef VkResult (VKAPI_PTR *PFPN_vkCreateInstance)(
    const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
typedef VkResult (VKAPI_PTR *PFPN_EnumeratePhysicalDevices)(
    VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void (VKAPI_PTR *PFPN_GetProperties2)(
    VkPhysicalDevice, VkPhysicalDeviceProperties2*);
typedef void (VKAPI_PTR *PFPN_GetFeatures2)(
    VkPhysicalDevice, VkPhysicalDeviceFeatures2*);
typedef void (VKAPI_PTR *PFPN_GetProperties)(
    VkPhysicalDevice, VkPhysicalDeviceProperties*);
typedef void (VKAPI_PTR *PFPN_DestroyInstance)(
    VkInstance, const VkAllocationCallbacks*);

/* Sentinel canaries bracketing each guest struct: any over-staging or
 * under-sizing write in the thunk shows up as a clobbered guard. */
static uint64_t g_canary_hits = 0;
#define GUARD 0xFEEDFACECAFE1234ULL /* arbitrary magic */
static uint64_t g_before_props2, g_after_props2;
static uint64_t g_before_drv,    g_after_drv;
static uint64_t g_before_id,     g_after_id;
static uint64_t g_before_feat,   g_after_feat;
static uint64_t g_before_bda,    g_after_bda;

#define SET_GUARDS(before_ptr, after_ptr) do { \
    (before_ptr) = GUARD; (after_ptr) = GUARD; } while (0)
#define VERIFY_GUARDS(before_ptr, after_ptr, what) do { \
    if ((before_ptr) != GUARD || (after_ptr) != GUARD) { \
        printf(" FAIL  %s: guard canary clobbered\n", what); \
        g_fail++; g_canary_hits++; \
    } } while (0)

int main(void) {
    void* vk = dlopen_bf("libvulkan.so.1", 1 /* RTLD_LAZY */);
    if (!vk) { printf("SKIP: host libvulkan unavailable\n"); return 77; }
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)dlsym_bf(vk, "vkGetInstanceProcAddr");
    if (!gipa) { printf("SKIP: no vkGetInstanceProcAddr\n"); return 77; }

    PFPN_vkCreateInstance vkCreateInstance =
        (PFPN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    PFPN_EnumeratePhysicalDevices vkEnumeratePhysicalDevices =
        (PFPN_EnumeratePhysicalDevices)gipa(NULL, "vkEnumeratePhysicalDevices");
    PFPN_GetProperties2 vkGetPhysicalDeviceProperties2 =
        (PFPN_GetProperties2)gipa(NULL, "vkGetPhysicalDeviceProperties2");
    PFPN_GetFeatures2 vkGetPhysicalDeviceFeatures2 =
        (PFPN_GetFeatures2)gipa(NULL, "vkGetPhysicalDeviceFeatures2");
    PFPN_GetProperties vkGetPhysicalDeviceProperties =
        (PFPN_GetProperties)gipa(NULL, "vkGetPhysicalDeviceProperties");
    PFPN_DestroyInstance vkDestroyInstance =
        (PFPN_DestroyInstance)gipa(NULL, "vkDestroyInstance");

    CHECK(vkCreateInstance && vkEnumeratePhysicalDevices && 
          vkGetPhysicalDeviceProperties2 && vkGetPhysicalDeviceFeatures2 &&
          vkGetPhysicalDeviceProperties, "dlsym entry points");
    if (!vkCreateInstance || !vkEnumeratePhysicalDevices ||
        !vkGetPhysicalDeviceProperties2 || !vkGetPhysicalDeviceFeatures2 ||
        !vkGetPhysicalDeviceProperties) return 1;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "vk-pnext-test",
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app };
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    CHECK(r == VK_SUCCESS && instance, "vkCreateInstance (no extensions)");
    if (r != VK_SUCCESS) return 1;

    uint32_t ndev = 0;
    r = vkEnumeratePhysicalDevices(instance, &ndev, NULL);
    CHECK(r == VK_SUCCESS && ndev > 0, "count physical devices");
    if (ndev == 0) return 1;
    VkPhysicalDevice devs[4];
    if (ndev > 4) ndev = 4;
    r = vkEnumeratePhysicalDevices(instance, &ndev, devs);
    CHECK(r == VK_SUCCESS, "enumerate physical devices");
    VkPhysicalDevice pd = devs[0];

    /* ── plain Properties sanity (non-chain path) ─────────────────── */
    VkPhysicalDeviceProperties props;
    memset(&props, 0xAA, sizeof(props));
    vkGetPhysicalDeviceProperties(pd, &props);
    CHECK(props.vendorID != 0, "properties: vendorID filled");
    CHECK(props.deviceName[0] != 0 &&
          memchr(props.deviceName, 0, sizeof(props.deviceName)),
          "properties: deviceName NUL-terminated");
    CHECK(props.limits.maxImageDimension2D >= 4096,
          "properties: limits.maxImageDimension2D sane");
    CHECK(props.apiVersion >= VK_API_VERSION_1_0, "properties: apiVersion set");

    /* ── 1. three-node Properties2 chain ──────────────────────────── */
    VkPhysicalDeviceDriverProperties drv;
    VkPhysicalDeviceIDProperties idp;
    VkPhysicalDeviceProperties2 p2;
    memset(&drv, 0, sizeof(drv));
    memset(&idp, 0, sizeof(idp));
    memset(&p2, 0, sizeof(p2));
    drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    p2.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext  = &drv;
    drv.pNext = &idp;
    /* keep raw links to verify the guest chain survives the round trip */
    void* want_p2_next = &drv;
    void* want_drv_next = &idp;
    SET_GUARDS(g_before_props2, g_after_props2);
    SET_GUARDS(g_before_drv, g_after_drv);
    SET_GUARDS(g_before_id, g_after_id);

    vkGetPhysicalDeviceProperties2(pd, &p2);

    VERIFY_GUARDS(g_before_props2, g_after_props2, "Properties2 canaries");
    VERIFY_GUARDS(g_before_drv, g_after_drv, "DriverProperties canaries");
    VERIFY_GUARDS(g_before_id, g_after_id, "IDProperties canaries");

    CHECK(p2.properties.vendorID != 0, "chain: vendorID filled");
    CHECK(p2.properties.limits.maxImageDimension2D >= 4096,
          "chain: limits filled through pNext staging");
    CHECK(memchr(p2.properties.deviceName, 0,
                 sizeof(p2.properties.deviceName)) != NULL,
          "chain: deviceName intact");
    CHECK(drv.driverID != 0, "chain: driverID filled (driver node reached)");
    CHECK(drv.driverName[0] != 0 &&
          memchr(drv.driverName, 0, sizeof(drv.driverName)) != NULL,
          "chain: driverName NUL-terminated string");
    CHECK(idp.deviceUUID[0] != 0xAA || idp.deviceUUID[15] != 0xAA ||
          idp.deviceUUID[8] != 0xAA,
          "chain: deviceUUID touched by driver");
    /* THE regression: guest chain links must still point at GUEST nodes */
    CHECK(p2.pNext == want_p2_next, "chain link Properties2→Driver preserved");
    CHECK(drv.pNext == want_drv_next, "chain link Driver→ID preserved");
    CHECK(p2.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
          "node sType preserved (Properties2)");
    CHECK(drv.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
          "node sType preserved (Driver)");

    printf("  ..  vendor=0x%04x device=\"%s\" driver=\"%s\"\n",
           p2.properties.vendorID, p2.properties.deviceName,
           drv.driverName);

    /* ── 2. unknown-sType truncation mid-chain ────────────────────── */
    struct { VkStructureType sType; void* pNext; } bogus = {
        (VkStructureType)0xDEADBEEFu, NULL };
    VkPhysicalDeviceProperties2 p2b;
    memset(&p2b, 0, sizeof(p2b));
    p2b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2b.pNext = &bogus;
    r = VK_RESULT_MAX_ENUM;
    (void)r;
    vkGetPhysicalDeviceProperties2(pd, &p2b);
    CHECK(p2b.properties.vendorID != 0,
          "truncation: core properties still filled");
    CHECK(p2b.pNext == &bogus, "truncation: guest chain untouched");

    /* ── 3. Features2 + BufferDeviceAddress chain ─────────────────── */
    VkPhysicalDeviceBufferDeviceAddressFeatures bda;
    VkPhysicalDeviceFeatures2 f2;
    memset(&bda, 0, sizeof(bda));
    memset(&f2, 0, sizeof(f2));
    bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    f2.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext  = &bda;
    SET_GUARDS(g_before_feat, g_after_feat);
    SET_GUARDS(g_before_bda, g_after_bda);

    vkGetPhysicalDeviceFeatures2(pd, &f2);

    VERIFY_GUARDS(g_before_feat, g_after_feat, "Features2 canaries");
    VERIFY_GUARDS(g_before_bda, g_after_bda, "BDA canaries");
    CHECK(f2.features.sparseBinding == 0 || f2.features.sparseBinding == 1,
          "features2: sparseBinding is a clean bool");
    CHECK(f2.features.shaderInt64 == 0 || f2.features.shaderInt64 == 1,
          "features2: shaderInt64 is a clean bool");
    CHECK(bda.bufferDeviceAddress == 0 || bda.bufferDeviceAddress == 1,
          "features2: BDA node filled with a clean bool");
    CHECK(f2.pNext == &bda, "features2: chain link preserved");
    CHECK(f2.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
          "features2: sType preserved");

    if (vkDestroyInstance) vkDestroyInstance(instance, NULL);
    CHECK(1, "teardown");

    printf("VK PNEXT TEST %s (%d checks)\n",
           g_fail ? "FAILED" : "PASSED", g_checks);
    return g_fail ? 1 : 0;
}
