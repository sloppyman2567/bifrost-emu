# Changelog

All notable changes to **bifrost-emu** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
with pre-release tags (`-beta.N`, `-rc.N`) for unstable versions.

## [1.1.0-beta.1] — 2026-06-17

**The mallocng loop is broken.** Toybox now gets past musl's mallocng
initialization — no more infinite `brk()` loop — and exits with code 1
on a null pointer dereference (a different, much simpler bug). glibc
static hello also gets further (exit 1 instead of hanging or 133).

The root cause turned out to be a structural bug in the instruction
decoder: the entire LSE atomics handler was unreachable, so every
CAS / LDADD / LDCLR / SWP / etc. was silently treated as a NOP.
This broke musl's lock acquisition, which caused the mallocng loop.

### Fixed (major)
- **LSE atomics handler was unreachable** (the big one). The LSE
  atomics block (CAS, LDADD, LDCLR, LDEOR, LDSET, SMAX, SMIN, UMAX,
  UMIN, SWP) was nested inside the exclusive load/store handler,
  which checks `bits 29:24 == 001000`. But LSE atomics have
  `bits 29:24 == 111000` — a completely different encoding group.
  The LSE atomics code was never reached; every LSE atomic was a NOP.

  Fix: moved the LSE atomics handler to the top level (before all
  load/store handlers), with proper bit checks to distinguish LSE
  atomics from regular load/store encodings that share bits 29:24 ==
  111000:
    - `bit 21 = 0` (load/store reg-offset has bit 21 = 1)
    - `bits 11:10 = 00` (LSE atomics fixed bits)

- **CAS argument order**. In `CAS <Rs>, <Rt>, [<Rn>]`, **Rs** is the
  comparand and **Rt** is the new value. The previous code had these
  swapped (used Rt as comparand, Rs as new value), which would have
  broken every CAS even if the handler had been reachable.

### Fixed (minor, from 1.1.0-alpha.1 carryover)
- **mremap** now grows mappings in-place by mapping additional pages
  at `old_addr + old_size`. Previously, mremap always allocated new
  memory + copied, which broke musl's meta_area tracking (musl expects
  mremap to grow mappings in-place when possible).
- **mmap MAP_FIXED zeroing**: when mmap is called with MAP_FIXED over
  existing pages, the old data is now zeroed out (matching Linux kernel
  behavior).
- **mmap MAP_FIXED hint handling**: the `addr` hint is now only honored
  when `MAP_FIXED` (0x10) is set. Without MAP_FIXED, the bump allocator
  picks a fresh address.
- **getppid** syscall added (was missing entirely, returned -ENOSYS).
- **set_tid_address** / **gettid** now properly per-thread.

### Changed
- **Stack size** increased from 8 MB to 64 MB.
- **Stack top** moved from `0x7ff0000000` to `0x8000000000` (avoids
  a 1-page guard-region conflict that was causing unmapped reads
  during deep musl recursion).
- **LSE atomics opcode table** corrected: each opcode is a distinct
  operation (0=LDADD, 1=LDCLR, 2=LDEOR, 3=LDSET, 4-7=SMAX/SMIN/
  UMAX/UMIN, 8=SWP, C-F=CAS variants). Previously, opcodes 0-3 were
  all treated as LDADD, 4-7 as LDCLR, etc.

### Known Issues
- **musl-static toybox** now gets past mallocng init but exits with
  code 1 on a null pointer dereference (PC=0). This is a different,
  simpler bug than the mallocng loop — likely a signal delivery or
  function-return issue. Investigation in 1.1.0-rc.1.
- **glibc 2.36+ static binaries** exit 1 instead of hanging or
  exiting 133. Same root cause as toybox — gets further but hits a
  null pointer.
- **No FP/SIMD arithmetic**, **no signal delivery**, **no dynamic
  linking** — unchanged.

