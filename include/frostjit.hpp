// frostjit.hpp — Experimental block-translation JIT for bifrost-emu
//                (v1.4.0-alpha).
//
// frostJIT is a *limited, experimental* just-in-time compiler that
// translates AArch64 basic blocks into x86_64 machine code at runtime.
// It shares the existing decoder (decoder.hpp / decoder.cpp) with the
// interpreter — both call `decode(DecodedInst&, uint32_t)` to extract
// fields from a 32-bit ARM64 instruction word.
//
// Design goals:
//   1. **Block translation.** A "block" is a maximal sequence of
//      instructions starting at some PC and ending at the first
//      control-flow instruction (B, BL, BR, BLR, RET, Bcond, CBZ,
//      CBNZ, TBZ, TBNZ, SVC, or any instruction the JIT can't
//      translate). The JIT emits a single x86_64 function per block.
//
//   2. **Register bank in memory.** ARM64 has 31 GPRs + SP + 32 SIMD
//      regs; x86_64 has only 16 GPRs. Rather than do complex register
//      allocation, frostJIT keeps the entire ARM64 register file in a
//      `uint64_t regs[32]` array (the CPU struct) and emits load/op/
//      store sequences for each instruction. This is slower than a
//      real regalloc but much simpler and still faster than the
//      switch-based interpreter for tight arithmetic loops.
//
//   3. **Fallback to interpreter.** Any instruction the JIT doesn't
//      support causes the block to end at that instruction, and the
//      JIT returns control to the interpreter for a single step. The
//      interpreter then re-enters the JIT for the next block.
//
//   4. **No FP/SIMD.** Floating-point and SIMD instructions are not
//      translated — they always fall back to the interpreter. This
//      keeps the JIT small and focused on integer workloads.
//
//   5. **Thread-safety.** Each Emulator (and thus each guest thread)
//      gets its own FrostJIT instance with its own code cache. No
//      locking needed.
//
// Supported instructions (v1.4.0-alpha):
//   - ADD/SUB (shifted register, immediate, with/without flags)
//   - AND/ORR/EOR/ANDS (shifted register, immediate)
//   - MOVN/MOVZ/MOVK
//   - CSEL/CSINC/CSINV/CSNEG
//   - CMP (SUBS XZR) / CMN (ADDS XZR)
//   - ADC/SBC (and flag-setting variants)
//   - MADD/MSUB (multiply-add/subtract)
//   - LSL/LSR/ASR/ROR (register form)
//   - LDR/STR (unsigned immediate, 64/32/16/8-bit)
//   - LDR/STR (unscaled LDUR/STUR)
//   - LDR/STR (register offset)
//   - LDP/STP (64-bit, signed offset)
//   - B (immediate)
//   - BL (immediate) — sets X30, jumps
//   - BR (register)
//   - BLR (register) — sets X30, jumps
//   - RET (register, default X30)
//   - Bcond (immediate)
//   - CBZ/CBNZ (immediate, 64/32-bit)
//   - TBZ/TBNZ (immediate)
//   - ADR/ADRP
//   - NOP
//
// Explicitly NOT supported (always fall back to interpreter):
//   - SVC (syscall) — ends block, interpreter handles
//   - All FP/SIMD
//   - All atomics (LDXR/STXR/LSE)
//   - MSR/MRS (system register access)
//   - BRK/HLT (debug traps)
//   - EXTR, SBFM/BFM/UBFM with non-LSL aliases (decoder routes these
//     to InstClass::SBFM/BFM/UBFM; the JIT only handles the LSL alias
//     of UBFM, and leaves other bitfield forms to the interpreter)
//
// Usage from the Emulator:
//   FrostJIT jit;
//   if (jit_enabled) {
//       while (cpu.running) {
//           uint64_t next_pc = jit.run_block(cpu, emu);
//           cpu.pc = next_pc;
//       }
//   } else {
//       // existing interpreter loop
//   }
//
// The JIT is enabled with the --jit command-line flag.
#pragma once

