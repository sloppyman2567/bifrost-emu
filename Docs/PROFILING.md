# Profiling and traces

How to see where a program spends its time, and how to trace a crash. Everything here is off by default.

## Where host time goes: `BIFROST_PROF`

```bash
BIFROST_PROF=1 BIFROST_PC_HIST=1 bifrost-emu -v program      # report at exit, with the full JIT stats
BIFROST_PROF=1 bifrost-emu program                           # the profile report alone, at exit
BIFROST_PROF=1 BIFROST_STATS_PERIOD=30 bifrost-emu program   # and every 30 s
```

`BIFROST_PROF=1` samples the host pc at 100 Hz of CPU time (`ITIMER_PROF`) and prints, besides the dispatch / translate /
interpreter / other split, one line for the JIT samples: how many, on how many threads, how many were lost, and how they divide
into the parts of a translated block. **Entry** loads the canonical register layout, **body** is the translated instructions,
**exit** publishes state back, **cold** is recovery and exit code (the same four parts as the `entry`, `lean-ir`, `exit` and
`cold` rows of the allocation profile), and **shared stubs** is code outside any registered block: the frame-exit and cold-path
stubs, and blocks loaded from the persistent cache, which are not registered. Below it come the busiest eight threads, and with
`BIFROST_PC_HIST=1` the twenty hottest guest blocks (`pc=` values map to functions with `aarch64 objdump`) and the loop report.

**Loop report (`[carry]`).** With `BIFROST_PC_HIST=1` each translated block also records the guest registers it reads before writing and
the ones it writes (`note_carry_block`, `Carry/LoopReport.cpp`). At the report the 200 hottest blocks are joined with their static
successors (the chain targets), and the cycles among them are printed: one line saying how much of the JIT samples sit in loops of several
blocks, in single-block loops (Lean already pins their registers) and elsewhere, then the ten hottest multi-block loops with their head pc,
blocks, guest instructions, share of the JIT samples, distinct guest registers (read, written), the register moves one pass over all
its blocks costs now, and the static edges that leave it (counted to cold blocks too). A loop through a call or a return has no static
edge and does not appear; the "elsewhere" share holds it. It reads what the sampler and the block table already hold and changes no code.
`carry_report` checks it on a hand-written four-block loop.

Under the report a second line splits the **body** samples by what the operation under them does: guest state moves
(`LOAD_REG`/`STORE_REG`), memory (a load or store with its guest-address guard, and atomics), integer ALU, flag setting, branches,
helper calls, FP and SIMD, and "between operations" (code in the body that belongs to no single operation). The same line gives
the share of body time that fell inside an operation whose code contains allocator spills or reloads. That share is an upper bound
on spill cost, because it includes the operation's own work. The code that ends a block (the
indirect branch of `ret`, flag and register commits) is in the exit section, not the body. The operation table costs 16 bytes per
IR operation translated while `BIFROST_PROF` is set (capped at 16 Mi operations; a note says when it is full).

Samples go to a per-thread ring of 16,384 and are resolved into running totals whenever a report is printed, when a code buffer
is released and when a new thread finds every ring taken; a thread that ends leaves its ring to the next of these. Resolving
sorts only the blocks and operation ranges registered since the last time and merges them in. A thread that outlives 164 s of samples between two reports overwrites its oldest ones; those
are counted as *lost*, not folded into a bucket, so `BIFROST_STATS_PERIOD` keeps a long run complete. The numbers are time, not
instruction counts: a 10 ms sample says where the host was, not how often a block ran. Sampling and section registration
(`Runtime/SampleProfile.cpp`) run only when the variable is set; otherwise the translator tests one flag per compiled
instruction (its cost was not measured). `Tests/Unit/jit_profiler.cpp` checks the classification at
each section boundary, across threads and after a code buffer is released, with synthetic samples and no timing.

**The report needs no `-v`.** `-v` adds the JIT's own counters (blocks translated and executed, cache use, memory);
the profile report prints at exit whenever `BIFROST_PROF` is set, including when the guest ends through `exit_group`.

**Guest instruction counts.** The `guest: N MIPS` line, the `-v` `executed N instructions` line and the `-v` counters
count what generated code retires, not dispatches. When `-v` or `BIFROST_STATS_PERIOD` is set, the translator emits a
two-instruction counter in every block entry (after the chain-skip entry, so chained edges count too) and in every
self-loop back edge, and the dispatcher folds it into the JIT's totals; without either nothing is emitted and nothing is
paid, so `BIFROST_PROF`'s sampler keeps its profile free of the counters. The counting code runs in the measured run: on
CoreMark it costs about 15 %, so the rate it gives is a floor. The figure is exact for Lean code; blocks served from
the persistent cache (recorded before the counters existed) are not counted.