### Compatibility Matrix
| Binary | 1.0.0-beta.1 | 1.1.0-alpha.1 | 1.1.0-beta.1 |
|--------|--------------|---------------|--------------|
| `hello.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `count.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `fib.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `cat.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `echo.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `repl.elf` (assembled) | ✅ Works | ✅ Works | ✅ Works |
| `hello_arm64_musi` | ✅ Works | ✅ Works | ✅ Works |
| `hello_arm64_static` (glibc) | ⚠️ Exits 133 | ❌ Hangs (brk loop) | ⚠️ Exit 1 (gets further) |
| `toybox-aarch64` | ❌ Hangs (atomics) | ❌ Hangs (mallocng) | ⚠️ Exit 1 (**past mallocng!**) |

### Next up (1.1.0-rc.1)
- Debug the PC=0 crash in toybox. Likely a function return issue —
  trace what's at the top of the stack when PC becomes 0, check if
  a `RET` is reading a corrupted LR or if a function pointer is NULL.
- Implement signal delivery (`rt_sigaction` + `rt_sigreturn` +
  trampoline page). Some musl init paths install signal handlers
  and expect them to work for SIGSEGV / SIGSYS.
- Audit existing 100 instructions for correctness bugs (NZCV flag
  edge cases, sign-extension issues).
- Implement FP/SIMD arithmetic (`FADD`/`FMUL`/`FCVT`/`FCMP`).

---

## [1.1.0-alpha.1] — 2026-06-17

First alpha toward the 1.1 "stability" release. Three real bugs in the
atomic / memory subsystem are fixed, but glibc-static and toybox still
don't run end-to-end (they get past the previous failure points but hit
a deeper musl-mallocng recursion issue that needs investigation).

### Added
- **Local exclusive monitor** for `LDXR`/`STXR`/`LDAXR`/`STLXR`/`CLREX`.
  Per-CPU monitor state (`excl_tag_valid`, `excl_tag_addr`,
  `excl_tag_size`) tracks the most recent exclusive load. `STXR` now
  succeeds only if the monitor is tagged for an overlapping address
  range, and clears the monitor either way. Any branch, SVC, or
  non-exclusive store also clears the monitor (conservative — real HW
  only clears on conflicting access, but clearing more often is always
  safe). `CLREX` (encoding `0xD503305F`) is now explicitly decoded
  instead of being treated as a NOP.

### Fixed
- **LSE atomics opcode table** (major). The previous code mapped
  opcodes 0-3 to LDADD variants, 4-7 to LDCLR variants, 8-11 to LDEOR
  variants, and 12-15 to LDSET variants — treating the 4-bit opcode
  as if it encoded the A/L ordering suffix. In reality, each opcode is
  a different operation: 0=LDADD, 1=LDCLR, 2=LDEOR, 3=LDSET, 4-7=
  SMAX/SMIN/UMAX/UMIN, 8=SWP, 12-15=CAS variants. The old code
  computed `a + b` for what should have been `a & ~b` (LDCLR), causing
  musl's lock bit to never be properly cleared.
- **CAS detection**. The old code used a partial mask that never
  matched real CAS instructions. CAS is encoded within the LSE atomic
  ops space (bits 29:24 = 111000) with opcodes 0xC-0xF. Now detected
  by checking `atom_opcode >= 0xC` before the LSE switch. CAS
  semantics: load old value, compare against Rt, store Rs if matched,
  always return old value in Rt.
- **mmap MAP_FIXED handling**. The previous code honored the `addr`
  hint unconditionally, returning the same address musl asked for even
  when `MAP_FIXED` wasn't set. This caused musl's malloc to think each
  `mmap(heap_end, ...)` succeeded without actually getting new memory,
  leading to a 4KB-at-a-time heap growth loop. Now: `addr` is only
  honored when `MAP_FIXED` (0x10) is set; otherwise we ignore the
  hint and use the bump allocator (matching Linux kernel behavior).

### Changed
- `set_tid_address` now stores the pointer per-thread and returns the
  calling thread's TID (was returning 1 unconditionally).
- `gettid` returns the guest TID of the calling thread (was returning
  `getpid()`).

### Known Issues
- **musl-static toybox** still hangs. The previous failure (LDXR/STXR
  always succeeding) is fixed, but toybox now hits a different bug:
  musl's mallocng enters a deep recursion (SP drops ~54KB per
  iteration) when allocating memory during `__libc_start_main`. Each
  iteration calls `brk()` to extend the heap by 4KB and recurses
  further. Likely cause: musl's "growable array" tracking structure
  isn't being updated correctly, possibly due to a subtle memory
  ordering or atomic semantics issue we haven't pinned down yet.
- **glibc 2.36+ static binaries** now hang instead of exiting 133.
  The mmap MAP_FIXED fix changed the address glibc receives, and the
  getrandom vDSO assertion is no longer triggered — but glibc enters
  its own brk loop before reaching main(). Probably related to the
  same mallocng-style issue as toybox.
- **No FP/SIMD arithmetic**, **no signal delivery**, **no dynamic
  linking** — unchanged from 1.0.0-beta.1.

### Compatibility Matrix
| Binary | 1.0.0-beta.1 | 1.1.0-alpha.1 |
|--------|--------------|---------------|
| `hello.elf` (assembled) | ✅ Works | ✅ Works |
| `count.elf` (assembled) | ✅ Works | ✅ Works |
| `fib.elf` (assembled) | ✅ Works | ✅ Works |
| `cat.elf` (assembled) | ✅ Works | ✅ Works |
| `echo.elf` (assembled) | ✅ Works | ✅ Works |
| `repl.elf` (assembled) | ✅ Works | ✅ Works |
| `hello_arm64_musi` | ✅ Works | ✅ Works |
| `hello_arm64_static` (glibc) | ⚠️ Exits 133 | ❌ Hangs (brk loop) |
| `toybox-aarch64` | ❌ Hangs (LDXR/STXR) | ❌ Hangs (mallocng recursion) |

### Next up (1.1.0-beta.1) — *completed in 1.1.0-beta.1*
- ~~Investigate the musl mallocng recursion~~ — root cause found:
  the LSE atomics handler was unreachable due to an encoding-group
  mismatch (bits 29:24 == 001000 vs 111000). Fixed in 1.1.0-beta.1.
- Audit existing 100 instructions for correctness bugs (NZCV flag
  edge cases, sign-extension issues). — *still pending*
- Implement signal delivery (`rt_sigaction` + `rt_sigreturn` +
  trampoline page). — *still pending*
- Implement FP/SIMD arithmetic (`FADD`/`FMUL`/`FCVT`/`FCMP`). — *still pending*

---

## [1.0.0-beta.1] — 2026-06-17

First tagged release. The interpreter is stable enough to run musl-static
ARM64 binaries and a small set of glibc-static binaries; threads and the
event-loop syscalls are in place but not yet battle-tested against real
multi-threaded workloads.

### Added
- **Versioning**: `1.0.0-beta.1` semver string, `--version` flag, `VERSION`
  constant exported from `arm64_emu.hpp`.
- **Threading support** (the big one):
  - `clone()` syscall now spawns real OS threads instead of returning
    `-ENOSYS`. Supports `CLONE_VM`, `CLONE_SETTLS`, `CLONE_PARENT_SETTID`,
    `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`. Fork-style clones
    (no `CLONE_VM`) still return `-ENOSYS`.
  - `futex()` is now a real implementation using per-address
    `(mutex, condvar)` pairs. Supports `FUTEX_WAIT`, `FUTEX_WAKE`,
    `FUTEX_WAIT_BITSET`, `FUTEX_WAKE_BITSET`, `FUTEX_REQUEUE`,
    `FUTEX_CMP_REQUEUE` (treated as wake). PI futexes return `-ENOSYS`.
  - `Memory` class is now thread-safe (per-page mutex on access).
  - `Emulator` now owns a pool of `GuestThread`s, each with its own
    `CPU` state and running on a real `std::thread`. The main thread
    runs on `main_cpu_`; cloned threads run on the pool.
  - `set_tid_address()` now stores the pointer per-thread and returns
    the calling thread's TID.
  - `gettid()` returns the guest TID of the calling thread.
- **Event-loop syscalls** (delegate to host kernel):
  - `eventfd2` (#19), `epoll_create1` (#20), `epoll_ctl` (#21),
    `timerfd_create` (#85), `timerfd_settime` (#86), `timerfd_gettime` (#87),
    `pselect6` (#72), `ppoll`/`poll` (#168), `socketpair` (#199),
    `listen` (#201), `accept` (#202), `clock_nanosleep` (#206),
    `rt_sigreturn` (#133), `sendfile` (#40 — see Known Issues).
- **Memory atomicity primitives**: `Memory::atomic_cas_32` and
  `atomic_cas_64` for use by LSE atomics and futex implementations.
- **GitHub-ready**: `LICENSE` (public domain / Unlicense), `.gitignore`,
  `CHANGELOG.md`.
- **Build**: Now requires `-pthread` (already in the documented build
  command).

### Changed
- **Rename**: project renamed from `bifrost` to `bifrost-emu` for SEO.
  The binary, namespace comments, banner, and CLI error messages all
  reflect the new name.
- **BRK is fatal**: `BRK #imm` now ends emulation with exit code 133
  (`128 + SIGTRAP`), matching real Linux behavior. Previously it was
  skipped, which caused libc's `abort()` to fall through and dump
  random `.rodata` strings to stderr.
