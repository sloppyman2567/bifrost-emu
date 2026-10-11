// Instruction fallback prediction and bounded leaf-inlining eligibility.
#include "FrostJIT/Compiler/BlockAnalysis.hpp"
#include "FrostJIT/CodeGen/Native/NativeSIMD.hpp"
#include "System/Core/Emulator.hpp"
#include "System/Core/DebugFlags.hpp"
#include "Generated/SIMDTable.hpp"
#include "Generated/FixedPointTable.hpp"
namespace arm64emu {
namespace jit_analysis {
// ── instr_will_call_interp — heuristic for block splitting ─────────────
// Returns true if the given ARM64 instruction is likely to generate a
// CALL_INTERP IR op (i.e. the JIT can't codegen it natively). Used by
// translate_block to decide where to split blocks — too many
// CALL_INTERP fallbacks in one block causes register pressure issues.
//
// v1.4.5-alpha: moved here from Flags.cpp (where it was
// extracted by accident — it's only used by translate_block).
bool instr_will_call_interp(const DecodedInst& d) {
    if ((d.cls == InstClass::SIMD_DP || d.cls == InstClass::FP_SCALAR) &&
        classify_native_simd(d.raw) != NativeSimdKind::None) return false;
    switch (d.cls) {
        case InstClass::MSR: case InstClass::MSR_SYS:
            return d.sys_crn == 7;  // DC/IC → CALL_INTERP (Integer.cpp)
        case InstClass::SIMD_DP: {
            // Native SIMD_DP ops are classified by the generated table
            // (arm64emu::simd::classify, from Tools/CodeGen/simd_dp.txt).
            // Anything the table doesn't recognize falls back to the
            // interpreter. Previously this was a hand-written set of
            // sub3_noq/fp_key/sm masks mirroring FloatSIMD.cpp (and
            // silently drifting — AND/ORR/EOR/DUP native paths were missing,
            // forcing those blocks down the interpreter).
            const auto family = simd::classify(d.raw).family;
            return family == simd::Family::UNKNOWN || family == simd::Family::SHRN_SAT;
        }
        case InstClass::FP_SCALAR: {
            uint32_t op = d.raw;
            if (fp_decode::is_reciprocal(op)) return true;
            uint8_t ftype = (op >> 22) & 3;
            // Mirror FloatSIMD.cpp's FP_SCALAR handling: nearly all
            // scalar FP ops translate to native IR (FP_BINOP/FP_UNOP/FP_CMP/
            // FCVT/FP_F2I/FP_I2F/FMADD/FCSEL/FRINT/FP_MOVI/FMOV). Only
            // half-precision (ftype==3) and unmatchable encodings fall back
            // to CALL_INTERP. Previously this returned true for EVERY
            // FP_SCALAR, so FP-heavy blocks (e.g. the voxel game's
            // transform/render math) tripped the interp_only heuristic and
            // ran the interpreter — ~92% of dispatches never hit JIT code.
            //
            // BIFROST_FP_NATIVE_GATE=<bitmask> (debug/bisect): when set,
            // only FP groups whose bit is set are treated as native; all
            // others fall back to CALL_INTERP. Bitmap:
            //   0x01 FMOV GPR<->FP / FMOV FP<->FP / FMOV imm
            //   0x02 FCMP/FCMPE
            //   0x04 FP 2-source (FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FMAXNM/FMINNM/FNMUL) + FABD
            //   0x08 FP 1-source (FABS/FNEG/FSQRT/FRINT*)
            //   0x10 FCVTZS/FCVTZU (FP->int)
            //   0x20 FCVT D<->S + SCVTF/UCVTF (int->FP)
            //   0x40 FMA family
            //   0x80 FCSEL
            //   0x100 Scalar 64-bit shift-by-immediate (SHL/USHR/SSHR Dd,Dn,#imm)
            static int fp_gate = [] {
                const char* s = getenv("BIFROST_FP_NATIVE_GATE");
                return s ? static_cast<int>(strtol(s, nullptr, 0)) : -1;  // -1 = all native
            }();
            // NOTE on polarity: `native` means "the IR translator handles
            // this op natively" so we must predict NO interp call. The
            // return is therefore `fp_gate >= 0 && !(fp_gate & bit)`:
            //   gate unset (-1)      → all native → false
            //   gate set + bit set   → native     → false
            //   gate set + bit clear → fallback   → true
            // (Previously this was `fp_gate < 0 || (fp_gate & bit)`, which
            // under the default unset gate returned TRUE for every native
            // FP op — splitting FP-heavy blocks every 2 instructions and,
            // via the old interp_only heuristic, demoting tiny FP blocks
            // to the interpreter.)
            if ((op & 0xFF200C00) == 0x1E200400 && ftype <= 1)
                return fp_gate >= 0 && !(fp_gate & 0x02);
            // FMOV GPR↔FP (32/64-bit) — ftype-agnostic in the translator.
            if ((op & 0xFFE0FC00) == 0x9E600000 && (op & (1u << 18)) && (op & (1u << 17)))
                return fp_gate >= 0 && !(fp_gate & 0x01);
            if ((op & 0xFFE0FC00) == 0x1E200000 && (op & (1u << 18)) && (op & (1u << 17)))
                return fp_gate >= 0 && !(fp_gate & 0x01);
            if ((op & 0xFF20FC00) == 0x5E000400) {
                unsigned imm5 = (op >> 16) & 31;
                if (imm5 && imm5 != 16) return fp_gate >= 0 && !(fp_gate & 0x100);
            }
            if (ftype > 1)
                return true;  // half-precision → interpreter
            // FMOV FP↔FP (double 0x1E604000 / single 0x1E204000).
            if ((op & 0xFFFFFC00) == 0x1E604000 || (op & 0xFFFFFC00) == 0x1E204000)
                return fp_gate >= 0 && !(fp_gate & 0x01);
            // FMOV (scalar, immediate).
            if (fp_decode::is_fmov_imm(op))
                return fp_gate >= 0 && !(fp_gate & 0x01);
            // FCMP/FCMPE (register or #0.0 form).
            if (fp_decode::is_fcmp(op))
                return fp_gate >= 0 && !(fp_gate & 0x02);
            // FP 2-source (FADD/FSUB/FMUL/FDIV/FMAX/FMIN/FMAXNM/FMINNM/FNMUL).
            // FMAX/FMIN (opc 4/5) propagate nan but maxsd/minsd suppress
            // it, so codegen routes them via CALL_INTERP — predict interp.
            if (((op >> 21) & 1) == 1 && ((op >> 10) & 0x3) == 0b10
                && (op & 0xFF000000) == 0x1E000000 && ((op >> 12) & 0xF) <= 8) {
                uint8_t opc2 = (op >> 12) & 0xF;
                if (opc2 == 4 || opc2 == 5) return true;
                return fp_gate >= 0 && !(fp_gate & 0x04);
            }
            // FABD (scalar/vector, S/D via bit22).
            if ((op & 0xFF00FC00) == 0x7E00D400)
                return fp_gate >= 0 && !(fp_gate & 0x04);
            // FP 1-source: FABS/FNEG/FSQRT (1..3) and FRINT* (0x08..0x0F).
            // FRINTA uses the native truncate/fractional-adjustment sequence.
            if (fp_decode::is_fp_1source(op)) {
                uint8_t fp1 = fp_decode::fp_1source_opcode(op);
                if ((fp1 >= 1 && fp1 <= 3) || (fp1 >= 0x08 && fp1 <= 0x0F))
                    return fp_gate >= 0 && !(fp_gate & 0x08);
            }
            // FCVT{N,P,M,Z,A}{S,U} — FP→int with explicit rounding mode.
            // rmode = bits[20:19] (0=N,1=P,2=M,3=Z); bit[18]=A (ties-away);
            // bit[16]=U. Mirror of FloatSIMD.cpp's FP_SCALAR block:
            // Unsigned non-Z variants still fall back to CALL_INTERP.
            if ((op & 0x7F220000) == 0x1E200000 && ((op >> 10) & 0x3F) == 0) {
                bool away = (op >> 18) & 1;
                uint8_t rmode = (op >> 19) & 3;
                bool is_unsigned = (op >> 16) & 1;
                if (is_unsigned && (away || rmode != 3))
                    return true;
                return fp_gate >= 0 && !(fp_gate & 0x10);
            }
            // FCVT D↔S (0x1E624000 double→single, 0x1E22C000 single→double).
            if ((op & 0xFFFFFC00) == 0x1E624000 || (op & 0xFFFFFC00) == 0x1E22C000)
                return fp_gate >= 0 && !(fp_gate & 0x20);
            // SCVTF/UCVTF (int→FP).
            if ((op & 0x7F3EFC00) == 0x1E220000)
                return fp_gate >= 0 && !(fp_gate & 0x20);
            // Fixed-point int↔FP converts (SCVTF/UCVTF/FCVTZS/FCVTZU #fbits).
            // Classified via the generated table (Tools/CodeGen/fp_fixconv.txt)
            // so this gate can't drift from interp/IR. Covers the FPDataProc1
            // forms (GPR source/dest) AND the AdvSIMD-scalar forms with FP
            // register source/dest (e.g. GCC's `scvtf s0, s0, #1`).
            if (fpfixed::classify(op).family == fpfixed::Family::FIXCONV)
                return fp_gate >= 0 && !(fp_gate & 0x20);
            // SIMD-scalar int↔FP conversions, FP source/dest (0x5E200800
            // group: SCVTF/UCVTF opcode 0x1D, FCVTZS/FCVTZU opcode 0x1B).
            // Mirror of FloatSIMD.cpp's FP_SCALAR block — only those
            // two opcodes translate natively; any other opcode in the group
            // (FRINTN etc.) falls back to CALL_INTERP, so predict interp.
            if ((op & 0xDF3E0C00) == 0x5E200800) {
                uint8_t gopc = (op >> 12) & 0x1F;
                if (gopc == 0x1D || gopc == 0x1B)
                    return fp_gate >= 0 && !(fp_gate & 0x20);
                return true;
            }
            // FMA family (FMADD/FMSUB/FNMADD/FNMSUB).
            if ((op & 0xFF000000) == 0x1F000000)
                return fp_gate >= 0 && !(fp_gate & 0x40);
            // FCSEL.
            if ((op & 0xFF200C00) == 0x1E200C00)
                return fp_gate >= 0 && !(fp_gate & 0x80);
            // Scalar 64-bit shift-by-immediate (SHL/USHR/SSHR Dd, Dn, #imm).
            // Mirror of FloatSIMD.cpp's FP_SCALAR block — native
            // except shift==64 (all bits shifted out), which still
            // CALL_INTERPs to the interpreter's exact clear/sign-fill.
            if ((op & 0xFF00FC00) == 0x5F005400 ||
                (op & 0xFF00FC00) == 0x7F000400 ||
                (op & 0xFF00FC00) == 0x5F000400)
                return fp_gate >= 0 && !(fp_gate & 0x100);
            return true;  // rare/unsupported scalar FP → interpreter
        }
        case InstClass::LDXR: case InstClass::STXR:
        case InstClass::LDAXR: case InstClass::STLXR:
        case InstClass::LDAR: case InstClass::STLR:
            return false;
        default:
            break;
    }
    // 1.5.4-alpha: SIMD&FP LDR/STR (B/H/S/D/Q) and SIMD LDP/STP are all
    // natively translated (Memory.cpp) — the old is_vec gate
    // here forced every one of them to CALL_INTERP, splitting FP-heavy
    // blocks every 1-2 instructions and killing the pinned-XMM vec cache
    // (each FP load/store cost ~18 interpreter steps in the voxel game).
    return false;
}
// ── Leaf inlining scan (1.5.4-alpha) ────────────────────────────────────
// A small leaf function (no calls, no memory, no stack usage, straight-line
// or with a single forward conditional branch) can be inlined into its
// caller's block. Inlining replaces a BL_CALL — whose codegen flushes ALL
// vregs + the fp-cache writeback/reload (~26 movsd in fp-cache blocks) and
// dispatches through jit_call_helper — with the leaf's body translated
// inline, so the caller block continues at the leaf's RET. The game's
// grad3 (16 instrs, one forward b.eq) is the target: ~80K calls per
// worldgen column.
//
// The scan walks linearly from the BL target and must end at a RET within
// 16 instructions. Every instruction must be inline-compatible:
//   - no branches except ONE forward conditional (Bcond) whose skip region
//     [pc+4, pc+4+imm) stays inside the leaf (becomes a BRCOND_SKIP)
//   - no memory, no SP access, no system/syscall, no vector/SIMD
//   - carry-dependent conditions (CS/CC/HI/LS) rejected: the skip's jcc
//     needs a cmc dance that would corrupt CF on the taken path

LeafScanResult leaf_scan(Emulator& emu, uint64_t target) {
    LeafScanResult r;
    uint64_t pc = target;
    for (int n = 0; n < 16; n++) {
        uint32_t inst;
        try {
            inst = emu.mem().fetch_inst(pc);
        } catch (...) { return {}; }
        DecodedInst d;
        if (!decode(d, inst)) return {};
        if (d.reads_sp || d.writes_sp) return {};
        if (d.cls == InstClass::RET) {
            // Any recorded branch must target on or before the RET.
            if (r.has_branch && r.branch_target > pc) return {};
            r.size = n;
            return r;
        }
        switch (d.cls) {
            // Control flow / system / memory / atomics / vectors / adrp → no.
            case InstClass::B: case InstClass::BL: case InstClass::BR:
            case InstClass::BLR:
            case InstClass::CBZ: case InstClass::CBNZ:
            case InstClass::TBZ: case InstClass::TBNZ:
            case InstClass::SVC: case InstClass::SVC_IMM:
            case InstClass::HVC_IMM: case InstClass::SMC_IMM:
            case InstClass::BRK: case InstClass::BRK_IMM:
            case InstClass::HLT: case InstClass::HLT_IMM:
            case InstClass::MSR: case InstClass::MSR_SYS:
            case InstClass::MRS: case InstClass::MRS_SYS:
            case InstClass::CLREX: case InstClass::CLREX_INST:
            case InstClass::BARRIER: case InstClass::HINT: case InstClass::SYS_NOP:
            case InstClass::LDR_IMM: case InstClass::STR_IMM:
            case InstClass::LDR_UNS: case InstClass::STR_UNS:
            case InstClass::LDR_REG: case InstClass::STR_REG:
            case InstClass::LDP: case InstClass::STP:
            case InstClass::LDRSW: case InstClass::LDRSB: case InstClass::LDRSH:
            case InstClass::LDXR: case InstClass::STXR:
            case InstClass::LDAXR: case InstClass::STLXR:
            case InstClass::STLR: case InstClass::LDAR:
            case InstClass::LSE_ATOMIC:
            case InstClass::SIMD_LD1: case InstClass::SIMD_ST1:
            case InstClass::ADR: case InstClass::ADRP:
            case InstClass::UNKNOWN:
                return {};
            case InstClass::Bcond: {
                // Forward conditional branch only; exactly one per leaf.
                if (d.imm <= 0) return {};
                if (r.has_branch) return {};
                uint8_t c = d.cond & 0xF;
                if (c == 2 || c == 3 || c == 8 || c == 9) return {};  // CS/CC/HI/LS
                r.has_branch = true;
                r.branch_pc = pc;
                r.branch_target = pc + 4 + static_cast<uint64_t>(d.imm);
                break;
            }
            default:
                break;  // data-processing / scalar FP → OK
        }
        pc += 4;
    }
    return {};  // no RET within the cap
}
} // namespace jit_analysis
} // namespace arm64emu
