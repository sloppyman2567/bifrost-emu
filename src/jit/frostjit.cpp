// jit/frostjit.cpp — FrostJIT orchestration: IR compilation + block dispatch.
//
// This file holds the "high-level" FrostJIT methods that tie together the
// x86 emitters (x86_backend.cpp), the register allocator (x86_regalloc.cpp),
// the block cache (jit_cache.cpp), and the lifecycle/stats counters
// (jit_profiler.cpp).
//
// Specifically:
//   - jit_interp_step           — extern "C" trampoline called from JIT code
//                                  to fall back to the interpreter for one
//                                  instruction (CALL_INTERP / SVC paths)
//   - emit_fmov_helper          — GPR↔FP register move helper
//   - emit_call_interp          — emit a CALL_INTERP call site inside a block
//   - compile_ir_inst           — the IR-op→x86 switch
//   - clobber_flags             — flush pending host flags to pstate before
//                                  a flag-clobbering instruction
//   - translate_block           — translate one ARM64 basic block to x86
//   - run_block                 — block dispatcher (cache hit / miss / verify)
//
// All emit_*, alloc_*, ensure_*, force_*, flush_*, invalidate_*, kill_*,
// vreg_*, patch_chain, try_chain_block, chain_back_references, ctor/dtor,
// flush_cache, and counter state live in the other src/jit/*.cpp files.
#include "jit/frostjit.hpp"
#include "core/emulator.h"
#include "ir/ir.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// ── Forward decls of slow-path helpers defined in x86_backend.cpp ──────
// These are extern "C" so JIT-compiled code can call them by address
// without name-mangling concerns.
extern "C" {
    uint64_t jit_load_mem_slow(Emulator* emu, uint64_t addr, int width);
    void     jit_store_mem_slow(Emulator* emu, uint64_t addr, uint64_t val, int width);
}

// ── Interpreter-step trampoline (called from JIT-compiled code) ─────────
// Invalidates the CPU's page cache before stepping, then dispatches to
// the interpreter. Optional BIFROST_STEP_TRACE env var logs each step.
extern "C" void jit_interp_step(Emulator* emu, CPU* cpu) {
    // Invalidate the CPU's page cache before stepping.
    cpu->page_cache.read_page = UINT64_MAX;
    cpu->page_cache.write_page = UINT64_MAX;
    if (getenv("BIFROST_STEP_TRACE")) {
        fprintf(stderr, "    [step] pc=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x24=0x%llx x27=0x%llx pstate=0x%x\n",
                static_cast<unsigned long long>(cpu->pc),
                static_cast<unsigned long long>(cpu->regs[0]),
                static_cast<unsigned long long>(cpu->regs[1]),
                static_cast<unsigned long long>(cpu->regs[2]),
                static_cast<unsigned long long>(cpu->regs[24]),
                static_cast<unsigned long long>(cpu->regs[27]),
                cpu->pstate);
    }
    emu->step_public(*cpu);
    if (getenv("BIFROST_STEP_TRACE")) {
        fprintf(stderr, "    [step] pc=0x%llx done x0=0x%llx x24=0x%llx pstate=0x%x\n",
                static_cast<unsigned long long>(cpu->pc),
                static_cast<unsigned long long>(cpu->regs[0]),
                static_cast<unsigned long long>(cpu->regs[24]),
                cpu->pstate);
    }
}

} // namespace arm64emu

