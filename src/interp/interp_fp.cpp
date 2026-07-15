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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
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
    uint64_t bits; memcpy(&bits, &d, 8);
    cpu.v_lo[r] = bits; cpu.v_hi[r] = 0;
}
static inline void write_fp_s(CPU& cpu, int r, float f) {
    if (r < 0 || r > 31) return;  // defensive: prevent OOB write
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
            // BUGFIX (Turn 60, H11): single-structure LD1/ST1
            // (e.g. LD1 {Vt.S}[idx]) loads/stores ONE element at a
            // specific lane index, not a whole register. The old code
            // treated it as multi-structure (reading simd_count whole
            // registers), silently corrupting memory for any guest
            // using single-structure LD1/ST1 (matrix transpose, RGBA
            // channel interleaving, etc.).
            //
            // Single-structure LD1/ST1 1-element variant encoding
            // (per ARM ARM C4.1.66):
            //   bits[14:13] = size[1:0] (high bits of size)
            //   bit[12]     = 1 (single-structure marker)
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
                // Element size: 00=B(1), 01=H(2), 10=S(4), 11=D(8).
                uint8_t size_field = (d.raw >> 13) & 3;
                int esize = 1 << size_field;
                // Index: for LD1 1-reg, Q=0 → index = bit[12]:bit[11]
                // (but only valid for size=00,01; for size=10 index is
                // bit[12] only; for size=11 index must be 0).
                // Q=1 → index = bit[12] (for size=00,01,10); size=11 →
                // index = 0.
                // We compute a conservative index that works for the
                // common cases; the exact decode per ARM ARM is:
                //   if Q==0: idx = (size_field==0) ? (bits[12:11]) :
                //              (size_field==1) ? (bit[12]) : 0
                //   if Q==1: idx = (size_field==0) ? (bits[13:12]) :
                //              (size_field==1) ? (bits[13:12]>>1) :
                //              (size_field==2) ? (bit[13]) : 0
                // Simpler: re-extract from raw bits per ARM ARM table.
                int idx = 0;
                uint8_t sz = size_field;
                if (Q == 0) {
                    // 64-bit form: 8 bytes per register.
                    switch (sz) {
                        case 0: idx = (d.raw >> 11) & 3; break;  // B, 8 elems
                        case 1: idx = (d.raw >> 12) & 1; break;  // H, 4 elems
                        case 2: idx = (d.raw >> 13) & 1; break;  // S, 2 elems
                        case 3: idx = 0; break;                  // D, 1 elem
                    }
                } else {
                    // 128-bit form: 16 bytes per register.
                    switch (sz) {
                        case 0: idx = (d.raw >> 12) & 0xF; break;  // B, 16 elems
                        case 1: idx = (d.raw >> 13) & 7;  break;   // H, 8 elems
                        case 2: idx = (d.raw >> 14) & 3;  break;   // S, 4 elems
                        case 3: idx = (d.raw >> 15) & 1;  break;   // D, 2 elems
                    }
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
                return;
            }
            // Multi-structure LD1/ST1 (original path).
            int nregs = d.simd_count;
            for (int i = 0; i < nregs; i++) {
                int r = (d.rt + i) & 0x1F;
                uint64_t a = base + i * total_bytes;
                if (d.is_load) {
                    uint8_t buf[16];
                    mem_.read(a, buf, total_bytes, pcache);
                    memcpy(&cpu.v_lo[r], buf, 8);
                    if (total_bytes == 16) memcpy(&cpu.v_hi[r], buf + 8, 8);
                    else cpu.v_hi[r] = 0;
                } else {
                    uint8_t buf[16];
                    memcpy(buf, &cpu.v_lo[r], 8);
                    if (total_bytes == 16) memcpy(buf + 8, &cpu.v_hi[r], 8);
                    mem_.write(a, buf, total_bytes, pcache);
                }
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
                // BUGFIX (rc.1): for Q=1 (128-bit), elements with idx >=
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
            // ── TBL/TBX (Table Lookup) ──
            // AArch64 TBL/TBX permute bytes from one or two source
            // vectors using indices from a third vector. Each byte
            // index in the index vector selects a byte from the
            // concatenated source(s). Indices >= the source length:
            //   TBL: result byte = 0
            //   TBX: result byte unchanged (in-place update)
            //
            // The SIMD_DP sub-dispatch uses mask 0xFFE0FC00 with Q
            // (bit 30) and L (bit 20) stripped, but op2 (bit 21)
            // kept. So:
            //   - TBL (op2=0) matches case 0x0E000000 (both Q and L
            //     variants).
            //   - TBX (op2=1) matches case 0x0E200000.
            // We read Q (bit 30) and L (bit 20) from the raw op.
            //
            // The old code was a stub that just copied Vn to Vd — any
            // code doing byte shuffles (hex encode, UTF-8 conversion,
            // base64) would get wrong results silently.
            case 0x0E000000:   // TBL (op2=0; Q and L stripped)
            case 0x0E200000: { // TBX (op2=1; Q and L stripped)
                bool is_tbx     = (op & 0x200000) != 0;  // op2 bit (bit 21)
                bool is_two_src = (op & 0x1000)   != 0;  // L bit (bit 20)
                // Q bit (bit 30) — already extracted as `Q` above.
                // Build the source table: 16 (single) or 32 (two) bytes.
                uint8_t table[32];
                memcpy(table,    &cpu.v_lo[rn], 8);
                memcpy(table+8,  &cpu.v_hi[rn], 8);
                if (is_two_src) {
                    int rn2 = (rn + 1) & 31;
                    memcpy(table+16, &cpu.v_lo[rn2], 8);
                    memcpy(table+24, &cpu.v_hi[rn2], 8);
                }
                int table_len = is_two_src ? 32 : 16;
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
            // ── CMGE / CMHS (vector) ──
            // "CMHS" but was actually CMGE (signed >=, opcode 0x0D).
            // The code used unsigned comparison, so it was implementing
            // CMHS behavior under the wrong case label. The actual CMHS
            // instruction (opcode 0x0F → case 0x2E203C00 after sub_noq)
            // was not matched at all and fell through silently.
            //
            // This broke glibc's strchrnul SIMD loop, which uses CMHS
            // (unsigned >=) to detect both the search character AND
            // the NUL terminator in one comparison. Without CMHS
            // matching, the loop never detected the NUL terminator
            // and ran forever through unmapped zero pages.
            //
            // Encoding (SIMD two-register misc, bits 15:10 = opcode):
            //   CMGE (signed >=):   U=0, opcode=0b001101 → sub_noq=0x2E203400
            //   CMHS (unsigned >=): U=1, opcode=0b001111 → sub_noq=0x2E203C00
            // Note: sub_noq = (op & 0xFFE0FC00) with Q (bit 30) stripped.
            // The U bit (29) IS kept in sub_noq, so CMGE and CMHS have
            // DIFFERENT sub_noq values (0x2E203400 vs 0x2E203C00) because
            // they have different opcodes (0x0D vs 0x0F), NOT because of U.
            // (U=0 vs U=1 alone wouldn't change sub_noq since both 0x2E...
            // and 0x6E... strip to 0x2E... after removing Q.)
            case 0x2E203400: {  // CMGE (signed >=, opcode 0x0D)
                int esize = 1 << size;
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf_n[16], buf_m[16];
                memcpy(buf_n, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_m, &cpu.v_lo[rm], 8);
                if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                uint8_t out[16] = {0};
                for (int i = 0; i < elems; i++) {
                    // Sign-extend for signed comparison.
                    int64_t n = 0, m = 0;
                    memcpy(&n, buf_n + i*esize, esize);
                    memcpy(&m, buf_m + i*esize, esize);
                    if (esize == 1) { n = (int8_t)n; m = (int8_t)m; }
                    else if (esize == 2) { n = (int16_t)n; m = (int16_t)m; }
                    else if (esize == 4) { n = (int32_t)n; m = (int32_t)m; }
                    bool ge = (n >= m);
                    memset(out + i*esize, ge ? 0xFF : 0x00, esize);
                }
                memcpy(&cpu.v_lo[rd], out, 8);
                if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            case 0x2E203C00: {  // CMHS (unsigned >=, opcode 0x0F)
                int esize = 1 << size;
                int elems = (Q ? 16 : 8) / esize;
                uint8_t buf_n[16], buf_m[16];
                memcpy(buf_n, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf_n + 8, &cpu.v_hi[rn], 8);
                memcpy(buf_m, &cpu.v_lo[rm], 8);
                if (Q) memcpy(buf_m + 8, &cpu.v_hi[rm], 8);
                uint8_t out[16] = {0};
                for (int i = 0; i < elems; i++) {
                    uint64_t n = 0, m = 0;
                    memcpy(&n, buf_n + i*esize, esize);
                    memcpy(&m, buf_m + i*esize, esize);
                    bool ge = (n >= m);  // unsigned
                    memset(out + i*esize, ge ? 0xFF : 0x00, esize);
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
            case 0x2E20A400: {  // UMAXP/UMINP (U=1) and SMAXP/SMINP (U=0)
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
                // The old code combined Vn and Vm into a single max/min,
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
                // BUGFIX (rc.1): for Q=1 (128-bit), elements with index >=
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
            case 0x2E208C00: { // CMEQ (U=0 or U=1, after sub_noq)
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
            default: break;  // fall through to size-based checks below
            }
            // Sub-discriminator for 1-source vector ops (REV/CNT/CMEQ#0).
            // Mask off Q (30), U (29), size (23:22), Rm (20:16), Rn (9:5), Rd (4:0).
            //
            // BUGFIX (rc.1): the old mask 0xBFFFFC00 did NOT mask off the
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
            // ── CNT (vector) ──
            case 0x0E205800: {
                uint8_t buf[16];
                memcpy(buf, &cpu.v_lo[rn], 8);
                if (Q) memcpy(buf + 8, &cpu.v_hi[rn], 8);
                int nbytes = Q ? 16 : 8;
                for (int i = 0; i < nbytes; i++) buf[i] = __builtin_popcount(buf[i]);
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
            // EXT (extract) — v0 mask 0xFFE00000 val 0x6E000000
            // MISSED Q=0 form. Fixed: strip Q from the comparison.
            if ((op & 0xBFE00000) == 0x2E000000) {
                uint8_t imm4 = (op >> 11) & 0xF;
                uint8_t buf[32];
                memcpy(buf, &cpu.v_lo[rn], 8);
                memcpy(buf + 8, &cpu.v_hi[rn], 8);
                memcpy(buf + 16, &cpu.v_lo[rm], 8);
                memcpy(buf + 24, &cpu.v_hi[rm], 8);
                uint8_t out[16] = {0};
                int nbytes = Q ? 16 : 8;
                memcpy(out, buf + imm4, nbytes);
                memcpy(&cpu.v_lo[rd], out, 8);
                if (Q) memcpy(&cpu.v_hi[rd], out + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // MOVI (vector immediate, MSL form)
            // Top byte 0x2F/0x6F. Strip Q (bit 30) and U (bit 29).
            if (((op & ~((1u << 30) | (1u << 29))) & 0xFF001C00) == 0x0F001C00) {
                uint8_t imm8 = ((op >> 16) & 0x7) << 5 | ((op >> 5) & 0x1F);
                uint8_t msl = (op >> 13) & 3;
                uint64_t val = 0;
                for (int i = 0; i < 8; i++) val |= (static_cast<uint64_t>(imm8)) << (i * 8);
                val <<= (8 * msl);
                cpu.v_lo[rd] = val;
                if (Q) cpu.v_hi[rd] = val;
                else cpu.v_hi[rd] = 0;
                return;
            }
            // MOVI (vector immediate, all cmode forms) — top byte 0x0F/0x4F/0x6F
            // Encoding: 0 Q U 01110 abc defgh cmode 01 Rn Rd
            // After stripping Q (bit 30) and U (bit 29), check:
            //   bits[31:24] = 0x0F, bits[11:10] = 01
            // This matches ALL cmode values (0x0 through 0xE), not just 0xE.
            // Previously only cmode=0xE was matched, causing MOVI Vd.4S, #0
            // (cmode=0, used to zero V registers) to be silently ignored.
            // This broke toybox sh's stack zeroing (STP Q0,Q0 after MOVI
            // V0.4S,#0), corrupting the option parse node list.
            //
            // CRITICAL: must check immh (bits[23:20]) == 0 to distinguish
            // MOVI from SSHR/USHR/SHL. The "Advanced SIMD modified immediate"
            // and "Advanced SIMD shift by immediate" groups share bits[28:24]
            // = 01110 and bit 23 = 0 (for 32-bit shifts where immh < 8).
            // The ARM ARM resolves the ambiguity: if immh != 0, it's a shift;
            // if immh == 0, it's MOVI. The old code didn't check immh, so
            // SSHR/USHR/SHL with 32-bit elements were misdecoded as MOVI
            // (writing an immediate instead of shifting). This silently broke
            // every NEON shift by immediate — the root cause of the md5sum
            // failure (toybox's MD5 uses vshrq_n_u32 for rotates).
            if (((op & ~((1u << 30) | (1u << 29))) & 0xFF800C00) == 0x0F000400
                && ((op >> 20) & 0xF) == 0  // immh == 0 → MOVI/MVNI, not shift
                && (((op >> 10) & 0x3F) != 0x21  // exclude SHRN (bits[15:10]=100001)
                    || ((op >> 29) & 1))) {      // Turn 85: but NOT for MVNI (U=1)
                // source, which collides with the MOVI/MVNI pattern. SHRN
                // has bits[15:10] = 100001 (0x21), while MOVI/MVNI has
                // bits[11:10] = 00. Check bits[15:10] to distinguish.
                // Without this, `shrn v0.8b, v0.8h, #4` (0x0f0c8400) was
                // treated as MVNI, producing wrong results.
                uint8_t cmode = (op >> 12) & 0xF;
                uint8_t imm8 = ((op >> 16) & 0x7) << 5 | ((op >> 5) & 0x1F);
                // U bit (bit 29): 0 = MOVI, 1 = MVNI (invert).
                // IMPORTANT: for cmode=0xE (byte replication), the U bit
                // does NOT select MOVI/MVNI — both U=0 and U=1 are MOVI.
                // This is because binutils uses U=1 for 'movi vD.2d, #0'
                // to encode the 128-bit zero form. MVNI (invert) only
                // applies for cmode 0x0-0xD. Without this exception,
                // 'movi v1.2d, #0' (0x6F00E401, U=1, cmode=0xE) would
                // produce all-ones instead of all-zeros, breaking every
                // program that uses MOVI to zero a vector register.
                bool is_mvni = ((op >> 29) & 1) && (cmode != 0xE);
                if (cmode == 0xE) {
                    // cmode=0xE: broadcast imm8 to all bytes
                    uint64_t val = 0;
                    for (int i = 0; i < 8; i++) val |= (static_cast<uint64_t>(imm8)) << (i * 8);
                    if (is_mvni) val = ~val;
                    cpu.v_lo[rd] = val;
                    if (Q) cpu.v_hi[rd] = val;
                    else cpu.v_hi[rd] = 0;
                } else {
                    uint8_t buf[16] = {0};
                    if (cmode <= 0x1) {
                        int byte_pos = cmode & 1;
                        for (int lane = 0; lane < (Q ? 4 : 2); lane++) {
                            buf[lane * 4 + byte_pos] = imm8;
                        }
                    } else if (cmode <= 0x3) {
                        int byte_pos = cmode & 1;
                        for (int lane = 0; lane < (Q ? 8 : 4); lane++) {
                            buf[lane * 2 + byte_pos] = imm8;
                        }
                    } else if (cmode <= 0x5) {
                        int byte_pos = 1 + (cmode & 1);
                        for (int lane = 0; lane < (Q ? 4 : 2); lane++) {
                            buf[lane * 4 + byte_pos] = imm8;
                        }
                    } else if (cmode <= 0x7) {
                        int byte_pos = 2 + (cmode & 1);
                        for (int lane = 0; lane < (Q ? 4 : 2); lane++) {
                            buf[lane * 4 + byte_pos] = imm8;
                        }
                    } else {
                        int byte_pos = cmode & 0x7;
                        buf[byte_pos] = imm8;
                        if (Q) buf[8 + byte_pos] = imm8;
                    }
                    if (is_mvni) {
                        for (int i = 0; i < 16; i++) buf[i] = ~buf[i];
                    }
                    memcpy(&cpu.v_lo[rd], buf, 8);
                    if (Q) memcpy(&cpu.v_hi[rd], buf + 8, 8);
                    else cpu.v_hi[rd] = 0;
                }
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
            // BUGFIX (rc.1): the old code had TWO bugs:
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
                    v >>= shift;
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
            // BUGFIX (1.4.5-alpha): This handler was missing entirely.
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
                for (int i = 0; i < elems; i++) {
                    if (esize == 1) {
                        int8_t v; memcpy(&v, buf + i, 1);
                        v >>= shift;
                        memcpy(buf + i, &v, 1);
                    } else if (esize == 2) {
                        int16_t v; memcpy(&v, buf + i*2, 2);
                        v >>= shift;
                        memcpy(buf + i*2, &v, 2);
                    } else if (esize == 4) {
                        int32_t v; memcpy(&v, buf + i*4, 4);
                        v >>= shift;
                        memcpy(buf + i*4, &v, 4);
                    } else {
                        int64_t v; memcpy(&v, buf + i*8, 8);
                        v >>= shift;
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
                    v >>= shift;
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
                for (int i = 0; i < elems; i++) {
                    if (esize == 1) {
                        int8_t v; memcpy(&v, buf+i, 1);
                        uint8_t a; memcpy(&a, acc+i, 1);
                        v >>= shift; a += (uint8_t)v;
                        memcpy(acc+i, &a, 1);
                    } else if (esize == 2) {
                        int16_t v; memcpy(&v, buf+i*2, 2);
                        uint16_t a; memcpy(&a, acc+i*2, 2);
                        v >>= shift; a += (uint16_t)v;
                        memcpy(acc+i*2, &a, 2);
                    } else if (esize == 4) {
                        int32_t v; memcpy(&v, buf+i*4, 4);
                        uint32_t a; memcpy(&a, acc+i*4, 4);
                        v >>= shift; a += (uint32_t)v;
                        memcpy(acc+i*4, &a, 4);
                    } else {
                        int64_t v; memcpy(&v, buf+i*8, 8);
                        uint64_t a; memcpy(&a, acc+i*8, 8);
                        v >>= shift; a += (uint64_t)v;
                        memcpy(acc+i*8, &a, 8);
                    }
                }
                memcpy(&cpu.v_lo[rd], acc, 8);
                if (Q) memcpy(&cpu.v_hi[rd], acc + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // SLI (vector, immediate, shift left insert) — 0x2F005400.
            // SLI Vd.<T>, Vn.<T>, #shift → Vd = (Vn << shift) | (Vd >> (esize-shift)).
            // Used by MD5 for vector ROTL: ROTL(x, n) = SLI(x, x, n) when Vd==Vn.
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
                int insert_shift = esize_bits - shift;
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
                    uint64_t hi = (n << shift) & mask;
                    uint64_t lo = (insert_shift < esize_bits) ? (d >> insert_shift) : 0;
                    uint64_t r = hi | lo;
                    memcpy(vd + i*esize, &r, esize);
                }
                memcpy(&cpu.v_lo[rd], vd, 8);
                if (Q) memcpy(&cpu.v_hi[rd], vd + 8, 8);
                else cpu.v_hi[rd] = 0;
                return;
            }
            // SRI (vector, immediate, shift right insert) — 0x2F004400.
            // SRI Vd.<T>, Vn.<T>, #shift → Vd = (Vn >> shift) | (Vd << (esize-shift)).
            // Used for vector ROTR: ROTR(x, n) = SRI(x, x, n).
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
                int insert_shift = esize_bits - shift;
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
                    uint64_t lo = (n >> shift);
                    uint64_t hi = (insert_shift < esize_bits) ? ((d << insert_shift) & mask) : 0;
                    uint64_t r = hi | lo;
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
            // The old code used `if (immh == 1) esize=2` which missed immh=0
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
            // Fallback: NOP for SIMD ops we don't model. This is
            // incorrect but lets glibc continue. Programs that actually
            // depend on FP results will produce wrong output.
            // Optional: log unhandled SIMD ops for debugging.
            static const bool simd_trace_ = (getenv("BIFROST_SIMD_TRACE") != nullptr);
            if (simd_trace_) {
                static uint64_t simd_unhandled_count_ = 0;
                if (simd_unhandled_count_ < 50) {
                    fprintf(stderr, "[SIMD] unhandled op=0x%08x pc=0x%llx (Q=%d U=%d size=%d)\n",
                            op, static_cast<unsigned long long>(cpu.pc),
                            Q, (op>>29)&1, (op>>22)&3);
                    simd_unhandled_count_++;
                }
            }
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
            // Encoding: 0x7F000000 (mask 0xFF00FC00).
            if ((op & 0xFF00FC00) == 0x7F000000) {
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
            // FP register access + half-precision helpers are now
            // file-scope functions (read_fp_d, read_fp_s, write_fp_d,
            // write_fp_s, h2f, f2h, d2h) — see top of this file.
            // FMOV (general ↔ FP, 64-bit)
            // Bit[18]=1 distinguishes FMOV from SCVTF/UCVTF (bit[18]=0).
            // Without this, SCVTF (0x9E62xxxx) matches the FMOV mask.
            if ((op & 0xFFE0FC00) == 0x9E600000 && (op & (1u << 18))) {
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
            if ((op & 0xFFE0FC00) == 0x1E200000 && (op & (1u << 18))) {
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
            // Encoding: 0x1E280000 (FCVTNS) .. 0x1E390000 (FCVTZU).
            // The rounding mode is in bits[20:19]:
            //   00 = N (nearest even), 01 = P (+inf), 10 = M (-inf),
            //   11 = Z (zero); bit[16]=1 selects the unsigned variant.
            // Mask 0x7F3E0000 excludes bit 16 so both signed and
            // unsigned variants of N/P/M/A match here. (The Z variant
            // also matches here, but is explicitly dispatched to the
            // FCVTZS/FCVTZU path below for clarity; the result is the
            // same either way since rmode=3 → std::trunc.)
            if ((op & 0x7F3E0000) == 0x1E280000) {
                // FCVTNS/FCVTNM/FCVTNP/FCVTNU (and FCVTAS via rmode=0b1100)
                uint8_t rmode = (op >> 19) & 0x7;  // bits 21:19
                bool is_unsigned = ((op >> 16) & 1);  // bit 16 = U
                bool is_64bit = sf_val;
                // rmode: 0=N, 1=P, 2=M, 3=Z, 4=A
                auto round_d = [&](double v) -> int64_t {
                    switch (rmode) {
                        case 0: return static_cast<int64_t>(std::llrint(v));   // N
                        case 1: return static_cast<int64_t>(std::ceil(v));     // P
                        case 2: return static_cast<int64_t>(std::floor(v));    // M
                        case 3: return static_cast<int64_t>(std::trunc(v));    // Z
                        default: return static_cast<int64_t>(std::llrint(v));  // A
                    }
                };
                auto round_s = [&](float v) -> int64_t {
                    switch (rmode) {
                        case 0: return static_cast<int64_t>(std::llrintf(v));
                        case 1: return static_cast<int64_t>(std::ceilf(v));
                        case 2: return static_cast<int64_t>(std::floorf(v));
                        case 3: return static_cast<int64_t>(std::truncf(v));
                        default: return static_cast<int64_t>(std::llrintf(v));
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
            // FCVTZS/FCVTZU (fixed-point variant)
            // Same encoding as the integer variant but bit 21 = 0 and a
            // 6-bit scale field at bits[15:10] selects the number of
            // fractional bits (fbits = 64 - scale). Semantics: scale the
            // FP value by 2^fbits, convert to integer with truncation
            // toward zero, and saturate to the destination's range on
            // overflow. NaN → 0.
            //
            // Without this handler, every fixed-point FCVTZU (e.g.
            // toybox MD5 K-table init: `fcvtzu w1, d0, #32` for
            // floor(|sin|*2^32)) was silently NOP'd, leaving the
            // destination register unchanged and producing wrong hashes.
            if ((op & 0x7F3E0000) == 0x1E180000) {
                bool is_unsigned = ((op >> 16) & 1);
                bool is_64bit = sf_val;
                int fbits = 64 - static_cast<int>((op >> 10) & 0x3F);
                double a = ftype ? read_fp_d(cpu, rn)
                                 : static_cast<double>(read_fp_s(cpu, rn));
                double scaled = std::ldexp(a, fbits);
                // Saturating conversion. NaN maps to 0 (ARM ARM).
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
                return;
            }
            // SCVTF/UCVTF (integer variant)
            // Mask 0x7F3E0000 with constant 0x1E220000 requires bit 21 = 1.
            if ((op & 0x7F3E0000) == 0x1E220000) {  // SCVTF/UCVTF
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
            // SCVTF/UCVTF (fixed-point variant)
            // Bit 21 = 0 distinguishes from the integer variant; the 6-bit
            // scale at bits[15:10] selects fractional bits (fbits = 64 - scale).
            // Semantics: convert integer to FP and divide by 2^fbits — i.e.
            // treat the source integer as a fixed-point value.
            if ((op & 0x7F3E0000) == 0x1E020000) {
                bool is_unsigned = ((op >> 16) & 1);
                bool is_64bit = sf_val;
                int fbits = 64 - static_cast<int>((op >> 10) & 0x3F);
                double v = is_unsigned
                    ? static_cast<double>(is_64bit ? cpu.regs[rn]
                                                   : static_cast<uint32_t>(cpu.regs[rn]))
                    : static_cast<double>(is_64bit ? static_cast<int64_t>(cpu.regs[rn])
                                                   : static_cast<int32_t>(cpu.regs[rn]));
                double result = std::ldexp(v, -fbits);
                if (ftype) write_fp_d(cpu, rd, result);
                else       write_fp_s(cpu, rd, static_cast<float>(result));
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
                        // double-precision → 64-bit int
                        double v = read_fp_d(cpu, rn);
                        uint64_t out = is_unsigned
                            ? static_cast<uint64_t>(v >= 18446744073709551616.0 ? UINT64_MAX
                                                   : v < 0.0 ? 0 : static_cast<uint64_t>(v))
                            : static_cast<uint64_t>(v >=  9223372036854775808.0 ? INT64_MAX
                                                   : v < -9223372036854775808.0 ? INT64_MIN
                                                   : static_cast<int64_t>(v));
                        cpu.v_lo[rd] = out;
                    } else {
                        // single-precision → 32-bit int
                        float v = read_fp_s(cpu, rn);
                        uint64_t out = is_unsigned
                            ? static_cast<uint64_t>(static_cast<uint32_t>(v >= 4294967296.0f ? UINT32_MAX
                                                   : v < 0.0f ? 0 : static_cast<uint32_t>(v)))
                            : static_cast<uint64_t>(static_cast<uint32_t>(v >= 2147483648.0f ? INT32_MAX
                                                   : v < -2147483648.0f ? INT32_MIN
                                                   : static_cast<int32_t>(v)));
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
            // The old code's mask (op & 0xFF200000) == 0x1F000000 only
            // matched FMADD/FMSUB (o2=0); FNMADD/FNMSUB (o2=1) fell
            // through to the "Unknown FP — NOP" path, silently
            // returning whatever was in Vd. This broke any guest
            // program that used FNMADD/FNMSUB (e.g. musl's __muldf3
            // long-double fallback for printf %Lf).
            //
            // Per ARM ARM, the four FMA variants are:
            //   FMADD  (o2=0, o1=0): Vd = Va + Vn*Vm       = c + a*b
            //   FMSUB  (o2=0, o1=1): Vd = Va - Vn*Vm       = c - a*b
            //   FNMADD (o2=1, o1=0): Vd = -Vn*Vm + Va      = -a*b + c
            //   FNMSUB (o2=1, o1=1): Vd = -Vn*Vm - Va      = -a*b - c
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
                    if      (!neg && !sub) r = prod + c;        // FMADD
                    else if (!neg &&  sub) r = c - prod;        // FMSUB
                    else if ( neg && !sub) r = -prod + c;       // FNMADD
                    else                   r = -prod - c;       // FNMSUB
                    write_fp_d(cpu, rd, r);
                } else {
                    float a = read_fp_s(cpu, rn), b = read_fp_s(cpu, rm),
                          c = read_fp_s(cpu, ra);
                    float prod = a * b;
                    float r;
                    if      (!neg && !sub) r = prod + c;        // FMADD
                    else if (!neg &&  sub) r = c - prod;        // FMSUB
                    else if ( neg && !sub) r = -prod + c;       // FNMADD
                    else                   r = -prod - c;       // FNMSUB
                    write_fp_s(cpu, rd, r);
                }
                return;
            }
            // Unknown FP instruction — NOP (don't crash)
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
