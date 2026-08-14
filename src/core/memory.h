// core/memory.h — sparse paged 64-bit memory model (thread-safe).
//
// All public methods take a shared lock internally so they can be called
// concurrently from multiple guest threads (vCPUs). The lock is fine-
// grained (single mutex for the page map; per-page data is not locked
// because guest code is responsible for its own atomicity via LDXR/STXR
// or LSE atomics).
//
// The Memory model has two storage tiers:
//   1. A 4 GiB "direct window" (mmap'd) covering the low 4 GiB of the
//      guest address space. The window IS the storage — pages in this
//      range are not duplicated in pages_. The JIT can address the
//      window directly: `mov rax, [window_base + guest_addr]`.
//   2. A sparse `pages_` map for addresses ≥ 4 GiB (sigreturn
//      trampoline, dynamic-linker interpreter, and any explicit high
//      MAP_FIXED allocations). Backed by std::vector<uint8_t> per page.
//
// The guest heap (mmap_alloc base) and the main thread's stack live
// INSIDE the direct window (see MMAP_BASE_MIN / STACK_TOP below) so the
// JIT fast path covers them — every heap/stack access is a direct
// window memcpy instead of the pages_ + rwlock slow path.
#pragma once
#include "bifrost/types.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
namespace arm64emu {
class Memory {
public:
    static constexpr uint64_t PAGE_SIZE = 4096;
    static constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;
    // 1.5.2-alpha: Address space limits for robustness and security.
    // These prevent a malicious/buggy guest from exhausting host memory
    // or corrupting emulator-internal state.
    //
    // MAX_TOTAL_PAGES: hard cap on total allocated pages (default: 1M
    //   pages = 4 GiB). Beyond this, mmap_alloc returns -ENOMEM. This
    //   matches typical RLIMIT_AS settings on production Linux systems.
    //   Real game engines rarely exceed 2 GiB of mapped memory.
    //
    // MAX_MMAP_LENGTH: per-allocation cap (default: 4 GiB). A single
    //   mmap larger than this is rejected with -ENOMEM. Prevents a
    //   malicious guest from requesting SIZE_MAX and OOMing the host.
    //
    // NULL_PAGE_LIMIT: addresses below this are treated as invalid
    //   (NULL dereference region). Matches Linux's
    //   /proc/sys/vm/mmap_min_addr (default 4096 = PAGE_SIZE).
    static constexpr size_t MAX_TOTAL_PAGES = 1ULL * 1024 * 1024; // 4 GiB
    static constexpr uint64_t MAX_MMAP_LENGTH = 4ULL * 1024 * 1024 * 1024;
    static constexpr uint64_t NULL_PAGE_LIMIT = PAGE_SIZE;
    // Guest heap + stack live INSIDE the 4 GiB direct window so the JIT
    // fast path (direct window memcpy) covers them. The layout formerly
    // put the heap at 0x5000000000 and the stack at 0x8000000000 — both
    // ABOVE the window — so every heap/stack access went through the
    // pages_ + rwlock slow path, which dominated the profile (the voxel
    // game ran ~2 MIPS with the JIT). The ELF image + brk stay in the
    // low region (they always were). Heap range 256..768 MiB, stack ends
    // at 1008 MiB (spans 944..1008 MiB) — no overlap, ~176 MiB headroom
    // below the stack if the heap outgrows MMAP_BASE_MAX.
    static constexpr uint64_t MMAP_BASE_MIN = 0x10000000ULL;     // 256 MiB
    static constexpr uint64_t MMAP_BASE_MAX = 0x30000000ULL;     // 768 MiB
    static constexpr uint64_t STACK_TOP = 0x3F000000ULL;         // 1008 MiB
    static constexpr uint64_t STACK_SIZE = 64 * 1024 * 1024;   // 64 MiB
    Memory();
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    // ── Page cache (per-thread, lock-free) ────────────────────────────
    // Each CPU keeps its own PageCache so the hot path avoids the page
    // map mutex. The cache stores a raw pointer into the vector's data;
    // this is stable because std::vector<uint8_t> objects inside the
    // unordered_map are heap-allocated and don't move when the map rehashes.
    struct PageCache {
        // UINT64_MAX = sentinel "no cached page" — can never collide with
        // a real page number because that would require a guest address
        // near 2^64 * 4096 (overflow).
        uint64_t       read_page  = UINT64_MAX;
        const uint8_t* read_ptr   = nullptr;
        uint64_t       write_page = UINT64_MAX;
        uint8_t*       write_ptr  = nullptr;
    };
    // ── Mapping ───────────────────────────────────────────────────────
    void map_range(uint64_t addr, uint64_t size);
    bool is_mapped(uint64_t addr, uint64_t size) const;
    // ── Bulk read/write ───────────────────────────────────────────────
    void write(uint64_t addr, const void* src, size_t n, PageCache* pc = nullptr);
    void read(uint64_t addr, void* dst, size_t n, PageCache* pc = nullptr) const;
    // Convenience templates for fixed-width LE access.
    template<typename T> T load(uint64_t addr) const {
        T v; read(addr, &v, sizeof(T)); return v;
    }
    template<typename T> T load(uint64_t addr, PageCache* pc) const {
        T v; read(addr, &v, sizeof(T), pc); return v;
    }
    template<typename T> void store(uint64_t addr, T v) {
        write(addr, &v, sizeof(T));
    }
    template<typename T> void store(uint64_t addr, T v, PageCache* pc) {
        write(addr, &v, sizeof(T), pc);
    }
    uint32_t fetch_inst(uint64_t addr) const { return load<uint32_t>(addr); }
    uint32_t fetch_inst(uint64_t addr, PageCache* pc) const { return load<uint32_t>(addr, pc); }
    // ── Allocators (bump + grow) ──────────────────────────────────────
    // Allocate a chunk of fresh memory; returns starting address.
    // When `hint` is non-zero, the allocation is placed at exactly `hint`
    // (MAP_FIXED semantic). Existing pages at that address are REPLACED
    // with fresh zeroed pages — matches Linux kernel behavior.
    uint64_t mmap_alloc(uint64_t size, uint64_t hint = 0);
    // Grow (or shrink) an allocation. When growth would collide with
    // another tracked allocation, a fresh region is allocated and the
    // data is copied (mirrors musl's mremap contract).
    uint64_t mremap_grow(uint64_t old_addr, uint64_t old_size, uint64_t new_size);
    // Remove an allocation from tracking AND reclaim its pages + address
    // range so a future mmap_alloc can reuse it (Linux munmap semantics).
    void untrack_allocation(uint64_t addr, uint64_t size);
    // ── Atomics ───────────────────────────────────────────────────────
    // Used by LSE atomics (CAS) and futex. Returns true if swapped.
    bool atomic_cas_32(uint64_t addr, uint32_t expected, uint32_t desired);
    bool atomic_cas_64(uint64_t addr, uint64_t expected, uint64_t desired);
    // ── Fixed-width accessors ─────────────────────────────────────────
    uint32_t load_32(uint64_t addr) const { return load<uint32_t>(addr); }
    uint64_t load_64(uint64_t addr) const { return load<uint64_t>(addr); }
    void store_32(uint64_t addr, uint32_t v) { store<uint32_t>(addr, v); }
    void store_64(uint64_t addr, uint64_t v) { store<uint64_t>(addr, v); }
    // ── Diagnostics ───────────────────────────────────────────────────
    size_t page_count() const;
    const std::unordered_map<uint64_t, std::vector<uint8_t>>& pages_map_public() const {
        return pages_;
    }
    // Snapshot of currently-tracked allocations (start → page-aligned size).
    // Used by /proc/self/maps to produce a real memory layout instead of
    // a hardcoded one. Returns a copy under the lock so callers can iterate
    // without holding the mutex.
    std::vector<std::pair<uint64_t, uint64_t>> allocations_snapshot() const {
        std::shared_lock<std::shared_mutex> g(mu_);
        std::vector<std::pair<uint64_t, uint64_t>> out;
        out.reserve(allocations_.size());
        for (const auto& kv : allocations_) {
            out.emplace_back(kv.first, kv.second);
        }
        return out;
    }
    // ── Fork support ──────────────────────────────────────────────────
    // Create a deep copy of this Memory object for fork(). The new
    // Memory has its own direct window and pages_ map, with all
    // mapped pages copied. This is O(total_mapped_size) — for typical
    // guests (~64 MB), it takes a few milliseconds.
    //
    // The returned Memory is independent: writes in the child do not
    // affect the parent, and vice versa. This is "copy-on-write" done
    // eagerly — simpler than true CoW but correct.
    std::unique_ptr<Memory> clone_for_fork() const;
    // Snapshot all mapped pages into a flat list of (addr, data) pairs.
    // Used by clone_for_fork() and for debugging. Addresses < 4 GiB
    // come from the direct window; addresses >= 4 GiB come from pages_.
    struct PageSnapshot {
        uint64_t addr;
        std::vector<uint8_t> data;
    };
    std::vector<PageSnapshot> snapshot_pages() const;
    // ── Direct-access window for JIT ──────────────────────────────────
    // A large mmap'd region that mirrors guest pages at their native
    // addresses. The JIT can do `mov rax, [window_base + guest_addr]`
    // directly — no function call, no push/pop, no stack alloc.
    //
    // The window covers the low 4 GiB of the guest address space. Guest
    // pages in this range are stored DIRECTLY in the window (not in
    // pages_). The interpreter's read()/write() also use the window for
    // addresses < 4 GiB, so there's no sync issue — the window IS the
    // storage. pages_ is only used for addresses ≥ 4 GiB (stack, high
    // mmap region).
    static constexpr uint64_t DIRECT_WINDOW_SIZE = 4ULL * 1024 * 1024 * 1024;  // 4 GiB
    uint8_t* direct_window_ = nullptr;
    uint8_t* direct_window() const { return direct_window_; }
    bool in_direct_window(uint64_t addr) const {
        return direct_window_ && addr < DIRECT_WINDOW_SIZE;
    }
    // 1.5.2-alpha: translate a guest address to a host pointer.
    // Used by the graphic/audio/display thunks to pass pointer arguments
    // to host GL/EGL/SDL2/ALSA functions. Returns nullptr if the address
    // is not in the direct window (addresses ≥ 4 GiB can't be directly
    // accessed — the thunk should copy the data to a buffer in the low
    // 4 GiB region first).
    //
    // For addresses in the direct window, this is a simple pointer
    // arithmetic: host_ptr = direct_window_ + guest_addr.
    uint8_t* guest_to_host_ptr(uint64_t guest_addr) const {
        if (in_direct_window(guest_addr)) {
            return direct_window_ + guest_addr;
        }
        return nullptr;
    }
private:
    // Use shared_mutex for reader-writer locking.
    mutable std::shared_mutex mu_;
    mutable std::unordered_map<uint64_t, std::vector<uint8_t>> pages_;
    std::unordered_map<uint64_t, uint64_t> allocations_;
    // Reclaimed (munmap'd) address ranges available for reuse, keyed by
    // start address → size. Kept coalesced/merged so mmap_alloc can do a
    // first-fit scan and hand back the same guest addresses the guest
    // freed (critical: a bump-only allocator marches the guest heap past
    // the 4 GiB direct window and exhausts MAX_TOTAL_PAGES after enough
    // malloc/free churn, then mmap starts failing and musl mallocng
    // silently builds its arena at address 0 — BRK #1000 in get_meta).
    std::map<uint64_t, uint64_t> free_ranges_;
    // 1.5.2-alpha: ASLR for mmap base. Randomized at construction time
    // using /dev/urandom (not rand — must be unpredictable to prevent
    // guest-side info leaks). The base is page-aligned and within the
    // low heap region (MMAP_BASE_MIN - MMAP_BASE_MAX, inside the window).
    uint64_t mmap_next_ = 0;
    // 1.5.2-alpha: Total page count for OOM protection. Tracked
    // incrementally (incremented on page allocation, decremented on
    // munmap) to avoid O(pages_.size()) scans on the hot path.
    // Mutable because read() (a const method) auto-allocates pages.
    mutable std::atomic<size_t> total_pages_{0};
    // 1.5.2-alpha: Validate that an address range doesn't overlap
    // kernel space or the NULL page region. Returns true if the range
    // is valid for guest allocation.
    bool is_valid_guest_range(uint64_t addr, uint64_t size) const;
    // 1.5.2-alpha: Check page count against MAX_TOTAL_PAGES.
    // Returns true if the allocation would exceed the limit.
    bool would_exceed_page_limit(size_t num_pages) const;
    // 1.5.2-alpha: Add an address range to the free list, merging it with
    // any adjacent free ranges (kept sorted by start address).
    void add_free_range(uint64_t addr, uint64_t size);
    // 1.5.2-alpha: Remove an address range from the free list (used when
    // a MAP_FIXED allocation lands on top of reclaimed space).
    void remove_free_range(uint64_t addr, uint64_t size);
};
} // namespace arm64emu
