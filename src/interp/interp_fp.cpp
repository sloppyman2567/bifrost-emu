// interp/interp_fp.cpp — FP/SIMD instruction handlers, extracted from
// interpreter.cpp.
//
// v1.4.5-alpha refactor: split out of interpreter.cpp. This file holds the
// FMOV_VD1, FMOV_RVD1, SIMD_LD1, SIMD_ST1, SIMD_DP, and FP_SCALAR cases of
// the dispatch switch, extracted into a member function (execute_fp) for
// readability. The main switch in interpreter.cpp dispatches to this method.
//
// No behavior change — pure file split. The method is a member of Emulator
// (declared in src/core/emulator.h) so it has full access to mem_, etc.
// The FP register access helpers (read_fp_d, read_fp_s, write_fp_d,
// write_fp_s, h2f, f2h, d2h) live at file scope here, mirroring the layout
// the interpreter had before the split.
#include "core/emulator.h"
#include "decoder.hpp"
#include "interp/interp_crypto.hpp"
#include "opgen_fpfixed.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <set>
namespace arm64emu {
// ── FP register access helpers (file-scope, no per-dispatch allocation) ──
// These were previously local lambdas inside the FP_SCALAR case, which
// meant they were reconstructed on every FP instruction dispatch. Moving
// them to file scope eliminates that overhead.
static inline double read_fp_d(const CPU& cpu, int r) {
    if (r < 0 || r > 31) r = 0;  // defensive: prevent OOB access
    uint64_t bits = cpu.v_lo[r];
    double d; memcpy(&d, &bits, 8); return d;
}
static inline float read_fp_s(const CPU& cpu, int r) {
    if (r < 0 || r > 31) r = 0;  // defensive: prevent OOB access
    uint32_t bits = static_cast<uint32_t>(cpu.v_lo[r]);
    float f; memcpy(&f, &bits, 4); return f;
}
static inline void write_fp_d(CPU& cpu, int r, double d) {
    if (r < 0 || r > 31) return;  // defensive: prevent OOB write
    static bool nan_dbg_ = (getenv("BIFROST_NAN_TRACE") != nullptr);
    if (nan_dbg_ && std::isnan(d))
        fprintf(stderr, "[NAN-D] r%d = %.17g pc=0x%llx\n", r, d, (unsigned long long)cpu.pc);
    uint64_t bits; memcpy(&bits, &d, 8);
    cpu.v_lo[r] = bits; cpu.v_hi[r] = 0;
}
static inline void write_fp_s(CPU& cpu, int r, float f) {
    if (r < 0 || r > 31) return;  // defensive: prevent OOB write
    static bool nan_dbg_ = (getenv("BIFROST_NAN_TRACE") != nullptr);
    if (nan_dbg_ && std::isnan(f))
        fprintf(stderr, "[NAN-S] r%d = %.9g pc=0x%llx\n", r, f, (unsigned long long)cpu.pc);
    uint32_t bits; memcpy(&bits, &f, 4);
    cpu.v_lo[r] = bits; cpu.v_hi[r] = 0;
}
// ── Half-precision (FP16) helpers ──────────────────────────────────────
// IEEE 754 binary16: 1 sign + 5 exp + 10 mantissa.
static inline float h2f(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t fbits;
    if (exp == 0) {
        if (mant == 0) {
            fbits = sign << 31;
        } else {
            int e = -1;
            while (!(mant & 0x400)) { mant <<= 1; e--; }
            mant &= 0x3FF;
            fbits = (sign << 31) | ((127 + e - 14) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        fbits = (sign << 31) | (0xFFu << 23) | (mant << 13);
    } else {
        fbits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f; memcpy(&f, &fbits, 4); return f;
}
static inline uint16_t f2h(float f) {
    uint32_t fbits; memcpy(&fbits, &f, 4);
    uint32_t sign = (fbits >> 31) & 1;
    int32_t  exp  = static_cast<int32_t>((fbits >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = (fbits & 0x7FFFFF) >> 13;
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign << 15);
        mant |= 0x400;
        mant >>= (1 - exp);
        return static_cast<uint16_t>((sign << 15) | mant);
    } else if (exp >= 0x1F) {
        return static_cast<uint16_t>((sign << 15) | (0x1F << 10));
    }
    return static_cast<uint16_t>((sign << 15) | (exp << 10) | mant);
}
static inline uint16_t d2h(double d) {
    return f2h(static_cast<float>(d));
}
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
    auto* pcache = &cpu.page_cache;
    (void)next_pc;  // FP/SIMD cases never write next_pc
    switch (d.cls) {
        // ── FMOV Vd.D[1], Rn / FMOV Rn, Vm.D[1] ──────────────────
        // Move 64-bit GPR to/from HIGH 64 bits of vector register.
        // Used by musl's 128-bit softfloat routines.
        case InstClass::FMOV_VD1:
            cpu.v_hi[d.rd] = cpu.regs[d.rn];
            return;
        case InstClass::FMOV_RVD1:
            cpu.regs[d.rd] = cpu.v_hi[d.rn];
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
                // LD1R {Vt.T}, [Xn] — single-structure REPLICATE load
                // (bits[15:14]==11). Reads ONE element of 2^size bytes from
                // [Xn] and broadcasts it to every lane of Vt (Q=1: 16 bytes
                // across v_lo/v_hi; Q=0: 8 bytes in v_lo). Post-index: Rm==31
                // → offset=esize, Rm==30 → 0, else Xm. Load-only; there is no
                // store-replicate form. Distinguished from the indexed LD1/
                // ST1 forms below by d.is_ld1r.
                if (d.is_ld1r) {
                    int esize = 1 << d.size;
                    uint8_t buf[8] = {0};
                    int r = d.rt;
                    if (d.is_load) {
                        mem_.read(base, buf, esize, pcache);
                        for (int off = 0; off < 8; off += esize)
                            memcpy(reinterpret_cast<uint8_t*>(&cpu.v_lo[r]) + off, buf, esize);
                        if (Q) {
                            for (int off = 0; off < 8; off += esize)
                                memcpy(reinterpret_cast<uint8_t*>(&cpu.v_hi[r]) + off, buf, esize);
                        } else {
                            cpu.v_hi[r] = 0;
                        }
                    }
                    if (d.post_indexed) {
                        uint64_t off = 0;
                        if (d.rm == 31) off = (uint64_t)esize;
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
                        idx   = ((int)(Q & 1) << 2) | ((int)Sbit << 1) | (int)(sz >> 1);
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
                int r = d.rt;
                if (d.is_load) {
                    // Load one element from memory into lane `idx`.
                    uint8_t buf[8];
                    mem_.read(base, buf, esize, pcache);
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
                    mem_.write(base, buf, esize, pcache);
                }
                // Post-index writeback for the single-structure form:
                // immediate offset = esize (Rm==0b11111), #0 (Rm==0b11110),
                // or the value in register Xm.
                if (d.post_indexed) {
                    uint64_t off = 0;
                    if (d.rm == 31) off = (uint64_t)esize;
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
                        mem_.read(base + reg_off(i, e), tmp, esize, pcache);
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
                        mem_.write(base + reg_off(i, e), regbuf + e * esize, esize, pcache);
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
            uint32_t op = d.raw;
            bool Q = (op >> 30) & 1;
            bool U = (op >> 29) & 1;
            uint8_t size = (op >> 22) & 3;
            uint8_t rm = (op >> 16) & 0x1F;
            uint8_t rn = (op >> 5) & 0x1F;
            uint8_t rd = op & 0x1F;
            (void)U;
            // v1.5.0.alpha: ARMv8 Crypto Extensions (AES, SHA1, SHA256,
            // PMULL). These are checked first because their encodings
            // overlap with regular SIMD ops in the same major group
            // (bits[28:24]=0b01110) but have specific high-bit patterns
            // that aren't covered by the regular sub-dispatch.
            if (exec_crypto(op, cpu)) return;
            // Sub-discriminator: bits[15:10] select the SIMD DP operation.
            // We mask off the Q bit (30) so both Q=0 (8-byte) and Q=1
            // (16-byte) forms route to the same handler. The Q bit is
            // passed separately to each handler via the `Q` variable.
            //
            // IMPROVEMENT over v0: v0 used flat if-chains with masks
            // that sometimes included bit 30 (Q), causing Q=1 forms
            // of DUP, INS, ORR(MOV), and EXT to be silently NOP'd.
            // This broke musl's 128-bit long-double softfloat, which
            // uses `mov v1.16b, v0.16b` to copy 128-bit values.
            uint32_t sub = op & 0xFFE0FC00;  // bits[31:24] + bits[20:10]
            // Strip Q from sub for matching purposes
            uint32_t sub_noq = sub & ~(1u << 30);
            switch (sub_noq) {
            // ── DUP (general): sf 0 0 11110 00 0 imm5 0000 0 1 Rn Rd ──
            // v0 only matched Q=0 (mask 0xFFE0FC00 val 0x0E000C00).
            // Fixed: strip Q, match both forms.
            case 0x0E000C00: {
                uint8_t imm5 = (op >> 16) & 0x1F;
                int esize;
                switch (imm5) {
                    case 0x01: esize = 1; break;
                    case 0x02: esize = 2; break;
                    case 0x04: esize = 4; break;
                    case 0x08: esize = 8; break;
                    default: throw DecodeError(cpu.pc, inst);
                }
                int elems = (Q ? 16 : 8) / esize;
                uint64_t src = cpu.regs[rn];
                uint8_t bytes[16] = {0};
                for (int i = 0; i < elems; i++) {
                    uint64_t v = src;
                    if (esize == 1) v = src & 0xFF;
                    else if (esize == 2) v = src & 0xFFFF;
                    else if (esize == 4) v = src & 0xFFFFFFFF;
                    memcpy(bytes + i * esize, &v, esize);
                }
                memcpy(&cpu.v_lo[rd], bytes, 8);
                if (Q) memcpy(&cpu.v_hi[rd], bytes + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── DUP (element): sf 0 0 11110 1 0 imm5 0000 1 1 Rn Rd ──
            // Copies one element from Vn to all lanes of Vd.
            // Encoding base: 0x4E000400 (Q=1) / 0x0E000400 (Q=0).
            // default and got silently NOP'd. This broke the vectorized
            // TLS init pattern `dup vN.2d, vM.d[0]` used by GCC -O2 to
            // broadcast a base value before adding an index vector,
            // causing multi-element __thread TLS arrays to get corrupted
            // (only lane 0 was correct, lanes 1+ stayed 0 or stale).
            case 0x0E000400: {
                uint8_t imm5 = (op >> 16) & 0x1F;
                int esize, idx;
                switch (imm5 & 0x1F) {
                    case 0x01: esize = 1; idx = imm5 >> 1; break;
                    case 0x02: esize = 2; idx = imm5 >> 2; break;
                    case 0x04: esize = 4; idx = imm5 >> 3; break;
                    case 0x08: esize = 8; idx = imm5 >> 4; break;
                    default: throw DecodeError(cpu.pc, inst);
                }
                // Read the source element from Vn.
                uint64_t src_val;
                int elems_per_qword = 8 / esize;
                if (idx < elems_per_qword) {
                    // Source is in v_lo[rn]
                    const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_lo[rn]);
                    memcpy(&src_val, p + idx * esize, esize);
                } else {
                    // Source is in v_hi[rn] (Q must be 1)
                    const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_hi[rn]);
                    memcpy(&src_val, p + (idx - elems_per_qword) * esize, esize);
                }
                // Broadcast src_val to all lanes of Vd.
                int elems = (Q ? 16 : 8) / esize;
                uint8_t bytes[16] = {0};
                for (int i = 0; i < elems; i++) {
                    memcpy(bytes + i * esize, &src_val, esize);
                }
                memcpy(&cpu.v_lo[rd], bytes, 8);
                if (Q) memcpy(&cpu.v_hi[rd], bytes + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── INS (general): sf 0 0 11110 10 0 imm5 0000 0 1 Rn Rd ──
            // v0 case label 0x4E000C00 was unreachable (it's DUP with
            // Q=1, which strips to the same sub_noq as DUP Q=0).
            // Real INS has bits[23:22]=10, giving sub_noq=0x0E001C00.
            case 0x0E001C00: {
                uint8_t imm5 = (op >> 16) & 0x1F;
                int esize = 0, idx = 0;
                for (int b = 0; b < 5; b++) {
                    if (imm5 & (1 << b)) { esize = 1 << b; break; }
                }
                idx = imm5 >> (esize == 1 ? 1 : (esize == 2 ? 2 : (esize == 4 ? 3 : 4)));
                uint64_t src = cpu.regs[rn];
                // (8/esize) must write to v_hi, not v_lo. The old code
                // always wrote to v_lo, causing out-of-bounds writes for
                // lane indices >= 2 (32-bit) or >= 1 (64-bit). This broke
                // INS used by MD5 to load message words into vector lanes.
                int elems_per_qword = 8 / esize;
                if (esize == 1) {
                    if (idx < elems_per_qword)
                        reinterpret_cast<uint8_t*>(&cpu.v_lo[rd])[idx] = src & 0xFF;
                    else if (Q)
                        reinterpret_cast<uint8_t*>(&cpu.v_hi[rd])[idx - elems_per_qword] = src & 0xFF;
                } else if (esize == 2) {
                    if (idx < elems_per_qword)
                        reinterpret_cast<uint16_t*>(&cpu.v_lo[rd])[idx] = src & 0xFFFF;
                    else if (Q)
                        reinterpret_cast<uint16_t*>(&cpu.v_hi[rd])[idx - elems_per_qword] = src & 0xFFFF;
                } else if (esize == 4) {
                    if (idx < elems_per_qword)
                        reinterpret_cast<uint32_t*>(&cpu.v_lo[rd])[idx] = src & 0xFFFFFFFF;
                    else if (Q)
                        reinterpret_cast<uint32_t*>(&cpu.v_hi[rd])[idx - elems_per_qword] = src & 0xFFFFFFFF;
                } else if (esize == 8) {
                    if (idx == 0) cpu.v_lo[rd] = src;
                    else if (idx == 1 && Q) cpu.v_hi[rd] = src;
                }
                return;
            }
            // ── ORR (vector) / MOV (vector alias): ... 0 1 Rn 0 0 0 1 1 1 0 0 0 0 0 Rm Rd
            // v0 mask 0xFF20FC00 val 0x0EA01C00 only matched Q=0.
            // Also note: v0 had a separate "ORR (vector) — full form"
            // check at 0x0EA01C00 that was unreachable (the MOV alias
            // caught it first). Merged here.
            case 0x0EA01C00: {
                cpu.v_lo[rd] = cpu.v_lo[rn] | cpu.v_lo[rm];
                if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] | cpu.v_hi[rm];
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── BIC (vector): Vd = Vn & ~Vm ──
            // Encoding: 0_Q_0_01110_01_1_Rm_000111_Rn_Rd (sub_noq=0x0E601C00)
            // never matched the actual BIC encoding (which has size=01
            // and bits[15:10]=000111, giving sub_noq=0x0E601C00). With
            // the wrong case, BIC was silently NOP'd, but no test caught
            // it because GCC -O2 usually lowers vbicq to AND + NOT rather
            // than the BIC instruction. glibc's SIMD strlen/strchr DO use
            // BIC, so the silent NOP broke these functions in subtle ways.
            case 0x0E601C00: {
                cpu.v_lo[rd] = cpu.v_lo[rn] & ~cpu.v_lo[rm];
                if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] & ~cpu.v_hi[rm];
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── ORN (vector): Vd = Vn | ~Vm ──
            // Encoding: 0_Q_0_01110_11_1_Rm_000111_Rn_Rd (sub_noq=0x0EE01C00)
            case 0x0EE01C00: {
                cpu.v_lo[rd] = cpu.v_lo[rn] | ~cpu.v_lo[rm];
                if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] | ~cpu.v_hi[rm];
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── AND (vector) ──
            case 0x0E201C00: {
                cpu.v_lo[rd] = cpu.v_lo[rn] & cpu.v_lo[rm];
                if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] & cpu.v_hi[rm];
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── EOR (vector) ──
            case 0x2E201C00: {
                cpu.v_lo[rd] = cpu.v_lo[rn] ^ cpu.v_lo[rm];
                if (Q) cpu.v_hi[rd] = cpu.v_hi[rn] ^ cpu.v_hi[rm];
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── BSL (vector): Vd = (Vn & Vd) | (Vm & ~Vd) ──
            // Vd is the mask; for each bit, if Vd=1 take Vn bit, else take Vm bit.
            // Encoding: 0_Q_1_01110_01_1_Rm_000111_Rn_Rd (sub_noq=0x2E601C00)
            // strchr/strchrnul use BSL to combine NUL-match and char-match
            // bitmaps. Without BSL, the function returned wrong results,
            // breaking curl's URL parser ("URL using bad/illegal format").
            case 0x2E601C00: {
                uint64_t mask_lo = cpu.v_lo[rd];
                uint64_t mask_hi = Q ? cpu.v_hi[rd] : 0;
                cpu.v_lo[rd] = (cpu.v_lo[rn] & mask_lo) | (cpu.v_lo[rm] & ~mask_lo);
                if (Q) cpu.v_hi[rd] = (cpu.v_hi[rn] & mask_hi) | (cpu.v_hi[rm] & ~mask_hi);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── BIT (vector): Vd = (Vn & Vm) | (Vd & ~Vm) ──
            // Vm is the mask; for each bit, if Vm=1 take Vn bit, else keep Vd bit.
            // Encoding: 0_Q_1_01110_10_1_Rm_000111_Rn_Rd (sub_noq=0x2EA01C00)
            // strchr uses BIT to merge char-match bits into the NUL-match
            // bitmap (the "Bitwise Insert if True" operation). Without BIT,
            // strchr could not find ':' or other non-NUL chars, returning
            // a pointer to the NUL terminator instead.
            case 0x2EA01C00: {
                uint64_t mask_lo = cpu.v_lo[rm];
                uint64_t mask_hi = Q ? cpu.v_hi[rm] : 0;
                cpu.v_lo[rd] = (cpu.v_lo[rn] & mask_lo) | (cpu.v_lo[rd] & ~mask_lo);
                if (Q) cpu.v_hi[rd] = (cpu.v_hi[rn] & mask_hi) | (cpu.v_hi[rd] & ~mask_hi);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── BIF (vector): Vd = (Vn & ~Vm) | (Vd & Vm) ──
            // Vm is the mask; for each bit, if Vm=0 take Vn bit, else keep Vd bit.
            // Encoding: 0_Q_1_01110_11_1_Rm_000111_Rn_Rd (sub_noq=0x2EE01C00)
            // complement of BIT; used by glibc's strrchr and some strlen paths.
            case 0x2EE01C00: {
                uint64_t mask_lo = cpu.v_lo[rm];
                uint64_t mask_hi = Q ? cpu.v_hi[rm] : 0;
                cpu.v_lo[rd] = (cpu.v_lo[rn] & ~mask_lo) | (cpu.v_lo[rd] & mask_lo);
                if (Q) cpu.v_hi[rd] = (cpu.v_hi[rn] & ~mask_hi) | (cpu.v_hi[rd] & mask_hi);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── ADDP (vector) ──
            // (used when the string is near a page boundary) uses ADDP to
            // reduce CMEQ results. Without it, strlen returned wrong lengths
            // for strings near page boundaries, breaking curl's URL parser.
            // ADDP: 0 Q 0 01110 size 1 Rm 0 101111 Rn Rd (base 0x0E20BC00)
            // Adds pairwise elements from Vn and Vm, placing results in Vd.
            case 0x0E20BC00: {  // ADDP (vector), 8B/16B
                int elems = Q ? 16 : 8;
                uint8_t buf_n[16], buf_m[16], buf_d[16];
                memcpy(buf_n, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_m, &cpu.v_lo[rm], 8);
                if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                // Pairwise add: for 16B, pairs are (n[0]+n[1]), (n[2]+n[3]), ...
                // For the two-register form, it's pairwise within the
                // concatenation. Actually, ADDP vD.16b, vN.16b, vM.16b
                // does: vD[i] = vN[2i] + vN[2i+1] for i=0..7,
                //        vD[8+i] = vM[2i] + vM[2i+1] for i=0..7.
                for (int i = 0; i < elems/2; i++) {
                    buf_d[i] = buf_n[2*i] + buf_n[2*i+1];
                    if (Q) buf_d[elems/2 + i] = buf_m[2*i] + buf_m[2*i+1];
                }
                if (!Q) {
                    // 8B form: only 4 results from Vn, 4 from Vm
                    for (int i = 0; i < 4; i++) {
                        buf_d[4+i] = buf_m[2*i] + buf_m[2*i+1];
                    }
                }
                memcpy(&cpu.v_lo[rd], buf_d, 8);
                if (Q) memcpy(&cpu.v_hi[rd], buf_d + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── ADDHN / SUBHN (vector, narrowing) ──────────────────────
            // ADDHN: 0 Q 0 01110 size 1 Rm 0 0000 0 Rn Rd  (base 0x0E204000)
            // SUBHN: 0 Q 1 01110 size 1 Rm 0 0000 0 Rn Rd  (base 0x4E204000)
            // These add/subtract corresponding 2*size-bit elements from Vn
            // and Vm, take the HIGH half of the result, and place it in the
            // corresponding size-bit elements of Vd (narrowing).
            //
            // default and was silently NOP'd. This broke glibc's SIMD
            // strlen, which uses `addhn v2.8b, v1.8h, v1.8h` to narrow
            // the 16-byte CMEQ result to 8 bytes. Without ADDHN, the
            // narrowing produced all-zeros, so strlen's `cbnz x2` never
            // branched, creating an infinite loop scanning for the NUL
            // terminator. This caused the 8thread-with-printf hang.
            case 0x0E204000: {  // ADDHN
                int esize_in = 1 << (size + 1);  // 2, 4, 8, 16 bytes
                int esize_out = esize_in / 2;
                // ADDHN always reads the FULL 128-bit source (8H/4S/2D).
                // Q=0: write result to LOW 64 bits of Vd.
                // Q=1 (ADDHN2): write result to HIGH 64 bits of Vd.
                int elems = 16 / esize_in;
                uint8_t buf_n[16], buf_m[16];
                memcpy(buf_n, &cpu.v_lo[rn], 8);
                memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_m, &cpu.v_lo[rm], 8);
                memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                uint8_t out[8];
                for (int i = 0; i < elems; i++) {
                    uint64_t a = 0, b = 0;
                    memcpy(&a, buf_n + i * esize_in, esize_in);
                    memcpy(&b, buf_m + i * esize_in, esize_in);
                    uint64_t sum = a + b;
                    uint64_t hi = sum >> (esize_out * 8);
                    memcpy(out + i * esize_out, &hi, esize_out);
                }
                if (Q) {
                    // ADDHN2: write to high 64 bits, preserve low 64
                    memcpy(&cpu.v_hi[rd], out, 8);
                } else {
                    // ADDHN: write to low 64 bits, zero high 64
                    memcpy(&cpu.v_lo[rd], out, 8);
                    cpu.v_hi[rd] = 0;
                }
                return;
            }
            case 0x4E204000: {  // SUBHN
                int esize_in = 1 << (size + 1);
                int esize_out = esize_in / 2;
                int elems = 16 / esize_in;
                uint8_t buf_n[16], buf_m[16];
                memcpy(buf_n, &cpu.v_lo[rn], 8);
                memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_m, &cpu.v_lo[rm], 8);
                memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                uint8_t out[8];
                for (int i = 0; i < elems; i++) {
                    uint64_t a = 0, b = 0;
                    memcpy(&a, buf_n + i * esize_in, esize_in);
                    memcpy(&b, buf_m + i * esize_in, esize_in);
                    uint64_t diff = a - b;
                    uint64_t hi = diff >> (esize_out * 8);
                    memcpy(out + i * esize_out, &hi, esize_out);
                }
                if (Q) {
                    memcpy(&cpu.v_hi[rd], out, 8);
                } else {
                    memcpy(&cpu.v_lo[rd], out, 8);
                    cpu.v_hi[rd] = 0;
                }
                return;
            }
            // ── SSHL / USHL (vector, shift left by register) ─────────────
            // Encoding: 0 Q U 01110 size 1 Rm 000100 Rn Rd (sub_noq=0x0E204400
            // for U=0, 0x2E204400 for U=1; size selects 8b/8h/4s/2d lanes).
            // SSHL shifts each lane of Vn left by the SIGNED value in the
            // corresponding Vm lane (negative shift → arithmetic right);
            // USHL uses the UNSIGNED Vm value (left shift only). Shift
            // amounts >= the lane width produce 0 (LSL) or sign fill (ASR).
            // glibc strspn lowers its SWAR "has zero / has non-matching byte"
            // trick to `sshl v0.16b, v0.16b, v2.16b` + `add`; without this
            // the interp SIGILL'd (signal 4) inside strspn during fontconfig
            // config parsing.
            case 0x0E204400: case 0x0E604400: case 0x0EA04400: case 0x0EE04400:  // SSHL
            case 0x2E204400: case 0x2E604400: case 0x2EA04400: case 0x2EE04400:  // USHL
            {
                int esize = 1 << size;          // 1 / 2 / 4 / 8 bytes
                int bits = esize * 8;
                int elems = (Q ? 16 : 8) / esize;
                uint64_t mask = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
                uint8_t buf_n[16], buf_m[16];
                memcpy(buf_n, &cpu.v_lo[rn], 8);
                memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_m, &cpu.v_lo[rm], 8);
                memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                uint8_t out[16] = {0};
                for (int i = 0; i < elems; i++) {
                    uint64_t vn_lane = 0, vm_lane = 0;
                    memcpy(&vn_lane, buf_n + i * esize, esize);
                    memcpy(&vm_lane, buf_m + i * esize, esize);
                    uint64_t result;
                    if (!U) {
                        // SSHL: signed shift amount; negative → ASR
                        int64_t s;
                        if      (esize == 1) s = (int64_t)(int8_t)(uint8_t)vm_lane;
                        else if (esize == 2) s = (int64_t)(int16_t)(uint16_t)vm_lane;
                        else if (esize == 4) s = (int64_t)(int32_t)(uint32_t)vm_lane;
                        else                 s = (int64_t)vm_lane;
                        if (s >= 0) {
                            if (s >= bits) result = 0;
                            else result = (vn_lane << s) & mask;
                        } else {
                            int64_t rs = -s;
                            int64_t sv;
                            if      (esize == 1) sv = (int64_t)(int8_t)(uint8_t)vn_lane;
                            else if (esize == 2) sv = (int64_t)(int16_t)(uint16_t)vn_lane;
                            else if (esize == 4) sv = (int64_t)(int32_t)(uint32_t)vn_lane;
                            else                 sv = (int64_t)vn_lane;
                            if (rs >= bits) result = (uint64_t)(sv < 0 ? -1 : 0) & mask;
                            else result = (uint64_t)(sv >> rs) & mask;
                        }
                    } else {
                        // USHL: unsigned shift amount, left shift only
                        if (vm_lane >= (uint64_t)bits) result = 0;
                        else result = (vn_lane << vm_lane) & mask;
                    }
                    memcpy(out + i * esize, &result, esize);
                }
                if (Q) {
                    memcpy(&cpu.v_lo[rd], out, 8);
                    memcpy(&cpu.v_hi[rd], out + 8, 8);
                } else {
                    memcpy(&cpu.v_lo[rd], out, 8);
                    cpu.v_hi[rd] = 0;
                }
                return;
            }
            // ── SIMD vector FP 2-source (FADD/FSUB/FMUL/FDIV/FMAX/FMIN/
            //     FMAXNM/FMINNM/FABD/FMULX, vector form) ──────────────
            // These are in the 0x0E/0x2E SIMD group (bits[28:24]=0b01110)
            // with bit[23]=1 (FP), bit[21]=1, bits[15:12]=opcode,
            // bits[11:10]=0b01. ftype in bits[23:22] (but bit23 is always
            // 1 here, so effectively bit22: 0=single, 1=double). Q (bit30)
            // selects .2s/.2d (Q=0, 2 lanes) vs .4s (Q=1, 4 lanes).
            // Without this, NEON-vectorized FP (libc memcpy/memset with
            // NEON, image/audio processing) silently produces wrong
            // results — the instruction falls through to the default.
            case 0x0e20d400:  // FADD v (single)
            case 0x0e60d400:  // FADD v (double)
            case 0x0ea0d400:  // FSUB v (single)
            case 0x0ee0d400:  // FSUB v (double)
            case 0x2e20dc00:  // FMUL v (single)
            case 0x2e60dc00:  // FMUL v (double)
            case 0x0e20f400:  // FMAX v (single)
            case 0x0e60f400:  // FMAX v (double)
            case 0x0ea0f400:  // FMIN v (single)
            case 0x0ee0f400:  // FMIN v (double)
            case 0x2ea0d400:  // FABD v (single)
            case 0x2ee0d400:  // FABD v (double)
            case 0x0e20c400:  // FMAXNM v (single)
            case 0x0e60c400:  // FMAXNM v (double)
            case 0x0ea0c400:  // FMINNM v (single)
            case 0x0ee0c400:  // FMINNM v (double)
            case 0x0e20dc00:  // FMULX v (single)
            case 0x0e60dc00:  // FMULX v (double)
            case 0x2e20fc00:  // FDIV v (single)
            case 0x2e60fc00:  // FDIV v (double)
            case 0x0e20cc00:  // FMLA v (single)
            case 0x0e60cc00:  // FMLA v (double)
            case 0x0ea0cc00:  // FMLS v (single)
            case 0x0ee0cc00: { // FMLS v (double)
                // The opcode is encoded across bits[29](U), bits[15:12],
                // and bits[11:10] — NOT a single field. Dispatch on the
                // full sub_noq (already matched by the case labels above).
                // sub_noq keeps bit22, so the double forms (.2d/.1d) have
                // bit22=1; mask it out so both sizes hit the same opcode.
                uint32_t key = sub_noq & ~(1u << 22);
                bool is_double = (op >> 22) & 1;  // bit22: 0=S, 1=D
                int lanes = is_double ? (Q ? 2 : 1) : (Q ? 4 : 2);
                auto get_op = [&](uint32_t k) -> int {
                    if (k == 0x0e20d400) return 0;  // FADD
                    if (k == 0x0ea0d400) return 1;  // FSUB
                    if (k == 0x2e20dc00) return 2;  // FMUL
                    if (k == 0x2e20fc00) return 3;  // FDIV
                    if (k == 0x0e20f400) return 4;  // FMAX
                    if (k == 0x0ea0f400) return 5;  // FMIN
                    if (k == 0x0e20c400) return 6;  // FMAXNM
                    if (k == 0x0ea0c400) return 7;  // FMINNM
                    if (k == 0x0e20dc00) return 0xB; // FMULX
                    if (k == 0x2ea0d400) return 0xD; // FABD
                    if (k == 0x0e20cc00) return 0xE; // FMLA
                    if (k == 0x0ea0cc00) return 0xF; // FMLS
                    return -1;
                };
                int opcode = get_op(key);
                if (is_double) {
                    auto gv = [&](int r, int lane) -> double {
                        uint64_t bits = (lane == 0) ? cpu.v_lo[r] : cpu.v_hi[r];
                        double d; memcpy(&d, &bits, 8); return d;
                    };
                    auto pv = [&](int r, int lane, double v) {
                        uint64_t bits; memcpy(&bits, &v, 8);
                        if (lane == 0) cpu.v_lo[r] = bits; else cpu.v_hi[r] = bits;
                    };
                    for (int i = 0; i < lanes; i++) {
                        double a = gv(rn, i), b = gv(rm, i), r = 0;
                        switch (opcode) {
                            case 0x0: r = a + b; break;             // FADD
                            case 0x1: r = a - b; break;             // FSUB
                            case 0x2: r = a * b; break;             // FMUL
                            case 0x3: r = a / b; break;             // FDIV
                            case 0x4: r = std::fmax(a, b); break;   // FMAX
                            case 0x5: r = std::fmin(a, b); break;   // FMIN
                            case 0x6: r = std::fmax(a, b); break;   // FMAXNM
                            case 0x7: r = std::fmin(a, b); break;   // FMINNM
                            case 0xB: r = a * b; break;             // FMULX (≈FMUL for non-special)
                            case 0xD: r = std::fabs(a - b); break;  // FABD
                            case 0xE: r = gv(rd, i) + a * b; break; // FMLA (accumulate into Vd)
                            case 0xF: r = gv(rd, i) - a * b; break; // FMLS (accumulate into Vd)
                            default: r = 0; break;
                        }
                        pv(rd, i, r);
                    }
                    // Zero unused high lanes for Q=0 (.1d → high 64 zeroed,
                    // matching the JIT/IR which always zero v_hi for Q=0).
                    if (!Q) cpu.v_hi[rd] = 0;
                } else {
                    auto gv = [&](int r, int lane) -> float {
                        // lanes 0,1 in v_lo (low 64), lanes 2,3 in v_hi.
                        uint64_t chunk = (lane < 2) ? cpu.v_lo[r] : cpu.v_hi[r];
                        uint32_t bits = static_cast<uint32_t>(chunk >> ((lane & 1) * 32));
                        float f; memcpy(&f, &bits, 4); return f;
                    };
                    auto pv = [&](int r, int lane, float v) {
                        uint32_t bits; memcpy(&bits, &v, 4);
                        uint64_t* chunk = (lane < 2) ? &cpu.v_lo[r] : &cpu.v_hi[r];
                        int sh = (lane & 1) * 32;
                        *chunk = (*chunk & ~(0xFFFFFFFFULL << sh))
                               | (static_cast<uint64_t>(bits) << sh);
                    };
                    for (int i = 0; i < lanes; i++) {
                        float a = gv(rn, i), b = gv(rm, i), r = 0;
                        switch (opcode) {
                            case 0x0: r = a + b; break;             // FADD
                            case 0x1: r = a - b; break;             // FSUB
                            case 0x2: r = a * b; break;             // FMUL
                            case 0x3: r = a / b; break;             // FDIV
                            case 0x4: r = std::fmax(a, b); break;   // FMAX
                            case 0x5: r = std::fmin(a, b); break;   // FMIN
                            case 0x6: r = std::fmax(a, b); break;   // FMAXNM
                            case 0x7: r = std::fmin(a, b); break;   // FMINNM
                            case 0xB: r = a * b; break;             // FMULX
                            case 0xD: r = std::fabsf(a - b); break; // FABD
                            case 0xE: r = gv(rd, i) + a * b; break; // FMLA (accumulate into Vd)
                            case 0xF: r = gv(rd, i) - a * b; break; // FMLS (accumulate into Vd)
                            default: r = 0; break;
                        }
                        pv(rd, i, r);
                    }
                    // Zero unused high lanes for Q=0 (.2s → high 64 zeroed).
                    if (!Q) cpu.v_hi[rd] = 0;
                }
                return;
            }
            // ── TBL/TBX (Table Lookup) ──
            // AArch64 TBL/TBX permute bytes from one or two source
            // vectors using indices from a third vector. Each byte
            // index in the index vector selects a byte from the
            // concatenated source(s). Indices >= the source length:
            //   TBL: result byte = 0
            //   TBX: result byte unchanged (in-place update)
            //
            // TBL/TBX encoding: 0 Q 0 0 1 1 1 0 size 0 0 Rm op2 L Rn Rd
            //   - op2 = bit 12 (0x1000): 0 = TBL, 1 = TBX
            //   - L   = bit 13 (0x2000): 0 = one source reg, 1 = two
            // Both bits survive the sub_noq mask (0xFFE0FC00 keeps
            // bits[15:10]), so each form has its own case:
            //   - TBL1: 0x0E000000, TBL2: 0x0E002000
            //   - TBX1: 0x0E001000, TBX2: 0x0E003000
            // v0 only matched TBL1 (mask 0xFFE00000 val 0x0E000000,
            // which also masks off bit 13); the 2-source TBL2 form
            // (e.g. GCC lowering of vextq_u8 with a 2-vector table)
            // silently fell through as a NOP, producing all zeros.
            case 0x0E000000:   // TBL (1 source reg)
            case 0x0E002000:   // TBL (2 source regs)
            case 0x0E001000:   // TBX (1 source reg)
            case 0x0E003000:   // TBX (2 source regs)
            case 0x0E004000:   // TBL (3 source regs)
            case 0x0E006000:   // TBL (4 source regs)
            case 0x0E005000:   // TBX (3 source regs)
            case 0x0E007000: { // TBX (4 source regs)
                bool is_tbx     = (op & 0x1000) != 0;  // op2 bit (bit 12)
                int nregs       = ((op >> 13) & 0x3) + 1;  // bits[14:13]
                // Q bit (bit 30) — already extracted as `Q` above.
                // Build the source table: nregs x (16 or 8) bytes.
                int bytes_per_reg = Q ? 16 : 8;
                uint8_t table[4 * 16];
                for (int r = 0; r < nregs; r++) {
                    int rn_r = (rn + r) & 31;
                    memcpy(table + r * bytes_per_reg, &cpu.v_lo[rn_r], 8);
                    if (Q) memcpy(table + r * bytes_per_reg + 8, &cpu.v_hi[rn_r], 8);
                }
                int table_len = nregs * bytes_per_reg;
                // Read the index vector Vm.
                uint8_t idx[16];
                memcpy(idx,    &cpu.v_lo[rm], 8);
                memcpy(idx+8,  &cpu.v_hi[rm], 8);
                int out_len = Q ? 16 : 8;
                // Read current Vd (for TBX in-place update).
                uint8_t out[16];
                memcpy(out,    &cpu.v_lo[rd], 8);
                memcpy(out+8,  &cpu.v_hi[rd], 8);
                for (int i = 0; i < out_len; i++) {
                    uint8_t b = idx[i];
                    if (b < table_len) {
                        out[i] = table[b];
                    } else if (!is_tbx) {
                        // TBL: out-of-range indices produce 0.
                        out[i] = 0;
                    }
                    // TBX: out-of-range indices leave out[i] unchanged.
                }
                cpu.v_lo[rd] = 0;
                memcpy(&cpu.v_lo[rd], out, 8);
                if (Q) {
                    memcpy(&cpu.v_hi[rd], out+8, 8);
                } else {
                    cpu.v_hi[rd] = 0;
                }
                return;
            }
            // ── CMGT / CMGE / CMHI / CMHS (register compare, vector) ──
            // SIMD 3-register integer compare. Encoding:
            //   0 Q U 01110 size 1 Rm opcode Rn Rd
            // opcode bits[15:10]: 0b001101 → GT/HI (strict >),
            //                      0b001111 → GE/HS (>=).
            // U bit (29): 0 = signed (CMGT/CMGE), 1 = unsigned (CMHI/CMHS).
            // sub_noq keeps U but strips Q (bit 30), so each of the four
            // ops has a distinct sub_noq base × 4 size variants (size in
            // bits 23:22, which survives the mask):
            //   CMGT (signed >):    0x0E203400 0x0E603400 0x0EA03400 0x0EE03400
            //   CMGE (signed >=):   0x0E203C00 0x0E603C00 0x0EA03C00 0x0EE03C00
            //   CMHI (unsigned >):  0x2E203400 0x2E603400 0x2EA03400 0x2EE03400
            //   CMHS (unsigned >=): 0x2E203C00 0x2E603C00 0x2EA03C00 0x2EE03C00
            // (Verified against cross binutils: cmhi v0.8h,v2.8h,v4.8h =
            // 0x6E643440 → sub_noq 0x2E603400.)
            case 0x0E203400: case 0x0E603400: case 0x0EA03400: case 0x0EE03400:
            case 0x0E203C00: case 0x0E603C00: case 0x0EA03C00: case 0x0EE03C00:
            case 0x2E203400: case 0x2E603400: case 0x2EA03400: case 0x2EE03400:
            case 0x2E203C00: case 0x2E603C00: case 0x2EA03C00: case 0x2EE03C00: {
                bool U = (op >> 29) & 1;       // 0=signed, 1=unsigned
                bool ge = (op >> 11) & 1;      // opcode 0x0F (>=) vs 0x0D (>)
                int esize = 1 << size;
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf_n[16], buf_m[16];
                memcpy(buf_n, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_m, &cpu.v_lo[rm], 8);
                if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                uint8_t out[16] = {0};
                for (int i = 0; i < elems; i++) {
                    bool c;
                    if (U) {
                        uint64_t n = 0, m = 0;
                        memcpy(&n, buf_n + i*esize, esize);
                        memcpy(&m, buf_m + i*esize, esize);
                        c = ge ? (n >= m) : (n > m);
                    } else {
                        int64_t n = 0, m = 0;
                        memcpy(&n, buf_n + i*esize, esize);
                        memcpy(&m, buf_m + i*esize, esize);
                        if (esize == 1) { n = (int8_t)n; m = (int8_t)m; }
                        else if (esize == 2) { n = (int16_t)n; m = (int16_t)m; }
                        else if (esize == 4) { n = (int32_t)n; m = (int32_t)m; }
                        c = ge ? (n >= m) : (n > m);
                    }
                    memset(out + i*esize, c ? 0xFF : 0x00, esize);
                }
                memcpy(&cpu.v_lo[rd], out, 8);
                if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── UMAXP/UMINP/SMAXP/SMINP family ──
            // (unsigned) for what was labeled SMAXP/SMINP (U=0, signed).
            // For byte elements with values 0x80-0xFF, signed vs unsigned
            // differ. We now use int64_t with sign-extension for the
            // signed (U=0) path.
            //
            // Encoding: 0 Q U 01110 size Rm 01101 0 Rn Rd
            //   U=0, C=0: SMAXP (signed pairwise max)
            //   U=0, C=1: SMINP (signed pairwise min)
            //   U=1, C=0: UMAXP (unsigned pairwise max)
            //   U=1, C=1: UMINP (unsigned pairwise min)
            // After sub_noq (Q stripped, U kept):
            //   U=0 → sub_noq = 0x2E20A400
            //   U=1 → sub_noq = 0x6E20A400
            // Wait — 0x6E... & ~(1<<30) = 0x2E... So both U=0 and U=1
            // map to sub_noq = 0x2E20A400! The U bit (29) is NOT
            // distinguished by sub_noq because sub_noq only strips Q (30).
            //
            // Actually: 0x2E = 0010 1110 (bit 29=1, bit 30=0)
            //           0x6E = 0110 1110 (bit 29=1, bit 30=1)
            // After stripping Q (bit 30): both become 0x2E.
            // So U=0 and U=1 BOTH map to 0x2E20A400 after sub_noq!
            // That means the existing case 0x2E20A400 already handles
            // BOTH SMAXP/SMINP (U=0) AND UMAXP/UMINP (U=1).
            //
            // The bug was NOT that UMAXP didn't match — it DID match
            // (via sub_noq). The bug was that the code used unsigned
            // comparison (uint64_t), which is correct for UMAXP but
            // WRONG for SMAXP. We now check the U bit at runtime to
            // select signed vs unsigned comparison.
            case 0x2E20A400: case 0x2E60A400: case 0x2EA0A400: case 0x2EE0A400: {  // UMAXP/UMINP (U=1) and SMAXP/SMINP (U=0); size 0-3
                // bit 15. Verified by comparing UMAXP (0x6e20a400) vs UMINP
                // (0x6e20ac00) — they differ only at bit 11. The old code
                // used bit 15, which is part of the opcode that
                // distinguishes pairwise ops from other SIMD ops, NOT max
                // from min. With the wrong bit, UMAXP was treated as UMINP
                // (C=1), computing min instead of max. This broke glibc's
                // strchrnul: `umaxp v4.16b, v3.16b, v3.16b` computed min
                // instead of max, so the mask-reduction returned 0 even
                // when a match was found, preventing '%' detection in
                // printf format strings.
                bool C = (op >> 11) & 1;   // 0=max, 1=min
                bool U = (op >> 29) & 1;   // 0=signed, 1=unsigned
                int esize = 1 << size;
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf_n[16], buf_m[16];
                memcpy(buf_n, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_m, &cpu.v_lo[rm], 8);
                if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                uint8_t out[16] = {0};
                // ops that operate on EACH source independently, producing
                // TWO half-results:
                //   first half of Vd = pairwise(max/min) of Vn
                //   second half of Vd = pairwise(max/min) of Vm
                // which is wrong — it only produced half the output bytes
                // AND used the wrong semantics. This broke glibc's
                // strchrnul SIMD path, which uses `umaxp v4.16b, v3.16b,
                // v3.16b` to reduce a 16-byte mask to 8 bytes; the old
                // code zeroed the second half and used a 4-way max that
                // happened to produce zeros for the test pattern,
                // preventing strchrnul from finding '%' in format strings.
                auto load_elem = [&](const uint8_t* p) -> int64_t {
                    uint64_t u = 0;
                    memcpy(&u, p, esize);
                    if (!U) {
                        if (esize == 1) return (int8_t)u;
                        if (esize == 2) return (int16_t)u;
                        if (esize == 4) return (int32_t)u;
                        return (int64_t)u;
                    }
                    return (int64_t)u;
                };
                auto do_pair = [&](int64_t a, int64_t b) -> int64_t {
                    return C ? ((a < b) ? a : b) : ((a > b) ? a : b);
                };
                // First half: pairwise op on Vn.
                for (int i = 0; i < elems / 2; i++) {
                    int64_t n0 = load_elem(buf_n + (2*i) * esize);
                    int64_t n1 = load_elem(buf_n + (2*i+1) * esize);
                    int64_t res = do_pair(n0, n1);
                    memcpy(out + i * esize, &res, esize);
                }
                // Second half: pairwise op on Vm (only for Q=1; for Q=0
                // the output is only 8 bytes and the second half goes to
                // the zeroed v_hi).
                if (Q) {
                    int half = (elems / 2) * esize;  // offset into out
                    for (int i = 0; i < elems / 2; i++) {
                        int64_t m0 = load_elem(buf_m + (2*i) * esize);
                        int64_t m1 = load_elem(buf_m + (2*i+1) * esize);
                        int64_t res = do_pair(m0, m1);
                        memcpy(out + half + i * esize, &res, esize);
                    }
                }
                memcpy(&cpu.v_lo[rd], out, 8);
                if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── UMOV (vector element to GPR) / INS (element, vector to vector) ──
            // Encoding: Q 0 0 11110 size 1 imm5 0 0 1 1 Rn Rd
            // Q=0 → 32-bit GPR dest, Q=1 → 64-bit GPR dest (UMOV only)
            // For INS(element), Rd is a vector register.
            // imm5 encodes element size and index.
            case 0x0E003C00: {
                uint8_t imm5 = (op >> 16) & 0x1F;
                // Find lowest set bit → element size
                int esize_log2 = 0;
                for (int b = 0; b < 5; b++) {
                    if (imm5 & (1 << b)) { esize_log2 = b; break; }
                }
                int esize = 1 << esize_log2;  // bytes: 1, 2, 4, or 8
                int index = imm5 >> (esize_log2 + 1);
                // UMOV: read from vector element, write to GPR.
                // (8/esize) must read from v_hi, not v_lo. The old code
                // always read from v_lo, returning wrong values for lane
                // indices >= 2 (32-bit) or >= 1 (64-bit).
                int elems_per_qword = 8 / esize;
                uint64_t val = 0;
                if (esize == 8) {
                    val = (index == 0) ? cpu.v_lo[rn] : cpu.v_hi[rn];
                } else if (esize == 4) {
                    if (index < elems_per_qword)
                        val = reinterpret_cast<uint32_t*>(&cpu.v_lo[rn])[index];
                    else
                        val = reinterpret_cast<uint32_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
                } else if (esize == 2) {
                    if (index < elems_per_qword)
                        val = reinterpret_cast<uint16_t*>(&cpu.v_lo[rn])[index];
                    else
                        val = reinterpret_cast<uint16_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
                } else {  // esize == 1
                    if (index < elems_per_qword)
                        val = reinterpret_cast<uint8_t*>(&cpu.v_lo[rn])[index];
                    else
                        val = reinterpret_cast<uint8_t*>(&cpu.v_hi[rn])[index - elems_per_qword];
                }
                if (rd != 31) cpu.regs[rd] = val;
                return;
            }
            // ── CMEQ two registers ──
            // CMEQ encoding: 0 Q U 01110 size Rm 100011 1 Rn Rd
            // Both U=0 and U=1 are valid and produce the same result
            // (equality is sign-agnostic). After sub_noq masking
            // (Q stripped, U kept), both map to 0x2E208C00 because
            // the U bit (29) is the same for 0x2E... and 0x6E...
            // after Q (bit 30) is removed: 0x6E... & ~(1<<30) = 0x2E...
            // So a single case 0x2E208C00 handles both encodings.
            case 0x2E208C00: case 0x2E608C00: case 0x2EA08C00: case 0x2EE08C00: { // CMEQ (U=0 or U=1, after sub_noq); size 0-3
                int esize = (size == 0) ? 1 : (size == 1 ? 2 : (size == 2 ? 4 : 8));
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf_rn[16], buf_rm[16];
                memcpy(buf_rn, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf_rn + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_rm, &cpu.v_lo[rm], 8);
                if (Q) memcpy(buf_rm + 8, &cpu.v_hi[rm], 8);
                uint8_t out[16] = {0};
                for (int i = 0; i < elems; i++) {
                    bool eq = (memcmp(buf_rn + i*esize, buf_rm + i*esize, esize) == 0);
                    memset(out + i*esize, eq ? 0xFF : 0x00, esize);
                }
                memcpy(&cpu.v_lo[rd], out, 8);
                if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── CMTST (test bits, vector) ──
            // Encoding 0 Q U 01110 size 1 Rm 100011 0 Rn Rd (U=0).
            // sub_noq = 0x0E208C00 (size 0) / 0x0E608C00 (1) / 0x0EA08C00
            // (2) / 0x0EE08C00 (3). Unlike CMEQ (U=1 → 0x2E...8C00), the
            // U=0 form keeps the 0x0E prefix after Q is stripped, so CMTST
            // gets its OWN case. (Old code assumed both U=0/U=1 map to
            // 0x2E208C00 — that only holds for the Q=1 0x6E... variant.)
            // Result per lane: all-ones if (Vn & Vm) != 0, else zero.
            case 0x0E208C00: case 0x0E608C00: case 0x0EA08C00: case 0x0EE08C00: {
                int esize = (size == 0) ? 1 : (size == 1 ? 2 : (size == 2 ? 4 : 8));
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf_rn[16], buf_rm[16];
                memcpy(buf_rn, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf_rn + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_rm, &cpu.v_lo[rm], 8);
                if (Q) memcpy(buf_rm + 8, &cpu.v_hi[rm], 8);
                uint8_t out[16] = {0};
                for (int i = 0; i < elems; i++) {
                    bool any = false;
                    for (int k = 0; k < esize; k++) {
                        if ((buf_rn[i*esize + k] & buf_rm[i*esize + k]) != 0) {
                            any = true;
                            break;
                        }
                    }
                    memset(out + i*esize, any ? 0xFF : 0x00, esize);
                }
                memcpy(&cpu.v_lo[rd], out, 8);
                if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── Narrowing (XTN/SQXTN/UQXTN/SQXTUN ±2) ──
            // bits[15:10] discriminates the op: 001010 → XTN (U=0) /
            // SQXTUN (U=1); 010010 → SQXTN (U=0) / UQXTN (U=1). size
            // (bits 23:22) selects the SOURCE element size (2× the dest
            // size): 00 → 16-bit src/8-bit dst, 01 → 32/16, 10 → 64/32.
            // Q=0 writes the low 64 bits of Rd and zeroes the upper 64;
            // Q=1 (*2 variants) writes the high 64 bits, preserving the
            // low half. SQXTUN saturates signed values into the unsigned
            // destination range. Verified encodings (cross-as):
            //   XTN   0x0E212820 (8b,8h) / 0x0E612820 (4h,4s) / 0x0EA12820 (2s,2d)
            //   XTN2  0x4E…, SQXTN 0x0E…4820, UQXTN 0x2E…4820, SQXTUN 0x2E…2820.
            case 0x0E202800: case 0x0E602800: case 0x0EA02800:  // XTN (U=0)
            case 0x2E202800: case 0x2E602800: case 0x2EA02800:  // SQXTUN (U=1)
            case 0x0E204800: case 0x0E604800: case 0x0EA04800:  // SQXTN (U=0)
            case 0x2E204800: case 0x2E604800: case 0x2EA04800:  // UQXTN (U=1)
            {
                if (size == 3) throw DecodeError(cpu.pc, inst);
                int src_esize = 2 << size;      // 2 / 4 / 8 bytes
                int dst_esize = src_esize >> 1; // 1 / 2 / 4 bytes
                int elems = 16 / src_esize;     // 8 / 4 / 2
                uint32_t opc = (op >> 10) & 0x3F;
                // opc 0x0A → XTN/SQXTUN; 0x12 → SQXTN/UQXTN. U picks the
                // saturating form in both families.
                bool sat_signed = (opc == 0x12) && !U;   // SQXTN
                bool sat_unsigned = (opc == 0x12) && U;  // UQXTN
                bool sat_to_unsigned = (opc == 0x0A) && U; // SQXTUN
                uint64_t max_u = (dst_esize == 8) ? ~0ULL : ((1ULL << (8 * dst_esize)) - 1);
                int64_t s_lo = -(1LL << (8 * dst_esize - 1));
                int64_t s_hi = (1LL << (8 * dst_esize - 1)) - 1;
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                memcpy(buf + 8, &cpu.v_hi[rn], 8);  // source is always full 128 bits
                uint8_t out[8] = {0};
                for (int i = 0; i < elems; i++) {
                    uint64_t v = 0;
                    memcpy(&v, buf + i * src_esize, src_esize);
                    if (sat_signed) {
                        int64_t sv;
                        if      (src_esize == 2) sv = (int64_t)(int16_t)(uint16_t)v;
                        else if (src_esize == 4) sv = (int64_t)(int32_t)(uint32_t)v;
                        else                     sv = (int64_t)v;
                        if (sv < s_lo) sv = s_lo;
                        if (sv > s_hi) sv = s_hi;
                        v = (uint64_t)sv;
                    } else if (sat_unsigned) {
                        if (v > max_u) v = max_u;
                    } else if (sat_to_unsigned) {
                        int64_t sv;
                        if      (src_esize == 2) sv = (int64_t)(int16_t)(uint16_t)v;
                        else if (src_esize == 4) sv = (int64_t)(int32_t)(uint32_t)v;
                        else                     sv = (int64_t)v;
                        if (sv < 0) v = 0;
                        else if ((uint64_t)sv > max_u) v = max_u;
                        else v = (uint64_t)sv;
                    }
                    // XTN: plain truncation — low dst_esize bytes copied below.
                    memcpy(out + i * dst_esize, &v, dst_esize);
                }
                if (Q) {
                    memcpy(&cpu.v_hi[rd], out, 8);  // *2: upper half, low preserved
                } else {
                    memcpy(&cpu.v_lo[rd], out, 8);
                    cpu.v_hi[rd] = 0;
                }
                return;
            }
            default: break;  // fall through to size-based checks below
            }
            // Sub-discriminator for 1-source vector ops (REV/CNT/CMEQ#0).
            // Mask off Q (30), U (29), size (23:22), Rm (20:16), Rn (9:5), Rd (4:0).
            //
            // size field (bits 23:22) or the U bit (29). This caused:
            //   - REV64 v0.4s (size=2) to not match the REV64 constant (size=0)
            //   - REV32 (U=1) to not match the REV64 case (U=0)
            // Every REV with size != 0 or U=1 fell through and was silently
            // NOP'd. This broke byte-swapping in MD5, SHA, and any NEON
            // code using REV32/REV64 on non-8-bit data.
            uint32_t sub2 = op & 0x9F3FFC00;  // mask Q, U, size, Rm, Rn, Rd
            switch (sub2) {
            // ── REV64 (vector, U=0) / REV32 (vector, U=1) ──
            // Both have bits[21:16] = 100000, bits[15:10] = 001000.
            // U bit (29) distinguishes them: REV64 (U=0) reverses within
            // 64-bit containers, REV32 (U=1) reverses within 32-bit words.
            // Since we masked off U, we check it explicitly inside.
            case 0x0E200800: {  // bits[21:16]=10, bits[15:10]=001000
                bool is_rev32 = (op >> 29) & 1;  // U=1 → REV32
                // size field determines the element size for reversal:
                //   size=0 → 8-bit elements, size=1 → 16-bit,
                //   size=2 → 32-bit, size=3 → 64-bit (REV64 only, reserved for REV32)
                // REV64 reverses elements within each 64-bit container.
                // REV32 reverses elements within each 32-bit word.
                int esize = (1 << size);  // bytes per element
                int container = is_rev32 ? 4 : 8;  // REV32: 32-bit, REV64: 64-bit
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                int nbytes = Q ? 16 : 8;
                for (int i = 0; i < nbytes; i += container) {
                    // Reverse elements within this container
                    for (int j = 0; j < container / 2; j += esize) {
                        for (int k = 0; k < esize; k++) {
                            std::swap(buf[i + j + k],
                                      buf[i + container - esize - j + k]);
                        }
                    }
                }
                memcpy(&cpu.v_lo[rd], buf, 8);
                if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── REV16 (vector) ──
            case 0x0E201800: {
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                int nbytes = Q ? 16 : 8;
                for (int i = 0; i < nbytes; i += 2) std::swap(buf[i], buf[i+1]);
                memcpy(&cpu.v_lo[rd], buf, 8);
                if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── CNT / NOT / RBIT (vector) ──
            // The sub2 mask (0x9F3FFC00) masks off size (bit 22) and U
            // (bit 29), so CNT/NOT/RBIT all collapse to 0x0E205800:
            //   CNT  = 0x0E205800 (U=0, size=0) — popcount per byte
            //   NOT  = 0x6E205800 (U=1, size=0) — bitwise invert
            //   RBIT = 0x6E605800 (U=1, size=1) — bit-reverse per byte
            // v0 silently ran popcount for RBIT (vrbitq_u8 gave 0 1 1 2
            // instead of 0 128 64). Distinguish by the raw op bits.
            case 0x0E205800: {
                bool is_rbit = ((op >> 22) & 1) == 1;   // size=1
                bool is_not  = !is_rbit && ((op >> 29) & 1) == 1;
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                int nbytes = Q ? 16 : 8;
                for (int i = 0; i < nbytes; i++) {
                    if (is_rbit) {
                        // Reverse the 8 bits of each byte.
                        uint8_t v = buf[i];
                        v = ((v & 0xAA) >> 1) | ((v & 0x55) << 1);
                        v = ((v & 0xCC) >> 2) | ((v & 0x33) << 2);
                        v = ((v & 0xF0) >> 4) | ((v & 0x0F) << 4);
                        buf[i] = v;
                    } else if (is_not) {
                        buf[i] = ~buf[i];
                    } else {
                        buf[i] = __builtin_popcount(buf[i]);
                    }
                }
                memcpy(&cpu.v_lo[rd], buf, 8);
                if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── CMEQ vs zero ──
            // but actual CMEQ #0 encoding (e.g. 0x4e209801) has bits[15:10]=0x26.
            // The old case NEVER matched — CMEQ #0 was silently NOP'd, breaking
            // glibc's strlen SIMD path. Corrected to 0x0E209800.
            case 0x0E209800: {
                int esize = (size == 0) ? 1 : (size == 1 ? 2 : (size == 2 ? 4 : 8));
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                for (int i = 0; i < elems; i++) {
                    bool is_zero = true;
                    for (int b = 0; b < esize; b++) {
                        if (buf[i*esize + b] != 0) { is_zero = false; break; }
                    }
                    memset(buf + i*esize, is_zero ? 0xFF : 0x00, esize);
                }
                memcpy(&cpu.v_lo[rd], buf, 8);
                if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── Integer compare against zero (SIMD 2-reg misc) ───────
            // CMGT/CMGE/CMLT/CMLE <Vd>.<T>, <Vn>.<T>, #0: per-element
            // signed comparison against 0; each lane becomes all-ones if
            // the test passes, all-zeros otherwise. Encodings
            // (0 Q U 01110 size 1 0 Rn opcode Rd, opcode = bits[15:10]):
            //   CMGT (signed >):  0x0E208800 (U=0, opcode 100010)
            //   CMGE (signed >=): 0x2E208800 (U=1, opcode 100010)
            //   CMLE (signed <=): 0x2E209800 (U=1, opcode 100110)
            //   CMLT (signed <):  0x0E20A800 (U=0, opcode 101010)
            // The size bits (23:22) land in sub_noq, so each size variant
            // is its own case label. The source register (Vn) is in the
            // 2-reg-misc Rn field (bits[9:5]) which the decoder maps to
            // `rn`. These were previously unhandled → DecodeError (SIGILL)
            // — Qt5Core's text handling lowers `v<0` to CMLT #0.
            case 0x0E208800: case 0x0E608800: case 0x0EA08800: case 0x0EE08800:
            case 0x2E208800: case 0x2E608800: case 0x2EA08800: case 0x2EE08800:
            case 0x2E209800: case 0x2E609800: case 0x2EA09800: case 0x2EE09800:
            case 0x0E20A800: case 0x0E60A800: case 0x0EA0A800: case 0x0EE0A800: {
                int esize = 1 << size;
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                int opc = (op >> 10) & 0x3F;
                bool is_u = U;
                // opcode 100010: CMGT(U=0) / CMGE(U=1); 101010: CMLT(U=0);
                // 100110: CMLE(U=1). cmp: 0=GT 1=GE 2=LT 3=LE.
                int cmp;
                if (opc == 0x22)      cmp = is_u ? 1 : 0;
                else if (opc == 0x2A) cmp = 2;
                else                  cmp = 3;
                for (int i = 0; i < elems; i++) {
                    int64_t v = 0;
                    memcpy(&v, buf + i * esize, esize);
                    int64_t sv = (esize == 8) ? v
                                 : (esize == 4) ? (int32_t)v
                                 : (esize == 2) ? (int16_t)v
                                                : (int8_t)v;
                    bool pass = (cmp == 0) ? (sv > 0)
                              : (cmp == 1) ? (sv >= 0)
                              : (cmp == 2) ? (sv < 0)
                                           : (sv <= 0);
                    memset(buf + i * esize, pass ? 0xFF : 0x00, esize);
                }
                memcpy(&cpu.v_lo[rd], buf, 8);
                if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ── SMAXV/SMINV/UMAXV/UMINV (across-lanes reduction) ─────
            // Compute the maximum (bit16=0) or minimum (bit16=1) of all
            // elements of Vn and write the scalar result to element 0 of
            // Rd (upper bits zeroed). U selects signed (SMAXV/SMINV) vs
            // unsigned (UMAXV/UMINV). size selects 8/16/32-bit elements;
            // Q selects 8 vs 16 bytes of source lanes.
            // Encoding: 0 Q U 01110 0 size 00000 <max/min> 10101 0 Vn Rd
            //   SMAXV s0,v1.4s = 0x4EB0A820, SMINV = 0x4EB1A820
            // The sub2 mask (0x9F3FFC00) strips Q/U/size, so both size
            // variants land on these two labels; read size/U off raw op.
            case 0x0E30A800:
            case 0x0E31A800: {
                int esize = 1 << size;          // 1 (B), 2 (H), 4 (S)
                int elems = (Q ? 16 : 8) / esize;
                bool is_unsigned = (op >> 29) & 1;
                bool is_min = (op >> 16) & 1;
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                uint64_t best = 0;
                memcpy(&best, buf, esize);
                for (int i = 1; i < elems; i++) {
                    uint64_t v = 0;
                    memcpy(&v, buf + i * esize, esize);
                    if (is_unsigned) {
                        if (is_min ? (v < best) : (v > best)) best = v;
                    } else {
                        int64_t sv, sb;
                        if (esize == 1)      { sv = (int8_t)v;       sb = (int8_t)best; }
                        else if (esize == 2) { sv = (int16_t)v;      sb = (int16_t)best; }
                        else                 { sv = (int32_t)v;      sb = (int32_t)best; }
                        if (is_min ? (sv < sb) : (sv > sb)) best = v;
                    }
                }
                cpu.v_lo[rd] = 0;
                cpu.v_hi[rd] = 0;
                memcpy(&cpu.v_lo[rd], &best, esize);
                return;
            }
            default: break;
            }
            // UADDLV — unique mask shape.
            if ((op & 0xBF3FFC00) == 0x0E31B800) {
                int esize = 1 << size;
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                uint64_t sum = 0;
                for (int i = 0; i < elems; i++) {
                    uint64_t v = 0;
                    memcpy(&v, buf + i * esize, esize);
                    sum += v;
                }
                cpu.v_lo[rd] = sum;
                cpu.v_hi[rd] = 0;
                return;
            }
            // INS (element, vector): mov vd.<T>[i], vn.<T>[j]
            // Encoding: 0 Q 0 0 1 1 1 0 0 0 0 imm5 0 <src_byte_off[3:0]> 1 Rn Rd
            // Same 0x2E000000 prefix as EXT but bit10=1 (EXT keeps bit10=0
            // and puts imm4 in bits[14:11]). src byte offset = j*esize.
            // GCC lowers vextq_u8 to a 2-source TBL whose index vector is
            // built with these INS moves, so this must not fall into EXT.
            if ((op & 0xBFE00000) == 0x2E000000 && (op & 0x400) != 0) {
                uint8_t imm5 = (op >> 16) & 0x1F;
                int esize, didx;
                // imm5 = (didx << (size+1)) | (1 << size): the lowest set
                // bit selects the element size, the upper bits hold the
                // destination element index.
                if (imm5 & 0x01) { esize = 1; didx = imm5 >> 1; }
                else if (imm5 & 0x02) { esize = 2; didx = imm5 >> 2; }
                else if (imm5 & 0x04) { esize = 4; didx = imm5 >> 3; }
                else if (imm5 & 0x08) { esize = 8; didx = imm5 >> 4; }
                else throw DecodeError(cpu.pc, inst);
                int src_off = (op >> 11) & 0xF;
                int sidx = src_off / esize;
                uint64_t src_val;
                int elems_per_qword = 8 / esize;
                if (sidx < elems_per_qword) {
                    const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_lo[rn]);
                    memcpy(&src_val, p + sidx * esize, esize);
                } else {
                    const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_hi[rn]);
                    memcpy(&src_val, p + (sidx - elems_per_qword) * esize, esize);
                }
                uint8_t* d = (didx < elems_per_qword)
                    ? reinterpret_cast<uint8_t*>(&cpu.v_lo[rd])
                    : reinterpret_cast<uint8_t*>(&cpu.v_hi[rd]);
                memcpy(d + (didx % elems_per_qword) * esize, &src_val, esize);
                return;
            }
            // EXT (extract) — v0 mask 0xFFE00000 val 0x6E000000
            // MISSED Q=0 form. Fixed: strip Q from the comparison.
            if ((op & 0xBFE00000) == 0x2E000000) {
                uint8_t imm4 = (op >> 11) & 0xF;
                uint8_t buf[32];
                // Per ARM ARM: concat = Vn:Vm where Vm is the low 128 bits
                // and Vn is the high 128 bits. So buf = [rm:rn].
                memcpy(buf, &cpu.v_lo[rm], 8);
                memcpy(buf + 8, &cpu.v_hi[rm], 8);
                memcpy(buf + 16, &cpu.v_lo[rn], 8);
                memcpy(buf + 24, &cpu.v_hi[rn], 8);
                uint8_t out[16] = {0};
                int nbytes = Q ? 16 : 8;
                memcpy(out, buf + imm4, nbytes);
                memcpy(&cpu.v_lo[rd], out, 8);
                if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // ZIP1/ZIP2 / UZP1/UZP2 / TRN1/TRN2 — permute pairs.
            // Encoding: 0 Q size 01110 00 Rm 0 opc 10 Rn Rd (U=0).
            // opc6 = bits[15:10] (empirically from gas):
            //   UZP1=0x06 TRN1=0x0A ZIP1=0x0E
            //   UZP2=0x16 TRN2=0x1A ZIP2=0x1E
            if (((op >> 24) & 0x1F) == 0x0E && ((op >> 21) & 1) == 0
                && ((op >> 29) & 1) == 0) {
                uint8_t opc6 = (op >> 10) & 0x3F;
                if (opc6 == 0x06 || opc6 == 0x0A || opc6 == 0x0E
                    || opc6 == 0x16 || opc6 == 0x1A || opc6 == 0x1E) {
                    int esize = 1 << size;
                    int pairs = (Q ? 16 : 8) / (esize * 2);
                    if (pairs < 1) pairs = 1;
                    uint8_t a[16], b[16], out[16] = {0};
                    memcpy(a, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(a + 8, &cpu.v_hi[rn], 8);
                    memcpy(b, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(b + 8, &cpu.v_hi[rm], 8);
                    int part = (opc6 == 0x16 || opc6 == 0x1A || opc6 == 0x1E) ? 1 : 0;
                    bool is_zip = (opc6 == 0x0E || opc6 == 0x1E);
                    bool is_uzp = (opc6 == 0x06 || opc6 == 0x16);
                    // TRN otherwise.
                    if (is_zip) {
                        // ZIP1: interleave low halves; ZIP2: high halves.
                        int base = part * pairs * esize;
                        for (int i = 0; i < pairs; i++) {
                            memcpy(out + (2 * i) * esize, a + base + i * esize, esize);
                            memcpy(out + (2 * i + 1) * esize, b + base + i * esize, esize);
                        }
                    } else if (is_uzp) {
                        // UZP1: even elements from a||b; UZP2: odd.
                        int total = pairs * 2;
                        int out_i = 0;
                        for (int src = 0; src < 2; src++) {
                            const uint8_t* s = src ? b : a;
                            for (int i = 0; i < total; i++) {
                                if ((i & 1) == part) {
                                    memcpy(out + out_i * esize, s + i * esize, esize);
                                    out_i++;
                                }
                            }
                        }
                    } else {
                        // TRN1/TRN2: transpose 2x2 element pairs.
                        for (int i = 0; i < pairs; i++) {
                            const uint8_t* s0 = a + (2 * i + part) * esize;
                            const uint8_t* s1 = b + (2 * i + part) * esize;
                            memcpy(out + (2 * i) * esize, s0, esize);
                            memcpy(out + (2 * i + 1) * esize, s1, esize);
                        }
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
            }
            // Advanced SIMD modified immediate (MOVI/MVNI/ORR/BIC + MSL).
            // Encoding: 0 Q op 01111 0 abc cmode o2 1 defgh Rd
            // Must require immh==0 (bits[22:19]) so shift-by-immediate
            // encodings (SHL/USHR/SSHR, which share bits[28:24]=01111) are
            // not stolen. Real MOVI/MVNI/ORR/BIC always have bits[22:19]==0.
            //
            // NOTE: no SHRN exclusion (bits[15:10]=100001) is needed here:
            // every VALID SHRN has immh (bits[22:19]) >= 1 (its immh:immb
            // field encodes the source size + shift), so the bits[22:19]==0
            // test already separates SHRN from this block. A crude
            // `bits[15:10] != 0x21` clause instead REJECTED cmode=8 MOVI
            // (16-bit LSL #0), whose bits[15:10] = 1000 o2 1 = 100001 == the
            // SHRN opcode, silently zeroing `movi vN.4h, #imm` in both the
            // interp and the JIT (0x0F058560 = movi v6.4h, #0xab returned 0).
            //
            // cmode/op decode per ARM ARM:
            //   0xx0 op=0/1 → MOVI/MVNI 32-bit LSL #(cmode[2:1]*8)
            //   0xx1 op=0/1 → ORR /BIC  32-bit LSL #(cmode[2:1]*8)
            //   10x0 op=0/1 → MOVI/MVNI 16-bit LSL #(cmode[1]*8)
            //   10x1 op=0/1 → ORR /BIC  16-bit LSL #(cmode[1]*8)
            //   110x op=0/1 → MOVI/MVNI 32-bit MSL #((cmode[0]+1)*8)
            //   1110 op=0   → MOVI 8-bit (replicate imm8 to every byte)
            //   1110 op=1   → MOVI 64-bit (each imm8 bit → 0x00/0xFF byte)
            //   1111        → FMOV (handled elsewhere / reserved here)
            // Use immh = bits[22:19] (not [23:20]): SSHLL/SHRN keep bit23=0
            // so bits[23:20] can look like "immh==0" while bits[22:19]!=0.
            // Real MOVI/MVNI/ORR/BIC always have bits[22:19]==0.
            if (((op & ~((1u << 30) | (1u << 29))) & 0xFF800C00) == 0x0F000400
                && ((op >> 19) & 0xF) == 0) {
                uint8_t cmode = (op >> 12) & 0xF;
                uint8_t imm8 = static_cast<uint8_t>(
                    (((op >> 16) & 0x7) << 5) | ((op >> 5) & 0x1F));
                bool opbit = (op >> 29) & 1;  // 'op' in ARM encoding
                uint8_t dst[16];
                memcpy(dst, &cpu.v_lo[rd], 8);
                memcpy(dst + 8, &cpu.v_hi[rd], 8);

                auto replicate_u32 = [&](uint32_t lane) {
                    int n = Q ? 4 : 2;
                    for (int i = 0; i < n; i++)
                        memcpy(dst + i * 4, &lane, 4);
                    if (!Q) memset(dst + 8, 0, 8);
                };
                auto replicate_u16 = [&](uint16_t lane) {
                    int n = Q ? 8 : 4;
                    for (int i = 0; i < n; i++)
                        memcpy(dst + i * 2, &lane, 2);
                    if (!Q) memset(dst + 8, 0, 8);
                };
                auto apply_or_bic_u32 = [&](uint32_t imm, bool bic) {
                    int n = Q ? 4 : 2;
                    for (int i = 0; i < n; i++) {
                        uint32_t v;
                        memcpy(&v, dst + i * 4, 4);
                        v = bic ? (v & ~imm) : (v | imm);
                        memcpy(dst + i * 4, &v, 4);
                    }
                    if (!Q) memset(dst + 8, 0, 8);
                };
                auto apply_or_bic_u16 = [&](uint16_t imm, bool bic) {
                    int n = Q ? 8 : 4;
                    for (int i = 0; i < n; i++) {
                        uint16_t v;
                        memcpy(&v, dst + i * 2, 2);
                        v = bic ? static_cast<uint16_t>(v & ~imm)
                                : static_cast<uint16_t>(v | imm);
                        memcpy(dst + i * 2, &v, 2);
                    }
                    if (!Q) memset(dst + 8, 0, 8);
                };

                if ((cmode & 0x9) == 0x0) {
                    // 0xx0: MOVI/MVNI 32-bit LSL
                    uint32_t imm = static_cast<uint32_t>(imm8)
                                   << (((cmode >> 1) & 3) * 8);
                    if (opbit) imm = ~imm;
                    replicate_u32(imm);
                } else if ((cmode & 0x9) == 0x1) {
                    // 0xx1: ORR/BIC 32-bit LSL
                    uint32_t imm = static_cast<uint32_t>(imm8)
                                   << (((cmode >> 1) & 3) * 8);
                    apply_or_bic_u32(imm, opbit);
                } else if ((cmode & 0xD) == 0x8) {
                    // 10x0: MOVI/MVNI 16-bit LSL
                    uint16_t imm = static_cast<uint16_t>(
                        static_cast<uint16_t>(imm8) << ((cmode & 2) ? 8 : 0));
                    if (opbit) imm = static_cast<uint16_t>(~imm);
                    replicate_u16(imm);
                } else if ((cmode & 0xD) == 0x9) {
                    // 10x1: ORR/BIC 16-bit LSL
                    uint16_t imm = static_cast<uint16_t>(
                        static_cast<uint16_t>(imm8) << ((cmode & 2) ? 8 : 0));
                    apply_or_bic_u16(imm, opbit);
                } else if ((cmode & 0xE) == 0xC) {
                    // 110x: MOVI/MVNI 32-bit MSL #8 or #16
                    int shift = ((cmode & 1) + 1) * 8;
                    uint32_t ones = (shift == 16) ? 0xFFFFu : 0xFFu;
                    uint32_t imm = (static_cast<uint32_t>(imm8) << shift) | ones;
                    if (opbit) imm = ~imm;
                    replicate_u32(imm);
                } else if (cmode == 0xE && !opbit) {
                    // MOVI 8-bit: replicate imm8 across all bytes
                    uint64_t val = 0;
                    for (int i = 0; i < 8; i++)
                        val |= static_cast<uint64_t>(imm8) << (i * 8);
                    cpu.v_lo[rd] = val;
                    cpu.v_hi[rd] = Q ? val : 0;
                    return;
                } else if (cmode == 0xE && opbit) {
                    // MOVI 64-bit: each imm8 bit selects 0x00 or 0xFF byte
                    uint64_t val = 0;
                    for (int i = 0; i < 8; i++) {
                        if (imm8 & (1u << i))
                            val |= 0xFFULL << (i * 8);
                    }
                    cpu.v_lo[rd] = val;
                    cpu.v_hi[rd] = Q ? val : 0;
                    return;
                } else {
                    // cmode=0xF FMOV (vector, immediate): floating-point
                    // constant.  AdvSIMDExpandImm; op bit picks precision:
                    //   op=1 → 64-bit double, replicated to .2D (Q=1)
                    //   op=0 → 32-bit single, replicated to .2S/.4S
                    if (opbit) {
                        uint64_t val = (static_cast<uint64_t>(imm8 & 0x3F)) << 48;
                        if (imm8 & 0x80) val |= 0x8000000000000000ULL;   // sign
                        if (imm8 & 0x40) val |= 0x3FC0000000000000ULL;   // exp
                        else             val |= 0x4000000000000000ULL;
                        cpu.v_lo[rd] = val;
                        cpu.v_hi[rd] = Q ? val : 0;
                    } else {
                        uint32_t val = (static_cast<uint32_t>(imm8 & 0x3F)) << 19;
                        if (imm8 & 0x80) val |= 0x80000000u;             // sign
                        if (imm8 & 0x40) val |= 0x1F000000u;             // exp
                        else             val |= 0x40000000u;
                        replicate_u32(val);
                        memcpy(&cpu.v_lo[rd], dst, 8);
                        memcpy(&cpu.v_hi[rd], dst + 8, 8);
                        return;
                    }
                    return;
                }
                memcpy(&cpu.v_lo[rd], dst, 8);
                memcpy(&cpu.v_hi[rd], dst + 8, 8);
                return;
            }
            // SHL (vector, immediate) — mask 0xBF00FC00 excludes Q.
            // Encoding: Q 0 1 1 0 1 1 1 1 0 immh immb 0 1 0 1 0 0 Rn Rd
            //
            // Element size from immh (the position of the highest set bit):
            //   immh=0     → 8-bit  (esize=1)
            //   immh=1     → 16-bit (esize=2)
            //   immh=2,3   → 32-bit (esize=4)
            //   immh=4-7   → 64-bit (esize=8)
            //   immh=8-15  → reserved (treat as 64-bit)
            // Shift = immh:immb - esize_bits
            //   (e.g. shl v0.4s, #4 → immh:immb=0x24=36, esize=32, shift=36-32=4)
            //
            // 1. immh extracted as (op>>19)&0xF — off by one bit (should be >>20).
            // 2. Element size rule was wrong: used 'immh < N' thresholds that
            //    gave 16-bit for immh=3 instead of 32-bit. The correct rule is
            //    based on the highest set bit position of immh.
            // These broke ALL vector shifts — e.g. `ushr v0.4s, #4` was treated
            // as a 16-bit shift, producing 0x0000 instead of 0x01000000.
            if ((op & 0xBF00FC00) == 0x0F005400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int esize, shift;
                if (immh == 0) { esize = 1; }
                else if (immh == 1) { esize = 2; }
                else if (immh <= 3) { esize = 4; }
                else { esize = 8; }
                shift = ((immh << 4) | immb) - (esize * 8);
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                for (int i = 0; i < elems; i++) {
                    uint64_t v = 0;
                    memcpy(&v, buf + i*esize, esize);
                    v <<= shift;
                    v &= (esize == 8) ? ~0ULL : ((1ULL << (esize*8)) - 1);
                    memcpy(buf + i*esize, &v, esize);
                }
                memcpy(&cpu.v_lo[rd], buf, 8);
                if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // Shift = (2 * esize_bits) - immh:immb.
            // (rc.1 fixed immh extraction + element-size rule — see SHL above.)
            if ((op & 0xBF00FC00) == 0x2F000400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int esize, shift;
                if (immh == 0) { esize = 1; }
                else if (immh == 1) { esize = 2; }
                else if (immh <= 3) { esize = 4; }
                else { esize = 8; }
                shift = (2 * esize * 8) - ((immh << 4) | immb);
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                for (int i = 0; i < elems; i++) {
                    uint64_t v = 0;
                    memcpy(&v, buf + i*esize, esize);
                    if (shift >= esize * 8) v = 0;  // shift == esize*8 clears (avoids UB `>> 64`)
                    else v >>= shift;
                    memcpy(buf + i*esize, &v, esize);
                }
                memcpy(&cpu.v_lo[rd], buf, 8);
                if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // SSHR (vector, immediate, signed) — mask 0xBF00FC00 excludes Q.
            // Encoding: Q 0 1 1 0 1 1 1 1 0 immh immb 0 1 0 0 0 0 Rn Rd
            // Shift = (2 * esize_bits) - immh:immb (same as USHR, but
            // the shift is arithmetic — the sign bit is propagated).
            //
            // The MOVI/shift ambiguity check above (line ~1981) catches
            // this encoding only when immh == 0 (MOVI); for immh != 0
            // (actual SSHR), control fell through past the USHR handler
            // (which has U=1, 0x2F...) and past the SHL handler (which
            // has a different low byte, 0x0F005400), landing in the
            // generic "unknown instruction" NOP path. Result: every
            // vector SSHR-by-immediate was silently a no-op, leaving
            // Vd unchanged. This broke `sshr v0.8h, v0.8h, #2` etc.
            // under both interpreter and JIT (the JIT routes SIMD_SHL/
            // SIMD_USHR/SIMD_SSHR to CALL_INTERP for the executor).
            if ((op & 0xBF00FC00) == 0x0F000400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int esize, shift;
                if (immh == 0) { esize = 1; }       // unreachable (MOVI guard above)
                else if (immh == 1) { esize = 2; }
                else if (immh <= 3) { esize = 4; }
                else { esize = 8; }
                shift = (2 * esize * 8) - ((immh << 4) | immb);
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                bool over = (shift >= esize * 8);  // shift == esize*8 sign-fills (avoids UB `>> 64`)
                for (int i = 0; i < elems; i++) {
                    if (esize == 1) {
                        int8_t v; memcpy(&v, buf + i, 1);
                        if (over) v = (v < 0) ? -1 : 0; else v >>= shift;
                        memcpy(buf + i, &v, 1);
                    } else if (esize == 2) {
                        int16_t v; memcpy(&v, buf + i*2, 2);
                        if (over) v = (v < 0) ? -1 : 0; else v >>= shift;
                        memcpy(buf + i*2, &v, 2);
                    } else if (esize == 4) {
                        int32_t v; memcpy(&v, buf + i*4, 4);
                        if (over) v = (v < 0) ? -1 : 0; else v >>= shift;
                        memcpy(buf + i*4, &v, 4);
                    } else {
                        int64_t v; memcpy(&v, buf + i*8, 8);
                        if (over) v = (v < 0) ? -1 : 0; else v >>= shift;
                        memcpy(buf + i*8, &v, 8);
                    }
                }
                memcpy(&cpu.v_lo[rd], buf, 8);
                if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // USRA (vector, immediate, accumulate) — mask 0xBF00FC00.
            // USRA Vd.<T>, Vn.<T>, #shift → Vd += (Vn >> #shift).
            // Used by MD5 to implement vector ROTL via
            //   ROTL(x,n) = USRA(x << n, 32-n).
            // The rounding variants URSRA/SRSRA (0x2F003400/0x0F003400,
            // bit13 set) are handled just below.
            if ((op & 0xBF00FC00) == 0x2F001400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int esize, shift;
                if (immh == 0) { esize = 1; }
                else if (immh == 1) { esize = 2; }
                else if (immh <= 3) { esize = 4; }
                else { esize = 8; }
                shift = (2 * esize * 8) - ((immh << 4) | immb);
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf[16];
                uint8_t acc[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                memcpy(acc, &cpu.v_lo[rd], 8);
                if (Q) memcpy(acc + 8, &cpu.v_hi[rd], 8);
                for (int i = 0; i < elems; i++) {
                    uint64_t v = 0, a = 0;
                    memcpy(&v, buf + i*esize, esize);
                    memcpy(&a, acc + i*esize, esize);
                    if (shift >= esize * 8) v = 0;  // shift == esize*8 clears (avoids UB `>> 64`)
                    else v >>= shift;
                    a += v;  // accumulate (wraps per element width)
                    a &= (esize == 8) ? ~0ULL : ((1ULL << (esize*8)) - 1);
                    memcpy(acc + i*esize, &a, esize);
                }
                memcpy(&cpu.v_lo[rd], acc, 8);
                if (Q) memcpy(&cpu.v_hi[rd], acc + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // SSRA (vector, immediate, signed accumulate) — 0x0F001400.
            // Same as USRA but arithmetic (signed) shift right.
            if ((op & 0xBF00FC00) == 0x0F001400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int esize, shift;
                if (immh == 0) { esize = 1; }
                else if (immh == 1) { esize = 2; }
                else if (immh <= 3) { esize = 4; }
                else { esize = 8; }
                shift = (2 * esize * 8) - ((immh << 4) | immb);
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf[16];
                uint8_t acc[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                memcpy(acc, &cpu.v_lo[rd], 8);
                if (Q) memcpy(acc + 8, &cpu.v_hi[rd], 8);
                bool over = (shift >= esize * 8);  // shift == esize*8 sign-fills (avoids UB `>> 64`)
                for (int i = 0; i < elems; i++) {
                    if (esize == 1) {
                        int8_t v; memcpy(&v, buf+i, 1);
                        uint8_t a; memcpy(&a, acc+i, 1);
                        if (over) v = (v < 0) ? -1 : 0; else v >>= shift; a += (uint8_t)v;
                        memcpy(acc+i, &a, 1);
                    } else if (esize == 2) {
                        int16_t v; memcpy(&v, buf+i*2, 2);
                        uint16_t a; memcpy(&a, acc+i*2, 2);
                        if (over) v = (v < 0) ? -1 : 0; else v >>= shift; a += (uint16_t)v;
                        memcpy(acc+i*2, &a, 2);
                    } else if (esize == 4) {
                        int32_t v; memcpy(&v, buf+i*4, 4);
                        uint32_t a; memcpy(&a, acc+i*4, 4);
                        if (over) v = (v < 0) ? -1 : 0; else v >>= shift; a += (uint32_t)v;
                        memcpy(acc+i*4, &a, 4);
                    } else {
                        int64_t v; memcpy(&v, buf+i*8, 8);
                        uint64_t a; memcpy(&a, acc+i*8, 8);
                        if (over) v = (v < 0) ? -1 : 0; else v >>= shift; a += (uint64_t)v;
                        memcpy(acc+i*8, &a, 8);
                    }
                }
                memcpy(&cpu.v_lo[rd], acc, 8);
                if (Q) memcpy(&cpu.v_hi[rd], acc + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // URSRA (vector, immediate, unsigned rounding accumulate) —
            // 0x2F003400. URSRA Vd.<T>, Vn.<T>, #shift → Vd += round(Vn >> #shift)
            // where the ARM RShr rounding is (x + 2^(shift-1)) >> shift
            // (round-half-up, full-precision add). For shift == esize*8 this
            // collapses to (x >= 2^(esize*8-1)) ? 1 : 0 (top bit set → the
            // rounding carries out to 1). Differs from USRA only in the
            // +2^(shift-1) rounding term.
            if ((op & 0xBF00FC00) == 0x2F003400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int esize, shift;
                if (immh == 0) { esize = 1; }
                else if (immh == 1) { esize = 2; }
                else if (immh <= 3) { esize = 4; }
                else { esize = 8; }
                shift = (2 * esize * 8) - ((immh << 4) | immb);
                int esize_bits = esize * 8;
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf[16];
                uint8_t acc[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                memcpy(acc, &cpu.v_lo[rd], 8);
                if (Q) memcpy(acc + 8, &cpu.v_hi[rd], 8);
                uint64_t mask = (esize == 8) ? ~0ULL : ((1ULL << esize_bits) - 1);
                for (int i = 0; i < elems; i++) {
                    uint64_t v = 0, a = 0;
                    memcpy(&v, buf + i*esize, esize);
                    memcpy(&a, acc + i*esize, esize);
                    uint64_t result;
                    if (shift == 0) {
                        result = v;  // shift 0 → no rounding, value passes through
                    } else if (shift >= esize_bits) {
                        // (x + 2^(esize-1)) >> esize == 1 iff top bit set.
                        result = (v >= (1ULL << (esize_bits - 1))) ? 1 : 0;
                    } else {
                        // (x + 2^(shift-1)) >> shift decomposed without a
                        // widening add (avoids 64-bit overflow for esize=8):
                        // result = (x >> shift) + (low_bits >= 2^(shift-1)).
                        uint64_t low = v & ((1ULL << shift) - 1);
                        result = (v >> shift) + ((low >= (1ULL << (shift - 1))) ? 1 : 0);
                    }
                    a += result;  // accumulate (wraps per element width)
                    a &= mask;
                    memcpy(acc + i*esize, &a, esize);
                }
                memcpy(&cpu.v_lo[rd], acc, 8);
                if (Q) memcpy(&cpu.v_hi[rd], acc + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // SRSRA (vector, immediate, signed rounding accumulate) —
            // 0x0F003400. Signed variant of URSRA: Vd += round(Vn >>> #shift)
            // with an arithmetic (sign-extending) shift and the same
            // +2^(shift-1) rounding increment on the signed value. For
            // shift == esize*8 the rounded result is always 0 (every signed
            // element |x| < 2^(esize*8-1) once rounded).
            if ((op & 0xBF00FC00) == 0x0F003400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int esize, shift;
                if (immh == 0) { esize = 1; }
                else if (immh == 1) { esize = 2; }
                else if (immh <= 3) { esize = 4; }
                else { esize = 8; }
                shift = (2 * esize * 8) - ((immh << 4) | immb);
                int esize_bits = esize * 8;
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf[16];
                uint8_t acc[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                memcpy(acc, &cpu.v_lo[rd], 8);
                if (Q) memcpy(acc + 8, &cpu.v_hi[rd], 8);
                uint64_t mask = (esize == 8) ? ~0ULL : ((1ULL << esize_bits) - 1);
                for (int i = 0; i < elems; i++) {
                    uint64_t vbits = 0, a = 0;
                    memcpy(&vbits, buf + i*esize, esize);
                    memcpy(&a, acc + i*esize, esize);
                    int64_t v;
                    if (esize == 1) v = (int8_t)(uint8_t)vbits;
                    else if (esize == 2) v = (int16_t)(uint16_t)vbits;
                    else if (esize == 4) v = (int32_t)(uint32_t)vbits;
                    else v = (int64_t)vbits;
                    int64_t result;
                    if (shift == 0) {
                        result = v;  // shift 0 → no rounding, value passes through
                    } else if (shift >= esize_bits) {
                        result = 0;  // (x + 2^(esize-1)) >> esize == 0 for all signed x
                    } else {
                        // Rounding increment on the signed value, decomposed
                        // (avoids widening-add overflow): result = (x >> shift)
                        // + (low_bits >= 2^(shift-1)). x >> shift is arithmetic.
                        uint64_t low = (uint64_t)v & ((1ULL << shift) - 1);
                        result = (v >> shift) + ((low >= (1ULL << (shift - 1))) ? 1 : 0);
                    }
                    a += (uint64_t)result;  // accumulate (wraps per element width)
                    a &= mask;
                    memcpy(acc + i*esize, &a, esize);
                }
                memcpy(&cpu.v_lo[rd], acc, 8);
                if (Q) memcpy(&cpu.v_hi[rd], acc + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // SLI (vector, immediate, shift left insert) — 0x2F005400.
            // SLI Vd.<T>, Vn.<T>, #shift → Vd = (Vn << shift) | (Vd & ((1<<shift)-1)).
            // The source shifts left; the destination's low `shift` bits are
            // retained in place (merged below the shifted source). Note: this
            // is NOT ROTL — ROTL(x,n) = (x<<n)|(x>>(w-n)) differs when Vd!=x.
            if ((op & 0xBF00FC00) == 0x2F005400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int esize, shift;
                if (immh == 0) { esize = 1; }
                else if (immh == 1) { esize = 2; }
                else if (immh <= 3) { esize = 4; }
                else { esize = 8; }
                shift = ((immh << 4) | immb) - (esize * 8);
                int esize_bits = esize * 8;
                int elems = (Q ? 16 : 8) / esize;
                uint8_t vn[16], vd[16];
                memcpy(vn, &cpu.v_lo[rn], 8);
                if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                memcpy(vd, &cpu.v_lo[rd], 8);
                if (Q) memcpy(vd + 8, &cpu.v_hi[rd], 8);
                uint64_t mask = (esize == 8) ? ~0ULL : ((1ULL << esize_bits) - 1);
                static bool sli_dbg_ = (getenv("BIFROST_LD2_DBG") != nullptr);
                if (sli_dbg_) {
                    fprintf(stderr, "[SLI-DBG] op=0x%08x esize=%d shift=%d elems=%d\n", op, esize, shift, elems);
                    fprintf(stderr, "[SLI-DBG]   vn:"); for (int i = 0; i < elems*esize; i++) fprintf(stderr, " %02x", vn[i]); fprintf(stderr, "\n");
                    fprintf(stderr, "[SLI-DBG]   vd:"); for (int i = 0; i < elems*esize; i++) fprintf(stderr, " %02x", vd[i]); fprintf(stderr, "\n");
                }
                for (int i = 0; i < elems; i++) {
                    uint64_t n = 0, d = 0;
                    memcpy(&n, vn + i*esize, esize);
                    memcpy(&d, vd + i*esize, esize);
                    uint64_t r;
                    if (shift <= 0) {
                        r = n & mask;
                    } else if (shift >= esize_bits) {
                        r = d & mask;
                    } else {
                        uint64_t lo_mask = (1ULL << shift) - 1;
                        r = ((n << shift) & mask) | (d & lo_mask);
                    }
                    memcpy(vd + i*esize, &r, esize);
                }
                if (sli_dbg_) {
                    fprintf(stderr, "[SLI-DBG]   out:"); for (int i = 0; i < elems*esize; i++) fprintf(stderr, " %02x", vd[i]); fprintf(stderr, "\n");
                }
                memcpy(&cpu.v_lo[rd], vd, 8);
                if (Q) memcpy(&cpu.v_hi[rd], vd + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // SRI (vector, immediate, shift right insert) — 0x2F004400.
            // SRI Vd.<T>, Vn.<T>, #shift → Vd = (Vn >> shift) | (Vd & (~((1<<(esize-shift))-1))).
            // The source shifts right; the destination's top `shift` bits are
            // retained in place (merged above the shifted source). Note: this
            // is NOT ROTR — ROTR(x,n) = (x>>n)|(x<<(w-n)) differs when Vd!=x.
            if ((op & 0xBF00FC00) == 0x2F004400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int esize, shift;
                if (immh == 0) { esize = 1; }
                else if (immh == 1) { esize = 2; }
                else if (immh <= 3) { esize = 4; }
                else { esize = 8; }
                int esize_bits = esize * 8;
                shift = (2 * esize_bits) - ((immh << 4) | immb);
                int elems = (Q ? 16 : 8) / esize;
                uint8_t vn[16], vd[16];
                memcpy(vn, &cpu.v_lo[rn], 8);
                if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                memcpy(vd, &cpu.v_lo[rd], 8);
                if (Q) memcpy(vd + 8, &cpu.v_hi[rd], 8);
                uint64_t mask = (esize == 8) ? ~0ULL : ((1ULL << esize_bits) - 1);
                for (int i = 0; i < elems; i++) {
                    uint64_t n = 0, d = 0;
                    memcpy(&n, vn + i*esize, esize);
                    memcpy(&d, vd + i*esize, esize);
                    uint64_t r;
                    if (shift <= 0) {
                        r = n & mask;
                    } else if (shift >= esize_bits) {
                        r = d & mask;
                    } else {
                        uint64_t lo_keep = (1ULL << (esize_bits - shift)) - 1;
                        uint64_t hi_mask = mask & ~lo_keep;
                        r = (n >> shift) | (d & hi_mask);
                    }
                    memcpy(vd + i*esize, &r, esize);
                }
                memcpy(&cpu.v_lo[rd], vd, 8);
                if (Q) memcpy(&cpu.v_hi[rd], vd + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // Narrowing shift right (SHRN). immh determines SOURCE element size
            // (2× the destination size).
            //
            // wrong. Verified empirically by compiling `shrn v0.8b, v0.8h, #4`
// (0x0f0c8400), `shrn v0.4h, v0.4s, #4` (0x0f1c8400), and
            // `shrn v0.2s, v0.2d, #4` (0x0f3c8400):
            //   immh=0 → esize=2 (16-bit source), immh=1 → esize=4 (32-bit),
            //   immh=2,3 → esize=8 (64-bit)
            //   shift = esize*8 - (immh:immb)
            // (16-bit source), and used `2*esize*8 - immh:immb` for the shift
            // (off by esize*8). This produced wrong shift amounts, corrupting
            // the narrowed result. With immh=0, esize was set to 8 (64-bit)
            // instead of 2 (16-bit), and shift = 128-12 = 116 instead of 4.
            //
            // Q register), even when Q=0. Q=0 means the destination is 64
            // bits (SHRN), Q=1 means 128 bits (SHRN2, writes to upper half).
            if ((op & 0xBF00FC00) == 0x0F008400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int esize, shift;
                if (immh == 0) { esize = 2; }       // 16-bit source → 8-bit dest
                else if (immh == 1) { esize = 4; }   // 32-bit source → 16-bit dest
                else { esize = 8; }                   // 64-bit source → 32-bit dest
                shift = (esize * 8) - ((immh << 4) | immb);
                if (shift < 0) shift = 0;  // sanity: shift can't be negative
                // Source is ALWAYS the full 128-bit register (8/4/2 elements).
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                memcpy(buf + 8, &cpu.v_hi[rn], 8);  // always read full 128 bits
                int dst_elems = 16 / esize;  // 8 for 16-bit, 4 for 32-bit, 2 for 64-bit
                uint8_t out[16] = {0};
                for (int i = 0; i < dst_elems; i++) {
                    uint64_t v = 0;
                    memcpy(&v, buf + i * esize, esize);
                    v >>= shift;
                    memcpy(out + i * (esize / 2), &v, esize / 2);
                }
                if (Q) {
                    // SHRN2: write to upper 64 bits, preserve lower 64 bits.
                    memcpy(&cpu.v_hi[rd], out, 8);
                } else {
                    // SHRN: write to lower 64 bits, zero upper 64 bits.
                    memcpy(&cpu.v_lo[rd], out, 8);
                    cpu.v_hi[rd] = 0;
                }
                return;
            }
            // SSHLL / USHLL / SSHLL2 / USHLL2 — widen + left shift.
            // Encoding: 0 Q U 01111 0 immh[3:0] immb[2:0] 101001 Rn Rd
            // immh is bits[22:19], immb bits[18:16] (bit23 always 0).
            // Using bits[23:20] wrongly sees immh=0 for 8-bit sources.
            if (((op & ~((1u << 30) | (1u << 29))) & 0xFF00FC00) == 0x0F00A400) {
                uint8_t immh = (op >> 19) & 0xF;   // bits[22:19]
                uint8_t immb = (op >> 16) & 0x7;   // bits[18:16]
                if (immh != 0) {
                bool is_unsigned = (op >> 29) & 1;
                int src_esize, shift;
                if (immh == 1) { src_esize = 1; }
                else if (immh <= 3) { src_esize = 2; }
                else { src_esize = 4; }
                int dst_esize = src_esize * 2;
                shift = ((immh << 3) | immb) - (src_esize * 8);
                if (shift < 0) shift = 0;
                uint8_t src[16];
                memcpy(src, &cpu.v_lo[rn], 8);
                memcpy(src + 8, &cpu.v_hi[rn], 8);
                int elems = 8 / src_esize;  // always 8/4/2 from a 64-bit half
                int src_base = Q ? 8 : 0;
                uint8_t out[16] = {0};
                for (int i = 0; i < elems; i++) {
                    uint64_t v = 0;
                    memcpy(&v, src + src_base + i * src_esize, src_esize);
                    if (!is_unsigned) {
                        // Sign-extend from src_esize bytes.
                        int bits = src_esize * 8;
                        int64_t sv = static_cast<int64_t>(v << (64 - bits)) >> (64 - bits);
                        v = static_cast<uint64_t>(sv);
                    }
                    v <<= shift;
                    memcpy(out + i * dst_esize, &v, dst_esize);
                }
                memcpy(&cpu.v_lo[rd], out, 8);
                memcpy(&cpu.v_hi[rd], out + 8, 8);
                return;
                }
            }
            // ── Vector ADD/SUB/MUL (integer) ──────────────────────────
            // Encoding: 0x0E208400 (ADD) / 0x2E208400 (SUB) / 0x0E209C00 (MUL)
            // These are the most common SIMD arithmetic ops used by
            // memcpy/memset/string routines.
            {
                uint32_t sub3 = op & 0xFF20FC00;
                uint32_t sub3_noq = sub3 & ~(1u << 30);
                // ADD (vector): 0E208400
                if (sub3_noq == 0x0E208400) {
                    int esize = 1 << size;
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf_n[16], buf_m[16];
                    memcpy(buf_n, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                    memcpy(buf_m, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                    uint8_t out[16] = {0};
                    for (int i = 0; i < elems; i++) {
                        uint64_t a = 0, b = 0;
                        memcpy(&a, buf_n + i*esize, esize);
                        memcpy(&b, buf_m + i*esize, esize);
                        uint64_t r = a + b;
                        memcpy(out + i*esize, &r, esize);
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // SUB (vector): 2E208400
                if (sub3_noq == 0x2E208400) {
                    int esize = 1 << size;
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf_n[16], buf_m[16];
                    memcpy(buf_n, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                    memcpy(buf_m, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                    uint8_t out[16] = {0};
                    for (int i = 0; i < elems; i++) {
                        uint64_t a = 0, b = 0;
                        memcpy(&a, buf_n + i*esize, esize);
                        memcpy(&b, buf_m + i*esize, esize);
                        uint64_t r = a - b;
                        memcpy(out + i*esize, &r, esize);
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // MUL (vector): 0E209C00
                if (sub3_noq == 0x0E209C00) {
                    int esize = 1 << size;
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t buf_n[16], buf_m[16];
                    memcpy(buf_n, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                    memcpy(buf_m, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                    uint8_t out[16] = {0};
                    for (int i = 0; i < elems; i++) {
                        uint64_t a = 0, b = 0;
                        memcpy(&a, buf_n + i*esize, esize);
                        memcpy(&b, buf_m + i*esize, esize);
                        uint64_t r = a * b;
                        memcpy(out + i*esize, &r, esize);
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // SADDW / SADDW2 (vector, widening add): sign-extend the
                // narrow lanes of Vm and add them to the corresponding wide
                // lanes of Vn. Q selects which half of Vm is used: Q=0 → low
                // half (SADDW), Q=1 → high half (SADDW2). Vd/Vn are always
                // full 128-bit (4S or 2D); the narrow source is 8H or 4S.
                //   SADDW  v.4s ← v.4s + v.4h   (size=1 → src H)
                //   SADDW  v.2d ← v.2d + v.2s   (size=2 → src S)
                if (sub3_noq == 0x0E201000) {
                    int esize_src = 1 << size;            // 2 (H) or 4 (S)
                    int esize_dst = esize_src * 2;
                    int lanes = 16 / esize_dst;         // 4 (4S) or 2 (2D)
                    uint8_t vn[16], vm[16], vd[16];
                    memcpy(vn, &cpu.v_lo[rn], 8);
                    memcpy(vn + 8, &cpu.v_hi[rn], 8);
                    memcpy(vm, &cpu.v_lo[rm], 8);
                    memcpy(vm + 8, &cpu.v_hi[rm], 8);
                    int src_off = Q ? lanes : 0;        // element index into Vm
                    for (int i = 0; i < lanes; i++) {
                        uint64_t wide = 0;
                        memcpy(&wide, vn + i * esize_dst, esize_dst);
                        uint64_t narrow = 0;
                        memcpy(&narrow, vm + (src_off + i) * esize_src, esize_src);
                        int64_t se = (esize_src == 2) ? (int16_t)narrow : (int32_t)narrow;
                        uint64_t r = wide + (uint64_t)se;
                        memcpy(vd + i * esize_dst, &r, esize_dst);
                    }
                    memcpy(&cpu.v_lo[rd], vd, 8);
                    memcpy(&cpu.v_hi[rd], vd + 8, 8);
                    return;
                }
                // SADDL/UADDL/SSUBL/USUBL (vector, widening long add/sub).
                // Sign- or zero-extend the narrow lanes of BOTH Vn and Vm
                // and add (SADDL/UADDL) or subtract (SSUBL/USUBL) them into
                // the double-width lanes of Vd. Q selects which half of
                // Vn/Vm is used: Q=0 → low half (no "2" suffix), Q=1 → high
                // half (SADDL2 etc). Vd is always full 128-bit (8H/4S/2D);
                // the narrow sources are 8B/8H/4S.
                //   SADDL  v.4s ← sxt(v.4h) + sxt(v.4h)   (size=1 → src H)
                //   SADDL  v.2d ← sxt(v.2s) + sxt(v.2s)   (size=2 → src S)
                //   UADDL  v.4s ← uxt(v.4h) + uxt(v.4h)   (U=1)
                //   SSUBL  v.4s ← sxt(v.4h) - sxt(v.4h)
                // Encoding sub3_noq values:
                //   SADDL 0x0E200000, UADDL 0x2E200000 (bit15=0)
                //   SSUBL 0x0E202000, USUBL 0x2E202000 (bit13=1)
                if (sub3_noq == 0x0E200000 || sub3_noq == 0x2E200000 ||
                    sub3_noq == 0x0E202000 || sub3_noq == 0x2E202000) {
                    int esize_src = 1 << size;          // 1 (B), 2 (H), 4 (S)
                    int esize_dst = esize_src * 2;
                    int lanes = 16 / esize_dst;         // 8 (8H), 4 (4S), 2 (2D)
                    bool is_unsigned = (sub3_noq >> 29) & 1;
                    bool is_subl = sub3_noq == 0x0E202000 || sub3_noq == 0x2E202000;
                    uint8_t vn[16], vm[16], vd[16];
                    memcpy(vn, &cpu.v_lo[rn], 8);
                    memcpy(vn + 8, &cpu.v_hi[rn], 8);
                    memcpy(vm, &cpu.v_lo[rm], 8);
                    memcpy(vm + 8, &cpu.v_hi[rm], 8);
                    int src_off = Q ? lanes : 0;        // element index into Vn/Vm
                    for (int i = 0; i < lanes; i++) {
                        uint64_t an = 0, am = 0;
                        memcpy(&an, vn + (src_off + i) * esize_src, esize_src);
                        memcpy(&am, vm + (src_off + i) * esize_src, esize_src);
                        uint64_t en = is_unsigned ? an :
                            static_cast<uint64_t>(static_cast<int64_t>(an << (64 - esize_src * 8)) >> (64 - esize_src * 8));
                        uint64_t em = is_unsigned ? am :
                            static_cast<uint64_t>(static_cast<int64_t>(am << (64 - esize_src * 8)) >> (64 - esize_src * 8));
                        uint64_t r = is_subl ? en - em : en + em;
                        memcpy(vd + i * esize_dst, &r, esize_dst);
                    }
                    memcpy(&cpu.v_lo[rd], vd, 8);
                    memcpy(&cpu.v_hi[rd], vd + 8, 8);
                    return;
                }
                // SMLAL/UMLAL/SMLSL/UMLSL/SMULL/UMULL (vector, widening
                // long multiply). Sign- or zero-extend the narrow lanes of
                // Vm and multiply into the wide lanes of Vd. Q selects which
                // half of Vm/Vn is used: Q=0 → low half (no "2" suffix),
                // Q=1 → high half (the "2" variants). Vd is always full
                // 128-bit (4S or 2D); the narrow source is 8H or 4S.
                //   SMLAL  v.4s ← v.4s + sxt(v.4h)*sxt(v.4h)  (size=1 → H)
                //   UMLAL  v.4s ← v.4s + uxt(v.4h)*uxt(v.4h)  (U=1)
                //   SMLSL  v.4s ← v.4s - sxt(v.4h)*sxt(v.4h)
                //   SMULL  v.4s ← sxt(v.4h)*sxt(v.4h)          (no accum)
                //   SMLAL  v.2d ← v.2d + sxt(v.2s)*sxt(v.2s)  (size=2 → S)
                // Encoding sub3_noq values:
                //   SMLAL 0x0E208000, UMLAL 0x2E208000 (bit15=1, +accum)
                //   SMLSL 0x0E20A000, UMLSL 0x2E20A000 (bit15=1, -accum)
                //   SMULL 0x0E20C000, UMULL 0x2E20C000 (bit15=1, no accum)
                if (sub3_noq == 0x0E208000 || sub3_noq == 0x2E208000 ||
                    sub3_noq == 0x0E20A000 || sub3_noq == 0x2E20A000 ||
                    sub3_noq == 0x0E20C000 || sub3_noq == 0x2E20C000) {
                    int esize_src = 1 << size;            // 1 (B), 2 (H), 4 (S)
                    int esize_dst = esize_src * 2;
                    int lanes = 16 / esize_dst;           // 8 (8H), 4 (4S), 2 (2D)
                    bool is_unsigned = (sub3_noq >> 29) & 1;
                    bool is_subl = sub3_noq == 0x0E20A000 || sub3_noq == 0x2E20A000;
                    bool no_accum = sub3_noq == 0x0E20C000 || sub3_noq == 0x2E20C000;
                    uint8_t vn[16], vm[16], vd[16];
                    memcpy(vn, &cpu.v_lo[rn], 8);
                    memcpy(vn + 8, &cpu.v_hi[rn], 8);
                    memcpy(vm, &cpu.v_lo[rm], 8);
                    memcpy(vm + 8, &cpu.v_hi[rm], 8);
                    memcpy(vd, &cpu.v_lo[rd], 8);
                    memcpy(vd + 8, &cpu.v_hi[rd], 8);
                    int src_off = Q ? lanes : 0;          // element index into Vn/Vm
                    for (int i = 0; i < lanes; i++) {
                        uint64_t an = 0, am = 0;
                        memcpy(&an, vn + (src_off + i) * esize_src, esize_src);
                        memcpy(&am, vm + (src_off + i) * esize_src, esize_src);
                        uint64_t en = is_unsigned ? an :
                            static_cast<uint64_t>(static_cast<int64_t>(an << (64 - esize_src * 8)) >> (64 - esize_src * 8));
                        uint64_t em = is_unsigned ? am :
                            static_cast<uint64_t>(static_cast<int64_t>(am << (64 - esize_src * 8)) >> (64 - esize_src * 8));
                        uint64_t prod = en * em;
                        uint64_t acc = 0;
                        if (!no_accum) memcpy(&acc, vd + i * esize_dst, esize_dst);
                        uint64_t r = is_subl ? acc - prod : acc + prod;
                        memcpy(vd + i * esize_dst, &r, esize_dst);
                    }
                    memcpy(&cpu.v_lo[rd], vd, 8);
                    memcpy(&cpu.v_hi[rd], vd + 8, 8);
                    return;
                }
                // TBL / TBX (vector, table lookup): Vd[i] =
                // table[index[i]] where `table` is the concatenation of
                // (len+1) consecutive vector registers starting at Vn.
                // Index is an unsigned byte; out-of-range → 0 (TBL) or
                // unchanged Vd[i] (TBX). Q selects 8 (Q=0, v_lo only) vs
                // 16 (Q=1) byte lanes.
                //   TBL Vd.8B, {Vn.8B}, Vm.8B        — 0x0E002000 (1 reg)
                //   TBL Vd.16B, {Vn.16B,..}, Vm.16B  — Q=1 → 0x4E002000
                //   TBX same but bit[12]=1 → 0x0E003000 / 0x4E003000
                // Register count = (bits[14:13]) + 1 (1..4).
                if (sub3_noq == 0x0E002000 || sub3_noq == 0x0E003000) {
                    bool is_tbx = sub3_noq == 0x0E003000;
                    int nregs = ((op >> 13) & 0x3) + 1;   // bits[14:13]
                    int lanes = Q ? 16 : 8;               // 8B or 16B
                    int tbl_bytes = nregs * lanes;        // concatenated table
                    uint8_t table[4 * 16];
                    for (int r = 0; r < nregs; r++) {
                        int reg = (rn + r) & 31;
                        memcpy(table + r * lanes, &cpu.v_lo[reg], 8);
                        if (Q) memcpy(table + r * lanes + 8, &cpu.v_hi[reg], 8);
                    }
                    uint8_t idx[16], out[16];
                    memcpy(idx, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(idx + 8, &cpu.v_hi[rm], 8);
                    memcpy(out, &cpu.v_lo[rd], 8);
                    if (Q) memcpy(out + 8, &cpu.v_hi[rd], 8);
                    for (int i = 0; i < lanes; i++) {
                        uint8_t ix = idx[i];
                        if (ix < tbl_bytes) out[i] = table[ix];
                        else if (!is_tbx) out[i] = 0;   // TBL: zero, TBX: keep
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // ── ABS / NEG (vector) — 0x0E20B800 / 0x2E20B800 ──
                // ABS Vd.<T>, Vn.<T> = |Vn| per lane; NEG = 0 - Vn per lane.
                // For ABS of the minimum signed value (e.g. INT8_MIN), ARM
                // defines the result as the input unchanged (no overflow).
                // NOTE: FCVTZS/FCVTZU share sub3_noq (0x0E20B800/0x2E20B800)
                // but set bit16 (0x10000), so exclude them here.
                if ((sub3_noq == 0x0E20B800 || sub3_noq == 0x2E20B800) &&
                    !(op & 0x10000)) {
                    int esize = 1 << size;
                    int elems = (Q ? 16 : 8) / esize;
                    bool is_abs = sub3_noq == 0x0E20B800;
                    uint8_t vn[16], out[16];
                    memcpy(vn, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                    for (int i = 0; i < elems; i++) {
                        uint64_t v = 0;
                        memcpy(&v, vn + i * esize, esize);
                        if (is_abs) {
                            int64_t sv = static_cast<int64_t>(v << (64 - esize * 8)) >> (64 - esize * 8);
                            uint64_t r = static_cast<uint64_t>(sv < 0 ? -sv : sv);
                            memcpy(out + i * esize, &r, esize);
                        } else {
                            uint64_t r = static_cast<uint64_t>(0) - v;
                            memcpy(out + i * esize, &r, esize);
                        }
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // ── FCVTZS / FCVTZU (vector, FP→int, toward zero) ──
                // Same sub3_noq as ABS/NEG but with bit16 (0x10000) set.
                //   FCVTZS Vd.4S, Vn.4S = 0x4EA1B800  (signed, U=0)
                //   FCVTZU Vd.4S, Vn.4S = 0x6EA1B800  (unsigned, U=1)
                //   size=2 → 32-bit (4S/2S), size=3 → 64-bit (2D/1D)
                // Truncate toward zero (rmode=3). NaN → 0.
                if ((sub3_noq == 0x0E20B800 || sub3_noq == 0x2E20B800) &&
                    (op & 0x10000) && size >= 2) {
                    bool is_unsigned = sub3_noq == 0x2E20B800;
                    int esize = 1 << size;
                    int elems = (Q ? 16 : 8) / esize;
                    uint8_t vn[16], out[16];
                    memcpy(vn, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                    for (int i = 0; i < elems; i++) {
                        uint64_t r = 0;
                        if (esize == 4) {
                            float f;
                            memcpy(&f, vn + i * esize, 4);
                            if (std::isfinite(f)) {
                                if (is_unsigned) {
                                    r = (f < 0.0f) ? 0 : static_cast<uint32_t>(f);
                                } else {
                                    r = static_cast<uint32_t>(static_cast<int32_t>(f));
                                }
                            } else {
                                r = 0;  // NaN/±inf → 0
                            }
                        } else {
                            double d;
                            memcpy(&d, vn + i * esize, 8);
                            if (std::isfinite(d)) {
                                if (is_unsigned) {
                                    r = (d < 0.0) ? 0 : static_cast<uint64_t>(d);
                                } else {
                                    r = static_cast<uint64_t>(static_cast<int64_t>(d));
                                }
                            } else {
                                r = 0;
                            }
                        }
                        memcpy(out + i * esize, &r, esize);
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // UMIN/UMAX/SMIN/SMAX (vector): lane-wise min/max.
                //   UMIN 0x2E206C00 (Q=1 → 0x6E206C00)  UMAX 0x2E206400
                //   SMIN 0x0E206C00 (Q=1 → 0x4E206C00)  SMAX 0x0E206400
                if (sub3_noq == 0x2E206C00 || sub3_noq == 0x2E206400 ||
                    sub3_noq == 0x0E206C00 || sub3_noq == 0x0E206400) {
                    int esize = 1 << size;
                    int elems = (Q ? 16 : 8) / esize;
                    bool is_unsigned = sub3_noq == 0x2E206C00 || sub3_noq == 0x2E206400;
                    bool is_min = sub3_noq == 0x2E206C00 || sub3_noq == 0x0E206C00;
                    uint8_t vn[16], vm[16], out[16];
                    memcpy(vn, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                    memcpy(vm, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(vm + 8, &cpu.v_hi[rm], 8);
                    for (int i = 0; i < elems; i++) {
                        uint64_t a = 0, b = 0;
                        memcpy(&a, vn + i * esize, esize);
                        memcpy(&b, vm + i * esize, esize);
                        uint64_t r;
                        if (is_unsigned) {
                            r = is_min ? (a < b ? a : b) : (a > b ? a : b);
                        } else {
                            int64_t sa = static_cast<int64_t>(a << (64 - esize * 8)) >> (64 - esize * 8);
                            int64_t sb = static_cast<int64_t>(b << (64 - esize * 8)) >> (64 - esize * 8);
                            int64_t sr = is_min ? (sa < sb ? sa : sb) : (sa > sb ? sa : sb);
                            r = static_cast<uint64_t>(sr);
                        }
                        memcpy(out + i * esize, &r, esize);
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // MLA / MLS (vector, multiply-accumulate):
                //   MLA Vd.<T>, Vn.<T>, Vm.<T> = Vd + Vn*Vm  (0x0E209400)
                //   MLS Vd.<T>, Vn.<T>, Vm.<T> = Vd - Vn*Vm  (0x2E209400)
                if (sub3_noq == 0x0E209400 || sub3_noq == 0x2E209400) {
                    int esize = 1 << size;
                    int elems = (Q ? 16 : 8) / esize;
                    bool is_mls = sub3_noq == 0x2E209400;
                    uint8_t vn[16], vm[16], vd[16];
                    memcpy(vn, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                    memcpy(vm, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(vm + 8, &cpu.v_hi[rm], 8);
                    memcpy(vd, &cpu.v_lo[rd], 8);
                    if (Q) memcpy(vd + 8, &cpu.v_hi[rd], 8);
                    for (int i = 0; i < elems; i++) {
                        uint64_t a = 0, b = 0, acc = 0;
                        memcpy(&a, vn + i * esize, esize);
                        memcpy(&b, vm + i * esize, esize);
                        memcpy(&acc, vd + i * esize, esize);
                        uint64_t r = is_mls ? acc - a * b : acc + a * b;
                        memcpy(vd + i * esize, &r, esize);
                    }
                    memcpy(&cpu.v_lo[rd], vd, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], vd + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // UHADD/UHSUB/URHADD/SHADD/SHSUB/SRHADD (vector, halving
                // add/sub): HADD = (a+b)>>1, HSUB = (a-b)>>1, RHADD =
                // (a+b+1)>>1, per lane. Signed variants use arithmetic
                // shift (round toward -inf); RHADD rounds to nearest.
                //   UHADD 0x2E200400  UHSUB 0x2E202400  URHADD 0x2E201400
                //   SHADD 0x0E200400  SHSUB 0x0E202400  SRHADD 0x0E201400
                if (sub3_noq == 0x2E200400 || sub3_noq == 0x2E202400 ||
                    sub3_noq == 0x2E201400 || sub3_noq == 0x0E200400 ||
                    sub3_noq == 0x0E202400 || sub3_noq == 0x0E201400) {
                    int esize = 1 << size;
                    int elems = (Q ? 16 : 8) / esize;
                    bool is_unsigned = sub3_noq == 0x2E200400 || sub3_noq == 0x2E202400 || sub3_noq == 0x2E201400;
                    bool is_sub = sub3_noq == 0x2E202400 || sub3_noq == 0x0E202400;
                    bool is_rhadd = sub3_noq == 0x2E201400 || sub3_noq == 0x0E201400;
                    uint8_t vn[16], vm[16], out[16];
                    memcpy(vn, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                    memcpy(vm, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(vm + 8, &cpu.v_hi[rm], 8);
                    for (int i = 0; i < elems; i++) {
                        uint64_t a = 0, b = 0;
                        memcpy(&a, vn + i * esize, esize);
                        memcpy(&b, vm + i * esize, esize);
                        uint64_t r;
                        if (is_unsigned) {
                            uint64_t sum = is_sub ? (a - b) : (a + b);
                            if (is_rhadd) sum += 1;
                            // UHSUB must wrap the difference to the
                            // element width BEFORE halving (ARM:
                            // (a + 2^N - b) >> 1). Without the wrap, a
                            // 64-bit borrow makes low bytes wrong (e.g.
                            // (0-3)>>1 = 0x7FFFFFFF...FE → 0xFE, not
                            // (0+256-3)>>1 = 126). UHADD/URHADD can't
                            // overflow (a+b ≤ 2^(N+1)-2), and esize=8
                            // already wraps in 64-bit arithmetic.
                            if (is_sub && esize < 8)
                                sum &= (1ULL << (esize * 8)) - 1;
                            r = sum >> 1;
                        } else {
                            // Signed: use 64-bit arithmetic (works for all esizes).
                            int64_t sa = static_cast<int64_t>(a << (64 - esize * 8)) >> (64 - esize * 8);
                            int64_t sb = static_cast<int64_t>(b << (64 - esize * 8)) >> (64 - esize * 8);
                            int64_t sum = is_sub ? (sa - sb) : (sa + sb);
                            if (is_rhadd) sum += 1;
                            r = static_cast<uint64_t>(sum >> 1);
                        }
                        memcpy(out + i * esize, &r, esize);
                    }
                    memcpy(&cpu.v_lo[rd], out, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // UMINP (vector, pairwise unsigned min): Vd[i] =
                // min(Vn[2i], Vn[2i+1]) for the low half, then
                // min(Vm[2i], Vm[2i+1]) for the high half.
                if (sub3_noq == 0x2E20AC00) {  // UMINP 8B/16B, 4H/8H, 2S/4S
                    int esize = 1 << size;
                    int elems_per_src = (Q ? 16 : 8) / esize;
                    int half = elems_per_src / 2;
                    uint8_t vn[16], vm[16], vd[16];
                    memcpy(vn, &cpu.v_lo[rn], 8);
                    if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                    memcpy(vm, &cpu.v_lo[rm], 8);
                    if (Q) memcpy(vm + 8, &cpu.v_hi[rm], 8);
                    for (int i = 0; i < half; i++) {
                        uint64_t a = 0, b = 0;
                        memcpy(&a, vn + (2 * i) * esize, esize);
                        memcpy(&b, vn + (2 * i + 1) * esize, esize);
                        uint64_t r = a < b ? a : b;
                        memcpy(vd + i * esize, &r, esize);
                        a = b = 0;
                        memcpy(&a, vm + (2 * i) * esize, esize);
                        memcpy(&b, vm + (2 * i + 1) * esize, esize);
                        r = a < b ? a : b;
                        memcpy(vd + (half + i) * esize, &r, esize);
                    }
                    memcpy(&cpu.v_lo[rd], vd, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], vd + 8, 8);
                    else cpu.v_hi[rd] = 0;
                    return;
                }
                // ── FP by-element (FMUL/FMLA/FMLS/FMULX × element) ──
                // FMUL Vd.4S, Vn.4S, Vm.S[index]: multiply Vn by the scalar
                // lane of Vm selected by the index field.
                // Encoding: 0 Q 0 11111 size L M Rm 1 opc[2:0] H Rn Rd
                // sub3_noq (mask 0xFF00F400, strips L/M/H index bits) =
                //   FMLA 0x0F001000  FMLS 0x0F005000
                //   FMUL 0x0F009000  FMULX 0x0F00D000
                // size: 1=H(16b), 2=S(32b), 3=D(64b). The element index is
                // formed from H:L:M: D→{H}, S→{H:L}, H→{H:L:M}.
                {
                    uint32_t be = (op & 0xFF00F400) & ~(1u << 30);
                    if ((be == 0x0F001000 || be == 0x0F005000 ||
                         be == 0x0F009000 || be == 0x0F00D000) && size >= 2) {
                        int esize = 1 << size;   // 2, 4, or 8
                        int lanes = (Q ? 16 : 8) / esize;
                        uint32_t H = (op >> 11) & 1;
                        uint32_t L = (op >> 21) & 1;
                        uint32_t M = (op >> 20) & 1;
                        int idx;
                        if (size == 3) idx = H;
                        else if (size == 2) idx = (H << 1) | L;
                        else idx = (H << 2) | (L << 1) | M;
                        int opc = (op >> 12) & 0xF;   // 1=FMLA 5=FMLS 9=FMUL 13=FMULX
                        uint8_t vn[16], vm[16];
                        memcpy(vn, &cpu.v_lo[rn], 8);
                        if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                        memcpy(vm, &cpu.v_lo[rm], 8);
                        memcpy(vm + 8, &cpu.v_hi[rm], 8);
                        if (esize == 8) {
                            double sc;
                            memcpy(&sc, vm + idx * 8, 8);
                            // Snapshot Vd (FMLA/FMLS accumulate into it).
                            uint64_t dlo0 = cpu.v_lo[rd], dhi0 = cpu.v_hi[rd];
                            for (int i = 0; i < lanes; i++) {
                                double a, r;
                                memcpy(&a, vn + i * 8, 8);
                                double d;
                                memcpy(&d, (i == 0) ? &dlo0 : &dhi0, 8);
                                if (opc == 9 || opc == 13) {
                                    r = a * sc;                 // FMUL / FMULX
                                } else {
                                    r = d + (opc == 1 ? a * sc : -(a * sc));  // FMLA / FMLS
                                }
                                if (i == 0) memcpy(&cpu.v_lo[rd], &r, 8);
                                else memcpy(&cpu.v_hi[rd], &r, 8);
                            }
                        } else if (esize == 4) {
                            float sc;
                            memcpy(&sc, vm + idx * 4, 4);
                            // Snapshot Vd (FMLA/FMLS accumulate into it).
                            uint64_t dlo0 = cpu.v_lo[rd], dhi0 = cpu.v_hi[rd];
                            for (int i = 0; i < lanes; i++) {
                                float a, r;
                                memcpy(&a, vn + i * 4, 4);
                                uint64_t chunk = (i < 2) ? dlo0 : dhi0;
                                int sh = (i & 1) * 32;
                                float d;
                                uint32_t db = (chunk >> sh) & 0xFFFFFFFF;
                                memcpy(&d, &db, 4);
                                if (opc == 9 || opc == 13) {
                                    r = a * sc;                 // FMUL / FMULX
                                } else {
                                    r = d + (opc == 1 ? a * sc : -(a * sc));  // FMLA / FMLS
                                }
                                uint32_t rb;
                                memcpy(&rb, &r, 4);
                                uint64_t* chunkp = (i < 2) ? &cpu.v_lo[rd] : &cpu.v_hi[rd];
                                *chunkp = (*chunkp & ~(0xFFFFFFFFULL << sh))
                                        | (static_cast<uint64_t>(rb) << sh);
                            }
                        }
                        if (!Q) cpu.v_hi[rd] = 0;
                        return;
                    }
                }
                // ── FABS / FNEG (FP absolute / negate, vector) ──
                // FABS Vd.<T>, Vn.<T>: clear sign bit. FNEG: flip sign bit.
                // Encoding (3-same, 2-operand, bit22=1 FP): 0 Q 0 1110 1
                // size 1 0 0000 1111 1 0 Rn Rd. sub3_noq: FABS=0x0E20F800,
                // FNEG=0x2E20F800. S (size=2) or D (size=3).
                {
                    uint32_t fs = (op & 0xFF20FC00) & ~(1u << 30);
                    if (fs == 0x0E20F800 || fs == 0x2E20F800) {
                        bool is_neg = fs == 0x2E20F800;
                        int esize = 1 << size;
                        int lanes = (Q ? 16 : 8) / esize;
                        uint8_t vn[16];
                        memcpy(vn, &cpu.v_lo[rn], 8);
                        if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                        for (int i = 0; i < lanes; i++) {
                            uint64_t v = 0;
                            memcpy(&v, vn + i * esize, esize);
                            if (is_neg) v ^= (1ULL << (esize * 8 - 1));
                            else v &= ~(1ULL << (esize * 8 - 1));
                            if (esize == 8) {
                                if (i == 0) memcpy(&cpu.v_lo[rd], &v, 8);
                                else memcpy(&cpu.v_hi[rd], &v, 8);
                            } else {
                                uint64_t* chunkp = (i < 2) ? &cpu.v_lo[rd] : &cpu.v_hi[rd];
                                int sh = (i & 1) * 32;
                                uint32_t rb = static_cast<uint32_t>(v);
                                *chunkp = (*chunkp & ~(0xFFFFFFFFULL << sh))
                                        | (static_cast<uint64_t>(rb) << sh);
                            }
                        }
                        if (!Q) cpu.v_hi[rd] = 0;
                        return;
                    }
                }
                // ── SCVTF / UCVTF (vector, integer → float, 2-operand) ──
                // SCVTF Vd.<T>, Vn.<T>: signed int → FP. UCVTF: unsigned.
                // Encoding (3-same, 2-operand): 0 Q U 0 1110 size 1 0 0000
                // 1111 1 0 Rn Rd. sub3_noq: SCVTF=0x0E20D800 (size 0/1),
                // UCVTF=0x2E20D800 (size 0/1). size=0 → 4S/2S (32-bit ints),
                // size=1 → 2D/1D (64-bit ints). Note FRECPE/FRSQRTE share
                // these sub3_noq values but use size=2/3, so `size < 2` here.
                {
                    uint32_t cv = (op & 0xFF20FC00) & ~(1u << 30);
                    if ((cv == 0x0E20D800 || cv == 0x2E20D800) && size < 2) {
                        bool is_unsigned = cv == 0x2E20D800;
                        bool is_double = size == 1;   // 2D/1D output
                        int lanes = is_double ? (Q ? 2 : 1) : (Q ? 4 : 2);
                        uint8_t vn[16];
                        memcpy(vn, &cpu.v_lo[rn], 8);
                        if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                        if (is_double) {
                            for (int i = 0; i < lanes; i++) {
                                int64_t sv;
                                memcpy(&sv, vn + i * 8, 8);
                                double r = is_unsigned ? static_cast<double>(static_cast<uint64_t>(sv))
                                                       : static_cast<double>(sv);
                                if (i == 0) memcpy(&cpu.v_lo[rd], &r, 8);
                                else memcpy(&cpu.v_hi[rd], &r, 8);
                            }
                        } else {
                            for (int i = 0; i < lanes; i++) {
                                int32_t sv;
                                memcpy(&sv, vn + i * 4, 4);
                                float r = is_unsigned ? static_cast<float>(static_cast<uint32_t>(sv))
                                                      : static_cast<float>(sv);
                                uint32_t rb;
                                memcpy(&rb, &r, 4);
                                uint64_t* chunkp = (i < 2) ? &cpu.v_lo[rd] : &cpu.v_hi[rd];
                                int sh = (i & 1) * 32;
                                *chunkp = (*chunkp & ~(0xFFFFFFFFULL << sh))
                                        | (static_cast<uint64_t>(rb) << sh);
                            }
                        }
                        if (!Q) cpu.v_hi[rd] = 0;
                        return;
                    }
                }
                // ── FRECPE / FRSQRTE (FP reciprocal estimate, vector) ──
                // FRECPE Vd.<T>, Vn.<T>: reciprocal estimate (1/x).
                // FRSQRTE Vd.<T>, Vn.<T>: reciprocal sqrt estimate.
                // Encoding (3-same, 2-operand): 0 Q U 0 1110 size 1 0 0000
                // 1101 1 0 Rn Rd. Sub3_noq: FRECPE=0x0E20D800, FRSQRTE=0x2E20D800
                // (stripped mask: 0x0E00D000 / 0x2E00D000). S (size=2) or D.
                {
                    uint32_t re = (op & 0xFF00F400) & ~(1u << 30);
                    if ((re == 0x0E00D000 || re == 0x2E00D000) && size >= 2) {
                        bool is_sqrt = re == 0x2E00D000;
                        int esize = 1 << size;
                        int lanes = (Q ? 16 : 8) / esize;
                        uint8_t vn[16];
                        memcpy(vn, &cpu.v_lo[rn], 8);
                        if (Q) memcpy(vn + 8, &cpu.v_hi[rn], 8);
                        for (int i = 0; i < lanes; i++) {
                            if (esize == 8) {
                                double x;
                                memcpy(&x, vn + i * 8, 8);
                                double r;
                                if (std::isnan(x)) r = x;
                                else if (x == 0.0) r = std::copysign(INFINITY, x);
                                else if (std::isinf(x)) r = std::copysign(0.0, x);
                                else if (is_sqrt && x < 0.0) r = std::nan("");
                                else r = is_sqrt ? (1.0 / std::sqrt(x)) : (1.0 / x);
                                if (i == 0) memcpy(&cpu.v_lo[rd], &r, 8);
                                else memcpy(&cpu.v_hi[rd], &r, 8);
                            } else {
                                float x;
                                memcpy(&x, vn + i * 4, 4);
                                float r;
                                if (std::isnan(x)) r = x;
                                else if (x == 0.0f) r = std::copysign(INFINITY, x);
                                else if (std::isinf(x)) r = std::copysign(0.0f, x);
                                else if (is_sqrt && x < 0.0f) r = std::nan("");
                                else r = is_sqrt ? (1.0f / std::sqrtf(x)) : (1.0f / x);
                                uint32_t rb;
                                memcpy(&rb, &r, 4);
                                uint32_t sh = (i & 1) * 32;
                                uint64_t* chunkp = (i < 2) ? &cpu.v_lo[rd] : &cpu.v_hi[rd];
                                *chunkp = (*chunkp & ~(0xFFFFFFFFULL << sh))
                                        | (static_cast<uint64_t>(rb) << sh);
                            }
                        }
                        if (!Q) cpu.v_hi[rd] = 0;
                        return;
                    }
                }
                // FADD/FSUB/FMUL/FDIV (vector, float) — 0x0E20D400 (FADD 4S)
                // and 0x0E20DC00 (FMUL 4S).
                // These use the FP arithmetic encoding with Q form.
                // For correctness with musl's printf path:
                if ((op & 0xBF20FC00) == 0x0E20D400) {
                    // FADD (vector) — fadd v0.4s, v1.4s, v2.4s
                    if (size == 0) { // 4S (32-bit float)
                        float* fn = reinterpret_cast<float*>(&cpu.v_lo[rn]);
                        float* fm = reinterpret_cast<float*>(&cpu.v_lo[rm]);
                        float* fd = reinterpret_cast<float*>(&cpu.v_lo[rd]);
                        float fn_hi[2], fm_hi[2], fd_hi[2];
                        if (Q) {
                            memcpy(fn_hi, &cpu.v_hi[rn], 8);
                            memcpy(fm_hi, &cpu.v_hi[rm], 8);
                        }
                        fd[0] = fn[0] + fm[0];
                        fd[1] = fn[1] + fm[1];
                        if (Q) {
                            memcpy(fd_hi, &cpu.v_hi[rd], 8);
                            fd_hi[0] = fn_hi[0] + fm_hi[0];
                            fd_hi[1] = fn_hi[1] + fm_hi[1];
                            memcpy(&cpu.v_hi[rd], fd_hi, 8);
                        } else cpu.v_hi[rd] = 0;
                    } else if (size == 1) { // 2D (64-bit double)
                        double fn_l, fm_l, fd_l;
                        memcpy(&fn_l, &cpu.v_lo[rn], sizeof(double));
                        memcpy(&fm_l, &cpu.v_lo[rm], sizeof(double));
                        fd_l = fn_l + fm_l;
                        memcpy(&cpu.v_lo[rd], &fd_l, sizeof(double));
                        if (Q) {
                            double fn_h, fm_h, fd_h;
                            memcpy(&fn_h, &cpu.v_hi[rn], sizeof(double));
                            memcpy(&fm_h, &cpu.v_hi[rm], sizeof(double));
                            fd_h = fn_h + fm_h;
                            memcpy(&cpu.v_hi[rd], &fd_h, sizeof(double));
                        } else cpu.v_hi[rd] = 0;
                    }
                    return;
                }
            }
            // Fallback: any SIMD_DP op we don't model used to be silently
            // NOP'd — "incorrect but lets glibc continue", with wrong FP
            // results for any program that actually depends on the op.
            // Now we surface it as a DecodeError (→ SIGILL to the guest)
            // so missing SIMD coverage becomes a loud, fixable failure
            // instead of silent corruption. Log the opcode first for
            // debugging (also via BIFROST_SIMD_TRACE=1).
            if (getenv("BIFROST_SIMD_TRACE")) {
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
            if (getenv("BIFROST_SIMD_COLLECT")) {
                static std::set<uint32_t> simd_collected_;
                if (simd_collected_.insert(op).second)
                    fprintf(stderr, "[SIMD-COL] op=0x%08x pc=0x%llx (Q=%d U=%d size=%d)\n",
                            op, static_cast<unsigned long long>(cpu.pc),
                            Q, (op >> 29) & 1, (op >> 22) & 3);
                return;
            }
            throw DecodeError(cpu.pc, op);
            return;
        }
        // ── FP scalar (FMOV/FADD/FSUB/FMUL/FDIV/FCMP/FCVT/...) ────
        // The decoder classifies the entire 0x1E000000/0x9E000000
        // group as FP_SCALAR. We sub-dispatch on raw opcode bits.
        case InstClass::FP_SCALAR: {
            uint32_t op = d.raw;
            uint8_t rn = (op >> 5) & 0x1F;
            uint8_t rd = op & 0x1F;
            uint8_t rm = (op >> 16) & 0x1F;
            uint8_t sf_val = (op >> 31) & 1;
            uint8_t ftype = (op >> 22) & 3;  // 0=S(32-bit), 1=D(64-bit), 3=H(16-bit)
            // ARMv8 Crypto Extensions (SHA1H, SHA1SU0/SU1, SHA256SU0/SU1)
            // are encoded in the 0x5Exxxxxx range, which the decoder
            // classifies as FP_SCALAR (because 0x5E has bits[28:24]=11110
            // matching the FP group). Dispatch them via exec_crypto first
            // — exec_crypto returns false for non-crypto instructions, so
            // this is safe. Without this, SHA1/SHA256 schedule-update
            // instructions would be silently NOP'd, producing wrong hashes.
            if (exec_crypto(op, cpu)) return;
            // ── SHL (scalar, immediate): Dd, Dn, #imm ─────────────────
            // Encoding: 0x5F005400 (mask 0xFF00FC00).
            // silently NOP'd, breaking GCC -O2's vectorized TLS init
            // pattern (shl d31, d31, #2 to multiply by 4).
            if ((op & 0xFF00FC00) == 0x5F005400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int shift = ((immh << 4) | immb) - 64;
                if (shift < 0) shift = 0;
                uint64_t v = cpu.v_lo[rn];
                v <<= shift;
                cpu.v_lo[rd] = v;
                cpu.v_hi[rd] = 0;
                return;
            }
            // ── USHR (scalar, immediate): Dd, Dn, #imm ────────────────
            // Encoding: 0x7F000400 (mask 0xFF00FC00).
            if ((op & 0xFF00FC00) == 0x7F000400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int shift = 128 - ((immh << 4) | immb);
                if (shift < 0) shift = 0;
                if (shift >= 64) cpu.v_lo[rd] = 0;
                else cpu.v_lo[rd] = cpu.v_lo[rn] >> shift;
                cpu.v_hi[rd] = 0;
                return;
            }
            // ── SSHR (scalar, immediate, signed): Dd, Dn, #imm ────────
            // Encoding: 0x5F000400 (mask 0xFF00FC00). Scalar USHR is the
            // unsigned twin with mask 0x7F000400 (handled above). The old
            // mask 0x7F000000 never matched the real SSHR encodings
            // (e.g. `sshr d9, d8, #32` = 0x5f600509), silently NOP'ing
            // the shift and breaking raster/geometry code.
            if ((op & 0xFF00FC00) == 0x5F000400) {
                uint8_t immh = (op >> 20) & 0xF;
                uint8_t immb = (op >> 16) & 0xF;
                int shift = 128 - ((immh << 4) | immb);
                if (shift < 0) shift = 0;
                int64_t v = static_cast<int64_t>(cpu.v_lo[rn]);
                if (shift >= 64) cpu.v_lo[rd] = (v < 0) ? ~0ULL : 0;
                else cpu.v_lo[rd] = static_cast<uint64_t>(v >> shift);
                cpu.v_hi[rd] = 0;
                return;
            }
            // ── DUP (element → scalar FP register): MOV Sd, Vn.S[i] ──
            // Also MOV Dd, Vn.D[i]. Encoding base 0x5E000400
            // (bits[31:24]=0x5E, bit21=0, bits[15:12]=0, bits[11:10]=01).
            //   mov s11, v9.s[1]  = 0x5e0c052b   (imm5 = 0x0C = 1<<3|4)
            //   mov d11, v9.d[1]  = 0x5e18052b   (imm5 = 0x18 = 1<<4|8)
            // Copies a single element of Vn into the scalar FP register Rd
            // (upper 64 bits zeroed). Qt's raster/bezier code pulls lane
            // values out of SIMD accumulators with this; without it the
            // instruction was silently NOP'd.
            if ((op & 0xFF20FC00) == 0x5E000400) {
                uint8_t imm5 = (op >> 16) & 0x1F;
                int esize, idx;
                if (imm5 & 0x08)      { esize = 8; idx = imm5 >> 4; }  // D (64-bit)
                else if (imm5 & 0x04) { esize = 4; idx = imm5 >> 3; }  // S (32-bit)
                else                  { esize = 4; idx = 0; }
                uint64_t src_val = 0;
                int elems_per_qword = 8 / esize;
                if (idx < elems_per_qword) {
                    const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_lo[rn]);
                    memcpy(&src_val, p + idx * esize, esize);
                } else {
                    const uint8_t* p = reinterpret_cast<const uint8_t*>(&cpu.v_hi[rn]);
                    memcpy(&src_val, p + (idx - elems_per_qword) * esize, esize);
                }
                cpu.v_lo[rd] = src_val;
                cpu.v_hi[rd] = 0;
                return;
            }
            // FP register access + half-precision helpers are now
            // file-scope functions (read_fp_d, read_fp_s, write_fp_d,
            // write_fp_s, h2f, f2h, d2h) — see top of this file.
            // FMOV (general ↔ FP, 64-bit)
            // Bit[18]=1 distinguishes FMOV from SCVTF/UCVTF (bit[18]=0).
            // Without this, SCVTF (0x9E62xxxx) matches the FMOV mask.
            // Bit[17]=1 additionally excludes FCVTAS (0x9E640020, bit17=0),
            // which would otherwise be misdecoded as a raw GPR↔FP bit copy.
            if ((op & 0xFFE0FC00) == 0x9E600000 && (op & (1u << 18))
                && (op & (1u << 17))) {
                bool to_fp = (op >> 16) & 1;
                if (to_fp) { cpu.v_lo[rd] = cpu.regs[rn]; cpu.v_hi[rd] = 0; }
                else       { cpu.regs[rd] = cpu.v_lo[rn]; }
                return;
            }
            // FMOV (general ↔ FP, 32-bit)
            // Bit[18]=1 distinguishes FMOV from SCVTF/UCVTF (bit[18]=0),
            // exactly mirroring the 64-bit check above. Without this
            // guard, `scvtf s0, w0` (0x1E220000) and `ucvtf s0, w0`
            // (0x1E230000) match this mask and get misdecoded as a raw
            // GPR↔FP bit copy, producing garbage for any int→FP
            // conversion from a 32-bit GPR. This broke musl's
            // __floatscan inf/nan detection (strtod("-inf") returned
            // -nan) because the sign computation does `scvtf s1, w23`
            // with w23=-1 and expects s1=-1.0f.
            // Bit[17]=1 additionally excludes FCVTAS (0x1E240000, bit17=0),
            // which would otherwise be misdecoded as a raw GPR↔FP bit copy.
            if ((op & 0xFFE0FC00) == 0x1E200000 && (op & (1u << 18))
                && (op & (1u << 17))) {
                bool to_fp = (op >> 16) & 1;
                if (to_fp) { cpu.v_lo[rd] = cpu.regs[rn] & 0xFFFFFFFF; cpu.v_hi[rd] = 0; }
                else       { cpu.regs[rd] = cpu.v_lo[rn] & 0xFFFFFFFF; }
                return;
            }
            // FMOV (scalar, immediate)
            // The 8-bit immediate is decoded via VFPExpandImm (ARM ARM):
            //   imm = sign : NOT(imm8[6]) : Replicate(imm8[6], K) : imm8[5:0] : Zeros(M)
            // where K and M depend on FP precision:
            //   single (32): K=5,  M=19  (1+1+5+6+19 = 32)
            //   double (64): K=8,  M=48  (1+1+8+6+48 = 64)
            //   half   (16): K=2,  M=6   (1+1+2+6+6  = 16)
            // FMOV (scalar, immediate): uses shared fp_decode helper.
            if (fp_decode::is_fmov_imm(op)) {
                uint8_t imm8 = (op >> 13) & 0xFF;
                uint64_t bits = fp_decode::vfp_expand_imm(imm8, ftype);
                if (ftype == 1) {
                    cpu.v_lo[rd] = bits; cpu.v_hi[rd] = 0;
                } else if (ftype == 0) {
                    cpu.v_lo[rd] = bits; cpu.v_hi[rd] = 0;
                } else {
                    // ftype == 3 → half precision. We don't model 16-bit
                    // FP natively, so expand to single-precision bits via
                    // the standard half→single conversion of the imm value.
                    // (Preserved from the original implementation.)
                    uint64_t sign = (imm8 >> 7) & 1;
                    uint64_t b     = (imm8 >> 6) & 1;
                    uint64_t not_b = b ^ 1;
                    uint64_t imm6  = imm8 & 0x3F;
                    uint16_t rep_b = static_cast<uint16_t>(b * 0x3u);
                    uint16_t hbits = static_cast<uint16_t>((sign << 15)
                                  | (not_b << 14)
                                  | (rep_b << 12)
                                  | (imm6 << 6));
                    uint32_t sexp = (hbits >> 10) & 0x1F;
                    uint32_t smant = hbits & 0x3FF;
                    uint32_t sbits;
                    if (sexp == 0) {
                        if (smant == 0) sbits = static_cast<uint32_t>(sign) << 31;
                        else {
                            int e = -1;
                            while (!(smant & 0x400)) { smant <<= 1; e--; }
                            smant &= 0x3FF;
                            sbits = (static_cast<uint32_t>(sign) << 31)
                                  | ((static_cast<uint32_t>(127 + e - 14)) << 23)
                                  | (smant << 13);
                        }
                    } else if (sexp == 0x1F) {
                        sbits = (static_cast<uint32_t>(sign) << 31) | (0xFFu << 23) | (smant << 13);
                    } else {
                        sbits = (static_cast<uint32_t>(sign) << 31)
                              | ((sexp - 15 + 127) << 23)
                              | (smant << 13);
                    }
                    cpu.v_lo[rd] = sbits; cpu.v_hi[rd] = 0;
                }
                return;
            }
            // FMOV (register, FP to FP)
            if ((op & 0xFFFFFC00) == 0x1E604000 || (op & 0xFFFFFC00) == 0x1E204000) {
                cpu.v_lo[rd] = cpu.v_lo[rn];
                if (ftype) cpu.v_hi[rd] = 0;
                else { cpu.v_lo[rd] &= 0xFFFFFFFF; cpu.v_hi[rd] = 0; }
                return;
            }
            // FP arithmetic (2-source): FADD/FSUB/FMUL/FDIV/FMAX/FMIN/
            // FMAXNM/FMINNM/FNMUL.
            // Per ARMv8 ARM C4.2.28:
            //   0x0=FMUL, 0x1=FDIV, 0x2=FADD, 0x3=FSUB,
            //   0x4=FMAX, 0x5=FMIN, 0x6=FMAXNM, 0x7=FMINNM, 0x8=FNMUL
            // BUGFIX: opcode 0x6 was FNMUL (should be FMAXNM), 0x7 was
            // missing (FMINNM), 0x8 (FNMUL) fell through to default=0.
            if ((op & 0xFF200000) == 0x1E200000 && ((op >> 21) & 1) == 1
                && ((op >> 10) & 0x3) == 0x2) {
                uint8_t opcode = (op >> 12) & 0xF;
                if (ftype) {
                    double a = read_fp_d(cpu, rn), b = read_fp_d(cpu, rm), r = 0;
                    switch (opcode) {
                        case 0x0: r = a * b; break;                      // FMUL
                        case 0x1: r = a / b; break;                      // FDIV
                        case 0x2: r = a + b; break;                      // FADD
                        case 0x3: r = a - b; break;                      // FSUB
                        case 0x4: r = std::fmax(a, b); break;            // FMAX
                        case 0x5: r = std::fmin(a, b); break;            // FMIN
                        case 0x6: r = std::fmax(a, b); break;            // FMAXNM
                        case 0x7: r = std::fmin(a, b); break;            // FMINNM
                        case 0x8: r = -(a * b); break;                   // FNMUL
                        default: r = 0; break;
                    }
                    write_fp_d(cpu, rd, r);
                } else {
                    float a = read_fp_s(cpu, rn), b = read_fp_s(cpu, rm), r = 0;
                    switch (opcode) {
                        case 0x0: r = a * b; break;                      // FMUL
                        case 0x1: r = a / b; break;                      // FDIV
                        case 0x2: r = a + b; break;                      // FADD
                        case 0x3: r = a - b; break;                      // FSUB
                        case 0x4: r = std::fmax(a, b); break;            // FMAX
                        case 0x5: r = std::fmin(a, b); break;            // FMIN
                        case 0x6: r = std::fmax(a, b); break;            // FMAXNM
                        case 0x7: r = std::fmin(a, b); break;            // FMINNM
                        case 0x8: r = -(a * b); break;                   // FNMUL
                        default: r = 0; break;
                    }
                    write_fp_s(cpu, rd, r);
                }
                return;
            }
            // SIMD scalar/vector 2-source FP (0x7E group): FABD.
            // Encoding: bits[31:24]=0x7E, bits[15:12]=0xD (FABD opcode),
            // bits[11:10]=0b01. bit22: 0=single, 1=double (not masked).
            // FABD computes |a - b|. musl's fabsf(got-want) is lowered to
            // `fabd s_, s0, s1` by the compiler; without this handler it
            // returns the first operand unchanged, breaking float
            // comparisons.
            if ((op & 0xFF00FC00) == 0x7E00D400) {
                bool is_double = (op >> 22) & 1;
                if (is_double) {
                    double a = read_fp_d(cpu, rn), b = read_fp_d(cpu, rm);
                    write_fp_d(cpu, rd, std::fabs(a - b));
                } else {
                    float a = read_fp_s(cpu, rn), b = read_fp_s(cpu, rm);
                    write_fp_s(cpu, rd, std::fabsf(a - b));
                }
                return;
            }
            // FP compare-with-zero: FCMGE/FCMGT/FCMLE/FCMLT <Dd>,<Dn>,#0.0
            // (also the 2D/4S/2S vector forms, size bits free).
            //   FCMGE 0x7EE08800  FCMGT 0x7EE08C00
            //   FCMLE 0x7EE0C800  FCMLT 0x7EE0CC00
            // mask 0xFF3F9000/0x7E208000: bits[31:24]=0x7E, bit21=1,
            // bits[20:16]=0, bit15=1, bit12=0; bit10 selects strict
            // (1=GT/LT, 0=GE/LE) and bit14 selects the swapped operand
            // (1=compare #0 vs Dn: LE/LT). libgcc's aarch64 unwinder
            // (uw_frame_state_for) lowers `context->ra` + a flag mask via
            // `cmge d0,d0,#0` (0x7EE08800) — a silent NOP here OR'd
            // 0x4000000000000000 into the pc, crashing every C++
            // exception with a bogus _Unwind_Find_FDE address.
            if ((op & 0xFF3F9000) == 0x7E208000) {
                // Result is a compare MASK (all-ones/all-zeros), not 1.0/0.0:
                // libgcc's aarch64 unwinder (uw_frame_state_for) does
                // `cmge d0,d0,#0` (0x7EE08800), `fmov x0,d0`, `add x0,ra,x0`
                // to fold a flag mask into the FDE pc (ra-1 when set). A
                // 1.0/0.0 result would OR 0x3FF0000000000000 into the pc.
                bool is_double = (op >> 22) & 1;
                bool strict    = (op >> 10) & 1;   // 0: GE/LE, 1: GT/LT
                bool swapped   = (op >> 14) & 1;   // 0: Dn op #0, 1: #0 op Dn
                if (is_double) {
                    double a = read_fp_d(cpu, rn);
                    bool t = swapped ? (strict ? (0.0 > a) : (0.0 >= a))
                                     : (strict ? (a > 0.0) : (a >= 0.0));
                    cpu.v_lo[rd] = t ? ~0ULL : 0ULL;
                    cpu.v_hi[rd] = t ? ~0ULL : 0ULL;
                } else {
                    float a = read_fp_s(cpu, rn);
                    bool t = swapped ? (strict ? (0.0f > a) : (0.0f >= a))
                                     : (strict ? (a > 0.0f) : (a >= 0.0f));
                    cpu.v_lo[rd] = t ? 0xFFFFFFFFu : 0u;
                    cpu.v_hi[rd] = 0;
                }
                return;
            }
            // FCMP/FCMPE: FP compare (sets NZCV).
            // Uses shared fp_decode helpers for encoding detection.
            // Must be checked BEFORE FP 1-source (below) because both
            // have bits[11:10]=0b00; without this explicit check,
            // FCMP would be misdecoded as FNEG.
            if (fp_decode::is_fcmp(op)) {
                bool with_zero = fp_decode::fcmp_with_zero(op);
                uint32_t nzcv;
                auto set_nzcv = [&](bool unordered, bool less, bool equal) {
                    // ARM FCMP NZCV (bits[31:28] = N Z C V):
                    //   unordered: N=0 Z=0 C=1 V=1 = 0x30000000
                    //   less:      N=1 Z=0 C=0 V=0 = 0x80000000
                    //   equal:     N=0 Z=1 C=1 V=0 = 0x60000000
                    //   greater:   N=0 Z=0 C=1 V=0 = 0x20000000
                    // (Previous code used 0x28000000 for unordered, which
                    //  decodes as N=0 Z=0 C=1 V=0 — same as "greater" —
                    //  diverging from both the JIT and the ARM ARM.)
                    if (unordered)      nzcv = 0x30000000;
                    else if (less)       nzcv = 0x80000000;
                    else if (equal)      nzcv = 0x60000000;
                    else                 nzcv = 0x20000000;
                };
                if (ftype == 1) {  // double
                    double a = read_fp_d(cpu, rn);
                    double b = with_zero ? 0.0 : read_fp_d(cpu, rm);
                    if (std::isnan(a) || std::isnan(b))
                        set_nzcv(true, false, false);
                    else if (a < b) set_nzcv(false, true, false);
                    else if (a > b) set_nzcv(false, false, false);
                    else            set_nzcv(false, false, true);
                } else if (ftype == 0) {  // single
                    float a = read_fp_s(cpu, rn);
                    float b = with_zero ? 0.0f : read_fp_s(cpu, rm);
                    if (std::isnan(a) || std::isnan(b))
                        set_nzcv(true, false, false);
                    else if (a < b) set_nzcv(false, true, false);
                    else if (a > b) set_nzcv(false, false, false);
                    else            set_nzcv(false, false, true);
                } else {
                    nzcv = 0x30000000;  // half-precision: treat as unordered
                }
                cpu.pstate = (cpu.pstate & 0x0FFFFFFF) | nzcv;
                return;
            }
            // ── FCCMP/FCCMPE: FP conditional compare ──────────────────
            // `fccmp d1, d2, #0x0, eq` = 0x1e620420.
            // Encoding: bits[31:24]=0x1E, bit21=1, bits[11:10]=01,
            // Rm=bits[20:16], cond=bits[15:12], nzcv=bits[3:0],
            // E bit=bit[4] (FCCMPE — we don't model FP exceptions, so
            // FCCMPE behaves identically to FCCMP here).
            // If cond is true: compare Fn vs Fm and set NZCV like FCMP.
            // If false: set NZCV = nzcv immediate. Without this the
            // instruction was silently NOP'd, leaving NZCV stale and
            // corrupting every subsequent conditional branch (Qt raster
            // code compares bezier/color values with FCCMP).
            if ((op & 0xFF200C00) == 0x1E200400) {
                uint8_t cond = (op >> 12) & 0xF;
                uint8_t nzcv = op & 0xF;
                if (cond_true(cond, cpu.pstate)) {
                    bool unordered = false, less = false, equal = false;
                    if (ftype == 1) {  // double
                        double a = read_fp_d(cpu, rn);
                        double b = read_fp_d(cpu, rm);
                        if (std::isnan(a) || std::isnan(b))       unordered = true;
                        else if (a < b)                            less = true;
                        else if (a > b)                            { /* greater */ }
                        else                                       equal = true;
                    } else {  // single (or half — no native FP16)
                        float a = read_fp_s(cpu, rn);
                        float b = read_fp_s(cpu, rm);
                        if (std::isnan(a) || std::isnan(b))       unordered = true;
                        else if (a < b)                            less = true;
                        else if (a > b)                            { /* greater */ }
                        else                                       equal = true;
                    }
                    uint32_t n;
                    if (unordered)      n = 0x30000000;
                    else if (less)       n = 0x80000000;
                    else if (equal)      n = 0x60000000;
                    else                 n = 0x20000000;
                    cpu.pstate = (cpu.pstate & 0x0FFFFFFF) | n;
                } else {
                    uint32_t n = static_cast<uint32_t>(nzcv) << 28;
                    cpu.pstate = (cpu.pstate & 0x0FFFFFFF) | n;
                }
                return;
            }
            // FP 1-source: FMOV/FABS/FNEG/FSQRT/FRINT*
            // Uses shared fp_decode helpers. The 6-bit opcode is in
            // bits[20:15] (= rmode:opcode in the ARM ARM).
            if (fp_decode::is_fp_1source(op)) {
                uint8_t opcode = fp_decode::fp_1source_opcode(op);
                if (ftype) {
                    double a = read_fp_d(cpu, rn), r = 0;
                    switch (opcode) {
                        case 0x0: r = a; break;                        // FMOV
                        case 0x1: r = std::fabs(a); break;             // FABS
                        case 0x2: r = -a; break;                       // FNEG
                        case 0x3: r = std::sqrt(a); break;             // FSQRT
                        case 0x8: r = std::rint(a); break;             // FRINTN
                        case 0x9: r = std::ceil(a); break;             // FRINTP
                        case 0xA: r = std::floor(a); break;            // FRINTM
                        case 0xB: r = std::trunc(a); break;            // FRINTZ
                        case 0xC: r = std::rint(a); break;             // FRINTA
                        // 0xD unused in A64
                        case 0xE: r = std::rint(a); break;             // FRINTX
                        case 0xF: r = std::rint(a); break;             // FRINTI
                        default: r = a; break;
                    }
                    write_fp_d(cpu, rd, r);
                } else {
                    float a = read_fp_s(cpu, rn), r = 0;
                    switch (opcode) {
                        case 0x0: r = a; break;
                        case 0x1: r = std::fabsf(a); break;
                        case 0x2: r = -a; break;
                        case 0x3: r = std::sqrtf(a); break;
                        case 0x8: r = std::rintf(a); break;             // FRINTN
                        case 0x9: r = std::ceilf(a); break;             // FRINTP
                        case 0xA: r = std::floorf(a); break;            // FRINTM
                        case 0xB: r = std::truncf(a); break;            // FRINTZ
                        case 0xC: r = std::rintf(a); break;             // FRINTA
                        // 0xD unused in A64
                        case 0xE: r = std::rintf(a); break;             // FRINTX
                        case 0xF: r = std::rintf(a); break;             // FRINTI
                        default: r = a; break;
                    }
                    write_fp_s(cpu, rd, r);
                }
                return;
            }
            // FCVT{N,P,M,Z,A}{S,U} — FP to int with explicit rounding mode.
            // Encoding (integer variant, bit 21 = 1):
            //   0x1E200000 = FCVTNS, 0x1E280000 = FCVTPS,
            //   0x1E300000 = FCVTMS, 0x1E240000 = FCVTAS,
            //   0x1E380000 = FCVTZS/FCVTZU.
            // The rounding mode is in bits[20:19]:
            //   00 = N (nearest even), 01 = P (+inf), 10 = M (-inf),
            //   11 = Z (zero); bit[18]=1 selects A (nearest, ties away
            //   from zero). bit[16]=1 selects the unsigned variant.
            // Mask 0x7F220000 leaves bits[20:19], bit 18 and bit 16 free
            // so every rounding/unsigned variant matches; the old mask
            // (0x7F3E0000 == 0x1E280000) only matched FCVTPS, silently
            // NOPing FCVTNS/FCVTMS/FCVTAS. bits[15:10]==0 excludes FCSEL
            // (0x1E200C00, bits[13:10]=1100), which otherwise collides.
            // The fixed-point variant (bit 21 = 0) is dispatched below.
            if ((op & 0x7F220000) == 0x1E200000 && ((op >> 10) & 0x3F) == 0) {
                bool is_away = (op >> 18) & 1;       // FCVTAS/FCVTAU
                uint8_t rmode = (op >> 19) & 0x3;    // bits 20:19: 0=N,1=P,2=M,3=Z
                bool is_unsigned = ((op >> 16) & 1);  // bit 16 = U
                bool is_64bit = sf_val;
                auto round_d = [&](double v) -> int64_t {
                    if (is_away) return static_cast<int64_t>(std::round(v));  // A: ties away from zero
                    switch (rmode) {
                        case 0: return static_cast<int64_t>(std::llrint(v));   // N
                        case 1: return static_cast<int64_t>(std::ceil(v));     // P
                        case 2: return static_cast<int64_t>(std::floor(v));    // M
                        default: return static_cast<int64_t>(std::trunc(v));   // Z
                    }
                };
                auto round_s = [&](float v) -> int64_t {
                    if (is_away) return static_cast<int64_t>(std::roundf(v));
                    switch (rmode) {
                        case 0: return static_cast<int64_t>(std::llrintf(v));
                        case 1: return static_cast<int64_t>(std::ceilf(v));
                        case 2: return static_cast<int64_t>(std::floorf(v));
                        default: return static_cast<int64_t>(std::truncf(v));
                    }
                };
                if (ftype) {
                    double a = read_fp_d(cpu, rn);
                    if (is_unsigned) {
                        uint64_t v = (a < 0) ? 0 : static_cast<uint64_t>(round_d(a));
                        cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
                    } else {
                        int64_t v = round_d(a);
                        cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(v));
                    }
                } else {
                    float a = read_fp_s(cpu, rn);
                    if (is_unsigned) {
                        uint64_t v = (a < 0) ? 0 : static_cast<uint64_t>(round_s(a));
                        cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
                    } else {
                        int64_t v = round_s(a);
                        cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(v));
                    }
                }
                return;
            }
            // FCVT (between FP precisions)
            if ((op & 0xFFFFFC00) == 0x1E624000) { // FCVT Sd, Dn
                write_fp_s(cpu, rd, static_cast<float>(read_fp_d(cpu, rn))); return;
            }
            if ((op & 0xFFFFFC00) == 0x1E22C000) { // FCVT Dd, Sn
                write_fp_d(cpu, rd, static_cast<double>(read_fp_s(cpu, rn))); return;
            }
            // FCVT H — half-precision conversions. We don't model 16-bit FP
            // natively, but we can route H↔S via host __gnu_f2h_ieee / __gnu_h2f_ieee
            // (or std::floor of a manual conversion) so at least the value
            // survives round-tripping. Common case is musl's printf path,
            // which sometimes uses FCVT Hn, Dn for hex-float formatting.
            if ((op & 0xFFFFFC00) == 0x1E63C000) { // FCVT Hd, Dn (D → H)
                // Half is stored in low 16 bits of v_lo.
                double d = read_fp_d(cpu, rn);
                uint16_t hbits = d2h(d);
                cpu.v_lo[rd] = hbits; cpu.v_hi[rd] = 0;
                return;
            }
            if ((op & 0xFFFFFC00) == 0x1E23C000) { // FCVT Hn, Sn (S → H)
                float f = read_fp_s(cpu, rn);
                uint16_t hbits = f2h(f);
                cpu.v_lo[rd] = hbits; cpu.v_hi[rd] = 0;
                return;
            }
            if ((op & 0xFFFFFC00) == 0x1E634000) { // FCVT Sn, Hn (H → S)
                uint16_t hbits = static_cast<uint16_t>(cpu.v_lo[rn] & 0xFFFF);
                float f = h2f(hbits);
                write_fp_s(cpu, rd, f);
                return;
            }
            if ((op & 0xFFFFFC00) == 0x1E224000) { // FCVT Dd, Hn (H → D)
                uint16_t hbits = static_cast<uint16_t>(cpu.v_lo[rn] & 0xFFFF);
                double d = static_cast<double>(h2f(hbits));
                write_fp_d(cpu, rd, d);
                return;
            }
            // FCVTZS/FCVTZU (integer variant)
            // Mask 0x7F3E0000 with constant 0x1E380000 requires bit 21 = 1
            // (integer variant). The fixed-point variant (bit 21 = 0) is
            // handled separately below.
            if ((op & 0x7F3E0000) == 0x1E380000) {  // FCVTZS/FCVTZU
                bool is_unsigned = ((op >> 16) & 1);
                bool is_64bit = sf_val;
                if (ftype) {
                    double a = read_fp_d(cpu, rn);
                    if (is_unsigned) {
                        uint64_t v = (a < 0) ? 0 : static_cast<uint64_t>(a);
                        cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
                    } else {
                        int64_t v = static_cast<int64_t>(a);
                        cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(v));
                    }
                } else {
                    float a = read_fp_s(cpu, rn);
                    if (is_unsigned) {
                        uint64_t v = (a < 0) ? 0 : static_cast<uint64_t>(a);
                        cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
                    } else {
                        int64_t v = static_cast<int64_t>(a);
                        cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v) : static_cast<uint32_t>(static_cast<int32_t>(v));
                    }
                }
                return;
            }
            // ── Fixed-point int↔FP conversions (SCVTF/UCVTF/FCVTZS/FCVTZU #fbits) ──
            // Decoded by the generated table (tools/opgen/fp_fixconv.txt →
            // include/opgen_fpfixed.hpp) so interp, IR translator and JIT
            // gate share one mask set. Subops:
            //   0 = SCVTF/UCVTF int→FP, GPR source (U=bit16, sf=bit31,
            //       fbits = 64-bits[15:10])
            //   1 = FCVTZS/FCVTZU FP→int, GPR dest (same field decode)
            //   2 = SCVTF/UCVTF int→FP, FP source/dest (U=bit29, size=bit22,
            //       fbits = 64-bits[21:16])
            //   3 = FCVTZS/FCVTZU FP→int, FP source/dest (same field decode)
            {
                fpfixed::Op fc = fpfixed::classify(op);
                if (fc.family == fpfixed::Family::FIXCONV) {
                    if (fc.subop <= 1) {
                        // FPDataProc1 forms: GPR source/dest.
                        bool is_unsigned = ((op >> 16) & 1);
                        bool is_64bit = sf_val;
                        int fbits = 64 - static_cast<int>((op >> 10) & 0x3F);
                        if (fc.subop == 1) {
                            // FCVTZS/FCVTZU: scale by 2^fbits, truncate toward
                            // zero, saturate to dest range, NaN → 0. Without
                            // this handler every fixed-point FCVTZU (e.g. toybox
                            // MD5 K-table init `fcvtzu w1, d0, #32`) was silently
                            // NOP'd, leaving wrong hashes.
                            double a = ftype ? read_fp_d(cpu, rn)
                                             : static_cast<double>(read_fp_s(cpu, rn));
                            double scaled = std::ldexp(a, fbits);
                            if (is_unsigned) {
                                double hi = is_64bit ? 18446744073709551616.0
                                                     : 4294967296.0;
                                uint64_t v = (std::isnan(a) || scaled < 0.0) ? 0
                                           : (scaled >= hi) ? (is_64bit ? ~0ULL : 0xFFFFFFFFu)
                                           : static_cast<uint64_t>(scaled);
                                cpu.regs[rd] = is_64bit ? v : static_cast<uint32_t>(v);
                            } else {
                                double hi = is_64bit ? 9223372036854775808.0
                                                     : 2147483648.0;
                                double lo = -hi;
                                int64_t v = std::isnan(a) ? 0
                                          : (scaled >= hi) ? (is_64bit ? INT64_MAX : INT32_MAX)
                                          : (scaled < lo)  ? (is_64bit ? INT64_MIN : INT32_MIN)
                                          : static_cast<int64_t>(scaled);
                                cpu.regs[rd] = is_64bit ? static_cast<uint64_t>(v)
                                                        : static_cast<uint32_t>(static_cast<int32_t>(v));
                            }
                        } else {
                            // SCVTF/UCVTF: convert integer to FP, divide by
                            // 2^fbits (treat the source as fixed-point).
                            double v = is_unsigned
                                ? static_cast<double>(is_64bit ? cpu.regs[rn]
                                                               : static_cast<uint32_t>(cpu.regs[rn]))
                                : static_cast<double>(is_64bit ? static_cast<int64_t>(cpu.regs[rn])
                                                               : static_cast<int32_t>(cpu.regs[rn]));
                            double result = std::ldexp(v, -fbits);
                            if (ftype) write_fp_d(cpu, rd, result);
                            else       write_fp_s(cpu, rd, static_cast<float>(result));
                        }
                    } else {
                        // AdvSIMD-scalar forms: FP register source/dest.
                        // GCC emits `scvtf s0, s0, #1` (0x5F3FE400) when the
                        // integer already sits in an FP register (e.g. `(float)x`
                        // with x loaded from memory). The old silent NOP left the
                        // dest holding the raw integer bit pattern as a denormal
                        // float, breaking the inRect() hit test in rudolf-cart.
                        bool is_unsigned = (op >> 29) & 1;
                        bool is_double = (op >> 22) & 1;
                        int fbits = 64 - static_cast<int>((op >> 16) & 0x3F);
                        uint64_t src_bits = cpu.v_lo[rn];
                        if (fc.subop == 2) {
                            if (is_double) {
                                double v = is_unsigned
                                    ? static_cast<double>(static_cast<uint64_t>(src_bits))
                                    : static_cast<double>(static_cast<int64_t>(src_bits));
                                write_fp_d(cpu, rd, std::ldexp(v, -fbits));
                            } else {
                                float v = is_unsigned
                                    ? static_cast<float>(static_cast<uint32_t>(src_bits))
                                    : static_cast<float>(static_cast<int32_t>(src_bits));
                                write_fp_s(cpu, rd, std::ldexpf(v, -fbits));
                            }
                        } else {
                            double a = is_double ? read_fp_d(cpu, rn)
                                                 : static_cast<double>(read_fp_s(cpu, rn));
                            double scaled = std::ldexp(a, fbits);
                            if (is_unsigned) {
                                double hi = is_double ? 18446744073709551616.0
                                                      : 4294967296.0;
                                uint64_t out = (std::isnan(a) || scaled < 0.0) ? 0
                                           : (scaled >= hi) ? (is_double ? ~0ULL : 0xFFFFFFFFu)
                                           : static_cast<uint64_t>(scaled);
                                // Write to Sd zeroes the upper 32 bits (matches
                                // the two-register-misc FCVTZS handler below).
                                cpu.v_lo[rd] = is_double ? out : (out & 0xFFFFFFFFULL);
                                cpu.v_hi[rd] = 0;
                            } else {
                                double hi = is_double ? 9223372036854775808.0
                                                      : 2147483648.0;
                                int64_t out = std::isnan(a) ? 0
                                            : (scaled >= hi) ? (is_double ? INT64_MAX : INT32_MAX)
                                            : (scaled < -hi) ? (is_double ? INT64_MIN : INT32_MIN)
                                            : static_cast<int64_t>(scaled);
                                cpu.v_lo[rd] = is_double ? static_cast<uint64_t>(out)
                                                         : (static_cast<uint32_t>(out) & 0xFFFFFFFFULL);
                                cpu.v_hi[rd] = 0;
                            }
                        }
                    }
                    return;
                }
            }
            // SCVTF/UCVTF (integer variant)
            // Mask 0x7F3EFC00 with constant 0x1E220000 requires bit 21 = 1
            // and bits[15:10] == 0. The 0xFC00 bits are essential: FCSEL
            // (0x1E220C01, bits[15:10] = 0b0011) matches the old loose mask
            // 0x7F3E0000 and was mis-executed as SCVTF (int→FP), converting
            // the hash GPR into an FP register. This broke grad3's
            // `fcsel s1, s0, s2, eq` (hash=151 → s1 became 151.0f).
            if ((op & 0x7F3EFC00) == 0x1E220000) {  // SCVTF/UCVTF
                bool is_unsigned = ((op >> 16) & 1);
                bool is_64bit = sf_val;
                if (ftype) {
                    if (is_unsigned) {
                        uint64_t v = is_64bit ? cpu.regs[rn] : static_cast<uint32_t>(cpu.regs[rn]);
                        write_fp_d(cpu, rd, static_cast<double>(v));
                    } else {
                        int64_t v = is_64bit ? static_cast<int64_t>(cpu.regs[rn]) : static_cast<int32_t>(cpu.regs[rn]);
                        write_fp_d(cpu, rd, static_cast<double>(v));
                    }
                } else {
                    if (is_unsigned) {
                        uint64_t v = is_64bit ? cpu.regs[rn] : static_cast<uint32_t>(cpu.regs[rn]);
                        write_fp_s(cpu, rd, static_cast<float>(v));
                    } else {
                        int64_t v = is_64bit ? static_cast<int64_t>(cpu.regs[rn]) : static_cast<int32_t>(cpu.regs[rn]);
                        write_fp_s(cpu, rd, static_cast<float>(v));
                    }
                }
                return;
            }
            // ── SIMD scalar int↔FP conversions (FP source/dest) ───────────────
            // These are in the "Advanced SIMD scalar two-register miscellaneous"
            // group (bits[31:24]=0x5E, bit 30=Q=1 for scalar form). They
            // differ from the standard FP scalar forms (0x1E...) by having
            // the integer source/dest in an FP register (Dn/Sn) instead of
            // a GPR (Xn/Wn). GCC/clang emit these for `(double)long_var`
            // when the long is already in an FP register from a load —
            // saving a GPR move.
            //
            // Encoding layout (after masking out U/size/opcode/Rn/Rd):
            //   bits[31:24] = 0x5E (constant — scalar SIMD)
            //   bit 29      = U (0=signed, 1=unsigned)
            //   bits[23:22] = size (prec + int width, see per-op below)
            //   bits[21:17] = 10000 (constant)
            //   bits[16:12] = opcode (5 bits)
            //   bits[11:10] = 10 (constant)
            //   bits[9:5]   = Rn (source FP reg)
            //   bits[4:0]   = Rd (dest FP reg)
            //
            // Group mask: 0xDF3E0C00 (bit 29 = U, allowed to vary),
            // group constant: 0x5E200800 (with U=0).
            // SCVTF/UCVTF opcode = 11101 (0x1D) → constant 0x5E21D800.
            // FCVTZS/FCVTZU opcode = 11011 (0x1B) → constant 0x5E21B800.
            //
            // For SCVTF/UCVTF (int→FP):
            //   size=01 → 64-bit int src, double-precision dest (Dd, Dn)
            //   size=00 → 32-bit int src, single-precision dest (Sd, Sn)
            // For FCVTZS/FCVTZU (FP→int):
            //   size=11 → double-precision src, 64-bit int dest (Dd, Dn)
            //   size=10 → single-precision src, 32-bit int dest (Sd, Sn)
            if ((op & 0xDF3E0C00) == 0x5E200800) {
                bool is_unsigned = (op >> 29) & 1;
                uint8_t opcode = (op >> 12) & 0x1F;
                bool is_double = (op >> 22) & 1;  // also doubles as "is 64-bit int"
                if (opcode == 0x1D) {  // SCVTF/UCVTF (int → FP, FP source)
                    // Read integer bits from FP source register.
                    uint64_t src_bits = cpu.v_lo[rn];
                    if (is_double) {
                        // 64-bit int → double-precision float
                        double v = is_unsigned
                            ? static_cast<double>(static_cast<uint64_t>(src_bits))
                            : static_cast<double>(static_cast<int64_t>(src_bits));
                        write_fp_d(cpu, rd, v);
                    } else {
                        // 32-bit int → single-precision float
                        float v = is_unsigned
                            ? static_cast<float>(static_cast<uint32_t>(src_bits))
                            : static_cast<float>(static_cast<int32_t>(src_bits));
                        write_fp_s(cpu, rd, v);
                    }
                    return;
                }
                if (opcode == 0x1B) {  // FCVTZS/FCVTZU (FP → int, FP dest)
                    if (is_double) {
                        double v = read_fp_d(cpu, rn);
                        uint64_t out = std::isnan(v) ? 0
                            : (is_unsigned
                               ? static_cast<uint64_t>(v >= 18446744073709551616.0 ? UINT64_MAX
                                                      : v < 0.0 ? 0 : static_cast<uint64_t>(v))
                               : static_cast<uint64_t>(v >=  9223372036854775808.0 ? INT64_MAX
                                                      : v < -9223372036854775808.0 ? INT64_MIN
                                                      : static_cast<int64_t>(v)));
                        cpu.v_lo[rd] = out;
                    } else {
                        float v = read_fp_s(cpu, rn);
                        uint64_t out = std::isnan(v) ? 0
                            : (is_unsigned
                               ? static_cast<uint64_t>(static_cast<uint32_t>(v >= 4294967296.0f ? UINT32_MAX
                                                                          : v < 0.0f ? 0 : static_cast<uint32_t>(v)))
                               : static_cast<uint64_t>(static_cast<uint32_t>(v >= 2147483648.0f ? INT32_MAX
                                                                          : v < -2147483648.0f ? INT32_MIN
                                                                          : static_cast<int32_t>(v))));
                        // Zero-extend 32-bit result into 64-bit FP register
                        cpu.v_lo[rd] = out & 0xFFFFFFFFULL;
                    }
                    return;
                }
                // Other opcodes in this group (FRINTN, FABS, etc.) are
                // encoded in the 0x1E... group, not 0x5E.... Fall through
                // to "Unknown FP" if we ever see a different opcode here.
            }
            // FCSEL
            if ((op & 0xFF200C00) == 0x1E200C00) {  // FCSEL (bit 21=0, bits[13:10]=1100)
                uint8_t cond = (op >> 12) & 0xF;
                if (ftype) {
                    double r = cond_true(cond, cpu.pstate) ? read_fp_d(cpu, rn) : read_fp_d(cpu, rm);
                    write_fp_d(cpu, rd, r);
                } else {
                    float r = cond_true(cond, cpu.pstate) ? read_fp_s(cpu, rn) : read_fp_s(cpu, rm);
                    write_fp_s(cpu, rd, r);
                }
                return;
            }
            // FMADD/FMSUB/FNMADD/FNMSUB
            // Encoding: bits[31:24]=0x1F, bit 15=o1 (sub), bit 21=o2 (neg).
            // matched FMADD/FMSUB (o2=0); FNMADD/FNMSUB (o2=1) fell
            // through to the "Unknown FP — NOP" path, silently
            // returning whatever was in Vd. This broke any guest
            // program that used FNMADD/FNMSUB (e.g. musl's __muldf3
            // long-double fallback for printf %Lf).
            //
            // Per ARM ARM, the four FMA variants are:
            //   FMADD  (o2=0, o1=0): Vd = Va + Vn*Vm       = c + a*b
            //   FMSUB  (o2=0, o1=1): Vd = Va - Vn*Vm       = c - a*b
            //   FNMADD (o2=1, o1=0): Vd = -Va - Vn*Vm      = -c - a*b
            //   FNMSUB (o2=1, o1=1): Vd = -Va + Vn*Vm      = a*b - c
            //
            // We model FMA3 fusion semantics by computing the product
            // and add/sub in a single C++ expression. C++ does NOT
            // guarantee single-rounding (the compiler may emit
            // separate mul+add machine instructions), so this matches
            // the JIT's non-FMA3 decomposed path — both produce
            // double-rounded results. On hosts with FMA3, the JIT's
            // FMA3 codegen produces single-rounded results, which
            // diverges from the interpreter for IEEE 754 edge cases
            // (e.g. mul=1e308 + acc=1e-300 → exact vs. ∞). This is
            // a known limitation; the fix requires FMA3 codegen in
            // the interpreter too (future work — currently we use
            // the C++ mul+add path).
            if ((op & 0xFF000000) == 0x1F000000) {
                uint8_t ra = (op >> 10) & 0x1F;
                bool sub = (op >> 15) & 1;   // o1
                bool neg = (op >> 21) & 1;   // o2
                if (ftype) {
                    double a = read_fp_d(cpu, rn), b = read_fp_d(cpu, rm),
                           c = read_fp_d(cpu, ra);
                    double prod = a * b;
                    double r;
                    if      (!neg && !sub) r = prod + c;        // FMADD  = Sa + Sn*Sm
                    else if (!neg &&  sub) r = c - prod;        // FMSUB  = Sa - Sn*Sm
                    else if ( neg && !sub) r = -prod - c;       // FNMADD = -Sa - Sn*Sm
                    else                   r = prod - c;       // FNMSUB = -Sa + Sn*Sm
                    write_fp_d(cpu, rd, r);
                } else {
                    float a = read_fp_s(cpu, rn), b = read_fp_s(cpu, rm),
                          c = read_fp_s(cpu, ra);
                    float prod = a * b;
                    float r;
                    if      (!neg && !sub) r = prod + c;        // FMADD  = Sa + Sn*Sm
                    else if (!neg &&  sub) r = c - prod;        // FMSUB  = Sa - Sn*Sm
                    else if ( neg && !sub) r = -prod - c;       // FNMADD = -Sa - Sn*Sm
                    else                   r = prod - c;       // FNMSUB = -Sa + Sn*Sm
                    write_fp_s(cpu, rd, r);
                }
                return;
            }
            // Unknown FP instruction — NOP (don't crash)
            {
                static uint64_t fp_nop_count_ = 0;
                if (fp_nop_count_ < 20) {
                    fprintf(stderr, "[FP-NOP] op=0x%08x pc=0x%llx\n", op,
                            (unsigned long long)cpu.pc);
                }
                fp_nop_count_++;
            }
            (void)sf_val; (void)rm;
            return;
        }
        default:
            // Not an FP/SIMD class — should never be called here.
            // The dispatcher in interpreter.cpp only routes FP/SIMD cases
            // to execute_fp(); reaching this default is a logic bug.
            break;
    }
}
} // namespace arm64emu