- **`Emulator::execute`** and **`Emulator::syscall`** now take a `CPU&`
  parameter instead of operating on the implicit `cpu_` member. This
  was necessary for multi-threading and also makes a future JIT
  (sharing the same decoder) easier to drop in.
- **Verbose output** uses `[bifrost-emu]` prefix instead of `[emu]`.
- **Instruction trace** now includes the guest TID: `[trace tid=1]`.

### Known Issues
- **Syscall number collisions**: a handful of legacy case labels in the
  syscall switch use numbers that conflict with the real AArch64 syscall
  table (e.g., `case 22` is labeled `pipe2` but is actually
  `epoll_pwait` on AArch64). These work for the test programs because
  musl hello / toybox happen not to use the conflicting syscalls, but
  real-world binaries that exercise `epoll_pwait`, `ppoll`, or
  `sendfile` (real #71) may misbehave. **Plan for v1.0.0-rc.1**: audit
  and renumber all syscalls against `linux/unistd.h`.
- **glibc 2.36+ static binaries** still hit the `getrandom` vDSO
  assertion during libc init and exit with code 133. musl-static
  binaries work perfectly.
- **No signal delivery**: `rt_sigaction` and `rt_sigprocmask` are
  no-ops; signals cannot actually be caught. `tgkill` / `tkill` return
  success but deliver nothing. This means timer-based code, segfault
  handlers, and `pthread_kill` will not work.
- **No FP/SIMD arithmetic**: `FADD`, `FMUL`, `FMLA`, etc. are stubbed.
  Loads/stores of FP/SIMD registers work, but any computation will
  produce garbage. This blocks most games and many numeric libraries.
- **No exclusive monitor**: `LDXR`/`STXR`/`LDAXR`/`STLXR` do not track
  exclusive reservations. `STXR` always reports success. This causes
  musl's malloc to loop forever in `toybox` (see issue tracker).
- **`bind` and `connect` return `-ENOSYS`**: marshalling `sockaddr`
  from guest memory safely requires knowing the address family, which
  we don't currently parse.

### Compatibility Matrix
| Binary | Status | Notes |
|--------|--------|-------|
| `hello.elf` (assembled) | ✅ Works | |
| `count.elf` (assembled) | ✅ Works | |
| `fib.elf` (assembled) | ✅ Works | |
| `cat.elf` (assembled) | ✅ Works | |
| `echo.elf` (assembled) | ✅ Works | Interactive, raw TTY |
| `repl.elf` (assembled) | ✅ Works | Line-buffered |
| `hello_arm64_musi` | ✅ Works | Full musl static |
| `hello_arm64_static` (glibc) | ⚠️ Exits 133 | getrandom vDSO assertion |
| `toybox-aarch64` | ❌ Hangs | musl malloc loop (atomics) |

### Internal Architecture Notes
- **JIT preparation**: the `execute()` and `syscall()` methods now take
  a `CPU&` parameter, which is the only state they touch (plus the
  shared `Memory`). A future v2.0 JIT can compile from the same decoder
  tables by emitting code that operates on a `CPU*` argument.
- **Memory locking**: per-page mutex on every read/write. This is
  conservative; for v1.1 we can switch to a read-write lock and skip
  locking entirely for instruction fetches (which are read-only and
  never race with themselves).

---

## [0.9.0] — 2026-06-16 (pre-release, untagged)

Initial development version. Single-threaded, no futex, no clone.
- Working interpreter for ~100 ARM64 instructions
- musl-static hello world works
- glibc-static hello world fails on getrandom vDSO assertion
