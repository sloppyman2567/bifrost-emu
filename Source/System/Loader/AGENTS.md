# ELF loading and dynamic linking

- Even static guests need a DynamicLinker for runtime thunk dlopen. Reject
  non-AArch64 on-disk ELFs; host GL/SDL libraries select thunk registration,
  rather than being mapped as guest machine code. Absolute-path failures retain
  the existing soname/root/toolchain search behavior.
- `/proc/self/maps` must contain non-overlapping real ELF/interpreter extents.
  Loading the interpreter does not extend the main ELF's end address; Bionic
  and glibc use the mapping containing SP to find the main stack. Every loader
  that copies PT_LOAD bytes names their file pages (`Memory::name_image`) with
  the file it read, so maps attributes them like kernel file mappings.
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
- ld.so never runs the main executable's initializers; `__libc_start_main` does.
  glibc 2.34+ runs the program's non-NULL `init` (legacy `_start`, imports
  `__libc_start_main@GLIBC_2.<34`) or else walks `link_map`, which native dynlink
  does not populate. `run_init_arrays_` therefore runs the main object's
  `DT_INIT`/`DT_INIT_ARRAY` only for modern-start executables
  (`legacy_start_runs_init_`); running them for legacy ones doubles every
  constructor. Do not size symbol scans by `symtab_count` (misses imports).
- `--guest-ld` (`Emulator::set_guest_linker`) execs a PT_INTERP program like the
  kernel: the program loads unrelocated (`defer_relocations_to_interp`), the
  interpreter loads from the rootfs through `ElfLoader::load(reserve_image)`,
  `entry_` is the interpreter and `AT_ENTRY`/`AT_BASE` carry the rest; no
  `DynamicLinker` is created for it. A guest glibc `ld.so` reads only
  `/etc/ld.so.cache`, which `Scripts/Setup/make-ld-cache.py` writes (and
  `ldconfig -p -C` can verify). Thunked libraries (libGL, libSDL2, libvulkan,
  audio, ...) have no rootfs file: when a `--guest-ld` open of one fails with
  ENOENT, `open_guest_at` returns `Emulator::open_thunk_stub`, a memfd holding
  `build_thunk_stub_elf`'s ET_DYN whose functions branch (x16) to the existing
  thunk trampolines. Keep it relocation-free with a writable PT_DYNAMIC (ld.so
  adjusts it in place); the thunks must be initialized first. A real AArch64
  file of that name in any directory ld.so searches (LD_LIBRARY_PATH,
  `ld.so.conf`, the defaults) wins, as with the native linker: ld.so tries the
  directories in turn, so answering the first miss would shadow a real
  library (dhewm3's `libopenal.so.1` lost `alcIsExtensionPresent` that way).
  `--guest-ld` forwards the host `LD_LIBRARY_PATH` to the guest, in guest
  paths; the default environment omits it for the native linker. Run
  `run_tests.sh --guest-ld` alongside the default dynamic suite.
- `RootfsDiscovery` picks the library root: `BIFROST_ROOT` (the sandbox, set by
  `--rootfs`, the environment or `[paths] rootfs`), else the first default rootfs
  (next to the executable, `$XDG_DATA_HOME`/`~/.local/share`, `/usr/{local/,}share`).
  A discovered root only adds library directories in `find_library`; it must never
  set `BIFROST_ROOT` or confine guest paths. A missing libc is fatal
  (`libc_missing()` throws in `load_elf_file`), because the guest `ld.so` fallback
  cannot recover. `test_rootfs_discovery.sh` copies the binary away from the
  checkout so the bundled toolchain cannot mask a lookup failure.
- Run glibc/musl dynamic tests, dlopen/thread/TLS regressions and static startup
  with the correct toolchain and rootfs. Static stand-ins do not test dynlink.
