# JIT architecture and contributor guide

This guide maps the AArch64-to-x86_64 JIT and records the steps for changing
instruction support without letting decode, translation, optimization, and
code generation drift apart.

Dated sections retain the measurements and failures from their recorded
revisions. Later checkpoints supersede earlier results where explicitly stated;
historical benchmark scores are not measurements of the current executable.

The [source map](ARCHITECTURE.md) describes the subsystem layout and split
boundaries. Compiler prediction/leaf analysis lives in `Compiler/BlockAnalysis.cpp`,
Tier-1 orchestration in `BlockCompiler.cpp` and differential store-footprint
analysis in `VerificationStores.cpp`. The refactor's build/test checkpoint is
recorded in [TESTS.md](TESTS.md#subsystem-layout-and-source-splits-2026-10-11);
it does not establish a new performance improvement.

## Direct-call padding, probe order and the emulator pointer (2026-10-25)

A review of the generated code (`BIFROST_JIT_DUMP=1`, bytes through `objdump -b binary
-mi386:x86-64`) found three avoidable costs:

- **Direct `BL` calls saved host flags for alignment only.** The call sequence (`BL_CALL`) pushed
  `RFLAGS` and popped it after the callee, 24 bytes of frame in all, though guest NZCV is in
  `cpu.pstate` and the `add rsp` that follows clobbers the host flags anyway. It now reserves the
  same 24 bytes with `sub rsp,16` after the `WIN_REG` push. On Zen 3 `PUSHF` is 11 macro-ops and `POPF` 37
  (reciprocal throughput 4 and 13 cycles, Agner Fog's instruction tables). `BL_CALL` exists only
  with chain-skip off (`BIFROST_NO_CHAIN_SKIP=1`; under chain-skip `bl_call_disabled_` makes `BL`
  end the block and chain), so the default configuration does not run this path. A throwaway loop
  of one non-leaf call plus two leaf calls took 26.7 ns per iteration before and 15.7 ns after with
  chain-skip off, five alternating runs each. With chain-skip on (the default) it takes 9.7 ns
  before and after; QEMU takes 36.5 ns.
- **The indirect-branch probe tested the call depth last.** A `RET` inside a nested
  `jit_call_helper` (`jit_call_depth != 0`) always misses, but ran about 13 instructions of table
  work first. The depth compare is now the probe's first instruction; the other checks are unchanged.
- **The `Emulator*` lived in the frame, 32 KiB below the saved registers.** Every prologue stored
  it at `[rbp-0x7ff8]` and every exit and helper call loaded it from there, so each nested native
  call touched a second page, 32 KiB (the same L1D set) away. It is now `CPU::jit_emu`, stored by
  every prologue (`emit_store_emu`) and loaded through `RBX` (`emit_load_emu`); a chain edge skips
  the prologue but keeps the CPU. The default-mode call loop above went from 9.7-9.9 to 9.6-9.7 ns
  per iteration (five alternating pairs): about 1%, on a loop that never nests deeply.

Checked: `run_tests.sh --unit --toybox --realworld --quick` (243 of 243) and `--unit --fwd` (178 of 178)
pass. `--unit --no-chain-skip` has two failures, `test_fork_lock_contention` and `test_exec_mt`,
which fail the same way with the previous binary. The `make test-native-simd-helpers` link line lacked
`DecodeCache.cpp` since 62e0fdf6, which stopped `make verify` before its differential gate; it is added.
Not measured: any game, and how often the 16-level `JIT_MAX_CALL_DEPTH` limit unwinds a call stack.

Still open from the same review: the default return path is the indirect-branch probe (about 21
instructions and six loads per `RET`), every exit still loads `RSI` with the emulator pointer that
chain successors no longer read, and `build_ssa` and `lower_ssa` each run `validate_ssa` in release
builds (about 1-2% of a very short cold run, not worth removing a safety net on that evidence).

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
  threads that need them). A persistent
  translation cache (`--jit-disk-cache`, work not yet committed when this was written) is meant to make
  that cheaper; it was run together with renewals only on the pressure fixture (correct results), not in a game.

Tests: `Tests/Unit/jit_cache_generations.c` through `Scripts/Testing/test_jit_generations.sh` (1 MiB
budget, about 500 renewals: sleepers in nanosleep and futex, a futex wait inside a nested call,
nested call loops that compile throughout, rewritten code, signals, a fork afterwards; every result
checked), and `test_jit_cache.sh`, which now expects renewals at a 1 MiB budget.

## Per-block bookkeeping and per-thread dispatch caches (2026-10-24)

Battle Cats (title screen, five runtime processes: the game, the WebView
renderer, GMS, the WebView service, the Play Store companion) used 5.25 GiB
PSS, of which only about 1.1 GiB was guest memory. Per process, `malloc_info`
showed the host heap was live data, not fragmentation (the game 964 MiB, the
renderer 465 MiB, 122-146 MiB in each helper), and `[JIT] code-cache` showed
1.4 GB of live code (the game 555 MB for 709k blocks, about 145 host bytes per
guest instruction; nothing retired).

`[JIT] metadata MiB:` (printed after `[JIT] code-cache`, `Runtime/MemoryStats.cpp`)
now reports the heap behind each per-block container, from container sizes and
capacities with glibc chunk rounding and libstdc++ node layouts. A generated
MT fixture with 29,638 blocks measured 658 bytes per block before the changes
below and 468 after:

- `BlockEntry` groups its fields by size and keeps code-buffer offsets in 32
  bits (`CODE_BUF_SIZE` is asserted to fit): 232 -> 160 bytes, a 176-byte
  instead of 256-byte malloc chunk per `blocks_` node.
- MT keeps no `back_refs_`: `chain_back_references` only refreshes
  `mt_edge_refs_` cells there, Tier-2 redirection returns before using it,
  and retirement clears the same cells through `mt_edge_refs_[victim]`. The
  dispatcher's chain retry refreshes MT edges without consulting it.
- `mt_edge_slots_` (code offset -> edge, one hash node per edge) is gone. An
  `MtEdge` records its slot in what was padding; `mt_slot_edge` scans the
  source's one to three edges in `mt_source_refs_`. `patch_chain` takes the
  source PC, and the Tier-2 sampling revoke matches `trial.sampling_edges`.
- Each thread's inline dispatch cache starts at 256 sets (16 KiB) and
  quadruples, up to the previous 8,192 sets, once it has evicted one target
  per set since its last resize; clearing writes only the current size. An
  app's ~200 threads are mostly idle pools, and 512 KiB each had been about
  100 MiB in the game. The 64 KiB IBL table keeps its size: generated code
  masks with `IBL_ENTRIES`.

Generated code is unchanged. With the new build Battle Cats peaked at
4.41 GiB (the game 1,982 MiB, the renderer 1,084 MiB); its game process
reported 520 bytes of bookkeeping per block (339 MiB: `blocks_` 145, MT edges
and their maps 146, code words 29, thread caches 28 MiB for 230 threads). The
runs differ in length (peaks at 81 s and about 100 s), and part of the drop is
the Play Store companion no longer running (see the Android runtime guide), so
the total is indicative. A hot indirect-call fixture (3,000 targets, 300
rounds) ran in the same time (about 0.26 s) as before. `jit_dispatch_cache_growth`
calls 2,048 targets from four threads while another unmaps code to change the
dispatch generation. The focused JIT/threading suite (77), `make verify` (its
known `jit_bfm_extract` timeout aside) and the MT/Tier-2 scripts pass.
`test_jit_mt_publication` fails intermittently in mode 0 of its self-loop
check, before and after this change (6/200 and 4/200 runs): the preemption
ticker that `enter_multithreaded` starts can end a fresh-lease loop early.

The game's remaining heap besides this bookkeeping (about 500 MiB) is not yet
attributed, and code size is the larger cost.

## Unsigned halfword min/max are native (2026-10-23)

`UMAX`/`UMIN` with 16-bit lanes (`.4h`, `.8h`) took `emit_call_interp` while the byte and
word forms were native: `SIMD_ARITH` had no halfword unsigned min/max. It now emits
`pminuw`/`pmaxuw` (66 0F 38 3A / 3E, SSE4.1), the same host-feature gate as the word forms
(`pminud`/`pmaxud`); a host without SSE4.1 still takes the interpreter step. Unsigned
halfword clamps are common in 16-bit pixel and audio code, and one interpreted step
costs about 130 ns here, which also forces the whole block out of the vector cache.

A pixel-style loop with four interpreted vector ops (`umax`, `umin`, `urshr`, `srshr`, all
`.8h`, 2.6 million iterations each) took 1.38 s; with the two min/max forms native it takes
0.64 s (three runs each, 0.07 s without the vector ops; the interpreted inventory from
`BIFROST_FALLBACK_PROFILE` no longer lists them). `Tests/Unit/jit_simd_minmax.c`
checks UMAX/UMIN/SMAX/SMIN for every lane size and both widths, with Rd distinct from,
equal to Rn and equal to Rm, against a lane model written from the Arm ARM pseudocode
(432,000 checks; edge values plus random lanes, Q=0 results must clear the upper half).
The model passes under QEMU, the interpreter and the JIT, and under `BIFROST_JIT_VERIFY=1`.

## Pixel and media integer forms are native (2026-10-23)

A profile of the English Umamusume client and `Scripts/Testing/simd_coverage.py` (22,739
FP/SIMD words that stepped the interpreter) listed integer NEON forms that image, video and
UI code uses constantly. One interpreted step costs about 130 ns and, because a block holding
one cannot use the vector cache, drags every neighbouring instruction down with it. These
forms are now native kinds in `Tools/CodeGen/native_simd.txt` (spec row, `run<K>` reference
body, inline emitter):

| Form | Kind | Inline sequence (XMM0-2 scratch only) |
|---|---|---|
| `SHADD/UHADD`, `SRHADD/URHADD`, `SHSUB/UHSUB` | `HalvingAdd` | bytes/halfwords through `pavgb/pavgw`; words with shifts |
| `SRSHR/URSHR` (vector; scalar D takes the helper) | `RoundShift` | see below |
| `RSHRN/RSHRN2` | `NarrowShift` (rounding bit 11) | `(x >> s) + ((x >> (s-1)) & 1)` in the source lanes, then the `SHRN` narrowing |
| `CMTST`; `CMGT/CMGE/CMLE/CMLT #0` | `TestBits`; `CompareZeroInt` | `pand` + `pcmpeq`; `pcmpgt` against zero, GE/LE inverted |
| `MUL .8b/.16b`, `MLS` | `MultiplyAdd` | the existing byte-multiply sequence, then optional accumulate or subtract |
| `SABA/UABA` | `AbsDiffAcc` | `max - min` (unsigned bytes and signed halfwords are SSE2, the rest SSE4.1) |
| `REV32`, `REV16` | `Reverse` (was `Reverse64`) | word shuffles and a byte swap |

Exact identities the emitters rely on (each checked by the tests below):

- `(a + b + 1) >> 1` is `pavg`. `UHADD = ~pavg(~a, ~b)`, `UHSUB = pavg(a, ~b) ^ S`, and signed forms bias
  by the lane sign bit `S` (`SRHADD = pavg(a^S, b^S) ^ S`, `SHADD = pavg(a^M, b^M) ^ M` with `M = ~S`,
  `SHSUB = pavg(a^S, b^M) ^ S`). 32-bit lanes use `(a>>1) + (b>>1) + carry` with the right shift kind.
- `RShr(x, s) = ((x >> (s-1)) + 1) >> 1`, which `pavgw` evaluates without the overflow that adding the
  rounding constant first has in a full-width lane; unsigned bytes widen to words first, signed ones
  sign-extend, round and `packsswb` back (the result is at most 128 in magnitude). Other lanes use the
  `(x >> s) + ((x >> (s-1)) & 1)` form. A signed 64-bit `SRSHR` needs `psraq` (AVX-512) and takes the helper.
- The `RSHRN` rounding constant cannot be added in the source lane (`0xffff + 0x80` overflows); the bit form is
  exact.

`native_simd_inlinable` declines what a host cannot do: signed 64-bit and scalar rounding shifts, `CMGT..#0`
on 64-bit lanes without SSE4.2, and `SABA/UABA` forms needing SSE4.1; those run the helper.

**What was checked.** `Tools/CodeGen/native_simd_expected.txt` gained 689 QEMU hashes (1,480 to 2,169
encodings); `jit_native_simd_gen` passes all of them in the JIT and interpreter. Because the JIT inlines these
kinds, the new host test `make test-native-simd-helpers` (also run by `make verify`) calls every `run<K>` body over
the fixture's inputs and compares with the same hashes: 2,169 pass, and it fails when a hash is altered.
`Tests/Unit/jit_simd_pixel.c` (376,104 checks) and `jit_simd_round.c` (158,628) compare against lane models
that compute in 128-bit integers: byte lanes see all 65,536 operand pairs (or all 256 values at every shift),
wider lanes edge values and random data, with Rd distinct from, equal to Rn and equal to Rm, plus hot loops that
chain 14-16 live vectors and carry accumulators. They pass under QEMU, the interpreter, the JIT and
`BIFROST_JIT_VERIFY=1`. Nine deliberate single-point defects in the emitters (wrong `pavg` width, swapped
`max`/`min`, dropped rounding bit, wrong constant, un-inverted `CMTST`, `MLS` adding, ...) and a dropped
`Vd` read declaration for `SABA/UABA` in `vec_inst_operands` were each caught. A dropped `Rm` read declaration
for `CMTST`, `HalvingAdd` or `SABA/UABA` was not, in loops where the operand is live into the loop or rewritten
at its end; I did not establish why, so those declarations rest on review.

**Measured.** The four-op pixel loop of the previous section (`umax`, `umin`, `urshr`, `srshr`, `.8h`, 2.6
million iterations each) now takes 0.070-0.075 s, from 1.28-1.34 s at `e7ed1b3c`, the commit before this work (QEMU 0.05-0.10 s on the
same machine, noisy); `BIFROST_FALLBACK_PROFILE` lists nothing for it. That is one synthetic loop, not a game frame
time.

**Still interpreted** (from the same sweep): the saturating family (`SQADD/UQADD/SQSUB/UQSUB` on word and
doubleword lanes, `SQSHL/UQSHL`, `SQSHRN` and the other saturating narrowing shifts, `SQXTN/UQXTN` on wider
lanes, `SQDMULH/SQRDMULH`, `SQABS/SQNEG`), the register rounding shifts (`SRSHL/URSHL/...`), `CLS/CLZ`, `PMUL`,
`URECPE/URSQRTE`, scalar-D variants of several shifts, `SRSRA/URSRA` scalar and the 64-bit lane
`SSHR/SSRA/SRSRA`. They keep the interpreter's correct but slow path.

## Single-thread self-loop reconcile is out of line (2026-10-23)

`BIFROST_JIT_VERIFY=1` showed `v28` taking `v27`'s value after a loop with 14 live vector registers. The
vector allocator keeps 13 XMM registers (`xmm3`-`xmm15`); the 14th value shares a demand slot, and the loop
back-edge reconciles that layout (store the dirty demand value, reload the entry value). The single-thread
back-edge emitted that reconcile *before* its patchable `jmp` slot. Differential replay unpatches the slot, so the
reconcile ran and the following exit still wrote back for the layout at the branch: `xmm15` held `v27` but was
stored to `v28`'s slot. A clean build of `e7ed1b3c` shows the same divergence with only instructions that
were already native (`mla`, `eor`, `movi`), so this predates the new kinds; a patched slot (every production run)
never reaches that exit. It surfaced when a new vector-pressure loop first ran under the differential gate.

`emit_selfloop_reconcile_stub` now emits the reconcile after the exit, patches the slot to the stub, and the stub
jumps to the body (the MT edge already ordered its checks ahead of the reconcile). When no reconcile code is
needed the slot still jumps straight to the body and nothing changes. A 12-accumulator `mla` loop (16 live vectors,
a non-empty reconcile) grows by the 5-byte `jmp` and runs the same: 0.416 s against 0.417 s over five alternating
runs against `e7ed1b3c`. `Tests/Unit/jit_vec_pressure_loop.c` is the focused regression (one divergence
before, none after, under `make verify`).

## Bitfield moves use immediate shifts and the allocator's register (2026-10-21)

`UBFM`/`SBFM` (ubfx, sbfx, ubfiz, sbfiz, lsl, lsr, asr, uxt*, sxt*) were emitted
through `RAX`, `RCX` and `RDX`: the source was staged in `RAX`, the general case
rotated through `CL` and built its mask with a `movabs` into `RDX` (a `ubfx` was
six instructions; every `lsl` also built a full mask that its shift made a
no-op). The emitter now works in the register `result_reg` chooses, like the
immediate shift ops, and uses only immediate forms:

- extract: `shr` + `and r32, imm32` (a `movzx` for 8/16 bits, `mov r32, r32` for 32,
  `shl` + `shr` for 33..63 bits), or `shl` + `sar` for the signed form;
- insert: the mask is skipped when `imms + 1 == immr` (a plain `lsl`), and
  `sbfiz` is `shl` + `sar`;
- 32-bit forms use 32-bit instructions, so the input's upper half is ignored and the
  output's is zero. `movzx r32, r8` always has a REX prefix (without one, registers
  4..7 mean `AH..BH`; the allocator pool is `RAX RCX RDX R8 R9 R11-R15`).

