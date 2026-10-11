# ELF loading and dynamic linking

- Even static guests need a DynamicLinker for runtime thunk dlopen. Reject
  non-AArch64 on-disk ELFs; host GL/SDL libraries select thunk registration,
  rather than being mapped as guest machine code. Absolute-path failures retain
  the existing soname/root/toolchain search behavior.
- `/proc/self/maps` must contain non-overlapping real ELF/interpreter extents.
  Loading the interpreter does not extend the main ELF's end address; Bionic
  and glibc use the mapping containing SP to find the main stack.
- glibc variant-I TLS reserves `TLS_PRE_TCB_SIZE` immediately below TP, with
  module TLS below the pthread descriptor. Keep `allocate_static_tls`,
  `allocate_thread_tls`, the syscall `_dl_allocate_tls` handler and patched
  `dl_tls_static_size` consistent. Do not add the descriptor size twice or
  apply this layout to musl's separate convention.
- Native dynlink must establish the existing main-thread pthread descriptor
  fields, not only module TLS. glibc hook offsets are detected from the actual
  loader implementation rather than pinned to one distro's struct layout.
- Keep parsing, relocations, symbol/version lookup, ifunc resolution and dlopen
  lifetimes coherent across the `DynamicLinker/` split. Its private header owns
  shared ELF layouts/constants; trace configuration belongs in shared helpers.
- Run glibc/musl dynamic tests, dlopen/thread/TLS regressions and static startup
  with the correct toolchain and rootfs. Static stand-ins do not test dynlink.