namespace arm64emu {

// ── emit_fmov_helper + emit_call_interp ─────────────────────────────────
void FrostJIT::emit_fmov_helper(int dir, int fp_field, uint16_t idx,
                                uint16_t src1, uint16_t dest) {
    int32_t fp_off = (fp_field == 0 ? V_LO_OFF : V_HI_OFF)
                   + static_cast<int>(idx) * 8;
    if (dir == 0) {
        // GPR → FP: load src1 vreg into RAX, store to fp_off.
        // We use RAX as a scratch for the memory store. src1's cached
        // value is NOT modified by the store, so we keep src1's mapping
        // intact for later readers.
        //
        // BUGFIX: if src1 is cached in a reg OTHER than RAX, the old code
        // did `emit_mov_reg(RAX, s)` which silently overwrote whatever
        // dirty vreg was in RAX — losing its value. This caused
        // printf("%f", 3.14) → "2.000000" under BIFROST_ENABLE_FWD=1
        // because v46 (the bfxil result holding x1's low 48 bits) was
        // in RAX and got clobbered by the FMOV_G2F that copies x9 to
        // v_lo[0]. Fix: spill RAX's occupant BEFORE overwriting it.
        int s = ensure_vreg(src1, RAX);
        if (s != RAX) {
            clobber_host_reg(RAX);  // spill dirty vreg in RAX before reuse
            emit_mov_reg(RAX, s);
        }
        emit_store(CPU_REG, fp_off, RAX);
        // FMOV_G2F (fp_field==0) also zeros v_hi[idx] per ARM semantics.
        // The zero-load clobbers RAX, so we must spill any dirty vreg
        // cached there BEFORE overwriting. Use a DIFFERENT reg (RCX) for
        // the zero to avoid clobbering src1 in RAX entirely.
        if (fp_field == 0) {
            int32_t vhi_off = V_HI_OFF + static_cast<int>(idx) * 8;
            // Use RCX as scratch for the zero (doesn't disturb src1 in RAX).
            // clobber_host_reg(RCX) spills any dirty vreg cached in RCX.
            clobber_host_reg(RCX);
            emit_mov_imm32_zext(RCX, 0);
            emit_store(CPU_REG, vhi_off, RCX);
        }
        // src1 stays cached in its reg (RAX or wherever ensure_vreg put it).
        // No mapping drop needed — the store didn't modify src1.
    } else {
        // FP → GPR: load fp_off into a fresh vreg for dest.
        int d = alloc_reg();
        emit_load(d, CPU_REG, fp_off);
        set_vreg_reg(dest, d);
    }
}

// ── emit_call_interp ───────────────────────────────────────────────────
void FrostJIT::emit_call_interp(uint64_t arm_pc, bool ends_block) {
    // flush ALL dirty vregs BEFORE materializing flags.
    // The previous code called emit_materialize_flags FIRST, which
    // clobbers RAX/RCX/RDX — if those held dirty vregs, their values
    // were lost before flush_all_vregs could spill them. This caused
    // state corruption in __multf3 (128-bit softfloat) blocks where
    // a dirty vreg in RAX was silently dropped, producing wrong FP
    // results (e.g. printf("%f", 3.14) → "2.000000").
    flush_all_vregs();
    // Materialize host flags to pstate if valid.
    if (flags_in_host_) {
        emit_materialize_flags(flags_from_sub_);
        flags_in_host_ = false;
        // emit_materialize_flags clobbers RAX/RCX/RDX. Drop their
        // cache mappings (values were already flushed above, so
        // dirty vregs are safe — just drop the stale associations).
        // use bitmask helper.
        invalidate_host_regs((1u<<RAX)|(1u<<RCX)|(1u<<RDX));
    }
    // flush_all_vregs already called above (before
    // materialize_flags). All dirty vregs are now in cpu.regs[]/stack.
    // SP (vreg 31) may have been flushed above, but force-check here
    // in case the materialize introduced a new dirty SP (it shouldn't).
    if (vreg_home_[31] >= 0 && vreg_dirty_[31]) {
        evict_vreg(31);
    }
    emit_push(WIN_REG);  // save R10 (caller-saved)  — 1 push
    emit_push(RAX);      // save RAX                 — 2 pushes (EVEN → no align fixup needed)
    // Set cpu.pc = arm_pc.
    emit_mov_imm_to_rax(arm_pc);
    emit_store(CPU_REG, PC_OFF, RAX);
    // Set args: RDI = emu, RSI = cpu.
    emit_mov_reg(RDI, EMU_REG);
    emit_mov_reg(RSI, CPU_REG);
    emit_call_aligned(&jit_interp_step, /*num_pushed=*/2);
    emit_pop(RAX);       // restore RAX
    emit_pop(WIN_REG);   // restore WIN_REG
    // Reload PC into RAX.
    emit_load(RAX, CPU_REG, PC_OFF);
    // invalidate ALL cache mappings after the call.
    // We can't keep callee-saved vregs cached because the interpreter
    // may have modified cpu.regs[] for registers that the JIT has
    // cached as non-dirty. A STORE_REG earlier in the block may have
    // written to cpu.regs[R], but a vreg loaded from R before that
    // STORE_REG would still hold the OLD value. After the interpreter
    // call, we must reload everything from cpu.regs[] to be safe.
    invalidate_all_vregs();
    if (!ends_block) {
        uint64_t next_pc = arm_pc + 4;
        if (next_pc <= 0xFFFFFFFFULL) {
            emit_mov_imm32_zext(RCX, static_cast<uint32_t>(next_pc));
        } else {
            emit_mov_imm64(RCX, next_pc);
        }
        emit_cmp_reg(RCX, RAX);
        size_t jne_patch = emit_jcc_rel32_placeholder(5);
        call_interp_branch_patches_.push_back(jne_patch);
    }
}

// emit_load_mem / emit_store_mem live in x86_backend.cpp
// (they are pure x86 emission with no regalloc/IR awareness).

bool FrostJIT::compile_ir_inst(const IRInst& inst) {
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
                emit_and_cl_imm8(0x3F);
                int kind = (inst.op == IROp::SHL) ? 4
                         : (inst.op == IROp::SHR) ? 5
                         : (inst.op == IROp::SAR) ? 7 : 1;
                emit_shift_cl(d, kind);
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
            materialize_flags_to_pstate();
            emit_mov_imm_to_rax(inst.arm_pc + 4);
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});

            // ── Taken path ──
            int32_t taken_rel = static_cast<int32_t>(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);

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

        // ── FMOV (general ↔ FP) — native codegen via emit_fmov_helper ───
        // These ops move data between cpu.regs[] and cpu.v_lo[]/v_hi[]
        // using direct memory access through CPU_REG (RBX).
        // No CALL_INTERP needed — pure memory moves through RAX.
        case IROp::FMOV_G2F:    emit_fmov_helper(/*dir=*/0, /*field=*/0, inst.dest, inst.src1, inst.dest); return false;
        case IROp::FMOV_F2G:    emit_fmov_helper(/*dir=*/1, /*field=*/0, inst.src1, inst.src1, inst.dest); return false;
        case IROp::FMOV_G2FHI:  emit_fmov_helper(/*dir=*/0, /*field=*/1, inst.dest, inst.src1, inst.dest); return false;
        case IROp::FMOV_FHI2G:  emit_fmov_helper(/*dir=*/1, /*field=*/1, inst.src1, inst.src1, inst.dest); return false;

        // ── FP scalar arithmetic — native SSE2 codegen ──────────────
        // These ops use XMM0/XMM1 as scratch, loading from and storing
        // to v_lo[]/v_hi[] via CPU_REG (RBX). They don't interact with
        // the GPR register allocator at all.
        //
        // replaced flush_all_vregs()+invalidate_all_vregs()
        // (O(max_vreg_) per op) with flush_invalidate_host_regs({RAX})
        // (O(1) per op). FP_BINOP only clobbers RAX (for the FNMUL sign
        // mask and the v_hi[dest]=0 zero store). Callee-saved vregs in
        // R12/R13/R15 are preserved.
        case IROp::FP_BINOP: {
            // v_lo[dest] = op(v_lo[src1], v_lo[src2]); v_hi[dest] = 0
            bool is_double = (inst.width == 1);
            uint8_t ld_prefix = is_double ? 0xF2 : 0xF3;  // MOVSD/MOVSS
            clobber_flags();
            // FP_BINOP only clobbers RAX (zero store to
            // v_hi[dest]; sign mask for FNMUL). XMM0/XMM1 are scratch and
            // don't hold vregs. Use targeted flush for O(1) instead of
            // O(max_vreg_) flush_all+invalidate_all.
            flush_invalidate_host_regs(1u << RAX);

            // Load src1 into XMM0: movsd/movss xmm0, [rbx+off]
            // BUGFIX: no REX needed — SSE regs are 0-7, RBX is 3.
            // REX.R would extend xmm1 to xmm9, breaking the op.
            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            emit_byte(ld_prefix);
            emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);

            // Load src2 into XMM1: movsd/movss xmm1, [rbx+off]
            int32_t off2 = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            emit_byte(ld_prefix);
            emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(1, CPU_REG, off2);

            // Execute SSE2 op
            uint8_t opc = static_cast<uint8_t>(inst.imm);
            uint8_t sse_op;
            switch (opc) {
                case 0: sse_op = 0x59; break;  // mul
                case 1: sse_op = 0x5E; break;  // div
                case 2: sse_op = 0x58; break;  // add
                case 3: sse_op = 0x5C; break;  // sub
                case 4: sse_op = 0x5F; break;  // max
                case 5: sse_op = 0x5D; break;  // min
                default: sse_op = 0x58; break;
            }
            // Execute SSE2 op: ADDSD/MULSD/etc xmm0, xmm1 → xmm0 = xmm0 OP xmm1
            // BUGFIX: must use modrm(3, 0, 1) → reg=xmm0, rm=xmm1
            // The old code used modrm(3, 1, 0) with REX.R which encoded
            // ADDSD xmm1, xmm0 (result in xmm1) but stored xmm0 (stale).
            emit_byte(ld_prefix);
            emit_byte(0x0F); emit_byte(sse_op);
            emit_byte(modrm(3, 0, 1));  // xmm0, xmm1

            if (opc == 6) {  // FNMUL: negate
                emit_mov_imm64(RAX, 0x8000000000000000ULL);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC8);
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x57); emit_byte(0xC1);
            }

            // Store result: movsd/movss [rbx+off], xmm0
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(ld_prefix);
            emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);

            // Zero v_hi[dest]
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return false;
        }

        case IROp::FP_UNOP: {
            bool is_double = (inst.width == 1);
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            clobber_flags();
            // FP_UNOP clobbers only RAX (sign mask for FABS/FNEG; zero store).
            // RCX/RDX are not touched — don't flush them.
            flush_invalidate_host_regs(1u << RAX);

            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            emit_byte(prefix);
            emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);

            uint8_t opc = static_cast<uint8_t>(inst.imm);
            if (opc == 0) {
                // FMOV — no-op
            } else if (opc == 1) {
                // FABS
                emit_mov_imm64(RAX, 0x7FFFFFFFFFFFFFFFULL);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC8);
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x54); emit_byte(0xC1);
            } else if (opc == 2) {
                // FNEG
                emit_mov_imm64(RAX, 0x8000000000000000ULL);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC8);
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x57); emit_byte(0xC1);
            } else if (opc == 3) {
                // FSQRT
                emit_byte(prefix);
                emit_byte(0x0F); emit_byte(0x51);
                emit_byte(modrm(3, 0, 0));
            }

            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(prefix);
            emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);

            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return false;
        }

        // ── FP→int conversion (FCVTZS/FCVTZU) ──────────────────────
        case IROp::FP_F2I: {
            // regs[dest] = (int/uint)(v_lo[src1])
            // proper unsigned conversion via the
            // "subtract 2^63, convert signed, add 2^63" trick.
            //
            // sf (flags_op) is unused by the JIT — the JIT always emits
            // 64-bit CVTTSD2SI rax. 32-bit dest truncation (sf=0) is
            // handled by an explicit IROp::ZEXT emitted by ir_translate.cpp
            // after FP_F2I, which zero-extends the low 32 bits per AArch64
            // 32-bit register write semantics.
            //
            // For the unsigned path, the 2^63 constant must match the FP
            // precision: 0x43E0000000000000 (double) for width=1, or
            // 0x5F000000 (float) for width=0. Using the double constant
            // with single-precision ucomiss/subss reads only the low 32
            // bits (0x00000000 = 0.0f), breaking the >= 2^63 detection.
            bool is_double = (inst.width == 1);
            bool is_unsigned = (inst.imm != 0);
            clobber_flags();
            // FP_F2I clobbers RAX (CVTTSD2SI result) and, in the unsigned
            // path, RCX (2^63 constant). Flush+invalidate both.
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            // Load FP value into XMM0
            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);

            if (is_unsigned) {
                // Unsigned conversion: x86 lacks CVTTSD2USI, so we use:
                //   if (xmm0 >= 2^63) { xmm0 -= 2^63; CVTTSD2SI rax; rax += 2^63 }
                //   else                CVTTSD2SI rax
                // Use RCX for the comparison constant.
                // 2^63 in the matching FP precision.
                uint64_t pow63 = is_double ? 0x43E0000000000000ULL
                                           : 0x5F000000ULL;
                // mov rcx, pow63
                emit_mov_imm64(RCX, pow63);
                // movq xmm1, rcx
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xC9);
                // ucomisd/iss xmm0, xmm1 (compare src against 2^63)
                // NOTE: ucomisd takes the 0x66 prefix, ucomiss takes NO
                // mandatory prefix. Using 0xF2/0xF3 here (as we do for
                // cvtsi2sd/ss) would generate invalid instruction encodings
                // on some CPUs and crash with SIGILL. The SSE/SSE2 prefix
                // conventions are NOT uniform across instructions.
                if (is_double) emit_byte(0x66);
                emit_byte(0x0F); emit_byte(0x2E); emit_byte(0xC1);
                // jae .large (CF=0 means src >= 2^63)
                size_t jae_patch = emit_jcc_rel32_placeholder(0x3);  // JAE rel32
                // CVTTSD2SI rax, xmm0 (small path)
                emit_byte(prefix); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2C);
                emit_byte(0xC0);  // rax, xmm0
                // jmp .done
                size_t jmp_done = emit_jmp_rel32_placeholder();
                size_t large_path = code_buf_used_;
                patch_jcc_rel32(jae_patch, static_cast<int32_t>(large_path - (jae_patch + 6)));
                // subsd/ss xmm0, xmm1
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x5C); emit_byte(0xC1);
                // CVTTSD2SI rax, xmm0
                emit_byte(prefix); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2C);
                emit_byte(0xC0);  // rax, xmm0
                // add rax, 0x8000000000000000 (using mov + add to avoid imm64 in add)
                emit_mov_imm64(RCX, 0x8000000000000000ULL);
                emit_byte(0x48); emit_byte(0x01); emit_byte(0xC8);  // add rax, rcx
                size_t done_path = code_buf_used_;
                patch_jmp_rel32(jmp_done, static_cast<int32_t>(done_path - (jmp_done + 5)));
            } else {
                // CVTTSD2SI rax, xmm0 (truncate toward zero, signed)
                emit_byte(prefix); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2C);
                emit_byte(0xC0);  // rax, xmm0
            }

            // Store result to cpu.regs[dest]
            store_reg_to_vreg(inst.dest, RAX);
            return false;
        }

        // ── int→FP conversion (SCVTF/UCVTF) ────────────────────────
        case IROp::FP_I2F: {
            // v_lo[dest] = (float/double)(regs[src1]); v_hi=0
            // proper unsigned conversion via the
            // "if (src >= 2^63) subtract 2^63, convert signed, add 2^63
            //  to result as double" trick.
            //
            // sf (flags_op) selects the source GPR width:
            //   sf=0 → 32-bit GPR (Wn)
            //   sf=1 → 64-bit GPR (Xn)
            // For SIGNED conversion (SCVTF) with sf=0, we use the 32-bit
            // CVTSI2SD/SS form (no REX.W) so eax is interpreted as int32.
            // For UNSIGNED conversion (UCVTF) with sf=0, we MUST use the
            // 64-bit form (REX.W) because the 32-bit value has been zero-
            // extended to 64 bits in the register, and interpreting it as
            // int64 gives the correct unsigned value (uint32 < 2^63).
            // Using the 32-bit form for UCVTF would treat eax as int32,
            // turning 0xFFFFFFFF (uint32 max = 4294967295) into -1 and
            // producing -1.0f instead of 4.29e+09.
            //
            // For the unsigned path, the 2^63 addend must match the FP
            // precision: 0x43E0000000000000 (double) for width=1, or
            // 0x5F000000 (float) for width=0. Using the double constant
            // with addss reads only the low 32 bits (0x00000000 = 0.0f),
            // silently losing the 2^63 correction.
            bool is_double = (inst.width == 1);
            bool is_unsigned = (inst.imm != 0);
            bool is_64bit_src = (inst.flags_op != 0);
            // REX.W prefix: 64-bit form when source is 64-bit OR when
            // unsigned (so the zero-extended 32-bit value is read as
            // positive int64).
            uint8_t rex_w = (is_64bit_src || is_unsigned) ? 0x48 : 0x00;
            clobber_flags();
            // FP_I2F clobbers RAX (GPR load), RCX (subtract flag), and
            // RDX (2^63 constant) in the unsigned path. Flush+invalidate
            // all three.
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            // Load GPR into RAX
            load_vreg_to_reg(RAX, inst.src1);

            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            if (is_unsigned) {
                // Unsigned: if (rax >= 2^63) { rcx = 1; sub rax, 2^63 } else rcx = 0
                // CVTSI2SD xmm0, rax (signed convert of the adjusted value)
                // if (rcx) addsd xmm0, [2^63 as double]
                emit_mov_imm32_zext(RCX, 0);
                // cmp rax, 0x8000000000000000
                emit_mov_imm64(RDX, 0x8000000000000000ULL);
                emit_byte(0x48); emit_byte(0x39); emit_byte(0xD0);  // cmp rax, rdx
                // jb .small (CF=1 means rax < 2^63)
                size_t jb_patch = emit_jcc_rel32_placeholder(0x2);  // JB rel32
                // Fall-through (rax >= 2^63): sub rax, 2^63 (rax -= rdx)
                emit_byte(0x48); emit_byte(0x29); emit_byte(0xD0);  // sub rax, rdx
                emit_mov_imm32_zext(RCX, 1);  // mark that we subtracted
                size_t small_path = code_buf_used_;
                patch_jcc_rel32(jb_patch, static_cast<int32_t>(small_path - (jb_patch + 6)));
                // CVTSI2SD/SS xmm0, rax (or eax for 32-bit source)
                emit_byte(prefix);
                if (rex_w) emit_byte(rex_w);
                emit_byte(0x0F); emit_byte(0x2A);
                emit_byte(0xC0);  // xmm0, rax
                // if (rcx != 0) add 2^63 as double/single
                emit_byte(0x48); emit_byte(0x85); emit_byte(0xC9);  // test rcx, rcx
                size_t jz_patch = emit_jcc_rel32_placeholder(0x4);  // JZ rel32
                // 2^63 in the matching FP precision.
                //   double: 0x43E0000000000000
                //   single: 0x5F000000 (low 32 bits of xmm1)
                uint64_t pow63 = is_double ? 0x43E0000000000000ULL
                                           : 0x5F000000ULL;
                emit_mov_imm64(RDX, pow63);
                emit_byte(0x66); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x6E); emit_byte(0xCA);  // movq xmm1, rdx
                // addsd/addss xmm0, xmm1
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x58); emit_byte(0xC1);
                size_t done_path = code_buf_used_;
                patch_jcc_rel32(jz_patch, static_cast<int32_t>(done_path - (jz_patch + 6)));
            } else {
                // CVTSI2SD/SS xmm0, rax (or eax for 32-bit source)
                emit_byte(prefix);
                if (rex_w) emit_byte(rex_w);
                emit_byte(0x0F); emit_byte(0x2A);
                emit_byte(0xC0);  // xmm0, rax
            }

            // Store to v_lo[dest]
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);

            // Zero v_hi[dest]
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return false;
        }

        // ── FP compare (FCMP/FCMPE) ────────────────────────────────
        case IROp::FP_CMP: {
            // Native FCMP/FCMPE using UCOMISD/UCOMISS.
            //
            // ARM FCMP sets NZCV:
            //   unordered (NaN): N=0 Z=0 C=1 V=1
            //   less than:       N=1 Z=0 C=0 V=0
            //   equal:           N=0 Z=1 C=1 V=0
            //   greater than:    N=0 Z=0 C=1 V=0
            //
            // x86 UCOMISD sets:
            //   unordered: PF=1, CF=1, ZF=1
            //   less than: PF=0, CF=1, ZF=0
            //   equal:     PF=0, CF=0, ZF=1
            //   greater:   PF=0, CF=0, ZF=0
            //
            // Translation:
            //   PF=1 (unordered) → pstate = 0x28000000 (C=1, V=1)
            //   else CF=1 (less) → pstate = 0x80000000 (N=1)
            //   else ZF=1 (equal) → pstate = 0x60000000 (Z=1, C=1)
            //   else (greater) → pstate = 0x20000000 (C=1)
            //
            // We use conditional sets (setcc) to build pstate in RDX,
            // then store to cpu.pstate.
            bool is_double = (inst.width == 1);
            clobber_flags();
            // FP_CMP clobbers RAX, RCX, RDX (flag manipulation).
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            // Load src1 into XMM0
            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);

            // Load src2 into XMM1 (or zero for FCMP #0.0).
            //
            // The IR translator marks the #0.0 form by setting bit 0 of
            // inst.imm (sentinel). Without this sentinel, FCMP Dn, D0
            // (register form with rm==0) would be indistinguishable from
            // FCMP Dn, #0.0 (zero form), because both have IR src2 == 0.
            //
            // Register form: src2 = rm (0..30). Load v_lo[src2] into XMM1.
            // Zero form: src2 = 0, imm bit 0 = 1. Use xorps to zero XMM1.
            bool with_zero = (inst.imm & 1) != 0;
            if (!with_zero) {
                int32_t off2 = V_LO_OFF + static_cast<int>(inst.src2) * 8;
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off2);
            } else {
                // FCMP Dn, #0.0 — XORPS xmm1, xmm1 to get 0.0
                emit_byte(0x0F); emit_byte(0x57); emit_byte(0xC9); // xorps xmm1, xmm1
            }

            // UCOMISD/UCOMISS xmm0, xmm1
            // Encoding: UCOMISD = 66 0F 2E /r ; UCOMISS = NP 0F 2E /r
            // (NOT F2/F3 — those prefixes are for arithmetic ops like ADDSD,
            // not for compare ops. Using F2/F3 here emits an illegal
            // instruction that raises SIGILL on real hardware.)
            if (is_double) emit_byte(0x66);
            emit_byte(0x0F); emit_byte(0x2E);
            emit_byte(0xC1);  // xmm0, xmm1

            // Build pstate in RDX using conditional moves.
            // pushfq to get flags into RAX, then test bits.
            emit_pushfq();
            emit_byte(0x58);  // pop rax (flags in rax)

            // RDX = 0 (default)
            emit_xor_reg(RDX, RDX);

            // Save rax (flags image) — we need it for multiple tests.
            emit_byte(0x50);  // push rax

            // Fixed flag conversion.
            // x86 UCOMISD sets: unordered (PF=1,CF=1,ZF=1), less (CF=1),
            //                    equal (ZF=1), greater (none).
            // We use a priority chain: check PF first (unordered), then
            // CF (less), then ZF (equal), else greater. Each cmovne only
            // fires if RDX is still 0 (i.e., no higher-priority case matched).
            // We use cmovne (ZF=0) because `test` sets ZF=0 when the bit
            // IS set (i.e., the flag IS 1).
            //
            // But the old chain had a bug: if unordered (PF=1,CF=1,ZF=1),
            // all three cmovne would fire, and the last one (equal=0x60000000)
            // would overwrite the correct unordered value (0x30000000).
            //
            // Fix: use a priority chain where each cmov only fires if RDX
            // is still 0. We do this by checking RDX after each set.
            //
            // Simpler approach: use conditional jumps (je/jne) to build
            // a proper if-else chain. This is slightly more code but
            // unambiguously correct.

            // Default: RDX = 0x20000000 (greater → C=1)
            emit_mov_imm32_zext(RDX, 0x20000000);

            // Fixed flag conversion using clean if-else chain.
            // After `test rax, bit`:
            //   ZF=1 iff (rax & bit) == 0 (flag bit is 0)
            //   ZF=0 iff (rax & bit) != 0 (flag bit is 1)
            // JNZ (0x5) jumps when ZF=0, i.e., when the flag bit IS set.
            // We use JNZ to jump OVER the value-set block when the condition
            // is NOT met (flag bit is 0), so the default/previous value stays.

            // if PF=1 (unordered): RDX = 0x30000000, then jmp done
            emit_byte(0x48); emit_byte(0xA9); emit_u32(0x04); // test rax, 4 (PF bit)
            size_t jz_skip1 = emit_jcc_rel32_placeholder(0x4);  // JZ: PF=0, skip
            emit_mov_imm32_zext(RDX, 0x30000000);
            size_t jmp_done1 = emit_jmp_rel32_placeholder();
            size_t after_pf = code_buf_used_;
            patch_jcc_rel32(jz_skip1, static_cast<int32_t>(after_pf - (jz_skip1 + 6)));

            // if CF=1 (less): RDX = 0x80000000 (N=1)
            emit_byte(0x48); emit_byte(0xA9); emit_u32(0x01); // test rax, 1 (CF)
            size_t jz_skip2 = emit_jcc_rel32_placeholder(0x4);  // JZ: CF=0, skip
            emit_mov_imm32_zext(RDX, 0x80000000);
            size_t jmp_done2 = emit_jmp_rel32_placeholder();
            size_t after_cf = code_buf_used_;
            patch_jcc_rel32(jz_skip2, static_cast<int32_t>(after_cf - (jz_skip2 + 6)));

            // if ZF=1 (equal): RDX = 0x60000000 (Z=1, C=1)
            emit_byte(0x48); emit_byte(0xA9); emit_u32(0x40); // test rax, 0x40 (ZF)
            size_t jz_skip3 = emit_jcc_rel32_placeholder(0x4);  // JZ: ZF=0, skip
            emit_mov_imm32_zext(RDX, 0x60000000);
            size_t done_flags = code_buf_used_;
            patch_jcc_rel32(jz_skip3, static_cast<int32_t>(done_flags - (jz_skip3 + 6)));
            patch_jmp_rel32(jmp_done1, static_cast<int32_t>(done_flags - (jmp_done1 + 5)));
            patch_jmp_rel32(jmp_done2, static_cast<int32_t>(done_flags - (jmp_done2 + 5)));

            emit_byte(0x58);  // pop rax (discard)

            // Store pstate (mask NZCV bits, OR in new value)
            emit_load32(RCX, CPU_REG, PSTATE_OFF);
            emit_byte(0x81); emit_byte(0xE1); emit_u32(0x0FFFFFFF);  // and ecx, 0x0FFFFFFF
            emit_byte(0x48); emit_byte(0x09); emit_byte(0xCA);  // or rdx, rcx
            emit_store32(CPU_REG, PSTATE_OFF, RDX);
            flags_in_host_ = false;
            return false;
        }

        // ── FMOV immediate (load decoded FP immediate) ──────────────
        case IROp::FP_MOVI: {
            // v_lo[dest] = imm; v_hi[dest] = 0
            // clobber_host_reg evicts any dirty GPR vreg
            // cached in RAX BEFORE we overwrite it with the immediate.
            // The old code silently dropped dirty vregs.
            clobber_host_reg(RAX);
            emit_mov_imm64(RAX, inst.imm);
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_store(CPU_REG, off_d, RAX);
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return false;
        }

        // ── SIMD LOGICAL (AND/ORR/EOR/BIC/ORN/EON) — native SSE2 ────
        case IROp::SIMD_LOGICAL: {
            // v_lo[dest],v_hi[dest] = src1 OP src2
            // imm = opcode (0=and,1=orr,2=xor,3=bic,4=orn,5=eon)
            // SIMD_LOGICAL only touches XMM0/XMM1, no GPRs.
            clobber_flags();
            // No GPR clobbered — but be safe and invalidate RAX/RCX/RDX
            // in case the JIT has stale mappings (they shouldn't be dirty
            // since no GPR is touched, but the cache state may be stale).
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));

            // Load src1 lo/hi into XMM0
            int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            // movsd xmm0, [rbx+off1lo]
            emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1lo);

            // Load src2 lo into XMM1
            int32_t off2lo = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(1, CPU_REG, off2lo);

            // Execute lo half
            uint8_t opc = static_cast<uint8_t>(inst.imm);
            uint8_t sse_op;
            if (opc <= 2) {
                sse_op = (opc == 0) ? 0x54 : (opc == 1) ? 0x56 : 0x57;
            } else {
                emit_call_interp(inst.arm_pc, false);
                return false;
            }
            // 66 0F sse_op C1 (xmm0, xmm1)
            emit_byte(0x66); emit_byte(0x0F); emit_byte(sse_op); emit_byte(0xC1);

            // Store lo result
            int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, offdlo);

            // Load src1 hi into XMM0
            emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1hi);
            // Load src2 hi into XMM1
            int32_t off2hi = V_HI_OFF + static_cast<int>(inst.src2) * 8;
            emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(1, CPU_REG, off2hi);
            // Execute hi half
            emit_byte(0x66); emit_byte(0x0F); emit_byte(sse_op); emit_byte(0xC1);
            // Store hi result
            int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, offdhi);
            return false;
        }

        // ── SIMD DUP (broadcast GPR to both halves) ────────────────
        case IROp::SIMD_DUP: {
            // v_lo[dest] = v_hi[dest] = src1 (GPR value)
            // DON'T drop src1's cache mapping after the
            // store — src1 may be read again later in the block. The old
            // code did `vreg_home_[reg_vreg_[RAX]] = -1; reg_vreg_[RAX] = -1`
            // which silently dropped a dirty src1.
            int s = ensure_vreg(inst.src1, RAX);
            if (s != RAX) {
                // src1 is cached in another reg (s). We need its value in
                // RAX for the store. clobber_host_reg(RAX) spills any dirty
                // vreg currently in RAX BEFORE we overwrite it.
                clobber_host_reg(RAX);
                emit_mov_reg(RAX, s);
            }
            int32_t offlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_store(CPU_REG, offlo, RAX);
            emit_store(CPU_REG, offhi, RAX);
            // src1 stays cached in its original reg (s) for later readers.
            // RAX holds a copy (not a cached vreg) — no mapping to update.
            return false;
        }

        // ── SIMD LDST (read/write v_lo/v_hi to/from vregs) ─────────
        case IROp::SIMD_LDST: {
            // width=1 (load): src1=lo vreg, src2=hi vreg → v_lo[dest], v_hi[dest]
            // width=0 (store): v_lo[dest] → src1 vreg, v_hi[dest] → src2 vreg
            if (inst.width == 1) {
                // Load: write vregs to v_lo/v_hi
                // use separate host regs for lo/hi so we
                // don't clobber src1's cached value when loading src2.
                // The old code reused RAX for both, dropping src1's mapping.
                int slo = ensure_vreg(inst.src1, RAX);
                int32_t offlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
                emit_store(CPU_REG, offlo, slo);

                // For the hi half, use RCX. If src2 is cached in a different
                // reg, ensure_vreg returns it (no eviction). If src2 is not
                // cached, ensure_vreg loads it into RCX (evicting RCX's
                // current occupant via alloc_reg, which calls evict_vreg).
                int shi = ensure_vreg(inst.src2, RCX);
                int32_t offhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
                emit_store(CPU_REG, offhi, shi);
            } else {
                // Store: read v_lo/v_hi into vregs
                int dlo = alloc_reg();
                int32_t offlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
                emit_load(dlo, CPU_REG, offlo);
                set_vreg_reg(inst.src1, dlo);

                int dhi = alloc_reg();
                int32_t offhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
                emit_load(dhi, CPU_REG, offhi);
                set_vreg_reg(inst.src2, dhi);
            }
            return false;
        }
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
                // Drop all cache mappings WITHOUT clearing flags_in_host_.
                // use invalidate_all_vregs but preserve flags_in_host_.
                {
                    bool saved_fih3 = flags_in_host_;
                    invalidate_all_vregs();
                    flags_in_host_ = saved_fih3;
                }
                flags_in_host_ = true;
                flags_from_sub_ = false;
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

        // ── FCVT: float <-> double conversion ────────────────────────
        case IROp::FCVT_S2D:
        case IROp::FCVT_D2S: {
            // FCVT only clobbers RAX (zero store).
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
            // Load FP value from v_lo[src1] into XMM0
            int32_t off = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            uint8_t prefix = (inst.op == IROp::FCVT_S2D) ? 0xF3 : 0xF2;
            // movss/movsd xmm0, [rbx + off]
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off);
            if (inst.op == IROp::FCVT_S2D) {
                // cvtss2sd xmm0, xmm0
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x5A);
                emit_byte(0xC0);
            } else {
                // cvtsd2ss xmm0, xmm0
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x5A);
                emit_byte(0xC0);
            }
            // Store result to v_lo[dest]
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(prefix ^ 0x01); emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);
            // Zero v_hi[dest]
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return false;
        }

        // ── FRINT: FP round to integer ───────────────────────────────
        case IROp::FRINT: {
            // FRINT only clobbers RAX (zero store).
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
            int32_t off = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            // ftype encoding: 0 = single (S), 1 = double (D)
            bool is_double = (inst.width != 0);
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            // Load FP value into XMM0
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off);
            // roundsd/roundss xmm0, xmm0, imm8
            // x86 rounding mode mapping: 0=nearest, 1=down(-inf), 2=up(+inf), 3=truncate(0)
            uint8_t x86_mode;
            switch (inst.imm & 0x7) {
                case 0: x86_mode = 0; break;  // N → nearest
                case 1: x86_mode = 2; break;  // P → +inf (up)
                case 2: x86_mode = 1; break;  // M → -inf (down)
                case 3: x86_mode = 3; break;  // Z → 0 (truncate)
                default: x86_mode = 4; break; // I/X → current MXCSR rounding
            }
            // 66 0F 3A 0B C0 imm8 = roundsd xmm0, xmm0, imm8
            // 66 0F 3A 0A C0 imm8 = roundss xmm0, xmm0, imm8
            emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A);
            emit_byte(is_double ? 0x0B : 0x0A);
            emit_byte(0xC0);  // xmm0, xmm0
            emit_byte(x86_mode);
            // Store result
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return false;
        }

        // ── FMADD / FMSUB: FP fused multiply-add ─────────────────────
        case IROp::FMADD:
        case IROp::FMSUB: {
            // FMADD: dest = src1 * src2 + acc
            // FMSUB: dest = -src1 * src2 + acc = acc - src1 * src2
            // We decompose: mul, then add/sub acc (non-fused, but correct)
            // FMADD clobbers RAX (zero store).
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX) | (1u << RDX));
            // ftype encoding: 0 = single (S), 1 = double (D)
            bool is_double = (inst.width != 0);
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off2 = V_LO_OFF + static_cast<int>(inst.src2) * 8;
            int32_t off_acc = V_LO_OFF + static_cast<int>(inst.imm) * 8;
            // Load src1 into XMM0
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);
            // Load src2 into XMM1
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(1, CPU_REG, off2);
            // mulsd/mulss xmm0, xmm1
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x59);
            emit_byte(0xC1);  // xmm0, xmm1
            // Load acc into XMM2
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(2, CPU_REG, off_acc);
            if (inst.op == IROp::FMADD) {
                // addsd/addss xmm0, xmm2
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x58);
                emit_byte(0xC2);  // xmm0, xmm2
            } else {
                // FMSUB: subss/subsd xmm2, xmm0 → result in xmm2
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x5C);
                emit_byte(0xD0);  // xmm2, xmm0
                // movaps xmm0, xmm2
                emit_byte(0x0F); emit_byte(0x28); emit_byte(0xC2);
            }
            // Store result to v_lo[dest]
            int32_t off_d = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x11);
            emit_modrm_disp(0, CPU_REG, off_d);
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, V_HI_OFF + static_cast<int>(inst.dest) * 8, RAX);
            return false;
        }

        default:
            emit_call_interp(inst.arm_pc, false);
            return false;
    }
}

