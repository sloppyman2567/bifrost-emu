# JIT architecture and contributor guide

This guide describes how the AArch64-to-x86_64 JIT works now and the steps for changing instruction support without
letting decode, translation, optimization and code generation drift apart. Read it before any JIT, shared decoder or IR
work.

Why each mechanism is shaped the way it is, with the measurements and failed attempts of its revision, is in
[the JIT history](archive/JIT-history.md), dated and listed under [Recorded changes](#recorded-changes): read the matching
entry before changing that code. Figures quoted here carry their date and are not measurements of the current executable.

The [source map](ARCHITECTURE.md) describes the subsystem layout and split
boundaries. Compiler prediction/leaf analysis lives in `Compiler/BlockAnalysis.cpp`,
Tier-1 orchestration in `BlockCompiler.cpp` and differential store-footprint
analysis in `VerificationStores.cpp`. The refactor's build/test checkpoint is
recorded in [TESTS.md](archive/TESTS-history.md#subsystem-layout-and-source-splits-2026-10-11);
it does not establish a new performance improvement.

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

Tier-1 compiles one block at a time (32 instructions by default, a loop body up to twice that) and chains blocks through
patched slots in single-threaded (ST) code or atomic target cells in multithreaded (MT) code. Tier-2 (`BIFROST_TIER2=1`,
experimental and off by default) fuses a hot path of two to eight Tier-1 blocks into a region, keeps it only when a timed
trial beats Tier-1, and retires it when its exit rate outgrows the trial's ([Tier-2](#tier-2-loop-discovery-and-publication)).
The interpreter is the reference semantics and the fallback for every instruction the JIT does not emit natively.

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

## Guest state in the CPU

### Vector registers

`CPU::v[32]` is `struct VReg { uint64_t lo, hi; }`, 16-byte aligned. Generated code addresses `vlo_off(n)`/`vhi_off(n)` and
moves a whole register with one `movups` (`load_v16`/`store_v16`): the vector cache's reloads and writebacks,
`load_vec`/`store_vec`, the Q forms of the FP vector helpers, LD2-LD4/ST2-ST4 and the uncached 16-byte guarded access. The
low-half scalar FP cache stores 8 bytes, because its XMM does not hold `hi`. Element offsets are `vlo_off(n) + byte`. C++
reads `cpu.v[n].lo`/`.hi`; a whole-state copy is one `memcpy` of `v`. Keep 16-byte reloads behind 16-byte stores: a 16-byte
load that spans two earlier 8-byte stores cannot be served by store forwarding.

### NZCV

`CPU::nzcv_host` holds the image that `lahf; seto al` leaves in AX: N = bit 15, Z = bit 14, bit 8 = x86 CF = NOT ARM C,
V = bit 0; the other bits are don't-care.

- A pack is `lahf; seto al; mov [nzcv], eax`, with a `cmc` first when the producer's CF is the ARM carry (additions,
  logical ops). A reload is `mov eax, [nzcv]; add al, 0x7f; sahf` and clobbers only RAX.
- The store is 32 bits wide because readers load 32 bits: a 16-bit store misses store forwarding.
- One-flag `BRCOND` tests read the bit in memory (C as "CF bit clear"). `FP_CMP` and `FCCMP` store constants converted at
  compile time (`host_from_arm_nzcv`), `CCMP` loads one into the host flags, and `mrs`/`msr nzcv` convert with one
  multiply each.
- C++ goes through `nzcv()`/`set_nzcv()` (the architectural view) and the flag accessors; only save-and-restore code copies
  the raw value. Signal frames, ptrace, the GDB stub, the C API and differential verification see the architectural view.
- `nzcv_dead_in` drops an exit's pack only when the successor rewrites all four flags before anything can fault, because a
  guest signal frame must show the flags of the faulting instruction
  ([Dead NZCV stores at block exits](archive/JIT-history.md#dead-nzcv-stores-at-block-exits-2026-10-20)).

`jit_flags_exit_dead` (section K faults on a successor's first load and checks the frame's NZCV), `jit_msr_nzcv`,
`jit_fcmp_flags`, `jit_ccmp` and `jit_branch_flags` cover the conversions and the memory tests.

### The guest PC

`cpu.pc` is current at dispatcher boundaries, at every exit that returns to the dispatcher and wherever a C++ helper may
read it; inside chained code it is stale.

- A static chain-skip exit is `slot; mov eax, pc; mov [cpu.pc], rax; frame return` (`defer_pc_store()`, which is
  chain-skip alone, with or without Tier-2). A patched slot jumps past the store; every other way out of the slot (still
  unpatched, an empty or revoked MT cell, the backward-edge checks) falls into it. An MT backward edge keeps `mov eax, pc`
  before its slot, which compares RAX with the stop sentinel. Indirect (`ret`/`br`/`blr`) and unchainable exits, and the
  ordinary layout (`BIFROST_NO_CHAIN_SKIP=1`), store before the slot.
- Every call emitter sets `block_calls_out_`. A block that calls out of generated code publishes a chain entry that stores
  its own start PC first: inline before the plain entry when a call is expected, in a stub after the block (store, then a
  jump to the plain entry) when one was emitted without being expected. Guarded accesses reach helpers only through exits
  that set the PC themselves, so integer blocks carry no entry store.
- A Tier-2 trial wrapper's three tails store their head's PC. A region's chain entry stores the head PC once per entry and
  the back-edge jumps past it; region exits store the PC once.

## Block exits and chaining

- **Exits store no scratch values.** `BR` and `BRCOND_FALLTHRU` end a Tier-1 block and use `flush_all_vregs_exit`, like
  the shared epilogue. A memory fault exit drops its scratch stores too: every path through it ends the frame.
- **Shared cold stubs.** Each JIT emits one copy before its first block (`FrostJIT::emit_cold_stubs`): page-check stubs, one
  per allocatable address register, width and direction, built by `emit_guard_page_checks` (a stub preserves every
  register and sets ZF when the access may proceed); a fault exit (the tail loads the guest PC into RAX and jumps to a stub
  that stores it, calls `jit_fault_step` and ends the frame); and the shared frame exit (`frame_exit_stub_`). A guarded
  access's tail is a `call`, a `je` back to the access and the state commit its own exit needs. Vector and pinned-register
  commits stay in each tail. Persistent-cache recording compiles (`use_cold_stubs()` is false) and `BIFROST_FAULT_TRACE`
  keep full inline tails: a call out of the block means nothing in another process, and fault traces pass their block's PC.
  The stubs serve widths up to 8; a 16-byte access keeps its own page checks.
- **Indirect branches.** Each thread's IBL table (`FrostJIT::IblTable`) starts at `IBL_MIN_ENTRIES` (1,024) and quadruples
  up to `IBL_MAX_ENTRIES` (65,536) once it has replaced one target per entry since its last resize. Only `entries` moves, so
  a table pointer saved across a nested run (`IblGuard`, Tier-2 trials) stays valid; the probe reads the pre-scaled mask
  (`byte_mask`) and the entry array from the header, and its miss jumps are `jcc rel8`. MT probes
  compare the preemption epoch instead of the generation: every generation change advances the epoch, and a native entry
  takes its lease before it syncs the table. ST and timed-sampling code keep the generation check. Keep the probe inline at
  each site: a shared lookup stub made `bench_dispatch` 44% slower (2026-11-03). `jit_dispatch_cache_growth` replaces code
  at an address only a grown table reaches.
- **CSET.** A `CSEL` whose operands are XZR or block constants 0 and 1 emits `setcc` and `movzx` (fold-lookahead kind 3
  skips the constants' moves). A `CSEL` read only by a ZEXT 32 joins W-form fusion: its 32-bit `cmov` clears bits 63:32.
- **Constants and flag saves.** `emit_mov_const` picks the shortest load (a sign-extended imm32 for small negatives).
  `clobber_flags` saves RAX around the pack only when RAX is still mapped or holds the next PC; ADD/SUB/AND/OR/XOR/MUL use
  its RDX-only form.

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
same limit ([details](archive/JIT-history.md#vector-fmla-by-element-ld1r-loop-bodies-and-q-accesses-2026-11-03)). Long blocks
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

### Loop pins

A self-loop keeps up to four loop-carried GPRs in R12-R15. `BRCOND` flushes with `flush_all_vregs_but_pins`, so a
`cmp; b.cond` loop leaves its pins mapped and dirty across iterations as a `cbz` loop does; the exits write them back, and
fault exits and the FP cold step store every mapped architectural register, clean or not. The pin preload must raise
`max_vreg_`: the allocator trusts a mapping only for `v <= max_vreg_`, so an op that reads a pin before anything else is
allocated (`scvtf s0, w1` at a loop head) would read the stale `cpu.regs` slot. The back-edge stores every dirty, unpinned
architectural vreg (`store_dirty_unpinned_arch_regs`) before its slot. `jit_selfloop_pins` and `jit_fp_ldst_cache` cover
both.

### Peepholes

Tier-1 decides each with a pre-scan and the emitter re-checks it:

- `LDRSB`/`LDRSH`/`LDRSW` are one sign-extending load (`sext_load_`).
- `sxtb`/`sxth`/`sxtw` through `SBFM` are one `movsx`/`movsxd`.
- A 64-bit `lsl #1..3` read only by the next `ADD` becomes that `lea`'s index scale (`scaled_add_`; one `LOAD_REG` may sit
  between them, as the translator emits for `[xN, xM, lsl #s]`).
- A register read only as the second operand of the next `CMP` is `cmp r, [cpu.regs]` (`lazy_load_`).

The last two leave a value unmaterialized for one op; `materialize_scaled` and `materialize_lazy_load` emit the original
instruction for any consumer that does not fold it, and the Tier-1 loop calls them before every other op. W-form fusion and
`sext_load_` also run in Tier-2 regions (`scan_w32_and_sext_loads`); `fp_load_`, `lazy_load_` and `scaled_add_` are Tier-1
only, because they need its per-op materializers. The kill-list scan stops at the highest vreg the block uses.

### Conditional compares

`CCMP`/`CCMN` keep both operands where the allocator holds them and fold a constant second operand. The condition comes
from the host flags, from one bit of `nzcv_host` (EQ/NE/CS/CC/MI/PL/VS/VC) or through a reload that clobbers only RAX. The
true path is `cmp` (`CCMN`: `mov` + `add` into a scratch register); the false path sets the immediate NZCV with
`mov eax, image; add al, 0x7f; sahf`, inside `push rax`/`pop rax` when RAX holds a value. Both paths leave the flags live in
the host in the op's carry convention and store nothing. AL and NV always compare. `jit_ccmp` covers every condition against
a C model that also passes under QEMU.

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

## Scalar memory guards

Native scalar loads/stores select guard scratch registers through the allocator
instead of discarding every caller-saved mapping on each access. Bounds,
permissions, userfaultfd missing-page bits, and crossing-page checks remain.
The software-check flag is read on every access; another guest CPU may raise it
while a block executes (see [Software page checks](#software-page-checks)). The result register is separate from
the address, preserving inputs if the host access faults. Cold recovery commits
pre-instruction guest state and steps the instruction before returning to the
dispatcher. Paired instructions retain the existing whole-footprint guard;
forced software guards and slow-store tracing retain their existing path.
`jit_memory_guard_address` covers live-register pressure, negative offsets,
and byte/halfword/word/doubleword accesses; the host-fault and userfaultfd
fixtures cover recovery and retry behavior.

NZCV is stored as described under [Guest state](#nzcv). BRCOND exits omit saving and restoring RFLAGS after their condition
has been consumed; both exits still commit NZCV. Other consumers keep RFLAGS. `jit_flags_passthrough` checks 32/64-bit
add/sub results and all NZCV bits on both branch paths.

### Software page checks

The software-check byte is raised only while some page needs it (`Memory::software_check_page`): userfaultfd registered and
missing, the only state whose host page admits a native access the guest must not make. Every page-flag change goes through
`Memory::set_page_flags_locked`, which counts those pages, raises the flag before the first one changes and clears it after
the last. ART registers its moving and linear-alloc spaces only during a compaction, so the flag is clear between
collections. While it is set, a guarded access pays a stub call.

- Execute-only pages are readable: Linux arm64's `protection_map` (`arch/arm64/mm/mmap.c`) maps `VM_EXEC` as
  `PAGE_READONLY_EXEC` without Enhanced PAN, which the emulated Cortex-A72 lacks. `Memory::effective_page_prot` applies that
  rule, as for write-only pages; the VMA and `/proc/self/maps` keep `--x`.
- `UFFDIO_REGISTER` clears MISSING on pages that `/proc/self/pagemap` shows present or swapped *and* that hold nonzero bytes.
  The contents test is required: a host transparent huge page populates untouched zero neighbours. Limitation: a discarded
  page that the guest touches but leaves all zero, then registers without discarding again, still delivers a missing fault
  (Linux would not). If pagemap cannot be read, the flag stays set.
- `check_access` clears an unregistered page's MISSING bit with a compare-and-swap that faults instead when a registration
  lands meanwhile, so the count cannot drift.
- `[mem] software page checks: pages=N activations=N sticky=0|1` (with the `[JIT]` statistics) shows the state. Tests:
  `test_userfaultfd`, `test_mem_guard`, `jit_guard_stubs`.

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

### Scalar FP in the XMM caches

- **Cold steps after the block.** `emit_fp_cold_tail` defers each scalar FP op's ARM-semantic step (commit, single-step the
  interpreter, reload the vector cache) to the end of the block, with the register layout captured at that point, so the
  hot path falls through.
- **Loads.** A 4- or 8-byte `LOAD_MEM` read only by the next `FMOV_G2F` (`fp_load_`) is `movss`/`movsd xmm, [mem]` into the
  register's cached XMM, and the `FMOV_G2F` emits nothing. The access is a registered fault site and the slot is claimed
  before the guard captures its layout, so a fault leaves the XMM untouched and the exit sees the state before the load.
- **Results.** A single-precision result reaches its cached XMM as `xorps xd, xd; movss xd, x` and a double as
  `movq xd, x`, so in the vector cache the following zero of bits 127:64 is dropped. Measured and rejected: `mov qword
  [hi], 0` for the upper-half zero and `insertps` for the result write, both slower on a culling kernel.
- **NaN checks stay exact.** Dropping one needs a result that cannot be NaN or whose payload nothing can observe; in the
  measured FP kernels 3 of 277 checked ops qualify. Relaxing NaN semantics is the maintainer's decision.
- **Compares.** When `fcmp` feeds a conditional branch or an integer select, `FP_CMP` leaves its result in the host flags as
  `SUBS` does: `seta`, `setb` and a `cmovp` pick two small integers, and one `cmp` of them leaves the flags a subtraction
  would (greater 1-0, equal 0-0, less 0-1, unordered 0x80000000-1). A compare feeding `fcsel`, `mrs` or anything else keeps
  the branchy sequence, which jumps on PF, CF and ZF directly; lazy flags there were slower on a clamp loop. `FCCMP` is
  unchanged. `jit_fcmp_flags` checks every pair of special values, both precisions, all forms and consumers.
- **By element.** Scalar FMUL, FMULX, FMLA and FMLS by element are `FP_BINOP`, `FMADD` and `FMSUB` with a `lane` (IR
  `cond` = element + 1; `fp_decode::scalar_fp_by_element`). `fp_load_lane` moves the element into scratch (`pshufd` from a
  pinned XMM, or a load from memory); the low-half FP cache reads an upper-half element from memory. Vd is written after the
  element is read, so it may alias Vm. FMULX's 0 times infinity takes the cold step. Vector FMLA/FMLS by element (2S/4S/2D)
  are `SIMD_FP_FMA` with the Vm element broadcast into scratch XMM2, then one FMA3
  (`fp_decode::vector_fp_fma_by_element`, decided before the native SIMD table); reserved D forms and FP16 are unchanged.
  `jit_fp_pw_elem`, `jit_simd_fp_indexed`.

## SIMD memory: structures, LD1R and Q accesses

- **LD2-LD4 and ST2-ST4** (`SIMD_STRUCT_MEM`), every element size and both widths, when the register list does not wrap past
  V31 and the arrangement is not `.1D`. The post-index update follows as ordinary IR, so the op writes no GPR. One software
  guard covers the whole span and steps the instruction on any failure, so no vector register changes before every byte is
  read. The emitter (`CodeGen/FloatSIMD/SIMD/Structure.cpp`) shuffles in registers: `LD2`/`ST2` one unpack or `shufps` (plus
  a `pshufb` mask for 8- and 16-bit elements), `LD4`/`ST4` a 4x4 transpose, 64-bit arrangements two unpacks, and `LD3`/`ST3`
  with any other combination a generic `pshufb` gather or scatter (correct, not tuned). It uses XMM0-2 and XMM12-15 only and
  tells the vector allocator so (`vec_inst_effects`, clobber set 0xF007). Tier-2 stops at the op. Single-lane forms,
  replicate forms other than `LD1R {Vt.T}`, `.1D` and wrapping lists use the `native_structure` helper. Only `Rm` = 31
  selects the immediate post-index; X30 is a register. `jit_neon_struct` builds every combination at run time against an
  independent model that passes under QEMU.
- **LD1R {Vt.T}** (one register, not `.1D`) is a guarded `LOAD_MEM` of the element and the native `SIMD_DUP`, then the
  post-index update; the base moves after the load, so a fault leaves Vt and Xn unchanged. LD2R-LD4R and `.1D` keep the
  helper. `jit_ld1r`.
- **Q loads and stores** (`SIMD_LD16`/`SIMD_ST16`, one register) take the scalar guarded path at width 16
  (`compile_guarded_vec16`): the allocator's registers, one window-table compare, a deferred tail and one MOVUPD into the
  pinned XMM or through XMM0. A 16-byte access can cross a page in bytes 8-15, so its tail keeps its own page checks. Pairs
  and Tier-2 keep the older path. `jit_guard_stubs`.

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
current service checkpoint in [the runtime reference](Android/ANDROID_RUNTIME_REFERENCE.md).

These policies share the buffer emulation, which maps the host buffer and
gives the guest a bounce of the same range (see the graphics guide). Flush
offsets are relative to the mapping, as the
[Khronos specification](https://wikis.khronos.org/opengl/GLAPI/glFlushMappedBufferRange)
requires. `test_sdl_gl_mapbuffer` checks valid GL usage only and passes natively
on the host driver as well; it is not an ES 3 conformance test.

## Tier-1 zero and bit branches

CBZ/CBNZ and TBZ/TBNZ first commit pending NZCV with `clobber_flags()`.
Their TEST/BT instructions can then overwrite host flags: guest flags remain
in `cpu.nzcv_host`, so the branch needs no PUSHFQ/POPFQ. Operands are tested in
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
different, provable case: [Dead NZCV stores at block exits](archive/JIT-history.md#dead-nzcv-stores-at-block-exits-2026-10-20).
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
and the emulator replaces the whole instance (see [generations](#code-cache-generations-and-syscall-exits) below:
the full one is released once no thread can run it). An instance that
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

### Code-cache generations and syscall exits

A full code cache is replaced as a whole. When a compile runs out of budget, `translate_entry` asks the emulator for a
successor (`Emulator::renew_jit`): a fresh `FrostJIT` becomes current and the full one is retired (`FrostJIT::retire`:
`invalidate_all`, then the retired flag, then the global epoch advances). `Emulator` keeps every instance in a `JitSet`; the
last is current. Only the shared instance is renewable; per-thread instances (`BIFROST_NO_SHARED_JIT=1`) are not.

- **Release.** A retired instance stays mapped until no thread can run its code (`Runtime/Generations.{hpp,cpp}`). Every
  thread has a slot. At a dispatch boundary with no generated frames (`cpu.jit_call_depth == 0` at `run_block` entry) it
  reads the epoch and acks it; a retired instance's `run_block` forwards to the current one. A thread blocked in a syscall
  that a block handed back to C++ is parked, and leaving the park exchanges the flag with a full barrier before the epoch
  read. A reaper releases an instance once every slot is acked or parked (`FrostJIT::release_resources`: the code buffer,
  the fault table and the per-block metadata, including heap cells generated code addresses directly), keeps the 56 KiB
  object (generated code embeds the address of members such as `cache_generation_`) and calls `malloc_trim`.
- **Bound.** At most one retired instance may be unreleased (`kMaxUnreleased`); while it is pinned, the current one falls
  back to the interpreter once full, so the worst case is twice the budget. A nested `jit_call_helper` loop follows the
  current instance once its own is retired (`retarget_scope`); helpers called from generated code (Tier-2 fire, trials,
  feedback) use `FrostJIT::running`. Under `BIFROST_NO_CHAIN_SKIP=1` the program runs inside nested frames, so only the
  first generation is ever replaced. A renewal discards every block, so the working set recompiles.
- **Syscalls leave generated code first.** A block that reaches a non-thunk SVC stores its address in `cpu.jit_svc_pc` and
  returns; `run_block` (and the nested `jit_call_helper` loop) runs it from C++ (`FrostJIT::run_pending_svc`) with `cpu.pc`
  already advanced, so a thread blocked in futex, epoll or sleep holds no generated frame. Thunk SVCs (`x8 == 0x1000`, GL,
  audio and display calls) stay inline. A signal or the debugger that moves `cpu.pc` first drops the pending SVC, and the
  guest re-executes it where it resumes. `run_pending_svc` clears the field before the syscall, because `clone` copies the
  CPU.

`[JIT] generations: retired=N released=M blocked_by_threads=K` (under `BIFROST_STATS_PERIOD` and with `--verbose` at exit)
shows whether releases keep up. Tests: `jit_cache_generations` through `test_jit_generations.sh` (1 MiB budget, about 500
renewals with sleepers, nested calls, rewritten code, signals and a fork) and `test_jit_cache.sh`.

## Persistent translation cache

Experimental and off by default. `--jit-disk-cache` (`BIFROST_JIT_DISK_CACHE=1`, `[jit] disk_cache = true`, or the per-app
setting) stores Tier-1 translations of file-backed guest code so a later launch installs them instead of compiling.
`--jit-disk-cache=verify` compiles every block anyway and compares the fresh record with the stored one
(`verify_mismatches` in the statistics). `Runtime/CacheRecording.cpp` records and installs; `Runtime/PersistentCache.*`
stores.

- **Key.** A record is found by the file the code came from (`Memory::code_origin`: a hash of the path the mapping's
  descriptor names, plus the offset; memfd and deleted files have none) and by MT or ST mode. Installing checks the guest
  words now at the address and the dead-NZCV successor words (`flag_dep_table_`) before relocating the code, then publishes
  through `publish_translation` like a fresh compile (granules, back references, chaining, pending calls).
- **Proof compile.** A recording compile logs every `emit_u64` field; emitters that embed a host address declare it with
  `note_host_reloc` (image, JIT object, window base, page flags, MT edge cell), and every constant from 0x10000 to
  0xFFFF_FFFF takes its imm64 form (`wide_guest_pc`), so the same code records the same bytes at any load address. Before
  publication the block is compiled again at guest PCs shifted by `kOracleBias` (`record_oracle_`). Outside the logged
  fields the bytes must match; each field must be a constant, a declared address inside what it names, a guest PC (moved by
  the bias) or a code address (moved by the copy's distance). Edges, fault sites, words and dependencies must agree up to
  the bias. A constant equal in both compiles that lies in host memory (`host_address`, or a page `mincore` reports mapped
  outside the guest window) is an undeclared address and leaves the block unrecorded. Anything unexplained costs a cache
  entry, never a wrong translation. Emit host addresses as imm64 with `note_host_reloc`: 32-bit immediates and
  RIP-relative operands outside the block are not covered by these checks.
- **Off when** Tier-2, `BIFROST_JIT_VERIFY`, leaf inlining, `BIFROST_MALLOC_INTERP` or block tracing is on. Blocks with
  direct BL call sites, inlined leaves or interpreter-only bodies are not recorded. Recording compiles keep full cold tails
  ([Block exits](#block-exits-and-chaining)), so a recording session's code is slightly larger.
- **Fingerprint.** One directory per application (`BIFROST_JIT_CACHE_APP`, set to the package by the managed launcher; else
  the program name and a hash of its path) and per codegen fingerprint: the emulator's GNU build-id, CPU features, window
  size, code-buffer size, chain/direct-call/W^X/FMA3/AVX2 state and the value of every `BIFROST_*` switch the JIT and
  `DebugFlags.hpp` read. The build lists those switches from the sources into
  `$(BUILD_DIR)/generated/JitCodegenSwitches.inc` (the Makefile's `JIT_SWITCHES` rule), so a new switch cannot be left out.
  A diagnostic switch therefore makes a run miss the cache.
- **Memory.** A process keeps `main.bin` mapped only to search its index in place. The record a lookup finds, in `main.bin`
  or a segment, is read with `pread` into a buffer of the calling thread (valid until its next `find`), and segments are
  unmapped once indexed: mapped records stay resident (fault-around reads 64 KiB around each, which `MADV_RANDOM` does not
  stop) and a Zygote's mappings reach every child it forks. `--verbose` prints
  `[JIT] disk cache files: mapped=… resident=…`, the index's Rss.
- **Storage.** `$BIFROST_JIT_DISK_CACHE_DIR`, else `$XDG_CACHE_HOME/bifrost/jit`, else `~/.cache/bifrost/jit`. Each process
  appends checksummed records to its own `seg-<pid>-<n>.bin`, locked (`flock`) before its header is written; a header-less
  segment is treated as abandoned only after a minute. Once segments reach 4 MiB or an eighth of `main.bin`, the next
  long-running process merges unlocked segments into a new `main.bin` (sorted index read in place, `rename` over the old
  one) in the background, keeping the most recently used records within three quarters of the budget
  (`BIFROST_JIT_DISK_CACHE_MB`, or `_KB` for tests, default 512 per application). Opening the cache first removes other
  fingerprints' directories (older builds) down to three quarters of the budget. The cache stays off when a directory on
  the way is not the user's own or is group/world-writable, or when the filesystem has no `flock`.

Not validated: play past Umamusume's title and other Android games from the cache (ART JIT code is memfd-backed and never
cached; native libraries are), a game session that renews its code cache, and network filesystems. Tests:
`Scripts/Testing/test_jit_disk_cache.sh` (`run_tests.sh` entry `jit_disk_cache`, which fails under Tier-2 by design).

## Fork children keep the JIT

A fork child keeps its parent's JIT and translations, so a Zygote child, which never execs, runs warm code.

- `fork_guest` adds every live instance's locks to the set it takes all or none before the host `fork()`
  (`FrostJIT::try_lock_for_fork`: the block table's read/write lock, the compile and compile-invalidation locks, and each
  Tier-2 trial's lock). Holding them, no other thread is translating, publishing or invalidating, so the child's copy of
  the tables is consistent.
- The child re-creates those locks (a read/write lock copies its count of waiting writers, which no thread there clears, as
  `Memory::reset_mutex_in_fork_child` handles for guest memory) and restarts the preemption ticker when the JIT is MT:
  MT chained code returns to the dispatcher only when the epoch moves. The code buffer is a private anonymous mapping, so
  parent and child extend and patch their own copies.
- The child runs interpreted until an `execve` only when a lock cannot be held (the 3 s deadline) or with per-thread JITs
  (`BIFROST_NO_SHARED_JIT`), which belong to threads the child does not have. An exec'd image gets a fresh JIT
  (`Emulator::restart_jit_for_exec`).

`Tests/Unit/test_fork_jit.c` checks that a child runs its parent's warmed loop within 4 times the parent's time, takes
SIGALRM in a tight loop as the child of a multi-threaded parent, starts a thread and forks again.

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
([Scalar FP in the XMM caches](#scalar-fp-in-the-xmm-caches)). The generated
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

## Multithreaded chaining and Tier-2

### Bounded MT self-loops

Tier-1 conditional branches to their own block now retain the current frame,
eligible GPR pins and cached vectors in MT mode. The back-edge uses an immutable
guard and an atomic cell pointing to this translation's body, bypassing prologue
reloads. It checks the callback stop PC and the preemption epoch; expiry or revocation commits carried state and returns to dispatch.
Range/full invalidation clears the cell. Target refresh and Tier-2 publication
never redirect a body cell into a different translation's frame or register
layout. Cells and retired code retain the existing lifetime policy.

BRCOND commits NZCV before the MT guard because its checks overwrite host
flags and any iteration may become an exit, unless the loop head overwrites
all four flags first (see [Dead NZCV stores at block exits](archive/JIT-history.md#dead-nzcv-stores-at-block-exits-2026-10-20)). The ST-only
loop-carry shortcut remains ST-only. CBZ/CBNZ and TBZ/TBNZ preserve their pre-branch NZCV contract.
`BIFROST_NO_SELFLOOP=1` restores ordinary loop edges; `BIFROST_NO_PIN=1`
disables GPR pinning, and `BIFROST_NO_CHAIN=1` also disables MT body edges.
Both frame layouts support this path. Tier-2 multi-block regions retain their
whole-iteration entry policy described below.

### Self-loop back-edge writeback

The back-edge jumps to the body start, whose allocator state holds only the
loop-carried pins. Any other dirty architectural register cached in a host
register was previously written back only on the exits emitted after the
self-loop slot, so the next iteration's `LOAD_REG` read the previous value.
`emit_selfloop_backedge` now stores every dirty, unpinned architectural vreg
(`store_dirty_unpinned_arch_regs`) before the slot in both ST and MT forms;
pins stay in their registers and the exits keep their own writeback.

### MT preemption epoch, forward edges and cell calls

MT code bounds chained execution with an epoch poll that only loads, not a shared read-modify-write counter.

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
fork child keeps the JIT and restarts the ticker when the JIT is MT (see [Fork children keep the JIT](#fork-children-keep-the-jit)), an exec'd image gets a fresh JIT
(`Emulator::restart_jit_for_exec`, because `jit_wanted_` remembers that one was enabled),
and the ticker is forgotten in a fork child (`pthread_atfork`) so an exec'd multi-threaded program starts its own;
`BIFROST_NO_CHAIN` and `BIFROST_NO_DIRECT_CALL` keep their meanings. Not changed: code blocks that never reach a backward edge, IBL probe or cell call (long straight-line
chains) are bounded by their length, as before for ST.

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
edges share the mechanism. Direct BL targets use the same cells (see the preemption section above),
so invalidation cannot leave an embedded stale callee address.

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
candidates restore Tier-1. A measured loss (all twelve pairs, not a path or work mismatch or a timeout) retries the head as
a shorter trace after fresh profiling: five or more blocks retry at four, three or four at two, and two blocks settle
(`tier2_block_limits_`, at most twice per head through `tier2_retrains_` and within the code and compile-time limits
below; a phase retirement clears the cap, since a new phase may favour a longer path). A trial
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

### Calls, returns and exits

- **A RET can end a region.** Its block keeps the `BR`; the side exit is `Tier2Trace::kDynamicExit` with the register in
  `dynamic_exit_reg`. The fall-through exit commits every vreg, pin and flag, loads the register's committed value into RAX,
  stores `cpu.pc` and tries Tier-1's indirect-branch probe (`emit_indirect_branch_probe`) before the shared exit tail. The
  planner does not count that final `BR` as a call, so such a region keeps pins and chain-skip.
- **The walker follows a BL into its callee** like a direct jump, pushing the return address (at most four deep). A RET that
  matches the top becomes `IMM ret; XOR t = xN ^ ret; BRCOND_ZERO t != 0`: a flag-neutral guard whose taken edge is a
  dynamic exit through xN, while the trace continues at the return address. A callee already in the trace, or deeper
  nesting, ends the trace with a static exit into it. `BIFROST_NO_CALLREGION=1` turns this off, as it does native call
  regions. `jit_tier2_calls` (suite entry `tier2_calls`).
- **Every region exit has a cell.** An ST exit whose target had no translation when the region compiled gets an empty cell,
  filled when the target is translated (`chain_back_references`); MT regions always had cells.
- **Settling and refresh.** A region whose trial does not start settles every constituent, not only its head. A head whose
  traced block no longer translates to the same length (Tier-1 split it at a newer block start) is retranslated at the
  current boundaries without being settled (`tier2_refresh_queue_`, at most twice per head through `tier2_retrains_`).
- **Region code** uses W-form fusion and one-instruction sign-extending loads, and `CBZ`/`CBNZ`/`TBZ`/`TBNZ` terminals test
  their operand where it is held.
- **Not admitted:** single-block loops (Tier-1 keeps them in registers, and a trial needs two blocks), loops whose iterations
  take different arms, loops with an inner loop and dispatch loops. Admitting them needs regions with internal joins: each
  join a commit point on both incoming edges, a barrier there for the planner's forwarding and LICM, and trial baselines
  for off-path blocks. Raising the eight-block cap alone admits none of the measured kernels. Tier-2 spends about 20 M
  cycles per run on compiles and trials before it pays (2026-11-11).

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

### Where host time goes: `BIFROST_PROF`

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

## Measuring JIT changes

- **Count, then time.** Host instruction counts (`perf stat -e instructions:u`) repeat to four digits. Cycle counts move by
  up to ±15% with code placement alone (identical code at 16 offsets of the code buffer), so compare cycles over several
  placements before trusting a single-digit change.
- **Attribute with IBS.** `perf record -e ibs_op/cnt_ctl=1,swfilt=1/u` with `BIFROST_JIT_DUMP=1`, sample addresses mapped
  onto the dumped blocks and each instruction classified by operand pattern; `instructions:u` skids too far to attribute
  single instructions. The dump holds pre-patch bytes, so chain slots show as `nopl` although they execute as jumps.
  `BIFROST_PROF` (above) gives the coarser split without perf.
- **Split a ratio to native** into guest instructions over native instructions (the guest compiler's share, which the JIT
  cannot change) and host instructions per guest instruction (the JIT's share).
- **CoreMark.** `Scripts/Bench/run_benchmarks.py` runs 200,000 iterations (`crcfinal` 0x4983, the value QEMU 11.1.1
  prints). A run under ten seconds is invalid, and a new count needs its CRC from QEMU. Fix the CPU governor to
  `performance`. Whole-process cache-miss counts mostly measure startup and translation (the dynamic loader, thunk
  registration), not generated code. CoreMark does not predict game frame rates.
- **Latest recorded figures.** CoreMark 14,874 iterations/s on the shipped build against 7,992 for QEMU 11.1.1 (Ryzen 7
  5800X3D, 2026-11-08), and 37,951 natively (gcc `-O2`, same host, 2026-11-07); records and methods are under
  [Recorded changes](#recorded-changes). The README's performance tables are the published figures.

## Recorded changes

Optimization checkpoints and fixes are in [the history](archive/JIT-history.md), unchanged, newest first:

- [A fork child keeps the JIT (2026-11-11)](archive/JIT-history.md#a-fork-child-keeps-the-jit-2026-11-11)
- [Tier-2 exits that chain, exit-rate feedback and profiler costs (2026-11-11)](archive/JIT-history.md#tier-2-exits-that-chain-exit-rate-feedback-and-profiler-costs-2026-11-11)
- [Tier-2 through calls and returns (2026-11-11)](archive/JIT-history.md#tier-2-through-calls-and-returns-2026-11-11)
- [Tier-2 without a Tier-1 tax, and leaner region code (2026-11-11)](archive/JIT-history.md#tier-2-without-a-tier-1-tax-and-leaner-region-code-2026-11-11)
- [CCMP and CCMN leave their flags in the host (2026-11-10)](archive/JIT-history.md#ccmp-and-ccmn-leave-their-flags-in-the-host-2026-11-10)
- [Guest register traffic and flag windows: measured, not merged (2026-11-10)](archive/JIT-history.md#guest-register-traffic-and-flag-windows-measured-not-merged-2026-11-10)
- [One 16-byte slot per vector register (2026-11-10)](archive/JIT-history.md#one-16-byte-slot-per-vector-register-2026-11-10)
- [Scalar FP loads into cached XMMs and cheaper scalar results (2026-11-10)](archive/JIT-history.md#scalar-fp-loads-into-cached-xmms-and-cheaper-scalar-results-2026-11-10)
- [NZCV stored as the `lahf` image (2026-11-10)](archive/JIT-history.md#nzcv-stored-as-the-lahf-image-2026-11-10)
- [PC stores behind the chain slot, loop pins and four peepholes (2026-11-10)](archive/JIT-history.md#pc-stores-behind-the-chain-slot-loop-pins-and-four-peepholes-2026-11-10)
- [CoreMark re-measurement (2026-11-08)](archive/JIT-history.md#coremark-re-measurement-2026-11-08)
- [FP compares in the host flags, and where last-level misses come from (2026-11-07)](archive/JIT-history.md#fp-compares-in-the-host-flags-and-where-last-level-misses-come-from-2026-11-07)
- [Scalar FP cold steps after the block (2026-11-07)](archive/JIT-history.md#scalar-fp-cold-steps-after-the-block-2026-11-07)
- [LD2-LD4 and ST2-ST4 inside the block (2026-11-07)](archive/JIT-history.md#ld2-ld4-and-st2-st4-inside-the-block-2026-11-07)
- [CoreMark native reference (2026-11-07)](archive/JIT-history.md#coremark-native-reference-2026-11-07)
- [Growing indirect-branch tables and shared block exits (2026-11-03)](archive/JIT-history.md#growing-indirect-branch-tables-and-shared-block-exits-2026-11-03)
- [Vector FMLA by element, LD1R, loop bodies and Q accesses (2026-11-03)](archive/JIT-history.md#vector-fmla-by-element-ld1r-loop-bodies-and-q-accesses-2026-11-03)
- [Scalar FP by element (2026-11-03)](archive/JIT-history.md#scalar-fp-by-element-2026-11-03)
- [Exit spills, CSET and two-flag branches (2026-11-03)](archive/JIT-history.md#exit-spills-cset-and-two-flag-branches-2026-11-03)
- [Shared cold stubs and smaller encodings (2026-11-02)](archive/JIT-history.md#shared-cold-stubs-and-smaller-encodings-2026-11-02)
- [CoreMark checkpoint (2026-10-27)](archive/JIT-history.md#coremark-checkpoint-2026-10-27)
- [Software page checks only while a page needs them (2026-10-26)](archive/JIT-history.md#software-page-checks-only-while-a-page-needs-them-2026-10-26)
- [Persistent translation cache (2026-10-26, experimental, off by default)](archive/JIT-history.md#persistent-translation-cache-2026-10-26-experimental-off-by-default)
- [Code-cache generations and syscall exits (2026-10-25)](archive/JIT-history.md#code-cache-generations-and-syscall-exits-2026-10-25)
- [MT preemption epoch, forward edges and cell calls (2026-10-23)](archive/JIT-history.md#mt-preemption-epoch-forward-edges-and-cell-calls-2026-10-23)
- [Self-loop back-edge writeback (2026-10-12)](archive/JIT-history.md#self-loop-back-edge-writeback-2026-10-12)
- [Bounded MT self-loops (2026-10-11)](archive/JIT-history.md#bounded-mt-self-loops-2026-10-11)
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
