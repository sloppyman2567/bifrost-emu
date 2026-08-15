// jit/frostjit.cpp — FrostJIT: integer/memory/branch IR-op codegen + dispatch.
//
// v1.4.5-alpha: split into multiple files for readability.
// v1.4.5-alpha: further split — ALU/memory/branch cases extracted
//   from compile_ir_inst()'s switch into three sub-dispatchers
//   (compile_ir_{alu,mem,branch}) defined in jit_codegen_{alu,mem,branch}.cpp.
//   This file holds:
//   - compile-time layout checks (CPU struct offsets)
//   - thread-local watchdog/hotness state
//   - compile_ir_inst() — the residual integer IR-op switch (NOP, ATOMIC,
//     LDXR/STXR/STLR_FAST, ADDS/SUBS, TST, CSINC/CSINV/CSNEG, BR,
//     ADCS/SBCS, MRS, MSR, SMULH/UMULH). FP/SIMD ops are delegated to
//     compile_ir_inst_fp_() (jit_codegen_fp.cpp); ALU/memory/branch ops
//     are delegated to compile_ir_{alu,mem,branch}().
//
// Other FrostJIT methods live in:
//   - jit_interp.cpp        — jit_interp_step() extern "C" trampoline
//   - jit_helpers.cpp       — emit_fmov_helper, emit_call_interp
//   - jit_codegen_fp.cpp    — FP/SIMD IR-op codegen
//   - jit_codegen_alu.cpp   — ALU/arithmetic IR-op codegen
//   - jit_codegen_mem.cpp   — memory IR-op codegen
//   - jit_codegen_branch.cpp— branch/call IR-op codegen
//   - jit_flags.cpp         — clobber_flags, materialize_flags_to_pstate
//   - jit_translate.cpp     — translate_block (ARM64 → x86)
//   - jit_dispatch.cpp      — run_block (block cache lookup + dispatch)
//   - x86_backend.cpp       — x86 instruction emitters
//   - x86_regalloc.cpp      — register allocator
//   - jit_cache.cpp         — block cache
//   - jit_profiler.cpp      — lifecycle + stats counters
//   - jit_glue.cpp          — Emulator↔FrostJIT glue
//   - cpu_features.cpp      — host CPU feature detection
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "ir/ir.hpp"
#include "bifrost/version.hpp"  // CODENAME
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <sys/mman.h>
#include <unordered_map>
#include <vector>
namespace arm64emu {
// ── Chain-skip env gate (BIFROST_CHAIN_SKIP=1, default OFF) ────────────
// Read once (static local, mirrors the BIFROST_NO_SELFLOOP pattern in
// jit_codegen_branch.cpp). When enabled, every block is emitted with a
// chain-skip entry point (BlockEntry.chain_entry) and a lease-style
// epilogue (chain slot before the callee-saved restore), so chain edges
// skip the predecessor's frame teardown AND the successor's prologue.
bool FrostJIT::chain_skip_enabled() {
    static const bool on = (getenv("BIFROST_CHAIN_SKIP") != nullptr);
    return on;
}
// ── Compile-time layout checks ─────────────────────────────────────────
// The JIT hardcodes offsets into the CPU struct (REGS_OFF, SP_OFF, etc.)
// for direct memory access in generated x86 code. If the CPU struct layout
// ever changes, these static_asserts will catch it at compile time instead
// of producing silently-wrong JIT code.
static_assert(offsetof(CPU, regs)   == FrostJIT::REGS_OFF,   "CPU regs offset mismatch");
static_assert(offsetof(CPU, sp)     == FrostJIT::SP_OFF,     "CPU sp offset mismatch");
static_assert(offsetof(CPU, pc)     == FrostJIT::PC_OFF,     "CPU pc offset mismatch");
static_assert(offsetof(CPU, pstate) == FrostJIT::PSTATE_OFF, "CPU pstate offset mismatch");
static_assert(offsetof(CPU, v_lo)   == FrostJIT::V_LO_OFF,   "CPU v_lo offset mismatch");
static_assert(offsetof(CPU, v_hi)   == FrostJIT::V_HI_OFF,   "CPU v_hi offset mismatch");
static_assert(offsetof(CPU, fpcr)   == FrostJIT::FPCR_OFF,   "CPU fpcr offset mismatch");
static_assert(offsetof(CPU, fpsr)   == FrostJIT::FPSR_OFF,   "CPU fpsr offset mismatch");
// Additional layout checks: the JIT hardcodes element counts (regs[31],
// v_lo[32], v_hi[32]) and element sizes (8 bytes each). If the CPU struct
// ever changes — e.g. v_lo becomes uint32_t[32] — the JIT's
// `V_LO_OFF + idx*8` addressing would silently produce wrong code.
// These static_asserts catch that at compile time.
static_assert(sizeof(CPU::regs) >= 31 * 8, "CPU::regs must hold 31 × 8-byte regs");
static_assert(sizeof(CPU::v_lo) == 32 * 8, "CPU::v_lo must be 32 × 8 bytes (uint64_t[32])");
static_assert(sizeof(CPU::v_hi) == 32 * 8, "CPU::v_hi must be 32 × 8 bytes (uint64_t[32])");
static_assert(sizeof(((CPU*)0)->regs[0]) == 8, "CPU reg element must be 8 bytes");
static_assert(sizeof(((CPU*)0)->v_lo[0]) == 8, "CPU v_lo element must be 8 bytes");
// ── Forward decls of slow-path helpers defined in x86_backend.cpp ──────
// These are extern "C" so JIT-compiled code can call them by address
// without name-mangling concerns. The CPU* arg is used for SIGSEGV
// delivery when the memory access faults (UnmappedMemory).
//
// v1.4.5-alpha: jit_interp_step() was moved to jit_interp.cpp.
extern "C" {
    uint64_t jit_load_mem_slow(Emulator* emu, CPU* cpu, uint64_t addr, int width);
    void     jit_store_mem_slow(Emulator* emu, CPU* cpu, uint64_t addr, uint64_t val, int width);
}
} // namespace arm64emu
namespace arm64emu {
extern "C" uint64_t jit_call_helper(CPU* cpu, Emulator* emu, uint64_t target_pc);
// ── Thread-local per-thread JIT state (Task 3: shared-JIT mode) ────────
// These are thread-local so that multiple threads sharing a single FrostJIT
// instance don't corrupt each other's watchdog/hotness counters. Each
// thread gets its own copy, initialized to the defaults.
thread_local uint64_t FrostJIT::tls_watchdog_last_pc_ = UINT64_MAX;
thread_local uint32_t FrostJIT::tls_watchdog_count_   = 0;
thread_local std::unordered_map<uint64_t, uint32_t> FrostJIT::tls_hot_pc_counts_;
thread_local FrostJIT::LastBlockCache FrostJIT::tls_last_block_;
thread_local FrostJIT::InlineCacheEntry FrostJIT::tls_inline_cache_[INLINE_CACHE_SLOTS];
// emit_load_mem / emit_store_mem live in x86_backend.cpp
// (they are pure x86 emission with no regalloc/IR awareness).
bool FrostJIT::compile_ir_inst(const IRInst& inst) {
    // v1.4.5-alpha: FP/SIMD ops are dispatched to
    // compile_ir_inst_fp_() (defined in jit_codegen_fp.cpp) before the
    // integer/memory/branch switch below. The FP handler sets
    // fp_handled_ to true if it recognized the op (regardless of
    // whether it ends the block), and returns the "ends_block" bool.
    // If fp_handled_ is false, the op is not an FP/SIMD op and we fall
    // through to the integer switch.
    fp_handled_ = false;
    bool ends_block = compile_ir_inst_fp_(inst);
    if (fp_handled_) return ends_block;
    // v1.4.5-alpha: ALU / memory / branch cases are dispatched
    // to compile_ir_{alu,mem,branch}() (defined in jit_codegen_*.cpp)
    // before the residual switch below. Each sub-dispatcher returns:
    //   -1 = not handled here (fall through to the next dispatcher)
    //    0 = handled, does NOT end the block (compile_ir_inst returns false)
    //    1 = handled AND ends the block (compile_ir_inst returns true)
    int ir_r;
    if ((ir_r = compile_ir_mem(inst))    >= 0) return ir_r == 1;
    if ((ir_r = compile_ir_alu(inst))    >= 0) return ir_r == 1;
    if ((ir_r = compile_ir_branch(inst)) >= 0) return ir_r == 1;
    switch (inst.op) {
        case IROp::NOP:
            return false;
        case IROp::ATOMIC: {
            // Native LSE atomics via x86 lock-prefixed instructions.
            // Fast path: address < 4 GiB (direct window) → lock op on
            // (window_base + addr). Slow path: addr ≥ 4 GiB → CALL_INTERP.
            //
            // Operations (atom_op in inst.cond):
            //   CAS (≥0xC): lock cmpxchg       (32/64-bit only)
            //   SWP (0x8):   lock xchg
            //   LDADD (0x0): lock xadd (LD) / lock add (ST)
            //   STSET (0x3): lock or   (ST only)
            //   STCLR (0x1): lock and (ST only, ~src)
            //   LDSET/LDCLR/LDEOR (LD): CAS-loop
            //   SMAX/SMIN/UMAX/UMIN (4-7): CAS-loop with cmp+cmov
            //
            // inst.imm = ARM reg index (rt for non-CAS, rs for CAS) for
            //   slow-path result reload.
            // inst.immr = CAS desired vreg index (packed; only for CAS).
            uint8_t atom_op = inst.cond;
            bool is_load = (inst.flags_op != 0);
            int w = inst.width;
            bool is_64 = (w == 8);
            bool is_16 = (w == 2);
            // SMAX/SMIN/UMAX/UMIN and 8-bit/16-bit CAS need special handling
            // that the fast path doesn't support. Fall back to CALL_INTERP.
            // 8-bit CAS needs cmpxchg r/m8 (0x0F 0xB0); the fast path only
            // emits cmpxchg r/m32/r64 (0x0F 0xB1). 16-bit CAS also works
            // but let's be conservative and fall back for sub-32-bit CAS.
            if ((atom_op >= 0x4 && atom_op <= 0x7) ||
                (atom_op >= 0xC && w < 4)) {
                emit_call_interp(inst.arm_pc, false);
                if (is_load && inst.dest != 0) {
                    emit_load_arm(RAX, static_cast<int>(inst.imm));
                    store_reg_to_vreg(inst.dest, RAX);
                }
                constexpr uint16_t MEM_CLOBBER =
                    (1u << RAX) | (1u << RCX) | (1u << RDX) |
                    (1u << R8)  | (1u << R9)  | (1u << R11);
                invalidate_host_regs(MEM_CLOBBER);
                return false;
            }
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            flush_invalidate_host_regs(MEM_CLOBBER);
            // Load base address → RAX.
            load_vreg_to_reg(RAX, inst.src1);
            // Check if addr + w <= 4GB (direct window fast path).
            emit_mov_imm32_zext(RDX, static_cast<uint32_t>(Memory::DIRECT_WINDOW_SIZE - w));
            emit_cmp_reg(RAX, RDX);
            size_t jae_patch = emit_jcc_rel32_placeholder(7);  // JA → slow
            // ── Fast path: direct window ──
            emit_add_reg(RAX, WIN_REG);  // RAX = window_base + addr
            // Load operand → RCX. For CAS, src2 = desired (rt). For others,
            // src2 = operand (rs). CAS loads expected from cpu.regs[imm].
            load_vreg_to_reg(RCX, inst.src2);  // RCX = desired (CAS) or operand
            // Helper: emit REX + opcode + modrm for a lock instruction.
            // reg = the register operand, base = RAX (host address).
            auto emit_lock_op = [&](uint8_t opcode, int reg) {
                emit_byte(0xF0);  // LOCK
                if (is_64) emit_byte(0x48);
                else if (is_16) emit_byte(0x66);
                emit_byte(opcode);
                emit_byte(modrm(0, reg & 7, RAX & 7));  // [rax], reg
            };
            if (atom_op >= 0xC) {
                // CAS: lock cmpxchg [R9], RCX
                // x86 cmpxchg: if RAX == [mem], [mem]=RCX; RAX = old always.
                // ARM CAS: old=[Xn]; if old==Ws, [Xn]=Wt; Ws=old.
                // Map: RAX=Ws(expected from cpu.regs[imm]), [mem]=Wt(RCX=desired).
                emit_mov_reg(R9, RAX);              // R9 = host addr
                emit_load_arm(RAX, static_cast<int>(inst.imm));  // RAX = expected
                // lock cmpxchg [R9], RCX
                emit_byte(0xF0);                    // LOCK
                emit_byte(is_64 ? 0x49 : 0x41);     // REX.WB(64) or REX.B(32) for R9
                if (is_16) emit_byte(0x66);         // operand-size prefix
                emit_byte(0x0F); emit_byte(0xB1);   // cmpxchg r/m, r
                emit_byte(0x09);                    // modrm(0, rcx, r9)
                if (is_load) {
                    store_reg_to_vreg(inst.dest, RAX);
                }
            } else if (atom_op == 0x8) {
                // SWP: lock xchg [RAX], RCX (RCX gets old value)
                emit_lock_op(0x87, RCX);
                if (is_load) store_reg_to_vreg(inst.dest, RCX);
            } else if (atom_op == 0x0) {
                // LDADD/STADD
                if (is_load) {
                    // LDADD: lock xadd [RAX], RCX (RCX gets old value)
                    // XADD is a 2-byte opcode (0F C1) — can't use emit_lock_op.
                    emit_byte(0xF0);  // LOCK
                    if (is_64) emit_byte(0x48);
                    else if (is_16) emit_byte(0x66);
                    emit_byte(0x0F); emit_byte(0xC1);  // xadd r/m, r
                    emit_byte(modrm(0, RCX & 7, RAX & 7));
                    store_reg_to_vreg(inst.dest, RCX);
                } else {
                    // STADD: lock add [RAX], RCX
                    emit_lock_op(0x01, RCX);
                }
            } else if (atom_op == 0x3 && !is_load) {
                // STSET: lock or [RAX], RCX
                emit_lock_op(0x09, RCX);
            } else if (atom_op == 0x1 && !is_load) {
                // STCLR: lock and [RAX], ~RCX
                emit_not_reg(RCX);
                emit_lock_op(0x21, RCX);  // and r/m, r
            } else {
                // LDSET/LDCLR/LDEOR (LD variants): CAS-loop.
                //   R9 = addr
                //   RCX = src (operand, preserved across iterations)
                //   RDX = ~src (for LDCLR only; preserved across iterations)
                //   retry:
                //     RAX = [R9]                 ; reload old
                //     R8  = RAX                  ; copy old
                //     R8  = R8 OP RCX (or RDX)   ; recompute new from CURRENT old
                //     lock cmpxchg [R9], R8      ; if RAX == [R9], [R9]=R8
                //     jnz retry                  ; else RAX = [R9] (new old), retry
                //   dest = RAX (old value)
                //
                // BUGFIX 1: previously `R8 = old OP src` was computed ONCE before
                //   the loop. On CAS failure, RAX was updated to the new [mem]
                //   but R8 was not recomputed — the retrying cmpxchg wrote the
                //   ORIGINAL old OP src instead of the CURRENT old OP src. Under
                //   multi-threaded contention this produced silently wrong RMW
                //   results for LDCLR/LDEOR/LDSET (e.g. broken atomic flags).
                //
                // BUGFIX 2: the original AND/XOR/OR opcodes used REX prefix
                //   0x4D (W,R,B all set), but REX.R=1 extends the reg field
                //   from rcx (1) to r9 (9). The intended instruction was
                //   `and r8, rcx` but the bytes encoded `and r8, r9`. Since
                //   R9 held the host address (a pointer, not the operand),
                //   the result was garbage. The correct REX is 0x49 (W=1,
                //   R=0, B=1) so reg stays rcx and rm extends to r8.
                emit_mov_reg(R9, RAX);  // R9 = host addr
                // For LDCLR, precompute ~src into RDX (preserved across iterations).
                // RDX is already invalidated above and clobbered by idiv; safe to use.
                if (atom_op == 0x1) {
                    // mov rdx, rcx (48 89 CA)
                    emit_byte(0x48); emit_byte(0x89); emit_byte(0xCA);
                    // not rdx — width-sensitive
                    if (is_16) emit_byte(0x66);
                    if (is_64) { emit_byte(0x48); emit_byte(0xF7); emit_byte(0xD2); }
                    else       {                  emit_byte(0xF7); emit_byte(0xD2); }
                    invalidate_host_regs(1u << RDX);
                }
                // CAS loop start: reload old value from memory.
                size_t loop_start = code_buf_used_;
                // RAX = [R9] (mov rax, [r9])
                if (is_64)      { emit_byte(0x49); emit_byte(0x8B); emit_byte(0x01); }
                else if (is_16) { emit_byte(0x66); emit_byte(0x41); emit_byte(0x8B); emit_byte(0x01); }
                else            { emit_byte(0x41); emit_byte(0x8B); emit_byte(0x01); }
                // R8 = RAX (mov r8, rax = 4C 8B C0)
                emit_byte(0x4C); emit_byte(0x8B); emit_byte(0xC0);
                // Compute new = R8 OP RCX (or R8 AND RDX for LDCLR).
                // REX: W=1 (64-bit) or 0 (32/16-bit), R=0 (reg = rcx/rdx, no extend), B=1 (rm = r8)
                //   → 0x49 for 64-bit, 0x41 for 32/16-bit
                // modrm: 11 001 000 = 0xC8 (reg=001=rcx, rm=000=r8-low-3) for XOR/OR
                // modrm: 11 010 000 = 0xD0 (reg=010=rdx, rm=000=r8-low-3) for LDCLR's AND
                switch (atom_op) {
                    case 0x1: // LDCLR: new = old & ~src = r8 & rdx
                        if (is_16) emit_byte(0x66);
                        if (is_64) emit_byte(0x49); else emit_byte(0x41);
                        emit_byte(0x21); emit_byte(0xD0);  // and r8, rdx
                        break;
                    case 0x2: // LDEOR: new = old ^ src = r8 ^ rcx
                        if (is_16) emit_byte(0x66);
                        if (is_64) emit_byte(0x49); else emit_byte(0x41);
                        emit_byte(0x31); emit_byte(0xC8);  // xor r8, rcx
                        break;
                    case 0x3: // LDSET: new = old | src = r8 | rcx
                        if (is_16) emit_byte(0x66);
                        if (is_64) emit_byte(0x49); else emit_byte(0x41);
                        emit_byte(0x09); emit_byte(0xC8);  // or r8, rcx
                        break;
                }
                // lock cmpxchg [R9], R8
                // REX.WRB (0x4D) = W=1, R=1(reg→r8), B=1(rm→r9).
                emit_byte(0xF0);                    // LOCK
                emit_byte(is_64 ? 0x4D : 0x45);     // REX.WRB(64) or REX.RB(32)
                if (is_16) emit_byte(0x66);
                emit_byte(0x0F); emit_byte(0xB1);   // cmpxchg r/m, r
                emit_byte(0x01);                    // modrm(0, r8, r9) = 00 000 001
                // jnz loop_start (retry if CAS failed)
                int32_t loop_rel = static_cast<int32_t>(loop_start - (code_buf_used_ + 6));
                emit_byte(0x0F); emit_byte(0x85); emit_u32(static_cast<uint32_t>(loop_rel));
                if (is_load) {
                    store_reg_to_vreg(inst.dest, RAX);
                }
            }
            // Jump past slow path.
            size_t jmp_past = emit_jmp_rel32_placeholder();
            // ── Slow path: addr ≥ 4 GiB → CALL_INTERP ──
            size_t slow_path = code_buf_used_;
            patch_jcc_rel32(jae_patch, static_cast<int32_t>(slow_path - (jae_patch + 6)));
            emit_call_interp(inst.arm_pc, false);
            // Reload result: inst.imm = ARM reg index (rt for non-CAS,
            // rs for CAS). The interpreter wrote the old value there.
            if (is_load && inst.dest != 0) {
                emit_load_arm(RAX, static_cast<int>(inst.imm));
                store_reg_to_vreg(inst.dest, RAX);
            }
            int32_t end_rel = static_cast<int32_t>(code_buf_used_ - (jmp_past + 5));
            patch_jmp_rel32(jmp_past, end_rel);
            invalidate_host_regs(MEM_CLOBBER);
            return false;
        }
        case IROp::LDXR_FAST: {
            // Fast LDXR via C helper — bypasses interpreter decode.
            // Args: RDI=emu, RSI=cpu, RDX=addr, RCX=width
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            flush_invalidate_host_regs(MEM_CLOBBER);
            emit_mov_reg(RDI, EMU_REG);
            emit_mov_reg(RSI, CPU_REG);
            load_vreg_to_reg(RDX, inst.src1);
            emit_mov_imm32(RCX, inst.width);
            emit_call_aligned(&jit_ldxr, 0);
            store_reg_to_vreg(inst.dest, RAX);
            invalidate_host_regs(MEM_CLOBBER);
            return false;
        }
        case IROp::STXR_FAST: {
            // Fast STXR via C helper — bypasses interpreter decode.
            // Args: RDI=emu, RSI=cpu, RDX=addr, RCX=val, R8=width
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            flush_invalidate_host_regs(MEM_CLOBBER);
            emit_mov_reg(RDI, EMU_REG);
            emit_mov_reg(RSI, CPU_REG);
            load_vreg_to_reg(RDX, inst.src1);
            load_vreg_to_reg(RCX, inst.src2);
            emit_mov_imm32(R8, inst.width);
            emit_call_aligned(&jit_stxr, 0);
            store_reg_to_vreg(inst.dest, RAX);
            invalidate_host_regs(MEM_CLOBBER);
            return false;
        }
        case IROp::STLR_FAST: {
            // Fast STLR via C helper — bypasses interpreter decode.
            // Args: RDI=emu, RSI=cpu, RDX=addr, RCX=val, R8=width
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            flush_invalidate_host_regs(MEM_CLOBBER);
            emit_mov_reg(RDI, EMU_REG);
            emit_mov_reg(RSI, CPU_REG);
            load_vreg_to_reg(RDX, inst.src1);
            load_vreg_to_reg(RCX, inst.src2);
            emit_mov_imm32(R8, inst.width);
            emit_call_aligned(&jit_stlr, 0);
            invalidate_host_regs(MEM_CLOBBER);
            return false;
        }
        case IROp::ADDS: case IROp::SUBS: {
            // Force src1 into RAX, src2 into RCX (same fixed-assignment
            // pattern as SHL/SHR/SAR/ROR — handled by force_two_vregs_to).
            // Call clobber_flags() first to materialize any pending flags
            // before force_two_vregs_to potentially clobbers them.
            clobber_flags();
            force_two_vregs_to(inst.src1, RAX, inst.src2, RCX);
            int s2 = RCX;
            bool is_sub = (inst.op == IROp::SUBS);
            bool is_32bit = (inst.width == 32);
            // Always compute in RAX (holds src1) to avoid alloc_reg_for
            // evicting the operands under register pressure.
            int d = RAX;
            if (inst.dest != inst.src1 && inst.dest != 0) {
                // Drop src1's mapping (don't spill — set_vreg_reg will
                // handle the transition). If src1 was dirty, spill first
                // to preserve its value for later readers.
                if (vreg_home_[inst.src1] == RAX) {
                    if (vreg_dirty_[inst.src1]) {
                        evict_vreg(inst.src1);
                    } else {
                        vreg_home_[inst.src1] = -1;
                        reg_vreg_[RAX] = -1;
                        dirty_host_regs_ &= ~(1u << RAX);
                        vreg_last_use_[inst.src1] = 0;  // clear LRU timestamp
                    }
                }
            } else if (inst.dest == inst.src1 && inst.dest != 0) {
                vreg_dirty_[inst.dest] = true;
                dirty_host_regs_ |= (1u << RAX);
                vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
            }
            if (is_32bit) {
                // 0x29 /r = SUB r/m32, r32 (sub dst, src)
                // 0x01 /r = ADD r/m32, r32 (add dst, src)
                bool need_rex = (s2 >= 8) || (d >= 8);
                if (need_rex) {
                    emit_byte(rex(false, s2>=8, false, d>=8));
                }
                if (is_sub) { emit_byte(0x29); emit_byte(modrm(3, s2&7, d&7)); }
                else        { emit_byte(0x01); emit_byte(modrm(3, s2&7, d&7)); }
                // Zero-extend dest to 64 bits (32-bit ops zero-extend).
                // mov e_d, e_d — BUGFIX: must set BOTH REX.R (reg field)
                // and REX.B (r/m field) when d >= 8, otherwise the reg
                // field defaults to EAX and we emit `mov r8d, eax` instead
                // of `mov r8d, r8d`, corrupting the dest with EAX's value.
                if (d >= 8) {
                    emit_byte(rex(false, d>=8, false, d>=8));
                }
                emit_byte(0x89); emit_byte(modrm(3, d&7, d&7));
            } else {
                if (is_sub) emit_sub_reg(d, s2);
                else        emit_add_reg(d, s2);
            }
            flags_in_host_ = true;
            flags_from_sub_ = is_sub;
            if (inst.dest == 0) kill_vreg(inst.src1);
            else if (inst.dest != inst.src1) set_vreg_reg(inst.dest, d);
            return false;
        }
        case IROp::TST: {
            // Force both operands into distinct host regs via force_two_vregs_to.
            // but ensure_vreg ignores the `preferred` hint when the vreg is
            // already cached elsewhere. Under high register pressure, the
            // second ensure_vreg could evict the first operand's register
            // (via alloc_reg evicting ALLOC_REGS[0]=RAX), causing both
            // operands to end up in the same register — test rax, rax tests
            // the wrong value.
            clobber_flags();
            force_two_vregs_to(inst.src1, RAX, inst.src2, RCX);
            emit_test_reg(RAX, RCX);
            flags_in_host_ = true;
            flags_from_sub_ = false;
            return false;
        }
        case IROp::TST_ZERO: {
            // Should not be reached — CBZ/CBNZ now use BRCOND_ZERO.
            // Fallback: treat as TST(val, val).
            int s1 = ensure_vreg(inst.src1, RAX);
            emit_test_reg(s1, s1);
            flags_in_host_ = true;
            flags_from_sub_ = false;
            return false;
        }
        // Defensive fallback: CSINC/CSINV/CSNEG are normally decomposed
        // to ADD+NOT+NEG + CSEL in ir_translate.cpp, so the JIT only sees
        // IROp::CSEL. If a future change re-emits these, fall back to the
        // interpreter (correct but slow) instead of crashing.
        case IROp::CSINC: case IROp::CSINV: case IROp::CSNEG: {
            emit_call_interp(inst.arm_pc, false);
            kill_vreg(inst.dest);
            {
                int d = alloc_reg();
                int rd = static_cast<int>(inst.imm);
                emit_load_arm(d, rd);
                set_vreg_reg(inst.dest, d);
            }
            return false;
        }
        case IROp::BR: {
            int s = ensure_vreg(inst.src1, RAX);
            // Flush all dirty vregs before returning.
            flush_all_vregs();
            if (s != RAX) emit_mov_reg(RAX, s);
            rax_holds_next_pc_ = true;
            unchainable_end_ = true;  // indirect branch — target is dynamic
            return true;
        }
        case IROp::ADCS: case IROp::SBCS: {
            // Native ADCS/SBCS using x86 ADC/SBB.
            //   ADCS: dst = src1 + src2 + C, set flags
            //   SBCS: dst = src1 - src2 - 1 + C, set flags
            // x86 ADC: dst = dst + src + CF
            // x86 SBB: dst = dst - src - CF
            // ARM C = x86 CF for ADC (carry out).
            // ARM C = NOT x86 CF for SBB (NOT borrow).
            //
            // We must load the C flag from pstate into x86 CF first.
            // After loading, x86 CF = ARM C XOR from_sub. We need:
            //   ADCS: x86 CF = ARM C (ADD convention)
            //   SBCS: x86 CF = NOT ARM C (SUB convention)
            // emit_normalize_cf_to_sub_convention normalizes to SUB convention.
            // For ADCS, we then cmc to get ADD convention.
            bool is_sub = (inst.op == IROp::SBCS);
            bool is_32bit = (inst.width == 32);
            // Load flags from pstate (we need CF in x86 CF).
            // If flags_in_host_, materialize first (to preserve pstate),
            // then we already have flags in host.
            if (flags_in_host_) {
                // Materialize to pstate (clobbers RAX/RCX/RDX).
                emit_pushfq();
                constexpr uint16_t FLAGS3a = (1u<<RAX)|(1u<<RCX)|(1u<<RDX);
                flush_dirty_host_regs(FLAGS3a);
                emit_materialize_flags(flags_from_sub_);
                emit_popfq();
                invalidate_host_regs(FLAGS3a);
                // Host flags are in the convention indicated by flags_from_sub_.
                // For SBCS: need SUB convention (x86 CF = NOT ARM C).
                //   If flags_from_sub_=true: already SUB. OK.
                //   If flags_from_sub_=false: ADD convention. Need cmc.
                // For ADCS: need ADD convention (x86 CF = ARM C).
                //   If flags_from_sub_=false: already ADD. OK.
                //   If flags_from_sub_=true: SUB convention. Need cmc.
                if (is_sub && !flags_from_sub_) {
                    emit_byte(0xF5);  // cmc: ADD → SUB
                } else if (!is_sub && flags_from_sub_) {
                    emit_byte(0xF5);  // cmc: SUB → ADD
                }
            } else {
                // emit_normalize_cf_to_sub_convention only clobber
                // RAX/RCX/RDX. Use targeted flush+invalidate to preserve
                // vregs cached in R8/R9/R11/R12/R13/R15.
                // No pushfq/popfq: the goal is to LOAD flags, and
                // popfq would restore the pre-load (garbage) flags.
                constexpr uint16_t FLAGS3 = (1u << RAX) | (1u << RCX) | (1u << RDX);
                flush_dirty_host_regs(FLAGS3);
                flush_scratch_host_regs(FLAGS3);
                emit_load_flags_from_pstate();
                // Normalize CF to SUB convention (x86 CF = NOT ARM C).
                emit_normalize_cf_to_sub_convention();
                invalidate_host_regs(FLAGS3);
                // For SBCS: CF is now NOT ARM C. Correct.
                // For ADCS: need ARM C. cmc to invert.
                if (!is_sub) {
                    emit_byte(0xF5);  // cmc: SUB → ADD
                }
            }
            // Force src1 into RAX, src2 into RCX (same pattern as SHL/ADDS).
            force_two_vregs_to(inst.src1, RAX, inst.src2, RCX);
            int s1 = RAX, s2 = RCX;
            int d;
            if (inst.dest == inst.src1 && inst.dest != 0) {
                d = s1;
                vreg_dirty_[inst.dest] = true;
                dirty_host_regs_ |= (1u << s1);  // maintain dirty-bitmask invariant
                vreg_last_use_[inst.dest] = ++regalloc_lru_counter_;
            } else if (inst.dest != 0) {
                d = alloc_reg_for(inst.dest, s1);
                if (d != s1) emit_mov_reg(d, s1);
            } else {
                d = s1;
            }
            if (is_32bit) {
                // 32-bit ADC/SBB: REX if needed.
                bool need_rex = (s2 >= 8) || (d >= 8);
                if (need_rex) emit_byte(rex(false, s2>=8, false, d>=8));
                if (is_sub) { emit_byte(0x19); emit_byte(modrm(3, s2&7, d&7)); }
                else        { emit_byte(0x11); emit_byte(modrm(3, s2&7, d&7)); }
                // Zero-extend dest (mov e_d, e_d) with correct REX.
                if (d >= 8) emit_byte(rex(false, d>=8, false, d>=8));
                emit_byte(0x89); emit_byte(modrm(3, d&7, d&7));
            } else {
                if (is_sub) emit_sbb_reg(d, s2);
                else        emit_adc_reg(d, s2);
            }
            flags_in_host_ = true;
            flags_from_sub_ = is_sub;  // SBB: ARM C = NOT CF; ADC: ARM C = CF
            if (inst.dest == 0) kill_vreg(inst.src1);
            return false;
        }
        // ── MRS — read system register ───────────────────────────────
        case IROp::MRS: {
            // inst.imm encodes the system register (op1/crn/crm/op2/op0).
            // We handle TPIDR_EL0, TPIDRRO_EL0, FPCR, FPSR, NZCV natively.
            // ID registers (CTR_EL0, DCZID_EL0, MIDR_EL1, MVFR*) return
            // fixed values matching the interpreter. Unknown registers
            // return 0.
            clobber_flags();  // xor d,d and emit_load don't preserve flags
            uint64_t sys = inst.imm;
            uint8_t op1 = (sys >> 16) & 0x7;
            uint8_t crn = (sys >> 12) & 0xF;
            uint8_t crm = (sys >> 8) & 0xF;
            uint8_t op2 = (sys >> 4) & 0x7;
            int d = alloc_reg_for(inst.dest, RAX);
            int32_t off = -1;
            uint64_t imm_val = 0;
            bool use_imm = false;
            if (op1 == 3 && crn == 13 && crm == 0 && op2 == 2) {
                off = 808;  // TPIDR_EL0
            } else if (op1 == 3 && crn == 13 && crm == 0 && op2 == 3) {
                off = 816;  // TPIDRRO_EL0
            } else if (op1 == 3 && crn == 4 && crm == 2 && op2 == 0) {
                // NZCV — materialize flags from host to pstate, then load.
                if (flags_in_host_) {
                    emit_materialize_flags(flags_from_sub_);
                    flags_in_host_ = false;
                    // Drop cache mappings for RAX/RCX/RDX (clobbered by materialize).
                    invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
                }
                // Read pstate and mask off the internal `from_sub` flag
                // (bit 27) that emit_materialize_flags stores alongside
                // NZCV. Without this mask, `mrs x0, nzcv` after a SUBS
                // returns 0x?a8000000 instead of 0x?a0000000, diverging
                // from the interpreter. Only bits 31:28 (N/Z/C/V) are
                // architecturally visible in the NZCV sysreg.
                int d2 = alloc_reg_for(inst.dest, RAX);
                emit_load32(d2, CPU_REG, PSTATE_OFF);
                // and r/m32, 0xF0000000 — 32-bit op (no REX.W), REX.B if
                // the dest is r8–r15. emit_load32 already zero-extended
                // the high 32 bits, so the 32-bit AND keeps them zero.
                if (d2 >= 8) emit_byte(0x41);
                emit_byte(0x81); emit_byte(modrm(3, 4, d2 & 7));
                emit_u32(0xF0000000u);
                set_vreg_reg(inst.dest, d2);
                return false;
            } else if (op1 == 3 && crn == 4 && crm == 4 && op2 == 0) {
                off = FPCR_OFF;  // FPCR
            } else if (op1 == 3 && crn == 4 && crm == 4 && op2 == 1) {
                off = FPSR_OFF;  // FPSR
            } else if (op1 == 3 && crn == 0 && crm == 0 && op2 == 1) {
                imm_val = 0x8444C004; use_imm = true;  // CTR_EL0
            } else if (op1 == 3 && crn == 0 && crm == 0 && op2 == 7) {
                imm_val = (1u << 4); use_imm = true;  // DCZID_EL0 (no DC ZVA)
            } else if (op1 == 3 && crn == 0 && crm == 0 && op2 == 0) {
                imm_val = 0x410FD080; use_imm = true;  // MIDR_EL1 (Cortex-A72)
            } else if (op1 == 3 && crn == 0 && crm == 0 && op2 == 5) {
                imm_val = 0x10110222; use_imm = true;  // MVFR0_EL1
            } else if (op1 == 3 && crn == 0 && crm == 0 && op2 == 6) {
                imm_val = 0x12122211; use_imm = true;  // MVFR1_EL1
            } else if (op1 == 3 && crn == 0 && crm == 2 && op2 == 0) {
                imm_val = 0x00000022; use_imm = true;  // ID_AA64PFR0_EL1
            } else if (op1 == 3 && crn == 0 && crm == 2 && op2 == 2) {
                imm_val = 0x00000000; use_imm = true;  // ID_AA64MMFR0_EL1
            } else if (op1 == 3 && crn == 0 && crm == 2 && op2 == 4) {
                imm_val = 0x00000000; use_imm = true;  // ID_AA64ISAR0_EL1
            }
            if (off >= 0) {
                // FPCR/FPSR/PSTATE are uint32_t fields — use emit_load32
                // to avoid leaking the adjacent CPU member (e.g. `running`,
                // `exit_code`) into the high 32 bits of the destination.
                // The leak previously caused `mrs x0, nzcv` to return
                // 0x1_a8000000 instead of 0xa0000000 (the high 0x1 was
                // the `running=true` byte just past pstate).
                if (op1 == 3 && crn == 4 &&
                    ((crm == 4 && (op2 == 0 || op2 == 1)) ||  // FPCR/FPSR
                     (crm == 2 && op2 == 0)))                  // NZCV
                    emit_load32(d, CPU_REG, off);
                else
                    emit_load(d, CPU_REG, off);
            } else if (use_imm) {
                if (imm_val <= 0xFFFFFFFFULL) {
                    emit_mov_imm32_zext(d, static_cast<uint32_t>(imm_val));
                } else {
                    emit_mov_imm64(d, imm_val);
                }
            } else {
                // Unknown system register — return 0.
                emit_byte(rex(true, false, false, d >= 8));
                emit_byte(0x31); emit_byte(modrm(3, d & 7, d & 7));  // xor d, d
            }
            set_vreg_reg(inst.dest, d);
            return false;
        }
        // ── MSR — write system register ──────────────────────────────
        case IROp::MSR: {
            uint64_t sys = inst.imm;
            uint8_t op1 = (sys >> 16) & 0x7;
            uint8_t crn = (sys >> 12) & 0xF;
            uint8_t crm = (sys >> 8) & 0xF;
            uint8_t op2 = (sys >> 4) & 0x7;
            int s = ensure_vreg(inst.src1, RAX);
            if (s != RAX) emit_mov_reg(RAX, s);
            int32_t off = -1;
            if (op1 == 3 && crn == 13 && crm == 0 && op2 == 2) {
                off = 808;  // TPIDR_EL0
            } else if (op1 == 3 && crn == 4 && crm == 2 && op2 == 0) {
                // NZCV — fall back to interpreter.
                emit_call_interp(inst.arm_pc, false);
                return false;
            } else if (op1 == 3 && crn == 4 && crm == 4 && op2 == 0) {
                off = FPCR_OFF;
            } else if (op1 == 3 && crn == 4 && crm == 4 && op2 == 1) {
                off = FPSR_OFF;
            }
            if (off >= 0) {
                emit_store(CPU_REG, off, RAX);
            } else {
                // Unknown system register — treat as NOP (discard the
                // write). Much faster than CALL_INTERP.
            }
            return false;
        }
        // ── SMULH / UMULH — 128-bit high-half multiply ──────────────
        case IROp::SMULH:
        case IROp::UMULH: {
            // x86 mul: RDX:RAX = RAX * r/m64 (unsigned)
            // x86 imul: RDX:RAX = RAX * r/m64 (signed, one-operand form)
            // Result high 64 bits in RDX.
            clobber_flags();
            // use bitmask helpers instead of open-coded loop.
            flush_invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
            load_vreg_to_reg(RAX, inst.src1);
            load_vreg_to_reg(RCX, inst.src2);
            if (inst.op == IROp::UMULH) {
                // mul rcx (unsigned): RDX:RAX = RAX * RCX
                emit_byte(0x48); emit_byte(0xF7); emit_byte(0xE1);
            } else {
                // imul rcx (signed, one-operand): RDX:RAX = RAX * RCX
                emit_byte(0x48); emit_byte(0xF7); emit_byte(0xE9);
            }
            // Result is in RDX (high 64 bits)
            int d = alloc_reg_for(inst.dest, RDX);
            if (d != RDX) emit_mov_reg(d, RDX);
            set_vreg_reg(inst.dest, d);
            return false;
        }
        // Call the block at a REGISTER target via jit_call_helper, then
        // continue the block. Same as BL_CALL but the target comes from
        // src1 (the BLR's rn register) instead of an immediate.
        case IROp::BLR_CALL: {
            flush_all_vregs();
            if (flags_in_host_) {
                emit_materialize_flags(flags_from_sub_);
                flags_in_host_ = false;
                invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
            }
            if (vreg_home_[31] >= 0 && vreg_dirty_[31]) {
                evict_vreg(31);
            }
            // ── Cache guard ─────────────────────────────────────────
            // The callee (jit_call_helper → the target block) may clobber
            // ANY XMM3-15 and ANY cpu.v_lo entry. Write the pinned FP/vec
            // regs back before the call and reload them after, so the
            // cache stays authoritative. Lo-only in fp-cache mode (the
            // writeback/prologue branch on fp_cache_active_).
            // Flush pinned regs written ANYWHERE in the block, not just the
            // statically-dirty set: a pinned FP reg written AFTER this call
            // in the IR (loop-carried accumulator) is clean at this codegen
            // point but dirty at runtime on the next self-loop iteration —
            // skipping its flush makes the post-call reload read a stale
            // cpu.v_lo entry.
            if (vec_cache_active_) vec_cache_writeback_all_pinned();
            // RAX = target (from the ARM reg / vreg src1 — after the flush
            // it's in cpu.regs[src1] or its stack slot).
            load_vreg_to_reg(RAX, inst.src1);
            emit_store(CPU_REG, PC_OFF, RAX);
            emit_mov_reg(RDI, CPU_REG);
            emit_mov_reg(RSI, EMU_REG);
            emit_mov_reg(RDX, RAX);  // RDX = target_pc
            emit_push(WIN_REG);
            emit_call_aligned(&jit_call_helper, /*num_pushed=*/1);
            emit_mov_reg(RCX, RAX);  // RCX = next PC
            emit_pop(WIN_REG);
            emit_store(CPU_REG, PC_OFF, RCX);
            invalidate_all_vregs();
            if (vec_cache_active_) vec_emit_prologue_loads();
            return false;  // does NOT end the block
        }
        // Call the target block via jit_call_helper, then continue the block.
        case IROp::BL_CALL: {
            // Flush ALL dirty vregs to cpu.regs[]/stack BEFORE the call.
            flush_all_vregs();
            // Materialize host flags to pstate if valid (callee may read pstate).
            if (flags_in_host_) {
                emit_materialize_flags(flags_from_sub_);
                flags_in_host_ = false;
                invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
            }
            // Force-evict SP (vreg 31) if dirty — the callee needs correct SP.
            if (vreg_home_[31] >= 0 && vreg_dirty_[31]) {
                evict_vreg(31);
            }
            // ── Cache guard ─────────────────────────────────────────
            // Same writeback+reload as BLR_CALL: the callee may clobber any
            // XMM3-15 / cpu.v_lo entry, so the pinned regs are flushed before
            // the call and reloaded after it. Flush regs written anywhere in
            // the block — a reg written after this call in the IR (loop-carried
            // accumulator in a self-loop block) is statically clean here but
            // dirty at runtime on the next iteration (see vec_cache_writeback_all_pinned).
            if (vec_cache_active_) vec_cache_writeback_all_pinned();
            // Set cpu.pc = target_pc so the callee's chain/self-loop logic works.
            emit_mov_imm_to_rax(inst.imm);
            emit_store(CPU_REG, PC_OFF, RAX);
            // Set args: RDI = cpu, RSI = emu, RDX = target_pc.
            emit_mov_reg(RDI, CPU_REG);
            emit_mov_reg(RSI, EMU_REG);
            emit_mov_imm64(RDX, inst.imm);  // RDX = target_pc (use imm64 for >4GB)
            // Save WIN_REG (R10, caller-saved) before the call.
            // emit_call_abs clobbers RAX (to load the function address),
            // so we can't save RAX across the call. The return value
            // (next PC) will be in RAX after the call — we must NOT
            // overwrite it with a pop.
            // 1 push (WIN_REG) → odd → emit_call_aligned adds alignment.
            emit_push(WIN_REG);
            emit_call_aligned(&jit_call_helper, /*num_pushed=*/1);
            // RAX = return value (next PC). Save it to RCX before popping WIN_REG.
            // RCX is caller-saved and was already invalidated by the call.
            emit_mov_reg(RCX, RAX);  // RCX = next PC
            emit_pop(WIN_REG);
            // Store next PC to cpu.pc.
            emit_store(CPU_REG, PC_OFF, RCX);
            // Invalidate ALL cache mappings after the call.
            // The callee may have modified ANY cpu.regs[] entry (x0-x30, sp).
            invalidate_all_vregs();
            if (vec_cache_active_) vec_emit_prologue_loads();
            return false;  // does NOT end the block
        }
        default:
            emit_call_interp(inst.arm_pc, false);
            return false;
    }
}
} // namespace arm64emu
