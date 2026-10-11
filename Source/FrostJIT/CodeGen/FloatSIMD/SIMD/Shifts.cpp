// SIMD shift-by-immediate family (SHL/USHR/SSHR/USRA/SSRA/URSRA/SRSRA/SLI/SRI) and AES.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "System/Core/Emulator.hpp"
#include "FrostJIT/IR/IR.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace arm64emu {
bool FrostJIT::compile_simd_shifts_(const IRInst& inst) {
    switch (inst.op) {
        case IROp::SIMD_SHL:
        case IROp::SIMD_USHR:
        case IROp::SIMD_SSHR:
        case IROp::SIMD_USRA:
        case IROp::SIMD_SSRA:
        case IROp::SIMD_URSRA:
        case IROp::SIMD_SRSRA:
        case IROp::SIMD_SLI:
        case IROp::SIMD_SRI: {
            const SimdShiftParams sp = inst.shift_params();
            int esize = static_cast<int>(sp.esize);
            uint8_t shift = sp.shift;
            bool q = sp.q;
            // Fall back to CALL_INTERP for unsupported element sizes.
            //  - esize=1 (8-bit): no PSLLB/PSRLB/PSRAB in SSE2.
            //  - esize=8 SSHR/SSRA/SRSRA: 64-bit arithmetic shifts need
            //    PSRAQ, which is AVX-512F only (NOT SSE2/AVX2 — a plain
            //    `66 0F 73 /4 ib` = PSRAQ SIGILLs the host).
            //  - Invalid esize: shouldn't happen, but be safe.
            if (esize != 2 && esize != 4 && esize != 8) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            if ((inst.op == IROp::SIMD_SSHR || inst.op == IROp::SIMD_SSRA ||
                 inst.op == IROp::SIMD_SRSRA) && esize == 8) {
                emit_call_interp(inst.arm_pc, false);
                return true;
            }
            // Decode the shift group subop (0x71/0x72/0x73) plus the primary
            // reg field (6=PSLL, 2=PSRL, 4=PSRA) applied to Vn.
            uint8_t subop = 0;
            uint8_t reg_field = 0;
            uint8_t padd_op = 0;
            if (esize == 2)      { subop = 0x71; padd_op = 0xFD; }  // paddw
            else if (esize == 4) { subop = 0x72; padd_op = 0xFE; }  // paddd
            else                 { subop = 0x73; padd_op = 0xD4; }  // paddq (esize == 8)
            if (inst.op == IROp::SIMD_SHL || inst.op == IROp::SIMD_SLI) {
                reg_field = 6;   // PSLL (shift left)
            } else if (inst.op == IROp::SIMD_USHR ||
                       inst.op == IROp::SIMD_USRA ||
                       inst.op == IROp::SIMD_URSRA ||
                       inst.op == IROp::SIMD_SRI) {
                reg_field = 2;   // PSRL (logical shift right)
            } else {             // SIMD_SSHR / SIMD_SSRA / SIMD_SRSRA
                reg_field = 4;   // PSRA (arithmetic shift right)
            }
            // URSRA/SRSRA are rounding shift-accumulates: the rounded
            // right-shift RShr(x, s) = (x >> s) + ((x >> (s-1)) & 1),
            // i.e. add the top bit of the discarded low `s` bits. Native
            // pipeline: shifted = Vn >> s; round_bit = (Vn >> (s-1)) & mask;
            // Vd = Vd + shifted + round_bit. (No overflow: the rounding
            // carry never re-enters the shifted value.)
            bool is_rounding = (inst.op == IROp::SIMD_URSRA ||
                                inst.op == IROp::SIMD_SRSRA);
            uint8_t insert_shift = static_cast<uint8_t>(esize * 8) - shift;
            clobber_flags();
            // SIMD shifts use XMM regs only (PSRL/PSLL/PSRA pipeline plus
            // the rounding round-trip). The translator already routed
            // non-native cases to CALL_INTERP, which self-flushes. No GPR
            // flush needed.
            int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
            int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
            int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
            int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
            // movsd xmmN, [rbx+off]  (F2 0F 10 — load 64 bits, zero upper 64)
            auto emit_load64 = [&](int x, int32_t off) {
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x10);
                emit_modrm_disp(x, CPU_REG, off);
            };
            // movsd [rbx+off], xmmN  (F2 0F 11 — store low 64 bits)
            auto emit_store64 = [&](int x, int32_t off) {
                emit_byte(0xF2); emit_byte(0x0F); emit_byte(0x11);
                emit_modrm_disp(x, CPU_REG, off);
            };
            // 66 0F <subop> <modrm(3, rf, xmmN)> <imm8>  (PSLL/PSRL/PSRA)
            auto emit_shift_imm = [&](int x, uint8_t rf, uint8_t sh) {
                emit_byte(0x66); emit_byte(0x0F); emit_byte(subop);
                emit_byte(0xC0 | (rf << 3) | x);
                emit_byte(sh);
            };
            auto emit_padd = [&](int d, int s) {
                emit_byte(0x66); emit_byte(0x0F); emit_byte(padd_op);
                emit_byte(0xC0 | (d << 3) | s);
            };
            auto emit_por = [&](int d, int s) {
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEB);
                emit_byte(0xC0 | (d << 3) | s);
            };
            // VEX 3-byte prefix: C4 <b1> <b2> — mmmmm selects the opcode map
            // (00001=0F, 00011=0F3A), R~=X~=B~=1 (low regs), L=1 (256-bit).
            // `vvvv_nds` is the raw NDS register index; pass 0 for "unused"
            // (encodes 1111b, the required reserved value).
            auto emit_vex_prefix = [&](uint8_t mmmmm, int vvvv_nds, uint8_t pp) {
                emit_byte(0xC4);
                emit_byte(0xE0 | mmmmm);          // 0xE1 (0F) / 0xE3 (0F3A)
                emit_byte((((~vvvv_nds) & 0x0F) << 3) | 0x04 | pp);  // L=1
            };
            // VEX.256 shift-by-immediate: vpsll*/vpsrl*/vpsra* ymm<dest>, ymm<src>, imm8
            auto emit_vex_shift_imm = [&](int dest, int src, uint8_t rf, uint8_t sh) {
                emit_vex_prefix(0x01, src, 0x01);
                emit_byte(subop);
                emit_byte(0xC0 | (rf << 3) | dest);
                emit_byte(sh);
            };
            // VEX.256 3-operand: vpadd*/vpor ymm<dest>, ymm<nds>, ymm<src>
            auto emit_vex_3op = [&](uint8_t opcode, int dest, int nds, int src) {
                emit_vex_prefix(0x01, nds, 0x01);
                emit_byte(opcode);
                emit_byte(0xC0 | (dest << 3) | src);
            };
            // VEX.256.66.0F3A.W0 38 /r ib — vinserti128 ymm<dest>, ymm<nds>, xmm<src>, 1
            auto emit_vinserti128 = [&](int dest, int nds, int src) {
                emit_vex_prefix(0x03, nds, 0x01);
                emit_byte(0x38);
                emit_byte(0xC0 | (dest << 3) | src);
                emit_byte(0x01);
            };
            // VEX.256.66.0F3A.W0 39 /r ib — vextracti128 xmm<dest>, ymm<src>, 1
            // NOTE: for vextracti128 ModRM.reg encodes the SRC (ymm) and
            // ModRM.rm the DEST (xmm) — the reverse of vinserti128. vvvv
            // is reserved (1111b).
            auto emit_vextracti128 = [&](int dest, int src) {
                emit_vex_prefix(0x03, 0, 0x01);
                emit_byte(0x39);
                emit_byte(0xC0 | (src << 3) | dest);
                emit_byte(0x01);
            };
            // Pack two 64-bit halves into ymm<base>: bits[0:63] = lo,
            // bits[128:191] = hi (uses xmm<base> and xmm<base+1>).
            auto emit_pack_halves = [&](int base, int32_t offlo, int32_t offhi) {
                emit_load64(base, offlo);        // xmm<base>   = lo
                emit_load64(base + 1, offhi);    // xmm<base+1> = hi
                emit_vinserti128(base, base, base + 1);  // ymm<base> = {hi, lo}
            };
            // Store ymm0's low 128 (lo half) and extracted high 128 (hi half).
            auto emit_store_256 = [&](int32_t offlo, int32_t offhi) {
                emit_vextracti128(1, 0);   // xmm1 = bits[128:255] (hi half)
                emit_store64(1, offhi);
                emit_store64(0, offlo);
            };
            // ── SSE2 128-bit path — process one 64-bit half ────────────
            // CRITICAL: use 0xF2 (movsd, 64-bit) NOT 0xF3 (movss, 32-bit).
            // movss would load only the low 32 bits (lane 0) and zero lanes
            // 1-3, then the SSE2 shift would only shift lane 0, then movss
            // would store only lane 0 — corrupting lanes 1-3. We use 0xF2
            // here to be correct.
            auto emit_half_sse = [&](int32_t off1, int32_t offd) {
                if (inst.op == IROp::SIMD_SHL || inst.op == IROp::SIMD_USHR ||
                    inst.op == IROp::SIMD_SSHR) {
                    emit_load64(0, off1);
                    emit_shift_imm(0, reg_field, shift);
                    emit_store64(0, offd);
                } else if (inst.op == IROp::SIMD_USRA ||
                           inst.op == IROp::SIMD_SSRA) {
                    emit_load64(0, off1);
                    emit_shift_imm(0, reg_field, shift);
                    emit_load64(1, offd);
                    emit_padd(1, 0);            // xmm1 = Vd + shifted Vn
                    emit_store64(1, offd);
                } else if (is_rounding) {
                    // URSRA/SRSRA: Vd += round(Vn >> #shift).
                    // RShr(Vn, shift) = (Vn >> shift) + roundbit, where
                    // roundbit is the top discarded bit (bit shift-1 of Vn),
                    // i.e. 1 if set else 0. Isolate it per element with a
                    // logical shift + PSLL/PSRL round-trip (bit0 -> top ->
                    // bit0), which avoids any per-element mask constant.
                    emit_load64(0, off1);                     // xmm0 = Vn
                    emit_load64(2, off1);                     // xmm2 = Vn (copy)
                    emit_shift_imm(2, 2, shift - 1);          // xmm2 >> (shift-1)
                    emit_shift_imm(2, 6, static_cast<uint8_t>(esize * 8) - 1);  // bit0 -> top
                    emit_shift_imm(2, 2, static_cast<uint8_t>(esize * 8) - 1);  // top -> bit0
                    emit_shift_imm(0, reg_field, shift);      // xmm0 = Vn >> shift
                    emit_padd(0, 2);                          // xmm0 = shifted + roundbit
                    emit_load64(1, offd);                     // xmm1 = Vd
                    emit_padd(1, 0);                          // xmm1 = Vd + rounded
                    emit_store64(1, offd);
                } else {  // SIMD_SLI / SIMD_SRI
                    emit_load64(0, off1);
                    emit_shift_imm(0, reg_field, shift);
                    emit_load64(1, offd);
                    if (inst.op == IROp::SIMD_SLI) {
                        // Vd_keep = Vd & ((1<<shift)-1): shift left then right.
                        emit_shift_imm(1, 6, insert_shift);
                        emit_shift_imm(1, 2, insert_shift);
                    } else {
                        // Vd_keep = Vd & (top shift bits): shift right then left.
                        emit_shift_imm(1, 2, insert_shift);
                        emit_shift_imm(1, 6, insert_shift);
                    }
                    emit_por(1, 0);             // xmm1 = Vn_part | Vd_keep
                    emit_store64(1, offd);
                }
            };
            if (has_avx2() && q) {
                // ── AVX2 256-bit path (both halves in one instruction) ──
                if (inst.op == IROp::SIMD_SHL || inst.op == IROp::SIMD_USHR ||
                    inst.op == IROp::SIMD_SSHR) {
                    emit_pack_halves(0, off1lo, off1hi);       // ymm0 = Vn
                    emit_vex_shift_imm(0, 0, reg_field, shift);
                    emit_store_256(offdlo, offdhi);
                } else if (inst.op == IROp::SIMD_USRA ||
                           inst.op == IROp::SIMD_SSRA) {
                    emit_pack_halves(0, off1lo, off1hi);       // ymm0 = Vn
                    emit_vex_shift_imm(0, 0, reg_field, shift);
                    emit_pack_halves(2, offdlo, offdhi);       // ymm2 = Vd
                    emit_vex_3op(padd_op, 0, 2, 0);            // ymm0 = Vd + shifted
                    emit_store_256(offdlo, offdhi);
                } else if (is_rounding) {
                    emit_pack_halves(0, off1lo, off1hi);       // ymm0 = Vn
                    emit_pack_halves(1, off1lo, off1hi);       // ymm1 = Vn (copy)
                    emit_vex_shift_imm(1, 1, 2, shift - 1);    // ymm1 >> (shift-1)
                    emit_vex_shift_imm(1, 1, 6, static_cast<uint8_t>(esize * 8) - 1);  // bit0 -> top
                    emit_vex_shift_imm(1, 1, 2, static_cast<uint8_t>(esize * 8) - 1);  // top -> bit0
                    emit_vex_shift_imm(0, 0, reg_field, shift); // ymm0 = Vn >> shift
                    emit_vex_3op(padd_op, 0, 0, 1);            // ymm0 = shifted + roundbit
                    emit_pack_halves(2, offdlo, offdhi);       // ymm2 = Vd
                    emit_vex_3op(padd_op, 0, 2, 0);            // ymm0 = Vd + rounded
                    emit_store_256(offdlo, offdhi);
                } else {  // SIMD_SLI / SIMD_SRI
                    emit_pack_halves(0, off1lo, off1hi);       // ymm0 = Vn
                    emit_vex_shift_imm(0, 0, reg_field, shift);
                    emit_pack_halves(2, offdlo, offdhi);       // ymm2 = Vd
                    if (inst.op == IROp::SIMD_SLI) {
                        // Vd_keep = Vd & ((1<<shift)-1): shift left then right.
                        emit_vex_shift_imm(2, 2, 6, insert_shift);
                        emit_vex_shift_imm(2, 2, 2, insert_shift);
                    } else {
                        // Vd_keep = Vd & (top shift bits): shift right then left.
                        emit_vex_shift_imm(2, 2, 2, insert_shift);
                        emit_vex_shift_imm(2, 2, 6, insert_shift);
                    }
                    emit_vex_3op(0xEB, 0, 2, 0);               // ymm0 = parts|parts
                    emit_store_256(offdlo, offdhi);
                }
            } else {
                // ── SSE2 128-bit path (Q=1: both halves; Q=0: lo + zero hi) ──
                emit_half_sse(off1lo, offdlo);
                if (q) {
                    emit_half_sse(off1hi, offdhi);
                } else {
                    // Q=0: the IR contract zeros v_hi of the result.
                    emit_byte(0x66); emit_byte(0x0F); emit_byte(0xEF);
                    emit_byte(0xC0);                 // pxor xmm0, xmm0
                    emit_store64(0, offdhi);
                }
            }
            return true;
        }
        // ── 1.5.4-alpha: AES / PMULL native codegen ──────────────────
        // Uses AES-NI (aesenc/aesdec/aesimc/aesmc) and PCLMULQDQ
        // (pclmulqdq) when the host CPU supports them. Falls back to
        // CALL_INTERP on hosts without these extensions.
        //
        // The 128-bit V register is stored as v_lo (bits[63:0]) +
        // v_hi (bits[127:64]). We load both halves into XMM0 (low in
        // bits[63:0], high in bits[127:64]), perform the 128-bit op,
        // then store back.
        //
        // XMM register usage:
        //   XMM0 = state (src1) → result (dest)
        //   XMM1 = key (src2) for AES, or second operand for PMULL
        //
        // AES-NI instruction encodings (66 0F 38 DC/DE/DD/DF /r):
        //   AESE   = 66 0F 38 DC /r  (aesenc)
        //   AESD   = 66 0F 38 DE /r  (aesdec)
        //   AESMC  = 66 0F 38 DD /r  (aesimc — note: AESMC maps to aesimc,
        //                              AESIMC maps to aesmc; the x86 and
        //                              ARM names are swapped relative to
        //                              each other — see below)
        //   AESIMC = 66 0F 38 DF /r
        //
        // IMPORTANT: ARM and x86 AES instructions are NOT identical:
        //   ARM AESE  = AddRoundKey + SubBytes + ShiftRows
        //   x86 AESENC = SubBytes + ShiftRows + MixColumns + XOR roundkey
        // The x86 instruction includes MixColumns, which ARM's AESE does
        // NOT. ARM splits this into AESE (no MixColumns) + AESMC
        // (MixColumns). To emulate ARM AESE on x86, we would need to
        // use the AESENC instruction WITHOUT the MixColumns step, which
        // x86 doesn't expose directly.
        //
        // However, in practice, ARM code always pairs AESE+AESMC (the
        // ARM ARM shows them used together in every AES round). The
        // x86 AESENC instruction does both in one step. So we can't
        // directly map them 1:1.
        //
        // For correctness, we fall back to CALL_INTERP for AESE/AESD
        // (which uses the interpreter's software table-driven
        // implementation). We DO use AES-NI for AESMC/AESIMC via the
        // aesimc/aesmc x86 instructions... actually those also have
        // semantic differences.
        //
        // Given the semantic mismatch, the safest approach is to
        // emit CALL_INTERP for all AES ops. The native AES-NI path is
        // left as a future optimization that requires careful mapping
        // of ARM's split semantics to x86's combined semantics.
        //
        // For PMULL/PMULL2, the semantics ARE identical (carry-less
        // multiplication is the same on both architectures), so we
        // use PCLMULQDQ natively when available.
        case IROp::AES_CRYPTO: {
            uint8_t sub_op = inst.aes_params().subop;
            // PMULL/PMULL2 — use PCLMULQDQ when available.
            if ((sub_op == 4 || sub_op == 5) && has_pclmulqdq()) {
                clobber_flags();
                // PCLMULQDQ path is XMM-only (movq/pclmulqdq/psrldq); the
                // AESE/AESD fallback goes through emit_call_interp, which
                // self-flushes. No GPR flush needed.
                int32_t off1lo = V_LO_OFF + static_cast<int>(inst.src1) * 8;
                int32_t off1hi = V_HI_OFF + static_cast<int>(inst.src1) * 8;
                int32_t off2lo = V_LO_OFF + static_cast<int>(inst.src2) * 8;
                int32_t off2hi = V_HI_OFF + static_cast<int>(inst.src2) * 8;
                int32_t offdlo = V_LO_OFF + static_cast<int>(inst.dest) * 8;
                int32_t offdhi = V_HI_OFF + static_cast<int>(inst.dest) * 8;
                // Select the architectural source half before loading. MOVQ
                // zeroes the other half and needs only SSE2, avoiding PINSRQ/
                // PEXTRQ and their extra SSE4.1 and REX.W requirements.
                const int32_t source1 = sub_op == 4 ? off1lo : off1hi;
                const int32_t source2 = sub_op == 4 ? off2lo : off2hi;
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x7E);
                emit_modrm_disp(0, CPU_REG, source1);  // movq xmm0, m64
                emit_byte(0xF3); emit_byte(0x0F); emit_byte(0x7E);
                emit_modrm_disp(1, CPU_REG, source2);  // movq xmm1, m64
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x3A); emit_byte(0x44);
                emit_byte(0xC1); emit_byte(0x00);      // pclmulqdq xmm0,xmm1,0
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0xD6);
                emit_modrm_disp(0, CPU_REG, offdlo);   // movq m64, xmm0
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0x73);
                emit_byte(0xD8); emit_byte(0x08);      // psrldq xmm0,8
                emit_byte(0x66); emit_byte(0x0F); emit_byte(0xD6);
                emit_modrm_disp(0, CPU_REG, offdhi);
                return true;
            }
            // AESE/AESD/AESMC/AESIMC — fall back to CALL_INTERP.
            // (The ARM-vs-x86 semantic mismatch makes direct AES-NI
            // mapping incorrect. The interpreter's table-driven
            // implementation is correct.)
            emit_call_interp(inst.arm_pc, false);
            return true;
        }
        // ── SIMD 2-REG MISC (CNT/NOT/RBIT/ABS/NEG) — native SSE2 ─────
        // imm = subop (0=CNT, 1=NOT, 2=RBIT, 3=ABS, 4=NEG); width = esize
        // (CNT/NOT/RBIT are per-byte, ABS/NEG use the lane width);
        // flags_op = Q. Full 128-bit source; Q=0 zeroes v_hi.
        default:
            return false;
    }
}
}  // namespace arm64emu
