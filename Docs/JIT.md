# JIT guide

How the JIT turns AArch64 code into x86_64 code, and what to do when you change it. Read this before working on the JIT, the decoder or the IR.

- [JIT history](archive/JIT-history.md): why things are the way they are, with measurements and things that didn't work. Read the entry for what you're changing.
- [Source/FrostJIT/AGENTS.md](../Source/FrostJIT/AGENTS.md): the rules that must not be broken, and the tests for them.
- [The old, long guide](archive/JIT-guide-2.0.0-beta.3.md): notes on single instructions and old bugs.
- [Profiling](PROFILING.md), [speed](PERFORMANCE.md) and the [roadmap](roadmap.md).

## Overview

### Translation path

```text
AArch64 word
  → decode() / DecodedInst
  → translate_to_ir() / typed IR
  → optimize_ir() and validate_ir_block()
  → compile_ir_inst() / x86_64 code
  → publish and chain
```

| What | Where (in `Source/FrostJIT/` unless noted) |
|---|---|
| Decoder and interpreter, shared by both engines | `Source/Interpreter/` |
| IR operations, parameters, factories, validation | `IR/IR.hpp`, `Parameters.hpp`, `Factories.hpp`, `Passes/Validate.cpp` |
| Translating to IR and optimizing it | `IR/Translate/`, `IR/Passes/Optimize.cpp`; scalar SSA in `IR/SSA/` (off by default) |
| Building a block | `Compiler/BlockCompiler.cpp`, `BlockAnalysis.cpp`, `SideExit.cpp`, `RegisterPassing.cpp` |
| Writing x86_64 code | `CodeGen/Integer/`, `CodeGen/FloatSIMD/`, `CodeGen/Native/`, `Backend/X86Emitter.cpp` |
| Register allocation | `RegisterAlloc/RegisterAllocator.cpp`; vectors in `CodeGen/FloatSIMD/VectorCache.cpp` |
| Carry's profile | `Carry/HotSide.cpp` |
| Running, publishing and invalidating code | `Runtime/Dispatch.cpp`, `CodeCache.cpp`, `Compilers.cpp`, `FaultHandler.cpp`, `Generations.cpp`, `Fork.cpp` |
| Disk cache | `Runtime/CacheRecording.cpp`, `PersistentCache.*` |

### Lean and Carry

- **Lean** compiles one block at a time, up to 32 instructions, or 64 for a loop body. The history calls it "Tier-1".
- **Carry** adds to Lean: blocks can go on past a forward branch, and registers are handed from one block to the next. It's on by default; `BIFROST_CARRY=0`
  turns it off.

The interpreter decides what's correct. It also runs any instruction the JIT can't.

### Single-threaded and multithreaded code

While the program has one thread, blocks are connected by patching jumps. When a second thread starts, that code is thrown away once. From then on,
published code never changes, and anything that has to change is kept in data the code reads.

### Host registers

RBX points to the CPU state, R10 to the start of guest memory, and RBP to the frame. R10 is loaded once when a chain of blocks is entered. Ten registers can be
allocated: RAX, RCX, RDX, R8, R9, R11 and R12 to R15. XMM3 to XMM15 hold guest vectors, and XMM0 to XMM2 are scratch.

## Adding or changing an instruction

1. Make `decode()` recognise it and read every field.
2. Get the interpreter right. An unsupported instruction has to raise an error, not do nothing.
3. Add the opcode to `IR.hpp`, its parameters to `Parameters.hpp`, its factory and reader to `Factories.hpp`, and its rules to `Validate.cpp`.
4. Translate it through the factory. Don't change `insts.back()` afterwards; forward skips use `set_skip_count()`.
5. Write the code generation in the matching `CodeGen/` directory. Watch for source and destination being the same register, zero-extension of W registers, the
   upper half of a vector when Q is 0, and CPUs without a feature.
