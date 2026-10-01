# zstd regression

`scripts/run_zstd_regression.sh` compresses a deterministic 1.2 MB corpus with
`ctest_real/zstd-aarch64`, checks every archive with host zstd, and compares the
restored bytes. It runs default and `--single-thread` compression in both JIT
and interpreter modes. It also checks both guest decoders against a
host-generated archive. The host zstd CLI is required.

```sh
./scripts/run_zstd_regression.sh
./scripts/run_tests.sh --filter 'zstd|variable_shift_source|shifted_operand_width'
```

The main JIT suite invokes the host harness through `HOST_JIT`; the harness
checks both engines itself. An interpreter-only suite skips this harness to
avoid running the same matrix twice. `BIFROST_EMU` selects an alternate emulator,
and `BIFROST_ZSTD` / `BIFROST_HOST_ZSTD` select the guest / host fixtures.

## Bugs fixed on 2026-10-01

- **Variable shifts destroyed the count source.** The misleadingly named
  `emit_and_cl_imm8` emitted `and rcx, imm8`, masking the entire cached source
  register to 31 or 63. zstd's Huffman encoder combines packed code bits with
  a count in one value, uses that value as a variable shift count, and then
  ORs the packed bits into its bitstream. Load forwarding reused the damaged
  register and emitted corrupt compressed data. x86 already masks the
  effective count according to the operand width, so the destructive mask
  and unused helper were removed. See Intel's
  [instruction-set reference](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf),
  SAL/SAR/SHL/SHR and ROR, for count masking semantics.
- **W-form shifted operands used a 64-bit shift.** `apply_shift()` specified
  width 32 only for ROR. A shifted-register instruction such as
  `add w0,w1,w2,lsr #24` could therefore pull X2's upper bits into the low
  result. All W-form shifts now carry width 32; constant folding also truncates
  the source and result and interprets bit 31 as the sign for SAR.

`ctest/jit_variable_shift_source.c` checks all four variable shift/rotate
operations at both widths for 256 count values, preserving high packed bits and
bits outside the effective count. `ctest/jit_shifted_operand_width.c` checks
shifted W operands with nonzero upper X bits, including a block-local constant
fold. Both focused tests fail on the pre-fix build and pass after the repairs.

These repairs address the current O2 fixture. Older notes about a separate O3
threading failure are not evidence that every O3 build has been repaired.

Validation: the full JIT suite passed 240/240 with IR validation and register
allocation checks enabled. The focused shift tests also passed with
`BIFROST_JIT_VERIFY=1` and memory verification. Applying those verification
settings to threaded zstd logged divergences and exceeded the harness's
90-second compression timeout; that extra diagnostic run is not a clean pass.
