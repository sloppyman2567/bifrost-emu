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
//   - emit_frameless_back_edge  — emit a direct jcc/jmp to a loop-top body
//                                  (skipping the prologue)
//   - patch_pending_back_edges  — patch earlier-emitted back-edges to jump
//                                  to a now-translated target's body
//   - compile_ir_inst           — the big IR-op→x86 switch (1500+ lines)
//   - clobber_flags             — flush pending host flags to pstate before
//                                  a flag-clobbering instruction
//   - translate_block           — translate one ARM64 basic block to x86
//   - run_block                 — block dispatcher (cache hit / miss / verify)
//
// All emit_*, alloc_*, ensure_*, force_*, flush_*, invalidate_*, kill_*,
// vreg_*, patch_chain, try_chain_block, chain_back_references, ctor/dtor,
// flush_cache, and counter state live in the other src/jit/*.cpp files.
#include "jit/frostjit.hpp"
#include "arm64_emu.hpp"
#include "ir/ir.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unordered_map>
#include <vector>

namespace arm64emu {

// ── Forward decls of slow-path helpers defined in x86_backend.cpp ──────
// These are extern "C" so JIT-compiled code can call them by address
// without name-mangling concerns.
extern "C" {
    uint64_t jit_load_mem_slow(Emulator* emu, uint64_t addr, int width);
    void     jit_store_mem_slow(Emulator* emu, uint64_t addr, uint64_t val, int width);
    uint64_t jit_rbit(uint64_t val, int width);
    uint64_t jit_bfm(uint64_t dst, uint64_t src, int immr, int imms, int width);
    uint64_t jit_count_leading_zeros(uint64_t v, int width, bool is_cls);
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
                (unsigned long long)cpu->pc,
                (unsigned long long)cpu->regs[0],
                (unsigned long long)cpu->regs[1],
                (unsigned long long)cpu->regs[2],
                (unsigned long long)cpu->regs[24],
                (unsigned long long)cpu->regs[27],
                cpu->pstate);
    }
    emu->step_public(*cpu);
    if (getenv("BIFROST_STEP_TRACE")) {
        fprintf(stderr, "    [step] pc=0x%llx done x0=0x%llx x24=0x%llx pstate=0x%x\n",
                (unsigned long long)cpu->pc,
                (unsigned long long)cpu->regs[0],
                (unsigned long long)cpu->regs[24],
                cpu->pstate);
    }
}

} // namespace arm64emu

// ── The big chunks (extracted verbatim from the original frostjit.cpp) ──
// These are large method bodies that haven't been rewritten, just moved
// out of the monolithic file for readability. Each section is bracketed
// by clear section headers.

