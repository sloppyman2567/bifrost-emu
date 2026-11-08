// VulkanAstc.cpp — see VulkanAstc.hpp.
#define VK_NO_PROTOTYPES
#include "../../../../../ThirdParty/VulkanHeaders/include/vulkan/vulkan_core.h"
#include "System/Graphics/Thunks/DisplayThunk/VulkanAstc.hpp"
#include "System/Graphics/Thunks/DisplayThunk/DisplayThunkInternal.hpp"
#include "System/Graphics/TextureCodec/AstcToBc7.hpp"
#include <algorithm>
#include <cstdio>
#include <thread>

namespace arm64emu {
namespace {

constexpr uint32_t kFirstAstc = VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
constexpr uint32_t kLastAstc = VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
struct Footprint { uint8_t w, h; };
// Formats come in UNORM/sRGB pairs in the order of the specification's footprint table.
constexpr Footprint kFootprints[14] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                       {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
bool is_astc(uint32_t f) { return f >= kFirstAstc && f <= kLastAstc; }
bool is_srgb(uint32_t f) { return ((f - kFirstAstc) & 1) != 0; }
Footprint footprint_of(uint32_t f) { return kFootprints[(f - kFirstAstc) / 2]; }
uint32_t bc7_for(uint32_t f) { return is_srgb(f) ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK; }

template <typename T> T handle_cast(uint64_t v) { return reinterpret_cast<T>(static_cast<uintptr_t>(v)); }
uint64_t to_u64(const void* h) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h)); }

void note_once(const char* what) {
    static std::mutex mu;
    static std::unordered_set<std::string> seen;
    std::lock_guard<std::mutex> g(mu);
    if (seen.insert(what).second) std::fprintf(stderr, "[vk-astc] %s\n", what);
}

constexpr size_t kBlockBytes = 32u << 20;   // scratch blocks are this big unless one upload needs more
constexpr size_t kAlign = 256;

}  // namespace

struct VulkanAstc::State {
    DisplayThunkImpl& impl;
    explicit State(DisplayThunkImpl& i) : impl(i) {}

    std::mutex mu;
    bool shutdown = false;  // the thunk is going away: the host device may be too, so touch nothing of Vulkan's

    // Host entry points, from the loader (trampolines that dispatch on their handle).
    struct Host {
        PFN_vkGetPhysicalDeviceFeatures features = nullptr;
        PFN_vkGetPhysicalDeviceFormatProperties format_properties = nullptr;
        PFN_vkGetPhysicalDeviceMemoryProperties memory_properties = nullptr;
        PFN_vkCreateBuffer create_buffer = nullptr;
        PFN_vkDestroyBuffer destroy_buffer = nullptr;
        PFN_vkGetBufferMemoryRequirements buffer_requirements = nullptr;
        PFN_vkAllocateMemory allocate = nullptr;
        PFN_vkFreeMemory free_memory = nullptr;
        PFN_vkBindBufferMemory bind = nullptr;
        PFN_vkMapMemory map = nullptr;
        PFN_vkUnmapMemory unmap = nullptr;
        bool ok = false;
    } host;
    bool host_resolved = false;

    struct DeviceInfo { uint64_t physical = 0; bool emulate = false; };
    std::unordered_map<uint64_t, bool> physical_emulates;
    std::unordered_map<uint64_t, DeviceInfo> devices;
    std::unordered_map<uint64_t, ImageInfo> images;
    struct Binding { uint64_t memory = 0, offset = 0; };
    std::unordered_map<uint64_t, Binding> buffers;

    // Scratch: BC7 data the guest's copy commands read. Blocks are host-visible, coherent and mapped for good.
    struct Block {
        uint64_t id = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* ptr = nullptr;
        size_t size = 0, used = 0;
        int live = 0;
    };
    std::unordered_map<uint64_t, std::vector<std::unique_ptr<Block>>> pools;  // by device
    uint64_t next_block_id = 1;