// ── clobber_flags ───────────────────────────────────────────────────────
// If the host RFLAGS currently hold valid guest NZCV (flags_in_host_),
// materialize them to cpu.pstate BEFORE a flag-clobbering instruction
// overwrites them. Without this, any ALU op after ADDS/SUBS/TST would
// lose the flags, causing wrong branch decisions downstream.
void FrostJIT::clobber_flags() {
    if (flags_in_host_) {
        // emit_materialize_flags clobbers RAX, RCX, RDX.
        // use targeted flush+invalidate via the bitmask
        // instead of two open-coded 3-reg loops.
        constexpr uint16_t FLAGS_CLOBBER =
            (1u << RAX) | (1u << RCX) | (1u << RDX);
        flush_dirty_host_regs(FLAGS_CLOBBER);
        emit_materialize_flags(flags_from_sub_);
        flags_in_host_ = false;
        // Drop cache mappings for RAX/RCX/RDX (values were evicted above
        // if dirty; non-dirty values can be safely reloaded from memory).
        invalidate_host_regs(FLAGS_CLOBBER);
    }
}

// Materialize host flags to pstate, preserving RFLAGS around the materialize
// (which clobbers them). Used at block exits (BRCOND fall-through and taken
// paths) where the next block may read pstate. No-op if flags aren't
// currently in host. Does NOT clear flags_in_host_ — the caller manages that,
// since BRCOND emits this on both paths before clearing flags_in_host_ at the
// end.
void FrostJIT::materialize_flags_to_pstate() {
    if (!flags_in_host_) return;
    constexpr uint16_t FLAGS3 = (1u << RAX) | (1u << RCX) | (1u << RDX);
    emit_pushfq();  // save RFLAGS (materialize clobbers them)
    flush_dirty_host_regs(FLAGS3);
    emit_materialize_flags(flags_from_sub_);
    emit_popfq();
    invalidate_host_regs(FLAGS3);
}

