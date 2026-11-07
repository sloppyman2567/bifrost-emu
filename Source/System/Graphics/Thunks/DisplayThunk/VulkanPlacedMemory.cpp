// Placed Vulkan memory maps (VK_EXT_map_memory_placed).
//
// Without them vkMapMemory hands the guest a bounce: a guest copy of the host
// mapping that vk_sync_push_all/vk_sync_pull_all keep coherent by comparing
// and copying every mapped allocation around each submit and wait. With them
// the driver maps the allocation itself at a guest address inside the window,
// so the guest reads and writes the real memory and nothing is copied.
//
// Rules this file keeps:
//   * Only the whole allocation is mapped (memoryMapRangePlaced is not
//     required): the guest's offset is added to the base it gets back.
//   * Unmapping always uses VK_MEMORY_UNMAP_RESERVE_BIT_EXT, so the range is
//     never a hole another host mapping could take; Memory::restore_anonymous
//     then puts guest memory back and the range is released.
//   * Every placed map is unmapped before its memory is freed or its device
//     destroyed: the driver would otherwise munmap the range itself.
//   * Anything that does not qualify keeps the bounce path.
#include "System/Graphics/Thunks/DisplayThunk/DisplayThunkInternal.hpp"
namespace arm64emu {
namespace {
// Vulkan values (vulkan_core.h); the host side has no Vulkan headers.
constexpr int32_t kStypeFeatures2 = 1000059000;
constexpr int32_t kStypeProperties2 = 1000059001;
constexpr int32_t kStypeMemoryMapInfo = 1000271000;
constexpr int32_t kStypeMemoryUnmapInfo = 1000271001;
constexpr int32_t kStypePlacedFeatures = 1000272000;
constexpr int32_t kStypePlacedProperties = 1000272001;
constexpr int32_t kStypePlacedInfo = 1000272002;
constexpr uint32_t kMapPlacedBit = 0x1;
constexpr uint32_t kUnmapReserveBit = 0x1;
constexpr uint64_t kWholeSize = ~0ull;
constexpr char kPlacedExtension[] = "VK_EXT_map_memory_placed";
constexpr char kMap2Extension[] = "VK_KHR_map_memory2";

struct ChainNode { int32_t sType; const void* pNext; };
struct PlacedProperties { int32_t sType; void* pNext; uint64_t alignment; };
struct Features2 { int32_t sType; void* pNext; uint32_t features[55]; };
struct Properties2 { int32_t sType; void* pNext; alignas(8) uint8_t properties[1024]; };
struct MemoryMapInfo {
    int32_t sType; const void* pNext; uint32_t flags; uint64_t memory; uint64_t offset; uint64_t size;
};
struct MemoryUnmapInfo { int32_t sType; const void* pNext; uint32_t flags; uint64_t memory; };
struct PlacedInfo { int32_t sType; const void* pNext; void* address; };
// VkDeviceCreateInfo field offsets (LP64).
constexpr size_t kInfoPNext = 8, kInfoExtCount = 48, kInfoExtNames = 56;

using EnumerateExtensions = int32_t (*)(uint64_t, const char*, uint32_t*, void*);
using GetFeatures2 = void (*)(uint64_t, void*);
using GetProperties2 = void (*)(uint64_t, void*);
using GetDeviceProcAddr = void* (*)(uint64_t, const char*);
using MapMemory2 = int32_t (*)(uint64_t, const MemoryMapInfo*, void**);
using UnmapMemory2 = int32_t (*)(uint64_t, const MemoryUnmapInfo*);

// Each distinct reason, once per process: a map that silently keeps its
// bounce costs a full copy per fence wait, and nothing else says why.
void note_once(const char* reason) {
    static std::mutex mu;
    static std::vector<const char*> seen;
    std::lock_guard<std::mutex> g(mu);
    for (const char* r : seen)
        if (r == reason) return;
    seen.push_back(reason);
    fprintf(stderr, "[vk-placed] %s\n", reason);
}

bool has_extension(void* lib, uint64_t physical, const char* name) {
    auto enumerate = reinterpret_cast<EnumerateExtensions>(dlsym(lib, "vkEnumerateDeviceExtensionProperties"));
    if (!enumerate) return false;
    uint32_t count = 0;
    if (enumerate(physical, nullptr, &count, nullptr) != 0 || !count || count > 4096) return false;
    constexpr size_t kPropertySize = 260;  // char[256] name + uint32 specVersion
    std::vector<uint8_t> list(static_cast<size_t>(count) * kPropertySize);
    if (enumerate(physical, nullptr, &count, list.data()) < 0) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!std::strncmp(reinterpret_cast<const char*>(list.data() + i * kPropertySize), name, 256))
            return true;
    return false;
}
} // namespace