    struct Scratch {
        State* state;
        uint64_t device, block_id;
        uint8_t* base;       // the block's mapping
        uint64_t buffer;     // the block's VkBuffer
        size_t offset, size; // this allocation
        ~Scratch() { state->release_block(device, block_id); }
    };
    struct Pending {
        std::shared_ptr<Scratch> scratch;
        uint64_t src_buffer = 0, src_offset = 0;
        uint8_t block_w = 0, block_h = 0;
        bool srgb = false;
        uint32_t width = 0, height = 0, layers = 1;
        size_t src_row_pitch = 0, src_layer_pitch = 0, dst_offset = 0, dst_layer_bytes = 0;
    };
    std::unordered_map<uint64_t, std::vector<Pending>> pending;   // by command buffer
    std::unordered_map<uint64_t, uint64_t> cmd_pool;              // command buffer -> pool

    bool resolve_host() {
        if (host_resolved) return host.ok;
        host_resolved = true;
        void* lib = impl.vk_lib_;
        if (!lib) return false;
        auto sym = [&](const char* n) { return dlsym(lib, n); };
        host.features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(sym("vkGetPhysicalDeviceFeatures"));
        host.format_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(sym("vkGetPhysicalDeviceFormatProperties"));
        host.memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(sym("vkGetPhysicalDeviceMemoryProperties"));
        host.create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(sym("vkCreateBuffer"));
        host.destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(sym("vkDestroyBuffer"));
        host.buffer_requirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(sym("vkGetBufferMemoryRequirements"));
        host.allocate = reinterpret_cast<PFN_vkAllocateMemory>(sym("vkAllocateMemory"));
        host.free_memory = reinterpret_cast<PFN_vkFreeMemory>(sym("vkFreeMemory"));
        host.bind = reinterpret_cast<PFN_vkBindBufferMemory>(sym("vkBindBufferMemory"));
        host.map = reinterpret_cast<PFN_vkMapMemory>(sym("vkMapMemory"));
        host.unmap = reinterpret_cast<PFN_vkUnmapMemory>(sym("vkUnmapMemory"));
        host.ok = host.features && host.format_properties && host.memory_properties && host.create_buffer &&
                  host.destroy_buffer && host.buffer_requirements && host.allocate && host.free_memory && host.bind && host.map &&
                  host.unmap;
        return host.ok;
    }

    bool emulates(uint64_t physical) {
        if (dbg().vk_no_astc || !physical) return false;
        {
            std::lock_guard<std::mutex> g(mu);
            auto it = physical_emulates.find(physical);
            if (it != physical_emulates.end()) return it->second;
            if (!resolve_host()) return false;
        }
        VkPhysicalDevice pd = handle_cast<VkPhysicalDevice>(physical);
        VkPhysicalDeviceFeatures f{};
        host.features(pd, &f);
        bool yes = !f.textureCompressionASTC_LDR && f.textureCompressionBC;
        if (yes) {
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            for (VkFormat fmt : {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK}) {
                VkFormatProperties p{};
                host.format_properties(pd, fmt, &p);
                yes = yes && (p.optimalTilingFeatures & need) == need;
            }
        }
        if (yes) note_once("host has BC7 but no ASTC: ASTC LDR textures are transcoded to BC7");
        std::lock_guard<std::mutex> g(mu);
        physical_emulates[physical] = yes;
        return yes;
    }

    bool device_emulates(uint64_t device) {
        std::lock_guard<std::mutex> g(mu);
        auto it = devices.find(device);
        return it != devices.end() && it->second.emulate;
    }