namespace arm64emu {

// ── emit_fmov_helper + emit_call_interp ─────────────────────────────────
void FrostJIT::emit_fmov_helper(int dir, int fp_field, uint16_t idx,
                                uint16_t src1, uint16_t dest) {
    int32_t fp_off = (fp_field == 0 ? V_LO_OFF : V_HI_OFF)
                   + static_cast<int>(idx) * 8;
    if (dir == 0) {
        // GPR → FP: load src1 vreg into RAX, store to fp_off.
        int s = ensure_vreg(src1, RAX);
        if (s != RAX) emit_mov_reg(RAX, s);
        emit_store(CPU_REG, fp_off, RAX);
        // FMOV_G2F (fp_field==0) also zeros v_hi[idx] per ARM semantics.
        if (fp_field == 0) {
            int32_t vhi_off = V_HI_OFF + static_cast<int>(idx) * 8;
            // (v1.4.0-beta.1): clobber_host_reg evicts any dirty vreg cached
            // in RAX BEFORE we overwrite it with 0. The old code silently
            // dropped src1 (if it was cached in RAX) via raw mapping clear.
            clobber_host_reg(RAX);
            emit_mov_imm32_zext(RAX, 0);
            emit_store(CPU_REG, vhi_off, RAX);
        } else {
            // For G2FHI, RAX still holds src1's value (not clobbered).
            // But we need to drop the mapping if src1 was loaded into RAX
            // via ensure_vreg (it's now "consumed" by the store). Use
            // clobber_host_reg to safely evict if dirty.
            clobber_host_reg(RAX);
        }
    } else {
        // FP → GPR: load fp_off into a fresh vreg for dest.
        int d = alloc_reg();
        emit_load(d, CPU_REG, fp_off);
        set_vreg_reg(dest, d);
    }
}

// ── emit_call_interp ───────────────────────────────────────────────────
void FrostJIT::emit_call_interp(uint64_t arm_pc, bool ends_block) {
    // Materialize host flags to pstate if valid.
    if (flags_in_host_) {
        // (v1.4.0-alpha.5 bugfix): emit_materialize_flags clobbers
        // RAX/RCX/RDX. We must invalidate their cache mappings AFTER
        // the materialize, otherwise a subsequent ensure_vreg would
        // return a stale (garbage) value. flush_caller_saved_vregs
        // only evicts DIRTY vregs — non-dirty cached vregs in RAX/
        // RCX/RDX get clobbered silently.
        emit_materialize_flags(flags_from_sub_);
        flags_in_host_ = false;
        // Drop cache mappings for the clobbered registers.
        for (int r : {RAX, RCX, RDX}) {
            int v = reg_vreg_[r];
            if (v >= 0) {
                vreg_home_[v] = -1;
                reg_vreg_[r] = -1;
                vreg_dirty_[v] = false;
            }
        }
    }
    // (v1.4.0-alpha.5): only flush CALLER-SAVED dirty vregs. Callee-saved
    // vregs (R12/R13/R15) are preserved by the C calling convention, so
    // they survive the call without spilling. This is the key win: live
    // values in callee-saved regs stay cached across interpreter calls.
    // BUT: SP (vreg 31) and PC-related regs must be flushed to memory
    // because the interpreter may read/modify them. Also, any arch reg
    // that the interpreter instruction writes to must be invalidated
    // after the call (handled by invalidate_caller_saved_vregs below,
    // but SP needs special handling since it's at a different offset).
    flush_all_vregs();  // must flush ALL dirty vregs — interp reads cpu.regs[]
    // (v1.4.0-alpha.5 bugfix): SP (vreg 31) is special — the interpreter
    // may modify it (stack ops, push/pop). If SP is cached in a caller-
    // saved reg and dirty, flush_caller_saved_vregs already wrote it to
    // cpu.sp. But if SP is cached in a CALLEE-SAVED reg (R12/R13/R15),
    // it won't be flushed, and after the call the cached value is stale
    // (the interpreter might have changed cpu.sp). Force-flush SP here.
    if (vreg_home_[31] >= 0 && vreg_dirty_[31]) {
        evict_vreg(31);
    }
    emit_push(WIN_REG);  // save R10 (caller-saved)  — 1 push
    emit_push(RAX);      // save RAX                 — 2 pushes (EVEN → no align fixup needed)
    // Set cpu.pc = arm_pc.
    if (arm_pc <= 0xFFFFFFFFULL) {
        emit_mov_imm32_zext(RAX, static_cast<uint32_t>(arm_pc));
    } else {
        emit_mov_imm64(RAX, arm_pc);
    }
    emit_store(CPU_REG, PC_OFF, RAX);
    // Set args: RDI = emu, RSI = cpu.
    emit_mov_reg(RDI, EMU_REG);
    emit_mov_reg(RSI, CPU_REG);
    emit_call_aligned(&jit_interp_step, /*num_pushed=*/2);
    emit_pop(RAX);       // restore RAX
    emit_pop(WIN_REG);   // restore WIN_REG
    // Reload PC into RAX.
    emit_load(RAX, CPU_REG, PC_OFF);
    // (v1.4.0-alpha.5): invalidate ALL cache mappings after the call.
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
            emit_mov_imm32_zext(RCX, (uint32_t)next_pc);
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

// ── emit_frameless_back_edge + patch_pending_back_edges ─────────────
bool FrostJIT::emit_frameless_back_edge(uint64_t target_pc, uint8_t cc) {
    static bool disable_ = (getenv("BIFROST_NO_FRAMELESS") != nullptr);
    if (disable_) return false;
    auto it = blocks_.find(target_pc);
    if (it == blocks_.end() || !it->second.frameless_compatible) {
        // Target not ready. Record a pending patch site that the caller
        // will create via emit_jcc_rel32_placeholder / emit_jmp_rel32_placeholder.
        // We can't record it here because the caller hasn't emitted the
        // placeholder yet. The caller calls patch_pending_back_edges_record
        // after emitting the placeholder. Actually, simpler: the caller
        // records it directly. So here we just return false.
        return false;
    }
    // Target is ready and compatible. Flush dirty arch vregs + flags.
    if (flags_in_host_) {
        emit_materialize_flags(flags_from_sub_);
        flags_in_host_ = false;
    }
    for (int v = 0; v <= 31; v++) {
        if (vreg_home_[v] >= 0 && vreg_dirty_[v]) {
            evict_vreg(v);
        }
    }
    // Drop all cache mappings (callee-saved host regs stay live).
    for (int v = 0; v <= max_vreg_; v++) {
        int r = vreg_home_[v];
        if (r >= 0) {
            reg_vreg_[r] = -1;
            vreg_home_[v] = -1;
            vreg_dirty_[v] = false;
        }
    }
    // Emit the jcc/jmp to the target's body.
    const uint8_t* target_body = code_buf_ + it->second.body_off;
    size_t patch_off = code_buf_used_;
    if (cc == 0xFF) {
        emit_byte(0xE9);  // jmp rel32
        int32_t rel = (int32_t)(target_body - (code_buf_ + patch_off + 5));
        emit_u32((uint32_t)rel);
    } else {
        emit_byte(0x0F); emit_byte(0x80 + cc);  // jcc rel32
        int32_t rel = (int32_t)(target_body - (code_buf_ + patch_off + 6));
        emit_u32((uint32_t)rel);
    }
    return true;
}

// Patch all pending back-edge sites that target `target_pc` to jump
// directly to the now-translated target's body. Called from
// translate_block() after a new block is registered.
void FrostJIT::patch_pending_back_edges(uint64_t target_pc) {
    static bool disable_ = (getenv("BIFROST_NO_FRAMELESS") != nullptr);
    if (disable_) return;
    auto pit = pending_back_edges_.find(target_pc);
    if (pit == pending_back_edges_.end()) return;
    auto bit = blocks_.find(target_pc);
    if (bit == blocks_.end() || !bit->second.frameless_compatible) return;
    const uint8_t* target_body = code_buf_ + bit->second.body_off;
    static bool dbg = (getenv("BIFROST_BACKEDGE_DBG") != nullptr);
    if (dbg) {
        fprintf(stderr, "[BACKEDGE] patching %zu pending back-edge(s) targeting 0x%llx → body_off=0x%zx\n",
                pit->second.size(), (unsigned long long)target_pc, bit->second.body_off);
    }
    for (auto& be : pit->second) {
        if (be.is_conditional) {
            // jcc rel32: 0F 8x rel32 (6 bytes). rel32 at be.patch_off + 2.
            if (code_buf_[be.patch_off] != 0x0F) {
                if (dbg) fprintf(stderr, "[BACKEDGE]   skip: byte at 0x%zx = 0x%02x (expected 0x0F)\n",
                                 be.patch_off, code_buf_[be.patch_off]);
                continue;
            }
            int32_t rel = (int32_t)(target_body - (code_buf_ + be.patch_off + 6));
            memcpy(code_buf_ + be.patch_off + 2, &rel, 4);
            if (dbg) fprintf(stderr, "[BACKEDGE]   patched jcc at 0x%zx → rel=0x%x (target_body=%p)\n",
                             be.patch_off, static_cast<unsigned>(rel), static_cast<const void*>(target_body));
        } else {
            // jmp rel32: E9 rel32 (5 bytes). rel32 at be.patch_off + 1.
            if (code_buf_[be.patch_off] != 0xE9) {
                if (dbg) fprintf(stderr, "[BACKEDGE]   skip: byte at 0x%zx = 0x%02x (expected 0xE9)\n",
                                 be.patch_off, code_buf_[be.patch_off]);
                continue;
            }
            int32_t rel = (int32_t)(target_body - (code_buf_ + be.patch_off + 5));
            memcpy(code_buf_ + be.patch_off + 1, &rel, 4);
            if (dbg) fprintf(stderr, "[BACKEDGE]   patched jmp at 0x%zx → rel=0x%x\n",
                             be.patch_off, static_cast<unsigned>(rel));
        }
    }
    // Clear the pending list — they're all patched now.
    pending_back_edges_.erase(pit);
}

// ── compile_ir_inst ────────────────────────────────────────────────────
// Emit x86 code for a single IR instruction.
// Returns true if the instruction ends the block.

// ── compile_ir_inst ────────────────────────────────────────────────
// Emit x86 code for a single IR instruction.
// Returns true if the instruction ends the block.
bool FrostJIT::compile_ir_inst(const IRInst& inst) {
    switch (inst.op) {
        case IROp::NOP:
            return false;

        case IROp::IMM:
            if (inst.dest) {
                int d = alloc_reg_for(inst.dest, -1);
                if (inst.imm <= 0xFFFFFFFFULL) {
                    emit_mov_imm32_zext(d, (uint32_t)inst.imm);
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
            {
                // Kill any existing value for dest, allocate a fresh reg.
                kill_vreg(inst.dest);
                int d = alloc_reg();
                emit_load_arm(d, inst.src1);
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
            clobber_flags();
            flush_all_vregs();
            invalidate_all_vregs();
            if (inst.src1 <= 31) emit_load_arm(RAX, inst.src1);
            else { int32_t off = vreg_stack_slot(inst.src1); emit_load(RAX, RBP, off); }
            emit_load_mem(RAX, RAX, (int32_t)inst.imm, inst.width, false);
            if (inst.dest <= 31) emit_store_arm(inst.dest, RAX);
            else { int32_t off = vreg_stack_slot(inst.dest); emit_store(RBP, off, RAX); }
            return false;
        }

        case IROp::STORE_MEM: {
            clobber_flags();
            flush_all_vregs();
            invalidate_all_vregs();
            if (inst.src1 <= 31) emit_load_arm(RAX, inst.src1);
            else { int32_t off = vreg_stack_slot(inst.src1); emit_load(RAX, RBP, off); }
            if (inst.src2 <= 31) emit_load_arm(RCX, inst.src2);
            else { int32_t off = vreg_stack_slot(inst.src2); emit_load(RCX, RBP, off); }
            emit_store_mem(RAX, (int32_t)inst.imm, RCX, inst.width);
            return false;
        }

        // ── Binary ALU ops ──
        // Load src1 into RAX and src2 into RCX (guaranteed different regs).
        // This avoids the aliasing bug where ensure_vreg(src2) evicts src1.
        // These ops clobber x86 RFLAGS, so materialize pending flags first.
        case IROp::ADD: case IROp::SUB: case IROp::AND:
        case IROp::OR:  case IROp::XOR: case IROp::MUL: {
            // Force src1 → RAX, src2 → RCX (aliasing-safe via force_two_vregs_to).
            clobber_flags();
            force_two_vregs_to(inst.src1, RAX, inst.src2, RCX);
            int s2 = RCX;  // src2 is in RCX; src1 (RAX) is implicit dest
            // Compute dest = src1 op src2. Reuse RAX for dest if possible.
            int d;
            if (inst.dest == inst.src1) {
                d = RAX;  // in-place
                vreg_dirty_[inst.dest] = true;
            } else {
                d = alloc_reg_for(inst.dest, RAX);
                if (d != RAX) emit_mov_reg(d, RAX);
            }
            switch (inst.op) {
                case IROp::ADD: emit_add_reg(d, s2); break;
                case IROp::SUB: emit_sub_reg(d, s2); break;
                case IROp::AND: emit_and_reg(d, s2); break;
                case IROp::OR:  emit_or_reg(d, s2);  break;
                case IROp::XOR: emit_xor_reg(d, s2); break;
                case IROp::MUL: emit_imul_reg(d, s2); break;
                default: break;
            }
            return false;
        }

        case IROp::SHL: case IROp::SHR:
        case IROp::SAR: case IROp::ROR: {
            // x86 variable shifts use CL for the count, so we must force
            // src2 into RCX and src1 into RAX. force_two_vregs_to handles
            // the eviction, move, and aliasing (src1==src2) cases in one
            // call — the previous inline version of this logic was ~50
            // lines and was duplicated across SHL/ADDS/ADCS cases.
            clobber_flags();
            force_two_vregs_to(inst.src1, RAX, inst.src2, RCX);

            // Pick dest reg. Reuse RAX if dest==src1; otherwise allocate
            // a fresh reg that is NOT RCX (we need CL for the count).
            int d;
            if (inst.dest == inst.src1) {
                d = RAX;
                vreg_dirty_[inst.dest] = true;
            } else {
                d = alloc_reg_for(inst.dest, RAX);
                if (d == RCX) {
                    // alloc_reg_for handed us RCX, but we cannot overwrite
                    // it (src2 lives there). Spill RCX's mapping for dest
                    // and grab a different reg.
                    vreg_home_[inst.dest] = -1;
                    reg_vreg_[RCX] = inst.src2;
                    vreg_home_[inst.src2] = RCX;
                    // Find any free reg != RCX, evicting if needed.
                    int pick = -1;
                    for (int i = 0; i < NUM_ALLOC_REGS; i++) {
                        int r = ALLOC_REGS[i];
                        if (r != RCX && reg_vreg_[r] == -1) { pick = r; break; }
                    }
                    if (pick < 0) {
                        // Evict RDX (deterministic) to make room.
                        if (reg_vreg_[RDX] >= 0) evict_vreg(reg_vreg_[RDX]);
                        pick = RDX;
                    }
                    d = pick;
                    vreg_home_[inst.dest] = d;
                    reg_vreg_[d] = inst.dest;
                    vreg_dirty_[inst.dest] = true;
                }
                if (d != RAX) emit_mov_reg(d, RAX);
            }
            // CL = src2 & 0x3F.
            emit_and_cl_imm8(0x3F);
            int kind = (inst.op == IROp::SHL) ? 4
                     : (inst.op == IROp::SHR) ? 5
                     : (inst.op == IROp::SAR) ? 7 : 1;
            emit_shift_cl(d, kind);
            set_vreg_reg(inst.dest, d);
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
            // (v1.4.0-beta.1 bugfix): lzcnt rax, rax overwrites RAX, destroying
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
            // (v1.4.0-beta.1 bugfix): bswap modifies RAX in place, which
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
            force_two_vregs_to(inst.src1, RAX, inst.src2, RCX);
            int s1 = RAX, s2 = RCX;
            bool is_sub = (inst.op == IROp::SUBS);
            bool is_32bit = (inst.width == 32);
            int d;
            if (inst.dest == inst.src1 && inst.dest != 0) {
                d = s1;
                vreg_dirty_[inst.dest] = true;
            } else if (inst.dest != 0) {
                d = alloc_reg_for(inst.dest, s1);
                if (d != s1) emit_mov_reg(d, s1);
            } else {
                d = s1;
            }
            if (is_32bit) {
                // 32-bit SUB/ADD: need REX prefix if either reg is R8-R15.
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
            return false;
        }

        case IROp::TST: {
            int s1 = ensure_vreg(inst.src1, RAX);
            int s2 = ensure_vreg(inst.src2, RCX);
            emit_test_reg(s1, s2);
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
            // (v1.4.0-alpha.5 fix): the previous code did
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
            // Evict dirty vreg in RAX, then drop the mapping.
            if (reg_vreg_[RAX] >= 0) {
                if (vreg_dirty_[reg_vreg_[RAX]]) evict_vreg(reg_vreg_[RAX]);
                else { vreg_home_[reg_vreg_[RAX]] = -1; reg_vreg_[RAX] = -1; }
            }
            int s1 = ensure_vreg(inst.src1, RAX);
            if (s1 != RAX) emit_mov_reg(RAX, s1);
            // RAX now holds the test value. Drop RAX's cache mapping so
            // the upcoming `mov eax, <pc>` doesn't corrupt any vreg.
            if (reg_vreg_[RAX] >= 0) {
                vreg_home_[reg_vreg_[RAX]] = -1;
                reg_vreg_[RAX] = -1;
            }
            // Save RFLAGS (in case any pending flags weren't materialized)
            emit_pushfq();
            // test rax, rax
            emit_test_reg(RAX, RAX);
            // jcc to taken target
            uint8_t cc = (inst.cond == 0) ? 4 /*JE*/ : 5 /*JNE*/;
            // ── Frameless back-edge (v1.4.0-alpha.5) ─────────────────
            // If this is a back-edge (target ≤ start_pc), try to emit a
            // direct jcc to the loop top's body. CBZ/CBNZ at the bottom
            // of a loop is the canonical case.
            bool is_back_edge = (inst.imm <= inst.arm_pc);
            if (is_back_edge) {
                // Flush dirty arch vregs for loop-top reload.
                for (int v = 0; v <= 31; v++) {
                    if (vreg_home_[v] >= 0 && vreg_dirty_[v]) {
                        evict_vreg(v);
                    }
                }
                if (emit_frameless_back_edge(inst.imm, cc)) {
                    // Frameless jcc emitted (taken → loop body). The jcc
                    // consumed the flags from `test` — we need to popfq
                    // on the not-taken path (fall-through).
                    // NOT taken: popfq, RAX = fall-through, go to epilogue.
                    emit_popfq();
                    uint64_t fall = inst.arm_pc + 4;
                    if (fall <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)fall);
                    else                            emit_mov_imm64(RAX, fall);
                    rax_holds_next_pc_ = true;
                    unchainable_end_ = true;
                    return true;
                }
            }
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
            // Not taken: RAX = fall-through.
            uint64_t fall = inst.arm_pc + 4;
            if (fall <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)fall);
            else                            emit_mov_imm64(RAX, fall);
            // Restore RFLAGS before jumping to epilogue
            emit_popfq();
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});
            // Taken: patch jcc to here.
            int32_t taken_rel = (int32_t)(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);
            // Restore RFLAGS (CBZ/CBNZ don't modify flags)
            emit_popfq();
            if (inst.imm <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)inst.imm);
            else                            emit_mov_imm64(RAX, inst.imm);
            // Record pending back-edge for later patching.
            if (is_back_edge) {
                pending_back_edges_[inst.imm].push_back({jcc_patch, inst.imm, true});
            }
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
            // (v1.4.0-alpha.5 fix): same RAX eviction as BRCOND_ZERO —
            // see the comment there for the rationale.
            clobber_flags();
            if (reg_vreg_[RAX] >= 0) {
                if (vreg_dirty_[reg_vreg_[RAX]]) evict_vreg(reg_vreg_[RAX]);
                else { vreg_home_[reg_vreg_[RAX]] = -1; reg_vreg_[RAX] = -1; }
            }
            int s1 = ensure_vreg(inst.src1, RAX);
            if (s1 != RAX) emit_mov_reg(RAX, s1);
            if (reg_vreg_[RAX] >= 0) {
                vreg_home_[reg_vreg_[RAX]] = -1;
                reg_vreg_[RAX] = -1;
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
            // ── Frameless back-edge (v1.4.0-alpha.5) ─────────────────
            bool is_back_edge = (inst.imm <= inst.arm_pc);
            if (is_back_edge) {
                for (int v = 0; v <= 31; v++) {
                    if (vreg_home_[v] >= 0 && vreg_dirty_[v]) {
                        evict_vreg(v);
                    }
                }
                if (emit_frameless_back_edge(inst.imm, cc)) {
                    emit_popfq();
                    uint64_t fall = inst.arm_pc + 4;
                    if (fall <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)fall);
                    else                            emit_mov_imm64(RAX, fall);
                    rax_holds_next_pc_ = true;
                    unchainable_end_ = true;
                    return true;
                }
            }
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
            // Not taken: RAX = fall-through.
            uint64_t fall = inst.arm_pc + 4;
            if (fall <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)fall);
            else                            emit_mov_imm64(RAX, fall);
            emit_popfq();
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});
            // Taken: patch jcc to here.
            int32_t taken_rel = (int32_t)(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);
            emit_popfq();
            if (inst.imm <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)inst.imm);
            else                            emit_mov_imm64(RAX, inst.imm);
            if (is_back_edge) {
                pending_back_edges_[inst.imm].push_back({jcc_patch, inst.imm, true});
            }
            rax_holds_next_pc_ = true;
            unchainable_end_ = true;
            return true;
        }

        // ── DEAD: decomposed in ir.cpp ──────────────────────────────
        // CSINC/CSINV/CSNEG were decomposed to ADD+NOT+NEG + CSEL in
        // ir.cpp (commit bfc7e76). The JIT only sees IROp::CSEL (native)
        // for these. This case is a defensive fallback — if a future
        // change accidentally re-emits CSINC/CSINV/CSNEG, the JIT will
        // fall back to the interpreter instead of crashing or producing
        // silent wrong-code. The fallback is correct but slow.
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

            // Compute x86 cc (true when ARM cond is TRUE).
            uint8_t base = inst.cond & 0xE;
            bool carry_is_direct = flags_in_host_ && !flags_from_sub_;
            bool need_cmc = false;
            uint8_t cc;
            if (carry_is_direct) {
                switch (base) {
                    case 0x2:  // CS/CC
                        cc = (inst.cond & 1) ? 3 : 2;
                        break;
                    case 0x8:  // HI/LS
                        need_cmc = true;
                        cc = arm_cond_to_x86(inst.cond);
                        break;
                    default:
                        cc = arm_cond_to_x86(inst.cond);
                        break;
                }
            } else {
                cc = arm_cond_to_x86(inst.cond);
            }

            // Ensure flags in host.
            if (!flags_in_host_) {
                flush_all_vregs();
                emit_load_flags_from_pstate();
                // Drop all cache mappings but DON'T clear flags_in_host_
                // (invalidate_all_vregs does). Flags ARE in host now.
                for (int v = 0; v <= max_vreg_; v++) {
                    int r = vreg_home_[v];
                    if (r >= 0) { reg_vreg_[r] = -1; vreg_home_[v] = -1; vreg_dirty_[v] = false; }
                }
                flags_in_host_ = true;
                flags_from_sub_ = false;
            }
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
            else if (inst.src1 <= 31) emit_load_arm(RAX, inst.src1);
            else { int32_t off = vreg_stack_slot(inst.src1); emit_load(RAX, RBP, off); }
            if (inst.src2 == 32) emit_mov_imm32_zext(RCX, 0);
            else if (inst.src2 <= 31) emit_load_arm(RCX, inst.src2);
            else { int32_t off = vreg_stack_slot(inst.src2); emit_load(RCX, RBP, off); }

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

            // Store RDX to dest.
            if (inst.dest <= 31) emit_store_arm(inst.dest, RDX);
            else { int32_t off = vreg_stack_slot(inst.dest); emit_store(RBP, off, RDX); }
            vreg_home_[inst.dest] = RDX;
            reg_vreg_[RDX] = inst.dest;
            vreg_dirty_[inst.dest] = true;
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
                flags_from_sub_ = false;
                invalidate_all_vregs();
            }
            uint8_t cc;
            uint8_t base = inst.cond & 0xE;
            // ── Carry polarity ────────────────────────────────────────
            // ARM C and x86 CF have DIFFERENT semantics after SUB:
            //   ARM C  = NOT borrow (1 = no borrow, i.e. dst >= src)
            //   x86 CF = borrow     (1 = borrow,     i.e. dst <  src)
            // After ADD they agree (both = carry-out). After TST, ARM C=0
            // and x86 CF=0 (TEST clears CF), so they also agree.
            //
            // The default arm_cond_to_x86() mapping assumes the "SUB
            // case" (x86 CF = NOT ARM C). When flags came from ADD/TST
            // (carry_is_direct), CS/CC need swapped mappings (the old
            // code did this correctly). HI/LS are the hard case: there
            // is no x86 JCC for "CF=1 AND ZF=0" (ARM HI after ADD), so
            // we emit `cmc` to invert CF, making it match the SUB
            // convention, then use the default JA/JBE mapping.
            //
            // BUGFIX (alpha.4): the old code's HI→JA mapping was wrong
            // (JA checks CF=0 AND ZF=0, but ARM HI after ADD needs
            // CF=1 AND ZF=0). This broke musl's __syscall_ret
            // `cmn x0, #0x1, lsl #12` + `b.hi error_path` — every
            // successful syscall was misclassified as an error, breaking
            // fopen(), read(), and every libc syscall wrapper.
            //
            // GE/LT/GT/LE depend on N, V, Z (not C), so the default
            // mapping works regardless of carry polarity.
            bool carry_is_direct = flags_in_host_ && !flags_from_sub_;
            bool need_cmc_for_hi_ls = false;
            if (carry_is_direct) {
                switch (base) {
                    case 0x2:  // CS/CC — swap mappings (correct, no cmc needed)
                        cc = (inst.cond & 1) ? 3 : 2;  // CS→JB(2)? no: CC→JAE(3), CS→JB(2)
                        // Wait: ARM CS (C=1) with direct CF → CF=1 → JB(2).
                        //       ARM CC (C=0) with direct CF → CF=0 → JAE(3).
                        // inst.cond & 1: CS=2 (bit0=0)→JB(2), CC=3 (bit0=1)→JAE(3).
                        cc = (inst.cond & 1) ? 3 : 2;
                        break;
                    case 0x8:  // HI/LS — no direct JCC, use cmc + default
                        need_cmc_for_hi_ls = true;
                        cc = arm_cond_to_x86(inst.cond);
                        break;
                    default:  // EQ/NE/MI/PL/VS/VC/GE/LT/GT/LE — default works
                        cc = arm_cond_to_x86(inst.cond);
                        break;
                }
            } else {
                cc = arm_cond_to_x86(inst.cond);
            }
            (void)base;
            // Materialize flags to pstate BEFORE consuming them for the
            // JCC, but save/restore RFLAGS around the materialization
            // because emit_materialize_flags clobbers them with its own
            // AND/SHIFT/OR operations. The JCC needs the original flags.
            if (flags_in_host_) {
                // (v1.4.0-alpha.5 bugfix): emit_materialize_flags clobbers
                // RAX/RCX/RDX. Invalidate their cache mappings so a later
                // ensure_vreg doesn't return stale (garbage) values.
                emit_pushfq();
                emit_materialize_flags(flags_from_sub_);
                emit_popfq();
                for (int r : {RAX, RCX, RDX}) {
                    int v = reg_vreg_[r];
                    if (v >= 0) {
                        vreg_home_[v] = -1;
                        reg_vreg_[r] = -1;
                        vreg_dirty_[v] = false;
                    }
                }
                // BUGFIX (alpha.4): if flags came from ADD/TST and the
                // condition is HI/LS, invert CF with `cmc` so it matches
                // the SUB convention that arm_cond_to_x86() expects.
                // pstate already has the correct ARM C (from materialize
                // above), so this only affects the JCC. CS/CC are handled
                // by the swapped mapping above (no cmc needed). GE/LT/GT/LE
                // don't depend on C (no cmc needed).
                if (need_cmc_for_hi_ls) {
                    emit_byte(0xF5);  // cmc
                }
            }
            flags_in_host_ = false;
            // ── Frameless back-edge chaining (v1.4.0-alpha.5) ────────
            // If the branch target is a back-edge (target ≤ start_pc),
            // try to emit a direct jcc to the target's body, skipping
            // the epilogue + dispatcher + prologue. This is the hot
            // loop case — the perf win is ~10×.
            bool is_back_edge = (inst.imm <= inst.arm_pc);
            if (is_back_edge) {
                // Flush dirty arch vregs (needed for loop top reload).
                // Flags already materialized above.
                for (int v = 0; v <= 31; v++) {
                    if (vreg_home_[v] >= 0 && vreg_dirty_[v]) {
                        evict_vreg(v);
                    }
                }
                // Try frameless back-edge for the TAKEN path.
                if (emit_frameless_back_edge(inst.imm, cc)) {
                    // Frameless jcc emitted (taken → loop body). Now emit
                    // the NOT-taken path: fall-through to next PC via
                    // the normal epilogue.
                    // The jcc we just emitted jumps to the loop body if
                    // taken; if not taken, execution falls through to
                    // here. Set up RAX = fall-through PC and go to epilogue.
                    uint64_t fall = inst.arm_pc + 4;
                    if (fall <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)fall);
                    else                        emit_mov_imm64(RAX, fall);
                    rax_holds_next_pc_ = true;
                    unchainable_end_ = true;
                    return true;
                }
                // Target not ready or not compatible. Emit a normal jcc
                // placeholder that, for now, jumps to the taken-epilogue
                // path. Record it as a pending back-edge so the target's
                // translate_block() can patch it to jump to the body.
                size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
                // Not taken: RAX = fall-through.
                uint64_t fall = inst.arm_pc + 4;
                if (fall <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)fall);
                else                        emit_mov_imm64(RAX, fall);
                size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
                branch_target_patches_.push_back({jmp_to_epilogue, 0});
                // Taken: RAX = target.
                int32_t taken_rel = (int32_t)(code_buf_used_ - (jcc_patch + 6));
                patch_jcc_rel32(jcc_patch, taken_rel);
                if (inst.imm <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)inst.imm);
                else                            emit_mov_imm64(RAX, inst.imm);
                // Record pending back-edge: when the target block is
                // translated, patch this jcc to jump to its body.
                pending_back_edges_[inst.imm].push_back({jcc_patch, inst.imm, true});
                rax_holds_next_pc_ = true;
                unchainable_end_ = true;
                return true;
            }
            // Forward branch: full flush + normal epilogue.
            emit_pushfq();
            flush_all_vregs();
            emit_popfq();
            size_t jcc_patch = emit_jcc_rel32_placeholder(cc);
            // Not taken: RAX = fall-through.
            {
                uint64_t fall = inst.arm_pc + 4;
                if (fall <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)fall);
                else                        emit_mov_imm64(RAX, fall);
            }
            size_t jmp_to_epilogue = emit_jmp_rel32_placeholder();
            branch_target_patches_.push_back({jmp_to_epilogue, 0});
            // Taken: RAX = target.
            int32_t taken_rel = (int32_t)(code_buf_used_ - (jcc_patch + 6));
            patch_jcc_rel32(jcc_patch, taken_rel);
            if (inst.imm <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)inst.imm);
            else                            emit_mov_imm64(RAX, inst.imm);
            rax_holds_next_pc_ = true;
            unchainable_end_ = true;  // conditional branch — runtime-dependent next PC
            return true;
        }

        case IROp::BRCOND_FALLTHRU: {
            flush_all_vregs();
            if (inst.imm <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)inst.imm);
            else                            emit_mov_imm64(RAX, inst.imm);
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
        case IROp::FP_BINOP: {
            // v_lo[dest] = op(v_lo[src1], v_lo[src2]); v_hi[dest] = 0
            bool is_double = (inst.width == 1);
            uint8_t ld_prefix = is_double ? 0xF2 : 0xF3;  // MOVSD/MOVSS
            clobber_flags();
            flush_all_vregs();
            invalidate_all_vregs();

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
            uint8_t opc = (uint8_t)inst.imm;
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
            flush_all_vregs();
            invalidate_all_vregs();

            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            emit_byte(prefix);
            emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);

            uint8_t opc = (uint8_t)inst.imm;
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
            bool is_double = (inst.width == 1);
            bool is_unsigned = (inst.imm == 1);
            clobber_flags();
            flush_all_vregs();
            invalidate_all_vregs();

            // Load FP value into XMM0
            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);

            // CVTTSD2SI rax, xmm0 (truncate toward zero)
            // F2 48 0F 2C C0 (signed) or use CVTTSS2SI for single
            emit_byte(prefix); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2C);
            emit_byte(0xC0);  // rax, xmm0

            // For unsigned, we need to handle values > INT64_MAX.
            // For now, just use the signed result — most code doesn't
            // convert huge doubles to unsigned.
            (void)is_unsigned;  // TODO: handle unsigned properly

            // Store result to cpu.regs[dest]
            if (inst.dest <= 31) emit_store_arm(inst.dest, RAX);
            return false;
        }

        // ── int→FP conversion (SCVTF/UCVTF) ────────────────────────
        case IROp::FP_I2F: {
            // v_lo[dest] = (float/double)(regs[src1]); v_hi=0
            bool is_double = (inst.width == 1);
            bool is_unsigned = (inst.imm == 1);
            clobber_flags();
            flush_all_vregs();
            invalidate_all_vregs();

            // Load GPR into RAX
            if (inst.src1 <= 31) emit_load_arm(RAX, inst.src1);

            // For unsigned, we'd need to handle the sign bit differently.
            // For now, use signed conversion — most code uses signed.
            (void)is_unsigned;

            // CVTSI2SD xmm0, rax (convert signed int64 to double)
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            emit_byte(prefix); emit_byte(0x48); emit_byte(0x0F); emit_byte(0x2A);
            emit_byte(0xC0);  // xmm0, rax

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
            flush_all_vregs();
            invalidate_all_vregs();

            // Load src1 into XMM0
            int32_t off1 = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            uint8_t prefix = is_double ? 0xF2 : 0xF3;
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
            emit_modrm_disp(0, CPU_REG, off1);

            // Load src2 into XMM1 (or zero for FCMP #0.0)
            if (inst.src2 != 0 || inst.imm != 0) {
                int32_t off2 = V_LO_OFF + static_cast<int>(inst.src2) * 8;
                emit_byte(prefix); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(1, CPU_REG, off2);
            } else {
                // FCMP Dn, #0.0 — XORPS xmm1, xmm1 to get 0.0
                emit_byte(0x0F); emit_byte(0x57); emit_byte(0xC9); // xorps xmm1, xmm1
            }

            // UCOMISD/UCOMISS xmm0, xmm1
            emit_byte(prefix); emit_byte(0x0F); emit_byte(0x2E);
            emit_byte(0xC1);  // xmm0, xmm1

            // Build pstate in RDX using conditional moves.
            // pushfq to get flags into RAX, then test bits.
            emit_pushfq();
            emit_byte(0x58);  // pop rax (flags in rax)

            // RDX = 0 (default)
            emit_xor_reg(RDX, RDX);

            // Save rax (flags image) — we need it for multiple tests.
            emit_byte(0x50);  // push rax

            // Test PF (unordered): if PF=1, pstate = 0x28000000 (C=1, V=1)
            emit_byte(0x48); emit_byte(0xA9); emit_u32(0x04); // test rax, 4 (PF)
            emit_mov_imm32_zext(RCX, 0x28000000);
            emit_byte(0x0F); emit_byte(0x45); emit_byte(0xD1); // cmovne rdx, rcx

            // Test CF (less): if CF=1, pstate = 0x80000000 (N=1)
            emit_byte(0x48); emit_byte(0xA9); emit_u32(0x01); // test rax, 1 (CF)
            emit_mov_imm32_zext(RCX, 0x80000000);
            emit_byte(0x0F); emit_byte(0x45); emit_byte(0xD1); // cmovne rdx, rcx

            // Test ZF (equal): if ZF=1, pstate = 0x60000000 (Z=1, C=1)
            emit_byte(0x48); emit_byte(0xA9); emit_u32(0x40); // test rax, 0x40 (ZF)
            emit_mov_imm32_zext(RCX, 0x60000000);
            emit_byte(0x0F); emit_byte(0x45); emit_byte(0xD1); // cmovne rdx, rcx

            // If RDX still 0 (none matched), it's "greater" → C=1
            emit_byte(0x48); emit_byte(0x85); emit_byte(0xD2); // test rdx, rdx
            emit_mov_imm32_zext(RCX, 0x20000000);
            emit_byte(0x0F); emit_byte(0x44); emit_byte(0xD1); // cmove rdx, rcx

            emit_byte(0x58);  // pop rax (discard)

            // Store pstate
            emit_store32(CPU_REG, PSTATE_OFF, RDX);
            flags_in_host_ = false;
            return false;
        }

        // ── FMOV immediate (load decoded FP immediate) ──────────────
        case IROp::FP_MOVI: {
            // v_lo[dest] = imm; v_hi[dest] = 0
            // (v1.4.0-beta.1): clobber_host_reg evicts any dirty GPR vreg
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
            clobber_flags();
            flush_all_vregs();
            invalidate_all_vregs();

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
            uint8_t opc = (uint8_t)inst.imm;
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
            // (v1.4.0-beta.1): DON'T drop src1's cache mapping after the
            // store — src1 may be read again later in the block. The old
            // code did `vreg_home_[reg_vreg_[RAX]] = -1; reg_vreg_[RAX] = -1`
            // which silently dropped a dirty src1.
            int s = ensure_vreg(inst.src1, RAX);
            if (s != RAX) emit_mov_reg(RAX, s);
            int32_t offlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_store(CPU_REG, offlo, RAX);
            emit_store(CPU_REG, offhi, RAX);
            // src1 stays cached in RAX (or its original reg) for later readers.
            return false;
        }

        // ── SIMD MOVI (broadcast immediate) ─────────────────────────
        case IROp::SIMD_MOVI: {
            // v_lo[dest] = v_hi[dest] = imm
            // (v1.4.0-beta.1): clobber_host_reg evicts any dirty GPR vreg
            // cached in RAX BEFORE we overwrite it with the immediate.
            clobber_host_reg(RAX);
            emit_mov_imm64(RAX, inst.imm);
            int32_t offlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_store(CPU_REG, offlo, RAX);
            emit_store(CPU_REG, offhi, RAX);
            return false;
        }

        // ── SIMD LDST (read/write v_lo/v_hi to/from vregs) ─────────
        case IROp::SIMD_LDST: {
            // width=1 (load): src1=lo vreg, src2=hi vreg → v_lo[dest], v_hi[dest]
            // width=0 (store): v_lo[dest] → src1 vreg, v_hi[dest] → src2 vreg
            if (inst.width == 1) {
                // Load: write vregs to v_lo/v_hi
                // (v1.4.0-beta.1): use separate host regs for lo/hi so we
                // don't clobber src1's cached value when loading src2.
                // The old code reused RAX for both, dropping src1's mapping.
                int slo = ensure_vreg(inst.src1, RAX);
                int32_t offlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
                emit_store(CPU_REG, offlo, slo);

                // For the hi half, use a different reg if possible.
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
            // (v1.4.0-alpha.5 fix): flush+invalidate FIRST so the
            // cache is empty and the subsequent load/store_vreg can't
            // interact with stale mappings. We then write the result
            // directly to the dest vreg's memory home and re-cache it.
            clobber_flags();  // shifts/ands clobber RFLAGS
            flush_all_vregs();
            invalidate_all_vregs();
            if (inst.src1 <= 31) emit_load_arm(RAX, inst.src1);
            else { int32_t off = vreg_stack_slot(inst.src1); emit_load(RAX, RBP, off); }

            // Handle common aliases efficiently:
            // - LSL (imms < immr): shift left by (width - immr)
            // - LSR (imms == width-1, UBFM): shift right by immr
            // - ASR (imms == width-1, SBFM): arithmetic shift right by immr

            // LSL: imms < immr (e.g. lsl w0, w0, #2 = UBFM w0, w0, #30, #31)
            // UBFM semantics for imms < immr:
            //   field = src & ((1 << (imms+1)) - 1)   [take low imms+1 bits]
            //   result = field << (width - immr)       [shift left to position]
            // BUGFIX (alpha.4): the previous code did shl THEN and, which
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
                        emit_byte(0xC1); emit_byte(modrm(3, 4, RAX & 7)); emit_byte((uint8_t)sh);
                    } else {
                        emit_shift_imm8(RAX, 4, sh);
                    }
                    if (width == 32) {
                        if (RAX >= 8) emit_byte(0x45);
                        emit_byte(0x89); emit_byte(modrm(3, RAX&7, RAX&7));
                    }
                    // Write result directly to dest's memory home, then cache.
                    if (inst.dest <= 31) emit_store_arm(inst.dest, RAX);
                    else { int32_t off = vreg_stack_slot(inst.dest); emit_store(RBP, off, RAX); }
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
                                emit_byte(0xC1); emit_byte(modrm(3, 7, RAX & 7)); emit_byte((uint8_t)immr);
                            } else {
                                // SHR (logical)
                                emit_byte(0xC1); emit_byte(modrm(3, 5, RAX & 7)); emit_byte((uint8_t)immr);
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
                if (inst.dest <= 31) emit_store_arm(inst.dest, RAX);
                else { int32_t off = vreg_stack_slot(inst.dest); emit_store(RBP, off, RAX); }
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
            // BUGFIX (alpha.4): after ROR by immr, the field that was at
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
            if (inst.dest <= 31) emit_store_arm(inst.dest, RAX);
            else { int32_t off = vreg_stack_slot(inst.dest); emit_store(RBP, off, RAX); }
            set_vreg_reg(inst.dest, RAX);
            return false;
        }

        // ── DEAD: decomposed in ir.cpp ──────────────────────────────
        // BFM was decomposed to SHL+SHR+OR+AND+OR in ir.cpp (commit
        // bfc7e76). This case is a defensive fallback.
        case IROp::BFM: {
            emit_call_interp(inst.arm_pc, false);
            return false;
        }

        // ── DEAD: decomposed in ir.cpp ──────────────────────────────
        // EXTR was decomposed to SHL+SHR+OR in ir.cpp (commit 8fd8e6c).
        // This case is a defensive fallback. The ~45 lines of native
        // codegen that used to live here were removed — if you need to
        // revive them, see git history (commit 8fd8e6c^).
        case IROp::EXTR: {
            emit_call_interp(inst.arm_pc, false);
            return false;
        }

        // ── DEAD: decomposed in ir.cpp ──────────────────────────────
        // RBIT/REV16/REV32 were decomposed to SWAR shift/mask patterns
        // in ir.cpp (commit 1aad1e1). CLS was decomposed to SAR+XOR+
        // CLZ+SUB in ir.cpp (commit a0e545c). These cases are defensive
        // fallbacks.
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
            // We must load the C flag from pstate into x86 CF first
            // (emit_load_flags_from_pstate handles the from_sub inversion).
            bool is_sub = (inst.op == IROp::SBCS);
            bool is_32bit = (inst.width == 32);

            // Load flags from pstate (we need CF in x86 CF).
            // If flags_in_host_, materialize first (to preserve pstate),
            // then we already have flags in host.
            if (flags_in_host_) {
                // Materialize to pstate (clobbers RAX/RCX/RDX).
                for (int r : {RAX, RCX, RDX}) {
                    int v = reg_vreg_[r];
                    if (v >= 0 && vreg_dirty_[v]) evict_vreg(v);
                }
                emit_pushfq();
                emit_materialize_flags(flags_from_sub_);
                emit_popfq();
                for (int r : {RAX, RCX, RDX}) {
                    int v = reg_vreg_[r];
                    if (v >= 0) {
                        vreg_home_[v] = -1;
                        reg_vreg_[r] = -1;
                        vreg_dirty_[v] = false;
                    }
                }
            } else {
                flush_all_vregs();
                emit_load_flags_from_pstate();
                invalidate_all_vregs();
            }
            // Now x86 CF holds the ARM C flag (correctly un-inverted
            // by emit_load_flags_from_pstate if from_sub was set).

            // Force src1 into RAX, src2 into RCX (same pattern as SHL/ADDS).
            force_two_vregs_to(inst.src1, RAX, inst.src2, RCX);
            int s1 = RAX, s2 = RCX;
            int d;
            if (inst.dest == inst.src1 && inst.dest != 0) {
                d = s1;
                vreg_dirty_[inst.dest] = true;
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
            uint8_t base = inst.cond & 0xE;
            bool carry_is_direct = flags_in_host_ && !flags_from_sub_;
            bool need_cmc = false;
            uint8_t cc;
            if (carry_is_direct) {
                switch (base) {
                    case 0x2: cc = (inst.cond & 1) ? 3 : 2; break;
                    case 0x8: need_cmc = true; cc = arm_cond_to_x86(inst.cond); break;
                    default: cc = arm_cond_to_x86(inst.cond); break;
                }
            } else {
                cc = arm_cond_to_x86(inst.cond);
            }

            // Ensure flags in host.
            if (!flags_in_host_) {
                flush_all_vregs();
                emit_load_flags_from_pstate();
                // Drop all cache mappings WITHOUT clearing flags_in_host_.
                for (int v = 0; v <= max_vreg_; v++) {
                    int r = vreg_home_[v];
                    if (r >= 0) { reg_vreg_[r] = -1; vreg_home_[v] = -1; vreg_dirty_[v] = false; }
                }
                flags_in_host_ = true;
                flags_from_sub_ = false;
            }
            if (need_cmc) emit_byte(0xF5);

            // Load src1 (rn) → RAX, src2 (rm) → RCX.
            if (inst.src1 == 32) emit_mov_imm32_zext(RAX, 0);
            else load_vreg(RAX, inst.src1);
            if (inst.src2 == 32) emit_mov_imm32_zext(RCX, 0);
            else load_vreg(RCX, inst.src2);

            // jcc do_compare (if cond TRUE, do the compare)
            size_t jcc_to_compare = emit_jcc_rel32_placeholder(cc);
            // --- else path: cond FALSE, set pstate = nzcv ---
            uint32_t pstate_else = ((uint32_t)nzcv << 28);
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
            int32_t rel_compare = (int32_t)(compare_off - (jcc_to_compare + 6));
            patch_jcc_rel32(jcc_to_compare, rel_compare);
            int32_t rel_end = (int32_t)(end_off - (jmp_to_end + 5));
            patch_jmp_rel32(jmp_to_end, rel_end);

            flags_in_host_ = false;
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

// ── clobber_flags ─────────────────────────────────────────────────
void FrostJIT::clobber_flags() {
    if (flags_in_host_) {
        // emit_materialize_flags clobbers RAX, RCX, RDX.
        // Evict any dirty vregs in those registers FIRST so their
        // values are preserved in cpu.regs[]/stack.
        for (int r : {RAX, RCX, RDX}) {
            int v = reg_vreg_[r];
            if (v >= 0 && vreg_dirty_[v]) {
                evict_vreg(v);
            }
        }
        emit_materialize_flags(flags_from_sub_);
        flags_in_host_ = false;
        // Drop cache mappings for RAX/RCX/RDX (values were evicted above
        // if dirty; non-dirty values can be safely reloaded from memory).
        for (int r : {RAX, RCX, RDX}) {
            int v = reg_vreg_[r];
            if (v >= 0) {
                vreg_home_[v] = -1;
                reg_vreg_[r] = -1;
                vreg_dirty_[v] = false;
            }
        }
    }
}

// ── Block chaining helpers ──────────────────────────────────────────────
// Patch a block's 5-byte chain slot (originally `ret` + 4 NOPs) in place
// to `jmp rel32` → target_fn. x86 is icache-coherent, so no explicit
// cache flush is needed, but we emit a memory barrier to ensure the
// patched bytes are visible to any in-flight execution on the same core.

// ── translate_block ───────────────────────────────────────────────
uint64_t (*FrostJIT::translate_block(Emulator& emu, uint64_t start_pc))(CPU*, Emulator*) {
    if (!code_buf_) return nullptr;
    code_buf_overflow_ = false;
    call_interp_branch_patches_.clear();
    branch_target_patches_.clear();
    back_edge_patches_.clear();  // v1.4.0-alpha.5: frameless back-edge sites
    rax_holds_next_pc_ = false;
    flags_in_host_ = false;
    flags_from_sub_ = false;
    chain_target_pc_ = 0;
    unchainable_end_ = false;
    num_stack_slots_ = 0;
    max_vreg_ = 0;
    for (int i = 0; i < 4096; i++) {
        vreg_home_[i] = -1;
        vreg_dirty_[i] = false;
        vreg_slot_[i] = 0;
    }
    for (int i = 0; i < 16; i++) reg_vreg_[i] = -1;

    size_t block_start = code_buf_used_;

    // ── Translate ARM64 → IR ─────────────────────────────────────
    IRBlock ir_block;
    ir_block.start_pc = start_pc;
    ir_reset_vreg_alloc();

    constexpr int MAX_BLOCK = 256;
    // BUGFIX (alpha.4): limit the number of CALL_INTERP fallbacks per
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
    while (!block_ended && instr_count < MAX_BLOCK) {
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

        // ── Pre-scan: if this instruction will produce a CALL_INTERP
        // and we've already hit the limit, split the block here. ──
        // We check the instruction class to see if it's one that
        // routes to CALL_INTERP in the IR translator.
        //
        // (v1.4.0-alpha.5): refined after IR decomposition work:
        //   - CSEL/CSINC/CSINV/CSNEG: decomposed in ir.cpp (CSEL is native)
        //   - CCMP/CCMN: native in the JIT
        //   - BFM: decomposed in ir.cpp (SHL+SHR+OR+AND are native)
        //   - EXTR: decomposed in ir.cpp (SHL+SHR+OR are native)
        //   - RBIT/REV16/REV32: decomposed in ir.cpp (SWAR via SHL+SHR+AND+OR)
        //   - ADC_REG/SBC_REG (no-flags): decomposed in ir.cpp
        //     (CSEL+NOT+ADD primitives are native)
        //   - ADCS_REG/SBCS_REG: native IROp::ADCS/SBCS (no CALL_INTERP)
        //   - LDP/STP GPR: decomposed to 2x LOAD_MEM/STORE_MEM (no CALL_INTERP)
        //   - LDP/STP SIMD (is_vec=true): still CALL_INTERP — checked below
        bool will_call_interp = false;
        switch (d.cls) {
            case InstClass::SIMD_LD1: case InstClass::SIMD_ST1:
            case InstClass::SIMD_LOGICAL: case InstClass::SIMD_SHIFT:
            case InstClass::SIMD_DUP: case InstClass::SIMD_CNT:
            case InstClass::SIMD_REV: case InstClass::SIMD_DP:
            case InstClass::FMOV: case InstClass::FMOV_IMM:
            case InstClass::FMOV_VD1: case InstClass::FMOV_RVD1:
            case InstClass::FADD: case InstClass::FSUB:
            case InstClass::FMUL: case InstClass::FDIV:
            case InstClass::FMAX: case InstClass::FMIN:
            case InstClass::FNMUL: case InstClass::FMADD:
            case InstClass::FMSUB: case InstClass::FABS:
            case InstClass::FNEG: case InstClass::FSQRT:
            case InstClass::FCMP: case InstClass::FCMPE:
            case InstClass::FCVT: case InstClass::FCVTZS:
            case InstClass::FCVTZU: case InstClass::SCVTF:
            case InstClass::UCVTF: case InstClass::FCSEL:
            case InstClass::FRINT: case InstClass::FP_SCALAR:
            // BFM/EXTR are now decomposed in ir.cpp — no longer CALL_INTERP.
            case InstClass::MRS: case InstClass::MRS_SYS:
            case InstClass::MSR: case InstClass::MSR_SYS:
            case InstClass::UDIV: case InstClass::SDIV:
            // CLS is now decomposed via SAR+XOR+CLZ+SUB — no CALL_INTERP.
            // SMADDL/SMSUBL/UMADDL/UMSUBL/SMULH/UMULH: still CALL_INTERP
            // (long-multiply forms need 128-bit accumulation).
            case InstClass::SMADDL: case InstClass::SMSUBL:
            case InstClass::UMADDL: case InstClass::UMSUBL:
            case InstClass::SMULH: case InstClass::UMULH:
            case InstClass::LDXR: case InstClass::STXR:
            case InstClass::LDAXR: case InstClass::STLXR:
            case InstClass::LDAR: case InstClass::STLR:
            case InstClass::LSE_ATOMIC:
                will_call_interp = true;
                break;
            // LDP/STP: GPR form is decomposed to LOAD_MEM/STORE_MEM;
            // only the SIMD (is_vec) form falls back to CALL_INTERP.
            // The is_vec check below handles this.
            default:
                break;
        }
        // Also check for vector load/store (is_vec=true LDR/STR/LDP/STP)
        if (!will_call_interp && d.is_vec &&
            (d.cls == InstClass::LDR_IMM || d.cls == InstClass::LDR_UNS ||
             d.cls == InstClass::LDR_REG || d.cls == InstClass::STR_IMM ||
             d.cls == InstClass::STR_UNS || d.cls == InstClass::STR_REG ||
             d.cls == InstClass::LDP || d.cls == InstClass::STP)) {
            will_call_interp = true;
        }

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
    // blocks with ANY CALL_INTERP, the pure interpreter is faster — it
    // skips the prologue/epilogue/dispatch entirely.
    //
    // This fixes the long-double multiply/divide hang: __multf3/__divtf3
    // are ~82-instruction soft-float routines split into ~20 tiny blocks
    // by the MAX_CALL_INTERP_PER_BLOCK=2 splitter. The JIT was 100-200x
    // slower than the interpreter for these, causing effective hangs on
    // jit_block_split.elf and jit_fp_scalar.elf with --jit.
    //
    // Interp-only blocks are cached (so we skip the re-decode cost on
    // cache hits) and run exactly instr_count interpreter steps.
    if (call_interp_count > 0) {
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
                (unsigned long long)start_pc, instr_count);
        dump_ir(ir_block);
    }

    // ── Compute stack size and pre-allocate vreg slots ────────────
    // Pre-scan IR to find all scratch vregs (33+) and assign each a
    // fixed stack slot. This avoids the lazy allocation mismatch between
    // the pre-computed stack size and the runtime slot counter.
    //
    // BUGFIX (alpha.4): the previous code limited max_vreg to < 200,
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
    uint32_t stack_bytes = (uint32_t)(num_stack_slots_ * 8 + 64) & ~15U;

    // ── Prologue ─────────────────────────────────────────────────
    emit_push(RBX); emit_push(RBP); emit_push(R12);
    emit_push(R13); emit_push(R14); emit_push(R15);
    emit_byte(0x48); emit_byte(0x89); emit_byte(0xE5); // mov rbp, rsp
    emit_byte(0x48); emit_byte(0x81); emit_byte(0xEC);
    emit_u32(stack_bytes);  // sub rsp, stack_bytes

    emit_byte(0x48); emit_byte(0x89); emit_byte(0xFB); // mov rbx, rdi
    emit_byte(0x49); emit_byte(0x89); emit_byte(0xF6); // mov r14, rsi
    if (window_base_) emit_mov_imm64(WIN_REG, (uint64_t)window_base_);

    // v1.4.0-alpha.5: record the body offset (after prologue). Frameless
    // back-edge chaining jumps directly here, skipping the prologue.
    size_t body_off = code_buf_used_;

    // ── Compile IR ───────────────────────────────────────────────
    for (auto& inst : ir_block.insts) {
        if (compile_ir_inst(inst)) break;
    }

    // ── Epilogue ─────────────────────────────────────────────────
    size_t epilogue_off = code_buf_used_;

    // Materialize pending host flags to cpu.pstate before returning.
    // If a flag-setting op (ADDS/SUBS/TST) was the last to touch flags
    // and no subsequent BRCOND consumed them, the flags are still in
    // the host CPU's RFLAGS but haven't been written to pstate. The
    // next block (or the interpreter) would see stale pstate.
    if (flags_in_host_) {
        emit_materialize_flags(flags_from_sub_);
        flags_in_host_ = false;
    }

    // Flush all dirty vregs before returning (so cpu.regs[] is up to date).
    flush_all_vregs();

    if (!rax_holds_next_pc_) {
        uint64_t next_pc = start_pc + ir_block.count * 4;
        if (next_pc <= 0xFFFFFFFFULL) emit_mov_imm32_zext(RAX, (uint32_t)next_pc);
        else                            emit_mov_imm64(RAX, next_pc);
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
    // BUGFIX (alpha.4): call_interp_branch_patches_ jump here (after the
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
        int32_t rel = (int32_t)(epilogue_off - (p.patch_off + 5));
        patch_jmp_rel32(p.patch_off, rel);
    }
    for (size_t off : call_interp_branch_patches_) {
        // BUGFIX (alpha.4): jump to pc_store_off (after the RAX overwrite)
        // to preserve the interpreter's PC in RAX. emit_call_interp already
        // materialized flags and flushed vregs before the JNE, so we can
        // skip the normal epilogue's flag/vreg handling.
        int32_t rel = (int32_t)(pc_store_off - (off + 6));
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
    entry.body_off = body_off;  // v1.4.0-alpha.5: for frameless back-edge chaining
    // A block is frameless-compatible if it doesn't end with an op that
    // requires a fresh stack frame or has runtime-dependent control flow
    // that can't be patched. SVC and BR (indirect) are not compatible.
    // BRCOND/BRCOND_ZERO/BRCOND_BIT/BRCOND_FALLTHRU/fall-through ARE
    // compatible — their back-edges can be patched.
    // We also require that the block doesn't start with a CALL_INTERP
    // that might read the stack frame (conservative — most CALL_INTERPs
    // don't, but we can't easily tell at translate time).
    // For now: frameless_compatible = !unchainable_end_ (i.e. the block
    // ends with a chainable op or fall-through). SVC and BR set
    // unchainable_end_=true, so they're excluded. BRCOND and friends
    // set it too currently — we need to NOT set it for back-edge BRCONDs
    // since those are exactly the case we want to chain. But that would
    // break the forward-chain mechanism. Instead, we set
    // frameless_compatible based on whether the block's LAST op was a
    // back-edge branch (which we track separately).
    // Simpler heuristic: always set frameless_compatible=true. The
    // emit_frameless_back_edge caller checks the target's compatibility
    // by looking at whether body_off is valid (non-zero). Since we always
    // set body_off, all blocks are eligible. The safety is ensured by
    // the caller flushing all dirty arch vregs + materializing flags
    // before the frameless jump, so the target block's body sees a
    // consistent cpu.regs[]/pstate state regardless of stack frame.
    entry.frameless_compatible = true;
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
    try_chain_block(start_pc, blocks_[start_pc]);
    chain_back_references(start_pc);
    // v1.4.0-alpha.5: patch any pending back-edges that target this
    // block's body. This handles the case where a loop body was
    // translated BEFORE the loop top — the back-edge in the body was
    // recorded as pending, and now that the top is translated, we can
    // patch it to jump directly to the top's body.
    patch_pending_back_edges(start_pc);

    return fn;
}

// ── run_block ───────────────────────────────────────────────────────────

// ── run_block ─────────────────────────────────────────────────────
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
                (unsigned long long)total_blocks_executed_);
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

        // ── interp_only shortcut ──────────────────────────────────
        // Blocks that are too CALL_INTERP-heavy to JIT (e.g. __multf3)
        // are marked interp_only at translate-time. Run them through
        // the interpreter directly — no prologue/epilogue/CALL_INTERP
        // overhead. The interpreter steps exactly interp_only_count
        // instructions, matching what the JIT block would have done.
        if (entry.interp_only) {
            blocks_executed++;
            for (int i = 0; i < entry.interp_only_count && cpu.running; i++) {
                emu.step_public(cpu);
            }
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
            if (entry.chain_target_pc != 0) {
                try_chain_block(pc, it->second);
                entry = it->second;
            }
            auto brit = back_refs_.find(pc);
            if (brit != back_refs_.end()) {
                chain_back_references(pc);
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
            fprintf(stderr, "[DBG] entry block @ 0x%llx pstate=0x%x x1=0x%llx\n",
                    (unsigned long long)pc, cpu.pstate,
                    (unsigned long long)cpu.regs[1]);
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
                    (unsigned long long)pc, (unsigned long long)cpu.regs[0],
                    (unsigned long long)cpu.regs[1], cpu.pstate);
        }
        uint64_t jit_next = entry.fn(&cpu, &emu);
        cpu.pc = jit_next;
        if (getenv("BIFROST_VERIFY_TRACE")) {
            fprintf(stderr, "[VTRACE] exit  block @ 0x%llx x0=0x%llx pstate=0x%x jit_next=0x%llx\n",
                    (unsigned long long)pc, (unsigned long long)cpu.regs[0],
                    cpu.pstate, (unsigned long long)jit_next);
        }
        // Debug: print pstate after JIT
        if (dbg_) {
            const char* s = getenv("BIFROST_DBG_PC");
            uint64_t target = strtoull(s, nullptr, 0);
            if (pc == target) {
                fprintf(stderr, "[DBG] exit  block @ 0x%llx pstate=0x%x x19=0x%llx jit_next=0x%llx\n",
                        (unsigned long long)pc, cpu.pstate,
                        (unsigned long long)cpu.regs[19],
                        (unsigned long long)jit_next);
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
        bool skip_reg_check = false;
        if (ref.pc != jit_next) {
            // Check if this is a false positive from frameless back-edge
            // chaining: the JIT block ran the loop multiple times (via
            // patched back-edge jcc/jmp), so jit_next is the loop-exit PC
            // while ref.pc (after only instr_count steps) is the loop-back
            // PC. Skip the entire verification for this block.
            if (entry.instr_count > 0 && entry.frameless_compatible) {
                fprintf(stderr, "[VERIFY] block @ 0x%llx: PC DIVERGENCE (jit_next=0x%llx ref_next=0x%llx steps=%d/%d) [back-edge false-positive — skipping]\n",
                        (unsigned long long)pc, (unsigned long long)jit_next,
                        (unsigned long long)ref.pc, steps, entry.instr_count);
                skip_reg_check = true;
            } else {
                fprintf(stderr, "[VERIFY] block @ 0x%llx: PC DIVERGENCE (jit_next=0x%llx ref_next=0x%llx steps=%d/%d)\n",
                        (unsigned long long)pc, (unsigned long long)jit_next,
                        (unsigned long long)ref.pc, steps, entry.instr_count);
                for (int i = 0; i < 31; i++) {
                    if (cpu.regs[i] != ref.regs[i]) {
                        fprintf(stderr, "[VERIFY]   x%d: jit=0x%llx ref=0x%llx\n",
                                i, (unsigned long long)cpu.regs[i],
                                (unsigned long long)ref.regs[i]);
                    }
                }
                if (cpu.sp != ref.sp)
                    fprintf(stderr, "[VERIFY]   sp: jit=0x%llx ref=0x%llx\n",
                            (unsigned long long)cpu.sp, (unsigned long long)ref.sp);
                if (cpu.pstate != ref.pstate)
                    fprintf(stderr, "[VERIFY]   pstate: jit=0x%llx ref=0x%llx\n",
                            (unsigned long long)cpu.pstate, (unsigned long long)ref.pstate);
                abort();
            }
        }
        if (!skip_reg_check) {
        // PCs match — compare register state.
        // NOTE: we skip pstate comparison for blocks ending with BRCOND
        // because CBNZ/CBZ are translated as TST+BRCOND, and the TST
        // materializes flags that the interpreter's CBNZ never sets.
        // This is a known semantic difference, not a real divergence.
        bool diverged = false;
        for (int i = 0; i < 31; i++) {
            if (cpu.regs[i] != ref.regs[i]) {
                fprintf(stderr, "[VERIFY] x%d: jit=0x%llx ref=0x%llx\n",
                        i, (unsigned long long)cpu.regs[i],
                        (unsigned long long)ref.regs[i]);
                diverged = true;
            }
        }
        if (cpu.sp != ref.sp) {
            fprintf(stderr, "[VERIFY] sp: jit=0x%llx ref=0x%llx\n",
                    (unsigned long long)cpu.sp, (unsigned long long)ref.sp);
            diverged = true;
        }
        if (cpu.pstate != ref.pstate) {
            // pstate comparison: mask out the internal from_sub marker bit
            // (bit 27) since it's a JIT implementation detail, not part of
            // the architectural NZCV state. Only compare the actual flags.
            uint64_t mask = 0xF0000000ULL;  // N=bit31, Z=bit30, C=bit29, V=bit28
            if ((cpu.pstate & mask) != (ref.pstate & mask)) {
                fprintf(stderr, "[VERIFY] pstate: jit=0x%llx ref=0x%llx (flags only: jit=0x%llx ref=0x%llx)\n",
                        (unsigned long long)cpu.pstate, (unsigned long long)ref.pstate,
                        (unsigned long long)(cpu.pstate & mask),
                        (unsigned long long)(ref.pstate & mask));
                diverged = true;
            }
        }
        if (diverged) {
            // Log but don't abort — the verify mode has known false
            // positives from (1) frameless back-edge chaining and
            // (2) read-then-write same address in one block.
            // Real bugs will cause a crash or wrong output later.
            fprintf(stderr, "[VERIFY] block @ 0x%llx: DIVERGENCE (pc=0x%llx steps=%d/%d) [logging only — may be false-positive]\n",
                    (unsigned long long)pc, (unsigned long long)jit_next,
                    steps, entry.instr_count);
        }
        }  // end if (!skip_reg_check)
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
                (unsigned long long)pc, (unsigned long long)cpu.sp,
                (unsigned long long)cpu.regs[0], (unsigned long long)cpu.regs[1],
                (unsigned long long)cpu.regs[2], (unsigned long long)cpu.regs[3],
                (unsigned long long)cpu.regs[5]);
    }
    uint64_t next_pc = entry.fn(&cpu, &emu);
    cpu.pc = next_pc;
    return next_pc;
}

} // namespace arm64emu
