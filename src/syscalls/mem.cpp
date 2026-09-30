// syscalls/mem.cpp — memory-management syscalls: mmap/munmap/mremap/
// mprotect/madvise/msync/brk.
//
// All case bodies are extracted verbatim from the original syscalls.cpp
// ; they reference the same private Emulator members
// (mem_, brk_, brk_start_, brk_mu_) via the friend declaration in
// core/emulator.h.
#include "core/emulator.h"
#include "core/memory.h"
#include "core/cpu.h"
#include "core/signal.h"
#include "debug_flags.h"
#include "syscalls/syscalls.h"
#include <errno.h>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <mutex>
namespace arm64emu {
// Resolve a guest file descriptor to its host file descriptor, or -1 if
// the guest fd is invalid or does not wrap a host fd. mmap() with a
// file-backed mapping needs the real host fd to preload the file
// contents into the mapping (FdTable allocates guest fds independently
// of the host fd numbers, so the two are NOT interchangeable).
static int resolve_host_fd(Emulator& emu, int guest_fd) {
    auto node = emu.fds().get(guest_fd);
    if (!node) return -1;
    return node->host_fd();
}
int64_t syscall_mem(Emulator& emu, CPU& cpu, uint64_t num) {
    uint64_t a0 = cpu.regs[0], a1 = cpu.regs[1], a2 = cpu.regs[2];
    uint64_t a3 = cpu.regs[3], a4 = cpu.regs[4], a5 = cpu.regs[5];
    (void)a3; (void)a4; (void)a5;
    auto& mem_ = emu.mem_;
    auto& brk_ = emu.brk_;
    auto& brk_start_ = emu.brk_start_;
    auto& brk_mu_ = emu.brk_mu_;
    auto& graphics_ = emu.graphics_;
    switch (num) {
        case 222: { // mmap
            // a0=addr, a1=length, a2=prot, a3=flags, a4=fd, a5=offset
            uint64_t addr = a0;
            uint64_t length = a1;
            uint64_t prot = a2;
            uint64_t flags = a3;
            constexpr uint64_t BIFROST_MAP_ANONYMOUS = 0x20;
            if (length == 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-22)); return 0; } // EINVAL
            // Sanity-check the length: real Linux rejects absurdly large
            // mmaps based on RLIMIT_AS and available address space. Without
            // this, a buggy/malicious guest passing length = SIZE_MAX could
            // OOM the host. We use a generous 64 GiB sanity cap here — the
            // strict per-allocation cap (Memory::MAX_MMAP_LENGTH = 4 GiB,
            // virtual reserves exempt) is enforced inside mmap_alloc below.
            constexpr uint64_t MAX_MMAP_SANITY = 64ULL * 1024 * 1024 * 1024;
            if (length > MAX_MMAP_SANITY) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-ENOMEM));
                return 0;
            }
            // Linux requires file offsets to be page aligned and a valid
            // descriptor for every non-anonymous mapping. Validate before
            // reserving guest address space so failures leave no mapping.
            if ((flags & BIFROST_MAP_ANONYMOUS) == 0) {
                if ((a5 & Memory::PAGE_MASK) != 0) {
                    ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EINVAL)));
                    return 0;
                }
                int64_t guest_fd = static_cast<int64_t>(a4);
                int host_fd = guest_fd >= 0
                    ? resolve_host_fd(emu, static_cast<int>(guest_fd)) : -1;
                if (host_fd < 0) {
                    ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EBADF)));
                    return 0;
                }
                struct stat st;
                if (::fstat(host_fd, &st) < 0) {
                    ret_errno();
                    return 0;
                }
            }
            constexpr uint64_t BIFROST_MAP_FIXED          = 0x10;
            constexpr uint64_t BIFROST_MAP_FIXED_NOREPLACE = 0x100000;
            if (prot & ~uint64_t(7)) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EINVAL)));
                return 0;
            }
            // MAP_NORESERVE and PROT_NONE are virtual reservations. They
            // still have a mapped VMA and permissions are applied below.
            const bool virt_reserve = (flags & 0x4000) || prot == 0;
            // Linux requires a page-aligned addr for MAP_FIXED /
            // MAP_FIXED_NOREPLACE (otherwise -EINVAL). Check here so the
            // allocator never sees an unaligned fixed base.
            if ((flags & (BIFROST_MAP_FIXED | BIFROST_MAP_FIXED_NOREPLACE)) &&
                (addr & Memory::PAGE_MASK) != 0) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EINVAL)));
                return 0;
            }
            // A wrapping length produces a tiny addr+length and bypasses
            // every overlap check below. Reject it (Linux -EINVAL).
            if ((flags & BIFROST_MAP_FIXED_NOREPLACE) && addr + length < addr) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EINVAL)));
                return 0;
            }
            // ── MAP_FIXED_NOREPLACE ─────────────────────────────────────
            // BUGFIX: previously MAP_FIXED_NOREPLACE was silently ignored
            // (treated as a non-FIXED mmap), so the kernel could place the
            // mapping at a different address than requested. The Linux
            // kernel guarantees MAP_FIXED_NOREPLACE either returns the
            // exact requested address or -EEXIST if it overlaps an existing
            // allocation. Game engines and allocators use this flag to
            // reserve address ranges without overwriting mappings.
            if (flags & BIFROST_MAP_FIXED_NOREPLACE) {
                std::lock_guard<std::mutex> brk_lock(brk_mu_);
                if (addr < brk_ && brk_start_ < addr + length) {
                    ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EEXIST)));
                    return 0;
                }
                // Check and reserve in one Memory lock. The range metadata
                // includes ELF PT_LOAD, brk, stack, and PROT_NONE mappings.
                // Start RW so a file-backed mapping can be populated, then
                // apply its requested permissions before returning.
                uint64_t mapped = mem_.mmap_fixed_noreplace(addr, length,
                    virt_reserve, Memory::GUEST_PROT_READ | Memory::GUEST_PROT_WRITE);
                if (mapped == UINT64_MAX) {
                    ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EEXIST)));
                    return 0;
                }
                if (mapped == 0) {
                    ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOMEM)));
                    return 0;
                }
                // Map succeeded — fall through to the file-load branch below
                // using `addr` as both the requested and actual address.
                // We continue into the regular MAP_FIXED path by setting
                // the BIFROST_MAP_FIXED bit conceptually (no-op since we
                // already placed the allocation).
                if (static_cast<int64_t>(a4) != -1 && (a3 & 0x20) == 0) {
                    int host_fd = resolve_host_fd(emu, static_cast<int>(a4));
                    if (host_fd >= 0) {
                        struct stat st;
                        if (::fstat(host_fd, &st) == 0) {
                            uint64_t avail = (st.st_size > (off_t)a5) ? (uint64_t)(st.st_size - (off_t)a5) : 0;
                            uint64_t want = std::min<uint64_t>(length, avail);
                            // Chunked preload so large libs/data files don't
                            // allocate a single huge host bounce (OOM).
                            uint64_t off = 0;
                            std::vector<uint8_t> chunk(64 * 1024);
                            while (off < want) {
                                size_t take = (size_t)std::min<uint64_t>(want - off, chunk.size());
                                ssize_t n = ::pread(host_fd, chunk.data(), take,
                                                    static_cast<off_t>(a5 + off));
                                if (n < 0 && errno == EINTR) continue;
                                if (n <= 0) break;
                                mem_.write(addr + off, chunk.data(), (size_t)n);
                                off += (uint64_t)n;
                                if ((size_t)n < take) break;
                            }
                        }
                    }
                    if (graphics_.ready() && graphics_.owns_fd(static_cast<int>(a4))) {
                        graphics_.set_guest_fb_addr(addr);
                    }
                }
                if (!mem_.mprotect_guest(addr, length, static_cast<uint8_t>(prot))) {
                    mem_.untrack_allocation(addr, length);
                    ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOMEM)));
                    return 0;
                }
                ret_host(addr);
                return 0;
            }
            // ── MAP_FIXED overlap with brk region ──────────────────────
            // musl's mallocng uses MAP_FIXED to carve pages out of the brk
            // region for its meta_area slots. The pattern is:
            //
            //   brk(0)                  → returns B (initial brk)
            //   brk(B + 2*pagesize)     → extends brk to [B, B+2*pagesize)
            //   mmap(B, pagesize, PROT_NONE, MAP_FIXED|MAP_ANON, ...)
            //       → marks [B, B+pagesize) as a guard page
            //   mprotect(B+pagesize, pagesize, PROT_READ|PROT_WRITE)
            //       → makes [B+pagesize, B+2*pagesize) the meta area
            //
            // If we let the MAP_FIXED mmap "own" those pages but leave
            // brk_ pointing past them, a later brk(new) call could
            // re-map the same pages via map_range and corrupt musl's
            // metadata. To prevent that, when a MAP_FIXED mmap lands
            // inside [brk_start_, brk_), we push brk_ forward past the
            // mmap'd region. This is the same behavior the Linux kernel
            // exhibits: a MAP_FIXED mmap inside the brk region implicitly
            // shrinks the brk to just below the mmap'd area, and any
            // future brk() growth must start above the mmap'd region.
            if ((flags & BIFROST_MAP_FIXED) && addr >= brk_start_ && addr < brk_) {
                uint64_t mmap_end = addr + length;
                if (mmap_end > brk_) {
                    // The mmap'd region extends past the current brk —
                    // push brk_ forward to cover it. (Note: this does
                    // NOT match kernel semantics exactly — the kernel
                    // would shrink brk_ to addr. But musl tracks its
                    // own ctx.brk separately and re-extends it via
                    // brk(new) calls, so pushing forward is safer
                    // because it avoids losing pages that musl might
                    // still reference.)
                    std::lock_guard<std::mutex> g(brk_mu_);
                    brk_ = mmap_end;
                }
            }
            // MAP_FIXED replaces any existing user mapping in the requested
            // range atomically (see Memory::mmap_fixed_replace). For an
            // interior overlap the old split-off code left stale
            // allocations/pages live, and a later reuse/zero treated part
            // of a live object as reclaimed space (vkQuake surfaces array
            // -> "AllocBlock: full").
            //
            // brk/map_range memory is intentionally not in allocations_, so
            // the helper preserves the special brk carve-out above while
            // making fixed mappings over normal mmap allocations Linux-like.
            // Protection controls access, not the new mapping's contents.
            // Anonymous replacements must be zeroed even for PROT_NONE;
            // file-backed replacements must preload before protection applies.
            uint64_t effective_hint = (flags & BIFROST_MAP_FIXED) ? addr : 0;
            // MAP_NORESERVE (0x4000 on AArch64 asm-generic — NOT 0x40!)
            // / PROT_NONE mappings are VIRTUAL reservations on real
            // Linux — they cost no memory until touched. Route them
            // through the lazy path: no eager page materialization, no
            // charge against MAX_TOTAL_PAGES at reserve time (per-fault
            // OOM checks still apply). vkQuake's mimalloc reserves
            // GiB-scale arenas this way.
            uint64_t mapped;
            if (flags & BIFROST_MAP_FIXED) {
                // Populate writable first; requested permissions are applied
                // after file-backed contents have been copied.
                mapped = mem_.mmap_fixed_replace(addr, length, virt_reserve,
                    true, Memory::GUEST_PROT_READ | Memory::GUEST_PROT_WRITE);
                if (mapped != 0) emu.invalidate_jit_range(addr, length);
            } else {
                mapped = mem_.mmap_alloc(length, effective_hint, virt_reserve,
                    Memory::GUEST_PROT_READ | Memory::GUEST_PROT_WRITE);
            }
            // BUGFIX: mmap_alloc returns 0 on failure (size cap, invalid
            // range, or OOM page-limit). Returning that 0 to the guest
            // reads as a *successful* mapping at address 0 (musl only
            // treats -1/MAP_FAILED as failure), so mallocng built its
            // arena at address 0 and free() crashed with BRK #1000.
            // Translate to a proper negative errno → guest MAP_FAILED.
            if (mapped == 0) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOMEM)));
                return 0;
            }
             if (dbg().trace_mmap) {
                fprintf(stderr, "[mmap(pc=0x%llx addr=0x%llx, len=%lu, prot=%lu, flags=0x%llx, fd=%lld, off=%llu) → 0x%llx]\n",
                        (unsigned long long)cpu.pc,
                        (unsigned long long)addr,
                        (unsigned long)length,
                        (unsigned long)prot,
                        (unsigned long long)flags,
                        (long long)(int64_t)a4,
                        (unsigned long long)a5,
                        (unsigned long long)mapped);
            }
            // If a file fd is given, read its contents in
            // MAP_ANONYMOUS is 0x20 on Linux AArch64. The old code checked
            // 0x2 (MAP_PRIVATE), so file-backed MAP_PRIVATE mmaps (the
            // standard mechanism for mapping executables and shared
            // libraries) skipped the file-load branch and the guest saw
            // zero pages. Fix: check the correct bit.
            if ((a3 & BIFROST_MAP_ANONYMOUS) == 0) {
                int host_fd = resolve_host_fd(emu, static_cast<int>(a4));
                if (host_fd >= 0) {
                    struct stat st;
                    if (::fstat(host_fd, &st) == 0) {
                        uint64_t avail = (st.st_size > (off_t)a5) ? (uint64_t)(st.st_size - (off_t)a5) : 0;
                        uint64_t want = std::min<uint64_t>(length, avail);
                        uint64_t off = 0;
                        std::vector<uint8_t> chunk(64 * 1024);
                        while (off < want) {
                            size_t take = (size_t)std::min<uint64_t>(want - off, chunk.size());
                            ssize_t n = ::pread(host_fd, chunk.data(), take,
                                                static_cast<off_t>(a5 + off));
                            if (n < 0 && errno == EINTR) continue;
                            if (n <= 0) break;
                            mem_.write(mapped + off, chunk.data(), (size_t)n);
                            off += (uint64_t)n;
                            if ((size_t)n < take) break;
                        }
                    }
                }
                // If the guest is mmap'ing the graphics framebuffer fd,
                // record the guest address so the host can sync the
                // pixels back from the guest's pages on demand (for
                // dump_to_ppm / refresh). Use owns_fd() (which does
                // fstat comparison) because open_dev_fb0() returns a
                // dup'd fd, not the original fb_fd_.
                if (graphics_.ready() && graphics_.owns_fd(static_cast<int>(a4))) {
                    graphics_.set_guest_fb_addr(mapped);
                }
            }
            if (!mem_.mprotect_guest(mapped, length, static_cast<uint8_t>(prot))) {
                mem_.untrack_allocation(mapped, length);
                emu.invalidate_jit_range(mapped, length);
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOMEM)));
                return 0;
            }
            ret_host(mapped);
            return 0;
        }
        case 215: { // munmap - reclaim the pages and address range
            // BUGFIX: the old code only removed the allocation from the
            // tracking map ("no-op OK"). That made mmap_alloc a pure bump
            // allocator: guest malloc/free churn (the game's 18 MB chunk
            // meshes every frame) marched the heap pointer up to ~8.5 GB,
            // exhausted MAX_TOTAL_PAGES (1M pages = 4 GiB), and mmap
            // started returning 0 — which the guest treated as a valid
            // mapping at address 0, so musl mallocng built its arena
            // there and free() crashed with BRK #1000 (get_meta). Now
            // munmap frees the pages (page-cap reflects live memory) and
            // returns the address range to the free list for reuse.
            if (dbg().trace_mmap) {
                fprintf(stderr, "[munmap(0x%llx, %lu)]\n",
                        (unsigned long long)a0,
                        (unsigned long)a1);
            }
            // Linux requires a page-aligned base. Do not let the memory
            // helper round an invalid address down and free its live page.
            if (a0 & Memory::PAGE_MASK) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EINVAL)));
                return 0;
            }
            // Linux rejects a wrapping length with -EINVAL (addr+len
            // overflow). Without this, the page rounding in
            // untrack_allocation could cover the entire address space and
            // free live mappings.
            if (a0 + a1 < a0) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EINVAL)));
                return 0;
            }
            mem_.untrack_allocation(a0, a1);
            // Unmapped bytes are gone — drop JIT translations of the range
            // so a later reuse of these addresses can't run stale code.
            emu.invalidate_jit_range(a0, a1);
            ret_host(0);
            return 0;
        }
        case 226: { // mprotect
            if ((a0 & Memory::PAGE_MASK) || (a2 & ~uint64_t(7))) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EINVAL)));
                return 0;
            }
            if (!mem_.mprotect_guest(a0, a1, static_cast<uint8_t>(a2))) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOMEM)));
                return 0;
            }
            if (a1) emu.invalidate_jit_range(a0, a1);
            ret_host(0);
            return 0;
        }
        case 216: { // mremap(old_addr, old_size, new_size, flags, new_addr) — AArch64 216
            // musl's mallocng uses mremap in two places:
            //
            //   1. Growing a "big" (> ~128KB) malloc allocation when
            //      realloc() needs more space than the current mmap'd
            //      region. musl passes MREMAP_MAYMOVE here and is
            //      prepared for the address to change.
            //
            //   2. Growing the meta_area (the page holding mallocng
            //      metadata headers). Some musl versions also use
            //      mremap with MREMAP_MAYMOVE here; older versions
            //      fall back to a fresh mmap on failure.
            //
            // The previous implementation always grew in place by
            // calling map_range() on the additional pages. That is
            // a no-op when those pages are already mapped — which is
            // exactly what happens when a subsequent mmap() (e.g. for
            // a small malloc group) has been placed right after the
            // big allocation by our bump pointer. The result was
            // silent corruption: musl thought the grown range was its
            // own, but the pages actually contained data from another
            // allocation. This broke sort.c at ~17K lines.
            //
            // The fix is in Memory::mremap_grow(): we now track every
            // mmap'd region and detect collisions. If growing in place
            // would overlap a later allocation, we move the mapping to
            // a fresh region and copy the old data — which musl handles
            // correctly thanks to MREMAP_MAYMOVE.
            uint64_t old_addr = a0;
            uint64_t old_size = a1;
            uint64_t new_size = a2;
            constexpr uint64_t MAYMOVE = 1;
            constexpr uint64_t FIXED = 2;
            const bool fixed = (a3 & FIXED) != 0;
            if ((a3 & ~(MAYMOVE | FIXED)) || (old_addr & Memory::PAGE_MASK) ||
                !old_size || !new_size ||
                old_size > UINT64_MAX - Memory::PAGE_MASK ||
                new_size > UINT64_MAX - Memory::PAGE_MASK ||
                (fixed && (!(a3 & MAYMOVE) || (a4 & Memory::PAGE_MASK)))) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EINVAL)));
                return 0;
            }
            const uint64_t old_len = (old_size + Memory::PAGE_MASK) & ~Memory::PAGE_MASK;
            const uint64_t new_len = (new_size + Memory::PAGE_MASK) & ~Memory::PAGE_MASK;
            if (old_addr > UINT64_MAX - old_len ||
                (fixed && (a4 > UINT64_MAX - new_len ||
                 (old_addr < a4 + new_len && a4 < old_addr + old_len)))) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EINVAL)));
                return 0;
            }
            // Address zero cannot be mapped by Memory; do not confuse it
            // with the helper's sentinel for an unspecified destination.
            if (fixed && a4 == 0) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOMEM)));
                return 0;
            }
            uint64_t result = mem_.mremap_grow(old_addr, old_size, new_size,
                (a3 & MAYMOVE) != 0, fixed ? a4 : 0);
            if (result == 0) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-ENOMEM)));
                return 0;
            }
            // A move leaves stale bytes at the old range and fresh copies
            // at the new one — invalidate both so neither runs stale code.
            emu.invalidate_jit_range(old_addr, old_size);
            if (result != old_addr) emu.invalidate_jit_range(result, new_size);
            if (dbg().trace_mmap) {
                fprintf(stderr, "[mremap(0x%llx, %lu → %lu) → 0x%llx]\n",
                        (unsigned long long)old_addr,
                        (unsigned long)old_size,
                        (unsigned long)new_size,
                        (unsigned long long)result);
            }
            ret_host(result);
            return 0;
        }
        case 227: { // msync(addr, length, flags) — AArch64 227
            // No-op: our sparse pages are always in sync.
            ret_host(0);
            return 0;
        }
        case 233: { // madvise
            if (dbg().trace_madvise) {
                fprintf(stderr, "[madvise(0x%llx, %lu, %lld)]\n",
                        (unsigned long long)a0,
                        (unsigned long)a1, (long long)(int64_t)a2);
            }
            if (a0 & Memory::PAGE_MASK) {
                ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EINVAL)));
                return 0;
            }
            // MADV_DONTNEED (4): Linux discards the pages — the next
            // access reads zeros. vkQuake's mimalloc decommits freed
            // segments this way and reuses them as initially-zero pages;
            // a no-op here kept stale data and shredded its heap
            // (AllocBlock: full). Other advices stay no-op (pure hints).
            if ((int64_t)a2 == 4 /* MADV_DONTNEED */) {
                mem_.madvise_dontneed(a0, a1);
                emu.invalidate_jit_range(a0, a1);
            }
            ret_host(0);
            return 0;
        }
        case 214: { // brk
            std::lock_guard<std::mutex> g(brk_mu_);
            if (a0 == 0) { ret_host(brk_); return 0; }
            if (a0 < brk_start_) { ret_host(brk_); return 0; }
            // The Linux kernel rounds the new break up to the page
            // boundary, so a guest that hands brk() an unaligned address
            // (e.g. musl's variadic syscall() wrapper passing a stale x0
            // for a no-arg brk(0) call) still gets a page-aligned break.
            // Returning the raw value here made brk(0) report an
            // unaligned break and broke test_brk once the heap/stack
            // moved into the low window.
            uint64_t new_brk = (a0 + Memory::PAGE_MASK) & ~Memory::PAGE_MASK;
            // extensions. The Linux kernel checks the new break against
            // RLIMIT_DATA and the available address space; without this
            // check, a buggy (or malicious) guest could request
            // brk(0xFFFFFFFFFFFFFFFF) and the emulator would try to
            // map_range() ~2^64 bytes, exhausting host memory or
            // hanging for minutes.
            //
            // We use a generous 1 GiB upper bound on the heap — way
            // more than any reasonable program needs, but small enough
            // to prevent runaway allocations. If a program legitimately
            // needs a bigger heap, it should use mmap() directly.
            constexpr uint64_t MAX_BRK_SIZE = 1ULL << 30;  // 1 GiB
            if (new_brk - brk_start_ > MAX_BRK_SIZE) {
                // Reject: return the current brk unchanged.
                ret_host(brk_);
                return 0;
            }
            // Keep the heap out of the stack region: a guest passing a
            // bogus (e.g. stack-relative) address must not let brk
            // shadow the stack pages. Linux likewise refuses brk growth
            // that would collide with the mmap/stack area.
            if (new_brk >= mem_.stack_top() - Memory::STACK_SIZE) {
                ret_host(brk_);
                return 0;
            }
            if (new_brk > brk_) {
                // A fixed guest mapping may have occupied the future brk
                // range since the previous break. Linux refuses to grow
                // across another VMA rather than merging the mappings.
                // Reserve atomically and reject ANY overlapping page, not
                // only a completely mapped range. This also serializes brk
                // growth with concurrent fixed mappings.
                uint64_t mapped = mem_.mmap_fixed_noreplace(brk_, new_brk - brk_,
                    false, Memory::GUEST_PROT_READ | Memory::GUEST_PROT_WRITE);
                if (mapped != brk_) {
                    ret_host(brk_);
                    return 0;
                }
            } else if (new_brk < brk_) {
                mem_.untrack_allocation(new_brk, brk_ - new_brk);
                emu.invalidate_jit_range(new_brk, brk_ - new_brk);
            }
            brk_ = new_brk;
            ret_host(brk_);
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
