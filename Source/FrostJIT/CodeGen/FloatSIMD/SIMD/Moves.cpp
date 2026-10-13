// SIMD data movement: DUP, UMOV/SMOV, saturating add/sub, MOVI, ORR/BIC immediate.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace arm64emu {
bool FrostJIT::compile_simd_moves_(const IRInst& inst) {
    switch (inst.op) {
        case IROp::SIMD_DUP: {
            // v_lo[dest] = v_hi[dest] = broadcast of src1's low element.
            // width = esize (1/2/4/8, default 8); flags_op = Q.
            // esize<8: mask the low element then shift-replicate it across
            // the qword in RAX (RCX = scratch); esize==8: full GPR value.
            const SimdDupParams dp = inst.dup_params();
            const int esize = dp.esize ? static_cast<int>(dp.esize) : 8;
            const bool q = dp.q;
            auto emit_dup_qword = [&]() {
                if (esize >= 8) return;
                clobber_host_reg(RCX);
                switch (esize) {
                case 1:  // keep low byte, replicate to 8 copies
                    emit_shift_imm8(RAX, 4, 56); emit_shift_imm8(RAX, 5, 56);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 8);  emit_or_reg(RAX, RCX);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 16); emit_or_reg(RAX, RCX);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 32); emit_or_reg(RAX, RCX);
                    break;
                case 2:  // keep low halfword, replicate to 4 copies
                    emit_shift_imm8(RAX, 4, 48); emit_shift_imm8(RAX, 5, 48);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 16); emit_or_reg(RAX, RCX);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 32); emit_or_reg(RAX, RCX);
                    break;
                case 4:  // keep low word, replicate to 2 copies
                    emit_shift_imm8(RAX, 4, 32); emit_shift_imm8(RAX, 5, 32);
                    emit_mov_reg(RCX, RAX); emit_shift_imm8(RAX, 4, 32); emit_or_reg(RAX, RCX);
                    break;
                }
            };
            // ── Vector cache fast path ──────────────
            // dest pinned: vmovq xd, gpr (zero upper) then vmovddup
            // (broadcast low qword to both halves) — no memory bounce.
            {
                int xd = vec_xmm(inst.dest);
                if (xd >= 0) {
                    int s = ensure_vreg(inst.src1, RAX);
                    if (s != RAX) {
                        clobber_host_reg(RAX);
                        emit_mov_reg(RAX, s);
                    } else if (esize < 8) {
                        // s == RAX: the shift-replicate chain below destroys
                        // src1's value in RAX while RAX is still mapped to
                        // src1. Drop the mapping (spilling if dirty) BEFORE
                        // the chain so later readers of src1 in this block
                        // reload from its home instead of the broadcast.
                        clobber_host_reg(RAX);
                    }
                    emit_dup_qword();
                    // vmovq xd, rax  (VEX.128.66.0F.W1 6E /r — pp=66, NOT F3:
                    // the F3.0F.W1 6E form is not a valid AVX encoding; the
                    // assembler emits 66.0F.W1 6E with the XMM dest in
                    // ModRM.reg and vvvv=1111 unused).
                    emit_vex3(1, true, 0, 1, xd, RAX, true, 0x6E);
                    // vmovddup xd, xd  (VEX.128.F2.0F.WIG 12: broadcast low qword)
                    if (q) emit_vex3(1, false, 0, 3, xd, xd, true, 0x12);
                    vec_cache_mark_dirty(static_cast<int>(inst.dest));
                    return true;
                }
            }
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
            } else if (esize < 8) {
                // s == RAX: the shift-replicate chain destroys src1's value
                // in RAX while RAX is still mapped to src1. Drop the mapping
                // (spilling if dirty) BEFORE the chain so later readers of
                // src1 reload from its home instead of the broadcast. (For
                // Q=0 the old code's trailing clobber masked this when src1
                // was clean, but a dirty src1 — or Q=1 — corrupted it.)
                clobber_host_reg(RAX);
            }
            emit_dup_qword();
            int32_t offlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_store(CPU_REG, offlo, RAX);
            if (q) {
                emit_store(CPU_REG, offhi, RAX);
            } else {
                clobber_host_reg(RAX);
                emit_mov_imm32_zext(RAX, 0);
                emit_store(CPU_REG, offhi, RAX);
            }
            // src1 stays cached in its original reg (s) for later readers.
            // RAX holds a copy (not a cached vreg) — no mapping to update.
            return true;
        }
        // ── SIMD UMOV (vector element -> GPR) ───────────────────────
        // regs[dest] = element[imm] of vector src1. width = esize
        // (1/2/4/8), imm = lane index, flags_op = Q (0=W d, 1=X d — the
        // codegen reads the full element regardless, mirroring the interp).
        // NOT in vec_cache_compatible_op, so src1 is never pinned: reading
        // cpu.v_lo/v_hi directly is always current (no stale-XMM hazard).
        // The zero-extending loads (emit_load32/16/8 are movzx) match the
        // interp's zero-extended element read for esize < 8; esize == 8
        // loads the full qword from v_lo or v_hi per the lane index.
        case IROp::SIMD_UMOV: {
            const SimdMovParams mp = inst.umov_params();
            const int esize = mp.esize ? static_cast<int>(mp.esize) : 8;
            const int index = static_cast<int>(mp.index);
            const int byte_off = index * esize;        // offset into the 16-byte vector
            const int qword = byte_off / 8;            // 0 -> v_lo, 1 -> v_hi
            const int32_t off = (qword == 0 ? V_LO_OFF : V_HI_OFF)
                              + static_cast<int>(inst.src1) * 8 + (byte_off % 8);
            int d = alloc_reg();
            switch (esize) {
                case 1:  emit_load8(d, CPU_REG, off);  break;
                case 2:  emit_load16(d, CPU_REG, off); break;
                case 4:  emit_load32(d, CPU_REG, off); break;
                default: emit_load(d, CPU_REG, off);   break;  // 8 bytes
            }
            set_vreg_reg(inst.dest, d);
            return true;
        }
        // ── SIMD SMOV (vector element -> GPR, SIGN-extended) ────────
        // Identical element addressing to UMOV (width = esize 1/2/4,
        // imm = lane index, flags_op = Q), followed by a left+arithmetic-
        // right shift pair so the sign bit propagates through the 64-bit
        // register. Q=1 writes Xd (full 64-bit sign extension). Q=0 writes
        // Wd: the low 32 bits hold the sign-extended element and the upper
        // 32 of Xd are ZERO (Dispatch.cpp truncates the sext to uint32_t),
        // so mask off the upper half with mov r32,r32. The .D form never
        // reaches here (table guard keeps it UNKNOWN -> interp ->
        // DecodeError).
        case IROp::SIMD_SMOV: {
            const SimdMovParams mp = inst.smov_params();
            const int esize = mp.esize ? static_cast<int>(mp.esize) : 4;
            const int index = static_cast<int>(mp.index);
            const int byte_off = index * esize;        // offset into the 16-byte vector
            const int qword = byte_off / 8;            // 0 -> v_lo, 1 -> v_hi
            const int32_t off = (qword == 0 ? V_LO_OFF : V_HI_OFF)
                              + static_cast<int>(inst.src1) * 8 + (byte_off % 8);
            int d = alloc_reg();
            switch (esize) {
                case 1:  emit_load8(d, CPU_REG, off);  break;
                case 2:  emit_load16(d, CPU_REG, off); break;
                default: emit_load32(d, CPU_REG, off); break;  // 4 bytes
            }
            clobber_flags();                           // shifts kill RFLAGS
            const int sh = 64 - esize * 8;             // 24 / 16 / 32
            emit_shift_imm8(d, 4, static_cast<uint8_t>(sh));  // shl
            emit_shift_imm8(d, 7, static_cast<uint8_t>(sh));  // sar
            if (!mp.q) {
                // Q=0 (Wd): zero the upper 32 bits (mov r32, r32).
                if (d >= 8) emit_byte(rex(false, d >= 8, false, d >= 8));
                emit_byte(0x89); emit_byte(modrm(3, d & 7, d & 7));
            }
            set_vreg_reg(inst.dest, d);
            return true;
        }
        // ── SIMD SATADDSUB (SQADD/UQADD/SQSUB/UQSUB, B/H lanes) ─────
        // imm = subop (0=SQADD,1=UQADD,2=SQSUB,3=UQSUB), width = esize
        // (1 or 2 ONLY — table guard `size < 2`), flags_op = Q. Lowered
        // with the EXACT SSE2 saturating instructions; 32/64-bit lanes
        // have no compact pre-AVX512 form and stay on the interpreter.
        // FPSR.QC comes from comparing with the wrapping result.
        case IROp::SIMD_SATADDSUB: {
            if (vec_cache_active_) { emit_call_interp(inst.arm_pc, false); return true; }
            const SimdSubopParams sp = inst.sataddsub_params();
            const int esize = static_cast<int>(sp.esize) == 2 ? 2 : 1;
            const bool q = sp.q;
            const int subop = static_cast<int>(sp.subop);
            // opcode[subop][esize]: PADDSB EC/W ED, PADDUSB DC/W DD,
            // PSUBSB E8/W E9, PSUBUSB D8/W D9
            static const uint8_t kOp[4][2] = {
                {0xEC, 0xED},  // SQADD
                {0xDC, 0xDD},  // UQADD
                {0xE8, 0xE9},  // SQSUB
                {0xD8, 0xD9},  // UQSUB
            };
            // Wrapping PADDB/W, PSUBB/W: a lane that differs from the
            // saturated result saturated, which sets FPSR.QC.
            static const uint8_t kWrap[2][2] = {{0xFC, 0xFD}, {0xF8, 0xF9}};
            clobber_flags();
            load_vec(0, static_cast<int>(inst.src1));
            load_vec(1, static_cast<int>(inst.src2));
            movdqa(2, 0);
            sse2_op(kWrap[(subop & 3) >= 2][esize == 2], 2, 1);  // xmm2 = wrapped
            sse2_op(kOp[subop & 3][esize == 2], 0, 1);  // xmm0 = xmm0 OP xmm1
            movdqa(1, 0);
            emit_simd_qc_unless_equal(2, 1, q);
            store_vec(0, static_cast<int>(inst.dest), q);
            return true;
        }
        // ── SIMD MOVI/MVNI (broadcast lane pattern, AdvSIMD modified imm) ──
        // v_lo[dest] = imm; v_hi[dest] = Q ? imm : 0. flags_op = Q.
        // One movabs + one vmovq (vmovq already zeroes the upper half for
        // Q=0); vmovddup broadcasts the pattern to both halves for Q=1.
        case IROp::SIMD_MOVI: {
            const SimdMoviParams mp = inst.movi_params();
            clobber_host_reg(RAX);
            emit_mov_imm64(RAX, mp.pattern);
            {
                int xd = vec_xmm(static_cast<int>(inst.dest));
                if (xd >= 0) {
                    emit_vex3(1, true, 0, 1, xd, RAX, true, 0x6E);  // vmovq xd, rax
                    if (mp.q)
                        emit_vex3(1, false, 0, 3, xd, xd, true, 0x12);  // vmovddup xd,xd
                    vec_cache_mark_dirty(static_cast<int>(inst.dest));
                    return true;
                }
            }
            int32_t mofflo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t moffhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_store(CPU_REG, mofflo, RAX);
            if (mp.q) {
                emit_store(CPU_REG, moffhi, RAX);
            } else {
                emit_mov_imm32_zext(RAX, 0);
                emit_store(CPU_REG, moffhi, RAX);
            }
            return true;
        }
        // ── SIMD ORR/BIC immediate (read-modify-write the destination) ──
        // v_lo[dest],v_hi[dest] = dest OR/BIC imm. cond = 0=ORR, 1=BIC;
        // flags_op = Q. ORR/BIC (vector, immediate) read Vd as their source,
        // exactly like the interpreter's apply_or_bic_u{32,16}.
        case IROp::SIMD_ORRIMM: {
            const SimdOrrImmParams op = inst.orrimm_params();
            clobber_flags();
            flush_invalidate_host_regs((1u << RAX) | (1u << RCX));
            clobber_host_reg(RAX);
            emit_mov_imm64(RAX, op.pattern);
            {
                int xd = vec_xmm(static_cast<int>(inst.dest));
                if (xd >= 0) {
                    // Pattern into scratch XMM0, then VEX logical with xd.
                    emit_vex3(1, true, 0, 1, 0, RAX, true, 0x6E);    // vmovq xmm0, rax
                    if (op.q)
                        emit_vex3(1, false, 0, 3, 0, 0, true, 0x12); // vmovddup xmm0,xmm0
                    if (op.invert) {
                        // BIC: xd = xd & ~imm = vpandn xd, xmm0(imm), xd
                        emit_vex3(1, false, 0, 1, xd, xd, true, 0xDF);
                    } else {
                        // ORR: xd = xd | imm
                        emit_vex3(1, false, xd, 1, xd, 0, true, 0xEB);
                    }
                    if (!op.q)
                        emit_vex3(1, false, 0, 2, xd, xd, true, 0x7E);  // vmovq xd,xd → zero upper (F3)
                    vec_cache_mark_dirty(static_cast<int>(inst.dest));
                    return true;
                }
            }
            // Memory path: GPR or/and against the pattern in RAX.
            int32_t olo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t ohi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            emit_load(RCX, CPU_REG, olo);
            if (op.invert) { emit_not_reg(RAX); emit_and_reg(RCX, RAX); }
            else           { emit_or_reg(RCX, RAX); }
            emit_store(CPU_REG, olo, RCX);
            if (op.q) {
                emit_mov_imm64(RAX, op.pattern);
                emit_load(RCX, CPU_REG, ohi);
                if (op.invert) { emit_not_reg(RAX); emit_and_reg(RCX, RAX); }
                else           { emit_or_reg(RCX, RAX); }
                emit_store(CPU_REG, ohi, RCX);
            } else {
                emit_mov_imm32_zext(RCX, 0);
                emit_store(CPU_REG, ohi, RCX);
            }
            return true;
        }
        // ── SIMD LDST (read/write v_lo/v_hi to/from vregs) ─────────
        default:
            return false;
    }
}
}  // namespace arm64emu
