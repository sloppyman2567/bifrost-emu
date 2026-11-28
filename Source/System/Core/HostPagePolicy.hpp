// HostPagePolicy.hpp — the transparent-huge-page policy of the emulator's host mappings.
//
// Guest storage and the JIT code buffer are dense and keep the host's setting: huge pages cut
// faults and iTLB misses there. Host-side tables are sparse — one byte per guest page, one slot
// per fault site, a bit per block — so with THP=always the first touch of a 4 KiB page can
// commit 2 MiB. Those tables opt out, which makes their cost the same whether the host runs
// THP=always or THP=madvise.
#pragma once
#include <cstddef>
#include <sys/mman.h>

namespace arm64emu {

// Call once, right after the mapping is made and before it is touched.
inline void host_pages_no_huge(void* base, size_t bytes) {
    if (base && bytes) (void)::madvise(base, bytes, MADV_NOHUGEPAGE);
}

}  // namespace arm64emu
