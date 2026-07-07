// interpreter.cpp — ARM64 instruction interpreter.
//
// This file implements Emulator::execute(), which decodes and executes
// a single ARM64 instruction. The decode logic is shared with the JIT
// (src/jit/frostjit.cpp) via decoder.hpp.
//
// When adding a new instruction:
//   1. Add the decode in decoder.cpp (sets InstClass)
//   2. Add the execute case here
//   3. Add JIT codegen in src/jit/frostjit.cpp (compile_ir_inst)

#include "core/emulator.h"
#include "decoder.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace arm64emu {

// Set NZCV from a 64-bit add-with-carry result.
static uint64_t set_add_flags(CPU& cpu, uint64_t a, uint64_t b, uint64_t carry_in,
                              int width, bool set_flags) {
    uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
    uint64_t a_w = a & mask;
    uint64_t b_w = b & mask;
    // Detect carry BEFORE the addition wraps. For 64-bit: carry = (a_w + b_w + carry_in) > mask.
    // Since uint64_t wraps, we detect carry as: a_w > (mask - b_w - carry_in).
    bool c = (a_w > (mask - b_w)) || (a_w == (mask - b_w) && carry_in);
    uint64_t sum = a_w + b_w + carry_in;
    uint64_t res = sum & mask;
    if (set_flags) {
        bool n = (res >> (width - 1)) & 1;
        bool z = (res == 0);
        bool sa = (a_w >> (width - 1)) & 1;
        bool sb = (b_w >> (width - 1)) & 1;
        bool sr = (res >> (width - 1)) & 1;
        bool v = (sa == sb) && (sa != sr);
        cpu.set_flag_n(n); cpu.set_flag_z(z); cpu.set_flag_c(c); cpu.set_flag_v(v);
    }
    return res;
}

// Set NZCV from a subtraction: a - b = a + ~b + 1
static uint64_t set_sub_flags(CPU& cpu, uint64_t a, uint64_t b, int width,
                              bool set_flags) {
    uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);
    uint64_t a_w = a & mask;
    uint64_t b_w = b & mask;
    uint64_t sum = a_w + (~b_w & mask) + 1;
    uint64_t res = sum & mask;
    if (set_flags) {
        bool n = (res >> (width - 1)) & 1;
        bool z = (res == 0);
        bool c = (a_w >= b_w);
        bool sa = (a_w >> (width - 1)) & 1;
        bool sb = (b_w >> (width - 1)) & 1;
        bool sr = (res >> (width - 1)) & 1;
        bool v = (sa != sb) && (sa != sr);
        cpu.set_flag_n(n); cpu.set_flag_z(z); cpu.set_flag_c(c); cpu.set_flag_v(v);
    }
    return res;
}

