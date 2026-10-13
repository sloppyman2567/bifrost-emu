// Memory.cpp — guest memory as one host reservation.
//
// Guest address x lives at direct_window_ + x. Every mapping operation is a
// host operation on that reservation (mmap MAP_FIXED, mprotect, madvise)
// plus bookkeeping under mu_: vmas_ (the mapping index), allocations_ and
// free_ranges_ (placement and reuse), file_pages_ (private/shared file backings)
// and the per-page flag mirror read lock-free by check_access and the JIT.
// Allocation.cpp places and frees mappings; Discard.cpp implements madvise,
// userfaultfd and MREMAP_DONTUNMAP.
#include "System/Memory/Memory.hpp"
#include "System/Memory/MemoryInternal.hpp"
#include "System/Core/DebugFlags.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
namespace arm64emu {
static_assert(sizeof(std::atomic<uint8_t>) == sizeof(uint8_t),
              "JIT byte-loads the atomic guest page flags directly");

// BIFROST_WATCH=addr[:size] (hex): log every Memory::write overlapping the
// range with the current interp pc (heap wild-write hunt; use --no-jit so
// all guest stores flow through write()). Parsed once; a single cached
// branch when unset.
namespace {
thread_local uint64_t t_watch_pc = 0;
struct WatchRange {
    bool on = false;
    uint64_t lo = 0, hi = 0;
};
const WatchRange& watch_range() {
    static const WatchRange w = [] {
        WatchRange r;
        const char* s = getenv("BIFROST_WATCH");
        if (!s || !*s) return r;
        char* end = nullptr;
        uint64_t addr = strtoull(s, &end, 0);
        uint64_t size = 64;
        if (end && *end == ':') size = strtoull(end + 1, nullptr, 0);
        if (size == 0) size = 64;
        r.on = true;
        r.lo = addr;
        r.hi = addr + size;
        return r;
    }();
    return w;
}
// BIFROST_WRITE_TRACE_RANGE=lo:hi (hex): restrict the store log to one
// region. Parsed once; `on == false` means log every write.
const WatchRange& trace_range() {
    static const WatchRange w = [] {
        WatchRange r;
        const std::string& s = dbg().write_trace_range;
        if (s.empty()) return r;
        char* end = nullptr;
        uint64_t lo = strtoull(s.c_str(), &end, 0);
        uint64_t hi = ~0ull;
        if (end && *end == ':') hi = strtoull(end + 1, nullptr, 0);
        r.on = true;
        r.lo = lo;
        r.hi = hi;
        return r;
    }();
    return w;
}
}  // namespace
void Memory::note_interp_pc(uint64_t pc) { t_watch_pc = pc; }
// BIFROST_MEMSTATS=N: every N seconds print live allocation bytes, the
// cumulative alloc/free volume, RSS and the five largest live allocations.
// A steadily growing live total with flat RSS means the guest leaks; RSS
// growing alone means host-side retention.
std::atomic<uint64_t> g_memstats_cum_alloc{0}, g_memstats_cum_free{0};
void Memory::memstats_reporter(uint64_t period_secs) {
    uint64_t t = 0;
    while (true) {
        std::this_thread::sleep_for(
            std::chrono::seconds(static_cast<int64_t>(period_secs)));
        t += period_secs;
        {
            std::shared_lock<std::shared_mutex> g(mu_);
            uint64_t live_bytes = 0;
            // Top-5 largest live allocations (address, size MB).
            std::pair<uint64_t, uint64_t> top[5] = {};
            for (const auto& kv : allocations_) {
                live_bytes += kv.second;
                for (int i = 0; i < 5; i++) {
                    if (kv.second > top[i].second) {
                        for (int j = 4; j > i; j--) top[j] = top[j - 1];
                        top[i] = kv;
                        break;
                    }
                }
            }
            long rss_pages = 0;
            if (FILE* f = fopen("/proc/self/statm", "r")) {
                unsigned long tot = 0, res = 0;
                if (fscanf(f, "%lu %lu", &tot, &res) == 2)
                    rss_pages = static_cast<long>(res);
                fclose(f);
            }
            fprintf(stderr,
                    "[memstats] t=%llus live=%zu allocs %.1f MB "
                    "cum_alloc=%.1f MB cum_free=%.1f MB rss=%ld MB | top:",
                    (unsigned long long)t, allocations_.size(),
                    static_cast<double>(live_bytes) / (1024.0 * 1024.0),
                    static_cast<double>(
                        g_memstats_cum_alloc.load(std::memory_order_relaxed)) /
                        (1024.0 * 1024.0),
                    static_cast<double>(
                        g_memstats_cum_free.load(std::memory_order_relaxed)) /
                        (1024.0 * 1024.0),
                    rss_pages * 4 / 1024);
            for (int i = 0; i < 5; i++) {
                if (!top[i].second) break;
                fprintf(stderr, " [0x%llx +%.2fMB]",
                        (unsigned long long)top[i].first,
                        static_cast<double>(top[i].second) / (1024.0 * 1024.0));
            }
            fprintf(stderr, "\n");
        }
    }
}

Memory::Memory(bool start_reporter) {
    // Reserve the largest guest address space the host grants. PROT_NONE +
    // MAP_NORESERVE costs only virtual address space (RLIMIT_AS still
    // counts it). The flag mirror is a lazily committed byte per page.
    // One extra readable page precedes the window: it holds the JIT's
    // sticky software-check flag (see JIT_SOFTWARE_CHECKS_OFF).
    for (uint64_t size = WINDOW_SIZE_MAX; size >= WINDOW_SIZE_MIN; size >>= 1) {
        void* window = ::mmap(nullptr, size + PAGE_SIZE, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (window == MAP_FAILED) continue;
        if (::mprotect(window, PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
            ::munmap(window, size + PAGE_SIZE);
            continue;
        }
        void* flags = ::mmap(nullptr, size / PAGE_SIZE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (flags == MAP_FAILED) {
            ::munmap(window, size + PAGE_SIZE);
            continue;
        }
        direct_window_ = static_cast<uint8_t*>(window) + PAGE_SIZE;
        jit_software_checks_ = new (direct_window_ + JIT_SOFTWARE_CHECKS_OFF)
            std::atomic<bool>(false);
        direct_page_flags_ = static_cast<std::atomic<uint8_t>*>(flags);
        window_size_ = size;
        for (uint64_t width = 1; width <= JIT_WINDOW_LIMIT_MAX_WIDTH; ++width) {
            const uint64_t limit = size - width;  // size is at least 512 GiB
            std::memcpy(direct_window_ + jit_window_limit_off(width), &limit, sizeof(limit));
        }
        break;
    }
    if (!direct_window_)
        throw std::runtime_error(
            "cannot reserve the guest address space (needs at least 512 GiB "
            "of virtual address space; check `ulimit -v`)");
    // ASLR for the mmap base, PIE bias and stack top. BIFROST_NO_ASLR=1
    // pins all three (reproducible JIT/interpreter trace comparison).
    mmap_next_ = MMAP_BASE_MIN + random_offset(MMAP_BASE_MAX - MMAP_BASE_MIN);
    pie_base_ = PIE_BASE_MIN + random_offset(PIE_JITTER);
    stack_top_ = STACK_TOP - random_offset(STACK_JITTER);
    if (getenv("BIFROST_MEMGUARD")) {
        fprintf(stderr, "[memguard] window=%p size=0x%llx stack_top=0x%llx\n",
                static_cast<void*>(direct_window_),
                (unsigned long long)window_size_,
                (unsigned long long)stack_top_);
    }
    if (start_reporter) {
        if (const char* ms = getenv("BIFROST_MEMSTATS")) {
            uint64_t period = strtoull(ms, nullptr, 10);
            if (period == 0) period = 5;
            std::thread(&Memory::memstats_reporter, this, period).detach();
        }
    }
}

bool Memory::aslr_disabled() {
    static const bool disabled = getenv("BIFROST_NO_ASLR") != nullptr;
    return disabled;
}

uint64_t Memory::random_offset(uint64_t range) {
    if (aslr_disabled() || range == 0) return 0;
    uint64_t npages = range / PAGE_SIZE;
    if (npages == 0) return 0;
    int fd = ::open("/dev/urandom", O_RDONLY);
    uint64_t entropy = 0;
    if (fd >= 0) {
        ssize_t n = ::read(fd, &entropy, sizeof(entropy));
        ::close(fd);
        if (n != sizeof(entropy)) entropy = 0;
    }
    if (entropy == 0) {
        // Fallback: use the address of a stack variable as entropy
        // (urandom unavailable — chroot/container). Better than a
        // fully fixed layout; still page-aligned.
        uint64_t stack_addr = reinterpret_cast<uint64_t>(&fd);
        entropy = stack_addr ^ 0x5DEECE66DULL;
    }
    return (entropy % npages) * PAGE_SIZE;
}
Memory::~Memory() {
    if (direct_window_) ::munmap(direct_window_ - PAGE_SIZE, window_size_ + PAGE_SIZE);
    if (direct_page_flags_) ::munmap(direct_page_flags_, window_size_ / PAGE_SIZE);
}
// A guest range must lie inside the reservation, above the NULL page and
// without wrapping.
bool Memory::is_valid_guest_range(uint64_t addr, uint64_t size) const {
    if (size == 0) return true;
    if (addr < NULL_PAGE_LIMIT) return false;
    if (addr >= window_size_ || size > window_size_ - addr) return false;
    return true;
}
bool Memory::set_direct_prot(uint64_t lo, uint64_t hi, int prot) const {
    if (lo >= hi || lo >= window_size_) return true;
    hi = std::min<uint64_t>(hi, window_size_);
    // The host never executes the reservation; the interpreter and JIT READ
    // guest code from it. Guest PROT_EXEC therefore maps to host PROT_READ.
    // Passing PROT_EXEC through made an execute-only guest mapping (legal
    // on AArch64, used by the Android framework) host execute-only on CPUs
    // with protection keys, so translating it SIGSEGV'd the emulator. Guest
    // reads of such pages are still refused by check_access(); JIT inline
    // loads rely on host protection and can read them.
    int host = PROT_NONE;
    if (prot & (PROT_READ | PROT_EXEC)) host |= PROT_READ;
    if (prot & PROT_WRITE) host |= PROT_WRITE;
    return ::mprotect(direct_window_ + lo, hi - lo, host) == 0;
}
// ── vmas_: coalesced mapping index ─────────────────────────────────────
void Memory::split_vma_locked(uint64_t at) {
    auto it = vmas_.upper_bound(at);
    if (it == vmas_.begin()) return;
    --it;
    if (it->first >= at || it->second.end <= at) return;
    const Vma right{it->second.end, it->second.prot};
    it->second.end = at;
    vmas_.emplace(at, right);
}
void Memory::erase_vma_locked(uint64_t lo, uint64_t hi) {
    if (lo >= hi) return;
    split_vma_locked(lo);
    split_vma_locked(hi);
    auto it = vmas_.lower_bound(lo);
    while (it != vmas_.end() && it->first < hi) it = vmas_.erase(it);
}
uint64_t Memory::find_gap_locked(uint64_t from, uint64_t limit, uint64_t size) const {
    uint64_t cur = from;
    auto it = vmas_.upper_bound(cur);
    if (it != vmas_.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end > cur) cur = prev->second.end;
    }
    while (cur < limit && size <= limit - cur) {
        if (it == vmas_.end() || it->first >= cur + size) return cur;
        cur = std::max(cur, it->second.end);
        ++it;
    }
    return 0;
}
bool Memory::range_mapped_locked(uint64_t lo, uint64_t hi) const {
    uint64_t cur = lo;
    auto it = vmas_.upper_bound(cur);
    if (it != vmas_.begin()) --it;
    while (cur < hi) {
        while (it != vmas_.end() && it->second.end <= cur) ++it;
        if (it == vmas_.end() || it->first > cur) return false;
        cur = std::min<uint64_t>(hi, it->second.end);
        ++it;
    }
    return true;
}
bool Memory::range_overlaps_locked(uint64_t lo, uint64_t hi) const {
    if (lo >= hi) return false;
    auto it = vmas_.lower_bound(lo);
    if (it != vmas_.begin() && std::prev(it)->second.end > lo) return true;
    return it != vmas_.end() && it->first < hi;
}
bool Memory::mapping_prot_at_locked(uint64_t addr, uint8_t& prot) const {
    auto it = vmas_.upper_bound(addr);
    if (it == vmas_.begin()) return false;
    --it;
    if (it->first > addr || it->second.end <= addr) return false;
    prot = it->second.prot & 7;
    return true;
}
void Memory::set_mapping_locked(uint64_t lo, uint64_t hi, uint8_t prot,
                                bool combine, bool keep_uffd) {
    if (lo >= hi) return;
    if (prot && !(prot & GUEST_PROT_READ))
        jit_software_checks_->store(true, std::memory_order_release);
    auto flags_for = [&](uint8_t old, uint8_t page_prot) {
        const uint8_t uffd = keep_uffd ? (old & GUEST_PAGE_UFFD_BITS) : 0;
        return static_cast<uint8_t>(page_prot ? (PAGE_MAPPED | uffd | page_prot) : uffd);
    };
    if (!combine) {
        (void)set_direct_prot(lo, hi, prot);
        if (prot) {
            for (uint64_t p = lo / PAGE_SIZE; p < hi / PAGE_SIZE; ++p)
                direct_page_flags_[p].store(
                    flags_for(direct_page_flags_[p].load(std::memory_order_relaxed), prot),
                    std::memory_order_release);
        } else {
            // Only pages that had access carry flags; a PROT_NONE range costs
            // nothing for the parts that were already inaccessible.
            auto it = vmas_.upper_bound(lo);
            if (it != vmas_.begin()) --it;
            for (; it != vmas_.end() && it->first < hi; ++it) {
                if (!it->second.prot || it->second.end <= lo) continue;
                const uint64_t a = std::max(lo, it->first), b = std::min(hi, it->second.end);
                for (uint64_t p = a / PAGE_SIZE; p < b / PAGE_SIZE; ++p)
                    direct_page_flags_[p].store(
                        flags_for(direct_page_flags_[p].load(std::memory_order_relaxed), 0),
                        std::memory_order_release);
            }
        }
        erase_vma_locked(lo, hi);
        vmas_[lo] = Vma{hi, prot};
    } else {
        // map_range: loader images and runtime regions. Overlapping ELF
        // segment pages receive the union of their permissions.
        uint64_t run_start = lo;
        uint8_t run_prot = 0;
        for (uint64_t p = lo / PAGE_SIZE; p < hi / PAGE_SIZE; ++p) {
            const uint64_t page_addr = p * PAGE_SIZE;
            uint8_t old_prot = 0;
            (void)mapping_prot_at_locked(page_addr, old_prot);
            const uint8_t next = static_cast<uint8_t>(old_prot | prot);
            direct_page_flags_[p].store(
                flags_for(direct_page_flags_[p].load(std::memory_order_relaxed), next),
                std::memory_order_release);
            if (page_addr == lo) {
                run_prot = next;
            } else if (next != run_prot) {
                (void)set_direct_prot(run_start, page_addr, run_prot);
                run_start = page_addr;
                run_prot = next;
            }
        }
        (void)set_direct_prot(run_start, hi, run_prot);
        split_vma_locked(lo);
        split_vma_locked(hi);
        uint64_t cur = lo;
        auto it = vmas_.lower_bound(lo);
        while (cur < hi) {
            if (it == vmas_.end() || it->first > cur) {
                const uint64_t gap_end = (it == vmas_.end())
                    ? hi : std::min<uint64_t>(hi, it->first);
                it = vmas_.emplace_hint(it, cur, Vma{gap_end, prot});
                cur = gap_end;
                ++it;
            } else {
                it->second.prot |= prot;
                cur = std::min<uint64_t>(hi, it->second.end);
                ++it;
            }
        }
    }
    // Coalesce adjacent ranges with equal permissions.
    auto it = vmas_.lower_bound(lo);
    if (it != vmas_.begin()) --it;
    while (it != vmas_.end()) {
        auto next = std::next(it);
        if (next == vmas_.end() || next->first > hi) break;
        if (it->second.end == next->first && it->second.prot == next->second.prot) {
            it->second.end = next->second.end;
            vmas_.erase(next);
        } else {
            it = next;
        }
    }
}
void Memory::clear_mapping_locked(uint64_t lo, uint64_t hi) {
    hi = std::min(hi, window_size_);
    if (lo >= hi) return;
    const bool track = uffd_tracking_.load(std::memory_order_acquire);
    auto it = vmas_.upper_bound(lo);
    if (it != vmas_.begin()) --it;
    for (; it != vmas_.end() && it->first < hi; ++it) {
        if (it->second.end <= lo) continue;
        const uint64_t a = std::max(lo, it->first), b = std::min(hi, it->second.end);
        // Registration bits can sit on PROT_NONE pages only once a
        // userfaultfd exists; otherwise those pages carry no flags.
        if (it->second.prot || track)
            for (uint64_t p = a / PAGE_SIZE; p < b / PAGE_SIZE; ++p)
                direct_page_flags_[p].store(0, std::memory_order_release);
        (void)set_direct_prot(a, b, PROT_NONE);
        // Above the low region, munmap returns the memory to the host. Low
        // pages stay resident and are zeroed on reuse (see memory.h).
        const uint64_t release = std::max(a, LOW_REGION_SIZE);
        if (release < b) (void)::madvise(direct_window_ + release, b - release, MADV_DONTNEED);
    }
    erase_vma_locked(lo, hi);
    for (auto g = guard_pages_.begin(); g != guard_pages_.end();) {
        const uint64_t at = *g * PAGE_SIZE;
        if (at >= lo && at < hi) g = guard_pages_.erase(g);
        else ++g;
    }
}
void Memory::zero_range_locked(uint64_t lo, uint64_t hi, bool noreserve) {
    hi = std::min(hi, window_size_);
    if (lo >= hi) return;
    // Low-region pages are zeroed eagerly: guests that fully rewrite large
    // buffers every frame would otherwise take a fault per page (a measured
    // 20x slower reuse). Above it, and for virtual reservations that can
    // span GiBs, discarding is cheaper and frees the memory.
    uint64_t eager_end = noreserve ? lo : std::min(hi, LOW_REGION_SIZE);
    if (lo < eager_end) {
        (void)set_direct_prot(lo, eager_end, PROT_READ | PROT_WRITE);
        std::memset(direct_window_ + lo, 0, eager_end - lo);
    }
    const uint64_t lazy = std::max(lo, eager_end);
    if (lazy < hi) (void)::madvise(direct_window_ + lazy, hi - lazy, MADV_DONTNEED);
}
void Memory::check_access(uint64_t addr, size_t size, uint8_t access) const {
    if (!(access & GUEST_PROT_EXEC)) addr = data_address(addr);
    if (size == 0) return;
    uint64_t end;
    if (__builtin_add_overflow(addr, static_cast<uint64_t>(size), &end) ||
        end > window_size_) {
        const uint64_t at = addr < window_size_ ? window_size_ : addr;
        throw UnmappedMemory(at, (access & GUEST_PROT_WRITE) != 0);
    }
    const bool write = (access & GUEST_PROT_WRITE) != 0;
    for (uint64_t p = addr / PAGE_SIZE; p < (end + PAGE_MASK) / PAGE_SIZE; ++p) {
        const uint64_t at = std::max(addr, p * PAGE_SIZE);
        uint8_t flags = direct_page_flags_[p].load(std::memory_order_acquire);
        if (!(flags & PAGE_MAPPED)) {
            // No access on this page: unmapped, or mapped PROT_NONE.
            uint8_t prot = 0;
            bool mapped;
            {
                std::shared_lock<std::shared_mutex> g(mu_);
                mapped = mapping_prot_at_locked(at, prot);
            }
            if (mapped && access == 0) continue;
            throw UnmappedMemory(at, write, mapped);
        }
        if (access && (flags & access) != access)
            throw UnmappedMemory(at, write, true);
        if (__builtin_expect(flags & GUEST_PAGE_UFFD_MISSING, 0)) {
            if (flags & GUEST_PAGE_UFFD_REGISTERED) {
                UnmappedMemory fault(at, write);
                fault.uffd = true;
                throw fault;
            }
            // Unregistered: Linux supplies a zero page; ours is zeroed.
            direct_page_flags_[p].fetch_and(
                static_cast<uint8_t>(~GUEST_PAGE_UFFD_MISSING),
                std::memory_order_acq_rel);
        }
    }
}
bool Memory::range_accessible(uint64_t addr, size_t size, uint8_t access) const {
    try {
        check_access(addr, size, access);
        return true;
    } catch (const UnmappedMemory&) {
        return false;
    }
}
// ── File-backed mappings ──────────────────────────────────────────────
Memory::FileBacking::~FileBacking() {
    if (fd >= 0) ::close(fd);
}
const Memory::FilePage* Memory::file_page_locked(uint64_t addr) const {
    auto it = file_pages_.find(addr / PAGE_SIZE);
    return it == file_pages_.end() ? nullptr : &it->second;
}
void Memory::detach_file_locked(uint64_t lo, uint64_t hi) {
    // Restore anonymous storage before zero/reuse, rather than overwriting
    // a live file through a former MAP_SHARED alias. Adjacent file mappings are
    // replaced with one host mapping.
    auto it = file_pages_.lower_bound(lo / PAGE_SIZE);
    while (it != file_pages_.end() && it->first * PAGE_SIZE < hi) {
        const uint64_t addr = it->first * PAGE_SIZE;
        uint64_t end = addr + PAGE_SIZE;
        auto next = std::next(it);
        while (next != file_pages_.end() && next->first * PAGE_SIZE == end && end < hi) {
            end += PAGE_SIZE;
            ++next;
        }
        void* mapped = ::mmap(direct_window_ + addr, end - addr, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
        if (mapped == MAP_FAILED) throw std::bad_alloc();
        it = file_pages_.erase(it, next);
    }
}
int Memory::map_shared_file(uint64_t addr, uint64_t size, int fd,
                            uint64_t offset, uint8_t prot) {
    return map_file(addr, size, fd, offset, prot, true);
}
int Memory::map_file(uint64_t addr, uint64_t size, int fd,
                     uint64_t offset, uint8_t prot, bool shared) {
    if (!size || (addr & PAGE_MASK) || (offset & PAGE_MASK) ||
        size > UINT64_MAX - PAGE_MASK) return EINVAL;
    const uint64_t length = (size + PAGE_MASK) & ~PAGE_MASK;
    if (!is_valid_guest_range(addr, length) || offset > INT64_MAX - length)
        return EINVAL;
    const int mode = ::fcntl(fd, F_GETFL);
    if (mode < 0) return errno;
    const bool writable = (mode & O_ACCMODE) != O_RDONLY;
    if ((mode & O_ACCMODE) == O_WRONLY ||
        (shared && (prot & GUEST_PROT_WRITE) && !writable)) return EACCES;
    auto owner = std::make_shared<FileBacking>();
    owner->fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (owner->fd < 0) return errno;
    owner->writable = writable;
    owner->shared = shared;
    owner->file_offset = offset;
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (!range_mapped_locked(addr, addr + length)) return EINVAL;
    int host_prot = 0;
    if (prot & (GUEST_PROT_READ | GUEST_PROT_EXEC)) host_prot |= PROT_READ;
    if (prot & GUEST_PROT_WRITE) host_prot |= PROT_WRITE;
    void* alias = ::mmap(direct_window_ + addr, length, host_prot,
                         (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED,
                         fd, static_cast<off_t>(offset));
    if (alias == MAP_FAILED) return errno;
    for (uint64_t a = addr; a < addr + length; a += PAGE_SIZE)
        file_pages_.insert_or_assign(a / PAGE_SIZE, FilePage{owner, a - addr});
    set_mapping_locked(addr, addr + length, prot, false);
    return 0;
}
int Memory::sync_shared(uint64_t addr, uint64_t size, int flags) {
    if ((addr & PAGE_MASK) || (flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) ||
        ((flags & MS_ASYNC) && (flags & MS_SYNC))) return EINVAL;
    if (size > UINT64_MAX - addr) return EINVAL;
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (!range_mapped_locked(addr, addr + size)) return ENOMEM;
    if (size == 0) return 0;
    // The aliases are host MAP_SHARED mappings at the guest addresses, so
    // the host msync of the same range writes them back.
    return ::msync(direct_window_ + addr, size, flags) < 0 ? errno : 0;
}
bool Memory::map_device_readonly(uint64_t addr, uint64_t size, int host_fd) {
    if (!size || (addr & PAGE_MASK) || (size & PAGE_MASK)) return false;
    return map_shared_file(addr, size, host_fd, 0, GUEST_PROT_READ) == 0;
}
bool Memory::mprotect_guest(uint64_t addr, uint64_t size, uint8_t prot, int* error) {
    if (error) *error = ENOMEM;
    if ((addr & PAGE_MASK) || (prot & ~uint8_t(7))) return false;
    if (size == 0) return true;
    uint64_t end;
    if (__builtin_add_overflow(addr, size, &end) ||
        __builtin_add_overflow(end, PAGE_MASK, &end)) return false;
    end &= ~PAGE_MASK;
    if (end <= addr || end > window_size_) return false;
    std::unique_lock<std::shared_mutex> g(mu_);
    if (!range_mapped_locked(addr, end)) return false;
    if (prot & GUEST_PROT_WRITE) {
        for (auto it = file_pages_.lower_bound(addr / PAGE_SIZE);
             it != file_pages_.end() && it->first < end / PAGE_SIZE; ++it) {
            if (it->second.backing->shared && !it->second.backing->writable) {
                if (error) *error = EACCES;
                return false;
            }
        }
    }
    set_mapping_locked(addr, end, prot, false, /*keep_uffd=*/true);
    return true;
}
bool Memory::map_range(uint64_t addr, uint64_t size) {
    if (size == 0) return true;
    uint64_t end;
    if (__builtin_add_overflow(addr, size, &end) || end > UINT64_MAX - PAGE_MASK)
        return false;
    const uint64_t map_lo = addr & ~PAGE_MASK;
    const uint64_t map_hi = (end + PAGE_MASK) & ~PAGE_MASK;
    if (map_hi > window_size_) return false;
    std::unique_lock<std::shared_mutex> g(mu_);
    set_mapping_locked(map_lo, map_hi, GUEST_PROT_READ | GUEST_PROT_WRITE, true);
    if (map_hi > LOW_REGION_SIZE) high_next_ = std::max(high_next_, map_hi);
    const uint64_t stack_bottom = stack_top_ - STACK_SIZE;
    if (map_lo < stack_bottom && map_hi > mmap_next_)
        mmap_next_ = map_hi;
    return true;
}
bool Memory::is_mapped(uint64_t addr, uint64_t size) const {
    if (size == 0) return true;
    return range_accessible(addr, size, 0);
}
void Memory::write(uint64_t addr, const void* src, size_t n) {
    addr = data_address(addr);
    if (n == 0) return;
    check_access(addr, n, GUEST_PROT_WRITE);
    record_write(addr, src, n);
    uint8_t* dst = direct_window_ + addr;
    // A runtime-sized memcpy stores 2..32 bytes as overlapping head/tail
    // moves, so a register-sized guest store is written twice. The replay
    // can undo another CPU's write in between (a released lock reopened
    // after a native LSE swap took it). Store each element exactly once;
    // aligned elements are single-copy atomic as AArch64 requires.
    using U16 = uint16_t __attribute__((aligned(1), may_alias));
    using U32 = uint32_t __attribute__((aligned(1), may_alias));
    using U64 = uint64_t __attribute__((aligned(1), may_alias));
    switch (n) {
        case 2: *reinterpret_cast<volatile U16*>(dst) = *static_cast<const U16*>(src); return;
        case 4: *reinterpret_cast<volatile U32*>(dst) = *static_cast<const U32*>(src); return;
        case 8: *reinterpret_cast<volatile U64*>(dst) = *static_cast<const U64*>(src); return;
        case 16:
            reinterpret_cast<volatile U64*>(dst)[0] = static_cast<const U64*>(src)[0];
            reinterpret_cast<volatile U64*>(dst)[1] = static_cast<const U64*>(src)[1];
            return;
        default: std::memcpy(dst, src, n);
    }
}
void Memory::record_write(uint64_t addr, const void* src, size_t n) {
    // BIFROST_WATCH: attribute stores overlapping the watch range. Log the
    // first 8 bytes of the written value too — the heap-stomp hunt needs to
    // spot the offending VALUE (e.g. a negative extents 0xC700 or a shifted
    // handle), not just the address.
    {
        const WatchRange& wr = watch_range();
        // BIFROST_WATCH_VAL=0xc700: log any write whose first 8 bytes contain
        // the 16-bit value anywhere (the stomp that writes the corrupt
        // extents). Near-zero volume, so the run reaches the fault.
        static const uint32_t watch_val_ = []() -> uint32_t {
            const char* s = getenv("BIFROST_WATCH_VAL");
            return s ? static_cast<uint32_t>(strtoul(s, nullptr, 0)) : 0;
        }();
        bool val_match = false;
        if (watch_val_) {
            uint64_t vv = 0;
            size_t vn = n < 8 ? n : 8;
            std::memcpy(&vv, src, vn);
            for (size_t b = 0; b + 2 <= vn; b++)
                if (static_cast<uint16_t>(vv >> (b * 8)) == watch_val_) { val_match = true; break; }
        }
        // BIFROST_WATCH_BADEXT: value-agnostic extents/stomp hunt. With
        // BIFROST_JIT_SLOW_STORES=1 every store lands here, so flag any
        // 2-byte store of a value that could drive AllocBlock (|v| > 4096;
        // legitimate extents/texturemins for small maps are tiny).
        static const bool bad_ext_ = getenv("BIFROST_WATCH_BADEXT") != nullptr;
        bool bad_ext = false;
        if (bad_ext_ && n == 2) {
            int16_t sv;
            std::memcpy(&sv, src, 2);
            if (sv < -4096 || sv > 4096) bad_ext = true;
        }
        // n<=8 only by default: the heap-stomp hunt cares about scalar
        // stores (handles, extents, pointers); logging every large memcpy
        // drowns the trace. BIFROST_WATCH_TRAP widens to every width so the
        // offending store is caught regardless of size.
        const bool range_hit = wr.on && addr < wr.hi && addr + n > wr.lo;
        if (val_match || bad_ext || (range_hit && (n <= 8 || dbg().watch_trap))) {
            uint64_t v0 = 0;
            size_t vn = n < 8 ? n : 8;
            std::memcpy(&v0, src, vn);
            fprintf(stderr, "[watch] w 0x%llx n=%zu v=0x%llx pc=0x%llx\n",
                    (unsigned long long)addr, n,
                    (unsigned long long)v0,
                    (unsigned long long)t_watch_pc);
            fflush(stderr);
        }
        // BIFROST_WATCH_TRAP: stop dead at the first write into the watched
        // range. The guest pc has just been printed — that IS the stomping
        // store. Host SIGILL here is intentional (loud, unambiguous).
        if (range_hit && dbg().watch_trap) __builtin_trap();
    }
    // BIFROST_WRITE_TRACE=<path>: log every guest store (addr, size, tid,
    // guest pc, value). With an interpreter run plus BIFROST_JIT_SLOW_STORES
    // on a JIT run this is a COMPLETE store stream (JIT inline stores
    // bypass write() otherwise), so a clean-vs-bad differential localizes a
    // stomp to the exact writing instruction. BIFROST_WRITE_TRACE_RANGE=lo:hi
    // narrows the log to one region.
    static const int wt_fd = [] {
        if (dbg().write_trace.empty()) return -1;
        return ::open(dbg().write_trace.c_str(),
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }();
    static thread_local const long wt_tid = wt_fd >= 0 ? ::syscall((long)186) : 0;
    if (wt_fd >= 0) {
        const WatchRange& tr = trace_range();
        if (!tr.on || (addr < tr.hi && addr + n > tr.lo)) {
            uint64_t v0 = 0;
            size_t vn = n < 8 ? n : 8;
            std::memcpy(&v0, src, vn);
            char rec[128];
            int len = snprintf(rec, sizeof(rec),
                               "w 0x%llx n=0x%zx t%ld pc=0x%llx v=0x%llx\n",
                               (unsigned long long)addr, n, wt_tid,
                               (unsigned long long)t_watch_pc,
                               (unsigned long long)v0);
            (void)::write(wt_fd, rec, len);
        }
    }
}
bool Memory::compare_exchange(uint64_t addr, uint64_t expected, uint64_t desired, unsigned width, uint64_t* observed) {
    addr = data_address(addr);
    check_access(addr, width, GUEST_PROT_READ | GUEST_PROT_WRITE);
    bool stored = false;
#define CAS_WIDTH(T) { T old = static_cast<T>(expected); stored = __atomic_compare_exchange_n( \
        reinterpret_cast<T*>(direct_window_ + addr), &old, static_cast<T>(desired), \
        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); if (observed) *observed = old; break; }
    switch (width) {
        case 1: CAS_WIDTH(uint8_t)
        case 2: CAS_WIDTH(uint16_t)
        case 4: CAS_WIDTH(uint32_t)
        case 8: CAS_WIDTH(uint64_t)
        default: throw EmuError("unsupported exclusive access width");
    }
#undef CAS_WIDTH
    if (stored) record_write(addr, &desired, width);
    return stored;
}
void Memory::read(uint64_t addr, void* dst, size_t n) const {
    read_access(addr, dst, n, GUEST_PROT_READ);
}
void Memory::read_access(uint64_t addr, void* dst, size_t n, uint8_t access) const {
    if (!(access & GUEST_PROT_EXEC)) addr = data_address(addr);
    if (n == 0) return;
    check_access(addr, n, access);
    std::memcpy(dst, direct_window_ + addr, n);
}
// ── Atomics ────────────────────────────────────────────────────────────
// Aligned words use a host atomic on the reservation itself, so they are
// coherent with JIT and interpreter stores to the same address. An
// unaligned word (std::atomic would be torn/UB there) is compared and
// written under mu_.
template <typename T>
static bool cas_in_window(uint8_t* host, std::shared_mutex& mu, uint64_t addr,
                          T expected, T desired) {
    if ((addr & (sizeof(T) - 1)) == 0) {
        auto* slot = reinterpret_cast<std::atomic<T>*>(host);
        return slot->compare_exchange_strong(expected, desired,
                                             std::memory_order_acq_rel);
    }
    std::unique_lock<std::shared_mutex> g(mu);
    T cur;
    std::memcpy(&cur, host, sizeof(T));
    if (cur != expected) return false;
    std::memcpy(host, &desired, sizeof(T));
    return true;
}
bool Memory::atomic_cas_32(uint64_t addr, uint32_t expected, uint32_t desired) {
    addr = data_address(addr);
    check_access(addr, sizeof(uint32_t), GUEST_PROT_READ | GUEST_PROT_WRITE);
    return cas_in_window(direct_window_ + addr, mu_, addr, expected, desired);
}
bool Memory::atomic_cas_64(uint64_t addr, uint64_t expected, uint64_t desired) {
    addr = data_address(addr);
    check_access(addr, sizeof(uint64_t), GUEST_PROT_READ | GUEST_PROT_WRITE);
    return cas_in_window(direct_window_ + addr, mu_, addr, expected, desired);
}
size_t Memory::page_count() const {
    std::shared_lock<std::shared_mutex> g(mu_);
    uint64_t bytes = 0;
    for (const auto& [lo, vma] : vmas_) bytes += vma.end - lo;
    return static_cast<size_t>(bytes / PAGE_SIZE);
}
} // namespace arm64emu
