# Changelog

All notable changes to **bifrost-emu** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
with pre-release tags (`-beta.N`, `-rc.N`) for unstable versions.

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
