// core/memory.cpp — guest memory as one host reservation.
//
// Guest address x lives at direct_window_ + x. Every mapping operation is a
// host operation on that reservation (mmap MAP_FIXED, mprotect, madvise)
// plus bookkeeping under mu_: vmas_ (the mapping index), allocations_ and
// free_ranges_ (placement and reuse), file_pages_ (private/shared file backings)
// and the per-page flag mirror read lock-free by check_access and the JIT.
#include "System/Memory/Memory.hpp"
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
static std::atomic<uint64_t> g_memstats_cum_alloc{0}, g_memstats_cum_free{0};
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
    std::memcpy(direct_window_ + addr, src, n);
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
            direct_page_flags_[p].fetch_or(GUEST_PAGE_UFFD_MISSING,
                                           std::memory_order_release);
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
        for (uint64_t p = lo / PAGE_SIZE; p < hi / PAGE_SIZE; ++p) {
            if (registered)
                direct_page_flags_[p].fetch_or(GUEST_PAGE_UFFD_REGISTERED,
                                               std::memory_order_release);
            else
                direct_page_flags_[p].fetch_and(
                    static_cast<uint8_t>(~GUEST_PAGE_UFFD_REGISTERED),
                    std::memory_order_release);
        }
    }
    return 0;
}
void Memory::uffd_drop_registrations() {
    if (!uffd_tracking_.load(std::memory_order_acquire)) return;
    std::unique_lock<std::shared_mutex> g(mu_);
    for (const auto& [lo, vma] : vmas_)
        for (uint64_t p = lo / PAGE_SIZE; p < vma.end / PAGE_SIZE; ++p)
            direct_page_flags_[p].fetch_and(static_cast<uint8_t>(~GUEST_PAGE_UFFD_REGISTERED),
                                            std::memory_order_relaxed);
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
        slot.fetch_and(static_cast<uint8_t>(~GUEST_PAGE_UFFD_MISSING),
                       std::memory_order_release);
        filled += PAGE_SIZE;
    }
    return filled;
}
namespace {
bool page_is_zero(const uint8_t* page) {
    const auto* words = reinterpret_cast<const uint64_t*>(page);
    for (size_t i = 0; i < Memory::PAGE_SIZE / sizeof(uint64_t); ++i)
        if (words[i]) return false;
    return true;
}
}  // namespace
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
    for (uint64_t i = 0; i < pages; ++i) {
        const uint64_t from = old_addr + i * PAGE_SIZE, to = dst + i * PAGE_SIZE;
        const uint8_t prot = prots[i];
        if (prot != first_prot) set_mapping_locked(to, to + PAGE_SIZE, prot, false);
        (void)set_direct_prot(from, from + PAGE_SIZE, PROT_READ | PROT_WRITE);
        (void)set_direct_prot(to, to + PAGE_SIZE, PROT_READ | PROT_WRITE);
        if (!page_is_zero(direct_window_ + from))
            std::memcpy(direct_window_ + to, direct_window_ + from, PAGE_SIZE);
        // The moved page keeps its permissions; a missing page stays
        // missing. The new VMA is not registered (no UFFD_FEATURE_EVENT_REMAP).
        const uint8_t missing = flags[i] & GUEST_PAGE_UFFD_MISSING;
        direct_page_flags_[to / PAGE_SIZE].store(
            static_cast<uint8_t>((prot ? (PAGE_MAPPED | prot) : 0) | missing),
            std::memory_order_release);
        (void)set_direct_prot(to, to + PAGE_SIZE, prot);
        // The old range keeps its permissions and registration, now empty
        // (released below).
        direct_page_flags_[from / PAGE_SIZE].store(
            static_cast<uint8_t>((prot ? (PAGE_MAPPED | prot) : 0) |
                                 (flags[i] & GUEST_PAGE_UFFD_REGISTERED) |
                                 (track ? GUEST_PAGE_UFFD_MISSING : 0)),
            std::memory_order_release);
        (void)set_direct_prot(from, from + PAGE_SIZE, prot);
    }
    // Private anonymous pages read back as zero, as after Linux's move.
    (void)::madvise(direct_window_ + old_addr, len, MADV_DONTNEED);
    return static_cast<int64_t>(dst);
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
