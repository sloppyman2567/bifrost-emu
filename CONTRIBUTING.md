# Contributing to bifrost-emu

Thanks for your interest in improving bifrost-emu! This is a small
project and we welcome contributions of all sizes — bug reports, test
cases, instruction implementations, syscall handlers, docs, and
architectural improvements.

## Project status

bifrost-emu is in **beta** (`1.0.0-beta.x`). The interpreter is stable
enough to run musl-static ARM64 binaries; threads and event-loop
syscalls are in place but not yet battle-tested against real-world
multi-threaded workloads.

The v2.0 roadmap includes a JIT compiler, FP/SIMD arithmetic, an
exclusive monitor for `LDXR`/`STXR`, signal delivery, and (eventually)
enough graphics/audio/input support to run simple ARM64 games.

## How to help

### Bug reports

When filing an issue, please include:

1. The exact binary you tried to run (and how it was built — `musl-gcc`,
   `aarch64-linux-gnu-gcc -static`, etc.).
2. The full `bifrost-emu -v` output (verbose mode includes instruction
   counts and memory usage).
3. If the program hangs or crashes, a short instruction trace from
   `bifrost-emu -d` (last ~50 lines is usually enough).
4. What you expected vs. what you got.

### Pull requests

Small, focused PRs are easier to review than mega-PRs. Some good
first issues:

- **Instruction implementations**: any unhandled instruction in
  `arm64_emu.cpp`'s `execute()` will throw a `DecodeError`. Pick one,
  implement it, add a test in `test/`, and submit.
- **Syscall handlers**: same idea — unhandled syscalls currently
  return `-ENOSYS`. Pick one from the Linux AArch64 syscall table,
  implement it (usually just delegating to the host), add a test.
- **Test programs**: the `test/` directory is small. More test
  programs that exercise specific code paths are very welcome.

### Code style

- C++17, no exceptions in the hot path (the interpreter loop).
- 4-space indent, no tabs.
- `snake_case` for variables and functions, `PascalCase` for classes.
- Comments explaining *why*, not *what*. The decoder is dense; future
  readers will thank you.
- Keep methods short. The `execute()` function is already enormous; if
  you add a new instruction group, consider extracting a helper.

### Building and testing

```bash
# Build
g++ -O3 -std=c++17 -pthread -o bifrost-emu main.cpp arm64_emu.cpp

# Run the test suite (just runs each test/ program and checks output)
for f in test/*.elf; do
    echo "--- $f ---"
    ./bifrost-emu "$f" || echo "FAILED"
done

# Assemble new test programs
python3 mini_arm64_asm.py mytest.s -o mytest.elf
```

### Debugging tips

- `-d` traces every instruction to stderr. Filter with `grep` to find
  specific patterns (e.g. `grep "BRK"` to find assertion traps).
- `-v` prints execution stats on exit (instruction count, MIPS, memory
  usage).
- For hangs: `kill -9` the process, then look at the last few trace
  lines to see where it's stuck.
- For crashes: the `DecodeError` and `UnmappedMemory` exceptions
  include the PC and (for decode) the offending instruction word. Use
  `aarch64-linux-gnu-objdump -d` to disassemble around that address.

### Architecture overview

```
main.cpp            CLI entry point, banner, terminal raw mode
└─ arm64_emu.hpp    Single-header with all class definitions
   ├─ Memory        Sparse paged, thread-safe
   ├─ ElfLoader     Static AArch64 ELF loader
   ├─ CPU           Register file + PSTATE + SIMD/FP regs + TLS regs
   └─ Emulator      Owns Memory + main CPU + thread pool + futex table
└─ arm64_emu.cpp    Instruction decoder (execute) + syscall layer
```

The decoder is a flat switch on the top bits of the opcode. It's not
the fastest possible dispatch, but it's the most readable and the
easiest to extend. The v2.0 JIT will share the same decoder tables via
a `decoder.hpp` header (planned).

### Roadmap (v2.0 and beyond)

1. **JIT compiler** — x86_64 codegen sharing decoder tables with the
   interpreter. Target: 100-500 MIPS (vs. current ~30 MIPS).
2. **FP/SIMD arithmetic** — `FADD`, `FMUL`, `FMLA`, `FCVT`, etc.
3. **Exclusive monitor** — proper LL/SC semantics for `LDXR`/`STXR`.
4. **Signal delivery** — real `rt_sigaction` + `rt_sigreturn`.
5. **Game support** — framebuffer/DRM emulation, audio (ALSA ioctls),
   input (`/dev/input/event*`). Long-term goal: run statically-linked
   ARM64 SDL2 games and retro emulators at playable framerates.

## License

By contributing, you agree that your contributions are dedicated to the
public domain under the terms of the [Unlicense](https://unlicense.org).