Measured on CoreMark (`perf stat`, 40,000 iterations, matched builds, three
alternating pairs): 1,578k -> 1,506k host instructions per iteration (-4.6%), 395k
-> 359k cycles (-9%), IPC 4.0 -> 4.2; 11.4k -> 12.65k iterations/s with every CRC
valid. The instruction saving is small; most of the cycles come from removing
spill/reload pairs the forced `RAX` caused (`mov [rbp-..],rax ... mov rax,[rbp-..]`).

`Tests/Unit/jit_bfm_extract.c` checks all of it: every `immr`/`imms` pair, both widths
and signedness, aliased and distinct registers, twelve inputs each, and a register-pressure
section that keeps 0-9 temporaries live so the result lands in the later pool
registers. Its expected values follow the Arm ARM `DecodeBitMasks` pseudocode, and the
test passes under QEMU and the interpreter (3,102,720 checks). The pressure section
exists because the first version, without it, still passed after the REX bits for
`R8`-`R15` were deliberately broken; random programs caught that bug (14 of 153) and
the extended test now does too. Random-program differential testing (compiler-generated
bitfield code against QEMU, 420 programs at -O1/-O2/-O3) also passes. These cover
`UBFM`/`SBFM` only; other emitters that force fixed registers were not changed.

## Branch flags, immediates and Q pairs (2026-10-20)

The two remaining weak spots of the game-style set were a pointer-chasing loop
(0.64x of QEMU) and a 64-byte struct copy (0.51x). Their generated code
(`BIFROST_JIT_DUMP=1`) showed five separate problems, each general.

- **Q-register pairs.** `ldp q`/`stp q` were still the only pairs on the old software
  guard (about 25 instructions per 16 bytes: push/pop, a window compare, a page-flag
  table walk, and a slow-path call). `emit_memory_guard` treated every pair as a whole
  instruction. A Q pair is two independent 16-byte accesses (no register can alias the
  base, a load writes only its own register, a store only memory), so a restart after a
  fault on either half repeats the first harmlessly, as for the GPR and S/D pairs. They
  now take the recoverable native guard like `ldr q`. Struct copy 753 → 316 ns/iter
  (QEMU 395).
- **Flags a branch only reads.** A block that is just `b.cond` loaded NZCV from `cpu.pstate`
  into host flags (14 instructions) and then stored the same flags back to `pstate` on
  both exits (9 instructions each). `flags_match_pstate_` records that the host flags
  were loaded by a BRCOND and no setter has run since; `clobber_flags`,
  `materialize_flags_to_pstate`, the BRCOND exits and the call paths then emit no store
  (`host_flags_match_pstate()`, public Tier-1 compiles only; every flag setter clears it).
  `pstate` holds the same NZCV; the loader ignores the stored marker bit.
- **Single-flag conditions need no host flags at all.** EQ/NE (Z), CS/CC (C), MI/PL (N)
  and VS/VC (V) on flags that are still in `pstate` are one `test dword [pstate], mask`
  and a branch: nothing is loaded, flushed or stored. The `b.eq` block of the chase loop
  went from 251 to about 100 bytes. HI/LS/GE/LT/GT/LE keep the loader.
- **Immediates for flag setters.** `ADDS`/`SUBS` (so `cmp`/`subs`/`adds` with a constant) and
  their `fold_ahead_kind_` pre-scan now fold a constant second operand into
  `cmp/sub/add r, imm` (32-bit form: any constant; 64-bit form: imm32 sign-extension fit;
  CMN keeps the generic path), as `ADD/SUB/AND/OR/XOR` already did. No `mov r, imm`, no
  register held for it.
- **Zero extensions that cannot do anything.** `Optimize.cpp` Pass 0b tracks scratch vregs that
  provably fit N bits (a load of N/8 bytes, a smaller ZEXT, a small IMM, a 32-bit
  ADDS/SUBS result, AND with such a value, OR/XOR of two) and turns a ZEXT to N or more bits
  into a copy that Pass 1 propagates away. The 32-bit ADDS/SUBS claim rests on the one emitter
  both tiers share using the 32-bit x86 form; the load claim on `LOAD_MEM` zero-extending.
- **A spill on the load chain.** `compile_guarded_mem` called `clobber_flags()`, which flushes
  and drops RAX, RCX and RDX because `emit_materialize_flags` used to clobber all three. It now
  overwrites RDX only (it saves and restores RAX, never touches RCX), and that path, which
  chooses registers through the allocator, passes `rdx_only`. In the chase loop the chased
  pointer (RAX) was stored to the stack and reloaded in front of the next load: a store-forward
  on the dependent chain. Other callers keep the three-register contract.
- **Measured (same machine, `HEAD` against this tree, minimum of seven runs, QEMU for
  reference).** Pointer chase 1,905 → 1,282 ns/iter (QEMU 1,177), struct copy 753 → 316
  (395), array bounds 2,350 → 1,862 (3,087); the others within noise of the earlier
  checkpoints. Against QEMU the whole 13-kernel set is now faster on twelve, 0.92x on one.
- **Checked.** `jit_branch_flags`: every condition against all sixteen NZCV values (MSR in the
  previous block), five-branch chains each in its own block on CMP and on ADDS flags ending in
  MRS (the flags must reach `pstate` unchanged on every edge), and LDP/STP Q copies of sizes
  32..1,024 bytes at every 16-byte source offset into page-crossing destinations. It fails
  with the clean-flags bit left set after `ADDS`/`SUBS` (checked). `jit_imm_flags_zext`: seven
  arithmetic immediates of both widths for SUBS/ADDS/CMP/CMN and logical immediates for TST/ANDS,
  against 22 values with garbage in bits 63:32, W results and 32-bit loads feeding 64-bit consumers, 64-bit results read back
  through W operations; one hash, QEMU's, equal on the JIT, the interpreter and the old build;
  it fails when a 64-bit `subs` is treated as a 32-bit result (checked). `jit_host_fault`
  gained Q-pair probes (second half unmapped, a store pair, an unmapped pair) and passes under
  QEMU, the new JIT and the old one. `make verify` (95 fixtures, 8 threaded), 155 unit tests
  (also `--no-chain-skip`, `--no-jit`, `--fwd`), Toybox and real-world (65), the MT/Tier-2/cache
  scripts, zstd and `generated-check` pass.
- **Not done.** Three loads from one base each keep their own bounds and software-check guard
  (five instructions). Sharing one check is only sound if it also rules out address wrap-around
  between the offsets, and the sticky software-check byte must not be hoisted across a loop
  iteration; left alone. `STORE_REG` is still write-through (about seven stores per iteration
  of the chase loop), the flags of a `subs` are stored before the next guarded access because
  the access may fault, and no game ran.

## Native exclusive loads and stores (2026-10-20)

An LL/SC loop (`ldaxr; add; stlxr; cbnz`, what every non-LSE `atomic_fetch_add`,
refcount and spinlock compiles to, and what the outlined `__aarch64_*` helpers
fall back to) cost about 65 ns per operation against QEMU's 15, and `stlr` 20 ns
against 3 (`atom_llsc`, below). LSE atomics were already native and level with
QEMU (2.4 ns). Each of `ldxr`, `stxr` and `stlr` was a C call that took a
`std::mutex` shard, looked an address up in an `unordered_map<addr, vector<CPU*>>`
and registered or erased the CPU in it (heap allocation included).

- **Monitor.** `System/Core/ExclusiveMonitor.hpp`: a process-wide table of 2,048
  cache-line-sized bucket words, hashed from the 8-byte granule (`kHashMul`, 32-bit
  arithmetic on the low address bits, which the generated code repeats). A word is a
  lock bit and a version. A reservation is the CPU's tag (address, size, loaded value)
  plus `CPU::excl_ver`, the bucket version read before the value at LDXR time.
  STXR locks the bucket, requires the version to still match, compare-and-swaps the
  loaded value, then unlocks (with a version bump if it wrote); STLR locks, stores and
  unlocks with a bump. The bump is one `add` on the locked word (bit 0 carries into
  the version), so a reader that sees a version also sees the write behind it.
  A collision or an unrelated write only makes a store-exclusive fail spuriously, which
  the architecture allows. Plain stores and LSE atomics do not bump, as before; the
  value compare covers them. The interpreter (`LDXR/STXR/STLR`, `LDXP/STXP`), the C++
  helpers (`jit_ldxr/jit_stxr/jit_stlr`, now only the slow path) and the native
  sequences all follow it; the old shard structures, `excl_remove_cpu` and the LSE/CASP
  shard locks are gone (LSE RMW and CASP were already host CAS loops).
- **A first version without a lock failed, on real parallelism only.** Bumping the
  version before the write and checking it in a `lock xadd` ahead of the
  `lock cmpxchg` left a window as wide as a cache miss on the contended word, in which
  other CPUs completed whole pop/push transactions, so recycled pointers passed the
  value compare (a corrupted LIFO; QEMU shows the same). It passed pinned to one host
  CPU. Holding the bucket across check and write, as the old mutex did, closes it.
- **Native code.** `CodeGen/Integer/Atomics.cpp` inlines the protocol (a handful of
  fixed registers, flags clobbered): LDXR is the version read, a guarded load and the
  tag stores; STXR the tag checks, `lock bts` on the bucket, version compare,
  `lock cmpxchg` and an unlock; STLR the lock, a store and the unlock. The access is a
  registered fault site (the exclusive ops now take the recoverable guard that
  `LOAD_MEM`/`ATOMIC` use), and its recovery stub first releases the bucket, then
  continues to the generic stub that steps the interpreter over the instruction under the
  same protocol. A contended lock calls `jit_excl_lock_slow` (spin, then `sched_yield`).
  Configurations without fault sites keep the helper calls.
- **CLREX did nothing, in both engines.** The decoder only recognised `clrex` with
  CRm=0 (`D503305F`); the assembler's plain `clrex` is CRm=15 (`D5033F5F`), which fell
  into the hint range as a NOP, and the JIT translated even a recognised CLREX to a NOP. So
  `ldxr; clrex; stxr` succeeded where QEMU's fails. The decoder now takes any CRm and
  the JIT has a native `CLEAR_EXCLUSIVE` op (one byte store to `excl_tag_valid`).
  This was found by comparing the new fixture with QEMU value by value; the old build
  fails it too.
- **Measured (same machine, `HEAD` against this tree, QEMU for reference).**
  `fetch_add` 68.2 → 11.3 ns/op (QEMU 17.7), `cas` 64.8 → 13.1 (20.8), `xchg64`
  62.9 → 8.8 (14.8), `ldar/stlr` 20.5 → 3.6 (2.7). The game-style `atomics` kernel
  (outlined LL/SC helpers, two per iteration) 128,537 → 22,160 ns/iter, minimum of six
  runs (QEMU 29,026). Four threads on separate counters 245 → 4.4 ns/op (QEMU 3–4);
  four threads on one counter 250 → 158 (QEMU 70: it contends one cache line, this
  two, the bucket and the data). LSE results are unchanged.
- **Checked.** `jit_llsc_native` (new): every width and acquire/release form of
  LDXR/STXR with no reservation, another address, CLREX, a success, a plain store of a
  different value and a replaced reservation, release/acquire accesses and LDXP/STXP:
  one hash over 2,492 values, identical under QEMU, the JIT and the interpreter. Four CPUs
  with exact totals: LL/SC counters of each width, a spinlock from `ldaxr/stxr` and `stlr`,
  cells sharing a cache line, and an ABA-prone LIFO (eight recycled nodes) that fails with
  the version check removed (checked) and under QEMU. Faults: `stxr` to a read-only page,
  `ldaxr` from a PROT_NONE page, `stlr` to a read-only page (each raises SIGSEGV once and
  the loop completes), and no stale reservation after a signal. `jit_host_fault` gained
  faulting `ldxr/ldaxr/ldaxrb/stlr/stlrh` probes. `make verify` (93 fixtures, 8 threaded
  oracle runs), 153 unit tests (also `--no-chain-skip`, `--no-jit`, `--fwd`), Toybox and
  the real-world filter (65), the MT/Tier-2/cache scripts, the zstd regression and
  `make generated-check` pass; six copies of the new fixture running at once on four
  threads each also passed.
- **Not covered.** Reservations never crossed OS processes (a process-local table, as
  the shard mutexes were) and still do not. A spinning waiter whose lock holder is
  descheduled burns its time slice (it yields after 2,048 spins) where the mutex slept.
  An ASan build was not available. No game ran.

## Scalar FP loads and stores keep the XMM cache (2026-10-20)

`BIFROST_JIT_DUMP=1` of a double-precision loop (`ldr d1, [x0, x2, lsl #3]`,
`fdiv`, `fmul`, `fnmsub`, `fadd`) showed no register cache at all: every FP
operand was loaded from `cpu.v_lo`, the result stored back, and a zero stored to
`cpu.v_hi`, about six memory operations per instruction. The cause was one op.
`IR/Translate/Memory.cpp` lowered `ldr`/`str` of B/H/S/D (and S/D `ldp`/`stp`) to
`LOAD_MEM`/`STORE_MEM` plus `SIMD_LDST`, which no cache admits
(`fp_cache_compatible_op`, `vec_inst_effects`), so any block holding a scalar FP
load or store was disqualified, and real float code is full of them.