    // ── scratch blocks ───────────────────────────────────────────────────────────────────────────────────────────
    void destroy_block_locked(uint64_t device, Block& b) {
        if (shutdown) return;
        if (b.buffer) host.destroy_buffer(handle_cast<VkDevice>(device), b.buffer, nullptr);
        if (b.memory) host.free_memory(handle_cast<VkDevice>(device), b.memory, nullptr);
    }
    Block* new_block_locked(uint64_t device, size_t size) {
        auto di = devices.find(device);
        if (di == devices.end() || !host.ok) return nullptr;
        VkDevice dev = handle_cast<VkDevice>(device);
        auto b = std::make_unique<Block>();
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (host.create_buffer(dev, &bi, nullptr, &b->buffer) != VK_SUCCESS) return nullptr;
        VkMemoryRequirements req{};
        host.buffer_requirements(dev, b->buffer, &req);
        VkPhysicalDeviceMemoryProperties props{};
        host.memory_properties(handle_cast<VkPhysicalDevice>(di->second.physical), &props);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        int type = -1;
        for (int pass = 0; pass < 2 && type < 0; pass++)   // plain system memory first, device-local as a fallback
            for (uint32_t i = 0; i < props.memoryTypeCount && type < 0; i++) {
                const VkMemoryPropertyFlags fl = props.memoryTypes[i].propertyFlags;
                if (!(req.memoryTypeBits & (1u << i)) || (fl & want) != want) continue;
                if (pass == 0 && (fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
                type = static_cast<int>(i);
            }
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = static_cast<uint32_t>(std::max(type, 0));
        void* mapped = nullptr;
        if (type < 0 || host.allocate(dev, &ai, nullptr, &b->memory) != VK_SUCCESS ||
            host.bind(dev, b->buffer, b->memory, 0) != VK_SUCCESS ||
            host.map(dev, b->memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            destroy_block_locked(device, *b);
            return nullptr;
        }
        b->ptr = static_cast<uint8_t*>(mapped);
        b->size = size;
        b->id = next_block_id++;
        auto& pool = pools[device];
        pool.push_back(std::move(b));
        return pool.back().get();
    }
    std::shared_ptr<Scratch> alloc_scratch(uint64_t device, size_t bytes) {
        bytes = (bytes + kAlign - 1) & ~(kAlign - 1);
        std::lock_guard<std::mutex> g(mu);
        Block* block = nullptr;
        auto& pool = pools[device];
        for (auto& b : pool)
            if (b->size - b->used >= bytes) { block = b.get(); break; }
        if (!block) block = new_block_locked(device, std::max(bytes, kBlockBytes));
        if (!block) return nullptr;
        auto s = std::shared_ptr<Scratch>(new Scratch{this, device, block->id, block->ptr, to_u64(block->buffer), block->used, bytes});
        block->used += bytes;
        block->live++;
        std::memset(s->base + s->offset, 0, bytes);  // an upload that never gets its data decodes as transparent black
        return s;
    }
    void release_block(uint64_t device, uint64_t id) {
        std::lock_guard<std::mutex> g(mu);
        auto pi = pools.find(device);
        if (pi == pools.end()) return;
        auto& pool = pi->second;
        for (size_t i = 0; i < pool.size(); i++) {
            if (pool[i]->id != id) continue;
            if (--pool[i]->live > 0) return;
            pool[i]->used = 0;
            if (pool.size() > 1 || pool[i]->size > kBlockBytes) {   // keep one ordinary block to avoid churn
                destroy_block_locked(device, *pool[i]);
                pool.erase(pool.begin() + i);
            }
            return;
        }
    }

    // ── source data ──────────────────────────────────────────────────────────────────────────────────────────────
    // Calls fn with the host address of `size` bytes at `offset` of the guest's buffer. They are read from whichever
    // mapping holds the memory; engines that map, copy and unmap their staging memory before submitting leave none,
    // so the memory is mapped here for the length of the call. False when the bytes cannot be reached.
    template <typename Fn>
    bool with_source(uint64_t buffer, uint64_t offset, uint64_t size, Fn fn) {
        Binding b;
        {
            std::lock_guard<std::mutex> g(mu);
            auto it = buffers.find(buffer);
            if (it == buffers.end()) return false;
            b = it->second;
        }
        const uint64_t at = b.offset + offset;
        const uint8_t* ptr = nullptr;
        bool guest_has_it_mapped = false;
        uint64_t device = 0;
        {
            std::lock_guard<std::mutex> g(impl.vk_maps_mu);
            auto it = impl.vk_maps_.find(b.memory);
            if (it != impl.vk_maps_.end()) {
                guest_has_it_mapped = true;
                const DisplayThunkImpl::VkMapped& m = it->second;
                if (m.placed) {
                    uint8_t* base = impl.mem ? impl.mem->guest_to_host_ptr(m.bounce) : nullptr;
                    if (base && at + size <= m.alloc_size) ptr = base + at;
                } else if (m.host_ptr && at >= m.map_offset && at + size <= m.map_offset + m.map_size) {
                    ptr = reinterpret_cast<const uint8_t*>(m.host_ptr) + (at - m.map_offset);
                }
            }
            auto ai = impl.vk_allocs_.find(b.memory);
            if (ai != impl.vk_allocs_.end()) device = ai->second.device;
        }
        if (ptr) { fn(ptr); return true; }
        if (guest_has_it_mapped || !device || !host.ok) return false;  // a second map of mapped memory is invalid
        void* mapped = nullptr;
        if (host.map(handle_cast<VkDevice>(device), handle_cast<VkDeviceMemory>(b.memory), at, size, 0, &mapped) != VK_SUCCESS)
            return false;
        fn(static_cast<const uint8_t*>(mapped));
        host.unmap(handle_cast<VkDevice>(device), handle_cast<VkDeviceMemory>(b.memory));
        return true;
    }

    unsigned threads() const {
        if (dbg().astc_threads) return dbg().astc_threads;
        return std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
    }

    void transcode(const Pending& p) {
        const size_t rows = (p.height + p.block_h - 1) / p.block_h;
        const size_t need = (p.layers - 1) * p.src_layer_pitch + rows * p.src_row_pitch;
        const bool ok = with_source(p.src_buffer, p.src_offset, need, [&](const uint8_t* src) {
            texcodec::Bc7Options options;
            for (uint32_t l = 0; l < p.layers; l++) {
                const texcodec::AstcRegion region{src + l * p.src_layer_pitch, p.src_row_pitch, p.block_w, p.block_h, p.srgb,
                                                  static_cast<int>(p.width), static_cast<int>(p.height)};
                texcodec::transcode_astc_to_bc7(region, options, threads(), p.scratch->base + p.dst_offset + l * p.dst_layer_bytes);
            }
        });
        if (!ok) note_once("an ASTC upload's staging memory cannot be read on the host: the texture stays blank");
    }

    void run_pending(const uint64_t* cmds, size_t n) {
        std::vector<Pending> work;
        {
            std::lock_guard<std::mutex> g(mu);
            if (pending.empty()) return;
            for (size_t i = 0; i < n; i++) {
                auto it = pending.find(cmds[i]);
                if (it != pending.end()) work.insert(work.end(), it->second.begin(), it->second.end());
            }
        }
        for (const Pending& p : work) transcode(p);
    }

    // Drops the pending transcodes of command buffers; scratch is returned when its last holder lets go.
    void drop_cmds(const std::vector<uint64_t>& cmds, bool forget_pool) {
        std::vector<std::vector<Pending>> doomed;
        {
            std::lock_guard<std::mutex> g(mu);
            for (uint64_t c : cmds) {
                auto it = pending.find(c);
                if (it != pending.end()) { doomed.push_back(std::move(it->second)); pending.erase(it); }
                if (forget_pool) cmd_pool.erase(c);
            }
        }
    }
    std::vector<uint64_t> cmds_of_pool(uint64_t pool) {
        std::vector<uint64_t> out;
        std::lock_guard<std::mutex> g(mu);
        for (const auto& kv : cmd_pool) if (kv.second == pool) out.push_back(kv.first);
        return out;
    }

    // ── buffer to image copies ───────────────────────────────────────────────────────────────────────────────────
    // Redirects the regions to a BC7 scratch buffer and records what to transcode into it at submit. Returns false
    // when no region can be honoured (the copy is then dropped).
    template <typename Region>
    bool redirect_copy(uint64_t cmd, uint64_t& src_buffer, uint64_t image, Region* regions, uint32_t& count) {
        ImageInfo img;
        {
            std::lock_guard<std::mutex> g(mu);
            auto it = images.find(image);
            if (it == images.end()) return true;  // not an emulated image: leave the copy alone
            img = it->second;
        }
        struct Plan { uint32_t index; size_t bytes; };
        std::vector<Plan> plan;
        size_t total = 0;
        uint32_t kept = 0;
        for (uint32_t i = 0; i < count; i++) {
            Region& r = regions[i];
            const uint32_t mip = r.imageSubresource.mipLevel;
            const uint32_t mw = std::max(1u, img.width >> mip), mh = std::max(1u, img.height >> mip);
            const bool aligned = r.imageOffset.x % 4 == 0 && r.imageOffset.y % 4 == 0 && r.imageOffset.x >= 0 &&
                                 r.imageOffset.y >= 0 && r.imageExtent.depth == 1 && r.imageExtent.width > 0 &&
                                 r.imageExtent.height > 0 &&
                                 (r.imageExtent.width % 4 == 0 || r.imageOffset.x + r.imageExtent.width == mw) &&
                                 (r.imageExtent.height % 4 == 0 || r.imageOffset.y + r.imageExtent.height == mh);
            if (!aligned) { note_once("an ASTC copy region is not 4-texel aligned: it is dropped"); continue; }
            const size_t layer_bytes = texcodec::bc7_image_bytes(r.imageExtent.width, r.imageExtent.height);
            const size_t bytes = layer_bytes * r.imageSubresource.layerCount;
            plan.push_back({i, (bytes + 15) & ~size_t(15)});
            total += plan.back().bytes;
            if (kept != i) regions[kept] = regions[i];
            kept++;
        }
        count = kept;
        if (!kept) return false;
        std::shared_ptr<Scratch> scratch = alloc_scratch(img.device, total);
        if (!scratch) { note_once("no scratch memory for an ASTC upload: the copy is dropped"); count = 0; return false; }
        std::vector<Pending> made;
        size_t running = scratch->offset;
        for (uint32_t k = 0; k < kept; k++) {
            Region& r = regions[k];
            Pending p;
            p.scratch = scratch;
            p.src_buffer = src_buffer;
            p.src_offset = r.bufferOffset;
            p.block_w = img.block_w; p.block_h = img.block_h; p.srgb = img.srgb;
            p.width = r.imageExtent.width; p.height = r.imageExtent.height;
            p.layers = r.imageSubresource.layerCount;
            const uint32_t row_len = r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width;
            const uint32_t img_h = r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height;
            p.src_row_pitch = static_cast<size_t>((row_len + img.block_w - 1) / img.block_w) * 16;
            p.src_layer_pitch = static_cast<size_t>((img_h + img.block_h - 1) / img.block_h) * p.src_row_pitch;
            p.dst_layer_bytes = texcodec::bc7_image_bytes(p.width, p.height);
            p.dst_offset = running;
            made.push_back(std::move(p));
            r.bufferOffset = running;
            r.bufferRowLength = 0;
            r.bufferImageHeight = 0;
            running += (made.back().dst_layer_bytes * p.layers + 15) & ~size_t(15);
        }
        src_buffer = scratch->buffer;
        std::lock_guard<std::mutex> g(mu);
        auto& list = pending[cmd];
        for (auto& p : made) list.push_back(std::move(p));
        return true;
    }
};

// ── the public face ──────────────────────────────────────────────────────────────────────────────────────────────

uint8_t VulkanAstc::hook_for(const std::string& full) {
    std::string name = full;
    if (name.size() > 3 && name.compare(name.size() - 3, 3, "KHR") == 0) name.resize(name.size() - 3);
    static const std::pair<const char*, Hook> kTable[] = {
        {"vkCreateDevice", kCreateDevice}, {"vkDestroyDevice", kDestroyDevice},
        {"vkGetPhysicalDeviceFeatures", kFeatures},
        {"vkGetPhysicalDeviceFormatProperties", kFormatProperties},
        {"vkGetPhysicalDeviceFormatProperties2", kFormatProperties2},
        {"vkGetPhysicalDeviceImageFormatProperties", kImageFormatProperties},
        {"vkGetPhysicalDeviceImageFormatProperties2", kImageFormatProperties2},
        {"vkCreateImage", kCreateImage}, {"vkDestroyImage", kDestroyImage}, {"vkCreateImageView", kCreateImageView},
        {"vkBindBufferMemory", kBindBufferMemory}, {"vkBindBufferMemory2", kBindBufferMemory2},
        {"vkDestroyBuffer", kDestroyBuffer},
        {"vkCmdCopyBufferToImage", kCopyBufferToImage}, {"vkCmdCopyBufferToImage2", kCopyBufferToImage2},
        {"vkQueueSubmit", kQueueSubmit}, {"vkQueueSubmit2", kQueueSubmit2},
        {"vkAllocateCommandBuffers", kAllocateCommandBuffers}, {"vkFreeCommandBuffers", kFreeCommandBuffers},
        {"vkResetCommandBuffer", kResetCommandBuffer}, {"vkResetCommandPool", kResetCommandPool},
        {"vkDestroyCommandPool", kDestroyCommandPool}, {"vkCmdExecuteCommands", kExecuteCommands},
    };
    for (const auto& e : kTable)
        if (name == e.first) return e.second;
    return kNone;
}

VulkanAstc::VulkanAstc(DisplayThunkImpl& impl) : s_(std::make_unique<State>(impl)) {}
VulkanAstc::~VulkanAstc() {
    if (!s_) return;
    std::lock_guard<std::mutex> g(s_->mu);
    s_->shutdown = true;
}

bool VulkanAstc::emulates(uint64_t physical) { return s_->emulates(physical); }

void VulkanAstc::begin_command_buffer(uint64_t cmd) { s_->drop_cmds({cmd}, false); }

void VulkanAstc::patch_features2(uint64_t physical, uint8_t* node) {
    int32_t type = 0;
    std::memcpy(&type, node, 4);
    if (type != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 || !emulates(physical)) return;
    reinterpret_cast<VkPhysicalDeviceFeatures2*>(node)->features.textureCompressionASTC_LDR = VK_TRUE;
}

bool VulkanAstc::before(uint8_t hook, uint64_t* args, Call& call) {
    State& s = *s_;
    switch (hook) {
    case kCreateDevice: {  // (physical, pCreateInfo, pAllocator, pDevice)
        auto* info = reinterpret_cast<VkDeviceCreateInfo*>(args[1]);
        if (!info || !s.emulates(args[0])) break;
        // The guest asks for ASTC; the host has BC. Enable the one it can and withhold the other.
        bool patched = false;
        for (auto* n = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(info->pNext)); n; n = n->pNext)
            if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                auto* f = reinterpret_cast<VkPhysicalDeviceFeatures2*>(n);
                f->features.textureCompressionASTC_LDR = VK_FALSE;
                f->features.textureCompressionBC = VK_TRUE;
                patched = true;
            }
        if (info->pEnabledFeatures) {
            auto* f = const_cast<VkPhysicalDeviceFeatures*>(info->pEnabledFeatures);
            f->textureCompressionASTC_LDR = VK_FALSE;
            f->textureCompressionBC = VK_TRUE;
            patched = true;
        }
        if (!patched) {  // the guest named no features: give the call a set of its own, valid until it returns
            static thread_local VkPhysicalDeviceFeatures own;
            own = VkPhysicalDeviceFeatures{};
            own.textureCompressionBC = VK_TRUE;
            info->pEnabledFeatures = &own;
        }
        break;
    }
    case kDestroyDevice: {  // (device, pAllocator): free scratch while the device still exists
        {
            std::lock_guard<std::mutex> g(s.mu);
            auto pi = s.pools.find(args[0]);
            if (pi != s.pools.end()) {
                for (auto& b : pi->second) s.destroy_block_locked(args[0], *b);
                s.pools.erase(pi);
            }
            s.devices.erase(args[0]);
        }
        break;
    }
    case kFormatProperties: case kFormatProperties2: case kImageFormatProperties:  // (physical, format, ...)
        if (is_astc(static_cast<uint32_t>(args[1])) && s.emulates(args[0])) args[1] = bc7_for(static_cast<uint32_t>(args[1]));
        break;
    case kImageFormatProperties2: {  // (physical, pImageFormatInfo, pProperties)
        auto* info = reinterpret_cast<VkPhysicalDeviceImageFormatInfo2*>(args[1]);
        if (info && is_astc(info->format) && s.emulates(args[0])) info->format = static_cast<VkFormat>(bc7_for(info->format));
        break;
    }
    case kCreateImage: {  // (device, pCreateInfo, pAllocator, pImage)
        auto* info = reinterpret_cast<VkImageCreateInfo*>(args[1]);
        if (!info || !is_astc(info->format) || !s.device_emulates(args[0])) break;
        if (info->imageType != VK_IMAGE_TYPE_2D) break;  // ASTC LDR is 2D only
        call.image.valid = true;
        call.image.device = args[0];
        call.image.srgb = is_srgb(info->format);
        const Footprint fp = footprint_of(info->format);
        call.image.block_w = fp.w; call.image.block_h = fp.h;
        call.image.width = info->extent.width; call.image.height = info->extent.height;
        call.image.mips = info->mipLevels; call.image.layers = info->arrayLayers;
        info->format = static_cast<VkFormat>(bc7_for(info->format));
        for (auto* n = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(info->pNext)); n; n = n->pNext)
            if (n->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
                auto* list = reinterpret_cast<VkImageFormatListCreateInfo*>(n);
                auto* formats = const_cast<VkFormat*>(list->pViewFormats);
                for (uint32_t i = 0; formats && i < list->viewFormatCount; i++)
                    if (is_astc(formats[i])) formats[i] = static_cast<VkFormat>(bc7_for(formats[i]));
            }
        break;
    }
    case kCreateImageView: {  // (device, pCreateInfo, pAllocator, pView)
        auto* info = reinterpret_cast<VkImageViewCreateInfo*>(args[1]);
        if (info && is_astc(info->format) && s.device_emulates(args[0])) info->format = static_cast<VkFormat>(bc7_for(info->format));
        break;
    }
    case kDestroyImage: {
        std::lock_guard<std::mutex> g(s.mu);
        s.images.erase(args[1]);
        break;
    }
    case kDestroyBuffer: {
        std::lock_guard<std::mutex> g(s.mu);
        s.buffers.erase(args[1]);
        break;
    }
    case kCopyBufferToImage: {  // (cmd, srcBuffer, dstImage, dstLayout, regionCount, pRegions)
        auto* regions = reinterpret_cast<VkBufferImageCopy*>(args[5]);
        uint32_t count = static_cast<uint32_t>(args[4]);
        if (!regions || !count) break;
        if (!s.redirect_copy(args[0], args[1], args[2], regions, count)) { call.result = 0; return false; }
        args[4] = count;
        break;
    }
    case kCopyBufferToImage2: {  // (cmd, pCopyBufferToImageInfo)
        auto* info = reinterpret_cast<VkCopyBufferToImageInfo2*>(args[1]);
        if (!info || !info->pRegions || !info->regionCount) break;
        uint64_t buffer = to_u64(info->srcBuffer);
        uint32_t count = info->regionCount;
        if (!s.redirect_copy(args[0], buffer, to_u64(info->dstImage),
                             const_cast<VkBufferImageCopy2*>(info->pRegions), count)) { call.result = 0; return false; }
        info->srcBuffer = handle_cast<VkBuffer>(buffer);
        info->regionCount = count;
        break;
    }
    case kQueueSubmit: {  // (queue, submitCount, pSubmits, fence)
        s.impl.vk_sync_push_all();  // the host mappings must hold the guest's final bytes
        auto* infos = reinterpret_cast<const VkSubmitInfo*>(args[2]);
        for (uint64_t i = 0; infos && i < args[1]; i++) {
            std::vector<uint64_t> cmds;
            for (uint32_t k = 0; k < infos[i].commandBufferCount; k++) cmds.push_back(to_u64(infos[i].pCommandBuffers[k]));
            s.run_pending(cmds.data(), cmds.size());
        }
        break;
    }
    case kQueueSubmit2: {  // (queue, submitCount, pSubmits, fence)
        s.impl.vk_sync_push_all();
        auto* infos = reinterpret_cast<const VkSubmitInfo2*>(args[2]);
        for (uint64_t i = 0; infos && i < args[1]; i++) {
            std::vector<uint64_t> cmds;
            for (uint32_t k = 0; k < infos[i].commandBufferInfoCount; k++)
                cmds.push_back(to_u64(infos[i].pCommandBufferInfos[k].commandBuffer));
            s.run_pending(cmds.data(), cmds.size());
        }
        break;
    }
    case kFreeCommandBuffers: {  // (device, pool, count, pCommandBuffers)
        const uint64_t* list = reinterpret_cast<const uint64_t*>(args[3]);
        std::vector<uint64_t> cmds(list, list ? list + std::min<uint64_t>(args[2], 1024) : list);
        s.drop_cmds(cmds, true);
        break;
    }
    case kResetCommandBuffer: s.drop_cmds({args[0]}, false); break;
    case kResetCommandPool: s.drop_cmds(s.cmds_of_pool(args[1]), false); break;
    case kDestroyCommandPool: s.drop_cmds(s.cmds_of_pool(args[1]), true); break;
    case kExecuteCommands: {  // (primary, count, pSecondaries): their copies run when the primary is submitted
        const uint64_t* list = reinterpret_cast<const uint64_t*>(args[2]);
        std::lock_guard<std::mutex> g(s.mu);
        if (s.pending.empty()) break;
        std::vector<State::Pending> adopted;
        for (uint64_t i = 0; list && i < std::min<uint64_t>(args[1], 1024); i++) {
            auto it = s.pending.find(list[i]);
            if (it != s.pending.end()) adopted.insert(adopted.end(), it->second.begin(), it->second.end());
        }
        auto& dst = s.pending[args[0]];
        dst.insert(dst.end(), adopted.begin(), adopted.end());
        break;
    }
    default: break;
    }
    return true;
}