**Where threads wait.** The sampler counts CPU time, so a thread that waits shows nothing. With `BIFROST_PROF` set, the report
also prints, for each guest thread, how long it spent inside guest syscalls by kind, next to its lifetime and CPU time
(`System/Core/WaitProfile.*`):

```
[bifrost-emu] waiting, 6 thread(s), 3.6 thread-seconds alive, cpu 0.6 s: blocked in futex 0.6 s, poll 0.6 s, sleep 1.2 s; inside io 0.6 s, ...; translating 0.0 s, waiting to translate 0.0 s
[bifrost-emu]   tid 3 "w-futex": alive 0.6 s, cpu 0.1% | blocked: futex 99.9% poll 0.0% sleep 0.0% | inside: io 0.0% ioctl 0.0% thunk 0.0% other 0.0% | jit: translating 0.1% waiting 0.0%, longest 0.0 ms
```

The percentages are shares of the thread's lifetime, counted from its first syscall; the busiest 12 threads by CPU time are listed.
**futex**, **poll** (`ppoll`, `pselect6`, `epoll_pwait`, `wait4`, `waitid`, `rt_sigtimedwait`) and **sleep** (`nanosleep`,
`clock_nanosleep`) are time blocked. **io** (`read`, `write`, their vectored and socket forms, `connect`, `accept`), **ioctl**
(Binder transactions are ioctls), **thunk** (the GL, EGL, SDL and audio thunks, where a buffer swap waits for the display) and
**other** are time inside the call, work and waiting together, so they overlap the CPU share. A thread whose CPU share and
blocked shares are both low is waiting somewhere the report does not see: another lock in the emulator, or being scheduled out.
**translating** is the thread's own translation and publication (CPU time too), **waiting** its wait for the compiler and the block
table, and **longest** its longest such wait. Waiting is for the table lock, for a free compiler, or for another thread that is translating the same block.
**Stall lines.** A freeze is one long wait, which totals hide. Any single call that takes a second or more on a *busy* thread (one
that spent at least 5% of its lifetime on the CPU, so not a pool worker that sleeps until it is needed) prints one line when it ends,
with the time of day in the form of Android's log lines, so it lines up with the app's own log:

```
[bifrost-emu] [stall] 11-19 20:50:41.512 tid 179 "UnityMain": futex took 2.41 s a0=0x7f31a0 a1=0x80 caller=/data/app/.../lib/arm64/libil2cpp.so+0x1a2b3c
```

`a0` and `a1` are the call's first two arguments (for a futex, the address of the lock and the operation), and `caller` is where the
call came from, one level up from the libc stub (`BIFROST_FUTEX_BT` gives a full backtrace). For a graphics thunk `a0` is the symbol
id. A call on a descriptor (`read`, `write`, `sendto`, `recvfrom`, `accept`, `connect` and their relatives, and `ioctl`) also says
what the descriptor is, found when the call ends:

```
[bifrost-emu] [stall] 11-23 09:18:21.184 tid 179 "UnityMain": read took 3.83 s a0=0x2f6 a1=0x7267 fd=758 tcp 10.0.0.2:41234 -> 203.0.113.5:443 caller=...
```

After `fd=` comes the file's path, `pipe:[inode]`, a socket (`tcp` or `udp` with its two addresses, `unix stream` with its paths,
`-` for none), or `virtual Name` for a descriptor the emulator serves itself (Binder, a memfd). `closed` means no thread holds that
number any more. The descriptor is looked up when the call ends, so if another thread closed it and the number was reused during
the stall, the line names the new object. If two stalls end at the same moment, the second waits up to 50 ms for the lookup and
then prints without the name, caller and descriptor.

Each call site (thread, call and caller) prints its first five stalls, and the fifth says it is the last. A loop that sleeps a
second, such as a protector's watchdog, would otherwise use up the log and hide a freeze that comes later. The report then lists
the twelve sites with the most time among those that stalled more than five times, and says how many more there are:

```
[bifrost-emu]   repeated stalls: tid 81 "games.umamusume" nanosleep x179, 179.0 s in all, longest 1.00 s, caller=[anon 0x3212d000-0x32134000 rwx]
[bifrost-emu]   and 2 more call sites with repeated stalls
```

At most 200 stall lines are printed per process (the `repeated stalls` lines are extra). A thread's first counted call can miss
its record when many threads start together, so that thread's first stall may be left out. It adds nothing to the cost of a call that is shorter than a second.

