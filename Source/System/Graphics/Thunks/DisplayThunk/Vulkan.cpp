// Vulkan entry points that need deep marshalling (create/submit/present/map).
#include "System/Graphics/Thunks/DisplayThunk/DisplayThunkInternal.hpp"
namespace arm64emu {
// ── Vulkan marshalling ───────────────────────────────────
// The generic bounce path copies pointer args byte-for-byte, which cannot
// fix the NESTED guest pointers inside the instance/device create infos
// (ppEnabledExtensionNames string arrays, pQueueCreateInfos struct array
// with pQueuePriorities, pApplicationInfo). Those two entry points get a
// deep-copy into a per-call host staging buffer. Opaque Vk handles are
// intentionally NOT translated: the guest stores the host pointer the
// host returned, so passing handle args verbatim round-trips them. Only
// the OUT handle slots (pInstance / pDevice) are written back after the
// host call.
namespace {
// Per-call host staging buffer. All nested strings, struct copies and
// pointer arrays for one Vulkan call live here; it is destroyed when the
// call returns.
// VkPresentInfoKHR (spec-stable, no padding on AArch64).
struct VkPresentInfoH {
    int32_t sType; void* pNext; uint32_t waitSemaphoreCount;
    const void* pWaitSemaphores; uint32_t swapchainCount;
    const void* pSwapchains; const uint32_t* pImageIndices;
    int32_t* pResults;  // VkResult array (may be NULL)
};
// vkMapMemory bounce stage (2026-08-21): VkMemoryAllocateInfo (records the
// allocation size for VK_WHOLE_SIZE maps) and VkMappedMemoryRange (flat —
// flush/invalidate move ranges between bounce and host mapping).
struct VkMemoryAllocateInfoH {
    int32_t sType; void* pNext; size_t allocationSize; uint32_t memoryTypeIndex;
};
struct VkMappedMemoryRangeH {
    int32_t sType; void* pNext; uint64_t memory; size_t offset; size_t size;
};
constexpr size_t kPhysicalDeviceFeaturesBytes = 220;
// Read a guest struct by value into `dst` (zero-fill on unmapped).
template <typename T> void read_guest_struct(Memory* mem, uint64_t g, T* dst) {
    if (dst) {
        if (!g) { std::memset(dst, 0, sizeof(*dst)); return; }
        try { mem->read(g, dst, sizeof(*dst)); }
        catch (...) { std::memset(dst, 0, sizeof(*dst)); }
    }
}
// Read `n` guest bytes into `dst` (zero-fill on unmapped).
void read_guest_bytes(Memory* mem, uint64_t g, void* dst, size_t n) {
    if (!g || !n) { if (dst && n) std::memset(dst, 0, n); return; }
    try { mem->read(g, dst, n); }
    catch (...) { std::memset(dst, 0, n); }
}

// ── pNext chain deep-marshal (generated, Phase A1) ───────────────────
// Extension structs chained via pNext are GUEST pointers; the host driver
// walks the chain and derefs them. Node layouts come from the GENERATED
// sType→descriptor map (vk_find_struct_by_stype, built from vk.xml) so
// every registry-known chainable struct is covered — not just a hand
// table. Unknown sTypes truncate the chain at that node (one-shot
// diagnostic). Sizes are validated against the vendored vulkan_core.h by
// the generator's layout engine.
constexpr uint32_t kMaxPnextNodes = kVkDeepMaxPnextNodes;
struct VkPnextNode { uint8_t* host; uint64_t guest; size_t size; };
// Walks the guest chain starting at `g`; fills up to kMaxPnextNodes nodes.
uint32_t vk_marshal_pnext_chain(Memory* mem, VkStage& st, uint64_t g,
                                VkPnextNode* nodes) {
    uint32_t n = 0;
    void* prev = nullptr;
    while (g && n < kMaxPnextNodes) {
        int32_t s = 0; uint64_t pn = 0;
        try { mem->read(g, &s, 4); mem->read(g + 8, &pn, 8); }
        catch (...) { break; }
        const thunk::VkStructDesc* d = thunk::vk_find_struct_by_stype(s);
        if (!d) { vk_deep_unknown_stype_once(s); break; }
        uint8_t* h = reinterpret_cast<uint8_t*>(st.bytes(d->size, 8));
        read_guest_bytes(mem, g, h, d->size);
        *reinterpret_cast<void**>(h + 8) = nullptr;
        if (prev) *reinterpret_cast<void**>(static_cast<uint8_t*>(prev) + 8) = h;
        nodes[n++] = {h, g, d->size};
        prev = h;
        g = pn;
    }
    return n;
}
void vk_writeback_pnext_chain(Memory* mem, const VkPnextNode* nodes,
                              uint32_t n) {
    auto live = mem->allocations_snapshot();
    auto room = [&](uint64_t g, size_t want) -> size_t {
        if (!want) return 0;
        for (const auto& kv : live) {
            if (g >= kv.first && g - kv.first < kv.second) {
                uint64_t r = kv.second - (g - kv.first);
                return (want <= r) ? want : static_cast<size_t>(r);
            }
        }
        // untracked base (stack/brk): allow up to next tracked neighbor.
        size_t allow = want;
        for (const auto& kv : live) {
            if (kv.first > g) {
                uint64_t gp = kv.first - g;
                if (gp < allow) allow = static_cast<size_t>(gp);
            }
        }
        return allow;
    };
    for (uint32_t i = 0; i < n; i++) {
        // Write back everything EXCEPT the pNext link (offsets 8..16):
        // our staged copy carries a HOST pointer there which must never
        // land in guest memory (it would zero/corrupt the guest chain).
        const VkPnextNode& nd = nodes[i];
        if (nd.size > 8) {
            if (room(nd.guest, 8) == 8) {
                try { mem->write(nd.guest, nd.host, 8); } catch (...) {}
            }
        }
        if (nd.size > 16) {
            size_t have = room(nd.guest + 16, nd.size - 16);
            if (have) {
                try { mem->write(nd.guest + 16, nd.host + 16, have); } catch (...) {}
            }
        }
    }
}
} // namespace

bool DisplayThunk::vk_dispatch_(CPU& cpu, const SymbolEntry& entry, bool trace) {
    Memory* mem = impl_->mem;
    if (!mem) return false;
    if (trace)
        fprintf(stderr, "[display-thunk] vk_dispatch_[T%lx]: %s\n",
                (unsigned long)pthread_self(), entry.name.c_str());

    // ── vkGetInstanceProcAddr / vkGetDeviceProcAddr ────────────────
    // The generic THUNK_GET_PROC path reads the name from arg 0 (GL
    // convention); Vulkan's proc-addr functions take the name in arg 1
    // (arg 0 is the instance/device handle).
    if (entry.flags & THUNK_GET_PROC) {
        char namebuf[256];
        size_t n = 0;
        uint64_t pname = cpu.regs[1];
        if (pname) {
            try {
                while (n + 1 < sizeof(namebuf)) {
                    uint8_t c = 0;
                    mem->read(pname + n, &c, 1);
                    namebuf[n++] = static_cast<char>(c);
                    if (c == 0) break;
                }
            } catch (...) { /* truncate */ }
        }
        namebuf[n] = 0;
        uint64_t found = 0;
        if (namebuf[0]) {
            for (const auto& lib : impl_->libs_) {
                for (const auto& e : lib.entries) {
                    if (e.name == namebuf) { found = e.guest_addr; break; }
                }
                if (found) break;
            }
        }
        if (trace) {
            fprintf(stderr, "[display-thunk] vkGetProcAddr('%s') → 0x%llx\n",
                    namebuf[0] ? namebuf : "(null)",
                    static_cast<unsigned long long>(found));
        }
        cpu.regs[0] = found;
        return true;
    }

    // ── vkQueuePresentKHR ─────────────────────────────────────────────
    // (queue, pPresentInfo) — pPresentInfo's NESTED pointers
    // (pWaitSemaphores / pSwapchains / pImageIndices / pResults) are guest
    // addresses the host cannot dereference: the generic bounce copies the
    // top-level struct but leaves nested guest pointers untouched, so the
    // host faults reading e.g. pSwapchains[0]. Re-point every array into
    // the staging buffer (handles round-trip VERBATIM), and writeback
    // pResults after the call.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_PRESENT) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        // Read/validate the guest struct BEFORE staging: the pNext chain
        // size must be known so st.buf.reserve() can happen before `pi`
        // (and every staged array) is handed out — a later realloc would
        // dangle them all.
        VkPresentInfoH tmp{};
        read_guest_struct(mem, cpu.regs[1], &tmp);
        // fail closed on oversize counts: leaving guest pointers in
        // place would let the host deref guest addresses (crash).
        // VK_ERROR_OUT_OF_HOST_MEMORY = -1.
        if (tmp.waitSemaphoreCount > 16 || tmp.swapchainCount > 16 ||
            tmp.swapchainCount == 0) {
            cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(-1));
            return true;
        }
        size_t chain_need = 0;
        if (tmp.pNext) {
            bool ok = true;
            chain_need = vk_deep_chain_size(mem,
                reinterpret_cast<uint64_t>(tmp.pNext), &ok);
            if (!ok) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(-1));
                return true;  // never fill a chain that failed the sizing pass
            }
        }
        st.buf.reserve(std::max<size_t>(chain_need + 4096, 65536));
        VkPresentInfoH* pi = st.alloc<VkPresentInfoH>();
        *pi = tmp;
        // stage known pNext nodes instead of silently dropping the chain.
        // unknown stypes truncate with a one-shot diagnostic (safe).
        if (pi->pNext) {
            void* staged = nullptr;
            vk_deep_fill_chain(mem, st,
                reinterpret_cast<uint64_t>(pi->pNext), &staged);
            pi->pNext = staged;
        } else {
            pi->pNext = nullptr;
        }
        if (pi->waitSemaphoreCount && pi->pWaitSemaphores && pi->waitSemaphoreCount <= 16) {
            uint64_t* arr = reinterpret_cast<uint64_t*>(
                st.bytes(static_cast<size_t>(pi->waitSemaphoreCount) * 8u, 8));
            read_guest_bytes(mem, reinterpret_cast<uint64_t>(pi->pWaitSemaphores), arr,
                             static_cast<size_t>(pi->waitSemaphoreCount) * 8u);
            pi->pWaitSemaphores = arr;
        }
        if (pi->swapchainCount && pi->pSwapchains && pi->swapchainCount <= 16) {
            uint64_t* arr = reinterpret_cast<uint64_t*>(
                st.bytes(static_cast<size_t>(pi->swapchainCount) * 8u, 8));
            read_guest_bytes(mem, reinterpret_cast<uint64_t>(pi->pSwapchains), arr,
                             static_cast<size_t>(pi->swapchainCount) * 8u);
            pi->pSwapchains = arr;
        }
        if (pi->pImageIndices && pi->swapchainCount && pi->swapchainCount <= 16) {
            uint32_t* arr = reinterpret_cast<uint32_t*>(
                st.bytes(static_cast<size_t>(pi->swapchainCount) * 4u, 4));
            read_guest_bytes(mem, reinterpret_cast<uint64_t>(pi->pImageIndices), arr,
                             static_cast<size_t>(pi->swapchainCount) * 4u);
            pi->pImageIndices = arr;
        }
        int32_t* results_host = nullptr;
        uint64_t results_guest = 0;
        if (pi->pResults && pi->swapchainCount && pi->swapchainCount <= 16) {
            results_host = reinterpret_cast<int32_t*>(
                st.bytes(static_cast<size_t>(pi->swapchainCount) * 4u, 4));
            read_guest_bytes(mem, reinterpret_cast<uint64_t>(pi->pResults), results_host,
                             static_cast<size_t>(pi->swapchainCount) * 4u);
            results_guest = reinterpret_cast<uint64_t>(pi->pResults);
            pi->pResults = results_host;
        }
        // Push all vkMapMemory bounces into their host mappings before the
        // GPU consumes the frame (PCWFC discipline — the practical
        // HOST_COHERENT guarantee for mapped memory).
        impl_->vk_sync_push_all();
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, const void*)>(entry.host_fn)(
            cpu.regs[0], pi);
        if (results_guest && results_host) {
            size_t want = static_cast<size_t>(pi->swapchainCount) * 4u;
            auto rlive = mem->allocations_snapshot();
            size_t have = 0;
            for (const auto& kv : rlive) {
                if (results_guest >= kv.first &&
                    results_guest - kv.first < kv.second) {
                    uint64_t room = kv.second - (results_guest - kv.first);
                    have = (want <= room) ? want : static_cast<size_t>(room);
                    break;
                }
            }
            if (have) {
                try { mem->write(results_guest, results_host, have); }
                catch (...) { /* out pointer unmapped — results lost */ }
            }
        }
        if (dbg().frame_trace)
            fprintf(stderr,"[window-stats] present ret=%d count=%u chain=%llx\n",static_cast<int32_t>(ret),pi->swapchainCount,pi->pSwapchains ? (unsigned long long)static_cast<const uint64_t*>(pi->pSwapchains)[0] : 0);
        if ((static_cast<int32_t>(ret) == 0 || static_cast<int32_t>(ret) == 1000001003) && pi->pSwapchains) {
            for (uint32_t i = 0; i < pi->swapchainCount; ++i)
                if (!results_host || results_host[i] == 0 || results_host[i] == 1000001003)
                    window_stats::vk_present(static_cast<const uint64_t*>(pi->pSwapchains)[i]);
        }
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) {
            fprintf(stderr, "[display-thunk] vkQueuePresentKHR → %d\n", static_cast<int32_t>(ret));
        }
        return true;
    }

    // ── vkAllocateMemory ────────────────────────────────────────────────
    // (device, pAllocateInfo, pAllocator, pMemory) — records the allocation
    // size so later VK_WHOLE_SIZE maps can resolve. OUT handle at arg 3.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_ALLOC_MEMORY) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        VkMemoryAllocateInfoH* info = st.alloc<VkMemoryAllocateInfoH>();
        read_guest_struct(mem, cpu.regs[1], info);
        info->pNext = nullptr;
        size_t alloc_size = info->allocationSize;
        uint64_t host_mem = 0;
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, const void*, const void*, void*)>(entry.host_fn)(
            cpu.regs[0], info, nullptr, &host_mem);
        if (ret == 0 && host_mem) {
            try { mem->write(cpu.regs[3], &host_mem, sizeof(host_mem)); }
            catch (...) { /* out pointer unmapped */ }
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            impl_->vk_allocs_[host_mem] = alloc_size;
        }
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) fprintf(stderr, "[display-thunk] vkAllocateMemory (%zu bytes) → %d\n",
                           alloc_size, static_cast<int32_t>(ret));
        return true;
    }

    // ── vkFreeMemory ─────────────────────────────────────────────────────
    // (device, memory, pAllocator) — drops our records; if the memory is
    // still mapped (spec violation, but be defensive like glDeleteBuffers),
    // push the bounce back and release it before freeing.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_FREE_MEMORY) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint64_t mem_handle = cpu.regs[1];
        bool was_mapped = false;
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            auto it = impl_->vk_maps_.find(mem_handle);
            if (it != impl_->vk_maps_.end()) {
                uint8_t* src = mem->guest_to_host_ptr(it->second.bounce);
                if (src && it->second.host_ptr && it->second.map_size)
                    vk_mapped::mapped_push(reinterpret_cast<uint8_t*>(it->second.host_ptr), src,
                                           it->second.baseline.data(), it->second.map_size);
                mem->untrack_allocation(it->second.bounce,
                                        (it->second.map_size + 0xFFFu) & ~0xFFFull);
                impl_->vk_maps_.erase(it);
                was_mapped = true;
            }
            impl_->vk_allocs_.erase(mem_handle);
        }
        // Spec requires unmap-before-free; do the host unmap first when a
        // live mapping existed (else the host mapping leaks / driver UB).
        if (was_mapped && impl_->vk_unmap_host_)
            reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(
                impl_->vk_unmap_host_)(cpu.regs[0], mem_handle);
        reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, const void*)>(entry.host_fn)(
            cpu.regs[0], mem_handle, nullptr);
        cpu.regs[0] = 0;
        if (trace) fprintf(stderr, "[display-thunk] vkFreeMemory → 0\n");
        return true;
    }

    // ── vkMapMemory ──────────────────────────────────────────────────────
    // (device, memory, offset, size, flags, ppData) — the host driver writes
    // a HOST pointer into ppData, useless in the guest. Instead: map on the
    // host with a scratch pointer, allocate a bounce in the guest
    // reservation, seed it from the host mapping, and write the BOUNCE's guest
    // address into ppData. Guest reads/writes then hit the window at full
    // JIT speed; coherence comes from the submit/present push + wait pull
    // (and explicit flush/invalidate ranges).
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_MAP_MEMORY) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint64_t mem_handle = cpu.regs[1];
        const uint64_t offset = cpu.regs[2];
        uint64_t size = cpu.regs[3];
        const uint64_t flags = cpu.regs[4];
        // Resolve VK_WHOLE_SIZE from the recorded allocation size.
        uint64_t alloc_size = 0;
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            auto ai = impl_->vk_allocs_.find(mem_handle);
            if (ai != impl_->vk_allocs_.end()) alloc_size = ai->second;
        }
        if (size == ~0ull) {
            if (alloc_size == 0 || offset >= alloc_size) {
                cpu.regs[0] = 0xFFFFFFFBu;  // VK_ERROR_MEMORY_MAP_FAILED
                return true;
            }
            size = alloc_size - offset;
        }
        // Remap of an already-mapped object: only an IDENTICAL
        // offset/size shape returns the existing bounce (what real drivers
        // do). A different shape must NOT reuse the old bounce — the guest
        // would index the OLD (possibly smaller) tracked range with the
        // NEW offset+size and overrun the bounce allocation into
        // neighboring window memory (guest-heap stomp). Treat it as an
        // implicit unmap+remap: push the old bounce back to the host,
        // release its window range, host-unmap, then fall through and
        // build a fresh mapping.
        {
            uint64_t s_host = 0, s_bounce = 0, s_size = 0;
            std::vector<uint8_t> s_baseline;
            bool had_stale = false;
            {
                std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
                auto it = impl_->vk_maps_.find(mem_handle);
                if (it != impl_->vk_maps_.end()) {
                    if (it->second.map_offset == offset &&
                        it->second.map_size == size) {
                        uint64_t same = it->second.bounce;
                        if (cpu.regs[5]) {
                            try { mem->write(cpu.regs[5], &same, sizeof(same)); }
                            catch (...) { /* out pointer unmapped */ }
                        }
                        cpu.regs[0] = 0;
                        return true;
                    }
                    s_host = it->second.host_ptr;
                    s_bounce = it->second.bounce;
                    s_size = it->second.map_size;
                    s_baseline = std::move(it->second.baseline);
                    impl_->vk_maps_.erase(it);
                    had_stale = true;
                }
            }
            if (had_stale) {
                uint8_t* src = mem->guest_to_host_ptr(s_bounce);
                if (src && s_host && s_size)
                    vk_mapped::mapped_push(reinterpret_cast<uint8_t*>(s_host), src,
                                           s_baseline.data(), s_size);
                mem->untrack_allocation(s_bounce,
                                        (s_size + 0xFFFu) & ~0xFFFull);
                // Host-unmap via the cached vkUnmapMemory — NEVER the MAP
                // arm's entry.host_fn (that's vkMapMemory; calling it with
                // 2 args feeds garbage offset/size/flags/ppData).
                if (impl_->vk_unmap_host_)
                    reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(
                        impl_->vk_unmap_host_)(cpu.regs[0], mem_handle);
            }
        }
        if (size == 0 || size > (1ull << 30)) {  // sanity cap: 1 GiB per map
            cpu.regs[0] = 0xFFFFFFFBu;
            return true;
        }
        void* host_ptr = nullptr;
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, void**)>(entry.host_fn)(
            cpu.regs[0], mem_handle, offset, size, flags, &host_ptr);
        if (ret != 0 || !host_ptr) {
            cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
            if (trace) fprintf(stderr, "[display-thunk] vkMapMemory → %d (FAILED)\n",
                               static_cast<int32_t>(ret));
            return true;
        }
        uint64_t bounce = mem->mmap_alloc(size);
        if (bounce == 0 || bounce == ~0ull) {
            // No window space — unmap on the host and report failure.
            if (impl_->vk_unmap_host_)
                reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(
                    impl_->vk_unmap_host_)(cpu.regs[0], mem_handle);
            cpu.regs[0] = 0xFFFFFFFBu;
            return true;
        }
        uint8_t* dst = mem->guest_to_host_ptr(bounce);
        if (!dst) {
            // The bounce is not writable as one host range: release it
            // and fail like a failed alloc instead of memcpy through null.
            mem->untrack_allocation(bounce, (size + 0xFFFu) & ~0xFFFull);
            if (impl_->vk_unmap_host_)
                reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(
                    impl_->vk_unmap_host_)(cpu.regs[0], mem_handle);
            cpu.regs[0] = 0xFFFFFFFBu;
            return true;
        }
        std::memcpy(dst, host_ptr, size);  // seed: host → bounce
        DisplayThunkImpl::VkMapped mapping{
            reinterpret_cast<uint64_t>(host_ptr), bounce, offset, size, alloc_size, {}};
        try {
            mapping.baseline.assign(dst, dst + size);
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            impl_->vk_maps_.emplace(mem_handle, std::move(mapping));
        } catch (const std::bad_alloc&) {
            mem->untrack_allocation(bounce, (size + 0xFFFu) & ~0xFFFull);
            if (impl_->vk_unmap_host_)
                reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(
                    impl_->vk_unmap_host_)(cpu.regs[0], mem_handle);
            cpu.regs[0] = 0xFFFFFFFFu; // VK_ERROR_OUT_OF_HOST_MEMORY
            return true;
        }
        if (cpu.regs[5]) {
            try { mem->write(cpu.regs[5], &bounce, sizeof(bounce)); }
            catch (...) { /* out pointer unmapped — address lost */ }
        }
        cpu.regs[0] = 0;
        if (trace) fprintf(stderr, "[display-thunk] vkMapMemory (off=0x%llx sz=%llu) → 0 (guest ptr=0x%llx)\n",
                           (unsigned long long)offset, (unsigned long long)size,
                           (unsigned long long)bounce);
        return true;
    }

    // ── vkUnmapMemory ────────────────────────────────────────────────────
    // (device, memory) — push the whole bounce back, release the window
    // range, then unmap on the host.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_UNMAP_MEMORY) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint64_t mem_handle = cpu.regs[1];
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            auto it = impl_->vk_maps_.find(mem_handle);
            if (it != impl_->vk_maps_.end()) {
                uint8_t* src = mem->guest_to_host_ptr(it->second.bounce);
                if (src && it->second.host_ptr && it->second.map_size)
                    vk_mapped::mapped_push(reinterpret_cast<uint8_t*>(it->second.host_ptr), src,
                                           it->second.baseline.data(), it->second.map_size);
                mem->untrack_allocation(it->second.bounce,
                                        (it->second.map_size + 0xFFFu) & ~0xFFFull);
                impl_->vk_maps_.erase(it);
            }
        }
        reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(entry.host_fn)(cpu.regs[0], mem_handle);
        cpu.regs[0] = 0;
        if (trace) fprintf(stderr, "[display-thunk] vkUnmapMemory → 0\n");
        return true;
    }

    // ── vkFlushMappedMemoryRanges ────────────────────────────────────────
    // (device, rangeCount, pRanges) — VkMappedMemoryRange is flat; move each
    // range bounce → host BEFORE the host flush (which makes non-coherent
    // memory visible to the GPU).
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_FLUSH_MAPPED) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint32_t count = static_cast<uint32_t>(cpu.regs[1]);
        if (count == 0 || !cpu.regs[2]) { cpu.regs[0] = 0; return true; }
        if (count > 64) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        VkMappedMemoryRangeH* rr = reinterpret_cast<VkMappedMemoryRangeH*>(
            st.bytes(count * sizeof(VkMappedMemoryRangeH), 8));
        read_guest_bytes(mem, cpu.regs[2], rr, count * sizeof(VkMappedMemoryRangeH));
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            auto flive = mem->allocations_snapshot();
            for (uint32_t i = 0; i < count; i++) {
                rr[i].pNext = nullptr;
                auto it = impl_->vk_maps_.find(rr[i].memory);
                if (it == impl_->vk_maps_.end()) continue;
                auto& m = it->second;
                uint64_t off = rr[i].offset;
                uint64_t sz = rr[i].size;
                if (sz == ~0ull) sz = m.map_size - (off - m.map_offset);
                if (off < m.map_offset) continue;
                off -= m.map_offset;
                // No `off + sz` here: the sum wraps in u64 and a crafted
                // range would pass. off < map_size is established above.
                if (off >= m.map_size || sz > m.map_size - off) continue;
                if (!impl_->tracked_room(flive, m.bounce + off, static_cast<size_t>(sz)))
                    continue;  // stale bounce: never push from reused heap
                uint8_t* src = mem->guest_to_host_ptr(m.bounce + off);
                if (src) {
                    std::memcpy(reinterpret_cast<void*>(m.host_ptr + off), src, sz);
                    std::memcpy(m.baseline.data() + off, src, sz);
                }
            }
        }
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, uint32_t, const void*)>(entry.host_fn)(
            cpu.regs[0], count, rr);
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) fprintf(stderr, "[display-thunk] vkFlushMappedMemoryRanges (n=%u) → %d\n",
                           count, static_cast<int32_t>(ret));
        return true;
    }

    // ── vkInvalidateMappedMemoryRanges ───────────────────────────────────
    // (device, rangeCount, pRanges) — host invalidate FIRST (pulls GPU
    // writes into the host mapping), then copy host → bounce so the guest
    // sees them.
    if (entry.spec && entry.spec->policy == thunk::Policy::VK_INVALIDATE_MAPPED) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        const uint32_t count = static_cast<uint32_t>(cpu.regs[1]);
        if (count == 0 || !cpu.regs[2]) { cpu.regs[0] = 0; return true; }
        if (count > 64) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        VkMappedMemoryRangeH* rr = reinterpret_cast<VkMappedMemoryRangeH*>(
            st.bytes(count * sizeof(VkMappedMemoryRangeH), 8));
        read_guest_bytes(mem, cpu.regs[2], rr, count * sizeof(VkMappedMemoryRangeH));
        for (uint32_t i = 0; i < count; i++) rr[i].pNext = nullptr;
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, uint32_t, const void*)>(entry.host_fn)(
            cpu.regs[0], count, rr);
        {
            std::lock_guard<std::mutex> g(impl_->vk_maps_mu);
            auto ilive = mem->allocations_snapshot();
            for (uint32_t i = 0; i < count; i++) {
                auto it = impl_->vk_maps_.find(rr[i].memory);
                if (it == impl_->vk_maps_.end()) continue;
                auto& m = it->second;
                uint64_t off = rr[i].offset;
                uint64_t sz = rr[i].size;
                if (sz == ~0ull) sz = m.map_size - (off - m.map_offset);
                if (off < m.map_offset) continue;
                off -= m.map_offset;
                // Same no-wrap form as the flush arm above.
                if (off >= m.map_size || sz > m.map_size - off) continue;
                if (!impl_->tracked_room(ilive, m.bounce + off, static_cast<size_t>(sz)))
                    continue;  // stale bounce: never pull into reused heap
                uint8_t* dst = mem->guest_to_host_ptr(m.bounce + off);
                if (dst && ret == 0) {
                    std::memcpy(dst, reinterpret_cast<const void*>(m.host_ptr + off), sz);
                    std::memcpy(m.baseline.data() + off, dst, sz);
                }
            }
        }
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace) fprintf(stderr, "[display-thunk] vkInvalidateMappedMemoryRanges (n=%u) → %d\n",
                           count, static_cast<int32_t>(ret));
        return true;
    }

    // ── vkGetPhysicalDeviceProperties2 / Features2 (+KHR aliases) ─────
    // The pNext CHAIN carries guest pointers (driver/subgroup/accel
    // properties, subgroup/BDA features) that the host driver derefs.
    // Walk the chain by sType, copy each known node into staging,
    // re-link host-side, call, then write every node back (the driver
    // FILLS them). Unknown sType truncates the chain there (safe: the
    // caller sees the node untouched instead of garbage).
    if (entry.spec &&
        entry.name.rfind("vkGetPhysicalDevice", 0) == 0 &&
        (entry.name.find("Properties2") != std::string::npos ||
         entry.name.find("Features2") != std::string::npos)) {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        uint64_t g = cpu.regs[1];
        if (!g) { cpu.regs[0] = 0xFFFFFFFFFFFFFFFEull; return true; }
        VkStage st;
        VkPnextNode nodes[kMaxPnextNodes];
        uint32_t nnodes = vk_marshal_pnext_chain(mem, st, g, nodes);
        if (!nnodes) return false;  // not our shape — generic path
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, const void*)>(entry.host_fn)(
            cpu.regs[0], nodes[0].host);
        vk_writeback_pnext_chain(mem, nodes, nnodes);
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace)
            fprintf(stderr, "[display-thunk] %s → %d (%u chain nodes)\n",
                    entry.name.c_str(), static_cast<int32_t>(ret), nnodes);
        return true;
    }

    // ── vkEnumerateDeviceExtensionProperties: hide RT extensions ──────
    // Ray tracing needs deep-marshal arms that don't exist yet
    // (vkCmdBuildAccelerationStructures* carry nested geometry pointer
    // trees). Guests that see VK_KHR_ray_query demand its entry points
    // and Sys_Error when vkGetDeviceProcAddr misses — so filter the RT
    // extensions out of the reported list; apps fall back to non-RT
    // rendering paths. Remove this once build-acceleration marshalling
    // lands.
    // ── vkCreateDebugUtilsMessengerEXT — INTERCEPTED, never forwarded ──
    // The create info embeds pfnUserCallback, a GUEST function pointer.
    // Forwarding the struct to the host driver would hand it a raw guest
    // address to call as x86 (the glDebugMessageCallback rule: callbacks
    // never cross). We accept the messenger and mint a fake handle; debug
    // messages simply never fire.
    if (entry.spec && entry.name == "vkCreateDebugUtilsMessengerEXT") {
        static uint64_t dbg_messenger_ctr = 0;
        const uint64_t handle =
            0xD6C0FFEE00000000ull + ++dbg_messenger_ctr;
        if (cpu.regs[3])
            mem->write(cpu.regs[3], &handle, sizeof(handle));
        cpu.regs[0] = 0;   // VK_SUCCESS
        if (trace) {
            fprintf(stderr,
                    "[display-thunk] vkCreateDebugUtilsMessengerEXT → "
                    "intercepted (handle=0x%llx, callbacks suppressed)\n",
                    (unsigned long long)handle);
        }
        return true;
    }
    if (entry.spec && entry.name == "vkDestroyDebugUtilsMessengerEXT") {
        cpu.regs[0] = 0;   // accepted no-op
        return true;
    }
    if (entry.spec && entry.name == "vkEnumerateDeviceExtensionProperties") {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        // Declarative policy (plan C3): advertise ⟺ implemented.
        // RT needs acceleration-structure build marshalling; descriptor
        // buffer needs a whole bindless model. Guests that don't see
        // the strings take non-RT / conventional-binding paths.
        static const char* kHiddenExt[] = {
            "VK_KHR_ray_query", "VK_KHR_acceleration_structure",
            "VK_EXT_descriptor_buffer" };
        auto hidden = [](const char* n) {
            for (const char* h : kHiddenExt)
                if (std::strcmp(n, h) == 0)
                    return true;
            return false;
        };
        uint32_t count = 0;
        if (!cpu.regs[2]) { cpu.regs[0] = 0xFFFFFFFFFFFFFFFEull; return true; }
        try { mem->read(cpu.regs[2], &count, 4); } catch (...) { count = 0; }
        VkStage st;
        uint32_t actual = 0;
        // VkExtensionProperties { char name[256]; uint32 specVersion; }
        constexpr uint32_t kExtPropSize = 260;
        uint8_t* props = nullptr;
        if (cpu.regs[3] && count && count <= 1024) {
            props = reinterpret_cast<uint8_t*>(st.bytes(count * kExtPropSize, 4));
            auto fn = reinterpret_cast<uint64_t (*)(uint64_t, const void*, uint32_t*, void*)>(entry.host_fn);
            uint32_t tmp = count;
            uint64_t ret = fn(cpu.regs[0], nullptr, &tmp, props);
            actual = tmp;
            // Filter in place.
            uint32_t w = 0;
            for (uint32_t i = 0; i < actual; i++) {
                const char* nm = reinterpret_cast<const char*>(props + i * kExtPropSize);
                if (hidden(nm)) continue;
                if (w != i) std::memcpy(props + w * kExtPropSize,
                                        props + i * kExtPropSize, kExtPropSize);
                w++;
            }
            try { mem->write(cpu.regs[2], &w, 4); } catch (...) {}
            try { mem->write(cpu.regs[3], props, w * kExtPropSize); } catch (...) {}
            cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
            return true;
        }
        // Count-only query: forward verbatim (hiding only matters when
        // the guest then fetches names).
        auto fn2 = reinterpret_cast<uint64_t (*)(uint64_t, const void*, uint32_t*, void*)>(entry.host_fn);
        uint32_t tmp = count;
        uint64_t ret = fn2(cpu.regs[0], nullptr, &tmp, nullptr);
        try { mem->write(cpu.regs[2], &tmp, 4); } catch (...) {}
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        return true;
    }

    // ── vkBeginCommandBuffer ─────────────────────────────────────────
    // (cmdBuffer, pBeginInfo) — VkCommandBufferBeginInfo is 32 bytes:
    // {sType, pNext, flags, pInheritanceInfo}. Secondary buffers
    // (multithreaded recording) chain a GUEST VkCommandBufferInheritanceInfo
    // (56 bytes: handles + pNext) that the driver derefs.
    if (entry.spec && entry.name == "vkBeginCommandBuffer") {
        if (!entry.host_fn) { cpu.regs[0] = 0xFFFFFFFDu; return true; }
        VkStage st;
        uint8_t* top = reinterpret_cast<uint8_t*>(st.bytes(32, 8));
        read_guest_bytes(mem, cpu.regs[1], top, 32);
        VkPnextNode nodes[kMaxPnextNodes];
        uint32_t dn = vk_marshal_pnext_chain(
            mem, st, *reinterpret_cast<uint64_t*>(top + 8), nodes);
        *reinterpret_cast<void**>(top + 8) = dn ? nodes[0].host : nullptr;
        VkPnextNode inodes[kMaxPnextNodes];
        uint32_t din = 0;
        uint64_t ginh = *reinterpret_cast<uint64_t*>(top + 24);
        if (ginh) {
            uint8_t* inh = reinterpret_cast<uint8_t*>(st.bytes(56, 8));
            read_guest_bytes(mem, ginh, inh, 56);
            din = vk_marshal_pnext_chain(
                mem, st, *reinterpret_cast<uint64_t*>(inh + 8), inodes);
            *reinterpret_cast<void**>(inh + 8) = din ? inodes[0].host : nullptr;
            // Re-point the begin info at the HOST copy — the raw guest
            // pointer must never reach the driver.
            *reinterpret_cast<void**>(top + 24) = inh;
        }
        uint64_t ret = reinterpret_cast<uint64_t (*)(uint64_t, const void*)>(
            entry.host_fn)(cpu.regs[0], top);
        vk_writeback_pnext_chain(mem, nodes, dn);
        vk_writeback_pnext_chain(mem, inodes, din);
        cpu.regs[0] = static_cast<uint64_t>(static_cast<uint32_t>(ret));
        if (trace)
            fprintf(stderr, "[display-thunk] vkBeginCommandBuffer → %d\n",
                    static_cast<int32_t>(ret));
        return true;
    }

    // Not one of the deep-marshalling entry points — let the generic path
    // handle it with the corrected pointer masks.
    return false;
}
}  // namespace arm64emu
