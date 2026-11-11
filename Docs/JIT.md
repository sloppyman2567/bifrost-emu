# JIT architecture and contributor guide

This guide maps the AArch64-to-x86_64 JIT and records the steps for changing
instruction support without letting decode, translation, optimization, and
code generation drift apart.

This guide covers how the JIT works now. Dated checkpoints, with the measurements and failures of their revisions, are in
[the JIT history](archive/JIT-history.md) and listed under [Recorded changes](#recorded-changes): read the matching one
before changing that code. The dated sections still here describe mechanisms in use; their scores, like the history's,
are not measurements of the current executable.

The [source map](ARCHITECTURE.md) describes the subsystem layout and split
boundaries. Compiler prediction/leaf analysis lives in `Compiler/BlockAnalysis.cpp`,
Tier-1 orchestration in `BlockCompiler.cpp` and differential store-footprint
analysis in `VerificationStores.cpp`. The refactor's build/test checkpoint is
recorded in [TESTS.md](archive/TESTS-history.md#subsystem-layout-and-source-splits-2026-10-11);
it does not establish a new performance improvement.

## Tier-2 exits that chain, exit-rate feedback and profiler costs (2026-11-11)

A profile of the game-kernel set with `BIFROST_TIER2=1` found that a kept region could cost more than it saved.

- **Region exits always get a cell.** A single-thread region created its exit cell only when the target already had a
  translation, and left through the dispatcher otherwise, for the region's life. Targets on the declined trace next to it
  were being settled (retired and rebuilt) when the region compiled, so the particles region's three side exits all
  returned to C++: 283,220 dispatches over 303 frames against 2,012 on Tier-1. The cell is now created anyway; an empty one
  leaves as before, and translating the target fills it (`chain_back_references`). MT regions always had cells. Particles
  then ran 8,946 dispatches and, per frame at steady state (difference of 200 and 1,200 frames, one pinned core), 7.7% fewer
  cycles and 2.6% fewer instructions than Tier-1, where it had been even in cycles and +4.7% in instructions.
- **Feedback compares exit rates** (rule above, under Tier-2 loop discovery). The trial was right for particles' early
  frames, where almost no particle bounced (16 exits in 128,000 iterations); the rule re-trials a region whose exit rate
  outgrows its trial's. A floor of one exit in 64 retired particles once its bounce rate rose: the new trial picked the
  other rotation of the loop (head `0x4020c8`), which loses, so one run ended in Tier-1. At one in 16, particles keeps
  its region in every run. `jit_tier2_feedback` checks that a steady exit every fourth iteration retires (the old rule kept
  it) and that one in forty does not.
- **Statistics:** the `BIFROST_STATS_PERIOD` line summed only the current JIT generation, so replacing a full code cache made
  its deltas wrap (`18446744073709... new blocks`); it sums every generation now, and a total that still falls (exec, an
  exiting per-thread JIT) prints the new total instead of wrapping. `BIFROST_PROF` no longer re-sorts its block table at
  every thread exit (see Where host time goes).

Game kernels, steady state Tier-2 against Tier-1, all checksums equal to native: instructions per frame particles -1.4%,
collide -7.4% (both keep a region), the other seven within 0.1%; cycles of those seven moved -22% to +31% with identical
instruction counts, which is code placement. CoreMark 200,000: crcfinal 0x4983, Tier-2 keeps four or five regions and runs
197.4-197.7 G host instructions against 199.8 G (-1.1 to -1.2%, as before this change), cycles -2.6% and -3.2% in two
pairs. Checked: `make verify` (106 differential fixtures, 11 threaded oracle runs), the five `test_jit_tier2_*` scripts, the
`jit_`/`tier2` unit entries with Tier-1 (48 pass) and Tier-2 (47 pass; `jit_disk_cache` fails by design). No game was run with
Tier-2.

Not done: loops whose iterations take different arms (cull, survival 0.16-0.37), loops with an inner loop (skinning) and
dispatch loops (vm) are still declined. Admitting them needs regions with internal joins, not a longer linear trace: each
join must be a commit point on both incoming edges, the planner's forwarding and LICM need a barrier there (cull's hot path
sets `w12 = 1` before a join that the culled path reaches with 0), and off-path blocks need their own trial baselines.
Raising the eight-block cap alone admits none of the measured kernels. Tier-2 still spends about 20 M cycles per run on
compiles and trials before it pays.

## Tier-2 through calls and returns (2026-11-11)

Under chain-skip a guest `BL` is a jump that sets X30 and `RET` an indirect exit, and Tier-2 stopped at both: a BL ended the
trace as an unsupported terminator, and a trace ending in RET was refused (its `BR` counted as a call). CoreMark's `crcu32`
(GCC inlined `crc16` and `crcu8` into it: 201 instructions in seven blocks) and `core_bench_state`'s caller loops stopped there.

- **A RET can end a region.** Its block keeps the `BR`; the side exit is `Tier2Trace::kDynamicExit` with the register in
  `dynamic_exit_reg`. The fall-through exit commits every vreg, pin and flag, loads the register's committed value into RAX,
  stores `cpu.pc` and tries Tier-1's indirect-branch probe (`emit_indirect_branch_probe`) before the shared exit tail. The
  planner no longer counts that final `BR` as a call, so such a region keeps pins and chain-skip.
- **The walker follows a BL into its callee** like a direct jump, pushing the return address (at most four deep). A RET that
  matches the top becomes `IMM ret; XOR t = xN ^ ret; BRCOND_ZERO t != 0`: a flag-neutral guard whose taken edge is a dynamic
  exit through xN, while the trace continues at the return address (`follows_taken`, `hot_successor`). A callee already in the
  trace, or deeper nesting, ends the trace with a static exit into it.
- **Trace blocks end where the published Tier-1 block does.** Tier-1 splits a block at every block start compiled since; a
  RET that sometimes returns past its call site creates one, and a trace block spanning it had no matching baseline, so the
  trial was declined.
- `BIFROST_NO_CALLREGION=1` turns all of this off, as it already did native call regions. `test_jit_tier2_measure.sh` and
  `test_jit_tier2_feedback.sh` set it: their single-head scenarios end in RET, which now forms its own neighbouring region.
- `BIFROST_TIER2_TRACE_VERBOSE` also names the previously silent outcomes: an unprofiled retry, a baseline mismatch, an
  unusable trace and a compiler refusal.

`Tests/Unit/jit_tier2_calls.c` (suite entry `tier2_calls`, run with `BIFROST_TIER2=1`) checks a loop whose callee moves its
return address every 1024th call (the guard must leave through X30), nested calls, the same callee twice per iteration and a
branchy leaf; it passes under QEMU, on Tier-1 and with Tier-2, where the guard loop's region is kept at 0.69 of Tier-1's time.

CoreMark, one pinned core, 200,000 iterations, Tier-2 against Tier-1 on the same build: the two CRC regions ending in RET are
kept (`crcu32` 0.86, `0x403404` 0.93, twelve wins each) with the list region and `0x401d70`; 197.51 G host instructions against
199.83 G (-1.2%); three alternating pairs ran 0.7%, 1.4% and 2.9% more iterations per second (cycles -0.8 to -2.3%), each
inside the placement range, so a small gain rather than a measured one. `core_state_transition` still gains nothing: it loops
over each token's characters, so a trace through the call meets a back-edge that is not the head. All CRCs match Tier-1.
Checked: the five `test_jit_tier2_*` scripts, the 47 `jit_`/`tier2` unit tests in both modes (`jit_disk_cache` fails under
Tier-2 by design) and `make verify` (106 differential fixtures). No game was run with Tier-2.

## Tier-2 without a Tier-1 tax, and leaner region code (2026-11-11)

`BIFROST_TIER2=1` made CoreMark slower although it kept no region: 219.37 G host instructions against 199.83 G (+9.8%),
cycles +8.6%. An IBS sample put the difference in the PC store before every chain slot (6.5% of executed ops, plus its
`mov eax, pc`): Tier-2 switched `defer_pc_store` off for all code because the trial wrapper's tails returned `cpu.pc`.

- **The PC store follows the chain slot under Tier-2 too.** The wrapper stands for one head, so its three tails (quota,
  nested trial, cold chain entry) store that head's PC instead of reading `cpu.pc`. A region's chain entry stores its head PC
  once per entry (helpers in the region may read it); the back-edge jumps past it. `defer_pc_store()` is chain-skip alone.
- **Every constituent of a region whose trial does not start is settled.** The fired path disables the counters of all trace
  blocks, but a failure before the trial starts settled only the head, so the others kept their edge profiles and the
  PC-storing chain entry their fire call needs, for the rest of the run.
- **Stale block boundaries are refreshed, not given up on.** Tier-1 ends a block where another block starts, so a block
  compiled before its neighbour can no longer be reproduced: CoreMark's `0x401f80` translates to one instruction once
  `0x401f84` has its own block, not the twelve its trace holds, and the trial's sampling copy failed on every attempt. That
  head and the stale block are now retranslated at the current boundaries without being settled
  (`tier2_refresh_queue_`, at most twice per head through `tier2_retrains_`), so the head fires again with a current trace;
  the other constituents are settled.
- **Trials decide from longer, cheaper windows.** See the trial timing description below: CLOCK_MONOTONIC instead of the
  thread CPU clock, 100 us pools instead of 10 us, twelve pairs with nine wins instead of eight with six.
- **Region code.** W-form fusion and the one-instruction `LDRSB/LDRSH/LDRSW` run in regions as in Tier-1
  (`scan_w32_and_sext_loads`, one pre-scan for both; `fp_load_`, `lazy_load_` and `scaled_add_` stay Tier-1-only because
  they need its per-op materializers). `CBZ`/`CBNZ`/`TBZ`/`TBNZ` terminals test the operand where it is held: they copied it
  into RAX first, and `TBZ` used `BTS` on the copy. Region exits store the PC once instead of twice.

CoreMark, one pinned core, same build, 200,000 iterations: Tier-1 is unchanged (199.83 G host instructions). Before the trial
changes Tier-2 executed +0.6% (counters of lukewarm blocks and trial runs) and kept no region; after them it keeps the
list-search region (`0x400f58`, two blocks, about 18 host operations per iteration against Tier-1's 27) in every run
(medians 0.89 to 0.95, 11 or 12 of 12 wins) and rejects `0x401010` (1.05 to 1.06, no wins), where the old rule flipped
between runs. Tier-2 then executes 197.70 G (-1.1% against Tier-1); its cycles were +2.9% and -0.8% in two runs, inside the
placement range measured below, so no speedup is claimed. The loop chases a linked list, so it runs close to load latency and
fewer operations buy little; that is an inference from the ratios, not a measured latency. The matrix inner loop stays outside Tier-2's reach: it is a single block (trials need two).

Checked: the five `test_jit_tier2_*` scripts, the 45 `jit_` unit tests with and without `BIFROST_TIER2=1` (`jit_disk_cache`
fails under Tier-2 by design: the persistent cache is off then), `make verify` (105 differential fixtures, 11 threaded
oracle runs) and CoreMark's CRCs in both modes. No game was run with Tier-2.

## CCMP and CCMN leave their flags in the host (2026-11-10)

`CCMP`/`CCMN` moved both operands into RAX and RCX (stack spills when other values lived there), flushed RDX, and on either path
stored NZCV to `cpu.nzcv_host`, so the `b.cond` after it tested memory. Now the operands stay where the allocator holds them and a
constant second operand (`#imm`) folds into the instruction; the fold-lookahead skips its `mov`. The condition is read from the host
flags, from one bit of `cpu.nzcv_host` (EQ/NE/CS/CC/MI/PL/VS/VC with the flags in memory, as `BRCOND` does) or through a reload
that clobbers only RAX. The true path is `cmp` (`CCMN`: `mov` + `add` into a scratch register); the false path sets the
immediate NZCV in the host flags with `mov eax, image; add al, 0x7f; sahf`, inside `push rax`/`pop rax` when RAX holds a value.
Both paths leave the flags live in the host, in the op's carry convention, and store nothing. AL and NV always compare: the old
emitter mapped them to `je`, so `ccmp ..., al` used the immediate whenever Z was clear (compilers do not emit that form).

The hash-map kernel's probe loop (`cmp w14, #0; ccmp w14, w11, #4, ne; b.eq`) ran 9.1% fewer host instructions per frame
(1.473M to 1.339M). Its cycles fell 17.5% in one pinned run per build, inside the ±15% placement range below, so that figure
is not established. Kernels without `ccmp` in their loops are unchanged. `jit_ccmp`'s table covers every condition with the
incoming flags in memory, from `cmp` and from `cmn`, both ops and widths, register, immediate and XZR operands, against a C model
that also passes under QEMU, read back by all fourteen `cset` conditions, `b.hi`, `mrs` and the operand registers.

## Guest register traffic and flag windows: measured, not merged (2026-11-10)

On the integer kernels guest register loads and stores are 20-27% of the ops executed in translated code. Most are block
entry loads and live-out stores, which only cross-block allocation would remove. Two smaller sources were found:

- An x86 ALU instruction (`and`, shifts, `xor`, register `sub`) between a compare and its `b.cond` writes EFLAGS, so codegen
  packs the live flags (`push; lahf; seto; mov; pop`) and the branch tests memory. 337 of 4,938 setter-to-consumer pairs in the
  game kernels and CoreMark have one. An IR pass that hoists such pure scratch computations above the setter (stashed, not
  merged) removes it: the bytecode-VM kernel's hottest block went from 16 to 7 host instructions, -6% instructions overall,
  but averaged over 16 code placements its cycles moved -1%, within noise.
- `CCMP` forced its operands into RAX/RCX (three stack spills in the hash kernel) and packed its flags to memory for the
  following branch. Changed since: see the section above.

Code placement alone moves these kernels' cycles by up to ±15% (the hash kernel: 398k to 527k cycles per frame for identical
code at 16 offsets of the code buffer). Compare builds over several placements before trusting a single-digit cycle change.

## One 16-byte slot per vector register (2026-11-10)

The CPU kept SIMD&FP registers as two arrays, `v_lo[32]` (bits 63:0) and `v_hi[32]` (bits 127:64), so moving a whole
register between memory and an XMM took two instructions (`movsd`/`movq` + `movhpd`). The vector cache's reload went further:
a 16-byte `movups` from `v_lo[n]` (which also read `v_lo[n+1]`) and a `movhpd` that replaced the wrong half. That load spans two
earlier 8-byte stores, which store forwarding cannot serve.

`CPU::v[32]` is now `struct VReg { uint64_t lo, hi; }`, 16-byte aligned, at the same offset and size, so nothing after it moved.
Generated code addresses `vlo_off(n)`/`vhi_off(n)` and moves a whole register with one `movups` (`load_v16`/`store_v16`): the
vector cache's reloads and writebacks, `load_vec`/`store_vec`, the Q forms of the FP vector helpers, LD2-LD4/ST2-ST4 and the
uncached 16-byte guarded access. The lo-only scalar FP cache still stores 8 bytes: its XMM does not hold `v_hi`. Element
offsets are `vlo_off(n) + byte`. C++ reads `cpu.v[n].lo`/`.hi`; whole-state copies are one `memcpy` of `v`.

Cycles per frame against the previous commit (best of two runs, pinned): skinning -42%, anim -35% (both -9.5% host
instructions); the scalar and integer kernels are unchanged within noise. No game was measured.

## Scalar FP loads into cached XMMs and cheaper scalar results (2026-11-10)

An IBS sample of the engine-shaped kernels (ops inside translated code, each classified by operand pattern) found real
FP arithmetic at only 2-6% of executed ops in the FP kernels; XMM copies were 14-21%, `v_lo`/`v_hi` traffic 5-24%,
`pxor` zeroing 3-13% and NaN checks 4-10%. Two changes take the cheap part:

- **`LDR S/D` loads into the XMM.** It was a guarded GPR load, `movq xmm, gpr` and a `v_hi` zero store. A 4- or 8-byte
  `LOAD_MEM` read only by the next `FMOV_G2F` (`fp_load_`) is now `movss`/`movsd xmm, [mem]` into the register's cached
  XMM; the `FMOV_G2F` emits nothing. The access is a registered fault site and the slot is claimed before the guard
  captures its layout, so a fault leaves the XMM untouched and the exit sees the state before the load.
- **Scalar results.** A single result reached its cached XMM as `pxor t; movss t, x; movaps xd, t`; it is now
  `xorps xd, xd; movss xd, x`, and a double result `movq xd, x` instead of a merging `movsd`, so in the vector cache
  (where the XMM holds `v_hi`) the following zero of bits 127:64 is dropped.

Not changed, measured: storing the `v_hi` zero as `mov qword [hi], 0` instead of `pxor; movsd` is one instruction fewer
but made cull and collide about 30% slower in cycles (cause not established). NaN checks were left alone: dropping one
exactly needs a result that cannot be NaN or whose payload nothing can observe, and in the five FP kernels 3 of 277 checked
ops qualify (results that are only compared still meet a load, which can fault, before they are overwritten).

Against the previous commit (best of two runs, 350 minus 50 frames, pinned): host instructions -5.4% skinning, -5.9%
particles and audiomix, -7.3% cull, -5.8% collide; cycles -6.3% audiomix, -2.4% cull, -2.3% collide, -1.1% skinning,
particles unchanged. No game was measured.

## NZCV stored as the `lahf` image (2026-11-10)

`cpu.pstate` held NZCV in the architectural layout (N=31, Z=30, C=29, V=28) plus a JIT carry-convention bit 27. Every exit
with live flags packed host RFLAGS into it with seven instructions (`lahf; seto al; and; imul; and; xor; mov`), and every
block that needed them back in RFLAGS rebuilt them with fifteen. `nzcv_dead_in` only removes the pack when the successor
rewrites the flags before anything can fault, and most successors start with a load, which can (a guest signal frame must
show the flags of the faulting instruction), so the pack stayed on most edges.

The field is now `CPU::nzcv_host`, the image `lahf; seto al` leaves in AX: N = bit 15, Z = bit 14, bit 8 = x86 CF = NOT ARM
C, V = bit 0, other bits don't-care. A pack is `lahf; seto al; mov [nzcv], eax`, with a `cmc` first when the producer's CF is
the ARM carry (additions, logical ops); a reload is `mov eax, [nzcv]; add al, 0x7f; sahf` and clobbers only RAX. Nothing is
skipped, so faults stay precise. One-flag `BRCOND` tests read the bit in memory (C as "CF bit clear"); the two-flag
pstate arithmetic of 2026-11-08 is gone, since the reload is cheaper. `FP_CMP` and `FCCMP` store constants converted
at compile time (`host_from_arm_nzcv`), and `CCMP` loads one into the host flags; `mrs`/`msr nzcv` convert with one multiply each. C++ reads and writes through
`nzcv()`/`set_nzcv()` (architectural view) and the flag accessors; only save-and-restore code copies the raw value. Signal
frames, ptrace, the GDB stub, the C API and differential verification use the architectural view.

Host instructions (`perf stat -e instructions:u`, one run each, against the build before):

| Program | Before | After | Change |
|---|---|---|---|
| CoreMark, 200,000 iterations | 332.86 G | 311.24 G | -6.5% |
| `bench_fib` | 1.506 G | 1.414 G | -6.1% |
| `bench_sort` | 1.433 G | 1.381 G | -3.6% |
| `bench_dispatch` | 33.66 G | 32.86 G | -2.4% |
| `bench_mips`, `bench_matrix` | | | under 0.1% |

The store is 32 bits wide on purpose: a first version stored AX, and every reader's 32-bit load then missed store forwarding,
which made a bytecode VM kernel 1.8x and a hash kernel 2.4x slower in cycles while executing fewer instructions.

On the engine-shaped kernels of 2026-11-07 (one frame each, cycles per frame from a 350-frame minus a 50-frame run, pinned
to one core, one run each, against the loop-pin build): audio mixing -16%, hash -9%, culling -6%, a bytecode VM -5%,
collision -3%; skinning and particles unchanged within noise; matmul and anim are too short per frame to read. No game was
measured.

`FP_CMP`'s branchy sequence (kept when the compare feeds `fcsel` or anything else that is not a branch or an integer select)
read the `ucomiss` result through `pushfq; pop rax` and tested the bits in RAX. `pushfq` alone was 14.7% of the ops an IBS
sample found in the audio-mixing kernel. It now jumps on PF, CF and ZF directly (`mov` leaves them intact): audio mixing -25%
cycles per frame, particles -12%, the other kernels unchanged.

Following unconditional and compare branches in the dead-flag scan was measured first
and rejected: it would have removed 3-5% of the remaining packs. `jit_flags_exit_dead` section K faults on a successor's first
load after CMP, ADDS and TST and checks the signal frame's NZCV (dropping the `cmc` fails it); `jit_msr_nzcv`, `jit_fcmp_flags`,
`jit_ccmp` and `jit_branch_flags` cover the conversions and the memory tests.

## PC stores behind the chain slot, loop pins and four peepholes (2026-11-10)

A sample of the operations executed inside translated blocks (AMD IBS, `perf record -e ibs_op/cnt_ctl=1,swfilt=1/u` with
`BIFROST_JIT_DUMP=1`, sample addresses mapped onto the dumped blocks and each instruction classified) on CoreMark, `bench_fib`,
`bench_dispatch` and busybox `gzip` and `sort` showed that most of them are bookkeeping, not guest work: guest register loads
and stores 18-24%, bounds guards 11-17%, flag packing at exits 4-17%, the PC store before each chain slot 5-8%, chain jumps
4.5-8.6%. The plain `instructions:u` event skids too far to attribute single instructions; IBS does not. This change takes the
cheap part of that list.

- **The PC store follows the chain slot.** A static chain-skip exit was `mov eax, pc; mov [cpu.pc], rax; slot; frame return`,
  so a patched edge stored a PC nothing read. It is now `slot; mov eax, pc; mov [cpu.pc], rax; frame return`
  (`defer_pc_store`): a patched slot jumps past both instructions, and every other way out of the slot (still unpatched, an
  empty or revoked MT cell, the backward-edge checks) falls into them. An MT backward edge keeps the `mov eax, pc` before its
  slot, which compares RAX with the stop sentinel. Indirect (`ret`/`br`/`blr`) and unchainable exits, the ordinary layout
  (`BIFROST_NO_CHAIN_SKIP=1`) keep the old order. Tier-2 used to keep it as well, until 2026-11-11 (see that section).
- **`cpu.pc` is stale inside a chained block, except where a helper may read it.** C++ helpers called from generated code can
  read `cpu.pc` (the slow memory paths raise a guest signal with it), and until now it held the block's start. A block that
  calls out of generated code therefore publishes a chain entry that stores its own start PC first. Every call emitter sets
  `block_calls_out_`, so this follows what was emitted, not a list. A list of ops whose emitters stay inside generated code only
  decides where the store goes: inline before the plain entry when a call is expected, in a stub after the block (store, then
  a jump to the plain entry) when a call was emitted without being expected. Guarded accesses reach helpers only through exits
  that set the PC themselves, so integer blocks carry no entry store.
- **Loop pins are not stored on every iteration.** `BRCOND` flushed every cached register before its `jcc`, including the
  loop-carried pins of a self-loop, so a `cmp; b.cond` loop stored up to four registers per iteration while a `cbz` loop stored
  none. `flush_all_vregs_but_pins` leaves the pins mapped and dirty in R12-R15, which neither path's flag commit touches; the
  exits write them back. The first attempt miscompiled `jit_fp_ldst_cache` and was backed out. The cause was older than it:
  the pin preload set each pin's mapping without raising `max_vreg_`, and the allocator trusts a mapping only for
  `v <= max_vreg_`, so an op that reads a pin before anything else is allocated (`scvtf s0, w1`, the first instruction of that
  test's entity loop) read `cpu.regs[1]`, which the back-edge no longer kept current: every entity got the same position. A
  `cbz` loop of that shape was already wrong (`jit_selfloop_pins`' sum of 1..1000 came out as 999,001: each iteration
  reconverted the counter's stale value) and a pin in x0 escaped by
  luck. The preload raises `max_vreg_` now. Exits before a pin's rewrite were already safe: fault exits and the FP cold step
  store every mapped architectural register, clean or not.
- **Four peepholes** (Tier-1; each is decided by a pre-scan and re-checked by the emitter):
  `LDRSB`/`LDRSH`/`LDRSW` are one sign-extending load instead of `movzx` + `movsx` (`sext_load_`); `sxtb`/`sxth`/`sxtw` through
  `SBFM` are one `movsx`/`movsxd` instead of a shift pair; a 64-bit `lsl #1..3` read only by the next `ADD` becomes that
  `lea`'s index scale (`scaled_add_`, one `LOAD_REG` may sit between them, as the translator emits for `[xN, xM, lsl #s]`); and
  a register read only as the second operand of the following `CMP` is `cmp r, [cpu.regs]` (`lazy_load_`). The last two leave
  a value unmaterialized for one op; `materialize_scaled` and `materialize_lazy_load` emit the original instruction for any
  consumer that does not fold it, and the Tier-1 loop calls them before every other op.
- **Compile time.** The kill-list scan walked all 4,096 vreg slots for every block; it stops at the highest vreg the block
  uses. That loop was about half of `translate_block`'s own time in a busybox shell run (17 microseconds per block).

Host instructions executed (`perf stat -e instructions:u`, one run each, same machine and session, the build before this
change against the build with it):

| Program | Before | After | Change |
|---|---|---|---|
| CoreMark, 20,000 iterations | 24.60 G | 21.66 G | -11.9% |
| `bench_mips` | 2.519 G | 2.211 G | -12.2% |
| `bench_sort` | 1.521 G | 1.433 G | -5.8% |
| `bench_fib` | 1.589 G | 1.506 G | -5.2% |
| `bench_dispatch` | 34.86 G | 33.66 G | -3.5% |

These are instruction counts, which repeat to four digits; run times were not compared (single-run cycle counts moved by less
than their noise except CoreMark, about -4%). Emitted code is 43.8 bytes per guest instruction on CoreMark against 43.6. No
game, multithreaded workload or FP kernel was measured.

The pin part alone, re-measured after its fix against the build that had backed it out (same method): `bench_mips` 2.609 G to
2.309 G (-11.5%), `bench_matrix` -4.2%, the 200,000-iteration CoreMark run -1.5% per iteration (its iterations per second did
not move beyond noise), `bench_sort` and `bench_dispatch` unchanged.

Still open from the same sample, largest first: flag packing at exits (the dead-flag scan stops at the first load, so a loop
whose blocks each start with a load packs seven instructions on every edge), one bounds check per access even for runs of
same-base stack accesses, guest-state stores overwritten later in the block, the 17-instruction indirect-branch probe, copies to
and from pinned registers, and the `movabs` of the window base on every chain entry.

## FP compares in the host flags, and where last-level misses come from (2026-11-07)

`fcmp` built NZCV with `pushfq`, a chain of up to three data-dependent branches and a read-modify-write of pstate: about 22 host
instructions and 110 bytes on the hot path of every compare, with branches that mispredict on random outcomes. When the compare
feeds a conditional branch or an integer select (`b.mi`, `cinc`), `FP_CMP` now leaves its result in the host flags, as `SUBS`
does: `seta`, `setb` and a `cmovp` pick two small integers and one `cmp` of them leaves exactly the flags a subtraction would for
each outcome (greater 1-0, equal 0-0, less 0-1, unordered 0x80000000-1), and the consumer reads them. Pstate is written only where
the flags are still live, by the existing materialisation. A compare that feeds `fcsel`, `mrs` or anything else keeps the old
sequence: with the lazy flags audiomix (clamps that rarely fire, then `fcsel`) was 17-38% slower, because the old branches were
predicted and took the compare off the data path while the lazy flags put it on a longer one. `FCCMP` is unchanged.
`Tests/Unit/jit_fcmp_flags.c` checks every pair of {-inf, -2.5, -0, +0, 3, +inf, quiet NaN, signalling NaN}, single and double,
register and `#0.0` forms, `FCMP`, `FCMPE` and `FCCMP` (condition true and false), plus branch and `cset` consumers (2328 checks;
it passes under QEMU, with the JIT in each cache configuration, with `--no-jit` and under `BIFROST_JIT_VERIFY`).

Measured here (same method as the sections below, against the build before this change): cull 33.9 to 27.1 times native, collide
11.4 to 8.1, particles 3.4 to 3.0, the other six unchanged; on a frame of all nine kernels, time -7.3%, cycles -6.6%, host
instructions -3.4%, branch mispredicts -12.6%; the cull loop's executed code spans 80 cache lines instead of 84. L1 miss counters
did not move beyond their run-to-run noise (instruction misses varied between 188k and 260k across identical runs).

Where last-level misses come from: a CoreMark run (`Tests/Unit/coremark.elf`) shows about 3.0M last-level misses and 20M L1D
misses at 2,000 iterations and at 20,000 alike, so they are fixed costs of the process, not of executing the guest; L1 instruction
misses do scale, at about 60 per iteration. A trivial guest costs 0.65-0.75M last-level misses and 39.6M instructions (10 ms) in a
warm process, with the dynamic loader relocating 103 libraries (the FFmpeg stack and its codecs are linked in). A sampled
`cache-misses` profile of one CoreMark run put 61% in `ld-linux`, 20% in the emulator (`GraphicThunk::register_function_` 7.5%,
`translate_block` 2.3%), 10.5% in libc and 0.9% in generated code; that was a single, cold run and is only indicative. So a
whole-process miss count against another emulator largely compares startup and translation, which a leaner loader footprint,
lazily registered thunks and less emitted code would cut, not the generated code's behaviour.

## Scalar FP cold steps after the block (2026-11-07)

Every scalar FP operation had its ARM-semantic step (the NaN path: commit, single-step the interpreter, reload the vector cache)
emitted inline right after it, with the hot path jumping over it. That step is a few hundred bytes, so hot code was spread thinly
over many cache lines and every FP operation carried a taken `jmp`. `emit_fp_cold_tail` now defers the step to the end of the
block with the register layout captured at that point, as the vector guards already did, and the hot path falls through.

Measured here (2026-11-07, same method as below): in the nine-kernel set's cull loop the seven hot blocks, which occupy 342 cache
lines and 21 KB, now execute from 84 distinct lines instead of 118 (samples can miss a line, so these are lower bounds). Running all
nine kernels as one frame, L1 instruction misses fell from 260k to 207k and host instructions by 2%; time, cycles and the
per-kernel ratios did not change, because none of these workloads is bound by L1: the cull loop's hot code is about 5 KB. The
counter `ic_fetch_stall.ic_stall_any` rose from 1.36 to 1.66 billion cycles over the same runs and I have not explained it. Total
emitted bytes are unchanged (the steps moved, they did not shrink), so code-cache size is not affected. Whether a larger
workload gains in time is untested: Zenless Zone Zero was not available.

## LD2-LD4 and ST2-ST4 inside the block (2026-11-07)

Multiple-structure NEON loads and stores (`ld4 {v0.4s-v3.4s}`, `st2`, ...) used to be `SIMD_NATIVE_STRUCT`: a C++ helper
(`native_structure`) that ended the block, so every one cost a dispatch and a generic loop. Clang emits them to move array-of-structure
data (float4 positions and quaternions, RGBA pixels, stereo samples) in and out of registers. On a reconstructed engine-shaped
kernel set (below) a quaternion blend spent 74% of its samples in the emulator's own C++ (`native_structure` 39%, `run_block` 20%)
and a 4-bone skinning loop 27% in `native_structure` and 15% in `run_block`, at 96 and 52 host instructions per guest instruction.

`SIMD_STRUCT_MEM` replaces the helper for LD2-LD4 and ST2-ST4 of every element size and both vector widths. The translator emits
it when the register list does not wrap past V31 and the arrangement is valid (not `.1D`); the post-index base update follows as
ordinary IR, so the op writes no GPR. One software guard covers the whole span (`n` x 16 or 8 bytes) and steps the guest
instruction on any failure, so the loads cannot fault and no vector register changes before every byte is read. The emitter
(`CodeGen/FloatSIMD/SIMD/Structure.cpp`) loads the chunks and shuffles in registers: `LD2`/`ST2` are one unpack or `shufps`
(plus a `pshufb` mask for 8- and 16-bit elements), `LD4`/`ST4` a 4x4 transpose of 32-bit lanes after a per-register grouping
mask, 64-bit arrangements two unpacks, and `LD3`/`ST3` plus any other combination a generic `pshufb` gather or scatter (a mask,
a shuffle and an OR per output and chunk, so tens of host instructions; correct, not tuned or measured). The op uses XMM0-2 and XMM12-15 only and tells the vector allocator so
(`vec_inst_effects`, clobber set 0xF007), so a loop's pinned vectors, which fill XMM3 upward, usually stay in registers. Tier-2
stops at the op. The single-lane forms (`ld1 {v.s}[i]`), the replicate forms other than `LD1R {Vt.T}`, `.1D` and wrapping lists
still use the helper.

`Tests/Unit/jit_neon_struct.c` builds every combination as a real instruction word at run time (LD2-LD4 and ST2-ST4, every
element size, both widths, register lists at three positions including V28-V31, no writeback, immediate and register post-index
by X2 and X30, SP as the base) and compares the registers, memory and moved base with a reference model written independently
of the JIT and the helper. The model itself passes under QEMU (1699 checks). It also covers a span ending exactly at a page end
(must succeed) and one byte past it (must fault with the PC at the instruction and the base unmoved), vectors live across the
op in loops, scalar FP around it, twelve live accumulators around an LD4 and an ST4, and a load/store round trip. It passes with
the JIT, with `--no-jit` and under `BIFROST_JIT_VERIFY` (the fault section is skipped there: `verify_jit.sh` passes `no-fault`
and then runs the full fixture in a normal pass). `make verify` passed (103 differential fixtures, 11 threaded oracle runs).

A post-index register offset of X30 had been treated as `#0` in the interpreter, the helper and the translator's plain `LD1`/`ST1`;
only `Rm` = 31 selects the immediate form. QEMU agrees with the fix, and `jit_neon_struct` fails on the old code (130 checks).

Measured here (2026-11-07, Ryzen 7 5800X3D, a game running, one core pinned, minimum of 40 frames, interleaved runs), on
nine kernels I wrote to resemble engine code, one source built by clang 22 for AArch64 and for x86-64-v3 (no AVX-512), checksums
identical to the native build: times native before and after, skinning 45.8 to 11.2, anim 22.7 to 4.2, the other seven within
noise of their old values (particles 3.6, audiomix 6.1, matmul 2.1, cull 34, collide 11.5, hash 5.5, vm 0.87); geometric mean 7.63
to 5.38. This is not the earlier throwaway corpus, so it does not continue the 4.15x figure of 2026-11-03, and it is no
measurement of a game. Split a kernel's ratio into guest instructions over native instructions (the compiler: cull's AArch64
loop is 4.5 times the instructions of the x86 build, which vectorized across planes) and host instructions per guest instruction
(the JIT: 5 to 15 for these kernels, 52 and 96 before this change). The first part is not the JIT's to fix. Skinning is still
14.5 host instructions per guest instruction after the change, and the scalar FP loops pay the block-boundary cost of
[the vector cache](#planned-codegen-retaining-guest-state-across-regions).

Tried and dropped: writing a cached single-precision result with one `insertps xd, xmm, 0x0E` instead of `pxor`/`movss`/`movaps`.
It was 5% faster on the audio mixer and 14% slower on cull (34.2 to 39.8 times native), with the rest unchanged.

## Growing indirect-branch tables and shared block exits (2026-11-03)

Each thread's IBL table (`FrostJIT::IblTable`) starts at `IBL_MIN_ENTRIES` (1,024) and quadruples up to `IBL_MAX_ENTRIES` (65,536) once it
has replaced one target per entry since its last resize. Only `entries` moves: the header stays put, so a table pointer saved across a
nested run (`IblGuard`, Tier-2 trials) stays valid. The probe reads the mask and entry array from the header. In a Subway Surfers lobby,
65,536 gave 34.1M dispatches a minute, 16,384 gave 42.7M, and the old fixed 4,096 gave 64.6M.

`emit_cold_stubs` adds a shared frame exit (`frame_exit_stub_`), and the probe's miss jumps are `jcc rel8`: every miss lands just past it.
Recording compiles (`use_cold_stubs()` is false) keep the inline tail, so a disk-cache session records slightly larger code.

Code fell from 64.5 to 61.0 bytes per guest instruction in that lobby, with no measurable change in CPU time: three baselines spread from
198 to 238 billion cycles over 60 s, which would hide a small effect. Pinned to one core, 15 runs each and alternating builds, `mips`, `sort`,
`matrix`, `fib` and `jit_dispatch_cache_growth` moved by about 1% or less and `bench_dispatch` was 2.6% slower. Those are small workloads;
other CPUs were not measured.

The probe shrank instead, from 105 to 73 bytes in MT. It took 10.2% of the bytes on instruction-cache lines that missed in Arknights,
in 28% of the hot blocks, and is still one per site. The table stores its mask pre-scaled (`byte_mask`), so one shift replaces two, and a
pointer to the epoch replaces a 10-byte constant. MT probes drop the generation check: every generation change advances the epoch, and a
native entry takes its lease before it syncs the table, so an entry made stale fails the epoch compare. Code without that compare (ST and
timed sampling) keeps the check. On a generated stand-in matching Arknights' counters this was 1.7% faster, `bench_dispatch` about 2%.

**Do not share the probe.** A stub for the lookup, with each block keeping its own `jmp [rcx+entry]`, reached 57.8 bytes per instruction and
passed every test, and the lobby showed nothing, but `bench_dispatch` was 44% slower (0.96 s to 1.39 s). Sharing only the exit cost about
3%, so the probe was the cause. The likely reason, not isolated, is the added `call`/`ret` and branches that lose per-site prediction.

`jit_dispatch_cache_growth` replaces code at an address only a grown table reaches: clearing just the first 1,024 entries on a generation
change makes it fail, which the fixture missed before that check was added.

## Vector FMLA by element, LD1R, loop bodies and Q accesses (2026-11-03)

Four changes from profiling the game-kernel workload (see the next section) against a native AVX2 build:

- **Vector FMLA/FMLS by element** (2S/4S/2D) are `SIMD_FP_FMA` with a Vm element (`SimdFpArithParams::lane`, IR `cond` =
  element + 1), broadcast into scratch XMM2 the way indexed FMUL does, then one FMA3. They used to be the out-of-line
  `IndexedFma` helper, which calls libm `fmaf` per lane (single precision), or the interpreter (FMLS and doubles).
  `fp_decode::vector_fp_fma_by_element` decides for the translator and the fallback predictor; it runs before the native SIMD
  table, so `IndexedFma` is no longer reached for these words. Reserved D forms (Q=0 or L=1) and FP16 are unchanged.
- **LD1R {Vt.T}** (one register, not .1D) is a guarded `LOAD_MEM` of the element and the native `SIMD_DUP`, then the post-index
  update. It was `native_structure`, a C++ call per execution. The base moves after the load, so a fault leaves Vt and Xn as
  they were. LD2R-LD4R and .1D keep the helper.
- **Loop bodies up to twice the block cap.** A loop of 33 to 64 instructions was cut at 32, so every iteration crossed a block
  boundary and reloaded the vector cache. When translation reaches the cap, `selfloop_extent` looks ahead for the first control
  transfer; if it is a branch back to the block start within twice the cap, the block grows to it. The look-ahead reads only
  plainly mapped executable pages (like `exit_flags_dead`) and decides only the size: the words are recorded if translated, and
  a block that does not grow depends on none of them. The known-tail rule that absorbs a compiled loop tail uses the same limit.
  Longer loops (skinning's 83 instructions) still split; a global cap of 64 helped some kernels and 128 hurt skinning.
- **Q-register loads and stores** (`SIMD_LD16`/`SIMD_ST16`, one register) take the scalar guarded path at width 16
  (`compile_guarded_vec16`): the allocator's registers, one window-table compare, a deferred tail, and one MOVUPD into the
  pinned XMM, or XMM0 and `v_lo`/`v_hi`. The old path evicted six GPRs, loaded a 64-bit limit and kept its page checks and
  slow helper inline. A 16-byte access can cross a page in bytes 8-15, so its tail keeps its own page checks: the shared stubs
  are built for widths up to 8. Pairs and Tier-2 keep the old path.

Tests: `jit_simd_fp_indexed` adds FMLA/FMLS by element for every element, high registers, Vd aliasing Vn or Vm and Q=0, with a
product that survives only a single rounding; `jit_ld1r` (new) covers every arrangement, both post-index forms, SP, a cached
loop and a fault at the instruction; `jit_guard_stubs` adds Q loads and stores at each pressure level with the gate open and
closed, and accesses crossing from a readable page into a userfaultfd-missing one (the only case where the software page checks
decide; a crossing into PROT_NONE faults in the host either way). `run_tests.sh` also runs the by-element fixtures with both FP
caches off, since their default run keeps every register cached. Breaking the S or D broadcast, the FMA element, the LD1R
post-index or DUP size, and routing a 16-byte guard to the 8-byte stub each failed a test. Loading only the low half of Vm on
the uncached FMA path passed the default run and fails only the uncached one. Dropping the 16-byte fault-site registration
passed everything: an unregistered host fault happens to recover in these fixtures.

Game kernels, three interleaved runs against the same native build (ms per frame, times native): skinning 0.706 (8.6x, from
2.72 after the scalar by-element change and 8.25 before it), particles 0.260 (5.4x, from 0.401); the other seven moved within
noise. Geometric mean over the nine: 5.76x native before this session's changes, 4.15x now. Not measured on CoreMark or games.

## Scalar FP by element (2026-11-03)

Scalar FMUL, FMULX, FMLA and FMLS by element (`fmul s3, s6, v0.s[1]`, `fmla d1, d2, v31.d[1]`) ran in the
interpreter on every execution, through the vector by-element code with three scratch registers swapped in. clang emits
them for matrix-times-vector code: Subway Surfers' and Arknights' `libunity.so` contain 2,915 and 2,263 of them.

- **Translation.** They become the scalar ops they extend: FMUL and FMULX an `FP_BINOP` multiply, FMLA and FMLS `FMADD` and
  `FMSUB` with Vd as the accumulator. `FpBinopParams::lane` and `FpFusedParams::lane` name Vm's element (IR `cond` = element
  + 1, 0 for the register form; the validator allows it only on these forms). `fp_decode::scalar_fp_by_element` decodes them
  for both the translator and the fallback predictor; FP16 and the unallocated D form with L=1 stay in the interpreter.
- **FMULX** differs from FMUL only for 0 times infinity, which x86 answers with a NaN; that takes the existing cold step, which
  interprets the real instruction and returns ±2. NaN results do the same for all four, as for other scalar FP ops.
- **The element.** `fp_load_lane` moves it into scratch: `pshufd` from a pinned XMM, or a load from `v_lo`/`v_hi`. The full
  vector cache keeps all 128 bits in its XMM; the low-half FP cache pins only `v_lo`, so an element in the upper half is read
  from memory, which that cache keeps current. Vd is written after the element is read, so Vd may alias Vm.

`jit_fp_pw_elem` sweeps all four forms at every element against C references over 12 values each for Vn, the element and Vd
(NaN, infinities, signed zeros, subnormals, 0 times infinity for FMULX), checks that bits above the result are cleared and Vd
aliasing Vm, and runs loops under each cache, where one register is rewritten as a scalar so its upper elements must read as
zero. A wrong shuffle for S or D, a wrong upper-half address, reading the upper half from a low-half-cache XMM and an FMLA that
ignores the element each failed it. It also passes with `BIFROST_NO_FMA3`, `BIFROST_NO_AVX2`, either cache disabled and Tier-2,
but was not run under QEMU, which is not installed here; its references are C and the interpreter.

A throwaway game-kernel workload (nine engine-shaped kernels, one source built by clang 23 at `-O2` for AArch64 and for
x86-64-v3, the native build kept off AVX-512) put 4-bone skinning at 8.25 ms per frame against 0.082 natively with AVX2. It now takes
2.72 ms (3.0x faster, five interleaved runs); the other eight kernels moved within noise. At that point two thirds of skinning's
time was vector FMLA by element (`IndexedFma`, out of line through libm `fmaf` per lane) and a tenth `ld1r` through
`native_structure`; [the section above](#vector-fmla-by-element-ld1r-loop-bodies-and-q-accesses-2026-11-03) made both native.
CPU skinning is uncommon in current engines; the same instructions appear in transform, animation and physics code, whose share
of game frames has not been measured.

## Exit spills, CSET and two-flag branches (2026-11-03)

A review of the code emitted for the hottest blocks of a throwaway guest workload (tree insert, musl `qsort`,
`snprintf` hashing, a dot product, a switch-dispatched bytecode VM) found waste around exits, flags and constants:

- **Exits store no scratch values.** `BR` (`ret`/`br`/`blr`) and `BRCOND_FALLTHRU` end a Tier-1 block, so they use
  `flush_all_vregs_exit` like the shared epilogue; every indirect exit had stored its target to a frame slot nothing read.
  Regions never admit `BR` and lay out their own direct edges. A memory fault exit drops its scratch stores too: every path
  through it ends the frame, and its loop over mapped architectural registers already covers the dirty ones.
- **Two-flag conditions test `pstate`.** A `BRCOND` whose flags are only in `cpu.pstate` tested one-flag conditions in memory but
  rebuilt RFLAGS (14 instructions, three registers) for the others, so the second branch of `cmp; b.eq; b.hi` paid that for one
  `ja`. HI/LS now compare `pstate & (Z|C)` with C, GE/LT take the sign of `pstate ^ (pstate << 3)`, and GT/LE OR in Z shifted to
  bit 31 (RAX, plus RCX for GT/LE). This relies on bit 29 holding ARM C whatever set it, as the CS/CC test already did.
- **CSET is `setcc` + `movzx`.** CSINC/CSINV become constants plus `CSEL`, so `cset w1, gt` was two constant moves, a copy, a
  `cmov` and a ZEXT. A `CSEL` whose operands are XZR or block constants 0 and 1 now emits `setcc` and `movzx`, and fold-lookahead
  kind 3 skips the constants' moves; the pre-scan accepts only that shape, and 0 and 1 are never wide guest PCs, so every such
  constant is in `jit_consts_`. A `CSEL` read only by a ZEXT 32 joins W-form fusion: its `cmov` is 32-bit, which clears bits 63:32
  whether or not it moves.
- **Flag saves and constants.** `clobber_flags` saves RAX around the pack only when RAX is still mapped (RDX-only callers) or holds
  the next PC; the full form keeps the save, since its callers may stage RAX outside the allocator. ADD/SUB/AND/OR/XOR/MUL use the
  RDX-only form, as every register they use comes from the allocator. `emit_mov_const` picks the shortest constant load, a
  sign-extended imm32 for small negatives instead of a 10-byte `movabs`, for both `IMM` and rematerialization.

On that workload Tier-1 code went from 109,331 to 102,112 bytes over the same 376 blocks (-6.6%), and a 600-round run took 6.88 to
6.98 s against 7.09 to 7.24 s for the previous commit (five interleaved runs each, headless builds from `git archive`, one desktop).
That is one program; CoreMark and games were not measured. `jit_csel` checks CSET, CSETM, CINC and a W-form `CSEL` with dirty
upper source bits for every condition, with flags from MSR, CMP and CMN; `jit_imm_flags_zext` checks constants on each side of the
encoding boundaries outside its QEMU hash; `jit_branch_flags` already drove every condition from `pstate`. Breaking each fix on
purpose (setcc polarity, the HI/LS `cmc`, the 32-bit `cmov`, the HI mask, GT without Z, a positive constant taken as
sign-extended) failed one of them. That last one passed both whole suites at first and was caught only by `BIFROST_JIT_VERIFY`: a
wrong `0x0101010101010101` in musl's string routines rarely changes a result.

Not changed: deferring architectural stores within a Tier-1 block, laying out a conditional exit so its fall-through needs no
jump, and CSEL/`BRCOND_SKIP` flag loads from `pstate`, which still rebuild RFLAGS.

## Shared cold stubs and smaller encodings (2026-11-02)

A Tier-1 guarded access used to carry its own software page checks (58 bytes) and fault exit (45 to 50 bytes) after
the block, which never run in normal execution. Each JIT now emits one copy before its first block
(`FrostJIT::emit_cold_stubs`), and a tail is a `call`, a `je` back to the access and the state commit its own exit needs.

- **Page-check stubs.** One per allocatable address register, width and direction, built by the same
  `emit_guard_page_checks` the inline form uses. A stub preserves every register and sets ZF when the access may
  proceed. The bound and flag rules are unchanged: only the location of the code moved.
- **Fault exit.** The tail loads the guest PC into RAX and jumps to one stub that stores it, calls `jit_fault_step` and
  ends the frame. Vector and pinned-register commits stay in each tail, since they depend on its state.
- **When the full form stays.** A persistent-cache recording keeps full tails, because a call
  out of the block means nothing in another process, so a session with `--jit-disk-cache` does not get the smaller code
  in what it records. `BIFROST_FAULT_TRACE` also keeps them, because its exits pass their block's PC.
- **The cost.** While the gate is closed (a registered, missing userfaultfd page, during an ART compaction), a guarded
  access pays a call and two saved registers: a loop of four accesses went from 2.5 to 4.3 ns per iteration. How long
  the gate is closed in a game has not been measured; Battle Cats and Subway Surfers showed 13 and 11 activations in a
  60 s launch. With the gate open the hot path is unchanged.

Smaller changes in the same pass:

- BRCOND's flag commit no longer saves RAX around the pack: RAX is already flushed there, and its exits load the next PC
  into it right after (`emit_materialize_flags`, `keep_rax`). Other callers keep the save.
- CSEL no longer stores a scratch result to its stack slot on every execution; it stays dirty in its register and
  is spilled if evicted, like other ALU results. An architectural destination still commits eagerly.
- `emit_rex_if_needed` omits a bare `0x40` where no byte register is involved. Byte-register forms must keep it, since
  it selects SIL/DIL rather than DH/BH. `emit_modrm_sib_win` omits a zero displacement: only SIB base 101 (RBP/R13)
  needs one, and the window base is R10.

Measured on one machine against the previous commit, with a throwaway mixed guest workload (hash table, tree, strings,
indirect calls, `qsort`, bytecode VM): emitted code 804 to 521 KB single-threaded (-35%) and 564 to 380 KB multithreaded
(-33%), Tier-1 compile time unchanged, and the VM kernel about 5% faster multithreaded. The other kernels moved less than
the noise of three runs, and the hash-table kernel varied too much to compare. Battle Cats launches measured 103.5 bytes
per guest instruction before and 70.6 after, and Subway Surfers 68.9 after, in runs of different lengths with no input,
so these are indicative only and no frame rate was measured.

`jit_guard_stubs` drives every width and direction, in and across pages, at eight levels of register pressure, with the
gate open and closed, and checks results, fault kinds and handler state; it passes on the JIT, the previous commit and the
interpreter, but has not been run under QEMU. Breaking the stubs on purpose showed what it covers: it caught a missing
userfaultfd check, an inverted answer, swapped pops, a wrong fault PC, a lost RAX and a lost CSEL value. It did not catch two:
swapped read and write checks, which host page protection then reports identically, and a missing ZF clear on the failing
path, which matters only at the exact window bound a guest cannot observe. The address registers reached were RAX, RCX,
RDX, R8, R9, R11 and R13, not R12, R14 or R15.

Found along the way, not changed: with `BIFROST_NO_CHAIN_SKIP=1`, `jit_disk_cache`, `test_posix_timers` and `test_exec_mt`
fail on this commit and the previous one alike. A page registered with userfaultfd without first being discarded after a
descriptor exists is not treated as missing, so a first touch does not fault; the kernel would deliver it.

## Software page checks only while a page needs them (2026-10-26)

ART calls `userfaultfd()` when libart loads (`KernelSupportsUffd` in `gUseUserfaultfd`'s static initializer,
[mark_compact.cc](https://android.googlesource.com/platform/art/+/refs/heads/main/runtime/gc/collector/mark_compact.cc)),
and every Umamusume process logs `Using CollectorTypeCMC GC`. That call used to set the software-check flag for
good, so every guest load and store in an ART app took the out-of-line page check (two taken jumps, a 10-byte
`movabs` and about a dozen instructions). A throwaway loop of four accesses per iteration ran 1.8 times slower
with the flag set (2.7 against 4.8 ns per iteration, three runs each).

The flag is now raised only while a page needs it (`Memory::software_check_page`): userfaultfd registered and
missing, the only state whose host page admits a native access the guest must not make. Every page-flag change
goes through `Memory::set_page_flags_locked`, which counts those pages, raises the flag before the first one
changes and clears it after the last one. ART registers its moving and linear-alloc spaces only during a
compaction (`KernelPreparation` to the end of `CompactionPhase`), so the flag is clear between collections.
Generated code is unchanged.

- **Execute-only pages are readable.** Linux arm64's `protection_map` (`arch/arm64/mm/mmap.c`, v6.12) maps
  `VM_EXEC` as `PAGE_READONLY_EXEC` and switches to `PAGE_EXECONLY` only with Enhanced PAN (ARMv8.7); the
  emulated CPU is a Cortex-A72 without it. `Memory::effective_page_prot` applies that rule, as it already did
  for write-only pages; the VMA and `/proc/self/maps` keep `--x`. Umamusume's protector library makes 72 pages of
  its text execute-only in every process, which had kept the flag set for good.
- **MISSING bits are refreshed at registration.** While the flag is clear, native code populates a discarded,
  unregistered page without clearing its MISSING bit. `UFFDIO_REGISTER` therefore clears MISSING on pages that
  `/proc/self/pagemap` shows present or swapped *and* that hold nonzero bytes. The contents test is required:
  a host transparent huge page populates up to 511 untouched neighbours, and trusting pagemap alone cleared
  442 such zero pages of ART's heap in one Umamusume launch, which then read zeros instead of faulting and crashed
  the game after its first collection. Limitation: a discarded page that the guest touches but leaves all zero,
  then registers without discarding again, still delivers a missing fault (Linux would not). ART discards or
  moves every range after registering it. If pagemap cannot be read, the flag stays set as before.
- `check_access` clears an unregistered page's MISSING bit with a compare-and-swap that faults instead when a
  registration lands meanwhile, so the count cannot drift.
- `[mem] software page checks: pages=N activations=N sticky=0|1` (with the `[JIT]` statistics) shows the state.

Measured (one machine): the loop above runs at 2.75 ns per iteration with and without a prior `userfaultfd()`
call. Umamusume EN (unattended, 150 s and 240 s to the title): no crash; the game process reported `pages=0` at
every 30 s sample after 14 activations (one per collection). No in-game frame rate was measured. Tests:
`test_userfaultfd` (a page rewritten by JIT-hot code while nothing is registered stays present after
registering; the host kernel agrees), `test_mem_guard` (a JIT-hot load from an execute-only page agrees with
the kernel's `write()` of it).

## Persistent translation cache (2026-10-26, experimental, off by default)

`--jit-disk-cache` (`BIFROST_JIT_DISK_CACHE=1`, `[jit] disk_cache = true`) stores Tier-1 translations of
file-backed guest code so a later launch installs them instead of compiling. `--jit-disk-cache=verify`
compiles every block anyway and compares the fresh record with the stored one (`verify_mismatches` in the
statistics). `Runtime/CacheRecording.cpp` records and installs; `Runtime/PersistentCache.*` stores.

- **Key.** A record is found by the file the code came from (`Memory::code_origin`: a hash of the path the
  mapping's descriptor names, plus the offset; memfd and deleted files have none) and by MT or ST mode.
  Installing checks the guest words now at the address and the dead-NZCV successor words
  (`flag_dep_table_`) before relocating the code, then publishes through `publish_translation` like a
  fresh compile (granules, back references, chaining, pending calls).
- **Proof compile.** A recording compile logs every `emit_u64` field; emitters that embed a host address
  declare it with `note_host_reloc` (image, JIT object, window base, page flags, MT edge cell), and every
  constant from 0x10000 to 0xFFFF_FFFF takes its imm64 form (`wide_guest_pc`): it could be a guest PC of a
  block mapped below 4 GiB, and the choice depends on the value alone, so the same code records the same bytes
  at any load address (an earlier rule keyed on the distance to the block made verify report thousands of
  encoding-only mismatches across Umamusume's processes). Before publication the block
  is compiled again at guest PCs shifted by `kOracleBias` (`record_oracle_`; fetches subtract the bias,
  everything it appended is discarded). Outside the logged fields the bytes must match; each field must be a
  constant, a declared address that lies inside what it names, a guest PC (moved by the bias) or a code
  address (moved by the copy's distance). Edges, fault sites, words and dependencies must agree up to the
  bias. A constant equal in both compiles that lies in host memory (`host_address`, or a page `mincore`
  reports mapped outside the guest window) is an undeclared address and leaves the block unrecorded.
  Anything unexplained costs a cache entry, never a wrong translation.
- **Off when** Tier-2, `BIFROST_JIT_VERIFY`, leaf inlining, `BIFROST_MALLOC_INTERP` or block tracing is on.
  Blocks with direct BL call sites, inlined leaves or interpreter-only bodies are not recorded.
- **Fingerprint.** One directory per application (`BIFROST_JIT_CACHE_APP`, set to the package by the
  managed launcher; else the program name and a hash of its path) and per codegen fingerprint: the emulator's
  GNU build-id, CPU features, window size, code-buffer size, chain/direct-call/W^X/FMA3/AVX2 state and the
  value of every `BIFROST_*` switch the JIT and `DebugFlags.hpp` read. The build lists those switches from
  the sources into `$(BUILD_DIR)/generated/JitCodegenSwitches.inc` (the Makefile's `JIT_SWITCHES` rule), so
  a new switch cannot be left out.
- **Storage.** `$BIFROST_JIT_DISK_CACHE_DIR`, else `$XDG_CACHE_HOME/bifrost/jit`, else `~/.cache/bifrost/jit`.
  Each process appends checksummed records to its own `seg-<pid>-<n>.bin`, locked (`flock`) before its
  header is written; a header-less segment is treated as abandoned only after a minute. Once segments reach
  4 MiB or an eighth of `main.bin`, the next long-running process merges unlocked segments into a new
  `main.bin` (sorted index read in place, `rename` over the old one) in the background, keeping the most
  recently used records within three quarters of the budget (`BIFROST_JIT_DISK_CACHE_MB`, or `_KB` for tests, default 512 per
  application). Opening the cache first removes other fingerprints' directories (older builds) down to
  three quarters of the budget, so a new build does not drop what it records. The cache stays off when a
  directory on the way is not the user's own or is group/world-writable, or when the filesystem has no
  `flock`.

Measurements (one machine, 2026-10-26; the root binary of that day): 18 dynamic glibc/musl, threaded and
dlopen guest programs reused 98.9–100 % of their blocks with output identical to cache-off runs, and verify
mode found no mismatch. `qt_test` (7,900 blocks): about 185 ms cache off, 105 ms reusing, 287 ms recording
(the proof compile doubles compile time on the first launch and after every rebuild). `bench_sort`,
`bench_matrix` and `bench_fib` ran at the same speed from cached code. With a build whose call sites did not
declare their helper addresses, `mincore` rejected every affected block (4,784 of 7,894 in `qt_test`) and the
reuse run stayed correct.

Umamusume (EN 1.35.2, managed Asgard session, unattended 150 s runs, 2026-10-26): the game process records
about 400,000 blocks in its first launch and fills the 512 MiB budget (about 1.3 KB a record; later blocks are
dropped). A second launch in verify mode found 517,000 stored records equal to fresh compiles in the game process
and 141,000 to 190,000 in each helper process, with no mismatch. Starting from the cache, 200,000 to 390,000
blocks per launch were installed instead of compiled, and the game reached its first Unity log 10.4 to 12.7 s
after start (three launches; 12.2 s in the one launch without the cache, 15.2 to 15.7 s while recording), too
few runs to state a startup gain; the recording launch reached its later
milestones about 25 % later than a launch without the cache. Unattended runs stop at the title, which needs a
tap, so play past it from the cache has not been checked.

Not yet validated: play past Umamusume's title and other Android games from the cache (ART JIT code is
memfd-backed and never cached; native libraries are), a game session that renews its code cache, and network
filesystems. Host addresses embedded as 32-bit immediates or
RIP-relative operands outside the block are not covered by the checks above: emit host addresses as imm64
with `note_host_reloc`. Tests: `Scripts/Testing/test_jit_disk_cache.sh` (`run_tests.sh` entry
`jit_disk_cache`).

## Code-cache generations and syscall exits (2026-10-25)

Generated code is never recycled in place: a stale function pointer, a chain edge or a live native
frame can still point into it. A full code cache therefore used to leave the process in the
interpreter for good, and a long session only ever grew. Melon Playground reached 968 MB of
code mapping after ten minutes (4.7 to 5.0 GB resident for the game process).

A full cache is now replaced as a whole. When a compile runs the budget out
(`BIFROST_JIT_CACHE_MB`, default now 512 MiB), `translate_entry` asks the emulator for a successor
(`Emulator::renew_jit`): a fresh `FrostJIT` becomes current and the full one is retired
(`FrostJIT::retire`: `invalidate_all`, then the retired flag, then the global epoch advances).
`Emulator` keeps every instance in a `JitSet`; the last is current. Only the shared instance is
renewable (per-thread instances, `BIFROST_NO_SHARED_JIT=1`, keep the old behaviour).

A retired instance stays mapped until no thread can run its code (`Runtime/Generations.{hpp,cpp}`).
Every thread has a slot. At a dispatch boundary with no generated frames (`cpu.jit_call_depth == 0`
at `run_block` entry) it reads the epoch and stores it (acked); a retired instance's `run_block`
forwards to the current one, so an acked thread runs only newer code. A thread blocked in a
syscall that a block handed back to C++ is parked (below); leaving the park exchanges the flag
with a full barrier before the epoch read, which orders a reaper that saw "parked" before the
thread's forward. A reaper thread releases an instance once every slot is acked or parked
(`FrostJIT::release_resources`): it unmaps the code buffer and the fault table and frees the
per-block metadata, including the heap cells the generated code addressed directly (MT edges,
hot counters, Tier-2 monitors). The 56 KiB object stays, because generated code embeds the
address of members such as `cache_generation_` and a parked thread may still return through
`run_block` on it. It then calls `malloc_trim`.

Syscalls leave generated code first. A block that reaches a non-thunk SVC stores its address in
`cpu.jit_svc_pc` and returns; `run_block` (and the nested `jit_call_helper` loop) runs the syscall
from C++ (`FrostJIT::run_pending_svc`) with `cpu.pc` already advanced, as before. A thread blocked
in futex, epoll or sleep then has no generated frame on its stack, so it does not hold the retired
cache. Thunk SVCs (`x8 == 0x1000`, GL/audio/display calls: short and very frequent) stay inline. A
signal or the debugger that moves `cpu.pc` first drops the pending SVC, and the guest re-executes
it where it resumes, which is the architectural behaviour for a signal that arrives before the
instruction. `run_pending_svc` clears the field before the syscall, because `clone` copies the CPU.

What can keep a retired generation alive: a thread blocked inside generated frames (a thunk SVC,
a native nested call from `jit_call_helper`), or one that never returns to its dispatcher. At most
one retired instance may be unreleased (`kMaxUnreleased`); while it is pinned the current one, once
full, falls back to the interpreter exactly as the single cache did before. So the worst case is
twice the budget, which with the 512 MiB default equals the old 1 GiB ceiling. A nested
`jit_call_helper` loop follows the current instance once its own is retired (`retarget_scope`), so
a long callee does not interpret forever; helpers called from generated code (Tier-2 fire,
trials, feedback) use `FrostJIT::running`, the instance whose code is executing. Under
`BIFROST_NO_CHAIN_SKIP=1` (ordinary frames, native BL/BLR calls) the whole program runs inside
nested frames, so only the first generation is ever replaced and later ones fall back.

`[JIT] generations: retired=N released=M blocked_by_threads=K` (with the other `[JIT]` lines under
`BIFROST_STATS_PERIOD` and at exit with `--verbose`) shows whether releases keep up and how many
threads kept the oldest retired instance alive at the reaper's last look.

Measured (2026-10-25, one host; the recompile cost is the price of the bound):

- `bigcode` (a throwaway guest, not in the repository: 60,000 distinct functions called round-robin,
  three passes; one pass translates 240,296 blocks, 680 MB of code; peak host RSS from `/proc/PID/status` VmHWM). The
  results are identical at every budget.

  | build, budget | peak RSS | wall time |
  |---|---|---|
  | before, 1024 MiB | 949 MB | 7.1 s |
  | after, 1024 MiB | 949 MB | 7.2 s |
  | after, 128 MiB | not sampled | 20.7 s |
  | after, 64 MiB | 179 MB | 20.8 s |
  | after, 32 MiB | 138 MB | not timed |

  When the code in use exceeds the budget every pass recompiles it (this workload is the worst case,
  a cycle larger than the cache), so the wall time is the compile time of one pass per pass. When it
  fits, nothing changes.
- A syscall loop (`getppid`, 1.5 M calls, pinned to one core, five alternating runs): 106 to 118.5 ns
  per call (+12 ns: the extra dispatch). An indirect-call loop (20 M calls through a table of eight):
  5.0 ns per call before and after.
- Melon Playground, five minutes of play with the 512 MiB default, one run: game RSS 3.87 GB with a 398 MB
  code mapping at 266 s, flat at 3.85 to 3.89 GB from about 110 s to 295 s. The JIT was replaced twice and
  both old generations were released (`retired=2 released=2 blocked_by_threads=0`: no thread pinned a
  generation); the current one held 582,164 blocks and 423 MB of code, metadata 289 MiB. The pre-change
  reading from another session was 4.70 GB with an 858 MB mapping at 263 s and 5.03 GB at 633 s. This is
  not a controlled A/B (different play, one run each; the side-by-side run with the old build was stopped),
  and whether the two renewals caused a visible hitch was not recorded. A renewal discards every block, so
  each one recompiles the working set (hundreds of thousands of blocks, seconds of compile time across the
  threads that need them). The persistent translation cache (`--jit-disk-cache`, see
  [its section](#persistent-translation-cache-2026-10-26-experimental-off-by-default)) is meant to make
  that cheaper; it was run together with renewals only on the pressure fixture (correct results), not in a game.

Tests: `Tests/Unit/jit_cache_generations.c` through `Scripts/Testing/test_jit_generations.sh` (1 MiB
budget, about 500 renewals: sleepers in nanosleep and futex, a futex wait inside a nested call,
nested call loops that compile throughout, rewritten code, signals, a fork afterwards; every result
checked), and `test_jit_cache.sh`, which now expects renewals at a 1 MiB budget.

## Planned codegen: retaining guest state across regions

This is the proposed v2 compiler direction, with milestones and performance
targets in [the roadmap](roadmap.md#jit-codegen-retain-guest-state-across-hot-regions).
Current region caching retains compatible XMM layouts internally; region
entries still preload their layout. Scalar SSA does not yet model complete
vector/FP architectural state. The proposal extends those foundations in
stages rather than assuming cross-region state is already supported.

- **Explicit guest values.** Track GPR widths, vector lanes and scalar FP
  aliases, including partial writes and upper-lane clearing, together with
  lazy NZCV. Start with mixed FP/vector allocation within current regions.
  Keep FPCR/FPSR, NaN, helper and fallback behavior observable under the
  existing instruction contracts. Extend integer allocation and flags based
  on the measured CoreMark hot path.
- **Recovery descriptions.** Before deferring architectural stores across
  regions, define metadata that maps host PCs and value locations to the
  precise guest PC, GPR/FP state and NZCV. Handle completed memory effects,
  faulting accesses, asynchronous signal delivery and interpreter/host-call
  boundaries. Metadata lifetime must cover in-flight code. Safe fallback
  commits state through the existing memory representation; memory guards
  remain intact.
- **Helper effects.** Record each classified helper's guest reads/writes,
  host-register clobbers and possible fault/control-flow effects. Save and
  reload only the state proven necessary. Unknown helpers retain full
  synchronization, and exceptions must not unwind through generated frames.
- **Entry/exit layouts.** Begin with two connected regions and a canonical
  entry that loads architectural memory state. A specialized entry accepts
  a specified host-register layout. Compatible edges pass values directly;
  adapters implement parallel copies, including cycles, or use the canonical
  path. Register/spill ownership, frame compatibility, lazy flags, helper
  scratch registers and per-vCPU state must be explicit.
- **Bounded adaptation.** Evaluate at most two layout variants per head per
  search, sharing the existing compilation, code and phase budgets. Count
  adapters and recovery metadata, and measure compile amortization separately
  from instrumented execution gain. Preserve work/exit matching in trials;
  evaluate layouts over equivalent guest paths. Publish and revoke variants
  through immutable MT code and tracked target cells. Invalidation must cover
  every constituent guest range and stale entry/adapter dependency.

The first milestones are mixed FP/vector allocation and validated recovery
contracts, followed by handoff between two regions. Expanding the scope
requires measurable benefit and the
[planned correctness/performance gates](archive/TESTS-history.md#planned-codegen-and-coremark-acceptance).
CoreMark's integer/control workload requires its own profile; FP throughput
alone does not predict progress toward 10,000 iterations/s.

Primary design references: QEMU's
[block chaining and exception recovery](https://www.qemu.org/docs/master/devel/tcg.html)
and LLVM's [runtime value-location maps](https://llvm.org/docs/StackMaps.html).
These provide design precedents; performance acceptance depends on Bifrost's
own tests and measurements.

## Native vector FP special cases

Packed arithmetic computes a tentative result in scratch XMM0 before writing
Vd. A NaN lane takes a cold ARM-semantic step, retaining the original operands
and accumulator even when Vd aliases a source. The cold call saves live GPRs,
commits pinned vectors (including values written later on a previous loop
iteration), reloads those vectors, and rejoins after the native result's commit.
Compile-time mappings and dirty flags are preserved on both paths. Q=0 tests
only active lanes. This permits differential replay of the complete block.
The hot mask test uses SSE4.1 PTEST when available, with an SSE2 MOVMSK
fallback. Neither path changes a live GPR. The verifier's one-iteration
self-loop exits also commit cached state when the normal back-edge is disabled.

MIN/MAX also guard their inputs: x86 can hide a NaN by selecting a numeric
second operand. Equal lanes, the only other difference from Arm, are fixed
inline: x86 returns the second input, so FMAX/FMAXNM select `a AND b` (+0
above -0) and FMIN/FMINNM `a OR b`. Only NaN inputs take the cold path. FMLA/FMLS use a single FMA3
operation; hosts without FMA3 use the fused interpreter operation, rather than
separate multiply and add. These changes cover NaN priority/payload/sign,
default NaNs for invalid arithmetic, and FMULX infinity-times-zero results.
Vector FSQRT uses the same guard.

Scalar FP uses the same contract. FADD/FSUB/FMUL/FDIV/FNMUL/FABD, FSQRT,
FMADD/FMSUB/FNMADD/FNMSUB, FCVT S↔D and FRINT* compute in scratch XMM0 and
test it with `ucomis xmm0,xmm0; jp` before the commit. FMAX/FMIN/FMAXNM/
FMINNM also branch on unordered inputs and zero results, so FMAX/FMIN are
now native instead of CALL_INTERP. `emit_fp_cold_tail` places the
interpreter step after the commit and rejoins past it (`emit_fp_interp_step`
is shared with the vector guard). FMADD no longer writes a pinned Vd in
place, so the step sees aliased sources. Hosts without FMA3 step the
interpreter instead of rounding twice. The extra test and branch cost about
3% on a scalar-double n-body loop (1.33 s to 1.37 s).

FPCR.DN is honoured everywhere a NaN can be produced: the shared
`arm_fp_binop`/`arm_fp_muladd`/`arm_fp_sqrt`/`arm_fp_convert`/`arm_frint`
helpers take it, every interpreter caller passes it, and native code
reaches them through the NaN cold steps (a NaN result is the only case DN
changes). FPCR.FZ and RMode, cumulative FPSR flags and enabled synchronous FP
traps remain outside this native arithmetic contract.

Indexed interpreter operations use the same ARM helpers; the native indexed
FMLA helper shares them too. Indexed FMULX has U=1 and the same opcode nibble
as FMUL. The widening integer absolute-difference handler must match its
three-different encoding space, not opcode bits alone: indexed FMLS previously
fell into SABAL and did integer arithmetic on floating-point bit patterns.

References: Arm [FMULX](https://www.scs.stanford.edu/~zyedidia/arm64/fmulx_advsimd_vec.html),
[FMAXNM](https://www.scs.stanford.edu/~zyedidia/arm64/fmaxnm_advsimd.html),
[FMLA](https://www.scs.stanford.edu/~zyedidia/arm64/fmla_advsimd_vec.html) and
[FMLS](https://www.scs.stanford.edu/~zyedidia/arm64/fmls_advsimd_vec.html).

## Android interpreter and generated-code contracts

`BLR` must capture its target before updating X30. ART's nterp uses
`ldr lr, [method, ...]; blr lr`; reading X30 after writing the return address
skips the callee and leaves its input in the result register. Keep the
interpreter and IR translator ordered alike. `jit_blr_lr` checks the exact
link address and compares X30-target calls with ordinary register targets.
See the [Arm BLR pseudocode](https://www.scs.stanford.edu/~zyedidia/arm64/blr.html).

Native BL/BLR callers must unwind when `CPU.running` becomes false, as well
as on `jit_stop_pc`. Otherwise `exit_group` returns through cached native
callers and executes libc's deliberate trap after the syscall.
`jit_call_exit` puts traps after every nested return; exit zero is its oracle.

Literal loads decode to the existing `LDR_IMM` width/extension contract with
`pc_relative=true` and a signed, scaled displacement. Both backends use the
instruction PC as the base, including for negative offsets. Reset the flag
on every decode: cache victims retain their other instruction fields. W loads clear
upper bits; LDRSW sign-extends; S/D loads clear the unused vector bits; Q loads
fill both halves. Literal PRFM is a hint. The reserved SIMD opc=3 encoding
remains illegal. `jit_literal_load` and the generated RX code in
`test_shared_alias` cover these paths. Reference:
[Arm literal-load pseudocode](https://www.scs.stanford.edu/~zyedidia/arm64/ldr_lit_gen.html).

`jit_android_semantics` covers the preceding Android instruction repairs:
W-target sign-extending loads, LDPSW, non-temporal pairs, vector CLZ/CLS,
shifted ADD/SUB to XZR, MOVK W, unconditional AL/NV branches, CLZ W,
FMOV D upper-half clearing (immediate, GPR and FP-register sources),
FCVTZU saturation, UCVTF rounding, byte LSE
atomics, and guest SIGILL for an illegal opcode. Use its `no-trap` argument
for JIT differential replay, which does not deliver the fixture's guest
signal handler.

### Guest memory barriers

Exclusive stores use both the local reservation and a host compare-exchange
against the value loaded by LDXR/LDAXR. A process-local mutex alone cannot
serialize against native JIT LSE instructions or another OS process mapping the
same shared file. Interpreter LSE RMW operations also use native CAS loops.
`test_atomic_stress` includes a deterministic LDAXR / competing CAS / STLXR
regression: the conditional store must fail and preserve the competing value.
This is value-based exclusive emulation; an ABA sequence that changes the word
and restores its original value is not detected across processes. Full
reservation-granule semantics need separate coverage.

LDXP/LDAXP/STXP/STLXP (bit 21, o1, separates them from LDXR/STXR whatever
Rt2 holds) run in the interpreter through `CALL_INTERP` on the same exclusive
monitor (the version table below). One reservation covers the 8- or 16-byte pair; STXP of X registers
uses one host `CMPXCHG16B` (`Memory::compare_exchange_16`). Hosts without
CMPXCHG16B, and misaligned pairs whose alignment fault is not modelled, use a
compare-and-write under the memory lock instead. `test_exclusive_pair` checks
both element orders, W pairs, XZR operands and a contended 128-bit counter.
CASP/CASPA/CASPL/CASPAL (FEAT_LSE, advertised by `HWCAP_ATOMICS`) share that
encoding space with bit 31 clear and Rt2 = 11111; odd Rs or Rt is unallocated,
as in QEMU. They also run through `CALL_INTERP`: one 8-byte compare-exchange or
`compare_exchange_16` (a full barrier; they no longer take a monitor lock), the
old value always returned in Rs (lower address) and Rs+1. `test_lse_inline` covers match, mismatch, W-pair
zero extension and a contended 128-bit CASPAL counter.

Helper and interpreter stores (`Memory::write`, used by STLR, slow-path
stores and every interpreter store) must reach the host exactly once per
element. glibc's x86-64 `memcpy` copies runtime sizes 2-32 with overlapping
head/tail moves, so an 8-byte STLR was written twice. IL2CPP's Boehm GC lock
(SWPAB acquire through outline atomics, `stlr xzr` release) let a second thread
in between the two stores; the replay then released that thread's lock, two
allocators popped one free-list entry, and SpongeBob crashed with corrupt
objects (2026-10-13). `Memory::write` now issues one access per 2/4/8-byte
element and two 8-byte accesses for 16 bytes. The `test_lse_inline` contention
case fails about 4 of 5 runs without the fix (`STLRB` releases were never
affected); QEMU and the interpreter pass. Reads are unchanged: duplicated loads
return one complete value.

DMB and DSB decode through `HINT`, but must retain their memory effects.
`is_data_memory_barrier()` recognizes their encodings including the domain/type
option, and `data_memory_barrier_types()` reads the type from CRm<1:0> as Arm's
DMB pseudocode does (01 loads, 10 stores, 11 and 00 all). Translation emits the
side-effecting `MEMORY_BARRIER` IR operation with that kind
(`BarrierParams`); it stays in the IR for every kind, so no pass moves a guest
access across it. The interpreter executes a sequentially consistent host fence
for every barrier.

What the JIT emits (2026-11-03):

- **DMB LD and DMB ST** (`ishld`, `ishst`, any domain): nothing. x86 TSO never
  reorders load/load, load/store or store/store for write-back memory, which is
  all guest memory is (GL buffer maps are bounce copies, gralloc buffers memfd,
  and generated code issues no non-temporal stores). Linux's x86 `smp_rmb()` and
  `smp_wmb()` are compiler barriers for the same reason.
- **Full DMB and every DSB**: `lock not qword [cpu.jit_fence_word]`. TSO still lets
  a younger load pass an older store; a locked instruction orders both (Intel SDM:
  reads and writes are not reordered with locked instructions), as Linux's x86
  `smp_mb()` does with `lock addl $0,-4(%rsp)`. NOT changes no flags, so lazy NZCV
  survives, and the word is per CPU. MFENCE, used before, additionally orders
  non-temporal and write-combining accesses, which guests cannot make here.

On a Ryzen 5 9600X a guest loop of store, barrier, load went from 16.8 ns to 4.05 ns
per iteration for `dmb ish` and from 17 ns to 0.55 ns for `dmb ishld`/`ishst`
(installed build against this one, one core). In a Subway Surfers menu profile
one `dmb ish` in Unity's job queue (`libunity.so+0x57d3fc`) had cost about 8% of
all translated-code time, and MFENCE about 9% overall; that profile has not yet been
repeated with this change.

This covers ordering of ordinary guest memory; it does not establish device-memory
or privileged cache/TLB semantics. ISB and the remaining hints retain their existing
behavior. References: Arm [DMB](https://www.scs.stanford.edu/~zyedidia/arm64/dmb.html)
and [DSB](https://www.scs.stanford.edu/~zyedidia/arm64/dsb.html), Linux
[arch/x86/include/asm/barrier.h](https://github.com/torvalds/linux/blob/master/arch/x86/include/asm/barrier.h).

`test_atomic_stress` includes two-worker store-buffer tests for DMB/DSB ISH/SY,
100,000 rounds each, a DMB ISHST/ISHLD message-passing test (2,000,000 values),
and GPR/NZCV preservation across additional options. With full barriers emitting
nothing the store-buffer tests saw 16 to 29% forbidden outcomes; with `lock not`, and
in the interpreter, zero. On x86 the message-passing test can only fail if
translation reorders guest accesses, so it guards the JIT rather than the host.
QEMU was not available for the 2026-11-03 change. These bounded tests do not prove
every multithreaded workload.

### Indexed memory accesses and fault retry

Base-register writeback for pre/post-index LDR/STR and LDP/STP belongs after
successful memory access, including SIMD transfers. A missing userfaultfd page
raises SIGBUS before the instruction completes; Android's handler fills the page
and retries the same PC with the original base. JIT guard recovery executes the
interpreter, so this ordering is required in both modes. SpongeBob's ART
Class::FindField exposed the old early writeback in `ldr w8,[x1],#4`: retry skipped
the array header and treated a declaring-class pointer as its length.
`test_userfaultfd` checks that exact warmed instruction and pre/post-index,
store, pair and page-crossing vector variants, including the saved signal
context's base register. The ordering follows the
[Arm LDR operation](https://www.scs.stanford.edu/~zyedidia/arm64/ldr_imm_gen.html).

### APK resource icon instructions

Rendering the installed launcher's icon through Android Skia exposed vector
FCVTNU (nearest-even unsigned conversion), FRINTP (round toward positive infinity),
and scalar D-register CMGT/CMGE/CMHI/CMHS. Their interpreter implementations are
also the JIT fallback. Inputs are captured before destination writes and unused
vector bits clear. FCVTNU uses nearest-even independently of FPCR rounding,
saturates unsigned results and records conversion exceptions. The existing
FRINT family retains its bounded exception model; synchronous FP traps are not
implemented. `jit_simd_fixconv` compares aliases, widths, ties, saturation,
NaNs, directed rounding and signed/unsigned comparisons with QEMU.
References: [Arm FCVTNU](https://www.scs.stanford.edu/~zyedidia/arm64/fcvtnu_advsimd.html),
[Arm CMGT](https://www.scs.stanford.edu/~zyedidia/arm64/cmgt_advsimd_reg.html).

## Translation path

```text
AArch64 word
  → decode() / DecodedInst
  → translate_to_ir() / typed IR
  → optimize_ir() and validate_ir_block()
  → compile_ir_inst() / x86_64 code
  → block chaining and optional Tier-2 region compilation
```

| Responsibility | Main files |
|---|---|
| Decode AArch64 encodings | `Source/Interpreter/Decoder/Decoder.hpp`, `Source/Interpreter/Decoder/Decoder.cpp` |
| Translate decoded instructions to IR | `Source/FrostJIT/IR/Translate/Integer.cpp`, `FloatSIMD.cpp`, `Memory.cpp` |
| Define typed IR parameters and factories | `Source/FrostJIT/IR/IR.hpp`, `Parameters.hpp`, `Factories.hpp`, `Builder.hpp` |
| Optimize and validate IR | `Source/FrostJIT/IR/Passes/Optimize.cpp`, `Source/FrostJIT/IR/Passes/Validate.cpp` |
| Construct, verify, optimize and lower scalar SSA | `Source/FrostJIT/IR/SSA/SSA.hpp`, `Source/FrostJIT/IR/SSA/SSA.cpp` |
| Analyze and compile Tier-1 blocks | `Source/FrostJIT/Compiler/BlockAnalysis.cpp`, `BlockCompiler.cpp`; replay footprints in `VerificationStores.cpp` |
| Emit x86_64 for IR operations and exits | `Source/FrostJIT/Compiler/FrostJIT.cpp`, `Source/FrostJIT/CodeGen/Integer/`, `CodeGen/FloatSIMD/` |
| Allocate host registers and spill vregs | `Source/FrostJIT/RegisterAlloc/RegisterAllocator.cpp` |
| Dispatch blocks and promote hot regions | `Source/FrostJIT/Runtime/Dispatch.cpp`, `Source/FrostJIT/Tier2/` |
| Define reference instruction semantics | `Source/Interpreter/Core/`, `Source/Interpreter/FloatSIMD/` including `SIMD/` |

Tier-2 compiles region IR through the normal IR code generator, with its own
region liveness, loop, and exit handling. An IR operation that works in a
single Tier-1 block can still need region-specific handling when it affects a
region boundary or control-flow exit.

## Scalar register allocation and block sizing

Scalar ALU emission reuses a temporary's host register after its last use,
while preserving architectural values, live aliases and loop pins. Immediate
MUL uses IMUL only when its constant fits the instruction's signed immediate;
larger constants retain the register form. ADD and eligible immediate SUB use
LEA when needed to preserve lazy NZCV. Narrow ZEXT materializes live flags
before allocating operands or emitting a flag-clobbering mask.

ADDS/SUBS and TST use their allocated host registers rather than forcing
operands through RAX/RCX. Flag arithmetic consumes a last-use scratch input
when possible, while live inputs and loop pins retain their values. An unused
SUBS result emits CMP instead of overwriting and preserving its first input;
32-bit CMP/TST still preserve the full 64-bit input containers. CMP's operand
encoding and SUB-equivalent flags follow the
[Intel SDM, Volume 2A, CMP](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf#page=279).
Two-input allocation temporarily pins an already-cached first operand before
loading the second, preventing an eviction followed by an immediate reload.

With the default block cap and guard settings, one matched CoreMark compile
recorded **455 → 394 emitted spills** across the same 354 benchmark-body blocks
(13.4% fewer). This is a compile-time instruction count including cold fault
paths, not a measurement of executed spill traffic. See
[the validation record](archive/TESTS-history.md#flag-arithmetic-register-pressure-2026-10-11)
for the independent value/flag checks and timing bounds.

The default block instruction limit remains 32. `BIFROST_JIT_BLOCK_LIMIT=8..128`
allows bounded comparisons; both Tier-1 and the Tier-2 collector stop growth
at the instruction cap or the shared IR/vreg complexity budget. A Tier-1 loop
body may reach twice the cap: at the cap `selfloop_extent` looks for a branch
back to the block start, and an already compiled loop tail is absorbed under the
same limit ([details](#vector-fmla-by-element-ld1r-loop-bodies-and-q-accesses-2026-11-03)). Long blocks
are no longer automatically demoted solely for exceeding 32 instructions;
actual interpreter-fallback density still controls demotion. Larger limits
are diagnostic options, not a verified performance default for every game.
The Tier-2 discovery, feedback and measure scripts unset the variable: their
expected region shapes assume the default cap.
`jit_bitfield_regalloc` covers immediate multiplier signs/widths, aliases and
a long body with fifteen live GPRs.

Belady eviction reads per-vreg use lists (`vreg_uses_`, 4,096 entries).
Each Tier-1 and Tier-2 use-scan starts with `reset_vreg_uses()`, which
empties only the lists that the previous scan filled (`note_vreg_use`
records them) and keeps their storage. Outside a compile every list is
empty, so `next_use_after` returns -1 as before. Rebuilding all 4,096
lists per block had been about a third of Tier-1 compile time in a
SpongeBob startup profile (2026-10-13).

## Scalar SSA

`Source/FrostJIT/IR/SSA/SSA.hpp` and `Source/FrostJIT/IR/SSA/SSA.cpp` define a scalar SSA graph with
explicit basic blocks, predecessor/successor edges, single-definition values,
and predecessor-labelled phis. Sealed-block construction resolves reads before
local definitions and loop backedges; trivial phis and copies are removed to a
fixed point. GPR slots and FP low-half slots have separate variable namespaces.
All scalar values are 64-bit containers; explicit ZEXT/SEXT and typed operation
parameters retain narrower instruction semantics.

The verifier checks unique definitions, complete phi predecessor lists,
consistent CFG edges, use dominance, same-block definition order, and phi uses
on predecessor edges. Construction and lowering validate transactionally:
failure retains the original IR. Value numbering uses dominator-scoped
availability, so sibling-branch expressions cannot supply each other's values.
Constants are not value-numbered: the backend folds an IMM into its single,
immediately following consumer as an x86 immediate, and a shared constant
register defeats that. Lowering also rematerializes a constant reached by more
than one use (for example through a STORE_REG/LOAD_REG pair) right before each
consumer that can fold it as src2, using the backend's fold rules (any shift
count; ALU constants that sign-extend from 32 bits). Tier-1 input loads are
placed at their first use in the entry block, never after a store to their own
slot and never between an IMM and its consumer; hoisting every input to the
block start held all live-ins at once against ten host registers.
Def-use traversal removes dead pure computations. Memory accesses, flags,
system-register operations, barriers, and guest-state stores retain order;
loads from guest memory are never commoned or speculated by SSA. Flag consumers
remain ordered effects because NZCV is not yet an SSA value.

Tier-1 runs this pipeline for supported scalar blocks before the existing
constant folding passes. `BIFROST_NO_SSA=1` selects the legacy path;
`BIFROST_NO_FWD=1` and FWD range restrictions also bypass Tier-1 SSA forwarding.
The legacy forwarding cache does not run again on an SSA-lowered block.
`BRCOND_SKIP`, direct FP/SIMD operations, calls/helpers, and atomics retain their
existing pipeline until their operand and state effects are modeled explicitly.
This is scalar value SSA, not MemorySSA or a complete FP/vector conversion.

Experimental Tier-2 superblocks build one scalar SSA graph across all their
blocks, with a separate entry and distinct conditional side exits. Retained
single-thread loops include the actual backedge; linear and MT paths use an
acyclic graph. Cross-block value numbering runs on this graph before
lowering. Architectural phis lower to guest-state reads at the join: retained
STORE_REG operations commit incoming values, pinned registers carry their
current value, and non-pinned deferred stores commit on the backedge. This
keeps the backend's existing precise side-exit and memory-fault state contract.
It also avoids assuming that temporary homes survive its loop boundary.
Regions with temporary phis or unsupported effects retain the existing path.
Multithreaded regions retain their existing bounded, single-iteration path.
Tier-2 remains opt-in; SSA does not change its promotion or timing policy.

The general SSA destructor also supports parallel edge copies and splits
critical edges. `make test-ir-ssa` executes independent non-SSA IR oracles for
diamonds, cyclic loop-carried swaps, critical edges, architectural-state phi
lowering, width-sensitive value numbering, and GPR/FP namespace separation;
malformed graphs must fail validation. `make verify` runs this host test before
the differential/threaded gate. The Tier-2 discovery oracle additionally runs
generated loops with twelve carried GPRs, beyond the available host pins.
These tests cover the scalar contracts above, not arbitrary FPCR/FPSR,
concurrent memory reordering, or whole-program optimization.

Construction follows [Braun et al., Simple and Efficient Construction of SSA](https://pp.ipd.kit.edu/uploads/publikationen/braun13cc.pdf).
The verifier's dominance and predecessor-edge rules match the
[LLVM SSA and phi contracts](https://llvm.org/docs/LangRef.html#phi-instruction).

## Scalar memory guards and NZCV packing

Native scalar loads/stores select guard scratch registers through the allocator
instead of discarding every caller-saved mapping on each access. Bounds,
permissions, userfaultfd missing-page bits, and crossing-page checks remain.
The software-check flag is read on every access; another guest CPU may raise it
while a block executes (see [its section](#software-page-checks-only-while-a-page-needs-them-2026-10-26)). The result register is separate from
the address, preserving inputs if the host access faults. Cold recovery commits
pre-instruction guest state and steps the instruction before returning to the
dispatcher. Paired instructions retain the existing whole-footprint guard;
forced software guards and slow-store tracing retain their existing path.
`jit_memory_guard_address` covers live-register pressure, negative offsets,
and byte/halfword/word/doubleword accesses; the host-fault and userfaultfd
fixtures cover recovery and retry behavior.

NZCV materialization packs adjacent host SF/ZF together, preserving the guest
carry convention and overflow bit. BRCOND exits omit saving/restoring RFLAGS
after their condition has already been consumed; both exits still commit NZCV.
Other consumers retain RFLAGS preservation. `jit_flags_passthrough` checks
32/64-bit add/sub results and all NZCV bits on both branch paths.

## Adding or changing an instruction

1. Check that `decode()` classifies the encoding correctly and extracts every
   field needed by both execution paths.
2. Keep interpreter semantics correct. Unsupported operations must raise the
   normal guest decode error; they must not silently act as NOPs.
3. Add the opcode in `Source/FrostJIT/IR/IR.hpp`, its parameter struct in `IR/Parameters.hpp`
   and its factory/reader in `IR/Factories.hpp`, then
   validate the operation's parameter contract in `Source/FrostJIT/IR/Passes/Validate.cpp`.
   IR parameter fields are private: producers use factories, consumers use
   readers, and optimizers must preserve the operation's semantics.
4. Translate through those factories in `Source/FrostJIT/IR/Translate/`. Do not
   patch `insts.back()` fields after emission; forward branch skip counts use
   `set_skip_count()` for their documented fix-up.
5. Add code generation in the matching `Source/FrostJIT/CodeGen/` operation family.
   Check register aliasing, W-register zero-extension, Q=0 upper-half rules,
   host feature gates, and interpreter fallback behavior.
6. Keep the block-splitting prediction in `Compiler/BlockAnalysis.cpp` aligned with
   fallback behavior. It is a pre-translation heuristic. The final
   interp-only decision and `BlockEntry::call_interp_count` use the
   post-optimization IR `CALL_INTERP` count.
7. Consider both Tier-1 and Tier-2 control flow, then add a focused guest
   regression under `Tests/Unit/` or `Tests/Apps/` and register it in
   `Scripts/Testing/run_tests.sh`.

Generated operation tables live under `Tools/CodeGen/`; when a family is in a
generated table, use its classifier instead of adding another handwritten
encoding mask. Regenerate generated headers with the corresponding opgen
script rather than editing generated output alone.

## Useful checks

```bash
BIFROST_IR_VALIDATE=1 ./bifrost-emu Tests/Unit/some_regression.elf
BIFROST_JIT_VERIFY=1 ./bifrost-emu Tests/Unit/some_regression.elf
./bifrost-emu --no-jit Tests/Unit/some_regression.elf
./Scripts/Testing/run_tests.sh --filter 'jit|simd|fp'
./bifrost-emu Tests/Unit/coremark.elf
```

`BIFROST_IR_VALIDATE=1` checks per-op IR contracts, including Tier-2 regions.
`BIFROST_JIT_VERIFY=1` compares JIT execution with the interpreter; syscalls,
in-block calls, and memory side effects have documented verification limits,
so pair it with a focused result check when relevant. Replay is suspended
with an explicit diagnostic once a second guest CPU starts: restoring shared
memory or patching live code during concurrent execution is unsafe.
`make verify` uses single-thread fixture modes and separately runs threaded
result oracles; suspension in a differential fixture fails the gate. CoreMark is a useful
compute workload, but it does not replace application workloads or targeted
correctness regressions.

### Where host time goes: `BIFROST_PROF` (2026-11-04)

```bash
BIFROST_PROF=1 BIFROST_PC_HIST=1 bifrost-emu -v program      # report at exit
BIFROST_PROF=1 BIFROST_STATS_PERIOD=30 bifrost-emu program   # and every 30 s
```

`BIFROST_PROF=1` samples the host pc at 100 Hz of CPU time (`ITIMER_PROF`) and prints, besides the dispatch / translate /
interpreter / other split, one line for the JIT samples: how many, on how many threads, how many were lost, and how they divide
into the parts of a translated block. **Entry** loads the canonical register layout, **body** is the translated instructions,
**exit** publishes state back, **cold** is recovery and exit code (the same four parts as the `entry`, `tier1-ir`, `exit` and
`cold` rows of the allocation profile), and **shared stubs** is code outside any registered block: the frame-exit and cold-path
stubs, and blocks loaded from the persistent cache, which are not registered. Below it come the busiest eight threads, and with
`BIFROST_PC_HIST=1` the twenty hottest guest blocks (`pc=` values map to functions with `aarch64 objdump`).

Under the report a second line splits the **body** samples by what the operation under them does: guest state moves
(`LOAD_REG`/`STORE_REG`), memory (a load or store with its guest-address guard, and atomics), integer ALU, flag setting, branches,
helper calls, FP and SIMD, and "between operations" (code in the body that belongs to no single operation). The same line gives
the share of body time that fell inside an operation whose code contains allocator spills or reloads. That share is an upper bound
on spill cost: the operation's own work is in the same range, and I cannot separate the two. The code that ends a block (the
indirect branch of `ret`, flag and register commits) is in the exit section, not the body. The operation table costs 16 bytes per
IR operation translated while `BIFROST_PROF` is set (capped at 16 Mi operations; a note says when it is full).

Samples go to a per-thread ring of 16,384 and are resolved into running totals whenever a report is printed, when a code buffer
is released and when a new thread finds every ring taken; a thread that ends leaves its ring to the next of these. Resolving
sorts only the blocks and operation ranges registered since the last time and merges them in: re-sorting the whole table at
every thread exit cost about 5% of a game's run. A thread that outlives 164 s of samples between two reports overwrites its oldest ones; those
are counted as *lost*, not folded into a bucket, so `BIFROST_STATS_PERIOD` keeps a long run complete. The numbers are time, not
instruction counts: a 10 ms sample says where the host was, not how often a block ran. Sampling and section registration
(`Runtime/SampleProfile.cpp`) run only when the variable is set; otherwise the translator tests one flag per compiled
instruction (its cost was not measured). `Tests/Unit/jit_profiler.cpp` checks the classification at
each section boundary, across threads and after a code buffer is released, with synthetic samples and no timing.

`BIFROST_REGALLOC_PROFILE=path` writes a row per emitted IR range (`%p` in the path is the process id). It is large: an Android
game wrote 3.3 GB in 15 seconds. The file now stops at `BIFROST_REGALLOC_PROFILE_MB` (default 256 MiB, counting what the file
already holds, since it is appended to), prints a notice on stderr and leaves a `# cap reached` line; blocks translated after that
are not recorded.

### Fault, block-entry and FWD bisection traces

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
  ranges, including Tier-2 regions that otherwise force it on.

Ranges are guest pcs, `[lo, hi)`. They are only stable when the code's load
address is: Android's ART relocates its boot image on every run, even with
`BIFROST_NO_ASLR=1`. To compare two modes, diff their fault and block traces
by method rather than by fixed address ranges.

## Logical immediates and SP destinations

Non-flag-setting AND/ORR/EOR immediate instructions write SP (or zero-extended
WSP) when Rd is 31. Rn is still XZR/WZR when it is 31. ANDS immediate and all
logical shifted-register forms instead discard an Rd=31 result. Keep the
decoder's `writes_sp`, interpreter and IR `STORE_REG` behavior consistent;
SP-writing instructions must also remain excluded from leaf inlining. See
the [Arm AND immediate operation](https://www.scs.stanford.edu/~zyedidia/arm64/and_log_imm.html).

Unity's `libunity.so+0x6ab608` uses `and sp,x9,#0xffffffffffffffc0`
after reserving a 0x520-byte frame. Ignoring this write left SP at its old
value, so the local zero store at `+0x6ab634` overwrote the caller's non-null
descriptor. The later virtual call at `+0x6ab890` then dereferenced zero.
The narrow block/debugger trace observed the descriptor before and after that
store. `jit_android_semantics` includes this exact frame-overlap reproducer,
all three logical immediate operations in both widths, zero-register sources,
flag preservation and SP-preserving zero-register destinations. Its 72 checks
pass QEMU, JIT and interpreter; the no-trap differential mode has 69 checks.
The fixed game trace confirms the aligned SP and intact descriptor at the
virtual call. It then exposed a separate invalid pointer store at
`libunity.so+0xca6e54`, caused by the GLES mapping policy described below.

## GLES mapped buffers and the SpongeBob renderer

The GLES `glMapBufferRange`, `glUnmapBuffer` and `glFlushMappedBufferRange`
entries must use the same guest-buffer policies as their GL counterparts.
Forwarding the map call directly returned a host pointer, which Unity stored
and later dereferenced from guest code at `libunity.so+0xca6e54`. The GLES
regression reproduced this invalid guest load before the policy change.
Afterward both GL and GLES modes of `Tests/Apps/test_sdl_gl_mapbuffer.elf`
pass 21 checks; GLES also passes in the interpreter. The GLES test mode loads
that namespace first, because thunk `dlsym` uses the first global definition.
It tests GLES entries in a desktop GL context, not a native ES context.

The real Android SpongeBob run now uploads buffers, draws, and calls
`eglSwapBuffers`; the user observed the Nickelodeon logo. Mark that EGL entry
`PRESENT` so the lightweight frame trace includes its swaps. This is rendering
progress, not confirmation of interactive gameplay. The first rendered run
ended with status 137 after four swaps because the strict Java launcher
rejected `reportActivityFullyDrawn`. After that and the subsequent service
calls were implemented, the game rendered 670 frames and reached an unhandled
`frecps v3.4s,v16.4s,v18.4s` (0x4e32fe03), which raised guest SIGILL. See the
current service checkpoint in [ANDROID_RUNTIME.md](Android/ANDROID_RUNTIME.md).

These policies share the buffer emulation, which maps the host buffer and
gives the guest a bounce of the same range (see the graphics guide). Flush
offsets are relative to the mapping, as the
[Khronos specification](https://wikis.khronos.org/opengl/GLAPI/glFlushMappedBufferRange)
requires. `test_sdl_gl_mapbuffer` checks valid GL usage only and passes natively
on the host driver as well; it is not an ES 3 conformance test.

## Tier-1 zero and bit branches

CBZ/CBNZ and TBZ/TBNZ first commit pending NZCV with `clobber_flags()`.
Their TEST/BT instructions can then overwrite host flags: guest flags remain
in `cpu.pstate`, so the branch needs no PUSHFQ/POPFQ. Operands are tested in
their allocated registers instead of copied through RAX. W-form TEST reads
only the low 32 bits without changing the source. The next-PC MOV still
spills any dirty RAX occupant before overwriting it. Immediate BT uses `/4`;
`/5` is BTS and would mutate an operand tested in place. The encodings follow
Intel's [instruction reference](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf).

`jit_flags_passthrough` checks all sixteen NZCV combinations, W/X zero
branches, low/high bit branches, unchanged source values and warmed chains.
Its 69,152 checks pass QEMU and differential replay. Five alternating runs of
a 50-million-iteration CBNZ loop measured median times of 0.586 s before and
0.372 s after this change (36% less time). This isolates branch overhead;
it does not establish a game FPS or loading-time improvement.

## Conditional compares and register forwarding

CCMP and CCMN only define NZCV flags. Their IR `dest=0` is a placeholder,
not a write to X0. `optimize_ir()` must preserve X0's forwarded value across
these operations; treating the placeholder as a destination lets a later CSEL
read stale architectural state when dead-store elimination removes an earlier
STORE_REG. `Tests/Unit/jit_ccmp_forward.c` covers the FreeType rasterizer pattern
that exposed this as missing glyph pixels in Neverball. See
[the Neverball validation notes](Apps/neverball.md) for the application repro.

## Same-operand two-register ops

FWD forwards both reads of `cmp w23, w23` to one vreg. ADDS/SUBS and TST
accept the same host register for both inputs via `ensure_two_vregs()`;
ADCS/SBCS still use `force_two_vregs_to(v, RAX, v, RCX)`.
A vreg has exactly one home: it stays in RAX, and RCX receives an unmapped
copy when fixed placement is needed. Those callers compute in RAX, so their
"spill a live src1 before overwriting RAX" logic sees the real home. An
allocation for a separate destination must not pick RCX (ADCS/SBCS pin it). Earlier code homed the
vreg in RCX while RAX still claimed it; SUBS then overwrote RAX without a
spill, and a later `mov w5, w23` read a never-written stack slot. ART's
boot image inlines `Arrays.copyOf` that way, so `System.arraycopy` got an
object address as its length (ArrayIndexOutOfBoundsException, seen only
when ART used its AOT boot code). `Tests/Unit/jit_same_operand.c` covers each
flag op and the ART sequence around an in-block call. Its pressure cases also
keep twelve independent values live across W/X flag arithmetic, checking
both destination aliases, CMP/CMN/TST inputs, result widths and NZCV.

## Shift source preservation and operand width

Variable shifts and rotates must preserve a live count vreg. The x86 CL forms
mask the effective count in hardware; an explicit AND on the cached RCX value
destroys source bits that a later IR instruction may read. Keep every W-form
shift at width 32, including shifted operands produced by `apply_shift()` and
constant folding. See [zstd regression notes](Apps/zstd.md) and the
`jit_variable_shift_source` / `jit_shifted_operand_width` regressions.

## Bitfield insertion and NZCV passthrough

BFM must apply both architectural write and top masks. For BFXIL
(`imms >= immr`), the destination mask is only the low `imms-immr+1` bits.
Rotating the write mask alone also inserts unwanted bits at the top of Rd.
This corrupted libX11 request sequence numbers and broke XWayland replies.
`jit_bfm_masks` covers all 5,120 W/X immediate pairs with 36 source/destination
combinations each (184,320 checks), independently checked under QEMU.

Cross-block NZCV materialization cannot be removed using only a successor's
local flag-consumer scan. A flag-neutral block may forward flags to another
block, return them to its caller or be replaced by invalidation. The JIT now
preserves NZCV on cross-block conditional exits and no longer retroactively
patches those stores out. Existing internal self-loop handling remains.
`jit_flags_passthrough` covers taken and fall-through edges through neutral
blocks with a later conditional consumer, including warmed chained execution.
Omitting the store when the successor itself overwrites the flags first is a
different, provable case: "Dead NZCV stores at block exits".
The old JIT fails both regressions. `xwayland_wire` additionally checks a real
guest libX11 connection, 100 round-trips and server-side pixel readback.

## Integer vector multiply and Q width

The SSE4.1 lowering of 32-bit vector `MUL` uses `PMULLD`, encoded as
`66 0F 38 40 /r`. The previous `66 0F 38 5F /r` sequence was invalid and
crashed Doom 3 during script initialization. `SimdArithParams::q` carries
the architectural vector width through the factory, translator and codegen:
Q=0 writes the low 64 bits and clears `v_hi`; Q=1 computes both halves.
`Tests/Unit/jit_simd_mul32.c` covers overflow, source/destination aliasing and both
widths with fixed expected products, independently checked under QEMU.

## Shared SIMD fallback semantics

Android linker bootstrap exposed SADDLV/UADDLV aliasing the saturating
accumulate interpreter handler after bit 16 was discarded. Their reference
implementation now widens the horizontal sum correctly. H/S/D vector ADDP
also has width-aware fallback semantics; byte ADDP remains native and loads
its second source before writing an aliased destination. `jit_simd_addlv`
checks 2,624 cases in both engines and QEMU.

CRC32 decoding uses opcode bits[1:0] for input size and bit 2 for the
Castagnoli polynomial, with sf=1 only for the X input form. The former
mapping rejected valid X/C forms and confused CRC32CB with CRC32X. The
reference tables are now generated at compile time from the reflected
polynomials instead of erroneous hand-copied entries. `jit_crc32` checks
2,048 cases against a bitwise oracle, QEMU and differential verification.
CRC32 retains the normal interpreter fallback in JIT mode.

Vector FMOV immediates are native: the modified-immediate translation expands `.2S`/`.4S`/`.2D` constants into the
`SIMD_MOVI` broadcast (it used to leave cmode 0xF to the interpreter, 7.6% of a game's CPU). `.2D` with Q=0 is
unallocated and raises SIGILL in both engines, as in QEMU. Scalar
DUP element reads now use native `SIMD_SCALAR_DUP`; their reference bugs affected both engines and
can escape JIT-versus-interpreter verification. Two Doom 3 collision-code
probes exposed this: `.2s #0.5` expanded with a wrong exponent, and
`mov s24, v24.s[1]` selected the low lane as a 64-bit element. FMOV's
single-precision exponent prefix for imm8 bit 6 set is `0x3E000000`.
Scalar DUP's element size is `1 << ctz(imm5)`, with the remaining higher
bits selecting the lane; testing bit 3 before bit 2 misdecodes S lane 1.
`jit_simd_fmov_imm` checks all 768 valid immediate/width combinations and
`jit_scalar_dup` checks all 30 valid element/width combinations, including
same-register aliasing. Both expected results are independently checked
under QEMU.

## Registered native memory faults

Halfword LSE operations require both correct x86 prefix ordering and a
zero-extended old-value result. Emit `LOCK, 66, REX, 0F B1` for the
LDSET/LDCLR/LDEOR CAS loop: a `66` after REX makes the processor ignore REX,
changing `[r9], r8w` into `[rcx], ax`. This caused a real WebView renderer
SIGSEGV at address 2. Load the expected halfword with MOVZX, and zero-extend
the old value after halfword XADD/XCHG; a 16-bit write alone preserves stale
upper register bits. See Intel's [prefix ordering contract, section 2.2.1](https://cdrdv2-public.intel.com/858446/253666-088-sdm-vol-2a.pdf).
`test_lse_inline` checks five halfword operations, full X results, dirty source
upper bits, overflow, and neighboring halfwords against the interpreter and
QEMU. `jit_host_fault` also checks halfword LDSET read/write fault recovery on
four CPUs. These additions do not establish all LSE orderings or contention
patterns.

Every LSE operation and access size is native inside the reservation; only
the out-of-window slow path calls the interpreter. Byte forms use the even
r/m8 opcodes (`86`, `0F C0`, `0F B0`, `00`/`08`/`20`). LDSMAX/LDSMIN/LDUMAX/
LDUMIN share the CAS loop and compare 64-bit sign or zero extensions with
`cmp`+`cmovcc`. In the CMPXCHG pseudocode the accumulator is written only when
the comparison fails, so CAS zero-extends RAX afterwards. Without that, a
successful 32-bit CAS kept stale upper Ws bits. Verify mode records ATOMIC
footprints like STORE_MEM, so replay does not see the JIT's write.
`jit_lse_widths` covers all of this, with a `single` argument for differential
replay.

Ordinary scalar loads/stores, single 16-byte vector accesses and native LSE
atomics use host page protection instead of consulting two guest page flags on
every access. The reservation bounds check remains: adding an arbitrary guest
address to a host base must never wrap into unrelated host storage. A single
`Memory::jit_software_checks_` byte (at `window_base - 8`, tested as
`cmp byte [r10-8], 0`) selects the software permission path while some page is
userfaultfd registered and missing: the emulated missing bits do not change host
page protection, so such a page reads zero natively. Write-only and execute-only
pages are readable, as Linux arm64 maps them on a CPU without Enhanced PAN, so
they need no software path. Already compiled code observes the flag without
invalidation. Pair and multi-register accesses, exclusive
helpers and IR without a guest PC retain the original checks.

`FaultHandler.cpp` installs SIGSEGV/SIGBUS handlers once. A fault is eligible only
inside a native `FaultScope`, at an exactly registered native instruction PC,
with the expected R10 window base and a fault address inside the reservation.
The handler performs lock-free metadata lookup and changes the saved host RIP
to the generated recovery stub. It does not lock, allocate, interpret or deliver
a guest signal. The stub commits the state preceding the guest instruction,
including loop-carried GPR/vector pins, then returns to dispatch after stepping
the instruction. SIGSEGV replay determines guest MAPERR versus ACCERR; a host
file-backed SIGBUS is delivered directly because replay through a host memcpy
would fault again. Unregistered host faults retain the prior handler or default
host signal disposition. See Linux's [sigaction context contract](https://man7.org/linux/man-pages/man2/sigaction.2.html)
and [async-signal-safe operations](https://man7.org/linux/man-pages/man7/signal-safety.7.html).

Each JIT owns a stable, address-sorted table of eight-byte atomic records. A
`MAP_NORESERVE` allocation reserves the worst case (four times the code-cache
budget in virtual address space), while only appended records consume physical
pages. Release publication precedes execution. Codegen rollback removes only
unpublished tail records; published code and records remain alive until the
owner has joined its CPUs. Metadata/handler installation failure leaves the
software guard enabled. Native entry through `run_block` scopes recovery;
embedders invoking a compiled function directly must use `FrostJIT::FaultScope`.

`BIFROST_JIT_SOFTWARE_GUARD=1` selects the old checked path for comparison.
Allocation guards and forced slow-store tracing also select it. Fault tracing
adds a `[jit-fault]` line outside the host handler for each redirected access.
`jit_host_fault` checks repeated scalar/vector/LSE faults on four CPUs, including
saved NZCV and register state; `interp_host_fault` runs its main-CPU mode.
Its optional `bus` argument checks scalar/vector accesses past a shared file's
EOF under native JIT. File EOF faults in software-only, interpreter and paired
access paths are outside this regression's coverage. The interpreter's worker
loop currently reports memory faults as uncaught exceptions; the four-CPU row
is therefore JIT-only. QEMU passes both fixture modes and the optional bus probe.

## Code-cache pressure and shared dispatch

The JIT reserves a stable virtual code range of at most 1 GiB and grows its
logical capacity from 64 MiB as code is emitted. Anonymous pages acquire
physical backing on writes; reservation does not allocate 1 GiB of RAM.
Published code is never moved or recycled because cached function pointers
and live call frames can still reference it. At the budget limit, one failed
compilation marks the budget exhausted; subsequent new PCs receive cached
single-step interpreter entries without repeating decode and code generation,
and the emulator replaces the whole instance (see the generations section at
the top: the full one is released once no thread can run it). An instance that
cannot be replaced, because the previous one is still pinned, stays in that
interpreter fallback. Branch-fixup writes check buffer bounds even after an
emission overflow.

Call-target lookups honor cached interpreter entries. Shared block-table
reads and publication hold `blocks_mutex_`, with a separate locked lookup for
compiler callers that already own it. TLS caches track their JIT owner and
a globally unique invalidation generation, including negative entries.
Mapping invalidation and the transition to multithreaded execution invalidate
TLS entries across CPUs at their next dispatch. Multithreaded mode disables
rewrites of published instructions; its chaining and Tier-2 publication use
atomic data cells. Code is not recycled while another CPU can execute it.

`BIFROST_STATS_PERIOD` reports cache usage, capacity, budget, growth and
overflow counts. The cached settings `BIFROST_JIT_CACHE_MB`
(default 512, the budget before the JIT is replaced) and `BIFROST_JIT_CACHE_INITIAL_MB`
(default 64) accept 1..1024 MiB; initial capacity is clipped to the budget.
`Scripts/Testing/test_jit_cache.sh` checks a three-thread guest with a forced
1 MiB budget (repeated renewals, exact results) and a growth run (no renewal,
no repeated translation of cached fallback targets).

Range invalidation queries an ordered, conservative index from 256-byte guest
granules to the PCs of published blocks whose own or inlined/Tier-2 dependency
ranges touch that granule. Data-only ranges with no indexed granule return
under the shared lock, without blocking table readers; code hits take the
exclusive lock and check only the listed blocks, instead of the whole block
table. Lists may hold retired PCs; a query prunes PCs no longer in the table
from the granules it visits, and whole-cache clears reset the index. The index
does not reclaim live code. The granule is smaller than a page because
retained neighbours would otherwise make every line publish scan all live
blocks on its page. `BIFROST_NO_CODE_PAGE_FILTER=1` restores the full scan for
A/B measurement.

Every `Emulator::invalidate_jit_range` caller names a `CodeInvalidation`
reason (`Runtime/CodeInvalidation.hpp`). Cache maintenance by VA (`DC`/`IC`,
one 64-byte line per instruction) overlaps exact lines; mapping, protection
and discard reasons cover whole pages, as the kernel applies them. Bifrost
reports `CTR_EL0.DIC=1`, so compiler-rt's `__clear_cache` (used by ART) issues
only `DC CVAU` per line; previously each line retired every block on its
page, including hot translations next to newly published methods.

A Tier-1 translation depends only on the instruction words it fetched.
`translate_block` records them in an append-only `code_words_` array (four
bytes per guest instruction; leaf-inlined blocks and Tier-2 regions record
none). Cache maintenance and `mprotect`/`pkey_mprotect` keep a block whose
pages are still mapped executable, not userfaultfd-missing, and whose words
compare equal; anything else retires it. Unmap, `MAP_FIXED` replacement,
`mremap` and discard always retire. The check reads page flags and the window
directly, because `check_access` would clear an unregistered missing-page
flag. Unit-test calls without a reason keep the retire-everything default.

Single-thread direct `BL` slots record their slow-path offset per target.
Retiring a target points its slots back at the slow path and queues them for
re-patching. Before this, a caller that survived its callee's invalidation
called the stale translation; chain-skip (the default) disables native
`BL_CALL`, so only `BIFROST_NO_CHAIN_SKIP=1` exposed it. `rewind_codegen`
drops pending/direct call records in a discarded tail.

In multithreaded mode, Tier-1 compiles no longer hold `blocks_mutex_`
exclusively. `translate_entry` serializes compilers with `compile_mutex_`,
compiles under the shared lock while other CPUs keep looking up and running
published code, and takes the exclusive lock only to publish. In staging
mode `translate_block` records its publication (`publish_or_stage`,
`publish_translation`) instead of applying it. Codegen only reads `blocks_`;
the MT edge and counter metadata it appends are read by exclusive-lock
holders, which cannot run during the compile. An invalidation that returns
early (no indexed page yet) can run concurrently, so `invalidate_range` and
`invalidate_all` log their range while a shared compile is active; sequentially
consistent fences on both sides guarantee that either the compile observes
the change or the range is logged. A logged overlap (or a full flush) discards
the staged result, revokes only the edges that compile created, and
retranslates under the exclusive lock. ST execution keeps the exclusive
compile: it patches published instructions and may toggle W^X.

`BIFROST_STATS_PERIOD` and `--verbose` print `[JIT] compile:` (Tier-1 blocks,
recompiles of previously retired PCs, total/average/maximum compile time,
shared compiles, retries and contended table/compiler lock wait) and
`[JIT] invalidate (calls/retired/kept):` per reason. Recompiles include the
one-time purge when a second CPU starts.

With `BIFROST_TIER2=1`, `[JIT] block-runs (counted blocks=N) 0:.. 1:.. 2:..
3-4:.. 5-9:.. 10-99:.. 100+:..` buckets the entries of every block that carries a
hot counter (conditional-branch or size-limited heads without a call, SVC
or BR). It shows how many compiled blocks run once or twice, for judging a compile
threshold. A counter stops at `BIFROST_TIER2_HITS` (default 1000), restarts from 0 on a
profile retry, and outlives its invalidated block until the cache is replaced; the line
is absent otherwise, and skipped while a compile holds the compiler lock.

`jit_cache_invalidation.elf churn` warms 32,768 executable functions, times
4,096 data-page protection changes, then rewrites one function 4,096 times with
IC maintenance (a guest JIT publishing onto a page with live code) and checks
each result and the retranslated same-page neighbours. Three same-machine runs
each measured the data-only phase at about 0.007 s (0.58 s without the filter)
and the code-page phase at 1.11 s before the per-page lists and 0.057 s after.
These are invalidation microbenchmarks, not application speedups. A 30-second
`perf` capture of SpongeBob loading under ART had attributed 54% of sampled user
CPU to the old whole-table scan, reached through per-line `IC IVAU`/`DC CVAU`.

With line-exact invalidation, retained neighbours and the 256-byte index,
three alternating runs measured the code-page phase at 0.064-0.068 s against
0.062-0.068 s for the previous binary; a page-granular index had measured
0.079-0.082 s. A cold ART framework startup (`test_android_framework.py --mode
jit`, five interleaved runs per binary) was unchanged at about 3.05 s. Its first
second compiled 39,975 Tier-1 blocks in 484 ms (12 us average, 1.1 ms maximum)
and kept 368 blocks across `mprotect`; the next second ran 4,182 compiles under
the shared lock with no retries and 35 ms of contended waiting. Its 3,063
recompiles came from the purge when the second CPU starts. The probe issued no
cache maintenance, so it does not measure ART JIT publication during gameplay.

## Vector unary FP decode distinction

AdvSIMD vector `FSQRT` (`0x2EA1F800`, excluding Q and the S/D bit) differs
from `FNEG` (`0x2EA0F800`) at bit 16. The interpreter's old
`0xFF20FC00` mask discarded that bit and executed square roots as negations.
The unary handler now retains bit 16 and the fixed encoding bits, handles
vector square root explicitly, and preserves Q=0 upper-half clearing. The JIT
now emits native packed FABS/FNEG/FSQRT through `SIMD_FP_UNARY`. `jit_simd_unary_fp` checks S/D widths,
aliasing, signed zero, FABS/FNEG separation, and all 512 inverse-square-root
seed calculations used by Doom 3's vectorized math initialization.

## Interpreter decode-cache coherence

The interpreter fetches the current instruction before looking up its
per-CPU decode cache. A hit requires both the PC and `DecodedInst::raw`
to match that fetched word. Checking only the PC let another CPU's code
rewrite leave a stale decoded instruction active after IC maintenance.
Comparing the word also handles address reuse without modifying another
CPU's private cache or adding a global cache lock. The synchronized
`jit_cache_invalidation` regression covers this path in both engines.

At the October 3 decode-cache repair checkpoint, the complete interpreter
suite passed 249/249 (three JIT-only skips), and the JIT unit suite passed
75/75 with IR validation. Current inventory/results are in [TESTS.md](TESTS.md).
The cross-thread regression passes five interpreter repeats and QEMU.

## Indexed vector floating-point multiply

Vector FMUL by a scalar lane is native for 2S, 4S and 2D, and FMLA/FMLS by
element use the same broadcast before their FMA (`SIMD_FP_FMA` with a lane);
scalar FMUL/FMULX/FMLA/FMLS by element are the scalar ops with a source element
([details](#scalar-fp-by-element-2026-11-03)). The generated
classifier retains the five-bit Rm field, rejects reserved 1D encodings, and
distinguishes single-precision L/H lane bits from the double-precision H bit.
`SimdFpArithParams::lane` is -1 for a vector operand and otherwise selects
the scalar lane. Both sources are loaded before destination writes, including
high-half scalar lanes when the destination aliases Rm. Q=0 clears v_hi.
Indexed FMUL and vector FABS/FNEG/FSQRT participate in the vector cache.
Indexed sources are broadcast into scratch XMM0 before writing the destination,
preserving source/destination aliases and high lanes. Unary operations use
register operands; sign masks are constructed across all lanes without
clobbering GPRs. Unsupported block operations and hosts without FMA3 retain the
existing memory path. Cached Q=0 logical/arithmetic/FMA/unary results use one
VEX VMOVQ to clear bits 127:64.

The old cached FABD path loaded its absolute-value mask with MOVQ, which zeros
the upper half of the XMM register. Its subsequent full-vector AND erased
upper-lane results. The shared packed-mask emitter fixes both .4s and .2d.
The isolated regression failed six cases before the repair, while QEMU and
the uncached path passed. `jit_simd_unary_fp` now checks 541 assertions,
including destination aliases and XMM8-15 encodings; the indexed fixture checks
all single/double lanes, aliases and Q=0 clearing. Both accept `--mt` to exercise
the sticky multithreaded JIT mode. These tests do not establish complete Arm
FPCR/FPSR or enabled floating-point trap equivalence for native FP. See the
vector FP special-case contract above for NaN handling.

Encoding and zeroing rules were checked against Intel SDM Volume 2B, MOVQ,
PSHUFD, UNPCKLPD/UNPCKHPD and SQRTPS/SQRTPD in the
[instruction reference](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf).
The full-lane absolute-difference contract follows
[Arm FABD pseudocode](https://www.scs.stanford.edu/~zyedidia/arm64/fabd_advsimd.html).

Three interleaved runs of `bench_doom_vector_fp` measured median loop time
0.0692 s before versus 0.00935 s after in single-thread mode (7.4x throughput).
With `--mt`, the medians were 0.0703 s and 0.0499 s (1.41x throughput), since
MT mode still reloads register state between loop iterations. This existing
probe reproduces Doom's instruction mix; these measurements are not game FPS.

`jit_simd_fp_indexed` covers all scalar lanes, high Rm registers, source and
destination aliasing, and Q=0 clearing against fixed expected results.

## Ties-away rounding and scalar lane copies

FRINTA and signed FCVTAS now use native SSE4.1 code. The shared helper
truncates the input, compares the absolute fractional part with 0.5 and
adjusts by signed 1.0 when necessary. It does not add 0.5 to the original
input: that mis-rounds adjacent values and large already-integral inputs.
Signed zero, infinities and NaN payloads retain the rounded result. FCVTAS
then uses the existing NaN-to-zero and signed saturation checks. Unsigned
non-truncating conversions retain their interpreter fallback. The helper
uses XMM0-2 and flushed RAX/RCX, preserving pinned XMM3-15. The splitter,
translator and IR validator agree on the newly native operations.

`SIMD_SCALAR_DUP` has typed element-size/lane parameters. It loads the
selected bytes before writing the destination, zero-extends into v_lo and
clears v_hi, including when source and destination alias. Its memory path
disables register pinning for the block.

`jit_round_away` checks 6,984 fixed-oracle cases, including adjacent ties,
large integers, saturation, signed zero, NaNs, infinities and cache-block
boundaries. `jit_scalar_dup` now checks all 30 valid lanes with and without
source/destination aliasing (60 checks). Both pass QEMU and JIT verification.

## Native helper and SDL thread boundaries

Complex SIMD helpers in `NativeSIMD.cpp` specialize a single operation at
translation time; they must not decode or step the interpreter at runtime.
They are typed, validated IR operations and optimizer barriers. Flush GPR
state before helpers that read `cpu.regs`, disable incompatible pinning, and
preserve `WIN_REG` across every SysV call: it is caller-saved R10, so an
otherwise correct C++ helper can corrupt all later direct-window accesses.
Tier-2 treats SIMD helper calls as calls for GPR pinning and LICM purposes.
The translator checks `classify_native_simd` before the interpreter
fallback, so a classified encoding must be one its `run<K>` implements.
The classifier, the kind enum and the dispatch list are generated from
`Tools/CodeGen/native_simd.txt` (`make nativegen`). A kind without a
`run<K>` body fails to compile. The same spec generates
`Tests/Unit/jit_native_simd_gen.c`, which runs every enumerated encoding
the classifier accepts against hashes recorded under QEMU
(`make nativegen-expect`). An encoding QEMU rejects with SIGILL fails the
test, so the classifier cannot accept unallocated forms unnoticed.

`FADDP` (vector), `FRECPE`/`FRSQRTE`, `FRECPS`/`FRSQRTS` and the vector `FCVTN/P/M/A` forms are helpers too. In Arknights they
were about 58% of the instructions the interpreter ran from translated code: Unity's vector normalise is a run of them. The
reciprocal and conversion helpers call the interpreter's own `exec_reciprocal` and `exec_vector_fcvt_round`, so both engines share
one definition and its FPSR behaviour; each still pays that code's `fenv` calls. A measured gain is not recorded: the
comparison runs were cut short.

Its first run found 605 classified encodings that QEMU rejects:
- shift-by-immediate rows missing bit 23;
- `.1d` forms of FCMEQ/FCMGE/SSHL/CMEQ #0/SCVTF/FCVTZS;
- double FMOV (immediate) without Q;
- ADDV `.2s`.

All 1,480 remaining encodings match QEMU in both engines. A
generated-versus-hand-written check over all 2^32 words showed no
classification change before the spec was corrected.

## Multithreaded Tier-2 and chaining (2026-10-04)

### Bounded MT self-loops (2026-10-11)

Tier-1 conditional branches to their own block now retain the current frame,
eligible GPR pins and cached vectors in MT mode. The back-edge uses an immutable
guard and an atomic cell pointing to this translation's body, bypassing prologue
reloads. It checks the callback stop PC and the preemption epoch (it used a
256-edge lease until 2026-10-23); expiry or revocation commits carried state and returns to dispatch.
Range/full invalidation clears the cell. Target refresh and Tier-2 publication
never redirect a body cell into a different translation's frame or register
layout. Cells and retired code retain the existing lifetime policy.

BRCOND commits NZCV before the MT guard because its checks overwrite host
flags and any iteration may become an exit, unless the loop head overwrites
all four flags first (see "Dead NZCV stores at block exits"). The ST-only
loop-carry shortcut remains ST-only. CBZ/CBNZ and TBZ/TBNZ preserve their pre-branch NZCV contract.
`BIFROST_NO_SELFLOOP=1` restores ordinary loop edges; `BIFROST_NO_PIN=1`
disables GPR pinning, and `BIFROST_NO_CHAIN=1` also disables MT body edges.
Both frame layouts support this path. Tier-2 multi-block regions retain their
whole-iteration entry policy described below.

In one matched synthetic batch, 200 million counter-loop iterations used
239.2 → 195.2 ms of thread CPU time (18% less), and a two-lane vector
accumulation loop used 1535.5 → 241.0 ms (6.4x throughput). The flag-heavy
integer loop was unchanged at approximately 286 ms. These are local loop
measurements, not game FPS or general MT scaling guarantees. See
[validation and measurement bounds](archive/TESTS-history.md#bounded-mt-self-loops-2026-10-11).

### Self-loop back-edge writeback (2026-10-12)

The back-edge jumps to the body start, whose allocator state holds only the
loop-carried pins. Any other dirty architectural register cached in a host
register was previously written back only on the exits emitted after the
self-loop slot, so the next iteration's `LOAD_REG` read the previous value.
`emit_selfloop_backedge` now stores every dirty, unpinned architectural vreg
(`store_dirty_unpinned_arch_regs`) before the slot in both ST and MT forms;
pins stay in their registers and the exits keep their own writeback.

Android 15 Bionic's `__strrchr_aarch64` exposed it: its scan loop writes x11
with `UMOV` (a direct architectural write, never pinned) and reads it in the
next iteration's `csel x7, x11, x7, ne`. Whether x11 stayed dirty in a host
register depended on allocation, so the result depended on which path compiled
the loop first. After unaligned scans, an aligned 104-byte path returned
index 73 instead of 94. Unity's il2cpp first-launch extraction built
destinations such as `il2cpp/ets/bin/.../ystem.Drawing...`, failed, and showed
"Not enough storage space to install required resources." in SpongeBob 5.12.0.

### MT preemption epoch, forward edges and cell calls (2026-10-23)

MT code paid for bounded execution with a shared read-modify-write: every chain edge, IBL probe and
self-loop back-edge ran `sub dword [cpu+jit_chain_budget],1`, a loop-carried store-forwarding dependency, then
loaded and tested its target cell. Measured on a clean `e7ed1b3c` build, a second (idle) vCPU made a 60M-iteration
LCG loop 21% slower, recursive `fib(18)` 22%, a byte copy 53% and `malloc`/`free` 58%, with the same code
generator in both modes, so the tax was MT's own.

- **Epoch poll.** `g_jit_preempt_epoch` (`Runtime/Preemption.hpp`) is advanced by a detached ticker thread every
  1 ms once `enter_multithreaded()` runs, and by every `invalidate_dispatch_cache()`. Each dispatcher entry
  (`run_block`, `jit_call_helper`, Tier-2 trials) calls `jit_grant_lease(cpu)`, which records the epoch in
  `CPU::jit_epoch_seen` before native code starts. MT back-edges and IBL probes only load both values
  (`emit_preempt_poll`: 2 loads and a `jne`, no store); a mismatch takes the existing cold exit to the dispatcher.
  A bumped epoch also revokes in-flight loops of retired code, as the cell clear did. The thread loops, the SDL
  thread loop and the main loop drain host and cross-thread signals on every tick, since native loops now return
  once per tick rather than every few thousand edges. The edge budget stays for ST code and for timed Tier-2
  sampling code, whose trial windows count dispatcher resumptions (`preempt_by_epoch()`).
- **Forward edges.** A static edge to a higher block start cannot close a cycle, so it emits only the cell jump
  (`mov r11, cell; jmp [r11]`). Only edges with `target_pc <= current_start_pc_` carry the sentinel compare and the
  poll. An empty cell now holds its own cold-exit address (`MtCell::cold`), so no emptiness test is needed.
- **Self-loops.** The MT back-edge is an immutable direct `jmp` to the body behind the poll and the sentinel
  compare (`cmp qword [cpu+stop], imm32` for pcs below 2 GiB) instead of a cell load and `jmp rax`. When the block
  does not read flags across iterations, the NZCV store is skipped as in ST: MT's epoch and sentinel exits resume at the
  same head, which rewrites the flags first. This also leaves a loop head that faults with stale NZCV, exactly as the ST
  loop already does.
- **BL.** MT `BL` is `call [cell]` with the same setup as the ST direct call. The cell holds the callee's entry, or
  this call site's `jit_call_helper` slow path while the callee is untranslated or retired, so invalidation cannot
  leave a stale callee address. The callee-completion guard after the call is shared with ST.

Edge cases kept: the epoch is read before native code starts, so a tick during entry is seen at the next poll; a
fork child runs interpreter-only until it execs and drops the inherited JIT without destroying it (its block-table and
compile locks may be held by threads that no longer exist; the new image gets a fresh JIT, `Emulator::restart_jit_for_exec`,
because `jit_wanted_` remembers that one was enabled),
and the ticker is forgotten in a fork child (`pthread_atfork`) so an exec'd multi-threaded program starts its own;
`BIFROST_NO_CHAIN` and `BIFROST_NO_DIRECT_CALL` keep their meanings. Not changed: code blocks that never reach a backward edge, IBL probe or cell call (long straight-line
chains) are bounded by their length, as before for ST.

Measured on the 5800X3D, median of five, ns per iteration, ST vs MT with an idle second vCPU (MT tax):
`loop` 1.04 vs 1.14 (+9.6%, was +21%), `branchy` 18.50 vs 18.17 (-1.8%, was +9%), `chase` +6.6% (was +13%),
`calls` 32.6 µs vs 34.2 µs (+5.1%, was +22%), `copy` 0.97 vs 1.11 (+14%, was +53%), `tiny` 0.69 vs 0.93 (+35%,
was +46%), `mutex` +5.8% (was +17%), `atomic` -5.6% (was +23%), `malloc` +22% (was +58%); ST is unchanged. The
remaining tax on `tiny` is a two-instruction loop body next to the poll, sentinel compare and jump: it is front-end
bound and was not reduced further by dropping an instruction.

### Region publication and execution

Tier-1 and block chaining are the default. Tier-2 promotion is experimental
and disabled by default in both single-threaded and MT execution. Opt in with
`BIFROST_TIER2=1`; `BIFROST_TIER2=0` explicitly disables it. The current MT
whole-iteration regions can regress application performance. Tight ST self-loop iteration hotness remains future work. Execution payoff
is measured by bounded live-work A/B trials; admission and feedback follow.

Before the second guest CPU starts, the JIT retires legacy single-thread
instruction patches and purges the table. New MT blocks use immutable
load/test/jump instructions and naturally aligned, lock-free atomic target
cells. Release publication changes a cell, not an instruction displacement;
x86-64 loads provide acquire ordering. Fall-through, taken and region-exit
edges share the mechanism. Direct BL targets use the same cells since
2026-10-23 (see MT preemption epoch below); before that they kept helper dispatch in MT
mode so invalidation cannot leave an embedded stale callee address.

Eligible MT heads count entries using an atomic XADD in separate writable
metadata. Exactly one entry reaches the promotion threshold; disabling the
counter updates an atomic flag instead of patching its prologue. The fire
notification includes the original code site, so a retired block cannot
promote a replacement translation. Collection, compilation and publication
hold the exclusive compiler lock with RAII release on failure.

MT regions fuse complete loop iterations. Their back-edges enter through
revocable data cells and rerun the region prologue; loop-carried pinning and
LICM across these iteration boundaries remain disabled. This differs from
the single-thread region's internal loop: it gives concurrent invalidation a
boundary without patching or reclaiming live code. Backward edges poll the
preemption epoch to return chained execution to dispatch (timed sampling code keeps
a per-CPU 256-edge budget), and callback stop sentinels also end chains. Both ordinary and chain-skip frame layouts are covered.

Publication advances every CPU's dispatch generation and redirects incoming
cells to the new head. Region metadata retains all constituent guest ranges,
including non-head pages. Invalidation clears incoming cells, permanently
retires outgoing cells and disables old hot counters. Executable code and its
metadata remain allocated for the JIT lifetime; reclamation, persistent
translations and unsynchronized guest code writes remain outside this change.
A pointer already acquired by another CPU may finish its current iteration;
guests must still synchronize code changes and instruction-cache maintenance.

`Tests/Unit/jit_mt_tier2.c` checks eight concurrent CPUs against a closed-form
arithmetic oracle, invalidates unchanged live code, rewrites only a region's
non-head page between synchronized runs, replaces that page with MAP_FIXED,
and churns data mappings. `Scripts/Testing/test_jit_mt.sh` additionally requires MT
edge/region publication, verifies both opt-outs and checks chain-skip mode.
`Scripts/Testing/test_jit_mt_publication.sh` snapshots published MT code bytes before
eight host CPUs trigger promotion, then checks that promotion, range/full
invalidation and retranslation preserve the snapshot. It also checks epoch exits. The verification gate runs the guest concurrency fixture as an independent oracle,
not shared-memory differential replay. These checks do not establish a Doom 3
FPS improvement; measure representative gameplay separately.

## Tier-2 loop discovery and publication

The collector follows direct `B` targets as well as conditional fall-through
edges. Internal direct edges become inline boundaries, retaining register and
flag state without a host jump. A body exceeding the 32-instruction block cap
gets an explicit IR continuation with no additional guest instruction count.
The eight-block/256-instruction bounds apply to loops and linear superblocks.
Non-head cycles are excluded, and unsupported terminators remain outside the
collected prefix. Collection stops at another published region head to avoid
compiling rotated copies of an already optimized loop.

Eligible native conditional, direct-branch and capped straight-line blocks
use generated entry counters for promotion, including chained entries. The
counter makes at most three attempts per translation while samples warm up. All automatic publication now goes through
`tier2_fire_region`, retaining each constituent code range and redirecting
incoming chains. The former dispatch/call-lookup promotion paths did not
retain those ranges and have been removed. Self-loop iterations that bypass
the generated entry still do not contribute hotness. Conditional exits count
taken/fall-through outcomes with one `inc qword [profile+off]` (64-bit, no check)
behind a `mov r11, imm64`. It is not `lock`ed in MT: the counts only feed the bias heuristic, so an update lost to a race does no harm.

Instrumentation lasts until a block is decided. Once promotion declines a head, or a trial ends either way, its blocks **settle**:
the dispatcher retires them and retranslates them without the hot counter or branch counters, then relinks their predecessors, which
retirement unlinks and forgets. The swap waits for the dispatcher because the decision is made inside the running block. Profiles
stay, for regions that pass through a settled block, and a code change forgets the decision. Before this, instrumentation cost CoreMark
22-29% of its cycles with no Tier-2 code running; the penalty is now about 3.4%, and Tier-2 still keeps no region there. Most of what is
left is 16.5 million dispatches against 0.9 million with Tier-2 off; which edge stays unlinked is not known.
A head that fires during the swap counts another window instead: the blocks it needs are briefly gone, and it would be rejected for good.

Regions defer guarded-access cold paths and use the shared stubs, as Tier-1 does: a six-instruction region went from 745 to 477 bytes.

Entry counts are `HotCounter`s outside the code buffer, in both modes. They used to sit in the 8 bytes before each
single-thread block, and an `inc` on the line being executed is self-modifying code to the CPU: `bench_memcpy` with counters
that never fire took 17.3 billion cycles and 272 M pipeline resyncs (`bp_redirects.resync`), against 0.76 billion and 18 thousand
now. At the default threshold the move changed little, because a counter stops at 1000 hits.
With at least 16 samples, a taken successor with 75% bias is linearized by
inverting the typed branch and retaining the original fall-through as a cold
exit. Head back-edges remain loop terminators. Promotion requires all branches
to be sampled and a product of continuation frequencies of at least 75%.
This is a profitability heuristic, not a measured speedup; multiplying
marginal frequencies can underestimate correlated paths. MT single-block loops are declined
because they fuse no boundaries and cannot retain loop state across entries.
A profile whose block was promoted or invalidated is dropped from the lookup
table and its counters go unread; storage outlives published code. A monitored region counts its completed
iterations in a CPU-local slot (`credit`: one `add dword [rbx+disp32],1` on each back-edge and on exits that complete
the path; the host flags are dead at all of them, and MT performs no shared atomic increment). A side exit calls a
stub shared by the region after flushing guest state; it counts the exit (`pressure`), and every
`kTier2FeedbackWindowExits` (32) exits it closes a window: a window that completed fewer iterations than the monitor's
`min_window_iterations` is a strike, a good one clears the strikes, and `kTier2FeedbackStrikes` (4) strikes in a row
retire the region. The minimum comes from the trial: its sampling copy counts the same iterations and exits, and a kept
region may exit up to `kTier2ExitRateSlack` (4) times as often as its candidate did, and at least once in
`kTier2ExitRateFloor` (16) iterations. A trial that saw no exits gets that floor (512 iterations per window). Then the
exit chains to its target like any other. Only a slot owned by another region, or the retiring strike, tears down the
frame and tail-calls `tier2_region_feedback` in C++. The stub and that function implement one rule and change
together: going through C++ and the dispatcher on every side exit cost a short loop more than the region saved.
A retirement is a request for a new trial at the current exit rate, not a measured loss: the old rule (64 exits with no
completed iteration in between) never retired a loop that exits on a steady fraction of its iterations. Sixteen
CPU-local slots bound metadata; colliding regions can delay detection by adding iterations or changing the slot's
owner, but cannot add exits to another region.

Retiring a region also forgets its blocks' predecessors, which nothing
retranslates. `retire_tier2_region_locked` records both, and the next dispatch
boundary rebuilds the blocks with fresh counters, relinks the predecessors and
returns them to `back_refs_` (`tier2_settle_queued`). Without the relink,
CoreMark's dispatches rose 69 times; without the index entry, a later retirement
could not unpatch the slot.

`BIFROST_TIER2_GRAPH` reports, for each hot head, the natural loop around it:
blocks that can return to the head along profiled edges, and the share of entries
that do. It emits nothing, and is the survey for regions with internal branches.

Retirement holds one compiler-lock critical section for version validation,
exact victim selection and revocation. Victims come from `code_index_` and profiles from an ordered map, so a
retirement costs the size of its ranges. Each used to walk every block and every profile: with Arknights' thousands of rejected
trials that was 43% of its user-mode CPU. It retires the head and overlapping
Tier-1 constituents and resets their profiles, preserving unrelated regions
on the same page. Mapping changes still use page-granular invalidation. At
most two phase retirements or terminal trial rejections per head are allowed
in a full code-cache epoch; exhausted heads stay in Tier-1. Full invalidation
resets that budget.
Published MT instructions remain immutable throughout retraining. Both ST and
MT region exits use tracked, revocable target cells, so a rewritten target on
the next page cannot leave the region executing stale guest instructions. ST
exit cells share a frame-teardown tail with stop-sentinel and execution-lease
checks. Promotion redirects their targets; precise/range/full invalidation and
the MT transition revoke them. Code and cells remain allocated for in-flight
CPUs. Empty cells return to dispatch until a replacement target is compiled.

Admission requires at least two cached native Tier-1 blocks with exactly the
trace's block boundaries, without calls or retained self-loop frames; the walker
ends each trace block where the published Tier-1 block at its start ends. Branch
profiles choose candidate paths; code size no longer predicts execution payoff.
A head that reaches its entry count is timed over a second window, and goes on
only if it runs at least 10 million guest instructions per second through its
block, about 1% of one host core. A head that misses three windows settles. Lifetime
entries alone let thousands of a game's blocks through: in Arknights 7,664 heads
filled the 32 MiB budget with 93% of trials rejected; with the gate it used 3.7 MB.
A rate shows activity, not whether a region will win, and a head that settles
stays out if it turns hot later. A fixed `BIFROST_TIER2_HITS` skips the gate, which
is why `test_jit_tier2_rate.sh` runs without it.

Each candidate enters a bounded A/B trial before permanent selection. The
retained Tier-1 graph and candidate run alternate windows on one vCPU's live
workload, without replaying memory writes or restoring architectural state.

Both arms execute private instrumented copies with identical work probes at
matching guest block boundaries. Normal Tier-1 blocks and selected regions
contain no work probes or inactive probe guards. The selected ST region also
omits sampling quota checks on its internal backedges. CPU-local counters
record guest instruction count; probes preserve scratch registers and leave
the host flags alone without saving them (`lea`, `jrcxz`). A `popfq` per block
costs more than the loops being timed: with one in every probe, a region that
was 1.58x faster in wall-clock time measured 1.2x slower.

Private Tier-1 chains bind only to other blocks in their sampling graph and
the trial's head gate. Private region exits use the same restricted bindings.
Cold targets remain unbound and return before executing unrelated guest work,
including targets inside gaps in the trace's address envelope. Private copies
never replace public cache entries or join the public ST back-reference graph.
MT sample edges are private data cells, excluded from public target refresh,
and revoked on selection or invalidation. ST sample chain slots are separately
retained and revoked, including before the second CPU starts. Published MT
instructions remain immutable. Trials suppress constituent hot-counter
compilation and phase feedback inside the timed interval. A data-cell lease
expiration at the head also counts as a completed iteration, so dispatcher
resumptions cannot inflate the work.

A window runs up to 256 iterations, or one traversal of a linear path, and ends
early wherever the workload leaves. Real loops leave after a few dozen
iterations, so no window has to complete: each one adds its time and guest
instructions to its arm's CPU-local pool. A window that spends its whole quota
in under 10 us raises the quota, up to 4096, to amortize timing overhead. The
clock is CLOCK_MONOTONIC, read through the vDSO in tens of nanoseconds; the
thread CPU clock used before is a system call costing about as much as a short
loop visit, which both arms paid per visit and which pulled every ratio towards
1. A window that loses the CPU becomes an outlier pair. A baseline pool closes
at 100 us (`kTier2TrialWindowNs`) and 128 instructions; a candidate pool at
100 us and at least the baseline's instructions: 10 us pools were dominated by
the switch between the arms' code. A pair compares time per guest instruction.
Twelve pairs (`kTier2TrialPairs`) must show a median candidate/baseline ratio
below 0.97, with at least nine pairs below 0.97, to select the region. The arms
run different live work, so a stretch of unusually cheap iterations can skew one
pair: the median and the nine-pair rule limit that, they do not rule it out. Windows also
leave out what happens after an exit, so a candidate that leaves badly can pass
here and still lose over the whole program; every region exit has a revocable cell, so a published exit chains to its
target once that is translated instead of returning to the dispatcher. Slower or noisy
candidates restore Tier-1; a measured loss can schedule a smaller prefix after
fresh profiling, following the bounded size search described above. A trial
with insufficient evidence after 128 pools or 131072 visits also restores Tier-1. Version
checks and normal revocation protect
in-flight references across mapping changes and ST-to-MT transition.

The decision measures instrumented execution gain, not lifetime amortization
of compilation or a guarantee of whole-application speedup. Sampling adds
short-lived execution cost and retained private code; validate application
benchmarks as well as local trial decisions. Codegen/retirement oracles select candidates
deterministically through a test friend; the separate timing oracle uses real
controlled CPU work to verify both acceptance and rejection, without fake
clock values. CoreMark comparisons use a fixed 200,000 iterations
(`Scripts/Bench/run_benchmarks.py`, crcfinal 0x4983, checked against QEMU 11.1.1) and
labelled CRC/validation checks; the earlier 160,000 (9.7 s on 2026-11-10), 110,000 and 60,000
iterations now finish below CoreMark's ten-second minimum. Auto-calibrated iteration counts cannot be compared by
wall time. Counters expose measured pairs, keeps and rejections.

Automatic compilation stops after 32 MiB of emitted region, private sampling
and trial-wrapper code or two seconds of measured compilation time per JIT lifetime. Rejected
unpublished candidates revoke their data cells and preserve baseline metadata.
These limits bound speculation. Actual timing selects winners, and the
existing phase-pressure monitor can retire a winner when its path changes.
Tier-2 stays
experimental and opt-in. Differential replay disables promotion explicitly:
its temporary Tier-1 exit patches and guest-memory restoration cannot safely
cover internal region loops. `BIFROST_JIT_VERIFY` therefore checks Tier-1 while
the region tests check independent result oracles. Replay counts have their
own per-block budget, separate from promotion hotness.

Single-thread chain-skip invalidation restores the entire five-byte chain slot
to NOPs; restoring only its opcode would execute the old displacement bytes.
MT invalidation continues to revoke atomic target cells.

`bash Scripts/Testing/test_jit_tier2_discovery.sh` exercises a non-contiguous loop
containing a 40-instruction body, early exits with loop-carried state,
non-head page rewriting, full invalidation, non-head cycles and duplicate-region
avoidance in ST/MT and ordinary/chain-skip modes. The host fixture has an optional
`benchmark` argument (and `mt` for MT execution); it times two million iterations
against a closed-form result oracle. This is a synthetic workload, not a
representative application performance measurement.

Region compilation clears the complete vreg mapping table. Clearing only the
new region's maximum left homes from a larger preceding Tier-1 block, then
published a smaller maximum that hid those stale entries from the next reset.
This corrupted musl pthread startup's loaded function pointer under frequent
promotion. `Scripts/Testing/test_jit_tier2_mutex.sh` repeats the mutex result oracle
with a threshold of 32. Nested call interpreter fallback also uses the existing
fault trampoline so guest memory exceptions cannot escape generated frames.

ST publication redirects only incoming edges whose recorded Tier-1 target is
the region head. A collected block may span multiple Tier-1 blocks; guessing
a back-edge slot from its terminator could bypass a partial block and skip
guest instructions. The rounding oracle covers this with Tier-2 enabled at
a low threshold. The discovery harness additionally checks biased branches
from all three conditional IR families, cold exits after promotion, stale
profile invalidation, weak-path rejection, sustained phase changes in all three
conditional families, and nested-call fault delivery.

`bash Scripts/Testing/test_jit_tier2_measure.sh` verifies measured faster/slower decisions,
a verdict from visits that never reach the quota, and that an outside predecessor
is chained and tracked again after retirement, in ST/MT with ordinary/skip chaining.
It also checks that MT publication changes no existing instruction bytes.

`bash Scripts/Testing/test_jit_tier2_feedback.sh` checks two regions sharing one page:
retiring one preserves the other's exact cached function pointer. Isolated
cold exits followed by recoveries do not cause retirement. Eight host CPUs
execute a sustained phase change while the neighboring region remains hot,
checking fixed arithmetic results, retraining and immutable published MT code
in ordinary and chain-skip modes. The same harness places a region exit target
on the next page, rewrites that target, and verifies the new result while the
region itself stays cached.

## CoreMark checkpoint (2026-10-27)

Superseded by the [2026-11-08 re-measurement](#coremark-re-measurement-2026-11-08); kept as the record of the alpha.3 numbers.

`Tests/Unit/coremark.elf` (GCC 15.2.0, `-O2 -static -O3`, an ignored guest fixture) run as
`coremark.elf 0 0 0 160000` on a Ryzen 7 5800X3D desktop. Each binary had one warmup and three measured
runs, interleaved. Every run printed the expected CRCs (`crclist` 0xe714, `crcmatrix` 0x1fd7, `crcstate`
0x8e3a, `crcfinal` 0xa14c) and no "Must execute for at least 10 secs" error.

The runner's old fixed count of 110,000 iterations took 8.4 s on the current build, which CoreMark reports as
an invalid run, so the count is now 160,000 (about 12 s on bifrost-emu, 20 s on QEMU). `crcfinal` depends on
the count; 0xa14c at 160,000 was taken from QEMU 11.1.1 and bifrost-emu matches it.

| Build | Runs (iterations/s) | Median |
|---|---|---|
| bifrost-emu 2.0.0-alpha.3, shipped SDL/GL build | 13,146 / 13,156 / 13,209 | 13,156 |
| same source (HEAD 02475878), headless build | 12,981 / 13,180 / 12,767 | 12,981 |
| 2.0.0-alpha.2 release commit (556557b0), headless build | 12,364 / 12,332 / 12,187 | 12,332 |
| QEMU 11.1.1 user mode | 7,182 / 7,817 / 7,710 | 7,710 |

The two headless builds came from `git archive` of the commit, built with `USE_SDL2=0 USE_SDL3=0
USE_THUNK_GL=0 USE_MEDIA=0` in a temporary directory, so the alpha.2 to alpha.3 comparison (+5.3%, 12,332 to
12,981) changes only the source. The shipped build differs from the headless one in host-library support,
not in the JIT. Limits: one host, one benchmark, a desktop that was not isolated (the first runs followed a
compile, and QEMU's own runs spread 9%), so a few percent is noise. The earlier October 14 figure (9,893.87
at 110,000 iterations) was not remeasured on its binary, and this comparison does not attribute the change
since then to particular commits. CoreMark does not predict game frame rates.

### CoreMark native reference (2026-11-07)

The native reference the 26%-of-native milestone was waiting for, on the same Ryzen 7 5800X3D, with the CPU governor on
`performance` (all 16 threads, boost on; about 4.44 GHz). CoreMark v1.01 (EEMBC's `v1.01` tag) built with `make PORT_DIR=linux64
XCFLAGS="-DPERFORMANCE_RUN=1 -DITERATIONS=160000"` and gcc 16.2.1 at its default `-O2` (no `-march=native`), run as
`coremark.exe 0x0 0x0 0x66 160000 7 1 2000` pinned to one core: 37,951 / 37,951 / 37,933 iterations/s, `crcfinal` 0xa14c. The
JIT (HEAD 36801777, default build, unpinned, `run_benchmarks.py --bench coremark --warmups 1 --runs 3`): median 10.83 s, 14,785
iterations/s, every CRC valid. That is about 39% of native. A first native run on a power-saving governor gave 28,700, 32% low,
so a CoreMark comparison is only meaningful with the governor fixed. The native build is gcc `-O2`; the AArch64 guest was
built with `-O3`, and a tuned native build would score higher. The Ryzen 5 9600X (5.68 GHz overclock) that measured about
24,000 iterations/s on November 4 is at roughly 40% of its own native score. Pinned and `BIFROST_TIER2=1` runs were not made here.

### CoreMark re-measurement (2026-11-08)

The October 27 method on the same Ryzen 7 5800X3D desktop, now with the CPU governor on `performance` (boost on, checked in
`/sys`). `Tests/Unit/coremark.elf` as `coremark.elf 0 0 0 160000`, one warmup and three measured runs interleaved across the four
binaries, every run with the expected CRCs (`crclist` 0xe714, `crcmatrix` 0x1fd7, `crcstate` 0x8e3a, `crcfinal` 0xa14c). The
runs started after the 1-minute load average had dropped below 2.5, so the compile that built the binaries was not measured. Each
bifrost-emu binary was built from `git archive` in a temporary directory (the shipped one with `USE_SDL2=1 USE_THUNK_GL=1`, the
headless ones with `USE_SDL2=0 USE_SDL3=0 USE_THUNK_GL=0 USE_MEDIA=0`), so no shared build was replaced.

| Build | Runs (iterations/s) | Median |
|---|---|---|
| tree at `ecbd7dd1`, shipped SDL/GL build | 14,507 / 14,874 / 14,884 | 14,874 |
| same source, headless build | 14,663 / 14,753 / 14,857 | 14,753 |
| 2.0.0-alpha.2 release commit (`556557b0`), headless build | 12,534 / 12,711 / 12,718 | 12,711 |
| QEMU 11.1.1 user mode | 7,992 / 7,981 / 8,065 | 7,992 |

The shipped build is 1.86 times QEMU; the headless build is 1.16 times alpha.2 (alpha.3 measured 1.05 times in the October
session). The `bench_*` table in the README comes from `Scripts/Bench/run_benchmarks.py --mode both --warmups 1 --runs 3` on the
shipped build for `bench_mips` (process wall time: JIT 0.177 s, interpreter 15.79 s, so 800M instructions, 8 per loop
iteration by disassembly, are about 4,515 and 51 MIPS), and three direct runs per mode of each other program for its own timer
(the runner keeps only the last run's output).

Limits: one host and one benchmark. The desktop was in use: a QEMU system VM (about 14% of one core) and an idle Asgard realm
(under 1%) ran throughout, so a few percent is noise even though these runs spread only 1% to 2.5%. The alpha.2 binary scored
3% higher than on October 27 (12,711 against 12,332), so numbers are comparable within a table, not across dates. At 160,000
iterations the shipped build runs 10.8 s, 8% over CoreMark's ten-second minimum; a build above about 16,000 iterations/s needs a
larger count, and the CRC for a new count has to be taken from QEMU again. Not pinned to a core, and no `BIFROST_TIER2=1` run.
The November 7 native reference (37,951 iterations/s) puts this at about 39%. CoreMark does not predict game frame rates.

## Recorded changes

Optimization checkpoints and fixes are in [the history](archive/JIT-history.md), unchanged:

- [Chain-skip exits no longer load RDI/RSI (2026-10-26)](archive/JIT-history.md#chain-skip-exits-no-longer-load-rdirsi-2026-10-26)
- [Smaller fault exits and one frame return per block (2026-10-26)](archive/JIT-history.md#smaller-fault-exits-and-one-frame-return-per-block-2026-10-26)
- [One compact checked path per guarded access (2026-10-26)](archive/JIT-history.md#one-compact-checked-path-per-guarded-access-2026-10-26)
- [The bounds compare gates the software checks (2026-10-26)](archive/JIT-history.md#the-bounds-compare-gates-the-software-checks-2026-10-26)
- [Direct-call padding, probe order and the emulator pointer (2026-10-25)](archive/JIT-history.md#direct-call-padding-probe-order-and-the-emulator-pointer-2026-10-25)
- [Per-block bookkeeping and per-thread dispatch caches (2026-10-24)](archive/JIT-history.md#per-block-bookkeeping-and-per-thread-dispatch-caches-2026-10-24)
- [Unsigned halfword min/max are native (2026-10-23)](archive/JIT-history.md#unsigned-halfword-minmax-are-native-2026-10-23)
- [Pixel and media integer forms are native (2026-10-23)](archive/JIT-history.md#pixel-and-media-integer-forms-are-native-2026-10-23)
- [Single-thread self-loop reconcile is out of line (2026-10-23)](archive/JIT-history.md#single-thread-self-loop-reconcile-is-out-of-line-2026-10-23)
- [Bitfield moves use immediate shifts and the allocator's register (2026-10-21)](archive/JIT-history.md#bitfield-moves-use-immediate-shifts-and-the-allocators-register-2026-10-21)
- [Branch flags, immediates and Q pairs (2026-10-20)](archive/JIT-history.md#branch-flags-immediates-and-q-pairs-2026-10-20)
- [Native exclusive loads and stores (2026-10-20)](archive/JIT-history.md#native-exclusive-loads-and-stores-2026-10-20)
- [Scalar FP loads and stores keep the XMM cache (2026-10-20)](archive/JIT-history.md#scalar-fp-loads-and-stores-keep-the-xmm-cache-2026-10-20)
- [Dead NZCV stores at block exits (2026-10-20)](archive/JIT-history.md#dead-nzcv-stores-at-block-exits-2026-10-20)
- [Flag setters no longer save dead flags (2026-10-20)](archive/JIT-history.md#flag-setters-no-longer-save-dead-flags-2026-10-20)
- [Load/store pairs use the native guarded path (2026-10-20)](archive/JIT-history.md#loadstore-pairs-use-the-native-guarded-path-2026-10-20)
- [Inline widening SIMD (2026-10-20)](archive/JIT-history.md#inline-widening-simd-2026-10-20)
- [Inline Select, Dup, ADDV, MLA and FRINT (2026-10-16)](archive/JIT-history.md#inline-select-dup-addv-mla-and-frint-2026-10-16)
- [Vector FMAX/FMIN signed zeros inline (2026-10-16)](archive/JIT-history.md#vector-fmaxfmin-signed-zeros-inline-2026-10-16)
- [Cold-path reloads keep vector validity (2026-10-16)](archive/JIT-history.md#cold-path-reloads-keep-vector-validity-2026-10-16)
- [Allocation costs, mixed FP and region layouts (2026-10-16)](archive/JIT-history.md#allocation-costs-mixed-fp-and-region-layouts-2026-10-16)
- [Azur Lane polynomial instructions (2026-10-15)](archive/JIT-history.md#azur-lane-polynomial-instructions-2026-10-15)
- [Inline pixel-conversion helpers (2026-10-15)](archive/JIT-history.md#inline-pixel-conversion-helpers-2026-10-15)
- [Cached integer SIMD (2026-10-15)](archive/JIT-history.md#cached-integer-simd-2026-10-15)
- [Tier-1 vector allocation under pressure (2026-10-15)](archive/JIT-history.md#tier-1-vector-allocation-under-pressure-2026-10-15)
- [Tier-1 hot-path code reduction (2026-10-14)](archive/JIT-history.md#tier-1-hot-path-code-reduction-2026-10-14)
- [Azur Lane generated-code audit (2026-10-14)](archive/JIT-history.md#azur-lane-generated-code-audit-2026-10-14)
- [Scalar FP scratch preservation (2026-10-13)](archive/JIT-history.md#scalar-fp-scratch-preservation-2026-10-13)
- [Base ARMv8.0 FP/SIMD sweep fixes (2026-10-13)](archive/JIT-history.md#base-armv80-fpsimd-sweep-fixes-2026-10-13)
- [Window limits in the header page (2026-10-12)](archive/JIT-history.md#window-limits-in-the-header-page-2026-10-12)
- [Single-instruction extensions (2026-10-12)](archive/JIT-history.md#single-instruction-extensions-2026-10-12)
- [Register moves through XZR (2026-10-12)](archive/JIT-history.md#register-moves-through-xzr-2026-10-12)
- [Integer ADDP for every element size (2026-10-12)](archive/JIT-history.md#integer-addp-for-every-element-size-2026-10-12)
- [Eight-block superblocks and region vector state (2026-10-12)](archive/JIT-history.md#eight-block-superblocks-and-region-vector-state-2026-10-12)
- [Native acquire loads and bounded trial timing (2026-10-12)](archive/JIT-history.md#native-acquire-loads-and-bounded-trial-timing-2026-10-12)
- [Indirect branches and host return boundaries (2026-10-12)](archive/JIT-history.md#indirect-branches-and-host-return-boundaries-2026-10-12)
- [Half-precision conversions and ARM NaN results (2026-10-12)](archive/JIT-history.md#half-precision-conversions-and-arm-nan-results-2026-10-12)
- [Reciprocal estimates and refinement (SpongeBob, 2026-10-07)](archive/JIT-history.md#reciprocal-estimates-and-refinement-spongebob-2026-10-07)
- [SIMD logical fallback and Unity UI colors (2026-10-08)](archive/JIT-history.md#simd-logical-fallback-and-unity-ui-colors-2026-10-08)
- [Optimization checkpoint (2026-10-11)](archive/JIT-history.md#optimization-checkpoint-2026-10-11)
- [Native-code reduction checkpoint (2026-10-11, later)](archive/JIT-history.md#native-code-reduction-checkpoint-2026-10-11-later)
- [Inline native SIMD kinds (2026-10-13)](archive/JIT-history.md#inline-native-simd-kinds-2026-10-13)
- [Dispatch cache and worker profiling (2026-10-04)](archive/JIT-history.md#dispatch-cache-and-worker-profiling-2026-10-04)
- [Guest-call helper overhead (2026-10-04)](archive/JIT-history.md#guest-call-helper-overhead-2026-10-04)
