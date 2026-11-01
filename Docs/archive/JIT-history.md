# JIT history

Dated optimization checkpoints and fixes moved out of [the JIT guide](../JIT.md), unchanged and in their original order. Their measurements describe the revisions they were recorded on, not the current executable. The guide and `Source/FrostJIT/AGENTS.md` hold the current contracts.

## Chain-skip exits no longer load RDI/RSI (2026-10-26)

Every Tier-1 exit loaded `mov rdi, rbx; mov rsi, [rbx+jit_emu]` (10 bytes) for its chain target. Under chain-skip
no target reads them: blocks, cache-installed blocks and regions enter past their prologue's `mov rbx, rdi` and
emulator store, and so do IBL hits. The readers were the code between the exit and its target, which now
addresses the CPU through RBX like everything else: the MT chain slot (`jit_stop_pc`, the preemption poll, the ST
budget) and the indirect-branch probe. The Tier-2 trial chain wrapper also switched to RBX; its cold tail sets
RDI/RSI itself before it jumps to the full region entry, and its quota tail reads the PC through RBX before the
pops restore the caller's RBX. Exits still load both in the ordinary frame layout (`BIFROST_NO_CHAIN_SKIP=1`),
whose slots jump to `fn`, and Tier-2 region exits keep theirs (cold-entry edges target `fn`). Those two pop first,
so their MT slots read the CPU through RDI; reading RBX there took whatever the C++ caller left in it, and
`jit_tier2_discovery` only crashed when the compiler left it null (GCC 13 and 14, a hang with clang 18, a pass
with GCC 16).

`qt_test`: 98.6 to 95.2 bytes per guest instruction. `bench_dispatch` ran in 1.90 s against 2.31 s (seven
alternating runs), but it varied between 1.95 and 2.30 s across today's builds with layout alone, so the size of
that gain is uncertain; `bench_fib` and `bench_sort` within about 2%.

## Smaller fault exits and one frame return per block (2026-10-26)

Every guarded access also carries a recoverable fault exit, and every Tier-1 exit its own 13-byte frame return
(`mov rsp,rbp; pop x6; ret`). Three changes:

- `jit_fault_step` returns `cpu->pc`, so the exit no longer reloads it (7 bytes), and reads `block_pc` only under
  `BIFROST_FAULT_TRACE`, so the 10-byte start-PC immediate is emitted only then.