6. Keep `BlockAnalysis.cpp` in step: it predicts which instructions fall back to the interpreter.
7. Think about chained blocks, loops and several threads. Add a small test and register it in `Scripts/Testing/run_tests.sh`.

`Tools/CodeGen/` generates the encoding tables. Change the spec, not the generated header. SIMD helpers come from `native_simd.txt` (`make nativegen`), and a
generated test compares every accepted encoding with QEMU's results.

## Guest state

**Vectors.** Each register in `CPU::v[32]` is 16 bytes and is moved in one piece. Don't load 16 bytes that were just written as two 8-byte halves: the CPU
can't forward that. A 64-bit vector result is zero-extended and stored as 16 bytes.

**Flags.** `CPU::nzcv_host` holds the flags the way `lahf; seto al` leaves them: N in bit 15, Z in bit 14, the inverted carry in bit 8 and V in bit 0. Loading them back
only changes RAX. Flags stay in the host's own flags as long as nothing needs them elsewhere. A block may skip saving them only if the next block overwrites all four
before anything can fault, because a signal handler must see them. In C++, use `nzcv()` and `set_nzcv()`.

**PC.** `cpu.pc` is right at the dispatcher, at exits and wherever C++ might read it. Inside chained code it's out of date. A block that calls C++ stores its own address
when it's entered.

## Blocks

`translate_block` reads instructions until it reaches a branch, the size limit, the start of another block, or its limit of two calls or two interpreter fallbacks. A loop
that jumps back to its own start stays in one block.

### Exits and chaining

- A direct exit is a slot. With one thread it's patched into a jump to the next block, past its prologue. With several threads it jumps through an `MtEdge`, which
  holds the target's address.
- Returns and other indirect branches look up a table that each thread has (1,024 to 65,536 entries).
- Rarely used code, such as page checks and fault exits, exists once per JIT and is shared (`emit_cold_stubs`).
- System calls are run from C++ after leaving the block, so a thread that blocks in one isn't inside translated code. Graphics and audio calls stay inside.

### Loops

#### Loop pins

A loop keeps up to four guest registers in R12 to R15 for its whole run (`select_arch_pins`). Instructions read and write them there directly.
`BIFROST_NO_PIN_INPLACE=1` turns the direct use off. Memory can be behind these registers, so every exit and every fault writes them back. Tests:
`jit_selfloop_pins`, `jit_pin_inplace`.

#### Mid-block exits

A `cbz`, `cbnz`, `tbz`, `tbnz` or `b.cond` that leaves such a loop becomes an exit in the middle of the block (`SideExit.cpp`). Its code is placed after the block and uses the
registers as they were at the branch. With one thread each exit has a stub entry in `blocks_`; with several it's one more edge. These blocks aren't checked by differential
replay and aren't stored in the disk cache. Test: `jit_side_exit`.

### Carry formation

With Carry, any forward conditional branch can become a mid-block exit, up to six in a block. The block continues with the instruction after the branch.
Backward branches still end it. It's off while the disk cache is recording. For an Android app it's "JIT Carry" on the settings page.

### Following the hot side of an exit

A block like that counts how often each exit is taken. After 1,024 runs, `carry_trip` picks the first exit that's taken more than half the time and throws the block away, and the new
translation follows that side instead. A branch is flipped at most twice and a block is redone at most three times. Test: `jit_carry_follow`.

### Register passing

When a block ends, some guest registers are still in host registers. The exit records which ones (its layout, up to seven). If the next block needs exactly those, it's entered past
its loads. If some are missing, a small stub loads them first. Memory is always up to date as well, so faults and signals work as before. It isn't used for loops,
`BIFROST_JIT_VERIFY`, recording or indirect exits. `BIFROST_NO_REG_PASS=1` turns it off. Test: `jit_reg_pass`.

### Register allocation and peepholes

