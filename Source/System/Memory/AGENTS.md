# Guest memory

`Memory.cpp` owns the reservation, mapping index, access checks, file backing
and guest reads/writes; `Allocation.cpp` placement, mremap growth, unmapping
and free ranges; `Discard.cpp` madvise, userfaultfd and MREMAP_DONTUNMAP.
State shared between them goes in `MemoryInternal.hpp`.

- Guest memory is one host reservation: translate through the actual
  `window_base`/`window_size`, never a hardcoded address limit. Keep bounds,
  permission and crossing-page checks, including host-address overflow checks.
- File-backed MAP_SHARED and MAP_PRIVATE mappings retain their host backing
  and file identity. Never preload them into anonymous copies. Detach partial
  replaced/unmapped aliases before zeroing anonymous storage. Preserve identity
  through fork/remap and refault file contents after MADV_DONTNEED.
- MADV_REMOVE punches writable shared file mappings; reject unsupported,
  private, read-only and unmapped ranges with the appropriate errno. Do not
  report successful discard while retaining stale data. Check wrapping lengths.
- userfaultfd SIGBUS mode supports ART's CMC GC. Preserve MISSING/REGISTERED
  bits, missing-page JIT guards, registration lifetime and precise retry state.
  DONTUNMAP moves sparse contents without touching every page of ART's large
  pools; keep its RSS regression and native-kernel comparison.
- Android leaves guest page zero unmapped for implicit null checks. Linux
  profiles retain their separate zero-page behavior.
- Allocation reuses freed holes while respecting live mappings and tracked
  aliases. `brk` rounds to page boundaries and must not collide with mmap storage.
- Relevant regressions: `test_shared_alias`, `test_madv_remove`,
  `test_userfaultfd`, `test_mem_guard`; rebuild fixtures before comparing modes.
  Known concurrent C++ copy versus mprotect/unmap limitations are documented in
  [the JIT guide](../../../Docs/JIT.md); do not claim native JIT recovery covers them.
