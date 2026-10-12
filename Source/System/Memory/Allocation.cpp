// Allocation.cpp — guest address-space placement: mmap_alloc and its fixed/
// hint/noreplace variants, callback stacks, mremap growth, unmapping
// (untrack_allocation) and the free-range index used to reuse holes.
//
// All bookkeeping runs under Memory::mu_; see Memory.cpp for the
// reservation layout and mapping index.
#include "System/Memory/Memory.hpp"
#include "System/Memory/MemoryInternal.hpp"
#include "System/Core/DebugFlags.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <shared_mutex>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
namespace arm64emu {
// ── Allocation ─────────────────────────────────────────────────────────
uint64_t Memory::mmap_alloc(uint64_t size, uint64_t hint, bool noreserve,
                            uint8_t prot, bool direct_only) {
    // BIFROST_ALLOC_TRACE=1: print every mmap_alloc (addr,size) — used to
    // audit placement/overlap of thunk bounces vs callback scratch stacks.
    static const bool alloc_trace_ = getenv("BIFROST_ALLOC_TRACE") != nullptr;
    if (alloc_trace_)
        fprintf(stderr, "[alloc] mmap_alloc(%zu) ...", (size_t)size);
    if (size == 0) size = PAGE_SIZE;
    // Per-allocation length cap. MAP_NORESERVE reservations are exempt:
    // they cost nothing until touched (vkQuake's mimalloc falls through an
    // 8 GiB → 4 → 2 → 1 GiB arena cascade when they are refused).
    if (!noreserve && size > MAX_MMAP_LENGTH) return 0;  // caller maps 0 to -ENOMEM
    if (size > UINT64_MAX - PAGE_MASK) return 0;
    std::unique_lock<std::shared_mutex> g(mu_);
    uint64_t base = hint;
    const uint64_t aligned_size = (size + PAGE_MASK) & ~PAGE_MASK;
    const uint64_t stack_bottom = stack_top_ - STACK_SIZE;
    bool reused = false;
    if (direct_only) {
        // Binder receive arenas: an unmapped hole in the low heap region,
        // found under the same lock as the allocation. PROT_NONE
        // reservations count as occupied.
        const uint64_t limit = std::min<uint64_t>(LOW_REGION_SIZE, stack_bottom);
        base = find_gap_locked(MMAP_BASE_MIN, limit, aligned_size);
        if (!base) return 0;
        reused = true;  // consume overlapping free ranges and zero backing
    }
    if (base == 0) {
        // Reuse a reclaimed (munmap'd) range first instead of always
        // bumping, so the guest heap stays clustered (the minecraft game's
        // per-frame mesh buffers otherwise marched the heap until mmap
        // failed and musl built its arena at address 0).
        for (auto it = free_ranges_.begin(); it != free_ranges_.end(); ++it) {
            if (it->second >= aligned_size) {
                // Never hand out a range that overlaps the main stack: a
                // "heap" segment there let mimalloc's page zeroing wipe
                // live stack frames (vkQuake).
                if (it->first < stack_top_ &&
                    it->first + it->second > stack_bottom)
                    continue;
                base = it->first;
                reused = true;
                break;
            }
        }
        if (!reused) {
            base = mmap_next_;
            mmap_next_ += aligned_size;
            // Skip over the main-stack band like the kernel picks a free
            // area (a vkQuake mimalloc arena marched into it and its
            // zeroing destroyed live frames).
            if (base < stack_top_ && base + aligned_size > stack_bottom) {
                if (low_hints_reserved_) {
                    // Leave the low region above the stack to explicit hints.
                    mmap_next_ = base;
                    base = LOW_REGION_SIZE;
                } else {
                    base = stack_top_;
                    mmap_next_ = stack_top_ + aligned_size;
                }
            }
            // Past the low region (enormous heaps, or Android's large
            // unhinted maps) allocation continues from the high cursor.
            if (base + aligned_size > LOW_REGION_SIZE) {
                base = std::max<uint64_t>(LOW_REGION_SIZE, high_next_);
                if (aligned_size > window_size_ - base) return 0;
                high_next_ = base + aligned_size;
            }
        }
    } else {
        if (!is_valid_guest_range(base, aligned_size)) return 0;
        mmap_next_ = std::max(mmap_next_, base + aligned_size);
        // A fixed mapping above the low region also advances the high
        // cursor, or a later mmap_alloc(NULL,…) could overlap it.
        if (base + aligned_size > LOW_REGION_SIZE)
            high_next_ = std::max(high_next_, base + aligned_size);
    }
    // BIFROST_MEMGUARD=1: scream if the chosen range overlaps an allocation
    // still tracked as live — free_ranges_ must never contain live space,
    // so a hit means accounting drift (untrack/mremap) and explains a
    // "zero-and-rewrite of live guest heap" corruption.
    static const bool memguard_ = getenv("BIFROST_MEMGUARD") != nullptr;
    if (memguard_) {
        const uint64_t glo = base, ghi = base + aligned_size;
        for (const auto& kv : allocations_) {
            if (kv.first < ghi && kv.first + kv.second > glo) {
                fprintf(stderr,
                        "[memguard] LIVE-OVERLAP new=[0x%llx..0x%llx) "
                        "live=[0x%llx..0x%llx)\n",
                        (unsigned long long)glo, (unsigned long long)ghi,
                        (unsigned long long)kv.first,
                        (unsigned long long)(kv.first + kv.second));
            }
        }
    }
    // Consume the taken free range (leave the tail for later reuse). A
    // MAP_FIXED allocation reclaims any free range it overlaps.
    if (reused || hint != 0) remove_free_range(base, aligned_size);
    const uint64_t end = base + aligned_size;
    // Reused or fixed pages may hold a previous owner's data. Fresh bump
    // space was never touched and is zero.
    if (reused || hint != 0) {
        detach_file_locked(base, end);
        zero_range_locked(base, end, noreserve);
    }
    // BIFROST_GUARD_ALLOCS=1: append a PROT_NONE guard page right after
    // each bump allocation so an out-of-bounds guest access faults at the
    // offending store instead of silently shredding the next chunk. Only
    // anonymous bump allocations are guarded: reused ranges, MAP_FIXED
    // hints and MAP_NORESERVE mappings are left alone. The guard page is
    // never entered into allocations_/free_ranges_ — deliberately leaked for
    // the debug run so it can never be handed out while PROT_NONE.
    static const bool guard_allocs_ = getenv("BIFROST_GUARD_ALLOCS") != nullptr;
    if (guard_allocs_ && !reused && hint == 0 && !noreserve &&
        end + PAGE_SIZE <= window_size_) {
        const bool over_stack = end < stack_top_ && end + PAGE_SIZE > stack_bottom;
        if (!over_stack && !range_overlaps_locked(end, end + PAGE_SIZE)) {
            set_mapping_locked(end, end + PAGE_SIZE, 0, false);
            guard_pages_.insert(end / PAGE_SIZE);
            // Reserve the guard VA so the bump allocator never reuses it.
            if (end < LOW_REGION_SIZE && end + PAGE_SIZE > mmap_next_)
                mmap_next_ = end + PAGE_SIZE;
            else if (end >= LOW_REGION_SIZE && end + PAGE_SIZE > high_next_)
                high_next_ = end + PAGE_SIZE;
        }
    }
    allocations_[base] = aligned_size;
    set_mapping_locked(base, end, prot, false);
    g_memstats_cum_alloc.fetch_add(aligned_size, std::memory_order_relaxed);
    if (alloc_trace_)
        fprintf(stderr, " [0x%llx..0x%llx)\n",
                (unsigned long long)base, (unsigned long long)end);
    return base;
}
uint64_t Memory::mmap_fixed_noreplace(uint64_t addr, uint64_t size,
                                      bool noreserve, uint8_t prot) {
    return mmap_noreplace(addr, size, noreserve, prot, false);
}
uint64_t Memory::mmap_hint(uint64_t addr, uint64_t size, bool noreserve, uint8_t prot) {
    uint64_t result = mmap_noreplace(addr, size, noreserve, prot, true);
    return result == UINT64_MAX ? 0 : result;
}
uint64_t Memory::mmap_noreplace(uint64_t addr, uint64_t size, bool noreserve,
                               uint8_t prot, bool ordinary_hint) {
    if (size == 0 || (addr & PAGE_MASK) || size > UINT64_MAX - PAGE_MASK)
        return 0;
    const uint64_t aligned_size = (size + PAGE_MASK) & ~PAGE_MASK;
    if (!is_valid_guest_range(addr, aligned_size)) return 0;
    if (!noreserve && size > MAX_MMAP_LENGTH) return 0;
    std::unique_lock<std::shared_mutex> g(mu_);
    if (ordinary_hint && addr < LOW_REGION_SIZE) {
        // Keep a colliding low hint in the low region when possible. ART's
        // compressed-reference allocator otherwise probes one page at a
        // time, repeatedly allocating and discarding a high-address heap.
        if (aligned_size > LOW_REGION_SIZE - addr) return 0;
        addr = find_gap_locked(addr, LOW_REGION_SIZE, aligned_size);
        if (!addr) return 0;
    }
    const uint64_t end = addr + aligned_size;
    if (range_overlaps_locked(addr, end)) return UINT64_MAX;
    const uint64_t stack_bottom = stack_top_ - STACK_SIZE;
    if (addr < stack_top_ && end > stack_bottom) return 0;
    detach_file_locked(addr, end);
    zero_range_locked(addr, end, noreserve);
    allocations_[addr] = aligned_size;
    remove_free_range(addr, aligned_size);
    mmap_next_ = std::max(mmap_next_, end);
    if (end > LOW_REGION_SIZE) high_next_ = std::max(high_next_, end);
    set_mapping_locked(addr, end, prot, false);
    g_memstats_cum_alloc.fetch_add(aligned_size, std::memory_order_relaxed);
    return addr;
}
uint64_t Memory::mmap_fixed_replace(uint64_t addr, uint64_t size, bool noreserve,
                                    bool zero_contents, uint8_t prot) {
    if (size == 0) size = PAGE_SIZE;
    // Fixed mappings require a page-aligned address (Linux returns EINVAL
    // otherwise; the syscall entry checks this too).
    if (addr & PAGE_MASK) return 0;
    if (size > UINT64_MAX - PAGE_MASK) return 0;
    const uint64_t aligned_size = (size + PAGE_MASK) & ~PAGE_MASK;
    if (!is_valid_guest_range(addr, aligned_size)) return 0;
    if (!noreserve && size > MAX_MMAP_LENGTH) return 0;
    std::unique_lock<std::shared_mutex> g(mu_);
    // The main stack is a fixed map_range mapping, not an allocation. A
    // fixed mmap over it would create dual ownership and shred frames, so
    // refuse (caller maps 0 to -ENOMEM). Real Linux would replace it, but
    // corrupting the stack is worse than diverging here.
    const uint64_t stack_bottom = stack_top_ - STACK_SIZE;
    if (addr < stack_top_ && addr + aligned_size > stack_bottom) return 0;
    const uint64_t lo = addr;
    const uint64_t hi = addr + aligned_size;
    // VMA-split eviction of overlapping live allocations.
    std::vector<std::pair<uint64_t, uint64_t>> hits;
    for (const auto& kv : allocations_) {
        const uint64_t b = kv.first;
        const uint64_t e = b + kv.second;
        if (b < hi && e > lo) hits.push_back({b, kv.second});
    }
    for (const auto& h : hits) {
        const uint64_t b = h.first;
        const uint64_t e = b + h.second;
        // BIFROST_MAPFIX_TRACE: log every live allocation a MAP_FIXED
        // replaces (mimalloc commits/decommits arenas this way).
        static const bool mapfix_trace_ =
            getenv("BIFROST_MAPFIX_TRACE") != nullptr;
        if (mapfix_trace_)
            fprintf(stderr,
                    "[mapfix] fixed [0x%llx..0x%llx) %s evicts live "
                    "[0x%llx..0x%llx) len=%llu\n",
                    (unsigned long long)lo, (unsigned long long)hi,
                    zero_contents ? "zero" : "keep",
                    (unsigned long long)b, (unsigned long long)e,
                    (unsigned long long)h.second);
        allocations_.erase(b);
        const uint64_t flo = (lo > b) ? lo : b;
        const uint64_t fhi = (hi < e) ? hi : e;
        if (flo > b) allocations_[b] = flo - b;
        if (e > fhi) allocations_[fhi] = e - fhi;
        if (fhi > flo)
            g_memstats_cum_free.fetch_add(fhi - flo, std::memory_order_relaxed);
    }
    // Reclaimed callback-stack guard pages become ordinary pages again.
    for (auto it = guard_pages_.begin(); it != guard_pages_.end();) {
        const uint64_t at = *it * PAGE_SIZE;
        if (at >= lo && at < hi) it = guard_pages_.erase(it);
        else ++it;
    }
    detach_file_locked(lo, hi);
    remove_free_range(lo, aligned_size);
    if (zero_contents) zero_range_locked(lo, hi, noreserve);
    allocations_[lo] = aligned_size;
    // The replacement mapping is not userfaultfd-registered.
    set_mapping_locked(lo, hi, prot, false);
    mmap_next_ = std::max(mmap_next_, hi);
    if (hi > LOW_REGION_SIZE) high_next_ = std::max(high_next_, hi);
    g_memstats_cum_alloc.fetch_add(aligned_size, std::memory_order_relaxed);
    return lo;
}
// ── mmap_alloc_callback_stack — guarded scratch stack for host-thread ──
// guest callbacks. Layout: [GUARD][usable][GUARD]. An overflow faults
// immediately (guest SIGSEGV) instead of silently corrupting neighboring
// malloc chunks (the audio pump's callback once descended >72 KB through
// adjacent scratch stacks and shredded live chunks).
uint64_t Memory::mmap_alloc_callback_stack(uint64_t usable_size) {
    constexpr uint64_t P = PAGE_SIZE;
    uint64_t usable = (usable_size + P - 1) & ~(P - 1);
    uint64_t base = mmap_alloc(usable + 2 * P, 0, false,
                               GUEST_PROT_READ | GUEST_PROT_WRITE);
    if (base == 0) return 0;
    (void)mprotect_guest(base, P, 0);
    (void)mprotect_guest(base + P + usable, P, 0);
    std::unique_lock<std::shared_mutex> g(mu_);
    guard_pages_.insert(base / P);
    guard_pages_.insert((base + P + usable) / P);
    return base + P;  // usable base; sp starts at base + P + usable
}
uint64_t Memory::mremap_grow(uint64_t old_addr, uint64_t old_size, uint64_t new_size,
                             bool may_move, uint64_t fixed_addr) {
    if (old_addr == 0 || old_size == 0 || new_size == 0 ||
        (old_addr & PAGE_MASK) || (fixed_addr && !may_move)) return 0;
    if (old_size > MAX_MMAP_LENGTH || new_size > MAX_MMAP_LENGTH ||
        old_size > UINT64_MAX - PAGE_MASK ||
        new_size > UINT64_MAX - PAGE_MASK) return 0;
    uint64_t old_aligned = (old_size + PAGE_MASK) & ~PAGE_MASK;
    uint64_t new_aligned = (new_size + PAGE_MASK) & ~PAGE_MASK;
    uint64_t old_end;
    uint64_t new_end;
    if (__builtin_add_overflow(old_addr, old_aligned, &old_end) ||
        __builtin_add_overflow(old_addr, new_aligned, &new_end) ||
        !is_valid_guest_range(old_addr, fixed_addr ? old_aligned :
                              std::max(old_aligned, new_aligned)))
        return 0;
    if (fixed_addr && ((fixed_addr & PAGE_MASK) ||
        !is_valid_guest_range(fixed_addr, new_aligned) ||
        (old_addr < fixed_addr + new_aligned && fixed_addr < old_end))) return 0;
    // File mappings retain their backing when resized or moved. Shared
    // views stay coherent; private views also retain their dirty COW bytes.
    std::shared_ptr<FileBacking> backing;
    uint64_t file_offset = 0;
    uint64_t file_size = 0;
    std::vector<uint8_t> file_prots;
    {
        std::shared_lock<std::shared_mutex> lock(mu_);
        if (const FilePage* first = file_page_locked(old_addr)) {
            backing = first->backing;
            file_offset = first->offset;
        }
        if (backing) {
            auto allocation = allocations_.find(old_addr);
            if (allocation == allocations_.end() || allocation->second != old_aligned) return 0;
            struct stat original;
            if (::fstat(backing->fd, &original) < 0) return 0;
            file_size = static_cast<uint64_t>(original.st_size);
            for (uint64_t at = old_addr; at < old_end; at += PAGE_SIZE) {
                const FilePage* page = file_page_locked(at);
                uint8_t prot;
                if (!page ||
                    page->backing->shared != backing->shared ||
                    page->backing->file_offset + page->offset !=
                        backing->file_offset + file_offset + at - old_addr ||
                    !mapping_prot_at_locked(at, prot)) return 0;
                if (page->backing != backing) {
                    struct stat current;
                    if (::fstat(page->backing->fd, &current) < 0 ||
                        current.st_dev != original.st_dev || current.st_ino != original.st_ino)
                        return 0;
                }
                file_prots.push_back(prot);
            }
        }
    }
    if (backing && (fixed_addr || new_aligned > old_aligned)) {
        const uint64_t offset = backing->file_offset + file_offset;
        if (!fixed_addr) {
            uint64_t tail = mmap_hint(old_end, new_aligned - old_aligned, true,
                                      file_prots.back());
            if (tail == old_end) {
                int error = map_file(tail, new_aligned - old_aligned,
                    backing->fd, offset + old_aligned, file_prots.back(), backing->shared);
                if (!error) {
                    std::unique_lock<std::shared_mutex> lock(mu_);
                    allocations_.erase(tail);
                    allocations_[old_addr] = new_aligned;
                    return old_addr;
                }
            }
            if (tail) untrack_allocation(tail, new_aligned - old_aligned);
        }
        if (!may_move) return 0;
        uint64_t target = fixed_addr
            ? mmap_fixed_replace(fixed_addr, new_aligned, true, true, file_prots.back())
            : mmap_alloc(new_aligned, 0, true, file_prots.back());
        if (!target) return 0;
        const uint8_t target_prot = backing->shared ? file_prots.back()
            : GUEST_PROT_READ | GUEST_PROT_WRITE;
        if (map_file(target, new_aligned, backing->fd, offset, target_prot, backing->shared)) {
            untrack_allocation(target, new_aligned);
            return 0;
        }
        if (!backing->shared) {
            // Copy dirty private data into a new MAP_PRIVATE view, rather
            // than anonymous storage: MADV_DONTNEED must still refault the
            // original file. No writes reach the file, including with an
            // O_RDONLY descriptor. Restore each source protection below.
            // Whole pages beyond EOF have no readable contents (SIGBUS).
            // Moving such a mapping is legal; do not touch those pages.
            const uint64_t file_end = (file_size + PAGE_MASK) & ~PAGE_MASK;
            const uint64_t readable = file_end > offset ? file_end - offset : 0;
            const uint64_t copy_size = std::min({old_aligned, new_aligned, readable});
            std::unique_lock<std::shared_mutex> lock(mu_);
            (void)set_direct_prot(old_addr, old_addr + copy_size, PROT_READ | PROT_WRITE);
            std::memcpy(direct_window_ + target, direct_window_ + old_addr, copy_size);
            for (size_t i = 0; i * PAGE_SIZE < copy_size; ++i)
                (void)set_direct_prot(old_addr + i * PAGE_SIZE,
                    old_addr + (i + 1) * PAGE_SIZE, file_prots[i]);
        }
        for (size_t i = 0; i < file_prots.size() && i * PAGE_SIZE < new_aligned; ++i)
            (void)mprotect_guest(target + i * PAGE_SIZE, PAGE_SIZE, file_prots[i]);
        if (!backing->shared && new_aligned > old_aligned)
            (void)mprotect_guest(target + old_aligned, new_aligned - old_aligned,
                                  file_prots.back());
        untrack_allocation(old_addr, old_aligned);
        return target;
    }
    // Shrink: keep the same address and unmap the tail.
    if (!fixed_addr && new_aligned <= old_aligned) {
        std::unique_lock<std::shared_mutex> g(mu_);
        auto it = allocations_.find(old_addr);
        if (it == allocations_.end() || it->second != old_aligned) return 0;
        const uint64_t fstart = old_addr + new_aligned;
        const uint64_t fend = old_end;
        it->second = new_aligned;
        if (fstart < fend) {
            detach_file_locked(fstart, fend);
            clear_mapping_locked(fstart, fend);
            add_free_range(fstart, fend - fstart);
        }
        return old_addr;
    }
    // Grow: the collision check, the in-place extension and the cursor
    // updates happen under ONE unique lock, so a racing mmap_alloc cannot
    // claim the pages between the check and the extension (musl's mallocng
    // once received "fresh" pages that were still dirty from that race).
    const uint64_t extra_start = old_end;
    const uint64_t extra_end = new_end;
    bool can_grow_in_place = true;
    // The stack region is a fixed mapping, not allocator space.
    if (extra_start < stack_top_ && extra_end > stack_top_ - STACK_SIZE)
        can_grow_in_place = false;
    std::unique_lock<std::shared_mutex> g(mu_);
    auto allocation = allocations_.find(old_addr);
    // The old range must still be the tracked mapping described by
    // old_size; its page permissions are preserved by either path.
    if (allocation == allocations_.end() || allocation->second != old_aligned ||
        !range_mapped_locked(old_addr, old_end)) return 0;
    std::vector<uint8_t> old_page_prots;
    old_page_prots.reserve(old_aligned / PAGE_SIZE);
    for (uint64_t s = old_addr; s < old_end; s += PAGE_SIZE) {
        uint8_t page_prot = 0;
        if (!mapping_prot_at_locked(s, page_prot)) return 0;
        old_page_prots.push_back(page_prot);
    }
    // Any existing VMA, including map_range-only images/guards, blocks an
    // in-place extension. Allocation tracking alone misses those mappings.
    if (fixed_addr || range_overlaps_locked(extra_start, extra_end))
        can_grow_in_place = false;
    if (can_grow_in_place) {
        detach_file_locked(extra_start, extra_end);
        zero_range_locked(extra_start, extra_end, false);
        allocations_[old_addr] = new_aligned;
        set_mapping_locked(extra_start, extra_end, old_page_prots.back(), false);
        // Bump the cursors past the grown region, or a later
        // mmap_alloc(NULL,...) could return an address inside it.
        mmap_next_ = std::max(mmap_next_, extra_end);
        if (extra_end > LOW_REGION_SIZE) high_next_ = std::max(high_next_, extra_end);
        remove_free_range(extra_start, extra_end - extra_start);
        return old_addr;
    }
    if (!may_move) return 0;  // leave the original mapping untouched
    // Move: allocate a fresh region, copy, then unmap the old one. The
    // lock is released because mmap_alloc/mmap_fixed_replace take it.
    g.unlock();
    uint64_t new_addr = fixed_addr
        ? mmap_fixed_replace(fixed_addr, new_size, false, true,
                             GUEST_PROT_READ | GUEST_PROT_WRITE)
        : mmap_alloc(new_size, 0, false, GUEST_PROT_READ | GUEST_PROT_WRITE);
    if (new_addr == 0) return 0;  // oom: keep old mapping intact
    const uint64_t copy_size = std::min(old_aligned, new_aligned);
    {
        std::unique_lock<std::shared_mutex> copy_lock(mu_);
        // Read-only and PROT_NONE sources move without weakening their
        // guest permissions: host access is opened only for the copy.
        (void)set_direct_prot(old_addr, old_addr + copy_size, PROT_READ | PROT_WRITE);
        std::memcpy(direct_window_ + new_addr, direct_window_ + old_addr, copy_size);
        // Every old page keeps its protection; new tail pages inherit the
        // former last page's permissions.
        for (uint64_t i = 0; i < new_aligned / PAGE_SIZE;) {
            const uint8_t prot = old_page_prots[std::min<uint64_t>(
                i, old_page_prots.size() - 1)];
            uint64_t j = i + 1;
            while (j < new_aligned / PAGE_SIZE &&
                   old_page_prots[std::min<uint64_t>(j,
                       old_page_prots.size() - 1)] == prot) ++j;
            set_mapping_locked(new_addr + i * PAGE_SIZE,
                               new_addr + j * PAGE_SIZE, prot, false);
            i = j;
        }
    }
    untrack_allocation(old_addr, old_aligned);
    return new_addr;
}
void Memory::untrack_allocation(uint64_t addr, uint64_t size) {
    // munmap(addr, 0) is a no-op; Linux rejects unaligned munmap with EINVAL.
    if (size == 0) return;
    if (addr & PAGE_MASK) return;
    std::unique_lock<std::shared_mutex> g(mu_);
    // Linux-like VMA-split semantics: a munmap may cover any sub-range of
    // one or more tracked allocations. Free exactly the tracked
    // intersection — NEVER hand untracked space to free_ranges_: a later
    // mmap_alloc would first-fit it and its zeroing would destroy live data
    // (vkQuake's corrupted BSP vertices).
    const uint64_t lo = addr;
    // Reject wrapping ranges before page-rounding (Linux -EINVAL; the
    // syscall layer enforces it, this is defense for internal callers).
    uint64_t end;
    if (__builtin_add_overflow(addr, size, &end)) return;
    uint64_t hi;
    if (__builtin_add_overflow(end, static_cast<uint64_t>(PAGE_MASK), &hi)) {
        hi = ~static_cast<uint64_t>(0);  // saturate: covers the rest
    } else {
        hi &= ~PAGE_MASK;
    }
    if (hi <= lo) return;  // nothing after page rounding
    std::vector<std::pair<uint64_t, uint64_t>> hits;  // [base, size)
    for (const auto& kv : allocations_) {
        const uint64_t b = kv.first;
        const uint64_t e = b + kv.second;
        if (b < hi && e > lo) hits.push_back({b, kv.second});
    }
    for (const auto& h : hits) {
        const uint64_t b = h.first;
        const uint64_t e = b + h.second;
        allocations_.erase(b);
        const uint64_t flo = (lo > b) ? lo : b;
        const uint64_t fhi = (hi < e) ? hi : e;
        // Head/tail remainders stay tracked (VMA-split semantics).
        if (flo > b) allocations_[b] = flo - b;
        if (e > fhi) allocations_[fhi] = e - fhi;
        if (fhi <= flo) continue;
        g_memstats_cum_free.fetch_add(fhi - flo, std::memory_order_relaxed);
        add_free_range(flo, fhi - flo);
    }
    // Remove mapping state even when the range was installed through
    // map_range (ELF PT_LOAD, brk, stacks, vDSO) rather than mmap_alloc.
    detach_file_locked(lo, std::min(hi, window_size_));
    clear_mapping_locked(lo, hi);
}
// ── Free-range bookkeeping ─────────────────────────────────────────────
void Memory::add_free_range(uint64_t addr, uint64_t size) {
    if (size == 0) return;
    // Merge with a previous adjacent free range.
    auto it = free_ranges_.lower_bound(addr);
    if (it != free_ranges_.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == addr) {
            // Extend the previous range to cover this one.
            prev->second += size;
            addr = prev->first;
            size = prev->second;
            free_ranges_.erase(prev);
            it = free_ranges_.lower_bound(addr);
        }
    }
    // Merge with a following adjacent free range.
    if (it != free_ranges_.end() && it->first == addr + size) {
        size += it->second;
        free_ranges_.erase(it);
    }
    free_ranges_[addr] = size;
}
void Memory::remove_free_range(uint64_t addr, uint64_t size) {
    if (size == 0) return;
    uint64_t lo = addr;
    uint64_t hi = addr + size;
    auto it = free_ranges_.lower_bound(addr);
    // Check if an existing free range starts before addr but overlaps.
    if (it != free_ranges_.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second > addr) it = prev;
    }
    while (it != free_ranges_.end() && it->first < hi) {
        uint64_t r_lo = it->first;
        uint64_t r_hi = it->first + it->second;
        // Split the reclaimed range around the overlap.
        uint64_t left_size = (lo > r_lo) ? lo - r_lo : 0;
        uint64_t right_lo = (hi < r_hi) ? hi : r_hi;
        uint64_t right_size = (hi < r_hi) ? r_hi - hi : 0;
        uint64_t keep_lo = left_size ? r_lo : 0;
        uint64_t keep_size = left_size;
        it = free_ranges_.erase(it);
        if (keep_size) free_ranges_[keep_lo] = keep_size;
        if (right_size) free_ranges_[right_lo] = right_size;
    }
}
} // namespace arm64emu
