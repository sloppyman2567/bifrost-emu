# JIT architecture and contributor guide

This guide maps the AArch64-to-x86_64 JIT and records the steps for changing
instruction support without letting decode, translation, optimization, and
code generation drift apart.

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
| Decode AArch64 encodings | `include/decoder.hpp`, `src/frontend/decoder.cpp` |
| Translate decoded instructions to IR | `src/ir/ir_translate.cpp`, `ir_translate_fp.cpp`, `ir_translate_mem.cpp` |
| Define typed IR parameters and factories | `include/ir/ir.hpp`, `src/ir/ir.h` |
| Optimize and validate IR | `src/ir/ir_optimize.cpp`, `src/ir/ir_validate.cpp` |
| Build Tier-1 blocks and patch exits | `src/jit/jit_translate.cpp`, `jit_codegen_branch.cpp` |
| Emit x86_64 for IR operations | `src/jit/frostjit.cpp`, `jit_codegen_*.cpp` |
| Allocate host registers and spill vregs | `src/jit/x86_regalloc.cpp` |
| Dispatch blocks and promote hot regions | `src/jit/jit_dispatch.cpp`, `jit_tier2.cpp` |
| Define reference instruction semantics | `src/interp/interpreter.cpp`, `interp_fp.cpp`, `interp_branch.cpp` |

Tier-2 compiles region IR through the normal IR code generator, with its own
region liveness, loop, and exit handling. An IR operation that works in a
single Tier-1 block can still need region-specific handling when it affects a
region boundary or control-flow exit.

## Adding or changing an instruction

1. Check that `decode()` classifies the encoding correctly and extracts every
   field needed by both execution paths.
2. Keep interpreter semantics correct. Unsupported operations must raise the
   normal guest decode error; they must not silently act as NOPs.
3. Add or update a typed IR factory and reader in `include/ir/ir.hpp`, then
   validate the operation's parameter contract in `src/ir/ir_validate.cpp`.
   IR parameter fields are private: producers use factories, consumers use
   readers, and optimizers must preserve the operation's semantics.
4. Translate through those factories in `src/ir/ir_translate*.cpp`. Do not
   patch `insts.back()` fields after emission; forward branch skip counts use
   `set_skip_count()` for their documented fix-up.
5. Add code generation in the matching `src/jit/jit_codegen_*.cpp` path.
   Check register aliasing, W-register zero-extension, Q=0 upper-half rules,
   host feature gates, and interpreter fallback behavior.
6. Keep the block-splitting prediction in `jit_translate.cpp` aligned with
   fallback behavior. It is a pre-translation heuristic. The final
   interp-only decision and `BlockEntry::call_interp_count` use the
   post-optimization IR `CALL_INTERP` count.
7. Consider both Tier-1 and Tier-2 control flow, then add a focused guest
   regression under `ctest/` or `ctest_real/` and register it in
   `scripts/run_tests.sh`.

Generated operation tables live under `tools/opgen/`; when a family is in a
generated table, use its classifier instead of adding another handwritten
encoding mask. Regenerate generated headers with the corresponding opgen
script rather than editing generated output alone.

## Useful checks

```bash
BIFROST_IR_VALIDATE=1 ./bifrost-emu ctest/some_regression.elf
BIFROST_JIT_VERIFY=1 ./bifrost-emu ctest/some_regression.elf
./bifrost-emu --no-jit ctest/some_regression.elf
./scripts/run_tests.sh --filter 'jit|simd|fp'
./bifrost-emu ctest/coremark.elf
```

`BIFROST_IR_VALIDATE=1` checks per-op IR contracts, including Tier-2 regions.
`BIFROST_JIT_VERIFY=1` compares JIT execution with the interpreter; syscalls,
in-block calls, and memory side effects have documented verification limits,
so pair it with a focused result check when relevant. CoreMark is a useful
compute workload, but it does not replace application workloads or targeted
correctness regressions.

## Conditional compares and register forwarding

CCMP and CCMN only define NZCV flags. Their IR `dest=0` is a placeholder,
not a write to X0. `optimize_ir()` must preserve X0's forwarded value across
these operations; treating the placeholder as a destination lets a later CSEL
read stale architectural state when dead-store elimination removes an earlier
STORE_REG. `ctest/jit_ccmp_forward.c` covers the FreeType rasterizer pattern
that exposed this as missing glyph pixels in Neverball. See
[the Neverball validation notes](neverball.md) for the application repro.

## Shift source preservation and operand width

Variable shifts and rotates must preserve a live count vreg. The x86 CL forms
mask the effective count in hardware; an explicit AND on the cached RCX value
destroys source bits that a later IR instruction may read. Keep every W-form
shift at width 32, including shifted operands produced by `apply_shift()` and
constant folding. See [zstd regression notes](zstd.md) and the
`jit_variable_shift_source` / `jit_shifted_operand_width` regressions.

## Integer vector multiply and Q width

The SSE4.1 lowering of 32-bit vector `MUL` uses `PMULLD`, encoded as
`66 0F 38 40 /r`. The previous `66 0F 38 5F /r` sequence was invalid and
crashed Doom 3 during script initialization. `SimdArithParams::q` carries
the architectural vector width through the factory, translator and codegen:
Q=0 writes the low 64 bits and clears `v_hi`; Q=1 computes both halves.
`ctest/jit_simd_mul32.c` covers overflow, source/destination aliasing and both
widths with fixed expected products, independently checked under QEMU.

## Shared SIMD fallback semantics

Vector FMOV immediates and scalar DUP element reads currently use the
interpreter fallback in JIT mode, so reference bugs affect both engines and
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
TLS entries across CPUs at their next dispatch. Multithreaded mode continues
to disable live-code patching and Tier-2 publication; it does not recycle
code while another CPU can execute it.

`BIFROST_STATS_PERIOD` reports cache usage, capacity, budget, growth and
overflow counts. The cached diagnostic settings `BIFROST_JIT_CACHE_MB`
(default 1024) and `BIFROST_JIT_CACHE_INITIAL_MB` (default 64) accept 1..1024
MiB; initial capacity is clipped to the budget. `scripts/test_jit_cache.sh`
checks a three-thread guest with a forced 1 MiB budget and a growth run,
including repeated interpreter-only calls, exact results, one bounded
overflow and no repeated translation of cached fallback targets.

## Vector unary FP decode distinction

AdvSIMD vector `FSQRT` (`0x2EA1F800`, excluding Q and the S/D bit) differs
from `FNEG` (`0x2EA0F800`) at bit 16. The interpreter's old
`0xFF20FC00` mask discarded that bit and executed square roots as negations.
The unary handler now retains bit 16 and the fixed encoding bits, handles
vector square root explicitly, and preserves Q=0 upper-half clearing. JIT
uses this corrected reference fallback. `jit_simd_unary_fp` checks S/D widths,
aliasing, signed zero, FABS/FNEG separation, and all 512 inverse-square-root
seed calculations used by Doom 3's vectorized math initialization.