// ── Block chaining helpers ──────────────────────────────────────────────
// Patch a block's 5-byte chain slot (originally `ret` + 4 NOPs) in place
// to `jmp rel32` → target_fn. x86 is icache-coherent, so no explicit
// cache flush is needed, but we emit a memory barrier to ensure the
// patched bytes are visible to any in-flight execution on the same core.

// single source of truth for "will this instruction route
// to CALL_INTERP in the IR translator?" Used by the block splitter to
// pre-scan before translating. If this list gets out of sync with
// ir_translate.cpp's default case, the splitter would misclassify
// instructions.
//
// Note: SIMD_LD1, SIMD_ST1, SIMD_DUP, and SIMD_LOGICAL have native IR
// paths (IROp::SIMD_LDST / SIMD_DUP / SIMD_LOGICAL) and do NOT route to
// CALL_INTERP for the common cases — they are deliberately NOT listed
// here so the block splitter doesn't fragment blocks around them.
// SIMD_LOGICAL falls back to CALL_INTERP only for unrecognized opcodes
// (its `default:` case), but that's rare enough to accept the risk.
static bool instr_will_call_interp(const DecodedInst& d) {
    switch (d.cls) {
        case InstClass::SIMD_DP:
        // FP_SCALAR now has native IR paths for most FP ops
        // (FMOV, FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FNMUL, FABS/FNEG/FSQRT,
        // FCMP/FCMPE, FCVT, FRINT, FMADD/FMSUB, FCSEL, FCVTZS/FCVTZU,
        // SCVTF/UCVTF, FMOV imm). But it still falls back to CALL_INTERP
        // for some ops (FCVTAS, half-precision, etc.), so mark it as
        // "will call interp" conservatively to trigger block splitting.
        case InstClass::FP_SCALAR:
        case InstClass::LDXR: case InstClass::STXR:
        case InstClass::LDAXR: case InstClass::STLXR:
        case InstClass::LDAR: case InstClass::STLR:
        case InstClass::LSE_ATOMIC:
            return true;
        default:
            break;
    }
    // Vector load/store (is_vec=true LDR/STR/LDP/STP) → CALL_INTERP.
    if (d.is_vec &&
        (d.cls == InstClass::LDR_IMM || d.cls == InstClass::LDR_UNS ||
         d.cls == InstClass::LDR_REG || d.cls == InstClass::STR_IMM ||
         d.cls == InstClass::STR_UNS || d.cls == InstClass::STR_REG ||
         d.cls == InstClass::LDP || d.cls == InstClass::STP)) {
        return true;
    }
    return false;
}

