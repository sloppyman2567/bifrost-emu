# Guest memory

`Memory.cpp` owns the reservation, mapping index, access checks, file backing
and guest reads/writes; `Allocation.cpp` placement, mremap growth, unmapping
and free ranges; `Discard.cpp` madvise, userfaultfd and MREMAP_DONTUNMAP.
State shared between them goes in `MemoryInternal.hpp`.

- Guest memory is one host reservation: translate through the actual
  `window_base`/`window_size`, never a hardcoded address limit. Keep bounds,
  permission and crossing-page checks, including host-address overflow checks.
- The window is the largest power of two the host grants, 512 GiB to 64 TiB,
  capped by the constructor's `window_max`. Binder realm processes and the
  broker's per-endpoint mirrors use `BINDER_REALM_WINDOW_SIZE` (512 GiB, a
  Android 15 GKI's 39-bit address space): one broker process holds up to 128 mirrors,
  and full-size windows exhaust the host's 47-bit user space after eight.
- The readable page before the window holds the JIT's software-check flag
  and per-width window limits (`JIT_WINDOW_LIMITS_OFF`); generated code
  reads both through the window register, so keep their offsets and contents
  in step with `window_size_`.
- `write()` stores each 2/4/8-byte element (and 16-byte halves) with a single
  host access. Never route instruction-sized guest stores through a
  runtime-sized `memcpy`: its overlapping moves replay the store and can undo
  another CPU's write (see the JIT guide's guest memory barriers).
- File-backed MAP_SHARED and MAP_PRIVATE mappings retain their host backing
  and file identity. Never preload them into anonymous copies. Detach partial
  replaced/unmapped aliases before zeroing anonymous storage. Preserve identity
  through fork/remap and refault file contents after MADV_DONTNEED. `mu_` is held across a host fork
  (`mutex_for_fork`) and re-initialized in the child (`reset_mutex_in_fork_child`): a read/write lock copies its
  queue of waiting writers, which the child cannot serve.
- `mappings_snapshot` builds `/proc/self/maps` from `vmas_`, splitting runs at
  protection, file identity and offset discontinuities. Loader-copied ELF
  pages are named with `name_image`; `detach_file_locked` drops those names
  wherever storage is replaced or unmapped (mprotect keeps them).
- `shared_futex_word` answers whether a 32-bit word lies in a writable MAP_SHARED
  file mapping and gives its host address; the futex syscall depends on it.
  Anonymous MAP_SHARED mappings are not covered.
- MADV_REMOVE punches writable shared file mappings; reject unsupported,
  private, read-only and unmapped ranges with the appropriate errno. Do not
  report successful discard while retaining stale data. Check wrapping lengths.
- userfaultfd SIGBUS mode supports ART's CMC GC. Preserve MISSING/REGISTERED
  bits, missing-page JIT guards, registration lifetime and precise retry state.
  DONTUNMAP moves sparse contents without touching every page of ART's large
  pools; keep its RSS regression and native-kernel comparison. Change page flags
  only through `set_page_flags_locked` (it keeps the software-check count and
  flag), and keep `software_check_page` limited to pages whose host protection
  admits a wrong native access. MISSING is exact only on registered pages:
  registration refreshes it from pagemap plus page contents (host THP populates
  untouched pages). Page access follows arm64's `protection_map` without Enhanced
  PAN (`effective_page_prot`): write or execute implies read. Change host
  protection once per run of equal permissions, not per page; page flags and
  userfaultfd bits stay per page.
- Android leaves guest page zero unmapped for implicit null checks. Linux
  profiles retain their separate zero-page behavior.
- Allocation reuses freed holes while respecting live mappings and tracked
  aliases. `brk` rounds to page boundaries and must not collide with mmap storage.
- `allocations_` is an ordered map of disjoint ranges; munmap and MAP_FIXED find
  overlaps with `allocation_overlaps_locked` (one predecessor plus a forward
  walk), not a full scan. Every insertion must keep ranges disjoint: evict
  (fixed), refuse (no-replace) or take free/bump space.
- Relevant regressions: `test_shared_alias`, `test_madv_remove`,
  `test_userfaultfd`, `test_mem_guard`, `test_lse_inline` (store replay);
  rebuild fixtures before comparing modes.
  Known concurrent C++ copy versus mprotect/unmap limitations are documented in
  [the JIT guide](../../../Docs/JIT.md); do not claim native JIT recovery covers them.