Only the outermost call on a thread is counted. A call that bypasses `Emulator::syscall` is not counted unless it has its own
timer (the JIT's thunk fast path does). The report prints with `BIFROST_STATS_PERIOD` and at exit (also under `--no-jit`), is
cumulative, and costs about 35 ns per counted call while `BIFROST_PROF` is set (a tight `getppid` loop went from 101 to 136 ns);
nothing is measured or paid without it.

## Timeline with Tracy: `BIFROST_PROF=tracy`

The sampler and the waiting report give totals. To see what each thread did at each moment (a render thread waiting for a
swap, a lock another thread held), Bifrost can feed a [Tracy](https://github.com/wolfpld/tracy) timeline. It is optional, and a
normal build contains none of it.

```bash
Tools/Fetch/fetch-tracy.sh              # once: the Tracy 0.15.0 client sources into Tools/tracy (BSD 3-clause, checksum-pinned)
make -j8 USE_TRACY=1                    # build/release-sdl1-gl1-tracy/bifrost-emu; the root bifrost-emu and libbifrost.a stay as they are
BIFROST_PROF=tracy build/release-sdl1-gl1-tracy/bifrost-emu app.elf
BIFROST_PROF=tracy:com.example.game asgard start com.example.game    # an Android app: only that process
```

Connect a Tracy viewer **of the same version (0.15)** to the process; it listens on port 8086, or the next free port. Tracy keeps
nothing until a viewer connects, so connect first: a call already in progress at that moment is not shown. Without a target the
process you started is profiled. With `tracy:NAME` the process whose command line or process name contains NAME is (the name is
also matched cut to 15 characters, as Android names a process after its package). Tracy cannot follow `fork`, so a forked child
never uses its parent's profiler: for an app use a target, because the Zygote forks.

You get one row per guest thread, named after the guest thread; a zone per guest syscall, coloured by kind (futex, poll, sleep, io,
ioctl, other); a zone per GL, EGL or SDL thunk call, named after the function; a zone for each block the JIT translates; a frame at each `eglSwapBuffers` or `SDL_GL_SwapWindow`; and a message when a full code cache is replaced. Vulkan
presents are not marked as frames. Tracy's own system sampling and context-switch tracing are not used. The profiler window is a
separate program built from the same release.

Cost, per syscall in a tight `getppid` loop: no measurable difference in a Tracy build that is not profiling, about 30 ns more with the
profiler started and no viewer, about 55 ns more with a viewer connected. Checked with Tracy's command-line `capture` and `csvexport`
tools built from the same release (zone names, thread names, frames, a fork-heavy program, target matching), not with the profiler
window or an Android game.

`BIFROST_REGALLOC_PROFILE=path` writes a row per emitted IR range (`%p` in the path is the process id). It is large: an Android
game wrote 3.3 GB in 15 seconds. The file now stops at `BIFROST_REGALLOC_PROFILE_MB` (default 256 MiB, counting what the file
already holds, since it is appended to), prints a notice on stderr and leaves a `# cap reached` line; blocks translated after that
are not recorded.

## Fault, block-entry and FWD bisection traces

These opt-in switches are parsed once in `Source/System/Core/DebugFlags.hpp`. Each
trace line is written by a single `fprintf`, so lines from concurrent guest
threads do not interleave.

```bash
BIFROST_FAULT_TRACE=1 ./bifrost-emu app.elf
BIFROST_BLOCK_TRACE=0x400000-0x401000 ./bifrost-emu app.elf
BIFROST_FWD_RANGES=0x400000-0x480000,0x500000-0x500100 ./bifrost-emu app.elf
```

- `BIFROST_FAULT_TRACE` prints one `[fault]` line per delivered
  SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP: thread, signal, si_code, address,
  pc, sp, lr, x0-x3 and the eight instruction bytes at pc (zero when unreadable).
  When a JIT memory guard left its block to interpret the
  faulting instruction, the line adds `jit-block=` with that block's start.
  The matching `rt_sigreturn` prints the resume pc and every general
  register and SP that the handler or ucontext changed.
  It also prints `[clone]` inputs for shared-VM children without
  `CLONE_THREAD` and `[guest-exit]` group termination with the requesting
  thread, code, PC, SP and last syscall.
- `BIFROST_PROCESS_TRACE` prints only those clone/exit records, including
  rejected shared-VM process clones. It does not change the JIT guard
  fallback used by fault tracing.
- `BIFROST_BLOCK_TRACE=lo-hi[,...]` makes blocks starting in those ranges
  log every cold and chained entry: pc, whether FWD ran, NZCV and the raw
  `nzcv_host` image, x0-x7, sp and lr. A
  flag change between two consecutive entries points at the first block.
  Self-loop iterations re-enter past the prologue and are not logged.
- `BIFROST_FWD_RANGES=lo-hi[,...]` restricts FWD to blocks starting in those
  ranges.

Ranges are guest pcs, `[lo, hi)`. They are only stable when the code's load
address is: Android's ART relocates its boot image on every run, even with
`BIFROST_NO_ASLR=1`. To compare two modes, diff their fault and block traces
by method rather than by fixed address ranges.