A value's register is free after its last use, and when registers run out, the value needed furthest in the future is evicted. `BlockCompiler.cpp` decides in advance which
instructions to fold together, and the code generator checks the same conditions again. If the two disagree, a value can end up nowhere.

## Guest memory

### Guards

A normal load or store checks the address against the end of guest memory and then accesses it directly. Guest page protection is the host's own, so a forbidden access faults and is
handled. Android's pointer tags are removed from a copy of the address. A second access to an address that was just checked skips the check (`scan_covered_guards`).
Pairs check everything first.

### Registered native memory faults

The signal handler only accepts a fault from a known instruction, inside a `FaultScope`, with the right memory base and an address in guest memory. It then jumps to that
instruction's recovery code, which puts the guest state back and runs the instruction in the interpreter. Each 256 KiB chunk of code has its own sorted list of
these instructions. Test: `jit_host_fault`.

### Software page checks

One byte in front of guest memory switches all accesses to a slower check. It's only set while a page is registered with userfaultfd and missing, because the host's protection can't
show that. Tests: `test_userfaultfd`, `test_mem_guard`, `jit_guard_stubs`.

### Atomics and exclusives

LSE atomics are native. `LDXR` and `STXR` follow one protocol everywhere (`ExclusiveMonitor.hpp`), and the pair versions run in the interpreter. A store must reach memory
exactly once.

### Guest memory barriers

`DMB LD` and `DMB ST` need no instruction on x86. A full `DMB` and any `DSB` is one `lock not`. The barrier stays in the IR either way, so nothing is moved across it.
Test: `test_atomic_stress`.

### Host-native memcpy, memmove and memset

The loader finds the guest's own routines by name. A call for 1 KiB or more runs the host's version, if all the memory is accessible. Otherwise the guest's routine runs, so faults happen
where they always did. Stripped libraries have no names, so it doesn't work for those. `BIFROST_NO_NATIVE_LIBC=1` turns it off. Tests: `jit_native_libc`, `test_native_libc_mt`.

## Floating point and SIMD

- NaNs follow Arm's rules. When a result is NaN, the instruction runs again in the interpreter, in code placed after the block. FPCR.DN works; FZ, rounding modes,
  FPSR flags and floating-point traps don't apply to native arithmetic.
- Fused operations use one FMA3 instruction.
- `fcmp` followed by `b.cond` is one compare and one or two jumps.
- Vectors stay in XMM registers within a block when every operation allows it.
- `LD2` to `LD4`, `ST2` to `ST4` and `LD1R` are native. If one faults, no register has changed.
- The other SIMD instructions call helpers in `CodeGen/Native/`. They must preserve R10.

## Multithreaded code

### Publication

Other threads may be running a block while it's replaced, so only data changes, never code. Removing a block clears the addresses that lead to it. Its code stays until the whole cache
is released. Tests: `jit_mt_loops`, `jit_mt_publication`.

### Preemption

`g_jit_preempt_epoch` changes every millisecond, and whenever code is invalidated. Backward jumps and indirect branches compare it and return to the dispatcher if it changed.
Single-threaded code counts 256 jumps instead. `cpu.jit_stop_pc` is checked once when a loop is entered.

### Several compilers at once

Several threads can translate at the same time (`Runtime/Compilers.cpp`). The **core** is the main `FrostJIT` object, which owns the code buffer and tables. A **compiler** is another
`FrostJIT` object that uses them. A thread takes a compiler, translates without a lock, and publishes the result under the table lock (`adopt_staged`). There's one
compiler per CPU, at most 16, and a thread gets the one it had last if it's free.