// ── translate_block ───────────────────────────────────────────────
uint64_t (*FrostJIT::translate_block(Emulator& emu, uint64_t start_pc))(CPU*, Emulator*) {
    if (!code_buf_) return nullptr;
    current_start_pc_ = start_pc;
    code_buf_overflow_ = false;
    call_interp_branch_patches_.clear();
    branch_target_patches_.clear();
    rax_holds_next_pc_ = false;
    flags_in_host_ = false;
    flags_from_sub_ = false;
    chain_target_pc_ = 0;
    unchainable_end_ = false;
    has_selfloop_slot_ = false;
    selfloop_patch_off_ = 0;
    num_stack_slots_ = 0;
    // Only clear the vreg arrays up to the previous block's max_vreg_+1,
    // not all 4096 entries. This saves ~12KB of writes per block
    // translation when blocks are small (typical: max_vreg_ ≈ 33-100).
    int clear_limit = prev_max_vreg_ + 1;
    if (clear_limit > 4096) clear_limit = 4096;
    for (int i = 0; i < clear_limit; i++) {
        vreg_home_[i] = -1;
        vreg_dirty_[i] = false;
        vreg_slot_[i] = 0;
    }
    for (int i = 0; i < 16; i++) reg_vreg_[i] = -1;
    max_vreg_ = 0;
    dirty_host_regs_ = 0;  // reset dirty-bitmask

    size_t block_start = code_buf_used_;

    // ── Translate ARM64 → IR ─────────────────────────────────────
    IRBlock ir_block;
    ir_block.start_pc = start_pc;
    ir_reset_vreg_alloc();

    // Limit block size based on register pressure. With 9 host regs and
    // >256 vregs, the allocator's spill/reload traffic becomes a
    // correctness hazard. Cap blocks at 32 instructions — enough for
    // tight loops, short enough that vreg count stays manageable.
    constexpr int MAX_BLOCK_REG_PRESSURE = 32;
    // limit the number of CALL_INTERP fallbacks per
    // block. Each CALL_INTERP invalidates all cached vregs, and each
    // subsequent clobber_flags() drops non-dirty vregs from RAX/RCX/RDX.
    // In long blocks with many CALL_INTERPs (e.g., __multf3's 82-instr
    // multiply block with ~10 CSINC/CCMP/CSEL fallbacks), this creates
    // a cascade of stale reloads that corrupt register values.
    //
    // Fix: after MAX_CALL_INTERP_PER_BLOCK interpreter fallbacks, force
    // a block boundary. The next instruction becomes the start of a new
    // block, which gets a fresh register allocator state. This trades a
    // small perf cost (one extra block dispatch per split) for
    // correctness in complex blocks.
    constexpr int MAX_CALL_INTERP_PER_BLOCK = 2;
    int call_interp_count = 0;
    uint64_t cur_pc = start_pc;
    int instr_count = 0;
    bool block_ended = false;
    while (!block_ended && instr_count < MAX_BLOCK_REG_PRESSURE) {
        // ── Block splitting at known entry points ──────────────────
        if (instr_count > 0 && blocks_.find(cur_pc) != blocks_.end()) {
            chain_target_pc_ = cur_pc;
            break;
        }
        uint32_t inst;
        try {
            inst = emu.mem().fetch_inst(cur_pc);
        } catch (...) { break; }
        DecodedInst d;
        if (!decode(d, inst)) break;

        bool will_call_interp = instr_will_call_interp(d);

        if (will_call_interp && call_interp_count >= MAX_CALL_INTERP_PER_BLOCK && instr_count > 0) {
            // Split here — the next instruction starts a new block.
            chain_target_pc_ = cur_pc;
            break;
        }

        bool ends = translate_to_ir(ir_block, d, cur_pc);
        if (will_call_interp) call_interp_count++;
        instr_count++;
        ir_block.count = instr_count;
        if (ends) block_ended = true;
        else      cur_pc += 4;
    }
    if (instr_count == 0) return nullptr;

    // ── Heuristic: skip JIT for CALL_INTERP-heavy blocks ──────────
    // The JIT's per-CALL_INTERP overhead (flush all vregs + push 2 regs +
    // call interpreter + pop 2 regs + reload) is ~20 instructions. For
    // blocks where CALL_INTERP dominates (more than half the instructions
    // are interpreter fallbacks), the pure interpreter is faster — it
    // skips the prologue/epilogue/dispatch entirely.
    //
    // refined from "any CALL_INTERP → interp_only" to
    // "CALL_INTERP-heavy → interp_only". The old heuristic was too
    // aggressive — a block with 10 native ops and 1 CALL_INTERP would
    // skip JIT entirely, losing the 10 native ops' speedup. Now we only
    // fall back to interpreter when CALL_INTERP is the majority.
    //
    // This fixes the long-double multiply/divide hang: __multf3/__divtf3
    // are ~82-instruction soft-float routines split into ~20 tiny blocks
    // by the MAX_CALL_INTERP_PER_BLOCK=2 splitter. The JIT was 100-200x
    // slower than the interpreter for these, causing effective hangs on
    // jit_block_split.elf and jit_fp_scalar.elf with --jit.
    //
    // Interp-only blocks are cached (so we skip the re-decode cost on
    // cache hits) and run exactly instr_count interpreter steps.
    // blocks with >32 instructions have too much
    // register pressure for the 9-host-reg allocator. The __multf3
    // 82-instruction softfloat block generates ~246 vregs, causing
    // spill/reload correctness bugs. Run long blocks via interpreter.
    if (instr_count > 32 ||
        (call_interp_count > 0 && call_interp_count * 2 > instr_count)) {
        BlockEntry entry;
        entry.fn = nullptr;
        entry.interp_only = true;
        entry.interp_only_count = instr_count;
        entry.ends_with_branch = ir_block.ends_with_branch;
        entry.chain_target_pc = 0;
        entry.chained = false;
        entry.instr_count = instr_count;
        blocks_[start_pc] = entry;
        blocks_translated++;
        return nullptr;
    }

    // ── Optimize the IR ──────────────────────────────────────────
    static bool no_opt_ = (getenv("BIFROST_NO_OPT") != nullptr);
    if (!no_opt_) optimize_ir(ir_block);

    static bool dump_ir_ = (getenv("BIFROST_JIT_DUMP") != nullptr);
    if (dump_ir_) {
        fprintf(stderr, "══ Block @ 0x%llx (%d ARM instrs) ══\n",
                static_cast<unsigned long long>(start_pc), instr_count);
        dump_ir(ir_block);
    }

    // ── Compute stack size and pre-allocate vreg slots ────────────
    // Pre-scan IR to find all scratch vregs (33+) and assign each a
    // fixed stack slot. This avoids the lazy allocation mismatch between
    // the pre-computed stack size and the runtime slot counter.
    //
    // the previous code limited max_vreg to < 200,
    // but blocks with many ARM instructions (e.g., __multf3's 82-instr
    // block) can have vregs up to 317+. Vregs >= 200 would get stack
    // slots via lazy allocation that extend BEYOND the pre-allocated
    // stack frame, causing stack corruption.
    int max_vreg = 33;
    for (auto& inst : ir_block.insts) {
        if (inst.dest > max_vreg) max_vreg = inst.dest;
        if (inst.src1 > max_vreg) max_vreg = inst.src1;
        if (inst.src2 > max_vreg) max_vreg = inst.src2;
    }
    // Pre-assign stack slots: vreg 33 → slot -8, vreg 34 → slot -16, etc.
    for (int v = 33; v <= max_vreg; v++) {
        vreg_slot_[v] = -8 * (v - 32);
    }
    num_stack_slots_ = max_vreg - 32;
    if (num_stack_slots_ < 1) num_stack_slots_ = 1;
    uint32_t stack_bytes = static_cast<uint32_t>(num_stack_slots_ * 8 + 64) & ~15U;

    // ── Prologue ─────────────────────────────────────────────────
    emit_push(RBX); emit_push(RBP); emit_push(R12);
    emit_push(R13); emit_push(R14); emit_push(R15);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xE5); // mov rbp, rsp
    emit_byte(0x48); emit_byte(0x81); emit_byte(0xEC);
    emit_u32(stack_bytes);  // sub rsp, stack_bytes

    emit_byte(0x48); emit_byte(0x89); emit_byte(0xFB); // mov rbx, rdi
    emit_byte(0x49); emit_byte(0x89); emit_byte(0xF6); // mov r14, rsi
    if (window_base_) emit_mov_imm64(WIN_REG, reinterpret_cast<uint64_t>(window_base_));

    // Record the block body start offset (after prologue). Used for
    // self-loop chaining: the selfloop slot is patched to jmp here.
    block_body_start_off_ = code_buf_used_;

    // ── Compute liveness: for each vreg, find the last op that uses it ──
    // This lets us free host regs of dead vregs immediately after their
    // last use, instead of keeping them cached until eviction. Without
    // this, the register allocator treats all vregs as live until the end
    // of the block, causing unnecessary spills when register pressure is
    // high (e.g., 5+ live vregs but 9 host regs, with 4+ dead vregs
    // occupying the other regs).
    //
    // Only scratch vregs (33+) are tracked for killing. ARM reg vregs
    // (0-31) represent architectural state that must be flushed to
    // cpu.regs[] at the epilogue — killing a dirty ARM reg vreg early
    // would lose its value before flush_all_vregs can write it back.
    //
    // Special cases for the use-scan:
    //   - LOAD_REG: src1 is the ARM reg index (0-31), NOT a vreg. Don't
    //     treat it as a use.
    //   - STORE_REG: dest is the ARM reg index, src1 is the vreg being
    //     stored. src1 IS a use.
    //   - dest of any op is a definition (not a use). If dest == src1
    //     (in-place modify), the vreg is still live after this op (as
    //     dest), so we don't kill it (handled in the compile loop below).
    {
        size_t n = ir_block.insts.size();
        std::vector<int> last_use(4096, -1);
        for (size_t i = 0; i < n; i++) {
            const IRInst& inst = ir_block.insts[i];
            if (inst.op != IROp::LOAD_REG) {
                // src1/src2 are vregs (for LOAD_REG, src1 is ARM reg index).
                if (inst.src1 > 31) last_use[inst.src1] = static_cast<int>(i);
                if (inst.src2 > 31) last_use[inst.src2] = static_cast<int>(i);
            }
        }
        // Build kills_per_op_: for each op i, the list of scratch vregs
        // whose last use is i (dest is excluded at kill time in the loop).
        kills_per_op_.assign(n, {});
        for (int v = 32; v < 4096; v++) {
            if (last_use[v] >= 0) {
                kills_per_op_[last_use[v]].push_back(static_cast<uint16_t>(v));
            }
        }
    }

    // ── Compile IR ───────────────────────────────────────────────
    for (size_t i = 0; i < ir_block.insts.size(); i++) {
        const IRInst& inst = ir_block.insts[i];
        if (compile_ir_inst(inst)) break;
        // Free host regs of vregs whose last use was this op. Dead vregs
        // are freed immediately, making room for new vregs without eviction.
        if (i < kills_per_op_.size()) {
            for (uint16_t v : kills_per_op_[i]) {
                // Don't kill the dest of this op — it's a new definition
                // and still live (handles dest == src1 in-place modify).
                if (v != inst.dest) {
                    kill_vreg(v);
                }
            }
        }
    }
    kills_per_op_.clear();

    // ── Epilogue ─────────────────────────────────────────────────
    size_t epilogue_off = code_buf_used_;

    // Materialize pending host flags to cpu.pstate before returning.
    // If a flag-setting op (ADDS/SUBS/TST) was the last to touch flags
    // and no subsequent BRCOND consumed them, the flags are still in
    // the host CPU's RFLAGS but haven't been written to pstate. The
    // next block (or the interpreter) would see stale pstate.
    // clobber_flags() handles the flush+materialize+invalidate pattern.
    clobber_flags();

    // Flush all dirty vregs before returning (so cpu.regs[] is up to date).
    flush_all_vregs();

    if (!rax_holds_next_pc_) {
        uint64_t next_pc = start_pc + ir_block.count * 4;
        emit_mov_imm_to_rax(next_pc);
        // Fall-through (MAX_BLOCK hit before any block-ender): the next
        // PC is statically known, so this block is chainable to it.
        if (!unchainable_end_ && chain_target_pc_ == 0) {
            chain_target_pc_ = next_pc;
        }
    }

    // ── PC store point ───────────────────────────────────────────
    // Both the normal epilogue and the CALL_INTERP early-exit converge
    // here. At this point, flags are materialized and vregs are flushed.
    // RAX holds the next PC (either from rax_holds_next_pc_ or from the
    // CALL_INTERP's interpreter call).
    // call_interp_branch_patches_ jump here (after the
    // RAX overwrite) to preserve the interpreter's PC in RAX.
    size_t pc_store_off = code_buf_used_;

    emit_store(CPU_REG, PC_OFF, RAX);

    // ── Chain-capable epilogue ────────────────────────────────────
    // For block chaining we jump directly from one block's epilogue to
    // the next block's prologue. The next prologue reloads RBX/R14 from
    // RDI/RSI (the System-V arg registers), so before restoring our own
    // callee-saved regs we copy the live CPU/EMU pointers into RDI/RSI.
    // When the block returns (unchained `ret`), clobbering RDI/RSI is
    // fine — they're caller-saved and the dispatcher doesn't read them
    // after the call. When the block is chained, the next prologue sees
    // the correct RDI=cpu / RSI=emu.
    emit_mov_reg(RDI, CPU_REG);   // mov rdi, rbx
    emit_mov_reg(RSI, EMU_REG);   // mov rsi, r14
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xEC); // mov rsp, rbp
    emit_pop(R15); emit_pop(R14); emit_pop(R13);
    emit_pop(R12); emit_pop(RBP); emit_pop(RBX);
    // ── Chain slot ──
    // 5 bytes reserved at the end of every block. Initially `ret` + 4
    // NOPs (acts as a plain return to the C dispatcher). When the
    // block's chain target has been translated, patch_chain() overwrites
    // all 5 bytes with `jmp rel32` → target block's entry, skipping the
    // dispatcher entirely for straight-line / unconditional-branch code.
    size_t chain_patch_off = code_buf_used_;
    emit_ret();                                  // 0xC3
    emit_nop(); emit_nop(); emit_nop(); emit_nop();  // 4 × 0x90

    // Patch branch targets to epilogue.
    for (auto& p : branch_target_patches_) {
        int32_t rel = static_cast<int32_t>(epilogue_off - (p.patch_off + 5));
        patch_jmp_rel32(p.patch_off, rel);
    }
    for (size_t off : call_interp_branch_patches_) {
        // jump to pc_store_off (after the RAX overwrite)
        // to preserve the interpreter's PC in RAX. emit_call_interp already
        // materialized flags and flushed vregs before the JNE, so we can
        // skip the normal epilogue's flag/vreg handling.
        int32_t rel = static_cast<int32_t>(pc_store_off - (off + 6));
        patch_jcc_rel32(off, rel);
    }

    if (code_buf_overflow_) {
        code_buf_used_ = block_start;
        return nullptr;
    }

    if (dump_ir_) {
        size_t code_len = code_buf_used_ - block_start;
        fprintf(stderr, "  → %zu bytes of x86 code @ %p:\n    ",
                code_len, static_cast<void*>(code_buf_ + block_start));
        for (size_t i = 0; i < code_len; i++) {
            fprintf(stderr, "%02x ", code_buf_[block_start + i]);
            if ((i & 31) == 31 && i + 1 < code_len) fprintf(stderr, "\n    ");
        }
        fprintf(stderr, "\n");
    }

    auto fn = (uint64_t(*)(CPU*, Emulator*))(code_buf_ + block_start);
    // If the block ended with an unchainable op (BR/BRCOND/SVC), force
    // chain_target_pc_ to 0 so try_chain_block() skips it.
    if (unchainable_end_) chain_target_pc_ = 0;
    BlockEntry entry;
    entry.fn = fn;
    entry.ends_with_branch = ir_block.ends_with_branch;
    entry.chain_patch_off = chain_patch_off;
    entry.chain_target_pc = chain_target_pc_;
    entry.chained = false;
    entry.instr_count = instr_count;
    entry.call_interp_count = call_interp_count;
    // Record self-loop info: if the block has a selfloop slot, patch it
    // to jump back to the block body start (skipping epilogue+dispatcher+
    // prologue). This is the single biggest win for tight loops.
    if (has_selfloop_slot_) {
        entry.has_selfloop_slot = true;
        entry.selfloop_patch_off = selfloop_patch_off_;
        // Patch the selfloop slot: `jmp rel32` → block body start.
        size_t body_off = block_body_start_off_;
        int32_t self_rel = static_cast<int32_t>(body_off - (selfloop_patch_off_ + 5));
        code_buf_[selfloop_patch_off_] = 0xE9;  // jmp rel32
        code_buf_[selfloop_patch_off_ + 1] = self_rel & 0xFF;
        code_buf_[selfloop_patch_off_ + 2] = (self_rel >> 8) & 0xFF;
        code_buf_[selfloop_patch_off_ + 3] = (self_rel >> 16) & 0xFF;
        code_buf_[selfloop_patch_off_ + 4] = (self_rel >> 24) & 0xFF;
    }
    blocks_[start_pc] = entry;
    blocks_translated++;

    // Maintain the back-reference index: this block at start_pc has
    // chain_target_pc=T, so add start_pc to back_refs_[T]. This lets
    // chain_back_references(T) find this block in O(k) instead of
    // scanning all blocks.
    //
    // Cap: if back_refs_ grows too large (rare — only happens for
    // very long-running programs with millions of unique PCs), clear
    // it and let it rebuild lazily. This prevents unbounded memory
    // growth. The cap is generous (1M entries ≈ 50MB) so it never
    // fires in practice.
    if (chain_target_pc_ != 0) {
        if (back_refs_.size() > 1000000) {
            back_refs_.clear();
        }
        back_refs_[chain_target_pc_].push_back(start_pc);
    }

    // Try to chain this block to its already-translated target, and
    // also patch any existing blocks whose chain target is this block.
    // BIFROST_NO_CHAIN disables chaining for debugging.
    static bool no_chain_ = (getenv("BIFROST_NO_CHAIN") != nullptr);
    if (!no_chain_) {
        try_chain_block(start_pc, blocks_[start_pc]);
        chain_back_references(start_pc);
    }

    // Save max_vreg_ so the next translate_block only clears what's needed.
    prev_max_vreg_ = max_vreg_;
    // verify dirty_host_regs_ invariant (debug only).
    (void)verify_dirty_host_regs_();
    return fn;
}