bool DisplayThunkImpl::vk_placed_prepare(uint64_t physical, uint64_t& info_arg,
                                         VkPlacedDeviceRequest& req) {
    if (dbg().vk_no_placed || !vk_lib_ || !physical || !info_arg) return false;
    if (!has_extension(vk_lib_, physical, kPlacedExtension) ||
        !has_extension(vk_lib_, physical, kMap2Extension)) {
        note_once("device lacks VK_EXT_map_memory_placed or VK_KHR_map_memory2: maps keep bounces");
        return false;
    }
    auto features2 = reinterpret_cast<GetFeatures2>(dlsym(vk_lib_, "vkGetPhysicalDeviceFeatures2"));
    auto properties2 = reinterpret_cast<GetProperties2>(dlsym(vk_lib_, "vkGetPhysicalDeviceProperties2"));
    if (!features2 || !properties2) {
        note_once("host loader lacks vkGetPhysicalDeviceFeatures2/Properties2: maps keep bounces");
        return false;
    }
    VkPlacedDeviceRequest::Features supported{kStypePlacedFeatures, nullptr, 0, 0, 0};
    Features2 f2{};
    f2.sType = kStypeFeatures2;
    f2.pNext = &supported;
    features2(physical, &f2);
    // Reserve-on-unmap is what keeps an unmapped range out of other hands.
    if (!supported.placed || !supported.reserve) {
        note_once("device lacks memoryMapPlaced or memoryUnmapReserve: maps keep bounces");
        return false;
    }
    PlacedProperties placed{kStypePlacedProperties, nullptr, 0};
    Properties2 p2{};
    p2.sType = kStypeProperties2;
    p2.pNext = &placed;
    properties2(physical, &p2);
    // Guest ranges are page aligned; a larger alignment keeps the bounce.
    const uint64_t alignment = placed.alignment;
    if (!alignment || alignment > Memory::PAGE_SIZE || (alignment & (alignment - 1))) {
        note_once("placed-map alignment is not a power of two up to a page: maps keep bounces");
        return false;
    }

    const auto* info = reinterpret_cast<const uint8_t*>(info_arg);
    const void* chain;
    std::memcpy(&chain, info + kInfoPNext, sizeof(chain));
    for (auto* node = static_cast<const ChainNode*>(chain); node;
         node = static_cast<const ChainNode*>(node->pNext))
        if (node->sType == kStypePlacedFeatures) {  // never a second copy
            note_once("app chained its own placed-map features: device left as asked");
            return false;
        }
    uint32_t count;
    const char* const* names;
    std::memcpy(&count, info + kInfoExtCount, sizeof(count));
    std::memcpy(&names, info + kInfoExtNames, sizeof(names));
    bool have_placed = false, have_map2 = false;
    req.extensions.clear();
    for (uint32_t i = 0; i < count && names; ++i) {
        if (!names[i]) continue;
        have_placed |= !std::strcmp(names[i], kPlacedExtension);
        have_map2 |= !std::strcmp(names[i], kMap2Extension);
        req.extensions.push_back(names[i]);
    }
    if (!have_placed) req.extensions.push_back(kPlacedExtension);
    if (!have_map2) req.extensions.push_back(kMap2Extension);
    std::memcpy(req.info, info, sizeof(req.info));
    req.features = {kStypePlacedFeatures, chain, 1, 0, 1};
    const void* head = &req.features;
    const char* const* list = req.extensions.data();
    const uint32_t list_count = static_cast<uint32_t>(req.extensions.size());
    std::memcpy(req.info + kInfoPNext, &head, sizeof(head));
    std::memcpy(req.info + kInfoExtCount, &list_count, sizeof(list_count));
    std::memcpy(req.info + kInfoExtNames, &list, sizeof(list));
    req.alignment = alignment;
    req.active = true;
    info_arg = reinterpret_cast<uint64_t>(req.info);
    return true;
}

void DisplayThunkImpl::vk_placed_note(const char* reason) { note_once(reason); }

