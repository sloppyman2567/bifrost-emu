// SIMD vector load/store codegen (LD1/ST1 single-register, 16-byte LD/ST).
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
extern "C" {
void jit_load_mem16_slow(arm64emu::Emulator* emu, arm64emu::CPU* cpu, uint64_t addr, int dst);
void jit_store_mem16_slow(arm64emu::Emulator* emu, arm64emu::CPU* cpu, uint64_t addr, int src);
}
namespace arm64emu {
bool FrostJIT::compile_simd_memory_(const IRInst& inst) {
    switch (inst.op) {
        case IROp::SIMD_LDST: {
            // width=1 (load): src1=lo vreg, src2=hi vreg → v_lo[dest], v_hi[dest]
            // width=0 (store): v_lo[dest] → src1 vreg, v_hi[dest] → src2 vreg
            if (inst.ldst_is_load()) {
                // Load: write vregs to v_lo/v_hi
                // use separate host regs for lo/hi so we
                // don't clobber src1's cached value when loading src2.
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
            return true;
        }
        // ── SIMD LD16/ST16 (16-byte guest memory access, 1..4 vregs) ──
        // One bounds-check + one movupd per 16 bytes (fast path), or one
        // call into jit_load_mem16_slow/jit_store_mem16_slow per reg
        // (slow path). flags_op = register count (1..4, default 1);
        // dest/src2 = FIRST vreg; vregs dest+0..count-1 are contiguous.
        // Replaces per-register 8-byte LOAD_MEM/STORE_MEM + SIMD_LDST
        // chains, which each did their own 10-byte mov imm64 limit + cmp +
        // jbe + add window.
        case IROp::SIMD_LD16: {
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            flush_invalidate_host_regs(MEM_CLOBBER);
            const Ld16Params lp = inst.ld16_params();
            uint32_t nregs = lp.count ? lp.count : 1;
            // rax = addr + imm
            load_vreg_to_reg(RAX, inst.src1);
            if (lp.offset != 0) emit_add_reg_imm(RAX, static_cast<int32_t>(lp.offset));
            size_t jmp_past = 0;
            if (!memory_fault_stub_) {
                // Limit check: addr + 16*nregs <= window size.
                int tmp = (RAX != RDX) ? RDX : RCX;
                emit_window_limit(tmp, 16ull * nregs);
                emit_cmp_reg(RAX, tmp);
                size_t jbe_patch = emit_jcc_rel32_placeholder(6);  // JBE
                // Slow path: N calls to jit_load_mem16_slow(emu, cpu, addr, dst).
                // NOTE: reload the base address each iteration — the C call
                // clobbers RAX (caller-saved), so a running "addr += 16" across
                // calls would add to garbage for i>=1.
                // Vec-cache guard: the C helper writes cpu.v_lo/v_hi[dest] and the
                // host call clobbers all XMM0-15 (caller-saved), so write back any
                // dirty pinned vectors first and reload them after — same round-trip
                // as emit_call_interp. CRITICAL: use writeback_all(FALSE) and emit
                // the reloads WITHOUT clearing vec_dirty_ — the slow-path code is
                // SKIPPED at runtime on the fast path, so clearing the flags here at
                // codegen time would suppress the epilogue writeback for vectors
                // dirtied earlier in the block (lost values → wrong branch → hang).
                if (vec_cache_active_) vec_cache_writeback_all(false);
                for (uint32_t i = 0; i < nregs; i++) {
                    load_vreg_to_reg(RAX, inst.src1);
                    if (lp.offset != 0 || i != 0) {
                        emit_add_reg_imm(RAX, static_cast<int32_t>(lp.offset + 16 * i));
                    }
                    emit_push(WIN_REG);
                    emit_load(RDI, RBP, emu_slot_off());
                    emit_mov_reg(RSI, CPU_REG);
                    emit_mov_reg(RDX, RAX);
                    emit_mov_imm32(RCX, (inst.dest + i) & 31);
                    emit_call_aligned(&jit_load_mem16_slow, /*num_pushed=*/1);
                    emit_pop(WIN_REG);
                }
                if (vec_cache_active_) {
                    for (int pi = 0; pi < vec_pinned_count_; pi++) {
                        int v = vec_pinned_[pi];
                        vec_emit_load_lo_hi(vec_cache_[v], v);
                    }
                }
                jmp_past = emit_jmp_rel32_placeholder();
                // Fast path: rax += window; per reg load the 16 bytes.
                // If dest+i is pinned in the vec cache, load straight into the
                // pinned XMM (one movupd) and mark it dirty for the epilogue
                // writeback. Otherwise movupd into scratch xmm0 then spill both
                // halves to v_lo/v_hi[dest+i].
                int32_t fast_rel = static_cast<int32_t>(code_buf_used_ - (jbe_patch + 6));
                patch_jcc_rel32(jbe_patch, fast_rel);
            }
            emit_add_reg(RAX, WIN_REG);
            for (uint32_t i = 0; i < nregs; i++) {
                int xdst = vec_xmm((inst.dest + i) & 31);
                if (xdst >= 0) {
                    // movupd xmmN, [rax+i*16] — 66 [REX.R] 0F 10 /r.
                    register_memory_fault_site();
                    emit_byte(0x66);
                    if (xdst >= 8) emit_byte(rex(false, true, false, false));
                    emit_byte(0x0F); emit_byte(0x10);
                    if (i == 0) {
                        emit_modrm_disp(xdst, 0, 0);
                    } else {
                        emit_modrm_disp(xdst, 0, static_cast<int32_t>(16 * i));
                    }
                    vec_cache_mark_dirty((inst.dest + i) & 31);
                } else {
                    // movupd xmm0, [rax+i*16]  — 66 0F 10 /r.
                    register_memory_fault_site();
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x10);
                    if (i == 0) {
                        emit_modrm_disp(0, 0, 0);
                    } else {
                        emit_modrm_disp(0, 0, static_cast<int32_t>(16 * i));
                    }
                    // movsd [rbx+v_lo[dest+i]], xmm0 — F2 0F 11 /r.
                    int32_t offlo = V_LO_OFF + ((inst.dest + i) & 31) * 8;
                    emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                    emit_modrm_disp(0, CPU_REG, offlo);
                    // movhpd [rbx+v_hi[dest+i]], xmm0 — 66 0F 17 /r.
                    int32_t offhi = V_HI_OFF + ((inst.dest + i) & 31) * 8;
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x17);
                    emit_modrm_disp(0, CPU_REG, offhi);
                }
            }
            if (jmp_past) {
                int32_t end_rel = static_cast<int32_t>(code_buf_used_ - (jmp_past + 5));
                patch_jmp_rel32(jmp_past, end_rel);
            }
            return true;
        }
        case IROp::SIMD_ST16: {
            clobber_flags();
            constexpr uint16_t MEM_CLOBBER =
                (1u << RAX) | (1u << RCX) | (1u << RDX) |
                (1u << R8)  | (1u << R9)  | (1u << R11);
            flush_invalidate_host_regs(MEM_CLOBBER);
            const St16Params sp = inst.st16_params();
            uint32_t nregs = sp.count ? sp.count : 1;
            // cond=1 → broadcast: every 16-byte half stores the SAME source
            // vector (`stp q0,q0` memset pattern), so src2 stays constant
            // across the loop instead of src2+i.
            bool broadcast = sp.broadcast;
            // rax = addr + imm
            load_vreg_to_reg(RAX, inst.src1);
            if (sp.offset != 0) emit_add_reg_imm(RAX, static_cast<int32_t>(sp.offset));
            size_t jmp_past = 0;
            if (!memory_fault_stub_) {
                // Limit check.
                int tmp = (RAX != RDX) ? RDX : RCX;
                emit_window_limit(tmp, 16ull * nregs);
                emit_cmp_reg(RAX, tmp);
                size_t jbe_patch = emit_jcc_rel32_placeholder(6);  // JBE
                // Slow path: N calls to jit_store_mem16_slow(emu, cpu, addr, src).
                // Reload the base address each iteration (the C call clobbers
                // RAX), same as the LD16 slow path above.
                // Vec-cache guard: the C helper reads cpu.v_lo/v_hi[src] and the
                // host call clobbers all XMM0-15 (caller-saved), so write back any
                // dirty pinned vectors first and reload them after — same round-trip
                // as emit_call_interp. CRITICAL: use writeback_all(FALSE) and emit
                // the reloads WITHOUT clearing vec_dirty_ — the slow-path code is
                // SKIPPED at runtime on the fast path, so clearing the flags here at
                // codegen time would suppress the epilogue writeback for vectors
                // dirtied earlier in the block (lost values → wrong branch → hang).
                if (vec_cache_active_) vec_cache_writeback_all(false);
                for (uint32_t i = 0; i < nregs; i++) {
                    load_vreg_to_reg(RAX, inst.src1);
                    if (sp.offset != 0 || i != 0) {
                        emit_add_reg_imm(RAX, static_cast<int32_t>(sp.offset + 16 * i));
                    }
                    emit_push(WIN_REG);
                    emit_load(RDI, RBP, emu_slot_off());
                    emit_mov_reg(RSI, CPU_REG);
                    emit_mov_reg(RDX, RAX);
                    emit_mov_imm32(RCX, broadcast ? (inst.src2 & 31) : ((inst.src2 + i) & 31));
                    emit_call_aligned(&jit_store_mem16_slow, /*num_pushed=*/1);
                    emit_pop(WIN_REG);
                }
                if (vec_cache_active_) {
                    for (int pi = 0; pi < vec_pinned_count_; pi++) {
                        int v = vec_pinned_[pi];
                        vec_emit_load_lo_hi(vec_cache_[v], v);
                    }
                }
                jmp_past = emit_jmp_rel32_placeholder();
                // Fast path: rax += window; per reg store the source vector.
                // If src2+i is pinned in the vec cache, store straight from the
                // pinned XMM (one movupd, no cpu.v_lo/v_hi reload) — the memset
                // loop's loop-invariant q0 stays resident across iterations.
                // Otherwise build xmm0 = {v_lo[src], v_hi[src]} then store.
                int32_t fast_rel = static_cast<int32_t>(code_buf_used_ - (jbe_patch + 6));
                patch_jcc_rel32(jbe_patch, fast_rel);
            }
            emit_add_reg(RAX, WIN_REG);
            for (uint32_t i = 0; i < nregs; i++) {
                int xsrc = vec_xmm(broadcast ? (inst.src2 & 31) : ((inst.src2 + i) & 31));
                if (xsrc >= 0) {
                    // movupd [rax+i*16], xmmN — 66 [REX.R] 0F 11 /r.
                    register_memory_fault_site();
                    emit_byte(0x66);
                    if (xsrc >= 8) emit_byte(rex(false, true, false, false));
                    emit_byte(0x0F); emit_byte(0x11);
                    if (i == 0) {
                        emit_modrm_disp(xsrc, 0, 0);
                    } else {
                        emit_modrm_disp(xsrc, 0, static_cast<int32_t>(16 * i));
                    }
                } else {
                    // movsd xmm0, [rbx+v_lo[src]] — F2 0F 10 /r.
                    int32_t s_lo = V_LO_OFF + (broadcast ? (inst.src2 & 31) : ((inst.src2 + i) & 31)) * 8;
                    emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                    emit_modrm_disp(0, CPU_REG, s_lo);
                    // movhpd xmm0, [rbx+v_hi[src]] — 66 0F 16 /r.
                    int32_t s_hi = V_HI_OFF + (broadcast ? (inst.src2 & 31) : ((inst.src2 + i) & 31)) * 8;
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x16);
                    emit_modrm_disp(0, CPU_REG, s_hi);
                    // movupd [rax+i*16], xmm0 — 66 0F 11 /r.
                    register_memory_fault_site();
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0x11);
                    if (i == 0) {
                        emit_modrm_disp(0, 0, 0);
                    } else {
                        emit_modrm_disp(0, 0, static_cast<int32_t>(16 * i));
                    }
                }
            }
            if (jmp_past) {
                int32_t end_rel = static_cast<int32_t>(code_buf_used_ - (jmp_past + 5));
                patch_jmp_rel32(jmp_past, end_rel);
            }
            return true;
        }
        // ── SIMD SHL/USHR/SSHR/USRA/SSRA/SLI/SRI (vector, by immediate) ──
        // native SSE2 codegen via psllw/pslld/psllq (SHL),
        // psrlw/psrld/psrlq (USHR), psraw/psrad (SSHR).
        // extended to USRA/SSRA/SLI/SRI (accumulate / insert-merge) and
        // added an AVX2 256-bit VEX path on capable hosts. Previously
        // these all fell back to CALL_INTERP (~20% overhead on SIMD-heavy
        // workloads).
        //
        // SSE2 shift-by-immediate encoding (66 0F <subop> <modrm> <imm8>):
        //   PSLLW xmmN, imm8 : 66 0F 71 F0|N  imm8    (reg field = 6)
        //   PSLLD xmmN, imm8 : 66 0F 72 F0|N  imm8
        //   PSLLQ xmmN, imm8 : 66 0F 73 F0|N  imm8
        //   PSRLW xmmN, imm8 : 66 0F 71 D0|N  imm8    (reg field = 2)
        //   PSRLD xmmN, imm8 : 66 0F 72 D0|N  imm8
        //   PSRLQ xmmN, imm8 : 66 0F 73 D0|N  imm8
        //   PSRAW xmmN, imm8 : 66 0F 71 E0|N  imm8    (reg field = 4)
        //   PSRAD xmmN, imm8 : 66 0F 72 E0|N  imm8
        //
        // Per-op semantic mapping (Vn = src1, Vd = dest accumulator):
        //   SHL   Vd = Vn << shift                          (PSLL)
        //   USHR  Vd = Vn >> shift   (logical)              (PSRL)
        //   SSHR  Vd = Vn >> shift   (arithmetic)           (PSRA)
        //   USRA  Vd = Vd + (Vn >> shift)                   (PSRL + PADD)
        //   SSRA  Vd = Vd + (Vn >> shift)  (arithmetic)     (PSRA + PADD)
        //   SLI   Vd = (Vn << shift) | (Vd & ((1<<shift)-1))
        //                                                    (PSLL + PSLL/PSRL + POR)
        //   SRI   Vd = (Vn >> shift) | (Vd & ~((1<<(esize*8-shift))-1))
        //                                                    (PSRL + PSRL/PSLL + POR)
        //
        // SLI/SRI are shift-INSERT ops: the source is shifted and OR'd with
        // the UNCHANGED destination bits in the vacated positions. Per the
        // ARM ARM pseudocode (result = (Vd AND NOT(mask)) OR shifted), SLI
        // retains Vd's low `shift` bits and SRI retains Vd's top `shift`
        // bits. A single shift of Vd by (esize*8-shift) would read the WRONG
        // half of Vd, so the retained half is produced by shifting Vd left
        // and right by (esize*8-shift) in sequence (which isolates the kept
        // bits in place).
        //
        // The modrm byte is 11_<reg>_<rm> where <rm> selects the xmmN.
        // PSLL/PSRL/PSRA do NOT have a PSLLB/PSRLB/PSRAB form in SSE2
        // (8-bit element shifts); we fall back to CALL_INTERP for esize=1.
        // 64-bit SSRA (psraq) requires AVX-512 — we fall back for esize=8.
        //
        // x86 shift semantics match ARM for shift ∈ [0, esize*8]:
        //   - shift=0: no-op (both)
        //   - shift=esize*8: SHL/USHR clear the lane; SSHR sign-fills it
        //     (immediate shifts with count ≥ element size saturate).
        // The IR translator guarantees shift ∈ [0, esize*8].
        //
        // Q bit (flags_op): 1 = 128-bit (process v_lo AND v_hi), 0 = 64-bit
        // (process v_lo only, ZERO v_hi of the result). On AVX2 hosts we
        // process both halves in one 256-bit VEX instruction: v_lo in
        // bits[0:63], v_hi in bits[128:191] (packed via vinserti128), so the
        // guest lane numbering maps directly onto 256-bit lanes and both
        // halves shift together. VEX 3-byte prefix format:
        //   C4 [R~ X~ B~ mmmmm] [W vvvv~ L pp] <opcode> <modrm>
        // with mmmmm=00001 (0F map) → byte1 0xE1, mmmmm=00011 (0F3A) → 0xE3,
        // L=1 (256-bit), pp=01 (66 prefix). vvvv~ is the inverted 4-bit
        // NDS register; 1111b when unused.
        default:
            return false;
    }
}
}  // namespace arm64emu
