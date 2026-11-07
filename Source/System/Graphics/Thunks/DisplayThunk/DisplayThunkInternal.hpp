// DisplayThunkInternal.hpp — DisplayThunk private state and Vulkan staging
// helpers shared by the files in Source/System/Graphics/Thunks/DisplayThunk/.
#pragma once
#include "System/Graphics/Display/WindowStats.hpp"
#include "System/Graphics/Thunks/DisplayThunk.hpp"
#include "System/Graphics/Thunks/VulkanMappedSync.hpp"
#include "System/Graphics/Thunks/GraphicThunk.hpp"  // for SYSCALL_NUMBER
#include "System/Graphics/Display/DisplayProxy.hpp"
#include "System/Android/Surface/Surface.hpp"
#include "System/Graphics/Thunks/ThunkCommon.hpp" // shared SymbolEntry (single definition — see header)
#include "Generated/ThunkTable.hpp"  // symbol signature table (single source of truth)
#include "Generated/WaylandTable.hpp"     // wayland protocol message signatures (wlgen)
#include "Generated/VulkanMarshal.hpp" // VK_CMD_DEEP: generated deep-marshal descriptors
#include "System/Core/DebugFlags.hpp"    // dbg() — cached trace gates (BIFROST_THUNK_TRACE)
#include "System/Core/CPU.hpp"
#include "System/Memory/Memory.hpp"
#include <climits>
#include <limits>
#include <dlfcn.h>
#include <unistd.h>
#include <csignal>
#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace arm64emu {
struct DisplayThunkImpl {
    bool   enabled = false;
    Memory* mem    = nullptr;
    bool   initialized = false;
    uint64_t trampoline_base = 0;
    static constexpr uint64_t TRAMPOLINE_PAGE_SIZE =
        DisplayThunk::TRAMPOLINE_SIZE * DisplayThunk::MAX_SYMBOLS;  // 32 KiB
    struct LibTable {
        std::string lib;
        std::vector<SymbolEntry> entries;
    };
    std::vector<LibTable> libs_;
    std::vector<std::pair<uint32_t, uint32_t>> id_to_idx_;
    // DisplayProxy for X11/Wayland fallback.
    std::unique_ptr<DisplayProxy> proxy_;
    // Wayland guest listeners (wl_proxy_add_listener): proxy handle ->
    // {guest fn addrs per event, data}. Delivered after dispatch via
    // the borrow-CPU runner (queue-then-deliver, never re-entrant).
    struct WlListener {
        std::vector<uint64_t> fns;
        uint64_t data = 0;
    };
    std::unordered_map<uint64_t, WlListener> wl_listeners_;
    DisplayThunk::WlCbRunner wl_cb_runner_;
    DisplayThunk::WlFdPublisher wl_fd_publisher_;
    // Guest-visible scratch page for host→guest string returns
    // (XGetAtomName, glGetString, …). Ring-allocated.
    uint64_t string_cache_base = 0;
    static constexpr uint64_t STRING_CACHE_SIZE = 4096;
    uint32_t string_cache_off = 0;
    std::mutex mu;
    // ── vkMapMemory guest-window bounce bookkeeping (2026-08-21) ──────
    // The host mapping address is meaningless in the guest (a host heap
    // address, outside the guest reservation), so vkMapMemory allocates a
    // bounce in the guest and returns ITS guest address; the guest
    // reads/writes it at full JIT speed. Publish changed guest bytes before
    // GPU-consuming calls, and merge readback after completion waits without
    // erasing unsubmitted guest writes. Explicit flush/invalidate refresh
    // the synchronization baseline for their specified ranges.
    struct VkMapped {
        uint64_t host_ptr;    // host mapping base (a HOST address)
        uint64_t bounce;      // guest address of the window bounce
        uint64_t map_offset;  // offset passed to vkMapMemory
        uint64_t map_size;    // bytes mapped (resolved from VK_WHOLE_SIZE)
        uint64_t alloc_size;  // total VkDeviceMemory allocation size
        std::vector<uint8_t> baseline; // last synchronized host/guest bytes
        // Placed (VK_EXT_map_memory_placed): the driver mapped the whole
        // allocation at `bounce` inside the guest window, so the guest uses
        // the real memory. No baseline and no push/pull; map_offset is 0 and
        // map_size the allocation size, whatever range the guest asked for.
        bool placed = false;
        uint64_t device = 0;  // host VkDevice (placed unmaps need it)
    };
    struct VkAlloc { uint64_t size; uint64_t device; };
    std::unordered_map<uint64_t, VkMapped> vk_maps_;    // memory handle → map
    std::unordered_map<uint64_t, VkAlloc> vk_allocs_;   // handle → allocation
    std::mutex vk_maps_mu;
    // ── Placed memory maps (VulkanPlacedMemory.cpp) ──────────────────
    // Devices created with VK_EXT_map_memory_placed + memoryUnmapReserve,
    // with their vkMapMemory2KHR/vkUnmapMemory2KHR. Guarded by vk_maps_mu.
    struct VkPlacedDevice { void* map2; void* unmap2; uint64_t alignment; };
    std::unordered_map<uint64_t, VkPlacedDevice> vk_placed_devices_;
    void* vk_lib_ = nullptr;  // host libvulkan handle (resolved at registration)
    // Storage for a vkCreateDevice create info extended with the placed
    // extension; it must outlive the host call.
    struct VkPlacedDeviceRequest {
        struct Features { int32_t sType; const void* pNext; uint32_t placed, range, reserve; } features{};
        uint8_t info[72] = {};  // VkDeviceCreateInfo (LP64)
        std::vector<const char*> extensions;
        uint64_t alignment = 0;
        bool active = false;
    };
    // Rewrites `info_arg` (a host VkDeviceCreateInfo*) when the physical
    // device supports placed maps; false leaves it untouched.
    bool vk_placed_prepare(uint64_t physical, uint64_t& info_arg, VkPlacedDeviceRequest& req);
    void vk_placed_register(uint64_t device, uint64_t alignment);
    // Maps a whole allocation at a fresh guest range; 0 when it cannot.
    uint64_t vk_placed_map(uint64_t device, uint64_t memory, uint64_t alloc_size);
    // Unmaps (keeping the range reserved), restores guest memory there and
    // releases the range. Called without vk_maps_mu.
    void vk_placed_unmap(uint64_t device, uint64_t memory, uint64_t base, uint64_t size);
    void vk_placed_release_device(uint64_t device);
    // Says once per distinct reason why a map keeps its bounce.
    static void vk_placed_note(const char* reason);
    // Handle registry for stale-handle fail-closed validation (vkquake:
    // single corrupt bind/cmd per ~10k good calls after 30+ good frames).
    // Every guest-visible descriptor set / command buffer is born in our
    // dispatch (alloc OUT writeback), so the registry is complete; a use
    // of an unknown handle is a stale/UAF handle → skip/empty the call
    // with a loud warn instead of segfaulting the host driver.
    std::unordered_set<uint64_t> vk_desc_sets_;
    std::unordered_set<uint64_t> vk_cmd_bufs_;
    std::mutex vk_handles_mu;
    // Cached host vkUnmapMemory (resolved in register_known_symbols_ from
    // the VK family handle). The map/unmap/free arms need it directly:
    // calling the MAP arm's own entry.host_fn as a 2-arg unmap invokes
    // vkMapMemory with garbage regs (ppData write through a garbage
    // pointer). Written once before initialized=true; never rewritten.
    void* vk_unmap_host_ = nullptr;
    // Guest instance API version, requested through VkApplicationInfo at
    // vkCreateInstance, encoded (major << 8) | minor. The Khronos loader
    // populates a device dispatch table only for that version, so a newer
    // core entry point jumps through a null slot and the host dies at
    // pc=0. dispatch rejects such calls with VK_ERROR_FEATURE_NOT_PRESENT.
    // Multi-instance guests are rare; the most recent create wins.
    uint32_t vk_instance_api_version_ = 0x0100u;   // 1.0
    // live bytes available at guest addr inside a tracked allocation.
    // untracked bases (stack/brk/elf via map_range) are allowed up to the
    // next tracked neighbor so a write never sprays into tracked heap.
    // 0 = nothing safe (must not touch).
    size_t live_room(const std::vector<std::pair<uint64_t,uint64_t>>& live,
                     uint64_t g, size_t want) {
        if (!want) return 0;
        for (const auto& kv : live) {
            if (g >= kv.first && g - kv.first < kv.second) {
                uint64_t room = kv.second - (g - kv.first);
                return (want <= room) ? want : static_cast<size_t>(room);
            }
        }
        size_t allow = want;
        for (const auto& kv : live) {
            if (kv.first > g) {
                uint64_t gap = kv.first - g;
                if (gap < allow) allow = static_cast<size_t>(gap);
            }
        }
        return allow;
    }
    // Strict variant for mmap_alloc'd bounces: an address that is NOT inside
    // ANY tracked allocation (freed / untracked / reused) is never safe to
    // touch — return 0. live_room's untracked allowance exists only for
    // guest stack/brk structs (pNext writeback), which are never bounces; a
    // stale bounce is always mmap_alloc'd, so "not tracked" means the range
    // was handed back and may now hold a live guest allocation. Mirrors the
    // gl safe_writeback_all skip-untracked rule (GraphicThunk.cpp).
    size_t tracked_room(const std::vector<std::pair<uint64_t,uint64_t>>& live,
                        uint64_t g, size_t want) {
        if (!want) return 0;
        for (const auto& kv : live) {
            if (g >= kv.first && g - kv.first < kv.second) {
                uint64_t room = kv.second - (g - kv.first);
                return (want <= room) ? want : static_cast<size_t>(room);
            }
        }
        return 0;
    }
    // Placed maps need no synchronization: only bounces make push/pull (and
    // their allocation snapshot) worth doing.
    bool has_bounce_maps_locked() const {
        for (const auto& kv : vk_maps_)
            if (!kv.second.placed) return true;
        return false;
    }
    // Publish changed guest bytes to host mappings before submits/presents.
    // No snapshot without a bounce: the map copy + lock is pure overhead at
    // ~10M thunk calls/s, and with placed maps the common case has none.
    void vk_sync_push_all() {
        std::lock_guard<std::mutex> g(vk_maps_mu);
        if (!has_bounce_maps_locked()) return;
        auto live = mem->allocations_snapshot();
        for (auto& kv : vk_maps_) {
            VkMapped& m = kv.second;
            if (m.placed || !m.map_size || !m.host_ptr) continue;
            if (!tracked_room(live, m.bounce, static_cast<size_t>(m.map_size)))
                continue;  // stale bounce: never push from reused heap
            uint8_t* src = mem->guest_to_host_ptr(m.bounce);
            if (src)
                vk_mapped::mapped_push(reinterpret_cast<uint8_t*>(m.host_ptr), src,
                                       m.baseline.data(), m.map_size);
        }
    }
    // Merge host readback after completion waits, preserving guest writes
    // prepared for later submits (including writes in other allocations).
    // Same no-bounce fast path as the push side.
    void vk_sync_pull_all() {
        std::lock_guard<std::mutex> g(vk_maps_mu);
        if (!has_bounce_maps_locked()) return;
        auto live = mem->allocations_snapshot();
        for (auto& kv : vk_maps_) {
            VkMapped& m = kv.second;
            if (m.placed || !m.map_size || !m.host_ptr) continue;
            if (!tracked_room(live, m.bounce, static_cast<size_t>(m.map_size)))
                continue;  // stale bounce: never pull into reused heap
            uint8_t* dst = mem->guest_to_host_ptr(m.bounce);
            if (dst)
                vk_mapped::mapped_pull(reinterpret_cast<const uint8_t*>(m.host_ptr), dst,
                                       m.baseline.data(), m.map_size);
        }
    }
    uint64_t cache_host_string_(const char* host_str) {
        if (!mem || !string_cache_base || !host_str) return 0;
        size_t len = std::strlen(host_str) + 1;
        if (len > STRING_CACHE_SIZE) len = STRING_CACHE_SIZE;
        if (string_cache_off + len > STRING_CACHE_SIZE)
            string_cache_off = 0;
        uint64_t guest = string_cache_base + string_cache_off;
        mem->write(guest, host_str, len);
        string_cache_off = static_cast<uint32_t>(
            (string_cache_off + len + 7u) & ~7u);
        return guest;
    }
};

