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
            if (length == 0) { cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-22)); return 0; } // EINVAL
            // Sanity-check the length: real Linux rejects absurdly large
            // mmaps based on RLIMIT_AS and available address space. Without
            // this, a buggy/malicious guest passing length = SIZE_MAX could
            // OOM the host. We use a generous 64 GiB cap — way more than
            // any reasonable program needs, but small enough to prevent
            // runaway allocations. (Game engines typically mmap 1-4 GiB
            // for texture streaming pools; databases rarely exceed 32 GiB
            // for shared buffers.)
            constexpr uint64_t MAX_MMAP_LENGTH = 64ULL * 1024 * 1024 * 1024;
            if (length > MAX_MMAP_LENGTH) {
                cpu.regs[0] = static_cast<uint64_t>(static_cast<int64_t>(-ENOMEM));
                return 0;
            }
            constexpr uint64_t BIFROST_MAP_FIXED          = 0x10;
            constexpr uint64_t BIFROST_MAP_FIXED_NOREPLACE = 0x100000;
            // ── MAP_FIXED_NOREPLACE ─────────────────────────────────────
            // BUGFIX: previously MAP_FIXED_NOREPLACE was silently ignored
            // (treated as a non-FIXED mmap), so the kernel could place the
            // mapping at a different address than requested. The Linux
            // kernel guarantees MAP_FIXED_NOREPLACE either returns the
            // exact requested address or -EEXIST if it overlaps an existing
            // allocation. Game engines and allocators use this flag to
            // reserve address ranges without overwriting mappings.
            if (flags & BIFROST_MAP_FIXED_NOREPLACE) {
                // Check if [addr, addr+length) overlaps any existing allocation.
                auto allocs = mem_.allocations_snapshot();
                for (const auto& [base, size] : allocs) {
                    uint64_t other_end = base + size;
                    if (addr < other_end && base < addr + length) {
                        // Overlap → reject without replacing.
                        ret_host(static_cast<uint64_t>(static_cast<int64_t>(-EEXIST)));
                        return 0;
                    }
                }
                // No overlap: place at the exact address (treat like MAP_FIXED
                // from here on, but with no overwrite of existing pages since
                // we just verified there are none in range).
                uint64_t mapped = mem_.mmap_alloc(length, addr);
                (void)mapped;
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
                            std::vector<uint8_t> buf(std::min<uint64_t>(length, st.st_size));
                            off_t old = ::lseek(host_fd, 0, SEEK_CUR);
                            ::lseek(host_fd, a5, SEEK_SET);
                            ssize_t n = ::read(host_fd, buf.data(), buf.size());
                            ::lseek(host_fd, old, SEEK_SET);
                            if (n > 0) mem_.write(addr, buf.data(), n);
                        }
                    }
                    if (graphics_.ready() && graphics_.owns_fd(static_cast<int>(a4))) {
                        graphics_.set_guest_fb_addr(addr);
                    }
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
            // PROT_NONE with MAP_FIXED: these are guard pages. Don't
            // zero existing pages (preserves musl's metadata). Just
            // return success.
            if (prot == 0 && (flags & 0x10)) { // PROT_NONE + MAP_FIXED
                ret_host(addr);
                return 0;
            }
            uint64_t effective_hint = (flags & BIFROST_MAP_FIXED) ? addr : 0;
            uint64_t mapped = mem_.mmap_alloc(length, effective_hint);
             if (getenv("BIFROST_TRACE_MMAP")) {
                fprintf(stderr, "[mmap(pc=0x%llx addr=0x%llx, len=%lu, prot=%lu, flags=0x%llx, fd=%lld, off=%llu) → 0x%llx]\n",
                        (unsigned long long)cpu.pc,
                        (unsigned long long)addr,
                        (unsigned long)length,
                        (unsigned long)prot,
                        (unsigned long long)flags,
                        (long long)(int64_t)a4,
                        (unsigned long long)a5,
                        (unsigned long long)mapped);
                if (length > (1u << 20)) {
                    // TEMP DEBUG: who makes a big mmap?
                    uint64_t fp = cpu.regs[29];
                    for (int i = 0; i < 24 && fp && (fp & 1) == 0; i++) {
                        uint64_t ra = 0, pfp = 0;
                        try {
                            emu.mem().read(fp, &pfp, 8);
                            emu.mem().read(fp + 8, &ra, 8);
                        } catch (...) { break; }
                        fprintf(stderr, "  bigmmap[%2d] fp=0x%llx ra=0x%llx\n",
                                i, (unsigned long long)fp, (unsigned long long)ra);
                        if (pfp <= fp) break;
                        fp = pfp;
                    }
                }
            }
            // Note: musl's mallocng uses MAP_FIXED with PROT_NONE to carve
            // pages from the brk region, then calls mprotect to make them
            // usable. The mmap_alloc above preserves existing pages on
            // MAP_FIXED (see Memory::mmap_alloc), so musl's metadata
            // written via the brk extension is not zeroed out.
            // If a file fd is given, read its contents in
            // MAP_ANONYMOUS is 0x20 on Linux AArch64. The old code checked
            // 0x2 (MAP_PRIVATE), so file-backed MAP_PRIVATE mmaps (the
            // standard mechanism for mapping executables and shared
            // libraries) skipped the file-load branch and the guest saw
            // zero pages. Fix: check the correct bit.
            if (static_cast<int64_t>(a4) != -1 && (a3 & 0x20) == 0 /* not MAP_ANONYMOUS */) {
                int host_fd = resolve_host_fd(emu, static_cast<int>(a4));
                if (host_fd >= 0) {
                    struct stat st;
                    if (::fstat(host_fd, &st) == 0) {
                        std::vector<uint8_t> buf(std::min<uint64_t>(length, st.st_size));
                        off_t old = ::lseek(host_fd, 0, SEEK_CUR);
                        ::lseek(host_fd, a5, SEEK_SET);
                        ssize_t n = ::read(host_fd, buf.data(), buf.size());
                        ::lseek(host_fd, old, SEEK_SET);
                        if (n > 0) mem_.write(mapped, buf.data(), n);
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
            ret_host(mapped);
            return 0;
        }
        case 215: { // munmap - we just leave pages allocated (no-op OK)
            // We don't actually reclaim the pages (our sparse memory
            // model has no mechanism to give pages back), but we DO
            // remove the allocation from our tracking map so that a
            // future mremap_grow collision check won't see it as an
            // obstacle. This matches the Linux kernel's contract:
            // after munmap returns, the address range is free for new
            // mmap use, even though we happen to keep the page data
            // around (the guest won't access it again because musl
            // has dropped its pointer to it).
            if (getenv("BIFROST_TRACE_MMAP")) {
                fprintf(stderr, "[munmap(0x%llx, %lu)]\n",
                        (unsigned long long)a0,
                        (unsigned long)a1);
            }
            mem_.untrack_allocation(a0);
            ret_host(0);
            return 0;
        }
        case 226: { // mprotect - no-op
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
            // a3 = flags (we honor MREMAP_MAYMOVE implicitly — we may
            //              return a different address whenever we have to)
            // a4 = new_addr (only used with MREMAP_FIXED; we don't support that)
            // musl sometimes calls mremap(0, 0, size, MREMAP_MAYMOVE)
            // as a "malloc via mremap" idiom (especially in the
            // meta_area init path). Treat that as a plain mmap.
            if (old_addr == 0 && old_size == 0) {
                uint64_t mapped = mem_.mmap_alloc(new_size, 0);
                if (getenv("BIFROST_TRACE_MMAP")) {
                    fprintf(stderr, "[mremap(0,0,%lu) → 0x%llx]\n",
                            (unsigned long)new_size,
                            (unsigned long long)mapped);
                }
                ret_host(mapped);
                return 0;
            }
            uint64_t result = mem_.mremap_grow(old_addr, old_size, new_size);
            if (getenv("BIFROST_TRACE_MMAP")) {
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
        case 233: { // madvise - no-op
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
            if (new_brk >= Memory::STACK_TOP - Memory::STACK_SIZE) {
                ret_host(brk_);
                return 0;
            }
            if (new_brk > brk_) {
                mem_.map_range(brk_, new_brk - brk_);
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