void VulkanAstc::after(uint8_t hook, const uint64_t* args, uint64_t ret, Call& call) {
    State& s = *s_;
    const bool ok = static_cast<int32_t>(ret) == 0;
    switch (hook) {
    case kFeatures: {  // (physical, pFeatures): void; the staged copy now holds the host's answer
        auto* f = reinterpret_cast<VkPhysicalDeviceFeatures*>(args[1]);
        if (f && s.emulates(args[0])) f->textureCompressionASTC_LDR = VK_TRUE;
        break;
    }
    case kCreateDevice:
        if (ok && args[3]) {
            std::lock_guard<std::mutex> g(s.mu);
            const bool emulate = s.physical_emulates.count(args[0]) && s.physical_emulates[args[0]];
            s.devices[*reinterpret_cast<const uint64_t*>(args[3])] = {args[0], emulate};
        }
        break;
    case kCreateImage:
        if (ok && call.image.valid && args[3]) {
            std::lock_guard<std::mutex> g(s.mu);
            s.images[*reinterpret_cast<const uint64_t*>(args[3])] = call.image;
        }
        break;
    case kBindBufferMemory:  // (device, buffer, memory, offset)
        if (ok) { std::lock_guard<std::mutex> g(s.mu); s.buffers[args[1]] = {args[2], args[3]}; }
        break;
    case kBindBufferMemory2: {  // (device, count, pBindInfos)
        auto* infos = reinterpret_cast<const VkBindBufferMemoryInfo*>(args[2]);
        if (!ok || !infos) break;
        std::lock_guard<std::mutex> g(s.mu);
        for (uint64_t i = 0; i < std::min<uint64_t>(args[1], 1024); i++)
            s.buffers[to_u64(infos[i].buffer)] = {to_u64(infos[i].memory), infos[i].memoryOffset};
        break;
    }
    case kAllocateCommandBuffers: {  // (device, pAllocateInfo, pCommandBuffers)
        auto* info = reinterpret_cast<const VkCommandBufferAllocateInfo*>(args[1]);
        auto* out = reinterpret_cast<const uint64_t*>(args[2]);
        if (!ok || !info || !out) break;
        std::lock_guard<std::mutex> g(s.mu);
        for (uint32_t i = 0; i < std::min(info->commandBufferCount, 1024u); i++) s.cmd_pool[out[i]] = to_u64(info->commandPool);
        break;
    }
    default: break;
    }
}

}  // namespace arm64emu