- Everything a compile creates stays in its compiler until it's published. Generated code never refers to a compiler, only to the core.
- Use `with_table` to read the core's tables. Two filters (`block_starts_` and `carry_tripped_`, both `PcFilter`) say without a lock that an address isn't in a table, which is the usual answer.
- `mt_targets_` has one record per jump target, with its own small locks. A compile finds its records before it takes the table lock.
- Publishing is the only part that can't overlap, so as much as possible is prepared before it.
- The tables grow in pieces (`GrowingTables.hpp`, `CodeIndex.hpp`), because growing one big table used to stop every thread for 26 ms.
- Each compiler writes into its own 256 KiB chunk of the code buffer.
- If code is invalidated while it's being translated, it's translated again under the table lock.
- If two threads need the same block, the second one waits.
- There's only one compiler with `BIFROST_JIT_COMPILERS=1`, per-thread JITs, `BIFROST_JIT_VERIFY`, the disk cache, or a small code cache.

Test: `jit_mt_compile` ([numbers](archive/JIT-history.md#several-threads-translate-at-once-2026-11-25)).

## The code cache

### Invalidation

Whoever invalidates code gives a reason. Cache maintenance covers exact 64-byte lines, and mapping changes cover pages. After cache maintenance or `mprotect`, a block is kept if its
instructions are the same and still executable. `CodeIndex` finds the affected blocks, so changes to plain data don't stop other threads. Test: `jit_cache_invalidation`.

### A full cache

The cache uses 512 MiB by default (`BIFROST_JIT_CACHE_MB`). When it's full, a new one replaces it, and the old one is released when no thread can still be using it
(`Runtime/Generations.hpp`). Only one old cache can wait to be released. Until then, a full new one falls back to the interpreter. Tests: `jit_cache`, `jit_generations`.

### Persistent translation cache

Experimental and off by default (`--jit-disk-cache`). It saves Lean translations of code that comes from files. Each block is compiled twice, at two addresses, and is only saved if every
difference can be explained. Blocks with mid-block exits aren't saved. Test: `jit_disk_cache`.

### Fork children keep the JIT

A forked child keeps its parent's translations. `fork_guest` takes all the JIT's locks or none, and no compiler may be busy. If it can't get them in 3 seconds, the child runs in the
interpreter until it calls `execve`. Test: `test_fork_jit`.

## Planned codegen: retaining guest state across regions

Not built yet. The goal is Lean first, Carry when hot: new code gets the quick translation, and only code that runs a lot gets the better one. Today everything gets
Carry at once, and code that runs once pays for it (9.0 µs a block instead of 4.8 µs when Android starts, 2026-11-25).

After that come regions: loops of several blocks that keep their registers the whole time. Decided so far:

- Find hot code by sampling or by counting backward jumps. No counters at block entries and no timed trials.
- Write registers back only at exits, calls and faults. Each of those needs to know which register is where.
- With several threads, switch to a region by changing one address. Regions aren't saved to disk at first.
- Tests force a region to be built and check its results, and never depend on timing.

[What it could gain at most](archive/JIT-history.md#carry-ceiling-guest-register-moves-in-coremark-2026-11-24), and the [roadmap](roadmap.md).

## Checking a change

```bash
make verify                                                       # compares with the interpreter, and runs the threaded tests
./Scripts/Testing/run_tests.sh --strict --filter 'jit|simd|fp'    # a part of the tests
BIFROST_IR_VALIDATE=1 ./bifrost-emu program                       # checks the IR
BIFROST_JIT_VERIFY=1 ./bifrost-emu program                        # compares every block with the interpreter (one thread)
./bifrost-emu --no-jit program                                    # interpreter only
```

The comparison with the interpreter stops when a second thread starts, and says so. Threaded code is checked against known results instead.

### Measuring

- Count instructions first (`perf stat -e instructions:u`), because that number is stable. Cycles can change by 15% just from where the code lands, so try several placements.
- To see which instructions cost time, use `perf record -e ibs_op/cnt_ctl=1,swfilt=1/u` with `BIFROST_JIT_DUMP=1`.
- For translation time, look at the `[JIT] compile:` line of `-v`. `Scripts/Android/test_android_jni.py` is a good test that opens no window.
- For CoreMark, set the CPU governor to `performance`, run for at least ten seconds and check the CRC.
