# JIT architecture and contributor guide

This guide maps the AArch64-to-x86_64 JIT and records the steps for changing
instruction support without letting decode, translation, optimization, and
code generation drift apart.

The [source map](ARCHITECTURE.md) describes the subsystem layout and split
boundaries. Compiler prediction/leaf analysis lives in `Compiler/BlockAnalysis.cpp`,
Tier-1 orchestration in `BlockCompiler.cpp` and differential store-footprint
analysis in `VerificationStores.cpp`. The refactor's build/test checkpoint is
recorded in [TESTS.md](TESTS.md#subsystem-layout-and-source-splits-2026-10-11);
it does not establish a new performance improvement.

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
[planned correctness/performance gates](TESTS.md#planned-codegen-and-coremark-acceptance).
CoreMark's integer/control workload requires its own profile; FP throughput
alone does not predict progress toward 10,000 iterations/s.

Primary design references: QEMU's
[block chaining and exception recovery](https://www.qemu.org/docs/master/devel/tcg.html)
and LLVM's [runtime value-location maps](https://llvm.org/docs/StackMaps.html).
These provide design precedents; performance acceptance depends on Bifrost's
own tests and measurements.

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
do not establish game FPS. See [validation](TESTS.md#eight-block-superblocks-and-region-vector-state-2026-10-12).

## Native acquire loads and bounded trial timing (2026-10-12)

Aligned `LDARB/LDARH/LDAR W/LDAR X` now reuse the guarded native-load emitter.
The optimizer retains their original LL/SC IR operation as a memory-effect
barrier; only code generation lowers the non-exclusive load to an x86 load.
Acquire-only blocks initialize the direct-window base at cold and chain
entries. Bounds, sticky permission/userfaultfd checks, fault-site registration
and precise architectural recovery use the existing memory path. Unaligned
accesses exit to the existing interpreter implementation; forced-software
guard/no-fault-recovery configurations retain the helper implementation.
Exclusive loads/stores retain the shared reservation monitor.

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
or FPS result. See [validation and remaining gaps](TESTS.md#native-acquire-loads-and-trial-timing-2026-10-12).

## Indirect branches and host return boundaries (2026-10-12)

With the default chain-skip frame layout, Tier-1 register-branch exits
(`BR`, `BLR`, `RET`) probe a per-host-thread indirect-branch lookup table
(IBL). Its 4,096 direct-mapped entries hold a guest PC and the cached block's
chain-skip entry. Dispatch populates entries; a cold target or hash collision
returns to dispatch. Ordinary frames (`BIFROST_NO_CHAIN_SKIP=1`) and
`BIFROST_NO_CHAIN=1` retain the previous indirect exits.

Generated probes check the JIT's dispatch generation, `jit_stop_pc`, nested
`jit_call_depth` and the shared 256-edge execution lease before jumping.
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
This does not disable the chain execution lease, host call-depth limit, guest
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
second operand. Zero results take the cold path to preserve ARM's signed-zero
ordering. Other finite results remain native. FMLA/FMLS use a single FMA3
operation; hosts without FMA3 use the fused interpreter operation, rather than
separate multiply and add. These changes cover NaN priority/payload/sign,
default NaNs for invalid arithmetic, and FMULX infinity-times-zero results.
FPCR controls, cumulative FPSR flags and enabled synchronous FP traps remain
outside this native arithmetic contract.

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
and restores its original value is not detected across processes. Pair-exclusive
operations and full reservation-granule semantics need separate coverage.

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

These policies reuse the existing buffer emulation, whose broader GL semantics
are not established by this regression. In particular the existing flush code
treats offsets as absolute buffer offsets, while the
[Khronos specification](https://wikis.khronos.org/opengl/GLAPI/glFlushMappedBufferRange)
requires offsets relative to the mapping. The persistent/coherent cases are
existing emulator behavior tests, not ES 3 conformance tests. Those separate
compatibility gaps are not repaired by selecting the GLES policies.

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

Vector FMOV immediates use the interpreter fallback in JIT mode. Scalar
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
single-step interpreter entries without repeating decode and code generation.
Branch-fixup writes check buffer bounds even after an emission overflow.

Call-target lookups honor cached interpreter entries. Shared block-table
reads and publication hold `blocks_mutex_`, with a separate locked lookup for
compiler callers that already own it. TLS caches track their JIT owner and
a globally unique invalidation generation, including negative entries.
Mapping invalidation and the transition to multithreaded execution invalidate
TLS entries across CPUs at their next dispatch. Multithreaded mode disables
rewrites of published instructions; its chaining and Tier-2 publication use
atomic data cells. Code is not recycled while another CPU can execute it.

`BIFROST_STATS_PERIOD` reports cache usage, capacity, budget, growth and
overflow counts. The cached diagnostic settings `BIFROST_JIT_CACHE_MB`
(default 1024) and `BIFROST_JIT_CACHE_INITIAL_MB` (default 64) accept 1..1024
MiB; initial capacity is clipped to the budget. `Scripts/Testing/test_jit_cache.sh`
checks a three-thread guest with a forced 1 MiB budget and a growth run,
including repeated interpreter-only calls, exact results, one bounded
overflow and no repeated translation of cached fallback targets.

Range invalidation queries an ordered, conservative index from guest page to
the PCs of published blocks whose own or inlined/Tier-2 dependency ranges touch
that page. Data-only ranges with no indexed page return immediately; code-page
hits check only the listed blocks with the exact overlap test, instead of the
whole block table. Publication and the query share the compiler lock. Lists may
hold retired PCs; a query prunes PCs no longer in the table from the pages it
visits, and whole-cache clears reset the index. Victims and retirement are
unchanged: the index does not suppress required code invalidation or reclaim
live code. `BIFROST_NO_CODE_PAGE_FILTER=1` restores the full scan for A/B
measurement.

`jit_cache_invalidation.elf churn` warms 32,768 executable functions, times
4,096 data-page protection changes, then rewrites one function 4,096 times with
IC maintenance (a guest JIT publishing onto a page with live code) and checks
each result and the retranslated same-page neighbours. Three same-machine runs
each measured the data-only phase at about 0.007 s (0.58 s without the filter)
and the code-page phase at 1.11 s before the per-page lists and 0.057 s after.
These are invalidation microbenchmarks, not application speedups. A 30-second
`perf` capture of SpongeBob loading under ART had attributed 54% of sampled user
CPU to the old whole-table scan, reached through per-line `IC IVAU`/`DC CVAU`.

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
Tier-2 treats SIMD helpers as calls for GPR pinning and LICM purposes.
The translator checks `classify_native_simd` before the interpreter
fallback, so only classify encodings whose `run<K>` covers every size/Q form
it accepts; across-lane reductions, widening three-different ops and
pairwise long adds are covered by `jit_simd_widen_reduce`. FP lane helpers
(FMAXP/FMINP/FMAXNMP/FMINNMP, FCMxx #0, all seven vector FRINT modes, and
half-precision FCVTL/FCVTN) call the shared `FloatCommon.hpp` semantics
(`arm_fp_binop`, `arm_frint`, `h2f`/`f2h`) that the interpreter uses.
`jit_simd_fp_lanes` checks them against per-encoding hashes generated by
QEMU. FPCR.DN is honoured by FRINT and half conversions only. FP arithmetic
in both engines, including the pairwise forms, ignores DN, FZ and RMode.

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

The thread-local dispatch cache uses 8,192 two-way sets (16,384 entries,
512 KiB per dispatching thread). It is allocated on a thread's first
dispatch and freed at thread exit (pthread key), so host threads that never
run guest code carry none of it. With the previous 1,024 sets, SpongeBob
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

A [native Doom 3 capture](Apps/doom3.md#native-dispatch-profile-2026-10-12)
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
reloads. It checks the callback stop PC and the existing 256-edge execution
lease; expiry or revocation commits carried state and returns to dispatch.
Range/full invalidation clears the cell. Target refresh and Tier-2 publication
never redirect a body cell into a different translation's frame or register
layout. Cells and retired code retain the existing lifetime policy.

BRCOND commits NZCV before the MT guard because its checks overwrite host
flags and any iteration may become an exit. The ST-only dead-flag shortcut
remains ST-only. CBZ/CBNZ and TBZ/TBNZ preserve their pre-branch NZCV contract.
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
edges share the mechanism. Direct BL targets retain helper dispatch in MT
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
boundary without patching or reclaiming live code. A per-CPU 256-edge budget
returns chained execution to dispatch, and callback stop sentinels also end
chains. Both ordinary and chain-skip frame layouts are covered.

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
invalidation and retranslation preserve the snapshot. It also checks the
execution lease. The verification gate runs the guest concurrency fixture as an independent oracle,
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
the generated entry still do not contribute hotness. Conditional exits collect
bounded taken/fall-through counts (4,096 per outcome, atomic increments in MT).
With at least 16 samples, a taken successor with 75% bias is linearized by
inverting the typed branch and retaining the original fall-through as a cold
exit. Head back-edges remain loop terminators. Promotion requires all branches
to be sampled and a product of continuation frequencies of at least 75%.
This is a profitability heuristic, not a measured speedup; multiplying
marginal frequencies can underestimate correlated paths. MT single-block loops are declined
because they fuse no boundaries and cannot retain loop state across entries.
Tier-1 profiles are disabled on promotion and mapping invalidation; their
storage outlives published code. Monitored regions deposit a recovery credit
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