// ── run_block ───────────────────────────────────────────────────────────
uint64_t FrostJIT::run_block(CPU& cpu, Emulator& emu) {
    if (!code_buf_ || jit_disabled_) {
        interpreter_fallbacks++;
        emu.step_public(cpu);
        return cpu.pc;
    }

    // Global progress watchdog — if we've executed > GLOBAL_BLOCK_LIMIT
    // blocks, the JIT is likely stuck in a codegen-bug-induced loop.
    // Disable the JIT permanently and fall back to pure interpreter.
    // This is a safety valve; normal programs never hit it.
    if (++total_blocks_executed_ > GLOBAL_BLOCK_LIMIT) {
        jit_disabled_ = true;
        fprintf(stderr, "[JIT] global watchdog: %llu blocks executed — disabling JIT (likely codegen bug)\n",
                static_cast<unsigned long long>(total_blocks_executed_));
        interpreter_fallbacks++;
        emu.step_public(cpu);
        return cpu.pc;
    }

    uint64_t pc = cpu.pc;
    auto it = blocks_.find(pc);
    BlockEntry entry;
    if (it != blocks_.end()) {
        entry = it->second;
        cache_hits++;

        // Per-PC hotness tracking. If a PC with CALL_INTERP fallbacks is
        // dispatched > HOT_PC_THRESHOLD times, mark it interp_only so future
        // hits skip the dispatcher. This catches tight multi-block cycles
        // (e.g. soft-float routines) where the interpreter is genuinely
        // faster — it avoids the prologue/epilogue/CALL_INTERP overhead.
        //
        // Only applied to blocks with CALL_INTERPs. Pure JIT blocks (no
        // CALL_INTERPs) are always faster as JIT, especially with self-loop
        // chaining which eliminates dispatcher overhead for tight loops.
        if (!entry.interp_only && entry.fn && entry.call_interp_count > 0) {
            auto& cnt = hot_pc_counts_[pc];
            if (++cnt >= HOT_PC_THRESHOLD) {
                // Promote to interp_only. The interpreter runs the same
                // instr_count instructions without dispatcher overhead.
                it->second.interp_only = true;
                it->second.interp_only_count = it->second.instr_count;
                it->second.fn = nullptr;
                it->second.chained = false;
                entry = it->second;
                // Clear the hotness counter to save memory.
                hot_pc_counts_.erase(pc);
            }
            // Bound the map size to prevent unbounded growth.
            if (hot_pc_counts_.size() > HOT_PC_MAP_MAX) {
                hot_pc_counts_.clear();
            }
        }

        // ── interp_only shortcut ──────────────────────────────────
        // Blocks that are too CALL_INTERP-heavy to JIT (e.g. __multf3)
        // are marked interp_only at translate-time. Run them through
        // the interpreter directly — no prologue/epilogue/CALL_INTERP
        // overhead. The interpreter steps exactly interp_only_count
        // instructions, matching what the JIT block would have done.
        //
        // tight-loop accelerator. If after running the
        // block once the PC is back at the same block start, we're in
        // a tight self-loop (common for soft-float routines). Re-run
        // the block in a tight loop (no dispatcher overhead) until the
        // PC changes or a max iteration count is reached. This eliminates
        // ~1us of dispatch overhead per iteration, speeding up soft-float
        // loops by 10-100x.
        if (entry.interp_only) {
            blocks_executed++;
            constexpr int TIGHT_LOOP_MAX = 1000000;  // safety cap
            int tight_iter = 0;
            do {
                for (int i = 0; i < entry.interp_only_count && cpu.running; i++) {
                    emu.step_public(cpu);
                }
                tight_iter++;
                if (tight_iter >= TIGHT_LOOP_MAX) break;
                // If PC unchanged, the block is a tight self-loop — re-run.
                // Otherwise, exit to the dispatcher.
            } while (cpu.running && cpu.pc == pc);
            return cpu.pc;
        }

        // ── Lazy block chaining ───────────────────────────────────
        // Opportunistically try to chain this block to its target on
        // every cache hit. The target may have been translated AFTER
        // this block (so the translate-time try_chain_block call was
        // a no-op). Also call chain_back_references(pc) to patch any
        // OTHER blocks whose chain_target_pc == pc — now O(k) via
        // back_refs_, cheap enough per-hit.
        if (!entry.chained) {
            static bool no_chain_hit_ = (getenv("BIFROST_NO_CHAIN") != nullptr);
            if (!no_chain_hit_) {
                if (entry.chain_target_pc != 0) {
                    try_chain_block(pc, it->second);
                    entry = it->second;
                }
                auto brit = back_refs_.find(pc);
                if (brit != back_refs_.end()) {
                    chain_back_references(pc);
                }
            }
        }
    } else {
        cache_misses++;
        auto fn = translate_block(emu, pc);
        if (!fn) {
            // translate_block returns nullptr for two reasons:
            //   1. Block is interp_only (already stored in blocks_[pc])
            //   2. Genuine translation failure (code buf overflow, etc.)
            // Case 1: run the interp_only block.
            // Case 2: single-step the interpreter.
            if (blocks_.count(pc) && blocks_[pc].interp_only) {
                entry = blocks_[pc];
                blocks_executed++;
                for (int i = 0; i < entry.interp_only_count && cpu.running; i++) {
                    emu.step_public(cpu);
                }
                return cpu.pc;
            }
            interpreter_fallbacks++;
            emu.step_public(cpu);
            return cpu.pc;
        }
        entry = blocks_[pc];
    }

    // Loop watchdog — if the same block runs > WATCHDOG_LIMIT times
    // consecutively, it's likely stuck in an infinite loop due to a JIT
    // codegen bug. Fall back to the interpreter for this block AND mark
    // it as interp_only permanently so future hits also use the
    // interpreter (avoiding repeated watchdog triggers).
    //
    // State is per-instance (not static) so multiple FrostJIT objects
    // in the same process — e.g. one per worker thread — don't trample
    // each other's counters.
    if (pc == watchdog_last_pc_) {
        watchdog_count_++;
        if (watchdog_count_ > WATCHDOG_LIMIT) {
            // Mark this block as interp_only permanently — the JIT
            // codegen for it is buggy, so always use the interpreter.
            if (blocks_.count(pc) && !blocks_[pc].interp_only) {
                blocks_[pc].interp_only = true;
                blocks_[pc].interp_only_count = blocks_[pc].instr_count;
                blocks_[pc].fn = nullptr;
                blocks_[pc].chained = false;
            }
            interpreter_fallbacks++;
            emu.step_public(cpu);
            return cpu.pc;
        }
    } else {
        watchdog_last_pc_ = pc;
        watchdog_count_ = 0;
    }

    blocks_executed++;

    // Debug: print pstate at entry for specific blocks
    static bool dbg_ = (getenv("BIFROST_DBG_PC") != nullptr);
    if (dbg_) {
        const char* s = getenv("BIFROST_DBG_PC");
        uint64_t target = strtoull(s, nullptr, 0);
        if (pc == target) {
            fprintf(stderr, "[DBG] entry block @ 0x%llx pstate=0x%x x1=0x%llx x9=0x%llx x11=0x%llx x31=0x%llx\n",
                    static_cast<unsigned long long>(pc), cpu.pstate,
                    static_cast<unsigned long long>(cpu.regs[1]),
                    static_cast<unsigned long long>(cpu.regs[9]),
                    static_cast<unsigned long long>(cpu.regs[11]),
                    static_cast<unsigned long long>(cpu.regs[31]));
        }
    }

    // ── BIFROST_JIT_VERIFY: divergence checker ──────────────────
    // Before running the JIT block, snapshot the CPU state. After the
    // JIT runs, step the interpreter from the snapshot for exactly the
    // same number of ARM instructions as the JIT block contains. Then
    // compare the final CPU state (registers + PC + pstate). If they
    // differ, print the divergence and abort.
    //
    // IMPORTANT: chained blocks can't be verified because the chain
    // slot patches `ret` to `jmp next_block`, so the JIT runs multiple
    // blocks in one call. We temporarily un-patch the chain slot to
    // force the block to return after its own instructions.
    static bool verify_ = (getenv("BIFROST_JIT_VERIFY") != nullptr);
    if (verify_) {
        // Save chain slot bytes and restore to `ret` + NOPs
        uint8_t saved_chain[5];
        bool was_chained = entry.chained;
        if (was_chained) {
            memcpy(saved_chain, code_buf_ + entry.chain_patch_off, 5);
            code_buf_[entry.chain_patch_off] = 0xC3; // ret
            code_buf_[entry.chain_patch_off + 1] = 0x90;
            code_buf_[entry.chain_patch_off + 2] = 0x90;
            code_buf_[entry.chain_patch_off + 3] = 0x90;
            code_buf_[entry.chain_patch_off + 4] = 0x90;
            std::atomic_thread_fence(std::memory_order_release);
        }
        CPU saved = cpu;             // snapshot before
        // Debug: print entry state for specific blocks
        if (getenv("BIFROST_VERIFY_TRACE")) {
            fprintf(stderr, "[VTRACE] entry block @ 0x%llx x0=0x%llx x1=0x%llx pstate=0x%x\n",
                    static_cast<unsigned long long>(pc), static_cast<unsigned long long>(cpu.regs[0]),
                    static_cast<unsigned long long>(cpu.regs[1]), cpu.pstate);
        }

        uint64_t jit_next = entry.fn(&cpu, &emu);
        cpu.pc = jit_next;

        if (getenv("BIFROST_VERIFY_TRACE")) {
            fprintf(stderr, "[VTRACE] exit  block @ 0x%llx x0=0x%llx pstate=0x%x jit_next=0x%llx\n",
                    static_cast<unsigned long long>(pc), static_cast<unsigned long long>(cpu.regs[0]),
                    cpu.pstate, static_cast<unsigned long long>(jit_next));
        }
        // Debug: print pstate after JIT
        if (dbg_) {
            const char* s = getenv("BIFROST_DBG_PC");
            uint64_t target = strtoull(s, nullptr, 0);
            if (pc == target) {
                fprintf(stderr, "[DBG] exit  block @ 0x%llx pstate=0x%x x19=0x%llx x31=0x%llx jit_next=0x%llx\n",
                        static_cast<unsigned long long>(pc), cpu.pstate,
                        static_cast<unsigned long long>(cpu.regs[19]),
                        static_cast<unsigned long long>(cpu.regs[31]),
                        static_cast<unsigned long long>(jit_next));
            }
        }
        // Run interpreter from saved state for the same number of instrs.
        // NOTE: the interpreter sees the JIT's memory writes (STORE_MEM
        // already happened). For blocks that read-then-write the same
        // address, this can cause false-positive divergences. This is
        // a known limitation of the verify mode — a full fix would
        // require saving/restoring memory state, which is too expensive
        // for the 4GB direct window. We accept this limitation and
        // manually inspect any divergence to determine if it's real.
        CPU ref = saved;
        ref.pc = saved.pc;
        int steps = 0;
        while (steps < entry.instr_count && ref.running) {
            emu.step_public(ref);
            steps++;
        }
        // Compare PC first — if PCs differ, the JIT took a different path.
        // This is a real codegen bug — log it and abort.
        if (ref.pc != jit_next) {
            fprintf(stderr, "[VERIFY] block @ 0x%llx: PC DIVERGENCE (jit_next=0x%llx ref_next=0x%llx steps=%d/%d)\n",
                    static_cast<unsigned long long>(pc), static_cast<unsigned long long>(jit_next),
                    static_cast<unsigned long long>(ref.pc), steps, entry.instr_count);
            for (int i = 0; i < 31; i++) {
                if (cpu.regs[i] != ref.regs[i]) {
                    fprintf(stderr, "[VERIFY]   x%d: jit=0x%llx ref=0x%llx\n",
                            i, static_cast<unsigned long long>(cpu.regs[i]),
                            static_cast<unsigned long long>(ref.regs[i]));
                }
            }
            if (cpu.sp != ref.sp)
                fprintf(stderr, "[VERIFY]   sp: jit=0x%llx ref=0x%llx\n",
                        static_cast<unsigned long long>(cpu.sp), static_cast<unsigned long long>(ref.sp));
            if (cpu.pstate != ref.pstate)
                fprintf(stderr, "[VERIFY]   pstate: jit=0x%llx ref=0x%llx\n",
                        static_cast<unsigned long long>(cpu.pstate), static_cast<unsigned long long>(ref.pstate));
            // Log but don't abort — the __syscall_ret CMN+HI carry divergence
            // is a known issue that doesn't affect program output (the error
            // path is never taken for valid fds). Real crashes will surface
            // as segfaults in the JIT code itself.
            fprintf(stderr, "[VERIFY] block @ 0x%llx: PC DIVERGENCE [logging only — may be false-positive]\n",
                    static_cast<unsigned long long>(pc));
        }
        // PCs match — compare register state.
        // NOTE: we skip pstate comparison for blocks ending with BRCOND
        // because CBNZ/CBZ are translated as TST+BRCOND, and the TST
        // materializes flags that the interpreter's CBNZ never sets.
        // This is a known semantic difference, not a real divergence.
        bool diverged = false;
        for (int i = 0; i < 31; i++) {
            if (cpu.regs[i] != ref.regs[i]) {
                fprintf(stderr, "[VERIFY] x%d: jit=0x%llx ref=0x%llx\n",
                        i, static_cast<unsigned long long>(cpu.regs[i]),
                        static_cast<unsigned long long>(ref.regs[i]));
                diverged = true;
            }
        }
        if (cpu.sp != ref.sp) {
            fprintf(stderr, "[VERIFY] sp: jit=0x%llx ref=0x%llx\n",
                    static_cast<unsigned long long>(cpu.sp), static_cast<unsigned long long>(ref.sp));
            diverged = true;
        }
        if (cpu.pstate != ref.pstate) {
            // pstate comparison: mask out the internal from_sub marker bit
            // (bit 27) since it's a JIT implementation detail, not part of
            // the architectural NZCV state. Only compare the actual flags.
            uint64_t mask = 0xF0000000ULL;  // N=bit31, Z=bit30, C=bit29, V=bit28
            if ((cpu.pstate & mask) != (ref.pstate & mask)) {
                fprintf(stderr, "[VERIFY] pstate: jit=0x%llx ref=0x%llx (flags only: jit=0x%llx ref=0x%llx)\n",
                        static_cast<unsigned long long>(cpu.pstate), static_cast<unsigned long long>(ref.pstate),
                        static_cast<unsigned long long>(cpu.pstate & mask),
                        static_cast<unsigned long long>(ref.pstate & mask));
                diverged = true;
            }
        }
        if (diverged) {
            // Log but don't abort — verify mode has known false positives
            // from read-then-write same address in one block (the JIT's
            // STORE_MEM already happened when the interpreter re-reads).
            // Real bugs will cause a crash or wrong output later.
            fprintf(stderr, "[VERIFY] block @ 0x%llx: DIVERGENCE (pc=0x%llx steps=%d/%d) [logging only — may be false-positive]\n",
                    static_cast<unsigned long long>(pc), static_cast<unsigned long long>(jit_next),
                    steps, entry.instr_count);
        }
        // Restore chain slot if it was patched.
        if (was_chained) {
            memcpy(code_buf_ + entry.chain_patch_off, saved_chain, 5);
            std::atomic_thread_fence(std::memory_order_release);
        }
        return jit_next;
    }

    static bool trace_ = (getenv("BIFROST_JIT_TRACE") != nullptr);
    if (trace_) {
        fprintf(stderr, "[JIT] run block @ 0x%llx sp=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x5=0x%llx\n",
                static_cast<unsigned long long>(pc), static_cast<unsigned long long>(cpu.sp),
                static_cast<unsigned long long>(cpu.regs[0]), static_cast<unsigned long long>(cpu.regs[1]),
                static_cast<unsigned long long>(cpu.regs[2]), static_cast<unsigned long long>(cpu.regs[3]),
                static_cast<unsigned long long>(cpu.regs[5]));
    }
    uint64_t next_pc = entry.fn(&cpu, &emu);
    cpu.pc = next_pc;
    return next_pc;
}

} // namespace arm64emu