- **Lowering.** A scalar load is `LOAD_MEM` then `FMOV_G2F` (Vd.lo = value, Vd.hi =
  0; the load zero-extends B/H/S into the register); a scalar store is `FMOV_F2G`
  then `STORE_MEM`. Both FMOVs already had cache-aware code and the same
  architectural effect as the old pair, and the write still follows the load, so a
  faulting load leaves Vd unchanged. Pair loads still complete both loads before
  writing either register.
- **Cache side.** `vec_inst_effects` lists `FMOV_F2G` as a read of its FP source, and
  `emit_fmov_helper` reads a pinned XMM for it in the full vector cache as well
  (it previously did so only in the lo-only scalar cache; the full cache treated the
  op as a synchronization boundary, so no such block reached that path).
  `FMOV_G2F` no longer moves its source into RAX (spilling RAX's occupant) before
  testing for a pinned destination, and in the full cache it skips the zero-upper
  instruction `MOVQ` makes redundant.
- **Measured (same machine, `HEAD` against this tree, minimum of five runs of the
  game-style kernels, a game running alongside).** Double-math loop 6,114 → 2,880
  ns/iter; float `sqrtf`/`floorf` loop 14,186 → 7,649; int16 PCM-to-float mix 12,205
  → 7,199; an entity-update loop with calls (`kern`) 0.179 → 0.160 s. Against QEMU's
  user-mode emulator on the same kernels that is about 6.3x, 4.8x and 3.2x faster
  (it was about 3x, 2.6x and 1.9x). Checksums match QEMU.
- **Not done.** Each scalar write to a cached register still stores a zero to
  `cpu.v_hi` in the lo-only cache. Deferring it to the writeback is only correct
  for registers statically dirty at that point (a loop-carried register may still
  hold a vector's upper half on the first trip), and every publication site would
  need to know; not attempted. Replacing the reg-reg `movsd`/`movss` copies with
  `movaps` (they merge into the scratch register, a false dependency) was measured
  and was within noise, so it was not kept. Tier-2 is off by default.
- **Checked.** `jit_fp_ldst_cache` hashes the architectural result, upper lanes
  included, of compiler-generated D/S loops (arithmetic, conversion, compare, struct
  fields) and of B/H/S/D loads, stores and pairs (offset, pre- and post-index) into
  registers preloaded with ones, plus more live FP registers than the cache holds;
  its three hashes are QEMU's. `jit_host_fault` gained `ldr d0/d8/s8/h8` and
  `str d8/s0` probes: `d8` is written just before the faulting access and must keep
  its value, also when it is the load's destination. `make verify` (93 fixtures, 7
  threaded), 152 unit tests, the MT and Tier-2 scripts and the zstd regression pass.
  Not run: an ASan/UBSan build (`make debug` fails on a missing Vulkan video header
  in `WindowStats.cpp`, unrelated), and no game.

## Dead NZCV stores at block exits (2026-10-20)

A conditional branch that leaves with live host flags stores them to `cpu.pstate` on
each exit (`lahf; seto; and; imul; and; xor; mov; pop`, nine instructions that also
clobber RAX/RCX/RDX). In a loop of `ldr; add; add; cmp; b.le` followed by `cmp; b.ne`
the fall-through exit's successor starts with `cmp`, which rewrites all four flags, so
the store was dead on every iteration. The earlier rule (see "Bitfield insertion and
NZCV passthrough") stands: a successor that merely does not read flags proves nothing,
because it may forward them. What can be proved is that the successor *overwrites*
them first.

- **Scan.** `jit_analysis::nzcv_dead_in` (`Compiler/BlockAnalysis.cpp`) reads the first
  six words at an exit target and returns true when a flag-neutral, fault-free integer
  instruction run (moves, ADD/SUB/logical without flags, bitfields, shifts, multiplies,
  divides, bit counts, ADR/ADRP, CRC32) ends in `ADDS`/`SUBS` (so CMP/CMN), `ANDS`
  (TST) or `FCMP`/`FCMPE`. Anything else ends it as live: a flag reader (`b.cond`,
  `csel`, `adc`, `ccmp`, `mrs`), any branch, call or system instruction, a load or
  store (it can fault, and a guest signal frame must show the flags), and any
  unrecognized word. It is a fact about the guest instructions, so it holds however
  they run (chained, dispatched or interpreted).
- **Use.** `FrostJIT::exit_flags_dead` reads the words straight from the guest window,
  only from pages that are mapped, executable and not userfaultfd-missing (it predicts
  code that may never run, so it must not populate a page or fault), and
  `clobber_flags(dead)` / `materialize_flags_to_pstate(preserve, dead)` update the
  allocator exactly as for a store but emit none. Sites: both `BRCOND` exits (and the
  HI/LS carry-inversion that went with the store), `CBZ`/`CBNZ`/`TBZ`/`TBNZ` (whose
  single store feeds both exits, so both targets must overwrite), a block-limit
  fall-through and a direct `B`. A taken self-loop back-edge uses it too, in MT as
  well; the earlier loop-carry shortcut for single-threaded loops is unchanged.
- **Dependency.** The words a dead verdict used are recorded per block
  (`BlockEntry::flag_deps_off/count`, an append-only `flag_dep_table_` like
  `code_words_`; the entry itself must stay small, see below). They are
  invalidation dependencies like `inlined_ranges_`: overlapped by `invalidate_range`,
  indexed by `remember_code_granules`, checked by `staged_entry_invalidated`; and
  `block_code_unchanged` also requires their words to match and their pages to stay
  executable, so cache maintenance and `mprotect` that leave the words alone still keep
  the block. Only dead verdicts are recorded; a live verdict is always safe. Private
  Tier-2 sampling copies never predict (nothing tracks their dependencies). Differential
  verification skips its NZCV comparison for a block that did this
  (`skips_dead_flag_store`).
- **Policy change in MT.** The bounded MT self-loop section says expiry or revocation
  of a back-edge commits carried state including flags, and `jit_mt_publication`
  checked that for `add; add; subs; b.ne`. Where the loop head overwrites the flags
  first, an expiry exit now leaves the entry flags in `cpu.pstate`: nothing guest code
  can run observes the difference, but a signal frame taken at that exit, or a debugger,
  would show them. The check now expects the entry flags for that loop and still
  expects the committed carry for the loop whose head reads it (`adcs`).
- **Cost found on the way.** The first version stored the dependencies in a vector inside
  `BlockEntry`; the call-heavy `atomics` kernel (two helper calls per iteration, each a
  table lookup) slowed 7% with identical generated code. `BlockEntry` is read on every
  dispatch lookup, so variable-size per-block data goes to side tables; with them out of
  line the kernel matched `HEAD` again.
- **Measured (same method as above).** `array_bounds` (IL2CPP-style) 2,337 → 2,211
  ns/iter, `switch_dispatch` 18,910 → 17,567, pointer chase flat; the effect is the
  nine instructions on exits whose successor begins with a compare, so it is a few
  percent of such loops, not a large one. A successor that starts with a load keeps
  the store (the load can fault); that excludes the loop head of most real loops.
- **Checked.** `jit_flags_exit_dead`: fall-through and taken successors that overwrite
  or read the flags (MRS, CSET, CCMP, ADC, a load), flag-neutral instructions before the
  overwrite, B.HI/B.LS after `adds` (the carry inversion), CBZ/TBZ with one or both
  successors overwriting, a direct `B`, FCMP/FCMPE as the overwriter, and a warmed block
  whose successor, on another cache line, is patched to a NOP and back (the block must
  be retired; unchanged maintenance may keep it); the same values under QEMU, the
  JIT, the interpreter and verify mode. Three deliberate breakages each fail it (no
  dependency overlap, no dependency word check, flag readers treated as neutral).
  `make verify` (93 fixtures, 7 threaded), 152 unit tests (also `--no-chain-skip`,
  `--no-jit`, `--fwd`), Toybox and the real-world filter (65), the MT/Tier-2/cache
  scripts and the zstd regression pass. Untested: an ASan build, and any game.

## Flag setters no longer save dead flags (2026-10-20)

A 5-instruction byte-count loop (`ldrb; cmp; cinc; cmp; b.ne`) compiled to 467
bytes and about 55 host instructions per iteration, and ran only 1.04x faster than
QEMU's user-mode emulator. Its first `cmp` feeds `cinc`; the second `cmp` rewrites
all of NZCV. `ADDS`, `SUBS` (so `cmp`/`cmn`) and `TST` still called
`clobber_flags()` first, which materializes host flags to `pstate` (`push rax; lahf;
seto al; and; imul; and; xor; mov [rbx+0x110]; pop rax`, nine instructions that also
clobber RAX/RCX/RDX and force spills) before the new setter overwrote them.

A flag setter that rewrites all four flags makes the host flags dead, so
`Compiler/FrostJIT.cpp` now just clears `flags_in_host_` for `ADDS`/`SUBS`/`TST`.
Every earlier reader of the old flags is already emitted (IR order is the ARM
order), and any op that can fault or exit between a setter and the next (a guarded
load, a call) calls `clobber_flags()` itself, so the flags still reach `pstate`
before anything can observe them. `ADCS`/`SBCS`/`CCMP`, which read flags, are not
touched, nor is the partial-write case.

- **Measured.** The loop went from about 55 to about 30 host instructions per
  iteration and from 0.206 s to 0.147 s (three runs each, same machine); a
  float-struct loop 0.25 s to 0.20 s together with the pair change above.
- **Checked.** `jit_flags_overwrite`: CMP/CMN/TST/ADDS over 144 value pairs, the
  first flags read by `cset`/`csel`/`cinc` and the second by `b.ne` and `mrs nzcv`,
  and flags that must survive an intervening load and a plain `add`. It passes under
  QEMU, the JIT, the interpreter and verify mode. `make verify` (90 fixtures,
  7 threaded), 150 unit tests and 9 Toybox tests pass.

## Load/store pairs use the native guarded path (2026-10-20)

Reading the generated code of a game-style loop (`BIFROST_JIT_DUMP=1`, bytes
disassembled with `objdump -b binary -mi386:x86-64`) showed that every `ldp` and
`stp` took the old whole-footprint software guard: for each half, three
`push`/`pop` pairs, a window compare, a page-flag table lookup for the first and
last page, then a second limit compare. `compile_guarded_mem` (the native path:
`lea`, one window compare, the sticky software-check byte, then the access, with
host page protection and registered fault sites) returned false for any
instruction whose class is `LDP` or `STP`, because the pair translation wrote Rt
before the second load: a fault on the second half would restart the pair with
Rt already changed, wrong for `ldp x0, x1, [x0]`. Compilers emit pairs for every
prologue and epilogue, struct copy and float pair, so this was a large share of
guest memory traffic.

- **Translation order.** `IR/Translate/Memory.cpp` now completes both loads of a
  GPR, LDPSW and S/D pair into scratch vregs, then writes Rt and Rt2; writeback
  stays after both. STP already wrote no register before its stores, and a store
  repeated after a fault stores the same data.
- **Guard.** `compile_guarded_mem` no longer excludes pairs.
- **Measured.** The hot 11-instruction block of a float-struct update loop went
  from 3,649 to 3,140 bytes of x86, one access from about 35 host instructions to
  about 7, and the loop from about 0.255 s to 0.21 s (three runs each, same
  machine); QEMU-identical output. A real program with more pairs gains more.
- **Checked.** `jit_host_fault` now probes pairs whose second half faults
  (`ldp x`, `w`, `d`, `s`, `ldpsw`, `stp x` and `d`), a pair whose destination is its
  own base (`ldp x13, x12, [x13]`) and pre/post-index writeback, on four CPUs with
  exact pc, registers and flags, against QEMU; with the ordering change reverted
  the test fails. `make verify` (90 fixtures, 7 threaded), the 150 unit tests,
  the Toybox tests (9) and the real-world filter pass; `test_sandbox_in_root` fails in
  that batch with the old binary too (it passes alone).
- **Not changed, and visible in the same dump:** a float register is written to
  the CPU state for every load (value, plus a zero for the upper half), reloaded
  into an XMM register, and written back with more zero stores, about six memory
  operations per scalar float instruction in a block like this. Tier-2 is off by
  default. Both are the next candidates.

## Inline widening SIMD (2026-10-20)

A `perf` capture of Hungry Shark World (user-space sampling, 499 Hz, one run)
put 8.6% of the first 14 s of samples in `run<NativeSimdKind 24>`: the
`WidenThree` helper, one C++ call per instruction. In a 190 s run it was 1.7%
overall but about 21% of the 5-10 s bucket, which lines up with the game's 3.6 s
freeze after the first window (214 frames skipped at 10.3 s). `WidenThree`
and `PairLong` now have inline bodies in `NativeSIMDInline.cpp`:

- `WidenThree`: S/U ADDL, SUBL, ADDW, SUBW, MLAL, MLSL and MULL, both halves
  (Q = the "2" forms) and every size. The narrow lanes are extended with
  `pmovsx/pmovzx{bw,wd,dq}` (after `psrldq 8` for Q), then `padd/psub{w,d,q}`
  or the multiply for the size: `pmullw`, `pmulld`, `pmuldq` (signed) or
  `pmuludq` (unsigned). The product of two extended lanes is exact in the
  double-width lane, so the low-lane multiplies match the helper. MLAL/MLSL
  load Vd after the product, so every source is read before Vd is written.
- `PairLong`: S/U ADDLP and ADALP. 8- and 16-bit lanes use shifts (`psll`+`psra`
  for signed even lanes, a `psrl` mask for unsigned, `psra`/`psrl` for odd
  lanes) and `padd`; 32-bit lanes shuffle even and odd dwords with `pshufd` and
  extend with `pmov{s,z}xdq`. Q=0 forms zero the upper half of Vn (and of Vd for
  ADALP) first, as the helper reads only the low half.
- Both need SSE4.1 (`WidenThree`; `PairLong` only for 32-bit lanes), as
  `native_simd_inlinable` states; other hosts keep the helper. Scratch is
  XMM0-2, so they stay cache-compatible, and `vec_inst_operands` declares the
  reads: Vm for `WidenThree`, Vd for MLAL/MLSL and ADALP.

Checked against QEMU: the generated `native_simd_gen` fixture (every enumerated
encoding), and `native_simd_alias` extended with 26 forms whose Vd aliases a
source, hash `5dd49e555995750b` (QEMU, old and new emulator, and the
interpreter all agree). A guest loop mixing SMLAL/UMLAL2/SADDL/UADDW2/SMLAL .2d/
UADALP/SADDLP ran 2 million iterations in about 1464 ms with the helpers and
5.2 ms inline, with the same result as QEMU (one run each; an upper bound for
this instruction mix, not a frame time). `make verify` (90 differential fixtures,
7 threaded) and the 150 unit tests pass. Not measured: the effect on the game
itself, which needs a new live profile.

## Inline Select, Dup, ADDV, MLA and FRINT (2026-10-16)

After the FMAX/FMIN fix below, a SpongeBob avatar-switch profile still spent
about 19% of the main thread in native SIMD helper calls for 3-4 seconds per
switch. The helper kinds were MultiplyAdd, ReduceAdd, RoundFp, Select and
Dup. Their inline bodies are listed under
[inline native SIMD kinds](#inline-native-simd-kinds-2026-10-13). They are
cache-compatible: `vec_inst_operands` declares Vm and Vd as reads for
BSL/BIT/BIF and MLA, so the allocator loads the accumulator and selector
before the body reads them. A guest loop mixing these instructions drops
from about 624 ms to 5.3 ms with the same result as QEMU. That loop is an
upper bound for this instruction mix, not a frame-time measurement.

## Vector FMAX/FMIN signed zeros inline (2026-10-16)

A SpongeBob fallback profile over repeated avatar switches recorded 243
million interpreted guest instructions. 56% were FP cold steps, nearly all
vector FMINNM/FMIN/FMAX `.4s`. Their guard sent any lane with a zero result
to the interpreter, so clamps such as `fmax(x, 0)` stepped the interpreter
almost every time. The guard now covers NaN inputs only. Signed-zero ties
are resolved with an equal-lane mask and AND/OR (see the native vector FP
special cases below). The uncached path reloads the first input from
`cpu.v`, which is current there. A loop of zero-heavy FMAX/FMIN/FMINNM
clamps drops from about 302 ms to 5.4 ms with the same result as QEMU.
Scalar FMAX/FMIN still take the cold step for zero results.

The same profile also lists UMIN/UMAX `.8h`, FCVTNU, four-register TBL,
URSHR, FRECPS/FRECPE and plain stores (`stp q`, `stur x`) as interpreted.
These remain fallbacks.

## Cold-path reloads keep vector validity (2026-10-16)

The allocation checkpoint below tracks whether each cached vector slot holds
its current value. Helper clobbers and boundaries leave slots invalid and
memory-authoritative until their next read. Code emitted on a path that the
hot path skips must not change that compile-time state. The FP NaN cold step
(`emit_fp_interp_step`) and the SIMD 16-byte load/store slow paths reload
every pinned slot after their host call. `vec_emit_load_lo_hi` marked each
slot valid, so after an inline scalar FP cold tail the hot path treated
clobbered slots as resident and read stale XMMs. These paths now restore each
slot's validity after the reload. They still reload every slot at runtime:
a vector guard's step writes the destination that rejoins after its commit.

In Android games this corrupted values after a native vector helper (for
example a USHL or BSL helper) followed by a scalar FP operation, and broke
touch input. `jit_vector_cold_step` reproduces it with an inline USHL and a
scalar FADD in a loop. The values were checked against qemu-aarch64. A first
attempt that reloaded only valid slots broke vector guard destinations
(`native_simd_inline` `FCVTL` Inf/NaN rows) and showed as green sprite
corruption; the full suite catches that variant.

## Allocation costs, mixed FP and region layouts (2026-10-16)

This checkpoint supersedes the earlier fixed-layout Tier-2 restriction and
eager fixed-core reloads described in the October 15 sections below.

Both compilers share GPR pin selection: eligible loop-carried registers are
ranked by references, with a penalty for estimated simultaneous temporary
pressure. At most four pin prefixes are scored. Scalar eviction still uses
next use, preferring clean or rematerializable values on a tie. Scratch
constants can be reconstructed with flag-neutral MOVs instead of a spill
home; architectural GPR values retain their publication requirements.
Existing dying-input reuse remains in place.

Vector effects distinguish reads, full writes, scalar recovery reads and
known helper clobbers. A demand-allocated full-write destination reserves an
XMM without loading its old value. Mixed scalar FP/vector blocks use the full
vector cache, including scalar upper-lane clearing. Pure scalar FP blocks
retain the existing lo-only cache. Scalar recovery conservatively keeps the
original destination available until commit. NaN and precise-memory guards
retain their semantic steps and captured pre-commit layouts.

The vector planner scores three core sizes: the maximum legal fixed core,
half that size and demand-only. It estimates loads, writes and next-use
evictions, conservatively charging helpers and loop tails full publication.
Entry-copy savings are not modeled. These are bounded static heuristics,
using instruction references rather than sampled execution frequency; they
are not a global allocator or a host-specific throughput model. The approach
follows the cost/rematerialization concerns in LLVM's
[spill weighting implementation](https://github.com/llvm/llvm-project/blob/main/llvm/lib/CodeGen/CalcSpillWeights.cpp),
without adopting LLVM's allocation pipeline.

Known native Select, RoundFp and VariableShift helpers publish their vector
inputs and any valid resident values whose XMMs the body can destroy. Inline
variable shifts declare XMM0-5 clobbers; C++ calls declare every SysV XMM
caller-clobbered. Written/clobbered values reload lazily on their next read.
Other effects retain full synchronization. These contracts were checked
against the local helper bodies, with the conservative publication rule also
documented by [QEMU's helper interface](https://www.qemu.org/docs/master/devel/tcg-ops.html#helpers).

Tier-2 now admits demand allocation over a complete fused path. Conditional
side exits snapshot vector homes, validity and dirtiness as well as GPR and
flag state. Loop entry can seed demand values read before definition. Taken
backedges publish other dirty values and reconcile the entry layout with
parallel copies, breaking cycles through XMM0. Early eviction must publish a
seeded value potentially written on a previous trip. Copies execute after
MT revocation/lease checks and region trial exits, so those exits still see
their original branch-point layout. External edges continue to publish
canonical architectural state; there are no specialized cross-region entry
variants. Leaf conditional-skip joins retain their complete-layout restriction.
The existing feature gates, opt-outs and immutable MT target cells remain.

`BIFROST_REGALLOC_PROFILE=/tmp/regalloc-%p.tsv` opts into compile-time rows
with guest PC, emitted host range, numeric IR operation, tracked scalar/vector
allocation deltas and synchronization reason. Translation and SSA lowering
preserve instruction provenance; genuinely synthetic operations may have PC
zero. `entry`, `exit` and `cold` rows have `counter_scope=range-only`: their
zero counters are placeholders, not measured zero cost. IR rows have
`counter_scope=ir-delta`. Hand-emitted region branch code can leave gaps;
inline recovery is part of its emitting IR's range, while `cold` identifies
separately emitted tails. This is not exhaustive instruction-level profiling.
The file is append-only, mutex-protected, and expands `%p` for multiprocess apps.

Join host ranges to sampling IPs to distinguish emitted traffic from executed
cost. Counts are static emission events, not dynamic loads or cache misses;
code reuse after a flush requires capture-time correlation. Existing
`BIFROST_REGALLOC_STATS=1` totals now include epilogues and deferred recovery,
with rematerialization and hot/deferred byte counts. Counters belong to each
compiler instead of one process-global object. Compare compile time separately
using the existing `[JIT] compile` statistics. Profiling itself adds overhead.

Five alternating measured replays after warm-up of the captured 31-instruction
Unity block reduced median thread CPU time from 91.012 to 63.127 ms per
million calls (30.6% less) and emitted bytes from 3,128 to 1,463. Complete
vector, NZCV and memory output matches the interpreter and saved QEMU oracle.
The prior fourteen-accumulator FADD/FMLA loops were essentially unchanged;
CoreMark throughput fell 0.6% and zstd compression CPU time rose 0.9% in this
run. CoreMark compilation rose from 14.55 to 15.02 ms and zstd compilation
from 61.75 to 62.41 ms. Other desktop workloads were not stopped. These
measurements do not establish a game FPS or stutter improvement. See the
[validation scope](archive/TESTS-history.md#allocator-effects-and-region-demand-layouts-2026-10-16).

## Azur Lane polynomial instructions (2026-10-15)

An installed Azur Lane 9.3.7 loading/download profile exposed byte PMULL in
the interpreter (about 13% of sampled user cycles by disassembly attribution).
The existing native PMULL path covered only 64-bit polynomial products.
The classifier now also accepts precisely the byte PMULL/PMULL2 encodings;
SSE2 widens the selected eight source bytes into halfwords and XOR-accumulates
their shifted products without integer carries. Both inputs are read before
writing an aliased destination. It preserves NZCV and requires no additional
host CPU extension. Other sizes retain the decoder's allocation checks.

The follow-up inventory identified CRC32C as another frequent fallback.
A typed, SSA-classified CRC IR operation handles all eight CRC32/CRC32C
byte/halfword/word/doubleword forms. SSE4.2 supplies native Castagnoli CRC;
IEEE CRC and hosts without SSE4.2 call a generated constant-table backend,
without instruction decoding or interpreter entry. Both preserve NZCV, consume
only the specified input width and zero-extend the 32-bit result. The
portable call preserves the guest window register and live caller-saved values.

Semantics were checked against Arm's
[PMULL](https://www.scs.stanford.edu/~zyedidia/arm64/pmull_advsimd.html) and
[CRC32C](https://www.scs.stanford.edu/~zyedidia/arm64/crc32c.html) pseudocode,
LLVM's [x86 CRC32C definitions](https://github.com/llvm/llvm-project/blob/main/clang/lib/Headers/crc32intrin.h)
and QEMU reference runs. Expanded fixtures pass 10,240 PMULL cases and 8,192
CRC cases, including register aliases, source identity, widths, CRC
polynomials and CRC NZCV/upper-bit behavior. Both fixtures run without any
interpreter entries in the native JIT; the CRC fixture also passes with
SSE4.2 detection disabled in a temporary build.

The final 25-second game capture contained 9,451 samples and zero lost
samples. `jit_interp_step` accounted for 4.45% inclusive sampled user cycles;
`Emulator::step` including direct interpreter dispatch accounted for 6.18%.
Remaining work includes packed FP rounding conversions (notably FCVTNU) and
memory/fault paths; the measured PMULL/CRC encodings were absent from the
fallback inventory. An earlier PMULL-only window measured 2.29% inclusive
`jit_interp_step`; these different workload phases must not be presented as
a controlled speedup comparison. Tracing/inventory add overhead, and the
capture does not establish game FPS, completed login or gameplay. Preserve
exceptional FP and precise-memory fallbacks when reducing this further.

## Inline pixel-conversion helpers (2026-10-15)

`SIMD_NATIVE_EXTRA` now inlines SSHLL/USHLL (including SXTL/UXTL), SHLL,
SHRN and their upper-half forms with SSE2 unpack, shift and pack sequences.
FCVTL/FCVTN between single and double precision use SSE2; half/single
conversions use F16C only after CPUID and AVX OS-state checks. Older hosts
keep the specialized C++ half-conversion helper. Inputs remain intact until
the result commits, including aliased narrowing destinations whose low half
must be preserved. These are the conversion families implicated in the
earlier SpongeBob outfit-repacking investigation.

FP conversion NaNs take the existing guarded ARM-semantic step before any
architectural write, preserving payloads and FPCR.DN. Ordinary values stay
inline. This retains the existing native FP contract: it does not add full
FPCR rounding/flush controls, cumulative FP exception flags or enabled traps.
F16C narrowing explicitly selects nearest-even. Semantics were checked against
Arm's [SSHLL](https://www.scs.stanford.edu/~zyedidia/arm64/sshll_advsimd.html),
[SHLL](https://www.scs.stanford.edu/~zyedidia/arm64/shll_advsimd.html),
[SHRN](https://www.scs.stanford.edu/~zyedidia/arm64/shrn_advsimd.html),
[FCVTL](https://www.scs.stanford.edu/~zyedidia/arm64/fcvtl_advsimd.html) and
[FCVTN](https://www.scs.stanford.edu/~zyedidia/arm64/fcvtn_advsimd.html)
pseudocode; LLVM's
[F16C intrinsics](https://github.com/llvm/llvm-project/blob/main/clang/lib/Headers/f16cintrin.h)
document the host conversion and rounding controls.

The vector-cache scan now admits inline native SIMD bodies confined to
XMM0-2 and 32-bit SCVTF/UCVTF. Shared vector loads/stores read/write pins
when active, and the scan records raw instruction operands and the complete
loop write set. NaN recovery and all exits use existing writeback/reload
contracts. USHL/SSHL's inline body still borrows XMM3-5 and is excluded;
helper-called words, FP-to-integer conversions and more than thirteen distinct
vectors retain the uncached path. Scalar FP and vector layouts remain separate.
The subsequent Tier-1 allocator below supersedes these block-wide pressure
and helper exclusions for ordinary Tier-1 blocks.

Five alternating measured runs per configuration, after one warm-up each,
of a temporary two-million-iteration FCVTL/USHLL/UCVTF/FADD loop gave median
thread CPU times of 99.042 ms before and 4.181 ms after (23.7x throughput).
The new binary with vector caching disabled took 8.895 ms. All output bits
matched an independent QEMU run. These measure this synthetic conversion mix,
not a game speedup, outfit-change duration or GC pause reduction. The heap
alignment issue is outside this change. See [validation](archive/TESTS-history.md#inline-pixel-conversion-helpers-2026-10-15).

## Cached integer SIMD (2026-10-15)

The SpongeBob hat-change capture exposed a cost in the allocator's local
boundaries: a sampled Unity block retained only four vectors but synchronized
them around twenty-one operations. Ordinary integer arithmetic, comparisons
and immediate shifts still used architectural memory. Enabling caching around
those emitters increased their loads/stores even without register pressure.

Supported `SIMD_ARITH` and `SIMD_CMP` forms now consume allocated XMM operands
and commit directly to their allocated destinations. Immediate shifts use the
allocated registers; byte shifts, accumulating/rounding shifts and shift-insert
forms use only XMM0-2 as scratch and commit after reading every input. The
operand scan includes the old destination for accumulating and insert forms.
Aliased inputs, signed/unsigned comparisons, Q=0 upper-half clearing and
existing SSE4.1/SSE4.2 guards are preserved. Other hosts keep the existing
uncached paths. The shared effect scan also admits these emitters in Tier-2's
existing fixed region layout; region allocation and promotion policy are
unchanged.

Shift behavior was checked against Arm's [USHR](https://www.scs.stanford.edu/~zyedidia/arm64/ushr_advsimd.html),
[SRSRA](https://www.scs.stanford.edu/~zyedidia/arm64/srsra_advsimd.html) and
[SLI](https://www.scs.stanford.edu/~zyedidia/arm64/sli_advsimd.html) pseudocode.
The [regressions](archive/TESTS-history.md#cached-integer-simd-2026-10-15) check the changed
register paths against the interpreter, with independent QEMU guest runs.

Replaying the captured 31-instruction Unity block at guest PC `0x736b0c929c`
through each compiler reduced synchronization boundaries from 21 to 7,
emitted bytes from 4,013 to 3,128 and static vector-file memory instructions
from 358 to 262 (including cold guard/recovery code). The original live
perf-map entry occupied 4,008 bytes; replayed layout is not byte-identical to
the surrounding live code cache. The first USHR alone fell from 148 to 10
bytes, with no cache spill or reload. Remaining boundaries include FP-to-int
conversion, bit-select helpers, narrowing and scalar/structured stores.

Both versions of the complete block match QEMU and the interpreter for the
temporary harness's vector inputs, architectural NZCV and output memory.
Five alternating measured one-million-call runs after one warm-up each gave
median thread CPU times of 681.240 ms before and 91.641 ms after (7.43x
throughput). This is repeated execution of one captured block with controlled
inputs, not a measured hat-change speedup. It does not cover real texture
inputs, surrounding blocks, frame pacing or GC pauses. Scratch traces,
disassembly, the QEMU harness and timing data remain under `/tmp`.

## Tier-1 vector allocation under pressure (2026-10-15)

Tier-1 no longer disables vector retention just because more than thirteen
guest vectors are used or an incompatible operation appears in the block.
A shared operand/effect scan records vector reads, writes and next-use lists.
Blocks that fit retain the existing complete XMM3-15 layout. Larger blocks
rank vectors by use frequency and keep a stable loop core; the remaining
XMMs are allocated on demand. The core size is chosen from the actual
simultaneous operands, rather than a fixed split: a broadcast arithmetic
block can retain twelve vectors, while a multi-register operation can
reserve more demand slots. Instruction operands are protected from eviction;
other demand values are evicted by farthest next use, preferring clean
values on a tie. Dirty architectural values are committed even when their
next block-local use is absent.

Compatible arithmetic therefore keeps register operands under pressure.
Unknown scalar/vector effects and helpers form local synchronization
boundaries: publish the vectors, emit with architectural memory authoritative,
drop demand mappings, then reload the fixed core. Later compatible operations
resume allocation. This does not assume helpers preserve XMM registers or
guess which architectural vectors an unknown operation changes. The
[QEMU helper contract](https://www.qemu.org/docs/master/devel/tcg-ops.html#helpers)
provides the same conservative state-publication precedent; this is not a
claim that the two allocators have identical policies.

Retained ST/MT loop backedges publish only the demand-allocated tail; the
fixed core remains resident. Compile-time mappings stay intact for emitting
the alternate exits. Deferred memory guards now snapshot vector homes,
owners, resident order, active state and dirty bits, so an XMM reused later
cannot corrupt fault recovery. Tier-1 vector NaN steps are also emitted
after the hot code with the captured layout and a continuation after the
native commit. The cold step still sees original aliased inputs, publishes
loop-carried writes and reloads the captured layout.

This does not replace scalar SSA or make vector allocation global across
arbitrary control flow. Leaf-inlined conditional skips retain their complete
fixed-layout requirement at joins; pressure or incompatible effects in such
a block still decline vector caching. Tier-2 uses the shared effect scan
but retains its existing fixed region layout, internal-edge contracts and
inline NaN steps. Region optimization, publication and measured promotion
policy are unchanged. The FMA3/AVX feature gate and cache opt-out remain.

Five alternating measured runs per configuration after one warm-up each,
using two-million-iteration loops and QEMU-checked results, measured:

- Fourteen accumulators updated twice per iteration with FMLA: median
  thread CPU time 44.051 ms before, 37.208 ms after (18.4% higher throughput).
  The final binary with vector caching disabled took 44.590 ms.
- The equivalent simple FADD loop: 34.400 ms before, 36.102 ms after
  (4.9% more CPU time); cache-disabled final binary 34.230 ms.

These show a workload-dependent tradeoff, not a universal speedup or a game
FPS result. Precise cold recovery still consumes generated code space, and
its new metadata/next-use planning has compile-time cost; application cache
size and loading-time benefits are not established here. Temporary benchmark
sources, JSON results and code dumps remain in `/tmp`. See
[validation and scope](archive/TESTS-history.md#tier-1-vector-allocation-under-pressure-2026-10-15).

## Tier-1 hot-path code reduction (2026-10-14)

CoreMark profiles attributed 99.4% of samples to generated code; the
dispatch microbenchmark attributed 96%. This batch reduces that code:

- Folded `IMM` operations clear obsolete source/auxiliary operands before
  liveness analysis, allowing dead MOVZ/MOVK intermediates to disappear.
- A scratch ALU result consumed immediately by `ZEXT 32` uses the x86
  32-bit ADD/SUB/AND/OR/XOR/IMUL form. SEXT 8/16 similarly uses MOVSX r32.
  Operand setup keeps its normal width; only the operation encoding changes.
  A following extension needs no instruction when it retains the same host
  register. Tier-2 retains its existing extension path.
- Guarded scalar accesses retain bounds and the sticky software-check test
  inline, with successful native accesses falling through. Tier-1 emits
  page checks and recoverable fault exits after the block, using captured
  GPR mappings, dirtiness and vector dirtiness. Fault-site offsets are fixed
  before code publication. Tier-2 retains inline cold paths.
- Final/taken exits write architectural GPRs without spilling dead scratch
  values; taken-path emission retains allocator state for fall-through.
- Unpatched ST chain-skip slots use one five-byte NOP (`0F 1F 44 00 00`).
  Verification, invalidation and trial revocation restore the whole slot.
  Ordinary frames keep their return slot; MT keeps immutable instructions
  and atomic target cells.
- An existing native entry whose bounded tail branches back to the current
  head no longer forces a split. Cached branch metadata is only a growth
  hint: normal translation fetches and records every word and retains all
  instruction, fallback and IR/vreg limits. This avoids speculative guest
  reads and permits the existing self-loop emitter to retain loop state.

Three interleaved 110,000-iteration CoreMark runs per binary, after one
warm-up each on the final corrected binary, measured medians of 9,049.03
iterations/s for the saved pre-batch binary and 9,893.87 iterations/s after
this batch (9.3% higher).
Every run exceeded ten seconds and validated all CRCs. This measures the
combined batch on one host, not each change separately or game FPS. See
[validation and limits](archive/TESTS-history.md#tier-1-hot-path-code-reduction-2026-10-14).

### Dead-code compaction accounting follow-up

The broader Android service probe exposed a `vector::reserve` exception in
`optimize_ir`: DSE counted stores when marking them NOP, then DCE counted
their physical removal again. Clearing folded IMM operands removed more
dead producers, so subtracting the diagnostic removal count from the input
size underflowed. Compaction now reserves its actual retained count and
counts each physical removal once. Repeated optimization also uses the
current pass's retained count, independently of cumulative statistics.
This changes allocation/accounting, not instruction semantics or the
retained IR. The regression and Android validation are recorded in
[TESTS.md](archive/TESTS-history.md#dead-code-compaction-accounting-2026-10-14).

## Azur Lane generated-code audit (2026-10-14)

A 20-second GPU-enabled startup capture with JIT symbols recorded 15,470
samples and zero lost samples. Generated guest code accounted for 78.34% of
sampled cycles; guest `libil2cpp.so` accounted for 52.49% and Unity for 10.00%.
Inclusive block translation was about 5.38% and interpreter fallback 3.46%.
These categories overlap and are not additive. The capture preceded the later
login scene's regular UI delays, so it does not identify their cause.

The corrected replay inspected 24 sampled blocks, about 12.13% of sampled
cycles, using the same MT compiler and each library's ELF PT_LOAD mappings.
File offsets must be translated through `p_vaddr + offset - p_offset` before
deriving guest load bias. An initial replay omitted this conversion and
misattributed a 353-byte single conditional branch to the hot guest PC
`0x7353ea1908`. That example is withdrawn: the actual PC is a five-instruction
return sequence and its corrected replay emits 1,619 bytes. A replay does not
reconstruct all live module, cache or entry-order state. Complete emitted byte
counts include cold recovery paths and both branch exits.

Source and emitted-code findings, not implemented optimizations:

- Paired-memory function entry/return blocks at `0x735649f400` (0.35%) and
  `0x735649f4fc` (0.33%) contain seven/nine ARM instructions and emit
  3,457/3,372 bytes. LDP/STP retain repeated inline page checks and address
  spills/reloads instead of the streamlined scalar guard path. Any replacement
  must preserve the whole-instruction footprint and precise writeback state.
- CRIWARE's `0x74057f26dc` (0.77%) contains 32 ARM instructions, 29 SIMD FP
  arithmetic operations and 17 distinct vector registers; the replay emits
  6,239 bytes and 178 vector-state memory operations. An extra DUP-element
  helper and the cache's register ceiling disqualify all-or-nothing vector
  retention. Partial allocation requires explicit aliases, helper clobbers and
  NaN/fault recovery, rather than blindly enlarging the register whitelist.
- Atomic/LLSC/native-extra operations disable legacy forwarding for their
  complete blocks; unsupported SSA contracts also limit optimization. The
  sampled exclusive loop `0x7353eaaf50` (0.40%) and Unity acquire-load block
  `0x734c397984` (0.34%) expose CPU-state round trips around arithmetic.
  Optimizing segments requires modeled barriers, not removal of safeguards.
- `0x735649f688` (0.49%) emits consecutive MOVZX operations for a halfword
  load. The exclusive-loop block also uses ECX/CL plus count masking for a
  constant bitfield rotation. x86 immediate rotate forms are documented in
  [Intel's instruction reference](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-2b-manual.pdf).

No game speedup is established by these inspections. The estimates of 3–8%
overall improvement for the inspected blocks and 10–25% for broad recurring
patterns assume substantial reductions in their execution cost; they are
extrapolations, not benchmark results. Capture the delayed scene and measure
matched cold/warm frame times before attributing stutter or accepting a fix.

## Scalar FP scratch preservation (2026-10-13)

Scalar ADD/SUB/MUL/DIV, min/max, SQRT and register MOV use XMM scratch
without changing RAX. Their emitter previously evicted RAX's live GPR
mapping anyway. It now retains that mapping, avoiding unnecessary spill and
reload instructions in mixed integer/FP blocks. FNMUL/FABD and FABS/FNEG
still evict RAX before constructing their sign/absolute-value masks.
Guest flags still pass through `clobber_flags`; NaN/zero cold paths retain
the existing interpreter step, which commits and saves/restores live GPRs.
FP upper-lane canonicalization, memory checks and block boundaries are
unchanged. This does not require a new host ISA feature.

An interleaved mixed integer/scalar-FP loop measured 103.516 ms before and
93.049 ms after (15 measured samples per build, first trial of each process
excluded): 10.1% less loop time on this host. Its generated function shrank
from 3,627 to 3,593 bytes, including cold paths. A replay of the profiled
SpongeBob IL2CPP block shrank by seven bytes. These are scoped codegen and
loop observations, not a gameplay FPS result; see the
[validation and limits](archive/TESTS-history.md#scalar-fp-scratch-preservation-2026-10-13).

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

## Base ARMv8.0 FP/SIMD sweep fixes (2026-10-13)

The coverage sweep (`Scripts/Testing/simd_coverage.py`) found these defects;
each fixed group matches QEMU in both engines. Most were interpreter bugs that
the JIT inherited through CALL_INTERP.

- **Wrong results.** CMLE #0 ran as CMEQ #0 (its `sub2` case ignored U).
  JIT UQSUB `.4h/.8h` emitted PMINUB (`66 0F DA`) instead of PSUBUSW
  (`66 0F D9`). USQADD zero-extended Vn. Scalar FCVTMS ran as FCVTZS in
  both engines, because bit 23 (o2) was ignored. URECPE/URSQRTE ran as
  FCMGT/FCMGE #0. AESE ran as AESMC; AESD/AESMC/AESIMC never reached the
  crypto handler, and AESMC/AESIMC transformed Vd instead of Vn. Integer
  by-element `.s` reads Vm from M:Rm. TBL/TBX table registers are always 16
  bytes. Halving and register-shift ops use exact arithmetic (UHSUB of
  `0 - 3` is 254), and the shift amount is `SInt(Vm<7:0>)`. JIT ADDHN2 with
  64-bit lanes uses `pshufd 0x88`.
- **Missing base forms.** Vector and scalar FCVT{N,P,M,A}{S,U}, SQDMULH/
  SQRDMULH, SQDMLAL/SQDMLSL/SQDMULL{2} (vector, scalar, by element),
  SABA/UABA, PMUL, scalar FRECPX and FMAXNMP/FMINNMP. The scalar AdvSIMD
  forms whose pseudocode is the vector operation on one element (scalar
  SQADD..UQRSHL, SSHL/USHL/SRSHL/URSHL d, SQDMULH, FMULX, SUQADD/USQADD,
  SQABS/SQNEG, ABS/NEG d, SQXTN/UQXTN/SQXTUN, the doubling multiplies and
  the saturating shift-right-narrow forms) share one table,
  `kScalarLaneForms` in `FloatScalar.cpp`. Each row runs the vector
  encoding on scratch v0-v2, with element 0 holding the operands and other
  lanes zero, so they cannot saturate. Vd gets element 0 and the rest is
  cleared. The scratch registers are restored with RAII, even on SIGILL.
- **FPSR.QC.** The interpreter sets QC for the saturating add/sub, shifts,
  narrows, SUQADD/USQADD/SQABS/SQNEG and doubling multiplies. Native JIT
  SQADD/UQADD/SQSUB/UQSUB (B/H) and SQXTN/UQXTN/SQXTUN compare the result
  with the wrapped or re-widened value and set QC with
  `emit_simd_qc_unless_equal`. It uses PTEST, or PMOVMSKB with RAX saved,
  and changes no live GPR. Both paths were checked; the SSE2 one in a
  temporary build that forced `has_sse41()` off. FP cumulative exception
  flags (IOC/IXC/OFC) of conversions and FMULX are still not modelled.
- **Unallocated encodings now raise SIGILL in both engines.**
  `fp_decode::is_allocated_fp_scalar` encodes the scalar FP group for
  this CPU profile (AT_HWCAP advertises FP, ASIMD, CRC32 and atomics only).
  ftype 10 is reserved except FMOV X↔Vn.D[1]. FEAT_FP16 (ftype 11) keeps
  only the FCVT precision conversions, so FP16 scalar FMOV/SCVTF/FCVT*S,
  which ran with wrong results or only partly worked, now trap.
  `is_unallocated_advsimd_imm` rejects words with bits 23 and 10 set in the
  shift/by-element space. `is_unallocated_advsimd_three` holds the
  three-same and three-different per-opcode size tables, including no
  `.1d`. Handlers reject reserved sizes and fields: compares `.1d`, EXT
  bit 15 and `imm4 >= 8` with Q=0, INS (element) with Q=0, CNT with size 1,
  and SHRN/SSHLL with immh 1xxx. Shift rows in `simd_dp.txt` fix bit 23
  and require Q for 64-bit lanes.

The FMADD-family words in the sweep's "interpreted" bucket are an artifact of
its NaN-heavy inputs: they take the documented NaN cold step. A loop of finite
FMADD/FNMSUB records no fallback, and one with a NaN addend records it only
there. By-element SQDMULH/SQRDMULH/SQDML*L and FMULX remain CALL_INTERP.
`jit_simd_base_forms` covers 139 forms and 30 reserved encodings against QEMU
(see [TESTS.md](TESTS.md)). References: QEMU's
[estimate and FRECPX helpers](https://github.com/qemu/qemu/blob/master/target/arm/tcg/vfp_helper.c)
and [helper-a64.c](https://github.com/qemu/qemu/blob/master/target/arm/tcg/helper-a64.c);
Arm [URECPE](https://www.scs.stanford.edu/~zyedidia/arm64/urecpe_advsimd.html).
The x86 saturating opcodes were checked with the host assembler.

## Window limits in the header page (2026-10-12)

Every native load/store bounds-checked its address with `movabs $limit, %reg`
(10 bytes and a scratch register) plus `cmp`. `Memory` now writes
`window_size - width` for widths 1..64 into the readable header page below the
guest window, next to the sticky software-check flag, and
`emit_window_limit_cmp` emits `cmp %idx, disp32(%r10)` instead. Scalar loads and
stores and the guarded native path use it; widths without an entry, and callers
whose `R10` is not the window base, keep `emit_window_limit`. The window size is
fixed when the reservation is created, so the table never changes. CoreMark's
median rose from 8,885 to 9,039 iterations/s (110,000 iterations, three runs,
CRCs valid). `jit_window_bounds` checks that accesses past the window, including
offsets wrapping past zero, still fault with the right `si_addr` for every width.

## Single-instruction extensions (2026-10-12)

`SEXT` 8/16/32 now emits `MOVSX`/`MOVSXD` and `ZEXT` 8/16 emits `MOVZX`
(32 stays `MOV r32`), each straight from source to destination. They replace
a copy plus a `SHL`/`SAR` pair or an `AND` with an immediate and, unlike those,
leave RFLAGS intact, so lazy NZCV no longer has to be materialized around
them. Byte sources always carry a REX prefix, so registers 4-7 encode as
SPL..DIL rather than AH..BH. Other widths keep the shift/mask forms. The
folding pass also turns `AND` with `0xFF`, `0xFFFF` or `0xFFFFFFFF` (UXTB,
UXTH and UXTW operands) into `ZEXT`. A signed 16-bit load into a W register
went from four host instructions to two. CoreMark's median rose from 8,763 to
8,885 iterations/s (110,000 iterations, three runs, CRCs valid).

## Register moves through XZR (2026-10-12)

ARM's `MOV Xd, Xm` is `ORR Xd, XZR, Xm`, so its zero constant is the first
operand. The constant-folding pass only recognized identities (`x + 0`,
`x | 0`, `x ^ 0`, `x & ~0`, `x * 1`, `x & 0`) with the constant second, and
every register move compiled to a zero materialization plus an OR. Commutative
operations now swap a lone constant into src2 first, so moves lower to a copy
that later stores forward directly. W forms keep their separate `ZEXT`, which
clears bits 63:32. CoreMark (110,000 iterations, three runs, CRCs valid) rose
from a median of 8,565 to 8,763 iterations/s. `jit_mov_alias` checks MOV,
ORR/EOR/ORN/AND with XZR/WZR, shifted and rotated sources, W-form clearing and
destination aliasing against fixed results that also pass under QEMU.

## Integer ADDP for every element size (2026-10-12)

A SpongeBob gameplay fallback profile recorded 17.2 million interpreted
instructions in about six minutes; 10.5 million were one `ADDP v.2d`
(`0x4ee6bcc6`). Native integer `ADDP` was byte-only. `SIMD_ADDP` now carries
the element size in `width` and lowers every valid form with SSE2: halfword
sums add each dword's halves, sign-extend the low 16 bits and pack with
`PACKSSDW` (no saturation after sign extension); words use `SHUFPS 0x88/0xDD`
plus `PADDD`; doublewords use `PUNPCKLQDQ/PUNPCKHQDQ` plus `PADDQ`. Q=0 forms
first merge the low halves with `PUNPCKLQDQ`, then pair within that register
and zero `v_hi`. Both sources are loaded before Vd is written. The classifier
excludes the reserved `.1d` form, which keeps raising the interpreter's decode
error. Reference: Arm [ADDP (vector)](https://www.scs.stanford.edu/~zyedidia/arm64/addp_advsimd_vec.html).

## Eight-block superblocks and region vector state (2026-10-12)

Experimental Tier-2 now collects hot paths of at most eight blocks and 256
guest instructions. `BIFROST_JIT_SUPERBLOCK_LIMIT=2..8` selects a smaller cap;
the per-block IR/vreg budgets and region's 4,096-vreg ceiling also apply.
Completed prefixes can end before an unsupported block. Non-head cycles and
paths through an existing region remain excluded. Linear paths are eligible
alongside closed loops, with cross-block scalar SSA, precise side exits and
dependency tracking for every included code range. Tier-2 remains opt-in with
`BIFROST_TIER2=1`; single-thread automatic promotion also requires
`BIFROST_NO_WEX=1`, as before. MT uses immutable generated instructions and
revocable data cells.

`BIFROST_TIER2_TRACE=1` prints concise compiled-candidate and timing-decision
lines. Discovery attempts, per-block paths, admission declines, SSA/allocation
details and duplicate private-sampling code records require
`BIFROST_TIER2_TRACE_VERBOSE=1`, which also enables the concise lines.
`BIFROST_STATS_PERIOD=5` retains periodic compilation/payoff counters; the
Tier-2 `regions` field is an interval count, while measured `keeps`/`rejects`
are cumulative. A compiled candidate is not necessarily a selected region.

Region codegen selects a vector or scalar-FP cache layout over its complete
IR. Cold/chain entries preload that layout; internal edges and ST backedges
retain XMM values. Every external exit, timing-quota exit and memory-fault
recovery commits the region-wide pinned write set. A side exit before the
current iteration's first write must also publish values carried from later
writes on the preceding iteration. Deferred exit emission leaves compile-time
vector dirtiness intact. Existing NaN helper calls reload the pinned layout.
Cache-incompatible mixes and paths using more than thirteen distinct vectors
retain the memory implementation; scalar FP retains its existing partial
pinning policy. This change does not unify scalar/vector caches or extend
the native FPCR/FPSR contract.

Closed-loop timing still measures whole-iteration windows. Linear superblocks
start each arm with enough completed live traversals for at least 128 guest
instructions, growing the quota when baseline time is below 10 us. They exclude
unrelated guest execution between visits. Both private sampling graphs have matching
work probes and timer overhead. Work counts, block fingerprints and exit PCs
must match; eight pairs need a median ratio below 0.97 and six winning pairs.
Invalid/short samples retain Tier-1. Linear sampling is additionally bounded
to 8,192 entries per trial, including CPU-local observation-slot collisions.
Per-traversal timer overhead can hide gains in very short paths, so these
decisions remain conservative rather than guaranteeing application speedup.

A measured losing candidate now re-arms a smaller prefix at the same head:
five-to-eight blocks retry at four, and three-to-four retry at two. A losing
two-block candidate stays in Tier-1. Each retry retires the old head and its
Tier-1 constituents, revokes their sample graph and clears their branch
profiles; fresh translations must warm their entry counters before another
trial. This bounds one search to at most three sizes (normally 8 -> 4 -> 2).
Insufficient or mismatched samples do not trigger size retries. The existing
32 MiB code and two-second compiler budgets apply to every size. A selected
region's phase retirement resets the size preference, while the existing
per-head phase/terminal-rejection budget remains in force. Head invalidation
and full cache clears also discard the size preference.

Region destination reuse now preserves virtual homes defined in a loop's
preheader. A value's last use in one body is not its last use across iterations:
destructively redefining its virtual register can overwrite its invariant
spill slot or pinned register. The saved pre-change binary reproduced a
single-thread zstd crash with Tier-2 enabled; excluding these homes repairs
that workload. The discovery oracle checks the immutable-home contract and
twelve invariant/accumulator pairs under register pressure.

Five interleaved cache-on/off runs of two-million-iteration generated regions
measured median throughput gains of 4.05x (2D vector ADD), 1.96x (2S vector
ADD), 2.45x (scalar D ADD), and 2.40x (scalar S ADD), with independent final
value checks. Fixed-iteration CoreMark medians were 15.8101 s for the saved
baseline, 15.8224 s with an eight-block cap, and 15.5748 s with four blocks.
Each used one warm-up and three measured 110,000-iteration runs with all CRCs
validated. Eight blocks were approximately neutral on CoreMark; the small
four-block difference is not a general performance guarantee. These results
do not establish game FPS. See [validation](archive/TESTS-history.md#eight-block-superblocks-and-region-vector-state-2026-10-12).

## Native acquire loads and bounded trial timing (2026-10-12)

Aligned `LDARB/LDARH/LDAR W/LDAR X` now reuse the guarded native-load emitter.
The optimizer retains their original LL/SC IR operation as a memory-effect
barrier; only code generation lowers the non-exclusive load to an x86 load.
Acquire-only blocks initialize the direct-window base at cold and chain
entries. Bounds, sticky permission/userfaultfd checks, fault-site registration
and precise architectural recovery use the existing memory path. Unaligned
accesses exit to the existing interpreter implementation; forced-software
guard/no-fault-recovery configurations retain the helper implementation.
Exclusive loads/stores use the shared exclusive monitor (see "Native exclusive
loads and stores", 2026-10-20).

The mapping follows Linux's reference implementations of
[arm64 acquire loads](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/include/asm/barrier.h)
and [x86 acquire ordering](https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/asm/barrier.h).
This source check supports the aligned acquire mapping; it does not extend
the existing unaligned atomic contract or prove arbitrary concurrent access
patterns correct. Widths, aliases, NZCV, XZR, reservation preservation and
precise SIGSEGV/SIGBUS state have focused executable checks against QEMU.

Linear trial quotas now begin at `ceil(128 / guest_instructions)` traversals
(at least one). The existing short-baseline rule doubles the quota up to
4,096; CPU-time clocks, matched work/fingerprints/exits, eight pairs and six
wins remain required. The global linear-entry ceiling is 8,192 instead of
524,288, limiting timer calls when profiles or CPU-local slots do not produce
comparable windows. Inconclusive trials retain Tier-1. Closed-loop sampling
retains its initial 256-iteration windows.

Five alternating before/after runs of a two-million-iteration aligned acquire
loop, with Tier-2 disabled and identical guard settings, reduce median loop
time from 32.4066 ms to 2.4128 ms (13.43x). Each run validates its independent
final sum. This isolates native acquire-load overhead; it is not a CoreMark
or FPS result. See [validation and remaining gaps](archive/TESTS-history.md#native-acquire-loads-and-trial-timing-2026-10-12).

## Indirect branches and host return boundaries (2026-10-12)

With the default chain-skip frame layout, Tier-1 register-branch exits
(`BR`, `BLR`, `RET`) probe a per-host-thread indirect-branch lookup table
(IBL). Its 4,096 direct-mapped entries hold a guest PC and the cached block's
chain-skip entry. Dispatch populates entries; a cold target or hash collision
returns to dispatch. Ordinary frames (`BIFROST_NO_CHAIN_SKIP=1`) and
`BIFROST_NO_CHAIN=1` retain the previous indirect exits.

Generated probes check the JIT's dispatch generation, `jit_stop_pc`, nested
`jit_call_depth` and the execution bound (the shared 256-edge budget in ST, the preemption epoch in MT) before jumping.
Invalidation and Tier-2 publication advance the generation, revoking stale
entries. The table belongs to the host thread and is rebound at native dispatch
and helper entry, including for cloned CPUs. Timed Tier-2 trials unbind it to
keep execution inside their private sampling graph. Differential replay also
unbinds it for the measured block: unpatching static edges alone cannot prevent
a warmed indirect target from executing extra instructions or stores outside
the reference instruction count and memory snapshot.

The signal-return trampoline in `Source/System/Core/Signals.cpp` is executable
guest code (`mov x8,#139; svc #0` at `TRAMPOLINE_ADDR`), rather than a host
callback exit sentinel. `deliver_signal` installs the handler's PC and LR and
returns; the normal guest dispatcher runs it. A handler's RET may therefore
chain into a warmed trampoline. Its SVC terminates the block through an
unchainable exit, reloads the PC restored by `rt_sigreturn` and returns to
dispatch before executing that continuation. This matches Linux's
[AArch64 signal restoration](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/signal.c)
and [vDSO trampoline](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/kernel/vdso/sigreturn.S).
Host thunk and loader callback loops in `ThunkWiring.cpp` and `ElfLoading.cpp`
use interpreter `step()` with LR `0x1000`; the JIT SDL thread runner explicitly
sets `jit_stop_pc`. A future host runner returning to real guest code must set
that stop boundary rather than rely on the target being absent from the IBL.

`jit_mt_publication.cpp` checks warmed trampoline hits and SVC cold exits in
ST/MT, plus executable stop PCs and nested helper boundaries, in both frame
layouts. `jit_bitfield_regalloc` reproduces the verifier escape when IBL remains
bound. See [the validation record](TESTS.md#indirect-branches-watchdogs-and-lazy-controllers-2026-10-12).

Hang detection is now an opt-in diagnostic (`BIFROST_HANG_WATCHDOG=1`): it
enables the main/worker same-PC limits, main frozen-loop detection and JIT
dispatch/call-helper limits that can stop or demote valid long-running code.
This does not disable the chain preemption bound, host call-depth limit, guest
memory checks or explicit instruction limits. `test_long_call_loop` checks
60 million calls on each of the main and worker CPUs against an inline oracle.

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

## Half-precision conversions and ARM NaN results (2026-10-12)

The JIT runs these through `CALL_INTERP`, so both engines shared each defect.
SpongeBob's Spine outfit repack widens half-float pixels with vector `FCVTL`;
the interpreter implemented only the single-to-double form and sent
`FCVTL .4s, .4h` (`0x0E217800`) to an unrelated handler. Fixed in the
interpreter, following Arm's FPConvert/FPRoundInt with FPCR defaults:

- `FCVTL/FCVTL2` from halves and `FCVTN/FCVTN2` to halves (Q=0 zeroes the top
  half, FCVTN2 keeps the low half). FCVTXN has no single-to-half form, so its
  sz=0 encoding raises the decode error.
- `h2f`/`d2h`/`f2h` (`FloatCommon.hpp`): exact widening including subnormals
  (they had been half their value), one round-to-nearest-even step for
  narrowing (they truncated, and double inputs rounded twice), overflow to
  infinity, and quieted NaNs keeping their payload (NaN had become infinity).
  `FPCR.DN` selects the default NaN; `FPCR.RMode`, `FPCR.AHP` and FPSR flags
  are not modeled.
- Scalar `FCVT Sd, Hn` and `FCVT Dd, Hn` were matched with unallocated
  encodings (`0x1E634000`, `0x1E224000`); the real `0x1EE24000`/`0x1EE2C000`
  ran as no-ops. Unknown scalar FP encodings now raise the decode error
  instead of the former `[FP-NOP]` log-and-skip.
- Vector `FRINT*` quiets signalling NaNs (host libm returned them unchanged);
  scalar `FADDP/FMAXP/FMINP` use `arm_fp_binop`, so invalid sums give +qNaN
  and NaN operands propagate in ARM order instead of x86's.

The instructions came from a fallback profile diffed across one equip.
`BIFROST_FALLBACK_PROFILE` now expands `%p` to the host PID: an Android app's
game, WebView and service processes otherwise overwrite one file. A throwaway
fixture covering every interpreted instruction in that window (FP min/max,
pairwise, rounding, conversions, widening adds, byte atomics, CASAH) then
matched QEMU 11.1 bit for bit in both engines. References: Arm
[FCVTL](https://www.scs.stanford.edu/~zyedidia/arm64/fcvtl_advsimd.html),
[FCVTN](https://www.scs.stanford.edu/~zyedidia/arm64/fcvtn_advsimd.html) and
[FCVT](https://www.scs.stanford.edu/~zyedidia/arm64/fcvt_float.html).

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
option. Translation emits the side-effecting `MEMORY_BARRIER` IR operation;
the JIT emits MFENCE without disturbing live GPRs, vectors or NZCV. The
interpreter executes a sequentially consistent host fence. x86 TSO alone is
insufficient: it permits a younger load to bypass an older store.

All supported DMB/DSB options conservatively use this full fence. This covers
ordering of ordinary guest memory; it does not establish device-memory or
privileged cache/TLB semantics. ISB and the remaining hints retain their existing
behavior. References: Arm [DMB](https://www.scs.stanford.edu/~zyedidia/arm64/dmb.html)
and [DSB](https://www.scs.stanford.edu/~zyedidia/arm64/dsb.html).

`test_atomic_stress` includes two-worker store-buffer tests for DMB/DSB ISH/SY,
100,000 rounds each, plus GPR/NZCV preservation across additional options.
An otherwise identical binary with the fences disabled observed forbidden
both-zero results in all four cases. The repaired JIT, interpreter and QEMU
observed zero; these bounded tests do not prove every multithreaded workload.

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

### Reciprocal estimates and refinement (SpongeBob, 2026-10-07)

The installed Unity library's first missing reciprocal step was
`libunity.so+0x4f6414`, `frecps v3.4s,v16.4s,v18.4s` (`0x4e32fe03`).
Disassembly identifies the complete sequence: FRECPE, FRECPS, FMUL,
FRECPS, FMUL. The RX mapping starts at ELF vaddr `0x4df000`, explaining
the crash report's anonymous-mapping-relative offset `0x17414`.
Unity contains 1,114 FRECPE, 3,607 FRECPS, 401 FRSQRTE and 1,125 FRSQRTS
instructions, in `.2s` and `.4s` forms. Static presence is not execution proof.

All four operations now share scalar S/D and vector `.2s`/`.4s`/`.2d`
interpreter semantics. JIT and Tier-2 use CALL_INTERP for this family.
The shared decoder predicate precedes scalar integer conversions: their
old mask discarded bit16 and misclassified scalar FRECPE/FRSQRTE as
SCVTF/UCVTF. Half-precision arithmetic and reserved Q=0,D forms trap;
FEAT_FP16, AFP and RPRES are not advertised.

FRECPS computes fused `2-a*b`; FRSQRTS computes fused `(3-a*b)/2`.
FRSQRTS halves an exactly scalable operand before FMA, preventing
intermediate overflow and a second rounding. Two tiny odd significands
use the directed-rounding result around 1.5. Source registers are
snapshotted before destination writes; scalar and Q=0 unused bits clear.
Infinity times zero returns 2 or 1.5 without invalid. NaN processing
negates the first operand's sign before selecting/quieting its payload.

FRECPE/FRSQRTE use Arm's specified eight-bit estimate, replacing full host
division/sqrt. FPCR rounding, default-NaN and flush-to-zero controls are
honored, and cumulative exceptions update FPSR while preserving the host
floating-point environment. Enabled synchronous FP exception traps remain
outside the emulator's existing FP exception model.

The contracts were checked against Arm's
[FRECPS](https://www.scs.stanford.edu/~zyedidia/arm64/frecps_advsimd.html),
[FRSQRTS](https://www.scs.stanford.edu/~zyedidia/arm64/frsqrts_advsimd.html),
[FRECPE](https://www.scs.stanford.edu/~zyedidia/arm64/frecpe_advsimd.html),
[FRSQRTE](https://www.scs.stanford.edu/~zyedidia/arm64/frsqrte_advsimd.html)
and their linked shared pseudocode. QEMU's
[fused-step helpers](https://github.com/qemu/qemu/blob/master/target/arm/tcg/helper-a64.c)
and [estimate helpers](https://github.com/qemu/qemu/blob/master/target/arm/tcg/vfp_helper.c)
provided an independent implementation comparison. `jit_simd_recip` checks
45 assertions, including result/FPSR corpus hashes from QEMU cortex-a53.

### SIMD logical fallback and Unity UI colors (2026-10-08)

The SSE2 memory path for `SIMD_LOGICAL` inverts its second operand with
`pcmpeqd xmm2,xmm2; pxor xmm1,xmm2`, then applies PAND, POR or PXOR for
BIC, ORN or EON. Use the register encoding helpers: the former literal
ModRM bytes `E2`/`E1` addressed XMM4 instead of the documented scratch
registers. Legacy PANDN computes `~destination & source`, so an all-ones
destination cannot be used to invert its source. The VEX cache path uses
VPANDN with the second Arm operand as its first source for BIC.
These operand rules were checked against Intel's
[instruction reference, PANDN](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf).

In the installed SpongeBob Unity library, the first wrong instruction in
the color packer at `libunity.so+0x4f4ab8` was the BIC at `+0x4f4ac8`.
Opaque white packed to `00000000` under JIT, versus `ffffffff` under the
interpreter and QEMU. UI meshes were submitted with zero vertex colors,
making the age dialog and ocean background invisible while bubbles rendered.
`Tests/Unit/jit_simd.c` reproduces that exact instruction sequence, checks six
packed colors including clamping, and checks 256 BIC/ORN cases across
both Q widths, destination aliases and equal operands. Run it with
`BIFROST_NO_VEC_CACHE=1` to force the repaired SSE2 fallback. These tests
do not directly exercise the EON IR suboperation.

The rebuilt game displayed the age dialog and ocean background without
the diagnostic color override, then advanced to its loading screen.
The real-GPU run presented 2,195 frames over 204 seconds, including
120 seconds after the age dialog appeared, with no reported guest failure.
Gameplay beyond the loading screen was not verified.

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
[the validation record](TESTS.md#flag-arithmetic-register-pressure-2026-10-11)
for the independent value/flag checks and timing bounds.

The default block instruction limit remains 32. `BIFROST_JIT_BLOCK_LIMIT=8..128`
allows bounded comparisons; both Tier-1 and the Tier-2 collector stop growth
at the instruction cap or the shared IR/vreg complexity budget. Long blocks
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
The sticky software-check flag is read on every access; another guest CPU may
enable tracking while a block executes. The result register is separate from
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

## Optimization checkpoint (2026-10-11)

Three matched 60,000-iteration CoreMark runs per binary measured median
throughput of 2,689.86 iterations/s before the flag/guard changes and
4,278.69 iterations/s after them: about 59.1% higher throughput. All runs
exceeded ten seconds and passed the seed, list, matrix, state and final CRC
checks. That comparison predates SSA and does not measure its benefit.
A separate same-binary Tier-1 SSA on/off comparison measured medians of
4,360.15 and 4,382.44 iterations/s respectively (about 0.5% lower with SSA).
Treat SSA's result as approximately neutral here. Unshared constants and
first-use input placement then turned SSA's CoreMark code-size change from
+207 bytes (243 blocks larger, 213 smaller) into -3,155 bytes (20 larger,
211 smaller), but medians stayed at 4,377.01 (SSA) and 4,397.22 (legacy)
iterations/s: code size was not the cause of the remaining difference. Neither comparison
remeasures the emulator's percentage of native CPU performance or establishes
a Tier-2 SSA speedup.

The differential gate passed 74 fixtures and six threaded oracle runs.
The SSA host oracle passed 21,574 assertions. After the final Tier-2 backedge
state fix, discovery, feedback and timing oracles passed all twelve combinations
of single/multithread execution and ordinary/chain-skip layout. Focused flags,
scalar guard and userfaultfd checks also passed with interpreter/reference
comparisons and forced software guards. These are bounded correctness tests,
not proof that arbitrary games are stable.

A bounded real-GPU voxel-game run continued presenting for roughly 90 seconds
without a reported guest failure; its swap-call count is not a gameplay FPS
benchmark. The user reported a noticeable improvement in dhewm3, followed by
intermittent freezes. The freezes remain unresolved: no failing PC, wait state
or controlled SSA on/off reproduction has been captured. See
[the Doom 3 status](Apps/doom3.md). Do not describe this checkpoint as a freeze fix.

## Native-code reduction checkpoint (2026-10-11, later)

CoreMark was 100% generated code under `BIFROST_PROF=1` (no dispatch,
translation or interpreter samples), so this pass removed host instructions
from hot blocks rather than dispatcher work. A four-instruction list-walk
block had been about 70 host instructions on its fast path.

- **Flag commit:** `emit_materialize_flags` is `lahf; seto al; and eax,
  0xC101; imul edx, eax, (1<<16)|(1<<21)|(1<<28); and edx, 0xF0000000`,
  plus one XOR for SUB's inverted C and the from_sub bit. Each of NZCV
  receives exactly one shifted copy and no lower copy can carry into
  them. RAX is saved around it; no PUSHFQ.
- **Flag load:** `emit_load_flags_from_pstate` builds the SAHF image, sets
  OF with `add al, 0x7F` (AL = V) and returns SUB convention directly,
  replacing its POPFQ and the separate CF-normalize helper (another
  PUSHFQ/POPFQ pair).
- **Guard flag:** the sticky software-check flag lives at
  `window_base - 8` in a readable header page before the guest
  reservation, so every native load/store tests it with one
  `cmp byte [r10-8], 0` instead of `movabs`, `movzx` and `test`.
- **Chain-skip by default:** chain edges skip the frame teardown and
  prologue. Enabling it exposed a latent bug in both layouts: a mid-block
  CALL_INTERP whose step changed the PC (SIGILL delivery) left through the
  chain slot, which ran the static successor instead of the handler. That
  exit now returns to the dispatcher (`jit_interp_exit`).

Same-binary CoreMark (auto-calibrated, it/s): 4,365 before; ~4,400 with the
guard flag; ~5,080 after the flag commit; ~5,420 after the flag load;
~7,400–7,550 with chain-skip on. QEMU 11.1 TCG measured 7,699 on the same
binary. The benchmark runner's fixed-iteration run (110,000) takes ~15.2 s.
The `bench_*` microbenchmarks moved within run-to-run noise except `sort`
(~8% faster) and `memcpy` (~0.78 s → ~0.59 s). The full suite (311),
`--no-chain-skip` and `--no-jit` quick suites, `make verify` and the Tier-2/MT
scripts pass. Games were not run in this pass, so application FPS is unmeasured.

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
  pstate (bit 27 is the JIT's carry-convention bit), x0-x7, sp and lr. A
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
ADCS/SBCS and CCMP still use `force_two_vregs_to(v, RAX, v, RCX)`.
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

Vector FMOV immediates are native (inline, see
[inline native SIMD kinds](#inline-native-simd-kinds-2026-10-13)). Scalar
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
sticky `Memory::jit_software_checks_` byte (at `window_base - 8`, tested as
`cmp byte [r10-8], 0`) selects the software permission path
after userfaultfd tracking starts or a W/X-only guest mapping is created. x86
cannot represent those guest read restrictions, and the emulated userfaultfd
missing bits do not change host page protection. Already compiled code observes
the flag without invalidation. Pair and multi-register accesses, exclusive
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

Vector FMUL by a scalar lane is native for 2S, 4S and 2D. The generated
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

Its first run found 605 classified encodings that QEMU rejects:
- shift-by-immediate rows missing bit 23;
- `.1d` forms of FCMEQ/FCMGE/SSHL/CMEQ #0/SCVTF/FCVTZS;
- double FMOV (immediate) without Q;
- ADDV `.2s`.

All 1,480 remaining encodings match QEMU in both engines. A
generated-versus-hand-written check over all 2^32 words showed no
classification change before the spec was corrected.

### Inline native SIMD kinds (2026-10-13)

`SIMD_NATIVE_EXTRA` codegen first tries `emit_native_simd_inline`
(`CodeGen/Native/NativeSIMDInline.cpp`) and calls `run<K>` only when it
declines. A helper call flushes every cached GPR, pushes `WIN_REG` and
counts as a call for Tier-2. In a SpongeBob steady-state profile, helper
bodies alone were 3.5% of sampled CPU. The inline kinds and their x86 forms:

- **EXT:** `psrldq`/`pslldq`/`por`. Q=0 packs Vn.8B and Vm.8B into one
  register first.
- **FCMEQ/FCMGE/FCMGT and the #0 forms:** `cmpps`/`cmppd` with ordered
  predicates (EQ_OQ, LT_OS, LE_OS), so NaN lanes compare false as on ARM.
  GE/GT swap operands. Generated code never sets MXCSR DAZ/FTZ, so
  denormal inputs compare as in the helper; FPCR.FZ is unmodelled in both.
- **CMEQ #0:** `pcmpeq{b,w,d}`. `.2d` uses `pcmpeqd`, a dword swap and
  `pand`, so it needs only SSE2.
- **REV64:** `pshufd`, or `pshuflw`/`pshufhw` plus a byte swap within words.
- **Vector FMOV (immediate):** the expanded constant via `emit_mask`
  (RAX only).
- **USHL/SSHL with 32- or 64-bit lanes:** needs AVX2 (`vpsllv`/`vpsrlv`).
  The count is Vm's low byte zero-extended, so a negative shift is
  >= 128 and shifts left to 0. The right count, `-b & 255`, is >= 129 for a
  positive shift. USHL is `left | right`. SSHL's right shift is
  `srl(n ^ sign, c) ^ sign`, which gives the sign for large counts, and it
  is selected only in lanes whose count byte is negative. 8/16-bit lanes and
  hosts without AVX2 keep the helper.
- **BSL/BIT/BIF:** `base ^ ((base ^ other) & mask)` with `pxor`/`pand`, after
  loading Vn, Vm and Vd.
- **DUP (element):** `pshufd` for 32/64-bit lanes; 8/16-bit lanes shift the
  lane to byte 0 (`psrldq`, `punpcklbw` for bytes), then `pshuflw`/`pshufd`.
- **ADDV:** shift-and-add halving with `psrldq` and `padd{b,w,d}` (Q=0
  clears the upper half first), then a byte-shift mask keeps lane 0.
- **MLA:** `pmullw` (16-bit), `pmulld` (32-bit, SSE4.1) or, for bytes, the
  low bytes of word products of the even and of the shifted odd bytes; then
  `padd` with Vd. MLS is not in this row.
- **FRINTN/M/P/Z:** SSE4.1 `roundps`/`roundpd` modes 0-3. A NaN lane takes
  the interpreter step (payloads, FPCR.DN). FRINTA/I/X keep the helper.

Every source is loaded before Vd is written, so aliased registers behave as
in the helper. Both caches already refuse blocks containing this op, so
`cpu.v_lo/v_hi` are current and XMM0-5 are scratch.
`native_simd_inlinable` is exact for the emitter, and
`plan_tier2_region` counts only helper-called words as calls.
`jit_native_simd_gen` runs the inline bodies against the QEMU hashes, and
mutating an inline predicate fails it. `jit_native_simd_inline` adds
every USHL/SSHL count byte and aliased Vd. A synthetic guest loop of EXT,
FCMGT, REV64, USHL, FMOV and ADD fell from 1,682 to 104 ms of CPU. That is
an upper bound for this op mix, not a game measurement.
`Scripts/Testing/simd_coverage.py` sweeps the whole FP/SIMD
data-processing group (2^19 words, Rd/Rn pinned) under QEMU and Bifrost in
about 20 seconds. It reports instructions Bifrost is missing, unallocated
encodings it accepts, differing results, and interpreted encodings, grouped
by mnemonic. Its first full run found a host SIGFPE: reserved DUP (element)
and INS (general) encodings divided by a zero element size in the
interpreter, crashing both engines.

Baseline after the [2026-10-13 fixes](#base-armv80-fpsimd-sweep-fixes-2026-10-13)
(the 2026-10-12 baseline had 23,806 missing, 38,403 accepted and 12,448
differing words):
- **Valid words:** 77,672 of 524,288 under QEMU.
- **Missing:** 25,651 words, none of them base ARMv8.0. All belong to
  features Bifrost does not advertise in AT_HWCAP (FP16, dot products,
  SHA3/SM3/SM4, RDM, FCMA, FHM, BF16, I8MM); FP16 scalar FP now traps.
- **Accepted unallocated:** 2,315 words: reserved sizes in two-register
  misc, permutes and FP by element.
- **Differing results:** 57 words, all FP16 or other unadvertised forms
  (for example FP16 fixed-point `scvtf h, h, #imm`, which should trap).
- **Interpreted:** 22,839 words: the NaN cold steps described above, the
  by-element saturating multiplies and other CALL_INTERP forms.
The sweep compares v2, x2 and NZCV only; FPSR is not part of its hash.

Across-lane reductions, widening three-different ops and
pairwise long adds are covered by `jit_simd_widen_reduce`. FP lane helpers
(FMAXP/FMINP/FMAXNMP/FMINNMP, FCMxx #0, all seven vector FRINT modes, and
half-precision FCVTL/FCVTN) call the shared `FloatCommon.hpp` semantics
(`arm_fp_binop`, `arm_frint`, `h2f`/`f2h`) that the interpreter uses.
`jit_simd_fp_lanes` checks them, together with scalar and vector FP
arithmetic under FPCR.DN, against per-encoding hashes generated by QEMU.

Byte shift-by-immediate (all nine SHL…SRSRA forms) has no SSE2 byte shift.
`emit_lane_shift` builds it from word shifts on the low and high bytes of
each word, using XMM1 (XMM2 when shifting XMM1) as the temp. Word counts
of 16 give 0 or the sign fill, which is the ARM full-byte result. Byte
forms stay on the SSE2 path even on AVX2 hosts.

`SIMD_NATIVE_STRUCT` terminates Tier-1 execution with the helper's dynamic
next PC, so a memory fault returns to the guest signal handler rather than
continuing through a static fall-through edge. Do not chain or fuse this
boundary into Tier-2 regions. Validate the full footprint before changing
vector registers or the post-index base, and snapshot aliased sources.

SDL thread callbacks set `CPU::jit_stop_pc` to their return sentinel. Nested
`jit_call_helper` dispatch must stop there, and generated call continuations
must unwind without reloading or writing stale cached caller state. Keep
this host boundary out of architectural CPU copies and retain the interpreter
runner for `--no-jit`. `test_sdl_thread` includes nonlocal return coverage.

Native BL/BLR execution retains at most 16 nested guest-call frames on the
host stack (`CPU::JIT_MAX_CALL_DEPTH`). Before another native call, commit
guest registers, vectors, LR and target PC, then request dispatcher execution
and unwind the existing native callers. Every direct-call continuation and
`jit_call_helper` must propagate that request without executing the caller's
guest continuation or restoring stale cached state. The outer dispatcher
clears the request once the depth reaches zero and resumes at the committed
target. This limits host stack use independently of guest recursion depth;
it does not change or enlarge the guest stack. The depth and request are
host-only fields, excluded from architectural CPU copies.

`jit_call_helpers 20000 deep` checks both recursive BL and indirect BLR
returns against their arithmetic result; the old unbounded implementation
overflowed the host stack on this fixture. Also run it with
`BIFROST_NO_DIRECT_CALL=1` to cover helper dispatch. These checks cover deep
calls, not arbitrary embedding stack sizes or concurrent mapping changes.

Separate known memory limitation: `Memory::read_access` and `Memory::write`
validate permissions before copying through the host reservation without
holding the mapping lock across the copy. Concurrent `mprotect`/unmap can
invalidate that range between the check and `memcpy`; a repeated protection
toggle against slow stores reproduces a host SIGSEGV. Registered native JIT
fault recovery does not cover those C++ copies. The call-depth fix does not
repair this race, and it was not the faulting operation in the captured
host-stack overflow.

Build the SDL/GL configuration, then run `bash Scripts/Testing/test_native_doom_gaps.sh`
for every retained Doom capture gap (625 encodings, 20,000 randomized cases).
`Tests/Unit/jit_doom_gap.elf` contains fixed QEMU-checked oracle cases and is also
registered in `Scripts/Testing/run_tests.sh`. A zero explicit IR fallback count alone
is insufficient: check actual generated execution and host capability gates.

## Dispatch cache and worker profiling (2026-10-04)

### Interpreter recovery reasons (2026-10-16)

`BIFROST_FALLBACK_PROFILE=/tmp/fallback-%p.tsv` retains the original six
inventory columns and appends `reason`: `general` for ordinary interpreter
entry and `fp-special` for a generated FP special-value recovery step.
The key includes the reason, so the same PC can have separate rows. The
classification describes the entry path: `fp-special` does not mean every
input was a NaN, and `general` does not by itself establish missing native
instruction support. The FP guards and interpreter semantics are unchanged.

The reason is scoped to one step and restored on nested calls and exceptions;
it is thread-local. No per-step scope is installed when inventory collection
is disabled. `jit_fp_interp_step` also gives native stack profilers a separate
entry from `jit_interp_step`. Use inventories with a sampling profile and
action markers; counts alone do not measure the duration or cause of a hitch.

The thread-local dispatch cache uses up to 8,192 two-way sets (16,384
entries, 512 KiB per dispatching thread). It starts at 256 sets and grows
fourfold after one eviction per set (see the 2026-10-24 section). It is
allocated on a thread's first dispatch and freed at thread exit (pthread key),
so host threads that never run guest code carry none of it. With the previous 1,024 sets, SpongeBob
gameplay sent 32% of dispatches (7.9M per 15 s, nearly all evictions) to the
900k-entry shared table, whose bucket walk and lock made `run_block` about
10% of sampled CPU; the larger cache's in-game effect is not yet measured.
Way 0 is most recently used; second-way hits move their target to the front.
Both native functions and null interpreter-fallback decisions are cached.
Owner/invalidation-generation checks revoke both ways before reuse. Tier-2
promotion and explicit invalidation use the same insertion/erasure helpers.

In shared multithread mode, existing call targets can be read under the
shared block-table lock. Compilation still rechecks under the exclusive lock.
That checkpoint preceded the MT publication implementation below. Code
reclamation remains deferred.
Range invalidation advances the dispatch generation only when a cached block
or an inlined guest-code range overlaps; data-only mapping changes retain
valid dispatch entries. Code bytes remain unrecycled during execution.

`BIFROST_JIT_DISPATCH_STATS=1` enables optional per-thread hit/eviction counters.
With `--verbose`, the code-cache summary includes `last`, `way0`, `way1`,
`table`, `evictions`, set count and associativity. Counters are batch-flushed
instead of atomically updated on every hit; worker snapshots can omit pending
counts. Owner changes discard pending diagnostic counts rather than assigning
them to another JIT. Counting is disabled by default.

`BIFROST_PROF=1` registers the stable reserved code-buffer range independently
on each host thread and JIT owner. Process signal/timer installation happens
once, and nested dispatch/translation guards restore their previous state.
The registered range covers later logical cache growth. The dispatch bucket
still includes native helpers called inside dispatch, and the sampled-PC
histogram remains local to the thread that reports it; this is not complete
helper/contention attribution.

`BIFROST_PERF_MAP=1` appends each published Tier-1 block to
`/tmp/perf-<pid>.map` (perf's JIT symbol-file format) as
`aarch64:<guest pc> @<host window address>`. The size runs to the emission
end at publication; Tier-2 regions are not listed, and a reused address after
a cache flush is named by its latest line. `perf record -p <pid>` then names
generated code, and the host-window address resolves through
`/proc/<pid>/maps` to the guest library and file offset. The file is not
removed at exit.

A [native Doom 3 capture](archive/apps/doom3-history.md#native-dispatch-profile-2026-10-12)
on October 12 resolves this distinction: native stacks in `jit_ldxr` account
for 11.1% of sampled user CPU, and Tier-2 trial timing for 6.57%, while
`run_block` itself accounts for 2.09% leaf time. The broad SIGPROF dispatch
percentage therefore must not be treated as pure block-lookup overhead.
These are loaded-save profile observations, not an FPS comparison; kernel
time and generated descendants are excluded from the native stack shares.

`Tests/Unit/jit_dispatch_cache.c` checks indirect calls on three guest threads,
synchronized executable-code replacement and data mmap/munmap churn against
an arithmetic oracle. `bash Scripts/Testing/test_jit_profiler.sh` deterministically
checks the sampler's actual range predicate for multiple host threads and
JIT owners, including addresses beyond the initial logical cache capacity.

## Guest-call helper overhead (2026-10-04)

Nested `jit_call_helper` returns no longer flush shared atomic counters on
every tiny call. A per-thread nesting guard publishes the remaining tail
when the outermost helper exits; long-running callees retain the existing
64K-block batch flush. The guard restores nesting on C++ exception unwind.
SDL callback/guest nonlocal-return checks retain their sentinel handling.

BL/BLR helper wrappers materialize guest NZCV before the call, invalidate
host flag tracking, and treat the callee's CPU state as authoritative. They
now use `emit_call_aligned(..., preserve_flags=false)`, avoiding redundant
PUSHFQ/POPFQ and their stack padding. Other native helpers retain flag
preservation by default. Call alignment, WIN_REG preservation, vector
writeback/reload and stop guards are unchanged. The ordinary BL helper path
also omits the duplicate copy of the helper's returned PC into CPU state,
matching the existing BLR path.

`jit_call_helpers` checks nested arithmetic and callee-written Z/C flags
across three guest threads. Optional `1000 single` arguments allow a clean
register/memory verification run with `BIFROST_NO_DIRECT_CALL=1` so the helper
wrapper is exercised even in a single-threaded process. The gate runs that mode for differential checks and separately runs the
original three-thread workload against its arithmetic/NZCV oracle. Concurrent
replay is suspended rather than restoring shared memory underneath workers.

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
[validation and measurement bounds](TESTS.md#bounded-mt-self-loops-2026-10-11).

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
taken/fall-through outcomes with one `inc qword [profile+off]` (64-bit, no check,
`lock`ed in MT) behind a `mov r11, imm64`.
With at least 16 samples, a taken successor with 75% bias is linearized by
inverting the typed branch and retaining the original fall-through as a cold
exit. Head back-edges remain loop terminators. Promotion requires all branches
to be sampled and a product of continuation frequencies of at least 75%.
This is a profitability heuristic, not a measured speedup; multiplying
marginal frequencies can underestimate correlated paths. MT single-block loops are declined
because they fuse no boundaries and cannot retain loop state across entries.
A profile whose block was promoted or invalidated is dropped from the lookup
table and its counters go unread; storage outlives published code. Monitored regions deposit a recovery credit
in a CPU-local slot on completed hot paths. This is one flag-neutral store,
including ST internal backedges; MT performs no shared atomic increment per
iteration. A cold exit tail-calls feedback after flushing guest state and
tearing down the generated frame. It consumes a recovery credit and reduces
pressure, or increases pressure if no completed path intervened. Pressure of
64 triggers retraining. This is a conservative recovery/miss heuristic, not
an estimate of a cold-exit probability. Sixteen CPU-local slots bound metadata;
colliding regions can delay detection by depositing extra credits or changing
the slot's owner, but cannot add pressure to another region.

Retirement holds one compiler-lock critical section for version validation,
exact victim selection and revocation. It retires the head and overlapping
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
trace's block boundaries, without calls or retained self-loop frames. Branch
profiles choose candidate paths; code size no longer predicts execution payoff.
Each candidate enters a bounded A/B trial before permanent selection. The
retained Tier-1 graph and candidate run alternate windows on one vCPU's live
workload, without replaying memory writes or restoring architectural state.

Both arms execute private instrumented copies with identical work probes at
matching guest block boundaries. Normal Tier-1 blocks and selected regions
contain no work probes or inactive probe guards. The selected ST region also
omits sampling quota checks on its internal backedges. CPU-local counters
record guest instruction count and a 64-bit block fingerprint; probes preserve
flags and scratch registers.

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

Windows initially contain 256 completed iterations. A short baseline window
increases the CPU-local quota up to 4096 to amortize timing overhead. The
clock is CLOCK_THREAD_CPUTIME_ID, which measures CPU time consumed by the
calling host thread, excluding time spent descheduled. Only complete windows
ending at the head are sampled. A baseline must consume at least 10 us; a
candidate must match its vCPU's instruction count, fingerprint and exit PC.
Eight comparable pairs must show a median candidate/baseline ratio below 0.97,
with at least six pairs below 0.97, to select the region. Slower or noisy
candidates restore Tier-1; a measured loss can schedule a smaller prefix after
fresh profiling, following the bounded size search described above. A trial
with insufficient evidence after 128 attempts also restores Tier-1. Version
checks and normal revocation protect
in-flight references across mapping changes and ST-to-MT transition.

The decision measures instrumented execution gain, not lifetime amortization
of compilation or a guarantee of whole-application speedup. Sampling adds
short-lived execution cost and retained private code; validate application
benchmarks as well as local trial decisions. Codegen/retirement oracles select candidates
deterministically through a test friend; the separate timing oracle uses real
controlled CPU work to verify both acceptance and rejection, without fake
clock values. CoreMark comparisons use a fixed 110,000 iterations
(`Scripts/Bench/run_benchmarks.py`, crcfinal 0x33ff, checked against QEMU) and
labelled CRC/validation checks; 60,000 iterations now finish below CoreMark's
ten-second minimum. Auto-calibrated iteration counts cannot be compared by
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

`bash Scripts/Testing/test_jit_tier2_measure.sh` verifies measured faster/slower decisions
and rejection of mismatched work in ST/MT with ordinary/skip chaining. It also
checks that MT publication changes no existing instruction bytes.

`bash Scripts/Testing/test_jit_tier2_feedback.sh` checks two regions sharing one page:
retiring one preserves the other's exact cached function pointer. Isolated
cold exits followed by recoveries do not cause retirement. Eight host CPUs
execute a sustained phase change while the neighboring region remains hot,
checking fixed arithmetic results, retraining and immutable published MT code
in ordinary and chain-skip modes. The same harness places a region exit target
on the next page, rewrites that target, and verifies the new result while the
region itself stays cached.