struct VkStage {
    std::vector<uint8_t> buf;
    // Reserve once up front: `alloc`/`bytes`/`guest_str*` hand out pointers
    // into `buf.data()`, and `resize` would REALLOCATE (dangling every
    // earlier pointer) once a later string/array grows the buffer. Every
    // size below is capped (guest_str 511 chars, string arrays/queue
    // arrays <= 1024/16 entries), so total staging stays well under this.
    explicit VkStage() { buf.reserve(65536); }
    size_t put(size_t sz, size_t align) {
        size_t off = (buf.size() + align - 1u) & ~(align - 1u);
        buf.resize(off + sz);
        return off;
    }
    template <typename T> T* alloc() {
        return reinterpret_cast<T*>(buf.data() + put(sizeof(T), alignof(T)));
    }
    void* bytes(size_t sz, size_t align) {
        return buf.data() + put(sz, align);
    }
    // Copy a guest string into staging. Returns the host pointer (or
    // nullptr when the guest pointer is 0). Short strings only — a cap of
    // 511 chars is fine for extension / app names.
    const char* guest_str(Memory* mem, uint64_t g) {
        if (!g) return nullptr;
        char tmp[512];
        size_t n = 0;
        try {
            while (n + 1 < sizeof(tmp)) {
                uint8_t c = 0;
                mem->read(g + n, &c, 1);
                tmp[n++] = static_cast<char>(c);
                if (c == 0) break;
            }
        } catch (...) { /* unmapped — truncate */ }
        tmp[n] = 0;
        size_t off = put(n + 1, 1);
        std::memcpy(buf.data() + off, tmp, n + 1);
        return reinterpret_cast<const char*>(buf.data() + off);
    }
    // Copy a guest array of `count` char* into staging, re-pointing each
    // string into the staging buffer. Returns the host array pointer (or
    // nullptr when the guest array / count is 0).
    const char** guest_str_array(Memory* mem, uint64_t arr, uint32_t count) {
        if (!arr || count == 0 || count > 1024) return nullptr;
        const char** out = reinterpret_cast<const char**>(
            bytes(static_cast<size_t>(count) * sizeof(const char*), 8));
        for (uint32_t i = 0; i < count; i++) {
            uint64_t p = 0;
            try { mem->read(arr + static_cast<uint64_t>(i) * 8u, &p, 8); }
            catch (...) { p = 0; }
            out[i] = guest_str(mem, p);
        }
        return out;
    }
};

