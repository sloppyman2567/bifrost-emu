# Decoder and reference interpreter

`Decoder/` extracts AArch64 instruction fields for both execution engines.
`Core/` implements integer/control-flow reference execution; `FloatSIMD/`
contains scalar FP and SIMD families. Read [the JIT guide](../../Docs/JIT.md)
before changing shared decode contracts or JIT-facing instruction behavior.

- Preserve input values before destination or writeback updates, including
  register aliases, BLR X30 and faulting pre/post-indexed accesses.
- Unsupported encodings raise DecodeError/SIGILL. Never silence a missing
  instruction with a successful no-op.
- Keep element width, signedness, Q=0 upper-half rules, FP special values and
  rounding explicit. Compare a failing case with Arm semantics or an independent
  reference before relying on agreement between the two Bifrost engines.
- Tests live in `Tests/Unit/`, `Tests/Apps/` and `Tests/Asm/`; run the interpreter
  as well as JIT verification for a shared decoder or semantic change.
- Literal loads use the shared `pc_relative`/displacement contract. Decode-cache
  hits require both the PC and freshly fetched instruction word to match.
- Paired/vector operations capture all inputs before writes; both source vectors
  contribute to Q=0 pairwise integer operations. Do not infer all Q=0 semantics
  from the behavior of a different instruction family.
- FP lanes use the shared ARM semantic helpers for NaNs, signed zero, rounding
  and saturation. Keep the reference implementation independent of native x86
  lowering; agreement between two copies of one bug is not a reference result.