void Emulator::execute(uint32_t inst, uint64_t& next_pc, CPU& cpu) {
    auto* pcache = &cpu.page_cache;
    // ── Decode via the shared decoder ─────────────────────────────
    // The decoder (decoder.cpp) is the single source of truth for
    // instruction classification. We call decode() once, then dispatch
    // on d.cls. Every instruction handler lives in the switch below.
    {
        // ── Per-vCPU 2-way set-associative decode cache ─────────
        // Hash PC to a set, check both ways. On hit, skip decode().
        // This is the hot path — ~100% hit rate for tight loops.
        // Using a const reference avoids copying the 88-byte DecodedInst.
        // The cache lives on `cpu` so each vCPU gets a lock-free cache.
        //
        // upgraded from direct-mapped to 2-way
        // set-associative to reduce conflict misses.
        size_t set_idx = (cpu.pc >> 2) & CPU::DECODE_CACHE_SET_MASK;
        size_t way0 = set_idx * 2;
        size_t way1 = way0 + 1;
        CPU::CacheEntry& ce0 = cpu.decode_cache[way0];
        CPU::CacheEntry& ce1 = cpu.decode_cache[way1];
        const DecodedInst* dp;
        if (__builtin_expect(ce0.tag == cpu.pc, 1)) {
            dp = &ce0.d;
            cpu.decode_cache_hits++;
            // Update LRU: way 0 is now MRU.
            cpu.decode_cache_lru[set_idx >> 3] &= ~(1u << (set_idx & 7));
        } else if (__builtin_expect(ce1.tag == cpu.pc, 1)) {
            dp = &ce1.d;
            cpu.decode_cache_hits++;
            // Update LRU: way 1 is now MRU.
            cpu.decode_cache_lru[set_idx >> 3] |= (1u << (set_idx & 7));
        } else {
            // Miss: evict the LRU way.
            bool lru_bit = (cpu.decode_cache_lru[set_idx >> 3] >> (set_idx & 7)) & 1;
            CPU::CacheEntry& victim = lru_bit ? ce0 : ce1;
            decode(victim.d, inst);
            victim.tag = cpu.pc;
            dp = &victim.d;
            cpu.decode_cache_misses++;
            // The victim way is now MRU.
            if (lru_bit) {
                cpu.decode_cache_lru[set_idx >> 3] &= ~(1u << (set_idx & 7));
            } else {
                cpu.decode_cache_lru[set_idx >> 3] |= (1u << (set_idx & 7));
            }
        }
        const DecodedInst& d = *dp;
        switch (d.cls) {
            // ── ADC/ADCS/SBC/SBCS (add/subtract with carry) ──────────
            // These were previously unimplemented and
            // caused decode errors. Now handled via the decoder.
            case InstClass::ADC_REG:
            case InstClass::ADCS_REG:
            case InstClass::SBC_REG:
            case InstClass::SBCS_REG: {
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                uint64_t b = cpu.regs[d.rm];
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
                uint64_t carry_in = cpu.flag_c() ? 1 : 0;
                uint64_t res;
                bool is_sub = (d.cls == InstClass::SBC_REG || d.cls == InstClass::SBCS_REG);
                if (is_sub) {
                    uint64_t not_b = ~b & (width == 64 ? ~0ULL : 0xFFFFFFFF);
                    res = set_add_flags(cpu, a, not_b, carry_in, width, d.set_flags);
                } else {
                    res = set_add_flags(cpu, a, b, carry_in, width, d.set_flags);
                }
                if (d.rd != 31) cpu.regs[d.rd] = res;
                return;
            }

            // ── FMOV Vd.D[1], Rn / FMOV Rn, Vm.D[1] ──────────────────
            // (Moved to src/interp/interp_fp.cpp — execute_fp().)
            case InstClass::FMOV_VD1:
            case InstClass::FMOV_RVD1:
                execute_fp(inst, next_pc, cpu, d);
                return;

            // ── Branches & system (moved to src/interp/interp_branch.cpp) ──
            case InstClass::B:
            case InstClass::BL:
            case InstClass::Bcond:
            case InstClass::CBZ:
            case InstClass::CBNZ:
            case InstClass::TBZ:
            case InstClass::TBNZ:
            case InstClass::BR:
            case InstClass::BLR:
            case InstClass::RET:
            case InstClass::SVC_IMM:
            case InstClass::BRK_IMM:
            case InstClass::HLT_IMM:
            case InstClass::CLREX_INST:
            case InstClass::HINT:
            case InstClass::MRS_SYS:
            case InstClass::MSR_SYS:
                execute_branch(inst, next_pc, cpu, d);
                return;

            // ── ADR / ADRP ────────────────────────────────────────────
            case InstClass::ADR:
            case InstClass::ADRP: {
                bool adrp = (d.cls == InstClass::ADRP);
                uint32_t immlo = (d.raw >> 29) & 3;
                uint32_t immhi = (d.raw >> 5) & 0x7FFFF;
                uint64_t imm = (immhi << 2) | immlo;
                if (adrp) {
                    uint64_t base = cpu.pc & ~0xFFFULL;
                    uint64_t v = sign_extend(imm, 21) << 12;
                    if (d.rd != 31) cpu.regs[d.rd] = base + v;
                } else {
                    uint64_t v = sign_extend(imm, 21);
                    if (d.rd != 31) cpu.regs[d.rd] = cpu.pc + v;
                }
                return;
            }

            // ── MOVN / MOVZ / MOVK ────────────────────────────────────
            case InstClass::MOVN:
            case InstClass::MOVZ:
            case InstClass::MOVK: {
                uint8_t hw = (d.raw >> 21) & 3;
                uint16_t imm16 = (d.raw >> 5) & 0xFFFF;
                int width = d.sf ? 64 : 32;
                int shift = hw * 16;
                uint64_t v;
                if (d.cls == InstClass::MOVN) {
                    v = ~(static_cast<uint64_t>(imm16) << shift);
                    if (!d.sf) v &= 0xFFFFFFFF;
                    if (d.rd != 31) cpu.regs[d.rd] = v;
                } else if (d.cls == InstClass::MOVZ) {
                    v = static_cast<uint64_t>(imm16) << shift;
                    if (!d.sf) v &= 0xFFFFFFFF;
                    if (d.rd != 31) cpu.regs[d.rd] = v;
                } else { // MOVK
                    uint64_t mask = (width == 64)
                        ? (0xFFFFULL << shift)
                        : (0xFFFFULL << shift) & 0xFFFFFFFFULL;
                    uint64_t cur = cpu.regs[d.rd];
                    if (!d.sf) cur &= 0xFFFFFFFF;
                    v = (cur & ~mask) | (static_cast<uint64_t>(imm16) << shift);
                    if (!d.sf) v &= 0xFFFFFFFF;
                    if (d.rd != 31) cpu.regs[d.rd] = v;
                }
                return;
            }

            // ── ADD/SUB immediate ─────────────────────────────────────
            case InstClass::ADD_IMM:
            case InstClass::ADDS_IMM:
            case InstClass::SUB_IMM:
            case InstClass::SUBS_IMM: {
                bool sh = (d.raw >> 22) & 1;
                uint16_t imm12 = (d.raw >> 10) & 0xFFF;
                int width = d.sf ? 64 : 32;
                bool set_flags = d.set_flags;
                bool is_sub = d.is_sub;
                uint64_t a = (d.rn == 31 && !set_flags) ? cpu.sp : cpu.regs[d.rn];
                if (!d.sf && !(d.rn == 31 && !set_flags)) a &= 0xFFFFFFFF;
                uint64_t b = static_cast<uint64_t>(imm12) << (sh ? 12 : 0);
                uint64_t res;
                if (is_sub) res = set_sub_flags(cpu, a, b, width, set_flags);
                else        res = set_add_flags(cpu, a, b, 0, width, set_flags);
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd == 31) {
                    if (!set_flags) cpu.sp = res;
                } else {
                    cpu.regs[d.rd] = res;
                }
                return;
            }

            // ── Bitfield (SBFM/BFM/UBFM) ─────────────────────────────
            case InstClass::SBFM:
            case InstClass::BFM:
            case InstClass::UBFM: {
                uint8_t opc = (d.raw >> 29) & 3;
                uint8_t immr = (d.raw >> 16) & 0x3F;
                uint8_t imms = (d.raw >> 10) & 0x3F;
                int width = d.sf ? 64 : 32;
                if (!d.sf && (immr & 0x20 || imms & 0x20))
                    throw DecodeError(cpu.pc, inst);
                uint64_t src = cpu.regs[d.rn];
                if (!d.sf) src &= 0xFFFFFFFF;
                int datasize = width;

                // Per ARM ARM, BFM/SBFM/UBFM all use DecodeBitMasks to compute
                // wmask (write mask) and tmask (top mask), then:
                //   bot = (dst & ~wmask) | (ROR(src, immr) & wmask)
                //   dst = (dst & ~tmask) | (bot & tmask)         [BFM]
                //   dst = sign_extend(bot, imms+1) & tmask        [SBFM]
                //   dst = bot & tmask                              [UBFM]
                //
                // For SBFM/UBFM, dst is 0 (so bot = ROR(src, immr) & wmask).
                // For BFM, dst is the current Rd value.
                //
                // DecodeBitMasks for the imms>=immr case (the "extract" case):
                //   len = highest_set_bit(N:~imms)
                //   esize = 1 << len
                //   levels = esize - 1
                //   S = imms & levels
                //   R = immr & levels
                //   if S == levels: reserved (we don't check)
                //   wmask = replicate(ones(S+1) >> R within esize, esize)
                //   tmask = replicate(ones(S+1) << (esize-1-S) within esize, esize)
                //
                // But there's a simpler equivalent for the common cases.
                // For SBFM/UBFM when imms >= immr:
                //   extracted = (src >> immr) & ones(imms-immr+1)
                // For SBFM/UBFM when imms < immr:
                //   extracted = ROR(src, immr) & ones(imms+1)
                // For BFM when imms >= immr (BFXIL):
                //   dst[imms:immr] = src[imms:immr]  (preserve rest of dst)
                // For BFM when imms < immr (BFI):
                //   dst[imms+immr:immr] = src[imms:0]  (preserve rest of dst)
                //
                // The v0 code had a bug in the BFI case: it computed
                //   field_mask = mask | hi_mask
                // which for BFI(x3, x0, #48, #16) gives field_mask = 0xFFFF | 0xFFFFFFFFFFFF0000 = ~0
                // → replaces ALL of dst instead of just bits[63:48].
                // This broke __floatsitf's BFI, corrupting 128-bit long doubles.

                if (imms >= immr) {
                    // BFXIL / extract case
                    int len = imms - immr + 1;
                    uint64_t mask = (len == 64) ? ~0ULL : ((1ULL << len) - 1);

                    // ── LSR #0 / LSL #0 special case (fix) ──
                    // On AArch64, `LSR Xd, Xn, #0` is encoded as
                    // `UBFM Xd, Xn, #0, #63`. Real hardware treats LSR
                    // by 0 as a shift by 64 (result = 0), NOT as a
                    // no-op. The UBFM decode (ROR by 0 + mask all-ones)
                    // would incorrectly produce the source unchanged.
                    //
                    // This was the root cause of musl's qsort (smoothsort)
                    // producing wrong results: smoothsort's shr() function
                    // does `p[0] >>= n` where n can be 0, and the compiler
                    // emits `LSR Xd, Xn, #0` expecting a zero result.
                    // Our emulator returned the original value, corrupting
                    // the bit vector and causing the sort to produce
                    // subtly wrong output (e.g. "1 2 3 4 6 7 8 5 9 10"
                    // instead of "1 2 3 4 5 6 7 8 9 10").
                    //
                    // The fix: when len == datasize (full-width field)
                    // and immr == 0, the UBFM result is 0 (shift by
                    // full width). For SBFM, the result is sign-extended
                    // (0 for non-negative, all-ones for negative).
                    if (len == datasize && immr == 0) {
                        if (opc == 0) {
                            // SBFM: ASR by 64 → sign bit replicated
                            uint64_t sign_bit = (src >> (datasize - 1)) & 1;
                            uint64_t result = sign_bit ? ~0ULL : 0;
                            if (datasize == 32) result &= 0xFFFFFFFF;
                            if (d.rd != 31) cpu.regs[d.rd] = result;
                        } else if (opc == 2) {
                            // UBFM: LSR by 64 → 0
                            if (d.rd != 31) cpu.regs[d.rd] = 0;
                        } else {
                            // BFM: BFXIL with full width and immr=0
                            // → replace entire destination with src
                            if (d.rd != 31) cpu.regs[d.rd] = src;
                        }
                        if (!d.sf && d.rd != 31) cpu.regs[d.rd] &= 0xFFFFFFFF;
                        return;
                    }

                    uint64_t extracted = (src >> immr) & mask;
                    if (opc == 0) {
                        // SBFM: sign-extend
                        uint64_t m = (1ULL << (len - 1));
                        if (extracted & m) {
                            uint64_t high = ~mask & (datasize == 64 ? ~0ULL : (1ULL<<datasize)-1);
                            extracted |= high;
                        }
                        if (d.rd != 31) cpu.regs[d.rd] = extracted;
                    } else if (opc == 2) {
                        // UBFM
                        if (d.rd != 31) cpu.regs[d.rd] = extracted;
                    } else {
                        // BFM (BFXIL): extract field from src[imms:immr],
                        // place in LOW bits of dest, preserve dest high bits.
                        // ARM ARM: Wd[width-1:0] = Wn[imms:immr]
                        uint64_t cur = cpu.regs[d.rd];
                        if (!d.sf) cur &= 0xFFFFFFFF;
                        uint64_t dst_mask = mask;  // low 'len' bits
                        uint64_t keep = cur & ~dst_mask;
                        if (d.rd != 31) cpu.regs[d.rd] = keep | (extracted & dst_mask);
                    }
                } else {
                    // BFI / rotate case (imms < immr).
                    //
                    // This covers SBFIZ, UBFIZ, BFI, LSL (when shift > 0),
                    // and the imms < immr form of SBFM/UBFM/BFM.
                    //
                    // The operation is: take the low (imms+1) bits of src,
                    // optionally sign-extend (SBFM), and shift left by
                    // (datasize - immr) to place them at the correct
                    // position in the result.
                    //
                    // fix: the previous code used `high_mask`
                    // (top bits) which was correct for LSL but WRONG for
                    // SBFIZ/UBFIZ where the field is NOT at the top of
                    // the register. This broke musl's smoothsort which
                    // uses `sbfiz x3, x19, #3, #32` to compute
                    // pshift * 8 for lp[] indexing — the result was 0
                    // instead of the correct value, causing qsort to
                    // produce wrong output for n >= 8.
                    int width = imms + 1;
                    int lsb = datasize - immr;
                    uint64_t field = src & ((width >= 64) ? ~0ULL : ((1ULL << width) - 1));
                    if (opc == 0) {
                        // SBFM (SBFIZ): sign-extend from bit width-1
                        if (width < 64 && (field & (1ULL << (width - 1)))) {
                            uint64_t high_bits = ~((1ULL << width) - 1);
                            if (datasize == 32) high_bits &= 0xFFFFFFFFULL;
                            field |= high_bits;
                        }
                    }
                    uint64_t result = (lsb >= 64) ? 0 : (field << lsb);
                    if (datasize == 32) result &= 0xFFFFFFFFULL;
                    if (opc == 0) {
                        // SBFM
                        if (d.rd != 31) cpu.regs[d.rd] = result;
                    } else if (opc == 2) {
                        // UBFM
                        if (d.rd != 31) cpu.regs[d.rd] = result;
                    } else {
                        // BFM (BFI): insert field into destination
                        uint64_t dst_mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
                        dst_mask = (lsb >= 64) ? 0 : (dst_mask << lsb);
                        if (datasize == 32) dst_mask &= 0xFFFFFFFFULL;
                        uint64_t cur = cpu.regs[d.rd];
                        if (!d.sf) cur &= 0xFFFFFFFF;
                        uint64_t keep = cur & ~dst_mask;
                        if (d.rd != 31) cpu.regs[d.rd] = keep | (result & dst_mask);
                    }
                }
                if (!d.sf && d.rd != 31) cpu.regs[d.rd] &= 0xFFFFFFFF;
                return;
            }

            // ── EXTR ─────────────────────────────────────────────────
            case InstClass::EXTR: {
                uint8_t immr = (d.raw >> 10) & 0x3F;
                int width = d.sf ? 64 : 32;
                // Per ARM ARM: EXTR Xd, Xn, Xm, #lsb extracts a width-bit
                // field from the 2*width-bit concatenation (Xn:Xm).
                //   Xd = ((Xn << width) | Xm) >> lsb   [masked to width]
                // v0 had two bugs:
                //   1. Concatenated Rm:Rn instead of Rn:Rm (operand order)
                //   2. Used (rn << width) which is UB when width == 64
                //      (shifting a uint64_t by its full width is UB in C++)
                // Both are now fixed: we use __uint128_t for the 128-bit
                // concatenation to avoid the UB.
                uint64_t rn = cpu.regs[d.rn];
                uint64_t rm = cpu.regs[d.rm];
                if (!d.sf) { rn &= 0xFFFFFFFF; rm &= 0xFFFFFFFF; }
                __uint128_t combined = ((__uint128_t)rn << width) | rm;
                __uint128_t shifted = combined >> immr;
                uint64_t v = static_cast<uint64_t>(shifted) & (width == 64 ? ~0ULL : 0xFFFFFFFFULL);
                if (!d.sf) v &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = v;
                return;
            }

            // ── Logical immediate (AND/ORR/EOR/ANDS) ────────────────
            case InstClass::AND_IMM:
            case InstClass::ORR_IMM:
            case InstClass::EOR_IMM:
            case InstClass::ANDS_IMM: {
                uint8_t opc = (d.raw >> 29) & 3;
                int width = d.sf ? 64 : 32;
                // Use the decoder's pre-decoded bitmask (d.imm_u).
                // The decoder's decode_bitmask_imm is now correct (fixed
                // the esize==64 case to use width instead of ~0).
                uint64_t imm_val = d.imm_u;
                uint64_t a = cpu.regs[d.rn];
                if (!d.sf) a &= 0xFFFFFFFF;
                uint64_t res;
                bool set_flags = false;
                switch (opc) {
                    case 0: res = a & imm_val; break;
                    case 1: res = a | imm_val; break;
                    case 2: res = a ^ imm_val; break;
                    case 3: res = a & imm_val; set_flags = true; break;
                    default: throw DecodeError(cpu.pc, inst);
                }
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = res;
                else if (opc == 1) cpu.sp = res;  // ORR to SP
                if (set_flags) {
                    cpu.set_flag_n((res >> (width - 1)) & 1);
                    cpu.set_flag_z(res == 0);
                    cpu.set_flag_c(false);
                    cpu.set_flag_v(false);
                }
                return;
            }

            // ── ADD/SUB shifted/extended register ─────────────────────
            case InstClass::ADD_REG:
            case InstClass::ADDS_REG:
            case InstClass::SUB_REG:
            case InstClass::SUBS_REG: {
                // Covers both shifted-register (mask 0x1F200000 == 0x0B000000)
                // and extended-register (mask 0x1FE00000 == 0x0B200000) forms.
                // The decoder collapsed both into the same InstClass; we
                // re-check the encoding to disambiguate.
                bool extended = ((d.raw & 0x1FE00000) == 0x0B200000);
                int width = d.sf ? 64 : 32;
                // For extended register: Rn=31 reads SP (not XZR).
                // For shifted register: Rn=31 reads XZR (regs[31]=0).
                uint64_t a = (extended && d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                uint64_t b;
                if (extended) {
                    b = extend_reg(cpu.regs[d.rm], d.extend, d.shift, d.sf);
                } else {
                    b = cpu.regs[d.rm];
                    if (!d.sf) b &= 0xFFFFFFFF;
                    switch (d.shift_type) {
                        case 0: b = b << d.shift; break;
                        case 1: b = (width == 64) ? (b >> d.shift) : (static_cast<uint32_t>(b) >> d.shift); break;
                        // ASR: sign-extend from the operation width. Casting
                        // the already-zero-extended b to int64_t directly
                        // would leave sign bit at 63 (always 0 for 32-bit),
                        // making ASR behave like LSR.
                        case 2:
                            b = (width == 64)
                                ? (static_cast<int64_t>(b) >> d.shift)
                                : (static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(b))) >> d.shift);
                            break;
                        case 3: {
                            // ROR: must rotate within the operation width, not 64 bits.
                            // ror64 on a zero-extended 32-bit value loses wrap bits.
                            if (width == 64) {
                                b = ror64(b, d.shift);
                            } else {
                                uint32_t v = static_cast<uint32_t>(b);
                                unsigned r = d.shift & 31;
                                b = r ? ((v >> r) | (v << (32 - r))) : v;
                            }
                        } break;
                    }
                }
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
                uint64_t res;
                if (d.is_sub) res = set_sub_flags(cpu, a, b, width, d.set_flags);
                else          res = set_add_flags(cpu, a, b, 0, width, d.set_flags);
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd == 31) {
                    if (!d.set_flags) cpu.sp = res;
                } else {
                    cpu.regs[d.rd] = res;
                }
                return;
            }

            // ── Logical shifted register (AND/ORR/EOR/ANDS) ──────────
            case InstClass::AND_REG:
            case InstClass::ORR_REG:
            case InstClass::EOR_REG:
            case InstClass::ANDS_REG: {
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                uint64_t b = cpu.regs[d.rm];
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
                switch (d.shift_type) {
                    case 0: b = b << d.shift; break;
                    case 1: b = (width == 64) ? (b >> d.shift) : (static_cast<uint32_t>(b) >> d.shift); break;
                    // ASR: sign-extend from operation width (see ADD/SUB above).
                    case 2:
                        b = (width == 64)
                            ? (static_cast<int64_t>(b) >> d.shift)
                            : (static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(b))) >> d.shift);
                        break;
                    case 3: {
                        // ROR: must rotate within the operation width, not 64 bits.
                        // ror64 on a zero-extended 32-bit value loses wrap bits.
                        if (width == 64) {
                            b = ror64(b, d.shift);
                        } else {
                            uint32_t v = static_cast<uint32_t>(b);
                            unsigned r = d.shift & 31;
                            b = r ? ((v >> r) | (v << (32 - r))) : v;
                        }
                    } break;
                }
                if (!d.sf) b &= 0xFFFFFFFF;
                uint8_t opc = (d.raw >> 29) & 3;
                bool N_bit = (d.raw >> 21) & 1;  // N bit: inverts b (BIC/ORN/EON/BICS)
                if (N_bit) b = ~b;
                uint64_t res;
                bool set_flags = false;
                switch (opc) {
                    case 0: res = a & b; break;   // AND (N=0) or BIC (N=1)
                    case 1: res = a | b; break;   // ORR (N=0) or ORN (N=1)
                    case 2: res = a ^ b; break;   // EOR (N=0) or EON (N=1)
                    case 3: res = a & b; set_flags = true; break; // ANDS (N=0) or BICS (N=1)
                    default: throw DecodeError(cpu.pc, inst);
                }
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = res;
                else if (opc == 1) cpu.sp = res;
                if (set_flags) {
                    cpu.set_flag_n((res >> (width - 1)) & 1);
                    cpu.set_flag_z(res == 0);
                    cpu.set_flag_c(false);
                    cpu.set_flag_v(false);
                }
                return;
            }

            // ── Conditional select (CSEL/CSINC/CSINV/CSNEG) ──────────
            case InstClass::CSEL:
            case InstClass::CSINC:
            case InstClass::CSINV:
            case InstClass::CSNEG: {
                uint64_t a = cpu.regs[d.rn];
                uint64_t b = cpu.regs[d.rm];
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
                uint64_t res;
                if (cond_true(d.cond, cpu.pstate)) {
                    res = a;
                } else {
                    switch (d.cls) {
                        case InstClass::CSEL:  res = b;     break;
                        case InstClass::CSINC: res = b + 1; break;
                        case InstClass::CSINV: res = ~b;    break;
                        case InstClass::CSNEG: res = -b;    break;
                        default: throw DecodeError(cpu.pc, inst);
                    }
                }
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = res;
                return;
            }

            // ── Conditional compare (CCMP/CCMN) ──────────────────────
            case InstClass::CCMP:
            case InstClass::CCMN: {
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                if (!d.sf) a &= 0xFFFFFFFF;
                uint64_t operand;
                if (d.is_register) {
                    operand = cpu.regs[d.rm];
                    if (!d.sf) operand &= 0xFFFFFFFF;
                } else {
                    operand = (d.raw >> 16) & 0x1F;  // imm5
                }
                if (cond_true(d.cond, cpu.pstate)) {
                    if (d.cls == InstClass::CCMP) {
                        set_sub_flags(cpu, a, operand, width, true);
                    } else {
                        set_add_flags(cpu, a, operand, 0, width, true);
                    }
                } else {
                    cpu.set_flag_n(d.nzcv_field & 8);
                    cpu.set_flag_z(d.nzcv_field & 4);
                    cpu.set_flag_c(d.nzcv_field & 2);
                    cpu.set_flag_v(d.nzcv_field & 1);
                }
                return;
            }

            // ── Data processing (1-source): RBIT/REV/REV16/REV32/CLZ/CLS ──
            case InstClass::RBIT:
            case InstClass::REV16:
            case InstClass::REV32:
            case InstClass::REV:
            case InstClass::CLZ:
            case InstClass::CLS: {
                int width = d.sf ? 64 : 32;
                uint64_t v = cpu.regs[d.rn];
                if (!d.sf) v &= 0xFFFFFFFF;
                switch (d.cls) {
                    case InstClass::RBIT: {
                        uint64_t r = 0;
                        for (int i = 0; i < width; i++)
                            if (v & (1ULL << i)) r |= (1ULL << (width - 1 - i));
                        v = r;
                        break;
                    }
                    case InstClass::REV16:
                        if (width == 64)
                            v = ((v & 0xFF00FF00FF00FF00ULL) >> 8) |
                                ((v & 0x00FF00FF00FF00FFULL) << 8);
                        else
                            v = ((v & 0xFF00FF00ULL) >> 8) | ((v & 0x00FF00FFULL) << 8);
                        break;
                    case InstClass::REV32:
                        // REV32: reverse bytes within each 32-bit word.
                        // 64-bit: apply bswap32 to both halves.
                        // 32-bit: just bswap32 (though REV32 is typically 64-bit only).
                        if (width == 64) {
                            uint32_t lo = static_cast<uint32_t>(v);
                            uint32_t hi = static_cast<uint32_t>(v >> 32);
                            v = (static_cast<uint64_t>(__builtin_bswap32(hi)) << 32) |
                                static_cast<uint64_t>(__builtin_bswap32(lo));
                        } else {
                            v = __builtin_bswap32(static_cast<uint32_t>(v));
                        }
                        break;
                    case InstClass::REV:
                        v = (width == 64) ? __builtin_bswap64(v)
                                          : __builtin_bswap32(static_cast<uint32_t>(v));
                        break;
                    case InstClass::CLZ:
                        if (v == 0) v = width;
                        else v = (width == 64) ? __builtin_clzll(v)
                                               : __builtin_clz(static_cast<uint32_t>(v));
                        break;
                    case InstClass::CLS:
                        // ARM CLS: count leading sign bits = CLZ(v ^ SAR(v, W-1)) - 1.
                        // Edge cases: CLS(0) = CLS(~0) = W-1 (the -1 makes CLZ(0)-1 = W-1).
                        // __builtin_clz(0) is UB, so handle 0 and ~0 explicitly.
                        if (width == 64) {
                            if (v == 0 || v == ~0ULL) v = 63;
                            else v = (v >> 63) ? __builtin_clzll(~v) - 1
                                               : __builtin_clzll(v)  - 1;
                        } else {
                            if (v == 0 || v == 0xFFFFFFFFULL) v = 31;
                            else v = (v >> 31) ? __builtin_clz(static_cast<uint32_t>(~v)) - 1
                                               : __builtin_clz(static_cast<uint32_t>(v))  - 1;
                        }
                        break;
                    default: break;
                }
                if (!d.sf) v &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = v;
                return;
            }

            // ── Data processing (2-source): UDIV/SDIV/LSL/LSR/ASR/ROR ──
            case InstClass::UDIV:
            case InstClass::SDIV:
            case InstClass::LSL:
            case InstClass::LSR:
            case InstClass::ASR:
            case InstClass::ROR: {
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                uint64_t b = cpu.regs[d.rm];
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
                uint64_t res = 0;
                switch (d.cls) {
                    case InstClass::UDIV:
                        if (b != 0)
                            res = (width == 64) ? a / b : static_cast<uint32_t>(a) / static_cast<uint32_t>(b);
                        break;
                    case InstClass::SDIV:
                        if (b != 0)
                            res = (width == 64) ? static_cast<uint64_t>(static_cast<int64_t>(a) / static_cast<int64_t>(b))
                                                : static_cast<uint64_t>(static_cast<int32_t>(a) / static_cast<int32_t>(b));
                        break;
                    case InstClass::LSL:
                        res = (width == 64) ? (a << (b & 63)) : (static_cast<uint32_t>(a) << (b & 31));
                        break;
                    case InstClass::LSR:
                        res = (width == 64) ? (a >> (b & 63)) : (static_cast<uint32_t>(a) >> (b & 31));
                        break;
                    case InstClass::ASR:
                        res = (width == 64) ? static_cast<uint64_t>(static_cast<int64_t>(a) >> (b & 63))
                                            : static_cast<uint64_t>(static_cast<int32_t>(a) >> (b & 31));
                        break;
                    case InstClass::ROR: {
                        // ROR rotates a value RIGHT by `b` bits, with the
                        // bits that fall off the bottom reappearing at the
                        // top. The 32-bit and 64-bit forms have DIFFERENT
                        // widths — the 32-bit form must rotate within 32 bits.
                        //
                        // BUGFIX: the old code called `ror64(a, b & 31)`
                        // for the 32-bit case. ror64 does a 64-bit rotate,
                        // so `v << (64 - r)` shifts the 32-bit value entirely
                        // out of the low word (e.g. r=7 → <<57, bits land
                        // at positions 57-88, all above bit 32). The result
                        // was missing the high bits that should have wrapped
                        // around — e.g. ROR(0x12345678, 7) returned 0x02468acf
                        // instead of 0xf2468acf. This broke MD5 (which uses
                        // 32-bit rotates in every round) and any other code
                        // using ROR — the JIT was correct, the interpreter
                        // was wrong, so verify mode flagged "false-positive"
                        // divergences on every ROR-heavy block.
                        if (width == 64) {
                            res = ror64(a, b & 63);
                        } else {
                            uint32_t v = static_cast<uint32_t>(a);
                            unsigned r = b & 31;
                            if (r == 0) {
                                res = v;
                            } else {
                                res = (v >> r) | (v << (32 - r));
                            }
                        }
                        break;
                    }
                    default: break;
                }
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = res;
                return;
            }

            // ── Data processing (3-source): MADD/MSUB/SMADDL/SMSUBL/UMADDL/UMSUBL/UMULH/SMULH ──
            case InstClass::MADD:
            case InstClass::MSUB:
            case InstClass::SMADDL:
            case InstClass::SMSUBL:
            case InstClass::UMADDL:
            case InstClass::UMSUBL:
            case InstClass::UMULH:
            case InstClass::SMULH: {
                int width = d.sf ? 64 : 32;
                uint64_t a = cpu.regs[d.rn];
                uint64_t b = cpu.regs[d.rm];
                uint64_t c = cpu.regs[d.ra];
                if (!d.sf) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; c &= 0xFFFFFFFF; }
                uint64_t res = 0;
                switch (d.cls) {
                    case InstClass::MADD:
                        res = (width == 64) ? (a * b) : (static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
                        res = c + res;
                        break;
                    case InstClass::MSUB:
                        res = (width == 64) ? (a * b) : (static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
                        res = c - res;
                        break;
                    case InstClass::SMADDL:
                        res = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int64_t>(static_cast<int32_t>(b)));
                        res = c + res;
                        break;
                    case InstClass::SMSUBL:
                        res = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int64_t>(static_cast<int32_t>(b)));
                        res = c - res;
                        break;
                    case InstClass::UMADDL:
                        res = static_cast<uint64_t>(static_cast<uint32_t>(a)) * static_cast<uint64_t>(static_cast<uint32_t>(b));
                        res = c + res;
                        break;
                    case InstClass::UMSUBL:
                        res = static_cast<uint64_t>(static_cast<uint32_t>(a)) * static_cast<uint64_t>(static_cast<uint32_t>(b));
                        res = c - res;
                        break;
                    case InstClass::UMULH: {
                        unsigned __int128 prod = (unsigned __int128)a * (unsigned __int128)b;
                        res = static_cast<uint64_t>(prod >> 64);
                        break;
                    }
                    case InstClass::SMULH: {
                        unsigned __int128 prod = (unsigned __int128)(static_cast<int64_t>(a) * static_cast<int64_t>(b));
                        res = static_cast<uint64_t>(prod >> 64);
                        break;
                    }
                    default: break;
                }
                if (!d.sf) res &= 0xFFFFFFFF;
                if (d.rd != 31) cpu.regs[d.rd] = res;
                return;
            }

            // ── Load/store pair (STP/LDP, all modes) ──────────────────
            // Encoding: opc 101 V mode L imm7 Rt2 Rn Rt
            //   mode (bits 24:23): 01=post, 10=offset, 11=pre (bit 25 is 0
            //   for post/offset; bit 25 = 1 for pre, but pre-index STP/LDP
            //   currently gets misclassified as ORR — known issue, same as
            //   — the hierarchical decoder now handles this.)
            case InstClass::STP:
            case InstClass::LDP: {
                uint8_t opc = (d.raw >> 30) & 3;
                int esize;
                if (d.is_vec) {
                    esize = (opc == 0) ? 4 : (opc == 1) ? 8 : 16;
                } else {
                    esize = (opc == 2) ? 8 : 4;
                }
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                int64_t disp = d.disp;  // already scaled by esize in decoder
                uint64_t addr;
                if (d.mode == 1) {            // post-index
                    addr = base;
                    uint64_t nb = base + disp;
                    if (d.rn == 31) cpu.sp = nb; else cpu.regs[d.rn] = nb;
                } else if (d.mode == 3) {     // pre-index
                    addr = base + disp;
                    uint64_t nb = base + disp;
                    if (d.rn == 31) cpu.sp = nb; else cpu.regs[d.rn] = nb;
                } else {                       // signed offset (mode == 2 or 0)
                    addr = base + disp;
                }
                if (d.is_vec) {
                    // BUGFIX: previously hardcoded 8 bytes per FP register
                    // for the lo/hi halves. For S-form (esize=4, single-
                    // precision), this corrupted the high 32 bits of v_lo
                    // and read 4 garbage bytes past the proper range. Fix:
                    // use esize for the per-register transfer, and only
                    // touch v_hi for 128-bit (Q-form) registers. The GPR
                    // path below already correctly uses esize.
                    if (d.is_load) {
                        uint64_t lo1 = 0, hi1 = 0, lo2 = 0, hi2 = 0;
                        mem_.read(addr, &lo1, esize, pcache);
                        if (esize >= 16) mem_.read(addr + 8, &hi1, 8, pcache);
                        mem_.read(addr + esize, &lo2, esize, pcache);
                        if (esize >= 16) mem_.read(addr + esize + 8, &hi2, 8, pcache);
                        cpu.v_lo[d.rt] = lo1;
                        cpu.v_hi[d.rt] = (esize >= 16) ? hi1 : 0;
                        cpu.v_lo[d.rt2] = lo2;
                        cpu.v_hi[d.rt2] = (esize >= 16) ? hi2 : 0;
                    } else {
                        mem_.write(addr, &cpu.v_lo[d.rt], esize, pcache);
                        if (esize >= 16) mem_.write(addr + 8, &cpu.v_hi[d.rt], 8, pcache);
                        mem_.write(addr + esize, &cpu.v_lo[d.rt2], esize, pcache);
                        if (esize >= 16) mem_.write(addr + esize + 8, &cpu.v_hi[d.rt2], 8, pcache);
                    }
                } else {
                    if (d.is_load) {
                        uint64_t v1 = 0, v2 = 0;
                        mem_.read(addr, &v1, esize, pcache);
                        mem_.read(addr + esize, &v2, esize, pcache);
                        if (d.rt  != 31) cpu.regs[d.rt]  = v1;
                        if (d.rt2 != 31) cpu.regs[d.rt2] = v2;
                    } else {
                        uint64_t v1 = (d.rt  == 31) ? 0 : cpu.regs[d.rt];
                        uint64_t v2 = (d.rt2 == 31) ? 0 : cpu.regs[d.rt2];
                        mem_.write(addr, &v1, esize, pcache);
                        mem_.write(addr + esize, &v2, esize, pcache);
                    }
                }
                return;
            }

            // ── LSE atomics (LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN/SWP/CAS) ──
            // The decoder classifies LSE atomics by encoding (mode_b==0b00,
            // V=0). If the decoder says LSE_ATOMIC, it IS an LSE atomic —
            // the has_lse_ PT_NOTE check was too conservative (musl GCC
            // 11.2.1 doesn't emit .gnu.property even with -march=armv8.1-a+lse).
            case InstClass::LSE_ATOMIC: {
                // Execute as an LSE atomic.
                // For LSE atomics, the "load" (return old value) is determined
                // by Rt != 31 (XZR), NOT by bit 22 (which is the acquire/release
                // flag o0). STADD = LDADD with Rt=31, STCLR = LDCLR with Rt=31, etc.
                int width_bytes = 1 << d.size;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                uint64_t mask = (width_bytes == 8) ? ~0ULL
                              : ((1ULL << (width_bytes * 8)) - 1);
                bool returns_old = (d.rt != 31);

                // BUGFIX: take the exclusive-monitor shard lock around the
                // entire RMW sequence. The old code did read→compute→write
                // without any lock, so concurrent LSE atomics from multiple
                // vCPUs could lose updates (and CAS could see stale values).
                // The JIT path uses `lock`-prefixed x86 instructions and is
                // correct, so this was also a JIT/interpreter divergence.
                // The LDXR/STXR handler above uses the same shard mutex.
                auto& shard = excl_monitor_shards_[excl_shard_idx(base)];
                std::lock_guard<std::mutex> gatom(shard.mu);

                // CAS family (atom_op >= 0xC): compare-and-swap.
                // ARM CAS Ws, Wt, [Xn]:
                //   old = [Xn]; if old == Ws, [Xn] = Wt; Ws = old
                // Ws (rs) is BOTH the expected (input) AND old value (output).
                // Wt (rt) is the desired value (unchanged).
                if (d.atom_op >= 0xC) {
                    uint64_t old = 0;
                    mem_.read(base, &old, width_bytes, pcache);
                    uint64_t cmp = cpu.regs[d.rs] & mask;
                    old &= mask;
                    if (old == cmp) {
                        uint64_t newv = cpu.regs[d.rt] & mask;
                        mem_.write(base, &newv, width_bytes, pcache);
                    }
                    // OLD value goes to Ws (rs), NOT Wt (rt). Guard XZR.
                    if (d.rs != 31) cpu.regs[d.rs] = old;
                    return;
                }

                // SWP (atom_op == 0x8): atomic swap.
                if (d.atom_op == 0x8) {
                    uint64_t old = 0;
                    mem_.read(base, &old, width_bytes, pcache);
                    old &= mask;
                    uint64_t newv = cpu.regs[d.rs] & mask;
                    mem_.write(base, &newv, width_bytes, pcache);
                    if (returns_old) cpu.regs[d.rt] = old;
                    return;
                }

                // Other LSE atomics (LDADD/LDCLR/LDEOR/LDSET/SMAX/SMIN/UMAX/UMIN).
                uint64_t a = 0, b = cpu.regs[d.rs];
                mem_.read(base, &a, width_bytes, pcache);
                a &= mask;
                b &= mask;
                uint64_t newv = 0;
                switch (d.atom_op) {
                    case 0x0: newv = (a + b) & mask; break;            // LDADD
                    case 0x1: newv = (a & ~b) & mask; break;           // LDCLR
                    case 0x2: newv = (a ^ b) & mask; break;            // LDEOR
                    case 0x3: newv = (a | b) & mask; break;            // LDSET
                    case 0x4: { // SMAX
                        int64_t sa = static_cast<int64_t>(a << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        int64_t sb = static_cast<int64_t>(b << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        newv = (sa > sb ? sa : sb) & mask; break;
                    }
                    case 0x5: { // SMIN
                        int64_t sa = static_cast<int64_t>(a << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        int64_t sb = static_cast<int64_t>(b << (64 - width_bytes*8)) >> (64 - width_bytes*8);
                        newv = (sa < sb ? sa : sb) & mask; break;
                    }
                    case 0x6: newv = (a > b ? a : b) & mask; break;    // UMAX
                    case 0x7: newv = (a < b ? a : b) & mask; break;    // UMIN
                    default:  newv = a & mask; break;
                }
                mem_.write(base, &newv, width_bytes, pcache);
                if (returns_old) cpu.regs[d.rt] = a;
                return;
            }

            // ── Load/store (unsigned immediate offset) ───────────────
            case InstClass::LDR_IMM:
            case InstClass::STR_IMM: {
                uint8_t size = d.size;
                uint8_t opc = d.opc_ls;
                uint16_t imm12 = (d.raw >> 10) & 0xFFF;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                if (d.is_vec) {
                    bool is_q = (opc & 2) && size == 0;
                    int nbytes = is_q ? 16 : (1 << size);
                    uint64_t scale = is_q ? 4 : size;
                    uint64_t addr = base + (static_cast<uint64_t>(imm12) << scale);
                    if (d.is_load) {
                        uint64_t lo = 0, hi = 0;
                        mem_.read(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8, pcache);
                        cpu.v_lo[d.rt] = lo;
                        cpu.v_hi[d.rt] = (nbytes >= 16) ? hi : 0;
                    } else {
                        uint64_t lo = cpu.v_lo[d.rt];
                        uint64_t hi = cpu.v_hi[d.rt];
                        mem_.write(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) mem_.write(addr + 8, &hi, nbytes - 8, pcache);
                    }
                    return;
                }
                uint64_t addr = base + (imm12 << size);
                int width_bytes = 1 << size;
                if (d.is_load) {
                    uint64_t v = 0;
                    mem_.read(addr, &v, width_bytes, pcache);
                    if (d.rt != 31) {
                        if (opc & 2) v = sign_extend(v, width_bytes * 8);
                        cpu.regs[d.rt] = v;
                    }
                } else {
                    uint64_t v = (d.rt == 31) ? 0 : cpu.regs[d.rt];
                    uint64_t mask = (width_bytes == 8) ? ~0ULL
                                  : ((1ULL << (width_bytes * 8)) - 1);
                    v &= mask;
                    mem_.write(addr, &v, width_bytes, pcache);
                }
                return;
            }

            // ── Load/store (unscaled / post-index / pre-index) ───────
            // LDUR/STUR (mode=0), post-index (mode=1), pre-index (mode=2)
            case InstClass::LDR_UNS:
            case InstClass::STR_UNS: {
                uint8_t size = d.size;
                uint8_t opc = d.opc_ls;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                int64_t disp = d.disp;  // sign-extended imm9
                uint64_t addr;
                if (d.mode == 1) {            // post-index
                    addr = base;
                    uint64_t nb = base + disp;
                    if (d.rn == 31) cpu.sp = nb; else cpu.regs[d.rn] = nb;
                } else {                       // unscaled (mode=0) or pre-index (mode=2)
                    addr = base + disp;
                    if (d.mode == 2) {         // pre-index: writeback
                        uint64_t nb = base + disp;
                        if (d.rn == 31) cpu.sp = nb; else cpu.regs[d.rn] = nb;
                    }
                }
                if (d.is_vec) {
                    bool is_q = (opc & 2) && size == 0;
                    int nbytes = is_q ? 16 : (1 << size);
                    if (d.is_load) {
                        uint64_t lo = 0, hi = 0;
                        mem_.read(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8, pcache);
                        cpu.v_lo[d.rt] = lo;
                        cpu.v_hi[d.rt] = (nbytes >= 16) ? hi : 0;
                    } else {
                        uint64_t lo = cpu.v_lo[d.rt];
                        mem_.write(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) {
                            uint64_t hi = cpu.v_hi[d.rt];
                            mem_.write(addr + 8, &hi, nbytes - 8, pcache);
                        }
                    }
                    return;
                }
                int width_bytes = 1 << size;
                if (d.is_load) {
                    uint64_t v = 0;
                    mem_.read(addr, &v, width_bytes, pcache);
                    if (d.rt != 31) {
                        if (opc & 2) v = sign_extend(v, width_bytes * 8);
                        cpu.regs[d.rt] = v;
                    }
                } else {
                    uint64_t v = (d.rt == 31) ? 0 : cpu.regs[d.rt];
                    uint64_t mask = (width_bytes == 8) ? ~0ULL
                                  : ((1ULL << (width_bytes * 8)) - 1);
                    v &= mask;
                    mem_.write(addr, &v, width_bytes, pcache);
                }
                return;
            }

            // ── Load/store (register offset) ──────────────────────────
            case InstClass::LDR_REG:
            case InstClass::STR_REG: {
                uint8_t size = d.size;
                uint8_t opc = d.opc_ls;
                uint8_t option = d.extend;
                uint8_t S = d.shift;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                uint64_t off = extend_reg(cpu.regs[d.rm], option, S ? size : 0, true);
                uint64_t addr = base + off;
                if (d.is_vec) {
                    bool is_q = (opc & 2) && size == 0;
                    int nbytes = is_q ? 16 : (1 << size);
                    if (d.is_load) {
                        uint64_t lo = 0, hi = 0;
                        mem_.read(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) mem_.read(addr + 8, &hi, nbytes - 8, pcache);
                        cpu.v_lo[d.rt] = lo;
                        cpu.v_hi[d.rt] = (nbytes >= 16) ? hi : 0;
                    } else {
                        uint64_t lo = cpu.v_lo[d.rt];
                        mem_.write(addr, &lo, std::min(nbytes, 8), pcache);
                        if (nbytes > 8) {
                            uint64_t hi = cpu.v_hi[d.rt];
                            mem_.write(addr + 8, &hi, nbytes - 8, pcache);
                        }
                    }
                    return;
                }
                int width_bytes = 1 << size;
                if (d.is_load) {
                    uint64_t v = 0;
                    mem_.read(addr, &v, width_bytes, pcache);
                    if (d.rt != 31) {
                        if (opc & 2) v = sign_extend(v, width_bytes * 8);
                        cpu.regs[d.rt] = v;
                    }
                } else {
                    uint64_t v = (d.rt == 31) ? 0 : cpu.regs[d.rt];
                    uint64_t mask = (width_bytes == 8) ? ~0ULL
                                  : ((1ULL << (width_bytes * 8)) - 1);
                    v &= mask;
                    mem_.write(addr, &v, width_bytes, pcache);
                }
                return;
            }

            // ── Load/store exclusive (LDXR/STXR/LDAXR/STLXR/STLR/LDAR) ──
            // The decoder distinguishes these by excl_low6 + acquire bits:
            //   0x0F = STXR/LDXR family (with Rs)
            //   0x1F = STLXR/LDAXR family (with Rs)
            //   0x3F = STLR/LDAR family (no Rs) — but LDXR/LDAXR with no Rs
            //          is also 0x3F; o0 (acquire) distinguishes them.
            case InstClass::LDXR:
            case InstClass::STXR:
            case InstClass::LDAXR:
            case InstClass::STLXR:
            case InstClass::LDAR:
            case InstClass::STLR: {
                int width_bytes = 1 << d.size;
                uint64_t base = (d.rn == 31) ? cpu.sp : cpu.regs[d.rn];
                bool o0 = d.acquire;
                bool use_monitor = (o0 == 0) || (d.excl_low6 != 0x3F);

                if (!d.is_load) {
                    // Store-exclusive (STXR/STLXR) or store-release (STLR).
                    if (use_monitor) {
                        // STXR must atomically: (1) check this CPU's
                        // reservation, (2) write memory, (3) invalidate
                        // OTHER CPUs' reservations at this address. Steps
                        // 1-3 must be atomic w.r.t. other CPUs' LDXR/STXR
                        // — otherwise a race between this CPU's check and
                        // write lets two CPUs both succeed. We hold the
                        // SHARD lock (selected by address hash) for the
                        // entire sequence. Sharding lets independent
                        // atomics on different addresses proceed in
                        // parallel — critical for high-contention workloads.
                        auto& shard = excl_monitor_shards_[excl_shard_idx(base)];
                        std::lock_guard<std::mutex> gmon(shard.mu);
                        bool ok = cpu.excl_check(base, width_bytes);
                        if (ok) {
                            uint64_t v = cpu.regs[d.rt];
                            uint64_t mask = (width_bytes == 8) ? ~0ULL
                                          : ((1ULL << (width_bytes * 8)) - 1);
                            v &= mask;
                            mem_.write(base, &v, width_bytes, pcache);
                            // Invalidate OTHER CPUs' reservations at this
                            // address (inline, since we hold the shard lock).
                            auto it = shard.reservations.find(base);
                            if (it != shard.reservations.end()) {
                                for (CPU* p : it->second) {
                                    if (p != &cpu && p->excl_tag_valid) {
                                        p->excl_tag_valid = false;
                                    }
                                }
                                it->second.erase(
                                    std::remove(it->second.begin(), it->second.end(), &cpu),
                                    it->second.end());
                                if (it->second.empty()) {
                                    shard.reservations.erase(it);
                                }
                            }
                        }
                        if (d.rs != 31) cpu.regs[d.rs] = ok ? 0 : 1;
                        cpu.excl_clear();
                    } else {
                        // STLR: store-release (no monitor check, always succeeds).
                        // Must hold the shard lock during the write + invalidate
                        // so a concurrent STXR can't sneak in between them.
                        uint64_t v = cpu.regs[d.rt];
                        uint64_t mask = (width_bytes == 8) ? ~0ULL
                                      : ((1ULL << (width_bytes * 8)) - 1);
                        v &= mask;
                        auto& shard = excl_monitor_shards_[excl_shard_idx(base)];
                        std::lock_guard<std::mutex> gmon(shard.mu);
                        mem_.write(base, &v, width_bytes, pcache);
                        // Invalidate OTHER CPUs' reservations at this address.
                        auto it = shard.reservations.find(base);
                        if (it != shard.reservations.end()) {
                            for (CPU* p : it->second) {
                                if (p != &cpu && p->excl_tag_valid) {
                                    p->excl_tag_valid = false;
                                }
                            }
                            shard.reservations.erase(it);
                        }
                    }
                } else {
                    // Load-exclusive (LDXR/LDAXR) or load-acquire (LDAR).
                    // LDXR must atomically: (1) read memory, (2) mark this
                    // CPU's reservation, (3) register globally. We hold the
                    // SHARD lock so that a concurrent STXR on the same
                    // address can't sneak in between our read and registration.
                    auto& shard = excl_monitor_shards_[excl_shard_idx(base)];
                    std::lock_guard<std::mutex> gmon(shard.mu);
                    uint64_t v = 0;
                    mem_.read(base, &v, width_bytes, pcache);
                    // Guard XZR: writes to regs[31] must be dropped (XZR
                    // always reads as 0). Other load handlers (LDR_IMM,
                    // LDR_UNS, LDR_REG) do this; LDXR was missing the guard.
                    if (d.rt != 31) cpu.regs[d.rt] = v;
                    if (use_monitor) {
                        cpu.excl_mark(base, width_bytes);
                        // Register globally (inline, since we hold the lock).
                        auto& vec = shard.reservations[base];
                        bool found = false;
                        for (auto*& p : vec) {
                            if (p == &cpu) { found = true; break; }
                        }
                        if (!found) vec.push_back(&cpu);
                    }
                }
                return;
            }

            // ── SIMD load/store multiple structures (LD1/ST1) ─────────
            // (Moved to src/interp/interp_fp.cpp — execute_fp().)
            case InstClass::SIMD_LD1:
            case InstClass::SIMD_ST1:
                execute_fp(inst, next_pc, cpu, d);
                return;

            // ── SIMD data-processing (moved to src/interp/interp_fp.cpp) ──
            case InstClass::SIMD_DP:
                execute_fp(inst, next_pc, cpu, d);
                return;

            // ── FP scalar (moved to src/interp/interp_fp.cpp — execute_fp()) ────
            case InstClass::FP_SCALAR:
                execute_fp(inst, next_pc, cpu, d);
                return;

            default:
                // Not recognized by the decoder — fall through
                // to the unhandled-instruction error below.
                break;
        }
    }

    // If we reach here, the instruction was not recognized by the decoder
    // switch above. Bail with a DecodeError so the caller can report the
    // PC and the offending instruction word.
    throw DecodeError(cpu.pc, inst);
}

// ---------------------------------------------------------------------------
// Linux AArch64 syscall layer
//   syscall number in x8, args in x0..x5, return value in x0
// ---------------------------------------------------------------------------

} // namespace arm64emu

// End of interpreter.cpp