void DisplayThunkImpl::vk_placed_register(uint64_t device, uint64_t alignment) {
    auto proc = reinterpret_cast<GetDeviceProcAddr>(vk_lib_ ? dlsym(vk_lib_, "vkGetDeviceProcAddr") : nullptr);
    if (!proc || !device) return;
    void* map2 = proc(device, "vkMapMemory2KHR");
    void* unmap2 = proc(device, "vkUnmapMemory2KHR");
    if (!map2 || !unmap2) {
        note_once("vkMapMemory2KHR/vkUnmapMemory2KHR missing on the device: maps keep bounces");
        return;
    }
    std::lock_guard<std::mutex> g(vk_maps_mu);
    vk_placed_devices_[device] = {map2, unmap2, alignment};
}

uint64_t DisplayThunkImpl::vk_placed_map(uint64_t device, uint64_t memory, uint64_t alloc_size) {
    VkPlacedDevice placed;
    {
        std::lock_guard<std::mutex> g(vk_maps_mu);
        auto it = vk_placed_devices_.find(device);
        if (it == vk_placed_devices_.end()) {
            note_once("memory of a device without placed maps: bounce");
            return 0;
        }
        placed = it->second;
    }
    // A whole-allocation placed map needs an aligned allocation size, and the
    // driver maps exactly that many bytes at the guest range.
    if (!mem || !alloc_size || alloc_size % Memory::PAGE_SIZE || alloc_size % placed.alignment) {
        note_once("allocation size not a multiple of the page/placement alignment: bounce");
        return 0;
    }
    const uint64_t base = mem->mmap_alloc(alloc_size, 0, false,
                                          Memory::GUEST_PROT_READ | Memory::GUEST_PROT_WRITE);
    if (!base || base == ~0ull) {
        note_once("no guest range for a placed map: bounce");
        return 0;
    }
    uint8_t* window = mem->guest_to_host_ptr(base);
    if (!window || base % placed.alignment) {
        mem->untrack_allocation(base, alloc_size);
        return 0;
    }
    PlacedInfo where{kStypePlacedInfo, nullptr, window};
    MemoryMapInfo info{kStypeMemoryMapInfo, &where, kMapPlacedBit, memory, 0, kWholeSize};
    void* host = nullptr;
    const int32_t result = reinterpret_cast<MapMemory2>(placed.map2)(device, &info, &host);
    if (result == 0 && host == window) return base;
    note_once(result ? "vkMapMemory2KHR refused the placed map: bounce"
                     : "driver placed the map elsewhere: bounce");
    // A failed MAP_FIXED may already have replaced the range: always restore.
    if (result == 0) {
        MemoryUnmapInfo unmap{kStypeMemoryUnmapInfo, nullptr, kUnmapReserveBit, memory};
        reinterpret_cast<UnmapMemory2>(placed.unmap2)(device, &unmap);
    }
    mem->restore_anonymous(base, alloc_size);
    mem->untrack_allocation(base, alloc_size);
    return 0;
}

void DisplayThunkImpl::vk_placed_unmap(uint64_t device, uint64_t memory, uint64_t base,
                                       uint64_t size) {
    void* unmap2 = nullptr;
    {
        std::lock_guard<std::mutex> g(vk_maps_mu);
        auto it = vk_placed_devices_.find(device);
        if (it != vk_placed_devices_.end()) unmap2 = it->second.unmap2;
    }
    if (unmap2) {
        MemoryUnmapInfo unmap{kStypeMemoryUnmapInfo, nullptr, kUnmapReserveBit, memory};
        reinterpret_cast<UnmapMemory2>(unmap2)(device, &unmap);
    }
    if (!mem) return;
    mem->restore_anonymous(base, size);
    mem->untrack_allocation(base, size);
}

void DisplayThunkImpl::vk_placed_release_device(uint64_t device) {
    struct Release { uint64_t memory, base, size; };
    std::vector<Release> release;
    {
        std::lock_guard<std::mutex> g(vk_maps_mu);
        if (!vk_placed_devices_.count(device)) return;
        for (auto it = vk_maps_.begin(); it != vk_maps_.end();) {
            if (it->second.placed && it->second.device == device) {
                release.push_back({it->first, it->second.bounce, it->second.map_size});
                it = vk_maps_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const Release& r : release) vk_placed_unmap(device, r.memory, r.base, r.size);
    std::lock_guard<std::mutex> g(vk_maps_mu);
    vk_placed_devices_.erase(device);
}
} // namespace arm64emu
