// Discard.cpp — page-content discard and fault handling: MADV_DONTNEED
// (anonymous zero and file refault), MADV_REMOVE (hole punching for shared
// file mappings), userfaultfd registration/fill in SIGBUS mode, and
// MREMAP_DONTUNMAP moves of sparse contents.
//
// See Source/System/Memory/AGENTS.md for the file-identity and
// userfaultfd contracts these paths must preserve.
#include "System/Memory/Memory.hpp"
#include "System/Memory/MemoryInternal.hpp"
#include "System/Core/DebugFlags.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <shared_mutex>
#include <sys/mman.h>
#include <unistd.h>
namespace arm64emu {
void Memory::madvise_dontneed(uint64_t addr, uint64_t len) {
    if (len == 0) return;
    // Linux applies MADV_DONTNEED to whole pages: addr is page-aligned and
    // len is rounded upward.
    uint64_t start = addr & ~PAGE_MASK;
    uint64_t end;
    if (__builtin_add_overflow(addr, len, &end) ||
        __builtin_add_overflow(end, PAGE_MASK, &end)) end = UINT64_MAX & ~PAGE_MASK;
    else end &= ~PAGE_MASK;
    end = std::min(end, window_size_);
    std::unique_lock<std::shared_mutex> g(mu_);
    const bool track = uffd_tracking_.load(std::memory_order_acquire);
    // Walk the mappings, not every page: a guest may pass an enormous len,
    // and Linux ignores unmapped holes inside the range.
    auto it = vmas_.upper_bound(start);
    if (it != vmas_.begin()) --it;
    for (; it != vmas_.end() && it->first < end; ++it) {
        if (it->second.end <= start) continue;
        const uint64_t a = std::max(start, it->first), b = std::min(end, it->second.end);
        // Anonymous pages read back as zero, file pages refault
        // from the file: exactly the host semantics of the same call.
        (void)::madvise(direct_window_ + a, b - a, MADV_DONTNEED);
        if (!track || !it->second.prot) continue;
        // Discarded private pages become userfaultfd-missing.
        for (uint64_t p = a / PAGE_SIZE; p < b / PAGE_SIZE; ++p) {
            if (file_pages_.count(p)) continue;
            set_page_flags_locked(p, direct_page_flags_[p].load(std::memory_order_relaxed) |
                                         GUEST_PAGE_UFFD_MISSING);
        }
    }
}
int Memory::madvise_remove(uint64_t addr, uint64_t len) {
    if (!len) return 0;
    uint64_t end;
    if (__builtin_add_overflow(addr, len, &end)) return EINVAL;
    end = std::min(end, window_size_);
    std::shared_lock<std::shared_mutex> lock(mu_);
    // Adjacent pages of one file are punched with a single fallocate.
    const FileBacking* run = nullptr;
    uint64_t run_offset = 0, run_length = 0;
    auto punch = [&]() -> int {
        if (!run_length) return 0;
        const int result = ::fallocate(run->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                       static_cast<off_t>(run_offset),
                                       static_cast<off_t>(run_length));
        run_length = 0;
        return result < 0 ? errno : 0;
    };
    // Linux stops at the first failing VMA after finishing the earlier ones.
    auto visit = [&](uint64_t at) -> int {
        const FilePage* page = file_page_locked(at);
        int error = 0;
        if (!page) error = EINVAL;
        else if (!page->backing->shared || !page->backing->writable) error = EACCES;
        if (error) {
            const int punched = punch();
            return punched ? punched : error;
        }
        const uint64_t offset = page->backing->file_offset + page->offset;
        if (run == page->backing.get() && run_offset + run_length == offset) {
            run_length += PAGE_SIZE;
            return 0;
        }
        if (int punched = punch()) return punched;
        run = page->backing.get();
        run_offset = offset;
        run_length = PAGE_SIZE;
        return 0;
    };
    // Walk the mapping list so holes cost nothing.
    int unmapped = 0;
    uint64_t at = addr;
    auto it = vmas_.upper_bound(at);
    if (it != vmas_.begin()) --it;
    while (at < end) {
        while (it != vmas_.end() && it->second.end <= at) ++it;
        if (it == vmas_.end() || it->first >= end) {
            unmapped = ENOMEM;
            break;
        }
        if (it->first > at) {
            unmapped = ENOMEM;
            at = it->first;
        }
        for (const uint64_t stop = std::min(end, it->second.end); at < stop; at += PAGE_SIZE)
            if (int error = visit(at)) return error;
    }
    if (int error = punch()) return error;
    return unmapped;
}
// ── userfaultfd ────────────────────────────────────────────────────────
namespace {
bool page_is_zero(const uint8_t* page) {
    const auto* words = reinterpret_cast<const uint64_t*>(page);
    for (size_t i = 0; i < Memory::PAGE_SIZE / sizeof(uint64_t); ++i)
        if (words[i]) return false;
    return true;
}
}  // namespace
void Memory::enable_uffd_tracking() {
    if (uffd_tracking_.load(std::memory_order_acquire)) return;
    std::unique_lock<std::shared_mutex> g(mu_);
    // Without a readable page table, registration cannot tell a populated page
    // from a discarded one, so every access keeps the software checks.
    const int fd = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    uint64_t entry;
    if (fd < 0 || ::pread(fd, &entry, sizeof(entry), 0) != sizeof(entry)) {
        software_checks_sticky_ = true;
        jit_software_checks_->store(true, std::memory_order_seq_cst);
    }
    if (fd >= 0) ::close(fd);
    uffd_tracking_.store(true, std::memory_order_release);
}
bool Memory::refresh_missing_locked(uint64_t lo, uint64_t hi) {
    // A guest write gives a discarded page a host page-table entry: present
    // (bit 63) or swapped (bit 62) in /proc/self/pagemap
    // (Documentation/admin-guide/mm/pagemap.rst). The host's own entries are
    // not the guest's, though: a transparent huge page populates up to 511
    // untouched neighbours (seen in Umamusume, where ART's moving space then
    // read zeros instead of faulting). Those are all zero, so a page counts as
    // populated only with nonzero contents; one the guest touched but left
    // zero stays missing. /proc/self is opened per call: a descriptor kept
    // across fork would read the parent.
    int fd = -1;
    constexpr uint64_t kPresentOrSwapped = 3ull << 62;
    constexpr uint64_t kBatch = 512;
    uint64_t entries[kBatch];
    for (uint64_t p = lo / PAGE_SIZE; p < hi / PAGE_SIZE;) {
        if (!(direct_page_flags_[p].load(std::memory_order_relaxed) & GUEST_PAGE_UFFD_MISSING)) {
            ++p;
            continue;
        }
        if (fd < 0 && (fd = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC)) < 0) return false;
        const uint64_t count = std::min(kBatch, hi / PAGE_SIZE - p);
        const uint64_t host_page = reinterpret_cast<uintptr_t>(direct_window_) / PAGE_SIZE + p;
        const ssize_t got = ::pread(fd, entries, count * sizeof(uint64_t),
                                    static_cast<off_t>(host_page * sizeof(uint64_t)));
        if (got != static_cast<ssize_t>(count * sizeof(uint64_t))) {
            ::close(fd);
            return false;
        }
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t flags = direct_page_flags_[p + i].load(std::memory_order_relaxed);
            // PAGE_MAPPED: the host page is readable (PROT_NONE pages are not).
            if ((flags & GUEST_PAGE_UFFD_MISSING) && (flags & PAGE_MAPPED) &&
                (entries[i] & kPresentOrSwapped) && !page_is_zero(direct_window_ + (p + i) * PAGE_SIZE))
                set_page_flags_locked(p + i, flags & ~GUEST_PAGE_UFFD_MISSING);
        }
        p += count;
    }
    if (fd >= 0) ::close(fd);
    return true;
}
int Memory::uffd_register(uint64_t addr, uint64_t len, bool registered) {
    uint64_t last;
    if (__builtin_add_overflow(addr, len, &last)) return -EINVAL;
    last = std::min(last, window_size_);
    std::unique_lock<std::shared_mutex> g(mu_);
    // Register anonymous VMAs present in the range; file backings are unsupported.
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    auto it = vmas_.upper_bound(addr);
    if (it != vmas_.begin()) --it;
    for (; it != vmas_.end() && it->first < last; ++it) {
        const uint64_t lo = std::max(addr, it->first), hi = std::min(last, it->second.end);
        if (lo >= hi) continue;
        auto shared = file_pages_.lower_bound(lo / PAGE_SIZE);
        if (shared != file_pages_.end() && shared->first < hi / PAGE_SIZE) return -EINVAL;
        ranges.emplace_back(lo, hi);
    }
    if (ranges.empty()) return -EINVAL;
    for (const auto& [lo, hi] : ranges) {
        if (registered && !refresh_missing_locked(lo, hi) && !software_checks_sticky_) {
            software_checks_sticky_ = true;
            jit_software_checks_->store(true, std::memory_order_seq_cst);
        }
        for (uint64_t p = lo / PAGE_SIZE; p < hi / PAGE_SIZE; ++p) {
            const uint8_t flags = direct_page_flags_[p].load(std::memory_order_relaxed);
            set_page_flags_locked(p, registered ? (flags | GUEST_PAGE_UFFD_REGISTERED)
                                                : (flags & ~GUEST_PAGE_UFFD_REGISTERED));
        }
    }
    return 0;
}
void Memory::uffd_drop_registrations() {
    if (!uffd_tracking_.load(std::memory_order_acquire)) return;
    std::unique_lock<std::shared_mutex> g(mu_);
    for (const auto& [lo, vma] : vmas_)
        for (uint64_t p = lo / PAGE_SIZE; p < vma.end / PAGE_SIZE; ++p)
            set_page_flags_locked(p, direct_page_flags_[p].load(std::memory_order_relaxed) &
                                         ~GUEST_PAGE_UFFD_REGISTERED);
}
int64_t Memory::uffd_fill(uint64_t dst, const uint8_t* src, uint64_t len) {
    if (!is_valid_guest_range(dst, len)) return -ENOENT;
    std::unique_lock<std::shared_mutex> g(mu_);
    // mm/userfaultfd.c validate_dst_vma: the whole destination must lie in
    // registered memory before anything is filled.
    for (uint64_t at = dst; at < dst + len; at += PAGE_SIZE) {
        const uint8_t flags = direct_page_flags_[at / PAGE_SIZE].load(
            std::memory_order_relaxed);
        uint8_t prot;
        if (!mapping_prot_at_locked(at, prot) || !(flags & GUEST_PAGE_UFFD_REGISTERED))
            return -ENOENT;
    }
    int64_t filled = 0;
    for (uint64_t at = dst; at < dst + len; at += PAGE_SIZE) {
        auto& slot = direct_page_flags_[at / PAGE_SIZE];
        const uint8_t flags = slot.load(std::memory_order_relaxed);
        if (!(flags & GUEST_PAGE_UFFD_MISSING))
            return filled ? filled : -EEXIST;  // page already present
        uint8_t prot = 0;
        (void)mapping_prot_at_locked(at, prot);
        if (!(prot & GUEST_PROT_WRITE))
            (void)set_direct_prot(at, at + PAGE_SIZE, PROT_READ | PROT_WRITE);
        if (src) std::memcpy(direct_window_ + at, src + (at - dst), PAGE_SIZE);
        else std::memset(direct_window_ + at, 0, PAGE_SIZE);
        if (!(prot & GUEST_PROT_WRITE))
            (void)set_direct_prot(at, at + PAGE_SIZE, prot);
        // Release: a guard/check that sees the page present sees its bytes.
        set_page_flags_locked(at / PAGE_SIZE, slot.load(std::memory_order_relaxed) &
                                                  ~GUEST_PAGE_UFFD_MISSING);
        filled += PAGE_SIZE;
    }
    return filled;
}
int64_t Memory::mremap_dontunmap(uint64_t old_addr, uint64_t len, uint64_t fixed_addr) {
    // ART's CMC moves its heap into a from-space reservation placed anywhere.
    // Linux moves page tables, so untouched pages cost nothing on either side.
    // The destination is lazily zeroed (noreserve): copy only nonzero pages,
    // and release the source with one MADV_DONTNEED. Copying every page made
    // each GC commit ART's whole 1 GiB linear-alloc pool and its from-space.
    if (!len || !is_valid_guest_range(old_addr, len) ||
        (fixed_addr && !is_valid_guest_range(fixed_addr, len)))
        return -EINVAL;
    const uint64_t pages = len / PAGE_SIZE;
    std::vector<uint8_t> flags(pages), prots(pages);
    {
        std::shared_lock<std::shared_mutex> g(mu_);
        for (uint64_t i = 0; i < pages; ++i) {
            const uint64_t at = old_addr + i * PAGE_SIZE;
            if (!mapping_prot_at_locked(at, prots[i])) return -EFAULT;  // Linux: no VMA
            if (file_page_locked(at)) return -EINVAL;  // anonymous only
            flags[i] = direct_page_flags_[at / PAGE_SIZE].load(std::memory_order_relaxed);
        }
    }
    const uint8_t first_prot = prots[0];
    const uint64_t dst = fixed_addr
        ? mmap_fixed_replace(fixed_addr, len, true, true, first_prot)
        : mmap_alloc(len, 0, true, first_prot);
    if (!dst) return -ENOMEM;
    std::unique_lock<std::shared_mutex> g(mu_);
    const bool track = uffd_tracking_.load(std::memory_order_acquire);
    // Change host protection once per run of equal permissions, not four
    // times per page: ART remaps 512 MiB-1 GiB pools on every collection.
    // Userfaultfd state is software page flags, so host protection already
    // matches each run's permissions and read/write runs need no change.
    for (uint64_t run = 0; run < pages;) {
        const uint8_t prot = prots[run];
        uint64_t run_end = run + 1;
        while (run_end < pages && prots[run_end] == prot) ++run_end;
        const uint64_t from = old_addr + run * PAGE_SIZE, to = dst + run * PAGE_SIZE;
        const uint64_t bytes = (run_end - run) * PAGE_SIZE;
        if (prot != first_prot) set_mapping_locked(to, to + bytes, prot, false);
        const bool raise = !(prot & GUEST_PROT_WRITE) ||
                           !(prot & (GUEST_PROT_READ | GUEST_PROT_EXEC));
        if (raise) {
            (void)set_direct_prot(from, from + bytes, PROT_READ | PROT_WRITE);
            (void)set_direct_prot(to, to + bytes, PROT_READ | PROT_WRITE);
        }
        for (uint64_t i = run; i < run_end; ++i) {
            const uint64_t page_from = old_addr + i * PAGE_SIZE, page_to = dst + i * PAGE_SIZE;
            if (!page_is_zero(direct_window_ + page_from))
                std::memcpy(direct_window_ + page_to, direct_window_ + page_from, PAGE_SIZE);
            // The moved page keeps its permissions; a missing page stays
            // missing. The new VMA is not registered (no UFFD_FEATURE_EVENT_REMAP).
            const uint8_t missing = flags[i] & GUEST_PAGE_UFFD_MISSING;
            const uint8_t access = effective_page_prot(prot);
            set_page_flags_locked(page_to / PAGE_SIZE,
                static_cast<uint8_t>((access ? (PAGE_MAPPED | access) : 0) | missing));
            // The old range keeps its permissions and registration, now empty
            // (released below).
            set_page_flags_locked(page_from / PAGE_SIZE,
                static_cast<uint8_t>((access ? (PAGE_MAPPED | access) : 0) |
                                     (flags[i] & GUEST_PAGE_UFFD_REGISTERED) |
                                     (track ? GUEST_PAGE_UFFD_MISSING : 0)));
        }
        if (raise) {
            (void)set_direct_prot(to, to + bytes, prot);
            (void)set_direct_prot(from, from + bytes, prot);
        }
        run = run_end;
    }
    // Private anonymous pages read back as zero, as after Linux's move.
    (void)::madvise(direct_window_ + old_addr, len, MADV_DONTNEED);
    return static_cast<int64_t>(dst);
}
} // namespace arm64emu