#include "decoder.hpp"
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace arm64emu {

// Forward declarations.
struct CPU;
class Emulator;
class Memory;

class FrostJIT {
public:
    FrostJIT();
    ~FrostJIT();

    FrostJIT(const FrostJIT&) = delete;
    FrostJIT& operator=(const FrostJIT&) = delete;

    // Translate and execute the block starting at cpu.pc. Returns the
    // next PC to execute (which may be a branch target, or cpu.pc + 4
    // if the block ended on a non-control-flow instruction that we
    // couldn't translate and had to delegate to the interpreter).
    //
    // On a cache hit, this just calls the already-translated block
    // function. On a cache miss, it translates the block first.
    //
    // Side effects: updates cpu.regs, cpu.sp, cpu.pstate as the
    // block executes. May invoke Emulator::syscall (for SVC, which
    // ends the block) or the interpreter (for unsupported ops, which
    // also end the block).
    uint64_t run_block(CPU& cpu, Emulator& emu);

    // Statistics for verbose mode.
    uint64_t blocks_translated = 0;  // total blocks translated
    uint64_t blocks_executed   = 0;  // total block executions
    uint64_t cache_hits        = 0;  // executions that hit the cache
    uint64_t cache_misses      = 0;  // executions that required translation
    uint64_t interpreter_fallbacks = 0;  // times we fell back to interpreter

    // Invalidate the entire code cache. Call this if the guest writes
    // to its own code (self-modifying code) — we don't detect that
    // automatically.
    void flush_cache();

    // Current size of the code cache (bytes used / bytes allocated).
    size_t code_buf_used()  const { return code_buf_used_; }
    size_t code_buf_size()  const { return CODE_BUF_SIZE; }
    size_t cache_entries()  const { return block_cache_.size(); }

private:
    // Code buffer: a single large RWX mmap'd region. We bump-allocate
    // from it and never free individual blocks. flush_cache() resets
    // the bump pointer and clears the cache map.
    static constexpr size_t CODE_BUF_SIZE = 16 * 1024 * 1024;  // 16 MB

    uint8_t* code_buf_       = nullptr;
    size_t   code_buf_used_  = 0;

    // Block cache: maps guest PC → translated function pointer.
    // The function pointer has C signature `uint64_t (*)(CPU* cpu, Emulator* emu)`.
    std::unordered_map<uint64_t, uint64_t (*)(CPU*, Emulator*)> block_cache_;

    // ── x86_64 code emission helpers ──────────────────────────────
    // We append bytes to code_buf_ at code_buf_used_.
    void emit_byte(uint8_t b);
    void emit_u32(uint32_t v);
    void emit_u64(uint64_t v);

    // Emit common x86_64 instruction patterns. Register numbering
    // follows the System V AMD64 ABI:
    //   0=RAX, 1=RCX, 2=RDX, 3=RBX, 4=RSP, 5=RBP, 6=RSI, 7=RDI,
    //   8=R8, ..., 15=R15.
    // We use RAX/RCX/RDX as scratch, RBX as the CPU pointer (callee-saved).

    // mov r64, imm64
    void emit_mov_imm64(int dst_reg, uint64_t imm);
    // mov r64, imm32 (zero-extended)
    void emit_mov_imm32(int dst_reg, uint32_t imm);
    // mov r64, r64
    void emit_mov_reg(int dst_reg, int src_reg);
    // mov r64, [base_reg + offset]
    void emit_load(int dst_reg, int base_reg, int32_t offset);
    // mov [base_reg + offset], r64
    void emit_store(int base_reg, int32_t offset, int src_reg);
    // add r64, r64
    void emit_add_reg(int dst_reg, int src_reg);
    // sub r64, r64
    void emit_sub_reg(int dst_reg, int src_reg);
    // and r64, r64
    void emit_and_reg(int dst_reg, int src_reg);
    // or  r64, r64
    void emit_or_reg(int dst_reg, int src_reg);
    // xor r64, r64
    void emit_xor_reg(int dst_reg, int src_reg);
    // cmp r64, r64
    void emit_cmp_reg(int a_reg, int b_reg);
    // shl/shr/sar r64, cl
    void emit_shift_cl(int dst_reg, int kind);  // kind: 4=shl, 5=shr, 7=sar, 0=rol, 1=ror
    // setcc r8 (low byte of reg)
    void emit_setcc(int dst_reg, uint8_t cc);
    // cmovcc r64, r64
    void emit_cmovcc(int dst_reg, int src_reg, uint8_t cc);
    // imul r64, r64 (two-operand signed multiply)
    void emit_imul_reg(int dst_reg, int src_reg);
    // ret
    void emit_ret();
    // call [abs target]  (used for interpreter fallback)
    void emit_call_abs(void* target);
    // jmp rel32 (to a known offset within the code buffer)
    // Returns the offset of the rel32 to patch later.
    size_t emit_jmp_rel32_placeholder();
    void patch_jmp_rel32(size_t patch_offset, int32_t rel);
    // jcc rel32
    size_t emit_jcc_rel32_placeholder(uint8_t cc);
    void patch_jcc_rel32(size_t patch_offset, int32_t rel);
    // nop
    void emit_nop();

    // ── ARM64 register file layout ────────────────────────────────
    // The CPU struct has `uint64_t regs[32]` at some offset. We find
    // that offset at JIT-construction time by computing
    // offsetof(CPU, regs). Similarly for sp, pc, pstate.
    // To keep the header clean, we just hardcode the offsets based on
    // the CPU struct layout and assert at construction time.
    //
    // CPU layout (from arm64_emu.hpp):
    //   offset 0:   uint64_t regs[32]   (256 bytes)
    //   offset 256: uint64_t sp
    //   offset 264: uint64_t pc
    //   offset 272: uint32_t pstate
    //   ...
    static constexpr int REGS_OFF   = 0;     // offsetof(CPU, regs)
    static constexpr int SP_OFF     = 256;   // offsetof(CPU, sp)
    static constexpr int PC_OFF     = 264;   // offsetof(CPU, pc)
    static constexpr int PSTATE_OFF = 272;   // offsetof(CPU, pstate)

    // x86_64 register we use to hold the CPU pointer. RBX is
    // callee-saved in the System V AMD64 ABI, so we can use it across
    // calls without spilling. RAX/RCX/RDX are scratch.
    static constexpr int CPU_REG = 3;  // RBX
    // x86_64 register for the Emulator pointer (for syscall fallback).
    static constexpr int EMU_REG = 6;  // RSI

    // ── Translation ───────────────────────────────────────────────
    // Translate the block starting at `start_pc`. Writes x86_64 code
    // to the code buffer, adds an entry to block_cache_, and returns
    // the function pointer.
    uint64_t (*translate_block(CPU& cpu, Emulator& emu, uint64_t start_pc))(CPU*, Emulator*);

    // Returns true if the JIT can translate this instruction. If false,
    // the translator ends the block before this instruction and
    // delegates to the interpreter.
    bool can_translate(const DecodedInst& d) const;

    // Emit code for a single ARM64 instruction. Updates `cur_pc` and
    // returns true if the instruction ends the block (branch/ret/svc/
    // unsupported). Returns false if execution should continue to the
    // next instruction in the block.
    //
    // `block_start` and `block_end` are used for relative branch
    // patching (the JIT emits forward jumps as placeholders and patches
    // them once the full block is emitted).
    bool emit_instruction(const DecodedInst& d, uint64_t cur_pc,
                          uint64_t block_start, size_t block_start_off);
};

} // namespace arm64emu
