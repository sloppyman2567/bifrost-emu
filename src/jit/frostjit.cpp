// jit/frostjit.cpp — FrostJIT: integer/memory/branch IR-op codegen + dispatch.
//
// v1.4.5-alpha (Turn 36): split into multiple files for readability.
// This file holds:
//   - compile-time layout checks (CPU struct offsets)
//   - thread-local watchdog/hotness state
//   - compile_ir_inst() — the integer/memory/branch IR-op switch
//     (FP/SIMD ops are delegated to compile_ir_inst_fp_() in
//     jit_codegen_fp.cpp)
//
// Other FrostJIT methods live in:
//   - jit_interp.cpp        — jit_interp_step() extern "C" trampoline
//   - jit_helpers.cpp       — emit_fmov_helper, emit_call_interp
//   - jit_codegen_fp.cpp    — FP/SIMD IR-op codegen
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
// v1.4.5-alpha (Turn 36): jit_interp_step() was moved to jit_interp.cpp.
extern "C" {
    uint64_t jit_load_mem_slow(Emulator* emu, CPU* cpu, uint64_t addr, int width);
    void     jit_store_mem_slow(Emulator* emu, CPU* cpu, uint64_t addr, uint64_t val, int width);
}

} // namespace arm64emu

namespace arm64emu {

// ── Thread-local per-thread JIT state (Task 3: shared-JIT mode) ────────
// These are thread-local so that multiple threads sharing a single FrostJIT
// instance don't corrupt each other's watchdog/hotness counters. Each
// thread gets its own copy, initialized to the defaults.
thread_local uint64_t FrostJIT::tls_watchdog_last_pc_ = UINT64_MAX;
thread_local uint32_t FrostJIT::tls_watchdog_count_   = 0;
thread_local std::unordered_map<uint64_t, uint32_t> FrostJIT::tls_hot_pc_counts_;


// emit_load_mem / emit_store_mem live in x86_backend.cpp
// (they are pure x86 emission with no regalloc/IR awareness).

bool FrostJIT::compile_ir_inst(const IRInst& inst) {
    // v1.4.5-alpha (Turn 36): FP/SIMD ops are dispatched to
    // compile_ir_inst_fp_() (defined in jit_codegen_fp.cpp) before the
    // integer/memory/branch switch below. The FP handler sets
    // fp_handled_ to true if it recognized the op (regardless of
    // whether it ends the block), and returns the "ends_block" bool.
    // If fp_handled_ is false, the op is not an FP/SIMD op and we fall
    // through to the integer switch.
    fp_handled_ = false;
    bool ends_block = compile_ir_inst_fp_(inst);
    if (fp_handled_) return ends_block;

    switch (inst.op) {
        case IROp::NOP:
            return false;

        case IROp::IMM:
            if (inst.dest) {
                int d = alloc_reg_for(inst.dest, -1);
                if (inst.imm <= 0xFFFFFFFFULL) {
                    emit_mov_imm32_zext(d, static_cast<uint32_t>(inst.imm));
                } else {
                    emit_mov_imm64(d, inst.imm);
                }
            }
            return false;

        case IROp::MOV:
            if (inst.dest) {
                int s = ensure_vreg(inst.src1);
                int d = alloc_reg(s);
                if (d != s) {
                    emit_mov_reg(d, s);
                }
                set_vreg_reg(inst.dest, d);
            }
            return false;

        case IROp::LOAD_REG:
            // dest = arm64_reg[src1]. src1 is the ARM64 reg index.
            //
            // If the ARM reg vreg (src1, in 0..31) is already cached in a
            // host reg — e.g., because a previous FP_F2I / FP_I2F / FMOV_F2G
            // wrote directly to it via store_reg_to_vreg(src1, RAX) without
            // going through STORE_REG — reuse the cached value. Otherwise
            // we would emit a load from cpu.regs[src1], which is STALE
            // (the dirty vreg hasn't been flushed yet). This was the root
            // cause of `fcvtzs w1, d0; add w19, w19, w1` losing the
            // conversion result: FP_F2I cached vreg 1, then LOAD_REG v34, x1
            // reloaded the stale cpu.regs[1] instead of the cached vreg 1.
            {
                kill_vreg(inst.dest);
                int d;
                if (inst.src1 <= 31 && vreg_home_[inst.src1] >= 0) {
                    int s = vreg_home_[inst.src1];
                    d = alloc_reg_excluding(s, -1);
                    emit_mov_reg(d, s);
                } else {
                    d = alloc_reg();
                    emit_load_arm(d, inst.src1);
                }
                set_vreg_reg(inst.dest, d);
            }
            return false;

        case IROp::STORE_REG:
            // arm64_reg[dest] = src1. Write to cpu.regs[dest].
            // DON'T cache dest — leave it uncached so it reloads from
            // cpu.regs[dest] if needed (correct value, just written).
            // DON'T touch src1 — it stays cached in its reg.
            // This avoids all aliasing problems and eliminates spills.
            {
                int s = ensure_vreg(inst.src1);
                emit_store_arm(inst.dest, s);
                // Kill any stale dest mapping (dest's value is now in cpu.regs).
                if (inst.dest <= 31) {
                    kill_vreg(inst.dest);
                }
            }
            return false;

        case IROp::LOAD_MEM: {
            // Memory access via emit_load_mem. The slow path calls
            // jit_load_mem_slow (clobbers caller-saved regs); the fast
            // path uses RAX/RDX/RCX/R10 internally. Now that
            // load_vreg_to_reg is cache-aware, we only need to flush
            // caller-saved dirty vregs — callee-saved vregs (R12/R13/
            // R15) survive the C call and load_vreg_to_reg will mov
            // from them correctly.
            //
            // replaced O(max_vreg_) flush + 6-reg
            // invalidate loop with two O(popcount) bitmask walks.
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            flush_invalidate_host_regs(MEM_CLOBBER);
            load_vreg_to_reg(RAX, inst.src1);
            emit_load_mem(RAX, RAX, static_cast<int32_t>(inst.imm), inst.width, false);
            store_reg_to_vreg(inst.dest, RAX);
            return false;
        }

        case IROp::STORE_MEM: {
            // Same as LOAD_MEM: only flush caller-saved dirty vregs.
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            flush_invalidate_host_regs(MEM_CLOBBER);
            load_vreg_to_reg(RAX, inst.src1);
            load_vreg_to_reg(RCX, inst.src2);
            emit_store_mem(RAX, static_cast<int32_t>(inst.imm), RCX, inst.width);
            return false;
        }

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
            emit_mov_imm64(RDX, Memory::DIRECT_WINDOW_SIZE - w);
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
                //   R9 = addr, RAX = old (from mem), RCX = operand
                //   R8 = old OP operand
                //   retry: lock cmpxchg [R9], R8; jnz retry
                //   dest = RAX (old value)
                emit_mov_reg(R9, RAX);  // R9 = host addr
                // Load current value → RAX (mov rax, [rax])
                if (is_64) { emit_byte(0x48); emit_byte(0x8B); emit_byte(0x00); }
                else if (is_16) { emit_byte(0x66); emit_byte(0x8B); emit_byte(0x00); }
                else { emit_byte(0x8B); emit_byte(0x00); }
                // Compute new = old OP src into R8.
                // R8 = old (mov r8, rax = 4C 8B C0)
                emit_byte(0x4C); emit_byte(0x8B); emit_byte(0xC0);  // mov r8, rax
                // RCX already has src (operand).
                // AND/XOR/OR: result goes into R8 (the rm field with REX.B).
                // Opcode 0x21=AND, 0x31=XOR, 0x09=OR (rm, r form).
                // REX.WRB (0x4D) = W=1, R=1(extends reg to R8-15), B=1(extends rm to R8-15).
                // modrm(3, rcx, r8) = 11 001 000 = 0xC8 → rm=R8, reg=RCX.
                switch (atom_op) {
                    case 0x1: // LDCLR: new = old & ~src
                        emit_not_reg(RCX);  // RCX = ~src
                        emit_byte(0x4D); emit_byte(0x21); emit_byte(0xC8);  // and r8, rcx
                        break;
                    case 0x2: // LDEOR: new = old ^ src
                        emit_byte(0x4D); emit_byte(0x31); emit_byte(0xC8);  // xor r8, rcx
                        break;
                    case 0x3: // LDSET: new = old | src
                        emit_byte(0x4D); emit_byte(0x09); emit_byte(0xC8);  // or r8, rcx
                        break;
                }
                // CAS loop: retry until cmpxchg succeeds.
                // lock cmpxchg [R9], R8
                // REX.WRB (0x4D) = W=1, R=1(reg→R8), B=1(rm→R9).
                size_t loop_start = code_buf_used_;
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

        // ── Binary ALU ops ──
        // Use src1 and src2 in whatever host regs they're already cached in.
        // Only allocate a fresh reg for dest when dest != src1 && dest != src2.
        // This avoids the old "force everything into RAX/RCX" pattern that
        // caused massive stack spilling — values now stay in their host regs
        // across ALU ops, and the register allocator's caching actually pays
        // off.
        //
        // Commutative ops (ADD/AND/OR/XOR/MUL) can swap operands, so
        // dest == src2 is handled by computing in src2's reg. Non-commutative
        // ops (SUB) need a fresh reg when dest == src2 (because we'd lose
        // src2 before the subtraction).
        case IROp::ADD: case IROp::SUB: case IROp::AND:
        case IROp::OR:  case IROp::XOR: case IROp::MUL: {
            clobber_flags();
            bool commutative = (inst.op != IROp::SUB);
            int s1 = ensure_vreg(inst.src1);
            int s2 = ensure_vreg(inst.src2);
            int d;

            // emit_alu_op: emit `d = d op src` for the current inst.op.
            auto emit_alu_op = [&](int d, int src) {
                switch (inst.op) {
                    case IROp::ADD: emit_add_reg(d, src); break;
                    case IROp::SUB: emit_sub_reg(d, src); break;
                    case IROp::AND: emit_and_reg(d, src); break;
                    case IROp::OR:  emit_or_reg(d, src);  break;
                    case IROp::XOR: emit_xor_reg(d, src); break;
                    case IROp::MUL: emit_imul_reg(d, src); break;
                    default: break;
                }
            };

            if (inst.dest == inst.src1) {
                // dest == src1: compute in s1 (in-place modify).
                d = s1;
                emit_alu_op(d, s2);
                vreg_dirty_[inst.dest] = true;
                dirty_host_regs_ |= (1u << d);
            } else if (inst.dest == inst.src2 && commutative) {
                // dest == src2, commutative: compute in s2 (swap operands).
                d = s2;
                emit_alu_op(d, s1);
                vreg_dirty_[inst.dest] = true;
                dirty_host_regs_ |= (1u << d);
            } else {
                // dest != src1 (and not the commutative src2 case):
                // allocate a fresh reg for dest that doesn't collide with
                // s1 or s2, then mov src1 and op src2.
                d = alloc_reg_excluding(s1, s2);
                if (d != s1) emit_mov_reg(d, s1);
                emit_alu_op(d, s2);
                set_vreg_reg(inst.dest, d);
            }
            return false;
        }

        case IROp::SHL: case IROp::SHR:
        case IROp::SAR: case IROp::ROR: {
            // x86 variable shifts use CL for the count. We force src2 into
            // RCX (clobbering its previous occupant), but leave src1 in
            // whatever reg it's cached in. dest is computed in src1's reg
            // when dest == src1, else in a fresh reg excluding src1 and RCX.
            clobber_flags();
            int s1 = ensure_vreg(inst.src1);
            // If s1 is in RCX, move it elsewhere first so forcing src2 into
            // RCX doesn't lose src1.
            if (s1 == RCX) {
                int tmp = alloc_reg_excluding(RCX, -1);
                emit_mov_reg(tmp, RCX);
                reg_vreg_[RCX] = -1;
                vreg_home_[inst.src1] = tmp;
                reg_vreg_[tmp] = inst.src1;
                if (vreg_dirty_[inst.src1]) {
                    dirty_host_regs_ &= ~(1u << RCX);
                    dirty_host_regs_ |= (1u << tmp);
                }
                s1 = tmp;
            }
            // Force src2 into RCX (evict current occupant if any).
            force_vreg_to_reg(inst.src2, RCX);

            // emit_shift: mask CL to 6 bits (x86 shift counts are mod 64)
            // and emit `d = d shift_cl` for the current inst.op.
            auto emit_shift = [&](int d) {
                // For 32-bit ROR: mask CL to 5 bits and use 32-bit ROR
                // (no REX.W) so rotation stays within the lower 32 bits.
                // 64-bit ROR on a zero-extended 32-bit value loses wrap bits.
                bool is_32bit_ror = (inst.op == IROp::ROR && inst.width == 32);
                if (is_32bit_ror) {
                    emit_and_cl_imm8(0x1F);  // mask to 5 bits for 32-bit
                    // Emit 32-bit ROR: no REX.W prefix.
                    emit_byte(rex(false, false, false, d >= 8));
                    emit_byte(0xD3);
                    emit_byte(modrm(3, 1, d & 7));
                } else {
                    emit_and_cl_imm8(0x3F);
                    int kind = (inst.op == IROp::SHL) ? 4
                             : (inst.op == IROp::SHR) ? 5
                             : (inst.op == IROp::SAR) ? 7 : 1;
                    emit_shift_cl(d, kind);
                }
            };

            int d;
            if (inst.dest == inst.src1) {
                d = s1;
                emit_shift(d);
                vreg_dirty_[inst.dest] = true;
                dirty_host_regs_ |= (1u << d);
            } else {
                d = alloc_reg_excluding(s1, RCX);
                if (d != s1) emit_mov_reg(d, s1);
                emit_shift(d);
                set_vreg_reg(inst.dest, d);
            }
            return false;
        }

        case IROp::NOT: {
            clobber_flags();
            int s = ensure_vreg(inst.src1);
            int d = alloc_reg(s);
            if (d != s) emit_mov_reg(d, s);
            emit_not_reg(d);
            set_vreg_reg(inst.dest, d);
            return false;
        }

        case IROp::NEG: {
            clobber_flags();
            int s = ensure_vreg(inst.src1);
            int d = alloc_reg(s);
            if (d != s) emit_mov_reg(d, s);
            emit_neg_reg(d);
            set_vreg_reg(inst.dest, d);
            return false;
        }

        case IROp::SEXT: {
            clobber_flags();  // shifts clobber RFLAGS
            int s = ensure_vreg(inst.src1);
            int d = alloc_reg(s);
            if (d != s) emit_mov_reg(d, s);
            int bits = inst.width;
            if (bits < 64) {
                int sh = 64 - bits;
                emit_shift_imm8(d, 4, sh);
                emit_shift_imm8(d, 7, sh);
            }
            set_vreg_reg(inst.dest, d);
            return false;
        }

        case IROp::ZEXT: {
            int bits = inst.width;
            int s = ensure_vreg(inst.src1);
            int d = alloc_reg(s);
            if (d != s) emit_mov_reg(d, s);
            if (bits < 64) {
                if (bits == 32) {
                    // mov e_d, e_d (zero-extends to 64 bits).
                    // MUST emit REX prefix if d >= R8 (R8-R15 need REX.B
                    // for both reg and rm fields, since they share the
                    // same register).
                    // NOTE: mov r32, r32 does NOT clobber RFLAGS.
                    if (d >= 8) {
                        emit_byte(0x45);
                    }
                    emit_byte(0x89); emit_byte(modrm(3, d&7, d&7));
                } else if (bits == 16) {
                    clobber_flags();  // AND clobbers RFLAGS
                    emit_byte(rex(true,false,false,d>=8));
                    emit_byte(0x81); emit_byte(modrm(3,4,d&7)); emit_u32(0x0000FFFF);
                } else if (bits == 8) {
                    clobber_flags();  // AND clobbers RFLAGS
                    emit_byte(rex(true,false,false,d>=8));
                    emit_byte(0x81); emit_byte(modrm(3,4,d&7)); emit_u32(0x000000FF);
                } else {
                    clobber_flags();  // AND clobbers RFLAGS
                    // Use RCX as scratch for the mask.
                    int tmp = alloc_reg(d);
                    emit_mov_imm64(tmp, (1ULL << bits) - 1);
                    emit_and_reg(d, tmp);
                }
            }
            set_vreg_reg(inst.dest, d);
            return false;
        }

        case IROp::CLZ: {
            // lzcnt rax, rax overwrites RAX, destroying
            // src1's cached value. If src1 is a scratch vreg holding a snapshot
            // of an arch reg (from LOAD_REG), later readers would reload from
            // an uninitialized stack slot. Use the same fix as REV64: allocate
            // a separate dest reg and copy src1 there BEFORE lzcnt.
            //
            // The previous code did `vreg_home_[inst.src1] = -1; vreg_dirty_[inst.src1] = false`
            // which silently dropped a dirty src1 — same bug class as REV64.
            clobber_flags();  // lzcnt doesn't clobber flags, but sub does (32-bit path)
            force_vreg_to_reg(inst.src1, RAX);
            int d = alloc_reg_for(inst.dest, RAX);
            if (d != RAX) {
                emit_mov_reg(d, RAX);  // copy src1 to d, preserving src1 in RAX
            }
            emit_lzcnt_reg(d, d);  // lzcnt d, d (in-place on d)
            // For 32-bit CLZ: x86 LZCNT counts 64-bit leading zeros.
            // ARM 32-bit CLZ should only count the lower 32 bits.
            // Subtract 32 to account for the upper 32 zero bits.
            if (inst.width == 32) {
                // sub d, 32 (use the right encoding for d >= R8)
                if (d >= 8) emit_byte(0x49); else emit_byte(0x48);
                emit_byte(0x83); emit_byte(0xE8 | (d & 7)); emit_byte(0x20);
                // mov e_d, e_d (zero-extend to 64 bits)
                if (d >= 8) emit_byte(0x45);
                emit_byte(0x89); emit_byte(modrm(3, d&7, d&7));
            }
            // dest is already cached in d (via alloc_reg_for) and marked dirty.
            return false;
        }

        case IROp::REV64: {
            // Force src1 into RAX (properly evicts old RAX occupant).
            force_vreg_to_reg(inst.src1, RAX);
            // bswap modifies RAX in place, which
            // destroys v(src1)'s value. If dest != src1, we must preserve
            // src1's value for potential later readers. Allocate a separate
            // dest reg and copy src1 there BEFORE bswap, so src1 stays
            // cached in RAX (or gets reloaded from cpu.regs[]/stack later).
            //
            // The previous code did `bswap eax; store_vreg(dest, RAX)` which
            // silently dropped src1's value if src1 was a scratch vreg
            // (v > 31) — store_vreg cleared src1's dirty flag without
            // spilling, and a later force_vreg_to_reg(src1) loaded from an
            // uninitialized stack slot. This caused jit_simd.elf's
            // `cmp w0, w5` to compute wrong flags and crash.
            int d = alloc_reg_for(inst.dest, RAX);
            if (d != RAX) {
                // Copy src1 to d, then bswap d (preserving src1 in RAX).
                emit_mov_reg(d, RAX);
            }
            // bswap d (in-place if d == RAX, or the copy if d != RAX).
            if (inst.width == 32) {
                // 32-bit bswap: 0F C8+r (no REX.W). REX.B if d >= 8.
                if (d >= 8) emit_byte(0x41);
                emit_byte(0x0F); emit_byte(0xC8 + (d & 7));
                // Zero-extend 32-bit result to 64 bits.
                emit_byte(rex(false, d>=8, false, d>=8));
                emit_byte(0x89); emit_byte(modrm(3, d&7, d&7));
            } else {
                // 64-bit bswap: REX.W 0F C8+r.
                emit_bswap_reg(d);
            }
            // dest is already cached in d (via alloc_reg_for) and marked dirty.
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
                    }
                }
            } else if (inst.dest == inst.src1 && inst.dest != 0) {
                vreg_dirty_[inst.dest] = true;
                dirty_host_regs_ |= (1u << RAX);
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
            // The old code used ensure_vreg(src1, RAX) + ensure_vreg(src2, RCX),
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

        case IROp::BRCOND_ZERO: {
            // CBZ/CBNZ: branch on (val == 0) without touching flags.
            // cond=0 (EQ) → branch if val == 0
            // cond=1 (NE) → branch if val != 0
            // We emit: test val, val; jcc (JE for EQ, JNE for NE)
            // The test sets ZF but we don't materialize flags (CBZ/CBNZ
            // don't modify architectural flags). We save/restore RFLAGS
            // around the test to avoid clobbering pending flags.
            //
            // the previous code did
            //   int s1 = ensure_vreg(inst.src1, RAX);
            //   if (s1 != RAX) emit_mov_reg(RAX, s1);
            // which would overwrite RAX without evicting whatever dirty
            // vreg was cached there — typically the value computed by the
            // immediately preceding SBFM/UBFM/ADDS that wrote to an
            // architectural reg. The epilogue's flush_all_vregs() would
            // then write the next-PC value (left in RAX by this branch)
            // to that architectural reg, corrupting it.
            //
            // Fix: evict any dirty vreg in RAX BEFORE loading the test
            // value, and drop RAX's cache mapping so flush_all_vregs
            // can't miswrite it.
            clobber_flags();  // materialize any pending flags first
            // use drop_vreg — evicts if dirty, drops if not.
            if (reg_vreg_[RAX] >= 0) {
                drop_vreg(reg_vreg_[RAX]);
            }
            int s1 = ensure_vreg(inst.src1, RAX);
            if (s1 != RAX) emit_mov_reg(RAX, s1);
            // RAX now holds the test value. Drop RAX's cache mapping so
            // the upcoming `mov eax, <pc>` doesn't corrupt any vreg.
            // use drop_vreg — if RAX holds a dirty vreg,
            // evict it to memory first so the value is preserved.
            if (reg_vreg_[RAX] >= 0) {
                drop_vreg(reg_vreg_[RAX]);
            }
            // Save RFLAGS (CBZ/CBNZ don't modify architectural flags).
            emit_pushfq();
            // test rax, rax
            emit_test_reg(RAX, RAX);
            // jcc to taken target
            uint8_t cc = (inst.cond == 0) ? 4 /*JE*/ : 5 /*JNE*/;
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
            // Not taken: RAX = fall-through.
            uint64_t fall = inst.arm_pc + 4;
            emit_mov_imm_to_rax(fall);
            // Restore RFLAGS before jumping to epilogue
            emit_popfq();
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});
            // Taken: patch jcc to here.
            int32_t taken_rel = static_cast<int32_t>(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);
            // Restore RFLAGS (CBZ/CBNZ don't modify flags)
            emit_popfq();
            emit_mov_imm_to_rax(inst.imm);
            rax_holds_next_pc_ = true;
            unchainable_end_ = true;  // conditional branch
            return true;
        }

        case IROp::BRCOND_BIT: {
            // TBZ/TBNZ: branch on ((val >> bit) & 1) without touching flags.
            // cond=0 (EQ) → branch if bit == 0 (TBZ)
            // cond=1 (NE) → branch if bit == 1 (TBNZ)
            // We emit: bt rax, bit; jcc (JNC for bit==0, JC for bit==1)
            // BT sets CF = (val >> bit) & 1. We save/restore RFLAGS.
            // same RAX eviction as BRCOND_ZERO —
            // see the comment there for the rationale.
            clobber_flags();
            // use drop_vreg — evicts if dirty, drops if not.
            if (reg_vreg_[RAX] >= 0) {
                drop_vreg(reg_vreg_[RAX]);
            }
            int s1 = ensure_vreg(inst.src1, RAX);
            if (s1 != RAX) emit_mov_reg(RAX, s1);
            // use drop_vreg — if RAX holds a dirty vreg,
            // evict it to memory first so the value is preserved.
            if (reg_vreg_[RAX] >= 0) {
                drop_vreg(reg_vreg_[RAX]);
            }
            emit_pushfq();
            // bt rax, imm8  — 0x48 0x0F 0xBA /5 r/m, imm8
            emit_byte(rex(true, false, false, RAX >= 8));
            emit_byte(0x0F); emit_byte(0xBA);
            emit_byte(modrm(3, 5, RAX & 7));
            emit_byte(inst.width);  // bit number
            // jcc: TBZ (cond=0, EQ) → JNC (bit==0, CF=0) → JAE (cc=3)
            //      TBNZ (cond=1, NE) → JC (bit==1, CF=1) → JB (cc=2)
            uint8_t cc = (inst.cond == 0) ? 3 /*JNC/JAE*/ : 2 /*JC/JB*/;
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
            // Not taken: RAX = fall-through.
            uint64_t fall = inst.arm_pc + 4;
            emit_mov_imm_to_rax(fall);
            emit_popfq();
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});
            // Taken: patch jcc to here.
            int32_t taken_rel = static_cast<int32_t>(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);
            emit_popfq();
            emit_mov_imm_to_rax(inst.imm);
            rax_holds_next_pc_ = true;
            unchainable_end_ = true;
            return true;
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

        case IROp::CSEL: {
            // Native CSEL/CSINC/CSINV/CSNEG via jcc+mov.
            //
            // Semantics:
            //   CSEL  Rd = cond ? Rn : Rm
            //   CSINC Rd = cond ? Rn : (Rm + 1)
            //   CSINV Rd = cond ? Rn : ~Rm
            //   CSNEG Rd = cond ? Rn : -Rm
            //
            // Strategy:
            //   1. Ensure flags in host RFLAGS.
            //   2. Flush all vregs (save/restore flags around flush).
            //   3. Load src1 → RAX, src2 → RCX (with XZR special case).
            //   4. For CSINC/CSINV/CSNEG: transform RCX (pushfq/popfq
            //      to preserve flags). For 32-bit ops, zero-extend RCX
            //      after the transform.
            //   5. RDX = RAX (d = src1).
            //   6. jcc skip (if cond TRUE, keep src1); else mov rdx, rcx.
            //   7. For 32-bit ops, zero-extend RDX.
            //   8. Store RDX to dest.
            //
            // Carry polarity: arm_cond_to_x86() assumes SUB convention
            // (ARM C = NOT x86 CF). When flags came from ADD/TST
            // (carry_is_direct), CS/CC need swapped mapping, HI/LS need cmc.

            // Track whether we loaded flags from pstate. If we did, the
            // flags in pstate are already correct and the epilogue should
            // NOT re-materialize (the loaded x86 flags have inverted CF,
            // and materialize(false) would corrupt the C flag).
            // If we didn't load (flags were already in host), the epilogue
            // must still materialize them.
            bool loaded_from_pstate = !flags_in_host_;

            // Ensure flags in host.
            if (!flags_in_host_) {
                flush_all_vregs();
                emit_load_flags_from_pstate();
                // emit_load_flags_from_pstate sets x86 CF = ARM C XOR from_sub.
                // Normalize to SUB convention (x86 CF = NOT ARM C) so the
                // default arm_cond_to_x86() mapping works correctly for ALL
                // conditions (CS/CC/HI/LS included) regardless of whether
                // the flags originally came from ADD or SUB.
                emit_normalize_cf_to_sub_convention();
                // Drop all cache mappings but DON'T clear flags_in_host_.
                bool saved_fih2 = flags_in_host_;
                invalidate_all_vregs();
                flags_in_host_ = saved_fih2;
                flags_in_host_ = true;
                flags_from_sub_ = true;  // CF is now in SUB convention
            }
            // Resolve condition code. With flags_from_sub_=true (SUB convention,
            // whether originally from SUB or normalized after loading),
            // resolve_arm_cond_with_carry uses the default mapping.
            bool need_cmc = false;
            uint8_t cc = resolve_arm_cond_with_carry(inst.cond, need_cmc);
            // Save flags, flush vregs, restore flags. CRITICAL: preserve
            // flags_in_host_ — invalidate_all_vregs would clear it, but
            // pushfq/popfq preserves the actual flags.
            bool saved_fih = flags_in_host_;
            bool saved_ffs = flags_from_sub_;
            emit_pushfq();
            flush_all_vregs();
            invalidate_all_vregs();
            emit_popfq();
            flags_in_host_ = saved_fih;
            flags_from_sub_ = saved_ffs;
            if (need_cmc) emit_byte(0xF5);  // cmc

            // Load src1 → RAX, src2 → RCX.
            if (inst.src1 == 32) emit_mov_imm32_zext(RAX, 0);
            else load_vreg_to_reg(RAX, inst.src1);
            if (inst.src2 == 32) emit_mov_imm32_zext(RCX, 0);
            else load_vreg_to_reg(RCX, inst.src2);

            // For CSINC/CSINV/CSNEG, transform RCX (the "else" value).
            if (inst.op != IROp::CSEL) {
                emit_pushfq();
                if (inst.op == IROp::CSINC) {
                    // add rcx, 1
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xC1); emit_byte(0x01);
                } else if (inst.op == IROp::CSINV) {
                    emit_not_reg(RCX);
                } else {  // CSNEG
                    emit_neg_reg(RCX);
                }
                emit_popfq();
            }

            // RDX = RAX (d = src1).
            emit_mov_reg(RDX, RAX);
            // jcc skip (if cond TRUE, keep src1 in RDX).
            // Use placeholder+patch instead of hardcoded offset.
            size_t jcc_off = emit_jcc_rel8_placeholder(cc);
            // mov rdx, rcx (cond FALSE: rdx = src2)
            emit_byte(0x48); emit_byte(0x89); emit_byte(0xCA);
            // Patch jcc to skip over the 3-byte mov.
            patch_jcc_rel8(jcc_off, 3);

            // Store RDX to dest, then cache it in RDX.
            store_reg_to_vreg(inst.dest, RDX);
            set_vreg_reg(inst.dest, RDX);
            // If we loaded flags from pstate, clear flags_in_host_ so the
            // epilogue doesn't re-materialize. The flags in pstate are
            // already correct (CSEL doesn't modify flags). Re-materializing
            // with the loaded x86 flags would corrupt the C flag: the load
            // inverted CF based on the from_sub bit, and materialize(false)
            // would set ARM C = x86 CF (the inverted value), losing the C.
            // If we loaded flags from pstate, clear flags_in_host_ so the
            // epilogue doesn't re-materialize. The flags in pstate are
            // already correct (CSEL doesn't modify flags). Re-materializing
            // with the loaded x86 flags would corrupt the C flag: the load
            // inverted CF based on the from_sub bit, and materialize(false)
            // would set ARM C = x86 CF (the inverted value), losing the C.
            // If flags were already in host (not loaded), keep flags_in_host_
            // so the epilogue materializes them normally.
            if (loaded_from_pstate) {
                flags_in_host_ = false;
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

        case IROp::BRCOND: {
            if (!flags_in_host_) {
                flush_all_vregs();
                emit_load_flags_from_pstate();
                // Normalize CF to SUB convention so the default
                // arm_cond_to_x86() mapping works for all conditions.
                emit_normalize_cf_to_sub_convention();
                flags_from_sub_ = true;  // CF is now in SUB convention
                invalidate_all_vregs();
                flags_in_host_ = true;
            }
            // Resolve condition code, handling carry polarity (ADD/TST vs SUB).
            // With flags_from_sub_=true (SUB convention, whether originally
            // from SUB or normalized), the default mapping is used.
            bool need_cmc_for_hi_ls = false;
            uint8_t cc = resolve_arm_cond_with_carry(inst.cond, need_cmc_for_hi_ls);

            // Emit the JCC first — it consumes host RFLAGS directly, so no
            // flag materialization is needed before it. Flags are materialized
            // to pstate on each path separately (the next block may read pstate).
            // Stores don't clobber RFLAGS, so flush_all_vregs needs no
            // pushfq/popfq wrapper here.
            flush_all_vregs();
            if (need_cmc_for_hi_ls) {
                emit_byte(0xF5);  // cmc — invert CF for HI/LS after ADD/TST
            }
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);

            // ── Fall-through path: materialize flags, set RAX = fall-through PC ──
            // If CMC was emitted (for HI/LS after ADD/TST), re-invert CF so
            // materialize_flags_to_pstate sees the original carry flag.
            if (need_cmc_for_hi_ls) {
                emit_byte(0xF5);  // cmc — restore CF to original
            }
            materialize_flags_to_pstate();
            emit_mov_imm_to_rax(inst.arm_pc + 4);
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});

            // ── Taken path ──
            int32_t taken_rel = static_cast<int32_t>(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);

            // If CMC was emitted, re-invert CF before materializing flags.
            if (need_cmc_for_hi_ls) {
                emit_byte(0xF5);  // cmc — restore CF to original
            }
            // Materialize flags to pstate (the taken-target block may read them).
            materialize_flags_to_pstate();

            // ── Self-loop chaining ──
            // If the taken target is the block's own start PC, emit a 5-byte
            // `jmp rel32` placeholder. After the block is fully compiled,
            // translate_block patches this slot to jump directly to the block
            // body start, creating a tight loop that skips the epilogue,
            // dispatcher, and prologue. This is the single biggest win for
            // tight loops (e.g. bench_mips: 7.4s → 1.4s).
            //
            // The placeholder is followed by the normal epilogue path
            // (set RAX = target PC, store PC, restore regs, ret) as a
            // fallback. translate_block overwrites the placeholder with
            // the real jmp, so the fallback only runs if the slot is not
            // patched (which never happens in practice — the slot is always
            // patched when has_selfloop_slot_ is true).
            //
            // Disable with BIFROST_NO_SELFLOOP=1 for debugging.
            static bool no_selfloop_ = (getenv("BIFROST_NO_SELFLOOP") != nullptr);
            bool is_selfloop = (inst.imm == current_start_pc_);
            if (is_selfloop && !no_selfloop_) {
                has_selfloop_slot_ = true;
                selfloop_patch_off_ = code_buf_used_;
                emit_byte(0xE9); emit_u32(0);  // jmp rel32 placeholder
            }

            emit_mov_imm_to_rax(inst.imm);
            rax_holds_next_pc_ = true;
            flags_in_host_ = false;
            unchainable_end_ = true;  // conditional branch — runtime-dependent next PC
            return true;
        }

        case IROp::BRCOND_FALLTHRU: {
            flush_all_vregs();
            emit_mov_imm_to_rax(inst.imm);
            rax_holds_next_pc_ = true;
            // Unconditional branch with statically-known target — record
            // it for block chaining. try_chain_block() will patch the
            // epilogue's chain slot to jmp directly to the target block
            // once it has been translated.
            chain_target_pc_ = inst.imm;
            return true;
        }

        case IROp::CALL_INTERP:
            emit_call_interp(inst.arm_pc, false);
            return false;

        case IROp::SVC:
            emit_call_interp(inst.arm_pc, true);
            rax_holds_next_pc_ = true;
            unchainable_end_ = true;  // syscall may modify PC
            return true;


        // These are very common (SXTB/SXTH/SXTW/UXTB/UXTH/UXTW/LSL/LSR/
        // ASR/SBFIZ/UBFIZ/BFI/BFXIL) and falling back to CALL_INTERP
        // for each one is both slow and a source of correctness bugs
        // (the interp call's PC-change check can cause spurious block
        // exits). Implement them directly.
        //
        // ARM64 bitfield semantics (width W = 32 or 64):
        //   SBFM Rd, Rn, #immr, #imms:
        //     R = ROR(Rn, immr)  (rotate right by immr)
        //     if imms < immr:  Rd = sign_extend(R[W-1:imms], imms+1 bits)
        //     else:            Rd = sign_extend(R[imms:0], imms-immr+1 bits)
        //   UBFM Rd, Rn, #immr, #imms:
        //     R = ROR(Rn, immr)
        //     if imms < immr:  Rd = zero_extend(R[imms:0], imms+1 bits)
        //     else:            Rd = R[imms:0] zero-extended (i.e. extract)
        //   BFM Rd, Rn, #immr, #imms:
        //     inserts Rn's bits into Rd (preserve outside bits)
        //   EXTR Rd, Rn, Rm, #imms:
        //     Rd = (Rn:Rm) >> imms
        case IROp::SBFM: case IROp::UBFM: {
            int width = inst.sf ? 64 : 32;
            int immr = inst.immr;
            int imms = inst.imms;
            // Load src into RAX.
            // flush+invalidate FIRST so the
            // cache is empty and the subsequent memory access can't
            // interact with stale mappings. We then write the result
            // directly to the dest vreg's memory home and re-cache it.
            clobber_flags();  // shifts/ands clobber RFLAGS
            flush_all_vregs();
            invalidate_all_vregs();
            load_vreg_to_reg(RAX, inst.src1);

            // Handle common aliases efficiently:
            // - LSL (imms < immr): shift left by (width - immr)
            // - LSR (imms == width-1, UBFM): shift right by immr
            // - ASR (imms == width-1, SBFM): arithmetic shift right by immr

            // LSL: imms < immr (e.g. lsl w0, w0, #2 = UBFM w0, w0, #30, #31)
            // UBFM semantics for imms < immr:
            //   field = src & ((1 << (imms+1)) - 1)   [take low imms+1 bits]
            //   result = field << (width - immr)       [shift left to position]
            // the previous code did shl THEN and, which
            // zeroed the result for shift >= 32. For example, lsl x0, x0, #32
            // (immr=32, imms=31): shl rax,32 → 0x100000000, then and rax,
            // 0xFFFFFFFF → 0. The correct order is: mask FIRST, then shift.
            if (imms < immr) {
                int sh = width - immr;
                if (sh > 0 && sh < width) {
                    // Mask to imms+1 bits FIRST.
                    uint64_t mask = (1ULL << (imms + 1)) - 1;
                    emit_mov_imm64(RDX, mask);
                    emit_and_reg(RAX, RDX);
                    // THEN shift left by sh.
                    if (width == 32) {
                        emit_byte(0xC1); emit_byte(modrm(3, 4, RAX & 7)); emit_byte(static_cast<uint8_t>(sh));
                    } else {
                        emit_shift_imm8(RAX, 4, sh);
                    }
                    if (width == 32) {
                        if (RAX >= 8) emit_byte(0x45);
                        emit_byte(0x89); emit_byte(modrm(3, RAX&7, RAX&7));
                    }
                    // Write result directly to dest's memory home, then cache.
                    store_reg_to_vreg(inst.dest, RAX);
                    set_vreg_reg(inst.dest, RAX);
                    return false;
                }
            }

            // LSR (UBFM) or ASR (SBFM): imms == width-1
            if (imms == width - 1) {
                if (immr > 0) {
                    if (width == 32) {
                        if (immr <= 31) {
                            if (inst.op == IROp::SBFM) {
                                // SAR (arithmetic)
                                emit_byte(0xC1); emit_byte(modrm(3, 7, RAX & 7)); emit_byte(static_cast<uint8_t>(immr));
                            } else {
                                // SHR (logical)
                                emit_byte(0xC1); emit_byte(modrm(3, 5, RAX & 7)); emit_byte(static_cast<uint8_t>(immr));
                            }
                        }
                    } else {
                        if (inst.op == IROp::SBFM) {
                            emit_shift_imm8(RAX, 7, immr);
                        } else {
                            emit_shift_imm8(RAX, 5, immr);
                        }
                    }
                }
                if (width == 32) {
                    if (RAX >= 8) emit_byte(0x45);
                    emit_byte(0x89); emit_byte(modrm(3, RAX&7, RAX&7));
                }
                store_reg_to_vreg(inst.dest, RAX);
                set_vreg_reg(inst.dest, RAX);
                return false;
            }

            // General case: ROR then extract then (for SBFM) sign-extend
            if (immr != 0) {
                emit_mov_imm32_zext(RCX, immr);
                if (width == 64) {
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xE1); emit_byte(0x3F);
                    emit_byte(rex(true,false,false,false));
                    emit_byte(0xD3); emit_byte(modrm(3,1,RAX&7));
                } else {
                    emit_byte(0x48); emit_byte(0x83); emit_byte(0xE1); emit_byte(0x1F);
                    emit_byte(0xD3); emit_byte(modrm(3, 1, RAX & 7));
                    emit_byte(rex(true,false,false,false));
                    emit_byte(0x81); emit_byte(modrm(3,4,RAX&7)); emit_u32(0xFFFFFFFF);
                }
            }
            // Extract bits [imms-immr:0] from RAX (after rotate).
            // after ROR by immr, the field that was at
            // [imms:immr] in the original is now at [imms-immr:0]. So the
            // mask must be (imms-immr+1) bits wide, NOT (imms+1) bits.
            // The old code used (1<<(imms+1))-1 which extracted too many
            // bits, pulling in garbage from above the field. This broke
            // musl's get_stride (ubfx x0, x0, #6, #6) which extracts a
            // 6-bit field — the JIT returned 0x57 instead of 0x17,
            // corrupting the malloc size class lookup.
            if (imms < width - 1) {
                int field_width = imms - immr + 1;
                uint64_t mask = (field_width >= 64) ? ~0ULL : ((1ULL << field_width) - 1);
                emit_mov_imm64(RDX, mask);
                emit_and_reg(RAX, RDX);
            }
            // For SBFM: sign-extend from the field's sign bit.
            // After ROR+mask, the field occupies bits [field_width-1:0]
            // where field_width = imms - immr + 1. The sign bit is at
            // bit (imms - immr). Sign-extend by shifting left then right.
            if (inst.op == IROp::SBFM && imms < width - 1) {
                int field_width = imms - immr + 1;
                int sh = width - field_width;
                if (sh > 0) {
                    emit_shift_imm8(RAX, 4, sh);
                    emit_shift_imm8(RAX, 7, sh);
                }
            }
            // For 32-bit ops: zero-extend result to 64 bits.
            if (width == 32) {
                emit_byte(0x89); emit_byte(modrm(3, RAX&7, RAX&7));
            }
            store_reg_to_vreg(inst.dest, RAX);
            set_vreg_reg(inst.dest, RAX);
            return false;
        }

        // Defensive fallback: BFM is normally decomposed to SHL+SHR+
        // OR+AND+OR in ir_translate.cpp. Falls back to interpreter.
        case IROp::BFM: {
            emit_call_interp(inst.arm_pc, false);
            return false;
        }

        // Defensive fallback: EXTR is normally decomposed to SHL+SHR+OR
        // in ir_translate.cpp. Falls back to interpreter.
        case IROp::EXTR: {
            emit_call_interp(inst.arm_pc, false);
            return false;
        }

        // Defensive fallback: RBIT/REV16/REV32 are decomposed to SWAR
        // shift/mask patterns in ir_translate.cpp, and CLS to SAR+XOR+
        // CLZ+SUB. Falls back to interpreter if re-emitted.
        case IROp::RBIT: case IROp::CLS: case IROp::REV16: case IROp::REV32:
            emit_call_interp(inst.arm_pc, false);
            kill_vreg(inst.dest);
            {
                int d = alloc_reg();
                int rd = static_cast<int>(inst.imm);
                emit_load_arm(d, rd);
                set_vreg_reg(inst.dest, d);
            }
            return false;

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
                flush_all_vregs();
                emit_load_flags_from_pstate();
                // Normalize CF to SUB convention (x86 CF = NOT ARM C).
                emit_normalize_cf_to_sub_convention();
                invalidate_all_vregs();
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

        case IROp::CCMP: {
            // CCMP/CCMN: if cond then set flags from (rn - rm) [CCMP]
            //            or (rn + rm) [CCMN]; else set flags to imm nzcv.
            // inst.width = nzcv field (4 bits), inst.cond = ARM cond,
            // inst.flags_op = 1 for CCMP (sub), 0 for CCMN (add).
            bool is_sub = (inst.flags_op == 1);
            uint8_t nzcv = inst.width & 0xF;

            // Compute x86 cc (true when ARM cond is TRUE).
            bool need_cmc = false;
            uint8_t cc = resolve_arm_cond_with_carry(inst.cond, need_cmc);

            // Ensure flags in host.
            if (!flags_in_host_) {
                flush_all_vregs();
                emit_load_flags_from_pstate();
                // emit_load_flags_from_pstate sets x86 CF = ARM C XOR from_sub.
                // Normalize to SUB convention (x86 CF = NOT ARM C) so the
                // default arm_cond_to_x86() mapping works correctly for ALL
                // conditions (CS/CC/HI/LS included) regardless of whether
                // the flags originally came from ADD or SUB. Without this
                // normalization, CCMP after ADDS would use the wrong Jcc
                // for the CC/CS condition (taking the wrong branch), causing
                // pstate divergences like jit=0x8000000 ref=0x88000000
                // (JIT skipped the compare; interpreter did it).
                emit_normalize_cf_to_sub_convention();
                // Drop all cache mappings WITHOUT clearing flags_in_host_.
                // use invalidate_all_vregs but preserve flags_in_host_.
                {
                    bool saved_fih3 = flags_in_host_;
                    invalidate_all_vregs();
                    flags_in_host_ = saved_fih3;
                }
                flags_in_host_ = true;
                flags_from_sub_ = true;  // CF is now in SUB convention
            }
            if (need_cmc) emit_byte(0xF5);

            // Load src1 (rn) → RAX, src2 (rm) → RCX.
            // Use force_two_vregs_to for proper aliasing/eviction handling.
            // The old ensure_vreg + mov pattern could lose src1's value when
            // the second ensure_vreg evicted RAX under register pressure.
            if (inst.src1 == 32 && inst.src2 == 32) {
                emit_mov_imm32_zext(RAX, 0);
                emit_mov_imm32_zext(RCX, 0);
            } else if (inst.src1 == 32) {
                force_vreg_to_reg(inst.src2, RCX);
                emit_mov_imm32_zext(RAX, 0);
            } else if (inst.src2 == 32) {
                force_vreg_to_reg(inst.src1, RAX);
                emit_mov_imm32_zext(RCX, 0);
            } else {
                force_two_vregs_to(inst.src1, RAX, inst.src2, RCX);
            }

            // CCMP clobbers RAX, RCX, RDX (via emit_materialize_flags on the
            // compare path, and via emit_mov_imm32_zext(RDX,...) on the else
            // path). force_two_vregs_to handled RAX/RCX eviction, but RDX
            // may still hold a live vreg (e.g., new_sp from a prior ADD).
            // Spill it before clobbering. Without this, the vreg in RDX is
            // lost — its value is only in the host reg, and the CCMP
            // overwrites it. This was the root cause of the FWD crash on
            // `toybox ls /` (v37 = new_sp was in RDX, lost to CCMP, then
            // STORE_MEM [v37+0x40] used garbage as the base address).
            flush_invalidate_host_regs((1u << RDX) | (1u << RAX) | (1u << RCX));

            // jcc do_compare (if cond TRUE, do the compare)
            size_t jcc_to_compare = emit_jcc_rel32_placeholder(cc);
            // --- else path: cond FALSE, set pstate = nzcv ---
            uint32_t pstate_else = (static_cast<uint32_t>(nzcv) << 28);
            if (is_sub) pstate_else |= (1U << 27);
            emit_mov_imm32_zext(RDX, pstate_else);
            emit_store32(CPU_REG, PSTATE_OFF, RDX);
            // Jump to end.
            size_t jmp_to_end = emit_jmp_rel32_placeholder();
            // --- cond TRUE path: do the compare ---
            size_t compare_off = code_buf_used_;
            if (is_sub) emit_sub_reg(RAX, RCX);
            else        emit_add_reg(RAX, RCX);
            // Materialize flags to pstate.
            emit_materialize_flags(is_sub);
            size_t end_off = code_buf_used_;

            // Patch jumps.
            int32_t rel_compare = static_cast<int32_t>(compare_off - (jcc_to_compare + 6));
            patch_jcc_rel32(jcc_to_compare, rel_compare);
            int32_t rel_end = static_cast<int32_t>(end_off - (jmp_to_end + 5));
            patch_jmp_rel32(jmp_to_end, rel_end);

            // After both paths, RAX/RCX/RDX hold garbage (materialize_flags
            // or mov_imm32 clobbered them). Drop any stale cache mappings
            // so later instructions reload from memory instead of using
            // the clobbered host regs.
            invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            flags_in_host_ = false;
            return false;
        }

        // ── UDIV / SDIV — native x86 div/idiv ────────────────────────
        case IROp::UDIV:
        case IROp::SDIV: {
            // ARM64 UDIV/SDIV by zero returns 0 (no exception).
            // x86 div/idiv by zero raises SIGFPE. We emit a test+jz
            // to skip the div and set result=0 when divisor is zero.
            clobber_flags();
            // use bitmask helpers instead of open-coded loop.
            flush_invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
            load_vreg_to_reg(RAX, inst.src1);  // dividend
            load_vreg_to_reg(RCX, inst.src2);  // divisor

            // For 32-bit division, zero-extend EAX into RAX (clear upper 32).
            // The dividend must be in EAX; if we loaded a 64-bit value,
            // the upper bits would corrupt the 32-bit div.
            if (inst.width == 32) {
                // mov eax, eax (zero-extends to RAX on x86-64)
                emit_byte(0x89); emit_byte(0xC0);
                // mov ecx, ecx (zero-extends divisor)
                emit_byte(0x89); emit_byte(0xC9);
            }

            // test rcx, rcx
            emit_test_reg(RCX, RCX);
            // jz zero_div (jump to xor eax,eax if divisor == 0)
            size_t jz_patch = emit_jcc_rel32_placeholder(4);  // JE
            // --- non-zero divisor path ---
            if (inst.width == 32) {
                // 32-bit division: use div/idiv on EAX.
                // xor edx, edx (clear upper for unsigned) or cdq (sign-extend)
                if (inst.op == IROp::UDIV) {
                    emit_byte(0x31); emit_byte(0xD2);  // xor edx, edx
                    emit_byte(0xF7); emit_byte(0xF1);  // div ecx
                } else {
                    emit_byte(0x99);                    // cdq
                    emit_byte(0xF7); emit_byte(0xF9);  // idiv ecx
                }
            } else {
                // 64-bit division
                if (inst.op == IROp::UDIV) {
                    emit_byte(0x48); emit_byte(0x31); emit_byte(0xD2);  // xor rdx, rdx
                    emit_byte(0x48); emit_byte(0xF7); emit_byte(0xF1);  // div rcx
                } else {
                    emit_byte(0x48); emit_byte(0x99);                    // cqo
                    emit_byte(0x48); emit_byte(0xF7); emit_byte(0xF9);  // idiv rcx
                }
            }
            // jmp past_zero
            size_t jmp_patch = emit_jmp_rel32_placeholder();
            // --- zero divisor path: result = 0 ---
            size_t zero_off = code_buf_used_;
            patch_jcc_rel32(jz_patch, static_cast<int32_t>(zero_off - (jz_patch + 6)));
            emit_byte(0x48); emit_byte(0x31); emit_byte(0xC0);  // xor rax, rax
            // --- past_zero ---
            size_t past_off = code_buf_used_;
            patch_jmp_rel32(jmp_patch, static_cast<int32_t>(past_off - (jmp_patch + 5)));

            // For 32-bit results, writing to EAX zero-extends to RAX.
            int d = alloc_reg_for(inst.dest, RAX);
            if (d != RAX) emit_mov_reg(d, RAX);
            set_vreg_reg(inst.dest, d);  // cache the result
            return false;
        }

        // ── SMADDL / UMADDL — widening multiply-accumulate ──────────
        case IROp::SMADDL:
        case IROp::UMADDL: {
            // SMADDL: dest = acc + (int64)(int32)src1 * (int64)(int32)src2
            // UMADDL: dest = acc + (uint64)(uint32)src1 * (uint64)(uint32)src2
            // x86: imul rax, rcx (64-bit multiply); add rax, acc
            clobber_flags();
            // use bitmask helpers instead of open-coded loop.
            flush_invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
            // Load src1 (32-bit, sign/zero-extended) into RAX
            load_vreg_to_reg(RAX, inst.src1);
            if (inst.op == IROp::SMADDL) {
                // cdqe (sign-extend EAX into RAX)
                emit_byte(0x48); emit_byte(0x98);
            } else {
                // mov eax, eax (zero-extend)
                emit_byte(0x89); emit_byte(0xC0);
            }
            // Load src2 (32-bit, extended) into RCX
            load_vreg_to_reg(RCX, inst.src2);
            if (inst.op == IROp::SMADDL) {
                // Sign-extend ECX into RCX (cdqe on RCX isn't directly
                // available; use movsxd rcx, ecx instead).
                // 48 63 c9 = movsxd rcx, ecx
                emit_byte(0x48); emit_byte(0x63); emit_byte(0xC9);
            } else {
                // mov ecx, ecx (zext)
                emit_byte(0x89); emit_byte(0xC9);
            }
            // imul rax, rcx (64-bit multiply — result in RAX, no RDX needed)
            emit_byte(0x48); emit_byte(0x0F); emit_byte(0xAF); emit_byte(0xC1);
            // Load accumulator from cpu.regs[inst.cond] directly.
            // inst.cond is the ARM64 register index (0-31). If 31 (XZR),
            // the accumulator is 0 (xor rdx, rdx). Otherwise, load from
            // cpu.regs[cond] = [CPU_REG + cond*8].
            if (inst.cond == 31) {
                // XZR — accumulator is 0
                emit_byte(0x48); emit_byte(0x31); emit_byte(0xD2);  // xor rdx, rdx
            } else {
                // Load cpu.regs[cond] into RDX
                int32_t acc_off = REGS_OFF + static_cast<int>(inst.cond) * 8;
                emit_load(RDX, CPU_REG, acc_off);
            }
            // add rax, rdx
            emit_byte(0x48); emit_byte(0x01); emit_byte(0xD0);
            int d = alloc_reg_for(inst.dest, RAX);
            if (d != RAX) emit_mov_reg(d, RAX);
            set_vreg_reg(inst.dest, d);  // cache the result (like UDIV)
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
                off = PSTATE_OFF;
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
                // FPCR/FPSR are uint32_t fields — use emit_load32 to avoid
                // leaking the adjacent CPU member into the high 32 bits.
                if (op1 == 3 && crn == 4 && crm == 4 && (op2 == 0 || op2 == 1))
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

        // ── SMSUBL / UMSUBL — widening multiply-subtract ────────────
        case IROp::SMSUBL:
        case IROp::UMSUBL: {
            // SMSUBL: dest = acc - (int64)(int32)src1 * (int32)src2
            // UMSUBL: dest = acc - (uint64)(uint32)src1 * (uint32)src2
            clobber_flags();
            // use bitmask helpers instead of open-coded loop.
            flush_invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
            load_vreg_to_reg(RAX, inst.src1);
            if (inst.op == IROp::SMSUBL) {
                emit_byte(0x48); emit_byte(0x98);  // cdqe
            } else {
                emit_byte(0x89); emit_byte(0xC0);  // mov eax, eax
            }
            load_vreg_to_reg(RCX, inst.src2);
            if (inst.op == IROp::SMSUBL) {
                emit_byte(0x48); emit_byte(0x63); emit_byte(0xC9);  // movsxd rcx, ecx
            } else {
                emit_byte(0x89); emit_byte(0xC9);  // mov ecx, ecx
            }
            emit_byte(0x48); emit_byte(0x0F); emit_byte(0xAF); emit_byte(0xC1);  // imul rax, rcx
            // Load accumulator
            if (inst.cond == 31) {
                emit_byte(0x48); emit_byte(0x31); emit_byte(0xD2);  // xor rdx, rdx
            } else {
                emit_load(RDX, CPU_REG, REGS_OFF + static_cast<int>(inst.cond) * 8);
            }
            // sub rdx, rax (dest = acc - product)
            emit_byte(0x48); emit_byte(0x29); emit_byte(0xC2);  // sub rdx, rax
            int d = alloc_reg_for(inst.dest, RDX);
            if (d != RDX) emit_mov_reg(d, RDX);
            set_vreg_reg(inst.dest, d);
            return false;
        }


        default:
            emit_call_interp(inst.arm_pc, false);
            return false;
    }
}



} // namespace arm64emu
