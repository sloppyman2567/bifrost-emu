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
  → block chaining
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
| Dispatch, cache and chain blocks | `Source/FrostJIT/Runtime/Dispatch.cpp`, `CodeCache.cpp` |
| Define reference instruction semantics | `Source/Interpreter/Core/`, `Source/Interpreter/FloatSIMD/` including `SIMD/` |

Tier-1 compiles one block at a time (32 instructions by default, a loop body up to twice that) and chains blocks through
patched slots in single-threaded (ST) code or atomic target cells in multithreaded (MT) code.
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
7. Consider chained, self-loop and MT control flow, then add a focused guest
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
  guest signal frame must show the flags of the faulting instruction. Integer data processing and scalar FP data processing
  (everything in the FP class except `fcsel` and `fccmp`, which read the flags) are skipped on the way to the rewrite
  ([Dead NZCV stores at block exits](archive/JIT-history.md#dead-nzcv-stores-at-block-exits-2026-10-20),
  [FP compare and branch](archive/JIT-history.md#fp-compare-and-branch-on-the-raw-ucomis-flags-2026-11-23)).

`jit_flags_exit_dead` (section K faults on a successor's first load and checks the frame's NZCV), `jit_msr_nzcv`,
`jit_fcmp_flags`, `jit_ccmp` and `jit_branch_flags` cover the conversions and the memory tests.

### The guest PC

`cpu.pc` is current at dispatcher boundaries, at every exit that returns to the dispatcher and wherever a C++ helper may
read it; inside chained code it is stale.

- A static chain-skip exit is `slot; mov eax, pc; mov [cpu.pc], rax; frame return` (`defer_pc_store()`, which is
  chain-skip alone). A patched slot jumps past the store; every other way out of the slot (still
  unpatched, an empty or revoked MT cell, the backward-edge checks) falls into it. An MT backward edge keeps `mov eax, pc`
  before its slot, which compares RAX with the stop sentinel. Indirect (`ret`/`br`/`blr`) and unchainable exits, and the
  ordinary layout (`BIFROST_NO_CHAIN_SKIP=1`), store before the slot.
- Every call emitter sets `block_calls_out_`. A block that calls out of generated code publishes a chain entry that stores
  its own start PC first: inline before the plain entry when a call is expected, in a stub after the block (store, then a
  jump to the plain entry) when one was emitted without being expected. Guarded accesses reach helpers only through exits
  that set the PC themselves, so integer blocks carry no entry store.

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
  a table pointer saved across a nested run (`IblGuard`) stays valid; the probe reads the pre-scaled mask
  (`byte_mask`) and the entry array from the header, and its miss jumps are `jcc rel8`. MT probes
  compare the preemption epoch instead of the generation: every generation change advances the epoch, and a native entry
  takes its lease before it syncs the table. ST code keeps the generation check. Keep the probe inline at
  each site: a shared lookup stub made `bench_dispatch` 44% slower (2026-11-03). `jit_dispatch_cache_growth` replaces code
  at an address only a grown table reaches.
- **Chain targets.** A static slot patches to the successor's `chain_entry`, past its prologue, so the successor reuses the frame. With
  `BIFROST_CARRY=1` it patches to the successor's register entry instead when the registers the slot leaves in host registers cover the
  successor's first loads ([Register passing](#register-passing-bifrost_carry1)); dispatcher and IBL entries always use the plain entry.
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
allows bounded comparisons; Tier-1 stops growth
at the instruction cap or the shared IR/vreg complexity budget. A Tier-1 loop
body may reach twice the cap: at the cap `selfloop_extent` looks for a branch
back to the block start, and an already compiled loop tail is absorbed under the
same limit ([details](archive/JIT-history.md#vector-fmla-by-element-ld1r-loop-bodies-and-q-accesses-2026-11-03)). Long blocks
are no longer automatically demoted solely for exceeding 32 instructions;
actual interpreter-fallback density still controls demotion. Larger limits
are diagnostic options, not a verified performance default for every game.
`jit_bitfield_regalloc` covers immediate multiplier signs/widths, aliases and
a long body with fifteen live GPRs.

Belady eviction reads per-vreg use lists (`vreg_uses_`, 4,096 entries).
Each Tier-1 use-scan starts with `reset_vreg_uses()`, which
empties only the lists that the previous scan filled (`note_vreg_use`
records them) and keeps their storage. Outside a compile every list is
empty, so `next_use_after` returns -1 as before. Rebuilding all 4,096
lists per block had been about a third of Tier-1 compile time in a
SpongeBob startup profile (2026-10-13).

### Loop back edges

A single-thread self-loop ends in `jcc taken; <exit>; taken: jmp <slot>`. When nothing sits between the `jcc`'s target and
the slot (`note_selfloop_jcc`), `translate_block` patches the slot and also points the `jcc` at the slot's target (the body, or
the reconcile stub), so an iteration takes one jump instead of two. `BlockEntry::selfloop_jcc_back` (16 bits, the slot's
distance from the `jcc`) lets differential replay point the `jcc` back at the NOPed slot; the disk cache records it
(`NativeBody::selfloop_jcc_back`, cache format version 4). MT edges are immutable and keep both jumps, as does an edge with
code before its slot (stores of dirty unpinned registers, flag materialization).

### Loop pins

A self-loop keeps up to four loop-carried GPRs in R12-R15. `BRCOND` flushes with `flush_all_vregs_but_pins`, so a
`cmp; b.cond` loop leaves its pins mapped and dirty across iterations as a `cbz` loop does; the exits write them back, and
fault exits and the FP cold step store every mapped architectural register, clean or not. The pin preload must raise
`max_vreg_`: the allocator trusts a mapping only for `v <= max_vreg_`, so an op that reads a pin before anything else is
allocated (`scvtf s0, w1` at a loop head) would read the stale `cpu.regs` slot. The back-edge stores every dirty, unpinned
architectural vreg (`store_dirty_unpinned_arch_regs`) before its slot. `jit_selfloop_pins` and `jit_fp_ldst_cache` cover
both.

A pinned register that the loop updates (`add x0, x0, #1`, `add w3, w3, w4`, `madd x3, x9, x8, x3`) is updated in its pin.
The IR is `LOAD_REG t <- a; OP s <- t, y; [ZEXT 32]; STORE_REG a <- s` and the generic path computes in a scratch copy of the
pin and moves the result back, three instructions on the loop-carried chain. `scan_pin_inplace` marks an `ADD`/`SUB`/`AND`/
`OR`/`XOR`/`MUL` of exactly that adjacent shape (the ZEXT only when `zext32_fused_` fused it into the op); the compile loop
emits it as `a <- a OP y`, the existing `dest == src1` form, on the pin, skips the `STORE_REG`, the `ZEXT` and a `LOAD_REG`
that only the op read, and gives readers of the new value after the `STORE_REG` a copy of the pin. Nothing sits between the
op and the end of the guest instruction, so no fault or exit can see the new value early. Readers of the old value before the
op keep their `LOAD_REG` copy. An `ADD` with `dest == src1` never moves its result into a dead second operand's register
(the pin would lose the value). `BIFROST_NO_PIN_INPLACE=1` goes back to the scratch copy. `jit_pin_inplace` covers it.

### Loops with CBZ/CBNZ exits

A loop that ends in a branch back to its first instruction, and whose only other branches are conditional ones (`cbz`, `cbnz`, `tbz`, `tbnz`,
`b.cond`) leaving it (a null-terminated list walk, a search that stops on a match), is one block with mid-block exits. `side_exit_loop_extent`
(`Compiler/SideExit.cpp`) scans from the block start when `translate_block` meets the first such branch; every other branch, a call, a return or an exit whose target lies inside the loop
leaves the loop split as before. Each exit is a `BRCOND_ZERO`, `BRCOND_BIT` or `BRCOND` with `aux = 1`. `compile_cbz_side_exit` and `compile_bit_side_exit` commit NZCV, test the
register and emit a `jcc`; `compile_cond_side_exit` picks the condition code as `BRCOND` does (a single-flag test in memory, or host flags with a CF
inversion that it undoes right after the `jcc`) and records the flag state at the branch, and the tail commits NZCV from it. A `b.cond` right after an
`FP_CMP` (raw `ucomis` flags) stays a block end. In every case the block goes on. The exit itself is emitted after the block from the allocator layout captured at the branch (the deferred-guard
mechanism): it stores every mapped guest register, clean or not (a loop pin is ahead of memory), writes the pinned vectors back and takes the
chain slot of `emit_taken_path_epilogue`. The loop's pins then cover the whole loop, so a list walk keeps its pointer and key in host registers.

Each exit's slot is published as a stub entry in `blocks_` under `side_exit_key(pc, index)` (bit 63 set, so no guest pc collides), with the exit
target as its only successor. Patching, `back_refs_`, MT cells (`edge_source_override_` files them under the stub key), unpatching, invalidation and
retirement then treat it as any block with one successor; `retire_blocks_locked` retires a block's stubs with it. A block with a mid-block exit runs
a different number of instructions per entry, so it is marked `has_call` (differential replay skips it), is not recorded in the persistent cache,
and is never formed under `BIFROST_JIT_VERIFY`. `BIFROST_NO_SIDE_EXIT=1` turns the formation off. `jit_side_exit` (`test_jit_side_exit.sh`) checks
six loop shapes against C, a fault inside a loop (the signal frame shows the finished iterations) and five threads; `[JIT] side-exit blocks`
in `-v` shows how many blocks were formed. On CoreMark it removes 1.8% of host instructions.

### Carry formation (`BIFROST_CARRY=1`)

The mid-block exits above also let a block go on past an ordinary forward conditional branch. With `BIFROST_CARRY=1`, `translate_block` treats every
forward `cbz`, `cbnz`, `tbz`, `tbnz` or `b.cond` (taken target above the branch) as an exit when the block has room (at most six such exits, and two
instructions of the cap left): the not-taken side is the next instruction in memory, so the block stays contiguous and its code words stay one run,
and the registers the code between the two blocks used to store and reload stay in host registers. The direction is the static fall-through, not a
profile. Backward branches still end a block. It is not used while the persistent cache records (or checks a recording), because a block with a
mid-block exit is not recordable. It is off by default: on CoreMark it removes 2.0% of host instructions and 4.5% of cycles (the best of five
alternating runs), and a 100 s SpongeBob start-up showed no crash and about 10% less process CPU, but that pair was not repeated and the exits are
taken often enough in game code (the cold share of JIT samples went from 0.3% to 2.9%) that a direction chosen from samples is the next step.
The registers that stay in host registers across a block edge are handed on by [register passing](#register-passing-bifrost_carry1).

### Following the hot side of an exit (`BIFROST_CARRY=1`)

Carry continues a block along a forward branch's fall-through and makes the taken side an exit. When the branch is taken more often than not, the
common path leaves through an exit tail and the code it goes on to runs in a block of its own. So a block that has exits Carry formed starts with a
profile (`CarryProfile`, `Carry/HotSide.cpp`): an entry budget of 1,024 (`sub dword [rax], 1; jle trip` at the entry all entries share, after the register
entry's loads) and a count of taken exits in each exit's tail (`add dword [rax], 1`). When the budget runs out the block leaves for the
dispatcher before it runs, which is safe because nothing is dirty at an entry (`emit_carry_trip_stub`, `carry_trip`). `carry_trip` takes the first
exit taken more than half of the time it was evaluated (exit i is evaluated only when the exits before it were not taken), records its branch pc in
`carry_follow_taken_`, and retires the block, keeping its predecessor list so the new translation is chained into again.

The next translation of that block flips the branch: the IR op is rebuilt with the inverted condition and the fall-through as its target, so the
exit leaves on the side the branch used to continue on, and formation goes on at the branch's target. That code is not contiguous with the block's
start, so the block takes the inlined-leaf path (`has_inlined_leaf`: a range of its own for invalidation, no recorded code words, its next pc is where
formation stopped), and it is copied in even where a block already starts there (`split_skip_pc`). A flipped branch can flip back when its new exit
turns out hotter (at most two toggles a branch, three rounds a block); after that the block is translated without profile code (`carry_done_`). A block
translated again after its profile also goes on through code that has a block of its own by now (`merge_known`), as the first translation did,
because otherwise the later translation comes out shorter than the one measured. A flipped block that stops at the two-call limit (`MAX_BL_CALL_PER_BLOCK`)
takes its next pc from `cur_pc`, which is moved past the call before the loop is left. `BIFROST_NO_CARRY_FOLLOW=1` turns the profile and the flip off.

A mid-block `cbz`/`tbz` exit commits NZCV at its test; that store is skipped when both the exit target and the code after the branch overwrite the flags
first (`exit_flags_dead`; for a flipped exit the code after the branch is the branch's target, `carry_continuation`), and the tail of a `b.cond` exit
skips it when its own target overwrites them. The guard-cover scan steps over mid-block exits: a later access to the address of an earlier one needs no
compare of its own on the path that went on.

### Register passing (`BIFROST_CARRY=1`)

A chain edge used to hand over nothing: the predecessor stored what it wrote, and the successor loaded the registers it reads first. With
`BIFROST_CARRY=1` the predecessor also leaves clean copies of guest registers in host registers at each chain slot, and a successor that has a
register entry, its normal entry minus the loads of the registers in its entry layout, is entered there when the copies cover every one of
them. Memory stays authoritative (a copy never gets ahead of `cpu.regs[]`), so fault recovery, signals, the dispatcher and IBL entries see what
they always did, and `BIFROST_NO_REG_PASS=1` turns it off.

- **Copies.** A copy is a clean mapping of the guest register's own vreg. A temporary loaded from a register, or stored to one, keeps its host
  register when it dies instead of being killed (`rp_release`), as long as that register is one of `RP_HOSTS` (R12-R15, R9, R8, RDX, RCX; never
  RAX, which carries the next pc, nor R11, the MT cell jump's scratch). A `LOAD_REG` of a register mapped clean takes the host register over. The
  allocator evicts a copy first and for free, since it is clean and dead.
- **Layouts.** A layout is up to seven (guest register, host register) pairs. Each slot's layout is captured where the slot is emitted: the
  fall-through slot, the taken slot, and the tail of each mid-block exit, which uses the state captured at its branch. Layouts and the entry
  offset live in `reg_pass_`, indexed by `BlockEntry::reg_pass`.
- **Entry set.** A block that is not a loop (pins own R12-R15) loads up to four registers it reads first at its entry (`rp_select_entry`), each in the
  host register its already translated predecessors leave it in (a register none of them holds is loaded lazily as before), else the host register its
  translated successors expect, else the register's canonical host register. The pc store of a block that calls out, the statistics count and the vector
  loads sit after the entry loads and run on both entries; the block trace and a native libc entry sit before them, so such blocks have none.
- **Partial entries.** An edge whose slot holds only some of the entry registers is patched to a stub of the successor that loads the missing ones
  from `cpu.regs[]` and jumps into the register entry (`rp_emit_stubs`; `RegStub` in `reg_stubs_`). The successor emits one stub for each different
  missing set among its translated predecessors (at most four), and an edge takes the stub that loads the fewest registers and covers what it lacks;
  an edge nothing fits enters the plain entry. `BIFROST_NO_REG_PASS_PARTIAL=1` turns the stubs off.
- **Pass-through (`BIFROST_REG_PASS_THROUGH=1`, off by default).** A block that does not call out may also load up to two registers it never touches,
  only to hand them on: the ones its translated successors read first, then the ones every predecessor slot holds in one host register. They
  stay mapped to the exit, so a register read three blocks down needs no load in the two between. It is off because it measured as a loss
  ([history](archive/JIT-history.md#register-passing-partial-entries-and-pass-through-2026-11-24)): the plain and stub entries pay the extra loads.
- **Results.** The temporary that is finally stored to a guest register is defined in the host register its translated successors expect
  (`rp_prepare_hints`), when that register is free or holds a copy nothing reads again.
- **Patching.** `chain_target_for` picks the entry at every patch site (`try_chain_block`, `chain_back_references`, `refresh_mt_edges`): the register
  entry when the slot's layout covers the entry layout exactly, the plain chain entry otherwise. A later translation of either block patches again, so
  a layout never outlives the code it describes. MT cells hold the chosen address like any other target.
- **Where it does not apply.** Loops (pins own R12-R15, and their entry loads run once per entry), blocks that call out before their reads, `BIFROST_JIT_VERIFY`, the persistent cache while it records, indirect
  exits (IBL probes enter the plain entry), calls and returns.

`jit_reg_pass` (`Tests/Unit/jit_reg_pass.c`) runs block chains, a diamond, register moves, a call in a chain, a merge and a faulting loop against C,
on one thread and four. Figures and the attempts that came before are in
[the history](archive/JIT-history.md#register-passing-between-chained-blocks-2026-11-24).

### Peepholes

Tier-1 decides each with a pre-scan and the emitter re-checks it:

- `LDRSB`/`LDRSH`/`LDRSW` are one sign-extending load (`sext_load_`).
- `sxtb`/`sxth`/`sxtw` through `SBFM` are one `movsx`/`movsxd`.
- A 64-bit `lsl #1..3` read only by the next `ADD` becomes that `lea`'s index scale (`scaled_add_`; one `LOAD_REG` may sit
  between them, as the translator emits for `[xN, xM, lsl #s]`).
- A register read only as the second operand of the next `CMP` is `cmp r, [cpu.regs]` (`lazy_load_`).
- A constant read only by the `STORE_REG` right after it (`mov w0, #0`) is stored as `mov qword [slot], imm32` and never put in a register
  (`fold_ahead_kind_` 4). The pre-scan marks it only for an eager store (a block that defers its stores keeps the register form), a GPR or SP
  destination, and a constant that survives a sign-extended imm32; the `STORE_REG` case re-reads the marker. About 0.9% of CoreMark's host
  instructions.

The last two leave a value unmaterialized for one op; `materialize_scaled` and `materialize_lazy_load` emit the original
instruction for any consumer that does not fold it, and the Tier-1 loop calls them before every other op. W-form fusion and
`sext_load_` come from `scan_w32_and_sext_loads`; `fp_load_`, `lazy_load_` and `scaled_add_` need the per-op materializers
of the Tier-1 compile loop. The kill-list scan stops at the highest vreg the block uses.

### Conditional compares

`CCMP`/`CCMN` keep both operands where the allocator holds them and fold a constant second operand. The condition comes
from the host flags, from one bit of `nzcv_host` (EQ/NE/CS/CC/MI/PL/VS/VC) or through a reload that clobbers only RAX. The
true path is `cmp` (`CCMN`: `mov` + `add` into a scratch register); the false path sets the immediate NZCV with
`mov eax, image; add al, 0x7f; sahf`, inside `push rax`/`pop rax` when RAX holds a value. Both paths leave the flags live in
the host in the op's carry convention and store nothing. AL and NV always compare. `jit_ccmp` covers every condition against
a C model that also passes under QEMU.

### Widening multiply-accumulate

`SMADDL`, `UMADDL`, `SMSUBL`, `UMSUBL` and their aliases (`smull`, `umull`, `smnegl`, `umnegl`) have no IR op. The translator emits
`SEXT` (signed) or `ZEXT` (unsigned) of 32 bits for both W operands, a 64-bit `MUL`, then `ADD` or `SUB` against Xa; `load_arm_reg`
reads XZR as the constant 0, which the optimizer folds. The low 64 bits of the product are the same for the signed and the unsigned
reading once both operands are extended, so one `MUL` serves all four. The allocator, the immediate forms and the pin updates see
ordinary ops, and nothing is flushed around them. `jit_maddl` covers every form against a reference that does not multiply with a
widening instruction (GCC compiles that into the instruction under test)
([details](archive/JIT-history.md#widening-multiply-accumulate-is-extend-multiply-and-add-2026-11-23)).

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

Tier-1 does not run this pipeline by default (`BIFROST_NO_SSA=1` keeps it off even with `BIFROST_TIER1_SSA=1`): the adapter was a third of
translation time for 0.6% fewer instructions and about 2% fewer cycles on CoreMark
([measured](archive/JIT-history.md#translation-cost-no-tier-1-ssa-no-store-footprints-outside-verify-flat-vreg-tables-2026-11-23)).
`BIFROST_TIER1_SSA=1` runs it on supported scalar blocks before the existing constant folding passes (`BIFROST_NO_FWD=1` and FWD
range restrictions bypass its forwarding); the legacy forwarding cache does not run again on an SSA-lowered block.
`BRCOND_SKIP`, direct FP/SIMD operations, calls/helpers, and atomics retain their
existing pipeline until their operand and state effects are modeled explicitly.
This is scalar value SSA, not MemorySSA or a complete FP/vector conversion.

The general SSA destructor also supports parallel edge copies and splits
critical edges. `make test-ir-ssa` executes independent non-SSA IR oracles for
diamonds, cyclic loop-carried swaps, critical edges, architectural-state phi
lowering, width-sensitive value numbering, and GPR/FP namespace separation;
malformed graphs must fail validation. `make verify` runs this host test before
the differential/threaded gate. These tests cover the scalar contracts above, not arbitrary FPCR/FPSR,
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

A Tier-1 access whose address an earlier access of the same block already checked emits no gate compare
(`scan_covered_guards`, `guard_cover_`): the same base value and offset, a width no larger, only straight-line scalar ops
between them (`ldr x3,[x19]; str x4,[x19]`), and no `BRCOND_SKIP` in the block. The access is still a registered fault site
with its fault exit; only the compare, its scratch register and the software page checks are gone. The earlier access
(`guard_checked_`) must itself have emitted a gate compare or been covered. The gate is raised without a handshake, so an
access past its check can already run after the gate closes; the second access joins that window for the few ops between
the two. An acquire load keeps its alignment check.
`jit_guard_stubs` runs the shapes that are covered and those that must not be (a wider or displaced second access
crossing into a missing page).

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
FMINNM branch on unordered inputs instead, and fix equal operands (the signed
zeros) inline as the vector forms do: the operation runs in both operand orders
and the results are ANDed (max) or ORed (min), so a clamp to zero stays native.
FMAX/FMIN are native instead of CALL_INTERP. `emit_fp_cold_tail` places the
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
- **Compares.** A scalar `fcmp` whose next op is a `BRCOND` on any condition but AL/NV leaves the raw `ucomis` flags in the host
  (`fp_raw_flags_live()`, valid only for that next op) and the branch is one `jcc`: GE `jae`, LT `jb`, GT `ja`, LE `jbe`, VS `jp`,
  VC `jnp` after `ucomis a, b`, and HI `jb`, LS `jae`, MI `ja`, PL `jbe`, CS `jbe`, CC `ja` after the swapped `ucomis b, a`
  (`fp_raw_flags_cc`; unordered sets CF, ZF and PF, which each of those reads as ARM does). EQ and NE are `je`/`jne` behind a
  `jp`, since unordered sets ZF too: EQ is `jp over; je taken`, NE is `jp taken; jne taken`. An exit whose successor reads
  NZCV converts the raw flags itself (`materialize_fp_raw_flags_to_pstate`: the sequence below, then the pack), so a live exit
  costs what the old sequence did and a dead one nothing.
  When `fcmp` feeds an integer select or a branch that does not qualify, `FP_CMP` leaves its result in the host flags as
  `SUBS` does: `seta`, `setb` and a `cmovp` pick two small integers, and one `cmp` of them leaves the flags a subtraction
  would (greater 1-0, equal 0-0, less 0-1, unordered 0x80000000-1). A compare feeding `fcsel`, `mrs` or anything else keeps
  the branchy sequence, which jumps on PF, CF and ZF directly; lazy flags there were slower on a clamp loop. `FCCMP` is
  unchanged. `jit_fcmp_flags` checks every pair of special values, both precisions, all forms and consumers, and `jit_fcmp_branch` every
  condition against every outcome with live, dead, `fcsel`-reading and `fccmp`-reading exits and loop back-edges.
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
  tells the vector allocator so (`vec_inst_effects`, clobber set 0xF007). Single-lane forms,
  replicate forms other than `LD1R {Vt.T}`, `.1D` and wrapping lists use the `native_structure` helper. Only `Rm` = 31
  selects the immediate post-index; X30 is a register. `jit_neon_struct` builds every combination at run time against an
  independent model that passes under QEMU.
- **LD1R {Vt.T}** (one register, not `.1D`) is a guarded `LOAD_MEM` of the element and the native `SIMD_DUP`, then the
  post-index update; the base moves after the load, so a fault leaves Vt and Xn unchanged. LD2R-LD4R and `.1D` keep the
  helper. `jit_ld1r`.
- **Q loads and stores** (`SIMD_LD16`/`SIMD_ST16`, one register) take the scalar guarded path at width 16
  (`compile_guarded_vec16`): the allocator's registers, one window-table compare, a deferred tail and one MOVUPD into the
  pinned XMM or through XMM0. A 16-byte access can cross a page in bytes 8-15, so its tail keeps its own page checks. Pairs
  keep the older path. `jit_guard_stubs`.

## Android interpreter and generated-code contracts

Android data pointers carry a tag in bits 63:56 (Bionic's allocator uses top-byte tags even without MTE), which data accesses
ignore (`Memory::data_address`). A Tier-1 memory guard copies the address (adding the offset), clears the tag with one
`and reg, [window_base - 16]` against a mask word in the window's header page, then bound-checks and accesses; the guest register
keeps its tag. Tag 0, 2, 0xb4 and 0xff are compared with the interpreter, with no interpreter steps, in `android_loader_host`.

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

## Saturating narrowing shifts

`SQSHRN`, `UQSHRN`, `SQRSHRN`, `UQRSHRN`, `SQSHRUN` and `SQRSHRUN` (vector forms) are native for 16- and 32-bit source lanes
(`SIMD_SHRN_SAT`, `Widening.cpp`). They used to be left to the interpreter by the translator itself, which a game's video
decoder exposed: `sqrshrun v.8b, v.8h, #7` is the last step of a fixed-point pixel conversion, and the interpreter ran it
for about 7% of the process. The destination lane size comes from `immh`, the source lanes are twice as wide and all of
them are read (`Q` picks the destination half only: 0 writes `v_lo` and clears `v_hi`, 1 writes `v_hi` and keeps `v_lo`),
and the shift is the source bits minus `immh:immb`.

The result is the exact `(x + round) >> shift`, saturated to the destination, so the rounding must not wrap in the source
lane. The emitter uses `(x >> s) + ((x >> (s-1)) & 1)`, which equals `(x + 2^(s-1)) >> s` for an arithmetic shift (signed
source) and a logical shift (unsigned source) and cannot overflow. The pack then saturates (`packsswb`/`packssdw` for a
signed destination, `packuswb`/`packusdw` for an unsigned one; an unsigned source clamps with `pminuw`/`pminud` first).
FPSR.QC is set when any lane saturates, found by comparing the shifted lanes with their saturated form
(`emit_simd_qc_unless_equal`). 64-bit sources (immh >= 4) and, without SSE4.1, the forms that need `pminu*` or `packusdw`
stay on the interpreter; `BlockAnalysis` predicts exactly that. `jit_shrn_sat` covers every subop, shift and half and
`Vd == Vn` against an exact model that matches QEMU; `jit_simd_sat_narrow` covers the JPEG IDCT shapes.

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
rewrites of published instructions; its chaining uses
atomic data cells. Code is not recycled while another CPU can execute it.

`BIFROST_STATS_PERIOD` reports cache usage, capacity, budget, growth and
overflow counts. The cached settings `BIFROST_JIT_CACHE_MB`
(default 512, the budget before the JIT is replaced) and `BIFROST_JIT_CACHE_INITIAL_MB`
(default 64) accept 1..1024 MiB; initial capacity is clipped to the budget.
`Scripts/Testing/test_jit_cache.sh` checks a three-thread guest with a forced
1 MiB budget (repeated renewals, exact results) and a growth run (no renewal,
no repeated translation of cached fallback targets).

Range invalidation queries an ordered, conservative index from 256-byte guest
granules to the PCs of published blocks whose own or inlined dependency
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
bytes per guest instruction; leaf-inlined blocks record
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
  current instance once its own is retired (`retarget_scope`); helpers called from generated code
  use `FrostJIT::running`. Under `BIFROST_NO_CHAIN_SKIP=1` the program runs inside nested frames, so only the
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
- **Off when** `BIFROST_JIT_VERIFY`, leaf inlining, `BIFROST_MALLOC_INTERP` or block tracing is on. Blocks with
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
`Scripts/Testing/test_jit_disk_cache.sh` (`run_tests.sh` entry `jit_disk_cache`).

## Fork children keep the JIT

A fork child keeps its parent's JIT and translations, so a Zygote child, which never execs, runs warm code.

- `fork_guest` adds every live instance's locks to the set it takes all or none before the host `fork()`
  (`FrostJIT::try_lock_for_fork`: the block table's read/write lock, and the compile and compile-invalidation locks).
  Holding them, no other thread is translating, publishing or invalidating, so the child's copy of
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
R10 is loaded once by the cold prologue and stays valid along a whole chain (chain entries do not reload it), so a call that returns into
generated code must restore it.
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

## Multithreaded chaining

### Bounded MT self-loops

Tier-1 conditional branches to their own block now retain the current frame,
eligible GPR pins and cached vectors in MT mode. The back-edge uses an immutable
guard and an atomic cell pointing to this translation's body, bypassing prologue
reloads. It checks the callback stop PC and the preemption epoch; expiry or revocation commits carried state and returns to dispatch.
Range/full invalidation clears the cell. Target refresh
never redirects a body cell into a different translation's frame or register
layout. Cells and retired code retain the existing lifetime policy.

BRCOND commits NZCV before the MT guard because its checks overwrite host
flags and any iteration may become an exit, unless the loop head overwrites
all four flags first (see [Dead NZCV stores at block exits](archive/JIT-history.md#dead-nzcv-stores-at-block-exits-2026-10-20)). The ST-only
loop-carry shortcut remains ST-only. CBZ/CBNZ and TBZ/TBNZ preserve their pre-branch NZCV contract.
`BIFROST_NO_SELFLOOP=1` restores ordinary loop edges; `BIFROST_NO_PIN=1`
disables GPR pinning, and `BIFROST_NO_CHAIN=1` also disables MT body edges.
Both frame layouts support this path.

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
  (`run_block`, `jit_call_helper`) calls `jit_grant_lease(cpu)`, which records the epoch in
  `CPU::jit_epoch_seen` before native code starts. MT back-edges and IBL probes only load both values
  (`emit_preempt_poll`: 2 loads and a `jne`, no store); a mismatch takes the existing cold exit to the dispatcher.
  A bumped epoch also revokes in-flight loops of retired code, as the cell clear did. The thread loops, the SDL
  thread loop and the main loop drain host and cross-thread signals on every tick, since native loops now return
  once per tick rather than every few thousand edges. The edge budget stays for ST code.
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

### MT publication

Before the second guest CPU starts, the JIT retires legacy single-thread
instruction patches and purges the table. New MT blocks use immutable
load/test/jump instructions and naturally aligned, lock-free atomic target
cells. Release publication changes a cell, not an instruction displacement;
x86-64 loads provide acquire ordering. Fall-through and taken edges share the
mechanism. Direct BL targets use the same cells (see the preemption section
above), so invalidation cannot leave an embedded stale callee address.

Invalidation clears incoming cells and permanently retires outgoing cells.
Executable code and its metadata remain allocated for the JIT lifetime;
reclamation, persistent translations and unsynchronized guest code writes
remain outside this mechanism. A pointer already acquired by another CPU may
finish its current iteration; guests must still synchronize code changes and
instruction-cache maintenance.

`Tests/Unit/jit_mt_loops.c` checks eight concurrent CPUs against a closed-form
arithmetic oracle, invalidates unchanged live code, rewrites a loop's non-head
page between synchronized runs, replaces that page with MAP_FIXED, and churns
data mappings. `Scripts/Testing/test_jit_mt.sh` additionally requires MT edge
publication and checks the no-chain and ordinary-frame modes.
`Scripts/Testing/test_jit_mt_publication.sh` snapshots published MT code bytes
before eight host CPUs run a loop, then checks that range/full invalidation and
retranslation preserve the snapshot. It also checks epoch exits. The
verification gate runs the guest concurrency fixture as an independent oracle,
not shared-memory differential replay. These checks do not establish a Doom 3
FPS improvement; measure representative gameplay separately.

## Host-native memcpy, memmove and memset

A guest `memset` of tens of kilobytes is a loop of guarded vector stores; in a Minecraft Weekend profile the chunk mesher's
`memset` was the hottest translated block (about 19% of the translated-code samples). The loader records where the guest's
`memcpy`, `memmove` and `memset` start, and a large call runs the host's routine instead.

- **Registry.** `NativeLibcRoutines` (`System/Core/NativeLibc.*`, a member of `Emulator`) maps an entry pc to a kind. It has
  three sources. `DynamicLinker::index_symbols` records the winning symbol, with an ifunc already resolved to its
  implementation, so glibc's `__memcpy_*` variants are the recorded addresses. `register_symtab_routines` reads an
  unstripped static executable's `.symtab` (functions named exactly `memcpy`, `memmove` or `memset`; IFUNC symbols are
  skipped, their value is the resolver) when the main image loads and again at `execve`, which clears the registry first.
  And for a libc the guest's own linker loads (Android's Bionic, which the `DynamicLinker` never sees), `Memory::map_file`
  calls an exec-map hook after it maps a file executable; for a file named `libc.so[.*]`, `libc.musl-*` or `ld-musl-*`,
  `register_mapped_routines` reads the file, finds the load base from the program header the mapping starts in, and records
  the routines of `.symtab` and `.dynsym` inside that segment: `memcpy`, `memmove` and `memset` by name and Bionic's ifunc
  targets (`__memcpy_aarch64`, `__memmove_aarch64_simd`, ..., never the `_chk` variants), each only after its 32 bytes in
  guest memory equal the file's. A memcpy and a memmove at one address (Bionic's are one function) record as memmove.
- **Block entry.** `translate_block` calls `emit_native_libc_entry` at the chain entry of a block that starts a registered
  routine, where nothing is cached: `cmp x2, kNativeLibcMinBytes; jb skip`, store `cpu.pc`, call `jit_native_libc`, and when
  `cpu.pc` changed (the helper left it at x30) take the dynamic exit `CALL_INTERP` uses for a changed pc, which returns to the
  dispatcher with RAX = the return address. A call below the threshold, or one the helper declines, falls into the block's
  own code, which is the guest routine. It is not emitted under `BIFROST_JIT_VERIFY` (replay would re-run the guest routine
  for one block and diverge), slow stores or store tracing, or a shifted proof compile (the registry
  lookup misses there, so the block differs and the disk cache never reuses it), or `BIFROST_NO_NATIVE_LIBC=1`.
- **The helper** (`NativeLibcRoutines::run`) declines unless `n >= kNativeLibcMinBytes` and every byte of every range is
  accessible with the right permission (`Memory::guest_to_host_ptr`: mapping, protection, userfaultfd state, window bound,
  Android pointer tags), and a `memcpy`'s ranges do not overlap. Nothing is written before all the checks pass. So every
  fault, userfaultfd page and protection error still happens in the guest routine, at the guest's addresses. Done, it sets
  x0 to the destination and `pc` to x30.
- **The threshold** is 1 KiB: the exit and the call cost about 33 ns, the guest routine ties them at 512 bytes and loses
  from 1 KiB up ([measurements](archive/JIT-history.md#host-native-memcpy-memmove-and-memset-2026-11-16)).
- **Not covered.** A stripped libc or static executable (nothing names the implementations; glibc's `libc.so.6` is
  stripped, so `--guest-ld` glibc finds nothing), and a dynamic program run through `execve` under the native linker
  (the guest `ld.so` links it, and only a libc with a symbol table is recognised). A guest thread that unmaps or protects a
  range while another thread's native call is inside it faults in the host, not in the guest (the guest routine would fault
  at its own address). `jit_native_libc` (single thread, run by `make verify` too), `test_native_libc_mt`,
  `test_jit_native_libc.sh` and, for Bionic under `linker64`, `Scripts/Android/test_android_native_libc.sh` cover it.

## Planned codegen: retaining guest state across regions

This is the proposed direction, with milestones and targets in [the roadmap](roadmap.md#jit-codegen-retain-guest-state-across-hot-regions).
Nothing in this section is built (its first piece that runs, passing registers along chain edges between Lean blocks, is
[Register passing](#register-passing-bifrost_carry1)). **Lean** is the working name for today's first JIT (what these docs still call Tier-1: cheap
translation and chaining); **Carry** is the planned second JIT. The names are used for new work only; Tier-1 has not been renamed.
Tier-2 regions, the only code that fused blocks, were removed on 2026-11-24
([why](archive/JIT-history.md#tier-2-regions-removed-2026-11-24)); Carry starts again from Lean blocks. Scalar SSA does not yet model
complete vector/FP architectural state.

The target is the guest-register traffic at block edges: Lean loads each live-in register at block entry and stores each written one at
exit. A simulation on CoreMark puts that at 0.93 moves per guest instruction, and an oracle choice of the 4 to 8 busiest registers per
window removes 63 to 84% of them ([the measurement and its limits](archive/JIT-history.md#carry-ceiling-guest-register-moves-in-coremark-2026-11-24)).
It is an upper bound on one small workload, not a promise.

A run of SpongeBob through loading into the lobby found the hottest 200 blocks holding 27% of the JIT samples and loops of several blocks only 1.4%
([numbers](archive/JIT-history.md#carry-loop-report-on-a-game-and-chain-edge-carrying-2026-11-24)), so loop regions alone would reach little of
that workload; merging a block into its single predecessor (a unit that loads and stores each register once) removed 26-31% of CoreMark's
register moves in a simulation and needs no profile. Which of the two comes first is open until a steady-gameplay profile exists.

Rules the design follows (decided 2026-11-24):

- **Cheap hotness.** No counter in block entries (Tier-2's hot and branch counters cost CoreMark 18-29%, and its entry counter was a `lock xadd` in MT). Hot blocks
  come from the host-pc samples the `BIFROST_PROF` sampler already takes, or from a counter on back edges only. Admission uses the region's
  static shape and a cheap check afterwards; there are no timed A/B trials and no sampling copies.
- **Region shape.** Loop trees or small CFGs, not linear traces: Tier-2 declined loops whose iterations take different arms, loops with an
  inner loop and dispatch loops, and raising its block cap alone admitted none of them.
- **Register-resident.** A region keeps its own working set of guest registers in host registers across blocks and back edges. Joins use
  edge-copy moves (parallel copies, including cycles). Dirty guest registers are written to `cpu.regs[]` only at exits, calls, helper
  boundaries and fault stubs. A second entry accepts a host-register layout; the canonical entry loads from `cpu.regs[]`. RBP stays the
  chain-skip frame base: freeing it for an eleventh register gains little for the risk.
- **Recovery.** Every fault site, helper call, signal boundary and interpreter step needs a map from carried host registers to guest
  registers, so its stub commits exactly what the instruction needs. Lean's per-guard `GuardState` is the starting point. The stubs grow
  the code, so the code-cache budget is a design input.
- **Helper effects.** Record each classified helper's guest reads and writes and its host-register clobbers; save and reload only what is
  proven necessary. Unknown helpers keep full synchronization, and exceptions must not unwind through generated frames.
- **MT.** Promotion swaps an atomic target cell, as Lean chaining does. Loops use the immutable back edge with the epoch poll and the stop
  check (`emit_selfloop_stop_entry_check`); exits go through chain-skip cells that enter past the prologue. No stale-pc rings and no trials.
- **Disk cache.** Lean's persistent cache stays on; Carry code is not recorded at first.
- **Tests.** A forced-promotion hook and a fixed hotness threshold, so a fixture promotes on demand and checks results against a closed-form
  or differential oracle. No decision in a test may depend on the clock; timing belongs to one benchmark that does not gate CI.

Milestones: (1) a clean start-up profile exists ([2026-11-24](archive/JIT-history.md#spongebob-profile-with-the-stats-counters-off-2026-11-24):
guest register moves about 37% of samples); a steady-gameplay profile with `BIFROST_PROF=1` alone (the stats counters add instructions to
every block entry) is still wanted; (2) done: sampled hotness and region discovery that only report (the `[carry]` loop report above;
first numbers in [the history](archive/JIT-history.md#carry-loop-report-first-numbers-2026-11-24)); (3) register-resident code for one
region behind forced promotion, with recovery maps, passing `make verify` and the fault/signal fixtures; (4) MT promotion and exit cells;
(5) a game check by the maintainer. Expanding the scope requires measurable benefit and the
[planned correctness/performance gates](archive/TESTS-history.md#planned-codegen-and-coremark-acceptance).
CoreMark's integer/control workload requires its own profile; FP throughput alone does not predict progress toward 10,000 iterations/s.

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

`BIFROST_IR_VALIDATE=1` checks per-op IR contracts.
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
BIFROST_PROF=1 BIFROST_PC_HIST=1 bifrost-emu -v program      # report at exit, with the full JIT stats
BIFROST_PROF=1 bifrost-emu program                           # the profile report alone, at exit
BIFROST_PROF=1 BIFROST_STATS_PERIOD=30 bifrost-emu program   # and every 30 s
```

`BIFROST_PROF=1` samples the host pc at 100 Hz of CPU time (`ITIMER_PROF`) and prints, besides the dispatch / translate /
interpreter / other split, one line for the JIT samples: how many, on how many threads, how many were lost, and how they divide
into the parts of a translated block. **Entry** loads the canonical register layout, **body** is the translated instructions,
**exit** publishes state back, **cold** is recovery and exit code (the same four parts as the `entry`, `tier1-ir`, `exit` and
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

**The report needs no `-v`.** `-v` adds the JIT's own counters (blocks translated and executed, cache use, memory);
the profile report prints at exit whenever `BIFROST_PROF` is set, including when the guest ends through `exit_group`.

**Guest instruction counts.** The `guest: N MIPS` line, the `-v` `executed N instructions` line and the `-v` counters
count what generated code retires, not dispatches. When `-v` or `BIFROST_STATS_PERIOD` is set, the translator emits a
two-instruction counter in every block entry (after the chain-skip entry, so chained edges count too) and in every
self-loop back edge, and the dispatcher folds it into the JIT's totals; without either nothing is emitted and nothing is
paid, so `BIFROST_PROF`'s sampler keeps its profile free of the counters. The counting code runs in the measured run: on
CoreMark it costs about 15 %, so the rate it gives is a floor. The figure is exact for Tier-1 code; blocks served from
the persistent cache (recorded before the counters existed) are not counted.

**Where threads wait.** The sampler counts CPU time, so a thread that waits shows nothing. With `BIFROST_PROF` set, the report
also prints, for each guest thread, how long it spent inside guest syscalls by kind, next to its lifetime and CPU time
(`System/Core/WaitProfile.*`):

```
[bifrost-emu] waiting, 6 thread(s), 3.6 thread-seconds alive, cpu 0.6 s: blocked in futex 0.6 s, poll 0.6 s, sleep 1.2 s; inside io 0.6 s, ...
[bifrost-emu]   tid 3 "w-futex": alive 0.6 s, cpu 0.1% | blocked: futex 99.9% poll 0.0% sleep 0.0% | inside: io 0.0% ioctl 0.0% thunk 0.0% other 0.0%
```

The percentages are shares of the thread's lifetime, counted from its first syscall; the busiest 12 threads by CPU time are listed.
**futex**, **poll** (`ppoll`, `pselect6`, `epoll_pwait`, `wait4`, `waitid`, `rt_sigtimedwait`) and **sleep** (`nanosleep`,
`clock_nanosleep`) are time blocked. **io** (`read`, `write`, their vectored and socket forms, `connect`, `accept`), **ioctl**
(Binder transactions are ioctls), **thunk** (the GL, EGL, SDL and audio thunks, where a buffer swap waits for the display) and
**other** are time inside the call, work and waiting together, so they overlap the CPU share. A thread whose CPU share and
blocked shares are both low is waiting somewhere the report does not see: a lock in the emulator itself, or being scheduled out.
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

### Timeline with Tracy: `BIFROST_PROF=tracy`

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
  ranges.

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
- **Translation time.** The `[JIT] compile:` line of `bifrost-emu -v` gives blocks, milliseconds and microseconds per block. The ART
  bootstrap (`Scripts/Android/test_android_jni.py`, which prints its command) translates about 93 thousand blocks in 1.6 s, so it
  shows translation cost without a window; `perf record` without call graphs groups by function, and per-call-site costs of `memset`
  and `malloc` need a throwaway `LD_PRELOAD` shim because `perf_event_paranoid` may forbid call graphs.
- **Split a ratio to native** into guest instructions over native instructions (the guest compiler's share, which the JIT
  cannot change) and host instructions per guest instruction (the JIT's share).
- **CoreMark.** `Scripts/Bench/run_benchmarks.py` runs 200,000 iterations (`crcfinal` 0x4983, the value QEMU 11.1.1
  prints). A run under ten seconds is invalid, and a new count needs its CRC from QEMU. Fix the CPU governor to
  `performance`. Whole-process cache-miss counts mostly measure startup and translation (the dynamic loader, thunk
  registration), not generated code. CoreMark does not predict game frame rates.
- **Latest recorded figures.** CoreMark 16,880 iterations/s on the shipped build against 8,024 for QEMU 11.1.3 (Ryzen 7
  5800X3D, pinned to one core, 2026-11-23), and 37,951 natively (gcc `-O2`, same host, 2026-11-07); records and methods are under
  [Recorded changes](#recorded-changes). The tables in [PERFORMANCE.md](PERFORMANCE.md) are the published figures.

## Recorded changes

Optimization checkpoints and fixes are in [the history](archive/JIT-history.md), unchanged, newest first:

- [SpongeBob profile with the stats counters off (2026-11-24)](archive/JIT-history.md#spongebob-profile-with-the-stats-counters-off-2026-11-24)
- [Register passing: partial entries and pass-through (2026-11-24)](archive/JIT-history.md#register-passing-partial-entries-and-pass-through-2026-11-24)
- [Carry: flag stores at exits, covered guards across exits, the hot side of an exit (2026-11-24)](archive/JIT-history.md#carry-flag-stores-at-exits-covered-guards-across-exits-the-hot-side-of-an-exit-2026-11-24)
- [Register passing between chained blocks (2026-11-24)](archive/JIT-history.md#register-passing-between-chained-blocks-2026-11-24)
- [Carry formation behind BIFROST_CARRY, and what did not help (2026-11-24)](archive/JIT-history.md#carry-formation-behind-bifrost_carry-and-what-did-not-help-2026-11-24)
- [Loops with CBZ/CBNZ exits become one block (2026-11-24)](archive/JIT-history.md#loops-with-cbzcbnz-exits-become-one-block-2026-11-24)
- [Window register loaded once per chain (2026-11-24)](archive/JIT-history.md#window-register-loaded-once-per-chain-2026-11-24)
- [SpongeBob host profile with perf: dispatch and translation after loading (2026-11-24)](archive/JIT-history.md#spongebob-host-profile-with-perf-dispatch-and-translation-after-loading-2026-11-24)
- [Carry loop report on a game, and chain-edge carrying (2026-11-24)](archive/JIT-history.md#carry-loop-report-on-a-game-and-chain-edge-carrying-2026-11-24)
- [Carry loop report: first numbers (2026-11-24)](archive/JIT-history.md#carry-loop-report-first-numbers-2026-11-24)
- [Carry ceiling: guest register moves in CoreMark (2026-11-24)](archive/JIT-history.md#carry-ceiling-guest-register-moves-in-coremark-2026-11-24)
- [Tier-2 regions removed (2026-11-24)](archive/JIT-history.md#tier-2-regions-removed-2026-11-24)
- [Hotspots from IBS and the allocation profile: tag untag and scalar FMAX/FMIN zeros (2026-11-24)](archive/JIT-history.md#hotspots-from-ibs-and-the-allocation-profile-tag-untag-and-scalar-fmaxfmin-zeros-2026-11-24)
- [FP equality compare and branch on the raw ucomis flags (2026-11-23)](archive/JIT-history.md#fp-equality-compare-and-branch-on-the-raw-ucomis-flags-2026-11-23)
- [Translation cost: no Tier-1 SSA, no store footprints outside verify, flat vreg tables (2026-11-23)](archive/JIT-history.md#translation-cost-no-tier-1-ssa-no-store-footprints-outside-verify-flat-vreg-tables-2026-11-23)
- [FP compare and branch on the raw ucomis flags (2026-11-23)](archive/JIT-history.md#fp-compare-and-branch-on-the-raw-ucomis-flags-2026-11-23)
- [Widening multiply-accumulate is extend, multiply and add (2026-11-23)](archive/JIT-history.md#widening-multiply-accumulate-is-extend-multiply-and-add-2026-11-23)
- [Game profiles: Zenless Zone Zero and Umamusume (2026-11-23)](archive/JIT-history.md#game-profiles-zenless-zone-zero-and-umamusume-2026-11-23)
- [Saturating narrowing shifts are native (2026-11-23)](archive/JIT-history.md#saturating-narrowing-shifts-are-native-2026-11-23)
- [In-place updates of pinned registers (2026-11-23)](archive/JIT-history.md#in-place-updates-of-pinned-registers-2026-11-23)
- [One jump per loop iteration and one check per repeated access (2026-11-16)](archive/JIT-history.md#one-jump-per-loop-iteration-and-one-check-per-repeated-access-2026-11-16)
- [Host-native memcpy, memmove and memset (2026-11-16)](archive/JIT-history.md#host-native-memcpy-memmove-and-memset-2026-11-16)
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