- The fault exit aligns its call with `push rax` instead of `pushf`/`popf`: NZCV was committed before the access
  (`clobber_flags` in `compile_guarded_mem`), and the frame return's `mov rsp,rbp` drops the push. POPF is 37
  macro-ops on Zen 3 (Agner Fog's instruction tables) and runs on every interpreter-stepped access.
- A fault exit jumps to a frame return already emitted in its compile unit (`emit_frame_return(true)`,
  `frame_return_off_`, reset with the block or region and by `rewind_codegen`; `jmp rel8` when it reaches). The
  jump stays inside the block, so cached blocks stay relocatable. Block exits keep their own: they return to the
  dispatcher whenever their chain slot is unpatched, and sharing theirs too made `bench_dispatch` 10% slower
  (2.30 to 2.56 s, seven alternating runs) for 1.4 bytes per instruction more.

`qt_test`: 110.2 to 98.6 bytes per guest instruction; Umamusume's game process 120.7 to 107.0 (that run shared
block exits' returns as well, so the shipped figure is slightly higher). `bench_dispatch`, `bench_fib`,
`bench_sort` and `qt_test` within 1%.

## One compact checked path per guarded access (2026-10-26)

Each Tier-1 guarded scalar access carried about 122 bytes of checked path after the block, run only while
software checks are on: the real bound, then two copies of the page check, each with a 10-byte `movabs` of the
page-flag table, an `add`, a `movzx`, and 64-bit `and`/`cmp` with imm32. `emit_guard_page_checks` now emits one
sequence for Tier-1 tails and Tier-2's inline path: the flag byte is read through the table's address in the
header page (`Memory::JIT_PAGE_FLAGS_OFF`, so the tail embeds no host address), `and`/`cmp` are byte-sized, a
crossing access repeats the same check with its last page (the second pass then sees the same page and leaves),
and failures jump short to the fault exit that follows. About 60 bytes for a 2- to 8-byte access.

Measured (one machine): `qt_test` emits 110.2 bytes per guest instruction instead of 135.7 (-19%); Umamusume's
game process 120.7 instead of 148.1 (150 s to the title, one run each, different block counts). The benchmarks
run at the same speed (this code runs only while checks are on); the gain is fewer instruction-cache lines and
pages per block in large programs, which no benchmark here measures, and no game frame rate was measured.

## The bounds compare gates the software checks (2026-10-26)

With the software-check flag normally clear (next section), its test was the remaining per-access cost of a
guarded scalar load or store: `cmp byte [r10-8],0; jne`, 11 bytes, a load and a branch. Removing it made the
four-access loop below 17% faster (2.74 to 2.28 ns per iteration, five runs each), so the bounds compare now does
both jobs. The header page holds two tables of bounds (`window_size - width + 1`, the first address an access
of that width must not start at): the gate (`Memory::jit_window_limit_off`) and the real bounds
(`jit_window_bound_off`). `Memory::set_software_checks` writes 0 into every gate before the flag goes up and
restores them after it comes down, so `cmp addr,[r10+gate]; jae` sends every address, 0 included, to the checked
path while some page needs it. That path compares with the real bound (`jae` to the fault exit) before the page
flags, then rejoins the access. An `LDAR`'s alignment check now precedes the gate because the checked path rejoins
after it. `emit_window_bound_cmp` returns the condition: `jae` for a table, `ja` for the `emit_window_limit`
fallback (a width without an entry, which also keeps the flag test). `emit_load_mem`/`emit_store_mem` read the
gate too; their C slow path checks everything. Other emitters (pairs, vectors, atomics) still test the flag.

Measured (one machine, against the previous commit, minimum of five alternating runs): `bench_fib` 10% faster,
`bench_memcpy` 6%, `bench_sort` 5%, `bench_matrix` 3%; `bench_mips`, `bench_dispatch` and `qt_test` within 1%.
Emitted code grew 0.6% (`qt_test`: 134.9 to 135.7 bytes per guest instruction): 11 hot bytes fewer per access,
14 cold bytes more for the real-bound compare. Umamusume ran 150 s to its title with 16 collections. A hot loop
of the same 1,220 bytes also ran about 10% slower at a different offset in its cache line, so microbenchmark
differences of that size need several layouts. `jit_window_bounds` repeats every check while a
userfaultfd-registered page is missing (closed gate); without the real-bound compare the emulator crashes there.

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
[validation scope](TESTS-history.md#allocator-effects-and-region-demand-layouts-2026-10-16).

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
alignment issue is outside this change. See [validation](TESTS-history.md#inline-pixel-conversion-helpers-2026-10-15).

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
The [regressions](TESTS-history.md#cached-integer-simd-2026-10-15) check the changed
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
[validation and scope](TESTS-history.md#tier-1-vector-allocation-under-pressure-2026-10-15).

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
[validation and limits](TESTS-history.md#tier-1-hot-path-code-reduction-2026-10-14).

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
[TESTS.md](TESTS-history.md#dead-code-compaction-accounting-2026-10-14).

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
[validation and limits](TESTS-history.md#scalar-fp-scratch-preservation-2026-10-13).

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
(see [TESTS.md](../TESTS.md)). References: QEMU's
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
do not establish game FPS. See [validation](TESTS-history.md#eight-block-superblocks-and-region-vector-state-2026-10-12).

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
or FPS result. See [validation and remaining gaps](TESTS-history.md#native-acquire-loads-and-trial-timing-2026-10-12).

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
bound. See [the validation record](TESTS-history.md#indirect-branches-watchdogs-and-lazy-controllers-2026-10-12).

Hang detection is now an opt-in diagnostic (`BIFROST_HANG_WATCHDOG=1`): it
enables the main/worker same-PC limits, main frozen-loop detection and JIT
dispatch/call-helper limits that can stop or demote valid long-running code.
This does not disable the chain preemption bound, host call-depth limit, guest
memory checks or explicit instruction limits. `test_long_call_loop` checks
60 million calls on each of the main and worker CPUs against an inline oracle.

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
[the Doom 3 status](../Apps/doom3.md). Do not describe this checkpoint as a freeze fix.

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

A [native Doom 3 capture](apps/doom3-history.md#native-dispatch-profile-2026-10-12)
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
