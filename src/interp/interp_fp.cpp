// interp/interp_fp.cpp — FP/SIMD dispatcher (Emulator::execute_fp).
//
// Handles FMOV_VD1/FMOV_RVD1 and SIMD_LD1/SIMD_ST1 here and hands the
// rest to the group files:
//   SIMD_DP   → interp_simd_three.cpp, interp_simd_misc.cpp,
//               interp_simd_imm.cpp, interp_simd_arith.cpp (in that order:
//               encodings overlap, so the first group that returns true
//               wins, exactly as in the old single if-chain)
//   FP_SCALAR → interp_fp_scalar.cpp
// Shared helpers live in interp_fp_common.hpp. The interpreter stays
// hand-written and independent of the JIT tables (tools/opgen/), so
// interp-vs-JIT divergence remains detectable.
#include "interp/interp_fp_common.hpp"
namespace arm64emu {
// execute_fp — handle all FP/SIMD instruction classes.
//
// Called from Emulator::execute() for:
//   InstClass::FMOV_VD1, FMOV_RVD1,
//   InstClass::SIMD_LD1, SIMD_ST1,
//   InstClass::SIMD_DP,
//   InstClass::FP_SCALAR
//
// The signature mirrors execute() (inst + next_pc + cpu + d) for
// consistency, even though FP/SIMD cases don't write next_pc.
void Emulator::execute_fp(uint32_t inst, uint64_t& next_pc, CPU& cpu, const DecodedInst& d) {
    (void)next_pc;  // FP/SIMD cases never write next_pc
    (void)inst;     // == d.raw; the groups read it as op
    switch (d.cls) {
        // ── FMOV Vd.D[1], Rn / FMOV Rn, Vm.D[1] ──────────────────
        // Move 64-bit GPR to/from HIGH 64 bits of vector register.
        // Used by musl's 128-bit softfloat routines.
        case InstClass::FMOV_VD1:
            cpu.v_hi[d.rd] = cpu.regs[d.rn];
            return;
        case InstClass::FMOV_RVD1:
            if (d.rd != 31) cpu.regs[d.rd] = cpu.v_hi[d.rn];
            return;
        // ── SIMD load/store multiple structures (LD1/ST1) ─────────
        case InstClass::SIMD_LD1:
        case InstClass::SIMD_ST1: {
            bool Q = d.Q;
            int total_bytes = Q ? 16 : 8;
            uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
            // (e.g. LD1 {Vt.S}[idx]) loads/stores ONE element at a
            // specific lane index, not a whole register. The old code
            // treated it as multi-structure (reading simd_count whole
            // registers), silently corrupting memory for any guest
            // using single-structure LD1/ST1 (matrix transpose, RGBA
            // channel interleaving, etc.).
            //
            // Single-structure LD1/ST1 1-element variant encoding
            // (per ARM ARM C4.1.66). The decoder sets d.is_single_struct
            // from bit[24] (base 0x0D = single, 0x0C = multiple):
            //   bits[14:13] = size[1:0] (high bits of size)
            //   bits[11:10] = size[1:0] (low bits) — combined size is
            //                 bits[14:13]:[11:10] but for LD1 (1-reg)
            //                 the index comes from Q and size.
            // For LD1 (1-element, 1-register), the decode is:
            //   Q=0: size = bits[14:13]; index = bit[11] (or bits[12:11]
            //        depending on size); esize = 1<<size
            //   Q=1: size = bits[14:13]; index = bit[12]:bit[11]
            // We decode conservatively: extract the element size from
            // bits[14:13] (which is the standard size field for the
            // 1-reg single-structure form), and the index from the
            // remaining bits.
            if (d.is_single_struct) {
                // LDnR {Vt..Vt+n-1.T}, [Xn] — single-structure REPLICATE load
                // (bits[15:14]==11). Reads ONE element of 2^size bytes from
                // [Xn] and broadcasts it to every lane of Vt (Q=1: 16 bytes
                // across v_lo/v_hi. Post-index: Rm==31 → offset=nregs*esize,
                // Rm==30 → 0, else Xm. Load-only; there is no
                // store-replicate form. Distinguished from the indexed LD1/
                // ST1 forms below by d.is_ld1r.
                if (d.is_ld1r) {
                    int esize = 1 << d.size;
                    uint8_t buf[8] = {0};
                    if (d.is_load) {
                        for (int i = 0; i < d.simd_count; i++) {
                            int r = (d.rt + i) & 31;
                            mem_.read(base + (uint64_t)i * esize, buf, esize);
                            for (int off = 0; off < 8; off += esize)
                                memcpy(reinterpret_cast<uint8_t*>(&cpu.v_lo[r]) + off, buf, esize);
                            if (Q) {
                                for (int off = 0; off < 8; off += esize)
                                    memcpy(reinterpret_cast<uint8_t*>(&cpu.v_hi[r]) + off, buf, esize);
                            } else {
                                cpu.v_hi[r] = 0;
                            }
                        }
                    }
                    if (d.post_indexed) {
                        uint64_t off = 0;
                        if (d.rm == 31) off = (uint64_t)d.simd_count * esize;
                        else if (d.rm == 30) off = 0;
                        else off = cpu.regs[d.rm];
                        if (d.rn == 31) cpu.sp = base + off;
                        else cpu.regs[d.rn] = base + off;
                    }
                    return;
                }
                // ── Single-structure indexed LD1/ST1 decode (ARM ARM) ──
                // Layout (no-offset / post-index classes, bit[24]=1):
                //   bit30  = Q        (0→64-bit v_lo, 1→128-bit v_lo+v_hi)
                //   bits[15:13] = opcode; scale = opcode<2:1>
                //   bit12  = S
                //   bits[11:10] = size
                //   bits[9:5] = Rn, bits[4:0] = Rt
                // Shared decode (index encoding):
                //   scale '00' → B:  esize=1; idx = Q:S:size     (bits[30],[12],[11:10])
                //   scale '01' → H:  esize=2; idx = Q:S:size<1>  (bits[30],[12],[11])
                //   scale '10': if size<1>=='1' UNDEF
                //                size<0>=='0' → S: idx = Q:S    (bits[30],[12]), esize=4
                //                size<0>=='1' → D: idx = Q      (bit[30]),      esize=8
                //   scale '11' → replicate (LD1R), already handled above.
                uint32_t opcode = (d.raw >> 13) & 0x7;
                uint8_t scale   = (opcode >> 1) & 3;
                uint8_t Sbit    = (d.raw >> 12) & 1;
                uint8_t sz      = (d.raw >> 10) & 3;   // size, bits[11:10]
                int idx = 0;
                int esize = 0;
                switch (scale) {
                    case 0: // B
                        idx   = ((int)(Q & 1) << 3) | ((int)Sbit << 2) | (int)sz;
                        esize = 1;
                        break;
                    case 1: // H
                        idx   = ((int)(Q & 1) << 2) | ((int)Sbit << 1) | (int)((sz >> 1) & 1);
                        esize = 2;
                        break;
                    case 2: // S or D
                        if ((sz & 1) == 0) {        // size<0>==0 → S
                            idx   = ((int)(Q & 1) << 1) | (int)Sbit;
                            esize = 4;
                        } else {                    // size<0>==1 → D (S must be 0)
                            idx   = (int)(Q & 1);
                            esize = 8;
                        }
                        break;
                    default: // scale '11' → LD1R handled earlier; unreachable
                        idx   = 0;
                        esize = 1;
                        break;
                }
                for (int i = 0; i < d.simd_count; i++) {
                int r = (d.rt + i) & 31;
                uint64_t addr = base + (uint64_t)i * esize;
                if (d.is_load) {
                    // Load one element from memory into lane `idx`.
                    uint8_t buf[8];
                    mem_.read(addr, buf, esize);
                    if (esize == 1) {
                        // Write into byte `idx` of the V register.
                        uint8_t* vp = (idx < 8)
                            ? reinterpret_cast<uint8_t*>(&cpu.v_lo[r]) + idx
                            : reinterpret_cast<uint8_t*>(&cpu.v_hi[r]) + (idx - 8);
                        *vp = buf[0];
                    } else if (esize == 2) {
                        uint16_t* vp = (idx < 4)
                            ? reinterpret_cast<uint16_t*>(&cpu.v_lo[r]) + idx
                            : reinterpret_cast<uint16_t*>(&cpu.v_hi[r]) + (idx - 4);
                        memcpy(vp, buf, 2);
                    } else if (esize == 4) {
                        uint32_t* vp = (idx < 2)
                            ? reinterpret_cast<uint32_t*>(&cpu.v_lo[r]) + idx
                            : reinterpret_cast<uint32_t*>(&cpu.v_hi[r]) + (idx - 2);
                        memcpy(vp, buf, 4);
                    } else {  // esize == 8 (D)
                        uint64_t* vp = (idx == 0) ? &cpu.v_lo[r] : &cpu.v_hi[r];
                        memcpy(vp, buf, 8);
                    }
                } else {
                    // Store one element from lane `idx` to memory.
                    uint8_t buf[8] = {0};
                    if (esize == 1) {
                        const uint8_t* vp = (idx < 8)
                            ? reinterpret_cast<const uint8_t*>(&cpu.v_lo[r]) + idx
                            : reinterpret_cast<const uint8_t*>(&cpu.v_hi[r]) + (idx - 8);
                        buf[0] = *vp;
                    } else if (esize == 2) {
                        const uint16_t* vp = (idx < 4)
                            ? reinterpret_cast<const uint16_t*>(&cpu.v_lo[r]) + idx
                            : reinterpret_cast<const uint16_t*>(&cpu.v_hi[r]) + (idx - 4);
                        memcpy(buf, vp, 2);
                    } else if (esize == 4) {
                        const uint32_t* vp = (idx < 2)
                            ? reinterpret_cast<const uint32_t*>(&cpu.v_lo[r]) + idx
                            : reinterpret_cast<const uint32_t*>(&cpu.v_hi[r]) + (idx - 2);
                        memcpy(buf, vp, 4);
                    } else {
                        const uint64_t* vp = (idx == 0) ? &cpu.v_lo[r] : &cpu.v_hi[r];
                        memcpy(buf, vp, 8);
                    }
                    mem_.write(addr, buf, esize);
                }
                }
                // Post-index writeback for the single-structure form:
                // immediate offset = esize (Rm==0b11111), #0 (Rm==0b11110),
                // or the value in register Xm.
                if (d.post_indexed) {
                    uint64_t off = 0;
                    if (d.rm == 31) off = (uint64_t)d.simd_count * esize;
                    else if (d.rm == 30) off = 0;
                    else off = cpu.regs[d.rm];
                    if (d.rn == 31) cpu.sp = base + off;
                    else cpu.regs[d.rn] = base + off;
                }
                return;
            }
            // Multi-structure LD1/ST1 (original path) plus LD2/ST2/LD3/ST3/
            // LD4/ST4 (de-interleaved). d.simd_struct distinguishes them:
            //   1 = LD1/ST1 (registers stored consecutively, total_bytes each)
            //   2/3/4 = LD2/LD3/LD4 (registers interleaved element-wise:
            //           element e of register i sits at (e*nregs + i)*esize).
            int nregs = d.simd_count;
            int esize = 1 << d.size;
            int elems = total_bytes / esize;
            static bool ld_dbg_ = (getenv("BIFROST_LD2_DBG") != nullptr);
            auto reg_off = [&](int i, int e) -> uint64_t {
                if (d.simd_struct == 1)
                    return (uint64_t)i * total_bytes + (uint64_t)e * esize;
                return (uint64_t)(e * nregs + i) * esize;
            };
            for (int i = 0; i < nregs; i++) {
                int r = (d.rt + i) & 0x1F;
                uint8_t regbuf[16] = {0};
                if (d.is_load) {
                    uint8_t tmp[8];
                    for (int e = 0; e < elems; e++) {
                        mem_.read(base + reg_off(i, e), tmp, esize);
                        memcpy(regbuf + e * esize, tmp, esize);
                    }
                    if (ld_dbg_ && d.simd_count == 2 && d.simd_struct == 2) {
                        fprintf(stderr, "[LD2-DBG] base=0x%llx nregs=2 reg%d bytes:",
                                (unsigned long long)base, r);
                        for (int k = 0; k < 16; k++) fprintf(stderr, " %02x", regbuf[k]);
                        fprintf(stderr, "\n");
                    }
                    memcpy(&cpu.v_lo[r], regbuf, 8);
                    if (total_bytes == 16) memcpy(&cpu.v_hi[r], regbuf + 8, 8);
                    else cpu.v_hi[r] = 0;
                } else {
                    memcpy(regbuf, &cpu.v_lo[r], 8);
                    if (total_bytes == 16) memcpy(regbuf + 8, &cpu.v_hi[r], 8);
                    for (int e = 0; e < elems; e++)
                        mem_.write(base + reg_off(i, e), regbuf + e * esize, esize);
                }
            }
            // Post-index writeback for the multi-structure form: immediate
            // offset = nregs*total_bytes (Rm==0b11111), #0 (Rm==0b11110), or
            // the value in register Xm.
            if (d.post_indexed) {
                uint64_t off = 0;
                if (d.rm == 31) off = (uint64_t)nregs * (uint64_t)total_bytes;
                else if (d.rm == 30) off = 0;
                else off = cpu.regs[d.rm];
                if (d.rn == 31) cpu.sp = base + off;
                else cpu.regs[d.rn] = base + off;
            }
            return;
        }
        // ── SIMD data-processing (sub-dispatched by raw opcode bits) ──
        // The decoder classifies the entire 0x0E000000 / 0x4E000000 /
        // 0x2E000000 / 0x6E000000 group as SIMD_DP. We re-extract the
        // Q/U/size/opcode fields here and sub-dispatch on the exact
        // encoding pattern. This is a large but flat sub-dispatch —
        // migrating it to per-opcode InstClass values is a future
        // cleanup (it would balloon the enum).
        case InstClass::SIMD_DP: {
            const SimdDpFields f(d.raw);
            uint32_t op = f.op;
            bool Q = f.Q;
            // 1.5.4-alpha: ARMv8 Crypto Extensions (AES, SHA1, SHA256,
            // PMULL). These are checked first because their encodings
            // overlap with regular SIMD ops in the same major group
            // (bits[28:24]=0b01110) but have specific high-bit patterns
            // that aren't covered by the regular sub-dispatch.
            if (exec_crypto(op, cpu)) return;
            if (exec_reciprocal(op, cpu)) return;
            if (exec_simd_three(f, cpu)) return;
            if (exec_simd_misc(f, cpu)) return;
            if (exec_simd_imm(f, cpu)) return;
            if (exec_simd_arith(f, cpu)) return;
            // Fallback: any SIMD_DP op we don't model used to be silently
            // NOP'd — "incorrect but lets glibc continue", with wrong FP
            // results for any program that actually depends on the op.
            // Now we surface it as a DecodeError (→ SIGILL to the guest)
            // so missing SIMD coverage becomes a loud, fixable failure
            // instead of silent corruption. Log the opcode first for
            // debugging (also via BIFROST_SIMD_TRACE=1).
            if (dbg().simd_trace) {
                static uint64_t simd_unhandled_count_ = 0;
                if (simd_unhandled_count_ < 50) {
                    fprintf(stderr, "[SIMD] unhandled op=0x%08x pc=0x%llx (Q=%d U=%d size=%d)\n",
                            op, static_cast<unsigned long long>(cpu.pc),
                            Q, (op >> 29) & 1, (op >> 22) & 3);
                    simd_unhandled_count_++;
                }
            }
            // BIFROST_SIMD_COLLECT=1: enumerate-all mode. Instead of throwing,
            // log each distinct unhandled opcode once and NOP the instruction
            // so the guest keeps running. Produces the full list of missing
            // SIMD coverage in a single run instead of one SIGILL at a time.
            // TEMPORARY — removed after coverage sweep.
            if (dbg().simd_collect) {
                static std::set<uint32_t> simd_collected_;
                if (simd_collected_.insert(op).second)
                    fprintf(stderr, "[SIMD-COL] op=0x%08x pc=0x%llx (Q=%d U=%d size=%d)\n",
                            op, static_cast<unsigned long long>(cpu.pc),
                            Q, (op >> 29) & 1, (op >> 22) & 3);
                return;
            }
            dump_decode_error(cpu, mem_, op);
                throw DecodeError(cpu.pc, op);
            return;
        }
        // ── FP scalar (FMOV/FADD/FSUB/FMUL/FDIV/FCMP/FCVT/...) ────
        // The decoder classifies the entire 0x1E000000/0x9E000000
        // group as FP_SCALAR. We sub-dispatch on raw opcode bits.
        case InstClass::FP_SCALAR:
            exec_fp_scalar(d, cpu);
            return;
        default:
            // Not an FP/SIMD class — should never be called here.
            // The dispatcher in interpreter.cpp only routes FP/SIMD cases
            // to execute_fp(); reaching this default is a logic bug.
            break;
    }
}
} // namespace arm64emu
