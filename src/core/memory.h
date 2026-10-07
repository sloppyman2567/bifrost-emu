// core/memory.h — guest memory as one host reservation (thread-safe).
//
// The guest address space is a single host mapping reserved at startup:
// guest VA x lives at host address window + x (the QEMU guest_base / FEX
// layout). Host mmap(MAP_FIXED), mprotect, madvise and munmap on that
// reservation do the real work: anonymous memory, MAP_SHARED file aliases,
// page permissions and freeing. The JIT and interpreter access guest memory
// as `[window + addr]`.
//
// The reservation is as large as the host allows, from 64 TiB down to
// 512 GiB (the emulator's own fixed guest addresses reach 448 GiB). Guest
// addresses beyond it cannot be mapped: MAP_FIXED fails with ENOMEM and
// hints fall back to normal placement.
//
// Bookkeeping (taken under mu_): `vmas_` is the coalesced mapping index
// (start → end + prot), `allocations_` the mmap'd regions for munmap/
// mremap and /proc/self/maps, `free_ranges_` the reusable address ranges.
// `direct_page_flags_` mirrors the mapping state per page, lock-free, for
// the interpreter's access check and the JIT memory guard; it is a lazily
// committed host mapping, so only pages near guest mappings use memory.
//
// The guest heap and the main stack stay in the low 4 GiB (MMAP_BASE_MIN,
// STACK_TOP). Above LOW_REGION_SIZE, freed memory is returned to the host;
// in the low region it stays resident and is zeroed on reuse, which is
// faster for guests that churn large buffers every frame.
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
#include <unordered_set>
#include <vector>
namespace arm64emu {
class Memory {
public:
    static constexpr uint64_t PAGE_SIZE = 4096;
    static constexpr uint64_t PAGE_MASK = PAGE_SIZE - 1;
    // MAX_MMAP_LENGTH: per-allocation cap for reserving mappings (4 GiB).
    // MAP_NORESERVE reservations are exempt, as on Linux they cost nothing
    // until touched.
    // NULL_PAGE_LIMIT: addresses below this are never mapped (Linux
    // /proc/sys/vm/mmap_min_addr).
    static constexpr uint64_t MAX_MMAP_LENGTH = 4ULL * 1024 * 1024 * 1024;
    static constexpr uint64_t NULL_PAGE_LIMIT = PAGE_SIZE;
    static constexpr uint8_t GUEST_PROT_READ = 1;
    static constexpr uint8_t GUEST_PROT_WRITE = 2;
    static constexpr uint8_t GUEST_PROT_EXEC = 4;
    static constexpr uint8_t GUEST_PAGE_MAPPED = 0x80;
    // userfaultfd (SIGBUS mode) per-page state in direct_page_flags_.
    // MISSING: no content since MADV_DONTNEED or an MREMAP_DONTUNMAP move
    // (tracked once a userfaultfd exists); the JIT memory guard rejects it,
    // so first touch reaches check_access. REGISTERED: covered by a
    // UFFDIO_REGISTER_MODE_MISSING range. Touching MISSING|REGISTERED is
    // SIGBUS BUS_ADRERR; touching an unregistered MISSING page populates it.
    static constexpr uint8_t GUEST_PAGE_UFFD_MISSING = 0x10;
    static constexpr uint8_t GUEST_PAGE_UFFD_REGISTERED = 0x20;
    static constexpr uint8_t GUEST_PAGE_UFFD_BITS =
        GUEST_PAGE_UFFD_MISSING | GUEST_PAGE_UFFD_REGISTERED;
    // The low 4 GiB holds the ELF image, brk, the mmap heap (256..768 MiB)
    // and the main stack (944..1008 MiB). Allocations that do not fit there
    // continue above LOW_REGION_SIZE. Android (set_low_hints_reserved)
    // sends large unhinted maps above it, keeping low hints free for ART.
    static constexpr uint64_t LOW_REGION_SIZE = 4ULL * 1024 * 1024 * 1024;
    static constexpr uint64_t MMAP_BASE_MIN = 0x10000000ULL;     // 256 MiB
    static constexpr uint64_t MMAP_BASE_MAX = 0x30000000ULL;     // 768 MiB
    static constexpr uint64_t STACK_TOP = 0x3F000000ULL;         // 1008 MiB
    static constexpr uint64_t STACK_SIZE = 64 * 1024 * 1024;   // 64 MiB
    // Reservation sizes tried at startup, largest first.
    static constexpr uint64_t WINDOW_SIZE_MAX = 1ULL << 46;  // 64 TiB
    static constexpr uint64_t WINDOW_SIZE_MIN = 1ULL << 39;  // 512 GiB
    // ASLR: per-process randomized load bias for ET_DYN executables and
    // jittered stack top. The PIE band ends well below MMAP_BASE_MIN so the
    // brk arena keeps ≥124 MiB of headroom. ET_EXEC loads at its preferred
    // vaddr (bias 0), matching kernel behavior.
    static constexpr uint64_t PIE_BASE_MIN = 0x400000ULL;
    static constexpr uint64_t PIE_JITTER = 128ull * 1024 * 1024;
    static constexpr uint64_t STACK_JITTER = 16ull * 1024 * 1024;
    Memory(bool start_reporter = true);
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    // Effective (possibly randomized) ET_DYN load bias and main-stack
    // top. Constant across execve within one process (we reload the ELF
    // into the same Memory); BIFROST_NO_ASLR=1 pins both.
    uint64_t pie_base() const { return pie_base_; }
    uint64_t stack_top() const { return stack_top_; }
    static bool aslr_disabled();
    // Configure once before starting any guest CPU. Android/Bionic uses
    // top-byte tags for internal allocator pointers even without MTE.
    void set_data_top_byte_ignore(bool enabled) { data_top_byte_ignore_ = enabled; }
    // Android profile: unhinted mappings that do not fit below the main
    // stack go above LOW_REGION_SIZE instead of filling the low 4 GiB.
    // Linux places them top-down, high; ART and Bionic then expect low
    // hints (the boot image near 0x70000000, low-4GB heap) to be free.
    void set_low_hints_reserved(bool enabled) { low_hints_reserved_ = enabled; }
    uint64_t data_address(uint64_t addr) const {
        return data_top_byte_ignore_ ? addr & 0x00FFFFFFFFFFFFFFULL : addr;
    }
    // ── Mapping ───────────────────────────────────────────────────────
    // Map [addr, addr+size) read/write (ELF segments, brk, stacks).
    // Overlapping pages receive the union of their permissions.
    bool map_range(uint64_t addr, uint64_t size);
    bool is_mapped(uint64_t addr, uint64_t size) const;
    bool mprotect_guest(uint64_t addr, uint64_t size, uint8_t prot,
                        int* error = nullptr);
    // Install a read-only shared device backing over an existing mapping.
    bool map_device_readonly(uint64_t addr, uint64_t size, int host_fd);
    // Replace existing storage with coherent file aliases. Returns a host errno.
    int map_shared_file(uint64_t addr, uint64_t size, int fd, uint64_t offset,
                        uint8_t prot);
    int sync_shared(uint64_t addr, uint64_t size, int flags);
    // ── Bulk read/write ───────────────────────────────────────────────
    void write(uint64_t addr, const void* src, size_t n);
    // Validate a whole thunk output range before a host call has side effects.
    void check_access(uint64_t addr, size_t size, uint8_t access) const;
    void read(uint64_t addr, void* dst, size_t n) const;
    // BIFROST_WATCH support: interpreter snapshots its pc here so the
    // watch check in write() can attribute host+guest stores.
    static void note_interp_pc(uint64_t pc);
    // Convenience templates for fixed-width LE access.
    template<typename T> T load(uint64_t addr) const {
        T v; read(addr, &v, sizeof(T)); return v;
    }
    template<typename T> void store(uint64_t addr, T v) {
        write(addr, &v, sizeof(T));
    }
    uint32_t fetch_inst(uint64_t addr) const {
        uint32_t v; read_access(addr, &v, sizeof(v), GUEST_PROT_EXEC); return v;
    }
    // ── Allocators ────────────────────────────────────────────────────
    // Allocate fresh zeroed memory; returns its address, or 0 on failure.
    // A non-zero `hint` places it at exactly `hint` (MAP_FIXED), replacing
    // existing pages with zeroed ones, as Linux does.
    // direct_only selects an unmapped heap hole in the low region under
    // mu_, ignoring hints (Binder receive arenas).
    uint64_t mmap_alloc(uint64_t size, uint64_t hint = 0,
                        bool noreserve = false,
                        uint8_t prot = GUEST_PROT_READ | GUEST_PROT_WRITE |
                                       GUEST_PROT_EXEC,
                        bool direct_only = false);
    // Atomic MAP_FIXED_NOREPLACE: checks all guest mappings and reserves
    // the exact range under mu_. Returns UINT64_MAX for overlap, 0 for
    // allocation/validation failure, or the mapped address on success.
    uint64_t mmap_fixed_noreplace(uint64_t addr, uint64_t size,
                                  bool noreserve = false,
                                  uint8_t prot = GUEST_PROT_READ | GUEST_PROT_WRITE);
    // Ordinary hint: try this address, then the next free low-region gap.
    // Never replaces mappings; returns 0 if the caller should use its
    // default allocator. Check and reservation share the same lock.
    uint64_t mmap_hint(uint64_t addr, uint64_t size, bool noreserve,
                       uint8_t prot);
    // Atomic MAP_FIXED replace: evict overlapping allocations, restore host
    // prot on reclaimed guard pages, reclaim free ranges, and track the new
    // mapping under one mu_ hold (no untrack/alloc gap for racing mmaps).
    // zero_contents=false preserves bytes (PROT_NONE guard / musl brk
    // metadata); true gives fresh-zero anonymous semantics. Returns the
    // address on success, 0 on failure (caller maps to -ENOMEM).
    uint64_t mmap_fixed_replace(uint64_t addr, uint64_t size, bool noreserve,
                                bool zero_contents,
                                uint8_t prot = GUEST_PROT_READ | GUEST_PROT_WRITE);
    // Resize an allocation, preserving bytes and per-page permissions.
    // Relocation requires may_move; fixed_addr requests an exact destination.
    uint64_t mremap_grow(uint64_t old_addr, uint64_t old_size, uint64_t new_size,
                         bool may_move = true, uint64_t fixed_addr = 0);
    // munmap: unmap [addr, addr+size), split partially covered allocations
    // and return the freed ranges to free_ranges_ for reuse.
    void untrack_allocation(uint64_t addr, uint64_t size);
    // Reset the allocation cursors after execve: the old image's heap is
    // gone (its ranges are in free_ranges_), so new mmaps start from a
    // fresh-process layout instead of the previous high-water mark.
    void reset_allocator_cursors() {
        std::unique_lock<std::shared_mutex> g(mu_);
        mmap_next_ = MMAP_BASE_MIN;
        high_next_ = LOW_REGION_SIZE;
    }
    // MADV_DONTNEED: discard covered pages without unmapping them.
    // Anonymous pages read back as zero; shared file pages refault from
    // the file. mimalloc reuses decommitted segments as "initially zero".
    void madvise_dontneed(uint64_t addr, uint64_t len);
    // MADV_REMOVE (mm/madvise.c madvise_remove): punch a hole in the file
    // behind a writable shared mapping, so the pages read back as zero.
    // Pages are visited in address order and the first failure stops:
    // EINVAL for storage with no file (private anonymous), EACCES for a
    // shared mapping of a read-only fd. Unmapped holes give ENOMEM once
    // the rest is done. addr is page-aligned and len page-rounded; returns
    // 0 or a host errno. ART's CMC arena release relies on EINVAL for
    // private arenas: success makes it skip zeroing them, and stale bytes
    // become live DexCache entries.
    int madvise_remove(uint64_t addr, uint64_t len);
    // ── userfaultfd (SIGBUS mode) ─────────────────────────────────────
    // Missing-page tracking starts with the first userfaultfd and stays on.
    // The state lives in direct_page_flags_ for every guest page.
    void enable_uffd_tracking() {
        jit_software_checks_.store(true, std::memory_order_release);
        uffd_tracking_.store(true, std::memory_order_release);
    }
    // Sticky fallback for state the host MMU cannot represent exactly:
    // userfaultfd-missing pages and readable host aliases of guest W/X-only pages.
    const std::atomic<bool>* jit_software_checks() const { return &jit_software_checks_; }
    // UFFDIO_REGISTER/UNREGISTER of [addr, addr+len): the range is already
    // validated by the caller. Only mapped private pages are covered;
    // returns -EINVAL if none is, or if a shared page is included.
    int uffd_register(uint64_t addr, uint64_t len, bool registered);
    // A forked child inherits no registration (no UFFD_FEATURE_EVENT_FORK);
    // its missing pages become ordinary zero pages.
    void uffd_drop_registrations();
    // UFFDIO_COPY (src != nullptr) / UFFDIO_ZEROPAGE (src == nullptr) of
    // whole pages. Returns bytes filled, or -ENOENT when [dst, dst+len) is
    // not one registered range, or -EEXIST at the first present page.
    int64_t uffd_fill(uint64_t dst, const uint8_t* src, uint64_t len);
    // MREMAP_DONTUNMAP: move [old_addr, +len) to fixed_addr (or a new
    // range when 0). The old range stays mapped with its permissions and
    // registration, but empty. Returns the new address or -errno.
    int64_t mremap_dontunmap(uint64_t old_addr, uint64_t len, uint64_t fixed_addr);
    // Isolated callback-stack allocation: usable_size bytes with a
    // PROT_NONE guard page on BOTH sides, placed like a normal mmap_alloc
    // block. Host-thread guest callbacks (audio pump, GLFW / SDL / Android
    // runners) execute on these stacks; an overflow hits the guard and
    // faults loudly instead of shredding neighboring malloc chunks.
    // Returns the usable base; the caller sets sp = base + usable_size.
    uint64_t mmap_alloc_callback_stack(uint64_t usable_size);
    // Exact-address lookup in the allocation map (start → page-aligned
    // size). Returns {false, 0} when `addr` is not a tracked allocation
    // start.
    std::pair<bool, uint64_t> find_allocation(uint64_t addr) const {
        std::shared_lock<std::shared_mutex> g(mu_);
        auto it = allocations_.find(addr);
        if (it == allocations_.end()) return {false, 0};
        return {true, it->second};
    }
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
    // Number of mapped guest pages.
    size_t page_count() const;
    // Snapshot of currently-tracked allocations (start → page-aligned size),
    // copied under the lock (/proc/self/maps).
    std::vector<std::pair<uint64_t, uint64_t>> allocations_snapshot() const {
        std::shared_lock<std::shared_mutex> g(mu_);
        std::vector<std::pair<uint64_t, uint64_t>> out;
        out.reserve(allocations_.size());
        for (const auto& kv : allocations_) {
            out.emplace_back(kv.first, kv.second);
        }
        return out;
    }
    // ── The reservation ───────────────────────────────────────────────
    // The JIT addresses guest memory as `[window_base + guest_addr]` after
    // checking guest_addr + width <= window_size().
    uint8_t* direct_window() const { return direct_window_; }
    uint64_t window_size() const { return window_size_; }
    const std::atomic<uint8_t>* direct_page_flags() const {
        return direct_page_flags_;
    }
    bool in_direct_window(uint64_t addr) const { return addr < window_size_; }
    // Translate a guest range to a host pointer, or nullptr when any byte
    // of it is not accessible with `access` (thunk pointer arguments).
    uint8_t* guest_to_host_ptr(uint64_t guest_addr,
                               uint8_t access = GUEST_PROT_READ,
                               size_t size = 1) const {
        if (!(access & GUEST_PROT_EXEC)) guest_addr = data_address(guest_addr);
        if (size && guest_addr < window_size_ &&
            size <= window_size_ - guest_addr &&
            range_accessible(guest_addr, size, access)) {
            return direct_window_ + guest_addr;
        }
        return nullptr;
    }
private:
    bool data_top_byte_ignore_ = false;
    bool low_hints_reserved_ = false;
    static constexpr uint8_t PAGE_MAPPED = GUEST_PAGE_MAPPED;
    uint8_t* direct_window_ = nullptr;
    uint64_t window_size_ = 0;
    std::atomic<uint8_t>* direct_page_flags_ = nullptr;
    struct Vma {
        uint64_t end = 0;
        uint8_t prot = 0;
    };
    bool range_mapped_locked(uint64_t lo, uint64_t hi) const;
    bool range_overlaps_locked(uint64_t lo, uint64_t hi) const;
    uint64_t mmap_noreplace(uint64_t addr, uint64_t size, bool noreserve,
                            uint8_t prot, bool ordinary_hint);
    bool mapping_prot_at_locked(uint64_t addr, uint8_t& prot) const;
    bool range_accessible(uint64_t addr, size_t size, uint8_t access) const;
    void split_vma_locked(uint64_t at);
    void erase_vma_locked(uint64_t lo, uint64_t hi);
    // First page-aligned address >= from where `size` bytes are unmapped
    // and end at or below `limit`; 0 if there is none.
    uint64_t find_gap_locked(uint64_t from, uint64_t limit, uint64_t size) const;
    // Record [lo, hi) as mapped with `prot` (or OR it into existing pages
    // when `combine`), update the page flags and the host protection.
    // Page flags carry PAGE_MAPPED only for pages with some access, so a
    // huge PROT_NONE reservation costs no flag writes; vmas_ still records
    // it. keep_uffd preserves the userfaultfd bits (mprotect).
    void set_mapping_locked(uint64_t lo, uint64_t hi, uint8_t prot,
                            bool combine, bool keep_uffd = false);
    // Unmap [lo, hi): flags cleared, host PROT_NONE, memory released above
    // the low region.
    void clear_mapping_locked(uint64_t lo, uint64_t hi);
    // Fresh-zero anonymous contents for [lo, hi), which must be private.
    void zero_range_locked(uint64_t lo, uint64_t hi, bool noreserve);
    bool set_direct_prot(uint64_t lo, uint64_t hi, int prot) const;
    void read_access(uint64_t addr, void* dst, size_t n, uint8_t access) const;
    // Use shared_mutex for reader-writer locking.
    mutable std::shared_mutex mu_;
    // A MAP_SHARED file alias: the host maps the file straight into the
    // reservation; this keeps its fd for fallocate, msync and mremap.
    struct SharedBacking {
        uint64_t file_offset = 0;
        int fd = -1;
        bool writable = false;
        ~SharedBacking();
    };
    struct SharedPage {
        std::shared_ptr<SharedBacking> backing;
        uint64_t offset = 0;  // from the start of the original mapping
    };
    std::map<uint64_t, SharedPage> shared_pages_;  // keyed by page number
    const SharedPage* shared_page_locked(uint64_t addr) const;
    // Restore private anonymous storage over shared aliases in [lo, hi).
    void detach_shared_locked(uint64_t lo, uint64_t hi);
    std::map<uint64_t, Vma> vmas_;
    std::unordered_map<uint64_t, uint64_t> allocations_;
    // Reclaimed (munmap'd) address ranges available for reuse, keyed by
    // start address → size. Kept coalesced so mmap_alloc can first-fit
    // and hand back the addresses the guest freed (a bump-only allocator
    // marched musl's heap until mmap failed and mallocng built its arena
    // at address 0).
    std::map<uint64_t, uint64_t> free_ranges_;
    std::atomic<bool> uffd_tracking_{false};
    std::atomic<bool> jit_software_checks_{false};
    // Allocation cursors: the low heap from MMAP_BASE_MIN (randomized), and
    // above the low region once it is full or for Android's large maps.
    uint64_t mmap_next_ = 0;
    uint64_t high_next_ = LOW_REGION_SIZE;
    // Per-process randomized ET_DYN load bias and main-stack top
    // (see pie_base()/stack_top() above).
    uint64_t pie_base_ = PIE_BASE_MIN;
    uint64_t stack_top_ = STACK_TOP;
    // Page-aligned random offset in [0, range) from /dev/urandom; falls
    // back to host stack-address entropy, then 0 (fixed layout).
    static uint64_t random_offset(uint64_t range);
    // BIFROST_MEMSTATS=N: background leak reporter (see memory.cpp).
    void memstats_reporter(uint64_t period_secs);
    // Validate that a range lies inside the reservation and above the
    // NULL page, without wrapping.
    bool is_valid_guest_range(uint64_t addr, uint64_t size) const;
    // Guard pages for callback scratch stacks (PROT_NONE). Host code must
    // never touch them via memset/memcpy (madvise, reuse zeroing).
    std::unordered_set<uint64_t> guard_pages_;
    void add_free_range(uint64_t addr, uint64_t size);
    void remove_free_range(uint64_t addr, uint64_t size);
};
} // namespace arm64emu