// VK_CMD_DEEP staging limits (VulkanStaging.cpp).
constexpr size_t kVkDeepMaxElems = 1024;
constexpr size_t kVkDeepMaxBytes = 4u << 20;   // 4 MiB staging cap
constexpr int kVkDeepMaxPnextNodes = 8;        // chain-node cap (hand-arm parity)
const void* vk_deep_read(Memory* mem, uint64_t g, size_t size, void* buf, size_t bufsz);
const void* vk_deep_ptr_field(const void* elem, const thunk::VkFieldDesc& f);
uint32_t vk_deep_count(const void* elem, const thunk::VkFieldDesc& f);
void vk_deep_unknown_stype_once(int32_t s);
void vk_deep_oversize_count_once(const char* name, uint8_t arg, uint64_t raw64, uint32_t masked);
void vk_deep_misaligned_once(const char* name, uint8_t arg, uint64_t ptr);
size_t vk_deep_guest_strlen(Memory* mem, uint64_t g);
size_t vk_deep_size_one(Memory* mem, uint64_t guest, const thunk::VkStructDesc* d,
                        uint32_t count, int depth, bool* ok, bool skip_pnext = false);
void vk_deep_fill_elem(Memory* mem, VkStage& st, uint64_t guest, const thunk::VkStructDesc* d,
                       uint8_t* out, int depth, bool skip_pnext = false);
size_t vk_deep_chain_size(Memory* mem, uint64_t head, bool* ok, int depth = 0);
void vk_deep_fill_chain(Memory* mem, VkStage& st, uint64_t head, void** slot, int depth = 0);
// Proxy symbols that need an SDL window (Proxy.cpp).
bool proxy_symbol_needs_sdl(const std::string& sym_name);
}  // namespace arm64emu
